/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2026 gavin_darkglider
 * Copyright (C) 2026 theofficialgman
 * Copyright (C) 2026 Anthropic (Claude AI assistant contributions)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/*
 * vk_layer_tegra_x11_present.c
 *
 * Vulkan implicit layer for NVIDIA Tegra L4T r32.x that fixes the broken
 * Vulkan-on-X11 present path by routing presentation through GL/GLX
 * instead of Vulkan WSI.
 *
 * BACKGROUND
 *
 * On Tegra L4T r32.x the Nvidia Vulkan ICD's WSI implementation for X11
 * does not produce vsync-locked presentation. Frames tear. This is a known
 * driver-side issue with no available fix from Nvidia (the BSP is EOL'd).
 *
 * However, the same driver does support:
 *   - Vulkan external memory export via VK_KHR_external_memory_fd
 *     (OPAQUE_FD handle type, OPTIMAL tiling, BGRA8/RGBA8 UNORM/SRGB)
 *   - Vulkan external semaphore export via VK_KHR_external_semaphore_fd
 *     (OPAQUE_FD handle type)
 *   - GL import of both via GL_EXT_memory_object_fd and GL_EXT_semaphore_fd
 *   - Working vsync-locked GL presentation through GLX_SGI_video_sync.
 *
 * This layer plumbs the two together. The application's Vulkan rendering is
 * untouched; we replace the WSI surface and swapchain with our own
 * implementation that:
 *
 *   1. Allocates the swapchain images as OPAQUE_FD-exportable Vulkan images
 *      (the application renders into them as if they were normal swapchain
 *      images).
 *   2. Imports each image into our GL/GLX context as a GL texture.
 *   3. At vkQueuePresentKHR time: bridges the application's render-done
 *      semaphore into a GL semaphore and posts the image to our worker.
 *   4. Bridges GL's sample-done back into a Vulkan semaphore so that the
 *      next vkAcquireNextImageKHR correctly gates the application's
 *      re-use of the image.
 *
 * No EGL, no dmabuf, no DRM. Only Nvidia's own Vulkan↔GL interop primitives.
 *
 * ARCHITECTURE
 *
 * One worker thread per swapchain owns the GLX context for the lifetime of
 * the swapchain. The application's render thread calls Acquire/Present;
 * Present hands work to the worker via a single-slot mailbox and returns
 * immediately. The worker samples the image into the GLX backbuffer, calls
 * glXSwapBuffers (with swap interval 0), then blocks on the actual hardware
 * vblank via glXWaitVideoSyncSGI. This pattern lets the worker thread sleep
 * in the kernel during vsync, rather than spinning in libGLX_nvidia's
 * sched_yield-based default wait. The technique is the same one KWin uses
 * for NVIDIA on X11 (see plugins/platforms/x11/standalone/glxbackend.cpp,
 * SGIVideoSyncVsyncMonitor).
 *
 * vkAcquireNextImageKHR CPU-blocks the application on a per-image fence
 * that the worker signals via the GL→Vulkan semaphore bridge; this is what
 * paces the app's render loop to real presentation rate.
 *
 * RUNTIME LIBRARY LOADING
 *
 * The .so does not link libGL, libGLX, or libX11. The Vulkan loader holds
 * an internal mutex during vkCreateInstance and dlopen()s implicit layers;
 * if our DT_NEEDED listed libGL, that library's constructor would run with
 * the loader mutex held, and on some systems that constructor calls back
 * into the loader, recursively deadlocking. We dlopen GL/X11 ourselves at
 * CreateSwapchain time (well past the loader-mutex window) and resolve
 * every function via dlsym into a table. See the lib_load() block.
 *
 * SYMBOL EXPORT
 *
 * Only vkNegotiateLoaderLayerInterfaceVersion is exported from the .so.
 * vkGetInstanceProcAddr and vkGetDeviceProcAddr are NOT — exporting them
 * causes the Vulkan loader's ICD-load path to resolve our symbols via
 * dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr") and re-enter our layer
 * recursively while the loader mutex is still held, deadlocking
 * vkCreateInstance. The loader doesn't need these symbols exported under
 * interface v2; it gets the function pointers from the
 * VkNegotiateLayerInterface struct that our negotiate fills in.
 *
 * BYPASS
 *
 * Setting the environment variable VK_TEGRA_X11_PRESENT_DISABLE=1 turns
 * the layer into a transparent passthrough. Useful for A/B testing the
 * native WSI against the layer-redirected one.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#define VK_USE_PLATFORM_XLIB_KHR 1
#define VK_USE_PLATFORM_XCB_KHR  1
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vk_icd.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>
#include <xcb/xcb.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

/* ----------------------------------------------------------------------- */
/* FLIP_TEST: prototype present backend. Replaces glXSwapBuffers with a
 * direct TEGRA_DC_EXT_FLIP4 ioctl on the display controller, to test
 * whether presenting real, GPU-fenced Vulkan-rendered content this way
 * avoids the tearing native vkQueuePresentKHR exhibits on this driver.
 * See README.md for the full result (short version: it doesn't -- FLIP4
 * tears at the same position/phase as native Vulkan, even called as
 * correctly as it can be from userspace).
 *
 * Architecture: entirely pure Vulkan in the data path, no GL at all.
 * create_flip_export_image() makes a second, separate LINEAR-tiled,
 * exportable Vulkan image per swapchain image slot; the worker thread
 * vkCmdCopyImage's the app's real OPTIMAL-tiled rendered image into it
 * (GPU-side detile, no CPU readback) and hands its raw Vulkan-exported
 * dma-buf fd to FLIP4 directly as buff_id -- no nvmap involvement at all,
 * dma_buf_get() (what tegra_dc_ext_pin_window() actually calls to resolve
 * buff_id) works with any dma-buf fd regardless of which subsystem
 * exported it. tegra_dc_ext.h's ioctls are plain syscalls via
 * open()/ioctl(), no dlopen indirection needed the way GL/X11 need (see
 * RUNTIME LIBRARY LOADING below).
 *
 * The one remaining non-Vulkan dependency: a minimal GLX context is kept
 * alive purely to call glXWaitVideoSyncSGI for vsync pacing, since FLIP4
 * itself doesn't block for vblank and there's no already-proven non-GLX
 * vblank wait in this codebase. It does no rendering and touches no image
 * data. There's no fallback path -- this file exists specifically to test
 * FLIP4 (see README.md), so any setup failure (opening /dev/tegra_dc_1,
 * claiming the window, GLX_SGI_video_sync missing, etc.) fails the
 * swapchain outright rather than silently degrading to a different
 * presentation mechanism; see layer_CreateSwapchainKHR. */
#define __user
#include "tegra_dc_ext.h"
#include "gob_swizzle_spv.h"
#include "uapi/linux/nvhost_ioctl.h"
#define FLIP_TEST_WIN_INDEX 1
#define FLIP_TEST_NVSYNCPT_INVALID ((__u32)-1)
/* ----------------------------------------------------------------------- */

/* RUNTIME LIBRARY LOADING
 *
 * We deliberately do NOT link against libGL, libGLX, or libX11 at build
 * time. The reason: implicit Vulkan layers are dlopen()'d by the Vulkan
 * loader while it holds an internal mutex during vkCreateInstance. If our
 * .so has DT_NEEDED entries for libGL, ld.so will recursively load libGL
 * before our constructors run, and libGL's own constructor (via the glvnd
 * dispatch path) calls back into the Vulkan loader to register vendor
 * support. That callback tries to acquire the same loader mutex that the
 * outer vkCreateInstance already holds — recursive deadlock.
 *
 * Observed symptom: Sascha Willems Vulkan samples hang inside
 * vkCreateInstance with a backtrace showing recursive entry into the
 * loader's createInstance through libGL's init path. RetroArch and vkcube
 * don't hit it because they happen to load libGL via other paths before
 * vkCreateInstance fires.
 *
 * Workaround: dlopen libGL/libGLX/libX11 ourselves at CreateSwapchain time
 * (when no loader mutex is held) and call every function through a table
 * of dlsym'd function pointers. The .so itself depends only on libc,
 * libdl, libpthread, and libvulkan headers (Vulkan calls go through the
 * layer dispatch, never linked).
 *
 * The macros after the LibTable struct redirect all GL/GLX/X11 calls in
 * the rest of this file to go through the table — minimal source-level
 * impact, but the link-time dependency vanishes. */

#define X11_FUNCS(M) \
    M(XInitThreads,      Status,  (void)) \
    M(XOpenDisplay,      Display*,(const char *)) \
    M(XCloseDisplay,     int,     (Display *)) \
    M(XInternAtom,       Atom,    (Display *, const char *, Bool)) \
    M(XCreateColormap,   Colormap,(Display *, Window, Visual *, int)) \
    M(XCreateWindow,     Window,  (Display *, Window, int, int, unsigned, unsigned, unsigned, int, unsigned, Visual *, unsigned long, XSetWindowAttributes *)) \
    M(XDestroyWindow,    int,     (Display *, Window)) \
    M(XFreeColormap,     int,     (Display *, Colormap)) \
    M(XFree,             int,     (void *)) \
    M(XMapWindow,        int,     (Display *, Window)) \
    M(XGetGeometry,      Status,  (Display *, Drawable, Window *, int *, int *, unsigned *, unsigned *, unsigned *, unsigned *)) \
    M(XGetWindowAttributes, Status, (Display *, Window, XWindowAttributes *)) \
    M(XResizeWindow,     int,     (Display *, Window, unsigned, unsigned)) \
    M(XFlush,            int,     (Display *)) \
    M(XChangeProperty,   int,     (Display *, Window, Atom, Atom, int, int, const unsigned char *, int)) \
    M(XTranslateCoordinates, Bool, (Display *, Window, Window, int, int, int *, int *, Window *))

/* FLIP_TEST: runtime DC auto-detection (detect_dc_for_window). Resolved the
 * same dlopen+dlsym way as X11_FUNCS, for the same reason (avoids the
 * Vulkan-loader-mutex deadlock from link-time DT_NEEDED, see the block
 * comment above) -- libXrandr.so is not required for the rest of this file
 * to work, so its absence is handled gracefully (detect_dc_for_window
 * checks for NULL function pointers and returns -1, same as any other
 * detection failure), unlike libX11/libGL which are hard requirements. */
#define XRANDR_FUNCS(M) \
    M(XRRGetScreenResourcesCurrent, XRRScreenResources*, (Display *, Window)) \
    M(XRRFreeScreenResources,       void,                (XRRScreenResources *)) \
    M(XRRGetCrtcInfo,               XRRCrtcInfo*,        (Display *, XRRScreenResources *, RRCrtc)) \
    M(XRRFreeCrtcInfo,              void,                (XRRCrtcInfo *)) \
    M(XRRGetOutputInfo,             XRROutputInfo*,      (Display *, XRRScreenResources *, RROutput)) \
    M(XRRFreeOutputInfo,            void,                (XRROutputInfo *))

/* Only glViewport survives here -- everything else this macro used to
 * declare (texture/shader/buffer/program functions) belonged to the GL
 * blit-and-sample rendering path this file no longer has; see the
 * FLIP_TEST banner comment near the top of this file and README.md. */
#define GL_FUNCS(M) \
    M(glViewport,                void,    (GLint, GLint, GLsizei, GLsizei))

/* glXSwapBuffers is deliberately absent -- FLIP4 replaces it entirely, see
 * worker_thread_main. The rest are still needed for the minimal GLX
 * context this file keeps alive purely so glXWaitVideoSyncSGI (resolved
 * separately in resolve_gl_funcs) has a current drawable to query vblank
 * timing from. */
#define GLX_FUNCS(M) \
    M(glXChooseFBConfig,         GLXFBConfig*, (Display *, int, const int *, int *)) \
    M(glXGetVisualFromFBConfig,  XVisualInfo*, (Display *, GLXFBConfig)) \
    M(glXCreateNewContext,       GLXContext,   (Display *, GLXFBConfig, int, GLXContext, Bool)) \
    M(glXDestroyContext,         void,         (Display *, GLXContext)) \
    M(glXMakeCurrent,            Bool,         (Display *, GLXDrawable, GLXContext)) \
    M(glXGetProcAddressARB,      __GLXextFuncPtr, (const GLubyte *))

/* Generate function-pointer typedefs for each entry. */
#define DECL_TYPEDEF(name, ret, args) typedef ret (*PFN_##name)args;
X11_FUNCS(DECL_TYPEDEF)
GL_FUNCS (DECL_TYPEDEF)
GLX_FUNCS(DECL_TYPEDEF)
XRANDR_FUNCS(DECL_TYPEDEF)
#undef DECL_TYPEDEF

/* The pointer table, populated by lib_load(). */
typedef struct {
    void *handle_x11;
    void *handle_gl;
    void *handle_xrandr;
    void *handle_glx;
    bool  loaded;
#define DECL_FIELD(name, ret, args) PFN_##name name;
    X11_FUNCS(DECL_FIELD)
    GL_FUNCS (DECL_FIELD)
    GLX_FUNCS(DECL_FIELD)
    XRANDR_FUNCS(DECL_FIELD)
#undef DECL_FIELD
} LibTable;

static LibTable g_libs;
static pthread_mutex_t g_libs_lock = PTHREAD_MUTEX_INITIALIZER;

/* dlopen and resolve everything. Idempotent; safe to call from multiple
   threads. Called from CreateSwapchain (which is past the loader-mutex
   window that causes the deadlock if we'd been DT_NEEDED-linked). */
static bool lib_load(void) {
    pthread_mutex_lock(&g_libs_lock);
    if (g_libs.loaded) {
        pthread_mutex_unlock(&g_libs_lock);
        return true;
    }
    /* Open libraries. libX11 first (libGL pulls it transitively, but we want
       to control which version). RTLD_GLOBAL so GLX dispatch resolves any
       symbols it needs across the libraries. */
    g_libs.handle_x11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_libs.handle_x11) g_libs.handle_x11 = dlopen("libX11.so",   RTLD_LAZY | RTLD_GLOBAL);
    g_libs.handle_gl  = dlopen("libGL.so.1",  RTLD_LAZY | RTLD_GLOBAL);
    if (!g_libs.handle_gl)  g_libs.handle_gl  = dlopen("libGL.so",    RTLD_LAZY | RTLD_GLOBAL);
    g_libs.handle_glx = dlopen("libGLX.so.0", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_libs.handle_glx) g_libs.handle_glx = g_libs.handle_gl;  /* libGL provides glX* on most stacks */
    /* FLIP_TEST: libXrandr for DC auto-detection -- not a hard requirement,
     * missing it just means detect_dc_for_window() returns -1 (caller
     * falls back to its own default). */
    g_libs.handle_xrandr = dlopen("libXrandr.so.2", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_libs.handle_xrandr) g_libs.handle_xrandr = dlopen("libXrandr.so", RTLD_LAZY | RTLD_GLOBAL);

    if (!g_libs.handle_x11 || !g_libs.handle_gl) {
        fprintf(stderr, "[" "VK_LAYER_TEGRA_x11_present" "] lib_load: failed to dlopen libX11/libGL (%s)\n",
                dlerror());
        pthread_mutex_unlock(&g_libs_lock);
        return false;
    }

    /* Resolve each symbol. We try the GL handle first for GL/GLX, then GLX
       handle as fallback, then global. For X11 symbols, X11 handle. */
#define RESOLVE_X11(name, ret, args) \
    g_libs.name = (PFN_##name)dlsym(g_libs.handle_x11, #name); \
    if (!g_libs.name) g_libs.name = (PFN_##name)dlsym(RTLD_DEFAULT, #name);
#define RESOLVE_GL(name, ret, args) \
    g_libs.name = (PFN_##name)dlsym(g_libs.handle_gl, #name); \
    if (!g_libs.name) g_libs.name = (PFN_##name)dlsym(RTLD_DEFAULT, #name);
#define RESOLVE_GLX(name, ret, args) \
    g_libs.name = (PFN_##name)dlsym(g_libs.handle_glx, #name); \
    if (!g_libs.name) g_libs.name = (PFN_##name)dlsym(g_libs.handle_gl, #name); \
    if (!g_libs.name) g_libs.name = (PFN_##name)dlsym(RTLD_DEFAULT, #name);
#define RESOLVE_XRANDR(name, ret, args) \
    if (g_libs.handle_xrandr) g_libs.name = (PFN_##name)dlsym(g_libs.handle_xrandr, #name);
    X11_FUNCS(RESOLVE_X11)
    GL_FUNCS (RESOLVE_GL)
    GLX_FUNCS(RESOLVE_GLX)
    XRANDR_FUNCS(RESOLVE_XRANDR)
#undef RESOLVE_X11
#undef RESOLVE_GL
#undef RESOLVE_GLX
#undef RESOLVE_XRANDR

    /* Now that XInitThreads is resolved, call it before any other Xlib
       function fires. Xlib requires this to enable its internal locking
       when multiple threads use Xlib (our worker thread has its own
       Display, but Xlib's internal locking is still a process-wide thing).
       Has to be called before any other Xlib call in the process. */
    if (g_libs.XInitThreads) g_libs.XInitThreads();

    g_libs.loaded = true;
    pthread_mutex_unlock(&g_libs_lock);
    return true;
}

/* Source-level redirection: every callsite below that says glXXX(...) or
   XXX(...) for X11 functions becomes g_libs.glXXX(...) without source
   changes. */
#define DECL_REMAP(name, ret, args) static const PFN_##name name##_indirect = NULL; (void)name##_indirect;
/* The above is unused; what we really want is a per-name #define. */
#undef DECL_REMAP

#define REMAP(name, ret, args) static inline ret name args;
/* Also unused — we just use the literal macros below. */
#undef REMAP

#define XInitThreads              (g_libs.XInitThreads)
#define XOpenDisplay              (g_libs.XOpenDisplay)
#define XCloseDisplay             (g_libs.XCloseDisplay)
#define XInternAtom               (g_libs.XInternAtom)
#define XCreateColormap           (g_libs.XCreateColormap)
#define XCreateWindow             (g_libs.XCreateWindow)
#define XDestroyWindow            (g_libs.XDestroyWindow)
#define XFreeColormap             (g_libs.XFreeColormap)
#define XFree                     (g_libs.XFree)
#define XMapWindow                (g_libs.XMapWindow)
#define XGetGeometry              (g_libs.XGetGeometry)
#define XGetWindowAttributes      (g_libs.XGetWindowAttributes)
#define XResizeWindow             (g_libs.XResizeWindow)
#define XFlush                    (g_libs.XFlush)
#define XChangeProperty           (g_libs.XChangeProperty)
#define XTranslateCoordinates     (g_libs.XTranslateCoordinates)

#define XRRGetScreenResourcesCurrent (g_libs.XRRGetScreenResourcesCurrent)
#define XRRFreeScreenResources       (g_libs.XRRFreeScreenResources)
#define XRRGetCrtcInfo                (g_libs.XRRGetCrtcInfo)
#define XRRFreeCrtcInfo                (g_libs.XRRFreeCrtcInfo)
#define XRRGetOutputInfo              (g_libs.XRRGetOutputInfo)
#define XRRFreeOutputInfo             (g_libs.XRRFreeOutputInfo)

#define glViewport                (g_libs.glViewport)

#define glXChooseFBConfig         (g_libs.glXChooseFBConfig)
#define glXGetVisualFromFBConfig  (g_libs.glXGetVisualFromFBConfig)
#define glXCreateNewContext       (g_libs.glXCreateNewContext)
#define glXDestroyContext         (g_libs.glXDestroyContext)
#define glXMakeCurrent            (g_libs.glXMakeCurrent)
#define glXGetProcAddressARB      (g_libs.glXGetProcAddressARB)

/* The vendored vk_layer.h used to define VK_LAYER_EXPORT for us, but the
   modern Vulkan-Loader removed it in favor of the layer's own visibility
   control. Define it portably here. With -fvisibility=hidden in the
   Makefile, this attribute is what makes vkGetInstanceProcAddr,
   vkGetDeviceProcAddr, and vkNegotiateLoaderLayerInterfaceVersion the
   only externally visible symbols. */
#if defined(__GNUC__) && (__GNUC__ >= 4)
#  define VK_LAYER_EXPORT __attribute__((visibility("default")))
#else
#  define VK_LAYER_EXPORT
#endif

/* ----------------------------------------------------------------------- */
/* Configuration                                                           */
/* ----------------------------------------------------------------------- */

#define LAYER_NAME            "VK_LAYER_FLIP_test"
#define LAYER_VERSION         2
#define LAYER_DESC            "Tegra L4T r32.x Vulkan→GL X11 present relay"

/* Clamp image counts to a sane range. MIN_IMAGES is 3, not 2: empirically
 * (2026-08-17, vkgears -fullscreen, which requests 2) a 2-image swapchain
 * ties buffer reuse to a ~33ms/2-vblank gap, which isn't enough margin for
 * FLIP4's deferred kthread-queued flip to reliably finish before we start
 * overwriting that slot again, causing a real recurring tear. 3 images
 * (~50ms/3-vblank gap) fixed it with no other change. See README.md. */
#define MIN_IMAGES   3
#define MAX_IMAGES   8

/* Default image count if the app requests something outside the range. */
#define DEFAULT_IMAGES 3

/* ----------------------------------------------------------------------- */
/* Logging                                                                 */
/* ----------------------------------------------------------------------- */

static int g_log_level = 1;   /* 0=silent, 1=warn/err, 2=info, 3=debug */
static FILE *g_log_fp = NULL;

/* Diagnostic mode: if VK_TEGRA_X11_PRESENT_DIAG=1 is set, every QueueSubmit
   is followed by DeviceWaitIdle. This catches GPU faults at the offending
   submit instead of letting them propagate to a later submit returning
   DEVICE_LOST. It's catastrophically slow (every submit becomes synchronous)
   and is only meant for one-shot fault localization. */
static bool g_diag_wait_after_submit = false;

static void layer_log_init(void) {
    /* X11 threading initialization is deferred until lib_load() runs at
       CreateSwapchain time. We don't link libX11 directly anymore (see the
       big comment near the top of this file for the loader-deadlock
       rationale), so we can't call XInitThreads here. */

    const char *lvl = getenv("VK_TEGRA_X11_PRESENT_LOG");
    if (lvl) g_log_level = atoi(lvl);
    const char *diag = getenv("VK_TEGRA_X11_PRESENT_DIAG");
    if (diag && atoi(diag) == 1) {
        g_diag_wait_after_submit = true;
        fprintf(stderr, "[" LAYER_NAME "] DIAG MODE: DeviceWaitIdle after every submit (SLOW)\n");
    }
    const char *path = getenv("VK_TEGRA_X11_PRESENT_LOG_FILE");
    if (path) {
        g_log_fp = fopen(path, "a");
        if (g_log_fp) setvbuf(g_log_fp, NULL, _IONBF, 0);
    }
    /* Make stderr unbuffered so log lines survive abnormal exit. */
    setvbuf(stderr, NULL, _IONBF, 0);
}

static void layer_logv(int lvl, const char *prefix, const char *fmt, va_list ap) {
    if (lvl > g_log_level) return;
    char buf[2048];
    int n = snprintf(buf, sizeof(buf), "[" LAYER_NAME " %s] ", prefix);
    if (n > 0 && n < (int)sizeof(buf))
        vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    fputs(buf, stderr); fputc('\n', stderr);
    if (g_log_fp) { fputs(buf, g_log_fp); fputc('\n', g_log_fp); }
}

#define LOG_ERR(fmt, ...)   do { if (g_log_level >= 1) layer_log(1, "ERR",  fmt, ##__VA_ARGS__); } while (0)
#define LOG_WARN(fmt, ...)  do { if (g_log_level >= 1) layer_log(1, "WARN", fmt, ##__VA_ARGS__); } while (0)
#define LOG_INFO(fmt, ...)  do { if (g_log_level >= 2) layer_log(2, "info", fmt, ##__VA_ARGS__); } while (0)
#define LOG_DBG(fmt, ...)   do { if (g_log_level >= 3) layer_log(3, "dbg ", fmt, ##__VA_ARGS__); } while (0)

static void layer_log(int lvl, const char *prefix, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); layer_logv(lvl, prefix, fmt, ap); va_end(ap);
}

/* Log every non-success Vulkan result from a call our layer makes. */
#define VK_CHECK(call) ({ \
    VkResult _r = (call); \
    if (_r != VK_SUCCESS) layer_log(1, "VK", "%s:%d %s -> %d", __func__, __LINE__, #call, _r); \
    _r; \
})

/* ----------------------------------------------------------------------- */
/* Layer enable / disable                                                  */
/* ----------------------------------------------------------------------- */

static bool g_layer_disabled = false;

static void layer_check_disabled(void) {
    const char *d = getenv("VK_TEGRA_X11_PRESENT_DISABLE");
    if (d && d[0] == '1') {
        g_layer_disabled = true;
        LOG_INFO("layer disabled via VK_TEGRA_X11_PRESENT_DISABLE=1 (passthrough)");
    }
}

/* ----------------------------------------------------------------------- */
/* Dispatch tables                                                         */
/* ----------------------------------------------------------------------- */

typedef struct {
    /* From next layer / loader */
    PFN_vkGetInstanceProcAddr               GetInstanceProcAddr;
    PFN_vkDestroyInstance                   DestroyInstance;
    PFN_vkEnumeratePhysicalDevices          EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties       GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;

    /* WSI bits we override or pass through */
    PFN_vkCreateXlibSurfaceKHR              CreateXlibSurfaceKHR;
    PFN_vkCreateXcbSurfaceKHR               CreateXcbSurfaceKHR;
    PFN_vkDestroySurfaceKHR                 DestroySurfaceKHR;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR        GetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR   GetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR        GetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR   GetPhysicalDeviceSurfacePresentModesKHR;

    /* VK_KHR_get_surface_capabilities2 — newer "2" variants that take an
       extensible chain instead of plain output structs. PPSSPP and several
       other modern Vulkan apps prefer these. We have to intercept them too;
       otherwise the underlying driver answers based on its own (broken) WSI
       state, and the app gets a surface capabilities object that doesn't
       match what we report for the v1 variants. The result is the app
       configuring its swapchain for one set of capabilities and then trying
       to use it against a different set — typically a NULL deref on the
       framebuffer/image-view chain that depends on the mismatched format. */
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR  GetPhysicalDeviceSurfaceCapabilities2KHR;
    PFN_vkGetPhysicalDeviceSurfaceFormats2KHR       GetPhysicalDeviceSurfaceFormats2KHR;

    PFN_vkGetPhysicalDeviceImageFormatProperties2 GetPhysicalDeviceImageFormatProperties2;
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties GetPhysicalDeviceExternalSemaphoreProperties;
} InstanceDispatch;

typedef struct {
    PFN_vkGetDeviceProcAddr      GetDeviceProcAddr;
    PFN_vkDestroyDevice          DestroyDevice;
    PFN_vkDeviceWaitIdle         DeviceWaitIdle;
    PFN_vkGetDeviceQueue         GetDeviceQueue;
    PFN_vkQueueSubmit            QueueSubmit;
    PFN_vkQueueWaitIdle          QueueWaitIdle;

    PFN_vkCreateImage            CreateImage;
    PFN_vkDestroyImage           DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory         AllocateMemory;
    PFN_vkFreeMemory             FreeMemory;
    PFN_vkBindImageMemory        BindImageMemory;
    /* FLIP_TEST_GOB_PROBE: CPU-write a coordinate-revealing pattern
     * directly into the LINEAR flip_image's memory. */
    PFN_vkMapMemory              MapMemory;
    PFN_vkUnmapMemory            UnmapMemory;
    PFN_vkCreateSemaphore        CreateSemaphore;
    PFN_vkDestroySemaphore       DestroySemaphore;
    PFN_vkCreateFence            CreateFence;
    PFN_vkDestroyFence           DestroyFence;
    PFN_vkResetFences            ResetFences;
    PFN_vkWaitForFences          WaitForFences;
    PFN_vkGetFenceStatus         GetFenceStatus;

    PFN_vkGetMemoryFdKHR         GetMemoryFdKHR;
    PFN_vkGetSemaphoreFdKHR      GetSemaphoreFdKHR;
    PFN_vkCmdPipelineBarrier     CmdPipelineBarrier;
    PFN_vkEndCommandBuffer       EndCommandBuffer;
    /* FLIP_TEST: needed for the vkCmdCopyImage-based OPTIMAL->LINEAR
     * detile, replacing the GL blit + glReadPixels path. */
    PFN_vkCreateCommandPool      CreateCommandPool;
    PFN_vkDestroyCommandPool     DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers     FreeCommandBuffers;
    PFN_vkBeginCommandBuffer     BeginCommandBuffer;
    PFN_vkCmdCopyImage           CmdCopyImage;
    /* FLIP_TEST_SOLID_FILL: a solid, uniform color fill is invariant to any
     * tiling/swizzle byte-permutation -- lets us test whether BLOCKLINEAR
     * itself changes tearing behavior without needing to have cracked
     * NVIDIA's real tiling layout first. */
    PFN_vkCmdClearColorImage     CmdClearColorImage;
    /* FLIP_TEST_BLOCKLINEAR + FLIP_TEST_SOLID_FILL: the buffer/memory
     * aliasing fix -- see the flip_alias_buf comment on PerImage. */
    PFN_vkCreateBuffer              CreateBuffer;
    PFN_vkDestroyBuffer             DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkBindBufferMemory          BindBufferMemory;
    PFN_vkCmdFillBuffer             CmdFillBuffer;
    PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout;
    /* FLIP_TEST GOB-swizzle compute pipeline: converts the app's rendered
     * OPTIMAL image into a real block-linear byte buffer per-frame,
     * combining the verified tear-free BLOCKLINEAR path with correct
     * content instead of the uniform-fill proxy. */
    PFN_vkCreateShaderModule         CreateShaderModule;
    PFN_vkDestroyShaderModule        DestroyShaderModule;
    PFN_vkCreateDescriptorSetLayout  CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreatePipelineLayout       CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout      DestroyPipelineLayout;
    PFN_vkCreateComputePipelines     CreateComputePipelines;
    PFN_vkDestroyPipeline            DestroyPipeline;
    PFN_vkCreateDescriptorPool       CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool      DestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets     AllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets       UpdateDescriptorSets;
    PFN_vkCreateImageView            CreateImageView;
    PFN_vkDestroyImageView           DestroyImageView;
    PFN_vkCmdBindPipeline            CmdBindPipeline;
    PFN_vkCmdBindDescriptorSets      CmdBindDescriptorSets;
    PFN_vkCmdDispatch                CmdDispatch;
    PFN_vkCmdPushConstants           CmdPushConstants;
    PFN_vkCreateRenderPass       CreateRenderPass;
    /* v2 variants — core in Vulkan 1.2, also available as KHR extension.
       Vulkan 1.2 apps (e.g. Play PS2 emulator) use these instead of v1. */
    PFN_vkCreateRenderPass2      CreateRenderPass2;
    PFN_vkCreateRenderPass2      CreateRenderPass2KHR;

    /* The real WSI calls (we delegate format/presentmode queries to them
       sometimes, but otherwise we replace these completely). */
    PFN_vkCreateSwapchainKHR     CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR    DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR  GetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR    AcquireNextImageKHR;
    PFN_vkQueuePresentKHR        QueuePresentKHR;
} DeviceDispatch;

/* ----------------------------------------------------------------------- */
/* Per-instance and per-device state, looked up by dispatch key            */
/* ----------------------------------------------------------------------- */

#define HASH_BUCKETS 64

typedef struct InstNode {
    void *key;
    VkInstance instance;
    InstanceDispatch d;
    bool external_mem_caps;
    bool external_sem_caps;
    /* True when no NVIDIA physical device was found at CreateInstance time.
       All surface creation and WSI calls for this instance pass through to
       the ICD unchanged so the layer is fully transparent for non-NVIDIA
       instances (llvmpipe, Mesa, etc.). */
    bool passthrough;
    struct InstNode *next;
} InstNode;
static InstNode *g_inst_table[HASH_BUCKETS];
static pthread_mutex_t g_inst_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct DevNode {
    void *key;
    VkDevice device;
    VkPhysicalDevice phys;
    /* When true this is a non-NVIDIA device.  d.GetDeviceProcAddr holds
       next_gdpa so layer_GetDeviceProcAddr can return the next layer's
       functions, making the layer fully transparent for this device.
       Only CreateSwapchainKHR and DestroyDevice are still intercepted to
       handle cleanup of state created by layer_CreateXlibSurfaceKHR /
       layer_CreateXcbSurfaceKHR (the icd_surface in our Surface*). */
    bool passthrough;
    VkInstance inst;
    DeviceDispatch d;
    InstanceDispatch *idisp;     /* points into the InstNode for this device */
    VkPhysicalDeviceMemoryProperties memp;
    uint32_t graphics_qfi;
    VkQueue graphics_queue;      /* lazy-resolved */
    /* Queue submit serialization.

       VkQueue is "externally synchronized" — per the Vulkan spec, only one
       thread may call vkQueueSubmit on a given queue at a time. The app is
       responsible for that mutex, but our layer's own bridge submits in
       Acquire/Present also touch the queue. If the app has a separate render
       thread (PPSSPP does) that submits concurrently with our bridge, the
       Nvidia Tegra driver hits an internal race and the GPU faults — observed
       as DEVICE_LOST on subsequent submits.

       We serialize on a per-device mutex covering ALL submits — ours and the
       app's (through our layer_QueueSubmit wrapper). The app may have its
       own mutex too; that's fine, double-locking a single submit costs only
       a few ns. The point is that no submit from any source happens
       concurrently with another. */
    pthread_mutex_t submit_lock;
    int             submit_inflight;  /* DEBUG: should always be 0 or 1 */
    struct DevNode *next;
} DevNode;
static DevNode *g_dev_table[HASH_BUCKETS];
static pthread_mutex_t g_dev_lock = PTHREAD_MUTEX_INITIALIZER;

/* The Vulkan loader's "dispatch key" is the first sizeof(void*) bytes of
   the object — both VkInstance and VkDevice are dispatchable handles. */
/* The Vulkan loader's "dispatch key" is the first sizeof(void*) bytes of
   the dispatchable object — it points to the loader's per-{instance,device}
   dispatch table. ALL dispatchable handles derived from the same
   device share that pointer (the device, every VkQueue, every VkCommandBuffer
   from that device). We use it as a hash key so any of those handles can
   resolve back to our DevNode.

   Important: pass the handle itself, NOT &handle. The argument is a
   dispatchable handle (a pointer-typed value). We dereference it to read
   the first 8 bytes. */
static void *dispatch_key(const void *handle) {
    return *(void * const *)handle;
}

static unsigned bucket(void *k) {
    uintptr_t x = (uintptr_t)k;
    x ^= x >> 16; x *= 0x9E3779B1u; x ^= x >> 16;
    return (unsigned)(x & (HASH_BUCKETS - 1));
}

static InstNode *inst_lookup(void *key) {
    pthread_mutex_lock(&g_inst_lock);
    InstNode *n = g_inst_table[bucket(key)];
    while (n && n->key != key) n = n->next;
    pthread_mutex_unlock(&g_inst_lock);
    return n;
}
static void inst_insert(InstNode *n) {
    pthread_mutex_lock(&g_inst_lock);
    unsigned b = bucket(n->key); n->next = g_inst_table[b]; g_inst_table[b] = n;
    pthread_mutex_unlock(&g_inst_lock);
}
static void inst_remove(void *key) {
    pthread_mutex_lock(&g_inst_lock);
    unsigned b = bucket(key);
    InstNode **p = &g_inst_table[b];
    while (*p && (*p)->key != key) p = &(*p)->next;
    if (*p) { InstNode *n = *p; *p = n->next; free(n); }
    pthread_mutex_unlock(&g_inst_lock);
}

static DevNode *dev_lookup(void *key) {
    pthread_mutex_lock(&g_dev_lock);
    DevNode *n = g_dev_table[bucket(key)];
    while (n && n->key != key) n = n->next;
    pthread_mutex_unlock(&g_dev_lock);
    return n;
}
static void dev_insert(DevNode *n) {
    pthread_mutex_lock(&g_dev_lock);
    unsigned b = bucket(n->key); n->next = g_dev_table[b]; g_dev_table[b] = n;
    pthread_mutex_unlock(&g_dev_lock);
}
static void dev_remove(void *key) {
    pthread_mutex_lock(&g_dev_lock);
    unsigned b = bucket(key);
    DevNode **p = &g_dev_table[b];
    while (*p && (*p)->key != key) p = &(*p)->next;
    if (*p) { DevNode *n = *p; *p = n->next; free(n); }
    pthread_mutex_unlock(&g_dev_lock);
}

/* For queue->device lookup. Vulkan queues are dispatchable; their key is the
   parent device's. */

/* Serialized queue submit. Acquires dev->submit_lock, calls the driver's
   QueueSubmit, releases. This is the ONLY way our layer touches the queue —
   the app's submits go through layer_QueueSubmit which uses the same lock.

   The Vulkan spec requires external synchronization on the queue parameter
   to vkQueueSubmit: the application must ensure only one thread submits to
   a given queue at a time. We have to provide that synchronization for
   our own bridge submits AND for the app's submits going through our
   wrapper, because the app's render thread and our Acquire/Present can
   both end up on the same VkQueue. */
static VkResult queue_submit_locked(DevNode *dev, VkQueue queue,
                                    uint32_t submitCount,
                                    const VkSubmitInfo *pSubmits,
                                    VkFence fence) {
    pthread_mutex_lock(&dev->submit_lock);
#ifndef NDEBUG
    /* Sanity check: with the mutex held, exactly one thread should be in
       the critical section at a time. If this ever exceeds 1 the lock is
       broken. Production builds skip this check. */
    int n = __atomic_add_fetch(&dev->submit_inflight, 1, __ATOMIC_SEQ_CST);
    if (n != 1) {
        LOG_ERR("queue_submit_locked: %d threads inside lock simultaneously! "
                "(self=%lu queue=%p)",
                n, (unsigned long)pthread_self(), (void*)queue);
    }
#endif
    VkResult r = dev->d.QueueSubmit(queue, submitCount, pSubmits, fence);
#ifndef NDEBUG
    __atomic_sub_fetch(&dev->submit_inflight, 1, __ATOMIC_SEQ_CST);
#endif
    pthread_mutex_unlock(&dev->submit_lock);
    return r;
}

/* ----------------------------------------------------------------------- */
/* Surfaces                                                                */
/* ----------------------------------------------------------------------- */

typedef enum {
    SURF_XLIB,
    SURF_XCB,
} SurfaceKind;

typedef struct Surface {
    /* Magic value so we can recognize our own VkSurfaceKHR-shaped handles. */
    uint64_t magic;
    SurfaceKind kind;
    Display *dpy;            /* Xlib handle. For XCB-only apps, we open one ourselves. */
    bool owns_dpy;
    Window window;
    /* Real ICD-owned VkSurfaceKHR created alongside our wrapper.  Used
       when a non-NVIDIA device needs a valid ICD surface handle — the ICD
       does not know about our Surface* pointer.  Destroyed in
       DestroySurfaceKHR together with our wrapper. */
    VkSurfaceKHR icd_surface;
    VkInstance   icd_inst;
    /* The GLX context lives in the SwapchainData, not here, because it
       must match the GLXFBConfig of the rendering format and the app
       chooses format at swapchain creation time. */
} Surface;

#define SURFACE_MAGIC 0x53524654594c5253ULL  /* "SRFTYLSR" backwards-ish */

/* Cast a VkSurfaceKHR (non-dispatchable, 64-bit) into our Surface*. */
static Surface *as_surface(VkSurfaceKHR s) {
    Surface *p = (Surface *)(uintptr_t)s;
    if (!p || p->magic != SURFACE_MAGIC) return NULL;
    return p;
}

/* ----------------------------------------------------------------------- */
/* Swapchains                                                              */
/* ----------------------------------------------------------------------- */

typedef struct PerImage {
    VkImage          image;        /* app-facing OPTIMAL-tiled render target */
    VkDeviceMemory   memory;

    VkSemaphore      vk_render_done;  /* App's queue submit signals this when
                                         rendering into `image` is done; our
                                         copy command buffer waits on it. */
    VkSemaphore      gl_sample_done;  /* Signaled by our copy command buffer's
                                         submit once `image` is safe for the
                                         app to reuse; Vulkan waits on it at
                                         the next Acquire. Name kept from the
                                         real project this was copied from --
                                         no GL involved in signaling it here. */

    /* The separate LINEAR-tiled, exportable image that `image` above gets
     * vkCmdCopyImage'd into each frame (worker_thread_main) -- GPU-side
     * detile, no CPU readback. See create_flip_export_image. Exported
     * exactly once at swapchain creation; the same fd is reused as FLIP4's
     * buff_id every frame -- dma_buf_get() (what tegra_dc_ext_pin_window()
     * calls to resolve buff_id) takes the raw fd directly, no import step. */
    VkImage          flip_image;
    VkDeviceMemory   flip_memory;
    int              flip_fd;
    VkDeviceSize     flip_row_pitch;
    VkDeviceSize     flip_offset;
    VkCommandBuffer  flip_cmdbuf;
    VkFence          flip_fence;

    /* FLIP_TEST_BLOCKLINEAR + FLIP_TEST_SOLID_FILL: a VkBuffer aliased over
     * the exact same VkDeviceMemory as `image` (only when flip_blocklinear;
     * requires that allocation to be non-dedicated -- see create_app_image).
     * vkCmdFillBuffer operates on raw linear byte ranges, unlike
     * vkCmdClearColorImage which is bounded by the image's logical texel
     * addressing and provably cannot reach any tiling padding
     * VK_IMAGE_TILING_OPTIMAL's opaque layout might reserve beyond that --
     * exactly the gap that let stale memory bleed through in testing
     * (2026-08-16). FillBuffer's whole-allocation raw write closes it. */
    VkBuffer         flip_alias_buf;
    uint64_t         flip_last_ns; /* diagnostic: wall-clock time of the last
                                       FLIP4 using this slot's flip_image, to
                                       measure real reuse spacing (see
                                       worker_thread_main). 0 = never used. */

    /* FLIP_TEST_GOB_REAL: real per-frame content in the verified block-
     * linear layout (README.md, "GOB block-linear formula", 2026-08-16).
     * gob_dst_buf/gob_dst_mem/gob_dst_fd are the exported destination --
     * what actually gets handed to FLIP4 as buff_id. gob_src_view is a
     * view into `image` (the app's OPTIMAL-tiled render target) for the
     * compute shader to imageLoad from. gob_dset is this slot's descriptor
     * set (src view + dst buffer), allocated from Swapchain's shared
     * gob_dpool. */
    VkBuffer         gob_dst_buf;
    VkDeviceMemory   gob_dst_mem;
    int              gob_dst_fd;
    VkImageView      gob_src_view;
    VkDescriptorSet  gob_dset;

    /* Per-image fence: used by vkAcquireNextImageKHR to provide CPU-side
       backpressure. Without this, Acquire returns immediately and the app's
       render loop is unblocked by the layer, causing it to spin at maximum
       framerate while the worker is the one waiting on vsync. With it,
       Acquire blocks on the fence until our bridge submit (which itself
       waits on gl_sample_done) completes — same semantic as real WSI
       Acquire which blocks until a swapchain image is genuinely free. */
    VkFence          acquire_fence;

    /* Tracking the acquire state.

       Vulkan's swapchain model says an acquired image is either in the
       app's possession or in the presentation engine. We treat it as:
         - "acquired by app" between Acquire and Present
         - "in flight" between Present and the moment the copy command
           buffer's submit (worker_thread_main) completes
         - "free" once that submit's fence signals

       We use gl_sample_done as the gate: when the app calls Acquire and
       requests image idx, we make the app's acquire semaphore wait on
       gl_sample_done[idx] via a bridge submit. */
    bool             acquired;        /* currently held by app */
    bool             in_flight;       /* worker has copy/present work pending on it */
} PerImage;

typedef struct Swapchain {
    uint64_t magic;
    DevNode *dev;
    Surface *surf;

    /* Properties */
    uint32_t      image_count;
    VkExtent2D    extent;
    VkFormat      format;
    VkColorSpaceKHR color_space;
    VkPresentModeKHR present_mode;
    /* Usage flags the app requested for swapchain images. We honor these
       (OR'd with what we need ourselves) when creating our images. PPSSPP
       in particular asks for INPUT_ATTACHMENT_BIT for its subpass effects;
       if we don't provide it, the app's later render passes / pipelines
       are valid at creation time but the GPU faults on use. */
    VkImageUsageFlags image_usage;

    PerImage      images[MAX_IMAGES];

    /* X / GLX resources owned by this swapchain.

       GLX requires the drawable's visual to match the GLX context's
       FBConfig visual. The application's surface window was created
       without GLX in mind and almost certainly has a visual that no GLX
       FBConfig matches — glXMakeCurrent on it returns BadMatch. We work
       around this by creating a child X window inside the app's window
       with our own chosen visual, sized to match the parent, and using
       that as the GLX drawable. The app never sees it; the X server
       composites it into the parent's area automatically. */
    GLXFBConfig   fbcfg;
    XVisualInfo  *visinfo;
    Colormap      child_colormap;
    Window        child_window;     /* the actual GLX drawable */
    GLXContext    glctx;
    bool          glctx_owned;
    /* The parent X window's actual size, refreshed each present from
       XGetGeometry. We resize the child window to match when it changes. */
    int           win_w, win_h;

    /* Worker's own X Display connection.

       Xlib is not thread-safe to share a single Display* across threads
       even with XInitThreads — the internal sequencer asserts if two
       threads make X requests concurrently. Our main thread uses
       sc->surf->dpy (the surface's Display) for swapchain setup and
       teardown; the worker thread uses its own dedicated connection
       (worker_dpy). The Window XID is a server-side ID and can be
       safely referenced from either connection. The GLX context is also
       a server-side object and can be made-current on the worker's
       connection. */
    Display      *worker_dpy;

    /* GLX_SGI_video_sync entrypoints. When available, the worker uses
       glXWaitVideoSyncSGI to block on the actual hardware vblank instead
       of relying on glXSwapBuffers to do so. NVIDIA's Tegra L4T r32.x
       implements glXSwapBuffers's vsync wait as a sched_yield() spin loop
       that pins the worker thread at ~100% CPU. glXWaitVideoSyncSGI does
       a real kernel-side vblank wait (DRM_IOCTL_WAIT_VBLANK or similar),
       so the thread actually sleeps. Pattern lifted from KWin's
       SGIVideoSyncVsyncMonitor — see
       plugins/platforms/x11/standalone/glxbackend.cpp in the KWin tree. */
    int  (*glXGetVideoSyncSGI )(unsigned int *count);
    int  (*glXWaitVideoSyncSGI)(int divisor, int remainder, unsigned int *count);

    /* Acquire ring */
    uint32_t      next_acquire;       /* round-robin starting point */

    /* Per-swapchain command pool for our bridge submits. */
    VkCommandPool  cpool;

    /* Async GL worker.

       Architecture: a dedicated thread owns the GLX context — made current
       once at thread start, never un-made until shutdown. This avoids the
       per-present cost of glXMakeCurrent and decouples the app's render
       thread from glXSwapBuffers's vsync wait. On Nvidia, glXSwapBuffers
       with swap interval >= 1 spin-waits on the CPU side until vblank;
       running it on a dedicated thread means it doesn't burn the app's
       core. The app thread submits a job and returns immediately.

       Pending slot: a single image-index awaiting present, plus a flag.
       This single-slot design is the backpressure mechanism — if the
       worker hasn't finished the last present when the app calls Present
       again, the app blocks in the post until the slot frees. With NIMG=3
       images and triple-buffered render, normal usage stays fully
       pipelined. */
    pthread_t        worker;
    bool             worker_running;
    pthread_mutex_t  worker_lock;
    pthread_cond_t   worker_cv_pending;   /* worker waits on this when idle */
    pthread_cond_t   worker_cv_done;      /* app waits on this when posting to full slot */
    bool             worker_pending;      /* slot has work? */
    uint32_t         worker_pending_idx;  /* image index in pending slot */
    /* VK_GOOGLE_display_timing: presentID and desiredPresentTime supplied
       by the app for the pending work (zero if not set). The worker
       captures actualPresentTime when the SGI vblank wait returns and
       pushes a history entry. */
    uint32_t         worker_pending_present_id;
    uint64_t         worker_pending_desired_ns;
    bool             worker_quit;

    /* Display-timing history ring, written by the worker after each
       vblank, read by vkGetPastPresentationTimingGOOGLE. The lock
       protects head/tail/count; the contents themselves are pure
       value-copies so no aliasing concern.

       64 entries is generous — the spec just requires us to remember
       "the most recent" presents; apps that drain regularly never see
       it fill up. If full, the oldest entry is overwritten — apps that
       don't poll lose old history but always see recent. */
    pthread_mutex_t  timing_lock;
    uint32_t         timing_count;        /* number of valid entries */
    uint32_t         timing_head;         /* index of oldest entry */
    VkPastPresentationTimingGOOGLE timing_ring[64];
    /* Measured refresh duration in nanoseconds. Filled in at swapchain
       create time from one observed inter-vblank interval, falling back
       to a conservative 60 Hz if measurement fails. */
    uint64_t         refresh_duration_ns;

    /* FLIP_TEST: direct FLIP4 present backend state (no-GL version). */
    int      flip_dc_fd;
    int      flip_win_index; /* FLIP_TEST_WIN, default FLIP_TEST_WIN_INDEX --
                                 see the comment where flip_dc_fd is opened */
    VkCommandPool flip_cpool;
    uint32_t flip_out_w, flip_out_h; /* clamped to fit the screen, 1:1 */
    bool     flip_ready;

    /* FLIP_TEST Option 1: kernel-side pre_syncpt_id wait. When enabled
       (FLIP_TEST_KERNEL_WAIT=1), FLIP4 itself blocks inside the kernel's
       deferred flip_worker on the DC's real vblank syncpoint before
       latching the buffer, instead of the worker thread doing a userspace
       glXWaitVideoSyncSGI wait beforehand. flip_nvhost_ctrl_fd is a handle
       to /dev/nvhost-ctrl, used once per frame to read the syncpoint's
       current value (NVHOST_IOCTL_CTRL_SYNCPT_READ) so we can compute the
       "next vblank" target (current + 1) to hand to FLIP4 as pre_syncpt_val. */
    bool     flip_kernel_wait;
    int      flip_nvhost_ctrl_fd;
    __u32    flip_vblank_syncpt_id;

    /* FLIP_TEST Option 1b: fully GLX-free pacing. glXWaitVideoSyncSGI is a
       software sleep woken by the scheduler, not an interrupt-precise
       primitive -- FLIP_TEST_KERNEL_PACE=<N> replaces it with a genuinely
       blocking wait on the DC's own hardware vblank syncpoint
       (NVHOST_IOCTL_CTRL_SYNCPT_WAITEX on /dev/nvhost-ctrl, which -- unlike
       FLIP4 -- really does block the calling thread until the real IRQ-
       driven syncpoint reaches the target), removing GLX from the pacing
       path entirely. N=1 paces to every vblank (~60fps); N=2 to every other
       (~30fps), testing whether more margin changes tearing. When active,
       FLIP4's own pre_syncpt_id is left at "invalid" -- submission is
       already precisely timed by the wait, so gating the latch again would
       just add another vblank of pure delay on top for no purpose. */
    bool     flip_kernel_pace;
    uint32_t flip_pace_divisor;
    __u32    flip_pace_target;

    /* FLIP_TEST_BLOCKLINEAR=1: kprobe capture of NVIDIA's own tear-free
       fullscreen FLIP4 calls (2026-08-16) showed win->flags=0x20
       (TEGRA_DC_EXT_FLIP_FLAG_BLOCKLINEAR) and block_height_log2=4 -- their
       scanout buffer is GPU-native block-linear tiled, not pitch-linear.
       Every earlier FLIP4 test in this prototype used a separate
       VK_IMAGE_TILING_LINEAR detile target (create_flip_export_image),
       which this hardware's DC may simply lack real atomic double-buffering
       for. This mode skips that detile step entirely and exports the app's
       own VK_IMAGE_TILING_OPTIMAL image directly (create_app_image adds
       the export capability when this is set), flipping it with
       TEGRA_DC_EXT_FLIP_FLAG_BLOCKLINEAR + block_height_log2=4 set to
       match. See worker_thread_main and the FLIP4 windowattr construction. */
    bool     flip_blocklinear;

    /* FLIP_TEST_SOLID_FILL=1: replaces the app's actual rendered content
       with our own alternating solid-color fill before FLIP4, on whichever
       image is actually being flipped (pi->image if flip_blocklinear,
       pi->flip_image otherwise). A uniform fill reads back correctly under
       ANY tiling/swizzle permutation, since every byte in the buffer holds
       the same value -- this isolates "does BLOCKLINEAR change tearing
       behavior" from "did we get NVIDIA's exact tiling layout right",
       which the visual corruption from FLIP_TEST_BLOCKLINEAR alone made
       impossible to tell apart. Alternates a fully opaque red/blue full-
       screen clear each present; a real tear shows as a split red/blue
       frame, visible regardless of tiling. Works with or without
       flip_blocklinear, for an apples-to-apples LINEAR vs BLOCKLINEAR
       tearing comparison under identical (trivial) content. */
    bool     flip_solid_fill;
    uint32_t flip_solid_fill_counter;

    /* FLIP_TEST_GOB_PROBE=<1|2>: empirically derive the DC's real
       BLOCKLINEAR address permutation instead of guessing it. Uses the
       known-good LINEAR flip_image path (CPU-mapped, HOST_VISIBLE|
       HOST_COHERENT memory -- see create_flip_export_image), written once,
       then flipped every frame while telling FLIP4 this LINEAR buffer is
       BLOCKLINEAR (flags + block_height_log2, same knob as
       flip_blocklinear). Mode 1: raw diagnostic pattern (coordinate-
       revealing, no assumed swizzle) -- whatever shows up on screen
       reveals the permutation empirically. Mode 2: writes a candidate
       swizzle (gob_swizzle()) ourselves at write time, then displays a
       plain (x,y) gradient -- if the candidate formula matches the DC's
       real one, the BLOCKLINEAR-flagged read should show a CORRECT, clean
       gradient (proof the formula works), not scrambled data. Mutually
       exclusive with flip_blocklinear/flip_solid_fill in practice (uses
       its own no-op content path in worker_thread_main). */
    int      flip_gob_probe;

    /* FLIP_TEST_GOB_REAL=1: renders real per-frame content through the
     * verified GOB block-linear compute shader (gob_swizzle.comp) instead
     * of the LINEAR detile copy or the uniform-fill causality proxy.
     * Shared pipeline objects (one set for the whole swapchain); per-image
     * descriptor sets/buffers live on PerImage (see its gob_* fields). */
    bool                  flip_gob_real;
    VkDescriptorSetLayout gob_dsl;
    VkPipelineLayout      gob_pipeline_layout;
    VkPipeline            gob_pipeline;
    VkDescriptorPool      gob_dpool;
    long                  gob_block_height_log2; /* cached FLIP_TEST_BLOCKHEIGHT_LOG2, default 4 */

    pthread_mutex_t lock;
} Swapchain;

#define SWAPCHAIN_MAGIC 0x5357415043484149ULL    /* "SWAPCHAI" — 8 bytes, fits uint64_t */

static Swapchain *as_swapchain(VkSwapchainKHR s) {
    Swapchain *p = (Swapchain *)(uintptr_t)s;
    if (!p || p->magic != SWAPCHAIN_MAGIC) return NULL;
    return p;
}

static void track_swapchain(Swapchain *sc);
static void untrack_swapchain(Swapchain *sc);
/* Forward-declared so layer_CreateSwapchainKHR can call it for
   ci->oldSwapchain cleanup (recreation handling) before its own definition
   later in the file. */
VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL
layer_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                           const VkAllocationCallbacks *pAlloc);

/* ----------------------------------------------------------------------- */
/* GLX helpers                                                             */
/* ----------------------------------------------------------------------- */

/* Choose an FBConfig matching the requested swapchain format. We always
   double-buffer; depth is irrelevant since GL renders only a textured quad.
   We need an FBConfig whose visual matches the X window's visual, but
   that's a problem the app already solved at window creation time — the
   window has SOME visual, and we ask GLX to find a doublebuffered RGBA
   FBConfig of equivalent depth. */
/* Pick a doublebuffered RGBA GLX framebuffer config. We prefer one whose
   X visual depth is 32 — that's the ARGB visual that compositors will
   alpha-blend with the desktop. With a 24-bit visual the X server treats
   the window as opaque regardless of how much alpha we render into it,
   which manifests as black borders around apps that use CSD shadows
   (Chromium, GTK3/4 client-side-decorated apps, Electron, etc.). If a
   32-bit visual isn't available we accept any RGBA FBConfig and let the
   compositor render the window opaquely.

   parent_depth, if nonzero, is a preference toward matching the X parent
   window's visual depth — if the app's window is depth 24 there's no
   compositor blending to preserve and the simpler/faster 24-bit visual
   is fine. */
static GLXFBConfig pick_fbconfig(Display *dpy, int screen, VkFormat fmt,
                                 int parent_depth, XVisualInfo **out_vi) {
    int red=8, green=8, blue=8, alpha=8;
    /* SRGB needs GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB. */
    bool want_srgb = (fmt == VK_FORMAT_R8G8B8A8_SRGB || fmt == VK_FORMAT_B8G8R8A8_SRGB);

    int attrs[32]; int n = 0;
    attrs[n++] = GLX_X_RENDERABLE;     attrs[n++] = True;
    attrs[n++] = GLX_DRAWABLE_TYPE;    attrs[n++] = GLX_WINDOW_BIT;
    attrs[n++] = GLX_RENDER_TYPE;      attrs[n++] = GLX_RGBA_BIT;
    attrs[n++] = GLX_RED_SIZE;         attrs[n++] = red;
    attrs[n++] = GLX_GREEN_SIZE;       attrs[n++] = green;
    attrs[n++] = GLX_BLUE_SIZE;        attrs[n++] = blue;
    attrs[n++] = GLX_ALPHA_SIZE;       attrs[n++] = alpha;
    attrs[n++] = GLX_DEPTH_SIZE;       attrs[n++] = 0;
    attrs[n++] = GLX_DOUBLEBUFFER;     attrs[n++] = True;
    if (want_srgb) { attrs[n++] = GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB; attrs[n++] = True; }
    attrs[n++] = None;

    int nfb = 0;
    GLXFBConfig *fbs = glXChooseFBConfig(dpy, screen, attrs, &nfb);
    if (!fbs || nfb == 0) {
        /* Retry without SRGB if that was the only constraint failing. */
        if (want_srgb) {
            int j = 0;
            while (attrs[j] != GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB && attrs[j] != None) j++;
            if (attrs[j] != None) { attrs[j] = None; }
            fbs = glXChooseFBConfig(dpy, screen, attrs, &nfb);
        }
    }
    if (!fbs || nfb == 0) return NULL;

    /* Search returned FBConfigs for one whose visual matches the parent
       window's depth (if specified) or is depth 32 (preferred for
       compositor alpha blending). Fall back to the first FBConfig if no
       match. */
    GLXFBConfig pick = fbs[0];
    XVisualInfo *pick_vi = glXGetVisualFromFBConfig(dpy, pick);
    int want_depth = (parent_depth == 32 || parent_depth == 24) ? parent_depth : 32;
    for (int i = 0; i < nfb; i++) {
        XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, fbs[i]);
        if (!vi) continue;
        if (vi->depth == want_depth) {
            if (pick_vi) XFree(pick_vi);
            pick = fbs[i];
            pick_vi = vi;
            break;
        }
        XFree(vi);
    }
    /* If nothing matched our preferred depth, fall back to ANY 32-bit
       visual — the compositor case is more important than matching the
       parent's depth exactly. */
    if (pick_vi && pick_vi->depth != want_depth && want_depth != 32) {
        for (int i = 0; i < nfb; i++) {
            XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, fbs[i]);
            if (!vi) continue;
            if (vi->depth == 32) {
                XFree(pick_vi);
                pick = fbs[i];
                pick_vi = vi;
                break;
            }
            XFree(vi);
        }
    }
    *out_vi = pick_vi;
    XFree(fbs);
    return pick;
}

static GLXContext create_glx_context(Display *dpy, GLXFBConfig fbc) {
    PFNGLXCREATECONTEXTATTRIBSARBPROC f =
        (PFNGLXCREATECONTEXTATTRIBSARBPROC)glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
    if (f) {
        int a[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
            GLX_CONTEXT_MINOR_VERSION_ARB, 3,
            GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
            None
        };
        GLXContext c = f(dpy, fbc, NULL, True, a);
        if (c) return c;
    }
    return glXCreateNewContext(dpy, fbc, GLX_RGBA_TYPE, NULL, True);
}

/* Worker thread main loop. Owns the GLX context for the swapchain's lifetime;
   the context is made current once at thread start and never released until
   the thread exits. The thread waits on worker_cv_pending for work, processes
   one frame at a time, and signals worker_cv_done when the slot is free.

   The worker opens its OWN X Display* connection (sc->worker_dpy) and uses
   that for all X operations. Sharing an Xlib Display* across threads is
   unsafe even with XInitThreads — the internal sequencer asserts. By giving
   the worker its own connection, X requests from both threads are
   independent. The Window and GLX context are server-side objects and can
   be referenced from any connection. */
static void *worker_thread_main(void *arg) {
    Swapchain *sc = (Swapchain *)arg;

    /* Open the worker's own X connection. The Window XID we're going to
       render into is the same; only the connection differs. */
    sc->worker_dpy = XOpenDisplay(NULL);
    if (!sc->worker_dpy) {
        LOG_ERR("worker_thread_main: XOpenDisplay failed");
        pthread_mutex_lock(&sc->worker_lock);
        sc->worker_running = false;
        pthread_cond_broadcast(&sc->worker_cv_done);
        pthread_mutex_unlock(&sc->worker_lock);
        return NULL;
    }

    if (!glXMakeCurrent(sc->worker_dpy, sc->child_window, sc->glctx)) {
        LOG_ERR("worker_thread_main: glXMakeCurrent failed at startup");
        XCloseDisplay(sc->worker_dpy); sc->worker_dpy = NULL;
        pthread_mutex_lock(&sc->worker_lock);
        sc->worker_running = false;
        pthread_cond_broadcast(&sc->worker_cv_done);
        pthread_mutex_unlock(&sc->worker_lock);
        return NULL;
    }

    /* No glXSwapIntervalEXT call needed -- glXSwapBuffers is never called
       in this design (FLIP4 replaces it entirely); only glXWaitVideoSyncSGI
       is used from this context, purely for vblank timing, and it doesn't
       consult the swap interval. */
    int last_win_w = sc->win_w, last_win_h = sc->win_h;
    glViewport(0, 0, last_win_w, last_win_h);

    /* Tracks the previous vblank wall-clock time for refresh-duration
       measurement. Zero on the first iteration; set after each vsync
       wait. Local because the worker thread runs the same loop for the
       swapchain's lifetime. */
    uint64_t prev_vblank_ns = 0;

    for (;;) {
        /* Wait for work or shutdown. */
        pthread_mutex_lock(&sc->worker_lock);
        while (!sc->worker_pending && !sc->worker_quit)
            pthread_cond_wait(&sc->worker_cv_pending, &sc->worker_lock);
        if (sc->worker_quit) {
            pthread_mutex_unlock(&sc->worker_lock);
            break;
        }
        uint32_t idx = sc->worker_pending_idx;
        uint32_t present_id = sc->worker_pending_present_id;
        uint64_t desired_ns = sc->worker_pending_desired_ns;
        pthread_mutex_unlock(&sc->worker_lock);

        /* Refresh window size if changed. Cheap when unchanged. Use the
           worker's own Display* for these X calls. */
        uint32_t ww = 0, wh = 0;
        Window root; int wx, wy; unsigned int wb, wd;
        if (XGetGeometry(sc->worker_dpy, sc->surf->window, &root, &wx, &wy, &ww, &wh, &wb, &wd)) {
            if ((int)ww != last_win_w || (int)wh != last_win_h) {
                XResizeWindow(sc->worker_dpy, sc->child_window, ww, wh);
                XFlush(sc->worker_dpy);
                last_win_w = (int)ww; last_win_h = (int)wh;
                glViewport(0, 0, last_win_w, last_win_h);
            }
        }

        {
            /* FLIP_TEST (no-GL): pure Vulkan. One command buffer does the
             * whole job -- barrier the app's OPTIMAL image to a copy
             * source, vkCmdCopyImage it (GPU-side detile, no CPU
             * readback, no GL top/bottom flip convention to fight) into
             * our separate LINEAR exportable image, barrier the app's
             * image back to the layout the app/layer's own barrier
             * rewriting expects it in between uses, then present via
             * FLIP4. Waits on vk_render_done and signals gl_sample_done
             * directly as plain Vulkan semaphores -- no cross-API import
             * needed since we never leave Vulkan. */
            DeviceDispatch *d = &sc->dev->d;
            PerImage *pi = &sc->images[idx];

            /* Diagnostic: how much real time elapsed since this specific
             * slot's flip_image was last used, i.e. how much margin exists
             * before we overwrite a buffer that might still be what the
             * display is scanning out. Logged for the first several
             * reuses of each slot only, to avoid flooding. */
            {
                struct timespec _ts; clock_gettime(CLOCK_MONOTONIC, &_ts);
                uint64_t _now = (uint64_t)_ts.tv_sec * 1000000000ULL + (uint64_t)_ts.tv_nsec;
                static int _reuse_log_count = 0;
                if (pi->flip_last_ns != 0 && _reuse_log_count < 20) {
                    LOG_INFO("FLIP_TEST: slot idx=%u reuse gap = %.2f ms",
                             idx, (double)(_now - pi->flip_last_ns) / 1e6);
                    _reuse_log_count++;
                }
                pi->flip_last_ns = _now;
            }

            d->ResetFences(sc->dev->device, 1, &pi->flip_fence);

            VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            d->BeginCommandBuffer(pi->flip_cmdbuf, &cbbi);

            if (sc->flip_gob_real) {
                /* FLIP_TEST_GOB_REAL: dispatch gob_swizzle.comp to convert
                   this frame's rendered content (pi->image, OPTIMAL) into
                   the real block-linear layout (pi->gob_dst_buf). The
                   app's own rendering already left pi->image in
                   SHADER_READ_ONLY_OPTIMAL (per the layer's normal barrier
                   rewriting); transition to GENERAL for the storage-image
                   read, dispatch, transition back. */
                VkImageMemoryBarrier to_general = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                to_general.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                to_general.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                to_general.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_general.image = pi->image;
                to_general.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                d->CmdPipelineBarrier(pi->flip_cmdbuf, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);

                d->CmdBindPipeline(pi->flip_cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, sc->gob_pipeline);
                d->CmdBindDescriptorSets(pi->flip_cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                                         sc->gob_pipeline_layout, 0, 1, &pi->gob_dset, 0, NULL);
                uint32_t pc[4] = {
                    sc->extent.width, sc->extent.height,
                    (uint32_t)sc->gob_block_height_log2,
                    (sc->extent.width * 4 + 63) / 64, /* round up -- must match
                        create_gob_dest's gobs_wide and pi->flip_row_pitch */
                };
                d->CmdPushConstants(pi->flip_cmdbuf, sc->gob_pipeline_layout,
                                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
                d->CmdDispatch(pi->flip_cmdbuf,
                               (sc->extent.width + 15) / 16, (sc->extent.height + 7) / 8, 1);

                VkImageMemoryBarrier to_shader = to_general;
                to_shader.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                to_shader.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                d->CmdPipelineBarrier(pi->flip_cmdbuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &to_shader);
            } else if (sc->flip_solid_fill) {
                /* FLIP_TEST_SOLID_FILL: overwrite whichever image actually
                   gets flipped with a full-screen solid color, alternating
                   each present, ignoring the app's real content entirely.
                   A uniform fill reads back correctly under ANY tiling/
                   swizzle permutation, so this tests tearing itself --
                   independent of flip_blocklinear's tiling-correctness
                   problem. Toggle only once every N frames (default 20,
                   ~0.33s at 60fps) instead of every frame -- an every-frame
                   toggle is too fast to consciously register as anything
                   but flicker, which was the exact problem reported
                   watching it live. FLIP_TEST_SOLID_FILL_PERIOD overrides. */
                static long period = -1;
                if (period < 0) {
                    const char *e = getenv("FLIP_TEST_SOLID_FILL_PERIOD");
                    period = e ? atol(e) : 20;
                    if (period < 1) period = 1;
                    LOG_INFO("FLIP_TEST: solid fill toggles every %ld frames", period);
                }
                uint32_t parity = (sc->flip_solid_fill_counter++ / (uint32_t)period) & 1;

                if (sc->flip_blocklinear) {
                    /* Raw byte-level fill via flip_alias_buf, aliased over
                       the exact same memory as pi->image (see its comment
                       on PerImage) -- reaches every physical byte of the
                       allocation, including any tiling padding
                       vkCmdClearColorImage provably cannot touch (that gap
                       is what let stale memory bleed through when this
                       used ClearColorImage on pi->image directly, tested
                       2026-08-16). No barriers needed: nothing else touches
                       this memory earlier in this same command buffer, and
                       the only consumer is the DC (external to Vulkan),
                       already ordered via the fence + FLIP4 ioctl below. */
                    uint32_t pattern = parity ? 0xFF0000FFu /* blue-ish, byte order irrelevant here */
                                               : 0xFFFF0000u /* red-ish */;
                    d->CmdFillBuffer(pi->flip_cmdbuf, pi->flip_alias_buf, 0, VK_WHOLE_SIZE, pattern);
                } else {
                    /* LINEAR path: already tested clean with a plain
                       vkCmdClearColorImage (no bleed-through observed), so
                       no need for the buffer-alias approach here. */
                    VkImageMemoryBarrier to_dst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    to_dst.image = pi->flip_image;
                    to_dst.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    d->CmdPipelineBarrier(pi->flip_cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);

                    VkClearColorValue color = parity
                        ? (VkClearColorValue){{ 0.0f, 0.0f, 1.0f, 1.0f }}   /* blue */
                        : (VkClearColorValue){{ 1.0f, 0.0f, 0.0f, 1.0f }};  /* red */
                    d->CmdClearColorImage(pi->flip_cmdbuf, pi->flip_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          &color, 1, &to_dst.subresourceRange);

                    VkImageMemoryBarrier to_general = to_dst;
                    to_general.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    to_general.dstAccessMask = 0;
                    to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                    d->CmdPipelineBarrier(pi->flip_cmdbuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);
                }
            } else if (sc->flip_gob_probe) {
                /* FLIP_TEST_GOB_PROBE: pi->flip_image was written once with
                   the test pattern in create_flip_export_image and must
                   stay untouched -- nothing to record here, same reasoning
                   as the flip_blocklinear no-op case below. */
            } else if (!sc->flip_blocklinear) {
                /* FLIP_TEST_BLOCKLINEAR without solid fill: pi->image itself
                   is what gets flipped (exported directly by
                   create_app_image) -- no detile copy needed, nothing to
                   record here. The empty command buffer is still submitted
                   below purely for its wait/signal/fence timing, identical
                   to the non-blocklinear path. */
                VkImageMemoryBarrier to_src = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                to_src.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                to_src.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_src.image = pi->image;
                to_src.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                d->CmdPipelineBarrier(pi->flip_cmdbuf, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);

                VkImageCopy region = {0};
                region.srcSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.dstSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.extent = (VkExtent3D){ sc->extent.width, sc->extent.height, 1 };
                d->CmdCopyImage(pi->flip_cmdbuf,
                                pi->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                pi->flip_image, VK_IMAGE_LAYOUT_GENERAL,
                                1, &region);

                VkImageMemoryBarrier to_shader = to_src;
                to_shader.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                d->CmdPipelineBarrier(pi->flip_cmdbuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &to_shader);
            }

            d->EndCommandBuffer(pi->flip_cmdbuf);

            VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &pi->vk_render_done;
            si.pWaitDstStageMask = &wait_stage;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &pi->flip_cmdbuf;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &pi->gl_sample_done;

            VkResult sr = queue_submit_locked(sc->dev, sc->dev->graphics_queue, 1, &si, pi->flip_fence);
            if (sr != VK_SUCCESS) {
                LOG_WARN("FLIP_TEST: copy QueueSubmit failed: %d", sr);
            } else {
                d->WaitForFences(sc->dev->device, 1, &pi->flip_fence, VK_TRUE, UINT64_MAX);
            }

            __u32 kernel_wait_syncpt_val = 0;
            bool  kernel_wait_ok = false;

            if (sc->flip_kernel_pace) {
                /* FLIP_TEST Option 1b: fully GLX-free pacing. Unlike FLIP4,
                 * this ioctl really does block the calling thread --
                 * nvhost_ioctl_ctrl_syncpt_waitex() (host1x.c) calls
                 * nvhost_syncpt_wait_timeout() synchronously and returns
                 * only once the real IRQ-driven syncpoint reaches
                 * flip_pace_target (or the timeout below elapses). No SGI,
                 * no software sleep -- the wakeup is the same hardware
                 * event that increments the syncpoint. 1000ms timeout is
                 * just a safety backstop (normal case: every ~16.6ms); a
                 * timeout here would mean vblank interrupts themselves
                 * stopped, which the log makes visible instead of hanging
                 * forever. */
                struct nvhost_ctrl_syncpt_waitex_args wa = {0};
                wa.id = sc->flip_vblank_syncpt_id;
                wa.thresh = sc->flip_pace_target;
                wa.timeout = 1000;
                struct timespec _wa_t0; clock_gettime(CLOCK_MONOTONIC, &_wa_t0);
                int wr = ioctl(sc->flip_nvhost_ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_WAITEX, &wa);
                struct timespec _wa_t1; clock_gettime(CLOCK_MONOTONIC, &_wa_t1);
                static int _wa_log_count = 0;
                if (_wa_log_count < 600) {
                    double _wa_ms = (_wa_t1.tv_sec - _wa_t0.tv_sec) * 1000.0 +
                                     (_wa_t1.tv_nsec - _wa_t0.tv_nsec) / 1e6;
                    LOG_INFO("FLIP_TEST: WAITEX target=%u ret=%d value=%u took=%.3fms",
                             wa.thresh, wr, wa.value, _wa_ms);
                    _wa_log_count++;
                }
                if (wr < 0) {
                    LOG_WARN("FLIP_TEST: SYNCPT_WAITEX failed: %m -- resyncing target");
                    struct nvhost_ctrl_syncpt_read_args rd = { .id = sc->flip_vblank_syncpt_id };
                    if (ioctl(sc->flip_nvhost_ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &rd) == 0)
                        sc->flip_pace_target = rd.value + sc->flip_pace_divisor;
                } else {
                    sc->flip_pace_target += sc->flip_pace_divisor;
                }
                /* Submission is already precisely timed by the wait above;
                 * FLIP4's own pre_syncpt_id stays invalid (kernel_wait_ok
                 * stays false) -- gating the latch again would just add
                 * another vblank of pure delay on top for no purpose. */
            } else {
                /* Wait for vblank HERE, right before FLIP4, instead of
                 * after -- gives the driver maximum lead time before the
                 * *next* vblank to latch the new buffer, instead of
                 * calling FLIP4 at an arbitrary phase (whenever the copy
                 * above happened to finish) and only pacing the *next*
                 * loop iteration. The previous ordering produced a tear
                 * consistently mid-frame instead of at the top -- exactly
                 * the signature of flipping at a fixed but not-vblank-
                 * aligned offset into each frame interval.
                 *
                 * FLIP_TEST Option 1 (flip_kernel_wait) does NOT remove
                 * this wait -- source-verified (dev.c: tegra_dc_ext_flip())
                 * that TEGRA_DC_EXT_FLIP4 calls kthread_queue_work() and
                 * returns immediately; the pre_syncpt_id/pre_syncpt_val
                 * wait lives inside tegra_dc_ext_flip_worker, a DEFERRED
                 * kthread that runs asynchronously after the ioctl already
                 * returned. It cannot provide backpressure to this thread
                 * -- an earlier attempt to skip this wait under
                 * flip_kernel_wait let FLIP4 calls run unthrottled
                 * (~238fps observed via the reuse-gap diagnostic), which
                 * also risks a genuine buffer-reuse race: only 3
                 * flip_image slots exist, and overwriting one via
                 * vkCmdCopyImage faster than the real ~16.6ms vblank
                 * period can race a still-pending kernel-queued flip
                 * targeting that same dma-buf. So this wait stays
                 * unconditional; Option 1 only adds a second, hardware-
                 * precise gate on top (below), testing latch precision,
                 * not replacing pacing. */
                if (sc->glXWaitVideoSyncSGI && sc->present_mode != VK_PRESENT_MODE_IMMEDIATE_KHR) {
                    unsigned int count = 0;
                    if (sc->glXGetVideoSyncSGI(&count) == 0)
                        sc->glXWaitVideoSyncSGI(2, (count + 1) & 1, &count);
                    else
                        sc->glXWaitVideoSyncSGI(2, 0, &count);
                }

                /* FLIP_TEST: manual timing knob. We don't actually know
                 * whether glXWaitVideoSyncSGI's return lands exactly at
                 * the hardware vblank edge or some measurable amount
                 * before/after it -- sweeping a small extra delay here
                 * lets us find out empirically by watching where the tear
                 * moves, rather than guessing. FLIP_TEST_DELAY_US=
                 * <microseconds>, read once and cached (not worth a
                 * getenv() every frame). */
                {
                    static long delay_us = -1;
                    if (delay_us < 0) {
                        const char *e = getenv("FLIP_TEST_DELAY_US");
                        delay_us = e ? atol(e) : 0;
                        LOG_INFO("FLIP_TEST: extra pre-FLIP4 delay = %ld us", delay_us);
                    }
                    if (delay_us > 0) usleep((useconds_t)delay_us);
                }

                /* FLIP_TEST Option 1: on top of the userspace SGI wait
                 * above (which paces us to ~vblank rate but sleeps in
                 * software, not a hardware-interrupt-precise primitive),
                 * also resolve "next vblank" as (current syncpt value + 1)
                 * and hand it to FLIP4 as pre_syncpt_id/pre_syncpt_val.
                 * tegra_dc_ext_flip_worker (dev.c) checks
                 * (s32)pre_syncpt_id >= 0 and, if so, calls
                 * nvhost_syncpt_wait_timeout_ext() to block itself --
                 * inside the kernel, gated on the real hardware syncpoint
                 * -- on that exact value before applying the window
                 * attributes. This is an absolute-value wait, no
                 * divisor/remainder semantics like glXWaitVideoSyncSGI.
                 * The vblank syncpoint auto-increments once per hardware
                 * vblank regardless of flips (confirmed via
                 * syncpt_probe.c: exactly 6 increments per 100ms = 60Hz),
                 * independent of software wakeup jitter -- this tests
                 * whether that hardware-precise final gate changes tear
                 * behavior versus the already-tested pure-SGI-wait
                 * baseline. */
                if (sc->flip_kernel_wait) {
                    /* Proof-of-execution knob: FLIP_TEST_SYNCPT_OFFSET
                     * overrides the "+1" delta below. Set to something
                     * unreachable within the kernel's hardcoded 5000ms
                     * nvhost_syncpt_wait_timeout_ext timeout (dev.c does
                     * not check its return value, so it always falls
                     * through and applies the flip afterward) -- e.g. 300
                     * (~5s at 60Hz) -- to get a distinctive, falsifiable
                     * ~5-second-per-frame visual freeze. That exact stall
                     * duration can only come from that kernel code path
                     * actually executing and blocking; nothing in
                     * userspace times out at 5000ms. Default 1 = normal
                     * "next vblank" behavior. */
                    static long offset = -1;
                    if (offset < 0) {
                        const char *e = getenv("FLIP_TEST_SYNCPT_OFFSET");
                        offset = e ? atol(e) : 1;
                        LOG_INFO("FLIP_TEST: kernel wait syncpt offset = +%ld", offset);
                    }
                    struct nvhost_ctrl_syncpt_read_args rd = { .id = sc->flip_vblank_syncpt_id };
                    if (ioctl(sc->flip_nvhost_ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &rd) == 0) {
                        kernel_wait_syncpt_val = rd.value + (__u32)offset;
                        kernel_wait_ok = true;
                    } else {
                        LOG_WARN("FLIP_TEST: SYNCPT_READ failed: %m");
                    }
                }
            }

            struct tegra_dc_ext_flip_windowattr win = {0};
            win.index = sc->flip_win_index;
            /* tegra_dc_ext_pin_window() (util.c) resolves buff_id via
             * dma_buf_get(fd) against the CALLING PROCESS's own fd table --
             * it wants a dma-buf fd, not an nvmap handle number. This is
             * the raw Vulkan-exported fd; no nvmap import step needed at
             * all, dma_buf_get() works with any dma-buf regardless of
             * which subsystem exported it. */
            win.buff_id = (__u32)(sc->flip_gob_real ? pi->gob_dst_fd : pi->flip_fd);
            win.blend = TEGRA_DC_EXT_BLEND_NONE;
            win.offset = (__u32)pi->flip_offset;
            win.stride = (__u32)pi->flip_row_pitch; /* actual Vulkan-reported
                pitch, may include padding -- don't assume width*4. */
            win.pixformat = TEGRA_DC_EXT_FMT_T_A8R8G8B8;
            win.w = sc->flip_out_w << 12;
            win.h = sc->flip_out_h << 12;
            win.out_x = 0; win.out_y = 0;
            win.out_w = sc->flip_out_w; win.out_h = sc->flip_out_h;
            win.z = 255;
            win.global_alpha = 255; /* fully opaque -- driver enables alpha
                                        blending at any value != 255, and
                                        zero-init left this at 0 (fully
                                        transparent). NOTE: kprobe capture
                                        of NVIDIA's own tear-free fullscreen
                                        FLIP4 calls showed alpha=0 with
                                        flags lacking TEGRA_DC_EXT_FLIP_FLAG_
                                        GLOBAL_ALPHA (1<<4) -- so global_alpha
                                        is gated by that flag bit and this
                                        assignment is likely a harmless no-op
                                        either way, not the real opacity
                                        mechanism. Left as-is; unrelated to
                                        the blocklinear retest below. */
            if (sc->flip_blocklinear || sc->flip_gob_probe || sc->flip_gob_real) {
                /* flags=0x20 + swap_interval=1 match NVIDIA's own captured
                   values exactly (kprobe on tegra_dc_ext_flip(),
                   2026-08-16, fullscreen glxgears, tear-free). block_height_
                   log2 visually produced tiling-scramble at the captured
                   value of 4 -- VK_IMAGE_TILING_OPTIMAL's actual physical
                   layout is implementation-opaque and isn't guaranteed to
                   be the same block-linear variant the DC expects, so the
                   right value (if any single value works at all) has to be
                   found empirically. FLIP_TEST_BLOCKHEIGHT_LOG2=<0-5>
                   overrides it per run without a rebuild. Under
                   flip_gob_probe, this same flag/value combo is applied to
                   the known-good LINEAR flip_image on purpose -- that's the
                   whole point of the probe. */
                static long bhl2 = -1;
                if (bhl2 < 0) {
                    const char *e = getenv("FLIP_TEST_BLOCKHEIGHT_LOG2");
                    bhl2 = e ? atol(e) : 4;
                    LOG_INFO("FLIP_TEST: blocklinear block_height_log2 = %ld", bhl2);
                }
                win.flags |= TEGRA_DC_EXT_FLIP_FLAG_BLOCKLINEAR;
                win.block_height_log2 = (__u8)bhl2;
                win.swap_interval = 1;
            }
            if (kernel_wait_ok) {
                win.pre_syncpt_id  = sc->flip_vblank_syncpt_id;
                win.pre_syncpt_val = kernel_wait_syncpt_val;
            } else {
                win.pre_syncpt_id = FLIP_TEST_NVSYNCPT_INVALID; /* (s32)0 >= 0
                    would make the driver wait on syncpoint 0, which is
                    invalid -- spams dmesg and never signals. */
            }

            struct tegra_dc_ext_flip_4 flip = {0};
            flip.win = (__u64)(uintptr_t)&win;
            flip.win_num = 1;
            flip.post_syncpt_fd = -1;

            /* Direct timing of the FLIP4 call itself, isolated from the
             * wait mechanisms above -- tegra_dc_ext_flip() (dev.c) does
             * tegra_dc_ext_pin_windows() (dma_buf_get + attach + map)
             * SYNCHRONOUSLY, in this calling thread, before it ever queues
             * the deferred kthread work. That's a real candidate for a
             * fixed per-call cost that no amount of wait-side precision
             * could avoid, and it's never been measured in isolation
             * before now (the reuse-gap diagnostic is dominated by GPU
             * copy/fence time and would hide anything on this scale). */
            struct timespec _f4_t0; clock_gettime(CLOCK_MONOTONIC, &_f4_t0);
            int _f4_ret = ioctl(sc->flip_dc_fd, TEGRA_DC_EXT_FLIP4, &flip);
            struct timespec _f4_t1; clock_gettime(CLOCK_MONOTONIC, &_f4_t1);
            static int _f4_log_count = 0;
            if (_f4_log_count < 300) {
                double _f4_ms = (_f4_t1.tv_sec - _f4_t0.tv_sec) * 1000.0 +
                                 (_f4_t1.tv_nsec - _f4_t0.tv_nsec) / 1e6;
                LOG_INFO("FLIP_TEST: FLIP4 call itself took=%.3fms ret=%d", _f4_ms, _f4_ret);
                _f4_log_count++;
            }
            if (_f4_ret < 0)
                LOG_WARN("FLIP_TEST: FLIP4 failed: %m");
            /* tegra_dc_ioctl() (dev.c) uses post_syncpt_fd as an OUTPUT
             * slot, not just input: with no explicit user_data syncpt
             * request (we send none), it creates a brand new post-flip
             * sync fence fd EVERY call and writes it back here, regardless
             * of what we passed in. We don't consume post-flip fences (our
             * own Vulkan-side fencing already gates reuse), so leaving
             * this unread leaks one fd per flip -- confirmed empirically
             * 2026-08-16 (open fd count climbing ~60/s, matching the flip
             * rate exactly) and matches the reported symptom: once the
             * process hits RLIMIT_NOFILE, the kernel's own internal fence
             * creation starts failing ("Failed creating fence err:-24" ==
             * -EMFILE), and tearing follows -- losing whatever ordering
             * that fence was providing. Must close it every call. */
            if (flip.post_syncpt_fd >= 0)
                close(flip.post_syncpt_fd);
        }

        /* Capture the vblank timestamp: right after the SGI wait above,
           which lands at the hardware vblank. */
        uint64_t actual_ns;
        {
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            actual_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
        }

        /* Refine the reported refresh duration from observed
           inter-vblank intervals. EWMA over a long window keeps the
           value stable; bias initially toward the default until a few
           frames have elapsed. We deliberately use the inter-frame
           interval observed AT vsync, which means glXWaitVideoSyncSGI
           actually fired — outliers from missed vblanks would inflate
           the average. We filter to [13ms, 21ms] which covers
           50-75Hz; anything outside that range is treated as bogus and
           ignored (e.g. an app that hides the window briefly). */
        if (prev_vblank_ns != 0) {
            uint64_t dt = actual_ns - prev_vblank_ns;
            if (dt > 13000000ULL && dt < 21000000ULL) {
                uint64_t cur = sc->refresh_duration_ns;
                /* EWMA: 1/8 new, 7/8 old. Converges in ~30 frames from a
                   default to true rate, then tracks slow drift. */
                sc->refresh_duration_ns = (7 * cur + dt) / 8;
            }
        }
        prev_vblank_ns = actual_ns;

        /* If the app marked this present with a presentID (via
           VkPresentTimesInfoGOOGLE in pNext of VkPresentInfoKHR), push
           a history entry. Apps that don't use the extension never set
           presentID, so the zero check filters their non-tracked
           presents out — keeps the ring uncluttered for apps that do
           use both modes. */
        if (present_id != 0) {
            pthread_mutex_lock(&sc->timing_lock);
            uint32_t slot;
            if (sc->timing_count < 64) {
                slot = (sc->timing_head + sc->timing_count) & 63;
                sc->timing_count++;
            } else {
                /* Ring full — overwrite oldest, advance head. */
                slot = sc->timing_head;
                sc->timing_head = (sc->timing_head + 1) & 63;
            }
            VkPastPresentationTimingGOOGLE *e = &sc->timing_ring[slot];
            e->presentID          = present_id;
            e->desiredPresentTime = desired_ns;
            e->actualPresentTime  = actual_ns;
            e->earliestPresentTime = actual_ns;        /* we don't pre-empt */
            e->presentMargin       = 0;                 /* unknown / not tracked */
            pthread_mutex_unlock(&sc->timing_lock);
        }

        /* Mark slot free AFTER the present completes. This is the
           backpressure point: while worker_pending is true, any caller in
           worker_post blocks. */
        pthread_mutex_lock(&sc->worker_lock);
        sc->worker_pending = false;
        pthread_cond_broadcast(&sc->worker_cv_done);
        pthread_mutex_unlock(&sc->worker_lock);
    }

    if (sc->flip_ready) {
        /* Disable the window before tearing down -- leaves no visible
         * leftover content on screen. */
        struct tegra_dc_ext_flip_windowattr win = {0};
        win.index = sc->flip_win_index;
        win.buff_id = 0;
        win.pre_syncpt_id = FLIP_TEST_NVSYNCPT_INVALID;
        struct tegra_dc_ext_flip_4 flip = {0};
        flip.win = (__u64)(uintptr_t)&win;
        flip.win_num = 1;
        flip.post_syncpt_fd = -1;
        ioctl(sc->flip_dc_fd, TEGRA_DC_EXT_FLIP4, &flip);
        if (flip.post_syncpt_fd >= 0) close(flip.post_syncpt_fd); /* see the per-frame call's comment */
    }
    if (sc->flip_nvhost_ctrl_fd >= 0) close(sc->flip_nvhost_ctrl_fd);
    if (sc->flip_dc_fd >= 0) close(sc->flip_dc_fd);

    glXMakeCurrent(sc->worker_dpy, None, NULL);
    XCloseDisplay(sc->worker_dpy);
    sc->worker_dpy = NULL;

    pthread_mutex_lock(&sc->worker_lock);
    sc->worker_running = false;
    pthread_cond_broadcast(&sc->worker_cv_done);
    pthread_mutex_unlock(&sc->worker_lock);
    return NULL;
}

/* Post an image index to the worker's pending slot.

   Behavior depends on the swapchain's present mode:

   - FIFO / FIFO_RELAXED (the default): blocks the caller if the slot is
     full. This is what gives us natural backpressure — the app's render
     loop is paced by the worker's swap rate. The function returns
     UINT32_MAX in *displaced_idx.

   - MAILBOX: replaces the slot contents without blocking. If the slot
     was already occupied, the previously-pending image index is
     returned via *displaced_idx so the caller can do the bookkeeping
     needed for a dropped frame (consume the dropped image's
     vk_render_done semaphore, signal its gl_sample_done so the next
     Acquire on it doesn't block). If the slot was empty, returns
     UINT32_MAX in *displaced_idx.

   present_id and desired_ns come from VkPresentTimesInfoGOOGLE on the
   app's VkPresentInfoKHR pNext; both zero when the app isn't using
   display-timing. The worker uses these to populate
   VK_GOOGLE_display_timing history. */
static void worker_post(Swapchain *sc, uint32_t idx,
                        uint32_t present_id, uint64_t desired_ns,
                        uint32_t *displaced_idx) {
    bool is_mailbox = (sc->present_mode == VK_PRESENT_MODE_MAILBOX_KHR);
    uint32_t displaced = UINT32_MAX;

    pthread_mutex_lock(&sc->worker_lock);

    if (!is_mailbox) {
        /* FIFO path: block until slot is free. */
        while (sc->worker_pending && sc->worker_running)
            pthread_cond_wait(&sc->worker_cv_done, &sc->worker_lock);
    } else if (sc->worker_pending) {
        /* MAILBOX path: a previous present hasn't been picked up yet.
           Replace it; the caller will handle the displaced image's
           semaphore cleanup. */
        displaced = sc->worker_pending_idx;
    }

    if (!sc->worker_running) {
        pthread_mutex_unlock(&sc->worker_lock);
        if (displaced_idx) *displaced_idx = UINT32_MAX;
        return;
    }
    sc->worker_pending = true;
    sc->worker_pending_idx = idx;
    sc->worker_pending_present_id = present_id;
    sc->worker_pending_desired_ns = desired_ns;
    pthread_cond_signal(&sc->worker_cv_pending);
    pthread_mutex_unlock(&sc->worker_lock);

    if (displaced_idx) *displaced_idx = displaced;
}

/* Tell the worker to exit and join the thread. */
static void worker_shutdown(Swapchain *sc) {
    pthread_mutex_lock(&sc->worker_lock);
    if (!sc->worker_running) { pthread_mutex_unlock(&sc->worker_lock); return; }
    sc->worker_quit = true;
    pthread_cond_broadcast(&sc->worker_cv_pending);
    pthread_mutex_unlock(&sc->worker_lock);
    pthread_join(sc->worker, NULL);
}

/* Resolve GL extension entrypoints. Must be called with the GLX context current. */
/* Resolves only what's actually used: GLX_SGI_video_sync, called from
 * worker_thread_main purely for vblank timing (see FLIP_TEST notes at the
 * top of this file). Nothing else needs resolving -- there's no GL
 * rendering or cross-API import anywhere in this design. Hard-fail if
 * unavailable: without it there's no vsync pacing at all (the worker
 * would free-run at whatever rate the copy+FLIP4 ioctl completes, ~124fps
 * observed), which defeats the point of this prototype. */
static bool resolve_gl_funcs(Swapchain *sc) {
    /* The functions take/return plain C int and unsigned int, so no
       GLX-headers typedefs needed; the casts to a local function-pointer
       type spell out the signatures. */
    sc->glXGetVideoSyncSGI  =
        (int (*)(unsigned *))         glXGetProcAddressARB((const GLubyte*)"glXGetVideoSyncSGI");
    sc->glXWaitVideoSyncSGI =
        (int (*)(int, int, unsigned*))glXGetProcAddressARB((const GLubyte*)"glXWaitVideoSyncSGI");
    if (!sc->glXGetVideoSyncSGI || !sc->glXWaitVideoSyncSGI) {
        LOG_ERR("GLX_SGI_video_sync not available -- no vsync pacing source");
        return false;
    }
    LOG_INFO("GLX_SGI_video_sync present; will sleep in glXWaitVideoSyncSGI between frames");
    return true;
}

/* ----------------------------------------------------------------------- */
/* Per-image setup / teardown                                              */
/* ----------------------------------------------------------------------- */

static int find_memtype(const VkPhysicalDeviceMemoryProperties *p, uint32_t bits,
                        VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < p->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (p->memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

/* Creates the app-facing OPTIMAL-tiled image the app renders into. Not
 * externally exported -- nothing needs to import it cross-API or
 * cross-process; the FLIP_TEST copy step (worker_thread_main) reads it
 * directly within the same VkDevice via vkCmdCopyImage. */
static VkResult create_app_image(DevNode *dev, Swapchain *sc, PerImage *pi) {
    DeviceDispatch *d = &dev->d;
    pi->flip_fd = -1;

    /* FLIP_TEST_BLOCKLINEAR: export this OPTIMAL-tiled image directly
       (OPAQUE_FD) instead of the separate LINEAR detile target -- see the
       flip_blocklinear comment on the Swapchain struct. */
    VkExternalMemoryImageCreateInfo emi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    if (sc->flip_blocklinear) ici.pNext = &emi;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = sc->format;
    ici.extent.width = sc->extent.width;
    ici.extent.height = sc->extent.height;
    ici.extent.depth = 1;
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    /* Honor whatever usage flags the app asked for on the swapchain, plus
       TRANSFER_SRC_BIT for our own vkCmdCopyImage read and COLOR_ATTACHMENT_BIT
       because the app renders into it. The OR is intentional — if the app
       already asked for SAMPLED or other flags, we keep them. Strip
       PRESENT_SRC-only flags that don't apply to our offscreen-allocated
       images. */
    ici.usage = sc->image_usage
              | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
              | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (sc->flip_gob_real) ici.usage |= VK_IMAGE_USAGE_STORAGE_BIT; /* gob_swizzle.comp imageLoad */
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult r = d->CreateImage(dev->device, &ici, NULL, &pi->image);
    if (r != VK_SUCCESS) return r;

    VkMemoryRequirements mreq;
    d->GetImageMemoryRequirements(dev->device, pi->image, &mreq);

    int mt = find_memtype(&dev->memp, mreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) mt = find_memtype(&dev->memp, mreq.memoryTypeBits, 0);
    if (mt < 0) { d->DestroyImage(dev->device, pi->image, NULL); pi->image = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }

    VkExportMemoryAllocateInfo eai = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    if (sc->flip_blocklinear) {
        /* Deliberately NOT a dedicated allocation (no
           VkMemoryDedicatedAllocateInfo) -- FLIP_TEST_SOLID_FILL needs to
           alias a plain VkBuffer over this same memory afterward (see
           flip_alias_buf below), which the spec disallows on memory
           dedicated to a different resource. Tested empirically: GetMemoryFdKHR
           still succeeds without dedication on this driver for a plain 2D
           color image (no multi-planar/compressed format requiring it). */
        eai.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        mai.pNext = &eai;
    }
    mai.allocationSize = mreq.size; mai.memoryTypeIndex = (uint32_t)mt;

    r = d->AllocateMemory(dev->device, &mai, NULL, &pi->memory);
    if (r != VK_SUCCESS) goto fail_img;
    r = d->BindImageMemory(dev->device, pi->image, pi->memory, 0);
    if (r != VK_SUCCESS) goto fail_mem;

    if (sc->flip_blocklinear) {
        VkMemoryGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
        gfi.memory = pi->memory; gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        r = d->GetMemoryFdKHR(dev->device, &gfi, &pi->flip_fd);
        if (r != VK_SUCCESS || pi->flip_fd < 0) { if (r == VK_SUCCESS) r = VK_ERROR_UNKNOWN; goto fail_mem; }
        pi->flip_offset = 0;
        /* Matches NVIDIA's own captured stride for A8R8G8B8 blocklinear
           (10240 for a 2560px-wide surface = width*4) -- the "stride" field
           appears to stay the plain logical row pitch even under
           BLOCKLINEAR; the DC derives the real tiled layout internally from
           width + block_height_log2, not from a caller-supplied physical
           pitch. Assumes 4 bytes/pixel, true for A8R8G8B8/A8B8G8R8. */
        pi->flip_row_pitch = (VkDeviceSize)sc->extent.width * 4;

        /* FLIP_TEST_SOLID_FILL's actual fix: a VkBuffer aliased over the
           whole mreq.size (not just the logical image extent) so
           vkCmdFillBuffer can stomp every physical byte, including any
           tiling padding vkCmdClearColorImage can't reach. Created
           regardless of whether solid-fill is active this run (cheap,
           keeps this function simple); only used if it is. */
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = mreq.size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        r = d->CreateBuffer(dev->device, &bci, NULL, &pi->flip_alias_buf);
        if (r != VK_SUCCESS) goto fail_mem;
        r = d->BindBufferMemory(dev->device, pi->flip_alias_buf, pi->memory, 0);
        if (r != VK_SUCCESS) { d->DestroyBuffer(dev->device, pi->flip_alias_buf, NULL); pi->flip_alias_buf = VK_NULL_HANDLE; goto fail_mem; }
    }
    return VK_SUCCESS;

fail_mem:
    d->FreeMemory(dev->device, pi->memory, NULL); pi->memory = VK_NULL_HANDLE;
fail_img:
    d->DestroyImage(dev->device, pi->image, NULL); pi->image = VK_NULL_HANDLE;
    return r;
}

/* FLIP_TEST_BLOCKLINEAR: no separate detile image needed (create_app_image
   exports pi->image directly), but we still need a command buffer + fence
   for the worker's per-frame wait-on-vk_render_done / signal-gl_sample_done
   submission (see worker_thread_main) -- it's just an empty command buffer
   now instead of one recording a copy. */
static bool create_flip_sync_only(DevNode *dev, Swapchain *sc, PerImage *pi) {
    DeviceDispatch *d = &dev->d;
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = sc->flip_cpool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (d->AllocateCommandBuffers(dev->device, &cbai, &pi->flip_cmdbuf) != VK_SUCCESS)
        return false;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (d->CreateFence(dev->device, &fci, NULL, &pi->flip_fence) != VK_SUCCESS)
        return false;
    return true;
}

/* FLIP_TEST: plain (non-exported) VkSemaphores. The original project
 * exports these as OPAQUE_FD to import into GL for cross-API waits/signals;
 * this prototype stays entirely in Vulkan (see worker_thread_main), so
 * there's nothing to export -- vk_render_done/gl_sample_done are used
 * directly as ordinary submit-time wait/signal semaphores. */
static VkResult create_exportable_semaphores(DevNode *dev, PerImage *pi) {
    DeviceDispatch *d = &dev->d;
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkResult r;
    r = d->CreateSemaphore(dev->device, &sci, NULL, &pi->vk_render_done);
    if (r != VK_SUCCESS) return r;
    r = d->CreateSemaphore(dev->device, &sci, NULL, &pi->gl_sample_done);
    if (r != VK_SUCCESS) goto fail_a;

    /* Per-image acquire fence — used to block Acquire on CPU side until
       the worker has actually finished with this image (which is when
       gl_sample_done gets re-signaled, by the copy command buffer's own
       pSignalSemaphores in worker_thread_main).
       Start signaled so the first NIMG acquires don't block. */
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    r = d->CreateFence(dev->device, &fci, NULL, &pi->acquire_fence);
    if (r != VK_SUCCESS) goto fail_b;

    return VK_SUCCESS;

fail_b:
    d->DestroySemaphore(dev->device, pi->gl_sample_done, NULL); pi->gl_sample_done = VK_NULL_HANDLE;
fail_a:
    d->DestroySemaphore(dev->device, pi->vk_render_done, NULL); pi->vk_render_done = VK_NULL_HANDLE;
    return r;
}

/* FLIP_TEST_GOB_PROBE=2: candidate intra-GOB byte swizzle for the standard
 * Fermi/Maxwell/Tegra "block-linear" GOB format (64 bytes wide x 8 rows
 * tall), best-effort recollection of the documented bit-interleave pattern
 * -- NOT independently re-verified from a primary source for this exact
 * revision, deliberately tested empirically below rather than trusted
 * blindly (this codebase already got burned once assuming a remembered
 * detail was right -- see FLIP_TEST_BLOCKHEIGHT_LOG2's history). x is a
 * byte offset within the GOB row (0-63), y is the row within the GOB
 * (0-7); returns the swizzled byte offset within the 512-byte GOB (0-511).
 * High bits of x/y are interleaved; the low 4 bits of x stay contiguous
 * (keeps small runs, e.g. one 4-byte pixel, un-split.) */
static inline uint32_t gob_swizzle(uint32_t x, uint32_t y) {
    uint32_t x0 = x & 1, x1 = (x >> 1) & 1, x2 = (x >> 2) & 1;
    uint32_t x3 = (x >> 3) & 1, x4 = (x >> 4) & 1, x5 = (x >> 5) & 1;
    uint32_t y0 = y & 1, y1 = (y >> 1) & 1, y2 = (y >> 2) & 1;
    return (x5 << 8) | (y2 << 7) | (y1 << 6) | (x4 << 5) | (y0 << 4) | (x3 << 3) | (x2 << 2) | (x1 << 1) | x0;
}

/* FLIP_TEST: create the separate LINEAR, exportable "detile target" image
 * for this slot, transition it once to GENERAL (the only layout linear
 * images reliably support besides PREINITIALIZED per spec), and allocate
 * the per-image command buffer + fence the worker will reuse every frame
 * for the vkCmdCopyImage detile step. No GL involved at all. */
static bool create_flip_export_image(DevNode *dev, Swapchain *sc, PerImage *pi) {
    DeviceDispatch *d = &dev->d;
    pi->flip_fd = -1;

    VkExternalMemoryImageCreateInfo emi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.pNext = &emi;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = sc->format;
    ici.extent.width = sc->extent.width;
    ici.extent.height = sc->extent.height;
    ici.extent.depth = 1;
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (d->CreateImage(dev->device, &ici, NULL, &pi->flip_image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements mreq;
    d->GetImageMemoryRequirements(dev->device, pi->flip_image, &mreq);

    int mt = -1;
    if (sc->flip_gob_probe) {
        /* Need CPU write access to hand-write the test pattern -- this
           device exposes a DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT type
           for LINEAR color images (confirmed via vulkaninfo), so this
           isn't a fallback-to-slow-memory path, just a different valid
           type for the same UMA pool. */
        mt = find_memtype(&dev->memp, mreq.memoryTypeBits,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                           | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt < 0) mt = find_memtype(&dev->memp, mreq.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (mt < 0) mt = find_memtype(&dev->memp, mreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) mt = find_memtype(&dev->memp, mreq.memoryTypeBits, 0);
    if (mt < 0) goto fail_img;

    /* FLIP_TEST_GOB_PROBE: over-allocate generously (256MB, small next to
       the ~2.6GB heap) instead of the exact mreq.size, and write the test
       pattern across the WHOLE allocation, not just the logical image
       extent -- the first probe run (2026-08-16) showed a "flickering"
       region on screen even though all 3 buffer slots hold identical
       static data, meaning some of what the DC reads under BLOCKLINEAR
       falls outside what we wrote (same out-of-bounds read suspected
       earlier for flip_blocklinear). A dedicated allocation
       (VkMemoryDedicatedAllocateInfo) forces allocationSize to exactly
       match mreq.size per spec, so it's skipped here, same fix as the
       flip_alias_buf path. */
    VkDeviceSize alloc_size = mreq.size;
    if (sc->flip_gob_probe) {
        VkDeviceSize generous = (VkDeviceSize)256 * 1024 * 1024;
        if (alloc_size < generous) alloc_size = generous;
    }

    VkExportMemoryAllocateInfo eai = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    eai.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryDedicatedAllocateInfo dai = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    if (!sc->flip_gob_probe) {
        dai.image = pi->flip_image;
        eai.pNext = &dai;
    }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = &eai; mai.allocationSize = alloc_size; mai.memoryTypeIndex = (uint32_t)mt;

    if (d->AllocateMemory(dev->device, &mai, NULL, &pi->flip_memory) != VK_SUCCESS)
        goto fail_img;
    if (d->BindImageMemory(dev->device, pi->flip_image, pi->flip_memory, 0) != VK_SUCCESS)
        goto fail_mem;

    VkMemoryGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    gfi.memory = pi->flip_memory; gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (d->GetMemoryFdKHR(dev->device, &gfi, &pi->flip_fd) != VK_SUCCESS || pi->flip_fd < 0)
        goto fail_mem;

    VkImageSubresource subres = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout layout;
    d->GetImageSubresourceLayout(dev->device, pi->flip_image, &subres, &layout);
    pi->flip_row_pitch = layout.rowPitch;
    pi->flip_offset = layout.offset;

    if (sc->flip_gob_probe == 2) {
        /* Mode 2: apply the candidate gob_swizzle() formula ourselves at
         * write time, targeting a full-width "block spans the whole row"
         * layout (empirically confirmed by mode 1: block_idx maps cleanly
         * and correctly onto output Y, one GOB-row-group per block, GOBs
         * laid out row-major -- all gob_col values for gob_row 0, then all
         * for gob_row 1, etc. -- across the full image width). Writes a
         * plain, clean (x,y) gradient (RED=x, GREEN=y) AT the swizzled
         * address for each logical pixel. If gob_swizzle() matches the
         * DC's real intra-GOB byte order, the BLOCKLINEAR-flagged read
         * should reconstruct this gradient correctly -- a clean, smooth
         * image, not the mode-1 scramble -- directly proving (or
         * disproving) the candidate formula. Zeroes the whole allocation
         * first so any address our loop doesn't reach (a wrong formula, or
         * legitimate padding) reads back as black, not stale memory. */
        long bhl2_for_pattern = 4;
        {
            const char *e = getenv("FLIP_TEST_BLOCKHEIGHT_LOG2");
            bhl2_for_pattern = e ? atol(e) : 4;
        }
        VkDeviceSize rows_per_block = (VkDeviceSize)8 << bhl2_for_pattern;
        VkDeviceSize gobs_per_block = rows_per_block / 8;
        uint32_t width = sc->extent.width, height = sc->extent.height;
        /* Round up -- see the matching comment in create_gob_dest. Also
           overrides pi->flip_row_pitch (win.stride) below to match, for
           the same reason: the DC must derive the same gobs_wide we used
           to place our writes, or the two sides disagree about where each
           block starts. */
        uint32_t gobs_wide = (width * 4 + 63) / 64;
        VkDeviceSize bytes_per_gob_row = (VkDeviceSize)gobs_wide * 512;
        VkDeviceSize bytes_per_block = bytes_per_gob_row * gobs_per_block;
        /* FLIP_TEST_GOB_ORDER: how GOBs are laid out within one block, still
           an open question after the first mode-2 test nailed Y perfectly
           but left X repeating every ~128-160px instead of sweeping the
           full width once. 0 = row-major (all gob_col for gob_row 0, then
           gob_row 1, ...; the mode-2 v1 assumption, produced the repeat).
           1 = column-major (all gob_row_in_block for gob_col 0, then
           gob_col 1, ...). 2 = GOB-level Morton/Z-order interleave of
           gob_col and gob_row_in_block bits (one level up from the
           intra-GOB byte swizzle, same idea applied to whole-GOB units). */
        int gob_order = 0;
        {
            const char *e = getenv("FLIP_TEST_GOB_ORDER");
            gob_order = e ? atoi(e) : 0;
        }

        void *mapped = NULL;
        if (d->MapMemory(dev->device, pi->flip_memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
            uint8_t *base = (uint8_t *)mapped + pi->flip_offset;
            VkDeviceSize total = alloc_size - pi->flip_offset;
            memset(base, 0, (size_t)total);

            for (uint32_t y = 0; y < height; y++) {
                VkDeviceSize block_idx = y / rows_per_block;
                uint32_t row_in_block = (uint32_t)(y % rows_per_block);
                uint32_t gob_row_in_block = row_in_block / 8;
                uint32_t row_in_gob = row_in_block % 8;
                uint32_t g = (uint32_t)(((uint64_t)y * 255) / (height > 1 ? height - 1 : 1));
                for (uint32_t x = 0; x < width; x++) {
                    uint32_t gob_col = x / 16;
                    uint32_t x_in_gob_px = x % 16;
                    uint32_t byte_x_in_gob = x_in_gob_px * 4;

                    VkDeviceSize gob_index_in_block;
                    if (gob_order == 1) {
                        gob_index_in_block = (VkDeviceSize)gob_col * gobs_per_block + gob_row_in_block;
                    } else if (gob_order == 2) {
                        /* Bit-interleave gob_col (up to 8 bits) with
                           gob_row_in_block (up to log2(gobs_per_block)
                           bits), same spirit as gob_swizzle() but at the
                           whole-GOB granularity. */
                        VkDeviceSize idx = 0;
                        for (int bit = 0; bit < 12; bit++) {
                            uint32_t colbit = (gob_col >> bit) & 1;
                            uint32_t rowbit = (bit < 8) ? ((gob_row_in_block >> bit) & 1) : 0;
                            idx |= (VkDeviceSize)colbit << (bit * 2);
                            idx |= (VkDeviceSize)rowbit << (bit * 2 + 1);
                        }
                        gob_index_in_block = idx;
                    } else {
                        gob_index_in_block = (VkDeviceSize)gob_row_in_block * gobs_wide + gob_col;
                    }
                    VkDeviceSize gob_base = block_idx * bytes_per_block + gob_index_in_block * 512;

                    uint32_t rr = (uint32_t)(((uint64_t)x * 255) / (width > 1 ? width - 1 : 1));
                    uint32_t c = 0xFF000000u | (rr << 16) | (g << 8) | 0x00u;

                    for (int b = 0; b < 4; b++) {
                        uint32_t swizzled = gob_swizzle(byte_x_in_gob + (uint32_t)b, row_in_gob);
                        VkDeviceSize addr = gob_base + swizzled;
                        if (addr < total) base[addr] = (uint8_t)(c >> (b * 8));
                    }
                }
            }
            d->UnmapMemory(dev->device, pi->flip_memory);
            /* win.stride (set from this) must match the rounded-up
               gobs_wide used for our own block-stride addressing above,
               not the natural LINEAR-image row pitch queried earlier --
               see the gobs_wide comment above. */
            pi->flip_row_pitch = (VkDeviceSize)gobs_wide * 64;
            LOG_INFO("FLIP_TEST: GOB swizzle verification pattern written (%ux%u, gobs_wide=%u, gobs_per_block=%llu)",
                     width, height, gobs_wide, (unsigned long long)gobs_per_block);
        } else {
            LOG_WARN("FLIP_TEST: GOB probe MapMemory failed, pattern not written");
        }
    } else if (sc->flip_gob_probe) {
        /* Mode 1: coordinate-revealing test pattern, written once via direct CPU
         * mapping (HOST_COHERENT, no explicit flush needed), across the
         * WHOLE over-sized allocation (alloc_size), not just the logical
         * image extent -- extends the same formula past height using raw
         * byte offset, so wherever an out-of-bounds DC read lands (see the
         * alloc_size comment above), it still finds our deterministic
         * pattern instead of unrelated memory. GOB = 16px wide x 8px tall
         * for a 4-byte/pixel format.
         *
         * Revised 2026-08-16, third pass: v1 (8-color hue by vrow/8) and v2
         * (6-hue family by block_idx) together established that block_idx
         * (which of the rows_per_block-row groups) maps correctly and
         * unscrambled onto output Y -- 12 clean horizontal bands in order.
         * What's not yet known is what happens on X *within* one block's
         * Y-band. This isolates exactly that: only "block 0" (vrow <
         * rows_per_block) gets a real signal -- RED = row_in_block (which
         * source row within the block), GREEN = vx (raw source column
         * position in MY row-major addressing). Every other block is a flat
         * gray marker. If GREEN varies smoothly left-to-right, source
         * column is preserved; if constant, the DC re-reads one column; if
         * it jumps/reorders, that's the scramble made visible directly. */
        long bhl2_for_pattern = 4;
        {
            const char *e = getenv("FLIP_TEST_BLOCKHEIGHT_LOG2");
            bhl2_for_pattern = e ? atol(e) : 4;
        }
        VkDeviceSize rows_per_block = (VkDeviceSize)8 << bhl2_for_pattern;
        void *mapped = NULL;
        if (d->MapMemory(dev->device, pi->flip_memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
            uint8_t *base = (uint8_t *)mapped + pi->flip_offset;
            VkDeviceSize total = alloc_size - pi->flip_offset;
            for (VkDeviceSize off = 0; off + 4 <= total; off += 4) {
                VkDeviceSize vrow = off / pi->flip_row_pitch;
                uint32_t vx = (uint32_t)((off % pi->flip_row_pitch) / 4);
                uint32_t c;
                if (vrow < rows_per_block) {
                    uint32_t r = (uint32_t)((vrow * 255) / (rows_per_block > 1 ? rows_per_block - 1 : 1));
                    uint32_t g = (uint32_t)(((uint64_t)vx * 255) / (sc->extent.width > 1 ? sc->extent.width - 1 : 1));
                    c = 0xFF000000u | (r << 16) | (g << 8) | 0x00u;
                } else {
                    c = 0xFF202020u; /* flat dark gray: "outside block 0" marker */
                }
                /* off is always 4-byte-aligned by construction (starts at
                   0, increments by 4) and base comes from a page-aligned
                   mapping -- a direct store avoids per-word memcpy() call
                   overhead across up to 768MB total (3 slots x 256MB). */
                *(uint32_t *)(base + off) = c;
            }
            d->UnmapMemory(dev->device, pi->flip_memory);
            LOG_INFO("FLIP_TEST: GOB probe pattern written across %llu bytes (row_pitch=%llu, %ux%u logical)",
                     (unsigned long long)total, (unsigned long long)pi->flip_row_pitch,
                     sc->extent.width, sc->extent.height);
        } else {
            LOG_WARN("FLIP_TEST: GOB probe MapMemory failed, pattern not written");
        }
    }

    /* One-time UNDEFINED -> GENERAL transition. */
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = sc->flip_cpool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer setup_cb;
    if (d->AllocateCommandBuffers(dev->device, &cbai, &setup_cb) != VK_SUCCESS)
        goto fail_mem;

    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->BeginCommandBuffer(setup_cb, &cbbi);
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = pi->flip_image;
    barrier.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    d->CmdPipelineBarrier(setup_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    d->EndCommandBuffer(setup_cb);

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &setup_cb;
    VkResult sr = queue_submit_locked(dev, dev->graphics_queue, 1, &si, VK_NULL_HANDLE);
    if (sr == VK_SUCCESS)
        d->DeviceWaitIdle(dev->device); /* one-time setup cost, not per-frame */
    d->FreeCommandBuffers(dev->device, sc->flip_cpool, 1, &setup_cb);
    if (sr != VK_SUCCESS) goto fail_mem;

    /* Per-frame command buffer + fence, allocated once and reused. */
    if (d->AllocateCommandBuffers(dev->device, &cbai, &pi->flip_cmdbuf) != VK_SUCCESS)
        goto fail_mem;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (d->CreateFence(dev->device, &fci, NULL, &pi->flip_fence) != VK_SUCCESS)
        goto fail_mem;

    return true;

fail_mem:
    if (pi->flip_fd >= 0) { close(pi->flip_fd); pi->flip_fd = -1; }
    d->FreeMemory(dev->device, pi->flip_memory, NULL); pi->flip_memory = VK_NULL_HANDLE;
fail_img:
    d->DestroyImage(dev->device, pi->flip_image, NULL); pi->flip_image = VK_NULL_HANDLE;
    return false;
}

/* FLIP_TEST_GOB_REAL: shared compute pipeline (one set for the whole
 * swapchain) that applies gob_swizzle.comp -- the verified GOB block-linear
 * transform -- to convert each frame's rendered OPTIMAL image into a real
 * block-linear byte buffer FLIP4 can present directly. Binding 0: storage
 * image (source, app's rendered content). Binding 1: storage buffer
 * (destination, block-linear layout). Push constants: width/height/
 * blockHeightLog2/gobsWide (must match gob_swizzle.comp's PC struct
 * exactly -- 4x uint32, no padding). */
static bool create_gob_pipeline(DevNode *dev, Swapchain *sc) {
    DeviceDispatch *d = &dev->d;

    VkDescriptorSetLayoutBinding bindings[2] = {0};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslci.bindingCount = 2;
    dslci.pBindings = bindings;
    if (d->CreateDescriptorSetLayout(dev->device, &dslci, NULL, &sc->gob_dsl) != VK_SUCCESS)
        return false;

    VkPushConstantRange pcr = {0};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = 16; /* 4x uint32: width, height, blockHeightLog2, gobsWide */

    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &sc->gob_dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (d->CreatePipelineLayout(dev->device, &plci, NULL, &sc->gob_pipeline_layout) != VK_SUCCESS)
        return false;

    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = sizeof(gob_swizzle_spv);
    smci.pCode = gob_swizzle_spv;
    VkShaderModule shader;
    if (d->CreateShaderModule(dev->device, &smci, NULL, &shader) != VK_SUCCESS)
        return false;

    VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = shader;
    cpci.stage.pName = "main";
    cpci.layout = sc->gob_pipeline_layout;
    VkResult pr = d->CreateComputePipelines(dev->device, VK_NULL_HANDLE, 1, &cpci, NULL, &sc->gob_pipeline);
    d->DestroyShaderModule(dev->device, shader, NULL); /* not needed after pipeline creation */
    if (pr != VK_SUCCESS) return false;

    VkDescriptorPoolSize sizes[2] = {0};
    sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[0].descriptorCount = MAX_IMAGES;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[1].descriptorCount = MAX_IMAGES;
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = MAX_IMAGES;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = sizes;
    if (d->CreateDescriptorPool(dev->device, &dpci, NULL, &sc->gob_dpool) != VK_SUCCESS)
        return false;

    return true;
}

/* FLIP_TEST_GOB_REAL: per-image destination buffer (the real block-linear
 * target, exported for FLIP4), source image view, and descriptor set. Sized
 * generously (roughly 1.25x the exact block-linear footprint) as a safety
 * margin -- same rationale as the earlier out-of-bounds-read fix, in case
 * the true footprint has rounding this formula doesn't yet account for. */
static bool create_gob_dest(DevNode *dev, Swapchain *sc, PerImage *pi) {
    DeviceDispatch *d = &dev->d;
    pi->gob_dst_fd = -1;

    /* Every content path needs its own per-frame command buffer + fence
       (see create_flip_sync_only) -- this mode has no separate detile
       target to also create it as a side effect of, so do it explicitly. */
    if (!create_flip_sync_only(dev, sc, pi))
        return false;

    uint32_t width = sc->extent.width, height = sc->extent.height;
    /* Round UP to a whole GOB-column (16px @ 4bpp), not down: a width not
       evenly divisible by 16 (e.g. vkcube's default 500, vs. gears'
       fullscreen 2560 which happens to be exactly 160 GOBs -- never
       exposed this before) otherwise leaves the last, partial GOB-column
       (here, 4 real pixels out of 16) writing past bytes_per_block's
       boundary into the *next* block's territory, corrupting it (found
       via vkcube + FLIP_TEST_GOB_PROBE=2 2026-08-16 -- visible as
       combing at the right edge plus a repeating sawtooth artifact at
       every block boundary). Rounding only OUR OWN gobs_wide up isn't
       enough by itself (tried 2026-08-16, made it worse) -- the DC derives
       its own internal gobs_wide from win.stride, so that must be told
       the same rounded-up value too, not the raw width*4 (see
       pi->flip_row_pitch below), or the two sides disagree about where
       each block starts. */
    uint32_t gobs_wide = (width * 4 + 63) / 64;
    long bhl2 = sc->gob_block_height_log2;
    VkDeviceSize gobs_per_block = (VkDeviceSize)1 << bhl2;
    VkDeviceSize rows_per_block = gobs_per_block * 8;
    VkDeviceSize num_blocks = (height + rows_per_block - 1) / rows_per_block; /* ceil */
    VkDeviceSize bytes_per_block = (VkDeviceSize)gobs_wide * gobs_per_block * 512;
    VkDeviceSize exact_size = num_blocks * bytes_per_block;
    VkDeviceSize alloc_size = exact_size + exact_size / 4; /* +25% safety margin */

    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = alloc_size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (d->CreateBuffer(dev->device, &bci, NULL, &pi->gob_dst_buf) != VK_SUCCESS)
        return false;

    VkMemoryRequirements mreq;
    d->GetBufferMemoryRequirements(dev->device, pi->gob_dst_buf, &mreq);
    int mt = find_memtype(&dev->memp, mreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) mt = find_memtype(&dev->memp, mreq.memoryTypeBits, 0);
    if (mt < 0) goto fail_buf;

    /* Not dedicated: alloc_size deliberately exceeds the buffer's own
     * minimum requirement (safety margin above), which a dedicated
     * allocation would forbid (must equal mreq.size exactly per spec). */
    VkExportMemoryAllocateInfo eai = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    eai.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = &eai;
    mai.allocationSize = mreq.size > alloc_size ? mreq.size : alloc_size;
    mai.memoryTypeIndex = (uint32_t)mt;
    if (d->AllocateMemory(dev->device, &mai, NULL, &pi->gob_dst_mem) != VK_SUCCESS)
        goto fail_buf;
    if (d->BindBufferMemory(dev->device, pi->gob_dst_buf, pi->gob_dst_mem, 0) != VK_SUCCESS)
        goto fail_mem;

    VkMemoryGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    gfi.memory = pi->gob_dst_mem; gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (d->GetMemoryFdKHR(dev->device, &gfi, &pi->gob_dst_fd) != VK_SUCCESS || pi->gob_dst_fd < 0)
        goto fail_mem;

    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image = pi->image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = sc->format;
    ivci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (d->CreateImageView(dev->device, &ivci, NULL, &pi->gob_src_view) != VK_SUCCESS)
        goto fail_mem;

    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool = sc->gob_dpool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &sc->gob_dsl;
    if (d->AllocateDescriptorSets(dev->device, &dsai, &pi->gob_dset) != VK_SUCCESS)
        goto fail_view;

    VkDescriptorImageInfo dii = {0};
    dii.imageView = pi->gob_src_view;
    dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo dbi = {0};
    dbi.buffer = pi->gob_dst_buf;
    dbi.offset = 0;
    dbi.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet writes[2] = {0};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = pi->gob_dset;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &dii;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = pi->gob_dset;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &dbi;
    d->UpdateDescriptorSets(dev->device, 2, writes, 0, NULL);

    pi->flip_fd = -1; /* not used in this mode; buff_id comes from gob_dst_fd */
    pi->flip_offset = 0;
    /* win.stride (set from this) must match the rounded-up gobs_wide used
       for our own block-stride addressing above, not the raw width*4 --
       see the gobs_wide comment. */
    pi->flip_row_pitch = (VkDeviceSize)gobs_wide * 64;
    return true;

fail_view:
    d->DestroyImageView(dev->device, pi->gob_src_view, NULL); pi->gob_src_view = VK_NULL_HANDLE;
fail_mem:
    if (pi->gob_dst_fd >= 0) { close(pi->gob_dst_fd); pi->gob_dst_fd = -1; }
    d->FreeMemory(dev->device, pi->gob_dst_mem, NULL); pi->gob_dst_mem = VK_NULL_HANDLE;
fail_buf:
    d->DestroyBuffer(dev->device, pi->gob_dst_buf, NULL); pi->gob_dst_buf = VK_NULL_HANDLE;
    return false;
}

/* Figure out which /dev/tegra_dc_N drives the monitor `win` is actually on,
 * via XRandR -- portable and driver-agnostic right up to the last step,
 * which needs a hardware-specific output-name -> DC-number mapping (no
 * public NVIDIA/tegra_dc ioctl or X11 property exposes this directly on
 * this driver; checked `xrandr --props` for a Tegra-specific property,
 * none exists). That mapping is fixed by this SoC's physical display
 * wiring (which connector goes to which DC), not something that changes
 * at runtime -- re-verify (add a case here) if this ever runs on
 * different hardware. Returns -1 (caller uses its own default) if
 * detection fails at any step, including an unrecognized output name. */
static int detect_dc_for_window(Display *dpy, Window win) {
    if (!XRRGetScreenResourcesCurrent || !XRRGetCrtcInfo ||
        !XRRGetOutputInfo || !XTranslateCoordinates) {
        LOG_WARN("FLIP_TEST: libXrandr not available, cannot auto-detect DC");
        return -1;
    }
    Window root;
    int wx, wy; unsigned int ww, wh, wb, wd;
    if (!XGetGeometry(dpy, win, &root, &wx, &wy, &ww, &wh, &wb, &wd))
        return -1;
    int abs_x, abs_y;
    Window child;
    if (!XTranslateCoordinates(dpy, win, root, 0, 0, &abs_x, &abs_y, &child))
        return -1;

    XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) return -1;

    /* Find the active CRTC with the largest rectangle overlap against the
       window -- the standard portable way to determine "which monitor is
       this window mostly on" (handles the window spanning two monitors,
       or not being perfectly positioned, gracefully). */
    long best_overlap = -1;
    RRCrtc best_crtc = None;
    for (int i = 0; i < res->ncrtc; i++) {
        XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, res->crtcs[i]);
        if (!ci) continue;
        if (ci->width > 0 && ci->height > 0) {
            int ox1 = abs_x, oy1 = abs_y;
            int ox2 = abs_x + (int)ww, oy2 = abs_y + (int)wh;
            int cx1 = ci->x, cy1 = ci->y;
            int cx2 = ci->x + (int)ci->width, cy2 = ci->y + (int)ci->height;
            int ix1 = ox1 > cx1 ? ox1 : cx1, iy1 = oy1 > cy1 ? oy1 : cy1;
            int ix2 = ox2 < cx2 ? ox2 : cx2, iy2 = oy2 < cy2 ? oy2 : cy2;
            long overlap = (ix2 > ix1 && iy2 > iy1) ? (long)(ix2 - ix1) * (long)(iy2 - iy1) : 0;
            if (overlap > best_overlap) { best_overlap = overlap; best_crtc = res->crtcs[i]; }
        }
        XRRFreeCrtcInfo(ci);
    }

    int result = -1;
    if (best_crtc != None) {
        XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, best_crtc);
        if (ci && ci->noutput > 0) {
            XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, ci->outputs[0]);
            if (oi) {
                if (strcmp(oi->name, "DSI-0") == 0) result = 0;      /* internal panel */
                else if (strcmp(oi->name, "DP-0") == 0) result = 1;  /* external/dock */
                LOG_INFO("FLIP_TEST: window is on output '%s' -> %s",
                         oi->name, result >= 0 ? "recognized" : "UNRECOGNIZED (add a case in detect_dc_for_window)");
                XRRFreeOutputInfo(oi);
            }
        }
        if (ci) XRRFreeCrtcInfo(ci);
    }
    XRRFreeScreenResources(res);
    return result;
}

static void destroy_perimage(DevNode *dev, Swapchain *sc, PerImage *pi) {
    DeviceDispatch *d = &dev->d;
    (void)sc; /* unused now that there's no GL-side per-image state */

    if (pi->vk_render_done) d->DestroySemaphore(dev->device, pi->vk_render_done, NULL);
    if (pi->gl_sample_done) d->DestroySemaphore(dev->device, pi->gl_sample_done, NULL);
    if (pi->acquire_fence)  d->DestroyFence    (dev->device, pi->acquire_fence, NULL);
    if (pi->image)          d->DestroyImage    (dev->device, pi->image, NULL);
    if (pi->flip_alias_buf) d->DestroyBuffer   (dev->device, pi->flip_alias_buf, NULL);
    if (pi->memory)         d->FreeMemory      (dev->device, pi->memory, NULL);

    /* FLIP_TEST: the separate no-GL detile target and its per-frame
     * command buffer/fence. The command buffer is freed implicitly when
     * sc->flip_cpool is destroyed (DestroySwapchainKHR), not here. */
    if (pi->flip_fence)  d->DestroyFence(dev->device, pi->flip_fence, NULL);
    if (pi->flip_image)  d->DestroyImage(dev->device, pi->flip_image, NULL);
    if (pi->flip_memory) d->FreeMemory  (dev->device, pi->flip_memory, NULL);
    if (pi->flip_fd >= 0) close(pi->flip_fd);

    /* FLIP_TEST_GOB_REAL: gob_dset is freed implicitly when sc->gob_dpool
     * is destroyed (DestroySwapchainKHR), not here. */
    if (pi->gob_src_view) d->DestroyImageView(dev->device, pi->gob_src_view, NULL);
    if (pi->gob_dst_buf)  d->DestroyBuffer   (dev->device, pi->gob_dst_buf, NULL);
    if (pi->gob_dst_mem)  d->FreeMemory      (dev->device, pi->gob_dst_mem, NULL);
    if (pi->gob_dst_fd >= 0) close(pi->gob_dst_fd);

    memset(pi, 0, sizeof(*pi));
    pi->flip_fd = -1;
    pi->gob_dst_fd = -1;
}

/* ----------------------------------------------------------------------- */
/* Surface bypass-compositor hint                                          */
/* ----------------------------------------------------------------------- */

/* ----------------------------------------------------------------------- */
/* Surface hooks                                                           */
/* ----------------------------------------------------------------------- */

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateXlibSurfaceKHR(VkInstance instance,
                            const VkXlibSurfaceCreateInfoKHR *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkSurfaceKHR *pSurface) {
    {
        InstNode *in = inst_lookup(dispatch_key(instance));
        if (g_layer_disabled || (in && in->passthrough))
            return in->d.CreateXlibSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    }
    /* Surface query functions (GetPhysicalDeviceSurfaceCapabilitiesKHR etc.)
       call XGetGeometry and other X11 functions via g_libs.* pointers that
       are only populated by lib_load().  Those queries can arrive before
       CreateSwapchainKHR, so we must load the libraries here. */
    if (!lib_load()) {
        InstNode *in = inst_lookup(dispatch_key(instance));
        return in->d.CreateXlibSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    }
    Surface *s = calloc(1, sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->magic = SURFACE_MAGIC;
    s->kind = SURF_XLIB;
    s->dpy = pCreateInfo->dpy;
    s->window = pCreateInfo->window;
    s->owns_dpy = false;
    /* Create a real ICD VkSurfaceKHR for this window alongside our wrapper.
       Non-NVIDIA devices (e.g. llvmpipe) don't know about our Surface*
       pointer; CreateSwapchainKHR uses this handle when forwarding to them. */
    {
        InstNode *in = inst_lookup(dispatch_key(instance));
        if (in && in->d.CreateXlibSurfaceKHR &&
            in->d.CreateXlibSurfaceKHR(instance, pCreateInfo, NULL,
                                        &s->icd_surface) == VK_SUCCESS)
            s->icd_inst = instance;
    }
    *pSurface = (VkSurfaceKHR)(uintptr_t)s;
    LOG_INFO("CreateXlibSurfaceKHR -> surface=%p dpy=%p win=0x%lx", s, s->dpy, s->window);
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateXcbSurfaceKHR(VkInstance instance,
                           const VkXcbSurfaceCreateInfoKHR *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkSurfaceKHR *pSurface) {
    {
        InstNode *in = inst_lookup(dispatch_key(instance));
        if (g_layer_disabled || (in && in->passthrough))
            return in->d.CreateXcbSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    }
    /* XCB-only apps don't give us an Xlib Display*. We need one for GLX
       (GLX is Xlib-bound). Open our own Display* over the same X server.
       lib_load() is deferred to CreateSwapchain; XOpenDisplay is resolved
       there too, so for the XCB path we load it eagerly here. */
    if (!lib_load()) {
        InstNode *in = inst_lookup(dispatch_key(instance));
        return in->d.CreateXcbSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    }
    Surface *s = calloc(1, sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->magic = SURFACE_MAGIC;
    s->kind = SURF_XCB;
    s->dpy = XOpenDisplay(NULL);
    if (!s->dpy) { free(s); return VK_ERROR_INITIALIZATION_FAILED; }
    s->owns_dpy = true;
    s->window = pCreateInfo->window;
    /* Create a real ICD VkSurfaceKHR alongside our wrapper for non-NVIDIA
       device passthrough (see comment in CreateXlibSurfaceKHR). */
    {
        InstNode *in = inst_lookup(dispatch_key(instance));
        if (in && in->d.CreateXcbSurfaceKHR &&
            in->d.CreateXcbSurfaceKHR(instance, pCreateInfo, NULL,
                                       &s->icd_surface) == VK_SUCCESS)
            s->icd_inst = instance;
    }
    *pSurface = (VkSurfaceKHR)(uintptr_t)s;
    LOG_INFO("CreateXcbSurfaceKHR -> surface=%p dpy=%p (opened) win=0x%x",
             s, s->dpy, pCreateInfo->window);
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL
layer_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                         const VkAllocationCallbacks *pAllocator) {
    if (!surface) return;
    Surface *s = as_surface(surface);
    InstNode *in = inst_lookup(dispatch_key(instance));
    if (!s) {  /* not ours — was created as passthrough */
        in->d.DestroySurfaceKHR(instance, surface, pAllocator);
        return;
    }
    if (s->icd_surface && in)
        in->d.DestroySurfaceKHR(s->icd_inst, s->icd_surface, NULL);
    if (s->owns_dpy && s->dpy) XCloseDisplay(s->dpy);
    free(s);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice,
                                          uint32_t queueFamilyIndex,
                                          VkSurfaceKHR surface,
                                          VkBool32 *pSupported) {
    Surface *s = as_surface(surface);
    if (!s) {
        InstNode *in = inst_lookup(dispatch_key(physicalDevice));
        return in->d.GetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamilyIndex, surface, pSupported);
    }
    /* Any graphics queue family supports our surface; we don't depend on
       Vulkan WSI presentation queues. */
    *pSupported = VK_TRUE;
    return VK_SUCCESS;
}

static void window_size(Surface *s, uint32_t *w, uint32_t *h) {
    Window root; int x, y; unsigned int ww, hh, b, dep;
    if (XGetGeometry(s->dpy, s->window, &root, &x, &y, &ww, &hh, &b, &dep)) {
        *w = ww; *h = hh;
    } else {
        *w = *h = 0;
    }
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice,
                                               VkSurfaceKHR surface,
                                               VkSurfaceCapabilitiesKHR *pCaps) {
    Surface *s = as_surface(surface);
    if (!s) {
        InstNode *in = inst_lookup(dispatch_key(physicalDevice));
        return in->d.GetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, pCaps);
    }
    uint32_t w = 0, h = 0; window_size(s, &w, &h);
    pCaps->minImageCount = MIN_IMAGES;
    pCaps->maxImageCount = MAX_IMAGES;
    pCaps->currentExtent.width  = w ? w : 1;
    pCaps->currentExtent.height = h ? h : 1;
    pCaps->minImageExtent.width  = 1;
    pCaps->minImageExtent.height = 1;
    pCaps->maxImageExtent.width  = 16384;
    pCaps->maxImageExtent.height = 16384;
    pCaps->maxImageArrayLayers = 1;
    pCaps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pCaps->currentTransform    = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pCaps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    /* Advertise everything common we can actually back. Apps will only use
       the bits they actually need, and we OR-include them in our image
       create. */
    pCaps->supportedUsageFlags =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
      | VK_IMAGE_USAGE_SAMPLED_BIT
      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
      | VK_IMAGE_USAGE_TRANSFER_DST_BIT
      | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
      | VK_IMAGE_USAGE_STORAGE_BIT;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice,
                                          VkSurfaceKHR surface,
                                          uint32_t *pCount,
                                          VkSurfaceFormatKHR *pFormats) {
    Surface *s = as_surface(surface);
    if (!s) {
        InstNode *in = inst_lookup(dispatch_key(physicalDevice));
        return in->d.GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, pCount, pFormats);
    }
    /* We support BGRA8 and RGBA8 UNORM and SRGB variants in OPTIMAL tiling
       with OPAQUE_FD export. These are the four formats we expose. */
    static const VkSurfaceFormatKHR formats[] = {
        { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_B8G8R8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_R8G8B8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    };
    uint32_t avail = sizeof(formats) / sizeof(formats[0]);
    if (!pFormats) { *pCount = avail; return VK_SUCCESS; }
    uint32_t n = *pCount < avail ? *pCount : avail;
    memcpy(pFormats, formats, n * sizeof(VkSurfaceFormatKHR));
    *pCount = n;
    return n == avail ? VK_SUCCESS : VK_INCOMPLETE;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice,
                                               VkSurfaceKHR surface,
                                               uint32_t *pCount,
                                               VkPresentModeKHR *pModes) {
    Surface *s = as_surface(surface);
    if (!s) {
        InstNode *in = inst_lookup(dispatch_key(physicalDevice));
        return in->d.GetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, pCount, pModes);
    }
    static const VkPresentModeKHR modes[] = {
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_FIFO_RELAXED_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
    };
    uint32_t avail = sizeof(modes) / sizeof(modes[0]);
    if (!pModes) { *pCount = avail; return VK_SUCCESS; }
    uint32_t n = *pCount < avail ? *pCount : avail;
    memcpy(pModes, modes, n * sizeof(VkPresentModeKHR));
    *pCount = n;
    return n == avail ? VK_SUCCESS : VK_INCOMPLETE;
}

/* VK_KHR_get_surface_capabilities2 — the "2" variants take a chain-extensible
   input struct (VkPhysicalDeviceSurfaceInfo2KHR) and write into a chain-extensible
   output. For our purposes we just delegate to the v1 implementation for our
   managed surfaces; we ignore any unknown pNext extensions on either side.
   For non-managed surfaces we pass through to the underlying driver. */
VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physicalDevice,
                                                const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
                                                VkSurfaceCapabilities2KHR *pCaps) {
    if (!pSurfaceInfo || !pCaps) return VK_ERROR_VALIDATION_FAILED_EXT;
    Surface *s = as_surface(pSurfaceInfo->surface);
    InstNode *in = inst_lookup(dispatch_key(physicalDevice));
    if (!s) {
        if (in && in->d.GetPhysicalDeviceSurfaceCapabilities2KHR)
            return in->d.GetPhysicalDeviceSurfaceCapabilities2KHR(physicalDevice, pSurfaceInfo, pCaps);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    /* Delegate to v1 for the core surfaceCapabilities. */
    return layer_GetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice,
                                                         pSurfaceInfo->surface,
                                                         &pCaps->surfaceCapabilities);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_GetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physicalDevice,
                                           const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
                                           uint32_t *pCount,
                                           VkSurfaceFormat2KHR *pFormats) {
    if (!pSurfaceInfo) return VK_ERROR_VALIDATION_FAILED_EXT;
    Surface *s = as_surface(pSurfaceInfo->surface);
    InstNode *in = inst_lookup(dispatch_key(physicalDevice));
    if (!s) {
        if (in && in->d.GetPhysicalDeviceSurfaceFormats2KHR)
            return in->d.GetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, pSurfaceInfo, pCount, pFormats);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    /* Get the v1 formats, then wrap each into a VkSurfaceFormat2KHR. */
    static const VkSurfaceFormatKHR formats[] = {
        { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_B8G8R8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_R8G8B8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    };
    uint32_t avail = sizeof(formats) / sizeof(formats[0]);
    if (!pFormats) { *pCount = avail; return VK_SUCCESS; }
    uint32_t n = *pCount < avail ? *pCount : avail;
    for (uint32_t i = 0; i < n; i++) {
        pFormats[i].sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
        pFormats[i].pNext = NULL;
        pFormats[i].surfaceFormat = formats[i];
    }
    *pCount = n;
    return n == avail ? VK_SUCCESS : VK_INCOMPLETE;
}

/* ----------------------------------------------------------------------- */
/* Swapchain creation                                                      */
/* ----------------------------------------------------------------------- */

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateSwapchainKHR(VkDevice device,
                          const VkSwapchainCreateInfoKHR *ci,
                          const VkAllocationCallbacks *pAlloc,
                          VkSwapchainKHR *pOut) {
    DevNode *dev = dev_lookup(dispatch_key(device));
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;

    /* FLIP_TEST: recreation (e.g. windowed -> fullscreen, a common trigger)
     * was never handled -- ci->oldSwapchain was silently ignored, leaving
     * its worker thread alive and its /dev/tegra_dc_N fd + hardware window
     * claim orphaned. A NEW swapchain then successfully claims the SAME
     * physical window via its OWN fd (GET_WINDOW doesn't prevent a second
     * claim from the same process), leaving two independent, actively-
     * flipping worker threads racing to present to one hardware window --
     * found 2026-08-16 via vkgears (which resizes windowed->fullscreen on
     * startup; gears/vkcube never recreate, so this never surfaced before).
     * layer_DestroySwapchainKHR already safely falls through to the native
     * ICD's destroy if the handle isn't one of ours (as_swapchain() checks
     * a magic number first), so it's safe to call unconditionally here. */
    if (ci->oldSwapchain != VK_NULL_HANDLE) {
        LOG_INFO("CreateSwapchainKHR: oldSwapchain=%p present, cleaning it up first", (void *)ci->oldSwapchain);
        layer_DestroySwapchainKHR(device, ci->oldSwapchain, pAlloc);
    }

    Surface *surf = as_surface(ci->surface);

    /* Non-NVIDIA passthrough device: forward to the ICD using the real ICD
       surface handle stored in our Surface* wrapper.  The ICD does not know
       about our Surface* pointer and would fault or return an error if given
       it directly. */
    if (dev->passthrough) {
        VkSwapchainCreateInfoKHR modci = *ci;
        if (surf && surf->icd_surface)
            modci.surface = surf->icd_surface;
        PFN_vkCreateSwapchainKHR icd_fn = (PFN_vkCreateSwapchainKHR)
            dev->d.GetDeviceProcAddr(device, "vkCreateSwapchainKHR");
        if (!icd_fn) return VK_ERROR_INITIALIZATION_FAILED;
        return icd_fn(device, &modci, pAlloc, pOut);
    }

    if (g_layer_disabled || !surf)
        return dev->d.CreateSwapchainKHR(device, ci, pAlloc, pOut);

    /* Lazy-load libX11, libGL, libGLX. We can't link these in at build time
       because doing so causes a recursive-mutex deadlock inside the Vulkan
       loader during vkCreateInstance — see the big block comment near the
       top of this file. CreateSwapchain is the first point we actually
       need them, and we're well past CreateInstance here, so no loader
       mutex is held. lib_load() is idempotent and thread-safe. */
    if (!lib_load()) {
        LOG_ERR("CreateSwapchainKHR: failed to load libGL/libX11; falling through");
        return dev->d.CreateSwapchainKHR(device, ci, pAlloc, pOut);
    }

    /* Clamp image count to our range (MIN_IMAGES=3, see its definition).
     * FLIP_TEST_MIN_IMAGES: optional override to force even more images
     * than requested, for further margin testing. */
    uint32_t want = ci->minImageCount;
    {
        static long force_min = -1;
        if (force_min < 0) {
            const char *e = getenv("FLIP_TEST_MIN_IMAGES");
            force_min = e ? atol(e) : 0;
        }
        if (force_min > 0 && (uint32_t)force_min > want) want = (uint32_t)force_min;
    }
    if (want < MIN_IMAGES) want = MIN_IMAGES;
    if (want > MAX_IMAGES) want = MAX_IMAGES;

    /* Reject formats we don't support. */
    switch (ci->imageFormat) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_SRGB:
        break;
    default:
        LOG_WARN("CreateSwapchainKHR: unsupported format %d, falling through to passthrough", ci->imageFormat);
        return dev->d.CreateSwapchainKHR(device, ci, pAlloc, pOut);
    }

    Swapchain *sc = calloc(1, sizeof(*sc));
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->magic = SWAPCHAIN_MAGIC;
    sc->dev   = dev;
    sc->surf  = surf;
    sc->image_count = want;
    sc->extent  = ci->imageExtent;
    sc->format  = ci->imageFormat;
    sc->color_space = ci->imageColorSpace;
    sc->present_mode = ci->presentMode;
    sc->image_usage  = ci->imageUsage;
    pthread_mutex_init(&sc->lock, NULL);

    int screen = DefaultScreen(surf->dpy);

    /* Query the app's window depth so we can prefer a matching FBConfig.
       If the app's window is 32-bit RGBA (Chromium, GTK CSD apps, etc.),
       we want our child window to also be 32-bit RGBA so the compositor
       can blend its translucent regions properly. If the app's window is
       24-bit, a 24-bit FBConfig is fine. */
    int parent_depth = 0;
    {
        XWindowAttributes pwa = {0};
        if (XGetWindowAttributes(surf->dpy, surf->window, &pwa)) {
            parent_depth = pwa.depth;
        }
    }

    sc->fbcfg = pick_fbconfig(surf->dpy, screen, sc->format, parent_depth, &sc->visinfo);
    if (!sc->fbcfg || !sc->visinfo) {
        LOG_ERR("pick_fbconfig: no suitable FBConfig for format %d", sc->format);
        free(sc);
        return dev->d.CreateSwapchainKHR(device, ci, pAlloc, pOut);
    }
    LOG_INFO("FBConfig visual: depth=%d (parent depth=%d)", sc->visinfo->depth, parent_depth);

    /* Create the GLX child window inside the app's surface window. We use
       our own visual (from the FBConfig) so GLX is happy. The app's window
       still has whatever visual the app chose; we never touch it directly. */
    {
        uint32_t pw = 0, ph = 0; window_size(surf, &pw, &ph);
        if (pw == 0 || ph == 0) { pw = sc->extent.width; ph = sc->extent.height; }
        sc->win_w = (int)pw; sc->win_h = (int)ph;

        sc->child_colormap = XCreateColormap(surf->dpy, surf->window,
                                              sc->visinfo->visual, AllocNone);
        XSetWindowAttributes swa = {0};
        swa.colormap = sc->child_colormap;
        swa.background_pixel = 0;
        swa.border_pixel = 0;
        /* No event mask — we don't want to receive events on the child,
           and even if we did, they'd go to whoever owns the X event queue. */
        sc->child_window = XCreateWindow(surf->dpy, surf->window,
                                         0, 0, pw, ph, 0,
                                         sc->visinfo->depth, InputOutput,
                                         sc->visinfo->visual,
                                         CWColormap | CWBackPixel | CWBorderPixel,
                                         &swa);
        if (!sc->child_window) {
            LOG_ERR("XCreateWindow(child) failed");
            if (sc->child_colormap) XFreeColormap(surf->dpy, sc->child_colormap);
            if (sc->visinfo) XFree(sc->visinfo);
            free(sc);
            return dev->d.CreateSwapchainKHR(device, ci, pAlloc, pOut);
        }
        XMapWindow(surf->dpy, sc->child_window);
        XFlush(surf->dpy);
    }

    sc->glctx = create_glx_context(surf->dpy, sc->fbcfg);
    if (!sc->glctx) {
        LOG_ERR("create_glx_context failed");
        XDestroyWindow(surf->dpy, sc->child_window);
        XFreeColormap(surf->dpy, sc->child_colormap);
        if (sc->visinfo) XFree(sc->visinfo);
        free(sc);
        return dev->d.CreateSwapchainKHR(device, ci, pAlloc, pOut);
    }
    sc->glctx_owned = true;

    if (!glXMakeCurrent(surf->dpy, sc->child_window, sc->glctx)) {
        LOG_ERR("initial glXMakeCurrent failed");
        glXDestroyContext(surf->dpy, sc->glctx);
        XDestroyWindow(surf->dpy, sc->child_window);
        XFreeColormap(surf->dpy, sc->child_colormap);
        if (sc->visinfo) XFree(sc->visinfo);
        free(sc);
        return dev->d.CreateSwapchainKHR(device, ci, pAlloc, pOut);
    }

    if (!resolve_gl_funcs(sc)) goto fail_gl_setup;

    /* FLIP_TEST: open the DC device, claim our window, and create the
     * command pool BEFORE the per-image loop, since each image's
     * create_flip_export_image() needs the pool to record its one-time
     * layout-transition command buffer. This prototype exists specifically
     * to test FLIP4 (see README.md) -- if any of this fails, that's a
     * setup bug worth surfacing loudly, not something to silently paper
     * over with a different presentation mechanism. Hard-fail the
     * swapchain (falls through to real native Vulkan WSI, same as any
     * other failure in this function). */
    sc->flip_out_w = sc->extent.width  > 2560 ? 2560 : sc->extent.width;
    sc->flip_out_h = sc->extent.height > 1600 ? 1600 : sc->extent.height;

    /* Which DC device and window index to target. Hardcoded to tegradc.1 /
     * window 1 for this entire prototype's history (confirmed free of
     * Xorg ownership specifically on DC1 -- see README safety notes) --
     * made runtime-detected 2026-08-16 after discovering the hardcoding
     * meant this file could only ever touch DC1, regardless of which
     * monitor the app's window was actually on. Default: auto-detect via
     * XRandR (detect_dc_for_window) which physical output the window is
     * on, mapped through a small hardware-specific table (this SoC's
     * fixed DSI-0->DC0 / DP-0->DC1 wiring). FLIP_TEST_DC, if set,
     * overrides detection entirely (useful for forcing a specific DC
     * while testing). Falls back to 1 (the historical default) if
     * detection fails for any reason. Window ownership has only been
     * verified on DC1 -- do not assume window 1 is free on a different
     * DC without checking first (see README). */
    int flip_dc_num;
    {
        const char *e = getenv("FLIP_TEST_DC");
        if (e) {
            flip_dc_num = atoi(e);
            LOG_INFO("FLIP_TEST: DC%d forced via FLIP_TEST_DC", flip_dc_num);
        } else {
            flip_dc_num = detect_dc_for_window(surf->dpy, surf->window);
            if (flip_dc_num < 0) {
                LOG_WARN("FLIP_TEST: DC auto-detection failed, falling back to DC1");
                flip_dc_num = 1;
            }
        }
    }
    int flip_win_index = FLIP_TEST_WIN_INDEX;
    {
        const char *e = getenv("FLIP_TEST_WIN");
        flip_win_index = e ? atoi(e) : FLIP_TEST_WIN_INDEX;
    }
    char flip_dc_path[32];
    snprintf(flip_dc_path, sizeof(flip_dc_path), "/dev/tegra_dc_%d", flip_dc_num);
    LOG_INFO("FLIP_TEST: targeting %s window %d", flip_dc_path, flip_win_index);
    sc->flip_win_index = flip_win_index;

    /* O_CLOEXEC matters here: without it, a helper process the app forks
     * (e.g. a screensaver-inhibit script Qt/KDE apps launch on entering
     * fullscreen) inherits this fd across exec(), and the kernel driver
     * won't release the window claim -- via TEGRA_DC_EXT_GET_WINDOW's
     * matching release logic -- while that unrelated child still holds a
     * duplicate reference, even after we close our own copy. Confirmed via
     * lsof 2026-08-17: dolphin-emu's windowed->fullscreen transition spawns
     * xdg-screensaver/xprop, which briefly held our just-closed DC fd open,
     * causing the immediately-following new swapchain's GET_WINDOW to fail
     * EBUSY. */
    sc->flip_dc_fd = open(flip_dc_path, O_RDWR | O_CLOEXEC);
    if (sc->flip_dc_fd < 0) {
        LOG_ERR("FLIP_TEST: open %s failed: %m", flip_dc_path);
        goto fail_perimg;
    }
    if (ioctl(sc->flip_dc_fd, TEGRA_DC_EXT_GET_WINDOW, (unsigned long)flip_win_index) < 0) {
        LOG_ERR("FLIP_TEST: GET_WINDOW %d on %s failed: %m", flip_win_index, flip_dc_path);
        goto fail_perimg;
    }

    /* FLIP_TEST Option 1 / 1b setup: resolve the DC's real vblank syncpoint
     * id (TEGRA_DC_EXT_GET_VBLANK_SYNCPT -- confirmed via kernel source to
     * be normal copy_to_user pointer semantics, unlike GET_WINDOW's
     * direct-value quirk) and open /dev/nvhost-ctrl, needed by either
     * FLIP_TEST_KERNEL_WAIT (per-frame NVHOST_IOCTL_CTRL_SYNCPT_READ to
     * compute FLIP4's pre_syncpt_val) or FLIP_TEST_KERNEL_PACE (per-frame
     * blocking NVHOST_IOCTL_CTRL_SYNCPT_WAITEX for GLX-free pacing). Hard-
     * fail if requested but unavailable -- same policy as the rest of this
     * file. The two modes are mutually exclusive; KERNEL_PACE wins if both
     * are set, since it supersedes KERNEL_WAIT's job (precise submission
     * timing) without needing FLIP4's own latch gate on top. */
    sc->flip_nvhost_ctrl_fd = -1;
    sc->flip_vblank_syncpt_id = FLIP_TEST_NVSYNCPT_INVALID;
    {
        static int kernel_wait = -1, kernel_pace = -1;
        if (kernel_wait < 0) {
            const char *e = getenv("FLIP_TEST_KERNEL_WAIT");
            kernel_wait = (e && atoi(e) != 0) ? 1 : 0;
        }
        if (kernel_pace < 0) {
            const char *e = getenv("FLIP_TEST_KERNEL_PACE");
            kernel_pace = e ? atoi(e) : 0;
        }
        sc->flip_kernel_pace = kernel_pace > 0;
        sc->flip_pace_divisor = sc->flip_kernel_pace ? (uint32_t)kernel_pace : 0;
        sc->flip_kernel_wait = (kernel_wait != 0) && !sc->flip_kernel_pace;
        if (sc->flip_kernel_pace)
            LOG_INFO("FLIP_TEST: GLX-free kernel pacing ENABLED (Option 1b), divisor=%u (~%.0ffps)",
                     sc->flip_pace_divisor, 60.0 / sc->flip_pace_divisor);
        else
            LOG_INFO("FLIP_TEST: kernel-side pre_syncpt_id wait %s",
                     sc->flip_kernel_wait ? "ENABLED (Option 1)" : "disabled (userspace SGI wait)");
    }
    if (sc->flip_kernel_wait || sc->flip_kernel_pace) {
        __u32 syncpt_id = FLIP_TEST_NVSYNCPT_INVALID;
        if (ioctl(sc->flip_dc_fd, TEGRA_DC_EXT_GET_VBLANK_SYNCPT, &syncpt_id) < 0) {
            LOG_ERR("FLIP_TEST: GET_VBLANK_SYNCPT failed: %m");
            goto fail_perimg;
        }
        sc->flip_nvhost_ctrl_fd = open("/dev/nvhost-ctrl", O_RDWR | O_CLOEXEC);
        if (sc->flip_nvhost_ctrl_fd < 0) {
            LOG_ERR("FLIP_TEST: open /dev/nvhost-ctrl failed: %m");
            goto fail_perimg;
        }
        sc->flip_vblank_syncpt_id = syncpt_id;
        LOG_INFO("FLIP_TEST: vblank syncpt id=%u, nvhost-ctrl fd=%d",
                 syncpt_id, sc->flip_nvhost_ctrl_fd);

        if (sc->flip_kernel_pace) {
            struct nvhost_ctrl_syncpt_read_args rd = { .id = syncpt_id };
            if (ioctl(sc->flip_nvhost_ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &rd) < 0) {
                LOG_ERR("FLIP_TEST: initial SYNCPT_READ failed: %m");
                goto fail_perimg;
            }
            sc->flip_pace_target = rd.value + sc->flip_pace_divisor;
        }
    }

    /* FLIP_TEST_BLOCKLINEAR=1: see the flip_blocklinear comment on the
     * Swapchain struct -- retest FLIP4 against NVIDIA's own observed
     * tear-free tiling mode instead of the LINEAR detile target used by
     * every earlier test in this prototype. */
    {
        static int blocklinear = -1;
        if (blocklinear < 0) {
            const char *e = getenv("FLIP_TEST_BLOCKLINEAR");
            blocklinear = (e && atoi(e) != 0) ? 1 : 0;
        }
        sc->flip_blocklinear = blocklinear != 0;
        LOG_INFO("FLIP_TEST: blocklinear scanout %s",
                 sc->flip_blocklinear ? "ENABLED (retest vs NVIDIA's own tiling)" : "disabled (LINEAR detile target)");
    }

    /* FLIP_TEST_SOLID_FILL=1: see the flip_solid_fill comment on the
     * Swapchain struct -- isolates whether BLOCKLINEAR itself changes
     * tearing behavior, independent of whether our tiling layout is
     * actually correct (it visually wasn't, per FLIP_TEST_BLOCKLINEAR
     * testing on 2026-08-16). */
    {
        static int solid_fill = -1;
        if (solid_fill < 0) {
            const char *e = getenv("FLIP_TEST_SOLID_FILL");
            solid_fill = (e && atoi(e) != 0) ? 1 : 0;
        }
        sc->flip_solid_fill = solid_fill != 0;
        sc->flip_solid_fill_counter = 0;
        LOG_INFO("FLIP_TEST: solid alternating fill %s",
                 sc->flip_solid_fill ? "ENABLED (tearing-causality test)" : "disabled");
    }

    /* FLIP_TEST_GOB_PROBE=<1|2>: see the flip_gob_probe comment on the
     * Swapchain struct -- empirically derives the DC's real BLOCKLINEAR
     * address permutation (mode 1) or tests a candidate swizzle formula
     * directly (mode 2). */
    {
        static int gob_probe = -1;
        if (gob_probe < 0) {
            const char *e = getenv("FLIP_TEST_GOB_PROBE");
            gob_probe = e ? atoi(e) : 0;
            if (gob_probe < 0) gob_probe = 0;
        }
        sc->flip_gob_probe = gob_probe;
        LOG_INFO("FLIP_TEST: GOB address probe %s",
                 sc->flip_gob_probe == 1 ? "ENABLED, mode 1 (empirical tiling derivation)" :
                 sc->flip_gob_probe == 2 ? "ENABLED, mode 2 (candidate swizzle verification)" : "disabled");
    }

    /* FLIP_TEST_GOB_REAL=1: see the flip_gob_real comment on the Swapchain
     * struct -- real per-frame content through the verified GOB block-
     * linear compute shader, combining correct content with the causally-
     * verified tear-free BLOCKLINEAR path. */
    {
        static int gob_real = -1;
        if (gob_real < 0) {
            const char *e = getenv("FLIP_TEST_GOB_REAL");
            gob_real = (e && atoi(e) != 0) ? 1 : 0;
        }
        sc->flip_gob_real = gob_real != 0;
        static long bhl2 = -1;
        if (bhl2 < 0) {
            const char *e = getenv("FLIP_TEST_BLOCKHEIGHT_LOG2");
            bhl2 = e ? atol(e) : 4;
        }
        sc->gob_block_height_log2 = bhl2;
        LOG_INFO("FLIP_TEST: GOB real-content compute path %s",
                 sc->flip_gob_real ? "ENABLED" : "disabled");
    }

    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = 0; /* matches dev->graphics_queue selection elsewhere */
    if (dev->d.CreateCommandPool(dev->device, &cpci, NULL, &sc->flip_cpool) != VK_SUCCESS) {
        LOG_ERR("FLIP_TEST: CreateCommandPool failed");
        goto fail_perimg;
    }

    if (sc->flip_gob_real && !create_gob_pipeline(dev, sc)) {
        LOG_ERR("FLIP_TEST: create_gob_pipeline failed");
        goto fail_perimg;
    }

    /* Allocate per-image Vulkan resources: the app-facing OPTIMAL image and
     * its semaphores (create_app_image/create_exportable_semaphores), plus
     * the separate LINEAR detile target (create_flip_export_image) FLIP4
     * actually presents. */
    for (uint32_t i = 0; i < sc->image_count; i++) {
        VkResult r = create_app_image(dev, sc, &sc->images[i]);
        if (r != VK_SUCCESS) { LOG_ERR("create_app_image[%u]: %d", i, r); goto fail_perimg; }
        r = create_exportable_semaphores(dev, &sc->images[i]);
        if (r != VK_SUCCESS) { LOG_ERR("create_exportable_semaphores[%u]: %d", i, r); goto fail_perimg; }
        if (sc->flip_gob_real) {
            if (!create_gob_dest(dev, sc, &sc->images[i])) {
                LOG_ERR("FLIP_TEST: create_gob_dest[%u] failed", i);
                goto fail_perimg;
            }
        } else if (sc->flip_blocklinear) {
            if (!create_flip_sync_only(dev, sc, &sc->images[i])) {
                LOG_ERR("FLIP_TEST: create_flip_sync_only[%u] failed", i);
                goto fail_perimg;
            }
        } else if (!create_flip_export_image(dev, sc, &sc->images[i])) {
            LOG_ERR("FLIP_TEST: create_flip_export_image[%u] failed", i);
            goto fail_perimg;
        }

        /* Pre-signal gl_sample_done so the first Acquire doesn't block. */
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &sc->images[i].gl_sample_done;
        VkResult psr = queue_submit_locked(dev, dev->graphics_queue, 1, &si, VK_NULL_HANDLE);
        if (psr != VK_SUCCESS) {
            LOG_ERR("pre-signal QueueSubmit for image %u failed: %d", i, psr);
            goto fail_perimg;
        }
    }
    LOG_INFO("FLIP_TEST: ready, window %d, out %ux%u",
             sc->flip_win_index, sc->flip_out_w, sc->flip_out_h);

    /* Release the GLX context from this thread before the worker takes it. */
    glXMakeCurrent(surf->dpy, None, NULL);

    /* Start the worker. From this point on, the worker owns sc->glctx. */
    pthread_mutex_init(&sc->worker_lock, NULL);
    pthread_cond_init(&sc->worker_cv_pending, NULL);
    pthread_cond_init(&sc->worker_cv_done, NULL);
    pthread_mutex_init(&sc->timing_lock, NULL);
    /* Default refresh duration to 60Hz; the worker will refine this
       from observed SGI vblank intervals once it starts running. */
    sc->refresh_duration_ns = 16666667ULL;
    sc->worker_running = true;
    if (pthread_create(&sc->worker, NULL, worker_thread_main, sc) != 0) {
        LOG_ERR("pthread_create(worker) failed");
        sc->worker_running = false;
        goto fail_perimg;
    }

    track_swapchain(sc);

    *pOut = (VkSwapchainKHR)(uintptr_t)sc;
    LOG_INFO("CreateSwapchainKHR -> sc=%p %ux%u fmt=%d images=%u present_mode=%d (async worker)",
             sc, sc->extent.width, sc->extent.height, sc->format,
             sc->image_count, sc->present_mode);
    return VK_SUCCESS;

fail_perimg:
    for (uint32_t i = 0; i < sc->image_count; i++) destroy_perimage(dev, sc, &sc->images[i]);
    if (sc->gob_dpool)           dev->d.DestroyDescriptorPool(dev->device, sc->gob_dpool, NULL);
    if (sc->gob_pipeline)        dev->d.DestroyPipeline(dev->device, sc->gob_pipeline, NULL);
    if (sc->gob_pipeline_layout) dev->d.DestroyPipelineLayout(dev->device, sc->gob_pipeline_layout, NULL);
    if (sc->gob_dsl)             dev->d.DestroyDescriptorSetLayout(dev->device, sc->gob_dsl, NULL);
    if (sc->flip_cpool) dev->d.DestroyCommandPool(dev->device, sc->flip_cpool, NULL);
    if (sc->flip_nvhost_ctrl_fd >= 0) close(sc->flip_nvhost_ctrl_fd);
    if (sc->flip_dc_fd >= 0) close(sc->flip_dc_fd);
fail_gl_setup:
    glXMakeCurrent(surf->dpy, None, NULL);
    if (sc->glctx_owned && sc->glctx) glXDestroyContext(surf->dpy, sc->glctx);
    if (sc->child_window) XDestroyWindow(surf->dpy, sc->child_window);
    if (sc->child_colormap) XFreeColormap(surf->dpy, sc->child_colormap);
    if (sc->visinfo) XFree(sc->visinfo);
    free(sc);
    /* Soft fail: fall through to the real Vulkan WSI so the app still runs (with tearing). */
    return dev->d.CreateSwapchainKHR(device, ci, pAlloc, pOut);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL
layer_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                           const VkAllocationCallbacks *pAlloc) {
    if (!swapchain) return;
    LOG_INFO("DestroySwapchainKHR: swapchain=%p", (void *)swapchain);
    Swapchain *sc = as_swapchain(swapchain);
    DevNode *dev = dev_lookup(dispatch_key(device));
    if (!sc) { if (dev) dev->d.DestroySwapchainKHR(device, swapchain, pAlloc); return; }

    /* Drain any in-flight work. */
    dev->d.DeviceWaitIdle(dev->device);

    untrack_swapchain(sc);

    /* Shut down the worker before touching GL resources — the worker owns the
       GLX context. After worker_shutdown returns, no thread has the context
       current; we can take it here for the final cleanup. */
    worker_shutdown(sc);

    pthread_mutex_lock(&sc->lock);
    if (sc->glctx) {
        glXMakeCurrent(sc->surf->dpy, sc->child_window, sc->glctx);
        for (uint32_t i = 0; i < sc->image_count; i++) destroy_perimage(dev, sc, &sc->images[i]);
        if (sc->gob_dpool)           dev->d.DestroyDescriptorPool(dev->device, sc->gob_dpool, NULL);
        if (sc->gob_pipeline)        dev->d.DestroyPipeline(dev->device, sc->gob_pipeline, NULL);
        if (sc->gob_pipeline_layout) dev->d.DestroyPipelineLayout(dev->device, sc->gob_pipeline_layout, NULL);
        if (sc->gob_dsl)             dev->d.DestroyDescriptorSetLayout(dev->device, sc->gob_dsl, NULL);
        /* Destroys the per-image flip_cmdbuf allocations implicitly. */
        if (sc->flip_cpool) dev->d.DestroyCommandPool(dev->device, sc->flip_cpool, NULL);
        glXMakeCurrent(sc->surf->dpy, None, NULL);
        if (sc->glctx_owned) glXDestroyContext(sc->surf->dpy, sc->glctx);
    }
    if (sc->child_window)   XDestroyWindow(sc->surf->dpy, sc->child_window);
    if (sc->child_colormap) XFreeColormap (sc->surf->dpy, sc->child_colormap);
    if (sc->visinfo) XFree(sc->visinfo);
    pthread_mutex_unlock(&sc->lock);
    pthread_mutex_destroy(&sc->lock);
    pthread_cond_destroy(&sc->worker_cv_pending);
    pthread_cond_destroy(&sc->worker_cv_done);
    pthread_mutex_destroy(&sc->worker_lock);
    pthread_mutex_destroy(&sc->timing_lock);
    memset(sc, 0, sizeof(*sc));
    free(sc);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                             uint32_t *pCount, VkImage *pImages) {
    Swapchain *sc = as_swapchain(swapchain);
    if (!sc) {
        DevNode *dev = dev_lookup(dispatch_key(device));
        return dev->d.GetSwapchainImagesKHR(device, swapchain, pCount, pImages);
    }
    if (!pImages) { *pCount = sc->image_count; return VK_SUCCESS; }
    uint32_t n = *pCount < sc->image_count ? *pCount : sc->image_count;
    for (uint32_t i = 0; i < n; i++) pImages[i] = sc->images[i].image;
    *pCount = n;
    return n == sc->image_count ? VK_SUCCESS : VK_INCOMPLETE;
}

/* ----------------------------------------------------------------------- */
/* VK_GOOGLE_display_timing                                                */
/* ----------------------------------------------------------------------- */

/* Report the display's refresh cycle duration to the application.
   The value is set at swapchain creation; we use a sensible default of
   1/60Hz and the worker refines it from observed inter-vblank intervals
   as the app runs. */
VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_GetRefreshCycleDurationGOOGLE(VkDevice device, VkSwapchainKHR swapchain,
                                     VkRefreshCycleDurationGOOGLE *pDisplayTimingProperties) {
    (void)device;
    Swapchain *sc = as_swapchain(swapchain);
    if (!sc || !pDisplayTimingProperties) return VK_ERROR_INITIALIZATION_FAILED;
    pDisplayTimingProperties->refreshDuration = sc->refresh_duration_ns;
    return VK_SUCCESS;
}

/* Return past presentation timing history. Standard count-query pattern:
   if pPresentationTimings is NULL, write the count of available entries
   to *pPresentationTimingCount. Otherwise, copy up to
   *pPresentationTimingCount entries into the output array and update
   the count to how many were actually written. Returned entries are
   removed from our ring. */
VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_GetPastPresentationTimingGOOGLE(VkDevice device, VkSwapchainKHR swapchain,
                                       uint32_t *pPresentationTimingCount,
                                       VkPastPresentationTimingGOOGLE *pPresentationTimings) {
    (void)device;
    Swapchain *sc = as_swapchain(swapchain);
    if (!sc || !pPresentationTimingCount) return VK_ERROR_INITIALIZATION_FAILED;

    pthread_mutex_lock(&sc->timing_lock);

    if (pPresentationTimings == NULL) {
        *pPresentationTimingCount = sc->timing_count;
        pthread_mutex_unlock(&sc->timing_lock);
        return VK_SUCCESS;
    }

    uint32_t want = *pPresentationTimingCount;
    uint32_t have = sc->timing_count;
    uint32_t out  = want < have ? want : have;
    for (uint32_t i = 0; i < out; i++) {
        uint32_t slot = (sc->timing_head + i) & 63;
        pPresentationTimings[i] = sc->timing_ring[slot];
    }
    /* Consume the entries we returned. */
    sc->timing_head  = (sc->timing_head + out) & 63;
    sc->timing_count = have - out;
    *pPresentationTimingCount = out;

    VkResult r = (out < have) ? VK_INCOMPLETE : VK_SUCCESS;
    pthread_mutex_unlock(&sc->timing_lock);
    return r;
}

/* ----------------------------------------------------------------------- */
/* Acquire / Present                                                       */
/* ----------------------------------------------------------------------- */

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                           uint64_t timeout, VkSemaphore semaphore, VkFence fence,
                           uint32_t *pIndex) {
    Swapchain *sc = as_swapchain(swapchain);
    DevNode *dev = dev_lookup(dispatch_key(device));
    if (!sc) return dev->d.AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pIndex);

    pthread_mutex_lock(&sc->lock);
    /* Find the next free image. Round-robin among acquired==false images. */
    uint32_t idx = sc->next_acquire;
    uint32_t tried = 0;
    while (tried < sc->image_count && sc->images[idx].acquired) {
        idx = (idx + 1) % sc->image_count;
        tried++;
    }
    if (tried == sc->image_count) {
        pthread_mutex_unlock(&sc->lock);
        return VK_NOT_READY;
    }
    sc->images[idx].acquired = true;
    sc->next_acquire = (idx + 1) % sc->image_count;

    /* Reset our internal acquire_fence (it was either signaled-at-creation
       for the first N acquires, or signaled by the previous bridge submit). */
    dev->d.ResetFences(dev->device, 1, &sc->images[idx].acquire_fence);

    /* Bridge submit: wait on the per-image gl_sample_done semaphore, signal
       the app's requested acquire semaphore and/or fence, AND signal our
       internal acquire_fence. The fence lets us CPU-block here until the
       worker has actually finished sampling the image, which is the real
       backpressure point: without this, the app's loop runs unbounded
       (matching the worker's vsync rate) and burns CPU. */
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &sc->images[idx].gl_sample_done;
    si.pWaitDstStageMask    = &stage;
    if (semaphore) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &semaphore; }
    VkResult r = queue_submit_locked(dev, dev->graphics_queue, 1, &si, sc->images[idx].acquire_fence);
    pthread_mutex_unlock(&sc->lock);
    if (r != VK_SUCCESS) {
        LOG_ERR("AcquireNextImageKHR: bridge QueueSubmit failed: %d", r);
        return r;
    }

    /* CPU-block until our internal fence signals. This is the backpressure
       point that paces the app's render loop to actual presentation rate. */
    r = dev->d.WaitForFences(dev->device, 1, &sc->images[idx].acquire_fence, VK_TRUE, timeout);
    if (r == VK_TIMEOUT) return VK_TIMEOUT;
    if (r != VK_SUCCESS) {
        LOG_ERR("AcquireNextImageKHR: WaitForFences failed: %d", r);
        return r;
    }

    /* If the app passed its OWN fence (separate from our internal one), we
       need to also signal it. Easiest: do a second tiny submit that signals
       just the app's fence. Skip if no app fence. */
    if (fence) {
        VkSubmitInfo si2 = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        r = queue_submit_locked(dev, dev->graphics_queue, 1, &si2, fence);
        if (r != VK_SUCCESS) {
            LOG_ERR("AcquireNextImageKHR: app-fence signal submit failed: %d", r);
        }
    }

    *pIndex = idx;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_QueueWaitIdle(VkQueue queue) {
    DevNode *dev = dev_lookup(dispatch_key(queue));
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = dev->d.QueueWaitIdle(queue);
    if (r != VK_SUCCESS) {
        LOG_ERR("vkQueueWaitIdle FAILED: ret=%d queue=%p", r, (void*)queue);
    }
    return r;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_DeviceWaitIdle(VkDevice device) {
    DevNode *dev = dev_lookup(dispatch_key(device));
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = dev->d.DeviceWaitIdle(device);
    if (r != VK_SUCCESS) {
        LOG_ERR("vkDeviceWaitIdle FAILED: ret=%d", r);
    }
    return r;
}

/* Track swapchains in a global list so we can answer "is this image ours" without
   knowing which swapchain it belongs to up front. */
#define MAX_TRACKED_SWAPCHAINS 16
static Swapchain *g_swapchains[MAX_TRACKED_SWAPCHAINS];
static pthread_mutex_t g_sc_lock = PTHREAD_MUTEX_INITIALIZER;

static void track_swapchain(Swapchain *sc) {
    pthread_mutex_lock(&g_sc_lock);
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS; i++) {
        if (!g_swapchains[i]) { g_swapchains[i] = sc; break; }
    }
    pthread_mutex_unlock(&g_sc_lock);
}
static void untrack_swapchain(Swapchain *sc) {
    pthread_mutex_lock(&g_sc_lock);
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS; i++) {
        if (g_swapchains[i] == sc) { g_swapchains[i] = NULL; break; }
    }
    pthread_mutex_unlock(&g_sc_lock);
}
/* Quick "is this image managed" check. Most barriers will hit non-managed
   images, so we want this to be as cheap as possible. The slowest part is
   acquiring the lock — but since the swapchain set rarely changes, we
   keep a generation counter and a thread-local cache to skip the lock
   entirely when nothing has changed. For simplicity, in the first pass we
   just take the lock and walk; if profiling shows this is still hot, the
   lock-free version is straightforward. */
static bool image_is_managed(VkImage img) {
    if (!img) return false;
    pthread_mutex_lock(&g_sc_lock);
    bool found = false;
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS && !found; i++) {
        Swapchain *sc = g_swapchains[i];
        if (!sc) continue;
        for (uint32_t j = 0; j < sc->image_count; j++) {
            if (sc->images[j].image == img) { found = true; break; }
        }
    }
    pthread_mutex_unlock(&g_sc_lock);
    return found;
}

/* Fast-path: are we tracking ANY swapchains at all? If not, no barriers can
   possibly hit our images, so skip the per-barrier check entirely. */
static bool any_swapchains_tracked(void) {
    pthread_mutex_lock(&g_sc_lock);
    bool any = false;
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS; i++) {
        if (g_swapchains[i]) { any = true; break; }
    }
    pthread_mutex_unlock(&g_sc_lock);
    return any;
}

/* Rewrite VK_IMAGE_LAYOUT_PRESENT_SRC_KHR (and SHARED_PRESENT_KHR) on barriers
   targeting our managed images. The driver can't transition a non-swapchain
   image to PRESENT_SRC — it tries to invoke present-engine metadata that doesn't
   exist for our externally-allocated images, and faults the GPU. We rewrite
   to SHADER_READ_ONLY_OPTIMAL, which is what we actually want anyway since GL
   will sample the image after the app is done with it. */
static VkImageLayout fix_layout(VkImageLayout l) {
    if (l == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (l == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR) return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return l;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL
layer_CmdPipelineBarrier(VkCommandBuffer cb,
                          VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                          VkDependencyFlags depFlags,
                          uint32_t memBarrierCount, const VkMemoryBarrier *memBarriers,
                          uint32_t bufBarrierCount, const VkBufferMemoryBarrier *bufBarriers,
                          uint32_t imgBarrierCount, const VkImageMemoryBarrier *imgBarriers) {
    DevNode *dev = dev_lookup(dispatch_key(cb));
    if (!dev || !dev->d.CmdPipelineBarrier) return;

    /* Fast path: no image barriers, or no swapchains tracked. Pass through with
       no allocation, no lock, no copy. This is the path 99% of barriers take
       in a real application — the swapchain-image barriers are the rare case. */
    if (imgBarrierCount == 0 || !any_swapchains_tracked()) {
        dev->d.CmdPipelineBarrier(cb, srcStage, dstStage, depFlags,
                                   memBarrierCount, memBarriers,
                                   bufBarrierCount, bufBarriers,
                                   imgBarrierCount, imgBarriers);
        return;
    }

    /* Slow path: walk the image barriers checking for managed images. Only
       allocate a copy if at least one barrier actually needs rewriting. */
    bool any_managed = false;
    for (uint32_t i = 0; i < imgBarrierCount; i++) {
        if (image_is_managed(imgBarriers[i].image)) { any_managed = true; break; }
    }
    if (!any_managed) {
        dev->d.CmdPipelineBarrier(cb, srcStage, dstStage, depFlags,
                                   memBarrierCount, memBarriers,
                                   bufBarrierCount, bufBarriers,
                                   imgBarrierCount, imgBarriers);
        return;
    }

    VkImageMemoryBarrier *fixed = malloc(imgBarrierCount * sizeof(*fixed));
    if (!fixed) {
        /* Best effort: pass through unmodified. May fault but at least
           doesn't drop the barrier. */
        dev->d.CmdPipelineBarrier(cb, srcStage, dstStage, depFlags,
                                   memBarrierCount, memBarriers,
                                   bufBarrierCount, bufBarriers,
                                   imgBarrierCount, imgBarriers);
        return;
    }
    memcpy(fixed, imgBarriers, imgBarrierCount * sizeof(*fixed));
    for (uint32_t i = 0; i < imgBarrierCount; i++) {
        if (image_is_managed(fixed[i].image)) {
            fixed[i].oldLayout = fix_layout(fixed[i].oldLayout);
            fixed[i].newLayout = fix_layout(fixed[i].newLayout);
        }
    }
    dev->d.CmdPipelineBarrier(cb, srcStage, dstStage, depFlags,
                               memBarrierCount, memBarriers,
                               bufBarrierCount, bufBarriers,
                               imgBarrierCount, fixed);
    free(fixed);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_EndCommandBuffer(VkCommandBuffer cb) {
    DevNode *dev = dev_lookup(dispatch_key(cb));
    if (!dev || !dev->d.EndCommandBuffer) return VK_ERROR_INITIALIZATION_FAILED;
    return dev->d.EndCommandBuffer(cb);
}

/* vkCreateRenderPass intercept.

   When a render pass's attachment description has finalLayout (or
   initialLayout) set to PRESENT_SRC_KHR, the driver will perform an implicit
   layout transition at the end of the render pass execution. For our
   managed (non-WSI) swapchain images that's the same fault as a manual
   PRESENT_SRC barrier — the driver tries to invoke present-engine metadata
   that doesn't exist for externally-allocated images, and faults the GPU.

   Unlike with vkCmdPipelineBarrier, at vkCreateRenderPass time we don't
   know which images the render pass will be used with — that's determined
   later by the framebuffer. We pessimistically rewrite ANY PRESENT_SRC
   attachment layout to SHADER_READ_ONLY_OPTIMAL. This is safe because:
   - When our layer is active, we replace the swapchain entirely. There are
     no "real" presentable images in the application; all swapchain images
     are our managed ones, and they all want SHADER_READ_ONLY_OPTIMAL at
     present time anyway.
   - For non-swapchain attachments, applications shouldn't be specifying
     PRESENT_SRC anyway — that layout only exists for swapchain images.
     If an app does it incorrectly, our rewrite improves correctness. */
VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateRenderPass(VkDevice device,
                        const VkRenderPassCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkRenderPass *pRenderPass) {
    DevNode *dev = dev_lookup(dispatch_key(device));
    if (!dev || !dev->d.CreateRenderPass) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pCreateInfo || pCreateInfo->attachmentCount == 0)
        return dev->d.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);

    /* Scan for PRESENT_SRC attachments. */
    bool any = false;
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
        VkImageLayout il = pCreateInfo->pAttachments[i].initialLayout;
        VkImageLayout fl = pCreateInfo->pAttachments[i].finalLayout;
        if (il == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || il == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR ||
            fl == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || fl == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR) {
            any = true; break;
        }
    }
    if (!any) return dev->d.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);

    VkAttachmentDescription *fixed = malloc(pCreateInfo->attachmentCount * sizeof(*fixed));
    if (!fixed) return dev->d.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
    memcpy(fixed, pCreateInfo->pAttachments,
           pCreateInfo->attachmentCount * sizeof(*fixed));
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
        fixed[i].initialLayout = fix_layout(fixed[i].initialLayout);
        fixed[i].finalLayout   = fix_layout(fixed[i].finalLayout);
    }
    VkRenderPassCreateInfo mod = *pCreateInfo;
    mod.pAttachments = fixed;
    VkResult r = dev->d.CreateRenderPass(device, &mod, pAllocator, pRenderPass);
    free(fixed);
    return r;
}

/* vkCreateRenderPass2 / vkCreateRenderPass2KHR intercept.
   Same PRESENT_SRC_KHR rewrite as v1, applied to the v2 struct.
   Vulkan 1.2 apps use this entrypoint instead of v1. */
VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateRenderPass2(VkDevice device,
                         const VkRenderPassCreateInfo2 *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkRenderPass *pRenderPass) {
    DevNode *dev = dev_lookup(dispatch_key(device));
    if (!dev || !dev->d.CreateRenderPass2) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pCreateInfo || pCreateInfo->attachmentCount == 0)
        return dev->d.CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);

    bool any = false;
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
        VkImageLayout il = pCreateInfo->pAttachments[i].initialLayout;
        VkImageLayout fl = pCreateInfo->pAttachments[i].finalLayout;
        if (il == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || il == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR ||
            fl == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || fl == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR) {
            any = true; break;
        }
    }
    if (!any) return dev->d.CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);

    VkAttachmentDescription2 *fixed = malloc(pCreateInfo->attachmentCount * sizeof(*fixed));
    if (!fixed) return dev->d.CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
    memcpy(fixed, pCreateInfo->pAttachments,
           pCreateInfo->attachmentCount * sizeof(*fixed));
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
        fixed[i].initialLayout = fix_layout(fixed[i].initialLayout);
        fixed[i].finalLayout   = fix_layout(fixed[i].finalLayout);
    }
    VkRenderPassCreateInfo2 mod = *pCreateInfo;
    mod.pAttachments = fixed;
    VkResult r = dev->d.CreateRenderPass2(device, &mod, pAllocator, pRenderPass);
    free(fixed);
    return r;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateRenderPass2KHR(VkDevice device,
                             const VkRenderPassCreateInfo2 *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             VkRenderPass *pRenderPass) {
    /* KHR alias — same struct, same rewrite logic.  Both d.CreateRenderPass2
       and d.CreateRenderPass2KHR are set to whichever entrypoints the driver
       exposes (with fallback to each other), so layer_CreateRenderPass2 calls
       through correctly regardless of which name the driver uses. */
    return layer_CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_QueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence) {
    DevNode *dev = dev_lookup(dispatch_key(queue));
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;

    static uint64_t submit_seq = 0;
    uint64_t my_seq = 0;
    if (g_diag_wait_after_submit) {
        my_seq = __atomic_add_fetch(&submit_seq, 1, __ATOMIC_SEQ_CST);
        LOG_INFO("DIAG QueueSubmit#%" PRIu64 ": queue=%p cnt=%u fence=%p [waits=%u cbs=%u sigs=%u]",
                 my_seq, (void*)queue, submitCount, (void*)fence,
                 pSubmits[0].waitSemaphoreCount,
                 pSubmits[0].commandBufferCount,
                 pSubmits[0].signalSemaphoreCount);
    }

    VkResult r = queue_submit_locked(dev, queue, submitCount, pSubmits, fence);
    if (r != VK_SUCCESS) {
        LOG_ERR("vkQueueSubmit FAILED at seq#%" PRIu64 ": ret=%d queue=%p submitCount=%u fence=%p",
                my_seq, r, (void*)queue, submitCount, (void*)fence);
        for (uint32_t i = 0; i < submitCount; i++) {
            LOG_ERR("  pSubmits[%u]: waitSem=%u cmdBuf=%u sigSem=%u",
                    i, pSubmits[i].waitSemaphoreCount,
                    pSubmits[i].commandBufferCount, pSubmits[i].signalSemaphoreCount);
        }
        return r;
    }

    if (g_diag_wait_after_submit) {
        VkResult wr = dev->d.DeviceWaitIdle(dev->device);
        if (wr != VK_SUCCESS) {
            LOG_ERR("DIAG DeviceWaitIdle AFTER QueueSubmit#%" PRIu64 " returned %d "
                    "(submit faulted: queue=%p cnt=%u cb_in_submit=%u)",
                    my_seq, wr, (void*)queue, submitCount,
                    submitCount > 0 ? pSubmits[0].commandBufferCount : 0);
        }
    }

    return r;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_WaitForFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences,
                     VkBool32 waitAll, uint64_t timeout) {
    DevNode *dev = dev_lookup(dispatch_key(device));
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = dev->d.WaitForFences(device, fenceCount, pFences, waitAll, timeout);
    if (r != VK_SUCCESS) {
        LOG_ERR("vkWaitForFences FAILED: ret=%d fenceCount=%u timeout=%" PRIu64,
                r, fenceCount, timeout);
    }
    return r;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_ResetFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences) {
    DevNode *dev = dev_lookup(dispatch_key(device));
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = dev->d.ResetFences(device, fenceCount, pFences);
    if (r != VK_SUCCESS) LOG_ERR("vkResetFences FAILED: ret=%d", r);
    return r;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pInfo) {
    /* Queue's dispatch key is the parent device's. */
    DevNode *dev = dev_lookup(dispatch_key(queue));
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    if (g_layer_disabled) {
        pthread_mutex_lock(&dev->submit_lock);
        VkResult r = dev->d.QueuePresentKHR(queue, pInfo);
        pthread_mutex_unlock(&dev->submit_lock);
        return r;
    }

    LOG_DBG("Present entry: queue=%p (graphics_queue=%p) swapchains=%u waitSems=%u",
             (void*)queue, (void*)dev->graphics_queue,
             pInfo->swapchainCount, pInfo->waitSemaphoreCount);

    /* Walk the pNext chain looking for VkPresentTimesInfoGOOGLE. If
       found, its pTimes[s] gives presentID and desiredPresentTime for
       swapchain index s. swapchainCount in that struct must equal the
       outer pInfo->swapchainCount per the extension spec; we trust the
       app on that. */
    const VkPresentTimesInfoGOOGLE *times_info = NULL;
    {
        const VkBaseInStructure *p = (const VkBaseInStructure *)pInfo->pNext;
        while (p) {
            if (p->sType == VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE) {
                times_info = (const VkPresentTimesInfoGOOGLE *)p;
                break;
            }
            p = p->pNext;
        }
    }

    VkResult overall = VK_SUCCESS;

    for (uint32_t s = 0; s < pInfo->swapchainCount; s++) {
        Swapchain *sc = as_swapchain(pInfo->pSwapchains[s]);
        if (!sc) {
            /* Mixed batch — submit just this one through real WSI. */
            VkPresentInfoKHR sub = *pInfo;
            sub.swapchainCount = 1;
            sub.pSwapchains    = &pInfo->pSwapchains[s];
            sub.pImageIndices  = &pInfo->pImageIndices[s];
            sub.pResults       = pInfo->pResults ? &pInfo->pResults[s] : NULL;
            sub.waitSemaphoreCount = (s == 0) ? pInfo->waitSemaphoreCount : 0;
            sub.pWaitSemaphores    = (s == 0) ? pInfo->pWaitSemaphores    : NULL;
            pthread_mutex_lock(&dev->submit_lock);
            VkResult r = dev->d.QueuePresentKHR(queue, &sub);
            pthread_mutex_unlock(&dev->submit_lock);
            if (pInfo->pResults) pInfo->pResults[s] = r;
            if (r != VK_SUCCESS && overall == VK_SUCCESS) overall = r;
            continue;
        }
        uint32_t idx = pInfo->pImageIndices[s];
        if (idx >= sc->image_count) {
            VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
            if (pInfo->pResults) pInfo->pResults[s] = r;
            if (overall == VK_SUCCESS) overall = r;
            continue;
        }

        pthread_mutex_lock(&sc->lock);

        /* Bridge: wait on the app's render-done semaphore(s), signal our
           per-image vk_render_done. Only do this for the first swapchain in
           the batch; subsequent ones don't get the app's waitSemaphores
           applied per WSI semantics. */
        VkPipelineStageFlags stages[16];
        for (int i = 0; i < 16; i++) stages[i] = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        VkSubmitInfo bridge = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        if (s == 0 && pInfo->waitSemaphoreCount > 0) {
            bridge.waitSemaphoreCount = pInfo->waitSemaphoreCount;
            bridge.pWaitSemaphores    = pInfo->pWaitSemaphores;
            bridge.pWaitDstStageMask  = stages;
        }
        bridge.signalSemaphoreCount = 1;
        bridge.pSignalSemaphores = &sc->images[idx].vk_render_done;
        VkResult rb = queue_submit_locked(dev, queue, 1, &bridge, VK_NULL_HANDLE);
        if (rb != VK_SUCCESS) {
            pthread_mutex_unlock(&sc->lock);
            LOG_ERR("QueuePresentKHR: bridge QueueSubmit failed: %d (queue=%p idx=%u waitSems=%u signal=%p)",
                    rb, (void*)queue, idx, pInfo->waitSemaphoreCount,
                    (void*)sc->images[idx].vk_render_done);
            if (pInfo->pResults) pInfo->pResults[s] = rb;
            if (overall == VK_SUCCESS) overall = rb;
            continue;
        }

        /* The GL side runs in the worker thread which owns the GLX context.
           In FIFO mode this call blocks if the worker is still busy with
           the previous image — that's the natural backpressure point.
           In MAILBOX mode this call returns immediately and reports back
           the index of any image whose place we just took, so we can
           clean up its dangling semaphores before the next iteration. */
        sc->images[idx].acquired = false;
        pthread_mutex_unlock(&sc->lock);

        uint32_t present_id = 0;
        uint64_t desired_ns = 0;
        if (times_info && times_info->pTimes && s < times_info->swapchainCount) {
            present_id = times_info->pTimes[s].presentID;
            desired_ns = times_info->pTimes[s].desiredPresentTime;
        }
        uint32_t displaced_idx = UINT32_MAX;
        worker_post(sc, idx, present_id, desired_ns, &displaced_idx);

        /* MAILBOX drop bookkeeping. If worker_post returned a displaced
           index, that image was Presented earlier but its turn at the
           GLX swap never came. Two things need to happen for its state
           to be consistent for future use:

             1. Consume its vk_render_done semaphore. We had already
                signalled it via the bridge submit for that earlier
                Present. The worker would normally consume it via
                glWaitSemaphoreEXT during its loop iteration; since
                that iteration is skipped we have to consume here, or
                a future Present's bridge submit attempting to signal
                the same binary semaphore is undefined behaviour.

             2. Signal its gl_sample_done semaphore. The next time the
                app calls Acquire on this image, the acquire bridge
                waits on gl_sample_done. Without us signalling it the
                app would deadlock waiting for a frame that never
                actually rendered.

           One bridge submit handles both. The wait/signal happen on
           the queue serially with no actual GPU work between them; the
           submit returns fast. */
        if (displaced_idx != UINT32_MAX) {
            VkPipelineStageFlags drop_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            VkSubmitInfo drop_si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            drop_si.waitSemaphoreCount   = 1;
            drop_si.pWaitSemaphores      = &sc->images[displaced_idx].vk_render_done;
            drop_si.pWaitDstStageMask    = &drop_stage;
            drop_si.signalSemaphoreCount = 1;
            drop_si.pSignalSemaphores    = &sc->images[displaced_idx].gl_sample_done;
            VkResult rd = queue_submit_locked(dev, queue, 1, &drop_si, VK_NULL_HANDLE);
            if (rd != VK_SUCCESS) {
                LOG_ERR("QueuePresentKHR: mailbox drop cleanup submit failed: %d "
                        "(displaced_idx=%u)", rd, displaced_idx);
                /* Don't propagate to the app — the present that displaced
                   this one is still in flight and is what the app actually
                   wanted. Best effort. */
            }
        }

        if (pInfo->pResults) pInfo->pResults[s] = VK_SUCCESS;
    }
    return overall;
}

/* ----------------------------------------------------------------------- */
/* Device / instance lifecycle                                             */
/* ----------------------------------------------------------------------- */

/* Forward decls. */
static PFN_vkVoidFunction layer_intercept_instance(const char *name);
static PFN_vkVoidFunction layer_intercept_device  (const char *name);
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetDeviceProcAddr(VkDevice dev, const char *name);
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetInstanceProcAddr(VkInstance inst, const char *name);

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateInstance(const VkInstanceCreateInfo *ci,
                      const VkAllocationCallbacks *pAlloc,
                      VkInstance *pInst) {
    VkLayerInstanceCreateInfo *lci = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (lci && !(lci->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                    lci->function == VK_LAYER_LINK_INFO))
        lci = (VkLayerInstanceCreateInfo *)lci->pNext;
    if (!lci) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = lci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    lci->u.pLayerInfo = lci->u.pLayerInfo->pNext;

    /* Like with CreateDevice, the app may not request the instance-level
       external memory/semaphore capability extensions. They're core in
       Vulkan 1.1 but still need to be listed if the app requested 1.0.
       Add them if the app didn't, harmlessly redundant if it did. */
    static const char *required_inst[] = {
        "VK_KHR_external_memory_capabilities",
        "VK_KHR_external_semaphore_capabilities",
        "VK_KHR_get_physical_device_properties2",
    };
    static const uint32_t n_required_inst = 3;
    uint32_t to_add = 0;
    bool need[3] = { true, true, true };
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        for (uint32_t j = 0; j < n_required_inst; j++) {
            if (need[j] && !strcmp(ci->ppEnabledExtensionNames[i], required_inst[j])) need[j] = false;
        }
    }
    for (uint32_t j = 0; j < n_required_inst; j++) if (need[j]) to_add++;

    VkInstanceCreateInfo modci = *ci;
    const char **new_exts = NULL;
    if (to_add > 0) {
        uint32_t total = ci->enabledExtensionCount + to_add;
        new_exts = malloc(total * sizeof(const char *));
        if (!new_exts) return VK_ERROR_OUT_OF_HOST_MEMORY;
        memcpy(new_exts, ci->ppEnabledExtensionNames,
               ci->enabledExtensionCount * sizeof(const char *));
        uint32_t k = ci->enabledExtensionCount;
        for (uint32_t j = 0; j < n_required_inst; j++) if (need[j]) new_exts[k++] = required_inst[j];
        modci.enabledExtensionCount = total;
        modci.ppEnabledExtensionNames = new_exts;
    }

    PFN_vkCreateInstance next_create = (PFN_vkCreateInstance)next_gipa(NULL, "vkCreateInstance");
    VkResult r = next_create(&modci, pAlloc, pInst);
    if (new_exts) free(new_exts);
    if (r != VK_SUCCESS) return r;

    InstNode *node = calloc(1, sizeof(*node));
    if (!node) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    node->instance = *pInst;
    node->key = dispatch_key(*pInst);
#define I(name) node->d.name = (PFN_vk##name)next_gipa(*pInst, "vk" #name)
    I(GetInstanceProcAddr);
    I(DestroyInstance);
    I(EnumeratePhysicalDevices);
    I(GetPhysicalDeviceProperties);
    I(GetPhysicalDeviceMemoryProperties);
    I(GetPhysicalDeviceQueueFamilyProperties);
    I(CreateXlibSurfaceKHR);
    I(CreateXcbSurfaceKHR);
    I(DestroySurfaceKHR);
    I(GetPhysicalDeviceSurfaceSupportKHR);
    I(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    I(GetPhysicalDeviceSurfaceFormatsKHR);
    I(GetPhysicalDeviceSurfacePresentModesKHR);
    I(GetPhysicalDeviceSurfaceCapabilities2KHR);
    I(GetPhysicalDeviceSurfaceFormats2KHR);
    I(GetPhysicalDeviceImageFormatProperties2);
    I(GetPhysicalDeviceExternalSemaphoreProperties);
#undef I
    /* Determine whether this instance has any NVIDIA physical device.
       We check now — before any surfaces are created — so that surface
       creation functions can immediately pass through for non-NVIDIA
       instances.  If EnumeratePhysicalDevices fails or returns zero
       devices we leave passthrough=false (default active) as a safe
       fallback; the missing-entrypoints guard in CreateDevice will catch
       any incompatibility later. */
    if (node->d.EnumeratePhysicalDevices && node->d.GetPhysicalDeviceProperties) {
        uint32_t nphys = 0;
        node->d.EnumeratePhysicalDevices(*pInst, &nphys, NULL);
        VkPhysicalDevice *physdevs = nphys ? calloc(nphys, sizeof(*physdevs)) : NULL;
        if (physdevs) {
            node->d.EnumeratePhysicalDevices(*pInst, &nphys, physdevs);
            bool has_nvidia = false;
            for (uint32_t i = 0; i < nphys && !has_nvidia; i++) {
                VkPhysicalDeviceProperties props = {0};
                node->d.GetPhysicalDeviceProperties(physdevs[i], &props);
                if (props.vendorID == 0x10DE) has_nvidia = true;
            }
            free(physdevs);
            if (!has_nvidia) {
                node->passthrough = true;
                LOG_INFO("CreateInstance: no NVIDIA device found — layer transparent for inst=%p", *pInst);
            }
        }
    }

    inst_insert(node);

    layer_check_disabled();
    LOG_INFO("CreateInstance: inst=%p%s", *pInst, g_layer_disabled ? " (disabled)" : "");
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL
layer_DestroyInstance(VkInstance inst, const VkAllocationCallbacks *pAlloc) {
    InstNode *node = inst_lookup(dispatch_key(inst));
    if (!node) return;
    node->d.DestroyInstance(inst, pAlloc);
    inst_remove(dispatch_key(inst));
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
                    const VkAllocationCallbacks *pAlloc, VkDevice *pDev) {
    VkLayerDeviceCreateInfo *lci = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (lci && !(lci->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                    lci->function == VK_LAYER_LINK_INFO))
        lci = (VkLayerDeviceCreateInfo *)lci->pNext;
    if (!lci) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = lci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr   next_gdpa = lci->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    lci->u.pLayerInfo = lci->u.pLayerInfo->pNext;

    /* Vendor check: this layer is designed exclusively for NVIDIA Tegra
       (vendorID 0x10DE).  For any other driver — llvmpipe, Mesa radv,
       etc. — pass through entirely without modifying the extension list
       or tracking the device.  This prevents the layer from injecting
       extensions the driver doesn't support and from crashing on missing
       entrypoints. */
    {
        InstNode *inst_hint = NULL;
        pthread_mutex_lock(&g_inst_lock);
        for (int b = 0; b < HASH_BUCKETS && !inst_hint; b++)
            for (InstNode *n = g_inst_table[b]; n; n = n->next) { inst_hint = n; break; }
        pthread_mutex_unlock(&g_inst_lock);

        if (inst_hint && inst_hint->d.GetPhysicalDeviceProperties) {
            VkPhysicalDeviceProperties props = {0};
            inst_hint->d.GetPhysicalDeviceProperties(phys, &props);
            if (props.vendorID != 0x10DE) {
                LOG_INFO("CreateDevice: non-NVIDIA device (vendor=0x%04x, '%s') — layer transparent for this device",
                         props.vendorID, props.deviceName);
                /* Call through with the original (unmodified) extension list.
                   Then create a minimal passthrough DevNode that stores
                   next_gdpa.  This is essential: our GDPA always runs first
                   in the chain, and without a DevNode it would return NULL
                   for non-intercepted functions (e.g. vkGetDeviceQueue),
                   leaving the app with NULL function pointers. */
                PFN_vkCreateDevice next_cd =
                    (PFN_vkCreateDevice)next_gipa(VK_NULL_HANDLE, "vkCreateDevice");
                if (!next_cd) return VK_ERROR_INITIALIZATION_FAILED;
                VkResult r = next_cd(phys, ci, pAlloc, pDev);
                if (r != VK_SUCCESS) return r;

                DevNode *pt = calloc(1, sizeof(*pt));
                if (!pt) return VK_ERROR_OUT_OF_HOST_MEMORY;
                pt->device      = *pDev;
                pt->key         = dispatch_key(*pDev);
                pt->passthrough = true;
                /* Store next_gdpa in d.GetDeviceProcAddr; GDPA uses it to
                   forward all lookups to the next layer for this device. */
                pt->d.GetDeviceProcAddr = next_gdpa;
                dev_insert(pt);
                return VK_SUCCESS;
            }
        }
    }

    /* The application's vkCreateDevice doesn't enable the extensions we
       depend on (VK_KHR_external_memory_fd, VK_KHR_external_semaphore_fd,
       etc.). We need them for the present path. Build a modified
       VkDeviceCreateInfo with our extensions appended if not already
       present, then call through with that. */
    static const char *required[] = {
        "VK_KHR_external_memory",
        "VK_KHR_external_memory_fd",
        "VK_KHR_external_semaphore",
        "VK_KHR_external_semaphore_fd",
        "VK_KHR_dedicated_allocation",
        "VK_KHR_get_memory_requirements2",
    };
    static const uint32_t n_required = sizeof(required) / sizeof(required[0]);

    /* Count how many of our required extensions the app didn't already enable. */
    uint32_t to_add = 0;
    bool need[6] = { true, true, true, true, true, true };
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        for (uint32_t j = 0; j < n_required; j++) {
            if (need[j] && !strcmp(ci->ppEnabledExtensionNames[i], required[j])) {
                need[j] = false;
            }
        }
    }
    for (uint32_t j = 0; j < n_required; j++) if (need[j]) to_add++;

    /* Extensions this LAYER provides to applications but which the
       underlying ICD does NOT implement. We must strip these from the
       extension list before passing CreateDevice down — otherwise the
       ICD rejects device creation with ERROR_EXTENSION_NOT_PRESENT.
       Apps that enable these extensions still get them: our intercepts
       are wired in at GetDeviceProcAddr time regardless of what the ICD
       says. */
    static const char *layer_provided[] = {
        "VK_GOOGLE_display_timing",
    };
    static const uint32_t n_layer_provided = 1;

    /* Build the down-going extension list: app's list minus any
       layer-provided names, plus our required extensions if not
       already present. We always allocate a new array since we may need
       to delete entries even if to_add is zero. */
    VkDeviceCreateInfo modci = *ci;
    uint32_t copy_n = 0;
    const char **new_exts = malloc(
        (ci->enabledExtensionCount + to_add) * sizeof(const char *));
    if (!new_exts) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        bool strip = false;
        for (uint32_t j = 0; j < n_layer_provided; j++) {
            if (!strcmp(ci->ppEnabledExtensionNames[i], layer_provided[j])) { strip = true; break; }
        }
        if (!strip) new_exts[copy_n++] = ci->ppEnabledExtensionNames[i];
    }
    for (uint32_t j = 0; j < n_required; j++) if (need[j]) new_exts[copy_n++] = required[j];
    modci.enabledExtensionCount = copy_n;
    modci.ppEnabledExtensionNames = new_exts;
    if (to_add > 0) {
        LOG_INFO("CreateDevice: appending %u required extensions", to_add);
    }

    PFN_vkCreateDevice next_create = (PFN_vkCreateDevice)next_gipa(VK_NULL_HANDLE, "vkCreateDevice");
    VkResult r = next_create(phys, &modci, pAlloc, pDev);
    if (new_exts) free(new_exts);
    if (r != VK_SUCCESS) {
        LOG_ERR("next CreateDevice failed: %d (extension support missing?)", r);
        return r;
    }

    InstNode *in = NULL;
    /* Find the instance whose physical device list contains 'phys'. We don't
       track that explicitly; just take the only instance in our table.
       This is brittle for multi-instance apps but matches our actual use. */
    pthread_mutex_lock(&g_inst_lock);
    for (int b = 0; b < HASH_BUCKETS && !in; b++)
        for (InstNode *n = g_inst_table[b]; n; n = n->next) { in = n; break; }
    pthread_mutex_unlock(&g_inst_lock);

    DevNode *node = calloc(1, sizeof(*node));
    if (!node) return VK_ERROR_OUT_OF_HOST_MEMORY;
    node->device = *pDev;
    node->phys = phys;
    node->key = dispatch_key(*pDev);
    node->idisp = in ? &in->d : NULL;
    node->inst = in ? in->instance : VK_NULL_HANDLE;

#define D(name) node->d.name = (PFN_vk##name)next_gdpa(*pDev, "vk" #name)
    D(GetDeviceProcAddr);
    D(DestroyDevice);
    D(DeviceWaitIdle);
    D(GetDeviceQueue);
    D(QueueSubmit);
    D(QueueWaitIdle);
    D(CreateImage);
    D(DestroyImage);
    D(GetImageMemoryRequirements);
    D(AllocateMemory);
    D(FreeMemory);
    D(BindImageMemory);
    D(MapMemory);
    D(UnmapMemory);
    D(CreateSemaphore);
    D(DestroySemaphore);
    D(CreateFence);
    D(DestroyFence);
    D(ResetFences);
    D(WaitForFences);
    D(GetFenceStatus);
    D(GetMemoryFdKHR);
    D(GetSemaphoreFdKHR);
    D(CmdPipelineBarrier);
    D(EndCommandBuffer);
    D(CreateCommandPool);
    D(DestroyCommandPool);
    D(AllocateCommandBuffers);
    D(FreeCommandBuffers);
    D(BeginCommandBuffer);
    D(CmdCopyImage);
    D(CmdClearColorImage);
    D(CreateBuffer);
    D(DestroyBuffer);
    D(GetBufferMemoryRequirements);
    D(BindBufferMemory);
    D(CmdFillBuffer);
    D(CreateShaderModule);
    D(DestroyShaderModule);
    D(CreateDescriptorSetLayout);
    D(DestroyDescriptorSetLayout);
    D(CreatePipelineLayout);
    D(DestroyPipelineLayout);
    D(CreateComputePipelines);
    D(DestroyPipeline);
    D(CreateDescriptorPool);
    D(DestroyDescriptorPool);
    D(AllocateDescriptorSets);
    D(UpdateDescriptorSets);
    D(CreateImageView);
    D(DestroyImageView);
    D(CmdBindPipeline);
    D(CmdBindDescriptorSets);
    D(CmdDispatch);
    D(CmdPushConstants);
    D(GetImageSubresourceLayout);
    D(CreateRenderPass);
    /* Vulkan 1.2 render pass v2: core name first, KHR alias as fallback
       for drivers that only expose the extension entrypoint. */
    D(CreateRenderPass2);
    if (!node->d.CreateRenderPass2)
        node->d.CreateRenderPass2 = (PFN_vkCreateRenderPass2)next_gdpa(*pDev, "vkCreateRenderPass2KHR");
    D(CreateRenderPass2KHR);
    if (!node->d.CreateRenderPass2KHR)
        node->d.CreateRenderPass2KHR = node->d.CreateRenderPass2;
    D(CreateSwapchainKHR);
    D(DestroySwapchainKHR);
    D(GetSwapchainImagesKHR);
    D(AcquireNextImageKHR);
    D(QueuePresentKHR);
#undef D

    /* Sanity check: any of these being NULL means the next layer / ICD
       didn't expose them, which means our present path can't function.
       Don't crash — set the disabled flag so subsequent swapchain hooks
       fall through to passthrough. GetSemaphoreFdKHR isn't required here
       (unlike the real project this was copied from) -- FLIP_TEST never
       exports semaphores cross-API, see create_exportable_semaphores. */
    if (!node->d.GetMemoryFdKHR || !node->d.CreateSwapchainKHR) {
        LOG_WARN("CreateDevice: required entrypoints missing (GetMemoryFdKHR=%p CreateSwapchainKHR=%p); disabling layer for this device",
                 (void*)node->d.GetMemoryFdKHR,
                 (void*)node->d.CreateSwapchainKHR);
        g_layer_disabled = true;
    }

    if (in) in->d.GetPhysicalDeviceMemoryProperties(phys, &node->memp);

    /* Find the first graphics-capable queue family and cache it. */
    uint32_t nqf = 0;
    if (in) in->d.GetPhysicalDeviceQueueFamilyProperties(phys, &nqf, NULL);
    VkQueueFamilyProperties *qfs = nqf ? calloc(nqf, sizeof(*qfs)) : NULL;
    if (in && qfs) in->d.GetPhysicalDeviceQueueFamilyProperties(phys, &nqf, qfs);
    node->graphics_qfi = 0;
    for (uint32_t i = 0; i < nqf; i++)
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { node->graphics_qfi = i; break; }
    free(qfs);
    node->d.GetDeviceQueue(*pDev, node->graphics_qfi, 0, &node->graphics_queue);
    pthread_mutex_init(&node->submit_lock, NULL);

    dev_insert(node);
    LOG_INFO("CreateDevice: dev=%p qfi=%u", *pDev, node->graphics_qfi);
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL
layer_DestroyDevice(VkDevice dev, const VkAllocationCallbacks *pAlloc) {
    DevNode *node = dev_lookup(dispatch_key(dev));
    if (!node) return;
    if (node->passthrough) {
        /* Passthrough device: look up the real DestroyDevice via the stored
           next_gdpa, remove our DevNode, then call through. */
        PFN_vkDestroyDevice real_destroy =
            (PFN_vkDestroyDevice)node->d.GetDeviceProcAddr(dev, "vkDestroyDevice");
        dev_remove(dispatch_key(dev));
        if (real_destroy) real_destroy(dev, pAlloc);
        return;
    }
    node->d.DestroyDevice(dev, pAlloc);
    pthread_mutex_destroy(&node->submit_lock);
    dev_remove(dispatch_key(dev));
}

/* ----------------------------------------------------------------------- */
/* Entrypoint dispatch                                                     */
/* ----------------------------------------------------------------------- */

static PFN_vkVoidFunction layer_intercept_instance(const char *name) {
#define MATCH(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)layer_##n
    MATCH(GetInstanceProcAddr);
    MATCH(CreateInstance);
    MATCH(DestroyInstance);
    MATCH(CreateDevice);
    MATCH(CreateXlibSurfaceKHR);
    MATCH(CreateXcbSurfaceKHR);
    MATCH(DestroySurfaceKHR);
    MATCH(GetPhysicalDeviceSurfaceSupportKHR);
    MATCH(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    MATCH(GetPhysicalDeviceSurfaceFormatsKHR);
    MATCH(GetPhysicalDeviceSurfacePresentModesKHR);
    MATCH(GetPhysicalDeviceSurfaceCapabilities2KHR);
    MATCH(GetPhysicalDeviceSurfaceFormats2KHR);
#undef MATCH
    return NULL;
}

static PFN_vkVoidFunction layer_intercept_device(const char *name) {
#define MATCH(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)layer_##n
    MATCH(GetDeviceProcAddr);
    MATCH(DestroyDevice);
    MATCH(CreateSwapchainKHR);
    MATCH(DestroySwapchainKHR);
    MATCH(GetSwapchainImagesKHR);
    MATCH(AcquireNextImageKHR);
    MATCH(QueuePresentKHR);
    MATCH(GetRefreshCycleDurationGOOGLE);
    MATCH(GetPastPresentationTimingGOOGLE);
    MATCH(QueueSubmit);
    MATCH(QueueWaitIdle);
    MATCH(DeviceWaitIdle);
    MATCH(WaitForFences);
    MATCH(ResetFences);
    MATCH(CmdPipelineBarrier);
    MATCH(EndCommandBuffer);
    MATCH(CreateRenderPass);
    MATCH(CreateRenderPass2);
    MATCH(CreateRenderPass2KHR);
#undef MATCH
    return NULL;
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetDeviceProcAddr(VkDevice dev, const char *name) {
    DevNode *node = dev_lookup(dispatch_key(dev));
    if (!node) return NULL;
    if (node->passthrough) {
        /* Non-NVIDIA passthrough device.  Return the next layer's function
           for everything except the two functions we must still intercept:
           DestroyDevice (to clean up our DevNode) and CreateSwapchainKHR
           (to substitute the real ICD surface handle for our Surface*
           wrapper before forwarding to the ICD). */
        if (!strcmp(name, "vkDestroyDevice"))     return (PFN_vkVoidFunction)layer_DestroyDevice;
        if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)layer_CreateSwapchainKHR;
        return node->d.GetDeviceProcAddr(dev, name);
    }
    PFN_vkVoidFunction fn = layer_intercept_device(name);
    if (fn) return fn;
    return node->d.GetDeviceProcAddr(dev, name);
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetInstanceProcAddr(VkInstance inst, const char *name) {
    PFN_vkVoidFunction fn = layer_intercept_instance(name);
    if (fn) return fn;
    fn = layer_intercept_device(name);
    if (fn) return fn;
    if (!inst) return NULL;
    InstNode *node = inst_lookup(dispatch_key(inst));
    if (!node) return NULL;
    return node->d.GetInstanceProcAddr(inst, name);
}

/* ----------------------------------------------------------------------- */
/* Loader negotiation (Vulkan layer interface v2)                          */
/* ----------------------------------------------------------------------- */

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pInterface) {
    if (pInterface->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    pInterface->loaderLayerInterfaceVersion = 2;
    /* Wire negotiate to our INTERNAL (hidden) GIPA/GDPA, not the public
       wrappers. The public wrappers are no longer exported by symbol; this
       eliminates any chance of the loader's ICD-load path dlsym-ing our
       symbol and re-entering us. The loader doesn't need the public symbol
       in interface v2 — it stores these function pointers and calls them
       directly. */
    pInterface->pfnGetInstanceProcAddr      = layer_GetInstanceProcAddr;
    pInterface->pfnGetDeviceProcAddr        = layer_GetDeviceProcAddr;
    pInterface->pfnGetPhysicalDeviceProcAddr = NULL;
    layer_log_init();

    /* Loud one-time banner so a tester running an older cached binary can spot
       the version mismatch immediately. Bump when the layer's behaviour changes. */
    fprintf(stderr, "[" LAYER_NAME "] loaded, build %s %s (negotiated interface v2)\n",
            __DATE__, __TIME__);
    return VK_SUCCESS;
}

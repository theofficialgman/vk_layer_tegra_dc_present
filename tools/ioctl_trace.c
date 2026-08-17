/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ioctl_trace.c - standalone LD_PRELOAD ioctl() tracer.
 *
 * Diagnoses whether a process's presentation path reaches the Tegra
 * display controller's real vsync-gated hardware flip mechanism
 * (TEGRA_DC_EXT_FLIP4 on /dev/tegra_dc_N) or the host1x syncpoint wait
 * ioctls on /dev/nvhost-ctrl (NVHOST_IOCTL_CTRL_SYNCPT_WAITEX/WAITMEX),
 * versus never touching those device nodes at all.
 *
 * This is a standalone diagnostic tool. It is independent of
 * vk_layer_tegra_x11_present.c and does not use or modify any of the
 * layer's Vulkan entry points.
 *
 * The ioctl request numbers decoded below were read directly out of the
 * L4T r32.x kernel headers on this system:
 *   include/uapi/video/tegra_dc_ext.h  (magic 'D' = /dev/tegra_dc_N,
 *                                        magic 'C' = /dev/tegra_dc_ctrl)
 *   include/uapi/linux/nvhost_ioctl.h  (magic 'H' = /dev/nvhost-ctrl*)
 * Decoding is by (type,nr) only -- the ioctl() request value does not
 * need struct-layout knowledge to intercept and identify, only to
 * fully decode its argument, which we don't attempt here.
 *
 * Usage:
 *   LD_PRELOAD=/path/to/ioctl_trace.so <command...>
 *
 * Environment variables:
 *   IOCTL_TRACE_LOG_FILE=path   default: stderr
 *   IOCTL_TRACE_MATCH=substr    extra substring to match against the
 *                                 resolved fd path (default: none).
 *                                 "tegra_dc", "nvhost", and "nvmap" are
 *                                 always matched.
 *
 * Build (native, on-target):
 *   gcc -O2 -Wall -fPIC -shared -o ioctl_trace.so ioctl_trace.c -ldl
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <pthread.h>
#include <stdint.h>

static int (*real_ioctl)(int, unsigned long, ...) = NULL;
static FILE *log_fp = NULL;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void trace_init(void)
{
    if (!real_ioctl)
        real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    if (!log_fp) {
        const char *path = getenv("IOCTL_TRACE_LOG_FILE");
        FILE *fp = path ? fopen(path, "a") : NULL;
        log_fp = fp ? fp : stderr;
        setvbuf(log_fp, NULL, _IOLBF, 0);
    }
}

/* Decode a (type,nr) pair. Returns NULL for anything not in the table --
   the raw hex/type/nr/size is always printed regardless. */
static const char *decode(unsigned long req)
{
    unsigned int type = _IOC_TYPE(req);
    unsigned int nr   = _IOC_NR(req);

    if (type == 'D') { /* /dev/tegra_dc_N */
        switch (nr) {
        case 0x00: return "TEGRA_DC_EXT_SET_NVMAP_FD";
        case 0x01: return "TEGRA_DC_EXT_GET_WINDOW";
        case 0x02: return "TEGRA_DC_EXT_PUT_WINDOW";
        case 0x04: return "TEGRA_DC_EXT_GET_CURSOR";
        case 0x05: return "TEGRA_DC_EXT_PUT_CURSOR";
        case 0x06: return "TEGRA_DC_EXT_SET_CURSOR_IMAGE";
        case 0x07: return "TEGRA_DC_EXT_SET_CURSOR";
        case 0x08: return "TEGRA_DC_EXT_SET_CSC";
        case 0x09: return "TEGRA_DC_EXT_GET_STATUS/GET_VBLANK_SYNCPT";
        case 0x0A: return "TEGRA_DC_EXT_SET_LUT";
        case 0x0C: return "TEGRA_DC_EXT_CURSOR_CLIP";
        case 0x0D: return "TEGRA_DC_EXT_SET_CMU";
        case 0x0F: return "TEGRA_DC_EXT_GET_CMU";
        case 0x10: return "TEGRA_DC_EXT_GET_CUSTOM_CMU";
        case 0x13: return "TEGRA_DC_EXT_SET_PROPOSED_BW";
        case 0x15: return "TEGRA_DC_EXT_SET_VBLANK";
        case 0x16: return "TEGRA_DC_EXT_SET_CMU_ALIGNED";
        case 0x17: return "TEGRA_DC_EXT_SET_NVDISP_WIN_CSC";
        case 0x18: return "TEGRA_DC_EXT_SET_NVDISP_CMU";
        case 0x19: return "TEGRA_DC_EXT_GET_NVDISP_CMU";
        case 0x1A: return "TEGRA_DC_EXT_GET_CUSTOM_NVDISP_CMU";
        case 0x1B: return "TEGRA_DC_EXT_SET_PROPOSED_BW_3";
        case 0x1C: return "TEGRA_DC_EXT_GET_CMU_ADBRGB";
        case 0x1D: return "TEGRA_DC_EXT_FLIP4  <-- real vsync-gated hardware flip";
        case 0x1E: return "TEGRA_DC_EXT_GET_WINMASK";
        case 0x1F: return "TEGRA_DC_EXT_SET_WINMASK";
        case 0x24: return "TEGRA_DC_EXT_GET_SCANLINE";
        case 0x25: return "TEGRA_DC_EXT_SET_SCANLINE";
        case 0x26: return "TEGRA_DC_EXT_CRC_ENABLE";
        case 0x27: return "TEGRA_DC_EXT_CRC_DISABLE";
        case 0x28: return "TEGRA_DC_EXT_CRC_GET";
        }
    } else if (type == 'C') { /* /dev/tegra_dc_ctrl */
        switch (nr) {
        case 0x00: return "TEGRA_DC_EXT_CONTROL_GET_NUM_OUTPUTS";
        case 0x01: return "TEGRA_DC_EXT_CONTROL_GET_OUTPUT_PROPERTIES";
        case 0x02: return "TEGRA_DC_EXT_CONTROL_GET_OUTPUT_EDID";
        case 0x03: return "TEGRA_DC_EXT_CONTROL_SET_EVENT_MASK";
        case 0x04: return "TEGRA_DC_EXT_CONTROL_GET_CAPABILITIES";
        case 0x05: return "TEGRA_DC_EXT_CONTROL_SCRNCAPT_PAUSE";
        case 0x06: return "TEGRA_DC_EXT_CONTROL_SCRNCAPT_RESUME";
        case 0x09: return "TEGRA_DC_EXT_CONTROL_GET_CAP_INFO";
        }
    } else if (type == 'H') { /* /dev/nvhost-ctrl* */
        switch (nr) {
        case 1:  return "NVHOST_IOCTL_CTRL_SYNCPT_READ";
        case 2:  return "NVHOST_IOCTL_CTRL_SYNCPT_INCR";
        case 3:  return "NVHOST_IOCTL_CTRL_SYNCPT_WAIT";
        case 6:  return "NVHOST_IOCTL_CTRL_SYNCPT_WAITEX  <-- blocking syncpt wait (e.g. vblank)";
        case 7:  return "NVHOST_IOCTL_CTRL_GET_VERSION";
        case 8:  return "NVHOST_IOCTL_CTRL_SYNCPT_READ_MAX";
        case 9:  return "NVHOST_IOCTL_CTRL_SYNCPT_WAITMEX  <-- blocking syncpt wait + timestamp";
        case 11: return "NVHOST_IOCTL_CTRL_SYNC_FENCE_CREATE";
        case 16: return "NVHOST_IOCTL_CTRL_POLL_FD_CREATE";
        case 17: return "NVHOST_IOCTL_CTRL_POLL_FD_TRIGGER_EVENT";
        }
    }
    return NULL;
}

/* Best-effort decode of the syncpt id/thresh/value fields relevant to the
   handful of ioctls we care about. Struct layouts (first fields only,
   which is all we need) taken directly from
   include/uapi/linux/nvhost_ioctl.h and include/uapi/video/tegra_dc_ext.h
   on this kernel tree:
     nvhost_ctrl_syncpt_read_args   { __u32 id; __u32 value; }
     nvhost_ctrl_syncpt_waitex_args { __u32 id; __u32 thresh; __s32 timeout; __u32 value; }
     nvhost_ctrl_syncpt_waitmex_args{ __u32 id; __u32 thresh; __s32 timeout; __u32 value; __u64 tv_sec; __u32 tv_nsec; __u32 clock_id; }
     TEGRA_DC_EXT_GET_VBLANK_SYNCPT out arg: plain __u32 */
static void decode_args(unsigned long req, void *arg, char *buf, size_t buflen,
                         const uint32_t *pre)
{
    unsigned int type = _IOC_TYPE(req);
    unsigned int nr   = _IOC_NR(req);
    buf[0] = '\0';

    if (type == 'D' && nr == 0x09 && _IOC_SIZE(req) == 4) {
        /* GET_VBLANK_SYNCPT: output-only __u32 */
        snprintf(buf, buflen, " vblank_syncpt_id=%u", *(uint32_t *)arg);
    } else if (type == 'H' && (nr == 1 || nr == 6 || nr == 9)) {
        /* SYNCPT_READ / WAITEX / WAITMEX: id,thresh are input (read
           pre-call), value is output (read post-call, offset 12 for
           WAITEX/WAITMEX; offset 4 for READ). */
        uint32_t id = pre[0];
        if (nr == 1) {
            uint32_t value = *((uint32_t *)arg + 1);
            snprintf(buf, buflen, " id=%u value=%u", id, value);
        } else {
            uint32_t thresh = pre[1];
            uint32_t value  = *((uint32_t *)arg + 3);
            snprintf(buf, buflen, " id=%u thresh=%u value=%u", id, thresh, value);
        }
    }
}

int ioctl(int fd, unsigned long req, ...)
{
    trace_init();

    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    char linkpath[64], path[256];
    snprintf(linkpath, sizeof(linkpath), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(linkpath, path, sizeof(path) - 1);
    if (n >= 0)
        path[n] = '\0';
    else
        path[0] = '\0';

    const char *extra = getenv("IOCTL_TRACE_MATCH");
    int interesting = (n >= 0) &&
        (strstr(path, "tegra_dc") || strstr(path, "nvhost") ||
         strstr(path, "nvmap") || (extra && strstr(path, extra)));

    /* Snapshot input fields (id/thresh) before the call -- once the real
       ioctl returns, id/thresh may have been overwritten by the driver's
       output fields depending on struct layout, so we can't rely on
       reading them post-call. Only for the syncpt read/wait family,
       whose structs are known to be >= 8 bytes -- avoids reading past a
       smaller arg buffer (e.g. the plain __u32 used by
       GET_VBLANK_SYNCPT/GET_WINMASK/etc). */
    uint32_t pre[2] = {0, 0};
    unsigned int _t = _IOC_TYPE(req), _nr = _IOC_NR(req);
    int is_syncpt_rw = (_t == 'H' && (_nr == 1 || _nr == 6 || _nr == 9));
    if (interesting && arg && is_syncpt_rw) {
        pre[0] = ((uint32_t *)arg)[0];
        pre[1] = ((uint32_t *)arg)[1];
    }

    int ret = real_ioctl(fd, req, arg);

    if (interesting) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const char *name = decode(req);
        char argbuf[128];
        decode_args(req, arg, argbuf, sizeof(argbuf), pre);
        pthread_mutex_lock(&log_lock);
        fprintf(log_fp,
                "[%ld.%06ld] pid=%d tid=%d fd=%d path=%s req=0x%08lx "
                "dir=%u type=%c nr=0x%02x size=%u name=%s%s ret=%d\n",
                (long)ts.tv_sec, ts.tv_nsec / 1000, getpid(), gettid(),
                fd, path, req,
                (unsigned)_IOC_DIR(req), (char)_IOC_TYPE(req),
                (unsigned)_IOC_NR(req), (unsigned)_IOC_SIZE(req),
                name ? name : "?", argbuf, ret);
        fflush(log_fp);
        pthread_mutex_unlock(&log_lock);
    }

    return ret;
}

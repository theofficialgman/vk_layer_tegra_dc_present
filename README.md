# FLIP4 present prototype — SOLVED (see "Update 2026-08-16 part 3" at the bottom)

Standalone Vulkan explicit layer that swaps the GL/GLX `glXSwapBuffers` present
step (from the real `vk_layer_tegra_x11_present.c`) for a direct
`TEGRA_DC_EXT_FLIP4` ioctl call on the display controller, to test whether
presenting real, GPU-fenced Vulkan-rendered content this way avoids the
tearing that native `vkQueuePresentKHR` exhibits on this driver.

It started as a copy of the real layer with the final present step replaced,
and was later stripped of the GL rendering/fallback infrastructure entirely
(no `gl_import_image`, no shader/blit program, no GL fallback path) once the
investigation concluded and the pure-Vulkan path was confirmed working —
this file now has no GL rendering code at all, only the minimal GLX context
kept alive for `glXWaitVideoSyncSGI` timing. Setup failures (opening the DC
device, claiming the window, missing `GLX_SGI_video_sync`, etc.) are hard
failures, not silent degradation to a different presentation mechanism —
appropriate for a focused test of one specific thing, not how the real
production layer should behave. Not integrated with the real layer; runs
standalone via explicit layer activation, no system install.

## Environment variable reference

Quick-reference for every env var this layer reads. Defaults produce the
current, confirmed-working, tear-free configuration (real content, GOB
block-linear, GLX SGI-wait pacing, fullscreen-only) -- everything below is
either a diagnostic knob from the investigation or an escape hatch, safe
to leave unset for normal use.

**Core behavior**
- `VK_TEGRA_X11_PRESENT_DISABLE=1` -- transparent passthrough; the layer
  does nothing, native Vulkan WSI handles everything (tearing, same as no
  layer at all). Useful for A/B comparison.
- `VK_TEGRA_X11_PRESENT_LOG=<0-3>` -- log level: 0 silent, 1 warn/err,
  2 info (recommended for normal runs), 3 debug.
- `VK_TEGRA_X11_PRESENT_LOG_FILE=<path>` -- also append the log to a file.
- `VK_TEGRA_X11_PRESENT_DIAG=1` -- calls `vkDeviceWaitIdle` after every
  single `vkQueueSubmit` (ours and the app's), logging a sequence number
  for each. Pinpoints exactly which submit faults instead of letting a GPU
  fault surface later as `DEVICE_LOST` on some unrelated call. Extremely
  slow (every submit becomes synchronous) -- one-shot fault localization
  only, not for normal runs.

**Content path** (default: real content, GOB block-linear, tear-free)
- `FLIP_TEST_GOB_REAL=0` -- disable the default. Real per-frame content
  through the verified GOB block-linear compute shader is what every app
  in this investigation (gears, vkcube, vkgears, dolphin-emu, the Play
  emulator) has been tested and fixed against; there's no longer a reason
  to opt out except to compare against the older paths below.
- `FLIP_TEST_BLOCKLINEAR=1` -- (only takes effect with `FLIP_TEST_GOB_REAL=0`)
  retest FLIP4 against a raw `BLOCKLINEAR`-flagged `OPTIMAL` image
  directly, bypassing the GOB compute shader entirely. Historical: this is
  what first proved BLOCKLINEAR itself is tear-free, before the correct
  swizzle formula was derived (see "Update 2026-08-16" parts 1-2) -- the
  content it produces is visually wrong (untiled), useful only for
  re-confirming tearing behavior in isolation.
- `FLIP_TEST_SOLID_FILL=1` -- alternating solid-color fill instead of real
  content, isolating tearing-causality from content-correctness.
  `FLIP_TEST_SOLID_FILL_PERIOD=<N>` overrides the toggle frequency
  (default 20 frames, ~0.33s at 60fps).
- `FLIP_TEST_GOB_PROBE=<1|2>` -- diagnostic patterns used to derive/verify
  the GOB tiling formula: mode 1 writes a coordinate-revealing pattern to
  a LINEAR buffer flagged BLOCKLINEAR (reveals the DC's real permutation
  empirically); mode 2 writes a candidate swizzle and checks whether
  BLOCKLINEAR-flagged FLIP4 reconstructs a clean gradient. Not for normal
  use -- see "Update 2026-08-16 part 2".
- `FLIP_TEST_BLOCKHEIGHT_LOG2=<N>` -- override the block-linear
  `block_height_log2` (default 4).
- `FLIP_TEST_GOB_ORDER=<0|1|2>` -- only relevant under
  `FLIP_TEST_GOB_PROBE=2`; historical hypothesis-testing knob for GOB
  ordering within a block (0 row-major, 1 column-major -- confirmed
  correct, 2 Morton/Z-order interleave). Not used by the real GOB_REAL
  path, which hardcodes the confirmed-correct column-major order.

**Window / DC targeting**
- `FLIP_TEST_DC=<N>` -- force `/dev/tegra_dc_<N>`, overriding the default
  XRandR-based auto-detection of which DC drives the window's current
  monitor.
- `FLIP_TEST_WIN=<N>` -- force the DC window (hardware overlay plane)
  index (default 1). Ownership has only been verified free on DC1 window
  1 -- do not point this at a different DC/window without checking first.
- `FLIP_TEST_ALLOW_WINDOWED=1` -- override the fullscreen gate (see
  "Update 2026-08-17 part 4") to test the FLIP4 path itself against a
  non-fullscreen window. The overlay's position will be wrong and it won't
  respect window occlusion -- diagnostic only, never for normal use.
- `FLIP_TEST_ALLOW_IMMEDIATE=1` -- override the IMMEDIATE mode gate (see
  "Update 2026-08-18 part 2") to test the FLIP4 path itself against a
  `VK_PRESENT_MODE_IMMEDIATE_KHR` swapchain. Real per-frame overhead with
  no tearing benefit for an app that already accepts tearing -- diagnostic
  only, never for normal use.

**Buffering / pacing**
- `FLIP_TEST_MIN_IMAGES=<N>` -- forces the swapchain image count to
  exactly `N`, overriding both the app's own request and the built-in
  safety floor (3, raised from 2 after "2-image swapchains are unsafe",
  see "Update 2026-08-17"). Works in both directions: `N` above what the
  app requests raises it for margin testing, `N` below the floor
  (including back down to 2) deliberately re-enables the confirmed
  2-image tearing bug on demand, e.g. to re-verify it against a specific
  app without a source edit. Logs a warning when going below the floor.
- `FLIP_TEST_FORCE_FIFO=1` -- force FIFO present mode regardless of what
  the app requests, for isolating whether a bug is specific to MAILBOX's
  displaced-image bookkeeping (see "Update 2026-08-17 part 3").
- `FLIP_TEST_KERNEL_WAIT=1` -- Option 1: adds a hardware `pre_syncpt_id`
  latch gate on top of the default GLX SGI wait. `FLIP_TEST_SYNCPT_OFFSET=<N>`
  overrides its target delta (default 1); large values (e.g. 300) are a
  proof-of-execution stall test, not a normal mode.
- `FLIP_TEST_KERNEL_PACE=<N>` -- Option 1b: fully GLX-free pacing via a
  blocking kernel syncpoint wait targeting every `N`th vblank (1 = ~60fps,
  2 = ~30fps); takes priority over `FLIP_TEST_KERNEL_WAIT` if both are
  set. Note a GLX context is still created and made current regardless --
  this only changes the pacing wait itself, not the rest of the swapchain
  setup. **Tested 2026-08-17 with `FLIP_TEST_GOB_REAL=1`: tears for the
  first few seconds before settling, worse than the default GLX SGI wait.**
  Kept for reference; not recommended over the default.
- `FLIP_TEST_DELAY_US=<us>` -- extra sleep inserted right before `FLIP4`,
  after the SGI wait. Used for a manual vblank-phase timing sweep.
- `FLIP_TEST_WAIT_AFTER_FLIP=0` -- moves the SGI vblank wait back to
  *before* `FLIP4` (the pre-2026-08-18 default), giving the deferred
  kernel worker a guaranteed-maximal, but higher-latency, margin before
  the next vblank. The default (unset, or explicitly `=1`) waits *after*
  `FLIP4` instead, calling it the instant content is ready for up to
  ~1 vblank interval (~16ms) less latency -- this exact ordering
  previously caused a consistent mid-frame tear on an earlier driver (see
  "Update 2026-08-18"), so revert to `=0` if tearing appears on a
  different driver, heavier scene, or under system load.

**Debugging**
- `FLIP_TEST_TRACE_SYNC=1` -- logs a global sequence-numbered trace of
  every touch of the per-image `vk_render_done`/`gl_sample_done`
  semaphores, tagged with the calling thread's TID. Built to chase the
  MAILBOX displaced-image race (see "Update 2026-08-17 part 3") by
  reconstructing real interleaving instead of guessing from code reading.
  Very verbose -- one line per semaphore touch, every frame.

## ⚠️ Superseded 2026-08-16 — see "Update" section below

The result and conclusion immediately below (originally titled "the hypothesis
is disproven") held for every test run through this prototype's LINEAR
detile-target architecture. It does **not** hold universally: a later test
using a `BLOCKLINEAR`-flagged buffer instead of `LINEAR` showed no detectable
tearing under a causally-isolated test, directly contradicting "FLIP4 itself
does not provide buffer-swap atomicity." The correct, narrower statement is
**"FLIP4 over a LINEAR-tiled buffer lacks atomicity on this hardware; FLIP4
over a BLOCKLINEAR-tiled buffer, matching NVIDIA's own tiling choice, does
not tear in testing."** Read the original result below for the LINEAR-path
evidence (still valid on its own terms), then read "Update 2026-08-16" for
what changed and why it's not yet a usable fix.

## Original result (LINEAR path only): FLIP4 tears too.

This version is **fully working and pure Vulkan** — no GL at all in the data
path (see "Final architecture" below) — and correctly:
- Presents at the right screen position, right orientation, fully opaque.
- Paces to real vsync (confirmed ~60fps, not free-running).
- Was tested with the `FLIP4` call reordered to fire at the same ideal
  timing phase (right after vblank, maximum lead time before the next one)
  that native Vulkan's own presentation appears to use.

**It still tears — at the same screen position/phase as native, unlayered
Vulkan presentation**, confirmed visually in both windowed and fullscreen
(2560x1600) modes.

Conclusion: `TEGRA_DC_EXT_FLIP4` itself does not provide the buffer-swap
atomicity that makes GL's own presentation tear-free on this driver, even
called about as correctly as it can be (real Vulkan-rendered content,
correct GPU fencing, correct format/geometry, vblank-aligned timing, no
scheduling primitive left untried — `tegra_dc_ext_flip_windowattr.timestamp`
was checked and is not a "schedule for exact future vblank" mechanism the
way `GLX_OML_sync_control`'s `target_msc` is; it's bookkeeping for
skipping/coalescing already-queued flips within one vsync window, nothing
more). This closes the loop on an earlier finding in the investigation that
led to this prototype: GL's own presentation was independently confirmed
(via kernel `dev_info` instrumentation) to **never call `FLIP4` at all**
when uncomposited. Put together: GL doesn't tear and doesn't use `FLIP4`;
we used `FLIP4` carefully and it tears anyway. Whatever gives GL's swap its
atomicity on this driver is something else entirely, inside NVIDIA's closed
GLX implementation, with no equivalent on the open `tegra_dc_ext` ioctl
surface available to userspace.

**Practical upshot for the actual project (as of this original result):**
this validates the existing GL/GLX bridge (`vk_layer_tegra_x11_present.c`)
as the right architecture, not a workaround standing in for a simpler fix
that was just waiting to be found — for the `LINEAR`-tiled path tested here.
**This was later found to be wrong — see "Update 2026-08-16 part 3" at the
bottom of this file.** A `BLOCKLINEAR`-tiled `FLIP4` target, fed real
content through a compute shader implementing the reverse-engineered GOB
tiling formula, presents correctly and tear-free, fullscreen, uncomposited,
with no GL/GLX bridge involved at all. The GL/GLX bridge is no longer the
only known working option — it's a real, working *alternative* now, with
its own tradeoffs (portability/robustness vs. this path's driver-version-
specific reverse-engineered constants) rather than the sole fix.

## Final architecture (pure Vulkan, no GL in the data path)

- `create_app_image`/`create_exportable_semaphores` create the app-facing
  `OPTIMAL`-tiled image and its `vk_render_done`/`gl_sample_done`
  semaphores. Simplified from the real project's versions (which these were
  originally copied from): no `OPAQUE_FD` export on either, since nothing
  imports them cross-API or cross-process anymore — the copy step reads the
  image directly within the same `VkDevice`, and the semaphores are used as
  plain `VkSemaphore`s.
- `create_flip_export_image` (new) creates a second, separate
  `VK_IMAGE_TILING_LINEAR`, `VK_IMAGE_USAGE_TRANSFER_DST_BIT` exportable
  image per swapchain image slot, transitions it once (`UNDEFINED` →
  `GENERAL` — linear images only reliably support `GENERAL`/
  `PREINITIALIZED` per spec) via a one-shot command buffer, exports its fd
  **once** at swapchain creation (no per-frame re-export), and records the
  real `rowPitch`/`offset` via `vkGetImageSubresourceLayout` (don't assume
  `width*4` — may have padding).
- Per frame (worker thread): one command buffer does the whole job —
  barrier the app's image `SHADER_READ_ONLY_OPTIMAL` → `TRANSFER_SRC_OPTIMAL`
  (matches the layout `layer_CmdPipelineBarrier`'s existing rewriting
  already leaves app images in), `vkCmdCopyImage` (GPU-side detile, no CPU
  readback, no GL top/bottom flip convention to fight — Vulkan has no such
  convention), barrier back to `SHADER_READ_ONLY_OPTIMAL`. Submitted with
  `pWaitSemaphores=[vk_render_done]`, `pSignalSemaphores=[gl_sample_done]`,
  a fence; `vkWaitForFences`. No cross-API semaphore import at all — both
  semaphores are used as plain `VkSemaphore`s the whole way.
- `glXWaitVideoSyncSGI` immediately before `FLIP4` (not after) — the one
  remaining GLX dependency, kept alive purely for vblank timing since
  there's no already-proven non-GLX vblank wait in this codebase. Ordering
  matters: doing the wait right before the flip (not after) gives the
  driver maximum lead time before the next vblank, which is what got the
  tear to move from mid-frame to matching native's position — though not
  eliminate it.
- No `nvmap` involvement at all — `dma_buf_get()` (what
  `tegra_dc_ext_pin_window()` actually calls) works with any dma-buf fd
  regardless of which subsystem exported it, so the raw Vulkan-exported fd
  is handed to `FLIP4` directly as `buff_id`.

## Bugs found and fixed along the way (all confirmed via kernel source, not guessed)

1. **`pre_syncpt_id` must be `(__u32)-1` (`NVSYNCPT_INVALID`), not left at 0.**
   `tegra_dc_ioctl` checks `(s32)pre_syncpt_id >= 0` to decide whether to
   wait on it; zero-initializing left it at literal syncpoint 0, which
   doesn't exist, spamming `nvhost_syncpt_wait_timeout: invalid syncpoint id 0`
   in `dmesg`.
2. **`global_alpha` must be `255`, not left at 0.** `window.c`'s blend code
   applies `global_alpha` *unconditionally*, regardless of
   `TEGRA_DC_EXT_BLEND_NONE` — `if (global_alpha == 255) { disable alpha
   blending } else { enable it at that value }`. Zero-initializing made the
   window blend at 0% opacity — real, correct pixel data reaching the
   display, rendered fully invisible.
3. **`buff_id` must be the dma-buf *fd*, not the nvmap *handle* number.**
   `tegra_dc_ext_pin_window()` (`ext/util.c`) calls `dma_buf_get(fd)`
   directly — it resolves against the *calling process's own fd table*, not
   any nvmap handle namespace. Passing the nvmap handle number (e.g. 1090)
   instead of the actual fd (e.g. 1093) meant `dma_buf_get` was resolving a
   small integer that happened to be *some other, unrelated open fd* in the
   process — explains the "shows stale/wrong content despite correct data
   in the buffer" symptom that took a long time to pin down.
4. **`glReadPixels` output needs a manual vertical flip** before handing to
   `FLIP4` — GL's row order is bottom-up, the display wants top-down. (Moot
   in the final architecture — the pure-Vulkan `vkCmdCopyImage` path has no
   such convention to fight at all.)
5. **Window 1 was found to be free** (unclaimed by Xorg) on `tegradc.1` via
   `/sys/kernel/debug/tegradc.1/window_toggle`; window 0 is Xorg's own and
   `GET_WINDOW` on it returns `-EBUSY`. `DC_N_WINDOWS=6` is a theoretical
   max — this specific DC instance only has 3 real windows (0-2) in
   `dc->valid_windows`.
6. **`TEGRA_DC_EXT_SET_NVMAP_FD` is a complete no-op** in this kernel
   (`case TEGRA_DC_EXT_SET_NVMAP_FD: return 0;`, dev.c) — doesn't matter
   since `dma_buf_get` doesn't need it (see fix #3), and the final
   architecture doesn't touch `/dev/nvmap` at all.
7. **Flip call ordering relative to vblank matters for *where* the tear
   lands, not *whether* it tears.** Calling `FLIP4` immediately when
   render-done, then SGI-waiting afterward (pacing only the *next*
   iteration), put the tear consistently mid-frame. Moving the SGI wait to
   immediately before `FLIP4` (maximum lead time before the next vblank)
   moved the tear to match native Vulkan's own position — but didn't
   remove it, which is the core finding above.

## Build & run (explicit layer, no system install)

```sh
gcc -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -fPIC -fvisibility=hidden -shared -Wl,--no-undefined -Wl,--version-script=flip_layer.map \
    -I. -o libVkLayer_flip_test.so flip_layer.c -lpthread -ldl

VK_LAYER_PATH=$(pwd) VK_INSTANCE_LAYERS=VK_LAYER_FLIP_test VK_TEGRA_X11_PRESENT_LOG=2 \
    ~/Vulkan/build/bin/gears -vs        # windowed -- falls through to native WSI, see part 4
    # or: ~/Vulkan/build/bin/gears -f -vs   # fullscreen -- engages FLIP4, tear-free by default
```

See the "Environment variable reference" section near the top of this file
for every env var this layer reads and what it does -- the defaults
already produce the confirmed-working, tear-free configuration, nothing
below is required for normal use.

Safety notes if you're poking at this again:
- Targets window index 1 on `tegradc.1` — confirmed free (Xorg owns window
  0 only). Do NOT try window 0.
- The real `vk_layer_tegra_x11_present.json` implicit layer must stay out
  of `/usr/share/vulkan/implicit_layer.d/` while testing this (it was
  removed earlier in the investigation this prototype came from) —
  otherwise both layers would stack.
- Compositor (KWin) should be off for a clean test —
  `qdbus org.kde.KWin /Compositor org.kde.kwin.Compositing.active` should
  say `false`; `xprop -root -notype _NET_WM_CM_S0` should say "not found".

## Option 1 / 1b: ruling out software wait jitter as the cause

The one loose end left by the original investigation: was the residual
near-top tear actually coming from `glXWaitVideoSyncSGI`'s own wakeup
latency (a software sleep woken by the scheduler, not a hardware
interrupt-precise primitive) rather than from `FLIP4` itself? Four
independently-built timing/pacing mechanisms were tested to close this out:

1. **Baseline** -- userspace `glXWaitVideoSyncSGI`, a software sleep, used
   for both pacing and phase alignment (as described above).
2. **Option 1** (`FLIP_TEST_KERNEL_WAIT=1`) -- keeps the SGI wait for
   pacing, but additionally sets `FLIP4`'s `pre_syncpt_id`/`pre_syncpt_val`
   to the DC's real vblank syncpoint (`TEGRA_DC_EXT_GET_VBLANK_SYNCPT`) and
   a live-read `current + 1` target, so the *actual buffer latch* is gated
   by a hardware syncpoint threshold inside the kernel, not by whenever the
   SGI-paced userspace call happened to fire.
   - Source-verified this doesn't replace pacing: `TEGRA_DC_EXT_FLIP4`
     (`tegra_dc_ext_flip()`, dev.c) calls `kthread_queue_work()` and
     returns immediately -- the `pre_syncpt_id` wait lives inside
     `tegra_dc_ext_flip_worker`, a *deferred* kthread that runs after the
     ioctl already returned. Skipping the SGI wait under this mode let
     `FLIP4` calls run unthrottled (~238fps observed via a reuse-gap
     diagnostic) and risked a genuine buffer-reuse race against the kernel-
     queued flip still pending on an old target. So the SGI wait stays for
     pacing; Option 1 only adds a second, hardware-precise gate on top.
   - Verified the kernel wait genuinely executes (not a silent no-op) with
     a deliberate stress test: setting `FLIP_TEST_SYNCPT_OFFSET=300`
     targets a syncpoint value ~5 seconds in the future, unreachable within
     the kernel's hardcoded `nvhost_syncpt_wait_timeout_ext(..., 5000ms,
     ...)` timeout (whose return value dev.c never even checks, so it
     falls through and applies the flip regardless once it expires).
     `/sys/kernel/debug/tegradc.1/flip_stats` showed `Flips completed`
     frozen at a fixed count for ~5 real seconds while `Flips queued` kept
     climbing at the normal submission rate, then drained in a burst --
     an unambiguous, kernel-counter-based signature that only a real
     blocking wait in that exact code path could produce.
   - **Result: identical near-top tear position to the baseline.**
3. **Option 1b** (`FLIP_TEST_KERNEL_PACE=1`) -- removes GLX from the loop
   entirely. Pacing itself (not just the latch) is done via a blocking
   `NVHOST_IOCTL_CTRL_SYNCPT_WAITEX` on `/dev/nvhost-ctrl`, which -- unlike
   `FLIP4` -- genuinely blocks the calling thread until the real syncpoint
   reaches the target (confirmed via direct per-call timing: a clean,
   consistent block on every call, `value` always exactly matching
   `target`, no drift). `FLIP4`'s own `pre_syncpt_id` is left invalid here,
   since submission is already precisely timed by the wait.
   - **Result: identical near-top tear position again**, confirmed at the
     expected pace via gears' own on-screen FPS/ms-per-frame overlay.
4. **Option 1b @ half rate** (`FLIP_TEST_KERNEL_PACE=2`) -- same mechanism,
   but targets `current + 2` (skip a vblank between flips) for roughly
   double the margin before each flip. Confirmed pacing to exactly half
   rate via the on-screen overlay (60fps -> 30fps, matching the divisor).
   **Same tear position again**, even with 2x the slack.

**Conclusion: this rules out software wait jitter.** Four mechanisms --
one pure software sleep, one hardware latch gate layered on top of it, and
two fully GLX-free kernel-blocking waits at two different margins -- all
land on the identical tear. The timing path used to trigger the flip is
not the variable. This reinforces the original finding: `FLIP4` lacks true
buffer-swap atomicity on this driver, independent of how or when it's
called.

A standalone diagnostic used during this phase, `syncpt_probe.c`, is kept
alongside `flip_layer.c` -- it isolates the vblank syncpoint's read/wait
behavior (`TEGRA_DC_EXT_GET_VBLANK_SYNCPT`, `NVHOST_IOCTL_CTRL_SYNCPT_READ`,
`NVHOST_IOCTL_CTRL_SYNCPT_WAITEX`) outside the layer, useful if this ever
needs re-verifying independent of the Vulkan/GLX machinery. Build with
`gcc -O0 -g -Wall -I. -o syncpt_probe syncpt_probe.c`.

## Update 2026-08-16: BLOCKLINEAR reopens the investigation

The original conclusion above rested on GL never calling `FLIP4` at all when
uncomposited (confirmed via kernel `dev_info` logging) plus every one of
*our own* `FLIP4` calls tearing regardless of timing. Both of those turned
out to still be true but incomplete: **GL's tear-free fullscreen swap *does*
use `FLIP4`** -- our earlier instrumentation only ever caught it uncomposited
and *windowed*, where GL blits into the root surface via a GPU copy engine
instead (see the ioctl-tracing section below). Fullscreen is a different
code path entirely.

### Finding GL's real fullscreen FLIP4 calls

With the compositor off, `glxgears` forced fullscreen (`wmctrl -b add,fullscreen`)
showed **zero** `tegra_dc_ext` ioctls from its own process (`strace -f -y`) --
confirming the client never touches the DC directly. Windowed, `Xorg` itself
(not the client) showed a tight, ~60Hz-periodic pair of ioctls per frame on
`/dev/nvmap` and an `nvhost-*.gpu` channel fd -- consistent with a GPU-copy-
engine blit into the (already block-linear) root/desktop surface, not a
flip. **Fullscreen**, `Xorg` showed real `TEGRA_DC_EXT_FLIP4` calls, at real
vsync cadence, with a genuine (non -1) `pre_syncpt_id`. This reconciles
everything: GL doesn't "avoid" `FLIP4`, it only uses it for the one case
(fullscreen, unredirected) where the client's own buffer *can* become the
literal scanout surface -- otherwise it blits.

Extending the existing `_log_dc`-style `dev_info` patch to the `FLIP4` case
(`kernel_patches/0001-log-flip4-ioctl-args.patch`, applied to a kernel built
with `CONFIG_KPROBES`/`CONFIG_KPROBE_EVENTS`/`CONFIG_FTRACE` enabled, per the
user's request) plus a dynamic kprobe on `tegra_dc_ext_flip()` itself
(`kernel_patches/kprobe_flipwin.sh` -- no rebuild needed for the kprobe part)
captured NVIDIA's own tear-free fullscreen `FLIP4` call in full:

```
flags=0x20 (TEGRA_DC_EXT_FLIP_FLAG_BLOCKLINEAR)  block_height_log2=4
swap_interval=1   x=0 y=0   pre_syncpt_id=12 (real, incrementing)
fmt=12 (A8R8G8B8)   stride=10240 (= width*4, i.e. plain logical pitch even
                                   under BLOCKLINEAR)
```

Every `FLIP4` test in this prototype up to this point used a `LINEAR`-tiled
detile target. NVIDIA's own driver uses a block-linear (GPU-native tiled)
surface. That's the one dimension nothing here had varied yet.

### Attempt 1: export `VK_IMAGE_TILING_OPTIMAL` directly, flag it BLOCKLINEAR

`FLIP_TEST_BLOCKLINEAR=1` skips the LINEAR detile-copy step entirely and
exports the app's own `OPTIMAL`-tiled image directly (`create_app_image`
adds `VkExternalMemoryImageCreateInfo`/export on that image instead of a
separate target), setting `win.flags |= TEGRA_DC_EXT_FLIP_FLAG_BLOCKLINEAR`,
`block_height_log2 = 4`, `swap_interval = 1` to match the capture above.
Export succeeded (`GetMemoryFdKHR` ok, `FLIP4` ret=0 every call) -- but the
displayed image was visibly corrupted: a fine, dense moiré/scramble texture
across both gears, with diagonal banding. **This is expected in hindsight:
`VK_IMAGE_TILING_OPTIMAL` is implementation-opaque per spec** -- nothing
guarantees it's the same block-linear variant `TEGRA_DC_EXT_FLIP_FLAG_
BLOCKLINEAR` expects, just because both happen to be "tiled."

Swept `FLIP_TEST_BLOCKHEIGHT_LOG2` (0, 4, 5) looking for a value that
untangles it: each produced a *structurally different* corruption (0: image
shredded into thin repeating horizontal bands with black gaps; 5: image
doubled and vertically stretched), not a spectrum from "very wrong" to
"nearly right." That rules out "just the wrong block height" -- Vulkan's
`OPTIMAL` tiling on this GPU is a different scheme than the DC's documented
block-linear, not a parametrically-close cousin of it. Checked for the
principled fix (`VK_EXT_image_drm_format_modifier`, which lets a driver
advertise/negotiate exactly this kind of display-compatible tiling
explicitly): **not supported**, confirmed via `vulkaninfo`'s device
extension list on both registered ICDs on this system. There's no supported,
non-guesswork way to ask this Vulkan driver for DC-compatible tiling.

### Isolating causation from content-correctness with a solid fill

The open question ("does `BLOCKLINEAR` actually change tearing, or did we
just get lucky/unlucky on one correlated observation?") doesn't require
correct tiling to answer. **A uniform full-screen color fill reads back
identically under *any* tiling/swizzle byte-permutation**, since every byte
in the buffer holds the same value -- scrambling identical data still looks
identical. `FLIP_TEST_SOLID_FILL=1` overwrites whichever buffer actually
gets flipped with an alternating solid color (`FLIP_TEST_SOLID_FILL_PERIOD`
frames per color, default 20) instead of the app's real content, testing
tearing itself independent of whether `block_height_log2` is correct.

- **`LINEAR` + solid fill (baseline)**: clear, repeated, easily visible tear
  along the top edge at every color transition -- confirms the test
  methodology itself and matches the position established throughout this
  whole investigation.
- **`BLOCKLINEAR` + solid fill, first attempt**: the *background* filled
  correctly, but a static "ghost" of the previous real-content test's gears
  was visible blended into it. Root cause: `vkCmdClearColorImage` is bounded
  by the image's *logical* texel addressing and provably cannot reach any
  physical padding `VK_IMAGE_TILING_OPTIMAL`'s opaque layout might reserve
  beyond that -- and `VkDeviceMemory` allocations aren't guaranteed
  zero-initialized, so stale content from an earlier process's allocation
  landing on the same physical pages could sit there forever, immune to a
  logically-bounded clear.
- **Fix**: alias a plain `VkBuffer` over the *exact same memory* as the
  image (`flip_alias_buf` on `PerImage`) and use `vkCmdFillBuffer` with
  `VK_WHOLE_SIZE` instead -- a raw linear-byte-range write that reaches
  every physical byte of the allocation, no logical-addressing blind spot
  possible. This requires the image's memory to be a **non-dedicated**
  allocation (`VkMemoryDedicatedAllocateInfo` forbids binding any other
  resource to that memory) -- tested empirically that `GetMemoryFdKHR` still
  succeeds without dedication on this driver for a plain 2D color image.
- **Result after the fix**: the background is now genuinely, provably
  uniform (every byte of the allocation was written), and the transitions
  are visibly clean -- **no tearing in the solid-filled regions**, at both a
  slow (~3Hz) and faster (~30Hz, every-2-frames) toggle rate. The ghost
  artifact *diminished but didn't fully disappear*, which -- since our own
  buffer is now provably 100% uniform -- points to the DC reading some
  addresses *outside our buffer's byte range entirely* under the
  (still-likely-wrong) `block_height_log2=4` assumption, rather than merely
  misreading bytes within it. That's a real, separate problem (see below),
  but it doesn't undermine the tearing result: a stray out-of-bounds read
  landing on unrelated old memory is a *spatial* content-correctness bug, not
  the *temporal* old/new-frame discontinuity that tearing is.

**Conclusion: this is genuine causal evidence, not just correlation.** Under
an identical test methodology that cancels out the tiling-correctness
confound, `LINEAR` tears and `BLOCKLINEAR` does not. Something about
block-linear scanout -- possibly the format itself, possibly `swap_interval`,
possibly the real (non -1) `pre_syncpt_id` NVIDIA's own driver pairs with it,
untested in isolation -- gives this DC real buffer-swap atomicity that
`LINEAR` lacks. The original "no equivalent atomicity primitive on the open
`tegra_dc_ext` surface" conclusion was wrong; there is one, we just weren't
using it.

**Update: this gap is now closed — see "Update 2026-08-16 part 3" at the
bottom of this file.** The plan sketched below (option b) is exactly what
got built: a compute shader implementing the empirically-derived GOB
block-linear layout, independent of Vulkan's opaque `OPTIMAL` tiling. Left
in place as a record of the reasoning that led there.

Closing the correctness gap needed one of: (a) over-allocating generously
beyond `mreq.size` so any out-of-bounds DC read still lands on our own
buffer (cheap, done as a side effect of the real fix, but wouldn't by
itself have fixed the *content* being wrong, only made it safe to look
at), (b) reverse-implementing NVIDIA's actual GOB block-linear byte layout
in a compute shader, independent of whatever Vulkan's opaque `OPTIMAL`
tiling actually does internally -- **this is the one that worked**, or (c)
some other means of getting the Vulkan driver to hand out a buffer in a
DC-documented layout that this driver doesn't currently expose (no
`VK_EXT_image_drm_format_modifier` support to ask for one cleanly).

## Update 2026-08-16 part 2: the GOB block-linear formula, fully derived and verified

Followed the compute-shader path from the previous update's "if this ever
gets revisited" list, but **empirically derived** the formula on this exact
hardware rather than trusting a from-memory recollection of nouveau/Switch
homebrew documentation -- the earlier `FLIP_TEST_BLOCKHEIGHT_LOG2` episode
already showed that a single wrong constant produces a plausible-but-wrong
result that's hard to tell apart from "right architecture, needs tuning."

**Method:** used the LINEAR `flip_image` path (known-good, CPU-mappable) to
write coordinate-revealing test patterns, then lied to `FLIP4` that the
buffer was `BLOCKLINEAR` and read back whatever the DC produced --
`FLIP_TEST_GOB_PROBE=1` (raw diagnostic patterns) and `=2` (write a
candidate swizzle ourselves, check if the DC's read reconstructs a clean
image). Three diagnostic iterations under mode 1:

1. **8-color palette, one color per 8 rows** (`(vrow/8) % 8`): showed clean
   vertical bands, count = `2^block_height_log2` exactly (verified at 8, 16,
   32 for log2=3,4,5) -- but this palette's period-64 aliased with the
   block structure, making it blind to whether different blocks actually
   differ.
2. **6-hue family by block index** (fixes the aliasing): showed 12 clean,
   correctly-ordered horizontal bands -- proved block_idx maps onto output
   Y *correctly and unscrambled*, one block per ~`rows_per_block` output
   rows.
3. **RED=row_in_block, GREEN=raw source column, block 0 only** (grey
   elsewhere): revealed a diagonal sawtooth/zigzag repeating every
   `2560/rows_per_block` pixels -- the textbook visual signature of a
   Morton/Z-order bit-interleaved swizzle, confirming intra-GOB addressing
   is a real (not exotic) tiling swizzle, not scrambled at the whole-image
   level.

**Verification (mode 2):** wrote a candidate formula --
`gob_swizzle(byte_x, row_y)`, a 9-bit bit-interleave of GOB-local
coordinates (best-effort recollection of the standard Fermi/Maxwell/Tegra
GOB format, see the function's own comment in `flip_layer.c`) -- and a
block layout with GOBs spanning the full image width per block. First
result: **Y axis came out perfectly clean** (proves `gob_swizzle()` itself
is correct) but **X repeated every ~128-160px** instead of sweeping once.
Root cause: GOBs within a block are laid out **column-major** (all
`gobs_per_block` GOB-rows for GOB-column 0, then GOB-column 1, ...), not
row-major as first assumed (`FLIP_TEST_GOB_ORDER=1`, one-line fix). With
that corrected, the result is a **perfectly clean, fully correct gradient
across the entire image, both axes** -- no scrambling, no repeats,
verified visually.

**The complete, verified formula**, for pixel (x, y) in a `bytesPerPixel`=4
surface, block height `2^block_height_log2` GOBs:

```
gob_col          = x / 16                         (16px = 64B GOB width @ 4bpp)
gobs_wide        = image_width_px * 4 / 64
block_idx        = y / (8 << block_height_log2)
row_in_block      = y % (8 << block_height_log2)
gob_row_in_block = row_in_block / 8
row_in_gob        = row_in_block % 8
gobs_per_block    = 1 << block_height_log2
bytes_per_block   = gobs_wide * gobs_per_block * 512

gob_index_in_block = gob_col * gobs_per_block + gob_row_in_block   (COLUMN-major)
gob_base            = block_idx * bytes_per_block + gob_index_in_block * 512

# intra-GOB byte offset, for each of the 4 bytes (b=0..3) of this pixel:
byte_x = (x % 16) * 4 + b
final_offset = gob_base + gob_swizzle(byte_x, row_in_gob)
```

where `gob_swizzle(x, y)` (`x`: 0-63 byte offset within the GOB row, `y`:
0-7 row within the GOB) bit-interleaves as:
`(x5<<8)|(y2<<7)|(y1<<6)|(x4<<5)|(y0<<4)|(x3<<3)|(x2<<2)|(x1<<1)|x0`
(`xN`/`yN` = bit N of `x`/`y`). See `gob_swizzle()` in `flip_layer.c` for
the exact code.

**Still open / not yet done:** this was verified with a static, CPU-written
gradient (`FLIP_TEST_GOB_PROBE=2`), not real per-frame rendered content. To
actually use this for a working tear-free layer, it needs a compute shader
applying this exact address transform per-frame (CPU-side, byte-at-a-time
writes are far too slow for real-time use -- the probe's one-time write of
768MB already took multiple seconds), replacing the LINEAR detile copy step
with a BLOCKLINEAR-targeting one, and then the causally-verified tear-free
result needs re-confirming with *this* real, byte-accurate content instead
of the earlier uniform-fill proxy.

## Update 2026-08-16 part 3: SOLVED — real content, tear-free, confirmed

`FLIP_TEST_GOB_REAL=1` wires the verified formula (part 2, above) into an
actual per-frame path: a compute shader (`gob_swizzle.comp`, embedded as
SPIR-V in `flip_layer.c` via `gob_swizzle_spv.h`, generated with
`glslangValidator -V`) reads the app's real rendered `OPTIMAL`-tiled image
and writes it, correctly swizzled, into an exported `BLOCKLINEAR`-layout
buffer every frame -- replacing both the LINEAR detile copy and the
uniform-fill causality proxy with the real thing.

**Result: real, correctly-rendered, actively-animating gears content,
fullscreen, uncomposited, with no visible tearing.** Confirmed directly
(not inferred from a synthetic pattern). This is the original question this
entire prototype was built to answer, fully resolved: yes, `FLIP4` can
present tear-free, entirely through the open `tegra_dc_ext` ioctl surface,
with no GL/GLX bridge and no compositor required -- it just needs a real
block-linear buffer, which Vulkan's opaque `OPTIMAL` tiling doesn't hand you
for free on this driver, so you have to build one yourself.

**Architecture summary of the final working path:**
1. App renders normally into `image` (`VK_IMAGE_TILING_OPTIMAL`, plus
   `VK_IMAGE_USAGE_STORAGE_BIT` so the compute shader can read it).
2. `gob_swizzle.comp` dispatches one thread per pixel
   (`ceil(width/16) x ceil(height/8)` workgroups of 16x8), `imageLoad`s the
   source pixel, computes its correct block-linear byte address via the
   verified formula, and writes it into `gob_dst_buf` -- a plain
   `VkBuffer` (not an image; byte-address control needs to be exact, not
   texel-granularity), non-dedicated allocation (`create_gob_dest`), sized
   at ~1.25x the exact computed footprint as a safety margin, exported via
   `OPAQUE_FD` the same way every other buffer in this file is.
3. `FLIP4` is called with `buff_id` = that exported fd, `flags |=
   TEGRA_DC_EXT_FLIP_FLAG_BLOCKLINEAR`, `block_height_log2` matching what
   the shader used, `swap_interval = 1` -- same window-attribute recipe
   established in part 1/2.
4. A barrier sequence (`SHADER_READ_ONLY_OPTIMAL` -> `GENERAL` ->
   `SHADER_READ_ONLY_OPTIMAL`) brackets the dispatch so the app's next
   render pass sees the layout it expects.

## Update 2026-08-16 part 4: fd leak found during extended runs, fixed

Confirmed the "only observed for a short live run" gap below was hiding a
real bug: after running for a while (variable delay, since it depends on how
many fds happen to already be open), `dmesg` showed repeated `tegradc
tegradc.1: Failed creating fence err:-24` (`-24` = `-EMFILE`, "too many open
files"), followed by tearing returning persistently.

**Root cause:** `tegra_dc_ioctl()` (dev.c) treats `struct
tegra_dc_ext_flip_4.post_syncpt_fd` as an *output* parameter, not just
input, whenever no explicit sync-fence is requested via flip user-data
(`syncpt_idx == -1`, our case, since we never set any) -- it creates a
**brand-new post-flip sync fence fd on every single `FLIP4` call** and
writes it back into that field, regardless of what was passed in (we always
sent `-1`). Every call site in this file set `post_syncpt_fd = -1` before
the ioctl and never looked at it afterward -- one leaked fd per flip call,
confirmed empirically (`ls /proc/<pid>/fd | wc -l` climbing ~60/s, matching
the flip rate exactly). Once the process hit `RLIMIT_NOFILE`, the kernel's
*own* internal fence creation for that same flip started failing, and
whatever ordering guarantee that fence was providing went with it --
explaining why tearing came back once the errors started.

**Fix:** close `flip.post_syncpt_fd` immediately after every
`ioctl(..., TEGRA_DC_EXT_FLIP4, ...)` call, in both the per-frame worker
loop and the shutdown/disable-window call. Verified: fd count is now flat
(~124-126, no growth) over 45+ seconds of continuous fullscreen rendering,
vs. the ~2700 fds that would have leaked in that time before the fix.

**Not yet validated / worth doing before treating this as production-ready:**
- Confirmed stable over ~45s continuous runs post-fix; still no long
  (multi-minute/hour) soak test, no stress test of the backpressure/reuse
  timing under this path's heavier per-frame GPU cost.
- Performance overhead of the compute dispatch itself hasn't been measured
  (adds real GPU work every frame beyond what the LINEAR copy needed;
  probably still cheap relative to a frame budget, but unmeasured).
- The ~1.25x safety-margin buffer size is a guess, not a proven bound --
  worth deriving the exact worst-case footprint instead of a fudge factor.
- Formula verified for A8R8G8B8/4-byte-per-pixel only; other formats (or a
  different `block_height_log2` than 4) aren't re-verified, though the
  derivation method (empirical probe, not guesswork) generalizes cleanly if
  needed.
- Hardware/driver-version specific by nature -- this is reverse-engineered
  behavior of one specific Tegra X1 driver build, not documented/guaranteed
  API. Treat `gob_swizzle()` and the column-major GOB-layout finding as
  "true for this exact system, re-verify (cheaply, via the same probe
  technique) if the driver/kernel ever changes."
- Not integrated into the real `vk_layer_tegra_x11_present.c` -- this
  prototype proves the mechanism works; porting it into the production
  layer (replacing the GL/GLX bridge, or offering it as an alternative
  path) is a separate, not-yet-started effort.

## Update 2026-08-16 part 5: DC device and window index made configurable

`/dev/tegra_dc_1` and window index `1` were hardcoded for this entire
prototype's history (this device confirmed free of Xorg ownership
specifically on DC1 -- see the safety notes near the top). This meant the
file could only ever target DC1 (the external/dock output, `DP-0` in
`xrandr`), regardless of intent -- surfaced when testing against the
internal panel (`DSI-0`, DC0) produced no visible result and `dmesg` showed
DC1 activity instead. Two separate things were going on: the hardcoding
(now fixed, see below), and the internal display being disabled at the X11
level at the time (`DC0 ... enabled=0` per the earlier `GET_STATUS` kprobe
capture, confirmed via `xrandr` showing `DSI-0 connected` with no active
mode) -- independent of anything in this file, a flip to a disabled DC
isn't expected to produce visible output no matter what device path is
used.

`FLIP_TEST_WIN=<N>` (default 1, same as the old hardcoded
`FLIP_TEST_WIN_INDEX`) selects the window index at runtime, stored on
`sc->flip_win_index` for the worker thread to use consistently. **Window
ownership has only ever been verified on DC1** -- window 1 being free
there doesn't guarantee it's free on DC0 or any other DC; re-check
(`GET_WINDOW`, or the debugfs `window_toggle` trick from the original
investigation) before pointing this at a different DC.

**Superseded later the same day** -- see part 6 below: the DC device
itself is now auto-detected at runtime instead of needing
`FLIP_TEST_DC` set by hand.

## Update 2026-08-16 part 6: DC auto-detection via XRandR

Runtime auto-detection: `detect_dc_for_window()` uses XRandR
(`XRRGetScreenResourcesCurrent`/`XRRGetCrtcInfo`/`XRRGetOutputInfo`) to find
which physical output the app's actual window is currently on (by root-
relative rectangle overlap against each active CRTC -- the standard
portable "which monitor is this window on" technique), then maps the
output's name through a small hardware-specific table (`DSI-0` -> DC0,
`DP-0` -> DC1 -- fixed by this SoC's physical display wiring, not something
that changes at runtime; no Tegra-specific X11 property or public
`tegra_dc_ext` ioctl exposes this mapping directly, checked `xrandr --props`
for one and found only generic/KDE properties). `FLIP_TEST_DC=<N>`, if set,
still overrides detection entirely (forces a specific DC regardless of
which output the window is on -- useful for testing). Falls back to DC1 if
detection fails for any reason (no XRandR, unrecognized output name, etc.).

Implementation note: `XTranslateCoordinates` and the `XRR*` functions are
resolved through the *same* dlopen+dlsym indirection as every other X11/GL/
GLX call in this file (see the big block comment near the top on the
Vulkan-loader-mutex deadlock this avoids) -- `libXrandr.so` is opened
alongside libX11/libGL/libGLX in `lib_load()`, but treated as optional
(missing it just disables auto-detection, doesn't fail setup) unlike the
other two, which are hard requirements.

Verified: with the dock connected (`DP-0` active), auto-detection correctly
logs `window is on output 'DP-0' -> recognized` and targets
`/dev/tegra_dc_1` window 1, identical to every prior test in this file, and
live end-to-end behavior (real content, tear-free) is unaffected by this
change.

## Update 2026-08-16 part 7: non-16-pixel-aligned widths (vkcube)

`vkcube` (default 500x500, unlike gears' fullscreen 2560x1600) rendered
visibly wrong under `FLIP_TEST_GOB_REAL` -- fine combing/interlacing across
the whole image. Confirmed via user testing: **width not evenly divisible
by 16px (one GOB, 64 bytes @ 4bpp) is the trigger; height doesn't matter**
(2560 is exactly 160 GOBs wide; 500 is 31.25).

Root cause, found via `FLIP_TEST_GOB_PROBE=2` at `--width 500`: `gobs_wide`
(`width*4/64`, used for `bytes_per_block = gobs_wide * gobs_per_block *
512`, i.e. where each vertical block starts) was computed with truncating
division. For width=500, `floor(2000/64)=31`, but the real last GOB-column
is index 31 (`gob_col = x/16` reaches 31 for x=496..499) -- so writes for
that last, partial column (`gob_index_in_block = gob_col * gobs_per_block +
gob_row_in_block`, up to `31*16+15=511`) land *past* `bytes_per_block`
(`31*16*512`), spilling into the next block's territory and corrupting its
first column.

First attempt (just rounding `gobs_wide` up to 32) made it *worse* --
because only our own write-side addressing changed; `win.stride` (what
tells the DC where it thinks each block starts) was left at the
unrounded `width*4`, so our writes and the DC's reads now disagreed about
block boundaries for *every* block, not just the last column. The correct
fix needs both sides consistent: round `gobs_wide` up **and** set
`win.stride` (via `pi->flip_row_pitch`) to `gobs_wide * 64` (the padded
value, 2048 for width=500) instead of the raw `width*4` (2000) -- applied
in both `create_gob_dest` (the real content path) and the
`FLIP_TEST_GOB_PROBE=2` verification path, which must stay consistent with
each other by construction.

This fixed the block-to-block misalignment (confirmed via probe: the
sawtooth pattern repeating across the whole image is gone). A smaller,
localized artifact at the last partial column specifically was still
visible in the synthetic gradient probe pattern -- but **confirmed fixed
for real rendered content** (`vkcube` at various widths, `FLIP_TEST_GOB_REAL=1`)
by direct visual check, which is what matters in practice. Not fully root-
caused at the byte level, but shipped since it resolves the actual
problem; worth revisiting with the probe technique if it resurfaces on
real content at some other non-aligned width.

Since that closed blob still has to talk to the *open* kernel driver to get
pixels on screen, its ioctl traffic is the one part of it that's still
observable: `kernel_patches/0001-log-flip4-ioctl-args.patch` (against
`drivers/video/tegra/dc/ext/dev.c`) extends the existing `_log_dc` GET-class
ioctl logging to the `TEGRA_DC_EXT_FLIP4` path -- one `dev_info()` per call
with caller `comm`/`pid` and win_num/flags, plus one line per window with
every field that could plausibly matter for tearing (`buff_id`, `blend`,
`offset`/`stride`, geometry, `z`, `global_alpha`, `pre_syncpt_id`/
`pre_syncpt_val`). Insertion point is right after `dev_cpy_from_usr()` in the
`TEGRA_DC_EXT_FLIP4` case, so it sees the fully-parsed kernel-side struct,
not a raw user pointer needing another copy. Apply with `patch -p1 <
kernel_patches/0001-log-flip4-ioctl-args.patch` from the kernel source root
(or `git apply`), rebuild, reflash, then `dmesg -w | grep tegra-dc-ext`
during a normal *tear-free* `glxgears`/native GL present (compositor off) to
see whether NVIDIA's own blob calls `FLIP4` at all in that case (the earlier
GL-vs-Vulkan investigation found it does *not*, when uncomposited -- this
patch is the tool to re-confirm that and, if it ever does show up under some
other condition, see exactly what it passes).

**Re-confirmed 2026-08-16, kernel 4.9.140-l4t #150, patch applied, compositor
off:** ran plain `glxgears` (no Vulkan, no layer) for 5s while watching
`dmesg -w | grep tegra-dc-ext`. It self-reported `302 frames in 5.0 seconds =
60.222 FPS` -- genuinely vsync-locked, real frames -- and the `dmesg` output
was **byte-identical before and after**: zero `FLIP4` calls logged for the
entire run, from any process. This upgrades the earlier finding from
"independently confirmed via kernel instrumentation" (an older, less direct
check) to a direct, reproducible capture: NVIDIA's tear-free GL swap path
genuinely never touches `TEGRA_DC_EXT_FLIP4`, full stop.

Incidental finding from the same capture: at idle (no GL client running),
`Xorg` itself (not any client) periodically calls `FLIP4` on window index 0
(its own root-window surface) -- same `buff_id` every time, irregular
~130-650ms spacing (not vblank-periodic, so likely idle/cursor-blink
housekeeping, not a real content update), with `pre_syncpt_id=16` (a real,
valid syncpoint -- unlike this layer's own `-1`) and `pre_syncpt_val`
strictly incrementing. This activity stopped the instant `glxgears` started
rendering and never resumed while it ran. Two takeaways: (1) `pre_syncpt_id`
*is* legitimately used by NVIDIA's own driver elsewhere, so it's a real
mechanism, just not the one behind GL's tear-free swap -- consistent with
Option 1 (same mechanism, tested above) not fixing tearing; (2) window 0's
idle refresh and a GL client's swap are evidently two entirely separate
code paths in the closed driver, reinforcing that whatever gives GL its
atomicity is specific to the swap path, not a general property of
`tegra_dc_ext` flips.

**Kernel config**, if rebuilding anyway: `CONFIG_KPROBES=y` +
`CONFIG_KPROBE_EVENTS=y` + `CONFIG_FTRACE=y` + `CONFIG_FUNCTION_TRACER=y` +
`CONFIG_DYNAMIC_FTRACE=y`. Not required for this specific patch (plain
`dev_info` doesn't need them), but worth turning on now since they were
found *off* on the running kernel earlier in this investigation
(`register_kprobe` failed with `-ENOSYS`), which blocked a dynamic kprobe
approach and forced the slower source-patch-and-rebuild cycle every time
something new needed instrumenting. With them on, a `kprobe_events` in
`/sys/kernel/debug/tracing` could hook things like `tegra_dc_ext_flip()` or
`tegra_dc_ext_flip_worker()` on demand, no rebuild required -- useful well
beyond this specific investigation.

## Update 2026-08-17: `vkgears -fullscreen` tearing -- 2-image swapchains are unsafe

`vkgears -fullscreen` (the system `/usr/bin/vkgears`, distinct from the
`gears`/`vkcube` demos used throughout this doc) kept tearing under
`FLIP_TEST_GOB_REAL` even with the compositor off, while `gears`/`vkcube`
were tear-free. Continuous/recurring, not a one-off.

Two theories were investigated and **ruled out**:

- **Missing `BLOCKLINEAR` flag on vkgears' flips**: an early kprobe capture
  was misread -- `args.flags` (the top-level `struct tegra_dc_ext_flip_4`
  byte, always 0 for our calls) was mistaken for `win.flags` (the per-window
  field that actually carries `TEGRA_DC_EXT_FLIP_FLAG_BLOCKLINEAR`, `1<<5`).
  Re-running the kprobe and reading the correct field showed both `gears`
  and `vkgears` correctly get `flags=32 blockh=4` -- BLOCKLINEAR was never
  the problem.
- **Orphaned worker thread from `vkgears`' windowed→fullscreen swapchain
  recreation**: `vkgears` creates a small windowed swapchain first, then
  recreates it fullscreen; no `DestroySwapchainKHR` was seen logged between
  the two `CreateSwapchainKHR` calls, suggesting the old swapchain's worker
  thread/window claim might linger and race the new one.
  `VkSwapchainCreateInfoKHR::oldSwapchain` handling was added to
  `layer_CreateSwapchainKHR` (destroys the old swapchain via
  `layer_DestroySwapchainKHR` before creating the new one) as a fix -- but
  added diagnostic logging then showed `ci->oldSwapchain` was actually
  `VK_NULL_HANDLE` the whole time: `vkgears` calls `vkDestroySwapchainKHR`
  explicitly instead of using `oldSwapchain`, and that call was already
  reaching our layer correctly. The theory was wrong, but the
  `oldSwapchain` handling is a real correctness fix for apps that *do* rely
  on it, so it was kept.

**Actual root cause**: `vkgears` requests `minImageCount=2`;
`gears`/`vkcube` request 3. With 2 images, each buffer is reused only ~2
vblanks (~33ms) after its previous flip; with 3, ~3 vblanks (~50ms).
`TEGRA_DC_EXT_FLIP4` is deferred/kthread-queued in the kernel (confirmed
from kernel source earlier in this investigation) -- `tegra_dc_ext_flip()`
queues work and returns immediately, with the actual window-attribute
apply happening asynchronously in a per-window kthread. Our CPU-side
backpressure only tracks our own GPU compute-shader work finishing, not
"the DC's deferred kthread has actually applied the flip and the hardware
has moved on." At the ~33ms margin the 2-image case provides, that's
evidently not always enough slack for the deferred kthread to settle
before we start rewriting that buffer's memory again -- producing a real,
reproducible tear. The ~50ms margin from 3 images empirically eliminates
it.

Confirmed via a `FLIP_TEST_MIN_IMAGES` env override (temporary, forced
`want` up regardless of `ci->minImageCount`): `FLIP_TEST_MIN_IMAGES=3
vkgears -fullscreen` ran tear-free. Since a 2-image swapchain is
apparently unsafe with this layer's architecture regardless of which app
asks for it, this was made the permanent default rather than an opt-in
flag: `MIN_IMAGES` (`flip_layer.c`) is now `3`, not `2`. Vulkan apps are
required to handle the driver returning more images than requested (they
must query the real count via `vkGetSwapchainImagesKHR`), so this is a
safe, spec-compliant floor. `FLIP_TEST_MIN_IMAGES` remains available as an
opt-in override for forcing *even more* images than 3, for further margin
testing.

Not yet root-caused at the kernel-timing level *why* ~33ms specifically
isn't enough (vs. some other deferred-kthread latency bound that could be
fixed at the source instead of worked around with more buffering) -- the
3-image floor is a confirmed, shipped fix, not a full explanation.

## Update 2026-08-17 part 2: dolphin-emu windowed→fullscreen transition, fixed

`dolphin-emu` reliably crashed (SIGSEGV) when transitioning from its
windowed startup UI to fullscreen (what it does on starting a game). It
was first reproduced under `gdb`, with an identical, unsymbolized crash
(inside dolphin's JIT-generated GameCube CPU recompiler code, "CPU
thread") occurring both with and without this layer loaded -- pointing at
a gdb/JIT interaction artifact, not a layer bug, and dolphin does run fine
outside gdb without the layer. That left one untested combination: no
gdb, **with** the layer.

Running that combination reproduced a real, deterministic failure --
no gdb involved, no segfault either, just a clean process exit right
after:

```
[...] DestroySwapchainKHR: swapchain=0x...        <- old 640x480 windowed swapchain torn down
[...] FLIP_TEST: targeting /dev/tegra_dc_1 window 1 <- new fullscreen swapchain starting
[VK_LAYER_FLIP_test ERR] FLIP_TEST: GET_WINDOW 1 on /dev/tegra_dc_1 failed: Device or resource busy
```

`worker_shutdown()` (called from `layer_DestroySwapchainKHR`) is fully
synchronous -- it sets a quit flag and `pthread_join()`s the worker
thread, which itself `close()`s `sc->flip_dc_fd` as the last thing it does
before returning. So by the time the old swapchain's `DestroySwapchainKHR`
returns, our own process should have zero references to that fd left.
Yet the very next `GET_WINDOW` (a different fd, freshly opened for the
new fullscreen swapchain) found the window still marked busy by the
kernel driver.

Root cause, found by polling `lsof /dev/tegra_dc_1` once per 100ms across
the transition: for a couple of polls right after our own close, the fd
was held open not by dolphin-emu, but by **`xdg-screensaver` and
`xprop`** -- unrelated helper processes that Qt/KDE apps commonly spawn to
inhibit the screensaver when entering fullscreen -- both holding the exact
same inode (confirmed via matching device/inode numbers in the `lsof`
output) that we had just closed our own copy of. `open(flip_dc_path,
O_RDWR)` (and the sibling `open("/dev/nvhost-ctrl", O_RDWR)`) were both
missing `O_CLOEXEC`, so when dolphin forked+exec'd that screensaver-inhibit
helper, the child inherited our raw fd across the `exec()`. The kernel
driver's window-release logic (tied to the file's last close, i.e. the
last reference across *all* processes, not just ours) kept the window
marked busy for as long as that unrelated, short-lived child process still
held its inherited copy open -- racing the immediately-following new
swapchain's `GET_WINDOW` and losing.

Fix: add `O_CLOEXEC` to both `open()` calls in `layer_CreateSwapchainKHR`
(`flip_dc_fd` and `flip_nvhost_ctrl_fd`) so fork+exec'd helper processes
never inherit them in the first place. Confirmed fixed: rebuilt, ran
dolphin-emu with the layer (no gdb) through the windowed→fullscreen
transition into actual gameplay multiple times with no `GET_WINDOW`
failures and no crash -- user-confirmed working, including closing the
app cleanly afterward.

This also means the original gdb-crash theory, while plausibly still real
for whatever dolphin+gdb-specific issue it was, was never actually tested
against the right combination -- the crash the user originally hit when
starting a game normally (no gdb, layer loaded) was this fd-inheritance
race the whole time, not the gdb/JIT artifact.

## Update 2026-08-17 part 3: Play emulator — no rendered frames, GPU hang

`~/Downloads/Play-Emulator-ARM64.AppImage --cdrom0` (a PS2 emulator, using
`VK_KHR_xcb_surface` rather than Xlib -- the first app tested that does)
showed a black window under the layer while reporting 60fps internally,
where it renders normally without the layer. Not a pacing issue: FLIP4
calls happened, just very sparsely at first glance, and the real content
was never making it to screen.

### Four real, independent bugs found along the way

All four were found via the Khronos validation layer (an older
1.3.204 build sideloaded from a mounted image, since no validation layer
was installed system-side; `library_path` in its manifest needed patching
to an absolute path, plus `libVkLayer_utils.so` alongside it) and via
`dmesg`, which is what actually revealed the failure mode: `vkQueueSubmit`
returning `VK_ERROR_DEVICE_LOST` (-4) cascading through everything
afterward was just the symptom. The real signal was
`nvgpu: gk20a_channel_timeout_handler: Job on channel N timed out` --
a genuine GPU hang caught by the driver's own watchdog (~5s), not a page
fault or crash. That distinction mattered: it pointed at a stuck
GPU wait (a semaphore never signaled) rather than memory corruption.

1. **`layer_QueueWaitIdle` wasn't taking `dev->submit_lock`.** Every other
   queue touchpoint in this layer is deliberately serialized through that
   per-device mutex (see its declaration comment, added earlier after a
   similar PPSSPP bug) because `vkQueueWaitIdle`'s queue parameter is
   externally synchronized too, same as `vkQueueSubmit`. Missed originally.
   Fixed by wrapping the driver call in `submit_lock`.

2. **`gob_dst_buf` and `flip_alias_buf` were missing
   `VkExternalMemoryBufferCreateInfo`.** Both get memory allocated with
   `VkExportMemoryAllocateInfo.handleTypes = OPAQUE_FD_BIT` bound to them,
   but neither buffer declared a matching external-memory type at creation
   -- `VUID-vkBindBufferMemory-memory-02726`, undefined behavior per spec.
   Fixed by adding the missing struct to each buffer's `pNext`.

3. **The app's rendered image never guaranteed `VK_IMAGE_USAGE_SAMPLED_BIT`.**
   This is the most broadly significant of the four: `layer_CreateRenderPass`
   /`CreateRenderPass2` rewrite every attachment's `PRESENT_SRC_KHR`
   `finalLayout` to `SHADER_READ_ONLY_OPTIMAL` (needed so the worker can
   read the app's content afterward) for *every* app using this layer, not
   just Play -- but `create_app_image`'s `ici.usage` only added
   `STORAGE_BIT` conditionally (GOB_REAL mode) and otherwise relied on
   whatever usage the app itself requested for its swapchain, which most
   apps don't include `SAMPLED_BIT` in (they don't normally sample their
   own swapchain images). Per `VUID-vkCmdBeginRenderPass-initialLayout-00897`,
   an attachment finalizing to `SHADER_READ_ONLY_OPTIMAL` requires the image
   to have been created with `SAMPLED_BIT` or `INPUT_ATTACHMENT_BIT` --
   without it, the Tegra driver's texture unit is plausibly missing tiling/
   compression metadata it needs, which lines up with a hang rather than an
   immediate error. Fixed by adding `SAMPLED_BIT` unconditionally.
   Every other app tested apparently tolerated this by luck (fewer frames,
   simpler content, or driver leniency) -- worth remembering if something
   *else* eventually looks like an intermittent hang too.

None of these four fixed Play's actual hang. That took isolating the
failure to `present_mode == VK_PRESENT_MODE_MAILBOX_KHR` specifically
(Play requests MAILBOX; every other app tested so far requests FIFO or
MAILBOX-with-low-throughput) via a `FLIP_TEST_FORCE_FIFO` diagnostic
override -- forcing FIFO made the hang disappear outright, pointing
straight at `worker_post`'s MAILBOX "displaced image" bookkeeping
(`QueuePresentKHR` / `worker_post` in `flip_layer.c`).

### The real bug: gaps in the mailbox displacement bookkeeping

MAILBOX mode lets `vkQueuePresentKHR` return immediately without blocking;
if a new present arrives before the worker thread has picked up the
previous one, the previous image is "displaced" and its
`vk_render_done`/`gl_sample_done` binary semaphores need exactly one
matching wait+signal cleanup so a later Acquire on it doesn't deadlock and
a later Present's bridge submit doesn't try to signal an already-signaled
binary semaphore. A binary semaphore's wait is claimed *in submission
order* the instant a matching wait is issued to the queue, regardless of
when it finishes executing -- if two submits both wait on the same
once-signaled semaphore, whichever was issued second can never be
satisfied. On this driver that manifests as a genuine GPU hang caught by
the kernel's own watchdog (`nvgpu: gk20a_channel_timeout_handler: Job on
channel N timed out`, ~5s), not a clean Vulkan error -- `DEVICE_LOST`
cascading through everything afterward was just the downstream symptom.

Getting "exactly one wait, exactly one signal" right against a fast
MAILBOX app took five fixes, found in order as each one moved the failure
point later instead of eliminating it (frame ~8 -> ~12 -> ~9 -> ~300 ->
finally indefinite) -- the last two were only found by building a
purpose-built trace tool (`FLIP_TEST_TRACE_SYNC=1`, a global sequence-
numbered log of every touch of these semaphores with the calling thread's
TID) after pure code reading stopped finding anything:

1. **`AcquireNextImageKHR`'s round-robin only checked `acquired`, not
   whether the candidate image was still `worker_pending_idx`.**
   `QueuePresentKHR` clears `acquired=false` immediately (needed for
   MAILBOX's non-blocking contract), so a fast app could re-acquire and
   re-present the *same* image while its earlier post was still
   unconsumed -- `worker_post` would then displace that idx against
   itself, double-signaling its own `vk_render_done`. Fixed by also
   checking `worker_pending && worker_pending_idx == idx` in the
   round-robin's busy test.

2. **`sc->lock` was released before `worker_post()` and the displaced-image
   drop-cleanup submit.** This left a window where a concurrent
   `AcquireNextImageKHR` could see a just-displaced image as neither
   `acquired` nor `worker_pending_idx` -- free -- and re-acquire it before
   the drop-cleanup submit had actually signaled its `gl_sample_done` yet.
   Fixed by holding `sc->lock` through the entire displacement handling
   (worker_post + drop-cleanup submit), not releasing it right after
   `acquired=false`.

3. **`worker_pending` stays `true` for a MAILBOX slot's *entire* frame** --
   through the compute-shader dispatch, the `FLIP4` ioctl, and the vsync
   wait -- and is only cleared at the very end, but the worker's own
   consumption of that slot's semaphores happens much earlier, right after
   the compute dispatch. The trace caught `worker_post(idx=2)` landing
   *after* the worker had already fully drained idx=1's semaphores itself
   but *before* `worker_pending` (still tracking the rest of idx=1's frame)
   went false, displacing idx=1 anyway and double-signaling a
   `gl_sample_done` the worker had already signaled. First fix: a second
   flag, `worker_pending_sem_live`, cleared right after the worker's
   semaphore-consuming submit *completed* (fence-confirmed) -- this only
   made the hang far rarer (~9 frames of margin to ~300), not gone.

4. **Root cause of #3's remaining gap**: clearing `worker_pending_sem_live`
   after the submit *completes* is still too late -- the semaphore wait is
   claimed when the submit is *issued*, not when it finishes. A second
   trace catch: `worker_post(idx=0)` displacing idx=2 and issuing its
   drop-cleanup wait on `vk_render_done` *before* the worker's own submit
   for idx=2 had even been issued yet (both dequeue and command-buffer
   recording take real, non-trivial CPU time) -- so the drop-cleanup's wait
   claimed the signal first, and the worker's own subsequent wait for the
   same semaphore could never be satisfied. Fixed by clearing
   `worker_pending_sem_live` at submit-*issue* time instead of completion
   time.

5. **The actual root cause**: none of the above matters if the *dequeue*
   itself doesn't claim the slot. Dequeuing a pending index already commits
   the worker to consuming its semaphores unconditionally (once it reaches
   the submit, which fix #4 handles) -- but between dequeue and that
   submit there's real CPU work (window-geometry check, command buffer
   recording), during which `worker_pending_sem_live` was still sitting at
   its stale `true` value. A new `worker_post()` landing in that window
   would displace the image the worker had *already dequeued and committed
   to*, again racing the worker's own soon-to-be-issued wait. Fixed by
   clearing `worker_pending_sem_live` at dequeue time, in the same locked
   section as the dequeue itself (no intervening unlock/relock, which would
   reopen the identical window) -- this made fix #4's clearing redundant,
   so it was removed. This is the one that actually closed the race for
   good.

Confirmed fixed: Play now runs a full play session (300+ consecutive
`FLIP4` calls, user played and closed it deliberately) under its native
MAILBOX request with zero `DEVICE_LOST` errors, in the same run
configuration (`FLIP_TEST_GOB_REAL=1`, no overrides) that previously hung
within the first ~300 frames at best. `FLIP_TEST_FORCE_FIFO` and
`FLIP_TEST_TRACE_SYNC` are kept as opt-in diagnostic env vars (both
default off) since they were directly responsible for isolating and then
pinpointing this bug and will likely be useful again for anything
MAILBOX-shaped in the future.

## Update 2026-08-17 part 4: windowed apps only make visual sense fullscreen -- gated it

Testing so far always used fullscreen apps (gears, vkgears, dolphin,
the Play emulator once it reaches gameplay). Testing a genuinely windowed
app exposed a real correctness gap: `TEGRA_DC_EXT_FLIP4` presents via a
raw hardware overlay plane (window index, not to be confused with an X11
window -- a DC hardware concept, see the FLIP_TEST banner comment near the
top of `flip_layer.c`), which the display hardware composites on top of
whatever Xorg's own desktop plane is showing, *unconditionally*, with no
awareness of X11 window stacking. For a windowed app this breaks two
things at once:

- **Position**: `win.out_x`/`win.out_y` are relative to the *target
  display's own origin*, not the X11 root window's -- they were hardcoded
  to `0,0`, so a windowed app's content always appeared pinned to the
  physical display's top-left corner instead of wherever its actual window
  was on screen.
- **Occlusion**: the overlay plane always draws on top, full stop -- drag
  another window over a windowed app running under this layer and the
  overlay content would still show through on top of it, since the DC
  hardware has no concept of X11 window stacking to respect.

The position bug is trivially fixable (store the window's on-screen
position, feed it into `out_x`/`out_y`). Occlusion is not, not without
routing the final content through Xorg's own compositing pipeline the way
every other window's content does -- and that's the exact mechanism this
whole layer exists to route *around*, since native Vulkan WSI already does
that and tears (see the top of this file). There's no way to get FLIP4's
tear-free hardware-plane presentation *and* have Xorg's compositor
correctly clip it around other windows; those are two different
presentation mechanisms.

**Decision: gate FLIP4 on the window being fullscreen.** Only engage the
FLIP4 present path when BOTH: (1) the window's size and position exactly
match its target display's CRTC rect, and (2) the window manager has the
EWMH `_NET_WM_STATE_FULLSCREEN` atom set on the window (`is_wm_fullscreen`,
alongside the XRandR/CRTC-matching machinery in `detect_dc_for_window`).
Size match alone isn't sufficient, and an area-overlap threshold (e.g.
98%) is worse still: a maximized-but-still-a-regular window can cover
nearly the whole display too, and on some WM/panel configurations can
match a display's resolution exactly (decoration-less/CSD windows, no
panel reserving space, etc.) -- maximized must NOT be treated as
fullscreen here, since unlike true exclusive fullscreen it's still a
normal occludable window, which a raw hardware overlay plane can't
respect. `_NET_WM_STATE_FULLSCREEN` (as opposed to
`_NET_WM_STATE_MAXIMIZED_VERT`/`_HORZ`, which is what maximizing actually
sets) is the semantically correct, WM-independent signal that actually
distinguishes the two -- confirmed 2026-08-17 by maximizing `vkcube` via
`wmctrl` mid-run: its window resized to 2560x1572 (not an exact CRTC
match on this desktop's panel layout, but `_NET_WM_STATE` correctly showed
`MAXIMIZED_VERT`/`MAXIMIZED_HORZ` with no `FULLSCREEN` atom either way),
and the layer correctly fell through to native WSI rather than treating
it as fullscreen. Windowed swapchains fall through to native passthrough
WSI -- tearing, exactly as they would without this layer at all, which is
a strictly better outcome than tearing *and* being mispositioned *and*
never being occluded correctly. This isn't just a workaround for
occlusion: gating on fullscreen also makes the `out_x=out_y=0` hardcoding
*correct by construction* (a window that covers its display necessarily
starts at that display's own origin), so no separate position fix was
even needed once the gate was in place.

This falls out naturally from how every app tested so far already
behaves: dolphin-emu, vkgears, and the Play emulator all create a small
windowed swapchain first and *recreate* it fullscreen on their own
windowed -> fullscreen transition. The first (windowed) `CreateSwapchainKHR`
now falls through to native WSI automatically; the second (fullscreen) one
engages FLIP4 exactly as before, with zero extra plumbing needed to detect
the transition. `FLIP_TEST_ALLOW_WINDOWED=1` overrides the gate for
testing the FLIP4 path itself against a non-fullscreen window (e.g.
vkcube's default 500x500, used throughout the width-alignment
investigation above) -- position will be wrong in that case, which is
expected and fine for that specific diagnostic purpose.

**A real, independent bug found and fixed while implementing this**: every
"soft fail, fall through to native WSI" path in `layer_CreateSwapchainKHR`
except the `dev->passthrough` branch forwarded `ci` to the ICD unmodified
-- but `ci->surface` may be *this layer's own* fake `Surface*` wrapper
handle (what `layer_CreateXlibSurfaceKHR`/`layer_CreateXcbSurfaceKHR` hand
back to the app), not a real native surface object. The ICD dereferencing
that as if it were real crashes (SIGSEGV) -- confirmed the moment the new
fullscreen gate turned "fall through" from a rare error path into the
common case for every windowed app. Fixed with a shared
`fallback_to_native_swapchain()` helper (mirroring what the
`dev->passthrough` branch already did correctly) that swaps in
`surf->icd_surface` before forwarding, used at all nine fallback sites in
the function. Confirmed fixed: a windowed `vkcube` now runs and renders
normally in its own window (native WSI, tearing, same as without the
layer) instead of crashing; a fullscreen `vkgears -fullscreen` still
engages FLIP4 and remains tear-free, both user-confirmed.

## Update 2026-08-17 part 5: resizing a windowed app crashed -- oldSwapchain double-destroy

Resizing `~/Vulkan/build/bin/gears -vs` (a windowed app, correctly using
the native-WSI fallback per the FULLSCREEN GATE above) crashed on every
resize. `layer_CreateSwapchainKHR` has always unconditionally destroyed
`ci->oldSwapchain` itself at the top of the function, added earlier to fix
a real bug (an app going windowed -> fullscreen leaving its old FLIP4
worker thread and hardware window claim orphaned, see the "recreation...
was never handled" comment above it). That fix's reasoning -- "safe to
call unconditionally, `layer_DestroySwapchainKHR` already falls through to
the native destroy for handles that aren't ours" -- covered the *destroy
call itself* safely, but missed a second consequence: `ci->oldSwapchain`
is still sitting in `ci`, which (for a windowed app) then gets forwarded
*unmodified* to a real `CreateSwapchainKHR` call in the native-WSI
fallback path a few lines later. The real ICD also consumes/retires
`oldSwapchain` as part of that call -- so the handle got destroyed twice:
once by us eagerly, once by the driver internally. Per the Vulkan spec,
`oldSwapchain` is only *retired* by a `CreateSwapchainKHR` call, not
destroyed -- the application still owns it and is expected to call
`vkDestroySwapchainKHR` on it separately itself, which is exactly what the
logs showed `gears -vs` doing on every resize, right into a handle we'd
already freed.

This bug predates the FULLSCREEN GATE (part 4 above) but was never
triggered by it: falling through to native WSI used to be a rare error
path, never exercised by a live app doing real swapchain recreation with
`oldSwapchain` set. The gate made native-WSI fallback the *normal* path
for every windowed app, so any windowed app that resizes (recreating its
swapchain, most do) now hit this on every single resize.

Fixed by only performing the eager destroy when `oldSwapchain` is
confirmed to be one of this layer's own fake `Swapchain*` structs
(`as_swapchain(ci->oldSwapchain) != NULL`) -- exclusively our call to make
in that case, since the real ICD never knew that handle existed. For a
real native handle, `ci->oldSwapchain` is now left completely untouched
and flows through unmodified to whatever `CreateSwapchainKHR` call ends up
handling the request (fallback or passthrough), exactly matching normal
Vulkan swapchain-recreation semantics. Confirmed fixed: `gears -vs`
survives repeated manual resizing with no crash (user-confirmed); the
original motivating scenario (an app transitioning windowed -> fullscreen
while its old swapchain is one of ours) still works correctly too --
re-tested with `vkgears -fullscreen`'s own internal 300x300 -> 2560x1600
swapchain recreation, still engages FLIP4 tear-free on both swapchains
with no orphaned worker thread.

## Update 2026-08-18: re-testing the SGI wait ordering (FLIP_TEST_WAIT_AFTER_FLIP)

Earlier in this investigation (see the "Wait for vblank HERE, right before
FLIP4, instead of after" comment in `worker_thread_main`), moving the SGI
vblank wait to *before* `FLIP4` instead of after was found to fix a
consistent mid-frame tear -- calling `FLIP4` at an arbitrary phase
(whenever the GPU copy/compute step happened to finish) rather than right
after a known vblank edge gave the driver less lead time to latch the new
buffer before the *next* vblank.

Re-tested 2026-08-18 with a new toggle, `FLIP_TEST_WAIT_AFTER_FLIP` (moves
the wait back to after `FLIP4`, the original ordering), against both
`vkgears -fullscreen` and `~/Vulkan/build/bin/gears -f -vs`: **both stayed
tear-free** in this session's testing, contradicting the earlier result.
The most likely explanation is the NVIDIA driver was changed at some point
during this investigation (mentioned by the user mid-session) -- the
original mid-frame-tear finding may be specific to whichever driver
version was in use when it was established, not a universal property of
this hardware/ioctl combination.

**Made the default.** After-FLIP4 is a real, mechanistic latency
improvement, not just a coin flip: it removes up to ~1 vblank interval
(~16ms) of "content ready but waiting to even call FLIP4" delay, at the
cost of a smaller, timing-dependent margin for the deferred kernel worker
to finish before the next vblank (versus the old ordering's guaranteed-
maximal margin) -- which is mechanistically *why* it tore before. Flipped
the default 2026-08-18 per direct request, with `FLIP_TEST_WAIT_AFTER_FLIP=0`
kept available to revert to the more conservative before-FLIP4 ordering
if tearing reappears under a different driver, heavier scene, or system
load than this session's relatively short (tens of seconds per app)
re-test happened to cover -- other bugs in this investigation (the
MAILBOX displacement race, in particular) only surfaced after much longer
runs or specific timing conditions a short test can miss, so this default
is a deliberate, informed bet rather than an exhaustively-soaked
guarantee.

## Update 2026-08-18 part 2: IMMEDIATE mode -- FLIP4 has no benefit, real cost

`~/Vulkan/build/bin/gears -f` (fullscreen, *without* `-vs` -- no vsync)
showed two symptoms under this layer: ran at ~417fps versus ~700fps
natively, and took ~10 seconds to actually exit after closing the window
(not a true hang, just a long delay).

Root cause of both, in one place: the app requests
`VK_PRESENT_MODE_IMMEDIATE_KHR` (confirmed via log:
`present_mode=0`) -- meaning it explicitly does not want vsync and
explicitly accepts tearing. There is no tearing problem for this layer to
fix for that app. But engaging FLIP4 anyway still costs real, unavoidable
per-frame overhead on top of the app's own rendering: the GOB compute-
shader conversion, a *synchronous* `tegra_dc_ext_pin_windows()` inside
every `FLIP4` ioctl call (confirmed ~12ms on the first few calls of a
fresh run in the log capture, dropping to sub-millisecond once buffers are
already pinned), and extra semaphore/fence round-trips for the
Acquire/Present bridge. For an app that already accepts tearing and wants
maximum throughput, that's pure cost with zero benefit -- exactly the
~40% fps regression observed. The slow-close symptom most likely followed
from the same root cause (running the full FLIP4 pipeline -- worker
thread, per-frame GPU work, ioctls -- unnecessarily) rather than being a
separate bug; it was not investigated further in isolation since the fix
below eliminated it too.

**Fix: extended the existing FULLSCREEN GATE pattern with an IMMEDIATE
MODE GATE.** `VK_PRESENT_MODE_IMMEDIATE_KHR` swapchains now fall through
to native passthrough WSI unconditionally, the same way non-fullscreen
windows already did -- no benefit to that app from engaging FLIP4, real
cost from doing so anyway. `FLIP_TEST_ALLOW_IMMEDIATE=1` overrides this
for testing the FLIP4 path itself against an IMMEDIATE-mode swapchain.
Confirmed fixed: `gears -f` now correctly bypasses FLIP4 (log:
"app requested VK_PRESENT_MODE_IMMEDIATE_KHR... falling through to native
WSI"), fps is back to normal, and the close delay is gone -- both
user-confirmed. Matches the user's own stated expectation going in: "I
pretty much expect the layer to not be active when vsync isn't enabled."

## Update 2026-08-18 part 3: Flatpak DDNet crash -- stale-handle-forwarded-to-ICD

`flatpak run tw.ddnet.ddnet` (a Flatpak-sandboxed Vulkan game, DDraceNetwork)
crashed (SIGSEGV) shortly after launch under this layer, but ran fine for
10+ seconds with `VK_TEGRA_X11_PRESENT_DISABLE=1` -- confirming this
layer's involvement was the trigger, not a pure app-side bug that would
crash regardless.

Debugging a Flatpak sandbox needed a different approach than every other
app in this investigation: gdb attaching from the host (following forks
across the `bwrap` sandbox boundary) failed to map shared library
sections correctly once execution crossed into the sandbox's own mount
namespace. `strace -f` also proved unreliable timing-wise (slowed
execution enough to change behavior). What worked: `flatpak run --devel`,
which swaps the app's `org.freedesktop.Platform` runtime for the matching
`org.freedesktop.Sdk` (already installed, includes gdb) *inside* the
sandbox -- then `--command=gdb ... --args /app/bin/DDNet` runs gdb
natively inside the same mount namespace as the target, avoiding the
cross-namespace symbol/fork issues entirely. A `-O2` build's backtrace
initially looked like the crash was in a `VkSubmitInfo` full of garbage
(a pointer into an X11 function, another into glibc malloc internals) --
misleading debug-info imprecision under optimization; a `-O0 -g3` debug
build (temporarily swapped into the Flatpak runtime's `nvidia_libs/`
folder in place of the production build) gave a clean, trustworthy
backtrace instead.

**Root cause**: `layer_AcquireNextImageKHR` crashed at
`if (!sc) return dev->d.AcquireNextImageKHR(...)` -- with `sc == NULL`
and every other local showing genuine uninitialized-stack garbage (not
misattributed this time; the `-O0` build confirmed it), meaning
`as_swapchain()` correctly didn't recognize the handle as one of ours,
and the fallback blindly forwarded it to the real ICD's
`vkAcquireNextImageKHR` -- which then crashed deep inside
`libnvidia-glcore.so.32.3.1`. The handle in question *used to be* one of
our own `Swapchain*` structs: DDNet's own swapchain lifecycle logic
double-destroys the same handle across its recreation flow (confirmed
directly in the fixed build's logs -- see below), and something (almost
certainly a separate render thread, given DDNet runs several) then calls
`vkAcquireNextImageKHR` on the already-destroyed handle. Calling any
Vulkan function on an already-destroyed swapchain is undefined behavior
per spec -- a genuine race/bug in the app -- but `layer_DestroySwapchainKHR`
previously `free()`'d the struct immediately, so by the time the stale
Acquire arrived, `as_swapchain()` correctly said "not ours" (the freed
memory no longer matched our magic number) and our layer naively treated
that as "must be a genuine native handle, forward it" -- except it was
*never* a real native handle in the first place (we don't create one for
the FLIP4 path), so the ICD received a completely foreign pointer it had
never allocated and crashed on it. This layer's swapchain-destroy timing
(worker thread join, GLX context teardown, etc. -- all absent from native
WSI's much lighter teardown) most likely widens the race window enough to
make it reliably hit, where native WSI's faster teardown mostly doesn't.

**Fix: never actually `free()` the `Swapchain` struct.**
`layer_DestroySwapchainKHR` now zeroes it (as before) but then tombstones
it with a second, distinct magic value (`SWAPCHAIN_MAGIC_DEAD`) instead of
freeing it -- a small, permanent, one-time-per-swapchain memory leak
(swapchains are created rarely: window resizes, fullscreen toggles, not
per-frame) in exchange for `as_swapchain()`-adjacent entry points
(`AcquireNextImageKHR`, `QueuePresentKHR`, `GetSwapchainImagesKHR`,
`DestroySwapchainKHR` itself for double-destroy) being able to positively
recognize "this handle used to be ours" via a new `is_dead_swapchain()`
check, and return a proper Vulkan error (`VK_ERROR_OUT_OF_DATE_KHR`,
spec-correct for "this swapchain is no longer usable") instead of
forwarding a guaranteed-foreign pointer to the real ICD. Confirmed fixed
end-to-end: the exact double-destroy race is now visible in the log as a
handled warning ("already destroyed, ignoring double-destroy") instead of
a crash, DDNet reaches its main menu and runs normally, user-confirmed.

## Update 2026-08-18 part 4: refined the crash fix to avoid the struct leak

Reasonable pushback on part 3's fix: tombstoning the `Swapchain` struct in
place (never `free()`-ing it, just overwriting its magic value) leaks the
*entire* multi-KB struct -- including its `PerImage images[MAX_IMAGES]`
array -- once per swapchain ever destroyed, for the rest of the process's
life.

**Replaced with a small dedicated registry** (`g_dead_swapchains`, reusing
the `bucket()`/`HASH_BUCKETS` hash-table pattern already used for
`g_dev_table`/`g_inst_table`) that tracks only the destroyed pointer
*value* in a ~16-byte node, while `layer_DestroySwapchainKHR` goes back to
freeing the struct normally. `is_dead_swapchain()` now checks this
registry instead of reading a magic value out of (necessarily still-alive)
struct memory.

This reopened a real, separate bug: since the struct *is* freed again, the
exact same address gets handed back out by the allocator for the very
next swapchain created -- confirmed directly in testing, a brand new,
perfectly valid swapchain landed at the same address as one destroyed
moments earlier, and the stale "dead" entry for that address was still in
the registry. `GetSwapchainImagesKHR` on that new-but-flagged-dead
swapchain returned `VK_ERROR_OUT_OF_DATE_KHR`, which DDNet's own error
handling treated as fatal ("Could not get swap chain images", a visible
error dialog -- not a crash, but a real regression from this specific
fix). Fixed with `unmark_swapchain_dead()`, called right after every
successful `calloc()` in `layer_CreateSwapchainKHR`: clears any stale
registry entry for the freshly (re)used address before anything can query
the new swapchain and get told it's dead.

Confirmed fixed end-to-end with extended, more adversarial testing than
the original repro: 128 swapchain recreations across the same session,
including repeatedly toggling DDNet's "fullscreen" vs "desktop fullscreen"
settings (both correctly detected as fullscreen and engaged FLIP4 --
apparently SDL sets `_NET_WM_STATE_FULLSCREEN` for both on X11, the
difference being whether SDL also does a video-mode switch, not the WM
hint) -- zero real errors, only the expected harmless double-destroy
warnings, clean shutdown on exit, user-confirmed working with no graphics
error dialog.

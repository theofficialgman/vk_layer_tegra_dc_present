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
gcc -O0 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -fPIC -fvisibility=hidden -shared -Wl,--no-undefined -Wl,--version-script=flip_layer.map \
    -I. -o libVkLayer_flip_test.so flip_layer.c -lpthread -ldl

VK_LAYER_PATH=$(pwd) VK_INSTANCE_LAYERS=VK_LAYER_FLIP_test VK_TEGRA_X11_PRESENT_LOG=2 \
    ~/Vulkan/build/bin/gears -vs        # windowed
    # or: ~/Vulkan/build/bin/gears -f -vs   # fullscreen
```

Optional env vars (all default to off/baseline if unset):
- `FLIP_TEST_DELAY_US=<us>` -- extra sleep inserted right before `FLIP4`,
  after the SGI wait. Used for the manual timing sweep.
- `FLIP_TEST_KERNEL_WAIT=1` -- Option 1: adds the hardware `pre_syncpt_id`
  latch gate on top of the SGI wait (see "Option 1 / 1b" below).
  `FLIP_TEST_SYNCPT_OFFSET=<N>` overrides its target delta (default 1);
  large values (e.g. 300) are a proof-of-execution stall test, not a
  normal mode -- see below before using it.
- `FLIP_TEST_KERNEL_PACE=<N>` -- Option 1b: fully GLX-free pacing via a
  blocking kernel syncpoint wait, targeting every `N`th vblank (`1` =
  ~60fps, `2` = ~30fps). Takes priority over `FLIP_TEST_KERNEL_WAIT` if
  both are set.

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

**Not yet validated / worth doing before treating this as production-ready:**
- Only observed for a short live run -- no extended-duration soak test,
  no stress test of the backpressure/reuse timing under this new (heavier
  per-frame GPU cost) path.
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

# gpu/ps4_vk_wsi.cpp: the merged VideoOut presenter

One Vulkan WSI for all ports: VK_EXT_headless_surface + VK_KHR_swapchain on VideoOut for the statically linked
RADV. Not used by any port yet (no migration now). Built from SSBU's `port/ps4_vk_wsi.cpp`; the behaviors the
Vita3K/M33/Conker branch grew were written again from their description, not copied (see License).

## Sources compared

| File | Lineage | Notes |
|---|---|---|
| `SSBU/port/ps4_vk_wsi.cpp` (698 lines) | SMW <- Resonance <- Crema | **base**: timeline semaphores kept, binary dropped; step trace; pitch; QueueSubmit2 |
| `Conker/ps4/ps4_wsi.cpp` (706) | Vita3K <- M33 <- Resonance <- Crema | banks + retirement, B8G8R8A8, storage, `ps4_wait_vblank`, `ps4_flip_pending`; GPL-2.0+ header |
| `~/vita3k-build/.../vita3k/ps4/ps4_wsi.cpp` (703, 2026-10-23) | newest of that branch | = Conker's minus B8G8R8A8, plus "FG forces vsync flips", slots 12-15 for the launcher |
| `~/m33-build/.../ps4/PS4VulkanWSI.cpp` (649, 10-21) | older | nothing the two above lack; PPSSPP GPL-2.0+ header |
| `~/cemups4-build/.../Vulkan/PS4WSI.cpp` (577, 10-17) | Crema, oldest | still signals the acquire semaphore in an empty submit (what later lost the device); frame trace |
| `ps4-kit` Mesa `ac_ps4_drm.c` | winsys | `ac_ps4_submit_flip` now only queues the job to the submission thread and returns 0; a failing `sceGnmSubmitAndFlipCommandBuffers` is logged by the winsys (`radv/ps4: ...`), not returned |

HENkaku 1.12's "vsync-paced frames" are the launcher's own `wait_flipped()` (ps4_launcher.cpp, its own buffers in
slots 12-15), and FG "outputs tracked by VideoOut flip numbers" is `ps4_flip_pending()` + the renderer. Neither
needed more in the WSI than `flip_pending()`, `wait_vblank()`, `set_force_vsync()` and slots 12-15 left free.

## Features and where they came from

| Behavior | From | Changed? |
|---|---|---|
| Headless surface, swapchain on linear host-visible images, 2 MiB aligned, pitch from the subresource layout | both (Crema) | no |
| GPU-side flip via `ac_ps4_submit_flip`, CPU wait + `sceVideoOutSubmitFlip` fallback | both | `ac_ps4_submit_flip` is a **weak** reference: links (and falls back) with a Mesa without it |
| Binary semaphores dropped, timeline semaphores kept (QueueSubmit + QueueSubmit2/KHR) | SSBU | fast path: submits with no semaphores pass straight through (no vector copies); real submit functions resolved once at vkCreateDevice; `vkQueueSubmit2KHR` fallback for 1.2 devices; QueueSubmit2 only handed out where RADV has it for the device |
| Acquire fence by an empty submit; acquire semaphore never signalled | both | no |
| 3 banks of 4 VideoOut slots (0-11), parked swapchains released after later flips | Conker/Vita3K (idea) | reimplemented; the "is anything parked" check in present is an atomic (Conker read the vector unlocked) |
| Slots 12-15 left for the app's own screens | Vita3K (launcher) | `kAppFirstBuffer` |
| R8G8B8A8 and B8G8R8A8 (VideoOut A8B8G8R8 / A8R8G8B8 = 0x80000000) | Conker | B8G8R8A8 listed first (plume needs it; DXVK takes whichever) |
| Storage usage where linear images allow it (FSR) | Conker | checked per format before creating (invalid usage isn't a dependable error code); Conker's retry without storage kept as a second net |
| `vkEnumerateInstanceLayerProperties` (0 layers), layers stripped from vkCreateInstance | Conker | + `vkEnumerateDeviceLayerProperties`, device layers stripped; extension enumeration under a layer name answers `VK_ERROR_LAYER_NOT_PRESENT` (SSBU returned RADV's list) |
| Step trace to `ps4_boot_trace` (first 400 lines) | SSBU | weak reference; `set_step_trace(n)`, 0 = off |
| Early `video_out_open()` with its own lock and step lines | SSBU | no |
| Flip event queue for the acquire wait, 1 ms polling if it can't be made | SSBU | no |
| Off-screen wait: 40 x 5 ms then use the image anyway | both | **per-image flip numbers** instead of `n-N+2` (see below) |
| IMMEDIATE -> HSYNC flips, FIFO/MAILBOX -> VSYNC | both | `set_force_vsync()` (Vita3K forced vsync while FG is on) |
| Splash hidden at the first flip | both | no |
| 10 s present stats in the log | both | + longest frame; `present_stats()` for an overlay; `set_stats_log()` |
| `flip_pending(grace_us)` | Vita3K/Conker (idea) | reimplemented, same polling |
| `wait_vblank(timeout_us)` on its own vblank queue, off for good after a timeout | Vita3K/Conker (idea) | the 58-61 Hz guard moved to the caller: `vblank_rate()` reports the measured rate |
| `flip_info()` | new | sceVideoOutGetFlipStatus fields + counts, no new hardware behavior |
| vkDestroyDevice with parked swapchains | new | parked swapchains of that device lose their Vulkan handles (not freed through a dead device later) |
| `vkGetPhysicalDeviceSurfaceFormats2KHR`, `...PresentRectanglesKHR`, device-group present queries | new | trivial answers; RADV's were null |
| Present lock | new | flip numbers reach VideoOut in the order they're counted if two threads present |

### Changes to proven behavior (not run on hardware yet)

- **Per-image off-screen tracking.** Each image keeps the flip number that showed it; acquire waits for that number
  + 1. In steady round-robin this is exactly SMW's `n-N+2`. It differs only where the old formula was wrong: a new
  swapchain's never-shown images (old: waited for the old swapchain's flips), and an image acquired again without a
  present in between (old: a full 200 ms timeout; new: no wait, since no later flip exists to wait for).
- **Acquire with timeout 0** returns `VK_NOT_READY` when the image is still on screen (spec). Any other timeout keeps
  the old behavior (wait up to 200 ms, then use the image). No known port passes 0.
- Everything in the table marked new/changed above.

## License situation

The merged file carries **no license header**: it is built from the SSBU/SMW lineage plus independent
reimplementation of ideas. No lines were taken from the GPL-headed Conker/Vita3K/M33 files; what came from them
are ideas and facts (bank/retire scheme, B8G8R8A8 support, the A8R8G8B8 enum value 0x80000000, ETIMEDOUT
0x8002003C, `sceVideoOutAddVblankEvent`'s real signature, the flip_pending/wait_vblank semantics), written anew.

Caveats for whoever decides the kit's license:
- SSBU's file sits under SSBU's MIT license (ShiroKlein); SMW's under SMW's MIT license.
- The common skeleton (both lineages are near-identical there) goes back to Resonance's `ps4_wsi.cpp` (Resonance is
  GPL-2.0) and Crema's `PS4WSI.cpp` (inside Cemu, MPL-2.0; the file has no header). If those files are the user's
  own work, the user can license them freely; if not, the SSBU lineage is GPL/MPL-derived too and the header question
  applies to it as well. This needs the author's answer, not a code change.

## Pacing ideas not taken (untested; for later hardware tests)

- **Blank flip before unregistering** (`sceVideoOutSubmitFlip(handle, -1, ...)`, the SDK's BUFFER_INDEX_BLANK) to free
  a bank at once instead of parking, and before vkDestroyDevice. Not used by any port.
- **`sceVideoOutSetFlipRate(30/20 Hz)`** for apps locked at 30/20 fps: VideoOut would hold each frame an even number
  of vblanks. Never tried in our ports.
- **Real MAILBOX**: flips are queued to the GPU and can't be replaced; only a CPU-side "newest frame" slot flipped from
  a vblank thread could do it (adds a CPU flip path and a thread). FIFO behavior kept.
- **Latency knob**: acquire could also wait until `submitted - shown <= n` (n = 1 halves input latency at 3 images,
  costs throughput). A trade-off, not clearly better: left to measure.
- **`sceVideoOutIsFlipPending`** for `flip_pending()` and **`sceVideoOutWaitVblank`** for `wait_vblank()`: both in the
  OpenOrbis stubs, neither used on hardware by us.
- **A second flip event queue** (`sceVideoOutAddFlipEvent` on two queues) so `flip_pending()` could sleep instead of
  poll; whether VideoOut delivers to both is unknown.
- **The flip event's data** (likely the flipArg) instead of a `sceVideoOutGetFlipStatus` call after each wakeup.
- **VK_KHR_present_id / present_wait** mapped onto flip numbers: a standard pacing API for apps; easy on top of
  `shown_by`, but no port asks for it.
- **SRGB and MUTABLE_FORMAT swapchains**; screen size from `sceVideoOutGetResolutionStatus` instead of fixed 1080p.

## Open questions

- `OrbisVideoOutFlipStatus::ptime` unit (process time in microseconds by the SDK's naming; exposed unconverted as
  `FlipInfo::flip_time`). Check on hardware against `sceKernelGetProcessTime()`.
- Dropping binary semaphores is only safe with one queue. Does the PS4 RADV expose a second queue family (compute)?
  Surface support answers family 0 only.
- QueueSubmit (v1) with a `VkTimelineSemaphoreSubmitInfo` replaces the whole pNext chain with the rebuilt timeline
  struct (SSBU's behavior; other structs in that chain are dropped). DXVK uses QueueSubmit2, so this path is
  untested with timelines.
- Since the winsys submission thread, `ac_ps4_submit_flip` can't report a failed flip; a lost flip shows up only as
  acquire timeouts (`N timeouts` in the stats line) and the winsys log.
- Compile check: the given command fails with the libc-overlay headers ahead of libc++'s; it compiles (no warnings with
  `-Wall -Wextra`) with `$PS4_CXX_FLAGS` placed **before** `$PS4_BASE_FLAGS`.

## 2026-11-20: submitDone moved behind flip_made

Present no longer references `ac_ps4_submit_done` at all. The last CPU-present path called it before
`sceVideoOutSubmitFlip`, which made every frame wait out the kernel's 500 ms submitDone deadline
(1 fps) and eventually stopped submission completion during texture loading (system coredump, shell
reboot). The WSI now calls `ac_ps4_flip_made(flipArg)` only when `sceVideoOutGetFlipStatus` reports a
newer flip, and `shutdown_video` passes the last observed `flipArg` after its bounded wait. Present
caps the frame-fence wait at 200 ms and skips the flip on timeout instead of waiting forever. The
Mesa side keeps a monotonic flip number and ignores repeats; the old `ac_ps4_submit_done` symbol is
inert. Details and the port checklist: `docs/gpu-submit-and-videout.md`.

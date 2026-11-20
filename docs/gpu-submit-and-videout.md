# GPU submission and VideoOut teardown

Two failures from SuperTuxKart's PS4 port that look unrelated and are not:

- the app froze for ~150 s in the middle of texture loading, then carried on;
- closing the app turned the whole display black until SceShellUI restarted.

Both came from one wrong assumption about `sceGnmSubmitDone`.

## What the console showed

The boot trace (kit WSI step trace) gave the shape of the freeze:

```
[   2.166] wsi: flip 1 of buffer 0 (GPU-side)
[ 151.946] wsi: flip 2 of buffer 1 (GPU-side)
[ 151.946] ps4kit/wsi: 0.0 flips/s, longest frame 149779.7 ms
```

A 149.8 s "frame" is not slow loading: the main thread was blocked, and the next
`vkAcquireNextImageKHR` only ran when whatever it waited for finished.

`hang.log` (watchdog thread dumps) showed why. All ten command-loader threads had
the same stack:

```
ps4_wait_common+0x509
ps4_wait+0x21
vk_drm_syncobj_wait_many
__vk_sync_wait_many
vk_common_WaitForFences
GEVulkanCommandLoader::endSingleTimeCommands
GEVulkanTexture::createTextureImage
GEVulkanTexture::reloadInternal
```

They were waiting on their upload fences. The GPU was not retiring submissions.

The kernel log named the actual fault:

```
GPU failed to become idle within 500 ms after submitDone. 0xa0003128
### GPU Timeout occurs, and async_suspend will be ignored.
### Can't suspend pid:96. submitDone() is not called.
# exception: 0xa0d0c008 (GPU_FAULT_SUBMITDONE_TIMEOUT_IN_SUSPEND_ASYNC)
```

## Cause: `submitDone` is a promise that the GPU will go idle

`sceGnmSubmitDone` tells the system the frame's work is in and that the GPU can
fall idle. The kernel then gives it **500 ms** to actually become idle. The
winsys used to call it after every flip *and at least every 4 ms otherwise*, to
satisfy the "once a frame" requirement. During texture loading there are no
flips for tens of seconds while ten threads keep submitting uploads, so
`submitDone` was fired thousands of times with the GPU permanently behind. Every
call started a 500 ms deadline the GPU could not meet; after enough timeouts the
GPU subsystem faulted, fences never signalled, and the app sat in a wait that
looked like a hang.

The fix is in `gpu/mesa-patches` (`ac_ps4_drm.c`, `ps4_submit_thread`): call
`submit_done()` only for a **flip that VideoOut reports made**, never at submit time
(see the 2026-11-20 update below for the guarded API). A
loading screen never promises an idle GPU it cannot deliver; a presented frame
does, once the uploads behind it are in.

This is the rule for any port on the kit: `submitDone` once per presented frame,
**after** that frame's work, never on a timer and never while more work is
queued.

## Closing the app must give VideoOut back

The same GPU fault is why closing the app blacked the screen: a process that
dies (or is killed by the shell) while its VideoOut buffers are still registered
can leave the display pipeline owned by a dead app; SceShellUI then restarts to
recover.

`ps4_kit_exit` now calls `ps4kit_video_shutdown()` (weak, implemented by
`gpu/ps4_vk_wsi.cpp`) before `sceSystemServiceLoadExec("exit")`. It unregisters
parked buffer sets, then closes the VideoOut handle (retrying briefly for
buffers still on screen). Any port that opens VideoOut should do the same before
asking the shell to close it.

Never force the console off while a port owns VideoOut; close with the PS
button. If the app is unresponsive, wait for the shell's own close to complete.

## Reading this next time

1. Pull logs: `python3 ps4/pull-logs.py`. `klog.txt` comes from the console's
   klog socket (3232); boot-trace, stdout and `hang.log` over FTP.
2. `boot-trace.log`: flips carry timestamps. A gap between two flips is the app
   not presenting — a stall *inside* the frame, not a VideoOut problem. A
   "0.0 flips/s, longest frame N ms" line confirms it.
3. `hang.log`: every thread's stack, every 12/25/45/90 s, but only until the
   first frame is drawn. Ten identical stacks mean a worker pool behind one
   GPU wait, not ten bugs.
4. Symbolize against the exact `.oelf` from the build. `llvm-symbolizer` returns
   `??` for this file; sort `llvm-nm` and take the nearest symbol:

   ```sh
   llvm-nm -n --defined-only supertuxkart.oelf |
     awk '$2 ~ /^[tTwW]$/ {print $1, $3}' > syms.txt
   ```

   Then bisect the sorted addresses for each stack entry (the mesa/patch
   addresses are `.text`-relative, so no base adjustment is needed).
5. `klog.txt`: `grep -E "GPU|submitDone|Timeout|suspend"`. The exact kernel
   sentence above is the signature of this bug.
6. RADV's own report, when `/data/Crema` exists, is
   `/data/Crema/gpu/gpu-hang.txt`. `pull-logs.py` fetches it now.

## Kit files involved

- `gpu/mesa-patches/*`: the PS4 RADV series; `ac_ps4_drm.c` owns submissions,
  the job ring, fences and `submitDone`.
- `gpu/ps4_vk_wsi.cpp`: VideoOut swapchain, flips, `shutdown_video`.
- `runtime/ps4_runtime.cpp`: `ps4_kit_exit`, the only exit path.
- `docs/bring-up-and-iteration.md`: the earlier black-screen failures (init
  loops, SDL driver selection, pthread attributes).

## The shell UI owns the display (STK port, 2026-11-19)

Going home used to freeze the whole console: the shell stopped servicing the app's flips, the fence of the frame
whose flip was waiting never signalled, `GEVulkanDriver::endScene()`'s 2 s timeout took its Windows recovery path,
and that path calls `vkDeviceWaitIdle`, which has no timeout. The app wedged, and the shell's VideoOut handover
wedged the console UI with it.

Two changes, both needed:

- `AcquireNextImageKHR` returns `VK_ERROR_OUT_OF_DATE_KHR` while the shell UI is in front (the two SystemService
  status queries, weak, plus a VideoOut flip backlog fallback for firmware without them). The app draws nothing
  and resumes on the first acquire after; no GPU work is queued behind a flip nobody services.
- The PS4 branch of `endScene`'s fence-timeout path skips the frame instead of destroying the swapchain. Never
  call `vkDeviceWaitIdle` or `destroySwapChainRelated()` from a timeout on PS4: the timeout is the display being
  taken, not a device error, and waiting for the device there is what froze the console.

Evidence: submit 567 never completed (CP/CPF busy, all shader units idle, no page fault), last WSI step was flip 67
at 22.634 s, then 2 s timeouts and a permanent block in `vkDeviceWaitIdle`.

## Update 2026-11-19: submitDone and acquire

- `sceGnmSubmitDone` is called only through `ac_ps4_flip_made(flipArg)`, and only when
  `sceVideoOutGetFlipStatus().flipArg` shows that a **newer** flip has been made. The
  submission thread never calls it. Calling it at submit time promises the kernel that
  the GPU can go idle while the frame and its texture uploads are still running; the
  kernel then faults with `GPU_FAULT_SUBMITDONE_TIMEOUT_IN_SUSPEND_ASYNC` after 500 ms.
  `ac_ps4_submit_done()` still exists for old ports but is inert (it logs once), so the
  old call can no longer start a deadline it cannot meet.
- `vkAcquireNextImageKHR` must never return `VK_ERROR_OUT_OF_DATE_KHR` while the shell UI
  is overlaid. GE resets the frame fence before acquiring, so an acquire error leaves
  that fence unsignaled and every later `endScene()` waits its 2 s timeout and skips the
  frame, leaving the app black. Wait for VideoOut to consume flips instead, with a
  bounded cap (30 s) so a wedged VideoOut still lets the frame loop proceed.
- If an acquire does fail, `GEVulkanDriver::endScene()` must re-signal the already-reset
  frame fence with an empty `vkQueueSubmit`; otherwise the frame loop is permanently
  bricked.

## Update 2026-11-20: the 500 ms trap in present, and the guarded API

This is the failure that made SuperTuxKart's menus run at 1 fps and froze the tutorial.
Both symptoms had one cause, and it was in the kit's CPU present path.

### Evidence

- The WSI stats line was `1.0 flips/s, longest frame 1029 ms, acquire waits 0 ms/s,
  0 timeouts, flipArg 84 of 86 submitted`. `vkAcquireNextImageKHR` and
  `sceVideoOutSubmitFlip` each took ~512 ms: the kernel's 500 ms `submitDone` deadline,
  not render cost.
- Loading the tutorial stopped with `gpu-hang.txt` saying `waiting for 632, GPU completed
  631, submitted 632`; every GPU unit was Idle. The klog showed a system coredump,
  `reason=0xa0d0c007`, `KillApp(STKR00001)`, and SceShellUI rebooting. It was not an STK
  segfault.
- The old `crash.log` was stale: the same signal/fault/thread signature appeared across
  runs. The klog, not `crash.log`, named the kill.

### Cause

Present called `ac_ps4_submit_done()` **before** `sceVideoOutSubmitFlip`. Every present
promised the kernel an idle GPU while the frame and its uploads were still queued, so
every frame waited out the 500 ms deadline. During loading, enough of those timeouts
stopped submission completion entirely. Present then waited the frame fence with **no
timeout**, so the app froze behind a fence that could no longer signal.

### Fix (kit, not port)

- Present no longer calls `submitDone` at all. When the WSI next reads
  `sceVideoOutGetFlipStatus` and `flipArg` has advanced, it calls
  `ac_ps4_flip_made(flipArg)` — once for that flip.
- The frame-fence wait in present is capped at **200 ms**. On timeout the WSI logs
  `frame fence not signalled after 200 ms, flip skipped` and returns without a flip, so
  the app keeps running.
- No `vkDeviceWaitIdle` and no device/queue wait without a timeout in the frame path.
- `shutdown_video` uses the last observed `flipArg` for its final `flip_made` call, then
  unregisters the banks and closes VideoOut.

### The API now enforces the rule

`ac_ps4_drm.c` keeps a monotonic `ps4_last_flip_made`. `ac_ps4_flip_made(int64_t)`
accepts only a strictly newer `flipArg` (compare-and-swap); a repeated or stale number
returns without calling `sceGnmSubmitDone`. `ac_ps4_submit_done(void)` remains as a
link-compatible, deliberately **inert** symbol that warns once. A port can therefore
still link, but it cannot promise an idle GPU without naming a flip that VideoOut made.

### Present recipe for any port

1. Submit the frame's work and keep its fence.
2. Wait that fence with a bound. If it is not signalled, **do not flip** and do not
   `submitDone`; the frame is late, not the device.
3. Submit the VideoOut flip.
4. On the next acquire/present, read `flipArg`; when it advances, call
   `ac_ps4_flip_made(flipArg)` once.
5. On exit, wait the last flip (bounded), `flip_made(lastArg)`, unregister every bank,
   close VideoOut.

Never: `submitDone` at submit time or on a timer; an unbounded fence/flip wait;
`vkDeviceWaitIdle` in a timeout path; closing VideoOut with flips still queued.

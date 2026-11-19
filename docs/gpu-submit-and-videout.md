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
`submit_done()` only for a **flip** whose submission queue has **drained**. A
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

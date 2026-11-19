# Bringing a port up: black screens, and iterating without full installs

Lessons from SuperTuxKart for PS4 (`STKR00001`: ps4-kit + SDL2 + Irrlicht + RADV/VideoOut), written
so the next bring-up does not repeat them. The symptoms below are generic; the exact paths and
numbers are from that port.

## 1. A black screen is usually a retry loop, not a crash

STK crashed before this, so "no crash log" looked like progress. It was not: the boot trace showed

```
SDL_TimerInit -> 0
SDL_InitSubSystem(0x21) -> -1: "No available video device"
SDL_InitSubSystem(0x8000) -> 0
```

repeating ~540,000 times in 69 seconds. Irrlicht's `IrrDriver` recreates the device whenever
`SDL_InitSubSystem(SDL_INIT_VIDEO)` fails, so the process stayed alive doing nothing.

- Read the error string (`SDL_GetError()`), not just the return code. "No available video device"
  names the subsystem; it does not mean the window or the GPU failed.
- When an init fails in a loop, **rate-limit the trace** (first 5 failures, then every 2000th) or the
  log rolls over and hides the first, useful lines.
- The loop also degrades over time: after ~500k retries the error became "Couldn't initialize pthread
  attributes" — an exhausted resource, not a second root cause. Fix the first failure.

## 2. SDL's dummy video driver is not a fallback: ask for it by name

`SDL2/src/video/dummy/SDL_nullvideo.c`:

```c
static int DUMMY_Available(void)
{
    const char *envr = SDL_GetHint(SDL_HINT_VIDEODRIVER);
    if (envr) {
        if (SDL_strcmp(envr, DUMMYVID_DRIVER_NAME) == 0) return 1;
        ...
    }
    return 0;
}
```

So a build with `SDL_DUMMYVIDEO=ON`, `SDL_VIDEO_DRIVER_DUMMY 1` and `DUMMY_bootstrap` linked in still
reports **no driver at all** unless the hint names it. `SDL_VideoInit(NULL)` walks `bootstrap[]`,
every `create()` returns NULL, and it sets exactly "No available video device".

Fix, before any SDL initialisation (`__attribute__((constructor(210)))` in the port):

```c
SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
setenv("SDL_VIDEODRIVER", "dummy", 1);   // for SDL builds that read the environment directly
```

For a Vulkan/VideoOut port the dummy driver is the right one: it only creates a window object, and
the real swapchain/surfaces come from the kit's WSI (`gpu/ps4_vk_wsi.*`, wrapped by
`SDL_Vulkan_GetInstanceExtensions` / `SDL_Vulkan_CreateSurface`). Do not go looking for a KMS/DRM
driver to write first.

General rule: a driver compiled in is not a driver selected. Trace `SDL_GetCurrentVideoDriver()`
after init, or log the hint at startup.

## 3. Third-party code does not check `pthread_*attr_init` — assume it can fail

libkernel does not store a `pthread_mutexattr_t` in the caller's variable: it takes a kernel object
and writes an 8-byte handle. On retail firmware that pool can fail once the process heap exists; the
call returns an error and leaves the variable untouched. SDL (`SDL_CreateMutex`) and libc++
(`__libcpp_recursive_mutex_init`) ignore the return and pass the uninitialised 8-byte value to
`pthread_mutexattr_settype`, which dereferences it in libkernel. That was STK's first crash
(rip `0x800009fdc`, thread in `SDL_CreateMutex`).

What works, in the port:

- Build SDL without the attribute API on PS4; `SDL_sysmutex.c` emulates recursive mutexes itself.
- Define `pthread_mutexattr_init/settype/destroy` and `pthread_mutex_init/lock/unlock/trylock`
  wrappers (`--wrap`) that hand every caller a valid attribute object of the port's own and emulate
  recursion when the kernel could not supply one. Never leave an uninitialised attribute in a
  caller's hands.
- Log the first failure to the boot trace. The crash log then stops pointing into libkernel.

## 4. Instrumentation that paid for itself

- A milestone logger in constructor priority 210 (after the kit's heap/threads/trace constructors,
  before the app's static initialisers) writing to `/data/ps4-kit/boot-trace.log`.
- `--wrap` on the init entry points you suspect (`SDL_Init`, `SDL_InitSubSystem`, `SDL_TimerInit`,
  `SDL_VideoInit`, `SDL_CreateWindow`, `SDL_PollEvent`), logging flags, return value and error text.
- A watchdog thread dumping stacks at 12/25/45/90 s into `/data/ps4-kit/hang.log`. Note: in this
  bring-up `ps4_dump_thread_stacks` only dumped the watchdog thread itself, so the file did not
  explain the hang. Dump the main thread explicitly (or every thread) or it is not worth having.
- Symbolise with `llvm-symbolizer --obj=bin/supertuxkart <addr>`; trace addresses are relative to
  `.text`.

## 5. Iterating without reinstalling the game image

What the console actually allows (verified):

- `/app0` is the **installed package image**. `/user/app/<TITLE_ID>/` (FTP-writable) holds only
  `app.pkg`, `app.json`, `app.xml`, `app.pbm` — there is no loose `eboot.bin` to replace.
- Installing a package replaces `app.pkg`; with assets inside it, every code change costs a full
  install (STK: 1.16 GB) of SSD writes.
- `/data` is writable over FTP and survives installs.

So: **code in the package, stable data on `/data`.**

1. Put the data on `/data` once, over FTP. For STK the asset tree (code `data/` + `stk-assets/`,
   6890 files, 834 MiB) is mirrored to `/data/stk/data` by `ps4/upload-data.py`.
   Decide the precedence when both trees hold the same relative path with different content
   (`stk_config.xml` is one). The package stages `data/` and then merges `stk-assets/` over it, so
   the assets copy is what the game sees; the uploader deduplicates the same way (5590 unique paths).
   Without that, the two copies overwrite each other and the mirror never converges.
2. Point the game at it at runtime. STK's `FileManager` reads `SUPERTUXKART_DATADIR` before its
   compile-time default, so the port's constructor does `setenv("SUPERTUXKART_DATADIR",
   "/data/stk", 1)`. No rebuild.
3. Package only the executable: `STK_PKG_EBOOT_ONLY=1` skips staging the data trees. STK's package
   went from 1,160,773,632 to 60,686,336 bytes (~19x fewer SSD writes, seconds to install instead of
   minutes).
4. One command per iteration (`ps4/deploy.sh`): build → eboot-only package → FTP upload to
   `/data/pkg`. The developer still installs and launches from the console (no remote launch: the
   Remote Package Installer/BinLoader ports are closed).

Rule of thumb: anything that changes every iteration goes in the package; anything stable goes on
`/data`. Shaders and GUI data are stable enough for `/data` and save most of the bytes.

**Do not move a data path by editing a global compile definition.** Changing
`add_definitions`/`target_compile_definitions` invalidates every object file and turns a ~30 s
incremental build into a full rebuild. A runtime `setenv` in a constructor does the same job for
free.

## 6. FTP and console workflow

- FTP: port **2121**, user `anonymous` / any password, root is `/`; `SITE DF` is unsupported,
  `MLSD`, `SIZE` and `MKD` work. One big file moves at ~85 MB/s, but thousands of small files run at
  ~3-4 MiB/s because each STOR pays a handshake: plan a few minutes for a full asset mirror.
- Upload: `curl -sS -T file ftp://<host>:2121/data/pkg/<name> --user anonymous:anonymous`.
- The installed app is `/user/app/<TITLE_ID>/app.pkg` plus `app.json` / `app.pbm`; there is no
  loose `eboot.bin`, so the code cannot be hot-swapped over FTP. Reinstalling the same content ID
  *and* version fails with "package already installed". Ship a higher `APP_VER` instead: the same
  content ID with a newer version installs as an update (~60 MB) without uninstalling, and the save
  data is untouched. `ps4/deploy.sh` keeps a counter in `out/.stk-version` and bumps it every run.
- Logs live in `/data/ps4-kit/`: `boot-trace.log`, `crash.log`, `hang.log`.
- An FTP upload tool needs a socket timeout, retries with reconnect, and size-based resume. Without
  them a single stalled data connection blocks forever and looks like "the command hangs" (it did).

## 7. Packaging gotchas

- `create-gp4`: some bundled builds advertise `-path` but only implement `-files`, and choke on
  spaces. Prefer a current OpenOrbis build; `make-pkg.sh` takes `CREATE_GP4` for this.
- The toolchain's `PkgTool.Core` stops past ~4 GB ("Playgo Chunk hash file was not allocated enough
  space"); build the patched one with `scripts/build-pkgtool.sh` for data-heavy packages.
- `gp4-fix-dirs.py` restores folders `create-gp4` drops when two entries share a name.
- Weak symbols nothing defines become imports and `create-fself` rejects the eboot; use weak
  definitions, not weak declarations.
- `NO_RIGHT_SPRX=1` drops `sce_sys/about/right.sprx` for homebrew packages.

## 8. Checklist for the next agent

1. Boot trace present? If not, add the constructor milestone logger first; everything else depends
   on knowing how far the process gets.
2. Init fails? Log the return value **and** the library's error string; rate-limit retries.
3. SDL video failing on a Vulkan port? Set `SDL_HINT_VIDEODRIVER=dummy` + `SDL_VIDEODRIVER=dummy`
   before `SDL_Init`, then confirm `SDL_GetCurrentVideoDriver()` says "dummy".
4. Crash inside `SDL_CreateMutex`/libkernel? Check the pthread attribute wrappers (§3) before
   suspecting the heap.
5. Assets in the package? Move them to `/data`, override the data root at runtime, and build an
   eboot-only package before the next iteration; otherwise every test burns a full install.
6. Verify the data root actually exists on the console before launching (a missing
   `/data/stk/data` looks like a black screen too).

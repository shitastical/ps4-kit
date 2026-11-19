# ps4-kit

The shared PS4 base of our ports: one runtime, one Mesa/RADV, one presenter, one toolchain and one
set of packaging scripts, instead of a diverging copy in every port. It is the merge of the twelve
ports' copies (CEMUPS4, Conker, March33, Resonance, SMW, SSBU, Sonic, Vita3K/HENkaku, rcps3,
xemu-ps4, G-Diffuser, PS2/DawnStation), taking each copy's fixes. `docs/merge-notes/` records where
each piece came from and what each port would set when it moves over. Newer ports (Conker: Live & Reloaded,
Tekken Tag 2, Etterna, Godot, Space Cadet Pinball) build with it; the older ones still carry their own copies.

## Layout

| Path | What |
|---|---|
| `ps4-kit.cmake` | include from a project: `ps4_kit_app(target)`, `ps4_kit_link_mesa(target)` |
| `toolchain/` | clang 21 + libc++ 21 toolchain file, `env.sh`, libc header overlay, sysroot/libc++ build scripts |
| `runtime/` | heap (dlmalloc, blocking locks, per-thread arenas, overflow heaps), libc fixes, stdio lock, file layer (`*at`, cwd, path tidying), DNS, SMB/NFS under `/net`, crash log, sampler, profiler. API: `ps4_kit.h`, `ps4_kit_diag.h`, `ps4_kit_io.h` |
| `gpu/` | `build-mesa.sh` + `mesa-patches/` (32 PS4 patches on Mesa 26.0.8), `ps4_vk_wsi.*` (Vulkan swapchain on VideoOut) |
| `scripts/` | `make-pkg.sh`, `upload.sh`, `seed-build-tree.sh`, crash log and profile tools |
| `samples/kit-test/` | the kit on the console, measured; also the template for a new port |

## Build (WSL)

```bash
bash ~/ps4-kit/scripts/seed-build-tree.sh      # once: ~/ps4kit-build, ~/ps4-kit link, sysroot
bash ~/ps4-kit/gpu/build-mesa.sh               # RADV static libraries in ~/ps4kit-build/build/mesa
KIT_TEST_VERSION=00.01 bash ~/ps4-kit/samples/kit-test/build.sh
```

Then from Windows (Git Bash; uploads from WSL get undone on the console):

```bash
scripts/upload.sh out/PS4Kit-Test-v00.01.pkg 192.168.0.212
```

## A new port

```cmake
include(~/ps4-kit/ps4-kit.cmake)
add_executable(app ...)
ps4_kit_link_mesa(app)   # if it draws with Vulkan
ps4_kit_app(app)         # runtime objects, linker wraps, eboot.bin
```

Configure with `-DCMAKE_TOOLCHAIN_FILE=~/ps4-kit/toolchain/ps4-kit-toolchain.cmake` (or
`ps4_cmake` from `toolchain/env.sh`). Heap and diagnostics settings are in the runtime headers;
every diagnostic has an off switch, the profilers start off.

## Rules learned the hard way

- A weak symbol nothing defines becomes an import, and create-fself refuses the eboot ("missing
  module for symbol"): hooks get weak no-op definitions, not weak declarations.
- A `--wrap` keeps the wrapped name as an import too: functions no linked system library exports
  (`mkdirat`, `renameat`, `faccessat`, `readlinkat`) are defined outright instead.
- In compile commands, libc++'s include flags go before the libc overlay's.
- musl's stdio is never locked on the PS4: it turns its FILE locks on in its own `pthread_create`,
  and threads here come from libkernel. An unbuffered stream (stderr) is pointed at a buffer on the
  printing thread's stack, so two threads printing at once wrote each other's stacks (TTT2 v00.07
  crashed with ASCII in a saved register). `runtime/ps4_stdio.cpp` wraps every stdio output call in
  one recursive lock; a port must not take these wraps out or print through `*_unlocked` calls from
  several threads.
- A `--wrap` of a function nothing else in the program calls leaves `__real_<name>` as an import, and
  create-fself refuses it (TTT2: vprintf, putc, putchar, perror). Wraps of libc functions the app may
  not use are also linked with `--undefined=<name>`, which pulls the libc member in.

## Bringing a port up

`docs/bring-up-and-iteration.md`: why a port sits on a black screen (SDL's dummy video driver has to
be selected by name), the pthread attribute failure that crashes `SDL_CreateMutex`, the boot-trace
and watchdog instrumentation, and the retry loops that hide behind the first failure.

`docs/ssd-friendly-iteration.md`: the measured, repeatable loop for changing code without rewriting
the game image — assets on `/data`, an eboot-only package, and the console's content-ID install
rules (including what does not work: version bumps and hand-swapped `app.pkg`).

## License

MIT (`LICENSE`). The Mesa patches are under Mesa's own MIT license, and `runtime/third_party/dlmalloc`
keeps Doug Lea's notice.

# Saving SSD life: iterate code without rewriting the game image

Measured on SuperTuxKart for PS4 (`STKR00001`) on 2026-11-19. The method is generic; paths, sizes
and script names below are STK's.

## The problem

A fake package is installed as **one image**, `/user/app/<TITLE_ID>/app.pkg`. There is no loose
`eboot.bin` to overwrite and no directory of code files. The Package Installer also keys on the
package's **content ID**, not its version: a rebuilt package with a higher `APP_VER` was refused
with "package already installed" (a 00.02 build was).

With the game's 761 MiB of assets inside that image, every code edit costs a full
1,160,773,632-byte install, plus the same amount to upload the package first. Development turns
into a flash-write test.

The console allows one clean split:

| Part | Lives in | Changes |
|---|---|---|
| executable (and its libraries) | `app.pkg` | every build |
| tracks, karts, textures, GUI data | `/data/...`, as ordinary files over FTP | rarely |

**Code in the package, data on `/data`.** The package shrinks to the executable alone.

## What the console permits (measured)

| Action | Result |
|---|---|
| install a package whose content ID is not installed | works |
| uninstall the app, then install a build with the same content ID | works; `/data` survives |
| write files under `/data` over FTP | works; survives install and uninstall |
| install over a package with the same content ID | refused: "package already installed", even with a higher `APP_VER` |
| replace `/user/app/<TITLE_ID>/app.pkg` over FTP | launch fails: "the data was corrupted" |
| repair `app.json` by hand after such a swap | no: `packageDigest` is not the SHA-256 of `app.pkg` (`84F401…` vs `070E9B…`) |
| rotate `TITLE_ID` per build | would install beside the old build; not tried on this port |

The installed directory also holds `app.json`, `app.xml` and `app.pbm`; `app.pkg` is the whole game.
The content-ID rule is why version bumps do not help: the installer is not comparing versions.

## Numbers (STK, measured)

| | bytes | |
|---|---:|---|
| full package (code + assets) | 1,160,773,632 | 1.08 GiB |
| eboot-only package | 60,686,336 | 57.9 MiB |
| assets mirrored to `/data/stk/data` | 798,079,947 | 761 MiB, 5,590 files |
| executable inside the package | 58,915,776 | |
| icon, `param.sfo`, prx files, container overhead | 1,703,342 | |

Cost per code iteration, counting both the write over FTP and the install write:

| | full package | eboot-only |
|---|---:|---:|
| upload to `/data/pkg` | 1,160,773,632 | 60,686,336 |
| install writes `app.pkg` | 1,160,773,632 | 60,686,336 |
| **total per iteration** | **2,321,547,264** | **121,372,672** |
| 100 iterations | ~232 GB | ~12 GB |

That is 19.1x fewer bytes per iteration; the same ratio holds if only the install write is counted.

Steady state on the console after the switch: 60,686,336 (installed app) + 798,079,947 (assets) =
858,766,283 bytes, or 919,452,619 bytes if the uploaded package is kept in `/data/pkg` too. The old
single image was 1,160,773,632 bytes, or 2.32 GB with its uploaded copy.

Assets are written **once**; after that the uploader sends only changed files. It compares remote
sizes, retries with reconnect, and stops after a wall-clock cap, so it is safe to re-run.

## One-time setup

### 1. Mirror the assets

```bash
cd stk-code
python3 ps4/upload-data.py --dry-run    # what is missing
python3 ps4/upload-data.py              # upload; re-run to resume
```

STK's package stages `data/` and merges `../stk-assets/` over it, so the asset tree wins on a
duplicate relative path (`stk_config.xml` is one). The uploader applies the same precedence and
uploads each remote path once; otherwise two local copies overwrite each other and the mirror never
converges. After it finishes, `--dry-run` says `to upload: 0 files`. The measured mirror is 5,590
files and 798,079,947 bytes at `/data/stk/data`.

### 2. Build an eboot-only package

```bash
BUILD_DIR=/tmp/stk-ps4-build bash ps4/deploy.sh
```

`deploy.sh` runs the incremental build, then `pkg.sh` with `STK_PKG_EBOOT_ONLY=1` (which skips
staging both data trees), prints the package and its SHA-256, and uploads it to
`ftp://<console>:2121/data/pkg/`. The stage is:

| file | bytes |
|---|---:|
| `eboot.bin` | 58,915,776 |
| `sce_sys/icon0.png` | 60,090 |
| `sce_sys/param.sfo` | 504 |
| `libc.prx` | 3,424 |
| `libFios2.prx` | 3,200 |
| **stage total** | **58,982,994** |

The package itself is 60,686,336 bytes.

### 3. Install it once

On the console: close the app, delete the old entry (XMB: highlight it, **Options → Delete**; this
removes `/user/app/<TITLE_ID>` only), then **Settings → Debug Settings → Game → Package Installer**
and choose `/data/pkg/IV0000-STKR00001_00-STKPORT000000000.pkg`.

The installer refuses a second install of the same content ID, so deleting first is not optional.
Launch once and check the trace before starting the loop; a wrong data root also looks like a black
screen.

## The iteration loop

| # | Step | How |
|---|---|---|
| 1 | edit code | |
| 2 | build, package, upload | `bash ps4/deploy.sh` (incremental build is ~30 s) |
| 3 | close and delete the app | XMB: Options → Delete |
| 4 | install the new package | Package Installer → `/data/pkg/…pkg` |
| 5 | launch | |
| 6 | read the logs | `/data/ps4-kit/boot-trace.log` |

Steps 3–5 are manual because the console exposes no remote install or launch: at the time of
measurement FTP 2121 and the kernel log on 3232 were the only open ports, and the Remote Package
Installer and BinLoader ports were closed. The 60 MB upload takes under a second at the measured
75–85 MB/s, so the two XMB steps dominate an iteration, not the transfer.

Fetch the tail of the trace without downloading the whole file:

```python
import ftplib, io
f = ftplib.FTP(timeout=30)
f.connect("192.168.0.3", 2121)
f.login("anonymous", "anonymous")
size = f.size("/data/ps4-kit/boot-trace.log")
buf = io.BytesIO()
f.retrbinary("RETR /data/ps4-kit/boot-trace.log", buf.write, rest=max(0, size - 40000))
f.quit()
for line in buf.getvalue().decode("utf-8", "replace").splitlines()[-30:]:
    print(line)
```

The first port line proves the split is active:

```
[    0.105] port: video driver dummy, data root /data/stk
```

If the data root is missing or wrong, the game starts and fails later; check this line before
debugging anything else.

## What the executable costs

A statically linked port is a single ~59 MB eboot, so ~60 MB per iteration is the floor for this
split as built; there are no smaller code files to replace. Only data separates into files, which
is what `/data/stk/data` does. Two reductions remain, neither required:

- `strip --strip-unneeded` on the linked ELF took it from 65,790,128 to 56,626,728 bytes (−9.1 MB,
  tested); the packaged eboot would shrink too, but the resulting package size has not been
  re-measured.
- Rotating `TITLE_ID` per build lets each build install as a new app, removing the delete step.
  `make-pkg.sh` requires four letters and five digits, and `pkg.sh` currently hardcodes
  `STKR00001`, so this needs a counter threaded through packaging. Each build still writes 60 MB,
  stays on the XMB until deleted, and gets its own save data because saves are keyed by title ID.

## What does not work (do not retry)

| Attempt | Observed result |
|---|---|
| raise `APP_VER` and reinstall (00.02 over 00.01) | "package already installed" |
| overwrite `/user/app/<TITLE_ID>/app.pkg` over FTP | app launches and reports "the data was corrupted" |
| edit `app.json` to match the new image | `packageDigest` is not a file hash; nothing to recompute |
| install from `/data/pkg` without deleting the old entry | refused for the same content ID |
| delete only `app.pkg` and reinstall | the installed entry still owns the content ID; delete the app |

The only recovery from a hand-swapped image is uninstall + install of a valid package. Keep
`out/app.pkg.backup` (the original 1,160,773,632-byte image, SHA-256
`070E9BE8FF14872BB5C01E0819050839932D7115CA4E7E6F2244AF7B9C1C868E`) for diagnosis, not as a
supported swap path.

## Operational notes

- FTP: port **2121**, user `anonymous`, any password, root `/`; `MLSD`, `SIZE` and `MKD` work.
  Large files move at ~75–85 MB/s, thousands of small files at ~3–4 MiB/s, so the first asset
  mirror takes minutes.
- `/data` survives uninstall and install, not a factory reset or "Initialize PS4". Keep the asset
  checkout and `out/app.pkg.backup` on the PC.
- Rate-limit retry logging. A failing call in a loop produced a 29 MB `boot-trace.log` in 55
  seconds; `boot-trace-prev.log` is rotated, but an unbounded loop still fills `/data`.
- Give any uploader a socket timeout and a wall-clock cap. Without one, a stalled data connection
  hangs forever and looks like a broken script.
- Do not move the data root by changing a compile definition: that invalidates every object and
  turns a ~30 s incremental build into a full rebuild. A constructor `setenv` does the same job.

## Checklist

1. Assets mirrored, and `upload-data.py --dry-run` reports zero files to send.
2. `deploy.sh` produced a ~60 MB `out/*.pkg`, not a ~1.16 GB one.
3. Old app deleted; new package installed from `/data/pkg`; installed `app.pkg` size equals the
   package size.
4. Trace starts with `video driver dummy, data root /data/stk`.
5. After each code change only steps 2–5 of the loop run; assets are not re-uploaded.

## Current STK status (2026-11-19)

The loop is verified end to end: `/user/app/STKR00001/app.pkg` is exactly 60,686,336 bytes and the
app launches, so the split and the content-ID workaround both work.

The build still black-screens, now for a different reason. The dummy driver is selected
(`SDL_InitSubSystem(0x21) -> 0`, trace line `video driver dummy, data root /data/stk`), but
`SDL_CreateWindow` returns NULL (`-> 0`). Irrlicht retries it about 292,000 times in 54 seconds,
exhausts the pthread attribute pool, and the process crashes in libkernel
(`rip 0x800009fdc`). The trace wrapper logs the NULL pointer but not `SDL_GetError()` after the
failed window creation, so the reason is not yet known; log that next. None of this affects the
iteration method, but each test still ends in a crash log rather than a frame.

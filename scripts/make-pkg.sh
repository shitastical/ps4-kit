#!/usr/bin/env bash
# Package a staged app directory into a PS4 fake-PKG.
#
#   make-pkg.sh <stage-dir> <TITLE_ID> <title> <version XX.YY> <content-label> <out-dir>
#
# <stage-dir> must contain eboot.bin and sce_sys/icon0.png; anything else in it (sce_module/, assets,
# ...) is packaged as-is. sce_module/ files already in the stage (e.g. a core's own libc.prx) win over
# the OpenOrbis sample copies. Prints the package's path as its last line.
#
# Environment:
#   SFO_STYLE   plain (default: the homebrew identity Vulkan/RADV runs under), retroarch (RetroArch
#               for PS4's launch parameters, for apps drawing with the system's Piglet), or keep (the
#               stage's own param.sfo, e.g. a PS2 Classic's; only its identity is replaced)
#   CONTENT_ID  overrides the IV0000-<TITLE_ID>_00-<label> homebrew form
#   NO_RIGHT_SPRX=1  leave out sce_sys/about/right.sprx
#   PKGTOOL     another PkgTool command line. By default LibOrbisPkg built with the PlayGo sizing fix
#               (scripts/build-pkgtool.sh) when it is there: the toolchain's stops past ~4 GB with
#               "Playgo Chunk hash file was not allocated enough space".
#   CREATE_GP4   current OpenOrbis create-gp4. Some bundled binaries advertise -path but only
#               implement -files; a current build handles recursive paths and spaces in names.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"
PSB="${PSB:-$HOME/ps4kit-build}"

STAGE="$(readlink -f "$1")"; TITLE_ID="$2"; TITLE="$3"; VERSION="$4"; LABEL="$5"; OUT="$(readlink -f "$6")"
TOOLS="$OPENORBIS/bin/linux"
CREATE_GP4="${CREATE_GP4:-$PSB/tools/create-gp4}"
[ -x "$CREATE_GP4" ] || CREATE_GP4="$TOOLS/create-gp4"
[ -x "$CREATE_GP4" ] || { echo "missing create-gp4 at $CREATE_GP4" >&2; exit 1; }
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

[[ "$TITLE_ID" =~ ^[A-Z]{4}[0-9]{5}$ ]] || { echo "TITLE_ID must look like ABCD12345" >&2; exit 1; }
[[ "$VERSION" =~ ^[0-9]{2}\.[0-9]{2}$ ]] || { echo "version must look like 01.00" >&2; exit 1; }
[ -f "$STAGE/eboot.bin" ] || { echo "missing $STAGE/eboot.bin" >&2; exit 1; }
[ -f "$STAGE/sce_sys/icon0.png" ] || { echo "missing $STAGE/sce_sys/icon0.png" >&2; exit 1; }

if [ -z "${PKGTOOL:-}" ]; then
    PKGTOOL="$TOOLS/PkgTool.Core"
    if [ -x "$PSB/tools/pkgtool-fixed/PkgTool.Core" ] && [ -x "$PSB/tools/dotnet/dotnet" ]; then
        export DOTNET_ROOT="$PSB/tools/dotnet"
        PKGTOOL="$PSB/tools/pkgtool-fixed/PkgTool.Core"
    fi
fi
# The toolchain's PkgTool stops past ~4 GB: say so before building rather than after (a package with a
# game's data in it, such as MP3's one-file package, is 8 GB).
if [ "$PKGTOOL" = "$TOOLS/PkgTool.Core" ]; then
    stage_mb=$(( $(du -sb "$STAGE" | cut -f1) / 1048576 ))
    if [ "$stage_mb" -gt 3500 ]; then
        echo "the stage holds $stage_mb MB: packages past ~4 GB need the PkgTool scripts/build-pkgtool.sh makes" >&2
        exit 1
    fi
fi
read -r -a PKGTOOL <<< "$PKGTOOL"
STYLE="${SFO_STYLE:-plain}"

mkdir -p "$STAGE/sce_module"
if [ "${NO_RIGHT_SPRX:-0}" != 1 ]; then
    mkdir -p "$STAGE/sce_sys/about"
    cp "$OPENORBIS/samples/piglet/sce_sys/about/right.sprx" "$STAGE/sce_sys/about/"
fi
for m in "$OPENORBIS/samples/piglet/sce_module/"*.prx; do
    [ -e "$STAGE/sce_module/$(basename "$m")" ] || cp "$m" "$STAGE/sce_module/"
done
# PkgTool's pkg_build crashes ("Sequence contains no elements") on empty folders.
find "$STAGE" -mindepth 1 -type d -empty -delete

LABEL="$(printf '%s' "$LABEL" | tr '[:lower:]' '[:upper:]' | tr -cd 'A-Z0-9')"
LABEL="$(printf '%-16s' "${LABEL:0:16}" | tr ' ' '0')"
CONTENT_ID="${CONTENT_ID:-IV0000-${TITLE_ID}_00-${LABEL}}"

SFO="$STAGE/sce_sys/param.sfo"
set_entry() { "${PKGTOOL[@]}" sfo_setentry "$SFO" "$1" --type "$2" --maxsize "$3" --value "$4" >/dev/null; }
case "$STYLE" in
keep)
    [ -f "$SFO" ] || { echo "SFO_STYLE=keep but no $SFO" >&2; exit 1; } ;;
retroarch)
    rm -f "$SFO"; "${PKGTOOL[@]}" sfo_new "$SFO"
    # RetroArch for PS4's launch parameters: with the OpenOrbis sample values the shell's Piglet
    # never returns an EGL display (verified by love-ps4).
    set_entry APP_TYPE Integer 4 0
    set_entry ATTRIBUTE Integer 4 0x20814016
    set_entry ATTRIBUTE2 Integer 4 0x6
    set_entry CATEGORY Utf8 4 gde
    set_entry FORMAT Utf8 4 obs
    set_entry SYSTEM_VER Integer 4 0x3fc ;;
plain)
    rm -f "$SFO"; "${PKGTOOL[@]}" sfo_new "$SFO"
    # Plain OpenOrbis/PSChrome values (known to launch with the default PAID).
    set_entry APP_TYPE Integer 4 1
    # 0x2: system dialogs (the keyboard) confirm with Cross. Without it they follow the console's
    # region default, which is Circle on some consoles (DolphinPS4 Lab's fix). SFO_ATTRIBUTE replaces
    # it, e.g. 0x800002 adds 0x800000, "supports NEO mode": a PS4 Pro runs the app at its own clocks
    # (CPU 2.13 GHz instead of 1.6) without Boost Mode. But NEO mode also hands the app the Pro's own
    # GPU (36 compute units), which the kit's RADV doesn't describe (it knows the PS4's Liverpool),
    # and the CPU gets no more than Boost Mode (PS4 Settings > System) gives an app without the flag:
    # DolphinPS4 froze in Mario Kart with it, so Dolphin and Crema ship without it and tell Pro owners
    # to turn Boost Mode on instead (Crema v03.44: sceKernelHasNeoMode + ps4_kit_cpu_mhz-style clock).
    set_entry ATTRIBUTE Integer 4 "${SFO_ATTRIBUTE:-0x2}"
    set_entry CATEGORY Utf8 4 gd
    set_entry SYSTEM_VER Integer 4 0 ;;
*)
    echo "SFO_STYLE must be plain, retroarch or keep" >&2; exit 1 ;;
esac
set_entry APP_VER Utf8 8 "$VERSION"
set_entry CONTENT_ID Utf8 48 "$CONTENT_ID"
set_entry DOWNLOAD_DATA_SIZE Integer 4 0
set_entry TITLE Utf8 128 "$TITLE"
set_entry TITLE_ID Utf8 12 "$TITLE_ID"
set_entry VERSION Utf8 8 "$VERSION"

rm -f "$STAGE/pkg.gp4"
( cd "$STAGE" && "$CREATE_GP4" -out pkg.gp4 -content-id="$CONTENT_ID" -path . ) >/dev/null
python3 "$HERE/gp4-fix-dirs.py" "$STAGE/pkg.gp4" >&2  # folders create-gp4 drops (same name twice)
mkdir -p "$OUT"
( cd "$STAGE" && "${PKGTOOL[@]}" pkg_build pkg.gp4 "$OUT" ) >/dev/null
rm -f "$STAGE/pkg.gp4"
echo "$OUT/$CONTENT_ID.pkg"

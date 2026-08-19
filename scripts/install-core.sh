#!/bin/bash
# Safely (re)install the RemoteJoy libretro core into RetroArch.
# Always deletes first so the new file gets a fresh inode: overwriting a dylib
# in place leaves macOS holding stale code-signed pages, which makes the kernel
# SIGKILL RetroArch with "Code Signature Invalid" the next time it dlopens it.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/libretro-remotejoy/remotejoy_libretro.dylib"
CORES="$HOME/Library/Application Support/RetroArch/cores"

[ -f "$SRC" ] || { echo "!! No build found. Run: cd libretro-remotejoy && make platform=osx"; exit 1; }

mkdir -p "$CORES"
xattr -c "$SRC" 2>/dev/null || true
codesign --force --sign - --timestamp=none "$SRC"
codesign -v --strict "$SRC"

rm -f "$CORES/remotejoy_libretro.dylib"          # fresh inode, never overwrite
cp "$SRC" "$CORES/remotejoy_libretro.dylib"
xattr -c "$CORES/remotejoy_libretro.dylib" 2>/dev/null || true
codesign -v --strict "$CORES/remotejoy_libretro.dylib"

echo "Core installed and signature verified:"
ls -l "$CORES/remotejoy_libretro.dylib"

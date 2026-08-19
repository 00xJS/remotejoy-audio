#!/bin/bash
# Launch RetroArch straight into the RemoteJoy core (no menu navigation needed)
# and log everything to ~/rj-log.txt for diagnosis.
CORE="$HOME/Library/Application Support/RetroArch/cores/remotejoy_libretro.dylib"
LOG="$HOME/rj-log.txt"

[ -f "$CORE" ] || { echo "!! Core not installed. Run ./install-core.sh first."; exit 1; }

rm -f "$LOG"
echo "Starting RemoteJoy core... (log: $LOG)"
echo "Keep the RetroArch window FOCUSED while it streams, or RetroArch pauses it."
echo "Let it run ~30 seconds, then quit RetroArch normally."
echo
"/Applications/RetroArch.app/Contents/MacOS/RetroArch" -L "$CORE" --verbose --log-file "$LOG"

echo
echo "=========== RemoteJoy / audio lines from this session ==========="
grep -aE 'Waiting for PSP|PSP connected|Link lost|\[audio\]|libusb|Failed' "$LOG" || echo "(none found)"

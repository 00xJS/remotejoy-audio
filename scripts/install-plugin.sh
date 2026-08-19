#!/bin/bash
# Install the RemoteJoy Audio plugin onto a PSP mounted in USB mode.
# Preserves any other plugins already listed in game.txt / vsh.txt / pops.txt.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$HERE/prebuilt/RemoteJoyLite.prx"
[ -f "$SRC" ] || { echo "!! $SRC not found"; exit 1; }

PSP=""
for v in /Volumes/*; do [ -d "$v/seplugins" ] && PSP="$v" && break; done
[ -z "$PSP" ] && for v in /Volumes/*; do [ -d "$v/PSP" ] && PSP="$v" && break; done
if [ -z "$PSP" ]; then
  echo "!! No PSP found. On the PSP: Settings -> USB Connection."
  echo "   (If it will not mount, hold R while powering on -> Recovery -> Plugins -> disable, then retry.)"
  exit 1
fi
echo "PSP volume: $PSP"

SEP="$PSP/seplugins"
mkdir -p "$SEP"
cp "$SRC" "$SEP/RemoteJoyLite.prx" || exit 1

python3 - "$SEP" <<'PYEOF'
import os, sys
sep = sys.argv[1]
for txt in ('game.txt', 'vsh.txt', 'pops.txt'):
    path  = os.path.join(sep, txt)
    lines = []
    if os.path.exists(path):
        for raw in open(path, errors='ignore').read().splitlines():
            s = raw.strip()
            if s and os.path.basename(s.split()[0]).lower() != 'remotejoylite.prx':
                lines.append(s)          # keep other plugins untouched
    lines.append('ms0:/seplugins/RemoteJoyLite.prx 1')
    open(path, 'w').write('\n'.join(lines) + '\n')
    print(f'  {txt}: enabled')
PYEOF

sync
echo
echo "Installed. Eject the PSP, exit USB mode, reboot it, then start a game."
echo "PSP Go users: change ms0: to ef0: in the seplugins txt files."

#!/bin/bash
# test/emu/boot.sh NAME CARD [EVENT...] - PsiBoost's start-up test: boots the
# emulated 5mx with the card already in (core/tools/emu/run.sh puts it in at
# 35 s, after start-up, so a recogniser on it would only be seen as a card
# change), opens Extras and the first program on the card at 60-62 s, plays
# the events (as run.sh's: "T KEY [HOLD]", "T tap X Y"), saves a screenshot
# every second to build/emu/NAME/, end.png, and the card as it ends
# (card.img), then prints PsiBoost.log from the card.
#   EMU_SECS=N      seconds after the last event (default 8)
#   EMU_LAUNCH=0    do not open Extras and the program
#   EMU_CTRL=1      hold Ctrl down from 2 s to 40 s (the recogniser's way out)
#   EMU_SERIAL=SOCK bridge the serial port (COMM::0) to a unix socket that
#                   something already listens on, at EMU_ATTACH s (default 40;
#                   switch the Remote link off before then with events, e.g.
#                   "36 22 30" "36.1 76" "37.5 14" "38.5 3"). With nothing
#                   attached the emulator's port never sends, and a write waits
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
# (the emulator: $PSION_EMU, else psionEmulators beside this checkout or one
# folder further up)
EMU=${PSION_EMU:-}
for d in "$REPO/../psionEmulators" "$REPO/../../psionEmulators"; do
	[ -n "$EMU" ] || { [ -d "$d" ] && EMU=$(cd "$d" && pwd); }
done
[ -n "$EMU" ] || { echo "boot.sh: set PSION_EMU to the psionEmulators checkout"; exit 1; }
ROM="$EMU/roms/Series5mx/5mx_v1.05(260)_eng/5mx_v1.05(260)_eng.bin"
N=${1:?usage: boot.sh NAME CARD [EVENT...]}; CARD=${2:?card image}; shift 2
OUT=$REPO/build/emu/$N
rm -rf "$OUT"; mkdir -p "$OUT"
A=(--tap-seq 60 670 262 --tap-seq 62 335 215)
[ "${EMU_LAUNCH:-1}" = 0 ] && A=()
[ "${EMU_CTRL:-0}" = 1 ] && A+=(--press-key 2 22 2300)
[ -n "${EMU_SERIAL:-}" ] && A+=(--serial-attach 2 "${EMU_ATTACH:-40}" --serial-bridge-socket "$EMU_SERIAL")
last=62
for e in "$@"; do
	set -- $e; t=${1%.*}; [ "$t" -gt "$last" ] && last=$t
	if [ "$2" = tap ]; then A+=(--tap-seq "$1" "$3" "$4")
	else A+=(--press-key "$1" "$2" "${3:-8}"); fi
done
POST=$((last + ${EMU_SECS:-8}))
timeout ${EMU_TIMEOUT:-1800} "$EMU/harness/run" "$ROM" --device 5mx --boot-seconds 0.2 --card-path "$CARD" \
	--post-attach-seconds "$POST" --quiet-logs --screenshot "$OUT/end.pgm" --save-card "$OUT/card.img" \
	"${A[@]}" --screenshot-every 1 "$OUT/s" > "$OUT/log" 2>&1 || true
grep -iE "panic|KERN-|WSERV|USER [0-9]|traps" "$OUT/log" | head -5 || true
python3 - "$OUT" <<'PY'
import sys, glob, os
from PIL import Image
out = sys.argv[1]
for f in sorted(glob.glob(out + '/*.pgm')):
    Image.open(f).convert('L').save(f[:-4] + '.png')
    os.remove(f)
PY
node --experimental-strip-types - "$OUT/card.img" "$EMU" <<'JS'
const [card, emu] = process.argv.slice(2);
const { readFileSync } = await import('node:fs');
const { findByPath, readFileBytes } = await import(emu + '/frontend/src/lib/fat16.ts');
const img = new Uint8Array(readFileSync(card));
for (const p of ['SYSTEM/APPS/PSIBOOST/PSIBOOST.LOG']) {
	const e = findByPath(img, p);
	console.log('---- ' + p + (e ? '' : ': not on the card'));
	if (e) process.stdout.write(Buffer.from(readFileBytes(img, e)).toString('latin1'));
}
JS
echo "screenshots: $OUT"

#!/bin/bash
# Sequencer timing test (host, Linux).
#   test/sequencer/run.sh            build, run every scenario, compare with golden/
#   test/sequencer/run.sh --capture  build, run every scenario, write golden/
# Output and build files go to $OUT (default /tmp/seqtime).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd); OUT=${OUT:-/tmp/seqtime}
SHIM=$ROOT/test/engine/shim
mkdir -p "$OUT/obj" "$OUT/timelines"

CFLAGS="-O1 -g -fno-fast-math -ffp-contract=off -fno-strict-aliasing -fcommon -w -DSTM32F765xx -DUSE_HAL_DRIVER -D__fp16=_Float16 -include $SHIM/host_cmsis.h"
INC="-I$HERE -I$SHIM -I$ROOT/Core/Inc -I$ROOT/Drivers/CMSIS/Include -I$ROOT/Drivers/CMSIS/Device/ST/STM32F7xx/Include -I$ROOT/Drivers/STM32F7xx_HAL_Driver/Inc -I$ROOT/Middlewares/JS/STM32_USB_Device_Library/Core/Inc -I$ROOT/Middlewares/JS/STM32_USB_Device_Library/Class/CDC/Inc -I$ROOT/USB_DEVICE_MIDI/Target -I$ROOT/USB_DEVICE_MIDI/App -I$ROOT/FATFS/App -I$ROOT/FATFS/Target -I$ROOT/Middlewares/Third_Party/FatFs/src"
for f in "$ROOT"/Core/Src/Soundengine/Sequencer/sequencer.c "$ROOT"/Core/Src/Soundengine/Sequencer/clockSync.c "$HERE"/main.c "$HERE"/stubs.c; do
	gcc -c $CFLAGS $INC "$f" -o "$OUT/obj/$(basename "$f" .c).o"
done
gcc -o "$OUT/seqtime" "$OUT"/obj/*.o -lm

capture=0; [ "${1:-}" = "--capture" ] && capture=1
fail=0
for sc in $("$OUT/seqtime" --list); do
	"$OUT/seqtime" "$sc" "$OUT/timelines/$sc.txt"
	#recording: every played hit must be recorded on the step it was played for
	if [[ $sc == record_* ]]; then
		hits=$(awk '$3=="HIT"{print $4}' "$OUT/timelines/$sc.txt" | sort -n | tr '\n' ' ')
		recs=$(awk '$3=="REC"{print $4}' "$OUT/timelines/$sc.txt" | sort -n | tr '\n' ' ')
		if [ "$hits" != "$recs" ]; then echo "WRONG STEPS: $sc played $hits recorded $recs"; fail=1; fi
	fi
	if [ $capture = 1 ]; then
		cp "$OUT/timelines/$sc.txt" "$HERE/golden/$sc.txt"
	elif ! cmp -s "$OUT/timelines/$sc.txt" "$HERE/golden/$sc.txt"; then
		echo "DIFFERS: $sc"; diff "$HERE/golden/$sc.txt" "$OUT/timelines/$sc.txt" | head -8 || true; fail=1
	fi
done
if [ $capture = 1 ]; then echo "golden timelines written: $(ls "$HERE/golden" | wc -l)"; exit 0; fi
[ $fail = 0 ] && echo "sequencer timing: all scenarios match the golden timelines"
exit $fail

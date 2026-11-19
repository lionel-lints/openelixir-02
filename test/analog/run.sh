#!/bin/bash
# Analog engine port test (host, Linux: the firmware headers use ELF sections).
# Renders every instrument with the original AnalogModels source (original/) and
# with the port in Core/: the samples must be identical. Also checks that every
# instrument sounds and stays finite.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd); OUT=${OUT:-/tmp/analog-test}
SHIM=$ROOT/test/engine/shim
mkdir -p "$OUT"
CFLAGS="-std=gnu11 -O1 -w -fno-fast-math -ffp-contract=off -fsanitize=undefined -fno-sanitize-recover=all -DSTM32F765xx -DUSE_HAL_DRIVER -D__fp16=_Float16 -include $SHIM/host_cmsis.h"
INC="-I$SHIM -I$ROOT/Core/Inc -I$ROOT/Drivers/CMSIS/Include -I$ROOT/Drivers/CMSIS/Device/ST/STM32F7xx/Include -I$ROOT/Drivers/STM32F7xx_HAL_Driver/Inc"
gcc $CFLAGS -DORIGINAL -I"$HERE/original" $INC "$HERE/test_analog.c" "$HERE/original/AnalogModels.c" -lm -o "$OUT/original"
gcc $CFLAGS $INC "$HERE/test_analog.c" "$ROOT/Core/Src/Soundengine/DSPAudio/AnalogModels.c" -lm -o "$OUT/port"
"$OUT/original" > "$OUT/original.raw"
"$OUT/port" > "$OUT/port.raw"
if cmp -s "$OUT/original.raw" "$OUT/port.raw"; then
	echo "port: bit-identical to AnalogModels ($(stat -c %s "$OUT/port.raw") bytes)"
else
	echo "FAIL: the port renders differently from AnalogModels"; cmp "$OUT/original.raw" "$OUT/port.raw" || true; exit 1
fi
python3 "$HERE/check.py" "$OUT/port.raw"

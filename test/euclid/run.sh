#!/bin/bash
# Euclidean generator test (host, Linux: the firmware headers use ELF sections).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd); OUT=${OUT:-/tmp/euclid-test}
SHIM=$ROOT/test/engine/shim
mkdir -p "$OUT"
#-fsanitize=undefined: undefined behaviour (like __CLZ(0)) stops the test
gcc -std=gnu11 -O1 -w -fsanitize=undefined -fno-sanitize-recover=all -DSTM32F765xx -DUSE_HAL_DRIVER \
	-D__fp16=_Float16 -include "$SHIM/host_cmsis.h" -I"$SHIM" -I"$ROOT/Core/Inc" -I"$ROOT/Drivers/CMSIS/Include" \
	-I"$ROOT/Drivers/CMSIS/Device/ST/STM32F7xx/Include" -I"$ROOT/Drivers/STM32F7xx_HAL_Driver/Inc" \
	"$HERE/test_euclid.c" -o "$OUT/test_euclid"
"$OUT/test_euclid"

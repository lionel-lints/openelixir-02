#!/bin/bash
# Host test of the settings defaults for older files (any C compiler: macOS or Linux).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd); OUT=${OUT:-/tmp/settings-test}
mkdir -p "$OUT"
SHIM=$ROOT/test/engine/shim
#Parameters.h includes the HAL: compiled like test/engine, with its host stand-ins for the ARM parts
${CC:-cc} -std=gnu11 -Wall -Werror -w -DSTM32F765xx -DUSE_HAL_DRIVER -D__fp16=_Float16 -include "$SHIM/host_cmsis.h" \
	-I"$SHIM" -I"$ROOT/Core/Inc" -I"$ROOT/Drivers/CMSIS/Include" -I"$ROOT/Drivers/CMSIS/Device/ST/STM32F7xx/Include" \
	-I"$ROOT/Drivers/STM32F7xx_HAL_Driver/Inc" "$HERE/test_global_defaults.c" -o "$OUT/test_global_defaults"
"$OUT/test_global_defaults"

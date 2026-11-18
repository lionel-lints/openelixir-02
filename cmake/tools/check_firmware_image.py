#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check that a firmware build can't touch the bootloader, before its update image is made.

    check_firmware_image.py --objdump <objdump> --main-c Core/Src/main.c [--remove <stale .img>] <elf>

The bootloader occupies the first flash sector (0x08000000-0x07FFF) and installs the firmware from
0x08008000 on. The firmware must never write the bootloader. Fails (exit 1) if
  - a section is stored in flash below 0x08008000, or the vector table isn't at 0x08008000
    (where the bootloader jumps),
  - flash erase/program/option-byte code is linked in (only the default FLASH_IRQHandler may be),
  - UPDATE_BOOTLOADER in main.c isn't 0 (that build rewrites the bootloader from the SD card).
On failure, --remove deletes the given update image, so an image of an earlier build can't be
mistaken for this one.
"""
import argparse
import os
import re
import subprocess
import sys

FLASH_START = 0x08000000
FLASH_END = 0x08100000              # 1 MB
APP_START = 0x08008000              # everything below is the bootloader
VECTOR_SECTION = ".isr_vector"
ALLOWED_FLASH_SYMBOLS = {"FLASH_IRQHandler"}
FLASH_SYMBOL = re.compile(r"^(HAL_FLASH|HAL_FLASHEx|FLASH_|FLASHEx_)")


def sections(objdump_h):
    """(name, size, vma, lma, flags) of every section in `objdump -h` output."""
    found = []
    lines = objdump_h.splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+\S+", line)
        if m:
            flags = lines[i + 1].strip() if i + 1 < len(lines) else ""
            found.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16), flags))
    return found


def symbols(objdump_t):
    """Names of the symbols in `objdump -t` output."""
    names = set()
    for line in objdump_t.splitlines():
        parts = line.split()
        if len(parts) >= 2 and re.match(r"^[0-9a-f]{8}$", parts[0]):
            names.add(parts[-1])
    return names


def check(objdump_h, objdump_t, main_c):
    """List of problems (empty = safe)."""
    problems = []
    vector_lma = None
    for name, size, vma, lma, flags in sections(objdump_h):
        if size == 0 or "LOAD" not in flags:
            continue
        if name == VECTOR_SECTION:
            vector_lma = lma
        if FLASH_START <= lma < FLASH_END and lma < APP_START:
            problems.append(f"section {name} is stored at 0x{lma:08X}, inside the bootloader sector")
    if vector_lma != APP_START:
        where = "missing" if vector_lma is None else f"at 0x{vector_lma:08X}"
        problems.append(f"{VECTOR_SECTION} is {where}; the bootloader starts the firmware at 0x{APP_START:08X}")

    flash_code = sorted(s for s in symbols(objdump_t) if FLASH_SYMBOL.match(s) and s not in ALLOWED_FLASH_SYMBOLS)
    if flash_code:
        problems.append("flash erase/program code is linked in: " + ", ".join(flash_code))

    m = re.search(r"^\s*#define\s+UPDATE_BOOTLOADER\s+(\S+)", main_c, re.M)
    if not m:
        problems.append("UPDATE_BOOTLOADER is not defined in main.c")
    elif m.group(1) != "0":
        problems.append(f"UPDATE_BOOTLOADER is {m.group(1)}: this build would rewrite the bootloader")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--main-c", required=True)
    parser.add_argument("--remove", help="update image to delete if the check fails")
    parser.add_argument("elf")
    args = parser.parse_args()

    run = lambda flag: subprocess.run([args.objdump, flag, args.elf], check=True, capture_output=True, text=True).stdout
    with open(args.main_c, encoding="utf-8", errors="replace") as f:
        problems = check(run("-h"), run("-t"), f.read())
    if problems:
        if args.remove and os.path.exists(args.remove):
            os.remove(args.remove)
        for p in problems:
            print(f"{args.elf}: UNSAFE: {p}", file=sys.stderr)
        print("No update image was made: this firmware could damage the bootloader.", file=sys.stderr)
        sys.exit(1)
    print("Firmware image check: bootloader untouched (sections from 0x08008000, no flash write code)")


if __name__ == "__main__":
    main()

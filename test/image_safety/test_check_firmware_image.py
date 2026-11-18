#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Tests for cmake/tools/check_firmware_image.py: every kind of unsafe firmware must be refused.

    python3 test/image_safety/test_check_firmware_image.py
"""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "..", "cmake", "tools", "check_firmware_image.py")
sys.path.insert(0, os.path.dirname(TOOL))
import check_firmware_image as c  # noqa: E402

FLAGS_LOAD = "CONTENTS, ALLOC, LOAD, READONLY, CODE"


def objdump_h(*sections):
    """`objdump -h` text for (name, vma, lma) sections, laid out like arm-none-eabi-objdump."""
    lines = ["", "x.elf:     file format elf32-littlearm", "", "Sections:",
             "Idx Name          Size      VMA       LMA       File off  Algn"]
    for i, (name, vma, lma) in enumerate(sections):
        lines.append(f"  {i} {name:<13} 00000100  {vma:08x}  {lma:08x}  00010000  2**2")
        lines.append(f"                  {FLAGS_LOAD}")
    return "\n".join(lines)


def objdump_t(*names):
    return "\n".join(f"08010000 g     F .text\t00000010 {n}" for n in names)


SAFE_H = objdump_h((".isr_vector", 0x08008000, 0x08008000), (".text", 0x08008200, 0x08008200),
                   (".itcm_text", 0x00000000, 0x080501f8), (".dtcm", 0x20000000, 0x0804716c))
SAFE_T = objdump_t("main", "seq_tick", "FLASH_IRQHandler")
SAFE_MAIN = "#define UPDATE_BOOTLOADER 0\n"


class Check(unittest.TestCase):
    def test_safe_firmware_passes(self):
        self.assertEqual(c.check(SAFE_H, SAFE_T, SAFE_MAIN), [])

    def test_section_in_bootloader_sector(self):
        h = objdump_h((".isr_vector", 0x08008000, 0x08008000), (".text", 0x08004000, 0x08004000))
        self.assertTrue(any("bootloader sector" in p for p in c.check(h, SAFE_T, SAFE_MAIN)))

    def test_ram_section_initial_values_in_bootloader_sector(self):
        h = objdump_h((".isr_vector", 0x08008000, 0x08008000), (".itcm_text", 0x00000000, 0x08007f00))
        self.assertTrue(any(".itcm_text" in p for p in c.check(h, SAFE_T, SAFE_MAIN)))

    def test_vector_table_moved(self):
        h = objdump_h((".isr_vector", 0x08010000, 0x08010000))
        self.assertTrue(any(".isr_vector" in p for p in c.check(h, SAFE_T, SAFE_MAIN)))

    def test_vector_table_missing(self):
        h = objdump_h((".text", 0x08008200, 0x08008200))
        self.assertTrue(any("missing" in p for p in c.check(h, SAFE_T, SAFE_MAIN)))

    def test_flash_write_code_linked(self):
        for sym in ("HAL_FLASHEx_Erase", "HAL_FLASH_Program", "HAL_FLASH_OB_Unlock", "FLASH_Erase_Sector"):
            with self.subTest(sym=sym):
                problems = c.check(SAFE_H, objdump_t("main", sym), SAFE_MAIN)
                self.assertTrue(any(sym in p for p in problems))

    def test_update_bootloader_enabled(self):
        self.assertTrue(any("UPDATE_BOOTLOADER is 1" in p
                            for p in c.check(SAFE_H, SAFE_T, "#define UPDATE_BOOTLOADER 1\n")))

    def test_update_bootloader_not_defined(self):
        self.assertTrue(any("not defined" in p for p in c.check(SAFE_H, SAFE_T, "")))


class CommandLine(unittest.TestCase):
    """Runs the tool on a real ELF when the ARM toolchain is available."""

    def setUp(self):
        self.objdump = os.environ.get("OBJDUMP", "arm-none-eabi-objdump")
        self.elf = os.environ.get("FIRMWARE_ELF", os.path.join(HERE, "..", "..", "build", "release", "OpenElixir-02.elf"))
        try:
            subprocess.run([self.objdump, "--version"], capture_output=True, check=True)
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("no ARM objdump")
        if not os.path.exists(self.elf):
            self.skipTest("no firmware build")

    def run_tool(self, main_c_text, stale_image):
        with tempfile.TemporaryDirectory() as d:
            main_c = os.path.join(d, "main.c")
            with open(main_c, "w") as f:
                f.write(main_c_text)
            img = os.path.join(d, "LXR_test.img")
            if stale_image:
                with open(img, "w") as f:
                    f.write("stale")
            r = subprocess.run([sys.executable, TOOL, "--objdump", self.objdump, "--main-c", main_c,
                                "--remove", img, self.elf], capture_output=True, text=True)
            return r.returncode, os.path.exists(img)

    def test_real_firmware_passes(self):
        self.assertEqual(self.run_tool(SAFE_MAIN, stale_image=False)[0], 0)

    def test_failure_removes_stale_image(self):
        code, img_left = self.run_tool("#define UPDATE_BOOTLOADER 1\n", stale_image=True)
        self.assertEqual(code, 1)
        self.assertFalse(img_left)


if __name__ == "__main__":
    unittest.main(verbosity=2)

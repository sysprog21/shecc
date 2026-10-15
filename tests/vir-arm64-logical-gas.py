#!/usr/bin/env python3
"""Compare the direct ARM64 encoder unit's words with GNU assembler."""
import pathlib
import platform
import struct
import subprocess
import sys
import tempfile

prefix = "" if platform.machine().lower() in ("aarch64", "arm64") else "aarch64-linux-gnu-"
# The same masks, in the same order, as masks() in vir-arm64-select-direct.c.
PATTERNS = {
    False: [0x8, 0xff00, 0x80000001, 0xfffffff8, 0x0f0f0f0f, 0xaaaaaaaa, 0x7ffffffe],
    True: [0x8, 0xff00, 0x8000000000000001, 0xfffffffffffffff8,
           0x5555555555555555, 0xffff0000ffff0000, 0x7ffffffffffffffe,
           0x00000000fffffff0],
}
instructions = []
for wide in (False, True):
    reg = "x" if wide else "w"
    masks = [(1 << ones) - 1 for ones in range(1, 64 if wide else 32)]
    for mask in masks + PATTERNS[wide]:
        for opcode in ("and", "orr", "eor"):
            instructions.append(f"{opcode} {reg}2, {reg}0, #{mask:#x}")
with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    source, obj, binary = (root / name for name in ("test.s", "test.o", "test.bin"))
    source.write_text(".text\n" + "\n".join(instructions) + "\n")
    subprocess.run([prefix + "as", str(source), "-o", str(obj)], check=True)
    subprocess.run([prefix + "objcopy", "-O", "binary", "--only-section=.text",
                    str(obj), str(binary)], check=True)
    expected = list(struct.unpack("<" + "I" * len(instructions), binary.read_bytes()))
    actual = [int(word, 16) for word in subprocess.check_output([sys.argv[1]], text=True).splitlines()]
    assert actual == expected
print(f"ARM64 logical immediates: {len(expected)} GNU assembler encodings passed")

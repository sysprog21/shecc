#!/usr/bin/env python3
"""Compare the direct ARM64 encoder unit's words with GNU assembler."""
import pathlib
import platform
import struct
import subprocess
import sys
import tempfile

prefix = "" if platform.machine().lower() in ("aarch64", "arm64") else "aarch64-linux-gnu-"
instructions = []
for wide in (False, True):
    reg = "x" if wide else "w"
    for opcode in ("and", "orr", "eor"):
        for ones in range(1, 64 if wide else 32):
            instructions.append(f"{opcode} {reg}2, {reg}0, #{(1 << ones) - 1}")
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

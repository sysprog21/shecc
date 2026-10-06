#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cc=${CC:-cc}
"$cc" -O1 -I"$ROOT/out" "$ROOT/tests/arm64-register-memory-unit.c" -o "$TMP/unit"
"$TMP/unit" "$TMP/actual.bin"
if command -v aarch64-linux-gnu-as > /dev/null; then
    AS=aarch64-linux-gnu-as
    OBJCOPY=aarch64-linux-gnu-objcopy
elif [[ $(uname -m) == aarch64 ]]; then
    AS=as
    OBJCOPY=objcopy
else
    echo 'ARM64 register memory: unit assertions passed; assembler unavailable'
    exit 0
fi
python3 - "$TMP/reference.s" << 'PY'
import sys
with open(sys.argv[1], "w") as output:
    output.write(".text\n")
    for width in range(4):
        for read in range(2):
            for unsigned in range(2):
                for scaled in range(2):
                    for word in range(2):
                        op = (("ldrsb", "ldrsh", "ldrsw", "ldr")[width]
                              if read and not unsigned else
                              ("ldrb", "ldrh", "ldr", "ldr")[width] if read else
                              ("strb", "strh", "str", "str")[width])
                        target = ("x" if width == 3 or read and not unsigned else "w") + "3"
                        index = "w27, uxtw" if word else "x28"
                        shift = width if scaled else 0
                        if shift:
                            index += (" #" if word else ", lsl #") + str(shift)
                        output.write(f"{op} {target}, [x0, {index}]\n")
PY
"$AS" "$TMP/reference.s" -o "$TMP/reference.o"
"$OBJCOPY" -O binary -j .text "$TMP/reference.o" "$TMP/reference.bin"
cmp "$TMP/actual.bin" "$TMP/reference.bin"
echo 'ARM64 register memory: assertions and 64 assembler encodings passed'

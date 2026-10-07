#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-out/shecc}
task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT
cat > "$task_dir/dirty.S" << 'ASM'
.text
.globl dirty_false, dirty_true, dirty_signed_byte, dirty_unsigned_byte
.globl dirty_signed_short, dirty_unsigned_short
.type dirty_false, @function
dirty_false: mov $0x100, %eax; ret
.type dirty_true, @function
dirty_true: mov $0x101, %eax; ret
.type dirty_signed_byte, @function
dirty_signed_byte: mov $0x123456ff, %eax; ret
.type dirty_unsigned_byte, @function
dirty_unsigned_byte: mov $0x123456ff, %eax; ret
.type dirty_signed_short, @function
dirty_signed_short: mov $0x1234fffe, %eax; ret
.type dirty_unsigned_short, @function
dirty_unsigned_short: mov $0x1234fffe, %eax; ret
.section .note.GNU-stack,"",@progbits
ASM
"${CC:-cc}" -shared -fPIC "$task_dir/dirty.S" -o "$task_dir/dirty.so"
"$compiler" --dynlink -o "$task_dir/test" tests/vir-call-return.c
chmod +x "$task_dir/test"
LD_PRELOAD="$task_dir/dirty.so" "$task_dir/test"
echo 'VIR dirty narrow call return test passed'

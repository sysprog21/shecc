#!/usr/bin/env bash
set -euo pipefail

task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT
cc=${CC:-cc}
read -r -a runner <<< "${TARGET_EXEC:-}"
read -r -a compiler_flags <<< "${SHECC_CFLAGS:-}"

"$cc" -O0 -g -std=c99 -fwrapv -Isrc tests/vir-lower.c -o "$task_dir/unit"
"$task_dir/unit"

# Invoke the shared lowerer with independent graphs after the regular compiler
# has parsed the smoke program. The generated amalgamation stays outside src.
sed 's/    peephole();/    vir_lower_test_hook(); ph2_compute_liveness(); peephole();/' src/main.c > "$task_dir/main.c"
"$cc" -O0 -g -std=c99 -fwrapv -DSHECC_VIR_LOWER_TEST \
    "-DVIR_LOWER_TEST_MAIN=\"$task_dir/main.c\"" -iquote src -Isrc \
    tests/vir-lower.c -o "$task_dir/compiler"
"$task_dir/compiler" "${compiler_flags[@]}" -o "$task_dir/smoke.elf" tests/vir-lower-smoke.c
chmod +x "$task_dir/smoke.elf"
"${runner[@]}" "$task_dir/smoke.elf"
echo 'VIR machine executable smoke passed'

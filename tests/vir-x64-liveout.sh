#!/usr/bin/env bash
set -euo pipefail
task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT
cc=${CC:-cc}

# Build a test compiler that replaces two parsed placeholders with independent
# VIR/PH2 graphs. --no-folds is private to this harness, not a compiler option.
sed 's/    peephole();/    liveout_test_hook(); ph2_compute_liveness(); peephole();/' src/main.c > "$task_dir/main.c"
# Disable lookahead only in this temporary test compiler.
sed '/^void emit_ph2_ir(ph2_ir_t \*ph2_ir)$/,/^{/ { /^{$/a\
    if (liveout_no_folds) { folds_off = true; emit_next_ir = NULL; }
}' src/x64-codegen.c > "$task_dir/codegen.c"
[[ $(grep -Fc 'if (liveout_no_folds)' "$task_dir/codegen.c") == 1 ]]
"$cc" -O0 -g -std=c99 -fwrapv \
    "-DVIR_LIVEOUT_TEST_MAIN=\"$task_dir/main.c\"" -iquote "$task_dir" -iquote src -Isrc \
    tests/vir-x64-liveout.c -o "$task_dir/compiler"
cat > "$task_dir/source.c" << 'SOURCE'
int probe_cmp(int a, int b) { return -1; }
int probe_and(int a, int b) { return -1; }
int main(int argc, char **argv)
{
    if (argc > 2) return probe_and(5, 3) != 1;
    return probe_cmp(5, 5) != 1;
}
SOURCE
for mode in default no-folds; do
    flags=()
    [[ $mode == default ]] || flags+=(--no-folds)
    "$task_dir/compiler" "${flags[@]}" --no-libc -o "$task_dir/$mode.elf" "$task_dir/source.c"
    chmod +x "$task_dir/$mode.elf"
    "$task_dir/$mode.elf" cmp
    "$task_dir/$mode.elf" and two
done
compiler=${1:-out/shecc}
for opt in 0 2; do
    opt_flags=()
    [ "$opt" -eq 2 ] || opt_flags=(--no-opt)
    "$compiler" --no-libc "${opt_flags[@]}" -o "$task_dir/shift.elf" tests/vir-x64-shift-counts.c
    chmod +x "$task_dir/shift.elf"
    "$task_dir/shift.elf"
done
echo 'x64 live branch-result and shift-count executable tests passed'

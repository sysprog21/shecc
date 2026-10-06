#!/usr/bin/env bash
set -euo pipefail
# The compiler command, an emulator prefix included, one word per argument.
compiler=("$@")
[ "${#compiler[@]}" -gt 0 ] || compiler=(out/shecc)
task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT
read -r -a runner <<< "${TARGET_EXEC:-}"
for fixture in shift-width adjusted-array operand-signedness global-scratch call-return-internal volatile-member void-callback-array bool-compound file-seek pointer-offset typed-arithmetic constant-relations global-value-reader machine-live machine-affinity pair-scratch subtract-alias typedef-alias-builder machine-transfers loop-update if-join call-result-pins entry-parameters comparison-inversion global-slot-void global-cast-metadata global-cast-scope global-wide-string-operands global-dead-operands typedef-callback-rows typedef-nested-row-sizes typedef-pointer-row-elements volatile-pointer-row-elements \
    pointer-scale-width pointer-scale-rows x64-and-loop global-conditional-compound \
    arm64-select-runtime arm64-indexed-widths; do
    source="tests/vir-$fixture.c"
    for level in 0 1 2; do
        "${compiler[@]}" "--vir-opt=$level" -o "$task_dir/test" "$source"
        chmod +x "$task_dir/test"
        "${runner[@]}" "$task_dir/test"
    done
done
if "${compiler[@]}" -o "$task_dir/test" tests/vir-typedef-direct-float.c > "$task_dir/diagnostic" 2>&1; then
    echo 'Floating direct-function typedef accepted' >&2
    exit 1
fi
grep -q 'Floating point types are not yet supported' "$task_dir/diagnostic"
printf 'typedef int (*bad[2]);\nint main(void){return 0;}\n' > "$task_dir/grouped-array.c"
if "${compiler[@]}" -o "$task_dir/test" "$task_dir/grouped-array.c" > "$task_dir/diagnostic" 2>&1; then
    echo 'Unsupported grouped array typedef accepted' >&2
    exit 1
fi
grep -q 'Typedef parenthesized declarator must be a function pointer' "$task_dir/diagnostic"
bash tests/vir-global-slot-initializers.sh "${compiler[@]}"
bash tests/vir-global-conditional-compound.sh "${compiler[@]}"
bash tests/vir-strength.sh "${compiler[@]}"
bash tests/vir-qualifiers.sh "${compiler[@]}"
echo 'Native VIR regression programs passed at O0, O1, and O2'

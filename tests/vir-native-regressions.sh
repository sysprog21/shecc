#!/usr/bin/env bash
set -euo pipefail
# The compiler command, an emulator prefix included, one word per argument.
compiler=("$@")
[ "${#compiler[@]}" -gt 0 ] || compiler=(out/shecc)
task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT
read -r -a runner <<< "${TARGET_EXEC:-}"
for fixture in shift-width adjusted-array operand-signedness global-scratch call-return-internal volatile-member void-callback-array bool-compound file-seek pointer-offset typed-arithmetic machine-live machine-affinity pair-scratch subtract-alias; do
    source="tests/vir-$fixture.c"
    for level in 0 1 2; do
        "${compiler[@]}" "--vir-opt=$level" -o "$task_dir/test" "$source"
        chmod +x "$task_dir/test"
        "${runner[@]}" "$task_dir/test"
    done
done
bash tests/vir-strength.sh "${compiler[@]}"
bash tests/vir-qualifiers.sh "${compiler[@]}"
echo 'Native VIR regression programs passed at O0, O1, and O2'

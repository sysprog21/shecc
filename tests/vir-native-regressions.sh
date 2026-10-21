#!/usr/bin/env bash
set -euo pipefail
# The compiler command, an emulator prefix included, one word per argument.
compiler=("$@")
[ "${#compiler[@]}" -gt 0 ] || compiler=(out/shecc)
task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT
read -r -a runner <<< "${TARGET_EXEC:-}"
# The list arrives on fd 3 so that a fixture reading stdin cannot consume it.
while read -r -u 3 source; do
    case "$source" in '' | '#'*) continue ;; esac
    for opt_flag in --no-opt ''; do
        "${compiler[@]}" ${opt_flag:+"$opt_flag"} -o "$task_dir/test" "$source"
        chmod +x "$task_dir/test"
        "${runner[@]}" "$task_dir/test"
    done
done 3< tests/vir-runtime-workloads.txt
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
bash tests/vir-bitwise-normalization.sh "${compiler[@]}"
bash tests/vir-essential.sh "${compiler[@]}"
bash tests/vir-qualifiers.sh "${compiler[@]}"
echo 'Native VIR regression programs passed with and without optimization'

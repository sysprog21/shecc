#!/usr/bin/env bash
set -euo pipefail
# The compiler command, an emulator prefix included, one word per argument.
compiler=("$@")
[ "${#compiler[@]}" -gt 0 ] || compiler=(out/shecc)
task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT
read -r -a runner <<< "${TARGET_EXEC:-}"

check_count()
{
    local graph=$1 pattern=$2 expected=$3 label=$4 actual
    actual=$(awk -v pattern="$pattern" '$0 ~ pattern { count++ } END { print count+0 }' <<< "$graph")
    if [[ $actual != "$expected" ]]; then
        printf '%s: expected %s, got %s\n%s\n' "$label" "$expected" "$actual" "$graph" >&2
        return 1
    fi
}

for level in 0 1 2; do
    for fixture in vir-const-typedef-pointer-slot-write vir-const-pointer-row-write vir-const-direct-pointer-row-write vir-const-callback-row-write vir-const-local-getter vir-const-abstract-return-pointer vir-const-hidden-return-alias vir-const-callback-pointer-return-alias vir-const-callback-return-pointee vir-const-callback-return-pointer vir-const-grouped-pointer-alias vir-const-typedef-realias vir-const-slot-realias; do
        if "${compiler[@]}" "--vir-opt=$level" -o "$task_dir/test" "tests/$fixture.c" > "$task_dir/diagnostic" 2>&1; then
            echo "$fixture accepted assignment" >&2
            exit 1
        fi
        grep -q 'assignment of read-only' "$task_dir/diagnostic"
    done
    for fixture in vir-hidden-const-return-compatible vir-pointer-comparison vir-grouped-pointer-alias; do
        "${compiler[@]}" "--vir-opt=$level" -o "$task_dir/test" "tests/$fixture.c"
        "${runner[@]}" "$task_dir/test"
    done
    while read -r fixture function pointers integers stores; do
        "${compiler[@]}" --dump-vir "--vir-opt=$level" -o "$task_dir/test" "tests/$fixture.c" 2> "$task_dir/graph"
        "${runner[@]}" "$task_dir/test"
        graph=$(awk -v name="$function" '
            /^function / { active = ($2 == name); next }
            active { print }
        ' "$task_dir/graph")
        [[ -n $graph ]]
        check_count "$graph" ' = volatile[.]load[.]ptr ' "$pointers" "$fixture O$level pointer loads"
        check_count "$graph" ' = volatile[.]load[.]i(8|16|32|64) ' "$integers" "$fixture O$level scalar loads"
        check_count "$graph" '^[[:space:]]+volatile[.]store ' "$stores" "$fixture O$level stores"
    done << 'CASES'
vir-volatile-pointer-row-elements touch_row 2 0 0
vir-typedef-alias-builder touch_alias_qualifiers 3 0 1
vir-volatile-arithmetic touch 0 3 0
vir-volatile-storage check 3 2 0
vir-volatile-local main 3 2 4
vir-volatile-callback touch 6 0 0
vir-volatile-pointer-member touch 1 1 0
vir-volatile-callback-return-pointer touch 2 0 0
vir-callback-pointer-return-alias touch 2 0 2
vir-volatile-local-getter touch 1 0 1
vir-callback-qualifier-compatibility main 0 0 0
vir-abstract-return-qualifiers touch 1 3 0
vir-callback-return-metadata touch 0 2 0
vir-callback-return-metadata constant_callback_assignments 0 0 0
vir-callback-return-metadata volatile_callback_assignments 0 0 0
vir-volatile-callback-pointer-return typedef_object 2 0 2
vir-volatile-callback-pointer-return typedef_slot 2 0 0
vir-volatile-callback-pointer-return typedef_outer_slot 1 0 0
vir-volatile-callback-pointer-return direct_slot 2 0 1
vir-volatile-comma-alias main 4 0 4
vir-volatile-member discard_pointees 2 0 0
vir-direct-arm64-i64-memory main 0 1 2
CASES
done
echo 'Native pointer and volatile qualifier checks passed at O0, O1, and O2'

#!/usr/bin/env bash
set -euo pipefail

source tests/vir-direct-common.sh

stage=$1
compiler=out/shecc
if test "$stage" -eq 2; then
    compiler=out/shecc-stage2.elf
fi
read -r -a runner <<< "${TARGET_EXEC:-}"
compiler_runner=()
if test "$stage" -eq 2; then
    compiler_runner=("${runner[@]}")
fi
run_compiler()
{
    "${compiler_runner[@]}" "$compiler" "$@"
}
dynamic_runner=("${runner[@]}")
has_sysroot=0
for runner_arg in "${runner[@]}"; do
    if test "$runner_arg" = -L; then
        has_sysroot=1
        break
    fi
done
case "${runner[0]:-}" in
    *qemu-aarch64)
        if test "$has_sysroot" -eq 0 && test -d /usr/aarch64-linux-gnu; then
            dynamic_runner+=(-L /usr/aarch64-linux-gnu)
        fi
        ;;
esac
# Fixtures read argv[0], so keep its "/tmp/" prefix independent of TMPDIR.
work=$(mktemp -d /tmp/vir-direct-arm64.XXXXXX)
trap 'rm -rf "$work"' EXIT

run_static()
{
    "${runner[@]}" "$@"
}

run_dynamic()
{
    "${dynamic_runner[@]}" "$@"
}

check_external_pointer_join()
{
    local name=$1 source=$2 status

    run_compiler --no-libc --dynlink --dump-vir \
        --stats --dump-vir \
        -o "$work/$name" "$source" 2> "$work/$name.stats"
    run_compiler --no-libc --dynlink --dump-vir \
        --stats --dump-vir \
        -o "$work/$name-repeat" "$source" 2> "$work/$name-repeat.stats"
    cmp "$work/$name" "$work/$name-repeat"
    cmp "$work/$name.stats" "$work/$name-repeat.stats"
    if test "$name" = external-pointer-loop-join; then
        vir_extract_function main \
            "$work/$name.stats" \
            | awk '/^b[0-9]+\(/ && !/^b0\(/ && gsub(/:ptr/, "&") >= 2 { header = 1 } / = ptradd / { ptradd = 1 } END { exit !(header && ptradd) }'
    else
        grep -Eq '^b[0-9]+\(.*:ptr.*\):$' "$work/$name.stats"
    fi
    if run_dynamic "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 0
    if run_dynamic "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq 0
}

check_direct()
{
    local name=$1 expected=$2 extra_expected=${3:--} status
    local source="tests/vir-direct-arm64-$name.c"

    if test "$name" = direct; then
        source=tests/vir-direct-arm64.c
    elif test "$name" = branch-initialized-root; then
        source=tests/vir-direct-branch-initialized-root.c
    elif test "$name" = narrow-global || [[ "$name" = global-ptradd* ]]; then
        source="tests/vir-direct-$name.c"
    fi
    vir_build_deterministic run_compiler "$source" \
        "$work/$name" "$work/$name-repeat" "$work/$name.stats" \
        --dump-vir
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if test "$extra_expected" != -; then
        if run_static "$work/$name" extra; then status=0; else status=$?; fi
        test "$status" -eq "$extra_expected"
    fi
}

check_args()
{
    local mode=$1 name=$2 expected=$3 status output stats
    shift 3
    output="$work/$name"
    stats="$work/$name.stats"
    if test "$mode" = dyn; then
        output="$work/$name-dyn"
        stats="$work/$name-dyn.stats"
        run_compiler --dynlink --dump-vir --stats -o "$output" \
            "tests/vir-direct-arm64-$name.c" 2> "$stats"
        if run_dynamic "$output" "$@"; then status=0; else status=$?; fi
    else
        vir_build_deterministic run_compiler \
            "tests/vir-direct-arm64-$name.c" "$output" \
            "$work/$name-repeat" "$stats" --dump-vir
        if run_static "$output" "$@"; then status=0; else status=$?; fi
    fi
    test "$status" -eq "$expected"
}

check_direct_args()
{
    check_args static "$@"
}

check_dynlink_args()
{
    check_args dyn "$@"
}

check_dynlink()
{
    local name=$1 expected=$2 extra_expected=$3 status
    local source="tests/vir-direct-arm64-$name.c"

    if test "$name" = narrow-global || [[ "$name" = global-ptradd* ]]; then
        source="tests/vir-direct-$name.c"
    fi

    run_compiler --dynlink --dump-vir --stats -o "$work/$name-dyn" \
        "$source" 2> "$work/$name-dyn.stats"
    if run_dynamic "$work/$name-dyn"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if run_dynamic "$work/$name-dyn" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"
}

check_direct_helper_leaf()
{
    local name=helper-leaf expected=2 extra_expected=3 status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    run_compiler --dump-vir -o "$work/$name-repeat" \
        "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name" "$work/$name-repeat"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if run_static "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"

    run_compiler --dynlink --dump-vir --stats \
        -o "$work/$name-dyn" "tests/vir-direct-arm64-$name.c" \
        2> "$work/$name-dyn.stats"
    run_compiler --dynlink --dump-vir \
        -o "$work/$name-dyn-repeat" "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name-dyn" "$work/$name-dyn-repeat"
    if run_dynamic "$work/$name-dyn"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if run_dynamic "$work/$name-dyn" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"
}

check_direct_helper_two()
{
    local name=helper-two expected=19 extra_expected=37 status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    run_compiler --dump-vir -o "$work/$name-repeat" \
        "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name" "$work/$name-repeat"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if run_static "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"

    run_compiler --dynlink --dump-vir --stats \
        -o "$work/$name-dyn" "tests/vir-direct-arm64-$name.c" \
        2> "$work/$name-dyn.stats"
    if run_dynamic "$work/$name-dyn"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if run_dynamic "$work/$name-dyn" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"
}

check_direct_helper_three()
{
    local name=helper-three expected=33 extra_expected=57 status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    run_compiler --dump-vir -o "$work/$name-repeat" \
        "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name" "$work/$name-repeat"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if run_static "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"

    run_compiler --dynlink --dump-vir --stats \
        -o "$work/$name-dyn" "tests/vir-direct-arm64-$name.c" \
        2> "$work/$name-dyn.stats"
    run_compiler --dynlink --dump-vir \
        -o "$work/$name-dyn-repeat" "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name-dyn" "$work/$name-dyn-repeat"
    if run_dynamic "$work/$name-dyn"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if run_dynamic "$work/$name-dyn" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"
}

check_direct_helper_i64()
{
    local name=helper-i64 expected=2 extra_expected=3 status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    run_compiler --dump-vir -o "$work/$name-repeat" \
        "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name" "$work/$name-repeat"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if run_static "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"

    run_compiler --dynlink --dump-vir --stats \
        -o "$work/$name-dyn" "tests/vir-direct-arm64-$name.c" \
        2> "$work/$name-dyn.stats"
    run_compiler --dynlink --dump-vir \
        -o "$work/$name-dyn-repeat" "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name-dyn" "$work/$name-dyn-repeat"
    if run_dynamic "$work/$name-dyn"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if run_dynamic "$work/$name-dyn" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"
}

check_direct_helper_i64_two()
{
    local name=helper-i64-two expected=3 extra_expected=4 status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    run_compiler --dump-vir -o "$work/$name-repeat" \
        "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name" "$work/$name-repeat"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if run_static "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"

    run_compiler --dynlink --dump-vir --stats \
        -o "$work/$name-dyn" "tests/vir-direct-arm64-$name.c" \
        2> "$work/$name-dyn.stats"
    run_compiler --dynlink --dump-vir \
        -o "$work/$name-dyn-repeat" "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name-dyn" "$work/$name-dyn-repeat"
    if run_dynamic "$work/$name-dyn"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if run_dynamic "$work/$name-dyn" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"
}

check_helper_i64_three_fallback()
{
    local name=helper-i64-three-fallback status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 4
}

check_direct_helper_branch()
{
    local name=helper-branch expected=23 extra_expected=47 status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    run_compiler --dump-vir -o "$work/$name-repeat" \
        "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name" "$work/$name-repeat"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if run_static "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"

    run_compiler --dynlink --dump-vir --stats \
        -o "$work/$name-dyn" "tests/vir-direct-arm64-$name.c" \
        2> "$work/$name-dyn.stats"
    run_compiler --dynlink --dump-vir \
        -o "$work/$name-dyn-repeat" "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name-dyn" "$work/$name-dyn-repeat"
    if run_dynamic "$work/$name-dyn"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if run_dynamic "$work/$name-dyn" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"

    run_compiler --dump-vir -o "$work/$name-dump" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.vir"
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^b[0-9]' | grep -qx 4
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  branch ' | grep -qx 1
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  return ' | grep -qx 2
}

check_helper_branch_merge_fallback()
{
    local name=helper-branch-merge-fallback status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 23
    if run_static "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq 47
}

check_helper_branch_unsigned_fallback()
{
    local name=helper-branch-unsigned-fallback status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 70
}

check_direct_helper_merge()
{
    local name=helper-merge expected=23 extra_expected=47 status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    run_compiler --dump-vir -o "$work/$name-repeat" \
        "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name" "$work/$name-repeat"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if run_static "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"

    run_compiler --dynlink --dump-vir --stats \
        -o "$work/$name-dyn" "tests/vir-direct-arm64-$name.c" \
        2> "$work/$name-dyn.stats"
    run_compiler --dynlink --dump-vir \
        -o "$work/$name-dyn-repeat" "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name-dyn" "$work/$name-dyn-repeat"
    if run_dynamic "$work/$name-dyn"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if run_dynamic "$work/$name-dyn" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"

    run_compiler --dump-vir -o "$work/$name-dump" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.vir"
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^b[0-9]' | grep -qx 3
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  branch ' | grep -qx 1
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  return ' | grep -qx 2
}

check_direct_helper_loop()
{
    local name=helper-loop expected=1 extra_expected=2 status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    run_compiler --dump-vir -o "$work/$name-repeat" \
        "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name" "$work/$name-repeat"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if run_static "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"

    run_compiler --dynlink --dump-vir --stats \
        -o "$work/$name-dyn" "tests/vir-direct-arm64-$name.c" \
        2> "$work/$name-dyn.stats"
    run_compiler --dynlink --dump-vir \
        -o "$work/$name-dyn-repeat" "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name-dyn" "$work/$name-dyn-repeat"
    if run_dynamic "$work/$name-dyn"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if run_dynamic "$work/$name-dyn" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"

    run_compiler --dump-vir -o "$work/$name-dump" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.vir"
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^b[0-9]' | grep -qx 4
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  branch ' | grep -qx 1
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  jump ' | grep -qx 2
    test "$(vir_extract_callee main \
        "$work/$name.vir" | grep -Ec '^b[0-9]+\([^)]*i32[^)]*\):$')" -eq 2
}

check_helper_loop_fallback()
{
    local name=helper-loop-fallback status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 1
    if run_static "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq 2
}

check_helper_global_loop_direct()
{
    local name=helper-global-loop status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    run_compiler --dump-vir -o "$work/$name-repeat" \
        "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name" "$work/$name-repeat"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 1

    run_compiler --dump-vir -o "$work/$name-dump" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.vir"
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^b[0-9]' | grep -qx 4
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  branch ' | grep -qx 1
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  jump ' | grep -qx 2
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  store ' | grep -qx 1
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  %d[0-9]* = load.i32 ' | grep -qx 1
}

check_helper_global_loop_bounded_direct()
{
    local suffix=$1 levels=$2
    local name=helper-global-loop-$suffix status blocks branches jumps

    blocks=$((1 + 3 * levels))
    branches=$levels
    jumps=$((2 * levels))
    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    run_compiler --dump-vir -o "$work/$name-repeat" \
        "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name" "$work/$name-repeat"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 1

    run_compiler --dump-vir -o "$work/$name-dump" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.vir"
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^b[0-9]' | grep -qx "$blocks"
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  branch ' | grep -qx "$branches"
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  jump ' | grep -qx "$jumps"
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  store ' | grep -qx 1
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  %d[0-9]* = load.i32 ' | grep -qx 1
}

check_helper_global_loop_nested_direct()
{
    check_helper_global_loop_bounded_direct nested 2
}

check_helper_global_loop_deep_direct()
{
    check_helper_global_loop_bounded_direct deep 3
}

check_helper_global_loop_four_direct()
{
    check_helper_global_loop_bounded_direct four 4
}

check_helper_global_loop_five_direct()
{
    check_helper_global_loop_bounded_direct five 5
}

check_helper_global_loop_six_direct()
{
    check_helper_global_loop_bounded_direct six 6
}

check_helper_global_loop_seven_direct()
{
    check_helper_global_loop_bounded_direct seven 7
}

check_helper_global_loop_eight_direct()
{
    check_helper_global_loop_bounded_direct eight 8
}

check_helper_global_loop_nine_direct()
{
    check_helper_global_loop_bounded_direct nine 9
}

check_helper_global_loop_ten_direct()
{
    check_helper_global_loop_bounded_direct ten 10
}

check_helper_global_loop_fallback()
{
    local name=helper-global-loop-fallback status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 1
    # The general helper check admits what the shape matchers did not.
}

check_helper_global_effect_direct()
{
    local name=$1 memory_prefix=$2 status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    run_compiler --dump-vir -o "$work/$name-repeat" \
        "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name" "$work/$name-repeat"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 1
    if run_static "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq 2

    run_compiler --dump-vir -o "$work/$name-dump" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.vir"
    vir_extract_callee main \
        "$work/$name.vir" | grep -c "^  ${memory_prefix}store " | grep -qx 1
    vir_extract_callee main \
        "$work/$name.vir" | grep -c "^  %d[0-9]* = ${memory_prefix}load.i32 " | grep -qx 1
}

check_helper_global_pair_direct()
{
    local name=helper-global-pair status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    run_compiler --dump-vir -o "$work/$name-repeat" \
        "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name" "$work/$name-repeat"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 1
    if run_static "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq 2

    run_compiler --dump-vir -o "$work/$name-dump" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.vir"
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  store ' | grep -qx 2
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  %d[0-9]* = load.i32 ' | grep -qx 1
}

check_helper_global_load_store_direct()
{
    local name=helper-global-load-store status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    run_compiler --dump-vir -o "$work/$name-repeat" \
        "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name" "$work/$name-repeat"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 17

    run_compiler --dump-vir -o "$work/$name-dump" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.vir"
    vir_extract_callee main \
        "$work/$name.vir" | awk '
            / = load\.i32 / { load = NR }
            / store / { store = NR }
            END { exit !(load && store && load < store) }'
}

check_helper_global_load_store_fallback()
{
    local name=helper-global-load-store-external-fallback status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 17
    # The general helper check admits what the shape matchers did not.
}

check_helper_global_branch_direct()
{
    local name=${1:-helper-global-branch} status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    run_compiler --dump-vir -o "$work/$name-repeat" \
        "tests/vir-direct-arm64-$name.c"
    cmp "$work/$name" "$work/$name-repeat"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 7

    run_compiler --dump-vir -o "$work/$name-dump" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.vir"
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  branch ' | grep -qx 1
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  store ' | grep -qx 2
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  %d[0-9]* = load.i32 ' | grep -qx 2
}

check_helper_global_branch_fallback()
{
    local name=helper-global-branch-fallback status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 1
    if run_static "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq 2
    # The general helper check admits what the shape matchers did not.

    run_compiler --dump-vir -o "$work/$name-dump" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.vir"
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  volatile.store ' | grep -qx 2
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  %d[0-9]* = volatile.load.i32 ' | grep -qx 2
}

check_helper_global_volatile_fallback()
{
    local name=helper-global-volatile-fallback status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 1
    if run_static "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq 2
    # The general helper check admits what the shape matchers did not.

    run_compiler --dump-vir -o "$work/$name-dump" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.vir"
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  volatile.store ' | grep -qx 2
    vir_extract_callee main \
        "$work/$name.vir" | grep -c '^  %d[0-9]* = volatile.load.i32 ' | grep -qx 1
}

check_helper_i64_mixed_fallback()
{
    local name=helper-i64-mixed-fallback status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 1
}

check_helper_i64_local_mixed_fallback()
{
    local name=helper-i64-local-mixed-fallback status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 1
}

check_helper_four_fallback()
{
    local name=helper-four-fallback status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 50
}

check_legacy_helper_fallback()
{
    local name=helper-fallback status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 2
}

check_address_taken_helper_fallback()
{
    local name=helper-address-fallback status

    run_compiler --dump-vir --stats -o "$work/$name" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name.stats"
    if run_static "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq 2
}

check_fallback()
{
    local name=$1 expected=$2 status
    local -a args=("${@:4}")
    local source="tests/vir-direct-arm64-$name.c"

    if test "$name" = branch-volatile-root; then
        source=tests/vir-direct-branch-volatile-root.c
    fi

    run_compiler --dump-vir --stats -o "$work/$name" \
        "$source" 2> "$work/$name.stats"
    if run_static "$work/$name" "${args[@]}"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
}

check_fallback_o0()
{
    local name=$1 expected=$2 status

    run_compiler --vir-opt=0 --dump-vir --stats -o "$work/$name-o0" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name-o0.stats"
    if run_static "$work/$name-o0"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
}

check_direct direct 62
check_direct_helper_leaf
check_direct_helper_two
check_direct_helper_three
check_helper_four_fallback
check_direct_helper_i64
check_direct_helper_i64_two
check_helper_i64_three_fallback
check_direct_helper_branch
check_helper_branch_merge_fallback
check_helper_branch_unsigned_fallback
check_direct_helper_merge
check_direct_helper_loop
check_helper_loop_fallback
check_helper_global_loop_direct
check_helper_global_loop_nested_direct
check_helper_global_loop_deep_direct
check_helper_global_loop_four_direct
check_helper_global_loop_five_direct
check_helper_global_loop_six_direct
check_helper_global_loop_seven_direct
check_helper_global_loop_eight_direct
check_helper_global_loop_nine_direct
check_helper_global_loop_ten_direct
check_helper_global_loop_fallback
check_helper_global_effect_direct helper-global-volatile volatile. 2
check_helper_global_effect_direct helper-global '' 0
check_helper_global_pair_direct
check_helper_global_load_store_direct
check_helper_global_load_store_fallback
check_helper_global_branch_direct
check_helper_global_branch_direct helper-global-branch-external
check_helper_global_branch_fallback
check_helper_global_volatile_fallback
check_helper_i64_mixed_fallback
check_helper_i64_local_mixed_fallback
check_legacy_helper_fallback
check_address_taken_helper_fallback
check_direct argc 8 15
check_direct argc-return 1 2
check_direct div 47 54
check_dynlink div 47 54
check_direct call-pointer 42
check_direct argv 4 5
check_direct argv-char-load 47
check_direct argv-char-offset 116
check_direct argv-char-offset-direct 116
check_direct pointer-join 47 101
check_fallback pointer-join-duplicate 47
check_direct_args argv-index-char-load 101 extra
check_direct_args argv-index-char-offset-one 120 extra
check_direct cfg 30 29
check_direct join 30 29
check_direct join-pair 89 88
check_direct ult 31 31
check_direct compare 13 10
check_direct logical 1 0
check_direct store 8 9
check_direct narrow 121 123
check_direct ptradd 43 44
check_direct ptradd-cfg 42 43
check_direct ptradd-dynamic 2 2

run_compiler --dump-vir -o "$work/ptradd-immediate" \
    tests/vir-direct-arm64-ptradd.c 2> "$work/ptradd-immediate.vir"
vir_extract_function main "$work/ptradd-immediate.vir" | awk '
    / = const.i(32|64) 4$/ { offsets[$1] = 1 }
    / = ptradd / {
        base = $4; sub(/,$/, "", base)
        if (!offsets[$5] || (root && base != root)) failed = 1
        root = base
        addresses[$1] = 1
        count++
    }
    /^  store / { address = $2; sub(/,$/, "", address); stored = addresses[address] }
    / = load.i32 / { loaded = addresses[$4] }
    END { exit failed || !count || !stored || !loaded }
'

check_direct global 15 17

run_compiler --dump-vir -o "$work/global-immediate" \
    tests/vir-direct-arm64-global.c 2> "$work/global-immediate.vir"
vir_extract_function main \
    "$work/global-immediate.vir" | awk '
        / = globaladdr / { roots[$4] = 1 }
        END { for (root in roots) count++; exit count != 2 }
    '

check_direct global-shift 6 7
run_compiler --dump-vir -o "$work/global-shift" \
    tests/vir-direct-arm64-global-shift.c 2> "$work/global-shift.vir"
vir_extract_function main \
    "$work/global-shift.vir" | awk '
        / = globaladdr / { roots[$4] = 1 }
        END { for (root in roots) count++; exit count != 1 }
    '

check_direct narrow-global 29
check_direct global-ptradd 8 9
check_direct global-ptradd-dynamic 8 9
check_direct global-ptradd-unsigned 8 9
check_direct global-pointer-fallback 13 14
check_direct global-pointer-narrow-signed 254 255
check_direct global-pointer-narrow-unsigned 6 7
check_direct global-pointer-wide 2 2
check_dynlink global-pointer-wide 2 2
check_direct global-pointer-wide-chain 3 3
check_dynlink global-pointer-wide-chain 3 3
check_direct local-pointer-chain 41 42
check_direct local-pointer-wide 4 4
check_direct local-pointer-deep 51 52
check_dynlink local-pointer-deep 51 52
check_direct branch-initialized-root 31 29
check_fallback branch-volatile-root 31 'root-address|volatile'
run_compiler --dump-vir --dump-vir --stats \
    -o "$work/branch-initialized-root-verified" \
    tests/vir-direct-branch-initialized-root.c \
    2> "$work/branch-initialized-root-verified.stats"
vir_extract_function main \
    "$work/branch-initialized-root-verified.stats" \
    > "$work/branch-initialized-root.dump"
test "$(grep -c ' = stackaddr ' "$work/branch-initialized-root.dump")" -eq 1
test "$(grep -c '^  store ' "$work/branch-initialized-root.dump")" -eq 2
grep -Eq '^b[0-9]+:$' "$work/branch-initialized-root.dump"
grep -Eq ' = load\.i32 ' "$work/branch-initialized-root.dump"
for level in 0 1 2; do
    run_compiler --vir-opt="$level" --dump-vir \
        --dump-vir --stats \
        -o "$work/branch-initialized-root-o$level" \
        tests/vir-direct-branch-initialized-root.c \
        2> "$work/branch-initialized-root-o$level.stats"
    run_compiler --vir-opt="$level" --dump-vir \
        --dump-vir --stats \
        -o "$work/branch-initialized-root-o$level-repeat" \
        tests/vir-direct-branch-initialized-root.c \
        2> "$work/branch-initialized-root-o$level-repeat.stats"
    cmp "$work/branch-initialized-root-o$level" \
        "$work/branch-initialized-root-o$level-repeat"
    cmp "$work/branch-initialized-root-o$level.stats" \
        "$work/branch-initialized-root-o$level-repeat.stats"
    if run_static "$work/branch-initialized-root-o$level"; then status=0; else status=$?; fi
    test "$status" -eq 31
    if run_static "$work/branch-initialized-root-o$level" extra; then status=0; else status=$?; fi
    test "$status" -eq 29
done

# Preserve explicit stack provenance through deep pointer chains.
check_direct local-pointer-overdepth 52 53
check_dynlink local-pointer-overdepth 52 53
check_direct local-pointer-level4 52 53
check_fallback local-pointer-volatile 52 root-address
check_fallback local-pointer-reassigned 52 root-object-late-write
check_fallback local-pointer-cyclic 1 root-object-late-write
check_fallback local-pointer-collapsed 52 direct-arm64-subset
check_fallback local-pointer-collapsed 53 direct-arm64-subset extra
run_compiler --dump-vir -o "$work/local-pointer-collapsed" \
    tests/vir-direct-arm64-local-pointer-collapsed.c \
    2> "$work/local-pointer-collapsed.vir"
vir_extract_function main \
    "$work/local-pointer-collapsed.vir" > "$work/local-pointer-collapsed.dump"
awk '
    / = stackaddr / { paths[$1] = $1 }
    / = load.ptr / { if (paths[$4]) paths[$1] = paths[$4] "/load" }
    /^  store / {
        address = $2; sub(/,$/, "", address)
        if (paths[address] ~ /load/) mutation = paths[address]
    }
    / = load.i32 / { if (mutation && paths[$4] == mutation) observed = 1 }
    END { exit !mutation || !observed }
' "$work/local-pointer-collapsed.dump"
run_compiler --dump-vir -o "$work/local-pointer-deep" \
    tests/vir-direct-arm64-local-pointer-deep.c 2> "$work/local-pointer-deep.vir"
vir_extract_function main \
    "$work/local-pointer-deep.vir" > "$work/local-pointer-deep.dump"
awk '
    / = stackaddr [0-9]+, 4, align 4$/ { scalar[$1] = 1 }
    / = stackaddr [0-9]+, 8, align 8$/ { pointer[$1] = 1 }
    /^  store / {
        address = $2; sub(/,$/, "", address)
        if (scalar[address]) initialized = 1
        if (pointer[address] && scalar[$3]) linked = 1
        if (loaded_pointer[address]) mutated = 1
    }
    / = load.ptr / { if (pointer[$4]) loaded_pointer[$1] = 1 }
    / = load.i32 / { if (loaded_pointer[$4]) observed = 1 }
    END { exit !initialized || !linked || !mutated || !observed }
' "$work/local-pointer-deep.dump"

check_dynlink global-pointer-unknown 7 7
check_direct volatile 8 8
check_dynlink cfg 30 29
check_dynlink argv 4 5
check_dynlink argv-char-load 47 47
check_dynlink argv-char-offset 116 116
check_dynlink argv-char-offset-direct 116 116
check_dynlink pointer-join 47 101
check_dynlink_args argv-index-char-load 101 extra
check_dynlink_args argv-index-char-offset-one 120 extra
check_dynlink join 30 29
check_dynlink join-pair 89 88
check_dynlink ult 31 31
check_dynlink compare 13 10
check_dynlink logical 1 0
check_dynlink store 8 9
check_dynlink narrow 121 123
check_dynlink ptradd 43 44
check_dynlink ptradd-cfg 42 43
check_dynlink ptradd-dynamic 2 2
check_dynlink global 15 17
check_dynlink narrow-global 29 29
check_dynlink global-ptradd 8 9
check_dynlink global-ptradd-dynamic 8 9
check_dynlink global-ptradd-unsigned 8 9
check_dynlink global-pointer-fallback 13 14
check_dynlink global-pointer-narrow-signed 254 255
check_dynlink global-pointer-narrow-unsigned 6 7
check_dynlink volatile 8 8
check_dynlink call-pointer 42 42
check_dynlink call-narrow 42 42
check_dynlink call-void 42 42
check_dynlink call-i64 120 120
check_dynlink call-i64-eight 42 42
check_direct i64-memory 0
check_direct i64-memory-high 0
check_direct i64-add 0
check_direct i64-sub 0
check_direct i64-bitwise 0
check_direct i64-unary 0
check_direct i64-shift 0
check_direct i64-compare 0
check_direct i64-cross 0
check_direct i64-const 0
check_direct i64-const-sparse 0
check_direct i64-const-movn 0
check_direct i64-const-tie 0
check_direct i64-cast 0
check_direct i64-immediate 0
check_direct i64-mul 0
check_direct pressure 0
check_direct call 0
check_dynlink call 0 0
check_direct call-bool 0
check_dynlink call-bool 0 0
check_direct call-bool-arg 42 7
check_dynlink call-bool-arg 42 7

# The i1 argument is explicitly zero-extended to a byte before its four-byte
# staging spill (UXTB X0, X0), and the check is tied to that caller sequence
# rather than any helper-side boolean normalization.
check_fallback call-bool-arg-fallback 0
check_direct call-bool-arg-stack 7 42
check_dynlink call-bool-arg-stack 7 42
check_direct call-bool-arg-stack-wide 7 42
check_dynlink call-bool-arg-stack-wide 7 42
check_direct call-i32 42
check_direct call-i32-eight 255
check_direct call-i32-argc 3 5
check_direct call-i32-branch 2 3
check_direct call-i32-pointer 42
check_direct call-i32-zero 42
check_direct call-narrow 42
check_direct call-void 42
check_direct call-i64 120
check_direct call-i64-eight 42
check_direct i64-div 0
run_compiler --dump-vir -o "$work/i64-memory-dump" \
    tests/vir-direct-arm64-i64-memory.c 2> "$work/i64-memory.vir"
vir_extract_function main \
    "$work/i64-memory.vir" \
    | awk '/^  volatile.store %d[0-9]+, %d[0-9]+$/ { event = event "S"; stores++ } /^  %d[0-9]+ = volatile.load.i64/ { event = event "L"; loads++ } /volatile.load.ptr/ { pointer_loads++ } END { exit stores != 2 || loads != 1 || pointer_loads || event != "SSL" }'
check_direct loop 21 12
check_dynlink loop 21 12
check_fallback shared-dag 0
check_fallback ptradd-large 0
check_fallback argv-deref 4 branches
check_direct argv-char-intervening 47
check_fallback argv-index 0 direct-arm64-subset
check_fallback argv-index-other 1 direct-arm64-subset extra extra
check_fallback argv-index-dynamic 0
check_fallback_o0 argv-index-unused 0
check_fallback argv-index-char-other 101 direct-arm64-subset extra extra
check_fallback argv-index-char-dynamic 101 direct-arm64-subset extra
check_fallback argv-index-char-intervening 101 direct-arm64-subset extra
check_fallback argv-index-char-offset-two 116 direct-arm64-subset extra
check_fallback argv-index-char-offset-other 120 direct-arm64-subset extra extra
check_fallback argv-index-char-offset-dynamic 120 direct-arm64-subset extra
check_fallback argv-index-char-offset-intervening 120 direct-arm64-subset extra
check_fallback argv-char-offset-two 109
check_fallback argv-char-offset-dynamic 116
check_direct argv-char-offset-intervening 116
check_direct argv-pointer-compare 1 1
check_direct argv-pointer-equal 0 0
check_direct argv-index-compare 0 1
check_direct argv-index-equal 1 0
check_dynlink argv-pointer-compare 1 1
check_dynlink argv-pointer-equal 0 0
check_dynlink argv-index-compare 0 1
check_dynlink argv-index-equal 1 0
check_fallback argv-wrong-first 4
check_fallback argv-wrong-type 47
check_fallback argv-const-char 4
check_fallback argv-const-pointer 4
check_fallback argv-signed-char 4
check_fallback argv-unsigned-char 4
check_fallback argv-volatile-char 4
check_fallback argv-volatile-pointer 4
check_direct global-pointer-unknown 7
check_fallback global-pointer-deep 7

run_compiler --dump-vir -o "$work/cfg-dump" tests/vir-direct-arm64-cfg.c \
    2> "$work/cfg.vir"
vir_extract_function main "$work/cfg.vir" \
    | grep -Eq '^  jump b[0-9]+\(\)$'

run_compiler --dump-vir -o "$work/argv-dump" \
    tests/vir-direct-arm64-argv.c \
    2> "$work/argv.vir"
vir_extract_function main "$work/argv.vir" \
    | grep -Eq '^b[0-9]+\(%d[0-9]+:i32, %d[0-9]+:ptr\):$'

run_compiler --dump-vir -o "$work/argv-char-load-dump" \
    tests/vir-direct-arm64-argv-char-load.c 2> "$work/argv-char-load.vir"
vir_extract_function main "$work/argv-char-load.vir" \
    | awk '$3 == "load.ptr" { ptr = $1; next } $3 == "load.i8" && $4 == ptr { byte = 1 } END { exit !byte }'
if vir_extract_function main "$work/argv-char-load.vir" \
    | grep -q 'ptradd'; then exit 1; fi

run_compiler --dump-vir \
    -o "$work/argv-char-offset-direct-dump" \
    tests/vir-direct-arm64-argv-char-offset-direct.c \
    2> "$work/argv-char-offset-direct.vir"
vir_extract_function main \
    "$work/argv-char-offset-direct.vir" \
    | awk '$3 == "load.ptr" { ptr = $1; next } $3 == "const.i64" && $4 == "1" { one = $1; next } $3 == "ptradd" { left = $4; sub(/,$/, "", left); if (left == ptr && $5 == one) add = $1; next } $3 == "load.i8" && $4 == add { byte = 1 } END { exit !byte }'

run_compiler --dump-vir \
    -o "$work/argv-char-offset-dump" tests/vir-direct-arm64-argv-char-offset.c \
    2> "$work/argv-char-offset.vir"
vir_extract_function main \
    "$work/argv-char-offset.vir" \
    | awk '$3 ~ /^load\./ { loads++ } $3 == "load.ptr" { ptr = $1; ptrs++; next } $3 == "const.i64" && $4 == "1" { one = $1; one_consts++; next } $3 == "ptradd" { left = $4; sub(/,$/, "", left); if (left == ptr && $5 == one) { add = $1; adds++ } next } $3 == "load.i8" && $4 == add { bytes++ } END { exit ptrs != 1 || one_consts != 1 || adds != 1 || bytes != 1 || loads != 2 }'

run_compiler --dump-vir \
    -o "$work/argv-pointer-compare-dump" \
    tests/vir-direct-arm64-argv-pointer-compare.c \
    2> "$work/argv-pointer-compare.vir"
vir_extract_function main \
    "$work/argv-pointer-compare.vir" \
    | awk '$3 == "load.ptr" { ptr = $1; next } $3 == "const.ptr" && $4 == "0" { null = $1; next } $3 == "eq.i32" { left = $4; sub(/,$/, "", left); if ((left == ptr && $5 == null) || (left == null && $5 == ptr)) equal = 1 } END { exit !equal }'

run_compiler --dump-vir \
    -o "$work/argv-index-compare-dump" \
    tests/vir-direct-arm64-argv-index-compare.c \
    2> "$work/argv-index-compare.vir"
vir_extract_function main \
    "$work/argv-index-compare.vir" \
    | awk '$3 == "const.i64" && $4 == "8" { eight = $1; next } $3 == "ptradd" { left = $4; sub(/,$/, "", left); if ($5 == eight) add = $1; next } $3 == "load.ptr" && $4 == add { ptr = $1; next } $3 == "const.ptr" && $4 == "0" { null = $1; next } $3 == "eq.i32" { left = $4; sub(/,$/, "", left); if ((left == ptr && $5 == null) || (left == null && $5 == ptr)) equal = 1 } END { exit !equal }'

run_compiler --dump-vir \
    -o "$work/argv-index-char-load-dump" \
    tests/vir-direct-arm64-argv-index-char-load.c \
    2> "$work/argv-index-char-load.vir"
vir_extract_function main \
    "$work/argv-index-char-load.vir" \
    | awk '$3 == "const.i64" && $4 == "8" { eight = $1; next } $3 == "ptradd" { left = $4; sub(/,$/, "", left); if ($5 == eight) add = $1; next } $3 == "load.ptr" && $4 == add { ptr = $1; next } $3 == "load.i8" && $4 == ptr { byte = 1 } END { exit !byte }'

run_compiler --dump-vir \
    -o "$work/argv-index-char-offset-one-dump" \
    tests/vir-direct-arm64-argv-index-char-offset-one.c \
    2> "$work/argv-index-char-offset-one.vir"
vir_extract_function main \
    "$work/argv-index-char-offset-one.vir" \
    | awk '$3 == "const.i64" && $4 == "8" { eight = $1; next } $3 == "ptradd" { left = $4; sub(/,$/, "", left); if ($5 == eight) ptr = $1; else if (left == loaded && $5 == one) byte_ptr = $1; next } $3 == "load.ptr" && $4 == ptr { loaded = $1; next } $3 == "const.i64" && $4 == "1" { one = $1; next } $3 == "load.i8" && $4 == byte_ptr { byte = 1 } END { exit !byte }'

run_compiler --dump-vir -o "$work/logical-dump" tests/vir-direct-arm64-logical.c \
    2> "$work/logical.vir"
vir_extract_function main "$work/logical.vir" \
    | grep -Eq '^  branch %d[0-9]+, b[0-9]+\(\), b[0-9]+\(\)$'

run_compiler --dump-vir -o "$work/store-dump" tests/vir-direct-arm64-store.c \
    2> "$work/store.vir"
vir_extract_function main "$work/store.vir" \
    | awk '/^  store / { stores++ } /^  %d[0-9]+ = load\./ { loads++ } END { exit stores || loads }'

run_compiler --dump-vir -o "$work/volatile-dump" tests/vir-direct-arm64-volatile.c \
    2> "$work/volatile.vir"
vir_extract_function main "$work/volatile.vir" \
    | awk '/^  volatile.store %d[0-9]+, %d[0-9]+$/ { event = event "S"; stores++ } /^  %d[0-9]+ = volatile.load\.(i8|i16|i32)/ { event = event "L"; loads++ } /volatile.load.ptr/ { pointer_loads++ } END { exit stores != 6 || loads != 3 || pointer_loads || event != "SSSSSLLSL" }'

run_compiler --dump-vir -o "$work/narrow-dump" tests/vir-direct-arm64-narrow.c \
    2> "$work/narrow.vir"
vir_extract_function main "$work/narrow.vir" \
    | grep -Eq '^  %d[0-9]+ = sext\.i8\.i32 %d[0-9]+$'

run_compiler --dump-vir -o "$work/ptradd-dynamic-dump" \
    tests/vir-direct-arm64-ptradd-dynamic.c 2> "$work/ptradd-dynamic.vir"
vir_extract_function main "$work/ptradd-dynamic.vir" \
    | awk '/^  %d[0-9]+ = sext\.i32\.i64 %d[0-9]+$/ { sext++ } /^  %d[0-9]+ = zext\.i32\.i64 %d[0-9]+$/ { zext++ } END { exit sext != 1 || zext != 1 }'

run_compiler --dump-vir -o "$work/global-dump" tests/vir-direct-arm64-global.c \
    2> "$work/global.vir"
vir_extract_function main "$work/global.vir" \
    | awk '/^  %d[0-9]+ = globaladdr @value, 4, align 4$/ { value++ } /^  %d[0-9]+ = globaladdr @first, 4, align 4$/ { first++ } END { exit value != 1 || first != 1 }'

run_compiler --dump-vir -o "$work/global-ptradd-dump" \
    tests/vir-direct-global-ptradd.c 2> "$work/global-ptradd.vir"
vir_extract_function main "$work/global-ptradd.vir" \
    | awk '/^  %d[0-9]+ = globaladdr @values, 8, align 4$/ { root++ } /^  %d[0-9]+ = ptradd %d[0-9]+, %d[0-9]+$/ { ptradd++ } END { exit root < 1 || ptradd != 2 }'

run_compiler --dump-vir -o "$work/global-ptradd-dynamic-dump" \
    tests/vir-direct-global-ptradd-dynamic.c \
    2> "$work/global-ptradd-dynamic.vir"
vir_extract_function main \
    "$work/global-ptradd-dynamic.vir" \
    | awk '/^  %d[0-9]+ = globaladdr @values, 8, align 4$/ { root++ } /^  %d[0-9]+ = sext\.i32\.i64 %d[0-9]+$/ { sext[$1] = 1 } /^  %d[0-9]+ = ptradd %d[0-9]+, %d[0-9]+$/ { if (!sext[$5]) bad = 1; ptradd++ } END { exit bad || root < 1 || ptradd != 2 }'

run_compiler --dump-vir -o "$work/global-ptradd-unsigned-dump" \
    tests/vir-direct-global-ptradd-unsigned.c \
    2> "$work/global-ptradd-unsigned.vir"
vir_extract_function main \
    "$work/global-ptradd-unsigned.vir" \
    | awk '/^  %d[0-9]+ = globaladdr @values, 8, align 4$/ { root++ } /^  %d[0-9]+ = zext\.i32\.i64 %d[0-9]+$/ { zext[$1] = 1 } /^  %d[0-9]+ = ptradd %d[0-9]+, %d[0-9]+$/ { if (!zext[$5]) bad = 1; ptradd++ } END { exit bad || root < 1 || ptradd != 2 }'
vir_extract_function main "$work/narrow.vir" \
    | grep -Eq '^  %d[0-9]+ = zext\.i16\.i32 %d[0-9]+$'

external_pointer_source=tests/vir-direct-external-pointer-origin.c
run_compiler --no-libc --dynlink --dump-vir --stats \
    -o "$work/external-pointer" "$external_pointer_source" \
    2> "$work/external-pointer.stats"
run_compiler --no-libc --dynlink --dump-vir \
    -o "$work/external-pointer-repeat" "$external_pointer_source"
cmp "$work/external-pointer" "$work/external-pointer-repeat"
if run_dynamic "$work/external-pointer"; then status=0; else status=$?; fi
test "$status" -eq 0
run_compiler --no-libc --dynlink -o "$work/external-pointer-legacy" \
    "$external_pointer_source"
if run_dynamic "$work/external-pointer-legacy"; then status=0; else status=$?; fi
test "$status" -eq 0

typed_external_pointer_source=tests/vir-direct-external-typed-pointer.c
run_compiler --no-libc --dynlink --dump-vir --stats --dump-vir \
    -o "$work/external-typed-pointer" "$typed_external_pointer_source" \
    2> "$work/external-typed-pointer.stats"
run_compiler --no-libc --dynlink --dump-vir \
    -o "$work/external-typed-pointer-repeat" \
    "$typed_external_pointer_source"
cmp "$work/external-typed-pointer" "$work/external-typed-pointer-repeat"
vir_extract_function main \
    "$work/external-typed-pointer.stats" \
    | grep -Eq '^  %d[0-9]+ = load\.i32 %d[0-9]+$'
vir_extract_function main \
    "$work/external-typed-pointer.stats" \
    | grep -Eq '^  store %d[0-9]+, %d[0-9]+$'
if run_dynamic "$work/external-typed-pointer"; then status=0; else status=$?; fi
test "$status" -eq 0
run_compiler --no-libc --dynlink -o "$work/external-typed-pointer-legacy" \
    "$typed_external_pointer_source"
if run_dynamic "$work/external-typed-pointer-legacy"; then status=0; else status=$?; fi
test "$status" -eq 0

typed_pointer_offset_source=tests/vir-direct-external-typed-pointer-offset.c
run_compiler --no-libc --dynlink --dump-vir --stats --dump-vir \
    -o "$work/external-typed-pointer-offset" \
    "$typed_pointer_offset_source" \
    2> "$work/external-typed-pointer-offset.stats"
vir_extract_function main \
    "$work/external-typed-pointer-offset.stats" \
    | grep -Eq ' = ptradd '
if run_dynamic "$work/external-typed-pointer-offset"; then status=0; else status=$?; fi
test "$status" -eq 0
if run_dynamic "$work/external-typed-pointer-offset" extra; then status=0; else status=$?; fi
test "$status" -eq 0

external_pointer_join_source=tests/vir-direct-arm64-external-pointer-join.c
check_external_pointer_join external-pointer-join \
    "$external_pointer_join_source" selected

external_pointer_join_offset_source=tests/vir-direct-arm64-external-pointer-join-offset.c
check_external_pointer_join external-pointer-join-offset \
    "$external_pointer_join_offset_source" selected

mixed_external_pointer_join_source=tests/vir-direct-arm64-external-pointer-mixed-join.c
check_external_pointer_join external-pointer-mixed-join \
    "$mixed_external_pointer_join_source" fallback

external_pointer_loop_join_source=tests/vir-direct-arm64-external-pointer-loop-join.c
check_external_pointer_join external-pointer-loop-join \
    "$external_pointer_loop_join_source" selected

mixed_external_pointer_loop_join_source=tests/vir-direct-arm64-external-pointer-mixed-loop-join.c
check_external_pointer_join external-pointer-mixed-loop-join \
    "$mixed_external_pointer_loop_join_source" fallback

external_pointer_bound_source=tests/vir-direct-arm64-external-pointer-bound.c
run_compiler --no-libc --dynlink --dump-vir \
    --stats --dump-vir \
    -o "$work/external-pointer-bound" "$external_pointer_bound_source" \
    2> "$work/external-pointer-bound.stats"
run_compiler --no-libc --dynlink --dump-vir \
    --stats --dump-vir \
    -o "$work/external-pointer-bound-repeat" "$external_pointer_bound_source" \
    2> "$work/external-pointer-bound-repeat.stats"
cmp "$work/external-pointer-bound" "$work/external-pointer-bound-repeat"
cmp "$work/external-pointer-bound.stats" \
    "$work/external-pointer-bound-repeat.stats"
test "$(vir_extract_function main \
    "$work/external-pointer-bound.stats" | grep -c ' = ptradd ')" -gt 256
if run_dynamic "$work/external-pointer-bound"; then status=0; else status=$?; fi
test "$status" -eq 0

mixed_pointer_offset_source=tests/vir-direct-arm64-external-pointer-offset-fallback.c
run_compiler --no-libc --dynlink --dump-vir --stats \
    -o "$work/external-pointer-offset-fallback" \
    "$mixed_pointer_offset_source" \
    2> "$work/external-pointer-offset-fallback.stats"
if run_dynamic "$work/external-pointer-offset-fallback"; then status=0; else status=$?; fi
test "$status" -eq 0
if run_dynamic "$work/external-pointer-offset-fallback" extra; then status=0; else status=$?; fi
test "$status" -eq 0

opaque_wide_source=tests/vir-direct-arm64-external-void-pointer-wide.c

# Preserve wide accesses through a cast from malloc's opaque void pointer.
run_compiler --no-libc --dynlink --dump-vir --stats \
    -o "$work/external-void-pointer-wide" "$opaque_wide_source" \
    2> "$work/external-void-pointer-wide.stats"
if run_dynamic "$work/external-void-pointer-wide"; then status=0; else status=$?; fi
test "$status" -eq 0

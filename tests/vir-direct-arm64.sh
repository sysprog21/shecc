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
# Keep call ABI and callee graph checks active when leaf inlining is enabled.
run_compiler()
{
    "${compiler_runner[@]}" "$compiler" --no-inline "$@"
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

# Run a binary through the static or dynamic runner and require its status.
run_status()
{
    local mode=$1 expected=$2
    shift 2
    vir_expect_status "$expected" "run_$mode" "$@"
}

# Most fixtures are tests/vir-direct-arm64-NAME.c; the shared ones drop the
# target from their name.
fixture_source()
{
    local name=$1

    if test "$name" = direct; then
        echo tests/vir-direct-arm64.c
    elif test "$name" = narrow-global || [[ "$name" = global-ptradd* ]]; then
        echo "tests/vir-direct-$name.c"
    else
        echo "tests/vir-direct-arm64-$name.c"
    fi
}

check_external_pointer_join()
{
    local name=$1 source=$2

    run_compiler --no-libc --dynlink --stats --dump-vir \
        -o "$work/$name" "$source" 2> "$work/$name.stats"
    run_compiler --no-libc --dynlink --stats --dump-vir \
        -o "$work/$name-repeat" "$source" 2> "$work/$name-repeat.stats"
    cmp "$work/$name" "$work/$name-repeat"
    cmp "$work/$name.stats" "$work/$name-repeat.stats"

    # The entry block always carries argv as a ptr parameter; the join must be a
    # later block.
    if test "$name" = external-pointer-loop-join; then
        vir_extract_function main \
            "$work/$name.stats" \
            | awk '/^b[0-9]+\(/ && !/^b0\(/ && gsub(/:ptr/, "&") >= 2 { header = 1 } / = ptradd / { ptradd = 1 } END { exit !(header && ptradd) }'
    else
        vir_extract_function main "$work/$name.stats" \
            | grep -Eq '^b[1-9][0-9]*\(.*:ptr.*\):$'
    fi
    run_status dynamic 0 "$work/$name"
    run_status dynamic 0 "$work/$name" extra
}

check_direct()
{
    local name=$1 expected=$2 extra_expected=${3:--}

    vir_build_deterministic run_compiler "$(fixture_source "$name")" \
        "$work/$name" "$work/$name-repeat" "$work/$name.stats"
    run_status static "$expected" "$work/$name"
    if test "$extra_expected" != -; then
        run_status static "$extra_expected" "$work/$name" extra
    fi
}

check_args()
{
    local mode=$1 name=$2 expected=$3 output
    shift 3
    output="$work/$name"
    if test "$mode" = dynamic; then
        output="$work/$name-dyn"
        run_compiler --dynlink --dump-vir -o "$output" \
            "tests/vir-direct-arm64-$name.c" 2> "$work/$name-dyn.vir"
        run_status dynamic "$expected" "$output" "$@"
    else
        vir_build_deterministic run_compiler \
            "tests/vir-direct-arm64-$name.c" "$output" \
            "$work/$name-repeat" "$work/$name.stats"
        run_status static "$expected" "$output" "$@"
    fi
}

check_dynlink()
{
    local name=$1 expected=$2 extra_expected=$3

    run_compiler --dynlink --dump-vir -o "$work/$name-dyn" \
        "$(fixture_source "$name")" 2> "$work/$name-dyn.vir"
    run_status dynamic "$expected" "$work/$name-dyn"
    run_status dynamic "$extra_expected" "$work/$name-dyn" extra
}

# Build a helper fixture with its VIR dump in $work/NAME.vir, and require a
# second build without the dump to produce the same binary.
build_helper()
{
    vir_build_deterministic run_compiler "tests/vir-direct-arm64-$1.c" \
        "$work/$1" "$work/$1-repeat" "$work/$1.vir"
}

# Require PATTERN (an ERE) to match EXPECTED lines of main's single callee.
callee_count()
{
    local name=$1 pattern=$2 expected=$3 body

    body=$(vir_extract_callee main "$work/$name.vir")
    test "$(printf '%s\n' "$body" | grep -cE -- "$pattern")" -eq "$expected"
}

# Static and dynamic deterministic builds, each run with and without an extra
# argument.
check_helper()
{
    local name=$1 expected=$2 extra_expected=$3

    build_helper "$name"
    run_status static "$expected" "$work/$name"
    run_status static "$extra_expected" "$work/$name" extra

    vir_build_deterministic run_compiler "tests/vir-direct-arm64-$name.c" \
        "$work/$name-dyn" "$work/$name-dyn-repeat" "$work/$name-dyn.vir" \
        --dynlink
    run_status dynamic "$expected" "$work/$name-dyn"
    run_status dynamic "$extra_expected" "$work/$name-dyn" extra
}

# A two-way branch whose arms both return, from a callee of BLOCKS blocks.
check_helper_branch()
{
    local name=$1 blocks=$2

    check_helper "$name" 23 47
    callee_count "$name" '^b[0-9]' "$blocks"
    callee_count "$name" '^  branch ' 1
    callee_count "$name" '^  return ' 2
}

check_helper_loop()
{
    check_helper helper-loop 1 2
    callee_count helper-loop '^b[0-9]' 4
    callee_count helper-loop '^  branch ' 1
    callee_count helper-loop '^  jump ' 2
    callee_count helper-loop '^b[0-9]+\([^)]*i32[^)]*\):$' 2
}

# LEVELS nested loops around one global store.
check_helper_global_loop()
{
    local name=$1 levels=$2

    build_helper "$name"
    run_status static 1 "$work/$name"
    callee_count "$name" '^b[0-9]' $((1 + 3 * levels))
    callee_count "$name" '^  branch ' "$levels"
    callee_count "$name" '^  jump ' $((2 * levels))
    callee_count "$name" '^  store ' 1
    callee_count "$name" '^  %d[0-9]* = load\.i32 ' 1
}

# The third argument counts the loads left: a plain reload right after the store
# is forwarded the stored value, a volatile one is not.
check_helper_global_effect()
{
    local name=$1 memory_prefix=$2 loads=$3

    build_helper "$name"
    run_status static 1 "$work/$name"
    run_status static 2 "$work/$name" extra
    callee_count "$name" "^  ${memory_prefix}store " 1
    callee_count "$name" "^  %d[0-9]* = ${memory_prefix}load\.i32 " "$loads"
}

check_helper_global_pair()
{
    local name=helper-global-pair

    build_helper "$name"
    run_status static 1 "$work/$name"
    run_status static 2 "$work/$name" extra
    callee_count "$name" '^  store ' 2
    # The reload of the global just stored is forwarded the stored value.
    callee_count "$name" '^  %d[0-9]* = load\.i32 ' 0
}

check_helper_global_load_store()
{
    local name=helper-global-load-store

    build_helper "$name"
    run_status static 17 "$work/$name"
    vir_extract_callee main \
        "$work/$name.vir" | awk '
            / = load\.i32 / { load = NR }
            / store / { store = NR }
            END { exit !(load && store && load < store) }'
}

check_helper_global_branch()
{
    local name=helper-global-branch

    build_helper "$name"
    run_status static 7 "$work/$name"
    callee_count "$name" '^  branch ' 1
    callee_count "$name" '^  store ' 2
    # Each arm's reload of the global it just stored is forwarded the value.
    callee_count "$name" '^  %d[0-9]* = load\.i32 ' 0
}

# Volatile globals: every store and reload stays, the reload after a store to
# another volatile global included.
check_helper_global_volatile()
{
    local name=$1 loads=$2

    build_helper "$name"
    run_status static 1 "$work/$name"
    run_status static 2 "$work/$name" extra
    callee_count "$name" '^  volatile\.store ' 2
    callee_count "$name" '^  %d[0-9]* = volatile\.load\.i32 ' "$loads"
}

check_run()
{
    local name=$1 expected=$2
    local -a args=("${@:3}")
    local source="tests/vir-direct-arm64-$name.c"

    run_compiler --dump-vir -o "$work/$name" \
        "$source" 2> "$work/$name.vir"
    run_status static "$expected" "$work/$name" "${args[@]}"
}

check_run_o0()
{
    local name=$1 expected=$2

    run_compiler --no-opt --dump-vir -o "$work/$name-o0" \
        "tests/vir-direct-arm64-$name.c" 2> "$work/$name-o0.vir"
    run_status static "$expected" "$work/$name-o0"
}

check_direct direct 62
check_helper helper-leaf 2 3
check_helper helper-two 19 37
check_helper helper-three 33 57
check_helper helper-i64 2 3
check_helper helper-i64-two 3 4
check_helper_branch helper-branch 4
check_run helper-branch-unsigned 70
check_helper_branch helper-merge 3
check_helper_loop
check_run helper-loop-break 1
check_run helper-loop-break 2 extra
check_helper_global_loop helper-global-loop 1
check_helper_global_loop helper-global-loop-nested 2
check_helper_global_loop helper-global-loop-deep 3
check_helper_global_loop helper-global-loop-ten 10
check_helper_global_effect helper-global-volatile volatile. 1
check_helper_global_effect helper-global '' 0
check_helper_global_pair
check_helper_global_load_store
check_helper_global_branch
check_helper_global_volatile helper-global-branch-volatile 2
check_helper_global_volatile helper-global-volatile-pair 1
check_run helper-i64-mixed 1
check_run helper-i64-local-mixed 1
check_direct argc 8 15
check_direct argc-return 1 2
check_direct div 47 54
check_dynlink div 47 54
check_direct call-pointer 42
check_direct argv 4 5
check_direct argv-char-load 47
check_direct argv-char-offset 116
check_direct pointer-join 47 101
check_run pointer-join-duplicate 47
check_args static argv-index-char-load 101 extra
check_args static argv-index-char-offset-one 120 extra
check_direct cfg 30 29
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
    / = load.i32 / { loads++; loaded += addresses[$4] }
    END { exit failed || !count || !stored || loaded != loads }
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
check_direct global-pointer-int 13 14
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
# Build twice at each level; the stats files hold the VIR dump and must match.
for level in 0 2; do
    opt_flags=()
    [ "$level" -eq 2 ] || opt_flags=(--no-opt)
    for suffix in '' -repeat; do
        run_compiler "${opt_flags[@]}" --dump-vir --stats \
            -o "$work/branch-initialized-root-o$level$suffix" \
            tests/vir-direct-branch-initialized-root.c \
            2> "$work/branch-initialized-root-o$level$suffix.stats"
    done
    cmp "$work/branch-initialized-root-o$level" \
        "$work/branch-initialized-root-o$level-repeat"
    cmp "$work/branch-initialized-root-o$level.stats" \
        "$work/branch-initialized-root-o$level-repeat.stats"
    run_status static 31 "$work/branch-initialized-root-o$level"
    run_status static 29 "$work/branch-initialized-root-o$level" extra
done
vir_extract_function main \
    "$work/branch-initialized-root-o2.stats" \
    > "$work/branch-initialized-root.dump"
test "$(grep -c ' = stackaddr ' "$work/branch-initialized-root.dump")" -eq 1
test "$(grep -c '^  store ' "$work/branch-initialized-root.dump")" -eq 2
grep -Eq '^b[0-9]+:$' "$work/branch-initialized-root.dump"
grep -Eq ' = load\.i32 ' "$work/branch-initialized-root.dump"

# Preserve explicit stack provenance through deep pointer chains.
check_direct local-pointer-overdepth 52 53
check_dynlink local-pointer-overdepth 52 53
check_direct local-pointer-level4 52 53
check_run local-pointer-volatile 52
check_run local-pointer-reassigned 52
check_run local-pointer-cyclic 1
check_run local-pointer-collapsed 52
check_run local-pointer-collapsed 53 extra
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
    # A pointer reloaded right after its store is forwarded the stored
    # address, so the write may land on the scalar itself.
    /^  store / {
        address = $2; sub(/,$/, "", address)
        if (loaded_pointer[address] || scalar[address] && linked) mutated = 1
        if (scalar[address]) initialized = 1
        if (pointer[address] && scalar[$3]) linked = 1
    }
    / = load.ptr / { if (pointer[$4]) loaded_pointer[$1] = 1 }
    / = load.i32 / { if (loaded_pointer[$4] || scalar[$4]) observed = 1 }
    END { exit !initialized || !linked || !mutated || !observed }
' "$work/local-pointer-deep.dump"

check_dynlink global-pointer-unknown 7 7
check_direct volatile 8 8
check_dynlink cfg 30 29
check_dynlink argv 4 5
check_dynlink argv-char-load 47 47
check_dynlink argv-char-offset 116 116
check_dynlink pointer-join 47 101
check_args dynamic argv-index-char-load 101 extra
check_args dynamic argv-index-char-offset-one 120 extra
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
check_dynlink global-pointer-int 13 14
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

# Eight _Bool register arguments push the trailing int argument onto the stack.
check_run call-bool-arg-int-stack 0
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
check_direct frame-runtime 0
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
check_run shared-dag 0
check_run ptradd-large 0
check_run argv-deref 4
check_run argv-index 0
check_run argv-index-other 1 extra extra
check_run argv-index-dynamic 0
check_run_o0 argv-index-unused 0
check_run argv-index-char-other 101 extra extra
check_run argv-index-char-dynamic 101 extra
check_run argv-index-char-intervening 101 extra
check_run argv-index-char-offset-two 116 extra
check_run argv-index-char-offset-other 120 extra extra
check_run argv-index-char-offset-dynamic 120 extra
check_run argv-index-char-offset-intervening 120 extra
check_run argv-char-offset-two 109
check_run argv-char-offset-dynamic 116
check_direct argv-pointer-compare 1 1
check_direct argv-pointer-equal 0 0
check_direct argv-index-compare 0 1
check_direct argv-index-equal 1 0
check_dynlink argv-pointer-compare 1 1
check_dynlink argv-pointer-equal 0 0
check_dynlink argv-index-compare 0 1
check_dynlink argv-index-equal 1 0
check_run argv-wrong-type 47
check_direct global-pointer-unknown 7
check_run global-pointer-deep 7

run_compiler --dump-vir -o "$work/cfg-dump" tests/vir-direct-arm64-cfg.c \
    2> "$work/cfg.vir"
vir_extract_function main "$work/cfg.vir" \
    | grep -Eq '^  jump b[0-9]+\(\)$'

run_compiler --dump-vir -o "$work/argv-char-load-dump" \
    tests/vir-direct-arm64-argv-char-load.c 2> "$work/argv-char-load.vir"
vir_extract_function main "$work/argv-char-load.vir" \
    | awk '$3 == "load.ptr" { ptr = $1; next } $3 == "load.i8" && $4 == ptr { byte = 1 } END { exit !byte }'
if vir_extract_function main "$work/argv-char-load.vir" \
    | grep -q 'ptradd'; then exit 1; fi

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

# The reload may be forwarded the stored value, dropping its own ptradd.
run_compiler --dump-vir -o "$work/global-ptradd-dump" \
    tests/vir-direct-global-ptradd.c 2> "$work/global-ptradd.vir"
vir_extract_function main "$work/global-ptradd.vir" \
    | awk '/^  %d[0-9]+ = globaladdr @values, 8, align 4$/ { root++ } /^  %d[0-9]+ = ptradd %d[0-9]+, %d[0-9]+$/ { ptradd++ } END { exit root < 1 || ptradd < 1 || ptradd > 2 }'

# Binary dump names retain .i32; the operand producers prove I64 scaling. A
# forwarded reload drops its own extension, scale and ptradd. Check a scaled
# global index: EXTENSION widens it, OPPOSITE must not appear, and each scale is
# a multiply by 4 or, after normalization, a shift by 2.
check_ptradd_scale()
{
    local name=$1 extension=$2 opposite=$3
    run_compiler --dump-vir -o "$work/global-ptradd-$name-dump" \
        "tests/vir-direct-global-ptradd-$name.c" \
        2> "$work/global-ptradd-$name.vir"
    vir_extract_function main "$work/global-ptradd-$name.vir" \
        | awk -v extension="$extension" -v opposite="$opposite" '
            /^  %d[0-9]+ = globaladdr @values, 8, align 4$/ { root++ }
            $3 == extension ".i32.i64" { extended[$1] = 1; extensions++ }
            $3 == opposite ".i32.i64" { bad = 1 }
            $3 == "const.i64" && $4 == "4" { scale[$1] = 1 }
            $3 == "const.i32" && $4 == "2" { shift[$1] = 1 }
            $3 == "shl.i32" {
                sub(/,$/, "", $4)
                if (extended[$4] && shift[$5]) scaled[$1] = 1
                else bad = 1
                multiplies++
            }
            $3 == "mul.i32" {
                sub(/,$/, "", $4)
                if (extended[$4] && scale[$5] || scale[$4] && extended[$5])
                    scaled[$1] = 1
                else bad = 1
                multiplies++
            }
            $3 == "ptradd" { if (!scaled[$5]) bad = 1; ptradd++ }
            END {
                exit bad || root < 1 || extensions < 1 || extensions > 2 ||
                     multiplies != extensions || ptradd != extensions
            }
        '
}
check_ptradd_scale dynamic sext zext
check_ptradd_scale unsigned zext sext
vir_extract_function main "$work/narrow.vir" \
    | grep -Eq '^  %d[0-9]+ = zext\.i16\.i32 %d[0-9]+$'

# Each repeat build omits --dump-vir, so the cmp also proves the dump does not
# perturb code generation.
external_pointer_source=tests/vir-direct-external-pointer-origin.c
run_compiler --no-libc --dynlink --dump-vir \
    -o "$work/external-pointer" "$external_pointer_source" \
    2> "$work/external-pointer.vir"
run_compiler --no-libc --dynlink \
    -o "$work/external-pointer-repeat" "$external_pointer_source"
cmp "$work/external-pointer" "$work/external-pointer-repeat"
run_status dynamic 0 "$work/external-pointer"

typed_external_pointer_source=tests/vir-direct-external-typed-pointer.c
run_compiler --no-libc --dynlink --dump-vir \
    -o "$work/external-typed-pointer" "$typed_external_pointer_source" \
    2> "$work/external-typed-pointer.vir"
run_compiler --no-libc --dynlink \
    -o "$work/external-typed-pointer-repeat" \
    "$typed_external_pointer_source"
cmp "$work/external-typed-pointer" "$work/external-typed-pointer-repeat"
# The reload right after the store is forwarded the stored value.
vir_extract_function main \
    "$work/external-typed-pointer.vir" \
    | grep -Eq '^  store %d[0-9]+, %d[0-9]+$'
run_status dynamic 0 "$work/external-typed-pointer"

typed_pointer_offset_source=tests/vir-direct-external-typed-pointer-offset.c
run_compiler --no-libc --dynlink --dump-vir \
    -o "$work/external-typed-pointer-offset" \
    "$typed_pointer_offset_source" \
    2> "$work/external-typed-pointer-offset.vir"
vir_extract_function main \
    "$work/external-typed-pointer-offset.vir" \
    | grep -Eq ' = ptradd '
run_status dynamic 0 "$work/external-typed-pointer-offset"
run_status dynamic 0 "$work/external-typed-pointer-offset" extra

for name in join join-offset mixed-join loop-join mixed-loop-join; do
    check_external_pointer_join "external-pointer-$name" \
        "tests/vir-direct-arm64-external-pointer-$name.c"
done

external_pointer_bound_source=tests/vir-direct-arm64-external-pointer-bound.c
run_compiler --no-libc --dynlink --stats --dump-vir \
    -o "$work/external-pointer-bound" "$external_pointer_bound_source" \
    2> "$work/external-pointer-bound.stats"
run_compiler --no-libc --dynlink --stats --dump-vir \
    -o "$work/external-pointer-bound-repeat" "$external_pointer_bound_source" \
    2> "$work/external-pointer-bound-repeat.stats"
cmp "$work/external-pointer-bound" "$work/external-pointer-bound-repeat"
cmp "$work/external-pointer-bound.stats" \
    "$work/external-pointer-bound-repeat.stats"
test "$(vir_extract_function main \
    "$work/external-pointer-bound.stats" | grep -c ' = ptradd ')" -gt 256
run_status dynamic 0 "$work/external-pointer-bound"

# Offset a malloc'd pointer, replace it with a libc result, offset it again.
run_compiler --no-libc --dynlink --dump-vir \
    -o "$work/external-pointer-reoffset" \
    tests/vir-direct-arm64-external-pointer-reoffset.c \
    2> "$work/external-pointer-reoffset.vir"
run_status dynamic 0 "$work/external-pointer-reoffset"
run_status dynamic 0 "$work/external-pointer-reoffset" extra

# Preserve wide accesses through a cast from malloc's opaque void pointer.
run_compiler --no-libc --dynlink --dump-vir \
    -o "$work/external-void-pointer-wide" \
    tests/vir-direct-arm64-external-void-pointer-wide.c \
    2> "$work/external-void-pointer-wide.vir"
run_status dynamic 0 "$work/external-void-pointer-wide"

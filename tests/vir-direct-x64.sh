#!/usr/bin/env bash
set -euo pipefail

source tests/vir-direct-common.sh

stage=$1
if test "$stage" -eq 0; then
    compiler=out/shecc
else
    compiler="out/shecc-stage${stage}.elf"
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

check_direct()
{
    local name=$1 expected=$2 extra_expected=$3 status
    local source="tests/vir-direct-x64-$name.c"

    if test "$name" = direct; then
        source=tests/vir-direct-x64.c
    elif test "$name" = branch-initialized-root; then
        source=tests/vir-direct-branch-initialized-root.c
    elif test "$name" = narrow-global || [[ "$name" = global-ptradd* ]]; then
        source="tests/vir-direct-$name.c"
    fi

    vir_build_deterministic "$compiler" "$source" \
        "$work/$name" "$work/$name-repeat" "$work/$name.stats" \
        --dump-vir
    if "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if test "$extra_expected" != -; then
        if "$work/$name" extra; then status=0; else status=$?; fi
        test "$status" -eq "$extra_expected"
    fi
}

check_direct_binary()
{
    local name=$1 source="tests/vir-direct-x64-$1.c"

    vir_build_deterministic "$compiler" "$source" \
        "$work/$name" "$work/$name-repeat" "$work/$name.stats" \
        --dump-vir
}

dump_main_vir()
{
    local name=$1 source=${2:-"tests/vir-direct-x64-$1.c"}
    local dump="$work/$name.vir"

    "$compiler" --dump-vir -o "$work/$name-dump" \
        "$source" 2> "$dump"
    "$compiler" --dump-vir -o "$work/$name-dump-repeat" \
        "$source" 2> "$dump.repeat"
    "$compiler" -o "$work/$name-plain" "$source"
    cmp "$work/$name-dump" "$work/$name-plain"
    cmp "$dump" "$dump.repeat"
    vir_extract_function main "$dump" > "$dump.main"
}

check_entry_root_effects()
{
    local name=$1 addresses=$2 entry_stores=$3 stores=$4
    local check_branch_order=${5:-0}

    awk -v expected_addresses="$addresses" \
        -v expected_entry_stores="$entry_stores" \
        -v expected_stores="$stores" \
        -v check_branch_order="$check_branch_order" '
        /^b[0-9]+/ { in_entry = !seen_block; seen_block = 1 }
        / = stackaddr / {
            if (!in_entry)
                failed = 1
            addresses++
        }
        /store %d[0-9]+, %d[0-9]+/ {
            stores++
            if (in_entry)
                entry_stores++
            else if (!branch_store)
                branch_store = NR
        }
        / = load\.i32 / && branch_store && !branch_load { branch_load = NR }
        END {
            if (failed || addresses != expected_addresses ||
                entry_stores != expected_entry_stores ||
                stores != expected_stores)
                exit 1
            if (check_branch_order &&
                (!branch_store || !branch_load || branch_store >= branch_load))
                exit 1
        }
    ' "$work/$name.vir.main"
}

check_global_pointer_effect_order()
{
    local dump=$1 first_event=$2

    # The slot is read after the event that may change it, the store goes
    # through that pointer, and the globals it may name are read after the
    # store. The assignment's own reread is dead and is no longer emitted.
    awk -v first_event="$first_event" '
        $0 ~ first_event && !first { first = NR }
        / = load\.ptr / && first && !pointer_load { pointer_load = NR }
        /store %d[0-9]+, %d[0-9]+/ && pointer_load && !pointee_store {
            pointee_store = NR
        }
        / = load\.i32 / && pointee_store && !pointee_load {
            pointee_load = NR
        }
        END {
            exit !first || !pointer_load || !pointee_store || !pointee_load ||
                 !(first < pointer_load && pointer_load < pointee_store &&
                   pointee_store < pointee_load)
        }
    ' "$dump"
}

check_vir_dump()
{
    local name=$1
    local dump="$work/$name.vir"

    dump_main_vir "$name"
    test "$(grep -c ' = zext.i32.i64 ' "$dump.main")" -eq 4
    if grep -q 'sext.i32.i64' "$dump.main"; then exit 1; fi
    awk '
        / = zext.i32.i64 / { zext[$1] = 1 }
        / = ptradd / { if (!zext[$NF]) failed = 1; ptradd++ }
        END { exit failed || ptradd != 4 }
    ' "$dump.main"
}

check_vir_effect_order()
{
    local name=$1

    dump_main_vir "$name"
    awk '
        /store %d|volatile.store %d/ { store = NR }
        /load.i32/ && store && !load { load = NR }
        /return %d/ && load { ret = NR }
        END { exit !store || !load || !ret || store >= load || load >= ret }
    ' "$work/$name.vir.main"
}

check_global_pointer_slot_dump()
{
    local dump="$work/global-pointer-slot.vir"

    dump_main_vir global-pointer-slot \
        tests/vir-direct-x64-global-pointer-slot.c
    awk '
        /^  %d[0-9]+ = globaladdr @[^,[:space:]]+, 8, align 8$/ {
            symbol = $0
            sub(/^.*globaladdr @/, "", symbol)
            sub(/,.*/, "", symbol)
            if (count && symbol != expected) failed = 1
            expected = symbol
            slots[$1] = 1
            count++
        }
        /^  store / {
            address = $2; sub(/,$/, "", address)
            if (slots[address]) stored = 1
        }
        / = load.ptr / { if (slots[$4]) loaded = 1 }
        END { exit failed || !count || !stored || !loaded }
    ' "$dump.main"
    test "$(grep -Ec '^  %d[0-9]+ = load\.ptr ' "$dump.main")" -eq 1
    test "$(grep -Ec '^  store %d[0-9]+, %d[0-9]+$' "$dump.main")" -eq 2
    check_global_pointer_effect_order "$dump.main" \
        'store %d[0-9]+, %d[0-9]+'
}

check_global_pointer_call_dump()
{
    local dump="$work/global-pointer-call.vir"
    local helper="$work/global-pointer-call.helper"

    dump_main_vir global-pointer-call \
        tests/vir-direct-x64-global-pointer-call.c
    vir_extract_callee main "$dump" > "$helper"
    test "$(grep -Ec '^  %d[0-9]+ = globaladdr @selected, 8, align 8$' \
        "$helper")" -eq 1
    test "$(grep -Ec '^  store %d[0-9]+, %d[0-9]+$' "$helper")" -eq 1
    test "$(grep -Ec '^  %d[0-9]+ = call\.i32 @[^ (]+\(\) sig\(\)->i32$' \
        "$dump.main")" -eq 1
    test "$(grep -Ec '^  %d[0-9]+ = load\.ptr ' "$dump.main")" -eq 1
    check_global_pointer_effect_order "$dump.main" \
        'call[.]i32 @[^ (]+[(][)]'
}

check_unknown_pointer_alias_dump()
{
    local dump="$work/unknown-pointer-alias.vir"
    local helper="$work/unknown-pointer-alias.helper"

    dump_main_vir unknown-pointer-alias \
        tests/vir-direct-x64-unknown-pointer-alias.c
    vir_extract_callee main "$dump" > "$helper"
    grep -Eq '^b[0-9]+\(%d[0-9]+:ptr, %d[0-9]+:ptr\):$' "$helper"
    test "$(grep -Ec '^  store %d[0-9]+, %d[0-9]+$' "$helper")" -eq 1

    # The read through the other pointer may alias the store, so it stays after
    # it; the store's own dead reread is gone.
    test "$(grep -Ec '^  %d[0-9]+ = load\.i32 ' "$helper")" -eq 1
    awk '
        /store %d[0-9]+, %d[0-9]+/ && !store { store = NR }
        / = load\.i32 / && store && !load { load = NR }
        END { exit !store || !load || store >= load }
    ' "$helper"
    test "$(grep -Ec '^  %d[0-9]+ = call\.i32 @[^ (]+\(' \
        "$dump.main")" -eq 2
}

check_main_case()
{
    local name=$1 expected=$2 status
    local source="tests/vir-direct-x64-$name.c"

    "$compiler" --dump-vir --stats -o "$work/$name" \
        "$source" 2> "$work/$name.stats"
    "$compiler" --dump-vir -o "$work/$name-repeat" "$source"
    cmp "$work/$name" "$work/$name-repeat"
    if "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
}

check_direct direct 11 -
check_direct cfg 23 7
check_direct join 23 7
check_direct loop 21 12
check_direct ops 13 13
check_direct div 0 0
check_direct udiv 63 206
check_direct div-i64 74 169
check_direct casts 15 0
check_direct truthy 0 31
check_direct truthy-join 0 31
check_direct pointer 0 29
check_direct pointer-join 14 7
check_direct pointer-store 0 29
check_direct call 29 -
check_direct r11-call 255 -
check_direct param-scratch 13 -
check_direct param-shift 250 -
check_direct param-loop 3 6
check_direct param-derived-join 8 -
check_direct edge-update-computed 22 -
check_direct r11-leaf 9 -
"$compiler" --no-libc --dump-vir -o "$work/r11-leaf-nolibc" \
    tests/vir-direct-x64-r11-leaf.c

check_direct call-direct-root 29 -
check_direct call-direct-args 13 25
check_direct call-direct-pointer 30 31
check_direct call-direct-stack 28 -
check_direct call-direct-stack-pointer 22 23
check_direct call-narrow 37 -
check_direct call-direct-legacy-reference 30 -
check_direct call-direct-global-reference 30 -
check_direct call-args 13 25
check_direct call-argv 25 26
check_direct call-pointer 30 31
check_direct call-pointer-high 11 12
check_direct call-pointer-result 29 -
check_direct call-six 21 -
check_direct call-seven 28 -
check_direct call-eight 36 -
check_direct call-stack-pointer 22 23
check_direct call-void 29 -
check_direct call-bool-arg 0 1

# SysV returns _Bool in AL; the direct caller canonicalizes the full EAX value
# before storing the VIR i1 result (AND EAX, 1; then store).
check_direct call-bool-arg-stack 0 1
check_main_case call-bool-arg-fallback 0 direct-x64-subset
check_direct call-i64 2 2
check_direct call-i64-stack 1 -
check_direct call-i64-mixed 4 -
check_direct i64-memory 1 1
check_direct i64-volatile 1 1
check_direct volatile 29 30
check_direct volatile-discard 29 30
check_direct load 29 29
check_direct store 29 29
check_direct store-init 29 29
check_direct store-stale-read 29 29
check_direct store-read-arith 30 30
check_direct store-read-pair 29 29
check_direct store-read-branch 30 29
check_direct merged-pointer-effect 29 30
check_direct merged-pointer-load 29 30
check_direct merged-volatile-pointer-effect 29 30
check_direct merged-pointer-effect-void-call 29 30
check_vir_effect_order merged-pointer-effect
check_vir_effect_order merged-volatile-pointer-effect
check_direct narrow-memory 29 -
check_direct narrow-global 29 -
check_direct narrow-array 229 -
check_direct narrow-array-index 229 229
check_direct narrow-array-index-unsigned 229 229
check_vir_dump narrow-array-index-unsigned
check_direct narrow-volatile 230 -
check_direct narrow-volatile-local 230 -
check_direct stack-array 29 -
check_direct ptradd-narrow 29 -
check_direct ptradd-narrow-signed 29 -
check_direct ptrsub 29 -
check_direct ptrsub-const 29 -
check_direct ptrsub-unsigned 29 -
check_direct ptrsub-unsigned-const 29 -
check_direct_binary load-i32
pointer_join_dump="$work/pointer-join.vir"
"$compiler" --dump-vir -o "$work/pointer-join-dump" \
    tests/vir-direct-x64-pointer-join.c 2> "$pointer_join_dump"
vir_extract_function main "$pointer_join_dump" > "$pointer_join_dump.main"
grep -Eq '^b[0-9]+\([^:]+:ptr\):$' "$pointer_join_dump.main"
test "$(grep -Ec '^  jump b[0-9]+\(%[^)]*\)$' "$pointer_join_dump.main")" -eq 2
grep -q 'store %' "$pointer_join_dump.main"
alias_dump="$work/i64-memory-alias.vir"
"$compiler" --dump-vir -o "$work/i64-memory-alias-dump" \
    tests/vir-direct-x64-i64-memory-alias.c 2> "$alias_dump"
vir_extract_function main "$alias_dump" > "$alias_dump.main"
test "$(grep -Ec '^b[0-9]+\([^:]+:ptr\):$' "$alias_dump.main")" -eq 1
test "$(grep -Ec '^  jump b[0-9]+\(%d[0-9]+\)$' "$alias_dump.main")" -eq 2
test "$(grep -Ec '^  (volatile\.)?store %' "$alias_dump.main")" -eq 3
# The store's own reread is the unused assignment value, so DCE drops it.
test "$(grep -Ec '^  %d[0-9]+ = (volatile\.)?load\.i64 ' "$alias_dump.main")" -eq 2

# An assignment's value is reread only when it is used, so each store is no
# longer followed by a load (and its address arithmetic) that nothing reads.

check_case()
{
    local name=$1 expected=$2 extra_expected=$3 status
    local source="tests/vir-direct-x64-$name.c"

    if test "$name" = branch-volatile-root; then
        source=tests/vir-direct-branch-volatile-root.c
    fi

    "$compiler" --dump-vir --stats -o "$work/$name" \
        "$source" 2> "$work/$name.stats"
    "$compiler" --dump-vir -o "$work/$name-repeat" "$source"
    cmp "$work/$name" "$work/$name-repeat"
    if "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"
}

check_program()
{
    local name=$1 expected=$2 extra_expected=$3 status
    local source="tests/vir-direct-x64-$name.c"

    "$compiler" -o "$work/$name" "$source"
    "$compiler" -o "$work/$name-repeat" "$source"
    cmp "$work/$name" "$work/$name-repeat"
    if "$work/$name"; then status=0; else status=$?; fi
    test "$status" -eq "$expected"
    if "$work/$name" extra; then status=0; else status=$?; fi
    test "$status" -eq "$extra_expected"
}

check_direct wide-cast 27 5
check_case pointer-int-cast 0 1 'direct-x64-subset|instruction|unsupported'
check_case pointer-int-return 1 1 'direct-x64-subset|instruction|unsupported'
check_case wide-return 0 0
check_case merged-narrow-effect 129 130 effects
check_direct local-pointer-chain 41 42
check_direct local-pointer-deep 51 52
check_direct branch-local-root-call 23 17
dump_main_vir branch-local-root-call
check_entry_root_effects branch-local-root-call 2 0 0
check_direct i64-memory-alias 2 1
check_direct selected-root-alias 31 222
dump_main_vir selected-root-alias
check_entry_root_effects selected-root-alias 2 2 3
check_direct global-pointer-slot 7 14
check_global_pointer_slot_dump
check_direct global-pointer-call 7 14
check_global_pointer_call_dump
check_direct unknown-pointer-alias 0 13
check_unknown_pointer_alias_dump
check_case i64-memory-aggregate 1 1 'direct-x64-subset|allocat|unsupported'
check_case call-aggregate 29 29 'direct-x64-subset|unsupported|instruction|allocat'

# Materialize the entry-only initializer before the branch and keep the later
# branch-specific write as a separate ordered effect.
check_direct store-branch 29 29
dump_main_vir store-branch
check_entry_root_effects store-branch 1 1 1
check_direct store-stale-branch 0 0
dump_main_vir store-stale-branch
check_entry_root_effects store-stale-branch 1 1 2 1
check_direct branch-initialized-root 31 29
dump_main_vir branch-initialized-root tests/vir-direct-branch-initialized-root.c
check_entry_root_effects branch-initialized-root 1 0 2
check_case branch-volatile-root 31 29 'root-address|volatile'
for level in 0 1 2; do
    "$compiler" --vir-opt="$level" \
        --dump-vir --dump-vir --stats \
        -o "$work/branch-initialized-root-o$level" \
        tests/vir-direct-branch-initialized-root.c \
        2> "$work/branch-initialized-root-o$level.stats"
    "$compiler" --vir-opt="$level" \
        --dump-vir --dump-vir --stats \
        -o "$work/branch-initialized-root-o$level-repeat" \
        tests/vir-direct-branch-initialized-root.c \
        2> "$work/branch-initialized-root-o$level-repeat.stats"
    cmp "$work/branch-initialized-root-o$level" \
        "$work/branch-initialized-root-o$level-repeat"
    cmp "$work/branch-initialized-root-o$level.stats" \
        "$work/branch-initialized-root-o$level-repeat.stats"
    if "$work/branch-initialized-root-o$level"; then status=0; else status=$?; fi
    test "$status" -eq 31
    if "$work/branch-initialized-root-o$level" extra; then status=0; else status=$?; fi
    test "$status" -eq 29
done
check_direct global-load 29 29
check_direct global-ptradd 8 9
check_direct global-ptradd-dynamic 8 9
check_direct global-ptradd-unsigned 8 9
# Both constant offsets fit in a sign-extended byte: "add rax, 4" (48 83 c0 04).
check_direct ptradd 0 29
check_direct ptradd-unsigned 0 29

external_pointer_source=tests/vir-direct-external-pointer-origin.c
"$compiler" --no-libc --dynlink --dump-vir --stats \
    -o "$work/external-pointer" "$external_pointer_source" \
    2> "$work/external-pointer.stats"
"$compiler" --no-libc --dynlink --dump-vir \
    -o "$work/external-pointer-repeat" "$external_pointer_source"
cmp "$work/external-pointer" "$work/external-pointer-repeat"
if "$work/external-pointer"; then status=0; else status=$?; fi
test "$status" -eq 0
"$compiler" --no-libc --dynlink -o "$work/external-pointer-legacy" \
    "$external_pointer_source"
if "$work/external-pointer-legacy"; then status=0; else status=$?; fi
test "$status" -eq 0

typed_external_pointer_source=tests/vir-direct-external-typed-pointer.c
"$compiler" --no-libc --dynlink --dump-vir --stats \
    -o "$work/external-typed-pointer" "$typed_external_pointer_source" \
    2> "$work/external-typed-pointer.stats"
"$compiler" --no-libc --dynlink --dump-vir \
    -o "$work/external-typed-pointer-repeat" \
    "$typed_external_pointer_source"
cmp "$work/external-typed-pointer" "$work/external-typed-pointer-repeat"
if "$work/external-typed-pointer"; then status=0; else status=$?; fi
test "$status" -eq 0
"$compiler" --no-libc --dynlink -o "$work/external-typed-pointer-legacy" \
    "$typed_external_pointer_source"
if "$work/external-typed-pointer-legacy"; then status=0; else status=$?; fi
test "$status" -eq 0

typed_pointer_offset_source=tests/vir-direct-external-typed-pointer-offset.c
"$compiler" --no-libc --dynlink --dump-vir --stats --dump-vir \
    -o "$work/external-typed-pointer-offset" \
    "$typed_pointer_offset_source" \
    2> "$work/external-typed-pointer-offset.stats"
vir_extract_function main \
    "$work/external-typed-pointer-offset.stats" \
    | grep -Eq ' = ptradd '
if "$work/external-typed-pointer-offset"; then status=0; else status=$?; fi
test "$status" -eq 0
if "$work/external-typed-pointer-offset" extra; then status=0; else status=$?; fi
test "$status" -eq 0

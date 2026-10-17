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

check_run()
{
    local name=$1 expected=$2 extra_expected=$3
    local source="tests/vir-direct-x64-$name.c"

    if test "$name" = direct; then
        source=tests/vir-direct-x64.c
    elif [[ "$name" = branch-*-root ]] || test "$name" = narrow-global \
        || [[ "$name" = global-ptradd* ]]; then
        source="tests/vir-direct-$name.c"
    fi

    vir_build_deterministic "$compiler" "$source" \
        "$work/$name" "$work/$name-repeat" "$work/$name.dump"
    vir_expect_status "$expected" "$work/$name"
    if test "$extra_expected" != -; then
        vir_expect_status "$extra_expected" "$work/$name" extra
    fi
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
            # The branch reload may be forwarded the stored value; one that
            # remains must follow the store.
            if (check_branch_order &&
                (!branch_store || branch_load && branch_store >= branch_load))
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
    local name=$1 extension=$2 opposite=$3
    local dump="$work/$name.vir"

    dump_main_vir "$name"

    # Two stores and two loads each extend their index, unless the load right
    # after the last store is forwarded the stored value instead.
    test "$(grep -c " = $extension.i32.i64 " "$dump.main")" -ge 3
    if grep -q "$opposite.i32.i64" "$dump.main"; then exit 1; fi
    awk -v extension="$extension" '
        $3 == extension ".i32.i64" { extended[$1] = 1 }
        / = const.i64 2$/ { scale[$1] = 1 }
        / = const.i32 1$/ { shift[$1] = 1 }
        / = shl\.i[0-9]+ / {
            sub(/,$/, "", $4)
            if (extended[$4] && shift[$5]) scaled[$1] = 1
            else failed = 1
            multiplies++
        }
        / = mul\.i[0-9]+ / {
            sub(/,$/, "", $4)
            if (extended[$4] && scale[$5] || scale[$4] && extended[$5])
                scaled[$1] = 1
            else failed = 1
            multiplies++
        }
        / = ptradd / {
            if (extended[$NF]) direct++
            else if (scaled[$NF]) multiplied++
            else failed = 1
        }
        END {
            exit failed || direct < 1 || multiplied < 1 ||
                 direct + multiplied < 3 || multiplies != multiplied
        }
    ' "$dump.main"
}

check_vir_effect_order()
{
    local name=$1 require_load=${2:-0}

    dump_main_vir "$name"

    # A plain load right after the store may be forwarded away; a volatile one
    # may not. A load that remains must still follow the store and precede the
    # return.
    awk -v require_load="$require_load" '
        /store %d|volatile.store %d/ { store = NR }
        /load.i32/ && store && !load { load = NR }
        /return %d/ && store { ret = NR }
        END {
            exit !store || !ret || (require_load && !load) ||
                 (load && (store >= load || load >= ret))
        }
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

check_run direct 11 -
check_run cfg 23 7
check_run join 23 7
check_run loop 21 12
check_run ops 13 13
check_run div 0 0
check_run udiv 63 206
check_run div-i64 74 169
check_run casts 15 0
check_run truthy 0 31
check_run truthy-join 0 31
check_run pointer 0 29
check_run pointer-join 14 7
check_run pointer-store 0 29
check_run call 29 -
check_run r11-call 255 -
check_run param-scratch 13 -
check_run param-shift 250 -
check_run param-loop 3 6
check_run param-derived-join 8 -
check_run edge-update-computed 22 -
check_run r11-leaf 9 -
"$compiler" --no-libc -o "$work/r11-leaf-nolibc" \
    tests/vir-direct-x64-r11-leaf.c

check_run call-local-root 29 -
check_run call-narrow 37 -
check_run call-global-fnptr 30 -
check_run call-args 13 25
check_run call-argv 25 26
check_run call-pointer 30 31
check_run call-pointer-high 11 12
check_run call-pointer-result 29 -
check_run call-six 21 -
check_run call-seven 28 -
check_run call-eight 36 -
check_run call-stack-pointer 22 23
check_run call-void 29 -
check_run call-bool-arg 0 1

# SysV returns _Bool in AL; the caller canonicalizes the full EAX value before
# storing the VIR i1 result (AND EAX, 1; then store).
check_run call-bool-arg-stack 0 1
check_run call-i64 2 2
check_run call-i64-stack 1 -
check_run call-i64-mixed 4 -
check_run i64-memory 1 1
check_run i64-volatile 1 1
check_run volatile 29 30
check_run volatile-discard 29 30
check_run load 29 29
check_run store 29 29
check_run store-init 29 29
check_run store-stale-read 29 29
check_run store-read-arith 30 30
check_run store-read-pair 29 29
check_run store-read-branch 30 29
check_run merged-pointer-effect 29 30
check_run merged-pointer-load 29 30
check_run merged-volatile-pointer-effect 29 30
check_run merged-pointer-effect-void-call 29 30
check_vir_effect_order merged-pointer-effect
check_vir_effect_order merged-volatile-pointer-effect 1
check_run narrow-memory 29 -
check_run narrow-global 29 -
check_run narrow-array 229 -
check_run narrow-array-index 229 229
check_run narrow-array-index-unsigned 229 229
check_vir_dump narrow-array-index sext zext
check_vir_dump narrow-array-index-unsigned zext sext
check_run narrow-volatile 230 -
check_run narrow-volatile-local 230 -
check_run stack-array 29 -
check_run ptradd-narrow 29 -
check_run ptradd-narrow-signed 29 -
check_run ptrsub 29 -
check_run ptrsub-const 29 -
check_run ptrsub-unsigned 29 -
check_run ptrsub-unsigned-const 29 -
check_run load-i32 29 29
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

check_run wide-cast 27 5
check_run pointer-int-cast 0 1
check_run pointer-int-return 1 1
check_run wide-return 0 0
check_run merged-narrow-effect 129 130
check_run local-pointer-chain 41 42
check_run local-pointer-deep 51 52
check_run branch-local-root-call 23 17
dump_main_vir branch-local-root-call
check_entry_root_effects branch-local-root-call 2 0 0
check_run i64-memory-alias 2 1
check_run selected-root-alias 31 222
dump_main_vir selected-root-alias
check_entry_root_effects selected-root-alias 2 2 3
check_run global-pointer-slot 7 14
check_global_pointer_slot_dump
check_run global-pointer-call 7 14
check_global_pointer_call_dump
check_run unknown-pointer-alias 0 13
check_unknown_pointer_alias_dump
check_run i64-memory-aggregate 1 1
check_run call-aggregate 29 29

# Materialize the entry-only initializer before the branch and keep the later
# branch-specific write as a separate ordered effect.
check_run store-branch 29 29
dump_main_vir store-branch
check_entry_root_effects store-branch 1 1 1
check_run store-stale-branch 0 0
dump_main_vir store-stale-branch
check_entry_root_effects store-stale-branch 1 1 2 1
check_run branch-initialized-root 31 29
dump_main_vir branch-initialized-root tests/vir-direct-branch-initialized-root.c
check_entry_root_effects branch-initialized-root 1 0 2
check_run branch-volatile-root 31 29
# check_run and dump_main_vir cover the default level; repeat at --no-opt.
for suffix in '' -repeat; do
    "$compiler" --no-opt --dump-vir --stats \
        -o "$work/branch-initialized-root-o0$suffix" \
        tests/vir-direct-branch-initialized-root.c \
        2> "$work/branch-initialized-root-o0$suffix.stats"
done
cmp "$work/branch-initialized-root-o0" "$work/branch-initialized-root-o0-repeat"
cmp "$work/branch-initialized-root-o0.stats" \
    "$work/branch-initialized-root-o0-repeat.stats"
vir_expect_status 31 "$work/branch-initialized-root-o0"
vir_expect_status 29 "$work/branch-initialized-root-o0" extra
check_run global-load 29 29
check_run startup-globals 29 28
check_run global-ptradd 8 9
check_run global-ptradd-dynamic 8 9
check_run global-ptradd-unsigned 8 9
check_run ptradd 0 29
check_run ptradd-unsigned 0 29

for name in external-pointer-origin external-typed-pointer; do
    vir_build_deterministic "$compiler" "tests/vir-direct-$name.c" \
        "$work/$name" "$work/$name-repeat" "$work/$name.vir" \
        --no-libc --dynlink
    vir_expect_status 0 "$work/$name"
done

typed_pointer_offset_source=tests/vir-direct-external-typed-pointer-offset.c
"$compiler" --no-libc --dynlink --dump-vir \
    -o "$work/external-typed-pointer-offset" \
    "$typed_pointer_offset_source" \
    2> "$work/external-typed-pointer-offset.vir"
vir_extract_function main \
    "$work/external-typed-pointer-offset.vir" \
    | grep -Eq ' = ptradd '
vir_expect_status 0 "$work/external-typed-pointer-offset"
vir_expect_status 0 "$work/external-typed-pointer-offset" extra

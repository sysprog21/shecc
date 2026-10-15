#!/usr/bin/env bash

set -euo pipefail
source tests/vir-direct-common.sh

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <stage>" >&2
    exit 1
fi

case "$1" in
    0)
        compiler=("$PWD/out/shecc")
        read -r -a runner <<< "${TARGET_EXEC:-}"
        suffix=stage0
        ;;
    2)
        read -r -a runner <<< "${TARGET_EXEC:-}"
        compiler=("${runner[@]}" "$PWD/out/shecc-stage2.elf")
        suffix=stage2
        ;;
    *)
        echo "stage must be 0 or 2" >&2
        exit 1
        ;;
esac

check_policy_workload()
{
    local name=$1 source=$2 expected_exit=$3

    for opt in 0 2; do
        opt_flags=()
        [ "$opt" -eq 2 ] || opt_flags=(--no-opt)
        local plain="$PWD/out/vir-frontend-$name-plain-o$opt-$suffix"
        local output="$PWD/out/vir-frontend-$name-dumped-o$opt-$suffix"
        local dump="$output.vir"

        "${compiler[@]}" "${opt_flags[@]}" -o "$plain" "$source"
        "${compiler[@]}" "${opt_flags[@]}" --dump-vir \
            -o "$output" "$source" 2> "$dump"
        cmp "$plain" "$output"
        if "${runner[@]}" "$output"; then
            actual_exit=0
        else
            actual_exit=$?
        fi
        test "$actual_exit" -eq "$expected_exit"

        if [ "$name" = multi-latch ] && [ "$opt" -eq 2 ]; then

            # The entry jump and two loop-latch jumps must all target the loop
            # header; this guards against collapsing to a single latch.
            vir_extract_callee main "$dump" | awk '
                index($0, "  jump b") == 1 {
                    target = substr($0, 8)
                    open = index(target, "(")
                    if (open)
                        target = substr(target, 1, open - 1)
                    if (!header)
                        header = target
                    if (target == header)
                        jumps++
                }
                END { exit jumps != 3 }
            '

        fi
    done
}

check_policy_workload gvn tests/vir-frontend-gvn.c 21
check_policy_workload nested-loop tests/vir-frontend-nested-loop.c 159
check_policy_workload licm tests/vir-frontend-licm.c 20
check_policy_workload multi-latch tests/vir-frontend-multi-latch.c 0

#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -gt 2 ]; then
    echo "Usage: $0 <stage> [workloads]" >&2
    exit 1
fi

workloads="${2:-tests/vir-frontend-workloads.txt}"

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

workdir=$(mktemp -d)
trap 'rm -rf -- "$workdir"' EXIT

run_exit()
{
    if "${runner[@]}" "$@"; then
        echo 0
    else
        echo $?
    fi
}

# A dumped build must be byte-identical to a plain one, so the dump cannot
# perturb what it describes.
while read -r fixture expected_exit extra_exit; do
    case "$fixture" in
        '' | '#'*)
            continue
            ;;
    esac

    source="tests/vir-frontend-$fixture.c"
    plain="$workdir/vir-frontend-$fixture-plain-$suffix"
    dumped="$workdir/vir-frontend-$fixture-$suffix"

    "${compiler[@]}" -o "$plain" "$source"
    "${compiler[@]}" --dump-vir -o "$dumped" "$source" 2> /dev/null
    cmp "$plain" "$dumped"
    test "$(run_exit "$plain")" -eq "$expected_exit"
    if [ -n "${extra_exit:-}" ]; then
        test "$(run_exit "$plain" extra)" -eq "$extra_exit"
    fi
done < "$workloads"

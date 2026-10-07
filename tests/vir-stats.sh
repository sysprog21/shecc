#!/usr/bin/env bash

set -eu

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <stage>" >&2
    exit 1
fi

case "$1" in
    0)
        compiler=("$PWD/out/shecc")
        ;;
    2)
        read -r -a compiler <<< "${TARGET_EXEC:-} $PWD/out/shecc-stage2.elf"
        ;;
    *)
        echo "stage must be 0 or 2" >&2
        exit 1
        ;;
esac

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

while read -r path _purpose; do
    case "$path" in
        '' | '#'*)
            continue
            ;;
    esac

    first_output="$tmpdir/first.elf"
    second_output="$tmpdir/second.elf"
    first_stats="$tmpdir/first.stats"
    second_stats="$tmpdir/second.stats"

    "${compiler[@]}" --stats -o "$first_output" "$path" \
        > /dev/null 2> "$first_stats"
    "${compiler[@]}" --stats -o "$second_output" "$path" \
        > /dev/null 2> "$second_stats"
    cmp -s "$first_output" "$second_output"
    cmp -s "$first_stats" "$second_stats"

    # Stats remain deterministic diagnostics, independent of internal IR phases.
    test -s "$first_stats"
done < tests/vir-baseline-workloads.txt

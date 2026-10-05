#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -gt 2 ]; then
    echo "Usage: $0 <stage> [workloads]" >&2
    exit 1
fi

workloads="${2:-tests/vir-frontend-workloads.txt}"
read -r -a vir_opt_args <<< "${VIR_OPT:-}"
frontend_flag=--dump-vir

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

while read -r fixture _functions expected_exit _dce _sccp _cse _cfg extra_exit _effects _parsed; do
    case "$fixture" in
        '' | '#'*)
            continue
            ;;
    esac

    source="tests/vir-frontend-$fixture.c"
    plain="$workdir/vir-frontend-$fixture-plain-$suffix"
    shadow="$workdir/vir-frontend-$fixture-$suffix"
    stats="$shadow.stats"

    "${compiler[@]}" "${vir_opt_args[@]}" -o "$plain" "$source"
    "${compiler[@]}" "${vir_opt_args[@]}" --stats "$frontend_flag" -o "$shadow" "$source" \
        2> "$stats"
    cmp "$plain" "$shadow"
    if "${runner[@]}" "$plain"; then
        actual_exit=0
    else
        actual_exit=$?
    fi
    test "$actual_exit" -eq "$expected_exit"
    if "${runner[@]}" "$shadow"; then
        actual_exit=0
    else
        actual_exit=$?
    fi
    test "$actual_exit" -eq "$expected_exit"
    if [ -n "${extra_exit:-}" ] && [ "$extra_exit" != "-" ]; then
        if "${runner[@]}" "$plain" extra; then
            actual_exit=0
        else
            actual_exit=$?
        fi
        test "$actual_exit" -eq "$extra_exit"
        if "${runner[@]}" "$shadow" extra; then
            actual_exit=0
        else
            actual_exit=$?
        fi
        test "$actual_exit" -eq "$extra_exit"
    fi
done < "$workloads"

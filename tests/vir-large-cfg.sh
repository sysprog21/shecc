#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <stage>" >&2
    exit 1
fi

case "$1" in
    0) compiler=("$PWD/out/shecc") ;;
    2) read -r -a compiler <<< "${TARGET_EXEC:-} $PWD/out/shecc-stage2.elf" ;;
    *)
        echo "stage must be 0 or 2" >&2
        exit 1
        ;;
esac

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT
stats="$workdir/stats"
output="$workdir/output.elf"
"${compiler[@]}" --dump-vir --stats \
    -o "$output" tests/vir-frontend-large-cfg.c \
    > /dev/null 2> "$stats"

runner=()
read -r -a runner <<< "${TARGET_EXEC:-}"
if [ "${#runner[@]}" -gt 0 ]; then
    "${runner[@]}" "$output"
else
    "$output"
fi

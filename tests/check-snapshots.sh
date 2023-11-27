#!/usr/bin/env bash

set -u

readonly SHECC="$PWD/out/shecc"

function check_snapshot() {
    local source="$1"
    local ref="tests/snapshots/$(basename $source .c).json"
    local temp_exe=$(mktemp)
    local temp_json=$(mktemp --suffix .json)

    $SHECC --dump-ir -o $temp_exe $source &>/dev/null
    dot -Tdot_json -o $temp_json CFG.dot
    diff -q <(cat $ref) <(sed -E "/0x[0-9a-f]+/d" $temp_json | jq -c .)
}

for file in tests/*.c; do
    check_snapshot "$file"
done

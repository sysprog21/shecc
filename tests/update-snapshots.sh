#!/usr/bin/env bash

set -u

readonly SHECC="$PWD/out/shecc"

function update_snapshot() {
    local source="$1"
    local dest="tests/snapshots/$(basename $source .c).json"
    local temp_exe=$(mktemp)
    local temp_json=$(mktemp --suffix .json)

    $SHECC --dump-ir -o $temp_exe $source &>/dev/null
    dot -Tdot_json -o $temp_json CFG.dot
    sed -i -E "/0x[0-9a-f]+/d" $temp_json
    jq -c . $temp_json > $dest
}

for file in tests/*.c; do
    update_snapshot "$file"
done

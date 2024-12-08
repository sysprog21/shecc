#!/usr/bin/env bash

set -u

readonly SHECC="$PWD/out/shecc"

if [ "$#" != 1 ]; then
    echo "Usage: $0 <architecture>"
    exit 1
fi

readonly ARCH="$1"

function check_snapshot() {
    local source="$1"
    local ref="tests/snapshots/$(basename $source .c)-$ARCH.json"
    local temp_exe=$(mktemp)
    local temp_json=$(mktemp --suffix .json)

    $SHECC --dump-ir -o $temp_exe $source &>/dev/null
    dot -Tdot_json -o $temp_json CFG.dot
    diff -q <(cat $ref) \
            <(sed -E "/0x[0-9a-f]+/d" $temp_json | \
                jq -S -c '.edges |= sort_by(._gvid) | .objects |= sort_by(._gvid) |
                            .objects |= map_values(.edges |= (. // [] | sort)) |
                            .objects |= map_values(.nodes |= (. // [] | sort)) |
                            .objects |= map_values(.subgraphs |= (. // [] | sort))')
}

for file in tests/*.c; do
    check_snapshot "$file"
done

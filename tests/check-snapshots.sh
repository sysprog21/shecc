#!/usr/bin/env bash

set -u

readonly SHECC="$PWD/out/shecc"

if [ "$#" != 2 ]; then
    echo "Usage: $0 <architecture> <dynlink>"
    exit 1
fi

readonly ARCH="$1"
readonly DYNLINK="$2"

if [ "$DYNLINK" = "1" ]; then
    readonly SHECC_CFLAGS="--dynlink"
    readonly MODE="dynamic"
else
    readonly SHECC_CFLAGS=""
    readonly MODE="static"
fi

function check_snapshot() {
    local source="$1"
    local ref="tests/snapshots/$(basename $source .c)-$ARCH-$MODE.json"
    local temp_exe=$(mktemp)
    local temp_json=$(mktemp --suffix .json)
    local status=0
    local log

    if [ ! -r "$ref" ]; then
        echo "No reference snapshot for $source at $ref"
        rm -f $temp_exe $temp_json
        return 1
    fi

    # Hold on to the compiler's own diagnostic. It is the only thing that says
    # why the IR could not be produced, and reporting the failure without it
    # leaves whoever reads the log with nowhere to start.
    if ! log=$($SHECC $SHECC_CFLAGS --dump-ir -o $temp_exe $source 2>&1); then
        echo "Failed to compile $source for $ARCH ($MODE linking)"
        echo "$log"
        rm -f $temp_exe $temp_json
        return 1
    fi
    dot -Tdot_json -o $temp_json CFG.dot
    diff -q $ref \
            <(sed -E "/0x[0-9a-f]+/d" $temp_json | \
                jq -S -c '.edges |= sort_by(._gvid) | .objects |= sort_by(._gvid) |
                            .objects |= map_values(.edges |= (. // [] | sort)) |
                            .objects |= map_values(.nodes |= (. // [] | sort)) |
                            .objects |= map_values(.subgraphs |= (. // [] | sort))') \
        || { echo "IR of $source does not match $ref"; status=1; }

    rm -f $temp_exe $temp_json
    return $status
}

# Every snapshot is checked, and any one of them failing fails the run. Letting
# the loop carry the status of whichever file happened to come last reported
# success for a mismatch in any earlier one.
ret=0
for file in tests/*.c; do
    check_snapshot "$file" || ret=1
done

exit $ret

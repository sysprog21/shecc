# shellcheck shell=bash
# Shared deterministic compilation for native VIR behavior tests.

vir_build_deterministic()
{
    local compiler_cmd=$1 source=$2 output=$3 repeat=$4 stats=$5
    shift 5

    "$compiler_cmd" "$@" --dump-vir -o "$output" "$source" 2> "$stats"
    "$compiler_cmd" "$@" -o "$repeat" "$source"
    cmp "$output" "$repeat"
}

# Extract exactly one graph and fail if its marker or block body is missing.
vir_extract_function()
{
    local function_name=$1 dump=$2
    awk -v name="$function_name" '
        /^function / {
            if (active)
                exit
            active = ($0 == "function " name)
            if (active)
                found = 1
            next
        }
        active { print; if ($0 ~ /^b[0-9]+/) blocks++ }
        END { if (!found || !blocks) exit 1 }
    ' "$dump"
}

# Resolve a fixture's single local callee through the caller's actual symbol.
vir_extract_callee()
{
    local caller=$1 dump=$2 callee
    callee=$(vir_extract_function "$caller" "$dump" | awk '
        /call(\.[^ ]+)? @/ {
            symbol = $0
            sub(/^.*call(\.[^ ]+)? @/, "", symbol)
            sub(/\(.*/, "", symbol)
            callees[symbol] = 1
        }
        END {
            for (symbol in callees) { count++; name = symbol }
            if (count != 1) exit 1
            print name
        }
    ') || return 1
    vir_extract_function "$callee" "$dump"
}

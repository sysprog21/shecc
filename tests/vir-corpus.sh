#!/usr/bin/env bash

# Differential execution of every standalone tests/*.c fixture: O0 against the
# default level. Fixtures that cannot build standalone, or that a dedicated
# harness already runs at both levels, are listed with their owner in
# tests/vir-corpus-exclusions.txt; an unexpected compiler rejection is a
# failure, never an implicit exclusion.

set -uo pipefail

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <stage> <arch>" >&2
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

compile_timeout=${VIR_CORPUS_COMPILE_TIMEOUT:-300}

runner=()
read -r -a runner <<< "${TARGET_EXEC:-}"

ulimit -c 0
workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

# Each listed fixture maps to its owner, or to nothing on the one target where
# it builds standalone and the corpus checks it after all.
declare -A owner=()
while read -r source source_owner standalone_arch; do
    case "$source" in '' | '#'*) continue ;; esac
    if [ ! -f "$source" ] || [ ! -f "$source_owner" ] || [ -n "${owner[$source]+x}" ]; then
        echo "Invalid or duplicate corpus exclusion: $source $source_owner" >&2
        exit 1
    fi
    case "$standalone_arch" in
        '' | arm | arm64 | riscv | x64) ;;
        *)
            echo "Invalid standalone target for $source: $standalone_arch" >&2
            exit 1
            ;;
    esac
    owner[$source]=$source_owner
    [ "$standalone_arch" = "$2" ] && owner[$source]=
done < tests/vir-corpus-exclusions.txt
while read -r source; do
    case "$source" in '' | '#'*) continue ;; esac
    if [ ! -f "$source" ] || [ -n "${owner[$source]+x}" ]; then
        echo "Invalid or duplicate runtime workload: $source" >&2
        exit 1
    fi
    owner[$source]=tests/vir-native-regressions.sh
done < tests/vir-runtime-workloads.txt

# Output and exit status of one program, as a single string to compare.
run()
{
    local -a args=()
    case "$2" in
        *-argv-index-char-other | *-argv-index-char-offset-other) args=(abc def) ;;
        *-argv-index-char-* | *-argv-index-other) args=(abc) ;;
    esac
    timeout 10 "${runner[@]}" "$1" "${args[@]}" < /dev/null 2>&1
    echo "exit=$?"
}

# Two generated programs join the corpus: more functions than the direct
# backends once had room for in one file, and a function with more blocks than
# one direct function once could have.
{
    echo 'static int h0(int x) { return x + 1; }'
    for i in $(seq 1 299); do
        echo "static int h$i(int x) { return h$((i - 1))(x) + 1; }"
    done
    echo 'int main() { return h299(0) & 255; }'
} > "$workdir/gen-many-functions.c"
{
    echo 'static int big(int x) { int s = 0;'
    for i in $(seq 0 199); do
        echo "    if (x > $i) s = s + $((i % 7)); else s = s - 1;"
    done
    echo '    return s; }'
    echo 'int main() { return (big(150) + big(3)) & 255; }'
} > "$workdir/gen-many-blocks.c"

programs=0
failed=0
delegated=0
for src in tests/*.c "$workdir"/gen-*.c; do
    name=$(basename "$src" .c)
    if [ -n "${owner[$src]:-}" ]; then
        delegated=$((delegated + 1))
        continue
    fi
    # Redirecting the group, not just the command, also silences the shell's
    # report should the compiler crash. Self-compiled compilers under emulation
    # need time to parse compiler-sized unit files.
    {
        timeout "$compile_timeout" "${compiler[@]}" --dump-ir --no-opt -o "$workdir/$name.o0" \
            "$src"
    } > "$workdir/$name.ir" 2> "$workdir/$name.err"
    status=$?
    if [ "$status" -gt 128 ] || [ "$status" -eq 124 ]; then
        echo "FAIL $name: the native O0 build crashes or hangs ($status)" >&2
        failed=$((failed + 1))
        continue
    fi
    if [ "$status" -ne 0 ]; then
        echo "FAIL $name: the native O0 build was rejected ($status)" >&2
        cat "$workdir/$name.ir" "$workdir/$name.err" >&2
        failed=$((failed + 1))
        continue
    fi

    if ! grep -q '^function main$' "$workdir/$name.ir"; then
        echo "FAIL $name: no main; register its multi-input or host harness" >&2
        failed=$((failed + 1))
        continue
    fi
    programs=$((programs + 1))
    expected=$(run "$workdir/$name.o0" "$name")
    if [ "${expected##*exit=}" -eq 124 ]; then
        echo "FAIL $name: the native O0 execution times out" >&2
        failed=$((failed + 1))
        continue
    fi

    if ! {
        timeout "$compile_timeout" "${compiler[@]}" -o "$workdir/$name.vir" "$src"
    } > /dev/null 2>&1; then
        echo "FAIL $name: does not compile" >&2
        failed=$((failed + 1))
        continue
    fi
    actual=$(run "$workdir/$name.vir" "$name")
    if [ "$actual" != "$expected" ]; then
        echo "FAIL $name: output differs" >&2
        diff <(echo "$expected") <(echo "$actual") | head -6 >&2
        failed=$((failed + 1))
    fi
done

if [ "$programs" -eq 0 ]; then
    echo 'FAIL VIR corpus: no runnable programs were checked' >&2
    failed=$((failed + 1))
fi

echo "VIR corpus: $programs programs, $delegated delegated fixtures, $failed failures"
[ "$failed" -eq 0 ]

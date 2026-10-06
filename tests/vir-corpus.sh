#!/usr/bin/env bash

# Differential run over every tests/*.c: each program is built once by the
# native compiler at each optimization level, checking output and status. Files
# without main are checked by multi-input tests.

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

modes=("--vir-opt=1" "--vir-opt=2")
compile_timeout=${VIR_CORPUS_COMPILE_TIMEOUT:-300}

runner=()
read -r -a runner <<< "${TARGET_EXEC:-}"

ulimit -c 0
workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

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
for src in tests/*.c "$workdir"/gen-*.c; do
    name=$(basename "$src" .c)
    case "$name" in
        vir-x64-encoders | vir-x64-and-flags | vir-x64-liveout) continue ;;
    esac
    # Redirecting the group, not just the command, also silences the shell's
    # report should the compiler crash on a file it cannot build. A refusal
    # skips the file; a crash is a failure of its own. Self-compiled compilers
    # under emulation need time to parse compiler-sized unit files.
    {
        timeout "$compile_timeout" "${compiler[@]}" --dump-ir --vir-opt=0 -o "$workdir/$name.o0" \
            "$src"
    } > "$workdir/$name.ir" 2> /dev/null
    status=$?
    if [ "$status" -gt 128 ] || [ "$status" -eq 124 ]; then
        echo "FAIL $name: the native O0 build crashes or hangs ($status)" >&2
        failed=$((failed + 1))
        continue
    fi
    [ "$status" -eq 0 ] || continue

    # A file without main is one part of a multi-file test, not a program.
    grep -q '^function main$' "$workdir/$name.ir" || continue
    programs=$((programs + 1))
    expected=$(run "$workdir/$name.o0" "$name")
    if [ "${expected##*exit=}" = 124 ]; then
        echo "FAIL $name: the native O0 build times out" >&2
        failed=$((failed + 1))
        continue
    fi

    for mode in "${modes[@]}"; do
        read -r -a flags <<< "$mode"
        if ! {
            timeout "$compile_timeout" "${compiler[@]}" "${flags[@]}" \
                -o "$workdir/$name.vir" "$src"
        } > /dev/null 2>&1; then
            echo "FAIL $name ($mode): does not compile" >&2
            failed=$((failed + 1))
            continue
        fi
        actual=$(run "$workdir/$name.vir" "$name")
        if [ "$actual" != "$expected" ]; then
            echo "FAIL $name ($mode): output differs" >&2
            diff <(echo "$expected") <(echo "$actual") | head -6 >&2
            failed=$((failed + 1))
        fi
    done
done

if [ "$programs" -eq 0 ]; then
    echo 'FAIL VIR corpus: no runnable programs were checked' >&2
    failed=$((failed + 1))
fi

echo "VIR corpus: $programs programs, ${#modes[@]} modes, $failed mismatches"
[ "$failed" -eq 0 ]

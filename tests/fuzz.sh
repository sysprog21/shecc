#!/usr/bin/env bash

# Differential fuzzing against the host compiler: tests/fuzz-gen.c writes one
# random program per seed, the host compiler builds the reference, and shecc's
# build of the same program must print the same checksum and exit the same way.
# A mismatch leaves the program behind in out/fuzz-<seed>.c.
#
# FUZZ_SEEDS sets how many seeds run and FUZZ_START selects the first seed.
set -uo pipefail

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

runner=()
read -r -a runner <<< "${TARGET_EXEC:-}"
seeds=${FUZZ_SEEDS:-200}
start=${FUZZ_START:-1}
host_cc=${CC:-cc}

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

"$host_cc" -O -o "$workdir/gen" tests/fuzz-gen.c || exit 1

# Output and exit status of one program, as a single string to compare.
run()
{
    timeout 10 "$@" < /dev/null 2>&1
    echo "exit=$?"
}

failed=0
for ((seed = start; seed < start + seeds; seed++)); do
    src="$workdir/p.c"
    "$workdir/gen" "$seed" > "$src"
    if ! timeout 60 "$host_cc" -O0 -fwrapv -w -include stdio.h \
        -o "$workdir/ref" "$src"; then
        echo "seed $seed: the host compiler rejects the program" >&2
        cp "$src" "out/fuzz-$seed.c"
        failed=$((failed + 1))
        continue
    fi
    expected=$(run "$workdir/ref")
    if [ "${expected##*exit=}" != 0 ]; then
        echo "seed $seed: the reference itself fails: ${expected##*$'\n'}" >&2
        cp "$src" "out/fuzz-$seed.c"
        failed=$((failed + 1))
        continue
    fi
    if ! {
        timeout 60 "${compiler[@]}" -o "$workdir/t" "$src"
    } > /dev/null 2> /dev/null; then
        actual="does not compile"
    else
        actual=$(run ${runner[@]+"${runner[@]}"} "$workdir/t")
    fi
    if [ "$actual" != "$expected" ]; then
        echo "seed $seed: expected ${expected//$'\n'/ }, got ${actual//$'\n'/ }" >&2
        cp "$src" "out/fuzz-$seed.c"
        failed=$((failed + 1))
    fi
done

summary="fuzz: seeds $start to $((start + seeds - 1)), $failed mismatches"
echo "$summary"
[ "$failed" -eq 0 ]

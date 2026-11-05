#!/usr/bin/env bash
# Code size of the configured target, optimized against --no-opt.
#
# Usage: scripts/bench-size.sh [SHECC] [file.c ...]
#
# The size is the file size of the first executable PT_LOAD segment, which is
# the code and what shares its page, so it moves with every instruction saved.
# The ELF file itself is padded and does not. Files are compiled with --no-libc
# so the embedded runtime does not drown the difference.

set -u

shecc=${1:-out/shecc}
[ $# -gt 0 ] && shift
if [ $# -eq 0 ]; then
    set -- tests/native-throughput/*.c
fi

command -v readelf > /dev/null || {
    echo "readelf is required" >&2
    exit 2
}
[ -x "$shecc" ] || {
    echo "$shecc is not built; run make first" >&2
    exit 2
}

work=$(mktemp -d) || exit 2
trap 'rm -rf "$work"' EXIT

# text_size ELF: FileSiz of the first PT_LOAD with the execute flag, in bytes.
text_size()
{
    local hex
    hex=$(readelf -lW "$1" 2> /dev/null \
        | awk '$1 == "LOAD" && $0 ~ / E / { print $5; exit }')
    [ -n "$hex" ] && echo $((hex))
}

printf '%-36s %10s %10s %8s\n' file no-opt opt change
total_opt=0
total_base=0
for src in "$@"; do
    name=$(basename "$src")
    if ! "$shecc" --no-libc -o "$work/opt" "$src" > /dev/null 2>&1 \
        || ! "$shecc" --no-libc --no-opt -o "$work/base" "$src" > /dev/null 2>&1; then
        printf '%-36s %s\n' "$name" "does not compile with --no-libc"
        continue
    fi
    opt=$(text_size "$work/opt")
    base=$(text_size "$work/base")
    [ -n "$opt" ] && [ -n "$base" ] || continue
    printf '%-36s %10d %10d %7d%%\n' "$name" "$base" "$opt" \
        $(((opt - base) * 100 / base))
    total_opt=$((total_opt + opt))
    total_base=$((total_base + base))
done
if [ "$total_base" -eq 0 ]; then
    echo "no input was measured" >&2
    exit 1
fi
printf '%-36s %10d %10d %7d%%\n' total "$total_base" "$total_opt" \
    $(((total_opt - total_base) * 100 / total_base))

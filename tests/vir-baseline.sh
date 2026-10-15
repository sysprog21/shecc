#!/usr/bin/env bash

set -euo pipefail
source tests/benchmark-time.sh

readonly runs="${VIR_BASELINE_RUNS:-5}"
readonly manifest="${VIR_BASELINE_MANIFEST:-tests/vir-baseline-workloads.txt}"
readonly no_timing="${VIR_BASELINE_NO_TIMING:-0}"

if [ "$#" -eq 0 ]; then
    compiler=("$PWD/out/shecc")
else
    compiler=("$@")
fi
read -r -a compiler_flags <<< "${VIR_BASELINE_FLAGS:-}"

if ! [[ "$runs" =~ ^[1-9][0-9]*$ ]]; then
    echo "VIR_BASELINE_RUNS must be a positive integer" >&2
    exit 1
fi

if [ "$no_timing" != 0 ] && [ "$no_timing" != 1 ]; then
    echo "VIR_BASELINE_NO_TIMING must be 0 or 1" >&2
    exit 1
fi

status_sha256()
{
    if command -v sha256sum > /dev/null; then
        sha256sum | awk '{print $1}'
    elif command -v shasum > /dev/null; then
        shasum -a 256 | awk '{print $1}'
    elif command -v openssl > /dev/null; then
        openssl dgst -sha256 | awk '{print $NF}'
    else
        printf 'unavailable\n'
    fi
}

readonly baseline="$(git rev-parse HEAD)"
readonly target="$(sed -n 's@^/\* target: \(.*\) \*/$@\1@p' config | head -1)"
readonly tree_status="$(git status --porcelain=v1)"
if [ -n "$tree_status" ]; then
    readonly tree_state=dirty
    readonly tree_status_sha256="$(printf '%s\n' "$tree_status" | status_sha256)"
else
    readonly tree_state=clean
    readonly tree_status_sha256=none
fi
readonly tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
if [ "$no_timing" = 1 ]; then
    readonly timing_source=not_measured
else
    benchmark_time_source
    readonly timing_source=$BENCHMARK_TIME_SOURCE
fi

printf 'baseline_commit=%s tree_state=%s tree_status_sha256=%s target=%s runner=%s host=%s runs=%s timing=%s\n' \
    "$baseline" "$tree_state" "$tree_status_sha256" "${target:-unknown}" \
    "${TARGET_EXEC:-none}" "$(uname -srm)" "$runs" "$((1 - no_timing))"
printf 'timing_source=%s\n' "$timing_source"
printf 'compiler_argv='
printf ' %q' "${compiler[@]}" "${compiler_flags[@]}"
printf '\n'

while read -r path purpose; do
    case "$path" in
        '' | '#'*)
            continue
            ;;
    esac

    if [ ! -f "$path" ]; then
        echo "missing workload: $path" >&2
        exit 1
    fi

    times="$tmpdir/times"
    : > "$times"
    warm_output="$tmpdir/warm.elf"
    warm_stats="$tmpdir/warm.stats"
    "${compiler[@]}" "${compiler_flags[@]}" --stats -o "$warm_output" "$path" \
        > /dev/null 2> "$warm_stats"
    grep -q '^stats layout ' "$warm_stats"
    grep -q '^stats phase=lower ' "$warm_stats"

    reference_output=""
    reference_stats=""
    for ((run = 1; run <= runs; run++)); do
        output="$tmpdir/output-$run.elf"
        stats="$tmpdir/stats-$run.log"
        if [ "$no_timing" = 0 ]; then
            benchmark_time_command "$tmpdir/time-$run" \
                "${compiler[@]}" "${compiler_flags[@]}" --stats -o "$output" "$path" \
                > /dev/null 2> "$stats"
            cat "$tmpdir/time-$run" >> "$times"
        else
            "${compiler[@]}" "${compiler_flags[@]}" --stats -o "$output" "$path" \
                > /dev/null 2> "$stats"
        fi
        grep -q '^stats layout ' "$stats"
        grep -q '^stats phase=lower ' "$stats"
        if [ -n "$reference_output" ]; then
            cmp -s "$reference_output" "$output"
            cmp -s "$reference_stats" "$stats"
        else
            reference_output="$output"
            reference_stats="$stats"
            cmp -s "$warm_output" "$reference_output"
            cmp -s "$warm_stats" "$reference_stats"
        fi
    done

    if [ "$no_timing" = 0 ]; then
        median_line=$(((runs + 1) / 2))
        median="$(sort -n "$times" | sed -n "${median_line}p")"
    else
        median=not_measured
    fi
    output_bytes="$(wc -c < "$reference_output")"
    printf 'workload=%s purpose=%s warmup=1 runs=%d median_seconds=%s output_bytes=%s deterministic=1\n' \
        "$path" "$purpose" "$runs" "$median" "$output_bytes"
    printf 'compiler_invocation='
    printf ' %q' "${compiler[@]}" "${compiler_flags[@]}" --stats -o '<output.elf>' "$path"
    printf '\n'
    sed -n '/^stats /p' "$tmpdir/stats-1.log"
done < "$manifest"

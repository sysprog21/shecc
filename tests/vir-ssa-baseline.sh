#!/usr/bin/env bash

set -euo pipefail
source tests/benchmark-time.sh

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <stage>" >&2
    exit 1
fi

case "$1" in
    0)
        compiler=("$PWD/out/shecc")
        ;;
    2)
        read -r -a compiler <<< "${TARGET_EXEC:-} $PWD/out/shecc-stage2.elf"
        ;;
    *)
        echo "stage must be 0 or 2" >&2
        exit 1
        ;;
esac

manifest="${VIR_SSA_BASELINE_MANIFEST:-tests/vir-ssa-baseline-workloads.txt}"
no_timing="${VIR_SSA_BASELINE_NO_TIMING:-0}"
runs="${VIR_SSA_BASELINE_RUNS:-3}"
if [ "$no_timing" != 0 ] && [ "$no_timing" != 1 ]; then
    echo "VIR_SSA_BASELINE_NO_TIMING must be 0 or 1" >&2
    exit 1
fi
case "$runs" in
    '' | *[!0-9]*)
        echo "VIR_SSA_BASELINE_RUNS must be a positive integer" >&2
        exit 1
        ;;
esac
if [ "$runs" -lt 2 ]; then
    echo "VIR_SSA_BASELINE_RUNS must be at least 2" >&2
    exit 1
fi
workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT
if [ "$no_timing" = 1 ]; then
    timing_source=not_measured
    rss_source=not_measured
else
    benchmark_time_source
    timing_source=$BENCHMARK_TIME_SOURCE
    if benchmark_time_prepare_rss 2> /dev/null; then
        rss_source=gnu-time-maxrss-kb
    else
        rss_source=not_available
    fi
fi

median_number()
{
    local format=${1:-%.6f}

    awk -v n="$runs" -v format="$format" '
        { sample[NR] = $1 }
        END {
            if (n % 2)
                printf format "\n", sample[(n + 1) / 2]
            else
                printf format "\n", (sample[n / 2] + sample[n / 2 + 1]) / 2
        }
    '
}

while read -r source purpose _expectation; do
    case "$source" in
        '' | '#'*)
            continue
            ;;
    esac
    for opt in 0 1 2; do
        for run in $(seq 1 "$runs"); do
            stats="$workdir/$opt-$run.stats"
            output="$workdir/$opt-$run.elf"
            elapsed="$workdir/$opt-$run.seconds"
            rss="$workdir/$opt-$run.rss"
            if [ "$no_timing" = 1 ]; then
                "${compiler[@]}" --vir-opt="$opt" --dump-vir --stats \
                    -o "$output" "$source" > /dev/null 2> "$stats"
                printf 'not_measured\n' > "$elapsed"
                printf 'not_measured\n' > "$rss"
            else
                benchmark_time_resource_command "$elapsed" "$rss" \
                    "${compiler[@]}" \
                    --vir-opt="$opt" --dump-vir --stats \
                    -o "$output" "$source" > /dev/null 2> "$stats"
            fi
            if [ "$run" -gt 1 ]; then
                cmp -s "$workdir/$opt-1.elf" "$output"
                cmp -s "$workdir/$opt-1.stats" "$stats"
            fi
        done
        stats="$workdir/$opt-1.stats"
        if [ "$no_timing" = 1 ]; then
            seconds=not_measured
            peak_rss_kb=not_measured
        else
            seconds=$(cat "$workdir/$opt-"*.seconds | sort -n | median_number)
            if [ "$rss_source" = gnu-time-maxrss-kb ]; then
                peak_rss_kb=$(cat "$workdir/$opt-"*.rss | sort -n | median_number '%.1f')
            else
                peak_rss_kb=not_measured
            fi
        fi
        output_bytes=$(wc -c < "$workdir/$opt-1.elf")
        VIR_SSA_BASELINE_STAGE="$1" VIR_SSA_BASELINE_RUNS="$runs" \
            VIR_SSA_BASELINE_SECONDS="$seconds" \
            VIR_SSA_BASELINE_TIMING_SOURCE="$timing_source" \
            VIR_SSA_BASELINE_PEAK_RSS_KB="$peak_rss_kb" \
            VIR_SSA_BASELINE_RSS_SOURCE="$rss_source" \
            VIR_SSA_BASELINE_OUTPUT_BYTES="$output_bytes" \
            awk -v source="$source" -v purpose="$purpose" -v opt="$opt" '
            END {
                printf("ssa-baseline stage=%s opt=%s workload=%s purpose=%s ",
                       ENVIRON["VIR_SSA_BASELINE_STAGE"], opt, source, purpose)
                printf("runs=%s seconds=%s timing_source=%s ",
                       ENVIRON["VIR_SSA_BASELINE_RUNS"],
                       ENVIRON["VIR_SSA_BASELINE_SECONDS"],
                       ENVIRON["VIR_SSA_BASELINE_TIMING_SOURCE"])
                printf("median_peak_rss_kb=%s rss_source=%s output_bytes=%s ",
                       ENVIRON["VIR_SSA_BASELINE_PEAK_RSS_KB"],
                       ENVIRON["VIR_SSA_BASELINE_RSS_SOURCE"],
                       ENVIRON["VIR_SSA_BASELINE_OUTPUT_BYTES"])
                printf("\n")
            }
        ' < "$stats"
    done
done < "$manifest"

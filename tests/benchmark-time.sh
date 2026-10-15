# Shared high-resolution timing for host-only compiler benchmarks.

BENCHMARK_TIME_SOURCE=
BENCHMARK_TIME_TOOL=

benchmark_time_source()
{
    if [ -n "${EPOCHREALTIME:-}" ]; then
        BENCHMARK_TIME_SOURCE=bash-epochrealtime-us
    else
        BENCHMARK_TIME_TOOL=$(benchmark_time_tool)
        BENCHMARK_TIME_SOURCE=gnu-time-centisecond
    fi
}

benchmark_time_tool()
{
    local tool

    if command -v gtime > /dev/null 2>&1; then
        tool=$(command -v gtime)
    elif [ -x /usr/bin/time ]; then
        tool=/usr/bin/time
    else
        echo 'GNU time (gtime or /usr/bin/time) is required for fallback timing or peak RSS' >&2
        return 1
    fi

    if ! "$tool" --version 2>&1 | grep -qi 'GNU time'; then
        echo 'GNU time is required for fallback timing or peak RSS (BSD time is not supported)' >&2
        return 1
    fi

    printf '%s\n' "$tool"
}

benchmark_time_prepare_rss()
{
    if [ -z "$BENCHMARK_TIME_TOOL" ]; then
        BENCHMARK_TIME_TOOL=$(benchmark_time_tool)
    fi
}

benchmark_time_write_elapsed()
{
    local elapsed_file=$1
    local start_us=$2
    local finish_us=$3
    local elapsed_us=$((finish_us - start_us))

    printf '%d.%06d\n' "$((elapsed_us / 1000000))" \
        "$((elapsed_us % 1000000))" > "$elapsed_file"
}

benchmark_time_resource_command()
{
    local elapsed_file=$1
    local rss_file=$2
    shift 2

    if [ -n "${EPOCHREALTIME:-}" ]; then
        local start_us=${EPOCHREALTIME/./}
        if benchmark_time_prepare_rss 2> /dev/null; then
            "$BENCHMARK_TIME_TOOL" -f '%M' -o "$rss_file" "$@"
        else
            "$@"
            printf 'not_measured\n' > "$rss_file"
        fi
        local finish_us=${EPOCHREALTIME/./}
        benchmark_time_write_elapsed "$elapsed_file" "$start_us" "$finish_us"
    else
        benchmark_time_prepare_rss
        "$BENCHMARK_TIME_TOOL" -f '%e %M' -o "$elapsed_file" "$@"
        local elapsed rss
        read -r elapsed rss < "$elapsed_file"
        printf '%s\n' "$elapsed" > "$elapsed_file"
        printf '%s\n' "$rss" > "$rss_file"
    fi
}

benchmark_time_command()
{
    local elapsed_file=$1
    shift

    if [ -n "${EPOCHREALTIME:-}" ]; then
        local start_us=${EPOCHREALTIME/./}
        "$@"
        local finish_us=${EPOCHREALTIME/./}
        benchmark_time_write_elapsed "$elapsed_file" "$start_us" "$finish_us"
    else
        benchmark_time_prepare_rss
        "$BENCHMARK_TIME_TOOL" -f '%e' -o "$elapsed_file" "$@"
    fi
}

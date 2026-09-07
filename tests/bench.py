#!/usr/bin/env python3

import subprocess
import time
import resource
import statistics
import json
import argparse

DEFAULT_RUNS = 5
LINE_SEPARATOR = "=" * 70

def main():
    parser = argparse.ArgumentParser(
        description="Run benchmarks for shecc"
    )
    parser.add_argument(
        "--hostcc",
        default="gcc",
        choices=["cc", "gcc", "clang"],
        help="Host C Compiler (default: %(default)s)"
    )
    parser.add_argument(
        "--arch",
        default="arm",
        choices=["arm", "riscv"],
        help="Target architecture (default: %(default)s)"
    )
    parser.add_argument(
        "--dynlink",
        action="store_true",
        help="Enable dynamic linking (default: static linking)"
    )
    parser.add_argument(
        "--output-json",
        default="out/benchmark.json",
        help="Output JSON file name (default: %(default)s)"
    )
    parser.add_argument(
        "--runs",
        type=int,
        default=5,
        help=f"Number of runs (default: %(default)s)"
    )
    args = parser.parse_args()

    # Check whether the given 'runs' is valid
    if args.runs < 1:
        parser.error("--runs must be at least 1")

    # "cc" and "gcc" are equivalent
    if args.hostcc == "cc":
        args.hostcc = "gcc"

    link_mode = "dynamic" if args.dynlink else "static"
    available_config = f"(HOSTCC, ARCH, DYNLINK)"
    config = f"({args.hostcc}, {args.arch}, {link_mode})"

    # Measure execution time and max resident set size (RSS)
    print(f"==> config: {available_config}={config}")
    print(f"==> runs: {args.runs}")
    print(f"==> output_json: {args.output_json}")

    build_cmd = ["make", f"CC={args.hostcc}", f"ARCH={args.arch}",
                 f"DYNLINK={int(args.dynlink)}", "--silent"]
    clean_cmd = ["make", "distclean", "--silent"]
    exec_time = []
    max_rss = 0

    for i in range(args.runs):
        subprocess.run(clean_cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=True
        )

        print(f"Running ({i + 1}/{args.runs})...")

        start = time.monotonic()
        subprocess.run(build_cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=True
        )
        end = time.monotonic()

        usage = resource.getrusage(resource.RUSAGE_CHILDREN)

        exec_time.append(end - start)
        max_rss = max(max_rss, usage.ru_maxrss)

    ave_time = statistics.mean(exec_time)

    # Clean the build after generating benchmark results
    subprocess.run(clean_cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=True
    )

    # Store the results to a json file
    benchmark_name = args.output_json
    result = [
        {
            "name": "Average Execution Time",
            "unit": "second",
            "value": ave_time,
            "runs": args.runs,
            "config": f"{available_config}={config}"
        },
        {
            "name": "Maximum Resident Set Size",
            "unit": "KBytes",
            "value": max_rss,
            "runs": args.runs,
            "config": f"{available_config}={config}"
        }
    ]

    with open(benchmark_name, "w") as f:
        # Append a newline character since dump() doesn't append one
        # at the end of file
        json.dump(result, f, indent=4)
        f.write("\n")

    # Output the results
    print("\n" + LINE_SEPARATOR)
    print("Benchmark results")
    print(f"Config     : {available_config}={config}")
    print(f"Output file: {args.output_json}")
    print(LINE_SEPARATOR)
    for res in result:
        print(f"    {res['name']:30s}: {res['value']} {res['unit']} ({res['runs']} runs)")
    print(LINE_SEPARATOR)

    
if __name__ == "__main__":
    main()

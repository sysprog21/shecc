# Native ARM64 and x86-64 throughput

Run manually on an idle native AArch64 or x86-64 Linux host after configuring/building
shecc for the matching architecture:

```
make benchmark-native-throughput
# Optional settings; output directory must not already exist:
python3 tests/native-throughput.py --shecc out/shecc --output /tmp/native-run \
    --cpu 3 --target 0.8
```

This target is excluded from `make check`. It requires Python 3.9+, GCC, Linux
CPU affinity, and native execution. It does not invoke sudo, change governors,
or accept QEMU timing. The selected CPU defaults to the last allowed CPU.
Remove or rename an earlier output directory before another Make invocation.

The seven workloads cover arithmetic, branches, pointers, recursion, calls,
sorting, and indexed access. Every binary returns failure unless its full 32-bit
checksum matches the literal expected result. Both compilers use the same source;
shecc uses `--no-libc` at its default optimization, GCC uses `-O1 -fwrapv` and
static minimal startup. One warmup precedes nine rotating rounds of eight executions per sample.
Wall and child CPU medians and raw samples are recorded separately. Throughput
is GCC time divided by shecc time; every fixture must reach the configurable
target (default 0.8) in both wall and child CPU time.

`results.json` records commands, expected checksums, successful execution counts,
source/compiler/binary SHA256 before and after, GCC version, affinity, governor,
and system load. Build/runtime failure writes `failure.json` and returns nonzero;
completed results remain available. Results describe these seven synthetic
workloads on the measured host.

## Latest results

Native x86-64: AMD Ryzen Threadripper 2990WX, Linux 6.8, GCC 14.2.0
`-O1 -fwrapv`, CPU 63, unchanged schedutil governor, nine rotating rounds of
eight executions after warmup, `--target 1`. Every checksum execution passed and
the source/compiler/binary SHA256 checks were unchanged. Throughput is GCC time
divided by shecc time; values above 1 favor shecc.

| Workload | Wall | Child CPU |
| --- | ---: | ---: |
| rng | 150.09x | 265.18x |
| branch | 0.97x | 0.97x |
| pointer | 1.41x | 1.48x |
| recursion | 1.18x | 1.19x |
| calls | 1.00x | 1.00x |
| sort | 1.11x | 1.11x |
| indexed | 1.00x | 1.00x |

The wall geometric mean is 2.23x and the child-CPU geometric mean 2.43x. RNG's
constant-trip, effect-free affine loop is summarized away, so its process time
is startup dominated. Five of seven workloads meet the strict per-workload
target; branch stays below GCC and calls is at parity, so the command returns
status 1. The geometric mean is descriptive and does not replace the gate.

```sh
python3 tests/native-throughput.py --shecc out/shecc \
    --output out/throughput --rounds 9 --runs 8 --target 1
```

To compare two compilers, save the baseline binary before rebuilding and run
the same command against each, serially and without concurrent build or test
jobs. Each output directory contains `results.json`, commands, environment, raw
samples, and SHA256 provenance. Use fresh output directories for another
measurement. Replace this section with the newest run rather than appending.

## Compilation time

`tests/compiler-throughput.py` measures native executable compilation against
GCC `-O1 -fwrapv`, including process startup, preprocessing, optimization, code
emission, and linking. It checks both output ELF targets, warms each compiler,
alternates their order, and records raw wall/child CPU samples and medians.
Use an otherwise idle CPU; the small programs emphasize startup and linking.

```sh
python3 tests/compiler-throughput.py --shecc out/shecc \
    --rounds 7 --batch 3 --output out/compile-programs.json \
    tests/native-throughput/*.c
python3 tests/compiler-throughput.py --shecc out/shecc \
    --rounds 5 --output out/compile-self.json src/main.c
```

Add `--no-libc` to measure programs that omit shecc's bundled libc.

These measure compilation speed separately from the generated-program
throughput measurements above. Results depend on the input and native host.

### Profiling compilation with uftrace

Build a separate instrumented host compiler after `make` for the native target:
`src/main.c` includes the generated `config` and `out/libc.inc`.
Use a shallow trace to find expensive phases, then restrict recording to a hot
function. Disabling host inlining exposes helper call counts but increases tracing
overhead; instrumented timings are for attribution, not speedup claims.

```sh
mkdir -p out/uftrace-compile
cc -O2 -g -std=c99 -pedantic -fwrapv -pg -fno-omit-frame-pointer \
    src/main.c -o out/uftrace-compile/shecc-profile
uftrace record --no-libcall -D 5 -d out/uftrace-compile/phases \
    out/uftrace-compile/shecc-profile -o out/uftrace-compile/self.elf src/main.c
uftrace report -d out/uftrace-compile/phases -s self --color=no
cc -O2 -g -std=c99 -pedantic -fwrapv -pg -fno-inline \
    -fno-omit-frame-pointer src/main.c -o out/uftrace-compile/shecc-detail
uftrace record --no-libcall -F vir_dce_with_stats -D 4 \
    -d out/uftrace-compile/dce out/uftrace-compile/shecc-detail \
    -o out/uftrace-compile/self-detail.elf src/main.c
uftrace report -d out/uftrace-compile/dce -s self --color=no
```

Compare an optimization by self-compiling a frozen copy of `src`, `config`, and
`out/libc.inc` with the before and after compilers, alternating rounds after
warmup, and require byte-identical ELF output from every pair. Prefer
instruction counts (`perf stat -e instructions:u`) over wall time on a loaded
host.

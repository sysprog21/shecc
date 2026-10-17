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

## Native measurements

October 6, 2026: nine rounds of eight executions on each host; all 1,022 checksum
executions passed per host. Results below are throughput relative to GCC `-O1`.
ARM64: native eMag, Linux 6.8, GCC 13.3, CPU 31, performance governor restored
after measurement. x86-64: Ryzen Threadripper 2990WX, Linux 6.8, GCC 14.2,
CPU 63, unchanged schedutil governor.

| Workload | ARM64 wall | ARM64 CPU | x86-64 wall | x86-64 CPU |
| --- | ---: | ---: | ---: | ---: |
| rng | 83.74% | 83.41% | 80.74% | 80.28% |
| branch | 92.50% | 92.35% | 91.89% | 91.66% |
| pointer | 89.34% | 88.83% | 82.83% | 81.33% |
| recursion | 89.88% | 89.69% | 92.93% | 92.71% |
| calls | 87.47% | 86.12% | 101.52% | 101.83% |
| sort | 81.52% | 81.28% | 86.57% | 86.48% |
| indexed | 89.29% | 88.85% | 99.67% | 99.73% |

## Essential VIR optimizations: October 16, 2026

The compiler now summarizes proven finite affine arithmetic loops, eliminates
direct/additive tail recursion, inlines bounded pure leaves, and lowers
power-of-two integer multiplication/division/remainder. The working-tree
compiler saved before these changes includes the earlier bitwise normalization.
Both comparisons used the unchanged seven workload sources, GCC 14.2.0
`-O1 -fwrapv`, Linux 6.8, CPU 63, and the existing schedutil governor. Nine
rotating rounds of eight executions followed warmup; each comparison passed
1,022 full checksum executions and its source/compiler/binary immutability check.

Throughput is GCC time divided by shecc time; values above 1 favor shecc.

| Workload | Before: wall | After: wall | After: child CPU |
| --- | ---: | ---: | ---: |
| rng | 1.0002x | 148.7932x | 262.9735x |
| branch | 0.9539x | 0.9541x | 0.9528x |
| pointer | 1.0036x | 1.0080x | 1.0084x |
| recursion | 0.9845x | 1.1926x | 1.2008x |
| calls | 0.9995x | 1.0023x | 1.0022x |
| sort | 0.9775x | 0.9767x | 0.9763x |
| indexed | 1.0029x | 0.9998x | 1.0001x |

The wall geometric mean rose from 0.9887x to 2.0775x; the new child-CPU geometric
mean is 2.2554x. The RNG result is driven by eliminating its entire constant-trip,
effect-free affine loop: its process time becomes startup dominated. Recursion
improves by 1.21x relative to the previous compiler and reaches 1.19x GCC's wall
throughput. The other workloads remain close to their previous performance.

The strict `--target 1` gate passes four of seven workloads, so both commands
below return status 1 for the unmet per-workload target. Branch and sort remain
below GCC, and indexed wall throughput is within 0.03% of parity. The geometric
mean is descriptive and does not replace the existing per-workload gate.

```sh
# Save the baseline compiler before applying the optimization changes.
cp out/shecc out/shecc-before-essential
# Build/check the updated compiler, then measure serially without build/test jobs.
python3 tests/native-throughput.py --shecc out/shecc-before-essential \
    --output out/essential-baseline-final --rounds 9 --runs 8 --target 1
python3 tests/native-throughput.py --shecc out/shecc \
    --output out/essential-final --rounds 9 --runs 8 --target 1
```

Each output directory contains `results.json`, commands, environment, raw samples,
and SHA256 provenance. Use fresh output directories for another measurement.

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

Build a separate instrumented host compiler after configuring the native target.
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

October 16, 2026, uftrace 0.15, native x86-64: the detailed self-compilation
trace identified repeated use-list dispatch and effect scans in pointer-address
scheduling, plus unconditional rebuilding of block positions during DCE. The
updated scheduler keeps a forward effect-barrier cursor, skips operand-use scans
when no motion is possible, and resolves each candidate value user directly.
Use accessors dispatch once on the owner tag, and DCE repairs positions only in
blocks it changes. Volatile accesses, calls, and stores retain their ordering.

On the same frozen compiler source, the DCE trace records 51,014 calls to
`vir_use_block` instead of 1,312,819, 49,219 calls to `vir_use_order` instead of
542,664, and 26,017 position repairs instead of 191,852. These are helper counts
inside DCE, not whole-compilation counts. Both traces run 4,296 DCE invocations
and 95,920 block scheduling visits.

An uninstrumented comparison on CPU 63 used a frozen copy of `src`, `config`,
and `out/libc.inc`, two warmup rounds, then 20 alternating before/after rounds.
The host governor was unchanged. Median wall time fell from 0.5415 s to 0.5221 s
(1.0371x throughput, 3.58% less time); median child CPU time fell from 0.5412 s
to 0.5218 s (1.0371x). Every pair produced byte-identical ELF output. Compiler
and complete frozen-input SHA256 checks passed before/after measurement. These
results describe native self-compilation on this host, not all compilation
workloads. The raw samples and profiles stayed in the gitignored `out/` tree and
are not part of the repository; rerun the commands above to reproduce them.
This comparison isolates the DCE and scheduling changes: its baseline already
contained the other optimizations described in this file, unlike the throughput
table above, whose baseline predates them.

Validation passed `make ARCH=x64 USE_QEMU=0 check` and
`make ARCH=x64 USE_QEMU=0 check-vir-core check-vir-sanitizer`, including both
3,075-test compiler stages, 341 corpus programs, and 200 differential fuzz seeds.
A scheduling regression exercises multiple ordinary-load/volatile-store groups
and repeated DCE. Each address is used only after every group, so the volatile
store alone must stop it; removing the barrier check makes the test fail.

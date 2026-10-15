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

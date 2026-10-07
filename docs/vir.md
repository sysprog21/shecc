# VIR — Value Intermediate Representation

VIR is shecc's target-independent SSA intermediate representation. Its
organizing rule is:

> CFG for control, graphs for values, order for effects.

## Pipeline

Every function and global initializer is constructed as VIR. The compiler
uses one pipeline for Armv7, AArch64, RV32, and x86-64:

```text
C frontend → typed VIR → shared ABI lowering and register allocation
           → allocated PH2 → machine peepholes and target lowering
           → target emitter → ELF
```

VIR owns values, control flow, and ordered effects. The shared lowerer assigns
physical registers, resolves edge copies, handles ABI boundaries, and allocates
spill storage. `ph2_ir_t` holds the resulting machine operations consumed by
all four emitters. Function graphs and analysis tables are released after
lowering; PH2 contains the operands needed for emission.

Reachability follows calls and function addresses from `main` and global
initialization. Used functions must have definitions unless dynamic linking
can resolve them. Unsupported source forms and invalid graphs produce
compiler diagnostics.

## Why VIR

VIR is SSA for scalar values: each value is defined once, and the verifier
checks dominance and use-def consistency. What separates it from classic SSA
is how the SSA form is expressed and what travels with it.

### Compared with classic SSA

- **Block parameters instead of phi instructions.** A jump edge carries
  arguments and the receiving block declares parameters (`edge->args`,
  `vir_block_add_param()`), as in MLIR and Cranelift. Adding, removing,
  redirecting (`vir_edge_redirect()`) or splitting (`vir_edge_split()`) an
  edge moves its arguments with it, so there is no phi operand list to keep in
  step with the predecessor list.
- **Edge arguments are the copies.** The values an edge passes are exactly the
  values that must move, so the shared lowering resolves each edge as one
  parallel copy. It materializes a copy block only when an edge needs one,
  emits nothing when the arguments already sit in their parameters' registers,
  and moves all-register edges without a round trip through spill homes.
- **Ordered memory effects.** Loads, stores, volatile accesses and calls form
  an ordered effect list in each block, with their widths and types. That is
  enough for the optimizer to decide which reorderings and removals are legal
  without Memory SSA or general alias analysis, which keeps the compiler small
  enough to stay self-hosting.
- **Calls carry fixed-prototype signatures.** Each call holds its arguments and
  signature, so ABI handling (paired registers on 32-bit targets, variadic
  saves, results) lives once in the shared lowering instead of in each of the
  four backends.
- **A standalone, verified core.** The VIR core landed before any frontend used
  it, with unit tests (`tests/vir-core.c`) and a structural fuzzer
  (`tests/vir-fuzz.c`), so its invariants were tested before anything depended
  on them. Every graph is verified after construction and again after
  optimization, at every level, and dumps are deterministic.

### Why shecc moves to VIR

- **The legacy pipeline paid for its phis in memory.** Unwound phis were stored
  to a stack slot and read straight back, and the register allocator gave up
  its register state at block boundaries, so values crossing an edge went
  through memory. Later passes such as `collapse_slot_roundtrip()` only undid
  part of that after the fact. Block parameters let lowering keep those values
  in registers from the start.
- **One lowering for four targets.** Register allocation, edge copies, ABI
  boundaries and spill storage are target-independent, so a fix or an
  improvement there reaches Armv7, AArch64, RV32 and x86-64 at once. Each
  backend keeps instruction selection, encoding and its target-specific
  folding and branch layout.
- **Optimization on a sound base.** SCCP, CSE, GVN, LICM, strength reduction
  and DCE work on typed values with explicit control flow and effect order,
  and the verifier rejects a pass that breaks an invariant instead of letting
  it reach code generation.
- **Measured result.** On the native throughput fixtures, generated code moved
  from 30-81% of GCC `-O1` speed to 80-102% on x86-64;
  `tests/native-throughput/README.md` records the measurements.
  The cost is compile time: self-compilation is slower than under the legacy
  pipeline, though still several times faster than GCC `-O1`.
- **Still small and self-hosting.** VIR is plain C with deterministic
  construction and no global interning, Memory SSA or alias-analysis
  framework, so shecc remains a compiler a reader can follow end to end.

## Representation contract

- Values and blocks have stable integer IDs within a graph. Construction,
  traversal, dumps, and lowering must be deterministic and independent of
  host addresses and hash-table iteration order.
- Pure computations retain their defining block, insertion position, and
  typed operands. Construction may fold constants, remove identities, and
  order commutative operands when their semantics permit it. Common unary
  and binary operands stay inline; unusual arities use side storage.
- Blocks and edges carry control. SSA merges use block parameters and edge
  arguments. Edge mutation must preserve argument indices, types, and reverse
  uses, including duplicate successors and loop backedges.
- Loads, stores, calls, and volatile accesses are ordered effects. Pure-value
  CSE and LICM do not reorder or speculate them. DCE can remove unused ordinary
  loads and exact same-address, same-type ordinary stores overwritten before
  an observing effect. Used loads, calls, volatile accesses, and intervening
  stores remain barriers. CFG rewrites may remove unreachable effects.
- Integer and pointer widths, alignment, constant bits, casts, and signed or
  unsigned operations follow the selected target. Pointer values use target
  pointer width.
- Arithmetic and signed or unsigned relational comparisons use i32/i64;
  equality accepts same-typed values. i8/i16 describe ABI, cast, and memory
  boundaries. The frontend applies C integer promotions before arithmetic.
  Pointer loads retain pointer type; ABI metadata retains integer signedness
  and `_Bool` identity.
- Source variables, SSA values, physical storage, and analysis data have
  separate identities. Dominance, loops, SCCP, GVN, liveness, and allocation
  tables belong to their passes and are invalidated after relevant CFG changes.

VIR keeps explicit CFG and conventional dominance. It remains small,
deterministic, implemented in C, and self-hosting. Global interning, Memory
SSA, and general alias analysis are not prerequisites for constructing a graph.

## Compiler options

VIR is the normal compilation path. Optimization is on by default (O2 below);
`--no-opt` selects O0. O0 exists for debugging and as a baseline for isolating
optimizer faults. It does not compile faster: the unoptimized graph leaves more
values for machine lowering.

| Option | Behavior |
| --- | --- |
| `--dump-ir` | Print optimized live-function VIR to standard output. |
| `--dump-vir` | Print optimized live-function VIR to standard error. If both dump options are present, standard output is used. |
| `--dot` | Write the live-function VIR CFG in Graphviz DOT format and stop before machine emission. |
| `--stats` | Print deterministic layout, phase, VIR, PH2, and arena counters to standard error. |
| `--no-opt` | Skip the optional VIR optimizations (O0). |

Textual graphs start with `function NAME`, followed by block definitions.
Tests resolve private functions and globals through their actual call and
address operands; private symbol spelling is not a source-language interface.

`--no-libc` excludes the embedded C library. Compiler builtins remain
available. `--dynlink`, `+m`, preprocessing, and include-path options retain
their existing target-specific behavior.

## Optimization and verification

Frontend completion always seals SSA bindings, removes unreachable blocks,
and verifies the constructed graph. Each live graph is verified again after
optimization, including at O0.

The compiler invokes these passes in order:

1. Remove unreachable blocks.
2. SCCP and CFG simplification.
3. Local CSE.
4. GVN.
5. LICM and pointer-induction strength reduction.
6. Local CSE and DCE, including ordinary overwritten-store elimination.
7. CFG simplification, then a final local CSE and DCE over the simplified graph.
8. Full graph verification before dumps and machine lowering.

| Level | Scheduled transformations |
| --- | --- |
| O0 | Construction folds, unreachable-block removal, and mandatory verification; optional optimization passes return without transforming the graph. |
| O2 (default) | SCCP, CFG simplification, local CSE, GVN, LICM, strength reduction, and DCE. |

Pass-local tables are temporary. O0 does not allocate the optional CSE, GVN,
loop, or optimization worklists. Mandatory verification still checks dominance,
types, use-def links, edges, and ordered effects.

## Validation

| Command | Coverage |
| --- | --- |
| `make check-vir-core` | Builders, core invariants, and transformations. |
| `make check-vir-lower` | Shared allocation and lowering invariants, plus an executable smoke test for the configured target. |
| `make check-vir` | Core, frontend, optimization policy, self-hosted VIR, fuzzing, and sanitizer gates. |
| `make check` | Full configured-target stage 0/2 behavior, ABI, and VIR checks. |
| `make check-all-targets` | Bootstrap and regression sweep across all four targets, using each configured runner. |
| `make check-vir-native-x64-stage0 check-vir-native-x64-stage2` | Native x86-64 execution and graph quality checks. |
| `make check-vir-native-arm64-stage0 check-vir-native-arm64-stage2` | Native AArch64 execution and graph quality checks using the configured `TARGET_EXEC`. |
| `make check-vir-native-regressions-stage0 check-vir-native-regressions-stage2` | Native frontend, ABI, and runtime regression fixtures with both compiler stages. |
| `make check-vir-multi-input-stage0 check-vir-multi-input-stage2` | Translation-unit isolation, private symbol identity, and cross-input linking. |
| `make check-vir-corpus-stage0` | Execution equivalence of the default build against O0 across the C fixture corpus. |
| `bash tests/vir-qualifiers.sh out/shecc` | Pointer and volatile qualifier execution and graph checks at O0 and the default level. |

Corpus compilation defaults to 300 seconds per build. Set
`VIR_CORPUS_COMPILE_TIMEOUT` for slower self-compiled runs under emulation;
program execution retains its 10-second limit.

Changes must preserve execution, deterministic dumps and output, a clean
verifier, and stage-1/stage-2 bootstrap identity. The AArch64 native harness
adds its default dynamic QEMU sysroot only when the configured runner has not
already supplied `-L`.

## Implementation map

| Concern | Location |
| --- | --- |
| Values, CFG, builders, verification, and optimizations | `src/vir.h`, `src/vir.c` |
| Native frontend and sealed SSA construction | `src/vir-frontend.c`, `src/vir-frontend.h` |
| Shared ABI lowering, register allocation, and spills | `src/vir-lower.c`, `src/vir-lower.h` |
| Machine emission | `src/arm-codegen.c`, `src/arm64-codegen.c`, `src/riscv-codegen.c`, `src/x64-codegen.c` |
| Pass scheduling, reachability, dumps, and graph lifetime | `src/main.c` |
| Core and frontend fixtures | `tests/vir-core.c`, `tests/vir-frontend-*.c`, and their manifests |
| Native execution and graph quality harnesses | `tests/vir-direct-*.sh`, `tests/vir-qualifiers.sh` |
| Aggregate and bootstrap gates | `Makefile` targets beginning with `check-vir` |

This page defines the current VIR contract and validation entry points.
Implementation history belongs in [DONE.md](../DONE.md), and remaining work
belongs in [TODO.md](../TODO.md). Keep the project overview in
[README.md](../README.md) linked to this reference.

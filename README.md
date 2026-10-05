# shecc : self-hosting and educational C optimizing compiler

<p align="center"><img src="https://user-images.githubusercontent.com/18013815/91671374-b2f0db00-eb58-11ea-8d55-858e9fb160c0.png" alt="logo image" width=40%></p>

## Introduction

`shecc` is built from scratch, targeting 32-bit Arm, AArch64, 32-bit RISC-V,
and x86-64, as a self-compiling compiler for a subset of the C language.
Despite its simplistic nature, it is capable of performing basic optimization strategies as a standalone optimizing compiler.

### Features

* Generate executable Linux ELF binaries for ARMv7-A, AArch64, RV32IM, and x86-64.
* Provide a minimal C standard library for basic I/O on GNU/Linux.
* The cross-compiler is written in ANSI C, making it compatible with most platforms.
* Include a self-contained C front-end with an integrated machine code generator; no external assembler or linker needed.
* Use [VIR](docs/vir.md), a value-centric SSA representation, for target-independent optimization.
* Share ABI lowering, register allocation, and spill handling across all four targets.
* Lower VIR to allocated machine IR before target-specific instruction emission.
* Support dynamic linking to allow generated executables to run with glibc.
* Emit both ELF32 (Arm, RISC-V) and ELF64 (AArch64, x86-64) images; the ELF class follows the target pointer width.

## Compatibility

`shecc` is capable of compiling C source files written in the following
syntax:
* data types: `char`, `short`, `int`, `_Bool`, `void`, `struct`, `union`, `enum`, `typedef`, and pointer types
* condition statements: `if`, `else`, `while`, `for`, `do-while`, `switch`, `case`, `default`, `break`, `continue`, `return`, `goto`
                        with labels, and general expressions
* operators: all arithmetic, logical, bitwise, and assignment operators including compound assignments
  (`+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`)
* arrays: global/local arrays with initializers, multi-dimensional arrays
* functions: function declarations, definitions, and calls with fixed arguments
* variadic functions: basic support via direct pointer arithmetic (no `<stdarg.h>`)
* typedef: type aliasing including typedef pointers (`typedef int *ptr_t;`)
* pointers: full pointer arithmetic, multi-level pointer dereference (`***ptr`)
* global/local variable initializations for all supported data types
    - e.g. `int i = [expr];`, `int arr[] = {1, 2, 3};`
* preprocessor directives: `#define`, `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`, `#undef`, `#error`, and `#include`
* function-like macros with parameters, `__VA_ARGS__`, stringification (`#`), and token pasting (`##`)

The Arm backend targets armv7hf with the Linux ABI, verified on Raspberry Pi 3.
The AArch64 backend follows AAPCS64 and supports static and eager-bound dynamic
linking, verified with QEMU AArch64 on eMag. Its images separate the load
segments by 64 KiB so they load under any of the 4 KiB, 16 KiB and 64 KiB
translation granules AArch64 Linux may be configured with.
The RISC-V backend targets RV32IM, verified with QEMU.
The x86-64 backend follows the System V AMD64 ABI and runs natively on an
x86-64 GNU/Linux host, so no emulator is involved.

## Bootstrapping

The steps to validate `shecc` bootstrapping:
1. `stage0`: `shecc` source code is initially compiled using an ordinary compiler
   which generates a native executable. The generated compiler can be used as a
   cross-compiler.
2. `stage1`: The built binary reads its own source code as input and generates a
   binary for the selected target.
3. `stage2`: The generated target binary is invoked with its own source code as
   input and generates another target binary. It runs natively for x86-64 and on
   the Arm boards the build system recognizes; every other case, including the
   RISC-V target, goes through QEMU.
4. `bootstrap`: Build the `stage1` and `stage2` compilers, and verify that they are
   byte-wise identical. If so, `shecc` can compile its own source code and produce
   new versions of that same program.

## Prerequisites

Code generator in `shecc` does not rely on external utilities. You only need
ordinary C compilers such as `gcc` and `clang`. However, `shecc` would bootstrap
itself, so the target binaries have to run somewhere. Building for `x64` on an
x86-64 GNU/Linux host needs nothing extra. Building for Arm or RISC-V on such a
host requires ISA emulation; install QEMU for Arm/RISC-V user emulation:

```shell
$ sudo apt-get install qemu-user
```

The build system is able to verify whether the running machine can perform native
execution without QEMU. The host machine may install the prebuilt
[fastfetch](https://github.com/fastfetch-cli/fastfetch/), which allows the build
system to determine whether native execution can be enabled.

It is still possible to build `shecc` on macOS or Microsoft Windows. However,
the second stage bootstrapping would fail due to `qemu-arm` absence, and the
`x64` target expects an x86-64 GNU/Linux host to execute its own output.

### Additional packages

The dynamic linking mode needs an ELF interpreter and the matching glibc for the
target. The `x64` target resolves both from the host system, so it needs nothing
beyond an x86-64 GNU/Linux installation. The Arm, AArch64, and RISC-V targets need a
cross-compile GNU toolchain to obtain them.

For the Arm architecture, you can install the ARM GNU toolchain using `apt-get`:

```shell
$ sudo apt-get install gcc-arm-linux-gnueabihf
```
Another approach is to manually download and install the toolchain from [ARM Developer website](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads).
Select "x86_64 Linux hosted cross toolchains" - "AArch32 GNU/Linux target with hard float (arm-none-linux-gnueabihf)"
to download the toolchain.

For AArch64 dynamic binaries, install the matching toolchain and user emulator:

```shell
$ sudo apt-get install gcc-aarch64-linux-gnu qemu-user
```

Since `apt-get` does not provide the necessary RISC-V GNU toolchain, it must be downloaded manually if you want to
run a dynamically linked `shecc` targeting the RISC-V architecture. For instance, you can download and extract the
`riscv32-glibc-ubuntu-22.04-gcc.tar.xz` package from the [riscv-gnu-gcc](https://github.com/riscv-collab/riscv-gnu-toolchain) repository.

## Build and Verify

Configure which backend you want. `shecc` supports the ARMv7-A, AArch64,
RV32IM, and x86-64 backends, with Arm as the default:
```shell
$ make config ARCH=arm
# Target machine code switch to arm

$ make config ARCH=riscv
# Target machine code switch to riscv

$ make config ARCH=arm64
# Target machine code switch to arm64

$ make config ARCH=x64
# Target machine code switch to x64
```

The selected target is recorded in the tree, so a later `make` with a different
`ARCH` stops and asks for an explicit `make config ARCH=...` rather than pairing
one target's settings with another target's generated configuration.

Run `make` and you should see this:
```shell
$ make
  CC+LD	out/inliner
  GEN	out/libc.inc
  CC	out/src/main.o
  LD	out/shecc
  SHECC	out/shecc-stage1.elf
  SHECC	out/shecc-stage2.elf
```

Run `make DYNLINK=1` to use the dynamic linking mode and generate the dynamically linked compiler:
```shell
# If using the dynamic linking mode, you should add 'DYNLINK=1' for each 'make' command.
# The target architecture comes from the last 'make config' (default: arm).
$ make DYNLINK=1
  CC+LD	out/inliner
  GEN	out/libc.inc
  CC	out/src/main.o
  LD	out/shecc
  SHECC	out/shecc-stage1.elf
  SHECC	out/shecc-stage2.elf

$ file out/shecc-stage2.elf
# ARCH=arm:
out/shecc-stage2.elf: ELF 32-bit LSB executable, ARM, EABI5 version 1 (SYSV), dynamically linked, interpreter /lib/ld-linux-armhf.so.3, not stripped
# ARCH=arm64:
out/shecc-stage2.elf: ELF 64-bit LSB executable, ARM aarch64, dynamically linked, interpreter /lib/ld-linux-aarch64.so.1, no section header
```

For development builds with memory safety checks:
```shell
$ make sanitizer
$ make check-sanitizer
```

File `out/shecc` is the first stage compiler. Its usage:
```shell
$ shecc [-o output] [+m] [--dot] [--no-libc] [--dump-ir] [--dump-vir] [--stats] [--no-opt] [--dynlink] [-E] <infile.c>
```

Compiler options:
- `-o` : Specify output file name (default: `out.elf`)
- `+m` : Use hardware multiplication/division instructions (default: disabled)
- `--dot` : Write the VIR control-flow graph in Graphviz DOT format and stop
- `--no-libc` : Exclude embedded C library (default: embedded)
- `--dump-ir` : Dump intermediate representation (IR)
- `--stats` : Print deterministic compiler and VIR counters
- `--no-opt` : Skip the optional VIR optimizations (default: enabled)
- VIR inspection and optimization options are
  documented in the [VIR reference](docs/vir.md).
- `--dynlink` : Use dynamic linking (default: disabled)
- `-E` : Preprocess only; write the expanded token stream and stop

Example 1: static linking mode
```shell
$ out/shecc -o fib tests/fib.c
$ qemu-arm fib
```

The compiler marks its own output executable, so neither example needs a
`chmod +x` in front of the run. Earlier revisions did: the mode used to be
whatever the libc that opened the file chose, which was 0666 for a
glibc-linked build.

An `x64` build produces a native binary, so `./fib` runs it directly with no
emulator in front.

Example 2: dynamic linking mode

For AArch64, `/usr/aarch64-linux-gnu` is a typical ELF interpreter prefix.
The path may differ if you manually install a GNU toolchain, so set it to the
actual sysroot.
```shell
$ out/shecc --dynlink -o fib tests/fib.c
$ qemu-arm -L /usr/arm-linux-gnueabihf fib      # ARCH=arm
$ qemu-aarch64 -L /usr/aarch64-linux-gnu fib    # ARCH=arm64
```

### Unit Tests

`shecc` has one behavioral test flow. `make check` runs the executable and
compiler-error tests with both the host-built and self-hosted compilers, then
runs the selected target's ABI tests. To run it:
```shell
# Add 'DYNLINK=1' if using the dynamic linking mode.
$ make check          # Consolidated suite: stage 0, stage 2, and ABI tests
$ make check-stage0   # Test stage 0 compiler only
$ make check-stage2   # Test stage 2 compiler only
$ make check-abi-stage0 # Check the target calling convention (stage 0)
$ make check-abi-stage2 # Same, for the stage 2 compiler
$ make check-sanitizer # Test with AddressSanitizer and UBSan
```

The test suite covers:
* Basic data types and operators
* Control flow statements
* Arrays and pointers (including multi-level dereference)
* Structs, enums, and typedefs
* Variadic functions
* Preprocessor directives and macros
* Calling convention conformance for the selected target
* Self-hosting validation

Reference output (Arm target; pointer sizes read 8 on x86-64):
```
  TEST STAGE 0
...
int main(int argc, int argv) { exit(sizeof(char)); } => 1
int main(int argc, int argv) { int a; a = 0; switch (3) { case 0: return 2; case 3: a = 10; break; case 1: return 0; } exit(a); } => 10
int main(int argc, int argv) { int a; a = 0; switch (3) { case 0: return 2; default: a = 10; break; } exit(a); } => 10
OK
  TEST STAGE 2
...
int main(int argc, int argv) { exit(sizeof(char*)); }
exit code => 4
output => 
int main(int argc, int argv) { exit(sizeof(int*)); }
exit code => 4
output => 
OK
```

To clean up the generated compiler files, execute the command `make clean`.
For resetting architecture configurations, use the command `make distclean`.

## Intermediate Representation

[VIR](docs/vir.md) (Value Intermediate Representation) is shecc's native
value-centric SSA representation. The reference describes its invariants,
optimization schedule, shared machine lowering, and validation commands.

### Control-flow graph

`--dot` writes the optimized VIR control-flow graph as Graphviz DOT and stops
before executable generation. Without `-o`, the output name replaces the input
path's final suffix with `.dot`, or appends `.dot` when no suffix exists.

```shell
$ out/shecc --dot -o fib.dot tests/fib.c
$ dot -Tsvg fib.dot -o fib.svg
```

Each node represents a basic block, labeled with its function name and block
number. Edges represent control flow between blocks. Only reachable functions
are included. Graphviz is needed to render the file, but not to produce it.

### Instruction dump

```shell
$ out/shecc --dump-ir -o fib tests/fib.c
$ out/shecc --dump-vir -o fib tests/fib.c 2> fib.vir
```

`--dump-ir` prints optimized VIR to standard output; `--dump-vir` prints it to
standard error. Each function starts with `function NAME`, followed by blocks,
typed SSA values, block parameters, and instructions. Calls carry their
arguments and signatures directly. Memory operations carry their widths, and
volatile operations remain explicit. The dump precedes shared register
allocation and machine lowering.

Optimization is on by default; `--no-opt` turns the optional passes off. That
is for debugging and for isolating an optimizer fault, not for speed: the
unoptimized graph leaves more values to lower, so compilation is no faster.
Construction and final graph verification run either way. Value and block
numbers identify entities within a dump and can change as source or
optimization settings change.

## C99 Compliance

shecc implements a subset of C99 suitable for self-hosting and systems programming.
For detailed information about supported features, missing functionality, and non-standard behaviors,
see [COMPLIANCE.md](COMPLIANCE.md).

## Known Issues

1. Full `<stdarg.h>` support is not available. Variadic functions work via direct pointer arithmetic.
   See the `printf` implementation in `lib/c.c` for the supported approach.
2. The C front-end operates directly on token streams without building a full AST.
3. Complex pointer arithmetic expressions like `*(p + offset)` have limited support.

## License

`shecc` is freely redistributable under the BSD 2 clause license.
Use of this source code is governed by a BSD-style license that can be found in the `LICENSE` file.

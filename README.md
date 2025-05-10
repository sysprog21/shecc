# shecc : self-hosting and educational C optimizing compiler

<p align="center"><img src="https://user-images.githubusercontent.com/18013815/91671374-b2f0db00-eb58-11ea-8d55-858e9fb160c0.png" alt="logo image" width=40%></p>

## Introduction

`shecc` is built from scratch, targeting both 32-bit Arm and RISC-V architectures,
as a self-compiling compiler for a subset of the C language.
Despite its simplistic nature, it is capable of performing basic optimization strategies as a standalone optimizing compiler.

### Features

* Generate executable Linux ELF binaries for ARMv7-A and RV32IM.
* Provide a minimal C standard library for basic I/O on GNU/Linux.
* The cross-compiler is written in ANSI C, making it compatible with most platforms.
* Include a self-contained C front-end with an integrated machine code generator; no external assembler or linker needed.
* Utilize a two-pass compilation process: the first pass checks syntax and breaks down complex statements into basic operations,
  while the second pass translates these operations into Arm/RISC-V machine code.
* Develop a register allocation system that is compatible with RISC-style architectures.
* Implement an architecture-independent, [static single assignment](https://en.wikipedia.org/wiki/Static_single-assignment_form) (SSA)-based middle-end for enhanced optimizations.

## Compatibility

`shecc` is capable of compiling C source files written in the following
syntax:
* data types: char, int, struct, and pointer
* condition statements: if, while, for, switch, case, break, return, and
                        general expressions
* compound assignments: `+=`, `-=`, `*=`
* global/local variable initializations for supported data types
    - e.g. `int i = [expr]`
* limited support for preprocessor directives: `#define`, `#ifdef`, `#elif`, `#endif`, `#undef`, and `#error`
* non-nested variadic macros with `__VA_ARGS__` identifier

The backend targets armv7hf with Linux ABI, verified on Raspberry Pi 3,
and also supports RISC-V 32-bit architecture, verified with QEMU.

## Bootstrapping

The steps to validate `shecc` bootstrapping:
1. `stage0`: `shecc` source code is initially compiled using an ordinary compiler
   which generates a native executable. The generated compiler can be used as a
   cross-compiler.
2. `stage1`: The built binary reads its own source code as input and generates an
   ARMv7-A/RV32IM  binary.
3. `stage2`: The generated ARMv7-A/RV32IM binary is invoked (via QEMU or running on
   Arm and RISC-V devices) with its own source code as input and generates another
   ARMv7-A/RV32IM binary.
4. `bootstrap`: Build the `stage1` and `stage2` compilers, and verify that they are
   byte-wise identical. If so, `shecc` can compile its own source code and produce
   new versions of that same program.

## Prerequisites

Code generator in `shecc` does not rely on external utilities. You only need
ordinary C compilers such as `gcc` and `clang`. However, `shecc` would bootstrap
itself, and Arm/RISC-V ISA emulation is required. Install QEMU for Arm/RISC-V user
emulation on GNU/Linux:
```shell
$ sudo apt-get install qemu-user
```

It is still possible to build `shecc` on macOS or Microsoft Windows. However,
the second stage bootstrapping would fail due to `qemu-arm` absence.

To execute the snapshot test, install the packages below:
```shell
$ sudo apt-get install graphviz jq
```

## Build and Verify

Configure which backend you want, `shecc` supports ARMv7-A and RV32IM backend:
```
$ make config ARCH=arm
# Target machine code switch to Arm

$ make config ARCH=riscv
# Target machine code switch to RISC-V
```

Run `make` and you should see this:
```
  CC+LD	out/inliner
  GEN	out/libc.inc
  CC	out/src/main.o
  LD	out/shecc
  SHECC	out/shecc-stage1.elf
  SHECC	out/shecc-stage2.elf
```

File `out/shecc` is the first stage compiler. Its usage:
```shell
$ shecc [-o output] [+m] [--no-libc] [--dump-ir] <infile.c>
```

Compiler options:
- `-o` : Specify output file name (default: `out.elf`)
- `+m` : Use hardware multiplication/division instructions (default: disabled)
- `--no-libc` : Exclude embedded C library (default: embedded)
- `--dump-ir` : Dump intermediate representation (IR)

Example:
```shell
$ out/shecc -o fib tests/fib.c
$ chmod +x fib
$ qemu-arm fib
```

### IR Regression Tests

To ensure the consistency of frontend (lexer, parser) behavior when working on it, the snapshot test is introduced.
The snapshot test dumps IRs from the executable and compares the structural identity with the provided snapshots.

Verify the emitted IRs by specifying `check-snapshots` target when invoking `make`:
```shell
$ make check-snapshots
```

If the compiler frontend is updated, the emitted IRs might be changed.
Thus, you can update snapshots by specifying `update-snapshots` target when invoking `make`:
```shell
$ make update-snapshots
```

Notice that the above 2 targets will update all backend snapshots at once, to update/check current backend's snapshot, 
use `update-snapshot` / `check-snapshot` instead.

### Unit Tests

`shecc` comes with unit tests. To run the tests, give `check` as an argument:
```shell
$ make check
```

Reference output:
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

Once the option `--dump-ir` is passed to `shecc`, the intermediate representation (IR)
will be generated. Take the file `tests/fib.c` for example. It consists of a recursive
Fibonacci sequence function.
```c
int fib(int n)
{
    if (n == 0)
        return 0;
    else if (n == 1)
        return 1;
    return fib(n - 1) + fib(n - 2);
}
```

Execute the following to generate IR:
```shell
$ out/shecc --dump-ir -o fib tests/fib.c
```

Line-by-line explanation between C source and IR (variable and label numbering may differ):
```c
C Source                  IR                                         Explanation
-------------------+--------------------------------------+--------------------------------------------------------------------------------------
int fib(int n)       def int @fib(int %n)
{                    {
  if (n == 0)          const %.t871, 0                      Load constant 0 into a temporary variable ".t871"
                       %.t872 = eq %n, %.t871               Test if "n" is equal to ".t871", store result in ".t872"
                       br %.t872, .label.1430, .label.1431  If ".t872" is non-zero, branch to label ".label.1430", otherwise to ".label.1431"
                     .label.1430:
    return 0;          const %.t873, 0                      Load constant 0 into a temporary variable ".t873"
                       ret %.t873                           Return ".t873"
                     .label.1431:
  else if (n == 1)     const %.t874, 1                      Load constant 1 into a temporary variable ".t874"
                       %.t875 = eq %n, %.t874               Test if "n" is equal to ".t874", store result in ".t875"
                       br %.t875, .label.1434, .label.1435  If ".t875" is true, branch to ".label.1434", otherwise to ".label.1435"
                     .label.1434:
    return 1;          const %.t876, 1                      Load constant 1 into a temporary variable ".t876"
                       ret %.t876                           Return ".t876"
                     .label.1435:
  return fib(n - 1)    const %.t877, 1                      Load constant 1 into ".t877"
                       %.t878 = sub %n, %.t877              Subtract ".t877" from "n", store in ".t878"
                       push %.t878                          Prepare argument ".t878" for function call
                       call @fib, 1                         Call function "@fib" with 1 argument
         +             retval %.t879                        Store the return value in ".t879"
         fib(n - 2);   const %.t880, 2                      Load constant 2 into ".t880"
                       %.t881 = sub %n, %.t880              Subtract ".t880" from "n", store in ".t881"
                       push %.t881                          Prepare argument ".t881" for function call
                       call @fib, 1                         Call function "@fib" with 1 argument
                       retval %.t882                        Store the return value in ".t882"
                       %.t883 = add %.t879, %.t882          Add ".t879" and ".t882", store in ".t883"
                       ret %.t883                           Return ".t883"
}                    }
```

## Known Issues

1. The generated ELF lacks of .bss and .rodata section
2. The support of varying number of function arguments is incomplete. No `<stdarg.h>` can be used.
   Alternatively, check the implementation `printf` in source `lib/c.c` for `var_arg`.
3. The C front-end is a bit dirty because there is no effective AST.

## License

`shecc` is freely redistributable under the BSD 2 clause license.
Use of this source code is governed by a BSD-style license that can be found in the `LICENSE` file.

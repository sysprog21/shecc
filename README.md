# shecc : self-hosting and educational C compiler

<p align="center"><img src="https://user-images.githubusercontent.com/18013815/91671374-b2f0db00-eb58-11ea-8d55-858e9fb160c0.png" alt="logo image" width=40%></p>

## Introduction

`shecc` is built from scratch, targeted at 32-bit Arm and RISC-V architecture, as
a self-compiling compiler for a subset of the C language.

### Features

* Generate executable Linux ELF binaries for ARMv7-A and RV32IM;
* Provide a minimal C standard library for basic I/O on GNU/Linux;
* The cross-compiler is written in ANSI C, arguably running on most platforms;
* Self-contained C language front-end and machine code generator;
* Two-pass compilation: on the first pass it checks the syntax of 
  statements and constructs a table of symbols, while on the second pass
  it actually translates program statements into Arm/RISC-V machine code.

## Compatibility

`shecc` is capable of compiling C source files written in the following
syntax:
* data types: char, int, struct, and pointer
* condition statements: if, while, for, switch, case, break, return, and
                        general expressions
* compound assignments: `+=`, `-=`, `*=`
* global/local variable initializations for supported data types
    - e.g. `int i = [expr]`
* non-nested variadic macros with `__VA_ARGS__` identifier

The backend targets armv7hf with Linux ABI, verified on Raspberry Pi 3.

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
```
shecc [-o output] [--no-libc] [--dump-ir] <infile.c>
```

Compiler options:
- `-o` : output file name (default: out.elf)
- `--no-libc` : Exclude embedded C library (default: embedded)
- `--dump-ir` : Dump intermediate representation (IR)

Example:
```shell
$ out/shecc -o fib tests/fib.c
$ chmod +x fib
$ qemu-arm fib
```

`shecc` comes with unit tests. To run the tests, give "check" as an argument:
```shell
$ make check
```

Reference output:
```
...
int main(int argc, int argv) { exit(sizeof(char)); } => 1
int main(int argc, int argv) { int a; a = 0; switch (3) { case 0: return 2; case 3: a = 10; break; case 1: return 0; } exit(a); } => 10
int main(int argc, int argv) { int a; a = 0; switch (3) { case 0: return 2; default: a = 10; break; } exit(a); } => 10
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

Line-by-line explanation between C source and IR:
```asm
 C Source            IR                         Explanation
-------------------+--------------------------+----------------------------------------------------

int fib(int n)      fib:                        Reserve stack frame for function fib
{                     {
    if (n == 0)         x0 = &n                 Get address of variable n
                        x0 = *x0 (4)            Read value from address into x0, length = 4 (int)
                        x1 := 0                 Set x1 to zero
                        x0 == x1 ?              Compare x0 with x1
                        if false then goto 1641 If x0 != x1, then jump to label 1641
        return 0;       x0 := 0                 Set x0 to zero. x0 is the return value.
                        return (from fib)       Jump to function exit
                    1641:
    else if (n == 1)    x0 = &n                 Get address of variable n
                        x0 = *x0 (4)            Read value from address into x0, length = 4 (int)
                        x1 := 1                 Set x1 to 1
                        x0 == x1 ?              Compare x0 with x1
                        if true then goto 1649  If x0 != x1, then jump to label 1649
        return 1;       x0 := 1                 Set x0 to 1. x0 is the return value.
                        return (from fib)       Jump to function exit
                    1649:
    return              x0 = &n                 Get address of variable n
       fib(n - 1)       x0 = *x0 (4)            Read value from address into x0, length = 4 (int)
                        x1 := 1                 Set x1 to 1
                        x0 -= x1                Subtract x1 from x0 i.e. (n - 1)
       +                x0 := fib() @ 1631      Call function fib() into x0
                        push x0                 Store the result on stack
       fib(n - 2);      x0 = &n                 Get address of variable n
                        x0 = *x0 (4)            Read value from address into x0, length = 4 (int)
                        x1 := 2                 Set x1 to 2
                        x0 -= x1                Subtract x1 from x0 i.e. (n - 2)
                        x1 := fib() @ 1631      Call function fib() into x1
                        pop x0                  Retrieve the result off stack into x0
                        x0 += x1                Add x1 to x0 i.e. the result of fib(n-1) + fib(n-2)
                        return (from fib)       Jump to function exit
                      }                         Restore the previous stack frame
                      exit fib
```

## Known Issues

1. The generated ELF lacks of .bss and .rodata section
2. The support of varying number of function arguments is incomplete. No `<stdarg.h>` can be used.
   Alternatively, check the implementation `printf` in source `lib/c.c` for `var_arg`.
3. The C front-end is a bit dirty because there is no effective AST.

## License

`shecc` is freely redistributable under the BSD 2 clause license.
Use of this source code is governed by a BSD-style license that can be found in the `LICENSE` file.

# Dynamic Linking

## Build dynamically linked shecc and programs

Build the dynamically linked version of shecc, but notice that shecc currently doesn't support dynamic linking for the RISC-V architecture:

```shell
$ make ARCH=arm DYNLINK=1
```

Next, you can use shecc to build dynamically linked programs by adding the `--dynlink` flag:

```shell
# Use the stage 0 compiler
$ out/shecc --dynlink -o <output> <input.c>
# Use the stage 1 or stage 2 compiler
$ qemu-arm -L <LD_PREFIX> out/shecc-stage2.elf --dynlink -o <output> <input.c>

# Execute the compiled program
$ qemu-arm -L <LD_PREFIX> <output>
```

When executing a dynamically linked program, you should set the ELF interpreter prefix so that `ld.so` can be invoked. Generally, it should be `/usr/arm-linux-gnueabihf` if you have installed the ARM GNU toolchain by `apt`. Otherwise, you should find and specify the correct path if you manually installed the toolchain.

## Stack frame layout

In dynamic linking mode, the stack frame layout for each function can be illustrated as follows:

```
High Address
+------------------+
| incoming args    |
+------------------+ <- sp + total_size
| saved lr         | 
+------------------+ <- sp + total_size - 4
| local variables  |
+------------------+ <- sp + 20
| saved r12 (ip)   |
+------------------+ <- sp + 16
| outgoing args    |
+------------------+ <- sp (MUST be aligned to 8 bytes)
Low Address
```

* `total_size`: includes the size of the following elements:
  * `outgoing args`: a fixed size - 16 bytes
  * `saved r12`: a fixed size - 4 bytes
  * All local variables
  * `saved lr`: a fixed size - 4 bytes
  

Currently, since the maximal number of arguments is 8, an additional 20 bytes of stack space are allocated for outgoing arguments and register `r12`.

For the Arm architecture, when the callee is an external function, the caller uses the first 16 bytes to push extra arguments onto stack to comply with calling convention..

In addition, because external functions may modify register `r12`, which holds the pointer of the global stack, the caller also preserves its value at `[sp + 16]` and restores it after the external function returns.

## About function arguments handling

### Arm (32-bit)

If the callee is an internal function meaning that its implementation is compiled by shecc, the caller directly puts all arguments into register `r0` - `r7`.

Conversely, the caller performs the following operations to comply with the Arm Architecture Procedure Call Standard (AAPCS).

* First four arguments are put into `r0` - `r3`
* Other additional arguments are passed to stack. Arguments are pushed onto stack starting from the last argument, so the fifth argument is at the lower address and the last argument is at the higher address.
* Align the stack pointer to 8 bytes, as external functions may access 8-byte objects, which require 8-byte alignment.

### RISC-V (32-bit)

(Currently not supported)

## Runtime execution flow

1. Program starts at ELF entry point.
2. Dynamic linker (`ld.so`) is invoked.
   * For the Arm architecture, the dynamic linker is `/lib/ld-linux-armhf.so.3`.
3. Linker loads shared libraries such as `libc.so`.
4. Linker resolves symbols and fills global offset table (GOT).
5. Control transfers to the program.
6. Program executes `__libc_start_main` at the beginning.
7. `__libc_start_main` calls the *main wrapper*, which pushes registers r4-r11 and lr onto stack, sets up a global stack for all global variables (excluding read-only variables), and initializes them.
8. Execute the *main wrapper*, and then invoke the main function.
9. After the `main` function returns, the *main wrapper* restores the necessary registers and passes control back to  `__libc_start_main`, which implicitly calls `exit(3)` to terminate the program.

## Dynamic sections

When using dynamic linking, the following sections are generated for compiled programs:

1. `.interp` - Path to dynamic linker
2. `.dynsym` - Dynamic symbol table
3. `.dynstr` - Dynamic string table
4. `.rel.plt` - PLT relocations
5. `.plt` - Procedure Linkage Table
6. `.got` - Global Offset Table
7. `.dynamic` - Dynamic linking information

### PLT explanation for Arm32

The first entry contains the following instructions to invoke resolver to perform relocation.

```
push	{lr}		@ (str lr, [sp, #-4]!)
movw	sl, #:lower16:(&GOT[2])
movt	sl, #:upeer16:(&GOT[2])
mov	lr, sl
ldr	pc, [lr]
```

1. Push register `lr` onto stack.
2. Set register `sl` to the address of `GOT[2]`.
3. Move the value of `sl` to `lr`.
4. Load the value located at `[lr]` into the program counter (`pc`).



The remaining entries correspond to all external functions, with each entry including the following instructions:

```
movw ip, #:lower16:(&GOT[x])
movt ip, #:upper16:(&GOT[x])
ldr  pc, [ip]
```

1. Set register `ip` to the address of `GOT[x]`. 
2. Assign register `pc` to the value of `GOT[x]`. That is, set `pc` to the address of the callee.


# Dynamic Linking

## Build dynamically linked shecc and programs

Build the dynamically linked version of shecc:

```shell
# The 'DYNLINK' variable can control the use of dynamic linking mode
$ make ARCH=arm DYNLINK=1
$ make ARCH=riscv DYNLINK=1

# Enable immediate binding using the 'BINDING' variable (default: lazy)
$ make ARCH=arm DYNLINK=1 BINDING=now
$ make ARCH=riscv DYNLINK=1 BINDING=now
```

Next, you can use shecc to build dynamically linked programs by adding the `--dynlink` flag:

```shell
# Use the stage 0 compiler
$ out/shecc --dynlink -o <output> <input.c>
# Use the stage 1 or stage 2 compiler
$ qemu-arm -L <LD_PREFIX> out/shecc-stage2.elf --dynlink -o <output> <input.c>
$ qemu-riscv32 -L <LD_PREFIX> out/shecc-stage2.elf --dynlink -o <output> <input.c>
# Enable immediate binding by the '-z now' option
$ qemu-arm -L <LD_PREFIX> out/shecc-stage2.elf --dynlink -z now -o <output> <input.c>
$ qemu-riscv32 -L <LD_PREFIX> out/shecc-stage2.elf --dynlink -z now -o <output> <input.c>

# Execute the compiled program
$ qemu-arm -L <LD_PREFIX> <output>
$ qemu-riscv32 -L <LD_PREFIX> <output>
```

When executing a dynamically linked program, you should set the ELF interpreter prefix so that `ld.so` can be invoked.

Generally, the prefix should be `/usr/arm-linux-gnueabihf` for the Arm architecture if you have installed the ARM GNU toolchain by `apt`. Otherwise, you should find and specify the correct path if you manually installed the toolchain. For RISC-V, you must manually download a 32-bit RISC-V GNU toolchain since `apt` may not provide any package to install the necessary toolchain.

## Stack frame layout

### Arm32

In both static and dynamic linking modes, the stack frame layout for each function can be illustrated as follows:

```
High Address
+------------------+
| incoming args    |
+------------------+ <- sp + total_size
| saved lr         |
+------------------+
| saved r11        |
+------------------+
| saved r10        |
+------------------+
| saved r9         |
+------------------+
| saved r8         |
+------------------+
| saved r7         |
+------------------+
| saved r6         |
+------------------+
| saved r5         |
+------------------+
| saved r4         |
+------------------+
| (padding)        |
+------------------+
| local variables  |
+------------------+ <- sp + (MAX_PARAMS - MAX_ARGS_IN_REG) * 4
| outgoing args    |
+------------------+ <- sp (MUST be aligned to 8 bytes)
Low Address
```

* `total_size`: includes the size of the following elements:
  * `outgoing args`: a fixed size - `(MAX_PARAMS - MAX_ARGS_IN_REG) * 4` bytes
  * `local variables`
  * `saved r4-r11 and lr`: a fixed size - 36 bytes

* Note that the space for `incoming args` belongs to the caller's stack frame, while the remaining space belongs to the callee's stack frame.

### RISC-V

```
High Address
+------------------+
| ...              |
+------------------+ <- sp + total_size
| preserved return |
| address          |
+------------------+
| (padding)        |
+------------------+
| local variables  |
+------------------+ <- sp + (MAX_PARAMS - MAX_ARGS_IN_REG) * 4
| (unused space)   |
+------------------+ <- sp (MUST be aligned to 16 bytes)
Low Address
```

`total_size`: includes the size of the following elements:

* `unused space`: a fixed size - `(MAX_PARAMS - MAX_ARGS_IN_REG) * 4` bytes
* `local variables`
* `preserved return address`: a fixed size - 4 bytes

## Calling Convention

Regardless of which mode is used, callers are ensured to perform a collection of required operations for complying with the ABI of the target architecture when calling a function.

### Arm32

Caller's behavior:

* The first four arguments are put into registers `r0` - `r3`.
* Any additional arguments are passed on the stack. Arguments are pushed onto the stack starting from the last argument, so the fifth argument resides at a lower address and the last argument at a higher address.

Callee's behavior:

- Preserve the contents of registers `r4` - `r11` on the stack upon function entry.
  - The callee also pushes the content of `lr` onto the stack to preserve the return address; however, this operation is not required by the AAPCS.
- Allocate necessary space on the stack and align the stack pointer to 8-byte, as external functions may access 8-byte objects that require such alignment.
- Restore registers `r4` - `r11` from the stack upon returning, and load the saved `lr` to `pc` to return.

### RISC-V

Caller's behavior:

- Preserve caller-saved registers:
  - `a0` - `a7`.
  - `ra` is always saved upon caller's entry.
  - Exception: `t0` - `t6` are always used to store temporary values by the code generator, so these temporary registers are not necessary to be saved.

- The first eight arguments are passed into registers `a0` - `a7`.
- Since the current implementation of shecc supports up to 8 arguments, no argument needs to be passed onto the stack.

Callee's behavior

- Allocate necessary space on the stack and align the stack pointer to 128-bit (16-byte).
- Preserve callee-saved registers:
  - Although `sp` is not explicitly saved onto the stack after allocating space for local variables, the code generator guarantees that `sp` is correctly restored for the caller prior to returning. Therefore, `sp` is not necessary to be additionally handled.
  - `s0` - `s1`:
    - Static linking mode: only `s0` is used at the program entry point; its original value does not need to be saved because no caller exist.
    - Dynamic linking mode: both registers are used to hold the `argc` and `argv` values prior to global initialization. The code generator ensures that their original values are saved to the stack and restored before transferring control back to `__libc_start_main`.
  - `s2` - `s11` are not used by the code generator, so they are unnecessary to be processed.
- Restore the return address and the stack pointer before returning.

## Runtime execution flow of a dynamically linked program

```
          |                                                                     +---------------------------+
          |                                                                     |  program                  |
          | +-------------+                             +----------------+      |                           |
          | | shell       |                             | Dynamic linker |      |  +--------+ +----------+  |
userspace | |             |                             |                +------+->| entry  | | main     |  |
          | | $ ./program |                             | (ld.so)        |      |  | point  | | function |  |
program   | +-----+-------+                             +----------------+      |  +-+------+ +-----+----+  |
          |       |                                             ^               |    |         ^    |       |
          |       |                                             |               +----+---------+----+-------+
          |       |                                             |                    |         |    |
          |       |                                             |                    |         |    |
----------+-------+---------------------------------------------+--------------------+---------+----+----------------------
          |       |                                             |                    |         |    |
          |       v                                             |                    v         |    v
          |   +-------+ (It may be another                      |                +-------------+-----+    +------+
glibc     |   | execl |                                         |                | __libc_start_main +--->| exit |
          |   +---+---+  equivalent call)                       |                +-------------------+    +---+--+
          |       |                                             |                                             |
----------+-------+---------------------------------------------+---------------------------------------------+------------
system    |       |                                             |                                             |
          |       v                                             |                                             v
call      |   +------+  (It may be another                      |                                         +-------+
          |   | exec |                                          |                                         | _exit |
interface |   +---+--+   equivalent syscall)                    |                                         +---+---+
          |       |                                             |                                             |
----------+-------+---------------------------------------------+---------------------------------------------+------------
          |       |                                             |                                             |
          |       v                                             |                                             v
          |   +--------------+    +---------------+    +--------+-------------+                        +---------------+
          |   | Validate the |    | Create a new  |    | Startup the kernel's |                        | Delete the    |
kernel    |   |              +--->|               +--->|                      |                        |               |
          |   | executable   |    | process image |    | program loader       |                        | process image |
          |   +--------------+    +---------------+    +----------------------+                        +---------------+
```

1. A running process (e.g.: a shell) executes the specified program (`program`), which is dynamically linked.
2. Kernel validates the executable and creates a process image if the validation passes.
3. Dynamic linker (`ld.so`) is invoked by the kernel's program loader.
   * For the Arm architecture, the dynamic linker is `/lib/ld-linux-armhf.so.3`.
   * For the RISC-V architecture, the dynamic linker is `/lib/ld-linux-riscv32-ilp32d.so.1`.
4. Linker loads shared libraries such as `libc.so`.
5. Linker resolves symbols and fills global offset table (GOT).
6. Control transfers to the program, which starts at the entry point.
7. Program executes `__libc_start_main` at the beginning.
8. `__libc_start_main` calls the *main wrapper*, which includes the following operations:
   * Architecture-specific behavior:
     * Arm: push registers `r4`-`r11` and `lr` onto the stack.
     * RISC-V: store register `ra` onto the stack (preserve the address back to `__libc_start_main`).
   * Preserve `argc` and `argv` for the main function.
   * Set up a global stack for all global variables (excluding read-only variables) and initialize them.
9. Execute the *main wrapper*, and then invoke the main function.
10. After the `main` function returns, the *main wrapper* restores the necessary registers and passes control back to  `__libc_start_main`, which implicitly calls `exit(3)` to terminate the program.
       * Alternatively, the `main` function can also call `exit(3)` or `_exit(2)` to directly terminate itself.

## Dynamic sections

When using dynamic linking, the following sections are generated for compiled programs:

1. `.interp` - Path to dynamic linker
2. `.dynsym` - Dynamic symbol table
3. `.dynstr` - Dynamic string table
4. `.rel.plt` - PLT relocations
5. `.plt` - Procedure Linkage Table
6. `.got` - Global Offset Table
7. `.dynamic` - Dynamic linking information

### Initialization of all GOT entries

* Arm:
  * `GOT[0]` is set to the starting address of the `.dynamic` section.
  * `GOT[1]` and `GOT[2]` are initialized to zero and reserved for `link_map` and resolver (`__dl_runtimer_resolve`), and they are modified to point to the actual addresses by the dynamic linker at runtime.

* RISC-V:
  * `GOT[0]` and `GOT[1]` are initialized to zero and reserved for resolver (`__dl_runtimer_resolve`) and  `link_map`, and they are modified to point to the actual addresses by the dynamic linker at runtime.

* The remaining entries are initially set to the address of `PLT[0]` at compile time, causing the first call to an external function to invoke the resolver at runtime.

### Explanation for PLT stubs (Arm32)

Under the Arm architecture, the resolver assumes that the following three conditions are met:

* `[sp]` contains the return address from the original function call.
* `ip` stores the address of the callee's GOT entry.
* `lr` stores the address of `GOT[2]`.

Therefore, the first entry (`PLT[0]`) contains the following instructions to satisfy the first and third requirements, and then to invoke the resolver.

```
push	{lr}		@ (str lr, [sp, #-4]!)
movw	sl, #:lower16:(&GOT[2])
movt	sl, #:upper16:(&GOT[2])
mov	lr, sl
ldr	pc, [lr]
```

1. Push register `lr` onto the stack.
2. Set register `sl` to the address of `GOT[2]`.
3. Move the value of `sl` to `lr`.
4. Load the value located at `[lr]` into the program counter (`pc`)

------

The remaining PLT entries correspond to all external functions, and each entry includes the following instructions to fulfill the second requirement:

```
movw ip, #:lower16:(&GOT[x])
movt ip, #:upper16:(&GOT[x])
ldr  pc, [ip]
```

1. Set register `ip` to the address of `GOT[x]`. 
2. Assign register `pc` to the value of `GOT[x]`. That is, set `pc` to the address of the callee.

### Explanation for PLT stubs (RISC-V)

In the RISC-V ABI document, the first entry of PLT can be produced as follows:

```
1: auipc  t2, %pcrel_hi(.got)
   sub    t1, t1, t3
   lw     t3, %pcrel_lo(1b)(t2)
   addi   t1, t1 -(PLT0_SIZE + 12)    # PLT0_SIZE is 32 bytes.
   addi   t0, t2, %pcrel_lo(1b)
   srli   t1, t1, log2(16 / PTRSIZE)  # PTRSIZE is 4 bytes.
   lw     t0, PTRSIZE(t0)
   jr     t3
```

- `t0` is set to `GOT[1]`, which is the `link_map` pointer.

- `t1` is a `.got` offset:

  | External Function | Corresponding GOT element | `.got` offset |
  | ----------------- | ------------------------- | ------------- |
  | 1st function      | `GOT[2]`                  | `0`           |
  | 2nd function      | `GOT[3]`                  | `4`           |
  | ...               | ...                       | ...           |
  | N-th function     | `GOT[N + 1]`              | `(N - 1) * 4` |

- `t2` is `%hi(%pcrel(.got))`, but it is not used by `__dl_runtime_resolve()`.

- `t3` is `GOT[0]` (a pointer to `__dl_runtime_resolve()`), and `PLT[0]` finally uses `t3` to jump to the resolver.

------

Each of the remaining entries can be generated with the following instructions:

```
1: auipc  t3, %pcrel_hi(function@.got)
   lw     t3, %pcrel_lo(1b)(t3)
   jalr   t1, t3
   nop
```

This instruction sequence sets `t1` and `t3` to the address of `nop` and `GOT[N]` respectively, and performs a jump via `t3` to call an external function.

## PLT execution path and performance overhead

Since calling an external function needs a PLT stub for indirect invocation, the execution path of the first call is as follows:

1. Call the corresponding PLT stub of the external function.
2. The PLT stub reads the GOT entry.
3. Since the GOT entry is initially set to point to the first PLT entry, the call jumps to `PLT[0]`, which in turn calls the resolver.
4. The resolver handles the symbol and updates the GOT entry.
5. Jump to the actual function to continue execution.

For subsequent calls, the execution path only performs steps 1, 2 and 5. Regardless of whether it is the first call or a subsequent call, calling an external function requires executing additional instructions. It is evident that the overhead accounts to 3-8 instructions compared to a direct call.

For a bootstrapping compiler, this overhead is acceptable.

## Binding

Each external function must perform relocation via the resolver; in other words, each "symbol" needs to **bind** to its actual address.

There are two types of binding:

### Lazy binding

The dynamic linker defers function call resolution until the function is called at runtime.

### Immediate handling

The dynamic linker resolves all symbols when the program is started, or when the shared library is loaded via `dlopen`.

## Limitations

For the current implementation of dynamic linking, note the following:

* GOT is located in a writable segment (`.data` segment).
* The `PT_GNU_RELRO` program header has not yet been implemented.

This implies that:

* GOT entries can be modified at runtime, which may create a potential ROP (Return-Oriented Programming) attack vector.
* Function pointers (GOT entries) might be hijacked due to the absence of full RELRO protection.

## Reference

* man page: `ld(1)`
* man page: `ld.so(8)`
* glibc implementation
  * [`__dl_runtime_resolve`](https://elixir.bootlin.com/glibc/glibc-2.41.9000/source/sysdeps/arm/dl-trampoline.S#L30) (Arm32)
  * [`__dl_runtime_resolve`](https://elixir.bootlin.com/glibc/glibc-2.41.9000/source/sysdeps/riscv/dl-trampoline.S#L34) (for RISC-V)
* Application Binary Interface for the Arm Architecture - [`abi-aa`](https://github.com/ARM-software/abi-aa)
  * `aaelf32.pdf`
  * `aapcs32.pdf`
* RISC-V ABIs Specification - [`riscv-elf-psabi-doc`](https://github.com/riscv-non-isa/riscv-elf-psabi-doc)
  * `riscv-abi.pdf`

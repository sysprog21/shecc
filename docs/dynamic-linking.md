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

(Currently not supported)

## Calling Convention

### Arm32

Regardless of which mode is used, the caller performs the following operations to comply with the Arm Architecture Procedure Call Standard (AAPCS) when calling a function.

* The first four arguments are put into registers `r0` - `r3`
* Any additional arguments are passed on the stack. Arguments are pushed onto the stack starting from the last argument, so the fifth argument resides at a lower address and the last argument at a higher address.
* Align the stack pointer to 8 bytes, as external functions may access 8-byte objects that require such alignment.

Then, the callee will perform these operations:

- Preserve the contents of registers `r4` - `r11` on the stack upon function entry.
  - The callee also pushes the content of `lr` onto the stack to preserve the return address; however, this operation is not required by the AAPCS.

- Restore these registers from the stack upon returning.

### RISC-V

In the RISC-V architecture, registers `a0` - `a7` are used as argument registers; that is, the first eight arguments are passed into these registers.

Since the current implementation of shecc supports up to 8 arguments, no argument needs to be passed onto the stack.

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
4. Linker loads shared libraries such as `libc.so`.
5. Linker resolves symbols and fills global offset table (GOT).
6. Control transfers to the program, which starts at the entry point.
7. Program executes `__libc_start_main` at the beginning.
8. `__libc_start_main` calls the *main wrapper*, which pushes registers r4-r11 and lr onto the stack, sets up a global stack for all global variables (excluding read-only variables), and initializes them.
9. Execute the *main wrapper*, and then invoke the main function.
10. After the `main` function returns, the *main wrapper* restores the necessary registers and passes control back to  `__libc_start_main`, which implicitly calls `exit(3)` to terminate the program.
       * Or, the `main` function can also call `exit(3)` or `_exit(2)` to directly terminate itself.

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

* `GOT[0]` is set to the starting address of the `.dynamic` section.
* `GOT[1]` and `GOT[2]` are initialized to zero and reserved for the `link_map` and the resolver (`__dl_runtimer_resolve`).
  * The dynamic linker modifies them to point to the actual addresses at runtime.
* `GOT[3]` - `GOT[N]` are initially set to the address of `PLT[0]` at compile time, causing the first call to an external function to invoke the resolver at runtime.

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

The remaining PLT entries correspond to all external functions, and each entry includes the following instructions to fulfill the second requirement:

```
movw ip, #:lower16:(&GOT[x])
movt ip, #:upper16:(&GOT[x])
ldr  pc, [ip]
```

1. Set register `ip` to the address of `GOT[x]`. 
2. Assign register `pc` to the value of `GOT[x]`. That is, set `pc` to the address of the callee.

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
* `DT_BIND_NOW` (force immediate binding) is not set.

This implies that:

* GOT entries can be modified at runtime, which may create a potential ROP (Return-Oriented Programming) attack vector.
* Function pointers (GOT entries) might be hijacked due to the absence of full RELRO protection.

## Reference

* man page: `ld(1)`
* man page: `ld.so(8)`
* glibc - [`__dl_runtime_resolve`](https://elixir.bootlin.com/glibc/glibc-2.41.9000/source/sysdeps/arm/dl-trampoline.S#L30) implementation (for Arm32)
* Application Binary Interface for the Arm Architecture - [`abi-aa`](https://github.com/ARM-software/abi-aa)
  * `aaelf32`
  * `aapcs32`

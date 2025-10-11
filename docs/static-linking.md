# Static Linking

## Build statically linked shecc and programs

Build the statically linked version of shecc:

```shell
$ make ARCH=<target arch>
```

Next, you can use shecc to generate statically linked programs. The following demonstration uses shecc targeting Arm architecture to illustrate:

```shell
# Use the stage 0 compiler
$ out/shecc -o <output> <input.c>
# Use the stage 1 or stage 2 compiler
$ qemu-arm out/shecc-stage2.elf -o <output> <input.c>

# Execute the compiled program
$ qemu-arm <output>
```

## Stack frame layout

In static linking mode, the stack frame layout for each function can be illustrated as follows:

```
High Address
+------------------+ <- sp + total_size
| saved lr         | 
+------------------+ <- sp + total_size - 4
| local variables  |
+------------------+ <- sp + 20
| (unused space)   |
+------------------+ <- sp (may be aligned to 8 bytes)
Low Address
```

* `total_size`: the total size of all local variables plus an extra space for preserving register `lr` and an unused space.
  * For Arm32, the total size will be aligned to 8 bytes by the code generator.
  * The size of the unused space is 20 bytes and is only used in dynamic linking mode.

When a function completes execution, it restores the caller's stack pointer by subtracting `total_size` from `sp`, retrieves the return address from `[sp - 4]` and transfers control back to the caller.

## About function arguments handling

In the current implementation, the maximal number of arguments that shecc can handle is 8.

### Arm (32-bit)

In the Arm Architecture Procedure Calling Standard (AAPCS), if the number of arguments is greater than 4, only the first four arguments are stored in `r0` - `r3`, and the remaining arguments should be pushed onto stack. Additionally, the stack must be properly aligned.

However, shecc puts all arguments to register `r0` - `r7` even if the number of arguments exceeds 4. Since all functions are compiled by shecc in static linking mode, execution can still succeed by retrieving arguments from `r0` - `r7`, even though this does not comply with the AAPCS.

### RISC-V (32-bit)

In the RISC-V architecture, the maximal number of arguments that can be put into registers is 8, so shecc also puts all arguments to `a0` - `a7` directly. Therefore, the compiled programs are fully compliant with the RISC-V calling convention as long as the number of arguments does not exceed 8.

If shecc needs to support handling more arguments in the future, it should be improved to generate instructions to push extra arguments onto stack properly.

## Runtime execution flow

1. Program starts at ELF entry point.
2. Execute the *main wrapper*, which sets up a global stack for all global variables (but excluding read-only variables) and initializes them.
3. After the *main wrapper* completes, it retrieves `argc` and `argv` from stack, puts them into registers properly, and calls the `main` function to continue execution.
4. After the `main` function returns, use the `_exit` system call to terminate the program.

# Enforce the use qemu of by setting the ARCH_NAME variable to empty
ARCH_NAME =
ARCH_RUNNER = qemu-riscv32
ARCH_DEFS = \
    "/* target: RISCV */\n$\
    \#pragma once\n$\
    \#define ARCH_PREDEFINED \"__riscv\" /* Older versions of the GCC toolchain defined __riscv__ */\n$\
    \#define ELF_MACHINE 0xf3\n$\
    \#define ELF_FLAGS 0\n$\
    \#define DYN_LINKER \"/lib/ld-linux.so.3\"\n$\
    \#define LIBC_SO \"libc.so.6\"\n$\
    \#define PLT_FIXUP_SIZE 20\n$\
    \#define PLT_ENT_SIZE 12\n$\
    \#define R_ARCH_JUMP_SLOT 0x5\n$\
    "

# TODO: Set this variable for RISC-V architecture
RUNNER_LD_PREFIX=

# Enforce the use qemu of by setting the ALLOW_MACHINES variable to empty
ALLOW_MACHINES =
ARCH_RUNNER = qemu-riscv32

# Since the selected dynamic linker uses 'double-float ABI',
# ELF_FLAGS is set to 0x04 here to match this ABI.
ARCH_DEFS = \
    "/* target: RISCV */\n$\
    \#pragma once\n$\
    \#define ARCH_PREDEFINED \"__riscv\" /* Older versions of the GCC toolchain defined __riscv__ */\n$\
    \#define ELF_MACHINE 0xf3\n$\
    \#define ELF_FLAGS 0x4\n$\
    \#define DYN_LINKER \"/lib/ld-linux-riscv32-ilp32d.so.1\"\n$\
    \#define LIBC_SO \"libc.so.6\"\n$\
    \#define LIBDL_SO \"libdl.so.2\"\n$\
    \#define PLT_FIXUP_SIZE 32\n$\
    \#define PLT_ENT_SIZE 16\n$\
    \#define RESERVED_GOT_NUM 2\n$\
    \#define R_ARCH_JUMP_SLOT 0x5\n$\
    \#define MAX_ARGS_IN_REG 8\n$\
    "

TOOLCHAIN_CANDIDATES = riscv32-unknown-linux-gnu-

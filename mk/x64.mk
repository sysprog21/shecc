# x64 builds run natively on an x86-64 host; no emulator is involved.
ARCH_RUNNER =
USE_QEMU = 0
ARCH_DEFS = \
    "/* target: x86-64 */\n$\
    \#pragma once\n$\
    \#define ARCH_PREDEFINED \"__x86_64__\" /* defined by GNU C */\n$\
    \#define ELF_MACHINE 0x3e /* AMD x86-64 architecture */\n$\
    \#define ELF_FLAGS 0x0\n$\
    \#define PTR_SIZE 8 /* 64-bit pointer size */\n$\
    \#define MAX_ARGS_IN_REG 6 /* System V: rdi rsi rdx rcx r8 r9 */\n$\
    \#define DYN_LINKER \"/lib64/ld-linux-x86-64.so.2\"\n$\
    \#define LIBC_SO \"libc.so.6\"\n$\
    \#define PLT_FIXUP_SIZE 16\n$\
    \#define PLT_ENT_SIZE 16\n$\
    \#define RESERVED_GOT_NUM 3\n$\
    \#define R_ARCH_JUMP_SLOT 7 /* R_X86_64_JUMP_SLOT */\n$\
    \#define REG_CNT 8 /* rdi rsi rdx rcx r8 r9 rax rbx */\n$\
    \#define DYN_BIND_NOW 1 /* this PLT has no lazy-resolution path */\n$\
    "

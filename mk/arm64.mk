# AArch64 Linux / AAPCS64 target.
ARCH_RUNNER = qemu-aarch64
ARCH_DEFS = \
    "/* target: AArch64 */\n$\
    \#pragma once\n$\
    \#define ARCH_PREDEFINED \"__aarch64__\"\n$\
    \#define ELF_MACHINE 0xb7 /* EM_AARCH64 */\n$\
    \#define ELF_FLAGS 0\n$\
    \#define PTR_SIZE 8\n$\
    \#define MAX_ARGS_IN_REG 8\n$\
    /* Eight register arguments leave no outgoing stack area at the default\n$\
     * limit of eight, so a ninth argument would land on the caller's own\n$\
     * locals. */\n$\
    \#define MAX_PARAMS 16\n$\
    /* AArch64 Linux uses a 4K, 16K or 64K translation granule. Separating the\n$\
     * load segments by the largest keeps the image loadable on all three; a 4K\n$\
     * gap leaves both inside one 64K page, where the writable mapping replaces\n$\
     * the executable one. */\n$\
    \#define PAGESIZE 65536\n$\
    \#define REG_CNT 11\n$\
    \#define CALLEE_SAVED_REGS 3\n$\
    \#define DYN_LINKER \"/lib/ld-linux-aarch64.so.1\"\n$\
    \#define LIBC_SO \"libc.so.6\"\n$\
    \#define PLT_FIXUP_SIZE 0\n$\
    \#define PLT_ENT_SIZE 16\n$\
    \#define RESERVED_GOT_NUM 3\n$\
    \#define R_ARCH_JUMP_SLOT 1026 /* R_AARCH64_JUMP_SLOT */\n$\
    "

# An Arm64 Linux host runs this target's output itself, so nothing has to stand
# in for it. The Arm target needs fastfetch to tell a board that can run its
# 32-bit output from one that cannot; here the host answers on its own. The
# check is for the kernel as well as the architecture, since what comes out is
# an AArch64 Linux ELF and no other system will execute it. With the emulator
# out of the way the dynamic build resolves its interpreter and libc from the
# running system rather than from a sysroot.
ifeq ($(shell uname -s -m),Linux aarch64)
    USE_QEMU = 0
endif

TOOLCHAIN_CANDIDATES := aarch64-linux-gnu- aarch64-none-linux-gnu-

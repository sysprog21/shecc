ARCH_NAME = armv7l
ARCH_RUNNER = qemu-arm
ARCH_DEFS = \
    "/* target: ARM */\n$\
    \#pragma once\n$\
    \#define ARCH_PREDEFINED \"__arm__\" /* defined by GNU C and RealView */\n$\
    \#define ELF_MACHINE 0x28 /* up to ARMv7/Aarch32 */\n$\
    \#define ELF_FLAGS 0x5000400\n$\
    \#define DYN_LINKER \"/lib/ld-linux-armhf.so.3\"\n$\
    \#define LIBC_SO \"libc.so.6\"\n$\
    \#define PLT_FIXUP_SIZE 20\n$\
    \#define PLT_ENT_SIZE 12\n$\
    \#define R_ARCH_JUMP_SLOT 0x16\n$\
    "
RUNNER_LD_PREFIX=-L /usr/arm-linux-gnueabihf/

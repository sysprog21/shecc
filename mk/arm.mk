ARCH_NAME = armv7l
ARCH_RUNNER = qemu-arm
ARCH_DEFS = \
    "/* target: ARM */\n$\
    \#pragma once\n$\
    \#define ARCH_PREDEFINED \"__arm__\" /* defined by GNU C and RealView */\n$\
    \#define ELF_MACHINE 0x28 /* up to ARMv7/Aarch32 */\n$\
    \#define ELF_FLAGS 0x5000200\n$\
    "

ifeq ($(HOST_ARCH),armv7l) # detect ARMv7-A only and assume Linux-compatible
    TARGET_EXEC :=
else
    TARGET_EXEC = qemu-arm
    TARGET_EXEC := $(shell which $(TARGET_EXEC))
    ifndef TARGET_EXEC
    $(warning "no qemu-arm found. Please check package installation")
    ARM_EXEC = echo WARN: unable to run
    endif
endif

export TARGET_EXEC

arm-specific-defs = \
    $(Q)$(PRINTF) \
        "/* target: ARM */\n$\
        \#pragma once\n$\
        \#define ARCH_PREDEFINED \"__arm__\" /* defined by GNU C and RealView */\n$\
        \#define ELF_MACHINE 0x28 /* up to ARMv7/Aarch32 */\n$\
        \#define ELF_FLAGS 0x5000200\n$\
        "

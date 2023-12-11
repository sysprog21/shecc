ifeq ($(HOST_ARCH),armv7l) # detect ARMv7-A only and assume Linux-compatible
    ARM_EXEC :=
else
    ARM_EXEC = qemu-arm
    ARM_EXEC := $(shell which $(ARM_EXEC))
    ifndef ARM_EXEC
    $(warning "no qemu-arm found. Please check package installation")
    ARM_EXEC = echo WARN: unable to run
    endif
endif

export ARM_EXEC

arm-specific-defs = \
    $(Q)$(PRINTF) \
        "/* target: ARM */\n$\
        \#define ARCH_PREDEFINED \"__arm__\" /* defined by GNU C and RealView */\n$\
        \#define ELF_MACHINE 0x28 /* up to ARMv7/Aarch32 */\n$\
        \#define ELF_FLAGS 0x5000200\n$\
        "

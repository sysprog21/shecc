TARGET_EXEC = qemu-riscv32
TARGET_EXEC := $(shell which $(TARGET_EXEC))
ifndef TARGET_EXEC
$(warning "no qemu-riscv32 found. Please check package installation")
TARGET_EXEC = echo WARN: unable to run
endif

export TARGET_EXEC

riscv-specific-defs = \
    $(Q)$(PRINTF) \
        "/* target: RISCV */\n$\
        \#pragma once\n$\
        \#define ARCH_PREDEFINED \"__riscv\" /* Older versions of the GCC toolchain defined __riscv__ */\n$\
        \#define ELF_MACHINE 0xf3\n$\
        \#define ELF_FLAGS 0\n$\
        "

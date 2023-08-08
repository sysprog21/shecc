RISCV_EXEC = qemu-riscv32
RISCV_EXEC := $(shell which $(RISCV_EXEC))
ifndef RISCV_EXEC
$(warning "no qemu-riscv32 found. Please check package installation")
RISCV_EXEC = echo WARN: unable to run
endif

export RISCV_EXEC

riscv-specific-defs = \
    $(Q)$(PRINTF) \
        "/* target: RISCV */\n$\
        \#define ARCH_PREDEFINED \"__riscv\" /* Older versions of the GCC toolchain defined __riscv__ */\n$\
        \#define ELF_MACHINE 0xf3\n$\
        \#define ELF_FLAGS 0\n$\
        "

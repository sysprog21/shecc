# Enforce the use qemu of by setting the ALLOW_MACHINES variable to empty
ALLOW_MACHINES =
ARCH_RUNNER = qemu-riscv32
ARCH_DEFS = \
    "/* target: RISCV */\n$\
    \#pragma once\n$\
    \#define ARCH_PREDEFINED \"__riscv\" /* Older versions of the GCC toolchain defined __riscv__ */\n$\
    \#define ELF_MACHINE 0xf3\n$\
    \#define ELF_FLAGS 0\n$\
    \#define DYN_LINKER \"/lib/ld-linux-riscv32-ilp32d.so.1\"\n$\
    \#define LIBC_SO \"libc.so.6\"\n$\
    \#define PLT_FIXUP_SIZE 32\n$\
    \#define PLT_ENT_SIZE 16\n$\
    \#define RESERVED_GOT_NUM 2\n$\
    \#define R_ARCH_JUMP_SLOT 0x5\n$\
    \#define MAX_ARGS_IN_REG 8\n$\
    "

ifeq ($(USE_QEMU),1)
    ifeq ($(DYNLINK),1)
        CROSS_COMPILE = riscv32-unknown-linux-gnu-
        RISCV_CC = $(CROSS_COMPILE)gcc
        RISCV_CC := $(shell which $(RISCV_CC))
        ifndef RISCV_CC
            $(error "Unable to find ARM GNU toolchain.")
        endif

        LD_LINUX_PATH := $(shell cd $(shell $(RISCV_CC) --print-sysroot) 2>/dev/null && pwd)
        ifeq ("$(LD_LINUX_PATH)","/")
            LD_LINUX_PATH := $(shell dirname "$(shell which $(RISCV_CC))")/..
            LD_LINUX_PATH := $(shell cd $(LD_LINUX_PATH) 2>/dev/null && pwd)
            LD_LINUX_PATH := $(LD_LINUX_PATH)/$(shell echo $(CROSS_COMPILE) | sed s'/.$$//')/libc
            LD_LINUX_PATH := $(shell cd $(LD_LINUX_PATH) 2>/dev/null && pwd)
            ifndef LD_LINUX_PATH
                LD_LINUX_PATH = /usr/$(shell echo $(CROSS_COMPILE) | sed s'/.$$//')
                LD_LINUX_PATH := $(shell cd $(LD_LINUX_PATH) 2>/dev/null && pwd)
            endif
        endif

        ifndef LD_LINUX_PATH
            $(error "Dynamic linking mode requires ld-linux.so")
        endif

        RUNNER_LD_PREFIX = -L $(LD_LINUX_PATH)
    endif
endif

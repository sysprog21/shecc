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
    \#define MAX_ARGS_IN_REG 4\n$\
    "

# Find the sysroot of the ARM GNU toolchain if using dynamic linking.
#
# Since developers may install the toolchain manually instead of
# using a package manager such as apt, we cannot assume that the
# path of ld-linux is always "/usr/arm-linux-gnueabihf".
#
# Therefore, the following process first locates find the correct
# sysroot of the toolchain, and then generate the ELF interpreter
# prefix for later use.
ifneq ($(HOST_ARCH),$(ARCH_NAME))
    ifeq ($(DYNLINK),1)
        CROSS_COMPILE = arm-none-linux-gnueabihf-
        ARM_CC = $(CROSS_COMPILE)gcc
        ARM_CC := $(shell which $(ARM_CC))
        ifndef ARM_CC
            CROSS_COMPILE = arm-linux-gnueabihf-
            ARM_CC = $(CROSS_COMPILE)gcc
            ARM_CC := $(shell which $(ARM_CC))
            ifndef ARM_CC
                $(error "Unable to find ARM GNU toolchain.")
            endif
        endif

        LD_LINUX_PATH := $(shell cd $(shell $(ARM_CC) --print-sysroot) 2>/dev/null && pwd)
        ifeq ("$(LD_LINUX_PATH)","/")
            LD_LINUX_PATH := $(shell dirname "$(shell which $(ARM_CC))")/..
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

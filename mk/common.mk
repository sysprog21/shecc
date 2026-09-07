UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    PRINTF = printf
else
    PRINTF = env printf
endif

# Control the build verbosity
ifeq ("$(VERBOSE)","1")
    Q :=
    VECHO = @true
    REDIR =
else
    Q := @
    VECHO = @$(PRINTF)
    REDIR = >/dev/null
endif

# Test suite
PASS_COLOR = \e[32;01m
NO_COLOR = \e[0m

pass = $(PRINTF) "$(PASS_COLOR)$1 Passed$(NO_COLOR)\n"

# Find the sysroot of the ARM/RISC-V GNU toolchain if using dynamic linking.
#
# Since developers may install the toolchain manually instead of
# using a package manager such as apt, we cannot assume that the
# path of ld-linux is always "/usr/arm-linux-gnueabihf" or other
# similar paths.
#
# Therefore, the following process first locates find the correct
# sysroot of the toolchain, and then generate the ELF interpreter
# prefix for later use.
ifeq ($(USE_QEMU),1)
    ifeq ($(DYNLINK),1)
        AVAILABLE_TOOLCHAINS := $(foreach tc, $(TOOLCHAIN_CANDIDATES), $(if $(shell which $(tc)gcc), $(tc)))
        CROSS_COMPILE := $(firstword $(AVAILABLE_TOOLCHAINS))

        ifndef CROSS_COMPILE
            $(error "Unable to find a proper GNU toolchain.")
        endif

        ARCH_CC = $(CROSS_COMPILE)gcc

        LD_LINUX_PATH := $(shell cd $(shell $(ARCH_CC) --print-sysroot) 2>/dev/null && pwd)
        ifeq ("$(LD_LINUX_PATH)","/")
            LD_LINUX_PATH := $(shell dirname "$(shell which $(ARCH_CC))")/..
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

# Check the prerequisites
PREREQ_LIST :=
TARGET_EXEC ?=
ifeq ($(USE_QEMU),1)
    # Add qemu to the list if the host and target architectures differ
    PREREQ_LIST += $(ARCH_RUNNER)
    ifeq ($(filter $(ARCH_RUNNER),$(notdir $(shell which $(ARCH_RUNNER)))),)
        STAGE1_WARN_MSG := "Warning: failed to build the stage 1 and $\
                               stage 2 compilers due to missing $(ARCH_RUNNER)\n"
        STAGE1_CHECK_CMD := $(VECHO) $(STAGE1_WARN_MSG) && exit 1
    endif

    # Generate the path to the architecture-specific qemu
    TARGET_EXEC = $(shell which $(ARCH_RUNNER))
    ifeq ($(DYNLINK),1)
        TARGET_EXEC += $(RUNNER_LD_PREFIX)
    endif
endif
export TARGET_EXEC

PREREQ_EXEC := $(shell which $(PREREQ_LIST))
PREREQ_MISSING := $(filter-out $(notdir $(PREREQ_EXEC)),$(PREREQ_LIST))

ifdef PREREQ_MISSING
    CONFIG_WARN_MSG := "Warning: missing packages: $(PREREQ_MISSING)\n$\
                            Warning: Please check package installation\n"
    CONFIG_CHECK_CMD := $(VECHO) $(CONFIG_WARN_MSG)
endif

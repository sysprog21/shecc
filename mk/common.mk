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

# Check the prerequisites
PREREQ_LIST := dot jq
TARGET_EXEC ?=
ifneq ($(HOST_ARCH),$(ARCH_NAME))
    # Add qemu to the list if the host and target architectures differ
    PREREQ_LIST += $(ARCH_RUNNER)
    ifeq ($(filter $(ARCH_RUNNER),$(notdir $(shell which $(ARCH_RUNNER)))),)
        STAGE1_WARN_MSG := "Warning: failed to build the stage 1 and $\
                               stage 2 compilers due to missing $(ARCH_RUNNER)\n"
        STAGE1_CHECK_CMD := $(VECHO) $(STAGE1_WARN_MSG) && exit 1
    endif

    # Generate the path to the architecture-specific qemu
    TARGET_EXEC = $(shell which $(ARCH_RUNNER))
endif
export TARGET_EXEC

PREREQ_EXEC := $(shell which $(PREREQ_LIST))
PREREQ_MISSING := $(filter-out $(notdir $(PREREQ_EXEC)),$(PREREQ_LIST))

ifdef PREREQ_MISSING
    CONFIG_WARN_MSG := "Warning: missing packages: $(PREREQ_MISSING)\n$\
                            Warning: Please check package installation\n"
    CONFIG_CHECK_CMD := $(VECHO) $(CONFIG_WARN_MSG)
endif

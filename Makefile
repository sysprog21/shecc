CFLAGS := -O -g \
	-std=c99 -pedantic

CFLAGS_TO_CHECK := \
	-fwrapv \
	-Wall -Wextra \
	-Wno-unused-but-set-variable \
	-Wno-unused-parameter \
	-Wno-unused-function \
	-Wshadow \
	-Wno-variadic-macros \
	-Wno-uninitialized \
	-Wno-strict-prototypes \
	-Wno-declaration-after-statement \
	-Wno-format \
	-Wno-format-pedantic \
	-Wno-overflow

SUPPORTED_CFLAGS :=
# Check if a specific compiler flag is supported, attempting a dummy compilation
# with flags. If successful, it returns the flag string; otherwise, it returns
# an empty string.
# Usage: $(call check_flag, -some-flag)
check_flag = $(shell $(CC) $(1) -S -o /dev/null -xc /dev/null 2>/dev/null; \
              if test $$? -eq 0; then echo "$(1)"; fi)

# Iterate through the list of all potential flags, effectively filtering out all
# unsupported flags.
$(foreach flag, $(CFLAGS_TO_CHECK), $(eval CFLAGS += $(call check_flag, $(flag))))

BUILD_SESSION := .session.mk

-include $(BUILD_SESSION)

STAGE0 := shecc
STAGE1 := shecc-stage1.elf
STAGE2 := shecc-stage2.elf

OUT ?= out
ARCHS = arm riscv
ARCH ?= $(firstword $(ARCHS))
HOST_ARCH = $(shell arch 2>/dev/null)
SRCDIR := $(shell find src -type d)
LIBDIR := $(shell find lib -type d)

BUILTIN_LIBC ?= c.c
STAGE0_FLAGS ?= --dump-ir
STAGE1_FLAGS ?=
ifeq ($(DYNLINK),1)
    ifeq ($(ARCH),riscv)
        # TODO: implement dynamic linking for RISC-V.
        $(error "Dynamic linking mode is not implemented for RISC-V")
    endif
    BUILTIN_LIBC := c.h
    STAGE0_FLAGS += --dynlink
    STAGE1_FLAGS += --dynlink
endif

SRCS := $(wildcard $(patsubst %,%/main.c, $(SRCDIR)))
OBJS := $(SRCS:%.c=$(OUT)/%.o)
deps := $(OBJS:%.o=%.o.d)
TESTS := $(wildcard tests/*.c)
TESTBINS := $(TESTS:%.c=$(OUT)/%.elf)
SNAPSHOTS := $(foreach SNAPSHOT_ARCH,$(ARCHS), $(patsubst tests/%.c, tests/snapshots/%-$(SNAPSHOT_ARCH).json, $(TESTS)))

all: config bootstrap

sanitizer: CFLAGS += -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -O0
sanitizer: LDFLAGS += -fsanitize=address -fsanitize=undefined
sanitizer: config $(OUT)/$(STAGE0)-sanitizer
	$(VECHO) "  Built stage 0 compiler with sanitizers\n"

ifeq (,$(filter $(ARCH),$(ARCHS)))
$(error Support ARM and RISC-V only. Select the target with "ARCH=arm" or "ARCH=riscv")
endif
include mk/$(ARCH).mk
include mk/common.mk

config:
	$(Q)ln -s $(PWD)/$(SRCDIR)/$(ARCH)-codegen.c $(SRCDIR)/codegen.c
	$(Q)$(PRINTF) $(ARCH_DEFS) > $@
	$(VECHO) "Target machine code switch to %s\n" $(ARCH)
	$(Q)$(MAKE) $(BUILD_SESSION) --silent
	$(Q)$(CONFIG_CHECK_CMD)

$(OUT)/tests/%.elf: tests/%.c $(OUT)/$(STAGE0)
	$(VECHO) "  SHECC\t$@\n"
	$(Q)$(OUT)/$(STAGE0) $(STAGE0_FLAGS) -o $@ $< > $(basename $@).log ; \
	chmod +x $@ ; $(PRINTF) "Running $@ ...\n"
	$(Q)$(TARGET_EXEC) $@ && $(call pass)

check: check-stage0 check-stage2

check-stage0: $(OUT)/$(STAGE0) $(TESTBINS) tests/driver.sh
	$(VECHO) "  TEST STAGE 0\n"
	tests/driver.sh 0 $(DYNLINK)

check-stage2: $(OUT)/$(STAGE2) $(TESTBINS) tests/driver.sh
	$(VECHO) "  TEST STAGE 2\n"
	tests/driver.sh 2 $(DYNLINK)

check-sanitizer: $(OUT)/$(STAGE0)-sanitizer tests/driver.sh
	$(VECHO) "  TEST STAGE 0 (with sanitizers)\n"
	$(Q)cp $(OUT)/$(STAGE0)-sanitizer $(OUT)/shecc
	tests/driver.sh 0 $(DYNLINK)
	$(Q)rm $(OUT)/shecc

check-snapshots: $(OUT)/$(STAGE0) $(SNAPSHOTS) tests/check-snapshots.sh
	$(Q)$(foreach SNAPSHOT_ARCH, $(ARCHS), $(MAKE) distclean config check-snapshot ARCH=$(SNAPSHOT_ARCH) --silent;)
	$(VECHO) "Switching backend back to %s\n" $(ARCH)
	$(Q)$(MAKE) distclean config ARCH=$(ARCH) --silent

check-snapshot: $(OUT)/$(STAGE0) tests/check-snapshots.sh
	$(VECHO) "Checking snapshot for %s\n" $(ARCH)
	tests/check-snapshots.sh $(ARCH)
	$(VECHO) "  OK\n"

update-snapshots: tests/update-snapshots.sh
	$(Q)$(foreach SNAPSHOT_ARCH, $(ARCHS), $(MAKE) distclean config update-snapshot ARCH=$(SNAPSHOT_ARCH) --silent;)
	$(VECHO) "Switching backend back to %s\n" $(ARCH)
	$(Q)$(MAKE) distclean config ARCH=$(ARCH) --silent

update-snapshot: $(OUT)/$(STAGE0) tests/update-snapshots.sh
	$(VECHO) "Updating snapshot for %s\n" $(ARCH)
	tests/update-snapshots.sh $(ARCH)
	$(VECHO) "  OK\n"

$(OUT)/%.o: %.c
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF $@.d $<

SHELL_HACK := $(shell mkdir -p $(OUT) $(OUT)/$(SRCDIR) $(OUT)/tests)

$(OUT)/norm-lf: tools/norm-lf.c
	$(VECHO) "  CC+LD\t$@\n"
	$(Q)$(CC) $(CFLAGS) -o $@ $^

$(OUT)/libc.inc: $(OUT)/inliner $(OUT)/norm-lf $(LIBDIR)/$(BUILTIN_LIBC)
	$(VECHO) "  GEN\t$@\n"
	$(Q)$(OUT)/norm-lf $(LIBDIR)/$(BUILTIN_LIBC) $(OUT)/c.normalized.c
	$(Q)$(OUT)/inliner $(OUT)/c.normalized.c $@
	$(Q)$(RM) $(OUT)/c.normalized.c

$(OUT)/inliner: tools/inliner.c
	$(VECHO) "  CC+LD\t$@\n"
	$(Q)$(CC) $(CFLAGS) -o $@ $^

$(OUT)/$(STAGE0): $(OUT)/libc.inc $(OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) $(OBJS) $(LDFLAGS) -o $@

$(OUT)/$(STAGE0)-sanitizer: $(OUT)/libc.inc $(OBJS)
	$(VECHO) "  LD\t$@ (with sanitizers)\n"
	$(Q)$(CC) $(OBJS) $(LDFLAGS) -o $@

$(OUT)/$(STAGE1): $(OUT)/$(STAGE0)
	$(Q)$(STAGE1_CHECK_CMD)
	$(VECHO) "  SHECC\t$@\n"
	$(Q)$(OUT)/$(STAGE0) $(STAGE0_FLAGS) -o $@ $(SRCDIR)/main.c > $(OUT)/shecc-stage1.log
	$(Q)chmod a+x $@

$(OUT)/$(STAGE2): $(OUT)/$(STAGE1)
	$(VECHO) "  SHECC\t$@\n"
	$(Q)$(TARGET_EXEC) $(OUT)/$(STAGE1) $(STAGE1_FLAGS) -o $@ $(SRCDIR)/main.c

bootstrap: $(OUT)/$(STAGE2)
	$(Q)chmod 775 $(OUT)/$(STAGE2)
	$(Q)if ! diff -q $(OUT)/$(STAGE1) $(OUT)/$(STAGE2); then \
	echo "Unable to bootstrap. Aborting"; false; \
	fi

$(BUILD_SESSION):
	$(PRINTF) "ARCH=$(ARCH)" > $@

.PHONY: clean
clean:
	-$(RM) $(OUT)/$(STAGE0) $(OUT)/$(STAGE1) $(OUT)/$(STAGE2)
	-$(RM) $(OBJS) $(deps)
	-$(RM) $(TESTBINS) $(OUT)/tests/*.log $(OUT)/tests/*.lst
	-$(RM) $(OUT)/shecc*.log
	-$(RM) $(OUT)/libc.inc

distclean: clean
	-$(RM) $(OUT)/inliner $(OUT)/norm-lf $(OUT)/target $(SRCDIR)/codegen.c config $(BUILD_SESSION)
	-$(RM) DOM.dot CFG.dot

-include $(deps)

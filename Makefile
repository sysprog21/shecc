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

USE_QEMU ?= 1
OUT ?= out
# Every architecture that can be selected as a build target. The first is the
# default when ARCH is not given.
ARCHS = arm riscv x64
# The subset carrying reference IR snapshots. x64 has none yet, so the snapshot
# targets skip it; it is still a fully supported build target.
SNAPSHOT_ARCHS = arm riscv
ARCH ?= $(firstword $(ARCHS))
SRCDIR := $(shell find src -type d)
LIBDIR := $(shell find lib -type d)

BUILTIN_LIBC_SOURCE ?= c.c
BUILTIN_LIBC_HEADER := c.h
STAGE0_FLAGS ?= --dump-ir
STAGE1_FLAGS ?=
DYNLINK ?= 0
ifeq ($(DYNLINK),1)
    STAGE0_FLAGS += --dynlink
    STAGE1_FLAGS += --dynlink
endif

SRCS := $(wildcard $(patsubst %,%/main.c, $(SRCDIR)))
OBJS := $(SRCS:%.c=$(OUT)/%.o)
deps := $(OBJS:%.o=%.o.d)
TESTS := $(wildcard tests/*.c)
TESTBINS := $(TESTS:%.c=$(OUT)/%.elf)
SNAPSHOTS = $(foreach SNAPSHOT_ARCH,$(SNAPSHOT_ARCHS), $(patsubst tests/%.c, tests/snapshots/%-$(SNAPSHOT_ARCH)-static.json, $(TESTS)))
SNAPSHOTS += $(patsubst tests/%.c, tests/snapshots/%-arm-dynamic.json, $(TESTS))

all: config bootstrap

sanitizer: CFLAGS += -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -O0
sanitizer: LDFLAGS += -fsanitize=address -fsanitize=undefined
sanitizer: config $(OUT)/$(STAGE0)-sanitizer
	$(VECHO) "  Built stage 0 compiler with sanitizers\n"

ifeq (,$(filter $(ARCH),$(ARCHS)))
$(error Unsupported ARCH "$(ARCH)". Select one of: $(ARCHS))
endif

# The tree carries the selected architecture in two generated files: "config",
# which the compiler sources include, and the build session below. Both are
# written together by the config target. Building with a different ARCH than the
# one on record would pair this architecture's Makefile settings with the
# previous architecture's generated config, so require an explicit reconfigure
# instead.
#
# Naming "config" or "distclean" anywhere in the goals is that reconfigure: the
# record is about to be rewritten or removed, so the architecture it still holds
# does not apply and the check must not fire. Testing for their presence rather
# than filtering them out is what lets a goal list combine them with real work
# -- check-snapshots and update-snapshots recurse with exactly
# "distclean config check-snapshot ARCH=...". "clean" touches no generated
# config, so a mismatch cannot affect it either.
CONFIGURED_ARCH := $(shell sed -n 's/^ARCH=//p' $(BUILD_SESSION) 2>/dev/null)
ifneq (,$(CONFIGURED_ARCH))
ifeq (,$(filter config distclean,$(MAKECMDGOALS)))
ifneq (,$(filter-out clean,$(or $(MAKECMDGOALS),all)))
ifneq ($(CONFIGURED_ARCH),$(ARCH))
$(error Tree is configured for ARCH=$(CONFIGURED_ARCH). Run "make config ARCH=$(ARCH)" to switch)
endif
endif
endif
endif

include mk/$(ARCH).mk
include mk/common.mk

# Selecting a target rewrites every file that records the choice, so switching
# architectures never needs a manual clean first: the codegen symlink is
# replaced in place, and the build session is rewritten rather than left saying
# whatever the previous selection said.
# "config" names a generated file, but selecting an architecture has to run
# even when that file already exists -- otherwise "make config ARCH=..." on a
# configured tree reports the file up to date and switches nothing. The
# generated definitions are moved into place only when they actually differ, so
# reasserting the current selection does not restamp the file and force a
# rebuild of every source that includes it.
.PHONY: config
config:
	$(Q)ln -sf $(PWD)/$(SRCDIR)/$(ARCH)-codegen.c $(SRCDIR)/codegen.c
	$(Q)$(PRINTF) $(ARCH_DEFS) > $@.tmp
	$(Q)if cmp -s $@.tmp $@; then $(RM) $@.tmp; else mv $@.tmp $@; fi
	$(Q)$(PRINTF) "ARCH=$(ARCH)" > $(BUILD_SESSION)
	$(VECHO) "Target machine code switch to %s\n" $(ARCH)
	$(Q)$(CONFIG_CHECK_CMD)

$(OUT)/tests/%.elf: tests/%.c $(OUT)/$(STAGE0)
	$(VECHO) "  SHECC\t$@\n"
	$(Q)$(OUT)/$(STAGE0) $(STAGE0_FLAGS) -o $@ $< > $(basename $@).log ; \
	chmod +x $@ ; $(PRINTF) "Running $@ ...\n"
	$(Q)$(TARGET_EXEC) $@ && $(call pass)

check: check-stage0 check-stage2 check-abi-stage0 check-abi-stage2

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
	# static linking
	$(Q)$(foreach SNAPSHOT_ARCH, $(SNAPSHOT_ARCHS), $(MAKE) distclean config check-snapshot ARCH=$(SNAPSHOT_ARCH) DYNLINK=0 --silent;)
	# dynamic linking
	$(Q)$(foreach SNAPSHOT_ARCH, $(SNAPSHOT_ARCHS), $(MAKE) distclean config check-snapshot ARCH=$(SNAPSHOT_ARCH) DYNLINK=1 --silent;)
	$(VECHO) "Switching backend back to %s (DYNLINK=0)\n" arm
	$(Q)$(MAKE) distclean config ARCH=arm DYNLINK=0 --silent

check-snapshot: $(OUT)/$(STAGE0) tests/check-snapshots.sh
	$(VECHO) "Checking snapshot for %s (DYNLINK=%s)\n" $(ARCH) $(DYNLINK)
	tests/check-snapshots.sh $(ARCH) $(DYNLINK)
	$(VECHO) "  OK\n"

check-abi-stage0: $(OUT)/$(STAGE0)
	tests/$(ARCH)-abi.sh 0 $(DYNLINK);

check-abi-stage2: $(OUT)/$(STAGE2)
	tests/$(ARCH)-abi.sh 2 $(DYNLINK);

update-snapshots: tests/update-snapshots.sh
	# static linking
	$(Q)$(foreach SNAPSHOT_ARCH, $(SNAPSHOT_ARCHS), $(MAKE) distclean config update-snapshot ARCH=$(SNAPSHOT_ARCH) DYNLINK=0 --silent;)
	# dynamic linking
	$(Q)$(foreach SNAPSHOT_ARCH, $(SNAPSHOT_ARCHS), $(MAKE) distclean config update-snapshot ARCH=$(SNAPSHOT_ARCH) DYNLINK=1 --silent;)
	$(VECHO) "Switching backend back to %s (DYNLINK=0)\n" arm
	$(Q)$(MAKE) distclean config ARCH=arm DYNLINK=0 --silent

update-snapshot: $(OUT)/$(STAGE0) tests/update-snapshots.sh
	$(VECHO) "Updating snapshot for %s (DYNLINK=%s)\n" $(ARCH) $(DYNLINK)
	tests/update-snapshots.sh $(ARCH) $(DYNLINK)
	$(VECHO) "  OK\n"

$(OUT)/%.o: %.c
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF $@.d $<

SHELL_HACK := $(shell mkdir -p $(OUT) $(OUT)/$(SRCDIR) $(OUT)/tests)

$(OUT)/norm-lf: tools/norm-lf.c
	$(VECHO) "  CC+LD\t$@\n"
	$(Q)$(CC) $(CFLAGS) -o $@ $^

$(OUT)/libc.inc: $(OUT)/inliner $(OUT)/norm-lf $(LIBDIR)/$(BUILTIN_LIBC_SOURCE) $(LIBDIR)/$(BUILTIN_LIBC_HEADER)
	$(VECHO) "  GEN\t$@\n"
	$(Q)$(OUT)/norm-lf $(LIBDIR)/$(BUILTIN_LIBC_SOURCE) $(OUT)/c.normalized.c
	$(Q)$(OUT)/norm-lf $(LIBDIR)/$(BUILTIN_LIBC_HEADER) $(OUT)/c.normalized.h
	$(Q)$(OUT)/inliner $(OUT)/c.normalized.c $(OUT)/c.normalized.h $@
	$(Q)$(RM) $(OUT)/c.normalized.c $(OUT)/c.normalized.h

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

.PHONY: clean
clean:
	-$(RM) $(OUT)/$(STAGE0) $(OUT)/$(STAGE1) $(OUT)/$(STAGE2)
	-$(RM) $(OBJS) $(deps)
	-$(RM) $(TESTBINS) $(OUT)/tests/*.log $(OUT)/tests/*.lst
	-$(RM) $(OUT)/shecc*.log
	-$(RM) $(OUT)/libc.inc

distclean: clean
	-$(RM) $(OUT)/inliner $(OUT)/norm-lf $(OUT)/target $(SRCDIR)/codegen.c config config.tmp $(BUILD_SESSION)
	-$(RM) DOM.dot CFG.dot

-include $(deps)

# -fwrapv is required, not probed: hashmap_hash_index() carries the FNV-1a
# accumulator in a signed int, because shecc has no 'unsigned' to compile
# itself with, and the multiply there overflows by design.
CFLAGS := -O -g \
	-std=c99 -pedantic -fwrapv

# Every -Wno- that used to sit here has been earned away rather than renewed:
# the tree is clean under gcc and clang with nothing switched off, so a warning
# that appears from now on is about the code and not about the flag list.
CFLAGS_TO_CHECK := \
	-Wall -Wextra \
	-Wshadow

SUPPORTED_CFLAGS :=
# Check if a specific compiler flag is supported, attempting a dummy compilation
# with flags. If successful, it returns the flag string; otherwise, it returns
# an empty string.
# Usage: $(call check_flag, -some-flag)
check_flag = $(shell $(CC) $(1) -S -o /dev/null -xc /dev/null 2>/dev/null; \
              if test $$? -eq 0; then echo "$(1)"; fi)

# Iterate through the list of all potential flags, effectively filtering out all
# unsupported flags. Half a second of $(CC) probing that the style and hook
# targets have no use for, so skip it when nothing is being compiled.
STYLE_GOALS := check-style check-newline check-comments check-format check-shell \
	indent install-hooks uninstall-hooks check-hooks
ifneq ($(filter-out $(STYLE_GOALS),$(or $(MAKECMDGOALS),all)),)
$(foreach flag, $(CFLAGS_TO_CHECK), $(eval CFLAGS += $(call check_flag, $(flag))))
endif

BUILD_SESSION := .session.mk

-include $(BUILD_SESSION)

STAGE0 := shecc
STAGE1 := shecc-stage1.elf
STAGE2 := shecc-stage2.elf

USE_QEMU ?= 1
OUT ?= out
# Every architecture that can be selected as a build target. The first is the
# default when ARCH is not given.
ARCHS = arm arm64 riscv x64
ARCH ?= $(firstword $(ARCHS))
SRCDIR := $(shell find src -type d)
LIBDIR := $(shell find lib -type d)

BUILTIN_LIBC_SOURCE ?= c.c
BUILTIN_LIBC_HEADER := c.h
# --dump-ir is what makes out/shecc-stage1.log the IR of the stage 1 build
# rather than an empty file. It is the only thing in the tree that exercises
# dump_insn()/dump_ph2_ir(), and it is what a failed CI run uploads.
STAGE0_FLAGS ?= --dump-ir
STAGE1_FLAGS ?=
DYNLINK ?= 0

COMMENTFLOW ?= commentflow
SHFMT ?= shfmt
ifeq ($(DYNLINK),1)
    STAGE0_FLAGS += --dynlink
    STAGE1_FLAGS += --dynlink
endif

SRCS := $(wildcard $(patsubst %,%/main.c, $(SRCDIR)))
OBJS := $(SRCS:%.c=$(OUT)/%.o)

# The sanitizer build keeps its objects apart from the normal one. Sharing them
# lets whichever ran last decide what the other links: a plain "make" after it
# fails outright on the missing runtime, and -- quietly, which is worse --
# "make sanitizer" after a plain build relinks an uninstrumented object, so
# check-sanitizer then passes having checked nothing.
SAN_OUT := $(OUT)/sanitize
SAN_OBJS := $(SRCS:%.c=$(SAN_OUT)/%.o)
deps := $(OBJS:%.o=%.o.d) $(SAN_OBJS:%.o=%.o.d)

all: config bootstrap

# Both goals carry the flags, because a target-specific variable reaches only
# that target and its prerequisites: "make check-sanitizer" on its own would
# otherwise build $(SAN_OBJS) with the ordinary CFLAGS and run the suite against
# a binary that instruments nothing.
SAN_CFLAGS := -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -O0
SAN_LDFLAGS := -fsanitize=address -fsanitize=undefined

sanitizer check-sanitizer: CFLAGS += $(SAN_CFLAGS)
sanitizer check-sanitizer: LDFLAGS += $(SAN_LDFLAGS)
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
# than filtering them out is what lets a goal list combine them with real work,
# as "make distclean config check ARCH=riscv" does. "clean" touches no
# generated config, so a mismatch cannot affect it either.
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

.PHONY: $(STYLE_GOALS)

check: check-stage0 check-stage2 check-abi-stage0 check-abi-stage2

# One checker per target: they share nothing, so "make -j check-style" runs them
# concurrently and finishes in the time the slowest one takes.
check-style: check-newline check-comments check-format check-shell

check-newline:
	$(Q).ci/check-newline.sh

check-comments:
	$(Q)COMMENTFLOW=$(COMMENTFLOW) .ci/check-commentflow.sh

check-format:
	$(Q).ci/check-format.sh

check-shell:
	$(Q)SHFMT=$(SHFMT) .ci/check-shell.sh

check-hooks:
	$(Q)scripts/test-git-hooks.sh

# Naming both goals would otherwise run each --write pass beside the checker
# reading the same files, so the rewrite goes first and the checkers then report
# on a tree that has stopped moving. Neither goal alone is affected.
ifneq ($(filter indent,$(MAKECMDGOALS)),)
check-newline check-comments check-format check-shell: | indent
endif

# The checkers own both halves: which files they cover and which tool rewrites
# them. Naming either one here again would only be a second place to update.
indent:
	$(Q).ci/check-newline.sh --write
	$(Q)SHFMT=$(SHFMT) .ci/check-shell.sh --write
	$(Q)COMMENTFLOW=$(COMMENTFLOW) .ci/check-commentflow.sh --write
	$(Q).ci/check-format.sh --write

install-hooks:
	$(Q)scripts/install-git-hooks.sh

uninstall-hooks:
	$(Q)scripts/install-git-hooks.sh --uninstall

check-stage0: $(OUT)/$(STAGE0) tests/driver.sh
	$(VECHO) "  TEST STAGE 0\n"
	tests/driver.sh 0 $(DYNLINK)

check-stage2: $(OUT)/$(STAGE2) tests/driver.sh
	$(VECHO) "  TEST STAGE 2\n"
	tests/driver.sh 2 $(DYNLINK)

check-sanitizer: $(OUT)/$(STAGE0)-sanitizer tests/driver.sh
	$(VECHO) "  TEST STAGE 0 (with sanitizers)\n"
	$(Q)cp $(OUT)/$(STAGE0)-sanitizer $(OUT)/shecc
	tests/driver.sh 0 $(DYNLINK)
	$(Q)rm $(OUT)/shecc

check-abi-stage0: $(OUT)/$(STAGE0)
	tests/$(ARCH)-abi.sh 0 $(DYNLINK);

check-abi-stage2: $(OUT)/$(STAGE2)
	tests/$(ARCH)-abi.sh 2 $(DYNLINK);

# Both prerequisites are order-only, and both exist because "make -j" would
# otherwise let a compile start beside the thing it reads. Selecting a target
# replaces src/codegen.c with "ln -sf", which unlinks before it relinks, so a
# compile racing "config" can find nothing there. And src/main.c includes
# out/libc.inc, which is generated: listing it only on the stage0 link left
# the two free to run in either order on a tree that has never been built.
$(OUT)/%.o: %.c | config $(OUT)/libc.inc
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF $@.d $<

SHELL_HACK := $(shell mkdir -p $(OUT) $(OUT)/$(SRCDIR) $(OUT)/tests \
                      $(SAN_OUT)/$(SRCDIR))

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

$(SAN_OUT)/%.o: %.c | config $(OUT)/libc.inc
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF $@.d $<

$(OUT)/$(STAGE0)-sanitizer: $(OUT)/libc.inc $(SAN_OBJS)
	$(VECHO) "  LD\t$@ (with sanitizers)\n"
	$(Q)$(CC) $(SAN_OBJS) $(LDFLAGS) -o $@

$(OUT)/$(STAGE1): $(OUT)/$(STAGE0)
	$(Q)$(STAGE1_CHECK_CMD)
	$(VECHO) "  SHECC\t$@\n"
	$(Q)$(OUT)/$(STAGE0) $(STAGE0_FLAGS) -o $@ $(SRCDIR)/main.c > $(OUT)/shecc-stage1.log
	$(Q)chmod a+x $@

# The mode belongs here rather than on "bootstrap", because "check" builds
# stage2 through this rule without going through that target. Statically the
# question does not arise: shecc's own libc opens the output 0775. Linked
# dynamically the open is glibc's, which gives 0666 before the umask, so a
# "make check" on a tree that had never been bootstrapped produced a stage2
# nothing could execute and every stage-2 test failed to compile.
$(OUT)/$(STAGE2): $(OUT)/$(STAGE1)
	$(VECHO) "  SHECC\t$@\n"
	$(Q)$(TARGET_EXEC) $(OUT)/$(STAGE1) $(STAGE1_FLAGS) -o $@ $(SRCDIR)/main.c
	$(Q)chmod 775 $@

bootstrap: $(OUT)/$(STAGE2)
	$(Q)if ! diff -q $(OUT)/$(STAGE1) $(OUT)/$(STAGE2); then \
	echo "Unable to bootstrap. Aborting"; false; \
	fi

.PHONY: clean
clean:
	-$(RM) $(OUT)/$(STAGE0) $(OUT)/$(STAGE1) $(OUT)/$(STAGE2)
	-$(RM) $(OUT)/$(STAGE0)-sanitizer
	-$(RM) $(OBJS) $(SAN_OBJS) $(deps)
	-$(RM) $(TESTBINS) $(OUT)/tests/*.log $(OUT)/tests/*.lst
	-$(RM) $(OUT)/shecc*.log
	-$(RM) $(OUT)/libc.inc

distclean: clean
	-$(RM) $(OUT)/inliner $(OUT)/norm-lf $(OUT)/target $(SRCDIR)/codegen.c config config.tmp $(BUILD_SESSION)
	-$(RM) DOM.dot CFG.dot

-include $(deps)

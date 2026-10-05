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

VIR_FRONTEND_FIXTURES := $(sort $(wildcard tests/vir-frontend-*.c))
VIR_NATIVE_SHARED_FIXTURES := $(sort $(wildcard tests/vir-direct-global-*.c) \
	$(wildcard tests/vir-direct-branch-*.c) \
	$(wildcard tests/vir-direct-narrow-global.c) \
	tests/vir-direct-external-pointer-origin.c \
	tests/vir-direct-external-typed-pointer.c \
	tests/vir-direct-external-typed-pointer-offset.c)
VIR_NATIVE_X64_FIXTURES := $(sort $(wildcard tests/vir-direct-x64*.c) \
	$(VIR_NATIVE_SHARED_FIXTURES))
VIR_NATIVE_ARM64_FIXTURES := $(sort $(wildcard tests/vir-direct-arm64*.c) \
	$(VIR_NATIVE_SHARED_FIXTURES))
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
# The translation timestamp belongs to generated configuration rather than the
# compiler's host clock. A single value is consequently embedded in stages 0,
# 1, and 2, which keeps bootstrap byte-for-byte reproducible. Rebuilders can
# supply SOURCE_DATE_EPOCH for a stable timestamp across separate invocations.
# The fallback is read once: a recursive "?=" would rerun date for the date
# and again for the time, which then disagree across a second boundary.
ifeq ($(origin SOURCE_DATE_EPOCH),undefined)
SOURCE_DATE_EPOCH := $(shell date -u +%s)
endif
# The epoch is spliced into the date command below, so anything but decimal
# digits would be run by the shell there. $(value) keeps make from expanding a
# supplied $(...) before it is checked.
EPOCH_NONDIGITS := $(value SOURCE_DATE_EPOCH)
$(foreach d,0 1 2 3 4 5 6 7 8 9,$(eval EPOCH_NONDIGITS := $$(subst $(d),,$$(EPOCH_NONDIGITS))))
ifneq ($(if $(strip $(value SOURCE_DATE_EPOCH)),$(EPOCH_NONDIGITS),empty),)
$(error SOURCE_DATE_EPOCH must be a Unix epoch in decimal seconds)
endif
# GNU date converts an epoch given as -d @SECONDS, which BSD and macOS date do
# not accept; they take the seconds as -r SECONDS instead. Ask for the Unix
# epoch itself to learn which spelling this date understands.
ifeq ($(shell date -u -d @0 +%s 2>/dev/null),0)
EPOCH_DATE = LC_ALL=C TZ=UTC date -u -d "@$(SOURCE_DATE_EPOCH)"
else
EPOCH_DATE = LC_ALL=C TZ=UTC date -u -r "$(SOURCE_DATE_EPOCH)"
endif
TRANSLATION_DATE := $(shell $(EPOCH_DATE) '+%b %e %Y')
TRANSLATION_TIME := $(shell $(EPOCH_DATE) '+%H:%M:%S')
ifeq ($(strip $(TRANSLATION_DATE)$(TRANSLATION_TIME)),)
$(error SOURCE_DATE_EPOCH must be a Unix epoch accepted by date)
endif
TRANSLATION_DEFS = "\#define SHECC_TRANSLATION_DATE \"$(TRANSLATION_DATE)\"\n\#define SHECC_TRANSLATION_TIME \"$(TRANSLATION_TIME)\"\n"
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
# Naming "config", "distclean", or "check-all-targets" anywhere in the goals
# is that reconfigure: the
# record is about to be rewritten or removed, so the architecture it still holds
# does not apply and the check must not fire. Testing for their presence rather
# than filtering them out is what lets a goal list combine them with real work,
# as "make distclean config check ARCH=riscv" does. "clean" touches no
# generated config, so a mismatch cannot affect it either.
CONFIGURED_ARCH := $(shell sed -n 's/^ARCH=//p' $(BUILD_SESSION) 2>/dev/null)
ifneq (,$(CONFIGURED_ARCH))
ifeq (,$(filter config distclean check-all-targets,$(MAKECMDGOALS)))
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
	$(Q)$(PRINTF) $(TRANSLATION_DEFS) >> $@.tmp
	$(Q)if cmp -s $@.tmp $@; then $(RM) $@.tmp; else mv $@.tmp $@; fi
	$(Q)$(PRINTF) "ARCH=$(ARCH)" > $(BUILD_SESSION)
	$(VECHO) "Target machine code switch to %s\n" $(ARCH)
	$(Q)$(CONFIG_CHECK_CMD)

.PHONY: $(STYLE_GOALS)
.PHONY: check-vir-frontend-stage0 check-vir-frontend-stage2 \
	check-vir-stage0 check-vir-stage2 \
	check-vir-frontend-policy-stage0 check-vir-frontend-policy-stage2 \
	check-vir-ssa-baseline check-vir-ssa-baseline-stage0 check-vir-ssa-baseline-stage2 \
	check-vir-large-cfg-stage0 check-vir-large-cfg-stage2 \
	check-vir-ssa-selfhost check-vir-ssa-selfhost-stage0 check-vir-ssa-selfhost-stage2

check: check-stage0 check-stage2 check-abi-stage0 check-abi-stage2 \
	check-vir-core check-vir-frontend-stage0 check-vir-frontend-stage2 check-vir-fuzz check-vir-stage0 check-vir-stage2 check-vir-arm-verifier-stage0 \
	check-vir-multi-input-stage0 check-vir-multi-input-stage2 \
	check-vir-arm-verifier-stage2 check-vir-stats-stage0 check-vir-stats-stage2 check-vir-baseline check-vir-ssa-baseline check-vir-large-cfg-stage0 check-vir-large-cfg-stage2 check-vir-ssa-selfhost \
	check-vir-corpus-stage0 check-fuzz-stage0 check-vir-frontend-policy-stage0

# Random programs checked against the host compiler and built by shecc.
# FUZZ_SEEDS and FUZZ_START widen or move the run; a failing program is kept
# as out/fuzz-<seed>.c.
.PHONY: check-fuzz-stage0
check-fuzz-stage0: $(OUT)/$(STAGE0) tests/fuzz.sh tests/fuzz-gen.c
	TARGET_EXEC="$(TARGET_EXEC)" CC="$(CC)" bash tests/fuzz.sh 0

# Native IR optimization levels must preserve each corpus program's behavior.
# Only stage 0 is part of check: under emulation, stage 2 compiles the corpus
# once per optimization level inside qemu, which costs more than the other checks.
.PHONY: check-vir-corpus-stage0 check-vir-corpus-stage2
check-vir-corpus-stage0: $(OUT)/$(STAGE0) tests/vir-corpus.sh
	TARGET_EXEC="$(TARGET_EXEC)" bash tests/vir-corpus.sh 0 $(ARCH)
check-vir-corpus-stage2: $(OUT)/$(STAGE2) tests/vir-corpus.sh
	TARGET_EXEC="$(TARGET_EXEC)" bash tests/vir-corpus.sh 2 $(ARCH)

.PHONY: check-vir-multi-input-stage0 check-vir-multi-input-stage2
check-vir-multi-input-stage0: $(OUT)/$(STAGE0) tests/vir-multi-input.sh tests/vir-multi-input-root.c tests/vir-multi-input-main.c
	TARGET_EXEC="$(TARGET_EXEC)" bash tests/vir-multi-input.sh 0
check-vir-multi-input-stage2: $(OUT)/$(STAGE2) tests/vir-multi-input.sh tests/vir-multi-input-root.c tests/vir-multi-input-main.c
	TARGET_EXEC="$(TARGET_EXEC)" bash tests/vir-multi-input.sh 2

check-vir-large-cfg-stage0: $(OUT)/$(STAGE0) tests/vir-large-cfg.sh tests/vir-frontend-large-cfg.c
	TARGET_EXEC="$(TARGET_EXEC)" bash tests/vir-large-cfg.sh 0

check-vir-large-cfg-stage2: $(OUT)/$(STAGE2) tests/vir-large-cfg.sh tests/vir-frontend-large-cfg.c
	TARGET_EXEC="$(TARGET_EXEC)" bash tests/vir-large-cfg.sh 2

ifeq ($(ARCH),x64)
.PHONY: check-vir-native-x64-stage0 check-vir-native-x64-stage2 \
	check-x64-startup-globals-stage0 check-x64-startup-globals-stage2
check: check-x64-startup-globals-stage0 check-x64-startup-globals-stage2 \
	check-vir-native-x64-stage0 check-vir-native-x64-stage2
check-vir-native-x64-stage0: $(OUT)/$(STAGE0) $(VIR_NATIVE_X64_FIXTURES) tests/vir-direct-x64.sh tests/vir-direct-common.sh
	bash tests/vir-direct-x64.sh 0
check-vir-native-x64-stage2: $(OUT)/$(STAGE2) $(VIR_NATIVE_X64_FIXTURES) tests/vir-direct-x64.sh tests/vir-direct-common.sh
	bash tests/vir-direct-x64.sh 2

.PHONY: check-x64-startup-globals-stage0 check-x64-startup-globals-stage2
check-x64-startup-globals-stage0: $(OUT)/$(STAGE0) tests/vir-direct-x64-startup-globals.c tests/x64-startup-globals.sh
	bash tests/x64-startup-globals.sh 0
check-x64-startup-globals-stage2: $(OUT)/$(STAGE2) tests/vir-direct-x64-startup-globals.c tests/x64-startup-globals.sh
	bash tests/x64-startup-globals.sh 2
endif

ifeq ($(ARCH),arm64)
.PHONY: check-vir-native-arm64-stage0 check-vir-native-arm64-stage2
check: check-vir-native-arm64-stage0 check-vir-native-arm64-stage2

check-vir-native-arm64-stage0: $(OUT)/$(STAGE0) $(VIR_NATIVE_ARM64_FIXTURES) tests/vir-direct-arm64.sh tests/vir-direct-common.sh
	TARGET_EXEC="$(TARGET_EXEC)" bash tests/vir-direct-arm64.sh 0
check-vir-native-arm64-stage2: $(OUT)/$(STAGE2) $(VIR_NATIVE_ARM64_FIXTURES) tests/vir-direct-arm64.sh tests/vir-direct-common.sh
	TARGET_EXEC="$(TARGET_EXEC)" bash tests/vir-direct-arm64.sh 2
endif

# Run the complete check -- driver and ABI suites at stages 0 and 2 -- on every
# backend, as CI does. Driver cases that need 64-bit values are gated on the
# target's pointer width, so each target runs everything it can represent.
# Configuration is global to this worktree, so keep recursive invocations
# sequential and restore the caller's architecture even when a target fails.
# Each target is rebuilt after its own `config`, preventing an old compiler from
# being paired with a newly selected backend. This make exported the emulator
# for the caller's architecture, so each recursive make has to derive its own:
# an inherited qemu-arm would run the x64 binaries.
.PHONY: check-all-targets
check-all-targets:
	$(Q)set -e; \
	unset TARGET_EXEC; \
	active_arch='$(or $(CONFIGURED_ARCH),$(ARCH))'; \
	trap '$(MAKE) config ARCH="$$active_arch"' EXIT; \
	for target_arch in $(ARCHS); do \
		$(MAKE) config ARCH="$$target_arch"; \
		$(MAKE) check ARCH="$$target_arch"; \
	done

.PHONY: vir-baseline
vir-baseline: $(OUT)/$(STAGE0) tests/vir-baseline.sh tests/benchmark-time.sh \
	tests/vir-baseline-workloads.txt
	VIR_BASELINE_RUNS=$${VIR_BASELINE_RUNS:-5} tests/vir-baseline.sh

.PHONY: vir-ssa-baseline
vir-ssa-baseline: $(OUT)/$(STAGE0) tests/vir-ssa-baseline.sh tests/benchmark-time.sh \
	tests/vir-ssa-baseline-workloads.txt tests/vir-frontend-large-cfg.c
	tests/vir-ssa-baseline.sh 0

.PHONY: vir-licm-profile
VIR_LICM_PROFILE_SAMPLES ?= 7
VIR_LICM_PROFILE_REPEATS ?= 5
vir-licm-profile: $(OUT)/vir-licm-profile
	$(OUT)/vir-licm-profile $(VIR_LICM_PROFILE_SAMPLES) $(VIR_LICM_PROFILE_REPEATS)

$(OUT)/vir-licm-profile: tests/vir-licm-profile.c src/vir.c src/vir.h
	$(CC) $(CFLAGS) -Isrc tests/vir-licm-profile.c src/vir.c -o $@

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

check-sanitizer: $(OUT)/$(STAGE0)-sanitizer tests/driver.sh check-vir-sanitizer
	$(VECHO) "  TEST STAGE 0 (with sanitizers)\n"
	$(Q)cp $(OUT)/$(STAGE0)-sanitizer $(OUT)/shecc
	tests/driver.sh 0 $(DYNLINK)
	$(Q)rm $(OUT)/shecc

check-abi-stage0: $(OUT)/$(STAGE0)
	tests/$(ARCH)-abi.sh 0 $(DYNLINK);

check-abi-stage2: $(OUT)/$(STAGE2)
	tests/$(ARCH)-abi.sh 2 $(DYNLINK);

check-vir-core: tests/vir-core.c src/vir.c src/vir.h
	$(CC) $(CFLAGS) -Isrc tests/vir-core.c src/vir.c -o $(OUT)/vir-core
	$(OUT)/vir-core

check-vir-frontend-stage0: $(OUT)/$(STAGE0) tests/vir-frontend.sh tests/vir-frontend-workloads.txt $(VIR_FRONTEND_FIXTURES)
	TARGET_EXEC="$(TARGET_EXEC)" bash tests/vir-frontend.sh 0

check-vir-frontend-stage2: $(OUT)/$(STAGE2) tests/vir-frontend.sh tests/vir-frontend-workloads.txt $(VIR_FRONTEND_FIXTURES)
	TARGET_EXEC="$(TARGET_EXEC)" bash tests/vir-frontend.sh 2

check-vir-frontend-policy-stage0: $(OUT)/$(STAGE0) tests/vir-frontend-policy.sh tests/vir-frontend-gvn.c tests/vir-frontend-licm.c tests/vir-frontend-nested-loop.c tests/vir-frontend-multi-latch.c
	TARGET_EXEC="$(TARGET_EXEC)" bash tests/vir-frontend-policy.sh 0

check-vir-frontend-policy-stage2: $(OUT)/$(STAGE2) tests/vir-frontend-policy.sh tests/vir-frontend-gvn.c tests/vir-frontend-licm.c tests/vir-frontend-nested-loop.c tests/vir-frontend-multi-latch.c
	TARGET_EXEC="$(TARGET_EXEC)" bash tests/vir-frontend-policy.sh 2

check-vir-fuzz: tests/vir-fuzz.c src/vir.c src/vir.h
	$(CC) $(CFLAGS) -Isrc tests/vir-fuzz.c src/vir.c -o $(OUT)/vir-fuzz
	$(OUT)/vir-fuzz

check-vir-sanitizer: tests/vir-core.c tests/vir-fuzz.c src/vir.c src/vir.h
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Isrc tests/vir-core.c src/vir.c -o $(OUT)/vir-core-sanitize
	ASAN_OPTIONS=detect_leaks=1 $(OUT)/vir-core-sanitize
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Isrc tests/vir-fuzz.c src/vir.c -o $(OUT)/vir-fuzz-sanitize
	ASAN_OPTIONS=detect_leaks=1 $(OUT)/vir-fuzz-sanitize

.PHONY: check-vir
check-vir: check-vir-core check-vir-frontend-stage0 \
	check-vir-frontend-stage2 check-vir-fuzz check-vir-stage0 check-vir-stage2 \
	check-vir-frontend-policy-stage0 \
	check-vir-frontend-policy-stage2 \
	check-vir-arm-verifier-stage0 check-vir-arm-verifier-stage2 check-vir-sanitizer

check-vir-stage0: $(OUT)/$(STAGE0) tests/vir-stage0.c src/vir.c src/vir.h
	$(OUT)/$(STAGE0) -o $(OUT)/vir-stage0 tests/vir-stage0.c
	$(TARGET_EXEC) $(OUT)/vir-stage0

check-vir-stage2: $(OUT)/$(STAGE2) tests/vir-stage0.c src/vir.c src/vir.h
	$(TARGET_EXEC) $(OUT)/$(STAGE2) -o $(OUT)/vir-stage2 tests/vir-stage0.c
	$(TARGET_EXEC) $(OUT)/vir-stage2

.PHONY: check-vir-native-regressions-stage0 check-vir-native-regressions-stage2
check: check-vir-native-regressions-stage0 check-vir-native-regressions-stage2 check-vir-lower
check-vir-native-regressions-stage0: $(OUT)/$(STAGE0)
	TARGET_EXEC="$(TARGET_EXEC)" tests/vir-native-regressions.sh $(OUT)/$(STAGE0)
check-vir-native-regressions-stage2: $(OUT)/$(STAGE2)
	TARGET_EXEC="$(TARGET_EXEC)" tests/vir-native-regressions.sh $(TARGET_EXEC) $(OUT)/$(STAGE2)

.PHONY: check-vir-lower
check-vir-lower: $(OUT)/$(STAGE0) tests/vir-lower.c tests/vir-lower-smoke.c tests/vir-lower.sh
	CC="$(CC)" TARGET_EXEC="$(TARGET_EXEC)" bash tests/vir-lower.sh

# This formerly exposed an Arm stage-0 verifier miscompile. Keep it in both
# bootstrap gates and on every target so CFG-heavy verifier paths cannot regress.
check-vir-arm-verifier-stage0: $(OUT)/$(STAGE0) tests/vir-arm-verifier.c src/vir.c src/vir.h
	$(OUT)/$(STAGE0) -o $(OUT)/vir-arm-verifier-stage0 tests/vir-arm-verifier.c
	$(TARGET_EXEC) $(OUT)/vir-arm-verifier-stage0

check-vir-arm-verifier-stage2: $(OUT)/$(STAGE2) tests/vir-arm-verifier.c src/vir.c src/vir.h
	$(TARGET_EXEC) $(OUT)/$(STAGE2) -o $(OUT)/vir-arm-verifier-stage2 tests/vir-arm-verifier.c
	$(TARGET_EXEC) $(OUT)/vir-arm-verifier-stage2

check-vir-stats-stage0: $(OUT)/$(STAGE0) tests/vir-stats.sh
	tests/vir-stats.sh 0

check-vir-stats-stage2: $(OUT)/$(STAGE2) tests/vir-stats.sh
	tests/vir-stats.sh 2

check-vir-baseline: $(OUT)/$(STAGE0) tests/vir-baseline.sh tests/benchmark-time.sh \
	tests/vir-baseline-workloads.txt
	VIR_BASELINE_RUNS=1 VIR_BASELINE_NO_TIMING=1 tests/vir-baseline.sh

check-vir-ssa-baseline-stage0: $(OUT)/$(STAGE0) tests/vir-ssa-baseline.sh tests/benchmark-time.sh \
	tests/vir-ssa-baseline-workloads.txt tests/vir-frontend-large-cfg.c
	VIR_SSA_BASELINE_NO_TIMING=1 VIR_SSA_BASELINE_RUNS=2 VIR_SSA_BASELINE_MANIFEST=tests/vir-ssa-baseline-workloads.txt tests/vir-ssa-baseline.sh 0

check-vir-ssa-baseline-stage2: $(OUT)/$(STAGE2) tests/vir-ssa-baseline.sh tests/benchmark-time.sh \
	tests/vir-ssa-baseline-workloads.txt tests/vir-frontend-large-cfg.c
	TARGET_EXEC="$(TARGET_EXEC)" VIR_SSA_BASELINE_NO_TIMING=1 VIR_SSA_BASELINE_RUNS=2 VIR_SSA_BASELINE_MANIFEST=tests/vir-ssa-baseline-workloads.txt tests/vir-ssa-baseline.sh 2

VIR_SSA_BASELINE_NORMALIZE = sed -e 's/stage=[02] /stage=both /' -e 's/ output_bytes=[0-9][0-9]*//'

# The recipe runs both stages itself to compare them, so depending on the
# per-stage targets would run every workload twice.
check-vir-ssa-baseline: $(OUT)/$(STAGE0) $(OUT)/$(STAGE2) tests/vir-ssa-baseline.sh \
	tests/benchmark-time.sh tests/vir-ssa-baseline-workloads.txt tests/vir-frontend-large-cfg.c
	@set -e; stage0=$$(mktemp); stage2=$$(mktemp); trap 'rm -f "$$stage0" "$$stage2" "$$stage0.normalized" "$$stage2.normalized"' EXIT; \
	VIR_SSA_BASELINE_NO_TIMING=1 VIR_SSA_BASELINE_RUNS=2 VIR_SSA_BASELINE_MANIFEST=tests/vir-ssa-baseline-workloads.txt tests/vir-ssa-baseline.sh 0 > "$$stage0"; \
	TARGET_EXEC="$(TARGET_EXEC)" VIR_SSA_BASELINE_NO_TIMING=1 VIR_SSA_BASELINE_RUNS=2 VIR_SSA_BASELINE_MANIFEST=tests/vir-ssa-baseline-workloads.txt tests/vir-ssa-baseline.sh 2 > "$$stage2"; \
	$(VIR_SSA_BASELINE_NORMALIZE) "$$stage0" > "$$stage0.normalized"; \
	$(VIR_SSA_BASELINE_NORMALIZE) "$$stage2" > "$$stage2.normalized"; \
	cmp -s "$$stage0.normalized" "$$stage2.normalized"

check-vir-ssa-selfhost-stage0: $(OUT)/$(STAGE0) tests/vir-ssa-baseline.sh tests/benchmark-time.sh \
	tests/vir-ssa-selfhost-workloads.txt
	VIR_SSA_BASELINE_NO_TIMING=1 VIR_SSA_BASELINE_RUNS=2 VIR_SSA_BASELINE_MANIFEST=tests/vir-ssa-selfhost-workloads.txt tests/vir-ssa-baseline.sh 0

check-vir-ssa-selfhost-stage2: $(OUT)/$(STAGE2) tests/vir-ssa-baseline.sh tests/benchmark-time.sh \
	tests/vir-ssa-selfhost-workloads.txt
	TARGET_EXEC="$(TARGET_EXEC)" VIR_SSA_BASELINE_NO_TIMING=1 VIR_SSA_BASELINE_RUNS=2 VIR_SSA_BASELINE_MANIFEST=tests/vir-ssa-selfhost-workloads.txt tests/vir-ssa-baseline.sh 2

check-vir-ssa-selfhost: $(OUT)/$(STAGE0) $(OUT)/$(STAGE2) tests/vir-ssa-baseline.sh \
	tests/benchmark-time.sh tests/vir-ssa-selfhost-workloads.txt
	@set -e; stage0=$$(mktemp); stage2=$$(mktemp); trap 'rm -f "$$stage0" "$$stage2" "$$stage0.normalized" "$$stage2.normalized"' EXIT; \
	VIR_SSA_BASELINE_NO_TIMING=1 VIR_SSA_BASELINE_RUNS=2 VIR_SSA_BASELINE_MANIFEST=tests/vir-ssa-selfhost-workloads.txt tests/vir-ssa-baseline.sh 0 > "$$stage0"; \
	TARGET_EXEC="$(TARGET_EXEC)" VIR_SSA_BASELINE_NO_TIMING=1 VIR_SSA_BASELINE_RUNS=2 VIR_SSA_BASELINE_MANIFEST=tests/vir-ssa-selfhost-workloads.txt tests/vir-ssa-baseline.sh 2 > "$$stage2"; \
	$(VIR_SSA_BASELINE_NORMALIZE) "$$stage0" > "$$stage0.normalized"; \
	$(VIR_SSA_BASELINE_NORMALIZE) "$$stage2" > "$$stage2.normalized"; \
	cmp -s "$$stage0.normalized" "$$stage2.normalized"

# Both prerequisites are order-only, and both exist because "make -j" would
# otherwise let a compile start beside the thing it reads. Selecting a target
# replaces src/codegen.c with "ln -sf", which unlinks before it relinks, so a
# compile racing "config" can find nothing there. And src/main.c includes
# out/libc.inc, which is generated: listing it only on the stage0 link left
# the two free to run in either order on a tree that has never been built.
$(OBJS) $(SAN_OBJS): $(OUT)/libc.inc

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

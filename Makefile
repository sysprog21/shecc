CC ?= gcc
CFLAGS := -O -g \
	-ansi -pedantic \
	-Wall -Wextra

include mk/common.mk
include mk/arm.mk
include mk/riscv.mk

STAGE0 := shecc
STAGE1 := shecc-stage1.elf
STAGE2 := shecc-stage2.elf

OUT ?= out
SRCDIR := $(shell find src -type d)
LIBDIR := $(shell find lib -type d)

SRCS := $(wildcard $(patsubst %,%/main.c, $(SRCDIR)))
OBJS := $(SRCS:%.c=$(OUT)/%.o)
deps := $(OBJS:%.o=%.o.d)
TESTS := $(wildcard tests/*.c)
TESTBINS := $(TESTS:%.c=$(OUT)/%.elf)

all: config bootstrap

# set ARM by default
ifeq ($(strip $(ARCH)),riscv)
ARCH = riscv
else
ARCH = arm
endif

ifneq ("$(wildcard $(PWD)/config)","")
TARGET_EXEC := $($(shell head -1 config | sed 's/.*: \([^ ]*\).*/\1/')_EXEC)
endif
export TARGET_EXEC

config:
	$(Q)ln -s $(PWD)/$(SRCDIR)/$(ARCH)-codegen.c $(SRCDIR)/codegen.c
	$(call $(ARCH)-specific-defs) > $@
	$(VECHO) "Target machine code switch to %s\n" $(ARCH)

$(OUT)/tests/%.elf: tests/%.c $(OUT)/$(STAGE0)
	$(VECHO) "  SHECC\t$@\n"
	$(Q)$(OUT)/$(STAGE0) --dump-ir -o $@ $< > $(basename $@).log ; \
	chmod +x $@ ; $(PRINTF) "Running $@ ...\n"
	$(Q)$(TARGET_EXEC) $@ && $(call pass)

check: $(TESTBINS) tests/driver.sh
	tests/driver.sh

$(OUT)/%.o: %.c
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF $@.d $<

SHELL_HACK := $(shell mkdir -p $(OUT) $(OUT)/$(SRCDIR) $(OUT)/tests)
$(OUT)/libc.inc: $(OUT)/inliner $(LIBDIR)/c.c
	$(VECHO) "  GEN\t$@\n"
	$(Q)$(OUT)/inliner $(LIBDIR)/c.c $@

$(OUT)/inliner: tools/inliner.c
	$(VECHO) "  CC+LD\t$@\n"
	$(Q)$(CC) $(CFLAGS) -o $@ $^

$(OUT)/$(STAGE0): $(OUT)/libc.inc $(OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) $(OBJS) -o $@

$(OUT)/$(STAGE1): $(OUT)/$(STAGE0)
	$(VECHO) "  SHECC\t$@\n"
	$(Q)$(OUT)/$(STAGE0) --dump-ir -o $@ $(SRCDIR)/main.c > $(OUT)/shecc-stage1.log
	$(Q)chmod a+x $@

$(OUT)/$(STAGE2): $(OUT)/$(STAGE1)
	$(VECHO) "  SHECC\t$@\n"
	$(Q)$(TARGET_EXEC) $(OUT)/$(STAGE1) -o $@ $(SRCDIR)/main.c

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
	-$(RM) $(OUT)/inliner $(OUT)/libc.inc $(OUT)/target config $(SRCDIR)/codegen.c

-include $(deps)
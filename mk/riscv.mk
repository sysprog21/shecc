RISCV_EXEC = qemu-riscv32                                                                                                                                           
RISCV_EXEC := $(shell which $(RISCV_EXEC))
ifndef RISCV_EXEC
$(warning "no qemu-riscv32 found. Please check package installation")
RISCV_EXEC = echo WARN: unable to run
endif

export RISCV_EXEC
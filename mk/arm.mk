CROSS_COMPILE ?= arm-linux-gnueabihf-

ARM_EXEC = qemu-arm                                                                                                                                           
ARM_EXEC := $(shell which $(ARM_EXEC))
ifndef ARM_EXEC
$(warning "no qemu-arm found. Please check package installation")
ARM_EXEC = echo WARN: unable to run
endif

export ARM_EXEC

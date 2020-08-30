CROSS_COMPILE ?= arm-linux-gnueabihf-

ARM_EXEC = qemu-arm                                                                                                                                           
ARM_EXEC := $(shell which $(ARM_EXEC))
ifndef ARM_EXEC
$(error "no qemu-arm found. Please check package installation")
endif

export ARM_EXEC

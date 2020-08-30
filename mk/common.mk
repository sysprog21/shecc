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

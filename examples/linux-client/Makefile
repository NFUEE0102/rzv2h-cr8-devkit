# Builds on the board (aarch64) with the stock toolchain.
# Requires libmetal / libopen_amp / libsysfs installed (see docs/04).
CC      ?= gcc
CFLAGS  ?= -O2 -Wall
LDLIBS   = -lopen_amp -lmetal -lsysfs -lpthread -lm
SRCS     = main.c global_vars.c helper.c platform_info.c rz_rproc.c
BIN      = r8_bench

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

clean:
	rm -f $(BIN)
.PHONY: clean

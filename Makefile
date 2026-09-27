# P2500 emulator.
#
# The tree is split three ways (TODO.md T33):
#
#   src/core/ -> libp2500.a  the machine model. C11, depends on nothing but
#                libc, holds no global state, and must stay that way - it is
#                what lets `make test` run headless and what keeps other
#                embeddings (a GUI, Emscripten) possible.
#   src/cli/  -> p2500-emu   the headless harness. What `make test` drives.
#   src/gui/  -> p2500-gui   SDL3 + Dear ImGui (TODO.md T36+). Does not exist
#                yet; when it does it is the only C++ in the tree, and
#                nothing in core/ or cli/ may depend on it.
#
# Core sources are picked up by wildcard, so adding a device to src/core/
# needs no edit here.

CC = cc
AR = ar
CFLAGS = -std=c11 -Wall -Wextra -O2 -Isrc

LIB = libp2500.a
BIN = p2500-emu

CORE_SRC = $(wildcard src/core/*.c) src/core/vendor/superzazu_z80/z80.c
CLI_SRC = $(wildcard src/cli/*.c)
CORE_OBJ = $(CORE_SRC:.c=.o)
CLI_OBJ = $(CLI_SRC:.c=.o)

.PHONY: all clean run test

all: $(BIN)

$(LIB): $(CORE_OBJ)
	$(AR) rcs $@ $^

$(BIN): $(CLI_OBJ) $(LIB)
	$(CC) $(CFLAGS) -o $@ $(CLI_OBJ) $(LIB)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(BIN) $(LIB) $(CORE_OBJ) $(CLI_OBJ)

run: $(BIN)
	./$(BIN) --rom roms/ipl.bin

test: $(BIN)
	@tools/run_tests.sh

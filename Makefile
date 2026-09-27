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
# -MMD -MP emits a .d per object listing the headers it used, so editing a
# header rebuilds everything that includes it. Without this, changing a
# struct in a header silently leaves other objects compiled against the old
# layout - which is a memory-corruption bug, not a stale-build annoyance.
CFLAGS = -std=c11 -Wall -Wextra -O2 -Isrc -MMD -MP

LIB = libp2500.a
BIN = p2500-emu

CORE_SRC = $(wildcard src/core/*.c) src/core/vendor/superzazu_z80/z80.c
CLI_SRC = $(wildcard src/cli/*.c)
CORE_OBJ = $(CORE_SRC:.c=.o)
CLI_OBJ = $(CLI_SRC:.c=.o)
DEPS = $(CORE_OBJ:.o=.d) $(CLI_OBJ:.o=.d)

.PHONY: all clean run test

all: $(BIN)

$(LIB): $(CORE_OBJ)
	$(AR) rcs $@ $^

$(BIN): $(CLI_OBJ) $(LIB)
	$(CC) $(CFLAGS) -o $@ $(CLI_OBJ) $(LIB)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(BIN) $(LIB) $(CORE_OBJ) $(CLI_OBJ) $(DEPS)

-include $(DEPS)

run: $(BIN)
	./$(BIN) --rom roms/ipl.bin

test: $(BIN)
	@tools/run_tests.sh

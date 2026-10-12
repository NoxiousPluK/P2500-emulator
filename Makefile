# P2500 emulator.
#
# The tree is split three ways:
#
#   src/core/ -> libp2500.a  the machine model. C11, depends on nothing but
#                libc, holds no global state, and must stay that way - it is
#                what lets `make test` run headless and what keeps other
#                embeddings (a GUI, Emscripten) possible.
#   src/cli/  -> p2500-emu   the headless harness. What `make test` drives.
#   src/gui/  -> p2500-gui   SDL3 + Dear ImGui, the only C++ in the tree.
#                Nothing in core/ or cli/ may depend on it.
#
# Core sources are picked up by wildcard, so adding a device to src/core/
# needs no edit here.
#
# Every object, dependency file and the library itself go under
# build/<toolchain>/, mirroring the source tree. That is not tidiness: the
# native and MinGW compilers used to write the same .o and libp2500.a
# paths, so `MINGW=1 make` after a native `make` linked ELF objects into a
# PE binary and failed - and, those objects being newer than their sources,
# never rebuilt them. Only the final binaries sit at the top level, where
# EXE_SUFFIX already keeps the two apart.

# MINGW=1 cross-compiles for Windows (x86_64-w64-mingw32-gcc/g++, needs the
# mingw-w64 toolchain and a mingw-w64 SDL3 build for the GUI). Everything
# below reads through CC/CXX/AR/PKG_CONFIG/EXE_SUFFIX/*_LDFLAGS rather than
# assuming the native toolchain, so this is the only place the two differ.
ifdef MINGW
CC = x86_64-w64-mingw32-gcc
CXX = x86_64-w64-mingw32-g++
AR = x86_64-w64-mingw32-ar
PKG_CONFIG = x86_64-w64-mingw32-pkg-config
WINDRES = x86_64-w64-mingw32-windres
EXE_SUFFIX = .exe
BUILD = build/mingw
# The PE icon resource. Windows reads the .exe's own icon off this; the
# SDL window icon is set at runtime from the same artwork (src/gui/icon.cpp).
# ELF has no equivalent, so this is Windows-only and stays empty elsewhere.
RES_OBJ = $(BUILD)/src/win/p2500.res.o
# p2500-emu has no other dependencies, so link it fully static: no
# MinGW-runtime DLLs to carry, only the Universal CRT forwarder DLLs,
# which Windows 10+ ships itself.
BIN_LDFLAGS = -static
# p2500-gui keeps SDL3 dynamic (SDL3.dll ships alongside it - see
# publish.sh): linking SDL3 itself statically pulls in a long tail of
# Windows system libraries (ole32, setupapi, cfgmgr32, ...) that the
# mingw-w64-sdl3 package's pkg-config file does not fully account for.
# Only the MinGW runtime is forced static, via -Wl,-Bstatic bracketing
# just libwinpthread so -lSDL3 right after it still resolves against the
# shared import library.
GUI_LDFLAGS = -static-libgcc -static-libstdc++ \
              -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,-Bdynamic,--no-whole-archive
else
CC = cc
CXX = c++
AR = ar
PKG_CONFIG = pkg-config
EXE_SUFFIX =
BUILD = build/native
RES_OBJ =
BIN_LDFLAGS =
GUI_LDFLAGS = -Wl,-rpath,'$$ORIGIN/lib'
endif

# -MMD -MP emits a .d per object listing the headers it used, so editing a
# header rebuilds everything that includes it.
# -Werror=format is the only warning made fatal, and it is here for one
# measured reason: a `%zu` inside an SDL_Log is three warnings and exit 0 on
# MinGW, so a cross build that merely succeeds proves nothing. It applies to
# the project's own code, not to the vendored ImGui below - see
# ../P2500-emulator-LLM-docs/windows-cross-build.md.
CFLAGS = -std=c11 -Wall -Wextra -Werror=format -O2 -Isrc -MMD -MP
# The GUI is C++ because Dear ImGui is. It is confined to src/gui/ and the
# vendored ImGui is compiled without -Wall/-Wextra.
IMGUI_DIR = src/gui/vendor/imgui
CXXFLAGS = -std=c++17 -O2 -Isrc -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -MMD -MP
GUI_CXXFLAGS = $(CXXFLAGS) -Wall -Wextra -Werror=format

LIB = $(BUILD)/libp2500.a
BIN = p2500-emu$(EXE_SUFFIX)
GUI = p2500-gui$(EXE_SUFFIX)

# The GUI is built only when asked for, and only if SDL3 is present, so a
# headless checkout still builds and `make test` still runs.
# Test for the package itself: --cflags is legitimately EMPTY when SDL3's
# headers sit on the default include path, so it cannot be the probe.
HAVE_SDL3 := $(shell $(PKG_CONFIG) --exists sdl3 && echo yes)
SDL_CFLAGS = $(shell $(PKG_CONFIG) --cflags sdl3 2>/dev/null)
SDL_LIBS = $(shell $(PKG_CONFIG) --libs sdl3 2>/dev/null)

CORE_SRC = $(wildcard src/core/*.c) src/core/vendor/superzazu_z80/z80.c
CLI_SRC = $(wildcard src/cli/*.c)
GUI_SRC = $(wildcard src/gui/*.cpp)
IMGUI_SRC = $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp \
            $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp \
            $(IMGUI_DIR)/backends/imgui_impl_sdl3.cpp \
            $(IMGUI_DIR)/backends/imgui_impl_sdlrenderer3.cpp
IMGUI_OBJ = $(IMGUI_SRC:%.cpp=$(BUILD)/%.o)
CORE_OBJ = $(CORE_SRC:%.c=$(BUILD)/%.o)
CLI_OBJ = $(CLI_SRC:%.c=$(BUILD)/%.o)
GUI_OBJ = $(GUI_SRC:%.cpp=$(BUILD)/%.o)
DEPS = $(CORE_OBJ:.o=.d) $(CLI_OBJ:.o=.d) $(GUI_OBJ:.o=.d) $(IMGUI_OBJ:.o=.d)

# Core-level test binaries: these link libp2500.a directly, because what
# they check cannot be reached through any guest program this project has -
# a read bounded by EOT, the write paths no surviving software takes
# (FORMAT TRACK, the write-protect tab, a DMA programmed the wrong way), and
# the configuration syntax, whose load-bearing claim is about bytes in a
# file rather than about anything the machine does.
# Built by `make test`, not part of `all`.
TEST_SRC = tests/fdc_eot tests/fdc_write tests/ini
TEST_BIN = $(addsuffix $(EXE_SUFFIX),$(TEST_SRC))

.PHONY: all clean run test gui demos

# The GUI is part of the default build whenever SDL3 is present,because 
# tests/run_tests.sh tests ./p2500-gui if it exists. A machine with no
# SDL3 still builds and still runs the suite; it just skips the GUI checks.
GUI_IF_AVAILABLE = $(if $(HAVE_SDL3),$(GUI))

all: $(BIN) $(GUI_IF_AVAILABLE)

$(LIB): $(CORE_OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(BIN): $(CLI_OBJ) $(LIB) $(RES_OBJ)
	$(CC) $(CFLAGS) $(BIN_LDFLAGS) -o $@ $(CLI_OBJ) $(LIB) $(RES_OBJ)

gui: $(GUI)

$(GUI): $(GUI_OBJ) $(IMGUI_OBJ) $(LIB) $(RES_OBJ)
	@test "$(HAVE_SDL3)" = yes || { echo "sdl3 not found by pkg-config - install it to build the GUI"; exit 1; }
	$(CXX) $(CXXFLAGS) $(GUI_LDFLAGS) -o $@ $(GUI_OBJ) $(IMGUI_OBJ) $(LIB) $(SDL_LIBS) $(RES_OBJ)

# -I. so the .rc's path to assets/icon/ resolves from the project root.
$(BUILD)/src/win/%.res.o: src/win/%.rc assets/icon/p2500icon.ico
	@mkdir -p $(@D)
	$(WINDRES) -I. -O coff -i $< -o $@

# Three compile rules, and make picks between them by shortest stem, so the
# vendored-ImGui rule wins over the src/gui/ one for its own sources and
# keeps being compiled without -Wall/-Wextra/-Werror.
$(BUILD)/src/gui/%.o: src/gui/%.cpp
	@test "$(HAVE_SDL3)" = yes || { echo "sdl3 not found by pkg-config - install it to build the GUI"; exit 1; }
	@mkdir -p $(@D)
	$(CXX) $(GUI_CXXFLAGS) $(SDL_CFLAGS) -c -o $@ $<

$(BUILD)/$(IMGUI_DIR)/%.o: $(IMGUI_DIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# The graphics demos. Assembled by tools/z80asm.py, which verifies its own
# output against the disassembler in libp2500, and packaged by
# tools/cpm_build.py onto a bootable image. Not part of `all` since it's a
# separate build product, not something every build needs.
DEMO_ASM = demos/logo.asm demos/stars.asm demos/spiro.asm demos/bench.asm
DEMO_COM = $(DEMO_ASM:.asm=.COM)
DEMO_DISK = demos/P2500DEMO.raw
BOOT_DONOR ?= disks/P25K_B.raw

demos: $(DEMO_DISK)

demos/%.COM: demos/%.asm demos/p2500.inc demos/logo_sprite.asm tools/z80asm.py $(BIN)
	python3 tools/z80asm.py $< -o $@ --verify

$(DEMO_DISK): $(DEMO_COM) tools/cpm_build.py
	@test -f "$(BOOT_DONOR)" || { echo "need a bootable P2500 image: BOOT_DONOR=path make demos"; exit 1; }
	python3 tools/cpm_build.py $@ $(DEMO_COM) --boot-from "$(BOOT_DONOR)"

tests/%$(EXE_SUFFIX): tests/%.c $(LIB)
	$(CC) $(CFLAGS) $(BIN_LDFLAGS) -o $@ $< $(LIB)

# Cleans BOTH toolchains whichever one invoked it: build/ holds all the
# objects, and the binaries are swept under both suffixes. A tree that
# cannot be fully cleaned from one command is how stale cross objects got
# linked in the first place.
CLEAN_BIN = p2500-emu p2500-gui $(TEST_SRC)
clean:
	rm -rf build
	rm -f $(CLEAN_BIN) $(addsuffix .exe,$(CLEAN_BIN)) $(addsuffix .d,$(CLEAN_BIN))
	rm -f $(DEMO_COM) $(DEMO_DISK)
#	Objects lived beside their sources until the per-toolchain split, so
#	sweep those too - a tree built before it then converges on one clean.
	rm -f libp2500.a src/win/*.res.o
	find src -name '*.o' -delete -o -name '*.d' -delete

-include $(DEPS)

run: $(BIN)
	./$(BIN) --rom roms/ipl.bin

# T= selects sections by id or short name (tests/run_tests.sh --list);
# empty, which is what `make test` means, runs all of them.
T =
test: $(BIN) $(GUI_IF_AVAILABLE) $(TEST_BIN)
	@tests/run_tests.sh $(T)

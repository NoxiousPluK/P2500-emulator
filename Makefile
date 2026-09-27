CC = cc
CFLAGS = -std=c11 -Wall -Wextra -O2 -Isrc
SRC = src/main.c src/machine.c src/intctl.c src/fdc.c src/sesam.c src/pio.c src/dma.c src/ctc.c src/keyboard.c src/vendor/superzazu_z80/z80.c
BIN = p2500-emu

.PHONY: all clean run test

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC)

clean:
	rm -f $(BIN)

run: $(BIN)
	./$(BIN) --rom roms/ipl.bin

test: $(BIN)
	@tools/run_tests.sh

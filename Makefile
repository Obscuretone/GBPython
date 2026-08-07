GBDK_HOME ?= $(HOME)/gbdk/gbdk
LCC := $(GBDK_HOME)/bin/lcc

# MBC5+RAM+BATTERY, 4 SRAM banks (matches SWITCH_RAM(1)/SWITCH_RAM(2) usage)
# -Wl-m/-Wl-j regenerate the .map/.noi symbol files the test harness reads;
# stale symbols make the harness poke the wrong WRAM addresses.
LCCFLAGS := -Wm-yoA -Wm-yt0x1B -Wm-ya4 -Wm-yn"GBPYTHON" -Wl-m -Wl-j

ROM := gbpython.gb
SRC := gbpython.c ui.c lexer.c float32.c runtime.c runtime2.c parser.c builtins.c methods.c interpreter.c

all: $(ROM)

$(ROM): $(SRC) gbpython.h
	$(LCC) $(LCCFLAGS) -o $@ $(SRC)

PYTHON ?= /opt/homebrew/bin/python3.13

test: $(ROM)
	cd tests && $(PYTHON) run_tests.py

clean:
	rm -f $(ROM) gbpython.map gbpython.noi gbpython.ihx gbpython.lst gbpython.sym gbpython.adb gbpython.asm gbpython.o gbpython.lk gbpython.rel

.PHONY: all test clean

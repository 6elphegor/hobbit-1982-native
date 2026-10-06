CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-missing-field-initializers -std=c11
# The clean edition's adapters for the faithful port (clean/*.c): in every
# build but hobbit-clean, the clean edition alone.
ADAPTERS := -DCLEAN_ADAPTERS
SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS := $(shell sdl2-config --libs 2>/dev/null)
BUILD := build

COMMON := common/tzx.c
PORT := $(wildcard port/*.c) $(wildcard clean/*.c)
REF := ref/spectrum.c ref/hybrid.c ref/script.c third_party/z80/z80.c $(COMMON) $(PORT)
HDRS := ref/spectrum.h ref/hybrid.h ref/script.h common/tzx.h $(wildcard port/*.h) $(wildcard clean/*.h)

all: $(BUILD)/hobbit $(BUILD)/hobbit-clean-sdl $(BUILD)/hobbit-ref $(BUILD)/hobbit-ref-sdl $(BUILD)/hobbit-replay $(BUILD)/hobbit-clean

# The game as a native program: the ported code, no Z80.
$(BUILD)/hobbit: native/hobbit.c native/window.c native/window.h $(REF) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(ADAPTERS) $(SDL_CFLAGS) -o $@ native/hobbit.c native/window.c $(REF) $(SDL_LIBS)

# The clean edition alone, in a window.
$(BUILD)/hobbit-clean-sdl: native/hobbit-clean.c native/window.c native/window.h ref/cleanhost.c ref/spectrum.c \
		third_party/z80/z80.c $(COMMON) $(wildcard clean/*.c) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ native/hobbit-clean.c native/window.c ref/cleanhost.c ref/spectrum.c \
		third_party/z80/z80.c $(COMMON) $(wildcard clean/*.c) $(SDL_LIBS)

# The clean edition alone: clean/ and its host, no port/ and no Z80 code
# run (the test machine's struct holds one, for its memory and devices).
CLEAN_ONLY := ref/cleanrun.c ref/cleanhost.c ref/script.c ref/spectrum.c third_party/z80/z80.c $(COMMON) \
	$(wildcard clean/*.c)
$(BUILD)/hobbit-clean: $(CLEAN_ONLY) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(CLEAN_ONLY)

$(BUILD)/hobbit-replay: ref/replay.c $(REF) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(ADAPTERS) -o $@ ref/replay.c $(REF)

$(BUILD)/hobbit-ref: ref/refrun.c $(REF) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(ADAPTERS) -o $@ ref/refrun.c $(REF)

$(BUILD)/hobbit-ref-sdl: ref/refsdl.c $(REF) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(ADAPTERS) $(SDL_CFLAGS) -o $@ ref/refsdl.c $(REF) $(SDL_LIBS)

$(BUILD):
	mkdir -p $@

TAPE ?= Hobbit, The v1.2 (1982)(Melbourne House).tzx

ROM ?= 48.rom

# With the ROM present, tests run against it (tests/expected-rom).
# BUILD=dir builds and tests elsewhere (one directory per person or agent
# working at once); ONLY=6E97,7585 limits check/hybrid to those routines;
# STRICT=1 checks the stack scratch too (--strict-stack). The default
# modes are check and native, every (script, mode) pair a parallel job;
# FULL=1 adds hybrid, the original alone and a determinism rerun.
# RNG=clean tests with the proper random number generator
# (tests/expected-clean), with the clean edition (clean/) run and checked
# against the faithful port. (NATIVE=1 is accepted; native always runs.)
test: $(BUILD)/hobbit-ref $(BUILD)/hobbit-clean
	if [ -f "$(ROM)" ]; then export HOBBIT_ROM="$(abspath $(ROM))"; fi; \
	BUILD_DIR="$(abspath $(BUILD))" HOBBIT_ONLY="$(ONLY)" HOBBIT_FULL="$(FULL)" HOBBIT_STRICT="$(STRICT)" HOBBIT_RNG="$(RNG)" \
	tests/run.sh "$(TAPE)"

# A quick run while working: a dozen scripts that between them reach every
# part of the game, native only (clean only with RNG=clean), in parallel. `make test`
# is the full suite.
QUICK_SCRIPTS := opening parser sentences save-load crash-repeat actions1-cave \
	events-trolls drawing-forest io-input main-restart meta-tape objects-contents
quick: $(BUILD)/hobbit-ref $(BUILD)/hobbit-clean
	if [ -f "$(ROM)" ]; then export HOBBIT_ROM="$(abspath $(ROM))"; fi; \
	BUILD_DIR="$(abspath $(BUILD))" HOBBIT_ONLY="$(ONLY)" HOBBIT_QUICK=1 HOBBIT_RNG="$(RNG)" \
	HOBBIT_SCRIPTS="$(QUICK_SCRIPTS)" tests/run.sh "$(TAPE)"

clean:
	rm -rf $(BUILD)

.PHONY: all test quick clean

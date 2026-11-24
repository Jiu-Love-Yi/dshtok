# dshtok - build system
#
#   make              build build/dshtok (build/dshtok.exe on Windows)
#   make test         build and run every tests/test_*.c from the repo root
#   make integration  run the end-to-end smoke test (tests/integration.sh)
#   make release      clean rebuild with -O2 -DNDEBUG, statically where possible
#   make clean        remove build/ and build-cmake/
#   make install      install the binary under $(PREFIX)/bin
#
# Layout, straight out of SPEC.md: objects live in build/, the binary links
# src/main.c plus the six core translation units, and every test binary links
# the core objects without main.c.
#
# The recipes are POSIX sh and work on Linux/macOS with GNU make and on Windows
# with mingw32-make run from an MSYS2 or Git Bash shell - the same environment
# the windows job in .github/workflows/ci.yml uses. Plain cmd.exe is not a
# supported recipe shell.

CC       ?= cc
CFLAGS   ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS  ?=
LDLIBS   ?= -lm
PREFIX   ?= /usr/local
DESTDIR  ?=
BUILD    ?= build
TEST_CFLAGS ?= -Itests

# Windows executables carry a suffix. $(OS) is set in every Windows shell,
# MSYS2 included, so the name matches what the compiler actually writes.
#
# Two very different shells can sit behind a recipe on Windows: a POSIX sh when
# make is started from MSYS2 or Git Bash, and cmd.exe when it is started from a
# plain Windows console. The recipes below only need mkdir and a recursive
# delete, so pick the vocabulary that matches the shell that is actually there.
#
# The default SHELL is the first clue: the MSYS2 make package points it at
# /usr/bin/sh, so a leading slash already settles it. The MinGW build defaults
# to the bare name sh.exe, which says nothing about whether it exists, so in
# that case ask `where` - it ships with Windows and writes to stdout only when
# it finds the program. Never run this probe outside Windows: 2>NUL would
# create a file called NUL on a POSIX host.
ifeq ($(OS),Windows_NT)
EXEEXT := .exe
STATIC ?= -static
ifneq ($(filter /%,$(SHELL)),)
HAVE_POSIX := yes
else ifneq ($(strip $(shell where sh 2>NUL)),)
HAVE_POSIX := yes
endif
else
EXEEXT :=
STATIC ?=
HAVE_POSIX := yes
endif

ifeq ($(HAVE_POSIX),yes)
RM_TREE := rm -rf
MKDIR_P := mkdir -p
endif

BIN      := $(BUILD)/dshtok$(EXEEXT)

CORE_SRC := src/util.c src/json.c src/ledger.c src/format.c src/path.c src/report.c
CORE_OBJ := $(CORE_SRC:src/%.c=$(BUILD)/%.o)
TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(TEST_SRC:tests/test_%.c=$(BUILD)/test_%$(EXEEXT))

.PHONY: all test integration release clean install
.SUFFIXES:

all: $(BIN)

$(BIN): $(BUILD)/main.o $(CORE_OBJ) | $(BUILD)
	$(CC) $(LDFLAGS) -o $@ $(BUILD)/main.o $(CORE_OBJ) $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# A test binary compiles its own source and links the core objects; main.c is
# deliberately absent so the test cannot reach into the CLI dispatcher.
$(BUILD)/test_%$(EXEEXT): tests/test_%.c $(CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(TEST_CFLAGS) -o $@ $< $(CORE_OBJ) $(LDFLAGS) $(LDLIBS)

$(BUILD):
ifeq ($(HAVE_POSIX),yes)
	$(MKDIR_P) $(BUILD)
else
	@if not exist "$(BUILD)" mkdir "$(BUILD)"
endif

# Each test gets its own recipe line, built with make functions rather than a
# shell loop, so the runner behaves the same under sh, dash and cmd.exe and the
# first failing binary stops the run.
define run_one_test
	@echo "== $(1)"
	@$(1)
endef

ifeq ($(strip $(TEST_BIN)),)
test:
	$(error make test: no tests/test_*.c found)
else
test: $(TEST_BIN) $(BIN)
	@echo "running $(words $(TEST_BIN)) test binaries from $(CURDIR)"
	$(foreach t,$(TEST_BIN),$(call run_one_test,$(t)))
endif

integration: $(BIN)
	DSHTOK_BIN="$(BIN)" bash tests/integration.sh

# Exercises scripts/install.sh end to end against a local file:// "release":
# download, checksum verification, install mode, and the two failure paths. It
# needs a POSIX shell and curl, so it only runs where integration does.
installer: $(BIN)
	sh tests/installer.sh

release: clean
	@$(MAKE) --no-print-directory CFLAGS="$(CFLAGS) -O2 -DNDEBUG" \
		LDFLAGS="$(LDFLAGS) $(STATIC)" all

clean:
ifeq ($(HAVE_POSIX),yes)
	$(RM_TREE) $(BUILD) build-cmake
else
	@if exist "$(BUILD)" rmdir /s /q "$(BUILD)"
	@if exist "build-cmake" rmdir /s /q "build-cmake"
endif

install: $(BIN)
ifeq ($(HAVE_POSIX),yes)
	$(MKDIR_P) "$(DESTDIR)$(PREFIX)/bin"
	cp $(BIN) "$(DESTDIR)$(PREFIX)/bin/"
else
	@if not exist "$(DESTDIR)$(PREFIX)/bin" mkdir "$(DESTDIR)$(PREFIX)/bin"
	copy /y $(BIN) "$(DESTDIR)$(PREFIX)/bin\\"
endif

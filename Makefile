# Makefile for Test Drive II Enhanced — SDL3 C11 port of Test Drive II: The Duel (1989)
# A thin wrapper over the CMake build; see README.md for the plain cmake commands.
# Targets are numbered by pipeline stage:
#   configure → 1 build → 2 check → 3 run → dev
SERVICE = Test Drive II Enhanced

# Variables
BUILD_DIR = build
BUILD_TYPE ?= Release
GENERATOR ?= Ninja
CMAKE = cmake
GAME_DIR ?= Game
SCALE ?=
FRAME_RATE ?=
ARGS ?=

ifeq ($(OS),Windows_NT)
EXE = .exe
# The README builds with the MSYS2 MinGW64 gcc; without this CMake may pick
# another compiler that is on PATH.
CMAKE_C_COMPILER ?= gcc
else
EXE =
CMAKE_C_COMPILER ?=
endif
BIN = $(BUILD_DIR)/testdrive2-enhanced$(EXE)

# Include flags for the `syntax` target only (the real build gets these from
# CMake). If SDL3 was found through its CMake config rather than pkg-config,
# pass the headers yourself: make syntax SDL_CFLAGS=-I/c/msys64/mingw64/include
SDL_CFLAGS := $(shell pkg-config --cflags sdl3 2>/dev/null)
SYNTAX_FLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing -Isrc $(SDL_CFLAGS)

.PHONY: help configure build check run syntax clean

# ── Environment ──────────────────────────────────────────────────────────────

help: ## Print this help message
	@printf '\033[01;32m$(SERVICE) — SDL3 build, check and run\033[00;37m\n\n'
	@printf "\033[33mUsage:\033[0m\n  make [target] [arg=\"val\"...]\n\n\033[33mTargets:\033[0m\n"
	@grep -E '^[-a-zA-Z0-9_\.\/]+:.*?## .*$$' $(MAKEFILE_LIST) | \
		awk 'BEGIN {FS = ":.*?## "}; \
		{printf "  \033[36m%-26s\033[0m %s\n", $$1, $$2}'

configure: ## Configure the CMake build tree (usage: make configure [BUILD_TYPE=Debug] [GENERATOR="Unix Makefiles"])
	$(CMAKE) -S . -B $(BUILD_DIR) -G "$(GENERATOR)" $(if $(CMAKE_C_COMPILER),-DCMAKE_C_COMPILER=$(CMAKE_C_COMPILER)) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

# Configure once, on demand. CMake re-runs itself when CMakeLists.txt changes,
# so only a change to BUILD_TYPE or GENERATOR needs an explicit `make configure`.
$(BUILD_DIR)/CMakeCache.txt:
	@$(MAKE) --no-print-directory configure

# ── Stage 1 · Build (CMake + Ninja) ──────────────────────────────────────────

build: $(BUILD_DIR)/CMakeCache.txt ## [STEP 1] Build the game binary (usage: make build [BUILD_TYPE=Debug])
	$(CMAKE) --build $(BUILD_DIR)

# ── Stage 2 · Check (load TD2EGA.EXE, no window) ──────────────────────────────

check: build ## [STEP 2] Verify TD2EGA.EXE loads and exit, without opening a window (usage: make check [GAME_DIR=Game])
	$(BIN) --game-dir "$(GAME_DIR)" --check

# ── Stage 3 · Run ────────────────────────────────────────────────────────────

run: build ## [STEP 3] Run the game (usage: make run [GAME_DIR=Game] [SCALE=3] [FRAME_RATE=60] [ARGS="..."])
	$(BIN) --game-dir "$(GAME_DIR)" $(if $(SCALE),--scale $(SCALE)) $(if $(FRAME_RATE),--frame-rate $(FRAME_RATE)) $(ARGS)

# ── Development ───────────────────────────────────────────────────────────────

syntax: ## Syntax-check sources without linking (usage: make syntax [FILE=src/host.c])
	@if [ -z "$(SDL_CFLAGS)" ] && \
	   ! printf '#include <SDL3/SDL.h>\n' | $(CC) -fsyntax-only -x c - >/dev/null 2>&1; then \
		echo "error: SDL3 headers not found." >&2; \
		echo "  pkg-config has no sdl3, and <SDL3/SDL.h> is not on the default include path." >&2; \
		echo "  Install SDL3, set PKG_CONFIG_PATH, or point at the headers directly:" >&2; \
		echo "    make syntax SDL_CFLAGS=-I/c/msys64/mingw64/include" >&2; \
		exit 1; \
	fi
	@files="$${FILE:-$$(find src -name '*.c' | sort)}"; \
	for f in $$files; do \
		echo "  CC -fsyntax-only $$f"; \
		$(CC) $(SYNTAX_FLAGS) -fsyntax-only $$f || exit 1; \
	done
	@echo "Syntax check complete."

clean: ## Remove the build directory and generated binaries
	rm -rf $(BUILD_DIR)
	@echo "Cleanup complete."

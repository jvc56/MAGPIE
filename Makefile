ifeq ($(origin NPROCS), undefined)
NPROCS := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
endif
ifeq ($(filter -j% --jobs%,$(MAKEFLAGS)),)
MAKEFLAGS += -j$(NPROCS)
endif

SRC_DIR := src
TEST_DIR := test
CMD_DIR := cmd
TUI_DIR := tui
OBJ_DIR := obj
TUI_OBJ_DIR := obj_tui
BIN_DIR := bin
COV_DIR := cov

SRC  := $(wildcard $(SRC_DIR)/**/*.c)
# Camera backend: the platform pick is made below. Exclude both
# candidates from the generic SRC wildcard so only the one we want
# gets compiled per platform.
SRC := $(filter-out $(SRC_DIR)/cam/cam_stub.c,$(SRC))
TEST := $(wildcard $(TEST_DIR)/*.c)
CMD := $(wildcard $(CMD_DIR)/*.c)
TUI := $(wildcard $(TUI_DIR)/*.c)
OBJ_SRC := $(SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/$(SRC_DIR)/%.o)
OBJ_TEST := $(TEST:$(TEST_DIR)/%.c=$(OBJ_DIR)/$(TEST_DIR)/%.o)
OBJ_CMD := $(CMD:$(CMD_DIR)/%.c=$(OBJ_DIR)/$(CMD_DIR)/%.o)
TUI_OBJ_SRC := $(SRC:$(SRC_DIR)/%.c=$(TUI_OBJ_DIR)/$(SRC_DIR)/%.o)
TUI_OBJ_TUI := $(TUI:$(TUI_DIR)/%.c=$(TUI_OBJ_DIR)/$(TUI_DIR)/%.o)

# Platform-specific camera backend object + link flags. macOS uses
# AVFoundation; everything else gets a no-op stub. The TUI link adds
# CAM_OBJ to its inputs and CAM_LDLIBS to its link line.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    CAM_OBJ := $(TUI_OBJ_DIR)/$(SRC_DIR)/cam/cam_macos.o
    # -lc++ is required: cam_macos.mm is Objective-C++ and LTO pulls
    # in __gxx_personality_v0, which lives in the C++ runtime.
    CAM_LDLIBS := -lc++ -framework AVFoundation -framework CoreMedia \
                  -framework CoreVideo -framework Foundation
    CAM_DEFINE := -DMAGPIE_HAVE_CAMERA
else
    CAM_OBJ := $(TUI_OBJ_DIR)/$(SRC_DIR)/cam/cam_stub.o
    CAM_LDLIBS :=
    CAM_DEFINE :=
endif

SRC_SUBDIRS := $(shell find $(SRC_DIR) -type d)
SRC_OBJ_SUBDIRS := $(patsubst $(SRC_DIR)/%,$(OBJ_DIR)/$(SRC_DIR)/%,$(SRC_SUBDIRS))
# Drop the bare $(SRC_DIR) entry that find emits; SRC_OBJ_SUBDIRS already
# carries it (untransformed) so its mkdir rule fires once. Listing it twice
# in the rule below trips a make warning.
TUI_SRC_OBJ_SUBDIRS := $(filter-out $(SRC_DIR),$(patsubst $(SRC_DIR)/%,$(TUI_OBJ_DIR)/$(SRC_DIR)/%,$(SRC_SUBDIRS)))

TEST_SUBDIRS := $(shell find $(TEST_DIR) -type d)
TEST_OBJ_SUBDIRS := $(patsubst $(TEST_DIR)/%,$(OBJ_DIR)/$(TEST_DIR)/%,$(TEST_SUBDIRS))

#dev is default, for another flavor : make BUILD=release
BUILD ?= dev

ifeq ($(BUILD),thread)
    FSAN_ARG := -fsanitize=thread
else
    FSAN_ARG := -fsanitize=address,undefined,pointer-compare,pointer-subtract
    # Test whether the leak flag is supported by the compiler
    ifeq ($(shell echo "int main() { return 0; }" | $(CC) -x c - -fsanitize=leak -o /dev/null >/dev/null 2>&1; echo $$?),0)
        FSAN_ARG += -fsanitize=leak
    endif
endif

cflags.dev := -g -O0 -Wall -Wno-trigraphs -Wextra -Wshadow -Wstrict-prototypes -Werror $(FSAN_ARG)
cflags.thread := -g -O0 -Wall -Wno-trigraphs -Wextra -Wshadow -Wstrict-prototypes -Werror -fsanitize=thread
cflags.vlg := -g -O0 -Wall -Wno-trigraphs -Wextra
cflags.cov := -g -O0 -Wall -Wno-trigraphs -Wextra --coverage
cflags.release := -O3 -flto -march=native -DNDEBUG -Wall -Wno-trigraphs
# Test-specific flags: like release but without DNDEBUG (asserts always enabled in tests)
cflags.test_release := -O3 -flto -march=native -Wall -Wno-trigraphs
cflags.profile := -O3 -g -march=native -DNDEBUG -Wall -Wno-trigraphs -fno-omit-frame-pointer -mllvm -inline-threshold=0
lflags.cov := --coverage

ldflags.dev := -pthread $(FSAN_ARG)
ldflags.thread := -pthread -fsanitize=thread
ldflags.vlg := -pthread
ldflags.release := -pthread
ldflags.profile := -pthread
ldflags.cov := -pthread

CFLAGS := ${cflags.${BUILD}}

ifndef BOARD_DIM
BOARD_DIM = 15
endif

ifndef RACK_SIZE
RACK_SIZE = 7
endif

CFLAGS += -DBOARD_DIM=$(BOARD_DIM) -DRACK_SIZE=$(RACK_SIZE)


LFLAGS := ${lflags.${BUILD}}
LDFLAGS  := ${ldflags.${BUILD}}
LDLIBS   := -lm

# notcurses detection for the optional magpie_tui target. Prefer pkg-config;
# fall back to a Homebrew prefix probe on macOS. magpie_tui is opt-in and is
# not part of `all`, so a missing notcurses does not break the default build.
NOTCURSES_HAS_PKGCONFIG := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --exists notcurses-core 2>/dev/null && echo yes)
ifeq ($(NOTCURSES_HAS_PKGCONFIG),yes)
    NOTCURSES_CFLAGS := $(shell pkg-config --cflags notcurses-core)
    NOTCURSES_LDLIBS := $(shell pkg-config --libs notcurses-core)
else
    NOTCURSES_BREW_PREFIX := $(shell command -v brew >/dev/null 2>&1 && brew --prefix notcurses 2>/dev/null)
    ifneq ($(wildcard $(NOTCURSES_BREW_PREFIX)/include/notcurses/notcurses.h),)
        NOTCURSES_CFLAGS := -I$(NOTCURSES_BREW_PREFIX)/include
        NOTCURSES_LDLIBS := -L$(NOTCURSES_BREW_PREFIX)/lib -lnotcurses-core
    endif
endif

# FreeType is required for magpie_tui: the 2x board-tile mode rasterizes
# TTF glyphs through it, and the renderer links the library unconditionally
# so the same binary works at both 1x and 2x. Same detection pattern as
# notcurses — pkg-config preferred, Homebrew prefix as macOS fallback.
FREETYPE_HAS_PKGCONFIG := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --exists freetype2 2>/dev/null && echo yes)
ifeq ($(FREETYPE_HAS_PKGCONFIG),yes)
    FREETYPE_CFLAGS := $(shell pkg-config --cflags freetype2)
    FREETYPE_LDLIBS := $(shell pkg-config --libs freetype2)
else
    FREETYPE_BREW_PREFIX := $(shell command -v brew >/dev/null 2>&1 && brew --prefix freetype 2>/dev/null)
    ifneq ($(wildcard $(FREETYPE_BREW_PREFIX)/include/freetype2/ft2build.h),)
        FREETYPE_CFLAGS := -I$(FREETYPE_BREW_PREFIX)/include/freetype2
        FREETYPE_LDLIBS := -L$(FREETYPE_BREW_PREFIX)/lib -lfreetype
    endif
endif

# magpie_tui builds engine and tui sources into a separate object tree
# without sanitizers. AddressSanitizer's munmap interception conflicts with
# notcurses/terminfo cleanup on macOS Darwin >= 25 and aborts on exit. The
# default obj/ tree (with sanitizers under BUILD=dev) is unaffected, and
# magpie_test still exercises the engine with ASAN.
#
# Engine sources use the project's release-style flags (no -Werror) since
# they are already linted under BUILD=dev for magpie/magpie_test. TUI
# sources use stricter flags so new code stays in project style.
# Use release-grade optimization for both engine and TUI sources. The
# 2x pixel composite + per-cell fill loops are tight per-pixel work
# that vectorizes well at -O3 -march=native, and the engine's move gen
# is the same code path the release magpie binary uses. -flto lets the
# linker inline across translation units (notably the engine's small
# accessor helpers called from the renderer).
# magpie_tui has its own build flags independent of the top-level BUILD
# variable used for `magpie`. Default is release-grade (-O3 -flto
# -march=native) for the 60fps-on-pixel-blits hot path. Pass
# BUILD=debug to swap in -O1 + AddressSanitizer + UBSan for crash
# troubleshooting; that build is slower, larger, and on macOS aborts on
# exit due to ASAN's munmap interception conflicting with notcurses/
# terminfo cleanup, but it prints a stack trace on SIGSEGV which is
# what makes it worth the noise.
ifeq ($(BUILD),debug)
    TUI_OPT := -O1
    TUI_LTO :=
    TUI_SAN := -fsanitize=address,undefined -fno-omit-frame-pointer
else
    TUI_OPT := -O3
    TUI_LTO := -flto
    TUI_SAN :=
endif

TUI_ENGINE_CFLAGS := $(TUI_OPT) $(TUI_LTO) -march=native -g -DNDEBUG -Wall -Wno-trigraphs -DBOARD_DIM=$(BOARD_DIM) -DRACK_SIZE=$(RACK_SIZE) $(TUI_SAN) -MMD -MP
TUI_CFLAGS := $(TUI_OPT) $(TUI_LTO) -march=native -g -DNDEBUG -Wall -Wno-trigraphs -Wextra -Wshadow -Wstrict-prototypes -Werror -DBOARD_DIM=$(BOARD_DIM) -DRACK_SIZE=$(RACK_SIZE) $(TUI_SAN) -MMD -MP
TUI_LDFLAGS := -pthread $(TUI_LTO) $(TUI_SAN)

.PHONY: all clean iwyu

all: magpie magpie_test

libmagpie_core.a: $(OBJ_SRC)
	ar rcs $@ $^

magpie: $(OBJ_SRC) $(OBJ_CMD) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/$@

magpie_test: $(OBJ_SRC) $(OBJ_TEST) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/$@

magpie_tui: $(TUI_OBJ_SRC) $(TUI_OBJ_TUI) $(CAM_OBJ) | $(BIN_DIR)
ifeq ($(strip $(NOTCURSES_CFLAGS)),)
	@echo "magpie_tui: notcurses not found. Install via 'brew install notcurses' (macOS) or 'apt install libnotcurses-dev' (Debian/Ubuntu)."
	@exit 1
else ifeq ($(strip $(FREETYPE_CFLAGS)),)
	@echo "magpie_tui: freetype not found. Install via 'brew install freetype' (macOS) or 'apt install libfreetype-dev' (Debian/Ubuntu)."
	@exit 1
else
	$(CC) $(TUI_LDFLAGS) $^ $(LDLIBS) $(NOTCURSES_LDLIBS) $(FREETYPE_LDLIBS) $(CAM_LDLIBS) -o $(BIN_DIR)/$@
endif

$(OBJ_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(SRC_DIR) $(SRC_OBJ_SUBDIRS)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/$(CMD_DIR)/%.o: $(CMD_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(CMD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TUI_OBJ_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c | $(TUI_OBJ_DIR) $(TUI_OBJ_DIR)/$(SRC_DIR) $(TUI_SRC_OBJ_SUBDIRS)
	$(CC) $(TUI_ENGINE_CFLAGS) -c $< -o $@

$(TUI_OBJ_DIR)/$(TUI_DIR)/%.o: $(TUI_DIR)/%.c | $(TUI_OBJ_DIR) $(TUI_OBJ_DIR)/$(TUI_DIR)
	$(CC) $(TUI_CFLAGS) $(NOTCURSES_CFLAGS) $(FREETYPE_CFLAGS) $(CAM_DEFINE) -c $< -o $@

# Objective-C++ rule for cam_macos.mm. Compiled only on Darwin, and
# only when CAM_OBJ refers to it; the recipe is unconditional but
# the dependency keeps it from running on other platforms.
$(TUI_OBJ_DIR)/$(SRC_DIR)/cam/cam_macos.o: $(SRC_DIR)/cam/cam_macos.mm \
                                          | $(TUI_OBJ_DIR) $(TUI_OBJ_DIR)/$(SRC_DIR) $(TUI_SRC_OBJ_SUBDIRS)
	clang -fobjc-arc -ObjC++ $(TUI_OPT) $(TUI_LTO) -g -DNDEBUG -Wall \
	      -Wno-trigraphs -DBOARD_DIM=$(BOARD_DIM) -DRACK_SIZE=$(RACK_SIZE) \
	      $(CAM_DEFINE) -MMD -MP -c $< -o $@

# Test files: use test_release flags if BUILD=release, otherwise use dev flags
$(OBJ_DIR)/$(TEST_DIR)/%.o: $(TEST_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(TEST_DIR) $(TEST_OBJ_SUBDIRS)
	$(CC) $(if $(filter release,$(BUILD)),${cflags.test_release},$(CFLAGS)) -DBOARD_DIM=$(BOARD_DIM) -DRACK_SIZE=$(RACK_SIZE) -c $< -o $@

$(BIN_DIR) $(OBJ_DIR) $(OBJ_DIR)/$(SRC_DIR) $(OBJ_DIR)/$(CMD_DIR) $(OBJ_DIR)/$(TEST_DIR) $(TUI_OBJ_DIR) $(TUI_OBJ_DIR)/$(SRC_DIR) $(TUI_OBJ_DIR)/$(TUI_DIR) $(SRC_OBJ_SUBDIRS) $(TUI_SRC_OBJ_SUBDIRS) $(TEST_OBJ_SUBDIRS):
	mkdir -p $@

clean:
	@$(RM) -rv $(BIN_DIR) $(OBJ_DIR) $(TUI_OBJ_DIR) libmagpie_core.a

-include $(OBJ_SRC:.o=.d)
-include $(OBJ_CMD:.o=.d)
-include $(OBJ_TEST:.o=.d)
-include $(TUI_OBJ_SRC:.o=.d)
-include $(TUI_OBJ_TUI:.o=.d)
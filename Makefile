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
BIN_DIR := bin
COV_DIR := cov

SRC  := $(wildcard $(SRC_DIR)/**/*.c)
TEST := $(wildcard $(TEST_DIR)/*.c)
CMD := $(wildcard $(CMD_DIR)/*.c)
TUI := $(wildcard $(TUI_DIR)/*.c)
OBJ_SRC := $(SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/$(SRC_DIR)/%.o)
OBJ_TEST := $(TEST:$(TEST_DIR)/%.c=$(OBJ_DIR)/$(TEST_DIR)/%.o)
OBJ_CMD := $(CMD:$(CMD_DIR)/%.c=$(OBJ_DIR)/$(CMD_DIR)/%.o)
OBJ_TUI := $(TUI:$(TUI_DIR)/%.c=$(OBJ_DIR)/$(TUI_DIR)/%.o)

SRC_SUBDIRS := $(shell find $(SRC_DIR) -type d)
SRC_OBJ_SUBDIRS := $(patsubst $(SRC_DIR)/%,$(OBJ_DIR)/$(SRC_DIR)/%,$(SRC_SUBDIRS))

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

.PHONY: all clean iwyu

all: magpie magpie_test

libmagpie_core.a: $(OBJ_SRC)
	ar rcs $@ $^

magpie: $(OBJ_SRC) $(OBJ_CMD) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/$@

magpie_test: $(OBJ_SRC) $(OBJ_TEST) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/$@

magpie_tui: $(OBJ_SRC) $(OBJ_TUI) | $(BIN_DIR)
ifeq ($(strip $(NOTCURSES_CFLAGS)),)
	@echo "magpie_tui: notcurses not found. Install via 'brew install notcurses' (macOS) or 'apt install libnotcurses-dev' (Debian/Ubuntu)."
	@exit 1
else
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) $(NOTCURSES_LDLIBS) -o $(BIN_DIR)/$@
endif

$(OBJ_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(SRC_DIR) $(SRC_OBJ_SUBDIRS)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/$(CMD_DIR)/%.o: $(CMD_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(CMD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/$(TUI_DIR)/%.o: $(TUI_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(TUI_DIR)
	$(CC) $(CFLAGS) $(NOTCURSES_CFLAGS) -c $< -o $@

# Test files: use test_release flags if BUILD=release, otherwise use dev flags
$(OBJ_DIR)/$(TEST_DIR)/%.o: $(TEST_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(TEST_DIR) $(TEST_OBJ_SUBDIRS)
	$(CC) $(if $(filter release,$(BUILD)),${cflags.test_release},$(CFLAGS)) -DBOARD_DIM=$(BOARD_DIM) -DRACK_SIZE=$(RACK_SIZE) -c $< -o $@

$(BIN_DIR) $(OBJ_DIR) $(OBJ_DIR)/$(SRC_DIR) $(OBJ_DIR)/$(CMD_DIR) $(OBJ_DIR)/$(TEST_DIR) $(OBJ_DIR)/$(TUI_DIR) $(SRC_OBJ_SUBDIRS) $(TEST_OBJ_SUBDIRS):
	mkdir -p $@

clean:
	@$(RM) -rv $(BIN_DIR) $(OBJ_DIR) libmagpie_core.a

-include $(OBJ_SRC:.o=.d)
-include $(OBJ_CMD:.o=.d)
-include $(OBJ_TEST:.o=.d)
-include $(OBJ_TUI:.o=.d)
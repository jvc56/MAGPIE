ifeq ($(origin NPROCS), undefined)
NPROCS := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
endif
ifeq ($(filter -j% --jobs%,$(MAKEFLAGS)),)
MAKEFLAGS += -j$(NPROCS)
endif

SRC_DIR := src
TEST_DIR := test
CMD_DIR := cmd
BIN_DIR := bin
COV_DIR := cov

# Build knobs that change the compiled objects. Defined up here (before OBJ_DIR)
# so object files can be keyed by them.
#dev is default, for another flavor : make BUILD=release
BUILD ?= dev
ifndef BOARD_DIM
BOARD_DIM = 15
endif
ifndef RACK_SIZE
RACK_SIZE = 7
endif

# Key every object (and its .d fragment) by the flags that change its contents:
# build flavor, board dim, rack size. Switching any of them selects a different
# obj subtree instead of relinking objects compiled with mismatched flags -- so
# no `make clean` is needed between flavors, and switching back reuses the cached
# objects. `clean` wipes the whole OBJ_ROOT.
OBJ_ROOT := obj
OBJ_DIR := $(OBJ_ROOT)/$(BUILD)-b$(BOARD_DIM)-r$(RACK_SIZE)

# Profile-guided optimization data. The default lives below OBJ_ROOT so it is
# ignored by git and removed by `make clean` with the other build artifacts.
PGO_DIR ?= $(OBJ_ROOT)/pgo-data
PGO_RAW_DIR ?= $(PGO_DIR)/raw
PGO_PROFILE ?= $(abspath $(PGO_DIR)/magpie.profdata)
# setup.sh does not generate RIT files, so the source-build default must work
# with the standard downloaded data. Override this when training with an RIT.
PGO_TRAIN_ENV ?= SIMBENCH_RIT=false
PGO_TRAIN_TEST ?= simbench
PGO_CC ?= clang
PGO_LDFLAGS ?=
LLVM_PROFDATA ?= $(shell command -v llvm-profdata 2>/dev/null)
ifeq ($(LLVM_PROFDATA),)
LLVM_PROFDATA := xcrun llvm-profdata
endif

SRC  := $(wildcard $(SRC_DIR)/**/*.c)
TEST := $(wildcard $(TEST_DIR)/*.c)
CMD := $(wildcard $(CMD_DIR)/*.c)
OBJ_SRC := $(SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/$(SRC_DIR)/%.o)
OBJ_TEST := $(TEST:$(TEST_DIR)/%.c=$(OBJ_DIR)/$(TEST_DIR)/%.o)
OBJ_CMD := $(CMD:$(CMD_DIR)/%.c=$(OBJ_DIR)/$(CMD_DIR)/%.o)

SRC_SUBDIRS := $(shell find $(SRC_DIR) -type d)
SRC_OBJ_SUBDIRS := $(patsubst $(SRC_DIR)/%,$(OBJ_DIR)/$(SRC_DIR)/%,$(SRC_SUBDIRS))

TEST_SUBDIRS := $(shell find $(TEST_DIR) -type d)
TEST_OBJ_SUBDIRS := $(patsubst $(TEST_DIR)/%,$(OBJ_DIR)/$(TEST_DIR)/%,$(TEST_SUBDIRS))

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
cflags.pgo_generate := $(cflags.release) -fprofile-instr-generate -fprofile-update=atomic
cflags.test_pgo_generate := $(cflags.test_release) -fprofile-instr-generate -fprofile-update=atomic
pgo_use_flags := -fprofile-instr-use=$(PGO_PROFILE) \
                 -Werror=profile-instr-out-of-date \
                 -Wno-profile-instr-unprofiled
cflags.pgo_use := $(cflags.release) $(pgo_use_flags)
cflags.test_pgo_use := $(cflags.test_release) $(pgo_use_flags)
cflags.profile := -O3 -g -march=native -DNDEBUG -Wall -Wno-trigraphs -fno-omit-frame-pointer -mllvm -inline-threshold=0
lflags.cov := --coverage

ldflags.dev := -pthread $(FSAN_ARG)
ldflags.thread := -pthread -fsanitize=thread
ldflags.vlg := -pthread
ldflags.release := -pthread -flto
ldflags.pgo_generate := -pthread -flto -fprofile-instr-generate $(PGO_LDFLAGS)
ldflags.pgo_use := -pthread -flto -fprofile-instr-use=$(PGO_PROFILE) $(PGO_LDFLAGS)
ldflags.profile := -pthread
ldflags.cov := -pthread

CFLAGS := ${cflags.${BUILD}}

# Emit a .d makefile fragment next to each .o listing the headers it includes
# (-MMD) plus phony targets for those headers (-MP, so deleting a header does
# not break the build). These fragments are -included at the bottom, so editing
# a header recompiles exactly the .c files that include it -- no `make clean`.
DEPFLAGS := -MMD -MP

CFLAGS += -DBOARD_DIM=$(BOARD_DIM) -DRACK_SIZE=$(RACK_SIZE)
ifeq ($(BUILD),pgo_use)
CMD_CFLAGS := $(cflags.release) -DBOARD_DIM=$(BOARD_DIM) -DRACK_SIZE=$(RACK_SIZE)
else
CMD_CFLAGS := $(CFLAGS)
endif


LFLAGS := ${lflags.${BUILD}}
LDFLAGS  := ${ldflags.${BUILD}}
LDLIBS   := -lm

.PHONY: all clean iwyu pgo pgo-instrument pgo-train pgo-merge pgo-build pgo-use

all: magpie magpie_test

libmagpie_core.a: $(OBJ_SRC)
	ar rcs $@ $^

magpie: $(OBJ_SRC) $(OBJ_CMD) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/$@

magpie_test: $(OBJ_SRC) $(OBJ_TEST) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/$@

$(OBJ_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(SRC_DIR) $(SRC_OBJ_SUBDIRS)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# The training executable and CLI executable have different `main` functions.
# Keep the CLI entry point out of profile use so its shared symbol name is not
# mistaken for stale profile data; all shared Magpie code remains profiled.
$(OBJ_DIR)/$(CMD_DIR)/%.o: $(CMD_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(CMD_DIR)
	$(CC) $(CMD_CFLAGS) $(DEPFLAGS) -c $< -o $@

# Compiler and linker flag changes in this file invalidate every object.
$(OBJ_SRC) $(OBJ_CMD) $(OBJ_TEST): Makefile

# A newly merged profile must rebuild every profile-use object. Without this
# dependency, make would reuse objects optimized against an older corpus.
ifeq ($(BUILD),pgo_use)
$(OBJ_SRC) $(OBJ_CMD) $(OBJ_TEST): $(PGO_PROFILE)
endif

# Optimized test builds keep assertions enabled.
$(OBJ_DIR)/$(TEST_DIR)/%.o: $(TEST_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(TEST_DIR) $(TEST_OBJ_SUBDIRS)
	$(CC) $(if $(filter release pgo_generate pgo_use,$(BUILD)),${cflags.test_$(BUILD)},$(CFLAGS)) $(DEPFLAGS) -DBOARD_DIM=$(BOARD_DIM) -DRACK_SIZE=$(RACK_SIZE) -c $< -o $@

$(BIN_DIR) $(OBJ_DIR) $(OBJ_DIR)/$(SRC_DIR) $(OBJ_DIR)/$(CMD_DIR) $(OBJ_DIR)/$(TEST_DIR) $(SRC_OBJ_SUBDIRS) $(TEST_OBJ_SUBDIRS):
	mkdir -p $@

clean:
	@$(RM) -rv $(BIN_DIR) $(OBJ_ROOT) libmagpie_core.a

# `make pgo` performs the complete instrument, train, merge, and optimized
# build sequence. For a custom training corpus, run `make pgo-instrument`, run
# one or more workloads with LLVM_PROFILE_FILE set to
# "$(abspath $(PGO_RAW_DIR))/magpie-%p.profraw", then run `make pgo-build`.
# To consume an already merged profile (for example, one downloaded from
# GitHub Actions), run `make pgo-use PGO_PROFILE=/path/to/magpie.profdata`.
pgo:
	$(MAKE) pgo-train
	$(MAKE) pgo-build

pgo-instrument:
	$(RM) -r $(PGO_RAW_DIR) $(PGO_PROFILE)
	mkdir -p $(PGO_RAW_DIR)
	$(MAKE) magpie_test BUILD=pgo_generate PGO_PROFILE=$(PGO_PROFILE) CC="$(PGO_CC)"
	@echo 'Write training profiles to $(abspath $(PGO_RAW_DIR))/magpie-%p.profraw'

pgo-train: pgo-instrument
	LLVM_PROFILE_FILE="$(abspath $(PGO_RAW_DIR))/magpie-%p.profraw" $(PGO_TRAIN_ENV) ./$(BIN_DIR)/magpie_test $(PGO_TRAIN_TEST)

pgo-merge:
	$(LLVM_PROFDATA) merge -sparse -o $(PGO_PROFILE) $(PGO_RAW_DIR)/*.profraw

pgo-build: pgo-merge
	$(MAKE) pgo-use \
		PGO_PROFILE="$(PGO_PROFILE)" \
		PGO_CC="$(PGO_CC)" \
		PGO_LDFLAGS="$(PGO_LDFLAGS)"

# Profile identity is not encoded in OBJ_DIR, so always rebuild when a merged
# profile is explicitly applied.
pgo-use:
	@test -f "$(PGO_PROFILE)" || \
		(echo 'PGO profile not found: $(PGO_PROFILE)' >&2; exit 1)
	$(MAKE) -B all \
		BUILD=pgo_use \
		PGO_PROFILE="$(PGO_PROFILE)" \
		CC="$(PGO_CC)" \
		PGO_LDFLAGS="$(PGO_LDFLAGS)"

-include $(OBJ_SRC:.o=.d)
-include $(OBJ_CMD:.o=.d)
-include $(OBJ_TEST:.o=.d)

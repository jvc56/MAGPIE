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
# Shared library flavor: release optimization plus -fPIC, no sanitizers
cflags.lib := -O3 -flto -march=native -DNDEBUG -Wall -Wno-trigraphs -fPIC
# Test-specific flags: like release but without DNDEBUG (asserts always enabled in tests)
cflags.test_release := -O3 -flto -march=native -Wall -Wno-trigraphs
cflags.profile := -O3 -g -march=native -DNDEBUG -Wall -Wno-trigraphs -fno-omit-frame-pointer -mllvm -inline-threshold=0
lflags.cov := --coverage

ldflags.dev := -pthread $(FSAN_ARG)
ldflags.lib := -pthread
ldflags.thread := -pthread -fsanitize=thread
ldflags.vlg := -pthread
ldflags.release := -pthread
ldflags.profile := -pthread
ldflags.cov := -pthread

CFLAGS := ${cflags.${BUILD}}

# Emit a .d makefile fragment next to each .o listing the headers it includes
# (-MMD) plus phony targets for those headers (-MP, so deleting a header does
# not break the build). These fragments are -included at the bottom, so editing
# a header recompiles exactly the .c files that include it -- no `make clean`.
DEPFLAGS := -MMD -MP

CFLAGS += -DBOARD_DIM=$(BOARD_DIM) -DRACK_SIZE=$(RACK_SIZE)


LFLAGS := ${lflags.${BUILD}}
LDFLAGS  := ${ldflags.${BUILD}}
LDLIBS   := -lm

.PHONY: all clean iwyu libmagpie

all: magpie magpie_test

libmagpie_core.a: $(OBJ_SRC)
	ar rcs $@ $^

# Shared library for embedding magpie; the API surface is src/impl/cmd_api.h
# and only magpie_* symbols are exported. Re-invokes make with BUILD=lib so
# objects are always compiled with -fPIC and without sanitizers.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SHARED_LIB := $(BIN_DIR)/libmagpie.dylib
else
SHARED_LIB := $(BIN_DIR)/libmagpie.so
endif

libmagpie:
	@$(MAKE) BUILD=lib $(SHARED_LIB)

$(OBJ_DIR)/libmagpie.map: | $(OBJ_DIR)
	printf '{ global: magpie_*; local: *; };\n' > $@

$(OBJ_DIR)/libmagpie.exports: | $(OBJ_DIR)
	printf '_magpie_*\n' > $@

$(BIN_DIR)/libmagpie.so: $(OBJ_SRC) $(OBJ_DIR)/libmagpie.map | $(BIN_DIR)
	$(CC) -shared $(LDFLAGS) $(LFLAGS) $(OBJ_SRC) $(LDLIBS) -Wl,--version-script=$(OBJ_DIR)/libmagpie.map -o $@

$(BIN_DIR)/libmagpie.dylib: $(OBJ_SRC) $(OBJ_DIR)/libmagpie.exports | $(BIN_DIR)
	$(CC) -dynamiclib $(LDFLAGS) $(LFLAGS) $(OBJ_SRC) $(LDLIBS) -Wl,-exported_symbols_list,$(OBJ_DIR)/libmagpie.exports -o $@

magpie: $(OBJ_SRC) $(OBJ_CMD) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/$@

magpie_test: $(OBJ_SRC) $(OBJ_TEST) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/$@

$(OBJ_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(SRC_DIR) $(SRC_OBJ_SUBDIRS)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJ_DIR)/$(CMD_DIR)/%.o: $(CMD_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(CMD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# Test files: use test_release flags if BUILD=release, otherwise use dev flags
$(OBJ_DIR)/$(TEST_DIR)/%.o: $(TEST_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(TEST_DIR) $(TEST_OBJ_SUBDIRS)
	$(CC) $(if $(filter release,$(BUILD)),${cflags.test_release},$(CFLAGS)) $(DEPFLAGS) -DBOARD_DIM=$(BOARD_DIM) -DRACK_SIZE=$(RACK_SIZE) -c $< -o $@

$(BIN_DIR) $(OBJ_DIR) $(OBJ_DIR)/$(SRC_DIR) $(OBJ_DIR)/$(CMD_DIR) $(OBJ_DIR)/$(TEST_DIR) $(SRC_OBJ_SUBDIRS) $(TEST_OBJ_SUBDIRS):
	mkdir -p $@

clean:
	@$(RM) -rv $(BIN_DIR) $(OBJ_ROOT) libmagpie_core.a

-include $(OBJ_SRC:.o=.d)
-include $(OBJ_CMD:.o=.d)
-include $(OBJ_TEST:.o=.d)
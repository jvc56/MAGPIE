ifeq ($(origin NPROCS), undefined)
NPROCS := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
endif
ifeq ($(filter -j% --jobs%,$(MAKEFLAGS)),)
MAKEFLAGS += -j$(NPROCS)
endif

SRC_DIR := src
TEST_DIR := test
CMD_DIR := cmd
OBJ_DIR := obj
BIN_DIR := bin
COV_DIR := cov

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
cflags.dll_dev = -g -O0 -fpic -Wall
cflags.dll_release = -O3 -fpic -flto -march=native -Wall -Wno-trigraphs

lflags.cov := --coverage

ldflags.dev := -pthread $(FSAN_ARG)
ldflags.thread := -pthread -fsanitize=thread
ldflags.vlg := -pthread
ldflags.release := -pthread
ldflags.profile := -pthread
ldflags.cov := -pthread
ldflags.dll_dev := -pthread
ldflags.dll_release := -pthread

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

UNAME_S := $(shell uname -s)
METAL_ENABLED := 0
ifeq ($(UNAME_S),Darwin)
  METAL_ENABLED := 1
endif

.PHONY: all clean iwyu

all: magpie magpie_test

magpie_so: $(OBJ_SRC)
	$(CC) -shared $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o libmagpie.so

magpie: $(OBJ_SRC) $(OBJ_CMD) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/$@

ifeq ($(METAL_ENABLED),1)
MAGPIE_TEST_METAL_OBJ := obj/src/metal/movegen_impl.o
MAGPIE_TEST_METAL_LDFLAGS := -framework Metal -framework Foundation
MAGPIE_TEST_METAL_DEPS := bin/movegen.metallib
else
MAGPIE_TEST_METAL_OBJ :=
MAGPIE_TEST_METAL_LDFLAGS :=
MAGPIE_TEST_METAL_DEPS :=
endif

magpie_test: $(OBJ_SRC) $(OBJ_TEST) $(MAGPIE_TEST_METAL_OBJ) $(MAGPIE_TEST_METAL_DEPS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $(OBJ_SRC) $(OBJ_TEST) $(MAGPIE_TEST_METAL_OBJ) $(LDLIBS) $(MAGPIE_TEST_METAL_LDFLAGS) -o $(BIN_DIR)/$@

$(OBJ_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(SRC_DIR) $(SRC_OBJ_SUBDIRS)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/$(CMD_DIR)/%.o: $(CMD_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(CMD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Test files: use test_release flags if BUILD=release, otherwise use dev flags
$(OBJ_DIR)/$(TEST_DIR)/%.o: $(TEST_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(TEST_DIR) $(TEST_OBJ_SUBDIRS)
	$(CC) $(if $(filter release,$(BUILD)),${cflags.test_release},$(CFLAGS)) -DBOARD_DIM=$(BOARD_DIM) -DRACK_SIZE=$(RACK_SIZE) -c $< -o $@

$(BIN_DIR) $(OBJ_DIR) $(OBJ_DIR)/$(SRC_DIR) $(OBJ_DIR)/$(CMD_DIR) $(OBJ_DIR)/$(TEST_DIR) $(SRC_OBJ_SUBDIRS) $(TEST_OBJ_SUBDIRS):
	mkdir -p $@

ifeq ($(METAL_ENABLED),1)
METAL_SRC_DIR := src/metal
METAL_OBJ_DIR := $(OBJ_DIR)/$(METAL_SRC_DIR)
METAL_HOST_FLAGS := -fobjc-arc -O2 -Wall -Wno-trigraphs

$(METAL_OBJ_DIR)/%.o: $(METAL_SRC_DIR)/%.m | $(SRC_OBJ_SUBDIRS)
	clang -ObjC $(METAL_HOST_FLAGS) -c $< -o $@

$(METAL_OBJ_DIR)/%.air: $(METAL_SRC_DIR)/%.metal | $(SRC_OBJ_SUBDIRS)
	xcrun -sdk macosx metal -std=metal3.1 -c $< -o $@

$(BIN_DIR)/%.metallib: $(METAL_OBJ_DIR)/%.air | $(BIN_DIR)
	xcrun -sdk macosx metallib $< -o $@

metal_hello: $(METAL_OBJ_DIR)/hello.o $(BIN_DIR)/hello.metallib | $(BIN_DIR)
	clang -framework Metal -framework Foundation $(METAL_OBJ_DIR)/hello.o -o $(BIN_DIR)/$@

metal_movegen: $(METAL_OBJ_DIR)/movegen.o $(METAL_OBJ_DIR)/movegen_impl.o $(BIN_DIR)/movegen.metallib | $(BIN_DIR)
	clang -framework Metal -framework Foundation $(METAL_OBJ_DIR)/movegen.o $(METAL_OBJ_DIR)/movegen_impl.o -o $(BIN_DIR)/$@
endif

clean:
	@$(RM) -rv $(BIN_DIR) $(OBJ_DIR)

-include $(OBJ_SRC:.o=.d)
-include $(OBJ_CMD:.o=.d)
-include $(OBJ_TEST:.o=.d)
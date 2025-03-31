SRC_DIR := src
TEST_DIR := test
CMD_DIR := cmd
OBJ_DIR := obj
BIN_DIR := bin
COV_DIR := cov
LIB_DIR := lib

SRC  := $(wildcard $(SRC_DIR)/**/*.c)
TEST := $(wildcard $(TEST_DIR)/**/*.c)
CMD := $(wildcard $(CMD_DIR)/*.c)

OBJ_SRC := $(SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/$(SRC_DIR)/%.o)
OBJ_TEST := $(TEST:$(TEST_DIR)/%.c=$(OBJ_DIR)/$(TEST_DIR)/%.o)
OBJ_CMD := $(CMD:$(CMD_DIR)/%.c=$(OBJ_DIR)/$(CMD_DIR)/%.o)

# Explicitly exclude test.c (which has a main)
OBJ_TEST_SRC := $(filter-out $(OBJ_DIR)/$(TEST_DIR)/tsrc/test.o,$(OBJ_TEST))

SRC_SUBDIRS := $(shell find $(SRC_DIR) -type d)
SRC_OBJ_SUBDIRS := $(patsubst $(SRC_DIR)/%,$(OBJ_DIR)/$(SRC_DIR)/%,$(SRC_SUBDIRS))

TEST_SUBDIRS := $(shell find $(TEST_DIR) -type d)
TEST_OBJ_SUBDIRS := $(patsubst $(TEST_DIR)/%,$(OBJ_DIR)/$(TEST_DIR)/%,$(TEST_SUBDIRS))

BUILD := dev

FSAN_ARG := -fsanitize=address,undefined,pointer-compare,pointer-subtract
ifeq ($(shell echo "int main() { return 0; }" | $(CC) -x c - -fsanitize=leak -o /dev/null >/dev/null 2>&1; echo $$?),0)
    FSAN_ARG += -fsanitize=leak
endif

cflags.dev := -g -O0 -Wall -Wno-trigraphs -Wextra -Wshadow -Wstrict-prototypes -Werror $(FSAN_ARG)
cflags.vlg := -g -O0 -Wall -Wno-trigraphs -Wextra
cflags.cov := -g -O0 -Wall -Wno-trigraphs -Wextra --coverage
cflags.release := -O3 -flto -funroll-loops -march=native -Wall -Wno-trigraphs

lflags.cov := --coverage

ldflags.dev := -Llib -pthread $(FSAN_ARG)
ldflags.vlg := -Llib -pthread
ldflags.release := -Llib -pthread
ldflags.cov := -Llib -pthread 

CFLAGS := ${cflags.${BUILD}}
LDFLAGS  := ${ldflags.${BUILD}}
LFLAGS := ${lflags.${BUILD}}
LDLIBS := -lm

CFLAGS += -mmacosx-version-min=14.0
LDFLAGS += -mmacosx-version-min=14.0

ifndef BOARD_DIM
BOARD_DIM = 15
endif

ifndef RACK_SIZE
RACK_SIZE = 7
endif

CFLAGS += -DBOARD_DIM=$(BOARD_DIM) -DRACK_SIZE=$(RACK_SIZE)

.PHONY: all clean iwyu

all: magpie magpie_test libmagpie.a

magpie: $(OBJ_SRC) $(OBJ_CMD) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/$@

magpie_test: $(OBJ_SRC) $(OBJ_TEST) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/$@

libmagpie.a: $(OBJ_SRC) $(OBJ_TEST_SRC) | $(LIB_DIR)
	ar rcs $(LIB_DIR)/$@ $^

$(OBJ_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(SRC_DIR) $(SRC_OBJ_SUBDIRS)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/$(CMD_DIR)/%.o: $(CMD_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(CMD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/$(TEST_DIR)/%.o: $(TEST_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(TEST_DIR) $(TEST_OBJ_SUBDIRS)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR) $(OBJ_DIR) $(LIB_DIR) $(OBJ_DIR)/$(SRC_DIR) $(OBJ_DIR)/$(CMD_DIR) $(OBJ_DIR)/$(TEST_DIR) $(SRC_OBJ_SUBDIRS) $(TEST_OBJ_SUBDIRS):
	mkdir -p $@

clean:
	@$(RM) -rv $(BIN_DIR) $(OBJ_DIR) $(LIB_DIR)

-include $(OBJ_SRC:.o=.d)
-include $(OBJ_CMD:.o=.d)
-include $(OBJ_TEST:.o=.d)

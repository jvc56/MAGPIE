SRC_DIR := src
TEST_DIR := test
CMD_DIR := cmd
OBJ_DIR := obj
BIN_DIR := bin
COV_DIR := cov

SRC := $(wildcard $(SRC_DIR)/*.c)
CMD := $(wildcard $(CMD_DIR)/*.c)
TEST := $(wildcard $(TEST_DIR)/*.c)
OBJ_SRC := $(SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/$(SRC_DIR)/%.o)
OBJ_CMD := $(CMD:$(CMD_DIR)/%.c=$(OBJ_DIR)/$(CMD_DIR)/%.o)
OBJ_TEST := $(TEST:$(TEST_DIR)/%.c=$(OBJ_DIR)/$(TEST_DIR)/%.o)

#dev is default, for another flavor : make BUILD=release
BUILD := dev

FSAN_ARG := -fsanitize=address,leak,undefined,pointer-compare,pointer-subtract

cflags.dev := -g -O0 -Wall -Wno-trigraphs -Wextra $(FSAN_ARG)
cflags.cov := -g -O0 -Wall -Wno-trigraphs -Wextra --coverage
cflags.prof := -g -O3 -Wall -Wno-trigraphs -Wextra -pg
cflags.release := -O3 -Wall -Wno-trigraphs

lflags.cov := --coverage
lflags.prof := -pg -g

ldflags.dev := -Llib -pthread $(FSAN_ARG)
ldflags.release := -Llib -pthread

CPPFLAGS := -Iinclude -MMD -MP
CFLAGS := ${cflags.${BUILD}}
LFLAGS := ${lflags.${BUILD}}
LDFLAGS  := ${ldflags.${BUILD}}
LDLIBS   := -lm

.PHONY: all clean

all: magpie magpie_test

magpie: $(OBJ_SRC) $(OBJ_CMD) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/$@

magpie_test: $(OBJ_SRC) $(OBJ_TEST) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/$@

$(OBJ_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(SRC_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/$(CMD_DIR)/%.o: $(CMD_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(CMD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/$(TEST_DIR)/%.o: $(TEST_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(TEST_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BIN_DIR) $(OBJ_DIR) $(OBJ_DIR)/$(SRC_DIR) $(OBJ_DIR)/$(CMD_DIR) $(OBJ_DIR)/$(TEST_DIR):
	mkdir -p $@

clean:
	@$(RM) -rv $(BIN_DIR) $(OBJ_DIR)

-include $(OBJ_SRC:.o=.d)
-include $(OBJ_CMD:.o=.d)
-include $(OBJ_TEST:.o=.d)

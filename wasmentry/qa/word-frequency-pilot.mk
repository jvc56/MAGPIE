# Run from the repository root:
# make -f wasmentry/qa/word-frequency-pilot.mk word-frequency-pilot BUILD=no_pgo_release
include Makefile
word-frequency-pilot: $(OBJ_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(LFLAGS) wasmentry/qa/word-frequency-pilot.c $(OBJ_SRC) $(LDLIBS) -o $(BIN_DIR)/word-frequency-pilot
.PHONY: word-frequency-pilot

#!/bin/bash

CLANG_TIDY_EXEC="$1"
if [ -z "$CLANG_TIDY_EXEC" ]; then
    echo "No clang-tidy executable specified. Using 'clang-tidy'."
    CLANG_TIDY_EXEC="clang-tidy"
fi

echo "$CLANG_TIDY_EXEC version:"
$CLANG_TIDY_EXEC --version

SEARCH_DIRECTORIES="src/ test/ cmd/"
EXCLUDED_FILES="linenoise.c"
CLANG_TIDY_CHECKS="*,
                  -readability-magic-numbers,
                  -cppcoreguidelines-avoid-magic-numbers,
                  -hicpp-signed-bitwise,
                  -cppcoreguidelines-init-variables,
                  -clang-analyzer-core.uninitialized.Assign,
                  -clang-analyzer-core.uninitialized.UndefReturn,
                  -clang-diagnostic-unknown-escape-sequence,
                  -llvmlibc-restrict-system-libc-headers,
                  -altera-struct-pack-align,
                  -readability-identifier-length,
                  -readability-function-cognitive-complexity,
                  -altera-unroll-loops,
                  -altera-id-dependent-backward-branch,
                  -bugprone-easily-swappable-parameters,
                  -concurrency-mt-unsafe,
                  -bugprone-multi-level-implicit-pointer-conversion,
                  -misc-no-recursion,
                  -llvm-header-guard,
                  -cppcoreguidelines-avoid-non-const-global-variables"
CLANG_TIDY_EXCLUDE_HEADER_FILTER="^(?!.*linenoise\.(c|h)).*"
C_COMPILER_FLAGS="-std=c99 -Wno-trigraphs -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -D__linux__ -U_WIN32 -U__APPLE__ "
LOG_FILE=$(mktemp)
# Ensure the temporary log file is removed when the script exits,
# regardless of how it exits (success, failure, or interruption).
trap 'rm -f "$LOG_FILE"' EXIT
# Set 'pipefail' option: If any command in a pipeline fails, the exit status
# of the pipeline will be the exit status of the last command that failed.
set -o pipefail

echo "Starting clang-tidy static analysis for C files in: $SEARCH_DIRECTORIES"
echo "Enabled checks: $CLANG_TIDY_CHECKS"
echo "---------------------------------------------------"

ANY_CLANG_TIDY_ISSUES=0
CLANG_TIDY_COMMAND_FAILED=0

# Build grep pattern for excluding files from analysis
EXCLUDE_PATTERN=""
for file in $EXCLUDED_FILES; do
    if [ -z "$EXCLUDE_PATTERN" ]; then
        EXCLUDE_PATTERN="$file"
    else
        EXCLUDE_PATTERN="$EXCLUDE_PATTERN\|$file"
    fi
done

# Find all C source files and process them
# 'find $SEARCH_DIRECTORIES -name "*.c"': Finds all files ending with .c
#                                        within the specified directories.
# '-print0': Prints file names separated by a null character,
#            which handles spaces or special characters in filenames correctly.
# 'while IFS= read -r -d $'\0' C_FILE; do ... done': Reads null-separated filenames
#                                                into the C_FILE variable.
find $SEARCH_DIRECTORIES -name "*.c" -print0 | grep -zv "$EXCLUDE_PATTERN" | while IFS= read -r -d $'\0' C_FILE; do
    echo "Analyzing: $C_FILE"

    CLANG_TIDY_CMD="$CLANG_TIDY_EXEC \"$C_FILE\" \
        --header-filter=\"$CLANG_TIDY_EXCLUDE_HEADER_FILTER\" \
        -checks=\"$CLANG_TIDY_CHECKS\" \
        -- $C_COMPILER_FLAGS"

    # Execute clang-tidy for the current file
    # Redirect stderr to stdout (2>&1) and pipe to tee.
    # tee outputs to console and appends to LOG_FILE.
    # The 'if ! eval ...' block captures the exit status of clang-tidy.
    if ! eval "$CLANG_TIDY_CMD 2>&1 | tee -a \"$LOG_FILE\""; then
        CLANG_TIDY_COMMAND_FAILED=1 # Set flag if clang-tidy command failed
        echo "ERROR: clang-tidy command failed for $C_FILE." >&2
    fi
    echo "---------------------------------------------------"
done

# Final check of the collected log file for "warning:" or "error:" strings.
# 'grep -q': Quiet mode; grep prints nothing to standard output,
#            it only sets its exit status (0 if found, 1 if not found).
# 'grep -E': Enables extended regular expressions, allowing '|' for OR.
if grep -q -E "warning:|error:" "$LOG_FILE"; then
    echo "ANALYSIS FAILED: Build output contains 'warning:' or 'error:' from clang-tidy." >&2
    exit 1
elif [ "$CLANG_TIDY_COMMAND_FAILED" -eq 1 ]; then
    echo "ANALYSIS FAILED: One or more clang-tidy commands failed to execute properly." >&2
    exit 1
else
    echo "ANALYSIS SUCCESS: No 'warning:' or 'error:' strings found from clang-tidy."
    exit 0
fi

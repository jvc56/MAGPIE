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
                  -android-cloexec-pipe,
                  -cppcoreguidelines-avoid-non-const-global-variables"
CLANG_TIDY_EXCLUDE_HEADER_FILTER="^(?!.*linenoise\.(c|h)).*"
C_COMPILER_FLAGS="-std=c99 -Wno-trigraphs -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -D__linux__ -U_WIN32 -U__APPLE__ "

MAX_CORES=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)
LOG_FILE=$(mktemp)
RESULT_DIR=$(mktemp -d)
trap 'rm -f "$LOG_FILE"; rm -rf "$RESULT_DIR"' EXIT
set -o pipefail

# Export variables for use in parallel subprocesses
export CLANG_TIDY_EXEC CLANG_TIDY_EXCLUDE_HEADER_FILTER CLANG_TIDY_CHECKS C_COMPILER_FLAGS

echo "Starting clang-tidy static analysis for C files in: $SEARCH_DIRECTORIES"
echo "Using $MAX_CORES parallel workers"
echo "Enabled checks: $CLANG_TIDY_CHECKS"
echo "---------------------------------------------------"

# Build grep pattern for excluding files from analysis
EXCLUDE_PATTERN=""
for file in $EXCLUDED_FILES; do
    if [ -z "$EXCLUDE_PATTERN" ]; then
        EXCLUDE_PATTERN="$file"
    else
        EXCLUDE_PATTERN="$EXCLUDE_PATTERN\|$file"
    fi
done

# Run clang-tidy on each file in parallel.
# Each file's output is written to a separate temp file to avoid interleaving.
# Progress lines ("Analyzing: ...") go to stdout for real-time feedback.
find $SEARCH_DIRECTORIES -name "*.c" -print0 | grep -zv "$EXCLUDE_PATTERN" | \
    xargs -0 -P "$MAX_CORES" -I{} bash -c '
        C_FILE="$1"
        RESULT_DIR="$2"
        SAFE_NAME=$(echo "$C_FILE" | tr "/" "_")
        OUTPUT_FILE="$RESULT_DIR/$SAFE_NAME"
        echo "Analyzing: $C_FILE"
        $CLANG_TIDY_EXEC "$C_FILE" \
            --header-filter="$CLANG_TIDY_EXCLUDE_HEADER_FILTER" \
            -checks="$CLANG_TIDY_CHECKS" \
            -- $C_COMPILER_FLAGS > "$OUTPUT_FILE" 2>&1
        TIDY_EXIT=$?
        if [ $TIDY_EXIT -ne 0 ]; then
            echo "ERROR: clang-tidy command failed for $C_FILE (exit code $TIDY_EXIT)." >> "$OUTPUT_FILE"
        fi
    ' _ {} "$RESULT_DIR"

XARGS_EXIT=$?

echo "---------------------------------------------------"
echo "All files analyzed. Combined output:"
echo "---------------------------------------------------"

# Combine per-file results in sorted order for readable output
for f in $(ls "$RESULT_DIR" 2>/dev/null | sort); do
    cat "$RESULT_DIR/$f"
done | tee "$LOG_FILE"

# Final check of the collected log file for "warning:" or "error:" strings.
if grep -q -E "warning:|error:" "$LOG_FILE"; then
    echo "ANALYSIS FAILED: Build output contains 'warning:' or 'error:' from clang-tidy." >&2
    exit 1
elif [ "$XARGS_EXIT" -ne 0 ]; then
    echo "ANALYSIS FAILED: One or more clang-tidy commands failed to execute properly." >&2
    exit 1
else
    echo "ANALYSIS SUCCESS: No 'warning:' or 'error:' strings found from clang-tidy."
    exit 0
fi

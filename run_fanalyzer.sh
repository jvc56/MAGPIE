#!/bin/bash

# This script executes a 'make' command, displays its output to the console,
# and then checks the output for specific warning or error strings.
# It exits with a non-zero status if warnings/errors are found, or if the
# 'make' command itself fails.

# Define the command to be executed.
# We use 'eval' later to ensure the command string is properly interpreted,
# especially if it contains complex arguments or redirections.
COMMAND="make clean && make magpie_test BUILD=fan"

# Create a temporary file to store the command's output.
# mktemp ensures a unique and secure temporary file name.
LOG_FILE=$(mktemp)

# Ensure the temporary log file is removed when the script exits,
# regardless of how it exits (success, failure, or interruption).
trap 'rm -f "$LOG_FILE"' EXIT

# Set 'pipefail' option: If any command in a pipeline fails, the exit status
# of the pipeline will be the exit status of the last command that failed.
# This is crucial for capturing the actual exit code of the 'make' command
# even when it's piped to 'tee'.
set -o pipefail

echo "Running command: $COMMAND"
echo "---------------------------------------------------"

# Execute the command:
# 1. "$COMMAND 2>&1": Runs the make command and redirects its standard error (2)
#    to its standard output (1). This merges both output streams.
# 2. "| tee \"$LOG_FILE\"": Pipes the combined output to 'tee'.
#    'tee' reads from standard input and writes to both standard output (your terminal)
#    and the specified file ($LOG_FILE). This ensures you see the output live.
#
# The 'if ! eval ...' block captures the exit status of the 'make' command.
# If 'make' itself fails (returns a non-zero exit code), this block will be entered.
# We store make's exit code to potentially use it later.
if ! eval "$COMMAND 2>&1 | tee \"$LOG_FILE\""; then
    MAKE_EXIT_CODE=$? # Capture the exit code of the 'make' command
    echo "---------------------------------------------------" >&2
    echo "The '$COMMAND' command itself failed with exit code $MAKE_EXIT_CODE." >&2
    # If make failed, we will still check for warnings/errors, but prioritize
    # exiting with an error if they are found.
else
    MAKE_EXIT_CODE=0 # make command succeeded
    echo "---------------------------------------------------"
fi

# Check the captured log file for "warning:" or "error:" strings.
# 'grep -q': Quiet mode; grep prints nothing to standard output,
#            it only sets its exit status (0 if found, 1 if not found).
# 'grep -E': Enables extended regular expressions, allowing '|' for OR.
if grep -q -E "warning:|error:" "$LOG_FILE"; then
    echo "ERROR: Build output contains 'warning:' or 'error:'." >&2
    exit 1 # Exit with a generic error code (1) indicating issues
else
    # If no warnings or errors were found, exit with the make command's
    # original exit code. This means if 'make' succeeded, the script succeeds.
    # If 'make' failed for other reasons (e.g., syntax error but no 'warning:'
    # or 'error:' in output), the script will still reflect that failure.
    if [ "$MAKE_EXIT_CODE" -eq 0 ]; then
        echo "SUCCESS: Build completed without 'warning:' or 'error:'."
    else
        echo "WARNING: No 'warning:' or 'error:' strings found, but the '$COMMAND' command still failed." >&2
    fi
    exit "$MAKE_EXIT_CODE"
fi

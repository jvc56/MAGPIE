#!/bin/bash

# --- Configuration ---
# Define the directories to search for source files
SOURCE_DIRS=("src/" "test/" "cmd/")
# Hardcoded C compiler
COMPILER="cc"

echo "Using hardcoded compiler: ${COMPILER}"

# --- Find and analyze C and Header files ---
echo "Searching for .c and .h files in: ${SOURCE_DIRS[@]}"
echo "Running ${COMPILER} -fanalyzer on each found file..."

# Initialize a counter for processed files
PROCESSED_FILES=0

# Loop through each specified source directory
for DIR in "${SOURCE_DIRS[@]}"; do
    # Check if the directory exists
    if [ ! -d "${DIR}" ]; then
        echo "Warning: Directory '${DIR}' not found. Skipping."
        continue
    fi

    # Find all .c files and iterate over them
    find "${DIR}" -name "*.c" -print0 | while IFS= read -r -d $'\0' FILE; do
        echo "Analyzing C file: ${FILE}"
        # Run the compiler with -fanalyzer
        "${COMPILER}" -fanalyzer "${FILE}" || { echo "Warning: Analysis failed for ${FILE}. Continuing..."; }
        PROCESSED_FILES=$((PROCESSED_FILES+1))
    done

    # Find all .h files and iterate over them
    find "${DIR}" -name "*.h" -print0 | while IFS= read -r -d $'\0' FILE; do
        echo "Analyzing Header file: ${FILE}"
        # Run the compiler with -fanalyzer
        "${COMPILER}" -fanalyzer "${FILE}" || { echo "Warning: Analysis failed for ${FILE}. Continuing..."; }
        PROCESSED_FILES=$((PROCESSED_FILES+1))
    done
done

echo "Analysis complete. Processed ${PROCESSED_FILES} files."
echo "Please review the output for any fanalyzer warnings or errors."

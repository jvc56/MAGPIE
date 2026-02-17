#!/bin/bash

# --- Configuration ---
CPPCHECK_VERSION="2.17.1"
CPPCHECK_ZIP_URL="https://github.com/danmar/cppcheck/archive/refs/tags/${CPPCHECK_VERSION}.zip"
CPPCHECK_SOURCE_DIR_UNZIPPED="cppcheck-${CPPCHECK_VERSION}" # Directory created by unzip
CPPCHECK_DIR="cppcheck_dir" # The desired generic directory name for the source
CPPCHECK_BUILD_DIR="cppcheck_build"
LOCAL_CPPCHECK_EXECUTABLE="./${CPPCHECK_DIR}/build/bin/cppcheck" # Path for the compiled executable
ZIP_FILE="${CPPCHECK_VERSION}.zip"

# --- Check for existing locally compiled cppcheck executable ---
CPPCHECK_EXECUTABLE=""

# Always prioritize the locally compiled executable
if [ -f "${LOCAL_CPPCHECK_EXECUTABLE}" ]; then
    echo "cppcheck executable found at ${LOCAL_CPPCHECK_EXECUTABLE}. Skipping download and compile."
    CPPCHECK_EXECUTABLE="${LOCAL_CPPCHECK_EXECUTABLE}"
else
    echo "Locally compiled cppcheck not found. Proceeding with download and compilation..."

    # --- Download and prepare cppcheck source ---
    if [ -d "${CPPCHECK_DIR}" ]; then
        echo "Directory '${CPPCHECK_DIR}' already exists. Assuming source is present."
        echo "If you want to re-download/rebuild, please delete the '${CPPCHECK_DIR}' directory and '${ZIP_FILE}'."
    elif [ -d "${CPPCHECK_SOURCE_DIR_UNZIPPED}" ]; then
        echo "Unzipped directory '${CPPCHECK_SOURCE_DIR_UNZIPPED}' found. Renaming to '${CPPCHECK_DIR}'."
        mv "${CPPCHECK_SOURCE_DIR_UNZIPPED}" "${CPPCHECK_DIR}" || { echo "Error: Failed to rename directory '${CPPCHECK_SOURCE_DIR_UNZIPPED}' to '${CPPCHECK_DIR}'. Exiting."; exit 1; }
    else
        echo "Downloading cppcheck release ${CPPCHECK_VERSION} from ${CPPCHECK_ZIP_URL}..."
        # Use curl for downloading, with -L to follow redirects
        curl -L "${CPPCHECK_ZIP_URL}" -o "${ZIP_FILE}" || { echo "Error: Failed to download cppcheck zip. Ensure 'curl' is installed and network is accessible. Exiting."; exit 1; }

        echo "Unzipping ${ZIP_FILE}..."
        unzip -q "${ZIP_FILE}" -d . || { echo "Error: Failed to unzip cppcheck archive. Ensure 'unzip' is installed. Exiting."; exit 1; }
        rm "${ZIP_FILE}" # Clean up the zip file

        # Rename the unzipped directory to the desired generic name
        mv "${CPPCHECK_SOURCE_DIR_UNZIPPED}" "${CPPCHECK_DIR}" || { echo "Error: Failed to rename directory '${CPPCHECK_SOURCE_DIR_UNZIPPED}' to '${CPPCHECK_DIR}'. Exiting."; exit 1; }
    fi

    # --- Compile cppcheck using CMake ---
    echo "Compiling cppcheck using CMake (this may take a few moments)..."
    # Ensure cmake is installed
    command -v cmake &>/dev/null || { echo "Error: 'cmake' command not found. Please install CMake. Exiting."; exit 1; }

    # Execute CMake build steps in a subshell to manage directory changes
    (
        cd "${CPPCHECK_DIR}" || { echo "Error: Failed to change directory to ${CPPCHECK_DIR}. Exiting."; exit 1; }
        mkdir -p build || { echo "Error: Failed to create build directory inside ${CPPCHECK_DIR}. Exiting."; exit 1; }
        cd build || { echo "Error: Failed to change directory to ${CPPCHECK_DIR}/build. Exiting."; exit 1; }
        cmake -DUSE_MATCHCOMPILER=ON .. || { echo "Error: CMake configuration failed. Please check CMake output for details. Exiting."; exit 1; }
        cmake --build . || { echo "Error: CMake build failed. Please check build output for details. Exiting."; exit 1; }
    ) || { echo "Error during cppcheck compilation process. Exiting."; exit 1; } # Catch errors from the entire subshell block

    echo "cppcheck compiled successfully."
    CPPCHECK_EXECUTABLE="${LOCAL_CPPCHECK_EXECUTABLE}"
fi

# --- Determine max number of available cores for cppcheck analysis ---
MAX_CORES=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)
echo "Using ${MAX_CORES} CPU cores for cppcheck analysis."

mkdir -p "${CPPCHECK_BUILD_DIR}"

# --- Run cppcheck command ---
echo "Running cppcheck analysis on src/, test/, and cmd/ directories..."
"${CPPCHECK_EXECUTABLE}" \
    --check-level=reduced \
    --enable=all \
    --force \
    --verbose \
    --cppcheck-build-dir="${CPPCHECK_BUILD_DIR}" \
    --inline-suppr \
    --suppress=missingIncludeSystem \
    --suppress=constParameterCallback \
    --suppress=unusedFunction \
    --suppress=normalCheckLevelMaxBranches \
    --suppress='*:*/linenoise.c' \
    --suppress='*:*/linenoise.h' \
    --std=c99 \
    --error-exitcode=1 \
    -U_WIN32 \
    -U__APPLE__ \
    -U__EMSCRIPTEN__ \
    -j "${MAX_CORES}" \
    src/ test/ cmd/

# --- Check exit code of cppcheck ---
CPPCHECK_EXIT_CODE=$?
if [ "${CPPCHECK_EXIT_CODE}" -eq 0 ]; then
    echo "cppcheck analysis completed successfully with no errors found."
elif [ "${CPPCHECK_EXIT_CODE}" -eq 1 ]; then
    echo "cppcheck analysis completed with errors (exit code 1). Please review the output."
    exit 1
else
    echo "cppcheck analysis failed with exit code ${CPPCHECK_EXIT_CODE}. Something unexpected happened."
    exit 1
fi

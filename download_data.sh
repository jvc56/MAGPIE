#!/bin/bash
set -e

# Versioned data configuration
DATA_VERSION="20251004"
TESTDATA_VERSION="20251127"

# GitHub repository details
REPO_OWNER="jvc56"
REPO_NAME="MAGPIE-DATA"
BRANCH="main"

# Base URL for raw files
BASE_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}/raw/${BRANCH}/versioned-tarballs"

# Get script directory (where MAGPIE is located)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Function to download and extract a tarball (handles chunking automatically)
download_and_extract() {
    local name=$1
    local version=$2
    local tarball_base="${BASE_URL}/${name}-${version}.tgz"

    echo "Downloading MAGPIE ${name} version ${version}..."

    # Check if tarball is chunked by trying to download the first chunk
    if curl -sfI "${tarball_base}.aa" > /dev/null 2>&1; then
        echo "Downloading chunked ${name} tarball..."
        local temp_dir=$(mktemp -d)
        local chunk_suffix="aa"
        local chunk_num=0

        while curl -sfL "${tarball_base}.${chunk_suffix}" -o "${temp_dir}/${name}.tgz.${chunk_suffix}"; do
            echo "  Downloaded chunk ${chunk_suffix}"
            chunk_num=$((chunk_num + 1))

            # Increment two-letter suffix (aa -> ab -> ac -> ... -> az -> ba -> bb -> ...)
            local second_char=$(echo "$chunk_suffix" | cut -c2)
            if [ "$second_char" = "z" ]; then
                local first_char=$(echo "$chunk_suffix" | cut -c1 | tr 'a-y' 'b-z')
                chunk_suffix="${first_char}a"
            else
                local first_char=$(echo "$chunk_suffix" | cut -c1)
                second_char=$(echo "$second_char" | tr 'a-y' 'b-z')
                chunk_suffix="${first_char}${second_char}"
            fi

            # Safety limit
            if [ $chunk_num -gt 26 ]; then
                echo "Error: Too many chunks, something went wrong"
                rm -rf "$temp_dir"
                exit 1
            fi
        done

        # Reassemble and extract
        cat "${temp_dir}"/${name}.tgz.* | tar -xzf - -C "$SCRIPT_DIR"
        rm -rf "$temp_dir"
    else
        echo "Downloading single ${name} tarball..."
        curl -sfL "${tarball_base}" | tar -xzf - -C "$SCRIPT_DIR"
    fi
}

# Download data and testdata
download_and_extract "data" "$DATA_VERSION"
download_and_extract "testdata" "$TESTDATA_VERSION"

echo "Data download complete!"
echo "  Data version: ${DATA_VERSION}"
echo "  Testdata version: ${TESTDATA_VERSION}"

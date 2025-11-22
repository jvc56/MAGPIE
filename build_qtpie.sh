#!/bin/bash
set -e

# Get the project root directory
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Check for data directory
if [ ! -d "$PROJECT_ROOT/data" ]; then
    echo "Data directory not found. Running download_data.sh..."
    if [ -f "$PROJECT_ROOT/download_data.sh" ]; then
        bash "$PROJECT_ROOT/download_data.sh"
    else
        echo "Error: download_data.sh not found!"
        exit 1
    fi
fi

# Create build directory
BUILD_DIR="$PROJECT_ROOT/build"
if [ ! -d "$BUILD_DIR" ]; then
    echo "Creating build directory..."
    mkdir -p "$BUILD_DIR"
fi

# Navigate to build directory
cd "$BUILD_DIR"

# Configure with CMake
echo "Configuring with CMake..."
cmake ..

# Build
echo "Building qtpie..."
cmake --build .

# Touch the app to force Finder to update the icon
if [ -d "qtpie.app" ]; then
    touch "qtpie.app"
fi

echo "Build complete!"
echo "You can run the application using: open build/qtpie.app"

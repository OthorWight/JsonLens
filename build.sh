#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

BUILD_DIR="build"

echo "Cleaning up previous build directory..."
rm -rf "$BUILD_DIR"

echo "Configuring project with Meson..."
meson setup "$BUILD_DIR"

echo "Building the project..."
meson compile -C "$BUILD_DIR"

echo "Build complete! You can run it with: ./$BUILD_DIR/json-lens"
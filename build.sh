#!/usr/bin/env bash
set -euo pipefail

# Build script for R GUI 2 - Made with Qt
# Usage:
#   ./build.sh           -> Build R GUI 2
#   ./build.sh --clean   -> Clean and rebuild
#   ./build.sh --package -> Build Arch package with makepkg

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RG2SRC="${ROOT_DIR}"
BUILD_DIR="${ROOT_DIR}/build"

# Parse arguments
CLEAN_BUILD=0
BUILD_PACKAGE=0

for arg in "$@"; do
    case "$arg" in
        --clean)
            CLEAN_BUILD=1
            ;;
        --package)
            BUILD_PACKAGE=1
            ;;
        --help)
            echo "R GUI 2 build script"
            echo ""
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --clean      Clean build directory before building"
            echo "  --package    Build Arch/Manjaro package with makepkg"
            echo "  --help       Show this help message"
            exit 0
            ;;
        *)
            echo "Unknown option: $arg"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

echo "=========================================="
echo "Building R GUI 2 - Simple R IDE"
echo "=========================================="
echo ""

if [ ! -d "${RG2SRC}" ]; then
    echo "Error: ${RG2SRC} not found." >&2
    exit 2
fi

# Check for required tools
echo "Checking dependencies..."

if ! command -v cmake &> /dev/null; then
    echo "ERROR: cmake not found. Please install cmake."
    exit 1
fi

# Locate qmake for Qt6
if command -v qmake-qt6 >/dev/null 2>&1; then
    QMAKE_EXECUTABLE="$(command -v qmake-qt6)"
elif [ -x /usr/lib/qt6/bin/qmake ]; then
    QMAKE_EXECUTABLE=/usr/lib/qt6/bin/qmake
else
    QMAKE_EXECUTABLE="$(command -v qmake || echo /usr/bin/qmake)"
fi

echo "Using qmake: ${QMAKE_EXECUTABLE}"

if ! command -v R &> /dev/null; then
    echo "ERROR: R not found. R is required to build the helper package." >&2
    exit 1
fi

echo "Installing jsonlite..."
R -e "if (!require('jsonlite', quietly=TRUE)) install.packages('jsonlite', repos='https://cloud.r-project.org')"

echo "Building rgui R package..."
rm -f "${RG2SRC}"/rgui2_*.tar.gz
R CMD build "${RG2SRC}/rgui2pkg"

R_PACKAGE_TARBALL="$(find "${RG2SRC}" -maxdepth 1 -type f \
    -name 'rgui2_*.tar.gz' -printf '%f\n' | sort -V | tail -n 1)"
if [ -z "${R_PACKAGE_TARBALL}" ]; then
    echo "ERROR: R CMD build did not create an rgui2 package archive." >&2
    exit 1
fi

echo "Installing ${R_PACKAGE_TARBALL}..."
R CMD INSTALL "${RG2SRC}/${R_PACKAGE_TARBALL}"

echo "All dependencies found!"

# Clean if requested
if [ "${CLEAN_BUILD}" = "1" ]; then
    echo ""
    echo "Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
fi

# Create build directory
echo ""
echo "Creating build directory..."
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Configure with CMake
echo ""
echo "Configuring with CMake..."
cmake "${RG2SRC}" \
    -DCMAKE_BUILD_TYPE=Release

# Build
echo ""
echo "Building..."
cmake --build . -j$(nproc)

# Success message
echo ""
echo "=========================================="
echo "Build completed successfully!"
echo "=========================================="
echo ""
echo "Executable: ${BUILD_DIR}/bin/rgui2"
echo ""
echo "To run the application:"
echo "  ${BUILD_DIR}/bin/rgui2"
echo ""

# Build package if requested
if [ "${BUILD_PACKAGE}" = "1" ]; then
    echo ""
    echo "=========================================="
    echo "Building Arch package..."
    echo "=========================================="
    cd "${ROOT_DIR}"
    makepkg -f
    echo ""
    echo "Package built! Install with:"
    echo "  sudo pacman -U r-gui-2-*.pkg.tar.zst"
    echo ""
fi

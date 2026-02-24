#!/bin/bash
# ==============================================================================
#   GPU-PCIe-Test v3.0 (Vulkan - Linux) - Build Script
# ==============================================================================
# Prerequisites:
#   sudo apt install cmake g++ libvulkan-dev libglfw3-dev    # Ubuntu/Debian
#   sudo dnf install cmake gcc-c++ vulkan-loader-devel glfw-devel   # Fedora
#   sudo pacman -S cmake vulkan-headers glfw-wayland         # Arch (Wayland)
#   sudo pacman -S cmake vulkan-headers glfw-x11             # Arch (X11)
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

echo "=================================================="
echo "  GPU-PCIe-Test v3.0 (Vulkan - Linux) Build"
echo "=================================================="
echo

# Check dependencies
echo "[1/3] Checking dependencies..."

if ! command -v cmake &> /dev/null; then
    echo "[ERROR] cmake not found. Install with:"
    echo "  Ubuntu/Debian: sudo apt install cmake"
    echo "  Fedora:        sudo dnf install cmake"
    echo "  Arch:          sudo pacman -S cmake"
    exit 1
fi

if ! pkg-config --exists vulkan 2>/dev/null && ! test -f /usr/include/vulkan/vulkan.h; then
    echo "[ERROR] Vulkan SDK not found. Install with:"
    echo "  Ubuntu/Debian: sudo apt install libvulkan-dev vulkan-tools"
    echo "  Fedora:        sudo dnf install vulkan-loader-devel vulkan-tools"
    echo "  Arch:          sudo pacman -S vulkan-headers vulkan-tools"
    exit 1
fi

if ! pkg-config --exists glfw3 2>/dev/null; then
    echo "[ERROR] GLFW3 not found. Install with:"
    echo "  Ubuntu/Debian: sudo apt install libglfw3-dev"
    echo "  Fedora:        sudo dnf install glfw-devel"
    echo "  Arch:          sudo pacman -S glfw-wayland  (or glfw-x11)"
    exit 1
fi

echo "     [OK] All dependencies found"

# Configure
echo "[2/3] Configuring with CMake..."

BUILD_TYPE="${1:-Release}"
EXTRA_CMAKE_ARGS="${@:2}"

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake "${SCRIPT_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    ${EXTRA_CMAKE_ARGS}

# Build
echo "[3/3] Building..."

cmake --build . --parallel "$(nproc)"

echo
echo "=================================================="
echo "  BUILD SUCCESSFUL!"
echo "=================================================="
echo
echo "Output: ${BUILD_DIR}/gpu-pcie-test-vulkan"
echo
echo "Run with:"
echo "  ${BUILD_DIR}/gpu-pcie-test-vulkan"
echo
echo "For detailed RAM info (speed, type, channels):"
echo "  sudo ${BUILD_DIR}/gpu-pcie-test-vulkan"
echo
echo "To enable Vulkan validation layers (debug build):"
echo "  $0 Debug -DENABLE_VULKAN_VALIDATION=ON"
echo

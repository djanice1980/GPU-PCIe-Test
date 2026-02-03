#!/bin/bash
# ============================================================================
# Build script for PCIe Bandwidth Test
# Requires NVIDIA CUDA Toolkit to be installed
# ============================================================================

echo "Building PCIe Bandwidth Test..."

# Check for nvcc
if ! command -v nvcc &> /dev/null; then
    echo "ERROR: nvcc not found in PATH"
    echo "Please install CUDA Toolkit and ensure nvcc is in your PATH"
    echo "Download from: https://developer.nvidia.com/cuda-downloads"
    exit 1
fi

# Build with optimization
nvcc -O3 pcie_bandwidth_test.cu -o pcie_bandwidth_test

if [ $? -eq 0 ]; then
    echo ""
    echo "Build successful! Run with: ./pcie_bandwidth_test"
    echo ""
    echo "Options:"
    echo "  -s SIZE_MB    Buffer size in MB (default: 256)"
    echo "  -i ITERATIONS Number of iterations (default: 10)"
    echo ""
    echo "Example: ./pcie_bandwidth_test -s 512 -i 20"
    chmod +x pcie_bandwidth_test
else
    echo ""
    echo "Build failed! Check CUDA installation."
fi

@echo off
REM ============================================================================
REM Build script for PCIe Bandwidth Test
REM Requires NVIDIA CUDA Toolkit to be installed
REM ============================================================================

echo Building PCIe Bandwidth Test...

REM Try to find nvcc
where nvcc >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo ERROR: nvcc not found in PATH
    echo Please install CUDA Toolkit and ensure nvcc is in your PATH
    echo Download from: https://developer.nvidia.com/cuda-downloads
    pause
    exit /b 1
)

REM Build with optimization
nvcc -O3 pcie_bandwidth_test.cu -o pcie_bandwidth_test.exe

if %ERRORLEVEL% equ 0 (
    echo.
    echo Build successful! Run with: pcie_bandwidth_test.exe
    echo.
    echo Options:
    echo   -s SIZE_MB    Buffer size in MB (default: 256)
    echo   -i ITERATIONS Number of iterations (default: 10)
    echo.
    echo Example: pcie_bandwidth_test.exe -s 512 -i 20
) else (
    echo.
    echo Build failed! Check CUDA installation.
)

pause

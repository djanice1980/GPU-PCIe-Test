@echo off
echo ==================================================
echo   GPU-PCIe-Test - Complete Build
echo ==================================================
echo.

REM Check compiler
where cl >nul 2>nul
if %errorlevel% neq 0 (
    echo ERROR: Run from Developer Command Prompt for VS
    pause
    exit /b 1
)

echo [1/3] Building D3D12 benchmark...
cl /EHsc /std:c++17 /O2 /MD /DNDEBUG /nologo main_improved.cpp d3d12.lib dxgi.lib /Fe:GPU-PCIe-Test_D3D12.exe
if %errorlevel% neq 0 (
    echo FAILED!
    pause
    exit /b 1
)
echo      [OK] GPU-PCIe-Test_D3D12.exe

echo.
echo [2/3] Building Vulkan benchmark...
cl /EHsc /std:c++17 /O2 /MD /DNDEBUG /nologo /I"%VULKAN_SDK%\Include" main_vulkan.cpp /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib /out:GPU-PCIe-Test_Vulkan.exe
if %errorlevel% neq 0 (
    echo FAILED! Check that Vulkan SDK is installed and VULKAN_SDK env var is set.
    pause
    exit /b 1
)
echo      [OK] GPU-PCIe-Test_Vulkan.exe

echo.
echo [3/3] Building launcher...
cl /EHsc /std:c++17 /O2 /MD /DNDEBUG /nologo main_launcher.cpp /Fe:GPU-PCIe-Test.exe
if %errorlevel% neq 0 (
    echo FAILED!
    pause
    exit /b 1
)
echo      [OK] GPU-PCIe-Test.exe

del *.obj 2>nul

echo.
echo ==================================================
echo   BUILD SUCCESSFUL!
echo ==================================================
echo.
dir /b GPU-PCIe-Test*.exe 2>nul
echo.
echo TO RUN: GPU-PCIe-Test.exe
echo.
pause

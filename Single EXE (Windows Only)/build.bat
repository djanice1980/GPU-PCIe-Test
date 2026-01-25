@echo off
echo ==================================================
echo   GPU-PCIe-Test - Single EXE Build
echo ==================================================
echo.

REM Check compiler
where cl >nul 2>nul
if %errorlevel% neq 0 (
    echo ERROR: Run from Developer Command Prompt for VS
    pause
    exit /b 1
)

REM Check Vulkan SDK
if "%VULKAN_SDK%"=="" (
    echo ERROR: VULKAN_SDK environment variable not set.
    echo Please install Vulkan SDK from https://vulkan.lunarg.com/
    pause
    exit /b 1
)

echo Building GPU-PCIe-Test.exe...
echo.

cl /EHsc /std:c++17 /O2 /MD /DNDEBUG /nologo /I"%VULKAN_SDK%\Include" GPUPCIETest.cpp d3d12.lib dxgi.lib /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib /out:GPU-PCIe-Test.exe

if %errorlevel% neq 0 (
    echo.
    echo BUILD FAILED!
    pause
    exit /b 1
)

del *.obj 2>nul

echo.
echo ==================================================
echo   BUILD SUCCESSFUL!
echo ==================================================
echo.
echo Output: GPU-PCIe-Test.exe
echo.
pause

@echo off
echo ==================================================
echo   GPU-PCIe-Test v2.0 - Single EXE Build
echo ==================================================
echo.
echo Building GPU-PCIe-Test.exe...
echo.

cl /EHsc /std:c++17 /O2 /MD /I"%VULKAN_SDK%\Include" GPUPCIETest.cpp d3d12.lib dxgi.lib /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib /out:GPU-PCIe-Test.exe

if errorlevel 1 (
    echo.
    echo ==================================================
    echo   BUILD FAILED!
    echo ==================================================
    echo Check that you are running from Developer Command Prompt
    echo and that Vulkan SDK is installed.
    pause
    exit /b 1
)

echo.
echo ==================================================
echo   BUILD SUCCESSFUL!
echo ==================================================
echo.
echo Output: GPU-PCIe-Test.exe
echo.
pause

@echo off
echo ==================================================
echo   GPU-PCIe-Test v2.0 - Complete Build
echo ==================================================
echo.

echo [1/3] Building D3D12 benchmark...
cl /EHsc /std:c++17 /O2 /MD main_improved.cpp d3d12.lib dxgi.lib /link /out:GPU-PCIe-Test_D3D12.exe >nul 2>&1
if errorlevel 1 (
    echo      [FAILED] D3D12 build failed
    cl /EHsc /std:c++17 /O2 /MD main_improved.cpp d3d12.lib dxgi.lib /link /out:GPU-PCIe-Test_D3D12.exe
    pause
    exit /b 1
)
echo      [OK] GPU-PCIe-Test_D3D12.exe

echo [2/3] Building Vulkan benchmark...
cl /EHsc /std:c++17 /O2 /MD /I"%VULKAN_SDK%\Include" main_vulkan.cpp /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib /out:GPU-PCIe-Test_Vulkan.exe >nul 2>&1
if errorlevel 1 (
    echo      [FAILED] Vulkan build failed
    cl /EHsc /std:c++17 /O2 /MD /I"%VULKAN_SDK%\Include" main_vulkan.cpp /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib /out:GPU-PCIe-Test_Vulkan.exe
    pause
    exit /b 1
)
echo      [OK] GPU-PCIe-Test_Vulkan.exe

echo [3/3] Building launcher...
cl /EHsc /std:c++17 /O2 /MD main_launcher.cpp /link /out:GPU-PCIe-Test.exe >nul 2>&1
if errorlevel 1 (
    echo      [FAILED] Launcher build failed
    cl /EHsc /std:c++17 /O2 /MD main_launcher.cpp /link /out:GPU-PCIe-Test.exe
    pause
    exit /b 1
)
echo      [OK] GPU-PCIe-Test.exe

echo.
echo ==================================================
echo   BUILD SUCCESSFUL!
echo ==================================================
echo.
echo GPU-PCIe-Test.exe
echo GPU-PCIe-Test_D3D12.exe
echo GPU-PCIe-Test_Vulkan.exe
echo.
echo TO RUN: GPU-PCIe-Test.exe
echo.
pause

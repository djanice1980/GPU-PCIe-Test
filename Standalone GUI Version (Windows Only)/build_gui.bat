@echo off
setlocal

echo ==================================================
echo   GPU-PCIe-Test v2.0 GUI - Build Script
echo ==================================================
echo.

REM Check for Visual Studio
where cl >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Visual Studio compiler not found!
    echo Please run from a Developer Command Prompt.
    pause
    exit /b 1
)

REM Check for --clean flag to force re-download
if "%1"=="--clean" (
    echo Cleaning imgui folder...
    if exist imgui rmdir /s /q imgui
)

REM Create imgui directory and download if needed
if not exist imgui (
    echo [1/4] Downloading ImGui docking branch...
    mkdir imgui
    
    echo      Downloading core files...
    curl -sL -o imgui/imgui.h "https://raw.githubusercontent.com/ocornut/imgui/docking/imgui.h"
    curl -sL -o imgui/imgui.cpp "https://raw.githubusercontent.com/ocornut/imgui/docking/imgui.cpp"
    curl -sL -o imgui/imgui_draw.cpp "https://raw.githubusercontent.com/ocornut/imgui/docking/imgui_draw.cpp"
    curl -sL -o imgui/imgui_tables.cpp "https://raw.githubusercontent.com/ocornut/imgui/docking/imgui_tables.cpp"
    curl -sL -o imgui/imgui_widgets.cpp "https://raw.githubusercontent.com/ocornut/imgui/docking/imgui_widgets.cpp"
    curl -sL -o imgui/imgui_internal.h "https://raw.githubusercontent.com/ocornut/imgui/docking/imgui_internal.h"
    curl -sL -o imgui/imstb_rectpack.h "https://raw.githubusercontent.com/ocornut/imgui/docking/imstb_rectpack.h"
    curl -sL -o imgui/imstb_textedit.h "https://raw.githubusercontent.com/ocornut/imgui/docking/imstb_textedit.h"
    curl -sL -o imgui/imstb_truetype.h "https://raw.githubusercontent.com/ocornut/imgui/docking/imstb_truetype.h"
    curl -sL -o imgui/imconfig.h "https://raw.githubusercontent.com/ocornut/imgui/docking/imconfig.h"
    
    echo      Downloading backends...
    curl -sL -o imgui/imgui_impl_win32.h "https://raw.githubusercontent.com/ocornut/imgui/docking/backends/imgui_impl_win32.h"
    curl -sL -o imgui/imgui_impl_win32.cpp "https://raw.githubusercontent.com/ocornut/imgui/docking/backends/imgui_impl_win32.cpp"
    curl -sL -o imgui/imgui_impl_dx12.h "https://raw.githubusercontent.com/ocornut/imgui/docking/backends/imgui_impl_dx12.h"
    curl -sL -o imgui/imgui_impl_dx12.cpp "https://raw.githubusercontent.com/ocornut/imgui/docking/backends/imgui_impl_dx12.cpp"
    
    echo      [OK] ImGui downloaded
) else (
    echo [1/4] ImGui already present
)

REM Download ImPlot - use master branch for compatibility with docking ImGui
if not exist imgui\implot.h (
    echo [2/4] Downloading ImPlot master branch...
    
    curl -sL -o imgui/implot.h "https://raw.githubusercontent.com/epezent/implot/master/implot.h"
    curl -sL -o imgui/implot.cpp "https://raw.githubusercontent.com/epezent/implot/master/implot.cpp"
    curl -sL -o imgui/implot_internal.h "https://raw.githubusercontent.com/epezent/implot/master/implot_internal.h"
    curl -sL -o imgui/implot_items.cpp "https://raw.githubusercontent.com/epezent/implot/master/implot_items.cpp"
    
    echo      [OK] ImPlot downloaded
) else (
    echo [2/4] ImPlot already present
)

REM Verify downloads
echo [3/4] Verifying files...
set MISSING=0

if not exist imgui\imgui.h set MISSING=1
if not exist imgui\imgui.cpp set MISSING=1
if not exist imgui\imgui_impl_dx12.cpp set MISSING=1
if not exist imgui\implot.h set MISSING=1
if not exist imgui\implot.cpp set MISSING=1

if %MISSING%==1 (
    echo [ERROR] Some files are missing. Delete imgui folder and retry.
    pause
    exit /b 1
)
echo      [OK] All files present

REM Compile
echo [4/4] Compiling...

cl /nologo /EHsc /std:c++17 /O2 /MD /I. /DUNICODE /D_UNICODE main_gui.cpp imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp imgui/imgui_impl_win32.cpp imgui/imgui_impl_dx12.cpp imgui/implot.cpp imgui/implot_items.cpp d3d12.lib dxgi.lib user32.lib gdi32.lib /Fe:GPU-PCIe-Test_GUI.exe /link /SUBSYSTEM:WINDOWS

if errorlevel 1 (
    echo.
    echo [ERROR] Compilation failed!
    pause
    exit /b 1
)

REM Cleanup
del *.obj >nul 2>&1

echo.
echo ==================================================
echo   BUILD SUCCESSFUL!
echo ==================================================
echo.
echo Output: GPU-PCIe-Test_GUI.exe
echo.
pause

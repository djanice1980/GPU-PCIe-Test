@echo off
REM ============================================================================
REM GPU-PCIe-Test v3.0 (Vulkan) - Build Script
REM Requires: Visual Studio 2019+ (MSVC), Vulkan SDK
REM Automatically downloads ImGui + ImPlot if not present
REM ============================================================================
setlocal enabledelayedexpansion

REM ---- Configuration ----
set "OUTPUT=GPU-PCIe-Test-Vulkan.exe"
set "MAIN_SRC=main_gui_vulkan.cpp"
set "IMGUI_DIR=imgui"
set "IMGUI_VERSION=v1.91.8-docking"
set "IMPLOT_VERSION=v0.16"
set "TEMP_DIR=_build_temp"

REM ---- Auto-detect Vulkan SDK ----
if defined VULKAN_SDK (
    set "VK_SDK=!VULKAN_SDK!"
) else if exist "C:\VulkanSDK" (
    for /f "delims=" %%d in ('dir /b /ad /o-n "C:\VulkanSDK" 2^>nul') do (
        set "VK_SDK=C:\VulkanSDK\%%d"
        goto :vk_found
    )
)
if not defined VK_SDK (
    echo [ERROR] Vulkan SDK not found
    echo   - Install from https://vulkan.lunarg.com/sdk/home
    echo   - Or set VULKAN_SDK environment variable
    exit /b 1
)
:vk_found
echo [INFO] Vulkan SDK: %VK_SDK%

if not exist "%VK_SDK%\Include\vulkan\vulkan.h" (
    echo [ERROR] vulkan.h not found in %VK_SDK%\Include\vulkan\
    exit /b 1
)
if not exist "%VK_SDK%\Lib\vulkan-1.lib" (
    echo [ERROR] vulkan-1.lib not found in %VK_SDK%\Lib\
    exit /b 1
)

REM ---- Verify MSVC is available ----
where cl.exe >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] cl.exe not found - Run this from a Visual Studio Developer Command Prompt
    echo   - Open "x64 Native Tools Command Prompt for VS 2022" (or 2019^)
    echo   - Or run: "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    exit /b 1
)
echo [INFO] MSVC compiler found

REM ---- Verify main source ----
if not exist "%MAIN_SRC%" (
    echo [ERROR] %MAIN_SRC% not found in current directory
    exit /b 1
)

REM ---- Check if ImGui/ImPlot need downloading ----
set "NEED_DOWNLOAD=0"
if not exist "%IMGUI_DIR%\imgui.cpp" set "NEED_DOWNLOAD=1"
if not exist "%IMGUI_DIR%\imgui_draw.cpp" set "NEED_DOWNLOAD=1"
if not exist "%IMGUI_DIR%\imgui_tables.cpp" set "NEED_DOWNLOAD=1"
if not exist "%IMGUI_DIR%\imgui_widgets.cpp" set "NEED_DOWNLOAD=1"
if not exist "%IMGUI_DIR%\imgui_impl_win32.cpp" set "NEED_DOWNLOAD=1"
if not exist "%IMGUI_DIR%\imgui_impl_vulkan.cpp" set "NEED_DOWNLOAD=1"
if not exist "%IMGUI_DIR%\implot.cpp" set "NEED_DOWNLOAD=1"
if not exist "%IMGUI_DIR%\implot_items.cpp" set "NEED_DOWNLOAD=1"
if not exist "%IMGUI_DIR%\imconfig.h" set "NEED_DOWNLOAD=1"

if "!NEED_DOWNLOAD!"=="0" (
    echo [INFO] ImGui sources found in %IMGUI_DIR%\
    goto :do_compile
)

echo.
echo [INFO] ImGui/ImPlot sources not found - Downloading
echo.

if not exist "%TEMP_DIR%" mkdir "%TEMP_DIR%"
if not exist "%IMGUI_DIR%" mkdir "%IMGUI_DIR%"

REM ---- Download ImGui ----
echo [DOWNLOAD] ImGui %IMGUI_VERSION%
set "IMGUI_ZIP=%TEMP_DIR%\imgui.zip"
set "IMGUI_URL=https://github.com/ocornut/imgui/archive/refs/tags/%IMGUI_VERSION%.zip"

powershell -NoProfile -Command ^
    "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; " ^
    "try { Invoke-WebRequest -Uri '%IMGUI_URL%' -OutFile '%IMGUI_ZIP%' -UseBasicParsing } " ^
    "catch { Write-Host '[ERROR] Failed to download ImGui'; exit 1 }"

if not exist "%IMGUI_ZIP%" (
    echo [ERROR] ImGui download failed - Check your internet connection
    echo   URL: %IMGUI_URL%
    goto :cleanup_fail
)
echo [OK] ImGui downloaded

REM ---- Extract ImGui ----
echo [EXTRACT] ImGui
powershell -NoProfile -Command ^
    "Expand-Archive -Path '%IMGUI_ZIP%' -DestinationPath '%TEMP_DIR%' -Force"

set "IMGUI_EXTRACT="
for /f "delims=" %%d in ('dir /b /ad "%TEMP_DIR%\imgui-*" 2^>nul') do set "IMGUI_EXTRACT=%TEMP_DIR%\%%d"

if not defined IMGUI_EXTRACT (
    echo [ERROR] Could not find extracted ImGui folder
    goto :cleanup_fail
)
echo [OK] Extracted to %IMGUI_EXTRACT%

REM ---- Copy ImGui core files ----
echo [COPY] ImGui core files
copy /y "%IMGUI_EXTRACT%\imgui.cpp" "%IMGUI_DIR%\" >nul 2>&1
copy /y "%IMGUI_EXTRACT%\imgui.h" "%IMGUI_DIR%\" >nul 2>&1
copy /y "%IMGUI_EXTRACT%\imgui_draw.cpp" "%IMGUI_DIR%\" >nul 2>&1
copy /y "%IMGUI_EXTRACT%\imgui_tables.cpp" "%IMGUI_DIR%\" >nul 2>&1
copy /y "%IMGUI_EXTRACT%\imgui_widgets.cpp" "%IMGUI_DIR%\" >nul 2>&1
copy /y "%IMGUI_EXTRACT%\imgui_demo.cpp" "%IMGUI_DIR%\" >nul 2>&1
copy /y "%IMGUI_EXTRACT%\imgui_internal.h" "%IMGUI_DIR%\" >nul 2>&1
copy /y "%IMGUI_EXTRACT%\imconfig.h" "%IMGUI_DIR%\" >nul 2>&1
copy /y "%IMGUI_EXTRACT%\imstb_rectpack.h" "%IMGUI_DIR%\" >nul 2>&1
copy /y "%IMGUI_EXTRACT%\imstb_textedit.h" "%IMGUI_DIR%\" >nul 2>&1
copy /y "%IMGUI_EXTRACT%\imstb_truetype.h" "%IMGUI_DIR%\" >nul 2>&1

REM ---- Copy ImGui backend files (Win32 + Vulkan) ----
echo [COPY] ImGui backends (Win32 + Vulkan)
copy /y "%IMGUI_EXTRACT%\backends\imgui_impl_win32.cpp" "%IMGUI_DIR%\" >nul 2>&1
copy /y "%IMGUI_EXTRACT%\backends\imgui_impl_win32.h" "%IMGUI_DIR%\" >nul 2>&1
copy /y "%IMGUI_EXTRACT%\backends\imgui_impl_vulkan.cpp" "%IMGUI_DIR%\" >nul 2>&1
copy /y "%IMGUI_EXTRACT%\backends\imgui_impl_vulkan.h" "%IMGUI_DIR%\" >nul 2>&1

if not exist "%IMGUI_DIR%\imgui_impl_win32.cpp" (
    echo [ERROR] Backend files failed to copy - check extracted folder structure
    goto :cleanup_fail
)

REM ---- Download ImPlot ----
echo [DOWNLOAD] ImPlot %IMPLOT_VERSION%
set "IMPLOT_ZIP=%TEMP_DIR%\implot.zip"
set "IMPLOT_URL=https://github.com/epezent/implot/archive/refs/tags/%IMPLOT_VERSION%.zip"

powershell -NoProfile -Command ^
    "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; " ^
    "try { Invoke-WebRequest -Uri '%IMPLOT_URL%' -OutFile '%IMPLOT_ZIP%' -UseBasicParsing } " ^
    "catch { Write-Host '[ERROR] Failed to download ImPlot'; exit 1 }"

if not exist "%IMPLOT_ZIP%" (
    echo [ERROR] ImPlot download failed - Check your internet connection
    echo   URL: %IMPLOT_URL%
    goto :cleanup_fail
)
echo [OK] ImPlot downloaded

REM ---- Extract ImPlot ----
echo [EXTRACT] ImPlot
powershell -NoProfile -Command ^
    "Expand-Archive -Path '%IMPLOT_ZIP%' -DestinationPath '%TEMP_DIR%' -Force"

set "IMPLOT_EXTRACT="
for /f "delims=" %%d in ('dir /b /ad "%TEMP_DIR%\implot-*" 2^>nul') do set "IMPLOT_EXTRACT=%TEMP_DIR%\%%d"

if not defined IMPLOT_EXTRACT (
    echo [ERROR] Could not find extracted ImPlot folder
    goto :cleanup_fail
)
echo [OK] Extracted to %IMPLOT_EXTRACT%

REM ---- Copy ImPlot files ----
echo [COPY] ImPlot files
copy /y "%IMPLOT_EXTRACT%\implot.cpp" "%IMGUI_DIR%\" >nul 2>&1
copy /y "%IMPLOT_EXTRACT%\implot.h" "%IMGUI_DIR%\" >nul 2>&1
copy /y "%IMPLOT_EXTRACT%\implot_items.cpp" "%IMGUI_DIR%\" >nul 2>&1
copy /y "%IMPLOT_EXTRACT%\implot_internal.h" "%IMGUI_DIR%\" >nul 2>&1

REM ---- Cleanup temp files ----
echo [CLEANUP] Removing temp files
rd /s /q "%TEMP_DIR%" 2>nul

echo.
echo [OK] All dependencies downloaded successfully
echo.

:do_compile

REM ---- Final verification ----
set "MISSING="
if not exist "%IMGUI_DIR%\imgui.cpp" echo [ERROR] Missing: %IMGUI_DIR%\imgui.cpp & set "MISSING=1"
if not exist "%IMGUI_DIR%\imgui_draw.cpp" echo [ERROR] Missing: %IMGUI_DIR%\imgui_draw.cpp & set "MISSING=1"
if not exist "%IMGUI_DIR%\imgui_tables.cpp" echo [ERROR] Missing: %IMGUI_DIR%\imgui_tables.cpp & set "MISSING=1"
if not exist "%IMGUI_DIR%\imgui_widgets.cpp" echo [ERROR] Missing: %IMGUI_DIR%\imgui_widgets.cpp & set "MISSING=1"
if not exist "%IMGUI_DIR%\imgui_impl_win32.cpp" echo [ERROR] Missing: %IMGUI_DIR%\imgui_impl_win32.cpp & set "MISSING=1"
if not exist "%IMGUI_DIR%\imgui_impl_vulkan.cpp" echo [ERROR] Missing: %IMGUI_DIR%\imgui_impl_vulkan.cpp & set "MISSING=1"
if not exist "%IMGUI_DIR%\implot.cpp" echo [ERROR] Missing: %IMGUI_DIR%\implot.cpp & set "MISSING=1"
if not exist "%IMGUI_DIR%\implot_items.cpp" echo [ERROR] Missing: %IMGUI_DIR%\implot_items.cpp & set "MISSING=1"
if not exist "%IMGUI_DIR%\imconfig.h" echo [ERROR] Missing: %IMGUI_DIR%\imconfig.h & set "MISSING=1"

if defined MISSING (
    echo [ERROR] Source files still missing - Check errors above
    exit /b 1
)

REM ---- Compile ----
echo.
echo ============================================================
echo  Building %OUTPUT%
echo ============================================================
echo.

set LIBS=vulkan-1.lib setupapi.lib cfgmgr32.lib wbemuuid.lib ole32.lib oleaut32.lib user32.lib gdi32.lib shell32.lib dwmapi.lib
set CFLAGS=/nologo /std:c++17 /EHsc /O2 /DNDEBUG /DUNICODE /D_UNICODE /DNOMINMAX /utf-8
set INCLUDES=/I"%VK_SDK%\Include" /I.
set LDFLAGS=/link /SUBSYSTEM:WINDOWS /LIBPATH:"%VK_SDK%\Lib" /ENTRY:WinMainCRTStartup

echo [COMPILE] %MAIN_SRC% + ImGui sources
cl.exe %CFLAGS% %INCLUDES% "%MAIN_SRC%" "%IMGUI_DIR%\imgui.cpp" "%IMGUI_DIR%\imgui_draw.cpp" "%IMGUI_DIR%\imgui_tables.cpp" "%IMGUI_DIR%\imgui_widgets.cpp" "%IMGUI_DIR%\imgui_impl_win32.cpp" "%IMGUI_DIR%\imgui_impl_vulkan.cpp" "%IMGUI_DIR%\implot.cpp" "%IMGUI_DIR%\implot_items.cpp" /Fe:"%OUTPUT%" %LIBS% %LDFLAGS%

if %errorlevel% neq 0 (
    echo.
    echo ============================================================
    echo  BUILD FAILED
    echo ============================================================
    echo.
    echo Common fixes:
    echo   1. Make sure you are in a x64 VS Developer Command Prompt
    echo   2. Verify Vulkan SDK is installed correctly
    echo   3. Check compiler errors above
    exit /b 1
)

REM ---- Clean up .obj files ----
del /q *.obj 2>nul

echo.
echo ============================================================
echo  BUILD SUCCESSFUL: %OUTPUT%
echo ============================================================
echo.
echo Run: %OUTPUT%
echo.
goto :eof

:cleanup_fail
echo.
echo [ERROR] Dependency download failed - Cleaning up
rd /s /q "%TEMP_DIR%" 2>nul
echo.
echo You can manually place the files in the %IMGUI_DIR%\ folder:
echo   ImGui:  https://github.com/ocornut/imgui/releases
echo   ImPlot: https://github.com/epezent/implot/releases
exit /b 1

endlocal

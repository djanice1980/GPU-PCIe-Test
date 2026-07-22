@echo off
setlocal enabledelayedexpansion

echo ==================================================
echo   GPU-PCIe-Test v3.0 GUI - Build Script
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

REM Pin ImGui to v1.91.8-docking (must match ImPlot v0.16 compatibility)
REM Every download is verified against a pinned SHA-256 so a moved tag,
REM compromised mirror, or truncated transfer fails the build loudly instead
REM of compiling unexpected code. Hashes are of the files at the pinned tags.
set "IMGUI_BASE=https://raw.githubusercontent.com/ocornut/imgui/v1.91.8-docking"
set "IMPLOT_BASE=https://raw.githubusercontent.com/epezent/implot/v0.16"

if not exist imgui mkdir imgui

echo [1/4] Fetching ImGui v1.91.8-docking (pinned + SHA-256 verified)...
call :fetch imgui\imgui.h            "%IMGUI_BASE%/imgui.h"            dfd07f054b29429887d8e8665ff408d0bdc7108690673ad4440016c6536cbd55 || goto :fetch_fail
call :fetch imgui\imgui.cpp          "%IMGUI_BASE%/imgui.cpp"          5178dbe4868ca9946f403036d82c010b6aec98146b9a1af0cd120af6e5f2797f || goto :fetch_fail
call :fetch imgui\imgui_draw.cpp     "%IMGUI_BASE%/imgui_draw.cpp"     df4f56f9fd3a3252684704d68529faaf4571d60a257fd2b2e1f0b21e3d2b0e93 || goto :fetch_fail
call :fetch imgui\imgui_tables.cpp   "%IMGUI_BASE%/imgui_tables.cpp"   b7f7de78d8cd7f5b6757f75499b6df0b2c42f85cdd276bf3016c2ab77eac6ad9 || goto :fetch_fail
call :fetch imgui\imgui_widgets.cpp  "%IMGUI_BASE%/imgui_widgets.cpp"  29e09708f06544892cd5d29d485900cbfbad1f76f273563b062041d2d0ba9ee4 || goto :fetch_fail
call :fetch imgui\imgui_internal.h   "%IMGUI_BASE%/imgui_internal.h"   efc0a56ea757a696c3a2e54520e56cc584e33b6ad904baa4193f1629264240a1 || goto :fetch_fail
call :fetch imgui\imstb_rectpack.h   "%IMGUI_BASE%/imstb_rectpack.h"   2efa3d5f7d003c19743b15155dad9f46d9f9fc783a18893d703b431d3c990972 || goto :fetch_fail
call :fetch imgui\imstb_textedit.h   "%IMGUI_BASE%/imstb_textedit.h"   77010e494caf72c178b573569612aec409d24380409676325a31d02de0ec64b7 || goto :fetch_fail
call :fetch imgui\imstb_truetype.h   "%IMGUI_BASE%/imstb_truetype.h"   87f08919ae8e7223451d0e9d06b62829075d3e920c9e6ad436db9d7e7427ffce || goto :fetch_fail
call :fetch imgui\imconfig.h         "%IMGUI_BASE%/imconfig.h"         bd89b32de6a8a286700383bae47b4f4bba753ba13058d1a350770c5232b2e4e8 || goto :fetch_fail
call :fetch imgui\imgui_impl_win32.h   "%IMGUI_BASE%/backends/imgui_impl_win32.h"   10450c3ceee5cbae2a0dfe545c5f3a5026ff1e42999b097910f101f5400009b2 || goto :fetch_fail
call :fetch imgui\imgui_impl_win32.cpp "%IMGUI_BASE%/backends/imgui_impl_win32.cpp" 5dac02977bf52c6b6ec6f04a0096e23ed137d036cc62e95015ae9117e46c1fd2 || goto :fetch_fail
call :fetch imgui\imgui_impl_dx12.h    "%IMGUI_BASE%/backends/imgui_impl_dx12.h"    90c4d0cb2c18a9523b83edc2f5d396ee08bf62d3821def15f5bc37c22c64bea7 || goto :fetch_fail
call :fetch imgui\imgui_impl_dx12.cpp  "%IMGUI_BASE%/backends/imgui_impl_dx12.cpp"  8a1cd7d0016d7fac5f2aedd03704a6ce399697bcc5ef01f2d73efd8dcc4c9bea || goto :fetch_fail

echo [2/4] Fetching ImPlot v0.16 (pinned + SHA-256 verified)...
call :fetch imgui\implot.h          "%IMPLOT_BASE%/implot.h"          d7c67b201b82e4a5ada9d26430799cb0acda916a05dd782a6764ede2b661b05d || goto :fetch_fail
call :fetch imgui\implot.cpp        "%IMPLOT_BASE%/implot.cpp"        5cde82830aef66e142419c88bbecb838913d49d2ef1fb25a0f7113d869067a41 || goto :fetch_fail
call :fetch imgui\implot_internal.h "%IMPLOT_BASE%/implot_internal.h" cb7143465228cebd50aaa25e7c2f4a10579ae1c2dff81ffc677e006df85c048a || goto :fetch_fail
call :fetch imgui\implot_items.cpp  "%IMPLOT_BASE%/implot_items.cpp"  ffb86555fe2de5046d0ecacf9486ee159f878033d583e32cab157fc28901331d || goto :fetch_fail

echo [3/4] All dependencies present and verified

REM Compile
echo [4/4] Compiling...

cl /nologo /W4 /WX /EHsc /std:c++17 /O2 /MD /I. /DUNICODE /D_UNICODE main_gui.cpp imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp imgui/imgui_impl_win32.cpp imgui/imgui_impl_dx12.cpp imgui/implot.cpp imgui/implot_items.cpp d3d12.lib dxgi.lib d3dcompiler.lib setupapi.lib cfgmgr32.lib wbemuuid.lib ole32.lib oleaut32.lib user32.lib gdi32.lib /Fe:GPU-PCIe-Test_GUI.exe /link /SUBSYSTEM:WINDOWS

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
exit /b 0

REM ---------------------------------------------------------------------------
REM :fetch <destination> <url> <sha256>
REM Downloads (unless already present and valid) then verifies the SHA-256.
REM A file that exists but does not match is re-downloaded once, then fatal.
REM ---------------------------------------------------------------------------
:fetch
set "F_DEST=%~1"
set "F_URL=%~2"
set "F_HASH=%~3"

if exist "%F_DEST%" (
    call :checkhash "%F_DEST%" %F_HASH%
    if not errorlevel 1 exit /b 0
    echo      [WARN] %F_DEST% exists but hash mismatches - re-downloading
    del "%F_DEST%" >nul 2>&1
)

curl -fsSL --retry 3 -o "%F_DEST%" "%F_URL%"
if errorlevel 1 (
    echo      [ERROR] Download failed: %F_URL%
    exit /b 1
)
call :checkhash "%F_DEST%" %F_HASH%
if errorlevel 1 (
    echo      [ERROR] SHA-256 mismatch for %F_DEST%
    echo              expected %F_HASH%
    echo              got      !F_ACTUAL!
    echo      The pinned tag content changed or the download was tampered with.
    exit /b 1
)
exit /b 0

:checkhash
set "F_ACTUAL="
for /f "skip=1 delims=" %%H in ('certutil -hashfile "%~1" SHA256 2^>nul') do if not defined F_ACTUAL set "F_ACTUAL=%%H"
set "F_ACTUAL=%F_ACTUAL: =%"
if /i "%F_ACTUAL%"=="%~2" exit /b 0
exit /b 1

:fetch_fail
echo.
echo [ERROR] Dependency download/verification failed - see message above.
echo         Delete the imgui folder and retry, or check your network.
pause
exit /b 1

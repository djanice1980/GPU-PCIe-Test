# Building GPU-PCIe-Test

## Prerequisites

### Windows (Required)

- **Visual Studio 2019 or later** with "Desktop development with C++" workload
  - Download: https://visualstudio.microsoft.com/
  - During installation, select "Desktop development with C++"

### Vulkan SDK (Optional - for Vulkan build)

- **LunarG Vulkan SDK**
  - Download: https://vulkan.lunarg.com/sdk/home
  - Run the installer with default options
  - The installer sets the `VULKAN_SDK` environment variable automatically
  - **Restart your command prompt** after installation for the environment variable to take effect

## Build Options

### Option 1: Build Script (Recommended)

1. Open **"Developer Command Prompt for VS"**
   - Press Start, search for "Developer Command Prompt"
   - Make sure it says "for VS 2019" or "for VS 2022" (not just "Command Prompt")

2. Navigate to the project folder:
   ```cmd
   cd C:\path\to\GPU-PCIe-Test
   ```

3. Run the build script:
   ```cmd
   build_final.bat
   ```

4. If successful, you'll see:
   ```
   ==================================================
     BUILD SUCCESSFUL!
   ==================================================

   GPU-PCIe-Test.exe
   GPU-PCIe-Test_D3D12.exe
   GPU-PCIe-Test_Vulkan.exe
   ```

### Option 2: Manual Build

Open **"Developer Command Prompt for VS"** and navigate to the project folder.

#### Build D3D12 version only:
```cmd
cl /EHsc /std:c++17 /O2 /MD /DNDEBUG main_improved.cpp d3d12.lib dxgi.lib /Fe:GPU-PCIe-Test_D3D12.exe
```

#### Build Vulkan version only:
```cmd
cl /EHsc /std:c++17 /O2 /MD /DNDEBUG /I"%VULKAN_SDK%\Include" main_vulkan.cpp /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib /out:GPU-PCIe-Test_Vulkan.exe
```

#### Build launcher:
```cmd
cl /EHsc /std:c++17 /O2 /MD /DNDEBUG main_launcher.cpp /Fe:GPU-PCIe-Test.exe
```

#### Clean up object files:
```cmd
del *.obj
```

## Troubleshooting Build Errors

### "'cl' is not recognized"

You're not in a Developer Command Prompt. 

**Solution:** Close your current command prompt and open "Developer Command Prompt for VS 2019" (or 2022) from the Start menu.

### "Cannot open include file: 'vulkan/vulkan.h'"

The Vulkan SDK is not installed or the environment variable isn't set.

**Solution:**
1. Install the Vulkan SDK from https://vulkan.lunarg.com/sdk/home
2. Restart your command prompt after installation
3. Verify with: `echo %VULKAN_SDK%` (should show the SDK path)

### "cannot open file 'vulkan-1.lib'"

Same as above - Vulkan SDK not installed or environment variable not set.

### "LINK : fatal error LNK1181: cannot open input file 'd3d12.lib'"

You're not using the Developer Command Prompt, or the Windows SDK is not installed.

**Solution:** 
1. Use "Developer Command Prompt for VS"
2. If still failing, repair Visual Studio installation and ensure "Windows 10 SDK" is selected

### Batch file shows garbled output or strange errors

The batch file may have encoding issues.

**Solution:** 
1. Open `build_final.bat` in Notepad
2. File → Save As
3. Change "Encoding" dropdown to "ANSI"
4. Save and overwrite

## File Descriptions

| File | Description |
|------|-------------|
| `main_improved.cpp` | Direct3D 12 benchmark implementation |
| `main_vulkan.cpp` | Vulkan benchmark implementation (cross-platform) |
| `main_launcher.cpp` | Menu launcher that runs D3D12/Vulkan executables |
| `bandwidth_analysis.h` | Shared header with bandwidth analysis and reference data |
| `build_final.bat` | Windows build script |

## Output Files

After building:

| File | Description |
|------|-------------|
| `GPU-PCIe-Test.exe` | Main launcher (run this) |
| `GPU-PCIe-Test_D3D12.exe` | Direct3D 12 benchmark (can run standalone) |
| `GPU-PCIe-Test_Vulkan.exe` | Vulkan benchmark (can run standalone) |

After running:

| File | Description |
|------|-------------|
| `gpu_benchmark_results.csv` | Test results log (appended each run) |

## Debug Build

For debugging, replace `/O2 /DNDEBUG` with `/Od /Zi`:

```cmd
cl /EHsc /std:c++17 /Od /Zi /MD main_improved.cpp d3d12.lib dxgi.lib /Fe:GPU-PCIe-Test_D3D12.exe
```

This enables debug symbols and disables optimization for easier debugging in Visual Studio.

## Linux Build (Vulkan only)

The Vulkan version supports Linux. Requirements:
- GCC or Clang with C++17 support
- Vulkan SDK / development headers (`libvulkan-dev` on Debian/Ubuntu)

```bash
# Install Vulkan development files (Debian/Ubuntu)
sudo apt install libvulkan-dev

# Build
g++ -std=c++17 -O2 main_vulkan.cpp -lvulkan -o GPU-PCIe-Test_Vulkan
```

Note: The launcher (`main_launcher.cpp`) is Windows-only. On Linux, run `GPU-PCIe-Test_Vulkan` directly.

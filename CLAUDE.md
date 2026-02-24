# GPU-PCIe-Test

## Project Overview
GPU/PCIe bandwidth, latency, and VRAM integrity benchmarking tool.
C++17 with D3D12 (Windows primary), Vulkan (Windows alternative), and Vulkan+GLFW (Linux).
GUI built with Dear ImGui (docking branch) + ImPlot.

## Build

### Windows
- Requires MSVC via Visual Studio 2019+ Developer Command Prompt
- `build_gui.bat` - Main GUI build (auto-downloads ImGui/ImPlot from GitHub)
- `Vulkan/build_vulkan.bat` - Vulkan GUI variant (needs VULKAN_SDK env var)
- Compiler flags: `/O2 /MD /EHsc /std:c++17 /DUNICODE /D_UNICODE`
- Links: d3d12.lib dxgi.lib setupapi.lib cfgmgr32.lib wbemuuid.lib ole32.lib oleaut32.lib

### Linux
- Requires CMake 3.16+, g++, Vulkan SDK, GLFW3
- `Linux/build_linux.sh` - Convenience build script (auto-downloads ImGui/ImPlot via CMake FetchContent)
- Or manual: `cd Linux && mkdir build && cd build && cmake .. && make -j$(nproc)`
- Optional: `-DENABLE_VULKAN_VALIDATION=ON` for debug builds

## Architecture
- `main_gui.cpp` (~5700 lines) - Monolithic D3D12 primary source (Windows)
- `Vulkan/main_gui_vulkan.cpp` - Vulkan implementation (Windows, Win32 windowing)
- `Linux/main_gui_vulkan_linux.cpp` - Vulkan implementation (Linux, GLFW windowing)
- Separate render device vs benchmark device (can test GPU #2 while rendering on GPU #1)
- Threading: main thread (message pump + ImGui), benchmark thread, VRAM scan thread
- Synchronization via atomics (`cancelRequested`, `benchmarkAborted`) and mutexes

## Key Implementation Details
- Upload measurement on discrete GPUs uses CPU round-trip minus download time (ReBAR workaround)
- Integrated GPUs use GPU timestamps directly for both upload/download
- PCIe link info: Windows via SetupAPI, Linux via sysfs (/sys/bus/pci/devices/)
- External GPU detection: Windows via cfgmgr32 device tree, Linux via sysfs thunderbolt subsystem
- VRAM scanning: 512MB chunks, 8 test patterns, fresh alloc per chunk for physical coverage
- System RAM: Windows via WMI (Win32_PhysicalMemory), Linux via /proc/meminfo + dmidecode

## Code Style
- Single monolithic .cpp files per variant (no header splitting)
- Constants namespace at top of file for tuning parameters
- ComPtr (WRL) for all COM objects on Windows - no manual Release()
- Windows: Unicode throughout with WideToUtf8(), Utf8ToWide() helpers
- Linux: Native UTF-8, sysfs helpers (ReadSysfsFile, ExecCommand)
- Versioned releases in commit messages (e.g., "v3.0.1: description")

## Portability
- D3D12 variant is Windows-only
- Vulkan variant has Windows (Win32) and Linux (GLFW) builds
- Hardware detection abstracted per-platform (SetupAPI/WMI vs sysfs/dmidecode)
- Benchmark methodology is identical across all variants for comparable results

## Directories
- `Vulkan/` - Vulkan GUI variant (Windows)
- `Linux/` - Vulkan GUI variant (Linux, GLFW)
- `Legacy/Single EXE (Windows Only)/` - Console combined build (no longer developed)
- `Legacy/Original Multi EXE Build/` - Launcher + separate D3D12/Vulkan exes (no longer developed)

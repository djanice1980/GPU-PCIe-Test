# GPU-PCIe-Test

## Project Overview
Windows GPU/PCIe bandwidth, latency, and VRAM integrity benchmarking tool.
C++17 with D3D12 (primary), Vulkan (alternative), and CUDA (reference) backends.
GUI built with Dear ImGui (docking branch) + ImPlot.

## Build
- Requires MSVC via Visual Studio 2019+ Developer Command Prompt
- `build_gui.bat` - Main GUI build (auto-downloads ImGui/ImPlot from GitHub)
- `Vulkan/build_vulkan.bat` - Vulkan GUI variant (needs VULKAN_SDK env var)
- `Single EXE (Windows Only)/build.bat` - Console combined D3D12+Vulkan
- `Original Multi EXE Build/build_final.bat` - Legacy 3-exe architecture
- Compiler flags: `/O2 /MD /EHsc /std:c++17 /DUNICODE /D_UNICODE`
- Links: d3d12.lib dxgi.lib setupapi.lib cfgmgr32.lib wbemuuid.lib ole32.lib oleaut32.lib

## Architecture
- `main_gui.cpp` (~5700 lines) - Monolithic primary source (GUI + benchmarks + HW detection)
- `Vulkan/main_gui_vulkan.cpp` - Parallel Vulkan implementation with same GUI
- Separate render device vs benchmark device (can test GPU #2 while rendering on GPU #1)
- Threading: main thread (message pump + ImGui), benchmark thread, VRAM scan thread
- Synchronization via atomics (`cancelRequested`, `benchmarkAborted`) and mutexes

## Key Implementation Details
- Upload measurement on discrete GPUs uses CPU round-trip minus download time (ReBAR workaround)
- Integrated GPUs use GPU timestamps directly for both upload/download
- PCIe link info obtained via SetupAPI (not exposed by D3D12)
- External GPU detection via device tree traversal matching controller IDs
- VRAM scanning: 512MB chunks, 8 test patterns, fresh alloc per chunk for physical coverage
- WMI queries (Win32_PhysicalMemory) for system RAM speed/channels

## Code Style
- Single monolithic .cpp files per variant (no header splitting)
- Constants namespace at top of file for tuning parameters
- ComPtr (WRL) for all COM objects - no manual Release()
- Unicode throughout: WideToUtf8(), Utf8ToWide() helpers
- Versioned releases in commit messages (e.g., "v3.0.1: description")

## Directories
- `Vulkan/` - Vulkan GUI variant
- `Single EXE (Windows Only)/` - Console combined build
- `Original Multi EXE Build/` - Legacy launcher + separate D3D12/Vulkan exes
- `NVIDIA_CUDA Only/` - Linux CUDA reference (bash build, not Windows)

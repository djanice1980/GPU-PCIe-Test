# GPU-PCIe-Test

A tool to benchmark GPU/PCIe bandwidth, latency, and VRAM integrity. Measures real-world data transfer speeds between CPU and GPU memory.

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey.svg)
![DirectX](https://img.shields.io/badge/DirectX-12-green.svg)
![Vulkan](https://img.shields.io/badge/Vulkan-1.1-red.svg)

## Features

### Bandwidth Testing
- **Upload Speed** - CPU to GPU transfer rates
- **Download Speed** - GPU to CPU transfer rates  
- **Bidirectional** - Simultaneous upload/download
- **Latency** - Round-trip timing measurements

### VRAM Integrity Scanning
- **Multi-chunk testing** - Tests up to 80-90% of VRAM by cycling through multiple allocations
- **8 test patterns** - Zeros, ones, checkerboard, inverse checkerboard, address, random, marching ones, marching zeros
- **Error clustering** - Groups nearby faults for easier diagnosis
- **Progress tracking** - Real-time progress with cancellation support

### Hardware Detection
- **PCIe generation & lanes** - Detects PCIe 3.0/4.0/5.0 and x1-x16 configurations
- **Thunderbolt/USB4** - Identifies external GPU connections
- **OCuLink** - Detects OCuLink eGPU setups
- **Integrated GPU detection** - Identifies iGPUs and shows system RAM bandwidth comparison
- **System RAM info** - Displays DDR4/DDR5/LPDDR5 speed, channels, and theoretical bandwidth

### Results & Export
- **Min/Avg/Max graphs** - Visual bandwidth comparison
- **Ranked comparison** - Compare results against PCIe/TB/USB4 standards
- **CSV export** - Export benchmark data for analysis
- **Clipboard copy** - Quick copy of VRAM scan results

## Screenshots

*Coming soon*

## Requirements

### Windows
- Windows 10/11 (64-bit)
- DirectX 12 compatible GPU
- Visual Studio 2019+ (for building)

### Linux
- Vulkan-capable GPU with drivers installed
- CMake 3.16+, g++, libglfw3-dev, libvulkan-dev
- X11 or Wayland display server

## Building

### Windows - Quick Build (Recommended)

1. Clone the repository:
   ```cmd
   git clone https://github.com/djanice1980/GPU-PCIe-Test.git
   cd GPU-PCIe-Test
   ```

2. Run the build script:
   ```cmd
   build_gui.bat
   ```
   
   The script automatically downloads ImGui and ImPlot dependencies.

3. Run the executable:
   ```cmd
   GPU-PCIe-Test.exe
   ```

### Windows - Manual Build

If you prefer manual compilation:

```cmd
cl /EHsc /O2 /DUNICODE /D_UNICODE main_gui.cpp imgui*.cpp implot*.cpp ^
   /link d3d12.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib ^
   /out:GPU-PCIe-Test.exe
```

### Linux Build

```bash
cd Linux
chmod +x build_linux.sh
./build_linux.sh
./build/gpu-pcie-test-vulkan
```

See [Linux/Linux_README.md](Linux/Linux_README.md) for detailed instructions and distro-specific package names.

## Usage

### Bandwidth Benchmark

1. Select your GPU from the dropdown
2. Adjust test parameters (buffer size, iterations) if desired
3. Click **Run Benchmark**
4. View results in the graph and log panels

### VRAM Scan

1. Select a discrete GPU (not available for integrated GPUs)
2. Pick a **Scan preset** (or use Custom for fine control):
   - **Quick** - 4 patterns, 50% coverage (~30 sec sanity check)
   - **Standard** - 8 patterns, 80% coverage (~2-3 min, default)
   - **Deep** - 8 patterns + refresh check + address-bus check, 90% coverage (~5 min)
   - **Thorough** - same as Deep with 10x re-reads, 95% coverage (~12-15 min)
3. Optionally fine-tune individual options:
   - **Patterns** - enable/disable any of 8 test patterns
   - **Refresh check** - re-read each chunk multiple times to catch retention errors
   - **Address-bus check** - randomize block read order to expose address-bus errors
   - **Coverage** - choose 50/80/90/95% of VRAM
4. Click **VRAM Scan**
5. Wait for completion - test covers multiple memory regions
6. Results window includes a bit-level **Error Breakdown** classifying any errors found (single-bit, multi-bit, address-bus, stuck-bit, refresh)

**Note:** VRAM scanning is a basic integrity test. For chip-level diagnosis, use vendor tools like NVIDIA MATS or AMD memory diagnostics. For extended overclock stress testing, consider [memtest_vulkan](https://github.com/GpuZelenograd/memtest_vulkan).

## How It Works

### Bandwidth Testing
Uses D3D12 copy operations between upload heaps (CPU-visible) and default heaps (GPU-local) with fence synchronization for accurate timing.

### VRAM Scanning
1. Allocates 512MB test buffers (upload, GPU, readback)
2. For each chunk (up to 25+ chunks for 80% coverage):
   - Writes test pattern to upload buffer
   - Copies to GPU buffer
   - Copies back to readback buffer
   - Compares against expected pattern
   - Releases and reallocates to potentially hit different physical VRAM
3. Reports errors by pattern type and memory region

### Interface Detection
Queries PCIe link status registers via DXGI and applies heuristics for:
- Thunderbolt (x4 links with specific bandwidth signatures)
- USB4 (similar to TB but different enumeration patterns)
- OCuLink (x4/x8 with PCIe-level bandwidth)

## Limitations

- **D3D12 abstraction** - Cannot directly address physical VRAM; relies on driver allocation patterns
- **No stress testing** - Tests static memory, not thermal/power stress conditions
- **D3D12 variant is Windows only** - The Vulkan variant runs on both Windows and Linux
- **No multi-GPU** - Tests one GPU at a time

## Version History

See [CHANGELOG.md](CHANGELOG.md) for detailed release notes.

## Contributing

Contributions welcome! Please feel free to submit issues and pull requests.

## License

MIT License - see [LICENSE](LICENSE) for details.

## Author

**David Janice**  
Email: djanice1980@gmail.com  
GitHub: [@djanice1980](https://github.com/djanice1980)

## Acknowledgments

- [Dear ImGui](https://github.com/ocornut/imgui) - Immediate mode GUI
- [ImPlot](https://github.com/epezent/implot) - Plotting library for ImGui
- Microsoft DirectX 12 documentation

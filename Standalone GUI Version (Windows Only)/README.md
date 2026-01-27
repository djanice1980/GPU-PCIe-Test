# GPU-PCIe-Test

A Windows GPU and PCIe bandwidth/latency benchmark tool with a modern GUI interface. Measures data transfer speeds between CPU and GPU to help identify PCIe lane configuration, potential bottlenecks, and external GPU connections.

![GPU-PCIe-Test Screenshot](screenshot.png)

## Features

- **Bandwidth Testing**: Measures CPU→GPU (upload) and GPU→CPU (download) transfer speeds
- **Bidirectional Testing**: Simultaneous upload/download bandwidth measurement
- **Latency Testing**: Measures transfer latency and command submission latency
- **Interface Detection**: Automatically identifies PCIe generation and lane count
- **eGPU Detection**: Detects external GPUs connected via Thunderbolt/USB4/OCuLink
- **Multi-GPU Support**: Test any installed GPU with automatic enumeration
- **VRAM-Aware Sizing**: Automatically caps test buffer sizes based on GPU memory
- **Average or Individual Runs**: Choose between averaged results or per-run data
- **Visual Comparison**: Compare your results against standard interface bandwidths
- **Export**: Save results to CSV for further analysis

## Supported Interface Standards

| Interface | Theoretical Bandwidth |
|-----------|----------------------|
| PCIe 3.0 x4 | 3.94 GB/s |
| PCIe 3.0 x8 | 7.88 GB/s |
| PCIe 3.0 x16 | 15.75 GB/s |
| PCIe 4.0 x4 | 7.88 GB/s |
| PCIe 4.0 x8 | 15.75 GB/s |
| PCIe 4.0 x16 | 31.51 GB/s |
| PCIe 5.0 x8 | 31.51 GB/s |
| PCIe 5.0 x16 | 63.02 GB/s |
| PCIe 6.0 x16 | 126.03 GB/s |
| OCuLink 1.0 | 3.94 GB/s |
| OCuLink 2.0 | 7.88 GB/s |
| Thunderbolt 3 | ~2.5 GB/s |
| Thunderbolt 4 | ~3.0 GB/s |
| Thunderbolt 5 | ~10 GB/s |
| USB4 40Gbps | 4.0 GB/s |
| USB4 80Gbps | 8.0 GB/s |

## Requirements

- Windows 10/11 (64-bit)
- DirectX 12 compatible GPU
- Visual Studio 2019 or later (for building)

## Building

### Prerequisites

1. Visual Studio 2019 or later with C++ desktop development workload
2. Windows SDK 10.0.19041.0 or later

### Build Steps

1. Clone the repository:
   ```
   git clone https://github.com/djanice1980/GPU-PCIe-Test.git
   cd GPU-PCIe-Test
   ```

2. Run the build script:
   ```
   build_gui.bat
   ```

   The script will:
   - Download ImGui and ImPlot if not present
   - Compile all source files
   - Create `GPU-PCIe-Test.exe`

### Manual Build

If you prefer to build manually:

```
cl /EHsc /O2 /DUNICODE /D_UNICODE main_gui.cpp imgui/imgui*.cpp imgui/implot*.cpp ^
   /Fe:GPU-PCIe-Test.exe /link d3d12.lib dxgi.lib d3dcompiler.lib user32.lib
```

## Usage

1. Run `GPU-PCIe-Test.exe`
2. Select your GPU from the dropdown (if multiple GPUs are installed)
3. Adjust test parameters as needed:
   - **Bandwidth Test Size**: Size of data buffer for bandwidth tests (16-max safe MB)
   - **Latency Test Size**: Size for latency tests (1-1024 bytes)
   - **Bandwidth Batches**: Number of measurement batches
   - **Copies per Batch**: Number of copy operations per batch
   - **Number of Runs**: How many complete test runs to perform
4. Enable/disable optional tests:
   - **Run Bidirectional Test**: Test simultaneous up/down transfers
   - **Run Latency Tests**: Include latency measurements
   - **Average Runs**: Combine runs into averaged results, or show individual runs
5. Click **Start Benchmark**
6. View results in the Results, Graphs, or Compare to Standards windows
7. Export to CSV if desired

### Quick Mode

Enable **Quick Mode** for faster benchmarks with reduced accuracy:
- 1 run
- 16 batches
- 500 latency iterations

## Interpreting Results

### Bandwidth

- Results close to theoretical maximums indicate optimal PCIe configuration
- Results below 70% of expected bandwidth may indicate:
  - Running at reduced lane count (x4 instead of x16)
  - PCIe generation mismatch
  - Motherboard slot limitations
  - External connection bottleneck (TB3/TB4/USB4)

### eGPU Detection

The tool automatically detects external GPUs based on bandwidth characteristics:
- Discrete GPUs with bandwidth < 5 GB/s are flagged as potential eGPUs
- Connection type is estimated (Thunderbolt 3/4/5, USB4, OCuLink)

### Average vs Individual Mode

- **Average Mode**: Best for getting stable, representative results
- **Individual Mode**: Best for identifying variance between runs, uses best (peak) values for interface comparison

## Changelog

### v2.5
- Added PCIe 6.0 x16 and Thunderbolt 5 to interface standards
- Results now accumulate across multiple benchmark runs
- Added "Clear Charts" and "Reset Settings" buttons
- Switching between Average/Individual modes now clears results
- Individual mode uses best (max) values for interface comparison
- Graphs show "Best" instead of "Max" label in individual mode
- Bandwidth slider now allows full safe size based on GPU VRAM
- Fixed various UI state issues

### v2.4
- Added Average Runs toggle for individual vs averaged results
- Improved graphs with Min/Avg/Max horizontal bars
- Enhanced Compare to Standards with sorted graphical comparison
- Results persist across multiple benchmark runs

### v2.3
- Added VRAM-aware buffer sizing
- Added eGPU auto-detection
- Enhanced error handling and timeout management
- Added GPU details display (type, VRAM, vendor ID)

### v2.2
- Improved interface detection accuracy
- Added OCuLink 1.0/2.0 standards
- Fixed Thunderbolt bandwidth estimates

### v2.0
- Complete GUI rewrite with Dear ImGui
- Added ImPlot-based graphs
- Multi-GPU support
- CSV export functionality

## License

MIT License - See LICENSE file for details.

## Author

David Janice  
Email: djanice1980@gmail.com  
GitHub: https://github.com/djanice1980/GPU-PCIe-Test

## Acknowledgments

- [Dear ImGui](https://github.com/ocornut/imgui) - Immediate mode GUI library
- [ImPlot](https://github.com/epezent/implot) - Plotting library for ImGui

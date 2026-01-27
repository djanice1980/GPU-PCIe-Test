# GPU-PCIe-Test v2.2 - GUI Edition

A graphical frontend for the GPU/PCIe bandwidth and latency benchmark tool.

![Screenshot placeholder]

## Features

- **Interactive GPU Selection** - Choose from detected GPUs with details
- **Real-time Progress** - Visual progress bars during benchmarks
- **Results Table** - Clear presentation of all test results
- **Bandwidth & Latency Graphs** - Bar charts via ImPlot
- **Interface Detection** - Automatic PCIe/Thunderbolt identification
- **CSV Export** - Save results for further analysis
- **Configurable Settings** - Adjust buffer sizes, iterations, runs

## Requirements

- Windows 10/11 (64-bit)
- DirectX 12 compatible GPU
- Visual Studio 2019 or later (for building)
- Internet connection (for initial dependency download)

## Building

1. Open a **Developer Command Prompt** for Visual Studio
2. Navigate to the `gui/` folder
3. Run:
   ```
   build_gui.bat
   ```

The build script will:
- Download Dear ImGui and ImPlot from GitHub
- Compile everything into a single executable
- Output: `GPU-PCIe-Test_GUI.exe`

## Usage

Simply run `GPU-PCIe-Test_GUI.exe`. The interface has four panels:

### Configuration Panel (Left)
- Select GPU from dropdown
- Adjust benchmark parameters:
  - Buffer size (64-1024 MB)
  - Number of batches
  - Copies per batch
  - Number of runs (for averaging)
- Toggle bidirectional and latency tests
- Start/Cancel buttons

### Progress Panel (Top)
- Shows current test name
- Progress bar for running tests
- Status indicator

### Results Panel (Center)
- Table with Min/Avg/Max for each test
- Detected interface type (PCIe version/lanes)
- Measured upload/download bandwidth
- Export to CSV button

### Graphs Panel (Right)
- Bar chart of bandwidth tests (GB/s)
- Bar chart of latency tests (μs)

## Tests Performed

| Test | Description |
|------|-------------|
| CPU→GPU Bandwidth | Upload speed to GPU memory |
| GPU→CPU Bandwidth | Download speed from GPU memory |
| Bidirectional | Simultaneous upload + download |
| Command Latency | Empty command submission overhead |
| CPU→GPU Latency | Small transfer round-trip (upload) |
| GPU→CPU Latency | Small transfer round-trip (download) |

## Dependencies

Downloaded automatically by build script:
- [Dear ImGui](https://github.com/ocornut/imgui) v1.90.1 - Immediate mode GUI
- [ImPlot](https://github.com/epezent/implot) v0.16 - Plotting extension

## Known Limitations

- D3D12 only (no Vulkan in GUI version currently)
- Window resize not fully implemented
- Multi-run averaging displays last run only in graphs

## Troubleshooting

**"Failed to initialize D3D12"**
- Ensure you have a DirectX 12 compatible GPU
- Update your graphics drivers

**"Visual Studio compiler not found"**
- Run from Developer Command Prompt, not regular CMD/PowerShell

**Download failures**
- Check internet connection
- Manually download files from GitHub if curl fails

## License

MIT License - See main repository.

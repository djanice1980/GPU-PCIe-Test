# GPU-PCIe-Test

A GPU performance benchmark tool for measuring PCIe and Thunderbolt bandwidth between CPU and GPU memory. Supports both Direct3D 12 (Windows) and Vulkan (cross-platform) APIs.

All based off the work of procrastineto
https://github.com/procrastineto/GPU-PCIe-Test

## Features

- **Bandwidth Testing** - Measures CPU→GPU and GPU→CPU transfer speeds with large buffers (default 256MB)
- **Latency Testing** - Measures round-trip latency for small transfers and command submission overhead
- **Interface Detection** - Automatically identifies your likely connection type (PCIe gen/lanes, Thunderbolt, OCuLink)
- **Dual API Support** - Choose between Direct3D 12, Vulkan, or run both for comparison
- **Multi-GPU Support** - Select which GPU to benchmark on systems with multiple graphics cards
- **CSV Logging** - Results are appended to a CSV file for tracking performance over time
- **Continuous Mode** - Run repeated tests to monitor thermal throttling or stability
- **Reference Charts** - Built-in PCIe/Thunderbolt bandwidth reference for comparison

## Use Cases

- Verify your GPU is running at the expected PCIe link speed (x16 vs x8, Gen 4 vs Gen 3)
- Test eGPU enclosure performance over Thunderbolt 3/4/5
- Compare OCuLink adapter performance
- Diagnose bandwidth bottlenecks
- Monitor for thermal throttling during sustained transfers

## Quick Start

1. Download the latest release (or build from source)
2. Run `GPU-PCIe-Test.exe`
3. Select your API (D3D12, Vulkan, or Both)
4. View results and interface detection

## Sample Output

```
==============================================
      GPU Performance Benchmark Tool
==============================================

Found 1 GPU: NVIDIA GeForce RTX 4090

Test 1 - CPU -> GPU 256MB Transfer Bandwidth
----------------------------------------------
Progress: 100%
Results:
  Min: 24.82 GB/s
  Avg: 25.14 GB/s
  Max: 25.31 GB/s
==============================================

==============================================================
Interface Detection
==============================================================

┌─ Likely Connection Type ─────────────────────
│
│  PCIe 4.0 x16
│  Modern GPU slot (AMD Ryzen 3000+, Intel 11th gen+)
│
│  Upload:   25.14 GB/s  (79.8% of PCIe 4.0 x16)
│  Download: 25.89 GB/s  (82.2% of PCIe 4.0 x16)
│
│  ✓ Performance is as expected for this interface
│
└───────────────────────────────────────────────
```

## Configuration Options

Press `C` at startup to access the configuration menu:

| Option | Description | Default |
|--------|-------------|---------|
| Large transfer size | Buffer size for bandwidth tests | 256 MB |
| Small transfer size | Buffer size for latency tests | 1 byte |
| Command latency iterations | Number of empty submit cycles | 100,000 |
| Transfer latency iterations | Number of small transfer cycles | 10,000 |
| Copies per batch | Transfers per timing measurement | 8 |
| Bandwidth test batches | Number of timing samples | 32 |
| Continuous mode | Repeat tests until ESC pressed | OFF |
| CSV logging | Save results to file | ON |
| CSV filename | Output file path | gpu_benchmark_results.csv |

Press `H` to view the PCIe/Thunderbolt bandwidth reference chart.

## Understanding Results

### Bandwidth (GB/s)

Typical real-world bandwidth is **70-90%** of theoretical maximum due to protocol overhead:

| Interface | Theoretical Max | Typical Real-World |
|-----------|-----------------|-------------------|
| PCIe 3.0 x16 | 15.75 GB/s | 11-14 GB/s |
| PCIe 4.0 x16 | 31.5 GB/s | 22-28 GB/s |
| PCIe 5.0 x16 | 63.0 GB/s | 44-57 GB/s |
| Thunderbolt 3/4 | 2.75 GB/s | 2.0-2.5 GB/s |
| Thunderbolt 5 | 6.0 GB/s | 4.2-5.4 GB/s |

### Latency (microseconds)

- **Command Latency** - Time to submit an empty command buffer and wait for completion. Tests driver/OS overhead.
- **Transfer Latency** - Time to copy a tiny buffer. Includes command latency plus DMA setup overhead.

Lower is better. Typical values: 2-10 μs for command latency, 5-20 μs for transfer latency.

## Troubleshooting

### Lower than expected bandwidth

- **GPU in reduced PCIe mode** - Check GPU-Z or HWiNFO for actual link speed. Some motherboards run x16 slots at x8 when multiple slots are populated.
- **Wrong PCIe slot** - Ensure GPU is in the primary x16 slot, not a secondary slot that may be wired as x4 or x8.
- **Power saving** - GPU may downclock PCIe link when idle. Run a game or GPU stress test first.
- **Thermal throttling** - Use continuous mode to check if bandwidth drops over time.
- **Driver issues** - Try updating GPU drivers.

### Vulkan benchmark won't run

- Ensure Vulkan SDK is installed and `VULKAN_SDK` environment variable is set
- Update GPU drivers (includes Vulkan runtime)
- Check that your GPU supports Vulkan 1.0+

### D3D12 benchmark won't run

- Requires Windows 10 or later
- Requires a GPU with D3D12 support (most GPUs from 2012+)

## CSV Output Format

Results are appended to `gpu_benchmark_results.csv`:

```csv
Timestamp,API,Test Name,Min,Avg,Max,99th Percentile,99.9th Percentile,Unit
2025-01-24 10:30:00,D3D12,Test 1 - CPU -> GPU 256MB Transfer Bandwidth,24.82,25.14,25.31,0,0,GB/s
2025-01-24 10:30:05,D3D12,Test 2 - GPU -> CPU 256MB Transfer Bandwidth,25.61,25.89,26.02,0,0,GB/s
```

## Building from Source

See [BUILDING.md](BUILDING.md) for detailed build instructions.

### Quick Build (Windows)

Requirements:
- Visual Studio 2019 or later (with C++ Desktop workload)
- Vulkan SDK (optional, for Vulkan build)

```cmd
# Open "Developer Command Prompt for VS"
cd GPU-PCIe-Test
build_final.bat
```

## License

MIT License - See [LICENSE](LICENSE) for details.

## Contributing

Contributions welcome! Please open an issue to discuss proposed changes before submitting a PR.

Ideas for future improvements:
- Linux support for D3D12 via vkd3d
- GPU memory bandwidth test (VRAM to VRAM)
- Compute shader bandwidth test
- GUI interface
- Historical result graphing

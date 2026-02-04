# GPU-PCIe-Test v2.0

A comprehensive GPU bandwidth and latency benchmarking tool for measuring PCIe, Thunderbolt, and other interconnect performance. Supports both Direct3D 12 and Vulkan APIs.

## Features

### New in v2.0
- **Multi-run Averaging**: Runs each test 3 times by default, reports averaged results with standard deviation for more reliable measurements
- **Bidirectional Bandwidth Test**: Measures simultaneous upload/download throughput for real-world workload simulation (ML inference, video streaming)
- **GPU/PCIe Detection**: Reports detected GPU name, vendor, VRAM, driver version, and identifies integrated vs discrete GPUs
- **Improved Code Structure**: constexpr constants, comprehensive comments explaining Vulkan memory flags and D3D12 heap types

### Core Features
- Direct3D 12 and Vulkan API support
- CPU-to-GPU and GPU-to-CPU bandwidth tests (256 MB default)
- Command submission latency test
- Small transfer latency tests (1 byte)
- Automatic PCIe/Thunderbolt interface detection
- CSV logging with timestamps
- Configurable test parameters

## Building

### Prerequisites
- Visual Studio 2019 or later with C++ Desktop workload
- Vulkan SDK (optional, for Vulkan support) - https://vulkan.lunarg.com/

### Build Steps

1. Open **Developer Command Prompt for VS 2019/2022**
2. Navigate to the source directory
3. Run `build.bat`

```cmd
cd C:\path\to\GPU-PCIe-Test
build.bat
```

Output: `GPU-PCIe-Test.exe`

## Usage

```cmd
GPU-PCIe-Test.exe
```

### Menu Options
1. **API Selection**: Choose D3D12, Vulkan, or both
2. **Configuration** (press 'C'):
   - Buffer sizes for bandwidth and latency tests
   - Iteration counts
   - Number of runs (1-10 for multi-run averaging)
   - Enable/disable bidirectional test
   - Enable/disable detailed GPU info
   - Toggle logging of individual runs vs averages only

### Configuration Menu
```
  1. Bandwidth buffer size: 256 MB
  2. Latency buffer size:   1 B
  3. Command iterations:    100000
  4. Transfer iterations:   10000
  5. Copies per batch:      8
  6. Bandwidth batches:     32
  7. Number of runs:        3 (multi-run averaging)
  8. Log all runs:          No (averages only)
  9. Bidirectional test:    Enabled
  A. Detailed GPU info:     Enabled
  C. Continuous mode:       Disabled
  H. Show reference chart
  0. Start benchmark
```

## Test Descriptions

| Test | Description | Unit |
|------|-------------|------|
| CPU-to-GPU Bandwidth | Large buffer upload throughput | GB/s |
| GPU-to-CPU Bandwidth | Large buffer download throughput | GB/s |
| Bidirectional | Simultaneous upload + download | GB/s (combined) |
| Command Latency | Empty command submission overhead | us |
| CPU-to-GPU Latency | Small transfer round-trip time | us |
| GPU-to-CPU Latency | Small transfer round-trip time | us |

## Output

### Console
- Per-run results (Min, Avg, Max, P99, P99.9)
- Aggregated results with standard deviation
- Interface detection (PCIe generation and lane width)

### CSV File
Results are saved to `gpu_benchmark_results.csv`:
```
Timestamp,API,Test Name,Run,Min,Avg,Max,P99,P99.9,StdDev,Unit
2026-01-25 12:00:00,D3D12,CPU->GPU 256 MB Bandwidth,AVG,45.2,48.5,52.1,51.8,52.0,1.2,GB/s
```

## Interface Detection

The tool automatically identifies your connection type:

| Interface | Typical Bandwidth |
|-----------|------------------|
| PCIe 3.0 x16 | 12-15 GB/s |
| PCIe 4.0 x16 | 25-30 GB/s |
| PCIe 5.0 x16 | 50-60 GB/s |
| Thunderbolt 3/4 | 2.0-2.5 GB/s |
| Thunderbolt 5 | 5-6 GB/s |

**Note**: Integrated GPUs (APUs) may show >100% efficiency because memory transfers don't traverse a real PCIe bus.

## Interpreting Results

### Bandwidth
- **60-95% of theoretical**: Normal, expected overhead
- **>95%**: Exceptional efficiency
- **<60%**: Possible bottleneck or throttling

### Latency
- **D3D12 command latency**: ~1-2 us typical
- **Vulkan command latency**: ~15-25 us typical (more driver overhead)
- **Transfer latency**: ~15-50 us typical

### Standard Deviation
- Low StdDev (<5% of avg): Consistent results
- High StdDev (>10% of avg): System noise, consider closing background apps

## Troubleshooting

### "No DirectX 12 capable GPU found"
- Ensure you have a DirectX 12 compatible GPU
- Update your graphics drivers

### "No Vulkan-capable GPU found"
- Install the Vulkan Runtime from your GPU vendor
- Install Vulkan SDK for development

### Inconsistent Results
- Close background applications
- Disable power saving modes
- Increase number of runs (option 7)
- Let the system warm up before testing

## License

MIT License - See LICENSE file

## Author

djanice1980 - https://github.com/djanice1980/GPU-PCIe-Test

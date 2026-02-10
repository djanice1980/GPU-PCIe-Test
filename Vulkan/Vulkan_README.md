# GPU-PCIe-Test v3.0 — Vulkan Version

Vulkan implementation of the GPU/PCIe bandwidth and latency benchmark. Uses dedicated transfer queues for direct DMA engine access, providing the most accurate measurement of raw hardware transfer capability.

## Building

### Prerequisites
- **Vulkan SDK** — [https://vulkan.lunarg.com/](https://vulkan.lunarg.com/)
- **Visual Studio 2022** (or Build Tools with MSVC v143+)
- **Windows 10/11**

### Build
```
build_vulkan.bat
```
The build script automatically downloads ImGui and ImPlot from GitHub on first run. Output: `GPU-PCIe-Test-Vulkan.exe` in the build directory.

## How It Differs From the D3D12 Version

Both versions implement the same benchmark methodology and produce comparable results. The key difference is the queue/engine path:

| | D3D12 | Vulkan |
|---|---|---|
| **Queue Type** | DIRECT (driver routes copies to DMA) | Dedicated transfer family (raw DMA) |
| **Bidirectional** | 2 × DIRECT queues | 2 × transfer queues |
| **Timestamps** | EndQuery(TIMESTAMP) | vkCmdWriteTimestamp |
| **Buffer States** | COMMON + implicit promotion | No barriers on transfer queue |

### When Results Differ

**Discrete GPUs (eGPU, PCIe desktop):** Unidirectional bandwidth is identical (±3%) — the PCIe link is the bottleneck, not the command path. Bidirectional may differ because D3D12's DIRECT queue can optimize TLP scheduling internally.

**Integrated GPUs (APUs):** Vulkan's dedicated SDMA engine bypasses the graphics command processor for significantly higher sustained throughput (+31–63%), at the cost of higher per-command latency. This is a genuine hardware path difference.

## Benchmark Methodology

### Bandwidth
- **Download (GPU→CPU):** GPU timestamps bracketing copy commands on the transfer queue
- **Upload (CPU→GPU), discrete:** CPU round-trip timing. Measures upload + readback, subtracts previously measured download time. Required because ReBAR can cause GPU timestamps to report completion before data reaches VRAM.
- **Upload (CPU→GPU), integrated:** GPU timestamps (same as download). No ReBAR issue — CPU and GPU share the same physical memory.

### Bidirectional
- Dual transfer queues submitted simultaneously (queue 1: upload, queue 2: download)
- Wall-clock timing over both fences
- Falls back to single-queue interleaved if only one transfer queue available

### Latency
- GPU timestamps per individual small copy (`BOTTOM_OF_PIPE` stage for serialization)
- Command latency: back-to-back timestamp pairs with no work between them

### Transfer Queue Selection
Priority order:
1. Dedicated transfer family (`VK_QUEUE_TRANSFER_BIT` without `VK_QUEUE_GRAPHICS_BIT`)
2. Graphics+transfer family (fallback)
3. Any family with transfer support (last resort)

On NVIDIA: family 1 (2 queues, dedicated DMA engines)
On AMD: family 1 (8 queues on RDNA4, SDMA engines)

## Features

Full feature parity with the D3D12 version:
- Upload/download/bidirectional bandwidth tests
- Transfer and command latency measurements
- VRAM integrity scanning (multiple test patterns, error clustering)
- eGPU auto-detection (Thunderbolt / USB4 / USB via device tree)
- Integrated GPU detection
- PCIe link detection via SetupAPI
- System RAM detection via WMI (speed, channels, type)
- Real-time progress visualization
- Min/Avg/Max graphs with standard comparisons
- CSV export
- VRAM-aware buffer sizing

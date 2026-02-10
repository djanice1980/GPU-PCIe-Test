# Changelog — GPU-PCIe-Test

## v3.0.1 — Unified Benchmark Methodology (February 2025)

Cross-API benchmark alignment update. D3D12 and Vulkan versions now use
comparable measurement paths, enabling side-by-side validation on the same
hardware.

### D3D12 (`main_gui.cpp`)

**Dual-Queue Bidirectional**
- Second DIRECT queue for simultaneous upload + download submission
- `WaitForMultipleObjects` on both fences; falls back to single-queue interleaved
- Warm-up pass matches actual test queue topology

**Buffer State Cleanup**
- DEFAULT heap buffers created in `D3D12_RESOURCE_STATE_COMMON`
- Implicit promotion/decay between command list executions
- Upload round-trip retains explicit `COPY_DEST → COPY_SOURCE` barrier (required
  for write→read within a single DIRECT queue command list)
- VRAM test: removed unnecessary barriers between write/read phases (separate
  command lists with fence wait handle the transition)

**Methodology Logging**
- `[INFO]` lines: queue type, dual-queue availability
- `[METHOD]` line: exact timing approach per test type
- Unified header documentation with cross-API equivalence table

**Queue Selection: DIRECT, Not COPY**
- COPY queues hang on NVIDIA over Thunderbolt 4 (confirmed on RTX 5070 Ti)
- DIRECT queue driver internally routes copies to DMA engines
- Unidirectional bandwidth matches Vulkan transfer queues within ±3%

### Vulkan (`Vulkan/main_gui_vulkan.cpp`) — New

Complete Vulkan implementation with full feature parity.

**Transfer Queue Architecture**
- Dedicated transfer queue family (bypasses graphics command processor)
- Dual DMA queues for bidirectional overlap
- Queue selection priority: dedicated transfer → graphics+transfer → any transfer

**Timestamp Methodology**
- `TOP_OF_PIPE` for start timestamps (first command in fresh buffer)
- `BOTTOM_OF_PIPE` for end timestamps and latency serialization
- GPU timestamps for download bandwidth on all GPUs
- CPU round-trip for upload on discrete GPUs (ReBAR-safe)
- GPU timestamps for upload on integrated GPUs (shared memory)

**Build**
- `build_vulkan.bat` auto-downloads ImGui and ImPlot from GitHub
- Requires Vulkan SDK installed

### Cross-API Validation

**RTX 5070 Ti (eGPU over Thunderbolt 4, PCIe 4.0 x4)**

| Test | D3D12 | Vulkan | Delta |
|------|-------|--------|-------|
| Download | 3.53 GB/s | 3.54 GB/s | +0.3% |
| Upload | 3.49 GB/s | 3.58 GB/s | +2.6% |
| Bidirectional | 6.98 GB/s | 5.65 GB/s | D3D12 driver TLP optimization |
| Transfer Latency | 1.03 μs | 0.50 μs | SDMA bypasses GFX CP |
| Command Latency | 0.22 μs | 0.22 μs | Identical |

**AMD Radeon 8060S (Integrated GPU, shared LPDDR5)**

| Test | D3D12 | Vulkan | Delta |
|------|-------|--------|-------|
| Download | 73.7 GB/s | 96.8 GB/s | SDMA direct memory path |
| Upload | 56.0 GB/s | 91.3 GB/s | Same |
| Bidirectional | 63.9 GB/s | 97.3 GB/s | Same |
| Transfer Latency | 2.3 μs | 5.4 μs | GFX CP faster dispatch |
| Command Latency | 0.17 μs | 0.78 μs | Same |

Unidirectional bandwidth converges on discrete GPUs (PCIe is the bottleneck).
On integrated GPUs, Vulkan's SDMA engine provides +31–63% higher throughput by
bypassing the graphics command processor, at the cost of higher per-command
latency.

---

## v3.0 — Initial GUI Release

- Dear ImGui + D3D12 graphical frontend
- Upload/download/bidirectional bandwidth tests
- Latency measurements (transfer + command)
- VRAM integrity scanning with error clustering
- eGPU auto-detection (Thunderbolt/USB4/USB via device tree)
- Integrated GPU detection (no fake PCIe reporting)
- PCIe link detection via SetupAPI
- Round-trip upload measurement (ReBAR-accurate)
- System RAM detection via WMI
- CSV export, min/avg/max graphs, standard comparisons

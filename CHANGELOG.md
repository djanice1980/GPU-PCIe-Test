# Changelog

All notable changes to GPU-PCIe-Test will be documented in this file.

## [3.0.6] - 2026-07-21

### Fixed
- **DDR4/earlier RAM speed reported at 2x** - System RAM speed for DDR4 and
  earlier was doubled on the assumption that WMI (`Win32_PhysicalMemory.Speed`)
  and `dmidecode` report the I/O clock in MHz. In practice both report the
  memory data rate directly in MT/s (e.g. DDR4-3200 -> 3200), so the doubling
  inflated DDR4/earlier speeds to 2x, doubled the theoretical-bandwidth baseline
  for the iGPU comparison, and could trigger a spurious "XMP/EXPO may not be
  enabled" warning. Now no doubling is applied for any memory type. Affected all
  three variants (DDR5+ was already correct).

## [3.0.5] - 2026-07-21

### Fixed
- **Data race on status strings (UI crash)** - `currentTest` and
  `vramTestCurrentPattern` were written by the benchmark/scan worker threads and
  read unsynchronized by the UI thread (`ImGui::Text("%s", ...c_str())`). A read
  racing a reassignment (which frees the old buffer) could hand a dangling
  pointer to `%s`, causing garbage output or a crash. All access now routes
  through mutex-guarded setter/getter helpers. Affected all three variants.
- **Warm-up / latency fence waits ignored Cancel and could hang forever** - The
  bidirectional warm-up (all variants) and the memory-latency chain upload
  (Vulkan/Linux) waited on GPU fences with an unbounded timeout (`INFINITE` /
  `UINT64_MAX`), so a GPU stall froze the worker thread and the Cancel button did
  nothing. These now poll on the same bounded timeout the measured loops use and
  bail on abort, matching existing measured-loop behavior. (Render-path frame
  fences are intentionally left unbounded.)

## [3.0.4] - 2026-07-21

### Fixed
- **GPU-verify silently under-tested VRAM (false PASS)** - The GPU compute-shader
  verification dispatched one thread group per 256 dwords, producing 131K-524K
  groups for 128-512MB chunks. Both D3D12 (`D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION`)
  and Vulkan (`maxComputeWorkGroupCount[0]`, guaranteed minimum) cap a dispatch
  dimension at 65535. Over-limit groups are dropped, so only the first ~64MB of
  each chunk was actually compared and the remainder reported error-free. Fixed
  by splitting each verify into sub-dispatches that stay within the 65535-group
  limit, using a per-sub-dispatch `baseIndex` (dword offset) push/root constant
  so every dword is covered. Affected all three variants (D3D12, Vulkan Windows,
  Vulkan Linux). `vram_verify.comp` recompiled; embedded SPIR-V regenerated.
- **GPU-verify chunk buffer bound without storage usage flag** - The chunk buffer
  was bound as a UAV (D3D12) / storage buffer (Vulkan) by the verify shader but
  created without `ALLOW_UNORDERED_ACCESS` / `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`,
  which is undefined behavior. Now requested only when GPU-verify is active, so
  bandwidth-test buffers keep their original transfer-only usage.

## [3.0.3] - 2025-02-24

### Added
- **Linux Vulkan port** - Full Linux version using Vulkan + GLFW in `Linux/` directory
  - CMake build system with FetchContent for ImGui/ImPlot (pinned versions)
  - PCIe link detection via sysfs (`/sys/bus/pci/devices/`)
  - Thunderbolt/USB4/eGPU detection via sysfs thunderbolt subsystem
  - System RAM detection via `/proc/meminfo` + `dmidecode`
  - GLFW windowing (supports X11 and Wayland)
  - Convenience build script (`build_linux.sh`) with dependency checking
  - Full feature parity with Windows Vulkan variant

## [3.0.2] - 2025-02-18

### Fixed
- **build_gui.bat missing linker libraries** - Added setupapi.lib, cfgmgr32.lib, wbemuuid.lib, ole32.lib, oleaut32.lib (was failing to link)
- **build_gui.bat version banner** - Updated from v2.0 to v3.0

### Changed
- **`/W4 /WX` clean compilation** - Build script now enforces warning-free builds
- **Magic numbers extracted to Constants namespace** - MAX_LOG_LINES, VRAM_CHUNK_PREFERRED, VRAM_CHUNK_MINIMUM, MAX_BANDWIDTH_BUFFER, EGPU_MAX_TREE_DEPTH, BENCHMARK_SLEEP_US
- **Documentation updates** - Added portability notes, admin privilege note, updated directory structure

## [3.0] - 2025-02-04

### Added
- **Multi-chunk VRAM scanning** - Now tests up to 80-90% of VRAM by cycling through multiple 512MB allocations
- **Real coverage tracking** - Reports actual bytes tested and percentage of total VRAM
- **Per-chunk progress** - Shows which chunk is being tested (e.g., "Chunk 15/25")
- **Fresh allocation per chunk** - Deallocates and reallocates buffers between chunks to potentially hit different physical VRAM regions
- **About dialog** - Author info and GitHub URL

### Fixed
- **Random pattern seed bug** - Was generating different random data on write vs verify, causing millions of false errors
- **Marching pattern buffer mismatch** - Was reading past allocated buffer when last chunk was smaller than chunk size
- **Cancelled test stale results** - Now clears previous results when starting new scan
- **Button text cutoff** - Changed "Copy to Clipboard" to "Copy" to fit button width

### Changed
- Full scan reduced from 95% to 90% for better stability
- Marching iterations reduced from 8 to 4 per chunk (still thorough with multi-chunk)
- Improved cancellation responsiveness during allocation phase

## [2.9] - 2025-02-03

### Added
- **VRAM integrity scanning** - 8 test patterns (zeros, ones, checkerboard, inverse, address, random, marching ones/zeros)
- **Error clustering** - Groups nearby memory faults for easier diagnosis
- **Progress bar** - Real-time VRAM scan progress with cancellation support
- **Results window** - Displays pattern results and error regions
- **System RAM detection** - Shows DDR4/DDR5/LPDDR5 with speed, channels, and theoretical bandwidth
- **Full Scan checkbox** - Option to test more VRAM (with stability warning)

### Fixed
- **Fence timeout handling** - Proper handling for VRAM tests exempt from global timeout
- **DDR5/LPDDR5 MT/s calculation** - WMI reports MT/s directly, not MHz
- **WMI COM cleanup** - Proper tracking of COM initialization to avoid double-uninitialize
- **Unicode consistency** - WideToUtf8 helper for proper string conversion

## [2.8] - 2025-02-02

### Added
- **USB4/Thunderbolt detection** - Identifies eGPU connections
- **OCuLink detection** - Recognizes OCuLink eGPU setups
- **AMD platform support** - Improved detection for AMD GPUs
- **Integrated GPU detection** - Identifies iGPUs with system RAM bandwidth comparison
- **Round-trip bandwidth method** - Fixed PCIe asymmetry issues

### Changed
- Validated against NVIDIA nvbandwidth for accuracy
- Improved interface type heuristics

## [2.0] - 2025-01-xx

### Added
- **GUI Edition** - Full graphical interface using Dear ImGui
- **Real-time graphs** - Min/Avg/Max bandwidth visualization with ImPlot
- **Multi-GPU support** - Dropdown to select which GPU to test
- **Configurable parameters** - Buffer size, iterations, individual vs average recording
- **Ranked comparison** - Compare results against PCIe/TB/USB4 standards
- **CSV export** - Export benchmark results
- **Log panel** - Scrollable log with copy functionality

### Changed
- Complete rewrite from console application to GUI
- DirectX 12 rendering for the interface itself

## [1.0] - 2025-01-xx

### Initial Release
- Console-based bandwidth benchmark
- Upload/Download/Bidirectional tests
- Basic latency measurement
- PCIe generation detection

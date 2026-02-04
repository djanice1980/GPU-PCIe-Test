# Changelog

All notable changes to GPU-PCIe-Test will be documented in this file.

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

# Changelog

All notable changes to GPU-PCIe-Test will be documented in this file.

## [3.4.2] - unreleased

### Fixed
- **CSV export went to the process working directory with a fixed name**, so a
  launch from a menu or AppImage put `gpu_benchmark_results.csv` somewhere
  unobvious and every export overwrote the last. "Export to CSV" now opens the
  native Save As dialog (Windows common dialog; kdialog or zenity on Linux),
  starts in Documents with a timestamped file name, and the log shows the full
  path. Without kdialog/zenity the file goes to Documents (or `$HOME`) and the
  path is logged.

## [3.4.1] - 2026-09-05

### Fixed
- **eGPU hosts: opening the tool corrupted the eGPU's channel (NVRM Xid 32)
  about five seconds after launch, before any benchmark ran.** Two causes,
  both fixed:
  - The Linux AppImage bundled the distro's GLFW 3.3.6, built X11-only, so on
    a Wayland desktop the window was an XWayland client and every frame went
    through the compositor's cross-GPU buffer sharing. The AppImage now bundles
    GLFW 3.4 built from source with both the Wayland and X11 backends; it uses
    native Wayland when `WAYLAND_DISPLAY` is set and X11 otherwise.
  - The Vulkan variants rendered the UI on the first *discrete* GPU, i.e. the
    eGPU itself, so the UI's frames crossed the Thunderbolt/USB4 tunnel on the
    way to the display. The UI now renders on the integrated GPU whenever one
    can present (falling back to a discrete GPU, then anything presentable).
    The benchmark device is unaffected: it is still selected by the user and
    lives on its own `VkDevice`. The chosen UI device is written to the log.

## [3.4.0] - 2026-09-04

### Fixed
- **Fence timeout was treated as a successful batch** (all variants) - a single
  8 s fence timeout returned from `WaitForBenchFenceEx` while the GPU work was
  still in flight. The CPU-timed bandwidth loops recorded the timeout as a real
  sample, and the next batch reset the command buffer / allocator underneath a
  live submission and re-submitted a fence still in use. Timeouts are now
  retried on the *same* submission (up to `MAX_FENCE_RETRIES`, ~24 s of grace
  for a slow GPU) and any non-Success result aborts the benchmark. The
  dual-queue bidirectional loop no longer resets fences that never signaled,
  and the latency tests no longer block indefinitely in
  `vkGetQueryPoolResults` after a timeout.
- **Rated RAM latency was 1000x too high** whenever the memory speed was not an
  exact table entry (DDR5-6200 showed ~12900 ns instead of 12.9 ns).
- **Bidirectional result was mislabeled** after the v3.0.7 halved-buffer
  fallback - the results table showed the requested size, not the size that
  was actually measured.
- **Benchmark summary window could walk `results` while the worker inserted
  into it** - results are now published before the window is told to open,
  and the window holds the results mutex for its whole body.
- **Vulkan memory-latency test crashed instead of skipping on allocation
  failure** - buffer, memory-type, allocation, bind and map results are now
  checked (matters on eGPUs with tight OS memory budgets).
- **Render-finished semaphores are now per swapchain image** (Vulkan
  variants) - a per-frame semaphore could be re-signaled while an earlier
  present still referenced it (VUID-vkQueueSubmit-pSignalSemaphores-00067).
- **Linux validation builds did nothing** - `-DENABLE_VULKAN_VALIDATION=ON`
  defined the macro but the Linux source never enabled
  `VK_LAYER_KHRONOS_validation`; it now mirrors the Windows Vulkan variant.
- **Soldered LPDDR reported as single-channel RAM** (Strix Halo and similar
  APUs) - channels were guessed from the SMBIOS device count, so firmware
  that describes a 256-bit LPDDR5X bus as one device produced a
  "single-channel" warning and a theoretical bandwidth 4x too low, which
  inflated the iGPU percent-of-RAM figures. Soldered memory now uses the
  SMBIOS per-device data widths when present, is never warned about, and
  the iGPU comparison raises the channel estimate when the measured
  bandwidth proves it too low (logged as inferred).
- **Linux build failed with CMake < 3.24** - `DOWNLOAD_EXTRACT_TIMESTAMP`
  is now only passed to CMake versions that know it (the stated minimum
  is 3.16).
- **Linux build broke with GCC 16** - a dead `successfulRuns` counter tripped
  `-Werror=unused-but-set-variable`.
- **`build_gui.bat` was stored LF** despite the v3.0.7 CRLF fix (the commit
  was normalized by `core.autocrlf=input`). `.gitattributes` now pins `*.bat`
  to CRLF on checkout so it cannot regress.

### Added
- GitHub Actions release workflow: the Windows D3D12 and Vulkan executables
  and a Linux AppImage are built and attached to every `v*` tag.
- `packaging/arch/PKGBUILD` for Arch Linux / CachyOS (`makepkg -si`), plus a
  desktop entry and icon installed by CMake (used by the AppImage too).

### Changed
- Version scheme unified: tag, changelog and in-app version now all read
  3.4.0 (earlier commits used 3.0.x while tags were 3.x.0).

## [3.0.7] - 2026-07-21

### Fixed
- **Thunderbolt 5 / USB4 80Gbps eGPUs misclassified** - the "closest standard"
  comparison picked the nearest interface by absolute difference, producing
  contradictions like "5.35 GB/s (134% of USB4 40Gbps)" - impossible, since a
  40 Gbps link's PCIe tunnel caps at 32 Gbps (~3.5-4 GB/s real). eGPUs
  confirmed behind a TB/USB4 tunnel now compare only against tunneling tiers
  and pick the smallest tier the measurement plausibly fits under; a tier the
  measurement exceeds by >15% is treated as disproven. Internal GPUs no longer
  match against TB/USB4 tiers either.
- **TB/USB4 tier table corrected for PCIe tunnel limits** - Thunderbolt 5 and
  USB4 80Gbps merged into one 80 Gbps-class entry (6.5 GB/s achievable / 8.0
  theoretical, reflecting the 64 Gbps PCIe tunnel cap - the old 10/12 figures
  described the link rate, not tunneled PCIe); Thunderbolt 4 and USB4 40Gbps
  merged at 3.5/4.0 (32 Gbps tunnel). Bandwidth-heuristic eGPU fallback gained
  a TB5 tier.
- **eGPU connection labels now state the measured link class** - when the
  device tree confirms a TB/USB4 tunnel but measured bandwidth disproves a
  40 Gbps link, the connection line is annotated "80 Gbps-class (TB5/USB4v2,
  by measured bandwidth)" (Windows exposes no USB4 link-rate query).
- **Bidirectional test no longer skipped on eGPU allocation failure** - the
  test needs 4 buffers at once (largest footprint of any test); on TB/USB4
  eGPUs the OS grants conservative memory budgets, so 256 MB could fail with
  E_OUTOFMEMORY despite 16 GB of free VRAM. Allocation now retries with
  progressively halved buffers (min 32 MB) with a clear warning; bandwidth
  math uses the actual size, so results remain valid.
- **HRESULTs logged as nonsense** - errors printed signed decimal with an "0x"
  prefix ("0x-2147024882"); now proper hex plus a friendly name
  ("0x8007000E (E_OUTOFMEMORY)"), and buffer-allocation failures name the heap
  type. D3D12 also logs OS video-memory budgets (local + non-local) at
  benchmark start and on allocation failure - on eGPUs the budget, not
  physical VRAM, is usually the real ceiling.
- **Bandwidth was reported in GiB/s but compared against decimal-GB standards**
  - all bandwidth math now uses decimal GB (1e9), matching how PCIe/TB/USB4
  standards are specified. Reported numbers rise ~7.4% versus previous
  versions; percent-of-standard figures are now accurate.
- **Command-latency timestamps could underflow into bogus samples** - equal or
  reordered GPU timestamps produced huge unsigned deltas that poisoned
  min/avg/max. All timestamp deltas now require tEnd > tStart (D3D12 command
  latency, D3D12 bandwidth GPU-timestamp path, Vulkan command latency).
- **Vulkan timestamps used undefined high bits** - per spec only
  `timestampValidBits` of each query result are meaningful; both Vulkan
  variants now mask timestamps to the queue family's valid bits before
  computing deltas (separate mask for the memory-latency compute family).
- **Hung worker thread at exit caused teardown use-after-free** - if a worker
  survived the 5s cancellation grace (GPU stall), it was detached and the
  device/ImGui were destroyed underneath it. The app now exits the process
  immediately in that case instead of tearing down state a live thread uses.
- **Swapchain OUT_OF_DATE stalled rendering without a resize event** - DPI or
  monitor changes can invalidate the swapchain with no WM_SIZE/framebuffer
  callback; acquire/present now schedule an explicit recreation at the current
  window size (both Vulkan variants).

### Security
- **Dependency downloads are now SHA-256 verified** - `build_gui.bat` pins a
  hash for every ImGui/ImPlot source file (with `curl -f --retry 3` and
  automatic re-download on mismatch), `Vulkan/build_vulkan.bat` verifies both
  release archives, and `Linux/CMakeLists.txt` uses `URL_HASH`. A moved tag,
  tampered mirror, or truncated download now fails the build loudly instead of
  compiling unexpected code.

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

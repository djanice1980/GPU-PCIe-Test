// ============================================================================
// GPU-PCIe-Test v3.4 - GUI Edition (Vulkan - Linux)
// Dear ImGui + Vulkan + GLFW Frontend
// ============================================================================
// Graphical frontend for the GPU/PCIe benchmark tool.
//
// UNIFIED BENCHMARK METHODOLOGY
// ─────────────────────────────────────────────────────────────────────────────
// This methodology is designed to be API-agnostic. The same approach applies
// to both the Vulkan and D3D12 versions, ensuring comparable results that can
// be cross-validated. The key principle: always test through the GPU's DMA
// copy engines directly, never through the graphics command processor.
//
// Queue Selection (the foundation of consistent results):
//   Vulkan  → Dedicated transfer queue family (VK_QUEUE_TRANSFER_BIT, no
//             VK_QUEUE_GRAPHICS_BIT). Falls back to graphics+transfer if no
//             dedicated transfer family has timestamp support.
//   D3D12   → D3D12_COMMAND_LIST_TYPE_COPY queue (equivalent to above).
//             Falls back to DIRECT queue.
//   Why:      Both map directly to the GPU's DMA copy engine hardware.
//             Graphics queues add scheduling overhead and may auto-route
//             copies differently per vendor, making results non-comparable.
//
// Bandwidth Tests:
//   Download (GPU→CPU):
//     GPU timestamps bracketing the copy commands on the copy/transfer queue.
//     Accurate on all GPU types since the DMA engine controls the transfer.
//
//   Upload (CPU→GPU) - Discrete GPUs:
//     CPU round-trip timing. Records: upload + barrier + download from same
//     buffer. Uses previously measured download speed to subtract download
//     time: upload_speed = data_size / (round_trip_time - download_time).
//     Required because ReBAR allows GPU timestamps to complete before data
//     actually reaches VRAM over the PCIe bus.
//
//   Upload (CPU→GPU) - Integrated GPUs:
//     GPU timestamps, same as download. No ReBAR issue because both "CPU"
//     and "GPU" memory are the same physical RAM with no bus transfer.
//
// Bidirectional Test:
//   Dual copy/transfer queues submitted simultaneously:
//     Queue 1: upload copies (CPU→GPU)
//     Queue 2: download copies (GPU→CPU)
//   Wait for both fences, measure total wall-clock time.
//   Total bandwidth = (upload_bytes + download_bytes) / elapsed_time.
//   Falls back to single-queue interleaved copies if only 1 queue available.
//   D3D12 equivalent: 2 × COPY queues with simultaneous ExecuteCommandLists.
//
// Latency Tests:
//   GPU timestamps per individual small copy on the copy/transfer queue.
//   Both start and end timestamps use the equivalent of "after all prior work
//   completes" (Vulkan: VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, D3D12:
//   EndQuery with TIMESTAMP type) to prevent overlap between measurements.
//
// Command Latency:
//   Back-to-back timestamp pairs with no work between them.
//   Measures minimum per-command dispatch overhead of the copy/transfer queue.
//
// CROSS-API EQUIVALENCES
// ─────────────────────────────────────────────────────────────────────────────
//   Vulkan                           D3D12
//   ──────────────────────────────── ──────────────────────────────────────
//   VkDevice (bench)                 ID3D12Device (bench)
//   Transfer queue family            D3D12_COMMAND_LIST_TYPE_COPY queue
//   vkCmdCopyBuffer                  CopyResource / CopyBufferRegion
//   vkCmdWriteTimestamp              EndQuery(TIMESTAMP)
//   VkQueryPool (TIMESTAMP)          ID3D12QueryHeap (TIMESTAMP)
//   vkGetQueryPoolResults            ResolveQueryData + Map readback
//   benchTimestampPeriod (ns/tick)   GetTimestampFrequency (ticks/sec)
//   VK_MEMORY_PROPERTY_HOST_VISIBLE  D3D12_HEAP_TYPE_UPLOAD
//   VK_MEMORY_PROPERTY_DEVICE_LOCAL  D3D12_HEAP_TYPE_DEFAULT
//   HOST_VISIBLE + HOST_CACHED       D3D12_HEAP_TYPE_READBACK
//   vkQueueSubmit + vkWaitForFences  ExecuteCommandLists + Signal/Wait fence
//   Dual transfer queues (bidir)     2 × COPY queues (bidir)
//
// D3D12 QUEUE DIVERGENCE NOTES
// ─────────────────────────────────────────────────────────────────────────────
// D3D12 uses DIRECT queues instead of COPY queues. COPY queues were tested but
// proved unreliable on some driver/hardware combinations (notably eGPU over
// Thunderbolt — produced driver crashes and incorrect results). The DIRECT
// queue driver internally routes CopyResource calls to DMA copy engines, but
// adds driver-level scheduling/optimization. This can inflate bidirectional
// numbers slightly compared to Vulkan's raw transfer queues.
//
// This is a known, accepted difference between the two API variants:
//   - Vulkan: raw DMA engine access via dedicated transfer queues
//   - D3D12:  driver-mediated DMA access via DIRECT queues
//   - Both measure the same underlying hardware path, but D3D12's driver
//     layer may auto-optimize copy routing, especially for bidirectional.
//   - Small measurement differences between APIs are expected.
//
// If future driver updates fix COPY queue reliability, D3D12 could be
// switched to COPY queues to fully converge with Vulkan's methodology.
//
// ============================================================================
// Features:
// - Real-time progress visualization
// - Interactive configuration
// - Results graphs and charts with standard comparisons
// - CSV export
// - VRAM-aware buffer sizing
// - VRAM integrity scanning (multiple test patterns, error clustering)
// - eGPU auto-detection (Thunderbolt/USB4/USB via device tree)
// - Integrated GPU (APU) proper detection - no fake PCIe reporting
// - Actual PCIe link detection via sysfs
// - System RAM detection via /proc/meminfo + dmidecode
// - Native UTF-8 (no conversion needed on Linux)
// ============================================================================

// Uncomment to enable debug logging for external GPU detection
// #define DEBUG_EXTERNAL_DETECTION

#include <vulkan/vulkan.h>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>
#include <chrono>
#include <ctime>
#include <thread>
#include <atomic>
#include <mutex>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <map>
#include <set>
#include <random>
#include <array>
#include <functional>
#include <optional>
#include <cassert>
#include <cstdio>
#include <cinttypes>
#include <cstring>
#include <climits>

// Linux-specific headers for hardware detection
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>

// ImGui headers (downloaded by CMake FetchContent)
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "implot.h"
#include "imgui_internal.h"

// Vulkan check macros
#define VK_CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { Log("[VULKAN ERROR] " + std::string(#x) + " = " + std::to_string((int)_r)); } } while(0)
#define VK_CHECK_RETURN(x, ret) do { VkResult _r = (x); if (_r != VK_SUCCESS) { Log("[VULKAN ERROR] " + std::string(#x) + " = " + std::to_string((int)_r)); return ret; } } while(0)


// ============================================================================
// CONSTANTS
// ============================================================================
namespace Constants {
    constexpr int LATENCY_WARMUP_ITERATIONS = 100;
    constexpr int WINDOW_WIDTH = 1400;
    constexpr int WINDOW_HEIGHT = 900;
    constexpr int NUM_FRAMES_IN_FLIGHT = 3;
    constexpr size_t DEFAULT_BANDWIDTH_SIZE = 256ull * 1024 * 1024;
    constexpr size_t DEFAULT_LATENCY_SIZE = 1;
    constexpr int DEFAULT_BANDWIDTH_BATCHES = 32;
    constexpr int DEFAULT_COPIES_PER_BATCH = 8;
    constexpr int DEFAULT_LATENCY_ITERS = 2000;
    constexpr int DEFAULT_NUM_RUNS = 3;
    constexpr float BASE_FONT_SCALE = 1.0f;
    
    constexpr uint32_t FENCE_WAIT_TIMEOUT_MS = 8000;
    constexpr int MAX_FENCE_RETRIES = 3;
    constexpr uint32_t GLOBAL_BENCHMARK_TIMEOUT_MS = 300000;
    
    constexpr double VRAM_SAFETY_MARGIN = 0.8;
    constexpr size_t MIN_BANDWIDTH_SIZE = 16ull * 1024 * 1024;
    
    // Bandwidth is reported in decimal GB/s (1 GB = 1e9 bytes) to match how
    // PCIe/TB/USB4 interface standards are specified. (Was 1024^3, which
    // under-reported by ~7.4% against every standard in the table.)
    constexpr double BYTES_PER_GB = 1e9;

    constexpr double EGPU_BANDWIDTH_THRESHOLD = 5.0;
    constexpr double TB3_MAX_BANDWIDTH = 3.5;
    constexpr double TB4_MAX_BANDWIDTH = 4.5;   // TB4 / USB4 40Gbps typical max (32 Gbps PCIe tunnel)
    constexpr double TB5_MAX_BANDWIDTH = 7.5;   // TB5 / USB4 80Gbps typical max (64 Gbps PCIe tunnel)

    // Memory latency compute shader test
    constexpr size_t MEMORY_LATENCY_BUFFER_SIZE = 32ull * 1024 * 1024;
    constexpr uint32_t MEMORY_LATENCY_NUM_CHASES = 100000;
    constexpr int MEMORY_LATENCY_WARMUP_DISPATCHES = 3;
    constexpr int MEMORY_LATENCY_MEASURE_DISPATCHES = 10;
}

// Embedded SPIR-V compute shader for GPU memory latency measurement (pointer-chase)
static const uint32_t g_memoryLatencySPIRV[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x0000002d, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0005000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060010, 0x00000004, 0x00000011,
    0x00000001, 0x00000001, 0x00000001, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004,
    0x6e69616d, 0x00000000, 0x00030005, 0x00000008, 0x00786469, 0x00040005, 0x00000009, 0x61726150,
    0x0000736d, 0x00060006, 0x00000009, 0x00000000, 0x436d756e, 0x65736168, 0x00000073, 0x00060006,
    0x00000009, 0x00000001, 0x72617473, 0x646e4974, 0x00007865, 0x00030005, 0x0000000b, 0x00000000,
    0x00030005, 0x00000011, 0x00000069, 0x00040005, 0x0000001f, 0x69616843, 0x0000006e, 0x00050006,
    0x0000001f, 0x00000000, 0x61746164, 0x00000000, 0x00040005, 0x00000021, 0x69616863, 0x0000006e,
    0x00030047, 0x00000009, 0x00000002, 0x00050048, 0x00000009, 0x00000000, 0x00000023, 0x00000000,
    0x00050048, 0x00000009, 0x00000001, 0x00000023, 0x00000004, 0x00040047, 0x0000001e, 0x00000006,
    0x00000004, 0x00030047, 0x0000001f, 0x00000003, 0x00050048, 0x0000001f, 0x00000000, 0x00000023,
    0x00000000, 0x00040047, 0x00000021, 0x00000021, 0x00000000, 0x00040047, 0x00000021, 0x00000022,
    0x00000000, 0x00040047, 0x0000002c, 0x0000000b, 0x00000019, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040020, 0x00000007,
    0x00000007, 0x00000006, 0x0004001e, 0x00000009, 0x00000006, 0x00000006, 0x00040020, 0x0000000a,
    0x00000009, 0x00000009, 0x0004003b, 0x0000000a, 0x0000000b, 0x00000009, 0x00040015, 0x0000000c,
    0x00000020, 0x00000001, 0x0004002b, 0x0000000c, 0x0000000d, 0x00000001, 0x00040020, 0x0000000e,
    0x00000009, 0x00000006, 0x0004002b, 0x00000006, 0x00000012, 0x00000000, 0x0004002b, 0x0000000c,
    0x00000019, 0x00000000, 0x00020014, 0x0000001c, 0x0003001d, 0x0000001e, 0x00000006, 0x0003001e,
    0x0000001f, 0x0000001e, 0x00040020, 0x00000020, 0x00000002, 0x0000001f, 0x0004003b, 0x00000020,
    0x00000021, 0x00000002, 0x00040020, 0x00000023, 0x00000002, 0x00000006, 0x00040017, 0x0000002a,
    0x00000006, 0x00000003, 0x0004002b, 0x00000006, 0x0000002b, 0x00000001, 0x0006002c, 0x0000002a,
    0x0000002c, 0x0000002b, 0x0000002b, 0x0000002b, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000007, 0x00000008, 0x00000007, 0x0004003b,
    0x00000007, 0x00000011, 0x00000007, 0x00050041, 0x0000000e, 0x0000000f, 0x0000000b, 0x0000000d,
    0x0004003d, 0x00000006, 0x00000010, 0x0000000f, 0x0003003e, 0x00000008, 0x00000010, 0x0003003e,
    0x00000011, 0x00000012, 0x000200f9, 0x00000013, 0x000200f8, 0x00000013, 0x000400f6, 0x00000015,
    0x00000016, 0x00000000, 0x000200f9, 0x00000017, 0x000200f8, 0x00000017, 0x0004003d, 0x00000006,
    0x00000018, 0x00000011, 0x00050041, 0x0000000e, 0x0000001a, 0x0000000b, 0x00000019, 0x0004003d,
    0x00000006, 0x0000001b, 0x0000001a, 0x000500b0, 0x0000001c, 0x0000001d, 0x00000018, 0x0000001b,
    0x000400fa, 0x0000001d, 0x00000014, 0x00000015, 0x000200f8, 0x00000014, 0x0004003d, 0x00000006,
    0x00000022, 0x00000008, 0x00060041, 0x00000023, 0x00000024, 0x00000021, 0x00000019, 0x00000022,
    0x0004003d, 0x00000006, 0x00000025, 0x00000024, 0x0003003e, 0x00000008, 0x00000025, 0x000200f9,
    0x00000016, 0x000200f8, 0x00000016, 0x0004003d, 0x00000006, 0x00000026, 0x00000011, 0x00050080,
    0x00000006, 0x00000027, 0x00000026, 0x0000000d, 0x0003003e, 0x00000011, 0x00000027, 0x000200f9,
    0x00000013, 0x000200f8, 0x00000015, 0x0004003d, 0x00000006, 0x00000028, 0x00000008, 0x00060041,
    0x00000023, 0x00000029, 0x00000021, 0x00000019, 0x00000019, 0x0003003e, 0x00000029, 0x00000028,
    0x000100fd, 0x00010038,
};
static const size_t g_memoryLatencySPIRVSize = sizeof(g_memoryLatencySPIRV);

// ============================================================================
// DATA STRUCTURES
// ============================================================================
struct GPUInfo {
    std::string name;
    std::string vendor;
    uint32_t    vendorId = 0;
    uint32_t    deviceId = 0;
    size_t      dedicatedVRAM = 0;
    size_t      sharedMemory = 0;
    bool        isIntegrated = false;
    bool        isValid = true;
    
    int         pcieGenCurrent = 0;
    int         pcieLanesCurrent = 0;
    int         pcieGenMax = 0;
    int         pcieLanesMax = 0;
    bool        pcieInfoValid = false;
    std::string pcieLocationPath;
    
    bool        isThunderbolt = false;
    bool        isUSB4 = false;
    bool        isUSB = false;
    int         thunderboltVersion = 0;
    std::string externalConnectionType;
    
    // Vulkan physical device handle for reliable selection
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
};

// System RAM information (detected via /proc/meminfo + dmidecode)
struct SystemMemoryInfo {
    uint32_t speedMT = 0;
    uint32_t configuredSpeedMT = 0;
    uint32_t channels = 0;
    uint32_t totalSticks = 0;
    uint64_t totalCapacityGB = 0;
    std::string type;
    std::string formFactor;
    double theoreticalBandwidth = 0;
    bool detected = false;
    std::string errorMessage;
    double ratedLatencyNs = 0;         // Estimated chip latency from speed tier (ns)
    int    estimatedCL = 0;            // Estimated CAS latency
    bool   latencyEstimated = false;   // True if we found a matching speed tier
    uint32_t busWidthBits = 0;         // Memory bus width from SMBIOS data widths (0 = unknown)
    bool   channelsUnverified = false; // Soldered LPDDR: channel count is only an SMBIOS-based guess
    bool   channelsInferred = false;   // Channel count raised to fit the measured iGPU bandwidth
    bool   needsElevation = false;     // Linux: SMBIOS (dmidecode) needs root; per-device details missing
};

// VRAM test pattern types
enum class VRAMTestPattern {
    AllZeros,
    AllOnes,
    Checkerboard,
    InverseCheckerboard,
    Random,
    MarchingOnes,
    MarchingZeros,
    AddressPattern
};

// Classification of a memory error by physical mechanism.
enum class VRAMErrorKind {
    SingleBit,
    MultiBit,
    AddressBus,
    StuckAtZero,
    StuckAtOne,
    RefreshError
};

inline const char* GetErrorKindName(VRAMErrorKind k) {
    switch (k) {
        case VRAMErrorKind::SingleBit:    return "single-bit";
        case VRAMErrorKind::MultiBit:     return "multi-bit";
        case VRAMErrorKind::AddressBus:   return "address-bus";
        case VRAMErrorKind::StuckAtZero:  return "stuck-at-0";
        case VRAMErrorKind::StuckAtOne:   return "stuck-at-1";
        case VRAMErrorKind::RefreshError: return "refresh";
        default:                          return "unknown";
    }
}

struct VRAMError {
    size_t offsetStart = 0;
    size_t offsetEnd = 0;
    uint32_t expected = 0;
    uint32_t actual = 0;
    VRAMTestPattern pattern;
    size_t errorCount = 0;

    // Bit-level diagnostic info (first error in cluster)
    uint32_t bitFlipMask = 0;
    uint32_t bitFlipCount = 0;
    int      bitIndex = -1;
    VRAMErrorKind kind = VRAMErrorKind::SingleBit;
};

struct VRAMTestResult {
    bool completed = false;
    bool cancelled = false;
    size_t totalBytesTested = 0;
    size_t totalErrors = 0;
    std::vector<VRAMError> errors;
    std::vector<std::string> patternResults;
    double testDurationSeconds = 0;
    std::string summary;

    // Aggregate bit-level error stats
    std::array<size_t, 32> bitFlipHistogram = {};
    std::array<size_t, 6>  errorKindCounts  = {};
    size_t refreshPassErrors = 0;
};

static const double PCIE_GEN_SPEEDS[] = {
    0.0, 2.5, 5.0, 8.0, 16.0, 32.0, 64.0
};

static const double PCIE_PROTOCOL_EFFICIENCY[] = {
    0.0, 0.65, 0.70, 0.85, 0.85, 0.85, 0.85
};

inline double CalculatePCIeBandwidth(int gen, int lanes) {
    if (gen < 1 || gen > 6 || lanes < 1) return 0.0;
    double encodingEfficiency = (gen >= 3) ? (128.0 / 130.0) : (8.0 / 10.0);
    double gtPerSec = PCIE_GEN_SPEEDS[gen];
    return (gtPerSec * lanes * encodingEfficiency) / 8.0;
}

inline double CalculateRealisticPCIeBandwidth(int gen, int lanes) {
    if (gen < 1 || gen > 6 || lanes < 1) return 0.0;
    double gtPerSec = PCIE_GEN_SPEEDS[gen];
    double efficiency = PCIE_PROTOCOL_EFFICIENCY[gen];
    return (gtPerSec * lanes * efficiency) / 8.0;
}

struct BenchmarkResult {
    std::string       testName;
    double            minValue = 0;
    double            avgValue = 0;
    double            maxValue = 0;
    std::string       unit;
    std::vector<double> samples;
};

// VRAM scan preset modes.
enum class VRAMScanPreset {
    Quick,
    Standard,
    Deep,
    Thorough,
    Marathon,
    Custom
};

struct BenchmarkConfig {
    size_t bandwidthSize = Constants::DEFAULT_BANDWIDTH_SIZE;
    size_t latencySize = Constants::DEFAULT_LATENCY_SIZE;
    int    bandwidthBatches = Constants::DEFAULT_BANDWIDTH_BATCHES;
    int    copiesPerBatch = Constants::DEFAULT_COPIES_PER_BATCH;
    int    latencyIters = Constants::DEFAULT_LATENCY_ITERS;
    int    numRuns = Constants::DEFAULT_NUM_RUNS;
    bool   runBidirectional = true;
    bool   runLatency = true;
    bool   runMemoryLatency = true;  // GPU memory latency via compute shader pointer-chase
    bool   quickMode = false;
    bool   averageRuns = true;
    bool   debugLogging = false;  // Verbose diagnostic logging for memory latency test etc.
    int    selectedGPU = 0;

    // VRAM scan options (borrowed from memtest_vulkan-style stress testing)
    VRAMScanPreset vramScanPreset = VRAMScanPreset::Standard;
    std::array<bool, 8> vramPatternsEnabled = { true, true, true, true, true, true, true, true };
    bool   vramRereadEnabled = false;
    int    vramRereadIterations = 4;
    bool   vramNonSequentialEnabled = false;
    int    vramNonSequentialBlockSize = 65536;
    int    vramCoveragePercent = 80;
    bool   vramPreheatEnabled = false;
    int    vramPreheatSeconds = 30;
    bool   vramMarathonMode = false;
    bool   vramGpuVerify = false;
};

struct InterfaceSpeed {
    const char* name;
    double      bandwidth;
    double      theoretical;
    const char* description;
    bool        tunneledExternal; // true = TB/USB4 PCIe-tunneling tier (eGPU over TB/USB4)
};

// Thunderbolt/USB4 PCIe payload is capped by the PCIe *tunnel*, not the link
// rate: 40 Gbps links tunnel at most 32 Gbps of PCIe (4.0 GB/s raw), 80 Gbps
// links (TB5 / USB4v2) tunnel at most 64 Gbps (8.0 GB/s raw). TB5 and USB4
// 80Gbps are indistinguishable from bandwidth alone, so they share one entry.
static const InterfaceSpeed INTERFACE_SPEEDS[] = {
    {"PCIe 3.0 x4",    3.40,   3.94,  "Entry-level GPU slot",                 false},
    {"PCIe 3.0 x8",    6.80,   7.88,  "Mid-range GPU slot",                   false},
    {"PCIe 3.0 x16",   13.60,  15.75, "Standard discrete GPU",                false},
    {"PCIe 4.0 x4",    6.80,   7.88,  "NVMe / Entry eGPU",                    false},
    {"PCIe 4.0 x8",    13.60,  15.75, "Mid-range PCIe 4.0",                   false},
    {"PCIe 4.0 x16",   27.20,  31.51, "High-end discrete GPU",                false},
    {"PCIe 5.0 x8",    27.20,  31.51, "PCIe 5.0 mid-range",                   false},
    {"PCIe 5.0 x16",   54.40,  63.02, "High-end PCIe 5.0 GPU slot",           false},
    {"PCIe 6.0 x16",   108.80, 126.03, "Next-gen PCIe 6.0 GPU slot",          false},
    {"OCuLink 1.0",    3.40,   3.94,  "PCIe 3.0 x4 external",                 false},
    {"OCuLink 2.0",    6.80,   7.88,  "PCIe 4.0 x4 external",                 false},
    {"Thunderbolt 3",  2.50,   2.80,  "40 Gbps link (variable PCIe allocation)", true},
    {"Thunderbolt 4 / USB4 40Gbps", 3.50, 4.00, "40 Gbps link (32 Gbps PCIe tunnel)", true},
    {"Thunderbolt 5 / USB4 80Gbps", 6.50, 8.00, "80 Gbps link (64 Gbps PCIe tunnel)", true},
};

static const int NUM_INTERFACE_SPEEDS = sizeof(INTERFACE_SPEEDS) / sizeof(INTERFACE_SPEEDS[0]);

// Memory bandwidth standards for integrated GPU (APU) comparison
// Realistic = ~80% of theoretical (memory controller overhead, contention)
// Theoretical = speed_MT/s * 8 bytes * 2 channels / 1000
static const InterfaceSpeed MEMORY_STANDARDS[] = {
    {"DDR4-2400 DC",    30.7,   38.4,  "Dual-channel DDR4-2400"},
    {"DDR4-3200 DC",    41.0,   51.2,  "Dual-channel DDR4-3200"},
    {"DDR5-4800 DC",    61.4,   76.8,  "Dual-channel DDR5-4800"},
    {"DDR5-5600 DC",    71.7,   89.6,  "Dual-channel DDR5-5600"},
    {"DDR5-6400 DC",    81.9,   102.4, "Dual-channel DDR5-6400"},
    {"DDR5-8800 DC",    112.6,  140.8, "Dual-channel DDR5-8800"},
    {"LPDDR5X-7500",    96.0,   120.0, "Dual-channel LPDDR5X-7500"},
    {"LPDDR5X-8533",    109.2,  136.5, "Dual-channel LPDDR5X-8533"},
    {"DDR6-12800 DC",   163.8,  204.8, "Dual-channel DDR6-12800 (projected)"},
    {"DDR6-17600 DC",   225.3,  281.6, "Dual-channel DDR6-17600 (projected)"},
};
static const int NUM_MEMORY_STANDARDS = sizeof(MEMORY_STANDARDS) / sizeof(MEMORY_STANDARDS[0]);

// Typical CAS latency by speed tier for rated chip latency estimation
struct MemoryLatencyEntry {
    uint32_t speedMT;
    const char* type;
    int typicalCL;
    double latencyNs;
};

static const MemoryLatencyEntry MEMORY_LATENCY_TABLE[] = {
    { 2400,  "DDR4",    17, 14.17 },
    { 2666,  "DDR4",    17, 12.76 },
    { 3200,  "DDR4",    16, 10.00 },
    { 3600,  "DDR4",    18, 10.00 },
    { 4800,  "DDR5",    40, 16.67 },
    { 5200,  "DDR5",    38, 14.62 },
    { 5600,  "DDR5",    36, 12.86 },
    { 6000,  "DDR5",    36, 12.00 },
    { 6400,  "DDR5",    40, 12.50 },
    { 6800,  "DDR5",    40, 11.76 },
    { 7200,  "DDR5",    40, 11.11 },
    { 7500,  "LPDDR5X", 36,  9.60 },
    { 7600,  "DDR5",    40, 10.53 },
    { 8000,  "DDR5",    40, 10.00 },
    { 8533,  "LPDDR5X", 36,  8.44 },
    { 8800,  "DDR5",    44, 10.00 },
    { 12800, "DDR6",    52,  8.13 },
    { 17600, "DDR6",    60,  6.82 },
};
static const int NUM_MEMORY_LATENCY_ENTRIES = sizeof(MEMORY_LATENCY_TABLE) / sizeof(MEMORY_LATENCY_TABLE[0]);

// ============================================================================
// VULKAN BUFFER ALLOCATION HELPER
// ============================================================================
// Replaces ComPtr<ID3D12Resource> for buffer management
enum class VkBufferType {
    Upload,     // HOST_VISIBLE | HOST_COHERENT (replaces D3D12_HEAP_TYPE_UPLOAD)
    DeviceLocal,// DEVICE_LOCAL (replaces D3D12_HEAP_TYPE_DEFAULT)
    Readback    // HOST_VISIBLE | HOST_CACHED (replaces D3D12_HEAP_TYPE_READBACK)
};

struct VkBufferAllocation {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize   size = 0;
    void*          mappedPtr = nullptr;  // For persistently mapped buffers
    
    bool IsValid() const { return buffer != VK_NULL_HANDLE && memory != VK_NULL_HANDLE; }
    operator bool() const { return IsValid(); }
    
    void Destroy(VkDevice device) {
        if (buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, buffer, nullptr); buffer = VK_NULL_HANDLE; }
        if (memory != VK_NULL_HANDLE) { vkFreeMemory(device, memory, nullptr); memory = VK_NULL_HANDLE; }
        mappedPtr = nullptr;
        size = 0;
    }
};

// ============================================================================
// APPLICATION STATE
// ============================================================================
enum class AppState { Idle, Running, Completed };

// Fence wait result for robust error handling
enum class FenceWaitResult { Success, Timeout, Error, Cancelled };

struct AppContext {
    // Window (GLFW)
    GLFWwindow* window = nullptr;
    int  windowWidth = Constants::WINDOW_WIDTH;
    int  windowHeight = Constants::WINDOW_HEIGHT;

    // Vulkan Rendering State
    VkInstance                 instance = VK_NULL_HANDLE;
    VkPhysicalDevice           renderPhysicalDevice = VK_NULL_HANDLE;
    VkDevice                   device = VK_NULL_HANDLE;
    VkQueue                    graphicsQueue = VK_NULL_HANDLE;
    uint32_t                   graphicsQueueFamily = UINT32_MAX;
    VkSurfaceKHR               surface = VK_NULL_HANDLE;
    VkSwapchainKHR             swapChain = VK_NULL_HANDLE;
    VkFormat                   swapChainFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D                 swapChainExtent = {};
    std::vector<VkImage>       swapChainImages;
    std::vector<VkImageView>   swapChainImageViews;
    std::vector<VkFramebuffer> swapChainFramebuffers;
    VkRenderPass               renderPass = VK_NULL_HANDLE;
    VkCommandPool              commandPool = VK_NULL_HANDLE;
    VkCommandBuffer            commandBuffers[Constants::NUM_FRAMES_IN_FLIGHT] = {};
    VkSemaphore                imageAvailableSemaphores[Constants::NUM_FRAMES_IN_FLIGHT] = {};
    std::vector<VkSemaphore>   renderFinishedSemaphores;  // one per swapchain image, indexed by imageIndex
    VkFence                    inFlightFences[Constants::NUM_FRAMES_IN_FLIGHT] = {};
    VkDescriptorPool           imguiDescriptorPool = VK_NULL_HANDLE;
    uint32_t                   frameIndex = 0;
    uint32_t                   imageIndex = 0;
#ifdef ENABLE_VULKAN_VALIDATION
    VkDebugUtilsMessengerEXT   debugMessenger = VK_NULL_HANDLE;
#endif

    // Vulkan Benchmark Device (separate device for benchmarking)
    VkPhysicalDevice           benchPhysicalDevice = VK_NULL_HANDLE;
    VkDevice                   benchDevice = VK_NULL_HANDLE;
    VkQueue                    benchQueue = VK_NULL_HANDLE;
    uint32_t                   benchQueueFamily = UINT32_MAX;
    VkCommandPool              benchCommandPool = VK_NULL_HANDLE;
    VkCommandBuffer            benchCommandBuffer = VK_NULL_HANDLE;
    VkFence                    benchFence = VK_NULL_HANDLE;
    uint64_t                   benchFenceValue = 1;
    float                      benchTimestampPeriod = 0.0f;  // nanoseconds per tick
    uint64_t                   benchTimestampMask = ~0ull;   // mask of timestampValidBits for the bench queue family

    // Second queue for bidirectional transfers (allows true simultaneous upload/download)
    VkQueue                    benchQueue2 = VK_NULL_HANDLE;
    VkCommandPool              benchCommandPool2 = VK_NULL_HANDLE;
    VkCommandBuffer            benchCommandBuffer2 = VK_NULL_HANDLE;
    VkFence                    benchFence2 = VK_NULL_HANDLE;
    bool                       hasDualQueues = false;

    // GPU list
    std::vector<GPUInfo>              gpuList;
    std::vector<std::string>          gpuComboNames;
    std::vector<const char*>          gpuComboPointers;

    // Config
    BenchmarkConfig config;

    // State
    AppState state = AppState::Idle;
    std::atomic<float> progress{ 0.0f };
    std::atomic<float> overallProgress{ 0.0f };
    std::atomic<int>   currentRun{ 0 };
    std::atomic<int>   totalTests{ 0 };
    std::atomic<int>   completedTests{ 0 };
    std::atomic<bool>  cancelRequested{ false };
    std::atomic<bool>  benchmarkAborted{ false };
    std::atomic<int>   fenceTimeoutCount{ 0 };
    std::string        currentTest;
    std::mutex         resultsMutex;
    std::vector<BenchmarkResult> results;
    std::thread        benchmarkThread;
    std::atomic<bool>  benchmarkThreadRunning{ false };
    
    // Benchmark timing for global timeout
    std::chrono::steady_clock::time_point benchmarkStartTime;

    // UI State
    bool showResultsWindow = false;
    bool showGraphsWindow = false;
    bool showCompareWindow = false;
    bool showAboutDialog = false;
    bool dockingInitialized = false;
    bool isResizing = false;
    bool pendingResize = false;
    int  pendingWidth = 0;
    int  pendingHeight = 0;

    // Log buffer
    std::mutex                 logMutex;
    std::vector<std::string>   logLines;

    // Detected interface results
    std::string detectedInterface;
    std::string detectedInterfaceDescription;
    double      uploadBW = 0;
    double      downloadBW = 0;
    double      uploadPercentage = 0;
    double      downloadPercentage = 0;
    std::string closestUploadStandard;
    std::string closestDownloadStandard;
    
    // eGPU detection
    bool        possibleEGPU = false;
    std::string eGPUConnectionType;
    
    // Integrated GPU memory info
    std::string integratedMemoryType;
    std::string integratedFabricType;
    
    // Summary window
    bool        showSummaryWindow = false;
    double      actualPCIeBandwidth = 0;
    std::string actualPCIeConfig;
    std::string summaryExplanation;
    
    // System memory info (detected via WMI)
    SystemMemoryInfo systemMemory;
    
    // VRAM test state
    std::atomic<bool> vramTestRunning{ false };
    std::atomic<bool> vramTestCancelRequested{ false };
    std::thread vramTestThread;
    VRAMTestResult vramTestResult;
    std::atomic<float> vramTestProgress{ 0.0f };
    std::string vramTestCurrentPattern;
    bool showVRAMTestWindow = false;
    bool memoryPromptDismissed = false;  // startup pkexec prompt answered
    // (Coverage is now driven by g_app.config.vramCoveragePercent - see BenchmarkConfig)
};

static AppContext g_app;

// Helper to add log messages
void Log(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_app.logMutex);
    g_app.logLines.push_back(msg);
    // Keep last 500 lines
    if (g_app.logLines.size() > 500u) {
        g_app.logLines.erase(g_app.logLines.begin());
    }
}

void ClearLog() {
    std::lock_guard<std::mutex> lock(g_app.logMutex);
    g_app.logLines.clear();
}

// currentTest and vramTestCurrentPattern are written by the benchmark/scan
// worker threads and read by the UI thread. Route every access through these
// helpers so the UI never observes a torn std::string (reallocation mid-read).
static std::mutex g_statusMutex;
void SetCurrentTest(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_app.currentTest = s;
}
std::string GetCurrentTest() {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    return g_app.currentTest;
}
void SetVramPattern(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_app.vramTestCurrentPattern = s;
}
std::string GetVramPattern() {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    return g_app.vramTestCurrentPattern;
}

// ============================================================================
// LINUX HELPER FUNCTIONS
// ============================================================================
// Linux is native UTF-8 - no Unicode conversion helpers needed.

// Read a sysfs file and return its trimmed content
static std::string ReadSysfsFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string content;
    std::getline(f, content);
    // Trim trailing whitespace/newlines
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r' || content.back() == ' '))
        content.pop_back();
    return content;
}

// Read entire file content (for multi-line sysfs/proc files)
static std::string ReadFileContents(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Execute a command and capture its stdout
static std::string ExecCommand(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    return result;
}


// ============================================================================
// SYSTEM MEMORY DETECTION (Linux: /proc/meminfo + dmidecode)
// ============================================================================

// DDR type mapping from SMBIOSMemoryType
std::string GetDDRTypeFromSMBIOS(uint16_t memoryType) {
    switch (memoryType) {
        case 20: return "DDR";
        case 21: return "DDR2";
        case 22: return "DDR2 FB-DIMM";
        case 24: return "DDR3";
        case 26: return "DDR4";
        case 27: return "LPDDR";
        case 28: return "LPDDR2";
        case 29: return "LPDDR3";
        case 30: return "LPDDR4";
        case 34: return "DDR5";
        case 35: return "LPDDR5";
        case 36: return "LPDDR5X";
        // DDR6/DDR7: JEDEC SMBIOS type codes not yet assigned as of 2025.
        // When assigned, add them here. Expected in SMBIOS 3.8+.
        // case ??: return "DDR6";
        // case ??: return "LPDDR6";
        // case ??: return "DDR7";
        default: return "Unknown";
    }
}

// Soldered memory (LPDDR on laptops / APUs; SMBIOS form factor "Row Of Chips")
// has no DIMM-per-channel relationship: firmware may describe a 256-bit bus as
// one device or as eight 32-bit devices. Device counting and locator letters
// therefore say little about the channel count.
static bool IsSolderedMemory(const std::string& type, const std::string& formFactor) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](char c) { return static_cast<char>(::tolower(static_cast<unsigned char>(c))); });
        return s;
    };
    std::string t = lower(type), f = lower(formFactor);
    return t.find("lpddr") != std::string::npos ||
           f.find("row of chips") != std::string::npos ||
           f.find("chip") != std::string::npos;
}

// Form factor mapping
std::string GetFormFactorName(uint16_t formFactor) {
    switch (formFactor) {
        case 8:  return "DIMM";
        case 9:  return "Row Of Chips";  // soldered (LPDDR on laptops / APUs)
        case 12: return "SODIMM";
        case 13: return "SRIMM";
        case 14: return "FB-DIMM";
        default: return "Unknown";
    }
}

// Helper: trim whitespace from both ends of a string
static std::string TrimString(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Detect system memory configuration via /proc/meminfo and dmidecode
SystemMemoryInfo DetectSystemMemoryFrom(const std::string& dmidecodeOutputIn) {
    SystemMemoryInfo info;

    // ---- Step 1: Get total capacity from /proc/meminfo (always available) ----
    std::string meminfo = ReadFileContents("/proc/meminfo");
    if (!meminfo.empty()) {
        std::istringstream stream(meminfo);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.find("MemTotal:") == 0) {
                // Format: "MemTotal:       32657436 kB"
                std::istringstream ls(line);
                std::string label;
                uint64_t kbValue = 0;
                ls >> label >> kbValue;  // label = "MemTotal:", kbValue = size in kB
                // Convert kB to GB (round to nearest GB for display)
                info.totalCapacityGB = (kbValue + 524288) / 1048576;  // +512K for rounding
                break;
            }
        }
    }

    // ---- Step 2: dmidecode output (requires root to read the SMBIOS table) ----
    // Unprivileged dmidecode still prints its banner ("# dmidecode 3.7 ...
    // SMBIOS 3.7.0 present.") before failing on the table, so "non-empty
    // output" is not "has data": require at least one Memory Device section.
    std::string dmidecodeOutput = dmidecodeOutputIn;

    if (dmidecodeOutput.find("Memory Device") == std::string::npos) {
        info.needsElevation = (geteuid() != 0);
        if (info.totalCapacityGB > 0) {
            info.detected = true;
            info.errorMessage = info.needsElevation
                ? "SMBIOS memory table needs root (Help > Read memory details, or run with sudo)"
                : "dmidecode unavailable or reported no memory devices";
        } else {
            info.errorMessage = "Failed to read /proc/meminfo";
        }
        return info;
    }

    // Parse dmidecode "Memory Device" sections
    uint32_t maxSpeed = 0;
    uint32_t maxConfiguredSpeed = 0;
    std::string detectedType;
    std::string detectedFormFactor;
    int stickCount = 0;
    uint32_t dataWidthSum = 0;  // sum of per-device SMBIOS data widths (bits), populated devices only
    uint64_t dmidecodeCapacity = 0;
    std::set<std::string> uniqueLocators;  // For channel counting
    std::set<char> bankChannelLetters;     // From "Bank Locator: ... CHANNEL X"

    // Split output into lines and parse Memory Device sections
    std::istringstream dmiStream(dmidecodeOutput);
    std::string line;
    bool inMemoryDevice = false;
    bool slotHasModule = false;
    std::string currentLocator;
    uint32_t currentDataWidth = 0;  // "Data Width" precedes "Size" in dmidecode output

    while (std::getline(dmiStream, line)) {
        // Detect start of a "Memory Device" section
        if (line.find("Memory Device") != std::string::npos &&
            line.find("Memory Device Mapped") == std::string::npos) {
            // If we were processing a previous device with a module, count it
            if (inMemoryDevice && slotHasModule) {
                stickCount++;
                dataWidthSum += currentDataWidth;
                if (!currentLocator.empty()) {
                    uniqueLocators.insert(currentLocator);
                }
            }
            inMemoryDevice = true;
            slotHasModule = false;
            currentLocator.clear();
            currentDataWidth = 0;
            continue;
        }

        if (!inMemoryDevice) continue;

        // Look for key fields within a Memory Device section
        // Each field is indented with a tab, format: "\tField: Value"
        size_t colonPos = line.find(':');
        if (colonPos == std::string::npos) continue;

        std::string field = TrimString(line.substr(0, colonPos));
        std::string value = TrimString(line.substr(colonPos + 1));

        // Size field - skip empty slots
        if (field == "Size") {
            if (value == "No Module Installed" || value == "0" || value.empty()) {
                slotHasModule = false;
                inMemoryDevice = false;  // Skip rest of this empty slot
                continue;
            }
            slotHasModule = true;
            // Parse size (e.g., "8192 MB", "16 GB")
            std::istringstream sizeStream(value);
            uint64_t sizeVal = 0;
            std::string sizeUnit;
            sizeStream >> sizeVal >> sizeUnit;
            // dmidecode >= 3.6 prints binary units ("16 GiB"); older prints "16 GB".
            if (sizeUnit == "MB" || sizeUnit == "mb" || sizeUnit == "MiB") {
                dmidecodeCapacity += sizeVal;  // Keep in MB
            } else if (sizeUnit == "GB" || sizeUnit == "gb" || sizeUnit == "GiB") {
                dmidecodeCapacity += sizeVal * 1024;  // Convert to MB
            }
        }

        // Speed (rated/max speed)
        if (field == "Speed" && slotHasModule) {
            // Format: "4800 MT/s" or "3200 MHz" or "Unknown"
            if (value != "Unknown" && !value.empty()) {
                std::istringstream speedStream(value);
                uint32_t speedVal = 0;
                speedStream >> speedVal;
                if (speedVal > maxSpeed) {
                    maxSpeed = speedVal;
                }
            }
        }

        // Configured Memory Speed (actual running speed)
        if ((field == "Configured Memory Speed" || field == "Configured Clock Speed") && slotHasModule) {
            if (value != "Unknown" && !value.empty()) {
                std::istringstream speedStream(value);
                uint32_t speedVal = 0;
                speedStream >> speedVal;
                if (speedVal > maxConfiguredSpeed) {
                    maxConfiguredSpeed = speedVal;
                }
            }
        }

        // Type (DDR4, DDR5, DDR6, etc.)
        if (field == "Type" && slotHasModule) {
            if (value != "Unknown" && !value.empty()) {
                detectedType = value;
            }
        }

        // Form Factor
        if (field == "Form Factor" && slotHasModule) {
            if (value != "Unknown" && !value.empty()) {
                detectedFormFactor = value;
            }
        }

        // Data Width ("64 bits"). It is printed BEFORE Size, so it cannot be
        // gated on slotHasModule here; remember it and add it when the device
        // turns out to be populated (summed for soldered memory).
        if (field == "Data Width") {
            if (value != "Unknown" && !value.empty()) {
                std::istringstream widthStream(value);
                uint32_t bits = 0;
                widthStream >> bits;
                if (bits > 0 && bits <= 1024) currentDataWidth = bits;
            }
        }

        // Locator (for channel counting - e.g., "DIMM_A1", "DIMM_B1", "ChannelA-DIMM0")
        if (field == "Locator" && slotHasModule) {
            if (value != "Unknown" && !value.empty()) {
                currentLocator = value;
            }
        }

        // Bank Locator often names the channel directly ("P0 CHANNEL A",
        // "BANK 0 CHANNEL B") - soldered LPDDR boards typically repeat the same
        // Locator ("DIMM 0") for every device, so this is the only channel hint.
        if (field == "Bank Locator" && slotHasModule) {
            std::string upper = value;
            std::transform(upper.begin(), upper.end(), upper.begin(),
                           [](char c) { return static_cast<char>(::toupper(static_cast<unsigned char>(c))); });
            size_t cp = upper.find("CHANNEL");
            if (cp != std::string::npos) {
                size_t i = cp + 7;
                while (i < upper.size() && (upper[i] == ' ' || upper[i] == '_' || upper[i] == '-')) ++i;
                if (i < upper.size() && upper[i] >= 'A' && upper[i] <= 'Z') bankChannelLetters.insert(upper[i]);
            }
        }
    }

    // Don't forget the last Memory Device section
    if (inMemoryDevice && slotHasModule) {
        stickCount++;
        dataWidthSum += currentDataWidth;
        if (!currentLocator.empty()) {
            uniqueLocators.insert(currentLocator);
        }
    }

    // ---- Step 3: Fill in the SystemMemoryInfo struct ----

    // Installed size from SMBIOS beats MemTotal (the kernel reserves some RAM,
    // so 128 GiB installed reads as ~125 GiB in /proc/meminfo).
    if (dmidecodeCapacity / 1024 > info.totalCapacityGB) {
        info.totalCapacityGB = dmidecodeCapacity / 1024;  // MB to GB
    }

    // Determine speed in MT/s.
    // dmidecode reports the SMBIOS Type-17 Speed field, which is defined in
    // MT/s (e.g. DDR4-3200 -> "3200 MT/s"; older builds print "3200 MHz" but
    // still the MT/s value), so no doubling is applied for any type.
    // (Previously DDR4 and earlier were doubled, inflating their speed to 2x
    // and skewing the theoretical-bandwidth comparison.)
    info.speedMT = maxSpeed;
    info.configuredSpeedMT = maxConfiguredSpeed;

    info.totalSticks = static_cast<uint32_t>(stickCount);
    info.type = detectedType;
    info.formFactor = detectedFormFactor;

    // Estimate channels from locator names or stick count
    // Common locator patterns: "DIMM_A1"/"DIMM_B1" (channel = A/B letter),
    // "ChannelA-DIMM0"/"ChannelB-DIMM0", "BANK 0"/"BANK 2"
    // Count unique channel identifiers from locator strings
    std::set<char> channelLetters;
    for (const auto& loc : uniqueLocators) {
        // Try to extract channel letter from common patterns
        // Pattern 1: "DIMM_A1" -> extract 'A'
        size_t underscorePos = loc.find('_');
        if (underscorePos != std::string::npos && underscorePos + 1 < loc.size()) {
            char ch = loc[underscorePos + 1];
            if (ch >= 'A' && ch <= 'H') {
                channelLetters.insert(ch);
                continue;
            }
        }
        // Pattern 2: "ChannelA-DIMM0" -> extract 'A'
        size_t channelPos = loc.find("Channel");
        if (channelPos != std::string::npos && channelPos + 7 < loc.size()) {
            char ch = loc[channelPos + 7];
            if (ch >= 'A' && ch <= 'H') {
                channelLetters.insert(ch);
                continue;
            }
        }
    }

    for (char c : bankChannelLetters) channelLetters.insert(c);

    if (channelLetters.size() >= 4u) {
        info.channels = 4;  // Quad channel
    } else if (channelLetters.size() >= 2u) {
        info.channels = static_cast<uint32_t>(channelLetters.size());
    } else if (stickCount >= 4) {
        info.channels = 4;  // Quad channel (heuristic)
    } else if (stickCount >= 2) {
        info.channels = 2;  // Dual channel (typical)
    } else if (stickCount == 1) {
        info.channels = 1;  // Single channel
    } else {
        info.channels = 0;  // No device data at all - unknown, not "single"
    }

    // Soldered memory: prefer the SMBIOS per-device data widths, which the
    // device-count / locator heuristics above cannot see. Strix Halo class APUs
    // report e.g. 8 x 32-bit LPDDR5X devices (256-bit bus) or a single device;
    // either way the estimate is flagged so no "single-channel" warning fires
    // and the iGPU comparison can correct it from the measurement.
    if (IsSolderedMemory(info.type, info.formFactor)) {
        info.channelsUnverified = true;
        if (dataWidthSum >= 64) {
            info.busWidthBits = dataWidthSum;
            info.channels = std::clamp<uint32_t>(dataWidthSum / 64, 1u, 8u);
        }
    }

    // Calculate theoretical bandwidth
    // DDR bandwidth = speed (MT/s) * 8 bytes * channels / 1000 = GB/s
    if (info.configuredSpeedMT > 0) {
        info.theoreticalBandwidth = (info.configuredSpeedMT * 8.0 * info.channels) / 1000.0;
    } else if (info.speedMT > 0) {
        info.theoreticalBandwidth = (info.speedMT * 8.0 * info.channels) / 1000.0;
    }

    info.detected = (stickCount > 0);

    // If dmidecode gave us sticks but no /proc/meminfo, still mark as detected
    // If we only have /proc/meminfo (no dmidecode), mark detected with limited info
    if (!info.detected && info.totalCapacityGB > 0) {
        info.detected = true;
        info.errorMessage = "dmidecode unavailable (run as root for full details)";
    }

    return info;
}

// Normal path: whatever dmidecode gives the current user (root sees the table;
// an unprivileged run only gets the banner and needsElevation is set).
SystemMemoryInfo DetectSystemMemory() {
    return DetectSystemMemoryFrom(ExecCommand("dmidecode --type memory 2>/dev/null"));
}

// Format system memory info as a string for logging
std::string FormatSystemMemoryInfo(const SystemMemoryInfo& mem) {
    if (!mem.detected) {
        return "System Memory: Detection failed (" + mem.errorMessage + ")";
    }

    std::ostringstream oss;
    oss << "System Memory: ";
    oss << mem.totalCapacityGB << "GB";
    if (!mem.type.empty()) oss << " " << mem.type;

    if (mem.configuredSpeedMT > 0) {
        oss << " @ " << mem.configuredSpeedMT << " MT/s";
        if (mem.speedMT > 0 && mem.speedMT != mem.configuredSpeedMT) {
            oss << " (rated " << mem.speedMT << " MT/s)";
        }
    } else if (mem.speedMT > 0) {
        oss << " @ " << mem.speedMT << " MT/s";
    }

    if (mem.totalSticks == 0) {
        oss << (mem.needsElevation ? ", SMBIOS memory table not readable without root" : ", no memory devices reported");
    } else {
        oss << ", " << mem.totalSticks << (mem.channelsUnverified ? " device" : " stick")
            << (mem.totalSticks != 1 ? "s" : "");
    }

    if (mem.channels > 0) {
        oss << ", ";
        if (mem.channels == 1) oss << "single";
        else if (mem.channels == 2) oss << "dual";
        else if (mem.channels == 4) oss << "quad";
        else oss << mem.channels;
        oss << "-channel";
        if (mem.busWidthBits > 0) oss << " (" << mem.busWidthBits << "-bit)";
        if (mem.channelsInferred) oss << " (inferred from measured bandwidth)";
        else if (mem.channelsUnverified) oss << " (SMBIOS estimate)";
    } else if (mem.totalSticks == 0) {
        oss << ", channels unknown";
    }

    if (mem.theoreticalBandwidth > 0) {
        oss << " (~" << std::fixed << std::setprecision(1) << mem.theoreticalBandwidth << " GB/s theoretical)";
    }

    if (mem.latencyEstimated) {
        oss << " | Est. Chip Latency: ~" << std::fixed << std::setprecision(1)
            << mem.ratedLatencyNs << " ns (~CL" << mem.estimatedCL << " typical for speed tier)";
    }

    return oss.str();
}

void EstimateRatedLatency(SystemMemoryInfo& mem) {
    if (mem.speedMT == 0 && mem.configuredSpeedMT == 0) return;
    uint32_t speed = mem.configuredSpeedMT > 0 ? mem.configuredSpeedMT : mem.speedMT;
    int bestIdx = -1;
    uint32_t bestDiff = UINT32_MAX;
    for (int i = 0; i < NUM_MEMORY_LATENCY_ENTRIES; i++) {
        if (!mem.type.empty() && mem.type.find(MEMORY_LATENCY_TABLE[i].type) == std::string::npos)
            continue;
        uint32_t diff = (speed > MEMORY_LATENCY_TABLE[i].speedMT)
            ? speed - MEMORY_LATENCY_TABLE[i].speedMT
            : MEMORY_LATENCY_TABLE[i].speedMT - speed;
        if (diff < bestDiff) { bestDiff = diff; bestIdx = i; }
    }
    if (bestIdx < 0) {
        for (int i = 0; i < NUM_MEMORY_LATENCY_ENTRIES; i++) {
            uint32_t diff = (speed > MEMORY_LATENCY_TABLE[i].speedMT)
                ? speed - MEMORY_LATENCY_TABLE[i].speedMT
                : MEMORY_LATENCY_TABLE[i].speedMT - speed;
            if (diff < bestDiff) { bestDiff = diff; bestIdx = i; }
        }
    }
    if (bestIdx >= 0) {
        mem.estimatedCL = MEMORY_LATENCY_TABLE[bestIdx].typicalCL;
        if (bestDiff == 0) {
            mem.ratedLatencyNs = MEMORY_LATENCY_TABLE[bestIdx].latencyNs;
        } else {
            // latency_ns = CL * clock_period_ns, clock_period_ns = 2000 / MT/s
            mem.ratedLatencyNs = static_cast<double>(mem.estimatedCL) * 2000.0 / static_cast<double>(speed);
        }
        mem.latencyEstimated = true;
    }
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================
std::string GetVendorName(uint32_t vendorId) {
    switch (vendorId) {
    case 0x10DE: return "NVIDIA";
    case 0x1002: return "AMD";
    case 0x8086: return "Intel";
    case 0x1414: return "Microsoft";
    default:     return "Unknown";
    }
}

std::string FormatSize(size_t bytes) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(0);
    
    if (bytes >= 1024ULL * 1024 * 1024) {
        double gb = bytes / (1024.0 * 1024.0 * 1024.0);
        if (gb >= 10.0) {
            oss << static_cast<int>(gb) << " GB";
        } else {
            oss << std::setprecision(1) << gb << " GB";
        }
    } else if (bytes >= 1024ULL * 1024) {
        double mb = bytes / (1024.0 * 1024.0);
        if (mb >= 10.0) {
            oss << static_cast<int>(mb) << " MB";
        } else {
            oss << std::setprecision(1) << mb << " MB";
        }
    } else if (bytes >= 1024) {
        oss << (bytes / 1024) << " KB";
    } else {
        oss << bytes << " B";
    }
    
    return oss.str();
}

std::string FormatMemory(size_t bytes) {
    double gb = bytes / (1024.0 * 1024.0 * 1024.0);
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << gb << " GB";
    return ss.str();
}

std::string FormatVendorDeviceId(uint32_t vendorId, uint32_t deviceId) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0');
    ss << "0x" << std::setw(4) << vendorId << ":0x" << std::setw(4) << deviceId;
    return ss.str();
}

// Check if global benchmark timeout has been exceeded
bool IsGlobalTimeoutExceeded() {
    auto elapsed = std::chrono::steady_clock::now() - g_app.benchmarkStartTime;
    return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > 
           Constants::GLOBAL_BENCHMARK_TIMEOUT_MS;
}

// Find closest interface standard and calculate percentage
// Find the interface standard to compare a measurement against.
//
// tunneledExternal (GPU confirmed behind a TB/USB4 PCIe tunnel): only the
// tunneling tiers are candidates, and we pick the SMALLEST tier the
// measurement plausibly fits under (measured <= achievable * 1.15). A
// measurement that exceeds a tier's achievable bandwidth by more than ~15%
// physically disproves that tier - reporting "134% of USB4 40Gbps" for a
// 5.35 GB/s link is wrong; that number is only possible on an 80 Gbps-class
// link. If the measurement disproves every tier, compare against the largest.
//
// Internal GPUs: nearest non-tunneled standard by absolute difference
// (original behavior, minus the nonsensical TB/USB4 candidates).
void FindClosestInterface(double measured, bool tunneledExternal,
                          std::string& outName, double& outPercentage) {
    if (measured <= 0) { outName = "Unknown"; outPercentage = 0; return; }

    if (tunneledExternal) {
        const InterfaceSpeed* fit = nullptr;
        const InterfaceSpeed* largest = nullptr;
        for (int i = 0; i < NUM_INTERFACE_SPEEDS; i++) {
            const InterfaceSpeed& e = INTERFACE_SPEEDS[i];
            if (!e.tunneledExternal) continue;
            if (!largest || e.bandwidth > largest->bandwidth) largest = &e;
            if (measured <= e.bandwidth * 1.15) {
                if (!fit || e.bandwidth < fit->bandwidth) fit = &e;
            }
        }
        const InterfaceSpeed* best = fit ? fit : largest;
        if (best) {
            outName = best->name;
            outPercentage = (measured / best->bandwidth) * 100.0;
            return;
        }
        // No external entries in the table (shouldn't happen) - fall through
    }

    const InterfaceSpeed* best = nullptr;
    double bestDiff = 1e9;
    for (int i = 0; i < NUM_INTERFACE_SPEEDS; i++) {
        if (INTERFACE_SPEEDS[i].tunneledExternal) continue;
        double diff = std::abs(measured - INTERFACE_SPEEDS[i].bandwidth);
        double ratio = measured / INTERFACE_SPEEDS[i].bandwidth;
        // Only consider if within reasonable range (30% to 200% of standard)
        if (ratio >= 0.3 && ratio <= 2.0 && diff < bestDiff) {
            best = &INTERFACE_SPEEDS[i];
            bestDiff = diff;
        }
    }

    if (best) {
        outName = best->name;
        outPercentage = (measured / best->bandwidth) * 100.0;
    } else if (measured > 50) {
        // Faster than any standard - compare to PCIe 5.0 x16 realistic bandwidth
        outName = "PCIe 5.0 x16";
        outPercentage = (measured / 54.40) * 100.0;  // Realistic PCIe 5.0 x16 bandwidth
    } else {
        outName = "Unknown";
        outPercentage = 0;
    }
}

// Detect interface type for integrated GPUs (APUs)
// These don't use PCIe - they share system memory via the CPU's memory controller
void DetectIntegratedGPUInterface(double upload, double download, const GPUInfo& gpu) {
    g_app.uploadBW = upload;
    g_app.downloadBW = download;
    
    // Determine memory type based on vendor and bandwidth
    std::string memoryType;
    std::string fabricType;
    
    double maxBandwidth = std::max(upload, download);
    
    // AMD APUs use Infinity Fabric, Intel uses ring bus/mesh
    if (gpu.vendor == "AMD") {
        fabricType = "AMD Infinity Fabric";
        // Estimate DDR generation from bandwidth (dual-channel realistic values)
        // DDR4-3200: ~41 GB/s    DDR5-5600: ~72 GB/s    DDR5-8800: ~113 GB/s
        // DDR6-12800: ~164 GB/s  DDR6-17600: ~225 GB/s
        if (maxBandwidth > 120) {
            memoryType = "DDR6";
        } else if (maxBandwidth > 60) {
            memoryType = "DDR5";
        } else if (maxBandwidth > 35) {
            memoryType = "DDR4/DDR5";
        } else {
            memoryType = "DDR4";
        }
    } else if (gpu.vendor == "Intel") {
        fabricType = "Intel Ring Bus / Mesh";
        if (maxBandwidth > 120) {
            memoryType = "DDR6";
        } else if (maxBandwidth > 50) {
            memoryType = "DDR5";
        } else {
            memoryType = "DDR4";
        }
    } else {
        fabricType = "On-die Interconnect";
        if (maxBandwidth > 120) {
            memoryType = "DDR6";
        } else if (maxBandwidth > 50) {
            memoryType = "DDR5";
        } else {
            memoryType = "System Memory";
        }
    }
    
    g_app.detectedInterface = "Integrated GPU (Shared Memory)";
    g_app.detectedInterfaceDescription = "UMA - CPU and GPU share " + memoryType + " via " + fabricType;
    
    // Store memory info for display
    g_app.integratedMemoryType = memoryType;
    g_app.integratedFabricType = fabricType;
    
    // For percentage display, compare against actual system memory bandwidth if detected
    // Otherwise fall back to typical DDR bandwidth estimates
    double expectedBandwidth;
    std::string bandwidthSource;
    
    if (g_app.systemMemory.detected && g_app.systemMemory.theoreticalBandwidth > 0) {
        // Use detected RAM bandwidth with ~80% efficiency factor
        expectedBandwidth = g_app.systemMemory.theoreticalBandwidth * 0.80;

        // An iGPU cannot pull system RAM faster than the bus delivers it, so a
        // measurement above the estimate proves the channel count is too low
        // (soldered LPDDR firmware often describes the whole bus as one
        // device). Raise the estimate to the smallest power-of-two channel
        // count that can carry the measurement and say so in the log.
        {
            uint32_t memSpeed = g_app.systemMemory.configuredSpeedMT > 0 ?
                g_app.systemMemory.configuredSpeedMT : g_app.systemMemory.speedMT;
            double perChannelGBs = memSpeed * 8.0 / 1000.0;   // one 64-bit channel
            uint32_t oldChannels = g_app.systemMemory.channels > 0 ? g_app.systemMemory.channels : 1;
            if (perChannelGBs > 0 && maxBandwidth > expectedBandwidth) {
                uint32_t ch = oldChannels;
                while (ch < 16 && maxBandwidth > perChannelGBs * ch * 0.80) ch *= 2;
                if (ch != oldChannels) {
                    char msg[320];
                    snprintf(msg, sizeof(msg),
                        "[INFO] Measured %.1f GB/s exceeds the %.1f GB/s achievable for %u-channel RAM - "
                        "SMBIOS under-reports the memory bus (typical for soldered LPDDR); "
                        "assuming %u channels (%u-bit, %.1f GB/s theoretical)",
                        maxBandwidth, expectedBandwidth, oldChannels, ch, ch * 64u, perChannelGBs * ch);
                    Log(msg);
                    g_app.systemMemory.channels = ch;
                    g_app.systemMemory.busWidthBits = ch * 64u;
                    g_app.systemMemory.theoreticalBandwidth = perChannelGBs * ch;
                    g_app.systemMemory.channelsInferred = true;
                    expectedBandwidth = g_app.systemMemory.theoreticalBandwidth * 0.80;
                }
            }
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%s @ %u MT/s (%.0f GB/s)", 
                g_app.systemMemory.type.c_str(),
                g_app.systemMemory.configuredSpeedMT > 0 ? 
                    g_app.systemMemory.configuredSpeedMT : g_app.systemMemory.speedMT,
                expectedBandwidth);
        bandwidthSource = buf;
    } else {
        // Fall back to estimates based on detected/heuristic memory type
        // DDR6 dual-channel realistic: ~164 GB/s (DDR6-12800 base)
        // DDR5 dual-channel realistic: ~70 GB/s
        // DDR4 dual-channel realistic: ~40 GB/s
        if (memoryType.find("DDR6") != std::string::npos) {
            expectedBandwidth = 164.0;
        } else if (memoryType.find("DDR5") != std::string::npos) {
            expectedBandwidth = 70.0;
        } else {
            expectedBandwidth = 40.0;
        }
        bandwidthSource = memoryType + " (estimated)";
    }
    
    g_app.closestUploadStandard = bandwidthSource;
    g_app.uploadPercentage = (upload / expectedBandwidth) * 100.0;
    g_app.closestDownloadStandard = bandwidthSource;
    g_app.downloadPercentage = (download / expectedBandwidth) * 100.0;
}

void DetectInterface(double upload, double download, int gpuIndex) {
    const GPUInfo& gpu = g_app.gpuList[gpuIndex];
    
    // For integrated GPUs, don't try to match PCIe standards - it's meaningless
    if (gpu.isIntegrated) {
        DetectIntegratedGPUInterface(upload, download, gpu);
        return;
    }
    
    // Discrete GPU - detect PCIe/Thunderbolt interface
    double measured = std::max(upload, download);
    bool tunneled = gpu.isThunderbolt || gpu.isUSB4 || gpu.isUSB;

    if (tunneled) {
        // Confirmed TB/USB4 PCIe tunnel: classify against tunneling tiers only,
        // using the fit-under rule (a tier the measurement exceeds is disproven).
        double pct = 0;
        FindClosestInterface(measured, true, g_app.detectedInterface, pct);
        g_app.detectedInterfaceDescription = "External GPU over TB/USB4 PCIe tunnel";
        for (int i = 0; i < NUM_INTERFACE_SPEEDS; i++) {
            if (g_app.detectedInterface == INTERFACE_SPEEDS[i].name) {
                g_app.detectedInterfaceDescription = INTERFACE_SPEEDS[i].description;
                break;
            }
        }
    } else {
        const InterfaceSpeed* best = nullptr;
        double bestDiff = 1e9;

        for (int i = 0; i < NUM_INTERFACE_SPEEDS; i++) {
            if (INTERFACE_SPEEDS[i].tunneledExternal) continue;  // internal GPU: PCIe/OCuLink only
            double diff = std::abs(measured - INTERFACE_SPEEDS[i].bandwidth);
            double ratio = measured / INTERFACE_SPEEDS[i].bandwidth;
            // More restrictive for detection (50% to 120% of standard)
            if (ratio >= 0.5 && ratio <= 1.2 && diff < bestDiff) {
                best = &INTERFACE_SPEEDS[i];
                bestDiff = diff;
            }
        }

        if (best) {
            g_app.detectedInterface = best->name;
            g_app.detectedInterfaceDescription = best->description;
        } else if (measured > 50) {
            g_app.detectedInterface = "PCIe 5.0 x16 (or faster)";
            g_app.detectedInterfaceDescription = "High-performance discrete GPU";
        } else {
            g_app.detectedInterface = "Unknown";
            g_app.detectedInterfaceDescription = "";
        }
    }

    g_app.uploadBW = upload;
    g_app.downloadBW = download;

    // Calculate percentages vs closest standards (external tiers for eGPUs)
    FindClosestInterface(upload, tunneled, g_app.closestUploadStandard, g_app.uploadPercentage);
    FindClosestInterface(download, tunneled, g_app.closestDownloadStandard, g_app.downloadPercentage);
}

// Detect if this is an eGPU - prefer hardware detection, fall back to bandwidth heuristic
void DetectEGPU(double upload, double download, const GPUInfo& gpu) {
    g_app.possibleEGPU = false;
    g_app.eGPUConnectionType.clear();
    
    // iGPUs are never eGPUs
    if (gpu.isIntegrated) return;
    
    // First, check if hardware detection found Thunderbolt/USB4/USB connection
    if (gpu.isThunderbolt || gpu.isUSB4 || gpu.isUSB) {
        g_app.possibleEGPU = true;
        g_app.eGPUConnectionType = gpu.externalConnectionType;
        // The sysfs thunderbolt subsystem doesn't reliably expose the
        // negotiated USB4 link rate, but measured bandwidth can disprove a
        // 40 Gbps link: its PCIe tunnel caps at 32 Gbps (~3.5-4 GB/s real).
        // Anything clearly above that is an 80 Gbps-class link (Thunderbolt 5
        // / USB4v2) - annotate so users see an accurate link class.
        double maxBw = std::max(upload, download);
        if (maxBw > Constants::TB4_MAX_BANDWIDTH) {
            g_app.eGPUConnectionType += " - 80 Gbps-class (TB5/USB4v2, by measured bandwidth)";
        }
        // Don't log here - it was already logged during enumeration
        return;
    }

    // Fallback: Use bandwidth heuristic for cases where hardware detection failed
    double maxBandwidth = std::max(upload, download);

    // If a discrete GPU has suspiciously low bandwidth, it might be external
    if (maxBandwidth < Constants::EGPU_BANDWIDTH_THRESHOLD) {
        g_app.possibleEGPU = true;

        // Identify the connection type from bandwidth
        // Don't add "inferred" qualifier - the eGPU detection itself is the confirmation
        if (maxBandwidth <= Constants::TB3_MAX_BANDWIDTH) {
            g_app.eGPUConnectionType = "Thunderbolt 3 / USB4 40Gbps";
        } else if (maxBandwidth <= Constants::TB4_MAX_BANDWIDTH) {
            g_app.eGPUConnectionType = "Thunderbolt 4 / USB4 40Gbps";
        } else if (maxBandwidth <= Constants::TB5_MAX_BANDWIDTH) {
            g_app.eGPUConnectionType = "Thunderbolt 5 / USB4 80Gbps";
        } else {
            g_app.eGPUConnectionType = "External (OCuLink / beyond 80 Gbps-class)";
        }

        Log("[INFO] eGPU detected - bandwidth indicates " + g_app.eGPUConnectionType);
    }
}

// ============================================================================
// PCIe LINK DETECTION (Linux sysfs)
// ============================================================================

// ReadSysfsFile() is defined in the LINUX HELPER FUNCTIONS section above

// Map PCIe link speed string (GT/s) to generation number
// sysfs current_link_speed format: "8.0 GT/s PCIe" or "8 GT/s"
static int ParsePCIeLinkSpeedToGen(const std::string& speedStr) {
    if (speedStr.empty()) return 0;
    // Extract the numeric GT/s value from the beginning of the string
    double gts = 0.0;
    try {
        gts = std::stod(speedStr);
    } catch (...) {
        return 0;
    }
    // Map GT/s to PCIe generation
    if (gts >= 62.0) return 6;  // 64.0 GT/s = Gen6
    if (gts >= 30.0) return 5;  // 32.0 GT/s = Gen5
    if (gts >= 14.0) return 4;  // 16.0 GT/s = Gen4
    if (gts >=  7.0) return 3;  //  8.0 GT/s = Gen3
    if (gts >=  4.0) return 2;  //  5.0 GT/s = Gen2
    if (gts >=  2.0) return 1;  //  2.5 GT/s = Gen1
    return 0;
}

// Parse link width string (e.g., "16" or "x16") to integer lane count
static int ParsePCIeLinkWidth(const std::string& widthStr) {
    if (widthStr.empty()) return 0;
    // Skip leading 'x' if present
    const char* p = widthStr.c_str();
    if (*p == 'x' || *p == 'X') p++;
    try {
        return std::stoi(p);
    } catch (...) {
        return 0;
    }
}

// Read PCIe link info from a known sysfs device path (e.g., "/sys/bus/pci/devices/0000:01:00.0")
// This is used when the PCI BDF address is known (e.g., from VK_EXT_pci_bus_info)
bool DetectPCIeLinkBySysfsPath(const std::string& sysfsPath, GPUInfo& outInfo) {
    outInfo.pcieInfoValid = false;

    // Verify the path exists
    struct stat st;
    if (stat(sysfsPath.c_str(), &st) != 0) {
        char logBuf[512];
        snprintf(logBuf, sizeof(logBuf), "[DEBUG] sysfs path not found: %s", sysfsPath.c_str());
        Log(logBuf);
        return false;
    }

    // Read current link speed and width
    std::string currentSpeed = ReadSysfsFile(sysfsPath + "/current_link_speed");
    std::string currentWidth = ReadSysfsFile(sysfsPath + "/current_link_width");
    std::string maxSpeed     = ReadSysfsFile(sysfsPath + "/max_link_speed");
    std::string maxWidth     = ReadSysfsFile(sysfsPath + "/max_link_width");

    outInfo.pcieGenCurrent  = ParsePCIeLinkSpeedToGen(currentSpeed);
    outInfo.pcieLanesCurrent = ParsePCIeLinkWidth(currentWidth);
    outInfo.pcieGenMax       = ParsePCIeLinkSpeedToGen(maxSpeed);
    outInfo.pcieLanesMax     = ParsePCIeLinkWidth(maxWidth);

    // Extract PCI BDF address from the sysfs path (last component, e.g., "0000:01:00.0")
    size_t lastSlash = sysfsPath.rfind('/');
    if (lastSlash != std::string::npos) {
        outInfo.pcieLocationPath = sysfsPath.substr(lastSlash + 1);
    } else {
        outInfo.pcieLocationPath = sysfsPath;
    }

    if (outInfo.pcieGenCurrent > 0 && outInfo.pcieLanesCurrent > 0) {
        outInfo.pcieInfoValid = true;

        char logBuf[256];
        snprintf(logBuf, sizeof(logBuf),
            "[INFO] Detected PCIe Gen%d x%d (Max: Gen%d x%d) for %s via sysfs",
            outInfo.pcieGenCurrent, outInfo.pcieLanesCurrent,
            outInfo.pcieGenMax, outInfo.pcieLanesMax,
            outInfo.name.c_str());
        Log(logBuf);
    }

    return outInfo.pcieInfoValid;
}

// Query PCIe link information for a GPU by scanning sysfs for matching vendor/device IDs
bool DetectPCIeLink(uint32_t vendorId, uint32_t deviceId, GPUInfo& outInfo) {
    outInfo.pcieInfoValid = false;

    const std::string sysfsBase = "/sys/bus/pci/devices";
    DIR* dir = opendir(sysfsBase.c_str());
    if (!dir) {
        Log("[DEBUG] Failed to open /sys/bus/pci/devices");
        return false;
    }

    // Format target vendor/device as sysfs hex strings (e.g., "0x10de", "0x2782")
    char targetVendor[16], targetDevice[16];
    snprintf(targetVendor, sizeof(targetVendor), "0x%04x", vendorId);
    snprintf(targetDevice, sizeof(targetDevice), "0x%04x", deviceId);

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Skip . and ..
        if (entry->d_name[0] == '.') continue;

        std::string devPath = sysfsBase + "/" + entry->d_name;

        // Read vendor and device files
        std::string vendor = ReadSysfsFile(devPath + "/vendor");
        std::string device = ReadSysfsFile(devPath + "/device");

        if (vendor != targetVendor || device != targetDevice)
            continue;

        // Found matching device - read PCIe link info
        char logBuf[512];
        snprintf(logBuf, sizeof(logBuf), "[DEBUG] Found PCI device %s at %s",
            entry->d_name, devPath.c_str());
        Log(logBuf);

        closedir(dir);
        return DetectPCIeLinkBySysfsPath(devPath, outInfo);
    }

    closedir(dir);

    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf),
        "[DEBUG] PCI device %04x:%04x not found in sysfs", vendorId, deviceId);
    Log(logBuf);
    return false;
}

// Format PCIe config as string (e.g., "PCIe 4.0 x16")
std::string FormatPCIeConfig(int gen, int lanes) {
    if (gen <= 0 || lanes <= 0) return "Unknown";
    char buf[64];
    snprintf(buf, sizeof(buf), "PCIe %d.0 x%d", gen, lanes);
    return buf;
}

// ============================================================================
// THUNDERBOLT / USB4 / USB DETECTION
// ============================================================================

// Vendor IDs
static const uint32_t VENDOR_INTEL = 0x8086;
static const uint32_t VENDOR_AMD = 0x1022;
static const uint32_t VENDOR_ASMEDIA = 0x1B21;
static const uint32_t VENDOR_REALTEK = 0x10EC;
static const uint32_t VENDOR_VIA = 0x1106;
static const uint32_t VENDOR_RENESAS = 0x1912;
static const uint32_t VENDOR_FRESCO = 0x1B73;  // Fresco Logic (now Cadence)
static const uint32_t VENDOR_ETRON = 0x1B6F;
static const uint32_t VENDOR_TEXAS = 0x104C;   // Texas Instruments

// Known Intel Thunderbolt controller device IDs (Vendor 0x8086)
static const uint32_t INTEL_TB3_DEVICE_IDS[] = {
    0x15D2, 0x15D9, 0x15DA, 0x15DB, 0x15DC, 0x15DD, 0x15DE, 0x15DF,  // Alpine Ridge
    0x15E7, 0x15E8, 0x15EA, 0x15EB, 0x15EC, 0x15EF,                  // Titan Ridge
    0x15BF, 0x15C0, 0x15C1,                                          // JHL6xxx
};

static const uint32_t INTEL_TB4_DEVICE_IDS[] = {
    0x9A1B, 0x9A1C, 0x9A1D, 0x9A1E, 0x9A1F,  // Tiger Lake TB4
    0x9A21, 0x9A23, 0x9A25, 0x9A27, 0x9A29,  // Tiger Lake TB4
    0xA73E, 0xA73F, 0xA76D, 0xA76E,          // Alder Lake TB4
    0x466D, 0x466E, 0x462E, 0x463E,          // Raptor Lake TB4
    0x7EB2, 0x7EB4, 0x7EC2, 0x7EC4,          // Meteor Lake TB4
    0xA0D3, 0xA0E3,                          // Ice Lake TB4
};

// Intel Thunderbolt/USB4 PCIe SWITCH device IDs (Vendor 0x8086)
// These are the PCIe switches INSIDE TB/USB4 tunneling chips and enclosures
// They appear in the device tree between the GPU and the root port
static const uint32_t INTEL_TB_PCIE_SWITCH_IDS[] = {
    // Goshen Ridge (USB4/TB4 PCIe switches) - found in USB4 eGPU enclosures
    0x5780, 0x5781, 0x5782, 0x5783, 0x5784, 0x5785, 0x5786, 0x5787,
    0x5788, 0x5789, 0x578A, 0x578B, 0x578C, 0x578D, 0x578E, 0x578F,
    
    // Titan Ridge PCIe switches
    0x15E9, 0x15EA, 0x15EB,
    
    // Alpine Ridge PCIe switches  
    0x1575, 0x1576, 0x1577, 0x1578, 0x1579,
    
    // JHL6xxx/JHL7xxx PCIe switches
    0x15D3, 0x15D4, 0x15D5,
    
    // Maple Ridge (TB4) PCIe switches
    0x1136, 0x1137,
};

// AMD USB4 controller device IDs (Vendor 0x1022)
// AMD implements USB4 in their chipsets starting with Ryzen 7000 series
static const uint32_t AMD_USB4_DEVICE_IDS[] = {
    0x162E, 0x162F,  // AMD Pink Sardine USB4
    0x163A, 0x163B, 0x163C, 0x163D,  // AMD USB4 Router
    0x164A, 0x164B, 0x164C, 0x164D,  // Ryzen 7000 series USB4
    0x14E9, 0x14EA,  // AMD Rembrandt USB4
    0x15B6, 0x15B7, 0x15B8, 0x15B9,  // Phoenix USB4
    0x1668, 0x1669,  // Hawk Point USB4
};

// ASMedia USB controllers (common in third-party USB cards/enclosures)
static const uint32_t ASMEDIA_USB_DEVICE_IDS[] = {
    0x2142,  // ASM2142 USB 3.1 Gen 2
    0x3242,  // ASM3242 USB 3.2 Gen 2x2
    0x3241,  // ASM3241 USB 3.2
    0x1242,  // ASM1242 USB 3.1
    0x1042,  // ASM1042 USB 3.0
    0x1142,  // ASM1142 USB 3.1
    0x2362,  // ASM2362 PCIe to USB 3.2 Bridge
    0x2364,  // ASM2364 USB4/TB4 Controller
    0x4242,  // ASM4242 USB4
};

// Intel USB xHCI controllers (for USB 3.x detection)
static const uint32_t INTEL_USB_XHCI_IDS[] = {
    0xA36D, 0xA2AF,  // 300 series
    0x8D31, 0x8C31,  // 100 series
    0x9D2F, 0x9DED,  // 100/200 series mobile
    0xA0ED, 0xA1ED,  // 500 series
    0x7A60, 0x7AE0,  // 600/700 series
    0x460E, 0x461E,  // Alder Lake
    0x7E7E, 0x7E7F,  // Meteor Lake
};

// Structure to hold detection result details
struct ExternalConnectionInfo {
    bool isExternal;
    bool isThunderbolt;
    bool isUSB4;
    bool isUSB3;
    int thunderboltVersion;  // 3 or 4, 0 if not TB
    int usbGeneration;       // 3, 4 (for USB 3.x, USB4)
    std::string connectionType;
    std::string controllerName;
};

// Check if a device ID is a Thunderbolt controller
bool IsThunderbolt3Controller(uint32_t vendorId, uint32_t deviceId) {
    if (vendorId != VENDOR_INTEL) return false;
    for (auto id : INTEL_TB3_DEVICE_IDS) {
        if (deviceId == id) return true;
    }
    return false;
}

bool IsThunderbolt4Controller(uint32_t vendorId, uint32_t deviceId) {
    if (vendorId != VENDOR_INTEL) return false;
    for (auto id : INTEL_TB4_DEVICE_IDS) {
        if (deviceId == id) return true;
    }
    return false;
}

// Check if a device is an Intel Thunderbolt/USB4 PCIe SWITCH
// These are found INSIDE eGPU enclosures - they're the PCIe fabric inside the tunnel
bool IsThunderboltPCIeSwitch(uint32_t vendorId, uint32_t deviceId) {
    if (vendorId != VENDOR_INTEL) return false;
    for (auto id : INTEL_TB_PCIE_SWITCH_IDS) {
        if (deviceId == id) return true;
    }
    return false;
}

// Check if a device ID is a USB4 controller (non-Thunderbolt branded)
bool IsUSB4Controller(uint32_t vendorId, uint32_t deviceId) {
    // AMD USB4 controllers
    if (vendorId == VENDOR_AMD) {
        for (auto id : AMD_USB4_DEVICE_IDS) {
            if (deviceId == id) return true;
        }
    }
    
    // ASMedia USB4 controllers
    if (vendorId == VENDOR_ASMEDIA) {
        if (deviceId == 0x2364 || deviceId == 0x4242) return true;
    }
    
    return false;
}

// Check if a device ID is a USB 3.x controller that could be used for external GPUs
bool IsUSB3Controller(uint32_t vendorId, uint32_t deviceId) {
    // Intel xHCI controllers
    if (vendorId == VENDOR_INTEL) {
        for (auto id : INTEL_USB_XHCI_IDS) {
            if (deviceId == id) return true;
        }
    }
    
    // ASMedia USB 3.x controllers
    if (vendorId == VENDOR_ASMEDIA) {
        for (auto id : ASMEDIA_USB_DEVICE_IDS) {
            if (deviceId == id) return true;
        }
    }
    
    // Fresco Logic USB 3.0 controllers
    if (vendorId == VENDOR_FRESCO) {
        return true;  // All Fresco Logic devices are USB controllers
    }
    
    // Renesas USB controllers
    if (vendorId == VENDOR_RENESAS) {
        return true;
    }
    
    // VIA USB controllers
    if (vendorId == VENDOR_VIA) {
        if (deviceId == 0x3483 || deviceId == 0x3432) return true;
    }
    
    // Etron USB controllers
    if (vendorId == VENDOR_ETRON) {
        return true;
    }
    
    return false;
}

// Get friendly name for USB controller
std::string GetUSBControllerName(uint32_t vendorId, uint32_t deviceId) {
    if (vendorId == VENDOR_AMD) {
        return "AMD USB4";
    } else if (vendorId == VENDOR_ASMEDIA) {
        if (deviceId == 0x2364 || deviceId == 0x4242) return "ASMedia USB4";
        if (deviceId == 0x3242) return "ASMedia USB 3.2 Gen 2x2";
        if (deviceId == 0x2142) return "ASMedia USB 3.1 Gen 2";
        return "ASMedia USB";
    } else if (vendorId == VENDOR_FRESCO) {
        return "Fresco Logic USB 3.0";
    } else if (vendorId == VENDOR_RENESAS) {
        return "Renesas USB 3.0";
    } else if (vendorId == VENDOR_INTEL) {
        return "Intel USB";
    }
    return "USB Controller";
}

// Helper: Read the target of a symlink, returning empty string on failure
static std::string ReadSymlinkTarget(const std::string& path) {
    char buf[PATH_MAX];
    ssize_t len = readlink(path.c_str(), buf, sizeof(buf) - 1);
    if (len < 0) return "";
    buf[len] = '\0';
    return std::string(buf);
}

// Helper: Resolve a sysfs path to its real (canonical) path
static std::string ResolveSysfsPath(const std::string& path) {
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved) == nullptr) return "";
    return std::string(resolved);
}

// Helper: Read PCI vendor/device IDs from a sysfs device directory
// Returns true if both were read successfully
static bool ReadSysfsPCIIds(const std::string& devPath, uint32_t& vendorId, uint32_t& deviceId) {
    std::string vendorStr = ReadSysfsFile(devPath + "/vendor");
    std::string deviceStr = ReadSysfsFile(devPath + "/device");
    if (vendorStr.empty() || deviceStr.empty()) return false;

    // sysfs vendor/device files contain hex values like "0x8086"
    try {
        vendorId = (uint32_t)std::stoul(vendorStr, nullptr, 16);
        deviceId = (uint32_t)std::stoul(deviceStr, nullptr, 16);
        return true;
    } catch (...) {
        return false;
    }
}

// Helper: Check if a sysfs device directory has a driver symlink containing a keyword
static bool SysfsDriverContains(const std::string& devPath, const std::string& keyword) {
    std::string driverLink = ReadSymlinkTarget(devPath + "/driver");
    if (driverLink.empty()) return false;

    // The driver symlink target looks like "../../../../bus/pci/drivers/thunderbolt"
    // We just need to check if the keyword appears anywhere in the path
    std::string driverLower = driverLink;
    std::string keywordLower = keyword;
    std::transform(driverLower.begin(), driverLower.end(), driverLower.begin(), [](char c) { return static_cast<char>(::tolower(static_cast<unsigned char>(c))); });
    std::transform(keywordLower.begin(), keywordLower.end(), keywordLower.begin(), [](char c) { return static_cast<char>(::tolower(static_cast<unsigned char>(c))); });
    return driverLower.find(keywordLower) != std::string::npos;
}

// Helper: Check if active Thunderbolt devices exist in the TB subsystem
static bool HasActiveThunderboltDevices() {
    const std::string tbPath = "/sys/bus/thunderbolt/devices";
    DIR* dir = opendir(tbPath.c_str());
    if (!dir) return false;

    bool found = false;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        std::string name(entry->d_name);
        // Thunderbolt domain devices look like "0-0", "0-1", "1-0", etc.
        // Domain controllers look like "domain0", "domain1"
        // We want actual connected devices (digit-digit pattern)
        if (name.size() >= 3 && isdigit(name[0]) && name[1] == '-' && isdigit(name[2])) {
            // Verify it has a device_name file (indicates a real connected device)
            std::string deviceName = ReadSysfsFile(tbPath + "/" + name + "/device_name");
            if (!deviceName.empty()) {
                found = true;
                break;
            }
        }
    }
    closedir(dir);
    return found;
}

// Helper: Find the sysfs path for a GPU given its PCI vendor:device IDs
// Uses pcieLocationPath (BDF address like "0000:01:00.0") if available,
// otherwise scans /sys/bus/pci/devices/ for a matching vendor:device
static std::string FindGPUSysfsPath(uint32_t gpuVendorId, uint32_t gpuDeviceId, const GPUInfo& info) {
    const std::string sysfsBase = "/sys/bus/pci/devices";

    // If we have a BDF address from PCIe link detection, use it directly
    if (!info.pcieLocationPath.empty()) {
        std::string directPath = sysfsBase + "/" + info.pcieLocationPath;
        struct stat st;
        if (stat(directPath.c_str(), &st) == 0) {
            return directPath;
        }
    }

    // Fall back to scanning for matching vendor:device
    char targetVendor[16], targetDevice[16];
    snprintf(targetVendor, sizeof(targetVendor), "0x%04x", gpuVendorId);
    snprintf(targetDevice, sizeof(targetDevice), "0x%04x", gpuDeviceId);

    DIR* dir = opendir(sysfsBase.c_str());
    if (!dir) return "";

    std::string result;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        std::string devPath = sysfsBase + "/" + entry->d_name;
        std::string vendor = ReadSysfsFile(devPath + "/vendor");
        std::string device = ReadSysfsFile(devPath + "/device");

        if (vendor == targetVendor && device == targetDevice) {
            result = devPath;
            break;
        }
    }
    closedir(dir);
    return result;
}

// Detect if a GPU is connected via Thunderbolt/USB4/USB by walking the sysfs device tree
void DetectExternalConnection(uint32_t gpuVendorId, uint32_t gpuDeviceId, GPUInfo& outInfo) {
    outInfo.isThunderbolt = false;
    outInfo.isUSB4 = false;
    outInfo.isUSB = false;
    outInfo.thunderboltVersion = 0;
    outInfo.externalConnectionType.clear();

    // ====== Step 1: Find the GPU's sysfs path ======
    std::string gpuSysfsPath = FindGPUSysfsPath(gpuVendorId, gpuDeviceId, outInfo);
    if (gpuSysfsPath.empty()) {
        #ifdef DEBUG_EXTERNAL_DETECTION
        Log("[DEBUG] Could not find GPU in sysfs for external connection detection");
        #endif
        return;
    }

    // Resolve to canonical path so we can walk up with ".."
    std::string gpuRealPath = ResolveSysfsPath(gpuSysfsPath);
    if (gpuRealPath.empty()) {
        gpuRealPath = gpuSysfsPath;  // Fall back to original path
    }

    #ifdef DEBUG_EXTERNAL_DETECTION
    Log("[DEBUG] Starting external connection detection for GPU at: " + gpuRealPath);
    #endif

    // ====== Step 2: Check Thunderbolt subsystem ======
    // If there are active Thunderbolt devices, this system has TB connections.
    // We'll confirm the GPU is behind one by walking the device tree below.
    bool tbSubsystemActive = HasActiveThunderboltDevices();

    #ifdef DEBUG_EXTERNAL_DETECTION
    Log("[DEBUG] Thunderbolt subsystem active: " + std::string(tbSubsystemActive ? "yes" : "no"));
    #endif

    // ====== Step 3: Walk up the PCI device tree ======
    // In sysfs, a PCI device's parent is found by going up directories.
    // Example path: /sys/devices/pci0000:00/0000:00:01.0/0000:01:00.0
    // Walking up from the GPU goes through bridge devices toward the root complex.

    bool foundUSB3 = false;
    std::string usb3ControllerName;

    std::string currentPath = gpuRealPath;
    int depth = 0;
    const int maxDepth = 15;

    while (depth < maxDepth) {
        // Move to parent directory
        size_t lastSlash = currentPath.rfind('/');
        if (lastSlash == std::string::npos || lastSlash == 0) {
            break;  // Reached filesystem root
        }
        currentPath = currentPath.substr(0, lastSlash);
        depth++;

        // Extract the basename of this directory (e.g., "0000:00:01.0" or "pci0000:00")
        std::string basename;
        size_t parentSlash = currentPath.rfind('/');
        if (parentSlash != std::string::npos) {
            basename = currentPath.substr(parentSlash + 1);
        } else {
            basename = currentPath;
        }

        // Stop if we've left the device tree (hit /sys/devices or above)
        if (basename == "devices" || basename == "sys" || basename.empty()) {
            break;
        }

        #ifdef DEBUG_EXTERNAL_DETECTION
        Log("[DEBUG] Depth " + std::to_string(depth) + ": " + currentPath + " (basename: " + basename + ")");
        #endif

        // ====== Check if this is a PCI device (has vendor/device files) ======
        uint32_t parentVendor = 0, parentDevice = 0;
        bool hasPCIIds = ReadSysfsPCIIds(currentPath, parentVendor, parentDevice);

        if (hasPCIIds) {
            #ifdef DEBUG_EXTERNAL_DETECTION
            char dbgBuf[128];
            snprintf(dbgBuf, sizeof(dbgBuf), "[DEBUG]   PCI IDs: %04X:%04X", parentVendor, parentDevice);
            Log(dbgBuf);
            #endif

            // ---- Check driver symlink for "thunderbolt" ----
            if (SysfsDriverContains(currentPath, "thunderbolt")) {
                outInfo.isThunderbolt = true;
                outInfo.isUSB = true;
                // Try to determine version from PCI IDs
                if (IsThunderbolt4Controller(parentVendor, parentDevice)) {
                    outInfo.thunderboltVersion = 4;
                    outInfo.isUSB4 = true;
                    outInfo.externalConnectionType = "Thunderbolt 4";
                } else if (IsThunderbolt3Controller(parentVendor, parentDevice)) {
                    outInfo.thunderboltVersion = 3;
                    outInfo.externalConnectionType = "Thunderbolt 3";
                } else {
                    // Driver says thunderbolt but PCI ID not in our tables
                    // Try to guess version from subsystem active state
                    outInfo.externalConnectionType = "Thunderbolt";
                }
                return;
            }

            // ---- Check for Intel Thunderbolt controllers by PCI ID (highest priority) ----
            if (IsThunderbolt4Controller(parentVendor, parentDevice)) {
                outInfo.isThunderbolt = true;
                outInfo.isUSB4 = true;
                outInfo.isUSB = true;
                outInfo.thunderboltVersion = 4;
                outInfo.externalConnectionType = "Thunderbolt 4";
                return;
            }
            if (IsThunderbolt3Controller(parentVendor, parentDevice)) {
                outInfo.isThunderbolt = true;
                outInfo.isUSB = true;
                outInfo.thunderboltVersion = 3;
                outInfo.externalConnectionType = "Thunderbolt 3";
                return;
            }

            // ---- Check for Intel TB/USB4 PCIe SWITCHES (inside eGPU enclosures) ----
            if (IsThunderboltPCIeSwitch(parentVendor, parentDevice)) {
                outInfo.isUSB4 = true;
                outInfo.isUSB = true;
                outInfo.externalConnectionType = "USB4 / Thunderbolt (PCIe Tunnel)";
                #ifdef DEBUG_EXTERNAL_DETECTION
                char dbgBuf2[128];
                snprintf(dbgBuf2, sizeof(dbgBuf2), "[DEBUG] Found Intel TB/USB4 PCIe Switch: %04X:%04X", parentVendor, parentDevice);
                Log(dbgBuf2);
                #endif
                return;
            }

            // ---- Check for USB4 controllers (AMD, ASMedia, etc.) ----
            if (IsUSB4Controller(parentVendor, parentDevice)) {
                outInfo.isUSB4 = true;
                outInfo.isUSB = true;
                std::string controllerName = GetUSBControllerName(parentVendor, parentDevice);
                outInfo.externalConnectionType = controllerName + " (PCIe Tunneling)";
                return;
            }

            // ---- Check driver symlink for "xhci" or "usb" ----
            if (SysfsDriverContains(currentPath, "xhci") ||
                SysfsDriverContains(currentPath, "usb")) {
                if (!foundUSB3) {
                    foundUSB3 = true;
                    usb3ControllerName = GetUSBControllerName(parentVendor, parentDevice);
                    if (usb3ControllerName == "USB Controller") {
                        usb3ControllerName = "USB";
                    }
                }
            }

            // ---- Check for USB 3.x controllers by PCI ID (track but keep looking for TB/USB4) ----
            if (IsUSB3Controller(parentVendor, parentDevice)) {
                if (!foundUSB3) {
                    foundUSB3 = true;
                    usb3ControllerName = GetUSBControllerName(parentVendor, parentDevice);
                }
            }
        }

        // ====== Check for thunderbolt in the path itself ======
        // Some sysfs paths include "thunderbolt" in directory names
        std::string basenameLower = basename;
        std::transform(basenameLower.begin(), basenameLower.end(), basenameLower.begin(), [](char c) { return static_cast<char>(::tolower(static_cast<unsigned char>(c))); });
        if (basenameLower.find("thunderbolt") != std::string::npos) {
            outInfo.isThunderbolt = true;
            outInfo.isUSB = true;
            outInfo.externalConnectionType = "Thunderbolt";
            // Try to refine version if we haven't returned yet
            if (outInfo.thunderboltVersion == 0 && tbSubsystemActive) {
                // Check the TB subsystem for version hints
                outInfo.externalConnectionType = "Thunderbolt";
            }
            return;
        }
    }

    // ====== Step 4: Check USB4/Type-C subsystem ======
    // /sys/class/typec/ contains USB Type-C port information
    // If the GPU wasn't found behind a TB controller in the PCI tree,
    // check if there are USB4-capable Type-C ports that might indicate
    // a USB4 connection
    {
        const std::string typecPath = "/sys/class/typec";
        DIR* dir = opendir(typecPath.c_str());
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (entry->d_name[0] == '.') continue;

                std::string portPath = typecPath + "/" + entry->d_name;

                // Check if this Type-C port supports USB4
                // The "usb4_version" file exists on USB4-capable ports
                std::string usb4Version = ReadSysfsFile(portPath + "/usb4_version");
                if (!usb4Version.empty()) {
                    #ifdef DEBUG_EXTERNAL_DETECTION
                    Log("[DEBUG] Found USB4 Type-C port: " + std::string(entry->d_name) +
                        " version: " + usb4Version);
                    #endif

                    // If TB subsystem is also active and we haven't detected it yet,
                    // it could be a TB-over-USB4 connection
                    if (tbSubsystemActive) {
                        outInfo.isThunderbolt = true;
                        outInfo.isUSB4 = true;
                        outInfo.isUSB = true;
                        outInfo.thunderboltVersion = 4;
                        outInfo.externalConnectionType = "Thunderbolt 4 (USB4)";
                        closedir(dir);
                        return;
                    }
                }
            }
            closedir(dir);
        }
    }

    // ====== Step 5: Cross-reference with TB subsystem ======
    // If the TB subsystem has active devices but we didn't find a TB controller
    // in the direct PCI ancestry, the GPU might still be behind a TB connection
    // if it's in a different PCI domain that was tunneled
    if (tbSubsystemActive && !outInfo.isThunderbolt) {
        // Check if the GPU's PCI domain/bus suggests it might be tunneled
        // Thunderbolt-tunneled devices often appear on higher-numbered PCI buses
        // This is a heuristic - not definitive
        #ifdef DEBUG_EXTERNAL_DETECTION
        Log("[DEBUG] TB subsystem active but no TB controller found in GPU ancestry");
        #endif
    }

    // If we found USB3 but not TB/USB4, report it
    // This is unusual for GPUs (USB3 doesn't normally support PCIe tunneling)
    // but some exotic setups might use it
    if (foundUSB3) {
        outInfo.isUSB = true;
        outInfo.externalConnectionType = usb3ControllerName + " (External - Unusual)";
        // Note: We don't set isUSB4 or isThunderbolt for USB3
        // USB3 doesn't support PCIe tunneling in the standard, but we detected
        // the GPU is somehow connected through a USB controller
    }
}

// Wrapper for backward compatibility - calls the new function
void DetectThunderboltConnection(uint32_t gpuVendorId, uint32_t gpuDeviceId, GPUInfo& outInfo) {
    DetectExternalConnection(gpuVendorId, gpuDeviceId, outInfo);
}

// ============================================================================
// GPU ENUMERATION (Vulkan)
// ============================================================================
void EnumerateGPUs() {
    g_app.gpuList.clear();

    // We need a temporary VkInstance to enumerate physical devices
    // If g_app.instance is already created, use that
    VkInstance enumInstance = g_app.instance;
    bool createdTempInstance = false;
    
    if (enumInstance == VK_NULL_HANDLE) {
        // This shouldn't happen in normal flow (InitVulkan creates instance first)
        // but handle it gracefully
        VkApplicationInfo appInfo = {};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "GPU-PCIe-Test";
        appInfo.applicationVersion = VK_MAKE_VERSION(3, 4, 2);
        appInfo.pEngineName = "No Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;

        uint32_t glfwExtCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtCount);
        createInfo.enabledExtensionCount = glfwExtCount;
        createInfo.ppEnabledExtensionNames = glfwExtensions;

        if (vkCreateInstance(&createInfo, nullptr, &enumInstance) != VK_SUCCESS) {
            Log("[ERROR] Failed to create Vulkan instance for enumeration");
            GPUInfo placeholder;
            placeholder.name = "No GPU Found";
            placeholder.vendor = "N/A";
            placeholder.isValid = false;
            g_app.gpuList.push_back(placeholder);
            return;
        }
        createdTempInstance = true;
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(enumInstance, &deviceCount, nullptr);

    if (deviceCount == 0) {
        Log("[WARNING] No Vulkan physical devices found!");
        GPUInfo placeholder;
        placeholder.name = "No GPU Found - Check drivers";
        placeholder.vendor = "N/A";
        placeholder.isValid = false;
        g_app.gpuList.push_back(placeholder);
        if (createdTempInstance) vkDestroyInstance(enumInstance, nullptr);
        return;
    }

    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(enumInstance, &deviceCount, physicalDevices.data());

    for (const auto& physDevice : physicalDevices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physDevice, &props);

        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);

        // Skip CPU-only / software devices
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;

        GPUInfo info;
        info.name = props.deviceName;
        info.vendorId = props.vendorID;
        info.deviceId = props.deviceID;
        info.vendor = GetVendorName(props.vendorID);
        info.physicalDevice = physDevice;

        // Calculate VRAM: sum of DEVICE_LOCAL heaps that aren't HOST_VISIBLE
        // Also track shared memory (HOST_VISIBLE + DEVICE_LOCAL)
        size_t dedicatedVRAM = 0;
        size_t sharedMemory = 0;
        
        for (uint32_t i = 0; i < memProps.memoryHeapCount; i++) {
            bool isDeviceLocal = (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
            
            if (isDeviceLocal) {
                // Check if any memory type using this heap is also HOST_VISIBLE
                bool hasHostVisible = false;
                for (uint32_t j = 0; j < memProps.memoryTypeCount; j++) {
                    if (memProps.memoryTypes[j].heapIndex == i &&
                        (memProps.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                        hasHostVisible = true;
                        break;
                    }
                }
                
                if (hasHostVisible) {
                    // HOST_VISIBLE + DEVICE_LOCAL heap
                    sharedMemory += memProps.memoryHeaps[i].size;
                    // For discrete GPUs with ReBAR, this is still dedicated VRAM
                    // (the entire VRAM is CPU-mappable via resizable BAR)
                    // For integrated GPUs, this is carved from system RAM
                    dedicatedVRAM += memProps.memoryHeaps[i].size;
                } else {
                    // Pure device-local = dedicated VRAM
                    dedicatedVRAM += memProps.memoryHeaps[i].size;
                }
            } else {
                // Non-device-local heaps (system RAM visible to GPU)
                sharedMemory += memProps.memoryHeaps[i].size;
            }
        }
        
        info.dedicatedVRAM = dedicatedVRAM;
        info.sharedMemory = sharedMemory;

        // Detect integrated GPU
        bool likelyIntegrated = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
        
        if (!likelyIntegrated) {
            // Additional heuristics from name
            std::string nameLower = info.name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), [](char c) { return static_cast<char>(::tolower(static_cast<unsigned char>(c))); });
            if (nameLower.find("graphics") != std::string::npos && 
                (nameLower.find("intel") != std::string::npos || 
                 nameLower.find("radeon(tm)") != std::string::npos ||
                 nameLower.find("vega") != std::string::npos ||
                 nameLower.find("apu") != std::string::npos)) {
                likelyIntegrated = true;
            }
            if (nameLower.find("uhd") != std::string::npos || 
                nameLower.find("iris") != std::string::npos) {
                likelyIntegrated = true;
            }
        }

        info.isIntegrated = likelyIntegrated;
        info.isValid = true;

        // Detect PCIe link configuration (uses SetupAPI, not graphics API)
        DetectPCIeLink(props.vendorID, props.deviceID, info);

        // Detect Thunderbolt/USB4/USB connection via device tree topology
        if (!info.isIntegrated) {
            DetectThunderboltConnection(props.vendorID, props.deviceID, info);
            if (info.isThunderbolt || info.isUSB4 || info.isUSB) {
                Log("[INFO] GPU '" + info.name + "' connected via " + info.externalConnectionType);
            }
        }

        g_app.gpuList.push_back(std::move(info));
    }

    // If no hardware GPUs found, add a placeholder
    if (g_app.gpuList.empty()) {
        Log("[WARNING] No hardware GPUs found!");
        GPUInfo placeholder;
        placeholder.name = "No GPU Found - Check drivers";
        placeholder.vendor = "N/A";
        placeholder.isValid = false;
        g_app.gpuList.push_back(placeholder);
    }

    if (createdTempInstance) vkDestroyInstance(enumInstance, nullptr);
}

// Get safe maximum bandwidth size based on GPU VRAM
size_t GetSafeMaxBandwidthSize(int gpuIndex) {
    if (gpuIndex < 0 || gpuIndex >= static_cast<int>(g_app.gpuList.size())) {
        return Constants::MIN_BANDWIDTH_SIZE;
    }
    
    const GPUInfo& gpu = g_app.gpuList[gpuIndex];
    if (!gpu.isValid) {
        return Constants::MIN_BANDWIDTH_SIZE;
    }
    
    size_t availableVRAM = gpu.dedicatedVRAM;
    
    // For iGPUs, use shared memory but be more conservative
    if (gpu.isIntegrated) {
        availableVRAM = gpu.sharedMemory / 4;
    }
    
    // Apply safety margin and account for needing multiple buffers
    size_t safeMax = static_cast<size_t>(availableVRAM * Constants::VRAM_SAFETY_MARGIN / 4);
    
    safeMax = std::max(safeMax, Constants::MIN_BANDWIDTH_SIZE);
    safeMax = std::min(safeMax, static_cast<size_t>(2ull * 1024 * 1024 * 1024));  // 2GB max
    
    return safeMax;
}

// Validate and potentially cap bandwidth size for selected GPU
size_t ValidateBandwidthSize(size_t requestedSize, int gpuIndex) {
    size_t maxSafe = GetSafeMaxBandwidthSize(gpuIndex);
    
    if (requestedSize > maxSafe) {
        Log("[WARNING] Requested bandwidth size " + FormatSize(requestedSize) + 
            " exceeds safe limit for this GPU. Capping to " + FormatSize(maxSafe));
        return maxSafe;
    }
    
    return requestedSize;
}

// ============================================================================
//                      VULKAN INITIALIZATION (Rendering)
// ============================================================================

// Helper: Find a memory type index that satisfies the requirements
uint32_t FindMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;  // Not found
}

// Helper: Find a queue family that supports the given flags
uint32_t FindQueueFamily(VkPhysicalDevice physDevice, VkQueueFlags flags, VkSurfaceKHR surface = VK_NULL_HANDLE) {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if ((queueFamilies[i].queueFlags & flags) == flags) {
            if (surface != VK_NULL_HANDLE) {
                VkBool32 presentSupport = false;
                vkGetPhysicalDeviceSurfaceSupportKHR(physDevice, i, surface, &presentSupport);
                if (presentSupport) return i;
            } else {
                return i;
            }
        }
    }
    return UINT32_MAX;
}

// Create Vulkan swapchain
// renderFinished semaphores are per swapchain IMAGE, not per frame-in-flight.
// vkQueuePresentKHR waits on the semaphore and nothing tells the CPU when that
// wait has been consumed, so a per-frame semaphore can be re-signaled while an
// earlier present still references it (VUID-vkQueueSubmit-pSignalSemaphores-
// 00067 under validation). Indexing by the acquired image guarantees the
// previous signal on that semaphore was consumed by the present of that same
// image, which must finish before the image can be acquired again.
bool CreateRenderFinishedSemaphores() {
    VkSemaphoreCreateInfo semInfo = {};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    g_app.renderFinishedSemaphores.assign(g_app.swapChainImages.size(), VK_NULL_HANDLE);
    for (auto& s : g_app.renderFinishedSemaphores) {
        VK_CHECK_RETURN(vkCreateSemaphore(g_app.device, &semInfo, nullptr, &s), false);
    }
    return true;
}

void DestroyRenderFinishedSemaphores() {
    for (auto s : g_app.renderFinishedSemaphores) {
        if (s != VK_NULL_HANDLE) vkDestroySemaphore(g_app.device, s, nullptr);
    }
    g_app.renderFinishedSemaphores.clear();
}

bool CreateSwapChain() {
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_app.renderPhysicalDevice, g_app.surface, &capabilities);

    // Choose surface format
    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_app.renderPhysicalDevice, g_app.surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_app.renderPhysicalDevice, g_app.surface, &formatCount, formats.data());

    VkSurfaceFormatKHR surfaceFormat = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = f;
            break;
        }
    }
    g_app.swapChainFormat = surfaceFormat.format;

    // Choose present mode (FIFO = VSync, guaranteed available)
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

    // Choose extent
    VkExtent2D extent;
    if (capabilities.currentExtent.width != UINT32_MAX) {
        extent = capabilities.currentExtent;
    } else {
        extent.width = std::clamp((uint32_t)g_app.windowWidth, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp((uint32_t)g_app.windowHeight, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    g_app.swapChainExtent = extent;

    uint32_t imageCount = std::max(capabilities.minImageCount, (uint32_t)Constants::NUM_FRAMES_IN_FLIGHT);
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = g_app.surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    VK_CHECK_RETURN(vkCreateSwapchainKHR(g_app.device, &createInfo, nullptr, &g_app.swapChain), false);

    // Get swapchain images
    uint32_t swapImageCount;
    vkGetSwapchainImagesKHR(g_app.device, g_app.swapChain, &swapImageCount, nullptr);
    g_app.swapChainImages.resize(swapImageCount);
    vkGetSwapchainImagesKHR(g_app.device, g_app.swapChain, &swapImageCount, g_app.swapChainImages.data());

    // Create image views
    g_app.swapChainImageViews.resize(swapImageCount);
    for (uint32_t i = 0; i < swapImageCount; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = g_app.swapChainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = g_app.swapChainFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        VK_CHECK_RETURN(vkCreateImageView(g_app.device, &viewInfo, nullptr, &g_app.swapChainImageViews[i]), false);
    }

    if (!CreateRenderFinishedSemaphores()) return false;

    return true;
}

// Create render pass
bool CreateRenderPass() {
    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = g_app.swapChainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentRef = {};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    VK_CHECK_RETURN(vkCreateRenderPass(g_app.device, &renderPassInfo, nullptr, &g_app.renderPass), false);
    return true;
}

// Create framebuffers for swapchain
bool CreateFramebuffers() {
    g_app.swapChainFramebuffers.resize(g_app.swapChainImageViews.size());
    for (size_t i = 0; i < g_app.swapChainImageViews.size(); i++) {
        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = g_app.renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &g_app.swapChainImageViews[i];
        fbInfo.width = g_app.swapChainExtent.width;
        fbInfo.height = g_app.swapChainExtent.height;
        fbInfo.layers = 1;
        VK_CHECK_RETURN(vkCreateFramebuffer(g_app.device, &fbInfo, nullptr, &g_app.swapChainFramebuffers[i]), false);
    }
    return true;
}

// Cleanup swapchain resources (for resize)
void CleanupSwapChain() {
    vkDeviceWaitIdle(g_app.device);
    DestroyRenderFinishedSemaphores();
    
    for (auto fb : g_app.swapChainFramebuffers) {
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(g_app.device, fb, nullptr);
    }
    g_app.swapChainFramebuffers.clear();

    for (auto iv : g_app.swapChainImageViews) {
        if (iv != VK_NULL_HANDLE) vkDestroyImageView(g_app.device, iv, nullptr);
    }
    g_app.swapChainImageViews.clear();
    g_app.swapChainImages.clear();

    if (g_app.swapChain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(g_app.device, g_app.swapChain, nullptr);
        g_app.swapChain = VK_NULL_HANDLE;
    }
}

// ============================================================================
// VULKAN VALIDATION LAYERS (compile-time opt-in)
// Enable with cmake -DENABLE_VULKAN_VALIDATION=ON (defines the macro).
// Requires VK_LAYER_KHRONOS_validation (e.g. vulkan-validation-layers package).
// ============================================================================
#ifdef ENABLE_VULKAN_VALIDATION
static VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT /*messageType*/,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* /*pUserData*/)
{
    const char* prefix = "[VULKAN]";
    if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        prefix = "[VULKAN ERROR]";
    else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        prefix = "[VULKAN WARN]";

    Log(std::string(prefix) + " " + pCallbackData->pMessage);
    fprintf(stderr, "%s %s\n", prefix, pCallbackData->pMessage);
    return VK_FALSE;
}

static void SetupDebugMessenger(VkInstance instance, VkDebugUtilsMessengerEXT* pMessenger) {
    auto createFunc = (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    if (!createFunc) {
        Log("[VULKAN] Debug messenger extension not available");
        return;
    }

    VkDebugUtilsMessengerCreateInfoEXT createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = VulkanDebugCallback;

    if (createFunc(instance, &createInfo, nullptr, pMessenger) != VK_SUCCESS) {
        Log("[VULKAN] Failed to set up debug messenger");
    } else {
        Log("[VULKAN] Validation layers enabled");
    }
}

static void DestroyDebugMessenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger) {
    if (messenger == VK_NULL_HANDLE) return;
    auto destroyFunc = (PFN_vkDestroyDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (destroyFunc) destroyFunc(instance, messenger, nullptr);
}
#endif // ENABLE_VULKAN_VALIDATION

bool InitVulkan() {
    // Create Vulkan Instance
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "GPU-PCIe-Test";
    appInfo.applicationVersion = VK_MAKE_VERSION(3, 4, 2);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    uint32_t glfwExtCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    std::vector<const char*> instanceExtensions(glfwExtensions, glfwExtensions + glfwExtCount);

    VkInstanceCreateInfo instanceInfo = {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
#ifdef ENABLE_VULKAN_VALIDATION
    // Validation build: enable VK_LAYER_KHRONOS_validation and route its
    // messages into the log via VK_EXT_debug_utils (same as the Windows Vulkan
    // variant; the CMake option used to define the macro without the source
    // acting on it).
    const char* validationLayers[] = { "VK_LAYER_KHRONOS_validation" };
    instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    instanceInfo.enabledLayerCount = 1;
    instanceInfo.ppEnabledLayerNames = validationLayers;
#endif
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();

    VK_CHECK_RETURN(vkCreateInstance(&instanceInfo, nullptr, &g_app.instance), false);

#ifdef ENABLE_VULKAN_VALIDATION
    SetupDebugMessenger(g_app.instance, &g_app.debugMessenger);
#endif

    // Create GLFW window surface
    VkResult surfResult = glfwCreateWindowSurface(g_app.instance, g_app.window, nullptr, &g_app.surface);
    if (surfResult != VK_SUCCESS) {
        Log("[ERROR] glfwCreateWindowSurface failed: " + std::to_string((int)surfResult));
        return false;
    }

    // Pick the UI render device. The UI is a handful of ImGui draw calls and
    // must never depend on the GPU under test, so prefer an INTEGRATED GPU. On
    // an eGPU host, rendering the window on the eGPU sends every presented
    // frame back across the Thunderbolt/USB4 tunnel through the compositor's
    // cross-GPU buffer sharing; on NVIDIA that produced a corrupted-pushbuffer
    // Xid 32 a few seconds after launch with no benchmark running. The
    // benchmark device (benchPhysicalDevice) is chosen separately by the user.
    // Preference: integrated > discrete > anything else; a candidate must have
    // a graphics queue that can present to our surface and VK_KHR_swapchain.
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(g_app.instance, &deviceCount, nullptr);
    if (deviceCount == 0) return false;

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(g_app.instance, &deviceCount, devices.data());

    g_app.renderPhysicalDevice = VK_NULL_HANDLE;
    {
        auto canPresent = [&](VkPhysicalDevice d) -> bool {
            if (FindQueueFamily(d, VK_QUEUE_GRAPHICS_BIT, g_app.surface) == UINT32_MAX) return false;
            uint32_t extCount = 0;
            vkEnumerateDeviceExtensionProperties(d, nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> exts(extCount);
            vkEnumerateDeviceExtensionProperties(d, nullptr, &extCount, exts.data());
            for (const auto& e : exts) {
                if (strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) return true;
            }
            return false;
        };
        auto rank = [](VkPhysicalDeviceType t) -> int {
            switch (t) {
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 2;
                case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 0;
                default:                                     return 1;
            }
        };
        int bestRank = -1;
        VkPhysicalDeviceProperties bestProps = {};
        for (auto& d : devices) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(d, &props);
            int r = rank(props.deviceType);
            if (r <= bestRank) continue;          // keep enumeration order on ties
            if (!canPresent(d)) continue;
            bestRank = r;
            bestProps = props;
            g_app.renderPhysicalDevice = d;
        }
        if (g_app.renderPhysicalDevice == VK_NULL_HANDLE) {
            Log("[ERROR] No Vulkan device can present to the window (graphics queue + VK_KHR_swapchain)");
            return false;
        }
        Log(std::string("[INFO] UI renders on ") + bestProps.deviceName +
            (bestProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                ? " (integrated GPU preferred so the UI never crosses an eGPU tunnel)"
                : " (no presentable integrated GPU found)"));
    }

    // Find queue family supporting graphics + present
    g_app.graphicsQueueFamily = FindQueueFamily(g_app.renderPhysicalDevice, VK_QUEUE_GRAPHICS_BIT, g_app.surface);
    if (g_app.graphicsQueueFamily == UINT32_MAX) return false;

    // Create logical device
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = g_app.graphicsQueueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo deviceCreateInfo = {};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount = 1;
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions;

    VK_CHECK_RETURN(vkCreateDevice(g_app.renderPhysicalDevice, &deviceCreateInfo, nullptr, &g_app.device), false);
    vkGetDeviceQueue(g_app.device, g_app.graphicsQueueFamily, 0, &g_app.graphicsQueue);

    // Create swapchain
    if (!CreateSwapChain()) return false;

    // Create render pass
    if (!CreateRenderPass()) return false;

    // Create framebuffers
    if (!CreateFramebuffers()) return false;

    // Create command pool and command buffers
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = g_app.graphicsQueueFamily;
    VK_CHECK_RETURN(vkCreateCommandPool(g_app.device, &poolInfo, nullptr, &g_app.commandPool), false);

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = g_app.commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = Constants::NUM_FRAMES_IN_FLIGHT;
    VK_CHECK_RETURN(vkAllocateCommandBuffers(g_app.device, &allocInfo, g_app.commandBuffers), false);

    // Create sync objects
    VkSemaphoreCreateInfo semInfo = {};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < Constants::NUM_FRAMES_IN_FLIGHT; i++) {
        VK_CHECK_RETURN(vkCreateSemaphore(g_app.device, &semInfo, nullptr, &g_app.imageAvailableSemaphores[i]), false);
        VK_CHECK_RETURN(vkCreateFence(g_app.device, &fenceInfo, nullptr, &g_app.inFlightFences[i]), false);
    }

    // Create descriptor pool for ImGui
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 }
    };
    VkDescriptorPoolCreateInfo dpInfo = {};
    dpInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpInfo.maxSets = 64;
    dpInfo.poolSizeCount = 1;
    dpInfo.pPoolSizes = poolSizes;
    VK_CHECK_RETURN(vkCreateDescriptorPool(g_app.device, &dpInfo, nullptr, &g_app.imguiDescriptorPool), false);

    return true;
}

// ============================================================================
//                      VULKAN BENCHMARK DEVICE
// ============================================================================

bool InitBenchmarkDevice(int gpuIndex) {
    if (gpuIndex < 0 || gpuIndex >= static_cast<int>(g_app.gpuList.size())) {
        Log("[ERROR] Invalid GPU index: " + std::to_string(gpuIndex));
        return false;
    }
    
    const GPUInfo& selectedGPU = g_app.gpuList[gpuIndex];
    
    if (!selectedGPU.isValid) {
        Log("[ERROR] Cannot benchmark - no valid GPU selected");
        return false;
    }
    
    if (selectedGPU.physicalDevice == VK_NULL_HANDLE) {
        Log("[ERROR] No Vulkan physical device handle for selected GPU");
        return false;
    }
    
    g_app.benchPhysicalDevice = selectedGPU.physicalDevice;
    
    // Get timestamp period for this device
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_app.benchPhysicalDevice, &props);
    g_app.benchTimestampPeriod = props.limits.timestampPeriod;  // nanoseconds per tick
    
    if (g_app.benchTimestampPeriod == 0) {
        Log("[WARNING] GPU reports zero timestamp period - timestamps may not be supported");
    }
    
    // Find a queue family that supports transfer (and preferably compute)
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_app.benchPhysicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(g_app.benchPhysicalDevice, &queueFamilyCount, queueFamilies.data());

    // Queue family selection strategy:
    // 1. Prefer dedicated TRANSFER family (no GRAPHICS bit) with timestamps
    //    - Maps directly to GPU DMA copy engines
    //    - Multiple queues map to separate upload/download DMA engines
    //    - This is critical for bidirectional overlap (NVIDIA has 2 dedicated transfer queues)
    // 2. Fall back to GRAPHICS+TRANSFER family if no dedicated transfer with timestamps
    //    - D3D12 DIRECT queue internally routes to DMA engines, but Vulkan graphics queues don't

    // Step 1: Look for dedicated transfer family with timestamps
    uint32_t dedicatedTransferFamily = UINT32_MAX;
    uint32_t graphicsTransferFamily = UINT32_MAX;

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        bool hasTransfer = (queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;
        bool hasGraphics = (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        bool hasTimestamps = queueFamilies[i].timestampValidBits > 0;

        if (hasTransfer && !hasGraphics && hasTimestamps && dedicatedTransferFamily == UINT32_MAX) {
            dedicatedTransferFamily = i;
        }
        if (hasTransfer && hasGraphics && hasTimestamps && graphicsTransferFamily == UINT32_MAX) {
            graphicsTransferFamily = i;
        }
    }
    
    // Step 2: Select primary bench queue family
    if (dedicatedTransferFamily != UINT32_MAX) {
        g_app.benchQueueFamily = dedicatedTransferFamily;
        Log("[INFO] Using dedicated transfer queue family " + std::to_string(dedicatedTransferFamily) + 
            " (" + std::to_string(queueFamilies[dedicatedTransferFamily].queueCount) + " queues) - direct DMA engine access");
    } else if (graphicsTransferFamily != UINT32_MAX) {
        g_app.benchQueueFamily = graphicsTransferFamily;
        Log("[INFO] Using graphics+transfer queue family " + std::to_string(graphicsTransferFamily) +
            " (no dedicated transfer family with timestamps available)");
    } else {
        // Last resort: any family with transfer
        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
                g_app.benchQueueFamily = i;
                Log("[WARNING] Using queue family " + std::to_string(i) + " without timestamp support");
                break;
            }
        }
    }
    
    if (g_app.benchQueueFamily == UINT32_MAX) {
        Log("[ERROR] No suitable queue family found on benchmark device");
        return false;
    }

    // Per spec, only timestampValidBits of each timestamp are meaningful; the
    // rest are undefined. Precompute the mask so readback sites can strip
    // undefined high bits before computing deltas.
    {
        uint32_t validBits = queueFamilies[g_app.benchQueueFamily].timestampValidBits;
        g_app.benchTimestampMask = (validBits >= 64) ? ~0ull : ((1ull << validBits) - 1);
    }
    
    // Create logical device for benchmarking
    // Request 2 queues for bidirectional overlap (separate DMA engines)
    uint32_t maxQueues = queueFamilies[g_app.benchQueueFamily].queueCount;
    uint32_t requestedQueues = (maxQueues >= 2) ? 2 : 1;
    float queuePriorities[2] = { 1.0f, 1.0f };

    // Build queue create infos
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    VkDeviceQueueCreateInfo transferQueueInfo = {};
    transferQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    transferQueueInfo.queueFamilyIndex = g_app.benchQueueFamily;
    transferQueueInfo.queueCount = requestedQueues;
    transferQueueInfo.pQueuePriorities = queuePriorities;
    queueCreateInfos.push_back(transferQueueInfo);

    // Enable host query reset if available (Vulkan 1.2 feature)
    VkPhysicalDeviceHostQueryResetFeatures hostQueryResetFeatures = {};
    hostQueryResetFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES;
    hostQueryResetFeatures.hostQueryReset = VK_TRUE;

    VkDeviceCreateInfo deviceInfo = {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &hostQueryResetFeatures;
    deviceInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    deviceInfo.pQueueCreateInfos = queueCreateInfos.data();
    // No extensions needed for benchmark device (no swapchain)

    VkResult result = vkCreateDevice(g_app.benchPhysicalDevice, &deviceInfo, nullptr, &g_app.benchDevice);
    if (result != VK_SUCCESS) {
        // Retry without host query reset
        deviceInfo.pNext = nullptr;
        VK_CHECK_RETURN(vkCreateDevice(g_app.benchPhysicalDevice, &deviceInfo, nullptr, &g_app.benchDevice), false);
    }

    vkGetDeviceQueue(g_app.benchDevice, g_app.benchQueueFamily, 0, &g_app.benchQueue);

    // Get second queue for bidirectional transfers
    g_app.hasDualQueues = false;
    if (requestedQueues >= 2) {
        vkGetDeviceQueue(g_app.benchDevice, g_app.benchQueueFamily, 1, &g_app.benchQueue2);
        g_app.hasDualQueues = true;
        if (dedicatedTransferFamily != UINT32_MAX && g_app.benchQueueFamily == dedicatedTransferFamily) {
            Log("[INFO] Dual DMA copy engines available for bidirectional overlap");
        } else {
            Log("[INFO] Dual queues available (graphics family - overlap may be limited)");
        }
    }

    // Create command pool
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = g_app.benchQueueFamily;
    VK_CHECK_RETURN(vkCreateCommandPool(g_app.benchDevice, &poolInfo, nullptr, &g_app.benchCommandPool), false);

    // Allocate command buffer
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = g_app.benchCommandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VK_CHECK_RETURN(vkAllocateCommandBuffers(g_app.benchDevice, &allocInfo, &g_app.benchCommandBuffer), false);

    // Create fence for synchronization
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    // Start unsignaled so first wait works correctly
    VK_CHECK_RETURN(vkCreateFence(g_app.benchDevice, &fenceInfo, nullptr, &g_app.benchFence), false);

    // Create second command pool, command buffer, and fence for bidirectional test
    if (g_app.hasDualQueues) {
        VK_CHECK_RETURN(vkCreateCommandPool(g_app.benchDevice, &poolInfo, nullptr, &g_app.benchCommandPool2), false);
        allocInfo.commandPool = g_app.benchCommandPool2;
        VK_CHECK_RETURN(vkAllocateCommandBuffers(g_app.benchDevice, &allocInfo, &g_app.benchCommandBuffer2), false);
        VK_CHECK_RETURN(vkCreateFence(g_app.benchDevice, &fenceInfo, nullptr, &g_app.benchFence2), false);
    }

    g_app.benchFenceValue = 1;
    g_app.fenceTimeoutCount = 0;

    return true;
}

void CleanupBenchmarkDevice() {
    if (g_app.benchDevice != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_app.benchDevice);
        
        if (g_app.benchFence2 != VK_NULL_HANDLE) {
            vkDestroyFence(g_app.benchDevice, g_app.benchFence2, nullptr);
            g_app.benchFence2 = VK_NULL_HANDLE;
        }
        if (g_app.benchCommandPool2 != VK_NULL_HANDLE) {
            vkDestroyCommandPool(g_app.benchDevice, g_app.benchCommandPool2, nullptr);
            g_app.benchCommandPool2 = VK_NULL_HANDLE;
        }
        g_app.benchCommandBuffer2 = VK_NULL_HANDLE;

        if (g_app.benchFence != VK_NULL_HANDLE) {
            vkDestroyFence(g_app.benchDevice, g_app.benchFence, nullptr);
            g_app.benchFence = VK_NULL_HANDLE;
        }
        if (g_app.benchCommandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(g_app.benchDevice, g_app.benchCommandPool, nullptr);
            g_app.benchCommandPool = VK_NULL_HANDLE;
        }
        g_app.benchCommandBuffer = VK_NULL_HANDLE;
        
        vkDestroyDevice(g_app.benchDevice, nullptr);
        g_app.benchDevice = VK_NULL_HANDLE;
    }
    g_app.benchPhysicalDevice = VK_NULL_HANDLE;
    g_app.benchQueue2 = VK_NULL_HANDLE;
    g_app.hasDualQueues = false;
    g_app.benchFenceValue = 1;
}

// ============================================================================
//                         BENCHMARK ENGINE
// ============================================================================
// All benchmark operations run on the dedicated transfer/copy queue.
// Buffer types map to D3D12 equivalents:
//   Upload      → HOST_VISIBLE + HOST_COHERENT  (D3D12: HEAP_TYPE_UPLOAD)
//   DeviceLocal → DEVICE_LOCAL                   (D3D12: HEAP_TYPE_DEFAULT)
//   Readback    → HOST_VISIBLE + HOST_CACHED     (D3D12: HEAP_TYPE_READBACK)
// EXCLUSIVE sharing mode is correct: both queues share the same family.
// ============================================================================

const char* BufferTypeName(VkBufferType t) {
    switch (t) {
        case VkBufferType::Upload:      return "Upload (host RAM)";
        case VkBufferType::DeviceLocal: return "DeviceLocal (VRAM)";
        case VkBufferType::Readback:    return "Readback (host RAM)";
        default:                        return "Other";
    }
}

VkBufferAllocation CreateBuffer(VkBufferType type, VkDeviceSize size, VkBufferUsageFlags extraUsage = 0) {
    VkBufferAllocation alloc = {};
    alloc.size = size;

    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkMemoryPropertyFlags memFlags = 0;

    switch (type) {
        case VkBufferType::Upload:
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            memFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;
        case VkBufferType::DeviceLocal:
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            memFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            break;
        case VkBufferType::Readback:
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            memFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            break;
    }
    // Extra usage (e.g. VK_BUFFER_USAGE_STORAGE_BUFFER_BIT for buffers bound as
    // storage by the GPU-verify compute shader). Passed only where needed so
    // bandwidth-test buffers keep their original transfer-only usage.
    bufferInfo.usage |= extraUsage;

    VkResult result = vkCreateBuffer(g_app.benchDevice, &bufferInfo, nullptr, &alloc.buffer);
    if (result != VK_SUCCESS) {
        Log("[ERROR] vkCreateBuffer failed: " + std::to_string((int)result) +
            " (Size: " + FormatSize(size) + ", Type: " + BufferTypeName(type) + ")");
        alloc.buffer = VK_NULL_HANDLE;
        return alloc;
    }
    
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(g_app.benchDevice, alloc.buffer, &memReqs);
    
    uint32_t memTypeIndex = FindMemoryType(g_app.benchPhysicalDevice, memReqs.memoryTypeBits, memFlags);
    
    // Fallback: For readback, try HOST_VISIBLE without HOST_CACHED
    if (memTypeIndex == UINT32_MAX && type == VkBufferType::Readback) {
        memFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        memTypeIndex = FindMemoryType(g_app.benchPhysicalDevice, memReqs.memoryTypeBits, memFlags);
    }
    
    if (memTypeIndex == UINT32_MAX) {
        Log("[ERROR] Failed to find suitable memory type for buffer");
        vkDestroyBuffer(g_app.benchDevice, alloc.buffer, nullptr);
        alloc.buffer = VK_NULL_HANDLE;
        return alloc;
    }
    
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = memTypeIndex;
    
    result = vkAllocateMemory(g_app.benchDevice, &allocInfo, nullptr, &alloc.memory);
    if (result != VK_SUCCESS) {
        Log("[ERROR] vkAllocateMemory failed: " + std::to_string((int)result) + 
            " (Size: " + FormatSize(size) + ")");
        vkDestroyBuffer(g_app.benchDevice, alloc.buffer, nullptr);
        alloc.buffer = VK_NULL_HANDLE;
        return alloc;
    }
    
    vkBindBufferMemory(g_app.benchDevice, alloc.buffer, alloc.memory, 0);
    
    return alloc;
}

// Enhanced fence wait with retry logic and global timeout checking
FenceWaitResult WaitForBenchFenceEx() {
    // Check for cancellation first
    if (g_app.cancelRequested || g_app.vramTestCancelRequested) {
        return FenceWaitResult::Cancelled;
    }

    // Wait for the fence. VK_TIMEOUT is retried up to MAX_FENCE_RETRIES times on
    // the SAME submission (a slow-but-alive GPU gets ~24 s); only then is it a
    // hang. Every non-Success return therefore means the fence was never seen
    // signaled, and benchmarkAborted is set so callers stop touching the
    // still-pending command buffer and fence. (Previously a single timeout
    // returned Timeout with the work still in flight, and callers treated it as
    // a completed batch: bogus 8-second "sample", then vkResetCommandBuffer on a
    // pending buffer and vkQueueSubmit with a fence already in use.)
    const uint64_t timeout = static_cast<uint64_t>(Constants::FENCE_WAIT_TIMEOUT_MS) * 1000000ULL; // ms -> ns
    for (;;) {
        VkResult result = vkWaitForFences(g_app.benchDevice, 1, &g_app.benchFence, VK_TRUE, timeout);
        if (result == VK_SUCCESS) break;
        if (result != VK_TIMEOUT) {
            Log("[ERROR] vkWaitForFences failed: " + std::to_string((int)result));
            g_app.benchmarkAborted = true;
            return FenceWaitResult::Error;
        }
        g_app.fenceTimeoutCount++;
        Log("[WARNING] Benchmark fence wait timed out after " +
            std::to_string(Constants::FENCE_WAIT_TIMEOUT_MS / 1000) + "s (timeout #" +
            std::to_string(g_app.fenceTimeoutCount.load()) + ")");
        if (g_app.cancelRequested || g_app.vramTestCancelRequested) {
            return FenceWaitResult::Cancelled;
        }
        if (g_app.fenceTimeoutCount >= Constants::MAX_FENCE_RETRIES) {
            Log("[ERROR] Max fence timeouts exceeded - possible GPU hang. Aborting benchmark.");
            g_app.benchmarkAborted = true;
            return FenceWaitResult::Timeout;
        }
    }

    // Reset fence for next use
    vkResetFences(g_app.benchDevice, 1, &g_app.benchFence);

    // Success - reset timeout counter
    g_app.fenceTimeoutCount = 0;
    g_app.benchFenceValue++;

    // Global timeout is evaluated after the wait so the submission it covered
    // has finished and nothing is left pending on the queue.
    if (!g_app.vramTestRunning && IsGlobalTimeoutExceeded()) {
        Log("[ERROR] Global benchmark timeout exceeded (5 minutes)");
        g_app.benchmarkAborted = true;
        return FenceWaitResult::Timeout;
    }
    return FenceWaitResult::Success;
}

// Submit bench command buffer and wait
FenceWaitResult SubmitAndWait() {
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &g_app.benchCommandBuffer;
    
    VkResult result = vkQueueSubmit(g_app.benchQueue, 1, &submitInfo, g_app.benchFence);
    if (result != VK_SUCCESS) {
        Log("[ERROR] vkQueueSubmit failed: " + std::to_string((int)result));
        return FenceWaitResult::Error;
    }
    
    return WaitForBenchFenceEx();
}

// Legacy wrapper
void WaitForBenchFence() {
    // For legacy compatibility - assumes command buffer already submitted
    // This is used after explicit SubmitAndWait patterns
}

bool ShouldAbortBenchmark() {
    return g_app.cancelRequested || g_app.benchmarkAborted || IsGlobalTimeoutExceeded();
}

// Helper: Begin recording benchmark command buffer
void BeginBenchCommandBuffer() {
    vkResetCommandBuffer(g_app.benchCommandBuffer, 0);
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_app.benchCommandBuffer, &beginInfo);
}

// Helper: End recording and submit benchmark command buffer
FenceWaitResult EndAndSubmitBenchCommandBuffer() {
    vkEndCommandBuffer(g_app.benchCommandBuffer);
    return SubmitAndWait();
}

// Bandwidth test with configurable measurement method
// 
// Runs on the dedicated transfer/copy queue (D3D12 equivalent: COPY queue).
//
// useCpuTiming = false (GPU timestamps):
//   Wraps copy commands with GPU timestamp queries on the transfer queue.
//   Accurate for: GPU→CPU on all GPUs, CPU→GPU on integrated GPUs.
//   D3D12 equivalent: EndQuery(TIMESTAMP) before/after CopyResource on COPY queue.
//
// useCpuTiming = true (CPU round-trip method):
//   Uploads data, then reads back from the SAME buffer to create a data dependency.
//   The readback forces the upload to fully complete before the fence signals.
//   Required for CPU→GPU on discrete GPUs where ReBAR/BAR mapping can cause
//   GPU timestamps to report completion before data actually crosses the PCIe bus.
//   Uses measured download speed to isolate upload time:
//     upload_time = round_trip_time - (data_size / measured_download_speed)
//   D3D12 equivalent: identical logic with CopyResource on COPY queue.
//
BenchmarkResult RunBandwidthTest(const std::string& name, VkBufferAllocation& src, VkBufferAllocation& dst, size_t size, int copies, int batches, bool useCpuTiming = false, double measuredDownloadGB = 0.0) {
    SetCurrentTest(name);
    BenchmarkResult result;
    result.testName = name;
    result.unit = "GB/s";

    if (!src || !dst) {
        Log("[ERROR] Invalid source or destination buffer in bandwidth test");
        return result;
    }
    
    std::vector<double> bandwidths;
    bandwidths.reserve(batches);
    int failedBatches = 0;
    
    // Warm-up pass
    {
        BeginBenchCommandBuffer();
        for (int j = 0; j < copies; ++j) {
            VkBufferCopy copyRegion = {};
            copyRegion.size = size;
            vkCmdCopyBuffer(g_app.benchCommandBuffer, src.buffer, dst.buffer, 1, &copyRegion);
        }
        EndAndSubmitBenchCommandBuffer();
    }

    if (useCpuTiming) {
        // Round-trip timing mode for accurate CPU->GPU measurement
        auto roundtripReadback = CreateBuffer(VkBufferType::Readback, size);
        if (!roundtripReadback) {
            Log("[WARNING] Could not create round-trip readback buffer - falling back to GPU timestamps");
            useCpuTiming = false;
        } else {
            for (int i = 0; i < batches && !ShouldAbortBenchmark(); ++i) {
                if (i % 8 == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
                
                BeginBenchCommandBuffer();
                
                // Upload: CPU -> GPU
                for (int j = 0; j < copies; ++j) {
                    VkBufferCopy copyRegion = {};
                    copyRegion.size = size;
                    vkCmdCopyBuffer(g_app.benchCommandBuffer, src.buffer, dst.buffer, 1, &copyRegion);
                }
                
                // Memory barrier to ensure upload completes before readback
                VkMemoryBarrier memBarrier = {};
                memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                memBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                memBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(g_app.benchCommandBuffer,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 1, &memBarrier, 0, nullptr, 0, nullptr);
                
                // Download: GPU -> CPU (creates data dependency)
                for (int j = 0; j < copies; ++j) {
                    VkBufferCopy copyRegion = {};
                    copyRegion.size = size;
                    vkCmdCopyBuffer(g_app.benchCommandBuffer, dst.buffer, roundtripReadback.buffer, 1, &copyRegion);
                }
                
                vkEndCommandBuffer(g_app.benchCommandBuffer);

                // Start CPU timer
                auto startTime = std::chrono::high_resolution_clock::now();
                
                VkSubmitInfo submitInfo = {};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &g_app.benchCommandBuffer;
                vkQueueSubmit(g_app.benchQueue, 1, &submitInfo, g_app.benchFence);
                
                FenceWaitResult fenceResult = WaitForBenchFenceEx();
                
                auto endTime = std::chrono::high_resolution_clock::now();
                
                if (fenceResult == FenceWaitResult::Cancelled) break;
                if (fenceResult != FenceWaitResult::Success || g_app.benchmarkAborted) {
                    Log("[ERROR] Critical fence error - aborting bandwidth test");
                    break;
                }

                double totalSeconds = std::chrono::duration<double>(endTime - startTime).count();
                if (totalSeconds > 0) {
                    double sizeGB = static_cast<double>(size) * copies / Constants::BYTES_PER_GB;
                    
                    double uploadBw;
                    bool usedImprovedMethod = false;
                    
                    if (measuredDownloadGB > 0.0) {
                        double downloadTime = sizeGB / measuredDownloadGB;
                        double uploadTime = totalSeconds - downloadTime;
                        
                        if (uploadTime > (totalSeconds * 0.1)) {
                            double calculatedUpload = sizeGB / uploadTime;
                            if (calculatedUpload <= measuredDownloadGB * 3.0) {
                                uploadBw = calculatedUpload;
                                usedImprovedMethod = true;
                            }
                        }
                    }
                    
                    if (!usedImprovedMethod) {
                        double totalBytes = static_cast<double>(size) * copies * 2;
                        double roundtripBw = (totalBytes / Constants::BYTES_PER_GB) / totalSeconds;
                        uploadBw = roundtripBw / 2.0;
                    }
                    
                    bandwidths.push_back(uploadBw);
                } else {
                    failedBatches++;
                }

                g_app.progress = static_cast<float>(i + 1) / static_cast<float>(batches);
            }
            roundtripReadback.Destroy(g_app.benchDevice);
        }
    }
    
    if (!useCpuTiming) {
        // GPU timestamp mode
        if (g_app.benchTimestampPeriod == 0) {
            Log("[WARNING] GPU timestamps not supported - falling back to CPU timing");
            // Fall back to simple CPU timing without round-trip
            for (int i = 0; i < batches && !ShouldAbortBenchmark(); ++i) {
                if (i % 8 == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
                
                BeginBenchCommandBuffer();
                for (int j = 0; j < copies; ++j) {
                    VkBufferCopy copyRegion = {};
                    copyRegion.size = size;
                    vkCmdCopyBuffer(g_app.benchCommandBuffer, src.buffer, dst.buffer, 1, &copyRegion);
                }
                vkEndCommandBuffer(g_app.benchCommandBuffer);

                auto startTime = std::chrono::high_resolution_clock::now();
                
                VkSubmitInfo submitInfo = {};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &g_app.benchCommandBuffer;
                vkQueueSubmit(g_app.benchQueue, 1, &submitInfo, g_app.benchFence);
                
                FenceWaitResult fenceResult = WaitForBenchFenceEx();
                auto endTime = std::chrono::high_resolution_clock::now();
                
                if (fenceResult == FenceWaitResult::Cancelled) break;
                if (fenceResult != FenceWaitResult::Success || g_app.benchmarkAborted) break;

                double seconds = std::chrono::duration<double>(endTime - startTime).count();
                if (seconds > 0) {
                    double bw = (static_cast<double>(size) * copies / Constants::BYTES_PER_GB) / seconds;
                    bandwidths.push_back(bw);
                } else {
                    failedBatches++;
                }
                g_app.progress = static_cast<float>(i + 1) / static_cast<float>(batches);
            }
        } else {
            // Create timestamp query pool
            VkQueryPoolCreateInfo queryPoolInfo = {};
            queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryPoolInfo.queryCount = 2;
            
            VkQueryPool queryPool = VK_NULL_HANDLE;
            if (vkCreateQueryPool(g_app.benchDevice, &queryPoolInfo, nullptr, &queryPool) != VK_SUCCESS) {
                Log("[ERROR] Failed to create timestamp query pool in bandwidth test");
                return result;
            }

            for (int i = 0; i < batches && !ShouldAbortBenchmark(); ++i) {
                if (i % 8 == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
                
                BeginBenchCommandBuffer();
                
                vkCmdResetQueryPool(g_app.benchCommandBuffer, queryPool, 0, 2);
                // TOP_OF_PIPE is correct here: this is the first command in a fresh
                // command buffer, so there's no prior work to wait for.
                // D3D12 equivalent: EndQuery(TIMESTAMP) as first command in list.
                vkCmdWriteTimestamp(g_app.benchCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);
                
                for (int j = 0; j < copies; ++j) {
                    VkBufferCopy copyRegion = {};
                    copyRegion.size = size;
                    vkCmdCopyBuffer(g_app.benchCommandBuffer, src.buffer, dst.buffer, 1, &copyRegion);
                }
                
                vkCmdWriteTimestamp(g_app.benchCommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
                
                vkEndCommandBuffer(g_app.benchCommandBuffer);

                VkSubmitInfo submitInfo = {};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &g_app.benchCommandBuffer;
                vkQueueSubmit(g_app.benchQueue, 1, &submitInfo, g_app.benchFence);
                
                FenceWaitResult fenceResult = WaitForBenchFenceEx();
                if (fenceResult == FenceWaitResult::Cancelled) break;
                if (fenceResult != FenceWaitResult::Success || g_app.benchmarkAborted) {
                    Log("[ERROR] Critical fence error - aborting bandwidth test");
                    break;
                }

                uint64_t timestamps[2] = {};
                VkResult qr = vkGetQueryPoolResults(g_app.benchDevice, queryPool, 0, 2,
                    sizeof(timestamps), timestamps, sizeof(uint64_t),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                timestamps[0] &= g_app.benchTimestampMask;  // strip undefined bits (timestampValidBits)
                timestamps[1] &= g_app.benchTimestampMask;

                if (qr == VK_SUCCESS && timestamps[1] > timestamps[0]) {
                    uint64_t delta = timestamps[1] - timestamps[0];
                    double seconds = static_cast<double>(delta) * static_cast<double>(g_app.benchTimestampPeriod) / 1e9;
                    if (seconds > 0) {
                        double bw = (static_cast<double>(size) * copies / Constants::BYTES_PER_GB) / seconds;
                        bandwidths.push_back(bw);
                    } else {
                        failedBatches++;
                    }
                } else {
                    failedBatches++;
                }

                g_app.progress = static_cast<float>(i + 1) / static_cast<float>(batches);
            }
            
            vkDestroyQueryPool(g_app.benchDevice, queryPool, nullptr);
        }
    }

    if (!bandwidths.empty()) {
        std::sort(bandwidths.begin(), bandwidths.end());
        result.minValue = bandwidths.front();
        result.maxValue = bandwidths.back();
        result.avgValue = std::accumulate(bandwidths.begin(), bandwidths.end(), 0.0) / bandwidths.size();
        result.samples = std::move(bandwidths);
        
        if (failedBatches > 0) {
            Log("[WARNING] " + std::to_string(failedBatches) + " batches failed in " + name);
        }
    } else {
        Log("[WARNING] No valid samples collected for " + name);
    }

    return result;
}

// Transfer latency test - measures per-copy overhead on the transfer/copy queue.
// Uses GPU timestamps with BOTTOM_OF_PIPE for both start and end to prevent
// overlapping measurements. Each copy is individually timed within batches of 64.
// D3D12 equivalent: EndQuery(TIMESTAMP) before/after each CopyResource on COPY queue.
BenchmarkResult RunLatencyTest(const std::string& name, VkBufferAllocation& src, VkBufferAllocation& dst, int iterations) {
    SetCurrentTest(name);
    BenchmarkResult result;
    result.testName = name;
    result.unit = "us";

    if (iterations <= 0) return result;
    
    if (!src || !dst) {
        Log("[ERROR] Invalid source or destination buffer in latency test");
        return result;
    }

    constexpr int QueriesPerBatch = 64;
    int batchCount = (iterations + QueriesPerBatch - 1) / QueriesPerBatch;

    if (g_app.benchTimestampPeriod == 0) {
        Log("[WARNING] GPU timestamps not supported - latency test requires timestamps");
        return result;
    }

    VkQueryPoolCreateInfo queryPoolInfo = {};
    queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryPoolInfo.queryCount = QueriesPerBatch * 2;
    
    VkQueryPool queryPool = VK_NULL_HANDLE;
    if (vkCreateQueryPool(g_app.benchDevice, &queryPoolInfo, nullptr, &queryPool) != VK_SUCCESS) {
        Log("[ERROR] Failed to create timestamp query pool");
        return result;
    }

    std::vector<double> latencies;
    latencies.reserve(iterations);

    for (int b = 0; b < batchCount && !ShouldAbortBenchmark(); ++b) {
        if (b % 4 == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        
        int opsThisBatch = std::min(QueriesPerBatch, iterations - static_cast<int>(latencies.size()));

        BeginBenchCommandBuffer();
        
        vkCmdResetQueryPool(g_app.benchCommandBuffer, queryPool, 0, opsThisBatch * 2);

        for (int i = 0; i < opsThisBatch; ++i) {
            uint32_t queryIndex = i * 2;
            // BOTTOM_OF_PIPE for both timestamps = serialized measurement.
            // D3D12 equivalent: EndQuery(TIMESTAMP) which also serializes.
            vkCmdWriteTimestamp(g_app.benchCommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, queryIndex);
            
            VkBufferCopy copyRegion = {};
            copyRegion.size = src.size;
            vkCmdCopyBuffer(g_app.benchCommandBuffer, src.buffer, dst.buffer, 1, &copyRegion);
            
            vkCmdWriteTimestamp(g_app.benchCommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, queryIndex + 1);
        }

        FenceWaitResult fenceResult = EndAndSubmitBenchCommandBuffer();
        if (fenceResult != FenceWaitResult::Success || g_app.benchmarkAborted) break;

        // Read timestamps
        std::vector<uint64_t> timestamps(opsThisBatch * 2);
        VkResult qr = vkGetQueryPoolResults(g_app.benchDevice, queryPool, 0, opsThisBatch * 2,
            timestamps.size() * sizeof(uint64_t), timestamps.data(), sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        
        if (qr == VK_SUCCESS) {
            for (int i = 0; i < opsThisBatch; ++i) {
                uint64_t tStart = timestamps[i * 2 + 0] & g_app.benchTimestampMask;
                uint64_t tEnd = timestamps[i * 2 + 1] & g_app.benchTimestampMask;
                if (tEnd > tStart) {
                    double deltaSec = static_cast<double>(tEnd - tStart) * static_cast<double>(g_app.benchTimestampPeriod) / 1e9;
                    double us = deltaSec * 1'000'000.0;
                    latencies.push_back(us);
                }
            }
        } else {
            Log("[ERROR] Failed to read query results");
        }

        g_app.progress = static_cast<float>(latencies.size()) / static_cast<float>(iterations);
    }

    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        result.minValue = latencies.front();
        result.maxValue = latencies.back();
        result.avgValue = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
        result.samples = std::move(latencies);
    } else {
        Log("[WARNING] No valid latency samples collected for " + name);
    }

    vkDestroyQueryPool(g_app.benchDevice, queryPool, nullptr);
    return result;
}

// Command latency test - measures minimum dispatch overhead on the transfer/copy queue.
// Back-to-back timestamp pairs with no work between them.
// D3D12 equivalent: consecutive EndQuery(TIMESTAMP) pairs on COPY queue.
BenchmarkResult RunCommandLatencyTest(int iterations) {
    SetCurrentTest("Command Latency");
    BenchmarkResult result;
    result.testName = "Command Latency";
    result.unit = "us";

    if (iterations <= 0) return result;

    if (g_app.benchTimestampPeriod == 0) {
        Log("[WARNING] GPU timestamps not supported - command latency test requires timestamps");
        return result;
    }

    constexpr int QueriesPerBatch = 64;
    int batchCount = (iterations + QueriesPerBatch - 1) / QueriesPerBatch;

    VkQueryPoolCreateInfo queryPoolInfo = {};
    queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryPoolInfo.queryCount = QueriesPerBatch * 2;
    
    VkQueryPool queryPool = VK_NULL_HANDLE;
    if (vkCreateQueryPool(g_app.benchDevice, &queryPoolInfo, nullptr, &queryPool) != VK_SUCCESS) {
        Log("[ERROR] Failed to create timestamp query pool for command latency");
        return result;
    }

    std::vector<double> latencies;
    latencies.reserve(iterations);

    for (int b = 0; b < batchCount && !ShouldAbortBenchmark(); ++b) {
        if (b % 4 == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        
        int opsThisBatch = std::min(QueriesPerBatch, iterations - static_cast<int>(latencies.size()));

        BeginBenchCommandBuffer();
        
        vkCmdResetQueryPool(g_app.benchCommandBuffer, queryPool, 0, opsThisBatch * 2);

        for (int i = 0; i < opsThisBatch; ++i) {
            uint32_t queryIndex = i * 2;
            vkCmdWriteTimestamp(g_app.benchCommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, queryIndex);
            vkCmdWriteTimestamp(g_app.benchCommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, queryIndex + 1);
        }

        FenceWaitResult fenceResult = EndAndSubmitBenchCommandBuffer();
        if (fenceResult != FenceWaitResult::Success || g_app.benchmarkAborted) break;

        std::vector<uint64_t> timestamps(opsThisBatch * 2);
        VkResult qr = vkGetQueryPoolResults(g_app.benchDevice, queryPool, 0, opsThisBatch * 2,
            timestamps.size() * sizeof(uint64_t), timestamps.data(), sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        
        if (qr == VK_SUCCESS) {
            for (int i = 0; i < opsThisBatch; ++i) {
                uint64_t tStart = timestamps[i * 2 + 0] & g_app.benchTimestampMask;
                uint64_t tEnd = timestamps[i * 2 + 1] & g_app.benchTimestampMask;
                // tEnd > tStart guard: equal/reordered timestamps would
                // underflow the unsigned subtraction into a bogus sample.
                if (tEnd > tStart) {
                    double deltaSec = static_cast<double>(tEnd - tStart) * static_cast<double>(g_app.benchTimestampPeriod) / 1e9;
                    double us = deltaSec * 1'000'000.0;
                    latencies.push_back(us);
                }
            }
        }

        g_app.progress = static_cast<float>(latencies.size()) / static_cast<float>(iterations);
    }

    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        result.minValue = latencies.front();
        result.maxValue = latencies.back();
        result.avgValue = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
        result.samples = std::move(latencies);
    } else {
        Log("[WARNING] No valid command latency samples collected");
    }

    vkDestroyQueryPool(g_app.benchDevice, queryPool, nullptr);
    return result;
}

// Bidirectional bandwidth test - measures full-duplex PCIe throughput.
// Uses dual transfer/copy queues to submit uploads and downloads simultaneously,
// allowing the GPU's separate upload and download DMA engines to operate in parallel.
// CPU wall-clock timing measures total elapsed time; both directions counted.
// Falls back to single-queue interleaved copies if dual queues unavailable.
// D3D12 equivalent: 2 × COPY queues with simultaneous ExecuteCommandLists.
BenchmarkResult RunBidirectionalTest(size_t size, int copies, int batches) {
    SetCurrentTest("Bidirectional " + FormatSize(size));
    BenchmarkResult result;
    result.testName = GetCurrentTest();
    result.unit = "GB/s";

    // The bidirectional test needs 4 buffers of `size` at once (2x host, 2x
    // device) - the largest simultaneous footprint of any test. On eGPUs over
    // Thunderbolt/USB4 the OS/driver grants conservative memory budgets, so
    // the same size that worked for single-direction tests can fail here
    // despite ample physical VRAM. Rather than skipping the test, retry with
    // progressively halved buffers; bandwidth math uses the actual size, so
    // results stay valid.
    constexpr VkDeviceSize MIN_BIDIR_SIZE = 32ull * 1024 * 1024;
    const size_t requestedSize = size;
    VkBufferAllocation cpuUpload = {}, gpuDefault = {}, gpuSrc = {}, cpuReadback = {};
    for (;;) {
        cpuUpload = CreateBuffer(VkBufferType::Upload, size);
        if (cpuUpload) gpuDefault = CreateBuffer(VkBufferType::DeviceLocal, size);
        if (gpuDefault) gpuSrc = CreateBuffer(VkBufferType::DeviceLocal, size);
        if (gpuSrc) cpuReadback = CreateBuffer(VkBufferType::Readback, size);
        if (cpuReadback) break;  // all four succeeded

        cpuUpload.Destroy(g_app.benchDevice);
        gpuDefault.Destroy(g_app.benchDevice);
        gpuSrc.Destroy(g_app.benchDevice);
        cpuReadback.Destroy(g_app.benchDevice);
        cpuUpload = {}; gpuDefault = {}; gpuSrc = {}; cpuReadback = {};
        if (size / 2 < MIN_BIDIR_SIZE) {
            Log("[ERROR] Failed to create resources for bidirectional test even at " +
                FormatSize(size) + " - skipping bidirectional measurement");
            return result;
        }
        size /= 2;
        Log("[WARNING] Bidirectional buffer allocation failed - retrying at " + FormatSize(size));
    }
    if (size != requestedSize) {
        Log("[WARNING] Bidirectional test running with " + FormatSize(size) + " buffers instead of " +
            FormatSize(requestedSize) + " (driver/OS refused the larger allocation - common for "
            "Thunderbolt/USB4 eGPUs with constrained memory budgets). Results remain valid.");
        SetCurrentTest("Bidirectional " + FormatSize(size));
        result.testName = GetCurrentTest();
    }

    std::vector<double> bandwidths;
    bandwidths.reserve(batches);
    
    // Warm-up pass - uses same queue topology as the actual test
    if (g_app.hasDualQueues) {
        VkBufferCopy copyRegion = {};
        copyRegion.size = size;
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        
        // Warm-up: upload on queue 1
        vkResetCommandBuffer(g_app.benchCommandBuffer, 0);
        vkBeginCommandBuffer(g_app.benchCommandBuffer, &beginInfo);
        for (int j = 0; j < copies; ++j)
            vkCmdCopyBuffer(g_app.benchCommandBuffer, cpuUpload.buffer, gpuDefault.buffer, 1, &copyRegion);
        vkEndCommandBuffer(g_app.benchCommandBuffer);
        
        // Warm-up: download on queue 2
        vkResetCommandBuffer(g_app.benchCommandBuffer2, 0);
        vkBeginCommandBuffer(g_app.benchCommandBuffer2, &beginInfo);
        for (int j = 0; j < copies; ++j)
            vkCmdCopyBuffer(g_app.benchCommandBuffer2, gpuSrc.buffer, cpuReadback.buffer, 1, &copyRegion);
        vkEndCommandBuffer(g_app.benchCommandBuffer2);
        
        VkSubmitInfo si1 = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si1.commandBufferCount = 1;
        si1.pCommandBuffers = &g_app.benchCommandBuffer;
        VkSubmitInfo si2 = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si2.commandBufferCount = 1;
        si2.pCommandBuffers = &g_app.benchCommandBuffer2;
        
        vkQueueSubmit(g_app.benchQueue, 1, &si1, g_app.benchFence);
        vkQueueSubmit(g_app.benchQueue2, 1, &si2, g_app.benchFence2);
        VkFence warmupFences[2] = { g_app.benchFence, g_app.benchFence2 };
        // Bounded, cancel-aware wait. A raw UINT64_MAX wait would hang the
        // worker on a GPU stall and ignore the Cancel button; poll on the same
        // timeout the measured loop uses and bail on abort.
        int warmupRetries = 0;
        bool warmupOk = true;
        while (vkWaitForFences(g_app.benchDevice, 2, warmupFences, VK_TRUE,
                               Constants::FENCE_WAIT_TIMEOUT_MS * 1000000ULL) == VK_TIMEOUT) {
            if (ShouldAbortBenchmark() || ++warmupRetries >= Constants::MAX_FENCE_RETRIES) {
                g_app.benchmarkAborted = true;
                warmupOk = false;
                break;
            }
        }
        // Only reset the fences if they actually signaled; resetting fences with
        // GPU work still pending (the abort case) is invalid.
        if (warmupOk) vkResetFences(g_app.benchDevice, 2, warmupFences);
    } else {
        BeginBenchCommandBuffer();
        VkBufferCopy copyRegion = {};
        copyRegion.size = size;
        for (int j = 0; j < copies; ++j) {
            vkCmdCopyBuffer(g_app.benchCommandBuffer, cpuUpload.buffer, gpuDefault.buffer, 1, &copyRegion);
            vkCmdCopyBuffer(g_app.benchCommandBuffer, gpuSrc.buffer, cpuReadback.buffer, 1, &copyRegion);
        }
        EndAndSubmitBenchCommandBuffer();
    }

    if (g_app.hasDualQueues) {
        // ---- DUAL QUEUE PATH: true simultaneous upload + download ----
        // Queue 1 handles upload, Queue 2 handles download.
        // D3D12 backport: 2 × COPY queues, one for upload, one for download.
        
        for (int i = 0; i < batches && !ShouldAbortBenchmark(); ++i) {
            if (i % 8 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            
            VkBufferCopy copyRegion = {};
            copyRegion.size = size;
            
            // Record upload commands into command buffer 1
            vkResetCommandBuffer(g_app.benchCommandBuffer, 0);
            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(g_app.benchCommandBuffer, &beginInfo);
            for (int j = 0; j < copies; ++j) {
                vkCmdCopyBuffer(g_app.benchCommandBuffer, cpuUpload.buffer, gpuDefault.buffer, 1, &copyRegion);
            }
            vkEndCommandBuffer(g_app.benchCommandBuffer);
            
            // Record download commands into command buffer 2
            vkResetCommandBuffer(g_app.benchCommandBuffer2, 0);
            vkBeginCommandBuffer(g_app.benchCommandBuffer2, &beginInfo);
            for (int j = 0; j < copies; ++j) {
                vkCmdCopyBuffer(g_app.benchCommandBuffer2, gpuSrc.buffer, cpuReadback.buffer, 1, &copyRegion);
            }
            vkEndCommandBuffer(g_app.benchCommandBuffer2);
            
            // Submit both simultaneously to separate queues
            VkSubmitInfo submitInfo1 = {};
            submitInfo1.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo1.commandBufferCount = 1;
            submitInfo1.pCommandBuffers = &g_app.benchCommandBuffer;
            
            VkSubmitInfo submitInfo2 = {};
            submitInfo2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo2.commandBufferCount = 1;
            submitInfo2.pCommandBuffers = &g_app.benchCommandBuffer2;
            
            auto startTime = std::chrono::high_resolution_clock::now();
            
            vkQueueSubmit(g_app.benchQueue, 1, &submitInfo1, g_app.benchFence);
            vkQueueSubmit(g_app.benchQueue2, 1, &submitInfo2, g_app.benchFence2);
            
            // Wait for both to complete. VK_TIMEOUT is retried (bounded) on the
            // same submission before it is declared a hang. Fences that never
            // signaled mean the GPU work is still pending, so they must not be
            // reset and the command buffers must not be re-recorded: abort.
            VkFence fences[2] = { g_app.benchFence, g_app.benchFence2 };
            uint64_t timeout = static_cast<uint64_t>(Constants::FENCE_WAIT_TIMEOUT_MS) * 1000000ULL;
            VkResult waitResult = vkWaitForFences(g_app.benchDevice, 2, fences, VK_TRUE, timeout);
            while (waitResult == VK_TIMEOUT) {
                g_app.fenceTimeoutCount++;
                Log("[WARNING] Bidirectional fence wait timed out after " +
                    std::to_string(Constants::FENCE_WAIT_TIMEOUT_MS / 1000) + "s (timeout #" +
                    std::to_string(g_app.fenceTimeoutCount.load()) + ")");
                if (ShouldAbortBenchmark() || g_app.fenceTimeoutCount >= Constants::MAX_FENCE_RETRIES) break;
                waitResult = vkWaitForFences(g_app.benchDevice, 2, fences, VK_TRUE, timeout);
            }
            auto endTime = std::chrono::high_resolution_clock::now();

            if (waitResult != VK_SUCCESS) {
                if (waitResult == VK_TIMEOUT) {
                    Log("[ERROR] Max fence timeouts exceeded in bidirectional test - possible GPU hang. Aborting benchmark.");
                } else {
                    Log("[ERROR] vkWaitForFences failed in bidirectional test: " + std::to_string((int)waitResult));
                }
                g_app.benchmarkAborted = true;
                break;
            }

            vkResetFences(g_app.benchDevice, 2, fences);
            g_app.fenceTimeoutCount = 0;

            
            double seconds = std::chrono::duration<double>(endTime - startTime).count();
            if (seconds > 0) {
                double bw = (static_cast<double>(size) * copies * 2 / Constants::BYTES_PER_GB) / seconds;
                bandwidths.push_back(bw);
            }

            g_app.progress = static_cast<float>(i + 1) / static_cast<float>(batches);
        }
    } else {
        // ---- SINGLE QUEUE FALLBACK: interleaved copies ----
        for (int i = 0; i < batches && !ShouldAbortBenchmark(); ++i) {
            if (i % 8 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            
            BeginBenchCommandBuffer();
            VkBufferCopy copyRegion = {};
            copyRegion.size = size;
            for (int j = 0; j < copies; ++j) {
                vkCmdCopyBuffer(g_app.benchCommandBuffer, cpuUpload.buffer, gpuDefault.buffer, 1, &copyRegion);
                vkCmdCopyBuffer(g_app.benchCommandBuffer, gpuSrc.buffer, cpuReadback.buffer, 1, &copyRegion);
            }
            vkEndCommandBuffer(g_app.benchCommandBuffer);

            auto startTime = std::chrono::high_resolution_clock::now();
            
            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &g_app.benchCommandBuffer;
            vkQueueSubmit(g_app.benchQueue, 1, &submitInfo, g_app.benchFence);
            
            FenceWaitResult fenceResult = WaitForBenchFenceEx();
            auto endTime = std::chrono::high_resolution_clock::now();
            
            if (fenceResult != FenceWaitResult::Success || g_app.benchmarkAborted) break;

            double seconds = std::chrono::duration<double>(endTime - startTime).count();
            if (seconds > 0) {
                double bw = (static_cast<double>(size) * copies * 2 / Constants::BYTES_PER_GB) / seconds;
                bandwidths.push_back(bw);
            }

            g_app.progress = static_cast<float>(i + 1) / static_cast<float>(batches);
        }
    }

    if (!bandwidths.empty()) {
        std::sort(bandwidths.begin(), bandwidths.end());
        result.minValue = bandwidths.front();
        result.maxValue = bandwidths.back();
        result.avgValue = std::accumulate(bandwidths.begin(), bandwidths.end(), 0.0) / bandwidths.size();
        result.samples = std::move(bandwidths);
    } else {
        Log("[WARNING] No valid bidirectional samples collected");
    }

    cpuUpload.Destroy(g_app.benchDevice);
    gpuDefault.Destroy(g_app.benchDevice);
    gpuSrc.Destroy(g_app.benchDevice);
    cpuReadback.Destroy(g_app.benchDevice);

    return result;
}

// Helper to aggregate results with the same base test name
std::vector<BenchmarkResult> AggregateResults(const std::vector<BenchmarkResult>& rawResults) {
    // Map from base test name to aggregated samples
    std::map<std::string, std::vector<double>> aggregatedSamples;
    std::map<std::string, std::string> unitMap;
    
    for (const auto& r : rawResults) {
        // Extract base name (remove " Run X" suffix if present)
        std::string baseName = r.testName;
        size_t runPos = baseName.find(" Run ");
        if (runPos != std::string::npos) {
            baseName = baseName.substr(0, runPos);
        }
        
        // Append all samples
        for (double s : r.samples) {
            aggregatedSamples[baseName].push_back(s);
        }
        unitMap[baseName] = r.unit;
    }
    
    // Build aggregated results
    std::vector<BenchmarkResult> aggregated;
    for (auto& [name, samples] : aggregatedSamples) {
        if (samples.empty()) continue;
        
        BenchmarkResult r;
        r.testName = name;
        r.unit = unitMap[name];
        
        std::sort(samples.begin(), samples.end());
        r.minValue = samples.front();
        r.maxValue = samples.back();
        r.avgValue = std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
        r.samples = std::move(samples);
        
        aggregated.push_back(r);
    }
    
    return aggregated;
}

// ============================================================================
// VRAM SCANNING / TESTING
// ============================================================================
// This provides a vendor-agnostic way to detect VRAM errors using Vulkan.
// It's NOT a replacement for vendor tools like NVIDIA MATS, but can help
// identify obvious VRAM issues before RMA or troubleshooting.

// Get pattern name for logging
std::string GetPatternName(VRAMTestPattern pattern) {
    switch (pattern) {
        case VRAMTestPattern::AllZeros:            return "All Zeros (0x00)";
        case VRAMTestPattern::AllOnes:             return "All Ones (0xFF)";
        case VRAMTestPattern::Checkerboard:        return "Checkerboard (0xAA)";
        case VRAMTestPattern::InverseCheckerboard: return "Inv. Checkerboard (0x55)";
        case VRAMTestPattern::Random:              return "Random Data";
        case VRAMTestPattern::MarchingOnes:        return "Marching Ones";
        case VRAMTestPattern::MarchingZeros:       return "Marching Zeros";
        case VRAMTestPattern::AddressPattern:      return "Address Pattern";
        default:                                   return "Unknown";
    }
}

// Generate test pattern data
void GenerateTestPattern(VRAMTestPattern pattern, uint32_t* data, size_t count, int iteration = 0) {
    switch (pattern) {
        case VRAMTestPattern::AllZeros:
            std::fill(data, data + count, 0x00000000);
            break;
            
        case VRAMTestPattern::AllOnes:
            std::fill(data, data + count, 0xFFFFFFFF);
            break;
            
        case VRAMTestPattern::Checkerboard:
            std::fill(data, data + count, 0xAAAAAAAA);
            break;
            
        case VRAMTestPattern::InverseCheckerboard:
            std::fill(data, data + count, 0x55555555);
            break;
            
        case VRAMTestPattern::Random: {
            // Use a fixed seed for reproducibility - the pattern just needs to be
            // pseudo-random, not cryptographically random. Using iteration allows
            // multiple passes to use different patterns.
            // IMPORTANT: Must be exactly reproducible between write and verify!
            const uint32_t RANDOM_BASE_SEED = 0xDEADBEEF;
            std::mt19937 rng(RANDOM_BASE_SEED + static_cast<uint32_t>(iteration));
            std::uniform_int_distribution<uint32_t> dist;
            for (size_t i = 0; i < count; ++i) {
                data[i] = dist(rng);
            }
            break;
        }
        
        case VRAMTestPattern::MarchingOnes: {
            // Walking 1 bit pattern - iteration determines which bit is set
            uint32_t pattern_val = 1u << (iteration % 32);
            std::fill(data, data + count, pattern_val);
            break;
        }
        
        case VRAMTestPattern::MarchingZeros: {
            // Walking 0 bit pattern - all 1s except one bit
            uint32_t pattern_val = ~(1u << (iteration % 32));
            std::fill(data, data + count, pattern_val);
            break;
        }
        
        case VRAMTestPattern::AddressPattern:
            // Each dword contains its offset - helps locate physical errors
            for (size_t i = 0; i < count; ++i) {
                data[i] = static_cast<uint32_t>(i);
            }
            break;
    }
}

// Popcount on uint32_t. Portable manual implementation.
inline uint32_t PopCount32(uint32_t v) {
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    v = (v + (v >> 4)) & 0x0F0F0F0Fu;
    return (v * 0x01010101u) >> 24;
}

// Classify a single dword mismatch.
inline VRAMErrorKind ClassifyError(uint32_t expected, uint32_t actual,
                                   uint32_t flipMask, uint32_t flipCount,
                                   int* outBitIndex) {
    if (outBitIndex) *outBitIndex = -1;
    if (actual == 0x00000000u && expected != 0x00000000u) return VRAMErrorKind::StuckAtZero;
    if (actual == 0xFFFFFFFFu && expected != 0xFFFFFFFFu) return VRAMErrorKind::StuckAtOne;
    if (flipCount == 1) {
        if (outBitIndex) {
            for (int b = 0; b < 32; ++b) {
                if (flipMask & (1u << b)) { *outBitIndex = b; break; }
            }
        }
        return VRAMErrorKind::SingleBit;
    }
    if (flipCount >= 2 && flipCount <= 6) return VRAMErrorKind::MultiBit;
    return VRAMErrorKind::AddressBus;
}

// Generate a Fisher-Yates shuffled list of block offsets covering [0, totalBytes).
std::vector<size_t> GenerateBlockReadOrder(size_t totalBytes, size_t blockBytes) {
    if (blockBytes == 0) blockBytes = 65536;
    size_t numBlocks = (totalBytes + blockBytes - 1) / blockBytes;
    std::vector<size_t> order(numBlocks);
    for (size_t i = 0; i < numBlocks; ++i) order[i] = i * blockBytes;
    std::mt19937 rng(0xCAFEBABEu);
    for (size_t i = numBlocks; i > 1; --i) {
        std::uniform_int_distribution<size_t> dist(0, i - 1);
        size_t j = dist(rng);
        std::swap(order[i - 1], order[j]);
    }
    return order;
}

// Helper: process a single dword mismatch.
static void RecordDwordError(uint32_t expected, uint32_t actual, size_t byteOffset,
                             VRAMTestPattern pattern, VRAMErrorKind forcedKind,
                             bool forceKind,
                             const size_t CLUSTER_THRESHOLD,
                             VRAMError& currentCluster, bool& inCluster,
                             std::vector<VRAMError>& errors,
                             VRAMTestResult* aggResult)
{
    uint32_t flipMask = expected ^ actual;
    uint32_t flipCount = PopCount32(flipMask);

    int bitIdx = -1;
    VRAMErrorKind kind = forceKind ? forcedKind
                                   : ClassifyError(expected, actual, flipMask, flipCount, &bitIdx);
    if (forceKind && kind == VRAMErrorKind::SingleBit && flipCount == 1) {
        for (int b = 0; b < 32; ++b) {
            if (flipMask & (1u << b)) { bitIdx = b; break; }
        }
    }

    if (aggResult) {
        for (int b = 0; b < 32; ++b) {
            if (flipMask & (1u << b)) aggResult->bitFlipHistogram[b]++;
        }
        size_t kindIdx = static_cast<size_t>(kind);
        if (kindIdx < aggResult->errorKindCounts.size()) {
            aggResult->errorKindCounts[kindIdx]++;
        }
        if (kind == VRAMErrorKind::RefreshError) aggResult->refreshPassErrors++;
    }

    if (!inCluster) {
        currentCluster = {};
        currentCluster.offsetStart = byteOffset;
        currentCluster.offsetEnd = byteOffset + sizeof(uint32_t);
        currentCluster.expected = expected;
        currentCluster.actual = actual;
        currentCluster.pattern = pattern;
        currentCluster.errorCount = 1;
        currentCluster.bitFlipMask = flipMask;
        currentCluster.bitFlipCount = flipCount;
        currentCluster.bitIndex = bitIdx;
        currentCluster.kind = kind;
        inCluster = true;
    } else if (byteOffset - currentCluster.offsetEnd <= CLUSTER_THRESHOLD * sizeof(uint32_t)) {
        currentCluster.offsetEnd = byteOffset + sizeof(uint32_t);
        currentCluster.errorCount++;
    } else {
        errors.push_back(currentCluster);
        currentCluster = {};
        currentCluster.offsetStart = byteOffset;
        currentCluster.offsetEnd = byteOffset + sizeof(uint32_t);
        currentCluster.expected = expected;
        currentCluster.actual = actual;
        currentCluster.pattern = pattern;
        currentCluster.errorCount = 1;
        currentCluster.bitFlipMask = flipMask;
        currentCluster.bitFlipCount = flipCount;
        currentCluster.bitIndex = bitIdx;
        currentCluster.kind = kind;
    }
}

// Compare buffers and find errors. If config.vramNonSequentialEnabled is true,
// reads occur in randomized block order to defeat row buffer caching.
void CompareBuffers(const uint32_t* expected, const uint32_t* actual, size_t count,
                   VRAMTestPattern pattern, std::vector<VRAMError>& errors,
                   size_t baseOffset, size_t& totalErrorCount,
                   VRAMTestResult* aggResult = nullptr,
                   bool forceRefreshKind = false) {

    const size_t CLUSTER_THRESHOLD = 256u;
    VRAMError currentCluster;
    bool inCluster = false;

    bool useNonSequential = g_app.config.vramNonSequentialEnabled;
    size_t blockBytes = static_cast<size_t>(g_app.config.vramNonSequentialBlockSize);
    if (blockBytes == 0) blockBytes = 65536;
    size_t dwordsPerBlock = blockBytes / sizeof(uint32_t);
    if (dwordsPerBlock == 0) dwordsPerBlock = 1;

    if (useNonSequential && count > dwordsPerBlock) {
        size_t numBlocks = (count + dwordsPerBlock - 1) / dwordsPerBlock;
        std::vector<size_t> blockOrder(numBlocks);
        for (size_t i = 0; i < numBlocks; ++i) blockOrder[i] = i * dwordsPerBlock;
        std::mt19937 rng(0xCAFEBABEu);
        for (size_t i = numBlocks; i > 1; --i) {
            std::uniform_int_distribution<size_t> dist(0, i - 1);
            size_t j = dist(rng);
            std::swap(blockOrder[i - 1], blockOrder[j]);
        }
        for (size_t b = 0; b < numBlocks; ++b) {
            size_t blockStart = blockOrder[b];
            size_t blockEnd = std::min(blockStart + dwordsPerBlock, count);
            if (inCluster) {
                errors.push_back(currentCluster);
                inCluster = false;
            }
            for (size_t i = blockStart; i < blockEnd; ++i) {
                if (expected[i] != actual[i]) {
                    totalErrorCount++;
                    size_t byteOffset = baseOffset + (i * sizeof(uint32_t));
                    RecordDwordError(expected[i], actual[i], byteOffset, pattern,
                                     VRAMErrorKind::RefreshError, forceRefreshKind,
                                     CLUSTER_THRESHOLD, currentCluster, inCluster,
                                     errors, aggResult);
                }
            }
        }
    } else {
        for (size_t i = 0; i < count; ++i) {
            if (expected[i] != actual[i]) {
                totalErrorCount++;
                size_t byteOffset = baseOffset + (i * sizeof(uint32_t));
                RecordDwordError(expected[i], actual[i], byteOffset, pattern,
                                 VRAMErrorKind::RefreshError, forceRefreshKind,
                                 CLUSTER_THRESHOLD, currentCluster, inCluster,
                                 errors, aggResult);
            }
        }
    }

    if (inCluster) {
        errors.push_back(currentCluster);
    }
}

// Format error address as hex string
std::string FormatErrorAddress(size_t offset) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << offset;
    return oss.str();
}


bool RunVRAMPatternTest(VRAMTestPattern pattern, size_t regionSize, size_t regionOffset,
                        VkBufferAllocation& uploadBuffer, VkBufferAllocation& gpuBuffer,
                        VkBufferAllocation& readbackBuffer, std::vector<VRAMError>& errors,
                        size_t& totalErrors, int iteration = 0) {
    
    if (g_app.vramTestCancelRequested) return false;
    
    // Ensure region size is aligned to 4 bytes
    regionSize = regionSize & ~3ULL;
    if (regionSize == 0) return true;
    
    size_t dwordCount = regionSize / sizeof(uint32_t);
    
    // Map upload buffer and write pattern
    void* mappedData = nullptr;
    VkResult mapResult = vkMapMemory(g_app.benchDevice, uploadBuffer.memory, 0, regionSize, 0, &mappedData);
    if (mapResult != VK_SUCCESS) {
        Log("[ERROR] Failed to map upload buffer for VRAM test");
        return false;
    }
    
    if (g_app.vramTestCancelRequested) {
        vkUnmapMemory(g_app.benchDevice, uploadBuffer.memory);
        return false;
    }
    
    uint32_t* uploadData = static_cast<uint32_t*>(mappedData);
    GenerateTestPattern(pattern, uploadData, dwordCount, iteration);
    
    // Flush (if not host coherent, but our Upload type uses HOST_COHERENT)
    vkUnmapMemory(g_app.benchDevice, uploadBuffer.memory);
    
    // Copy pattern to GPU
    BeginBenchCommandBuffer();
    VkBufferCopy copyRegion = {};
    copyRegion.size = regionSize;
    vkCmdCopyBuffer(g_app.benchCommandBuffer, uploadBuffer.buffer, gpuBuffer.buffer, 1, &copyRegion);
    
    FenceWaitResult result = EndAndSubmitBenchCommandBuffer();
    if (result != FenceWaitResult::Success) {
        Log("[ERROR] GPU fence wait failed during VRAM write");
        return false;
    }
    
    if (g_app.vramTestCancelRequested) return false;
    
    // Memory barrier and copy back
    BeginBenchCommandBuffer();
    
    VkMemoryBarrier memBarrier = {};
    memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    memBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(g_app.benchCommandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &memBarrier, 0, nullptr, 0, nullptr);
    
    vkCmdCopyBuffer(g_app.benchCommandBuffer, gpuBuffer.buffer, readbackBuffer.buffer, 1, &copyRegion);
    
    result = EndAndSubmitBenchCommandBuffer();
    if (result != FenceWaitResult::Success) {
        Log("[ERROR] GPU fence wait failed during VRAM read");
        return false;
    }
    
    // Map readback buffer and compare
    void* readbackData = nullptr;
    mapResult = vkMapMemory(g_app.benchDevice, readbackBuffer.memory, 0, regionSize, 0, &readbackData);
    if (mapResult != VK_SUCCESS) {
        Log("[ERROR] Failed to map readback buffer for VRAM test");
        return false;
    }
    
    // Invalidate memory range for readback (if HOST_CACHED but not HOST_COHERENT)
    VkMappedMemoryRange memRange = {};
    memRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    memRange.memory = readbackBuffer.memory;
    memRange.offset = 0;
    memRange.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(g_app.benchDevice, 1, &memRange);
    
    // Regenerate expected pattern for comparison
    std::vector<uint32_t> expectedData(dwordCount);
    GenerateTestPattern(pattern, expectedData.data(), dwordCount, iteration);
    
    // Compare
    const uint32_t* actualData = static_cast<const uint32_t*>(readbackData);
    CompareBuffers(expectedData.data(), actualData, dwordCount, pattern, errors,
                   regionOffset, totalErrors, &g_app.vramTestResult, false);
    
    vkUnmapMemory(g_app.benchDevice, readbackBuffer.memory);
    
    return true;
}

// Apply a VRAM scan preset by snapping all individual options to the preset's values.
void ApplyVRAMScanPreset(VRAMScanPreset preset, BenchmarkConfig& cfg) {
    cfg.vramScanPreset = preset;
    cfg.vramPreheatEnabled = false;
    cfg.vramPreheatSeconds = 30;
    cfg.vramMarathonMode = false;
    cfg.vramGpuVerify = false;
    switch (preset) {
        case VRAMScanPreset::Quick:
            cfg.vramPatternsEnabled = { true, true, true, false, true, false, false, false };
            cfg.vramRereadEnabled = false;
            cfg.vramRereadIterations = 4;
            cfg.vramNonSequentialEnabled = false;
            cfg.vramNonSequentialBlockSize = 65536;
            cfg.vramCoveragePercent = 50;
            break;
        case VRAMScanPreset::Standard:
            cfg.vramPatternsEnabled = { true, true, true, true, true, true, true, true };
            cfg.vramRereadEnabled = false;
            cfg.vramRereadIterations = 4;
            cfg.vramNonSequentialEnabled = false;
            cfg.vramNonSequentialBlockSize = 65536;
            cfg.vramCoveragePercent = 80;
            break;
        case VRAMScanPreset::Deep:
            cfg.vramPatternsEnabled = { true, true, true, true, true, true, true, true };
            cfg.vramRereadEnabled = true;
            cfg.vramRereadIterations = 4;
            cfg.vramNonSequentialEnabled = true;
            cfg.vramNonSequentialBlockSize = 65536;
            cfg.vramCoveragePercent = 90;
            cfg.vramPreheatEnabled = true;
            cfg.vramPreheatSeconds = 30;
            cfg.vramGpuVerify = true;
            break;
        case VRAMScanPreset::Thorough:
            cfg.vramPatternsEnabled = { true, true, true, true, true, true, true, true };
            cfg.vramRereadEnabled = true;
            cfg.vramRereadIterations = 10;
            cfg.vramNonSequentialEnabled = true;
            cfg.vramNonSequentialBlockSize = 65536;
            cfg.vramCoveragePercent = 95;
            cfg.vramPreheatEnabled = true;
            cfg.vramPreheatSeconds = 60;
            cfg.vramGpuVerify = true;
            break;
        case VRAMScanPreset::Marathon:
            cfg.vramPatternsEnabled = { true, true, true, true, true, true, true, true };
            cfg.vramRereadEnabled = true;
            cfg.vramRereadIterations = 10;
            cfg.vramNonSequentialEnabled = true;
            cfg.vramNonSequentialBlockSize = 65536;
            cfg.vramCoveragePercent = 95;
            cfg.vramPreheatEnabled = true;
            cfg.vramPreheatSeconds = 30;
            cfg.vramGpuVerify = true;
            cfg.vramMarathonMode = true;
            break;
        case VRAMScanPreset::Custom:
        default:
            break;
    }
}

VRAMScanPreset DetectVRAMScanPreset(const BenchmarkConfig& cfg) {
    auto matches = [&](const BenchmarkConfig& tmp) -> bool {
        return tmp.vramPatternsEnabled == cfg.vramPatternsEnabled
            && tmp.vramRereadEnabled == cfg.vramRereadEnabled
            && tmp.vramRereadIterations == cfg.vramRereadIterations
            && tmp.vramNonSequentialEnabled == cfg.vramNonSequentialEnabled
            && tmp.vramNonSequentialBlockSize == cfg.vramNonSequentialBlockSize
            && tmp.vramCoveragePercent == cfg.vramCoveragePercent
            && tmp.vramPreheatEnabled == cfg.vramPreheatEnabled
            && tmp.vramPreheatSeconds == cfg.vramPreheatSeconds
            && tmp.vramMarathonMode == cfg.vramMarathonMode
            && tmp.vramGpuVerify == cfg.vramGpuVerify;
    };
    for (auto p : { VRAMScanPreset::Quick, VRAMScanPreset::Standard,
                    VRAMScanPreset::Deep, VRAMScanPreset::Thorough,
                    VRAMScanPreset::Marathon }) {
        BenchmarkConfig tmp;
        ApplyVRAMScanPreset(p, tmp);
        if (matches(tmp)) return p;
    }
    return VRAMScanPreset::Custom;
}

// SPIR-V compute shader: VRAM pattern verification (631 words / 2524 bytes)
// Generated from vram_verify.comp with glslc -O --target-env=vulkan1.1
static const uint32_t g_vramVerifySPIRV[] = {
    0x07230203, 0x00010300, 0x000d000b, 0x000000d6, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x00000058, 0x00060010, 0x00000004,
    0x00000011, 0x00000100, 0x00000001, 0x00000001, 0x00030047, 0x0000000c, 0x00000002, 0x00050048,
    0x0000000c, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x0000000c, 0x00000001, 0x00000023,
    0x00000004, 0x00050048, 0x0000000c, 0x00000002, 0x00000023, 0x00000008, 0x00050048, 0x0000000c,
    0x00000003, 0x00000023, 0x0000000c, 0x00040047, 0x00000058, 0x0000000b, 0x0000001c, 0x00040047,
    0x00000069, 0x00000006, 0x00000004, 0x00030047, 0x0000006a, 0x00000002, 0x00040048, 0x0000006a,
    0x00000000, 0x00000013, 0x00040048, 0x0000006a, 0x00000000, 0x00000018, 0x00050048, 0x0000006a,
    0x00000000, 0x00000023, 0x00000000, 0x00030047, 0x0000006c, 0x00000013, 0x00030047, 0x0000006c,
    0x00000018, 0x00040047, 0x0000006c, 0x00000021, 0x00000000, 0x00040047, 0x0000006c, 0x00000022,
    0x00000000, 0x00040047, 0x0000007b, 0x00000006, 0x00000004, 0x00030047, 0x0000007c, 0x00000002,
    0x00050048, 0x0000007c, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x0000007c, 0x00000001,
    0x00000023, 0x00000004, 0x00040047, 0x0000007e, 0x00000021, 0x00000001, 0x00040047, 0x0000007e,
    0x00000022, 0x00000000, 0x00040047, 0x00000095, 0x0000000b, 0x00000019, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x0006001e,
    0x0000000c, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00040020, 0x0000000d, 0x00000009,
    0x0000000c, 0x0004003b, 0x0000000d, 0x0000000e, 0x00000009, 0x00040015, 0x0000000f, 0x00000020,
    0x00000001, 0x0004002b, 0x0000000f, 0x00000010, 0x00000001, 0x00040020, 0x00000011, 0x00000009,
    0x00000006, 0x0004002b, 0x00000006, 0x00000014, 0x00000000, 0x00020014, 0x00000015, 0x0004002b,
    0x00000006, 0x0000001c, 0x00000001, 0x0004002b, 0x00000006, 0x00000020, 0xffffffff, 0x0004002b,
    0x00000006, 0x00000024, 0x00000002, 0x0004002b, 0x00000006, 0x00000028, 0xaaaaaaaa, 0x0004002b,
    0x00000006, 0x0000002c, 0x00000003, 0x0004002b, 0x00000006, 0x00000030, 0x55555555, 0x0004002b,
    0x00000006, 0x00000034, 0x00000005, 0x0004002b, 0x0000000f, 0x00000038, 0x00000002, 0x0004002b,
    0x00000006, 0x0000003b, 0x0000001f, 0x0004002b, 0x00000006, 0x00000041, 0x00000006, 0x0004002b,
    0x00000006, 0x0000004d, 0x00000007, 0x00040017, 0x00000056, 0x00000006, 0x00000003, 0x00040020,
    0x00000057, 0x00000001, 0x00000056, 0x0004003b, 0x00000057, 0x00000058, 0x00000001, 0x00040020,
    0x00000059, 0x00000001, 0x00000006, 0x0004002b, 0x0000000f, 0x0000005c, 0x00000003, 0x0004002b,
    0x0000000f, 0x00000061, 0x00000000, 0x0003001d, 0x00000069, 0x00000006, 0x0003001e, 0x0000006a,
    0x00000069, 0x00040020, 0x0000006b, 0x0000000c, 0x0000006a, 0x0004003b, 0x0000006b, 0x0000006c,
    0x0000000c, 0x00040020, 0x0000006e, 0x0000000c, 0x00000006, 0x0003001d, 0x0000007b, 0x00000006,
    0x0004001e, 0x0000007c, 0x00000006, 0x0000007b, 0x00040020, 0x0000007d, 0x0000000c, 0x0000007c,
    0x0004003b, 0x0000007d, 0x0000007e, 0x0000000c, 0x0004002b, 0x00000006, 0x00000082, 0x00000100,
    0x0006002c, 0x00000056, 0x00000095, 0x00000082, 0x0000001c, 0x0000001c, 0x00050036, 0x00000002,
    0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x000300f7, 0x00000096, 0x00000000,
    0x000300fb, 0x00000014, 0x00000097, 0x000200f8, 0x00000097, 0x00050041, 0x00000059, 0x0000005a,
    0x00000058, 0x00000014, 0x0004003d, 0x00000006, 0x0000005b, 0x0000005a, 0x00050041, 0x00000011,
    0x0000005d, 0x0000000e, 0x0000005c, 0x0004003d, 0x00000006, 0x0000005e, 0x0000005d, 0x00050080,
    0x00000006, 0x0000005f, 0x0000005b, 0x0000005e, 0x00050041, 0x00000011, 0x00000062, 0x0000000e,
    0x00000061, 0x0004003d, 0x00000006, 0x00000063, 0x00000062, 0x000500ae, 0x00000015, 0x00000064,
    0x0000005f, 0x00000063, 0x000300f7, 0x00000066, 0x00000000, 0x000400fa, 0x00000064, 0x00000065,
    0x00000066, 0x000200f8, 0x00000065, 0x000200f9, 0x00000096, 0x000200f8, 0x00000066, 0x00060041,
    0x0000006e, 0x0000006f, 0x0000006c, 0x00000061, 0x0000005f, 0x0004003d, 0x00000006, 0x00000070,
    0x0000006f, 0x000300f7, 0x000000d3, 0x00000000, 0x000300fb, 0x00000014, 0x000000a5, 0x000200f8,
    0x000000a5, 0x00050041, 0x00000011, 0x000000a6, 0x0000000e, 0x00000010, 0x0004003d, 0x00000006,
    0x000000a7, 0x000000a6, 0x000500aa, 0x00000015, 0x000000a8, 0x000000a7, 0x00000014, 0x000300f7,
    0x000000aa, 0x00000000, 0x000400fa, 0x000000a8, 0x000000a9, 0x000000aa, 0x000200f8, 0x000000a9,
    0x000200f9, 0x000000d3, 0x000200f8, 0x000000aa, 0x000500aa, 0x00000015, 0x000000ad, 0x000000a7,
    0x0000001c, 0x000300f7, 0x000000af, 0x00000000, 0x000400fa, 0x000000ad, 0x000000ae, 0x000000af,
    0x000200f8, 0x000000ae, 0x000200f9, 0x000000d3, 0x000200f8, 0x000000af, 0x000500aa, 0x00000015,
    0x000000b2, 0x000000a7, 0x00000024, 0x000300f7, 0x000000b4, 0x00000000, 0x000400fa, 0x000000b2,
    0x000000b3, 0x000000b4, 0x000200f8, 0x000000b3, 0x000200f9, 0x000000d3, 0x000200f8, 0x000000b4,
    0x000500aa, 0x00000015, 0x000000b7, 0x000000a7, 0x0000002c, 0x000300f7, 0x000000b9, 0x00000000,
    0x000400fa, 0x000000b7, 0x000000b8, 0x000000b9, 0x000200f8, 0x000000b8, 0x000200f9, 0x000000d3,
    0x000200f8, 0x000000b9, 0x000500aa, 0x00000015, 0x000000bc, 0x000000a7, 0x00000034, 0x000300f7,
    0x000000c2, 0x00000000, 0x000400fa, 0x000000bc, 0x000000bd, 0x000000c2, 0x000200f8, 0x000000bd,
    0x00050041, 0x00000011, 0x000000be, 0x0000000e, 0x00000038, 0x0004003d, 0x00000006, 0x000000bf,
    0x000000be, 0x000500c7, 0x00000006, 0x000000c0, 0x000000bf, 0x0000003b, 0x000500c4, 0x00000006,
    0x000000c1, 0x0000001c, 0x000000c0, 0x000200f9, 0x000000d3, 0x000200f8, 0x000000c2, 0x000500aa,
    0x00000015, 0x000000c5, 0x000000a7, 0x00000041, 0x000300f7, 0x000000cc, 0x00000000, 0x000400fa,
    0x000000c5, 0x000000c6, 0x000000cc, 0x000200f8, 0x000000c6, 0x00050041, 0x00000011, 0x000000c7,
    0x0000000e, 0x00000038, 0x0004003d, 0x00000006, 0x000000c8, 0x000000c7, 0x000500c7, 0x00000006,
    0x000000c9, 0x000000c8, 0x0000003b, 0x000500c4, 0x00000006, 0x000000ca, 0x0000001c, 0x000000c9,
    0x000400c8, 0x00000006, 0x000000cb, 0x000000ca, 0x000200f9, 0x000000d3, 0x000200f8, 0x000000cc,
    0x000500aa, 0x00000015, 0x000000cf, 0x000000a7, 0x0000004d, 0x000300f7, 0x000000d2, 0x00000000,
    0x000400fa, 0x000000cf, 0x000000d0, 0x000000d2, 0x000200f8, 0x000000d0, 0x000200f9, 0x000000d3,
    0x000200f8, 0x000000d2, 0x000200f9, 0x000000d3, 0x000200f8, 0x000000d3, 0x001300f5, 0x00000006,
    0x000000d5, 0x00000014, 0x000000a9, 0x00000020, 0x000000ae, 0x00000028, 0x000000b3, 0x00000030,
    0x000000b8, 0x000000c1, 0x000000bd, 0x000000cb, 0x000000c6, 0x0000005f, 0x000000d0, 0x00000014,
    0x000000d2, 0x000500ab, 0x00000015, 0x00000077, 0x00000070, 0x000000d5, 0x000300f7, 0x00000079,
    0x00000000, 0x000400fa, 0x00000077, 0x00000078, 0x00000079, 0x000200f8, 0x00000078, 0x00050041,
    0x0000006e, 0x0000007f, 0x0000007e, 0x00000061, 0x000700ea, 0x00000006, 0x00000080, 0x0000007f,
    0x0000001c, 0x00000014, 0x0000001c, 0x000500b0, 0x00000015, 0x00000083, 0x00000080, 0x00000082,
    0x000300f7, 0x00000085, 0x00000000, 0x000400fa, 0x00000083, 0x00000084, 0x00000085, 0x000200f8,
    0x00000084, 0x00050084, 0x00000006, 0x00000087, 0x00000080, 0x0000002c, 0x00060041, 0x0000006e,
    0x0000008a, 0x0000007e, 0x00000010, 0x00000087, 0x0003003e, 0x0000008a, 0x0000005f, 0x00050080,
    0x00000006, 0x0000008d, 0x00000087, 0x0000001c, 0x00060041, 0x0000006e, 0x0000008f, 0x0000007e,
    0x00000010, 0x0000008d, 0x0003003e, 0x0000008f, 0x000000d5, 0x00050080, 0x00000006, 0x00000092,
    0x00000087, 0x00000024, 0x00060041, 0x0000006e, 0x00000094, 0x0000007e, 0x00000010, 0x00000092,
    0x0003003e, 0x00000094, 0x00000070, 0x000200f9, 0x00000085, 0x000200f8, 0x00000085, 0x000200f9,
    0x00000079, 0x000200f8, 0x00000079, 0x000200f9, 0x00000096, 0x000200f8, 0x00000096, 0x000100fd,
    0x00010038,
};
static const size_t g_vramVerifySPIRVSize = 2596;

bool PreheatGPUForVRAMScan(int durationSeconds) {
    if (durationSeconds <= 0) return true;
    Log("Pre-heating GPU for " + std::to_string(durationSeconds) + " seconds...");
    SetVramPattern("Pre-heat (warming GPU)");

    const size_t SCRATCH = 64ull * 1024 * 1024;
    auto srcBuf = CreateBuffer(VkBufferType::DeviceLocal, SCRATCH);
    auto dstBuf = CreateBuffer(VkBufferType::DeviceLocal, SCRATCH);
    if (!srcBuf || !dstBuf) {
        Log("[WARNING] Could not allocate pre-heat scratch buffers; skipping pre-heat");
        srcBuf.Destroy(g_app.benchDevice);
        dstBuf.Destroy(g_app.benchDevice);
        return true;
    }

    auto startTime = std::chrono::steady_clock::now();
    int copiesDone = 0;
    int lastReportedSec = -1;
    while (!g_app.vramTestCancelRequested) {
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
        if (elapsed >= durationSeconds) break;

        BeginBenchCommandBuffer();
        VkBufferCopy copy = {};
        copy.size = SCRATCH;
        for (int i = 0; i < 4; ++i) {
            if (i & 1) vkCmdCopyBuffer(g_app.benchCommandBuffer, srcBuf.buffer, dstBuf.buffer, 1, &copy);
            else       vkCmdCopyBuffer(g_app.benchCommandBuffer, dstBuf.buffer, srcBuf.buffer, 1, &copy);
        }
        if (EndAndSubmitBenchCommandBuffer() != FenceWaitResult::Success) {
            Log("[WARNING] Pre-heat fence wait failed; ending pre-heat early");
            break;
        }
        copiesDone += 4;

        int sec = static_cast<int>(elapsed);
        if (sec != lastReportedSec) {
            lastReportedSec = sec;
            g_app.vramTestProgress = static_cast<float>(elapsed / durationSeconds);
        }
    }

    srcBuf.Destroy(g_app.benchDevice);
    dstBuf.Destroy(g_app.benchDevice);
    Log("Pre-heat done (" + std::to_string(copiesDone) + " copies of "
        + FormatSize(SCRATCH) + ")");
    g_app.vramTestProgress = 0.0f;
    return true;
}

struct VRAMGpuVerifyState {
    VkShaderModule           shaderModule       = VK_NULL_HANDLE;
    VkDescriptorSetLayout    descSetLayout      = VK_NULL_HANDLE;
    VkPipelineLayout         pipelineLayout     = VK_NULL_HANDLE;
    VkPipeline               pipeline           = VK_NULL_HANDLE;
    VkDescriptorPool         descPool           = VK_NULL_HANDLE;
    VkDescriptorSet          descSet            = VK_NULL_HANDLE;
    VkBufferAllocation       errorBuf           = {};
    VkBufferAllocation       errorRb            = {};
    bool ready = false;
};

void DestroyGpuVerifyState(VRAMGpuVerifyState& s) {
    if (s.pipeline)        vkDestroyPipeline(g_app.benchDevice, s.pipeline, nullptr);
    if (s.pipelineLayout)  vkDestroyPipelineLayout(g_app.benchDevice, s.pipelineLayout, nullptr);
    if (s.descSetLayout)   vkDestroyDescriptorSetLayout(g_app.benchDevice, s.descSetLayout, nullptr);
    if (s.descPool)        vkDestroyDescriptorPool(g_app.benchDevice, s.descPool, nullptr);
    if (s.shaderModule)    vkDestroyShaderModule(g_app.benchDevice, s.shaderModule, nullptr);
    s.errorBuf.Destroy(g_app.benchDevice);
    s.errorRb.Destroy(g_app.benchDevice);
    s = {};
}

bool InitGpuVerifyState(VRAMGpuVerifyState& s) {
    s = {};
    VkShaderModuleCreateInfo smci = {};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = g_vramVerifySPIRVSize;
    smci.pCode = g_vramVerifySPIRV;
    if (vkCreateShaderModule(g_app.benchDevice, &smci, nullptr, &s.shaderModule) != VK_SUCCESS) {
        Log("[ERROR] GPU verify shader module creation failed");
        return false;
    }

    VkDescriptorSetLayoutBinding b[2] = {};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dslci = {};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 2;
    dslci.pBindings = b;
    if (vkCreateDescriptorSetLayout(g_app.benchDevice, &dslci, nullptr, &s.descSetLayout) != VK_SUCCESS) {
        Log("[ERROR] GPU verify descriptor set layout creation failed");
        DestroyGpuVerifyState(s);
        return false;
    }

    VkPushConstantRange pcr = {};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = 16;
    VkPipelineLayoutCreateInfo plci = {};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &s.descSetLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(g_app.benchDevice, &plci, nullptr, &s.pipelineLayout) != VK_SUCCESS) {
        Log("[ERROR] GPU verify pipeline layout creation failed");
        DestroyGpuVerifyState(s);
        return false;
    }

    VkComputePipelineCreateInfo cpci = {};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = s.shaderModule;
    cpci.stage.pName = "main";
    cpci.layout = s.pipelineLayout;
    if (vkCreateComputePipelines(g_app.benchDevice, VK_NULL_HANDLE, 1, &cpci, nullptr, &s.pipeline) != VK_SUCCESS) {
        Log("[ERROR] GPU verify pipeline creation failed");
        DestroyGpuVerifyState(s);
        return false;
    }

    VkDescriptorPoolSize dps = {};
    dps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    dps.descriptorCount = 2;
    VkDescriptorPoolCreateInfo dpci = {};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &dps;
    if (vkCreateDescriptorPool(g_app.benchDevice, &dpci, nullptr, &s.descPool) != VK_SUCCESS) {
        Log("[ERROR] GPU verify descriptor pool creation failed");
        DestroyGpuVerifyState(s);
        return false;
    }
    VkDescriptorSetAllocateInfo dsai = {};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = s.descPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &s.descSetLayout;
    if (vkAllocateDescriptorSets(g_app.benchDevice, &dsai, &s.descSet) != VK_SUCCESS) {
        Log("[ERROR] GPU verify descriptor set allocation failed");
        DestroyGpuVerifyState(s);
        return false;
    }

    const size_t errBufBytes = (1 + 256 * 3) * sizeof(uint32_t);
    s.errorBuf = CreateBuffer(VkBufferType::DeviceLocal, errBufBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    s.errorRb  = CreateBuffer(VkBufferType::Readback,    errBufBytes);
    if (!s.errorBuf || !s.errorRb) {
        Log("[ERROR] GPU verify error buffer allocation failed");
        DestroyGpuVerifyState(s);
        return false;
    }

    s.ready = true;
    return true;
}

bool VerifyChunkOnGPU_Vulkan(VRAMGpuVerifyState& s,
                             VkBufferAllocation& chainBuffer, size_t chainBytes,
                             VRAMTestPattern pattern, int iteration,
                             size_t baseOffset,
                             std::vector<VRAMError>& errors, size_t& patternErrors)
{
    if (!s.ready) return false;
    if (pattern == VRAMTestPattern::Random) return false;

    size_t dwordCount = chainBytes / sizeof(uint32_t);
    const size_t errBufBytes = (1 + 256 * 3) * sizeof(uint32_t);

    VkDescriptorBufferInfo bi[2] = {};
    bi[0].buffer = chainBuffer.buffer;
    bi[0].offset = 0;
    bi[0].range  = chainBytes;
    bi[1].buffer = s.errorBuf.buffer;
    bi[1].offset = 0;
    bi[1].range  = errBufBytes;
    VkWriteDescriptorSet ws[2] = {};
    ws[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ws[0].dstSet = s.descSet;
    ws[0].dstBinding = 0;
    ws[0].descriptorCount = 1;
    ws[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ws[0].pBufferInfo = &bi[0];
    ws[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ws[1].dstSet = s.descSet;
    ws[1].dstBinding = 1;
    ws[1].descriptorCount = 1;
    ws[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ws[1].pBufferInfo = &bi[1];
    vkUpdateDescriptorSets(g_app.benchDevice, 2, ws, 0, nullptr);

    {
        BeginBenchCommandBuffer();
        vkCmdFillBuffer(g_app.benchCommandBuffer, s.errorBuf.buffer, 0, errBufBytes, 0);
        VkMemoryBarrier mb = {};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(g_app.benchCommandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &mb, 0, nullptr, 0, nullptr);

        vkCmdBindPipeline(g_app.benchCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeline);
        vkCmdBindDescriptorSets(g_app.benchCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                s.pipelineLayout, 0, 1, &s.descSet, 0, nullptr);
        uint32_t pc[4] = {
            static_cast<uint32_t>(dwordCount),
            static_cast<uint32_t>(pattern),
            static_cast<uint32_t>(iteration),
            0   // baseIndex - set per sub-dispatch below
        };
        // Vulkan guarantees maxComputeWorkGroupCount[0] >= 65535 only. A 512MB
        // chunk needs ~524288 groups, so split into sub-dispatches that each
        // stay within the guaranteed limit. All sub-dispatches accumulate into
        // the same error buffer via atomicAdd, so no barrier between them is
        // needed.
        const uint32_t THREADS_PER_GROUP = 256;
        const uint32_t MAX_GROUPS_PER_DISPATCH = 65535;
        const uint64_t totalGroups = (static_cast<uint64_t>(dwordCount) + THREADS_PER_GROUP - 1) / THREADS_PER_GROUP;
        for (uint64_t groupBase = 0; groupBase < totalGroups; groupBase += MAX_GROUPS_PER_DISPATCH) {
            uint32_t groupsThis = static_cast<uint32_t>(std::min<uint64_t>(MAX_GROUPS_PER_DISPATCH, totalGroups - groupBase));
            pc[3] = static_cast<uint32_t>(groupBase * THREADS_PER_GROUP);  // baseIndex (dword offset)
            vkCmdPushConstants(g_app.benchCommandBuffer, s.pipelineLayout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
            vkCmdDispatch(g_app.benchCommandBuffer, groupsThis, 1, 1);
        }

        VkMemoryBarrier mb2 = {};
        mb2.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb2.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(g_app.benchCommandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 1, &mb2, 0, nullptr, 0, nullptr);

        VkBufferCopy cp = {};
        cp.size = errBufBytes;
        vkCmdCopyBuffer(g_app.benchCommandBuffer, s.errorBuf.buffer, s.errorRb.buffer, 1, &cp);

        if (EndAndSubmitBenchCommandBuffer() != FenceWaitResult::Success) {
            Log("[ERROR] GPU verify dispatch fence wait failed");
            return false;
        }
    }

    uint32_t errCount = 0;
    std::vector<uint32_t> records(256 * 3, 0);
    {
        void* mapped = nullptr;
        if (vkMapMemory(g_app.benchDevice, s.errorRb.memory, 0, errBufBytes, 0, &mapped) != VK_SUCCESS || !mapped) {
            Log("[ERROR] GPU verify readback map failed");
            return false;
        }
        VkMappedMemoryRange r = {};
        r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        r.memory = s.errorRb.memory;
        r.offset = 0;
        r.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(g_app.benchDevice, 1, &r);
        const uint32_t* p = static_cast<const uint32_t*>(mapped);
        errCount = p[0];
        memcpy(records.data(), p + 1, records.size() * sizeof(uint32_t));
        vkUnmapMemory(g_app.benchDevice, s.errorRb.memory);
    }

    if (errCount == 0) return true;
    patternErrors += errCount;
    size_t recordedCount = std::min<uint32_t>(errCount, 256u);

    std::vector<std::tuple<uint32_t,uint32_t,uint32_t>> sortedRecs;
    sortedRecs.reserve(recordedCount);
    for (size_t i = 0; i < recordedCount; ++i) {
        sortedRecs.emplace_back(records[i*3+0], records[i*3+1], records[i*3+2]);
    }
    std::sort(sortedRecs.begin(), sortedRecs.end(),
              [](const auto& a, const auto& b){ return std::get<0>(a) < std::get<0>(b); });

    const size_t CLUSTER_THRESHOLD = 256u;
    VRAMError currentCluster;
    bool inCluster = false;
    for (auto& rec : sortedRecs) {
        uint32_t dwordIdx = std::get<0>(rec);
        uint32_t expVal   = std::get<1>(rec);
        uint32_t actVal   = std::get<2>(rec);
        size_t byteOffset = baseOffset + (size_t)dwordIdx * sizeof(uint32_t);
        uint32_t flipMask = expVal ^ actVal;
        uint32_t flipCount = PopCount32(flipMask);
        int bitIdx = -1;
        VRAMErrorKind kind = ClassifyError(expVal, actVal, flipMask, flipCount, &bitIdx);
        for (int b = 0; b < 32; ++b) {
            if (flipMask & (1u << b)) g_app.vramTestResult.bitFlipHistogram[b]++;
        }
        size_t kindIdx = static_cast<size_t>(kind);
        if (kindIdx < g_app.vramTestResult.errorKindCounts.size()) {
            g_app.vramTestResult.errorKindCounts[kindIdx]++;
        }
        if (!inCluster) {
            currentCluster = {};
            currentCluster.offsetStart = byteOffset;
            currentCluster.offsetEnd = byteOffset + sizeof(uint32_t);
            currentCluster.expected = expVal;
            currentCluster.actual = actVal;
            currentCluster.pattern = pattern;
            currentCluster.errorCount = 1;
            currentCluster.bitFlipMask = flipMask;
            currentCluster.bitFlipCount = flipCount;
            currentCluster.bitIndex = bitIdx;
            currentCluster.kind = kind;
            inCluster = true;
        } else if (byteOffset - currentCluster.offsetEnd <= CLUSTER_THRESHOLD * sizeof(uint32_t)) {
            currentCluster.offsetEnd = byteOffset + sizeof(uint32_t);
            currentCluster.errorCount++;
        } else {
            errors.push_back(currentCluster);
            currentCluster = {};
            currentCluster.offsetStart = byteOffset;
            currentCluster.offsetEnd = byteOffset + sizeof(uint32_t);
            currentCluster.expected = expVal;
            currentCluster.actual = actVal;
            currentCluster.pattern = pattern;
            currentCluster.errorCount = 1;
            currentCluster.bitFlipMask = flipMask;
            currentCluster.bitFlipCount = flipCount;
            currentCluster.bitIndex = bitIdx;
            currentCluster.kind = kind;
        }
    }
    if (inCluster) errors.push_back(currentCluster);

    if (errCount > 256) {
        Log("  GPU verify: " + std::to_string(errCount) + " errors found ("
            + std::to_string(errCount - 256) + " beyond detail capture limit)");
    }
    return true;
}

bool RunVRAMPatternTestGpu(VRAMTestPattern pattern, size_t regionSize, size_t regionOffset,
                           VkBufferAllocation& uploadBuffer,
                           VkBufferAllocation& gpuBuffer,
                           VRAMGpuVerifyState& gpuVerify,
                           std::vector<VRAMError>& errors,
                           size_t& totalErrors, int iteration = 0)
{
    if (g_app.vramTestCancelRequested) return false;
    if (pattern == VRAMTestPattern::Random) return false;

    regionSize = regionSize & ~3ULL;
    if (regionSize == 0) return true;
    size_t dwordCount = regionSize / sizeof(uint32_t);

    void* mapped = nullptr;
    if (vkMapMemory(g_app.benchDevice, uploadBuffer.memory, 0, regionSize, 0, &mapped) != VK_SUCCESS || !mapped) {
        Log("[ERROR] GPU-verify: upload map failed");
        return false;
    }
    GenerateTestPattern(pattern, static_cast<uint32_t*>(mapped), dwordCount, iteration);
    vkUnmapMemory(g_app.benchDevice, uploadBuffer.memory);

    BeginBenchCommandBuffer();
    VkBufferCopy copy = {};
    copy.size = regionSize;
    vkCmdCopyBuffer(g_app.benchCommandBuffer, uploadBuffer.buffer, gpuBuffer.buffer, 1, &copy);
    if (EndAndSubmitBenchCommandBuffer() != FenceWaitResult::Success) {
        Log("[ERROR] GPU-verify: write fence wait failed");
        return false;
    }

    size_t patternErrors = 0;
    if (!VerifyChunkOnGPU_Vulkan(gpuVerify, gpuBuffer, regionSize, pattern, iteration,
                                  regionOffset, errors, patternErrors)) {
        return false;
    }
    totalErrors += patternErrors;
    return true;
}

// Main VRAM test thread function
void VRAMTestThreadFunc() {
    auto startTime = std::chrono::steady_clock::now();
    
    Log("=== VRAM Scan Started ===");
    Log("GPU: " + g_app.gpuList[g_app.config.selectedGPU].name);
    Log("");
    Log("DISCLAIMER: This is a basic VRAM integrity test using Vulkan.");
    Log("It can detect obvious errors but is NOT a replacement for");
    Log("vendor-specific tools like NVIDIA MATS or AMD memory diagnostics.");
    Log("For chip-level diagnosis, use manufacturer tools.");
    Log("");
    
    g_app.vramTestResult = {};
    g_app.vramTestProgress = 0.0f;
    
    g_app.fenceTimeoutCount = 0;
    g_app.benchmarkAborted = false;
    
    CleanupBenchmarkDevice();
    if (!InitBenchmarkDevice(g_app.config.selectedGPU)) {
        Log("[ERROR] Failed to initialize benchmark device for VRAM test");
        g_app.vramTestResult.completed = false;
        g_app.vramTestRunning = false;
        return;
    }
    
    const GPUInfo& gpu = g_app.gpuList[g_app.config.selectedGPU];
    
    int coveragePct = g_app.config.vramCoveragePercent;
    if (coveragePct < 10) coveragePct = 10;
    if (coveragePct > 99) coveragePct = 99;
    double targetPercent = coveragePct / 100.0;
    size_t targetTestSize = static_cast<size_t>(gpu.dedicatedVRAM * targetPercent);

    Log("Coverage target: " + std::to_string(coveragePct) + "% (" + FormatSize(targetTestSize) + ")");
    if (coveragePct >= 90) {
        Log("[WARNING] High coverage allocates near-maximum VRAM - may cause instability");
    }
    if (g_app.config.vramRereadEnabled) {
        Log("Refresh check enabled: " + std::to_string(g_app.config.vramRereadIterations) + " re-reads per chunk");
    }
    if (g_app.config.vramNonSequentialEnabled) {
        Log("Address-bus check enabled: non-sequential read order, "
            + FormatSize((size_t)g_app.config.vramNonSequentialBlockSize) + " blocks");
    }
    Log("");
    
    Log("NOTE: GPU drivers and OS reserve some VRAM for:");
    Log("  - Command buffers and page tables");
    Log("  - Desktop compositor (Wayland/X11)");
    Log("  - Vulkan runtime scratch space");
    Log("We test as much as can be allocated via Vulkan.");
    Log("");
    
    // Define test patterns - filtered by user's per-pattern enable flags.
    std::vector<VRAMTestPattern> patterns;
    auto maybeAdd = [&](VRAMTestPattern p, int idx) {
        if (idx >= 0 && idx < (int)g_app.config.vramPatternsEnabled.size()
            && g_app.config.vramPatternsEnabled[idx]) {
            patterns.push_back(p);
        }
    };
    maybeAdd(VRAMTestPattern::AllZeros,            0);
    maybeAdd(VRAMTestPattern::AllOnes,             1);
    maybeAdd(VRAMTestPattern::Checkerboard,        2);
    maybeAdd(VRAMTestPattern::InverseCheckerboard, 3);
    maybeAdd(VRAMTestPattern::Random,              4);
    bool useMarchingOnes  = g_app.config.vramPatternsEnabled.size() > 5 && g_app.config.vramPatternsEnabled[5];
    bool useMarchingZeros = g_app.config.vramPatternsEnabled.size() > 6 && g_app.config.vramPatternsEnabled[6];
    bool useAddressPattern = g_app.config.vramPatternsEnabled.size() > 7 && g_app.config.vramPatternsEnabled[7];
    if (useAddressPattern) {
        patterns.push_back(VRAMTestPattern::AddressPattern);
    }

    if (patterns.empty() && !useMarchingOnes && !useMarchingZeros) {
        Log("[ERROR] No test patterns enabled - nothing to do");
        g_app.vramTestResult.completed = false;
        g_app.vramTestRunning = false;
        return;
    }

    const int MARCH_ITERATIONS = 4;
    
    const size_t PREFERRED_CHUNK_SIZE = 512ull * 1024 * 1024;
    const size_t MIN_CHUNK_SIZE = 128ull * 1024 * 1024;
    
    size_t chunkSize = PREFERRED_CHUNK_SIZE;
    
    Log("Finding optimal chunk size...");
    
    {
        while (chunkSize >= MIN_CHUNK_SIZE) {
            if (g_app.vramTestCancelRequested) {
                g_app.vramTestResult.cancelled = true;
                g_app.vramTestRunning = false;
                return;
            }
            
            auto testUpload = CreateBuffer(VkBufferType::Upload, chunkSize);
            auto testGpu = CreateBuffer(VkBufferType::DeviceLocal, chunkSize);
            auto testReadback = CreateBuffer(VkBufferType::Readback, chunkSize);
            
            if (testUpload && testGpu && testReadback) {
                Log("Using " + FormatSize(chunkSize) + " chunk size");
                testUpload.Destroy(g_app.benchDevice);
                testGpu.Destroy(g_app.benchDevice);
                testReadback.Destroy(g_app.benchDevice);
                break;
            }
            
            testUpload.Destroy(g_app.benchDevice);
            testGpu.Destroy(g_app.benchDevice);
            testReadback.Destroy(g_app.benchDevice);
            chunkSize /= 2;
        }
        
        if (chunkSize < MIN_CHUNK_SIZE) {
            Log("[ERROR] Failed to allocate test buffers even at " + FormatSize(MIN_CHUNK_SIZE));
            Log("[ERROR] Try closing other applications to free VRAM");
            g_app.vramTestResult.completed = false;
            g_app.vramTestRunning = false;
            return;
        }
    }
    
    size_t numChunks = (targetTestSize + chunkSize - 1) / chunkSize;
    size_t patternsPerChunk = patterns.size()
                            + (useMarchingOnes ? 1 : 0)
                            + (useMarchingZeros ? 1 : 0)
                            + (g_app.config.vramRereadEnabled ? 1 : 0);
    if (patternsPerChunk == 0) patternsPerChunk = 1;
    size_t totalSteps = numChunks * patternsPerChunk;
    size_t completedSteps = 0;
    
    double targetPercentDisplay = (static_cast<double>(targetTestSize) / gpu.dedicatedVRAM) * 100.0;
    char percentBuf[64];
    snprintf(percentBuf, sizeof(percentBuf), "%.0f%%", targetPercentDisplay);
    
    Log("");
    Log("Will test " + FormatSize(targetTestSize) + " (" + percentBuf + " of VRAM) in " + 
        std::to_string(numChunks) + " chunks");
    {
        std::string per = "Each chunk: " + std::to_string(patterns.size()) + " patterns";
        if (useMarchingOnes)  per += " + marching-ones";
        if (useMarchingZeros) per += " + marching-zeros";
        if (g_app.config.vramRereadEnabled) per += " + refresh check x" + std::to_string(g_app.config.vramRereadIterations);
        Log(per);
    }
    Log("Reallocating between chunks to potentially hit different physical regions");
    Log("");
    
    size_t totalErrors = 0;
    std::vector<VRAMError> allErrors;
    bool hadCriticalFailure = false;
    size_t totalBytesTested = 0;

    // ========== PRE-HEAT PHASE ==========
    if (g_app.config.vramPreheatEnabled && !g_app.vramTestCancelRequested) {
        int sec = g_app.config.vramPreheatSeconds;
        if (sec < 5) sec = 5;
        if (sec > 600) sec = 600;
        PreheatGPUForVRAMScan(sec);
    }

    // ========== GPU VERIFY STATE ==========
    VRAMGpuVerifyState gpuVerify;
    bool useGpuVerify = g_app.config.vramGpuVerify;
    if (useGpuVerify) {
        if (!InitGpuVerifyState(gpuVerify)) {
            Log("[WARNING] GPU-verify init failed - falling back to CPU verification");
            useGpuVerify = false;
        } else {
            Log("GPU-verify enabled: using compute shader to compare patterns "
                "(skips full chunk readback; Random pattern still uses CPU)");
        }
    }

    // ========== MARATHON OUTER LOOP ==========
    int marathonPass = 0;
    do {
        marathonPass++;
        if (g_app.config.vramMarathonMode) {
            Log("");
            Log("================ Marathon pass " + std::to_string(marathonPass) + " starting ================");
            totalBytesTested = 0;
        }

    for (size_t chunkNum = 0; chunkNum < numChunks && !g_app.vramTestCancelRequested && !hadCriticalFailure; ++chunkNum) {
        size_t chunkOffset = chunkNum * chunkSize;
        size_t thisChunkSize = std::min(chunkSize, targetTestSize - chunkOffset);
        
        Log("=== Chunk " + std::to_string(chunkNum + 1) + "/" + std::to_string(numChunks) + 
            " (" + FormatSize(thisChunkSize) + " at logical offset " + FormatSize(chunkOffset) + ") ===");
        
        auto uploadBuffer = CreateBuffer(VkBufferType::Upload, thisChunkSize);
        // When GPU-verify is active the chunk buffer is bound as a storage
        // buffer by the verify shader, so it needs STORAGE_BUFFER usage.
        auto gpuBuffer = CreateBuffer(VkBufferType::DeviceLocal, thisChunkSize,
                                      useGpuVerify ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0);
        auto readbackBuffer = CreateBuffer(VkBufferType::Readback, thisChunkSize);
        
        if (!uploadBuffer || !gpuBuffer || !readbackBuffer) {
            Log("[WARNING] Failed to allocate buffers for chunk " + std::to_string(chunkNum + 1) + " - stopping");
            uploadBuffer.Destroy(g_app.benchDevice);
            gpuBuffer.Destroy(g_app.benchDevice);
            readbackBuffer.Destroy(g_app.benchDevice);
            break;
        }
        
        size_t chunkErrors = 0;
        bool chunkFailed = false;

        auto runPattern = [&](VRAMTestPattern p, int iter, size_t& patErrors) -> bool {
            if (useGpuVerify && p != VRAMTestPattern::Random) {
                return RunVRAMPatternTestGpu(p, thisChunkSize, chunkOffset,
                                             uploadBuffer, gpuBuffer, gpuVerify,
                                             allErrors, patErrors, iter);
            }
            return RunVRAMPatternTest(p, thisChunkSize, chunkOffset,
                                      uploadBuffer, gpuBuffer, readbackBuffer,
                                      allErrors, patErrors, iter);
        };

        for (const auto& pattern : patterns) {
            if (g_app.vramTestCancelRequested || chunkFailed) break;

            std::string patternName = GetPatternName(pattern);
            SetVramPattern(patternName + " [" + std::to_string(chunkNum + 1) + "/" + std::to_string(numChunks) + "]");

            g_app.fenceTimeoutCount = 0;
            size_t patternErrors = 0;

            if (!runPattern(pattern, 0, patternErrors)) {
                if (!g_app.vramTestCancelRequested) {
                    Log("  [WARNING] " + patternName + " failed");
                    chunkFailed = true;
                }
                break;
            }

            chunkErrors += patternErrors;
        }

        if (useMarchingOnes && !g_app.vramTestCancelRequested && !chunkFailed) {
            SetVramPattern("Marching ones [" + std::to_string(chunkNum + 1) + "/" + std::to_string(numChunks) + "]");
            g_app.fenceTimeoutCount = 0;

            for (int iter = 0; iter < MARCH_ITERATIONS && !g_app.vramTestCancelRequested && !chunkFailed; ++iter) {
                size_t marchErrors = 0;
                if (!runPattern(VRAMTestPattern::MarchingOnes, iter, marchErrors)) {
                    chunkFailed = true;
                    break;
                }
                chunkErrors += marchErrors;
            }
        }

        if (useMarchingZeros && !g_app.vramTestCancelRequested && !chunkFailed) {
            SetVramPattern("Marching zeros [" + std::to_string(chunkNum + 1) + "/" + std::to_string(numChunks) + "]");
            g_app.fenceTimeoutCount = 0;

            for (int iter = 0; iter < MARCH_ITERATIONS && !g_app.vramTestCancelRequested && !chunkFailed; ++iter) {
                size_t marchErrors = 0;
                if (!runPattern(VRAMTestPattern::MarchingZeros, iter, marchErrors)) {
                    chunkFailed = true;
                    break;
                }
                chunkErrors += marchErrors;
            }
        }

        // Refresh check (re-read pass): catches data retention errors.
        if (g_app.config.vramRereadEnabled && !g_app.vramTestCancelRequested && !chunkFailed) {
            int rereadIters = g_app.config.vramRereadIterations;
            if (rereadIters < 1) rereadIters = 1;
            if (rereadIters > 20) rereadIters = 20;

            SetVramPattern("Refresh check [" + std::to_string(chunkNum + 1) + "/" + std::to_string(numChunks) + "]");
            g_app.fenceTimeoutCount = 0;

            size_t regionSize = thisChunkSize & ~3ULL;
            size_t dwordCount = regionSize / sizeof(uint32_t);
            std::vector<uint32_t> expectedData(dwordCount);
            std::mt19937 refreshRng(0xFEEDF00Du + static_cast<uint32_t>(chunkNum));
            std::uniform_int_distribution<uint32_t> dist;
            for (size_t i = 0; i < dwordCount; ++i) expectedData[i] = dist(refreshRng);

            // Step 1: write pattern once
            void* uploadMap = nullptr;
            if (vkMapMemory(g_app.benchDevice, uploadBuffer.memory, 0, regionSize, 0, &uploadMap) == VK_SUCCESS && uploadMap) {
                memcpy(uploadMap, expectedData.data(), regionSize);
                vkUnmapMemory(g_app.benchDevice, uploadBuffer.memory);

                BeginBenchCommandBuffer();
                VkBufferCopy copyW = {};
                copyW.size = regionSize;
                vkCmdCopyBuffer(g_app.benchCommandBuffer, uploadBuffer.buffer, gpuBuffer.buffer, 1, &copyW);
                if (EndAndSubmitBenchCommandBuffer() == FenceWaitResult::Success) {
                    for (int rr = 0; rr < rereadIters && !g_app.vramTestCancelRequested && !chunkFailed; ++rr) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));

                        BeginBenchCommandBuffer();
                        VkMemoryBarrier mb = {};
                        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                        vkCmdPipelineBarrier(g_app.benchCommandBuffer,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 1, &mb, 0, nullptr, 0, nullptr);
                        VkBufferCopy copyR = {};
                        copyR.size = regionSize;
                        vkCmdCopyBuffer(g_app.benchCommandBuffer, gpuBuffer.buffer, readbackBuffer.buffer, 1, &copyR);
                        if (EndAndSubmitBenchCommandBuffer() != FenceWaitResult::Success) {
                            Log("[WARNING] Refresh check fence wait failed on iter " + std::to_string(rr));
                            break;
                        }

                        void* readbackData = nullptr;
                        if (vkMapMemory(g_app.benchDevice, readbackBuffer.memory, 0, regionSize, 0, &readbackData) != VK_SUCCESS) {
                            Log("[WARNING] Refresh check could not map readback buffer");
                            break;
                        }
                        VkMappedMemoryRange invRange = {};
                        invRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                        invRange.memory = readbackBuffer.memory;
                        invRange.offset = 0;
                        invRange.size = VK_WHOLE_SIZE;
                        vkInvalidateMappedMemoryRanges(g_app.benchDevice, 1, &invRange);

                        const uint32_t* actualData = static_cast<const uint32_t*>(readbackData);
                        size_t refreshErrors = 0;
                        CompareBuffers(expectedData.data(), actualData, dwordCount,
                                       VRAMTestPattern::Random, allErrors,
                                       chunkOffset, refreshErrors,
                                       &g_app.vramTestResult, true);
                        vkUnmapMemory(g_app.benchDevice, readbackBuffer.memory);
                        chunkErrors += refreshErrors;
                    }
                } else {
                    Log("[WARNING] Refresh check initial write fence wait failed");
                }
            } else {
                Log("[WARNING] Refresh check could not map upload buffer");
            }
        }

        uploadBuffer.Destroy(g_app.benchDevice);
        gpuBuffer.Destroy(g_app.benchDevice);
        readbackBuffer.Destroy(g_app.benchDevice);
        
        if (!chunkFailed) {
            totalBytesTested += thisChunkSize;
        }
        totalErrors += chunkErrors;
        
        std::string chunkResult = chunkFailed ? "INCOMPLETE" : 
                                  (chunkErrors == 0) ? "PASS" : "FAIL (" + std::to_string(chunkErrors) + " errors)";
        Log("  Chunk " + std::to_string(chunkNum + 1) + " result: " + chunkResult);
        
        if (chunkFailed && !g_app.vramTestCancelRequested) {
            hadCriticalFailure = true;
        }
        
        completedSteps += patternsPerChunk;
        g_app.vramTestProgress = static_cast<float>(completedSteps) / static_cast<float>(totalSteps);
        
        if (chunkNum < numChunks - 1 && !g_app.vramTestCancelRequested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }  // end chunk loop

        if (g_app.config.vramMarathonMode && !g_app.vramTestCancelRequested && !hadCriticalFailure) {
            Log("================ Marathon pass " + std::to_string(marathonPass) +
                " complete (" + std::to_string(totalErrors) + " errors so far) ================");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    } while (g_app.config.vramMarathonMode && !g_app.vramTestCancelRequested && !hadCriticalFailure);  // end Marathon outer loop

    // Tear down GPU-verify resources before exiting
    if (gpuVerify.ready) DestroyGpuVerifyState(gpuVerify);

    double coveragePercent = (static_cast<double>(totalBytesTested) / gpu.dedicatedVRAM) * 100.0;
    snprintf(percentBuf, sizeof(percentBuf), "%.1f%%", coveragePercent);
    g_app.vramTestResult.patternResults.push_back(
        std::to_string(totalBytesTested / (1024*1024)) + " MB tested (" + percentBuf + " coverage)");
    
    auto endTime = std::chrono::steady_clock::now();
    double duration = std::chrono::duration<double>(endTime - startTime).count();
    
    g_app.vramTestResult.completed = !g_app.vramTestCancelRequested;
    g_app.vramTestResult.cancelled = g_app.vramTestCancelRequested;
    g_app.vramTestResult.totalBytesTested = totalBytesTested;
    g_app.vramTestResult.totalErrors = totalErrors;
    g_app.vramTestResult.errors = std::move(allErrors);
    g_app.vramTestResult.testDurationSeconds = duration;
    
    Log("");
    Log("=== VRAM Scan " + std::string(g_app.vramTestCancelRequested ? "Cancelled" : "Complete") + " ===");
    snprintf(percentBuf, sizeof(percentBuf), "%.1f%%", coveragePercent);
    Log("Tested: " + FormatSize(totalBytesTested) + " (" + percentBuf + " of " + FormatSize(gpu.dedicatedVRAM) + ")");
    
    char durationBuf[64];
    snprintf(durationBuf, sizeof(durationBuf), "Duration: %.1f seconds", duration);
    Log(durationBuf);
    
    if (totalErrors == 0) {
        Log("Result: PASS - No errors detected");
        g_app.vramTestResult.summary = "PASS - No errors in " + FormatSize(totalBytesTested) + " (" + percentBuf + " coverage)";
    } else {
        Log("Result: FAIL - " + std::to_string(totalErrors) + " total errors detected!");
        g_app.vramTestResult.summary = "FAIL - " + std::to_string(totalErrors) + " errors in " + FormatSize(totalBytesTested);
        
        Log("");
        Log("Error Regions:");
        for (const auto& err : g_app.vramTestResult.errors) {
            if (err.errorCount > 0) {
                Log("  " + FormatErrorAddress(err.offsetStart) + " - " + 
                    FormatErrorAddress(err.offsetEnd) + " (" + 
                    std::to_string(err.errorCount) + " errors, pattern: " + 
                    GetPatternName(err.pattern) + ")");
            }
        }
        
        Log("");
        Log("NOTE: Error addresses are logical offsets in the test buffer,");
        Log("not physical VRAM addresses. For chip-level diagnosis, use");
        Log("vendor tools like NVIDIA MATS or AMD memory diagnostics.");
    }
    
    Log("");
    
    CleanupBenchmarkDevice();
    
    g_app.vramTestRunning = false;
    g_app.vramTestProgress = 1.0f;
    g_app.showVRAMTestWindow = true;
}

// ============================================================================
// GPU MEMORY LATENCY TEST (Compute shader pointer-chase)
// ============================================================================
std::vector<uint32_t> GeneratePointerChaseChain(size_t numElements) {
    std::vector<uint32_t> chain(numElements);
    for (size_t i = 0; i < numElements; i++)
        chain[i] = static_cast<uint32_t>(i);

    // Sattolo's algorithm: guaranteed single cycle visiting all elements
    std::mt19937 rng(42);  // Fixed seed for reproducibility
    for (size_t i = numElements - 1; i > 0; i--) {
        size_t j = std::uniform_int_distribution<size_t>(0, i - 1)(rng);
        std::swap(chain[i], chain[j]);
    }
    return chain;
}

BenchmarkResult RunMemoryLatencyTest() {
    BenchmarkResult result;
    result.testName = "GPU Memory Latency";
    result.unit = "ns";

    SetCurrentTest("GPU Memory Latency");
    g_app.progress = 0;

    // ===== Create a SEPARATE VkDevice for compute work =====
    // This isolates compute operations from the benchmark device's transfer queues.
    // On AMD iGPUs, requesting queues from both compute (family 0) and dedicated transfer
    // (family 1) on the same device causes driver-internal state corruption, leading to
    // fence timeouts and GPU hangs during normal transfer operations.
    // Using a separate device avoids this entirely.

    VkDevice computeDevice = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;
    VkCommandPool computeCommandPool = VK_NULL_HANDLE;
    VkCommandBuffer computeCmdBuf = VK_NULL_HANDLE;
    uint32_t computeFamily = UINT32_MAX;
    double timestampPeriod = g_app.benchTimestampPeriod;

    // 1. Find a compute-capable queue family with timestamp support
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_app.benchPhysicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(g_app.benchPhysicalDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        bool hasCompute = (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        bool hasTimestamps = queueFamilies[i].timestampValidBits > 0;
        if (hasCompute && hasTimestamps) {
            computeFamily = i;
            break;
        }
    }
    if (computeFamily == UINT32_MAX) {
        Log("[WARNING] No compute-capable queue with timestamps - skipping GPU memory latency test");
        return result;
    }
    // The compute family may differ from the bench transfer family, so compute
    // its own timestampValidBits mask (undefined high bits must be stripped).
    uint64_t computeTsMask = ~0ull;
    {
        uint32_t validBits = queueFamilies[computeFamily].timestampValidBits;
        computeTsMask = (validBits >= 64) ? ~0ull : ((1ull << validBits) - 1);
    }
    if (g_app.config.debugLogging)
        Log("[DEBUG] Creating separate compute device (family " + std::to_string(computeFamily) + ") for memory latency test");

    // 2. Create a separate VkDevice with ONLY the compute queue
    float priority = 1.0f;
    VkDeviceQueueCreateInfo computeQueueInfo = {};
    computeQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    computeQueueInfo.queueFamilyIndex = computeFamily;
    computeQueueInfo.queueCount = 1;
    computeQueueInfo.pQueuePriorities = &priority;

    VkDeviceCreateInfo deviceInfo = {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &computeQueueInfo;

    VkResult vr = vkCreateDevice(g_app.benchPhysicalDevice, &deviceInfo, nullptr, &computeDevice);
    if (vr != VK_SUCCESS) {
        Log("[ERROR] Failed to create compute device for memory latency test: " + std::to_string((int)vr));
        return result;
    }
    if (g_app.config.debugLogging)
        Log("[DEBUG] Compute device created successfully");

    vkGetDeviceQueue(computeDevice, computeFamily, 0, &computeQueue);

    // 3. Create command pool and buffer on compute device
    VkCommandPoolCreateInfo poolCreateInfo = {};
    poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCreateInfo.queueFamilyIndex = computeFamily;
    if (vkCreateCommandPool(computeDevice, &poolCreateInfo, nullptr, &computeCommandPool) != VK_SUCCESS) {
        Log("[ERROR] Failed to create compute command pool");
        vkDestroyDevice(computeDevice, nullptr);
        return result;
    }

    VkCommandBufferAllocateInfo cmdAllocInfo = {};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = computeCommandPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(computeDevice, &cmdAllocInfo, &computeCmdBuf) != VK_SUCCESS) {
        Log("[ERROR] Failed to allocate compute command buffer");
        vkDestroyCommandPool(computeDevice, computeCommandPool, nullptr);
        vkDestroyDevice(computeDevice, nullptr);
        return result;
    }

    // ===== Now create all resources on the compute device =====

    // 4. Create SPIR-V shader module
    VkShaderModuleCreateInfo shaderInfo = {};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = g_memoryLatencySPIRVSize;
    shaderInfo.pCode = g_memoryLatencySPIRV;

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    vr = vkCreateShaderModule(computeDevice, &shaderInfo, nullptr, &shaderModule);
    if (vr != VK_SUCCESS) {
        Log("[ERROR] Failed to create memory latency shader module: " + std::to_string((int)vr));
        vkDestroyCommandPool(computeDevice, computeCommandPool, nullptr);
        vkDestroyDevice(computeDevice, nullptr);
        return result;
    }

    // 5. Create descriptor set layout
    VkDescriptorSetLayoutBinding storageBinding = {};
    storageBinding.binding = 0;
    storageBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    storageBinding.descriptorCount = 1;
    storageBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &storageBinding;

    VkDescriptorSetLayout descSetLayout = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(computeDevice, &layoutInfo, nullptr, &descSetLayout);

    // 6. Create pipeline layout with push constants
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = 8;  // 2 x uint32: numChases, startIndex

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(computeDevice, &pipelineLayoutInfo, nullptr, &pipelineLayout);

    // 7. Create compute pipeline
    VkComputePipelineCreateInfo pipelineCreateInfo = {};
    pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineCreateInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineCreateInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineCreateInfo.stage.module = shaderModule;
    pipelineCreateInfo.stage.pName = "main";
    pipelineCreateInfo.layout = pipelineLayout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    vr = vkCreateComputePipelines(computeDevice, VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &pipeline);
    if (vr != VK_SUCCESS) {
        Log("[ERROR] Failed to create compute pipeline: " + std::to_string((int)vr));
        vkDestroyShaderModule(computeDevice, shaderModule, nullptr);
        vkDestroyPipelineLayout(computeDevice, pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(computeDevice, descSetLayout, nullptr);
        vkDestroyCommandPool(computeDevice, computeCommandPool, nullptr);
        vkDestroyDevice(computeDevice, nullptr);
        return result;
    }
    if (g_app.config.debugLogging)
        Log("[DEBUG] Compute pipeline created successfully");

    // 8. Generate chain data
    const size_t numElements = Constants::MEMORY_LATENCY_BUFFER_SIZE / sizeof(uint32_t);
    if (g_app.config.debugLogging)
        Log("[DEBUG] Generating pointer-chase chain (" + std::to_string(numElements) +
            " elements, " + FormatSize(Constants::MEMORY_LATENCY_BUFFER_SIZE) + ")");
    auto chainData = GeneratePointerChaseChain(numElements);

    // 9./10. Create the chain buffer (device-local storage) and the staging
    // buffer, and upload the chain. Every allocation step is checked: on an eGPU
    // whose OS memory budget refuses even these 32 MB, the old code fell through
    // to vkBindBufferMemory/vkMapMemory with null handles and crashed instead of
    // skipping the test.
    VkBufferAllocation chainBuffer = {};
    VkBufferAllocation stagingBuffer = {};
    auto memLatFail = [&](const std::string& what) -> BenchmarkResult {
        Log("[ERROR] Memory latency test: " + what + " - skipping test");
        vkDeviceWaitIdle(computeDevice);
        stagingBuffer.Destroy(computeDevice);
        chainBuffer.Destroy(computeDevice);
        vkDestroyPipeline(computeDevice, pipeline, nullptr);
        vkDestroyPipelineLayout(computeDevice, pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(computeDevice, descSetLayout, nullptr);
        vkDestroyShaderModule(computeDevice, shaderModule, nullptr);
        vkDestroyCommandPool(computeDevice, computeCommandPool, nullptr);
        vkDestroyDevice(computeDevice, nullptr);
        return result;
    };
    auto createComputeBuffer = [&](VkBufferAllocation& out, VkBufferUsageFlags usage,
                                   VkMemoryPropertyFlags memFlags, const char* label) -> bool {
        VkBufferCreateInfo bufInfo = {};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = Constants::MEMORY_LATENCY_BUFFER_SIZE;
        bufInfo.usage = usage;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult r = vkCreateBuffer(computeDevice, &bufInfo, nullptr, &out.buffer);
        if (r != VK_SUCCESS) {
            Log(std::string("[ERROR] vkCreateBuffer (") + label + ") failed: " + std::to_string((int)r));
            return false;
        }
        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(computeDevice, out.buffer, &memReqs);
        uint32_t memType = FindMemoryType(g_app.benchPhysicalDevice, memReqs.memoryTypeBits, memFlags);
        if (memType == UINT32_MAX) {
            Log(std::string("[ERROR] No suitable memory type for ") + label);
            return false;
        }
        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = memType;
        r = vkAllocateMemory(computeDevice, &allocInfo, nullptr, &out.memory);
        if (r != VK_SUCCESS) {
            Log(std::string("[ERROR] vkAllocateMemory (") + label + ", " +
                FormatSize(Constants::MEMORY_LATENCY_BUFFER_SIZE) + ") failed: " + std::to_string((int)r));
            return false;
        }
        r = vkBindBufferMemory(computeDevice, out.buffer, out.memory, 0);
        if (r != VK_SUCCESS) {
            Log(std::string("[ERROR] vkBindBufferMemory (") + label + ") failed: " + std::to_string((int)r));
            return false;
        }
        out.size = Constants::MEMORY_LATENCY_BUFFER_SIZE;
        return true;
    };
    if (!createComputeBuffer(chainBuffer,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "chain buffer")) {
        return memLatFail("chain buffer allocation failed");
    }
    if (!createComputeBuffer(stagingBuffer,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             "staging buffer")) {
        return memLatFail("staging buffer allocation failed");
    }
    {
        void* mapped = nullptr;
        VkResult mr = vkMapMemory(computeDevice, stagingBuffer.memory, 0, stagingBuffer.size, 0, &mapped);
        if (mr != VK_SUCCESS || !mapped) {
            return memLatFail("staging buffer map failed (" + std::to_string((int)mr) + ")");
        }
        memcpy(mapped, chainData.data(), Constants::MEMORY_LATENCY_BUFFER_SIZE);
        vkUnmapMemory(computeDevice, stagingBuffer.memory);

    }

    // Upload chain data via compute queue (compute families implicitly support transfer)
    {
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(computeCmdBuf, &beginInfo);

        VkBufferCopy copyRegion = {};
        copyRegion.size = Constants::MEMORY_LATENCY_BUFFER_SIZE;
        vkCmdCopyBuffer(computeCmdBuf, stagingBuffer.buffer, chainBuffer.buffer, 1, &copyRegion);

        // Barrier: transfer write -> shader read (same queue family, no ownership transfer)
        VkBufferMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = chainBuffer.buffer;
        barrier.offset = 0;
        barrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(computeCmdBuf,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 1, &barrier, 0, nullptr);

        vkEndCommandBuffer(computeCmdBuf);

        VkFence uploadFence = VK_NULL_HANDLE;
        VkFenceCreateInfo fenceCI = {};
        fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCreateFence(computeDevice, &fenceCI, nullptr, &uploadFence);

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &computeCmdBuf;
        vkQueueSubmit(computeQueue, 1, &submitInfo, uploadFence);
        // Bounded, cancel-aware wait. A raw UINT64_MAX wait would hang the
        // worker on a GPU stall and ignore the Cancel button.
        int uploadRetries = 0;
        bool uploadOk = true;
        while (vkWaitForFences(computeDevice, 1, &uploadFence, VK_TRUE,
                               Constants::FENCE_WAIT_TIMEOUT_MS * 1000000ULL) == VK_TIMEOUT) {
            if (ShouldAbortBenchmark() || ++uploadRetries >= Constants::MAX_FENCE_RETRIES) {
                g_app.benchmarkAborted = true;
                uploadOk = false;
                break;
            }
        }
        // Destroy only if the fence signaled; destroying a fence with a pending
        // submission (the GPU-stall abort case) is invalid, so leak it to the
        // device teardown instead.
        if (uploadOk) vkDestroyFence(computeDevice, uploadFence, nullptr);
    }

    if (g_app.config.debugLogging)
        Log("[DEBUG] Chain data uploaded successfully (" + FormatSize(Constants::MEMORY_LATENCY_BUFFER_SIZE) + ")");

    // Free staging buffer
    stagingBuffer.Destroy(computeDevice);

    // 11. Create descriptor pool and set
    VkDescriptorPoolSize descPoolSize = {};
    descPoolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descPoolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo descPoolInfo = {};
    descPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descPoolInfo.maxSets = 1;
    descPoolInfo.poolSizeCount = 1;
    descPoolInfo.pPoolSizes = &descPoolSize;

    VkDescriptorPool descPool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(computeDevice, &descPoolInfo, nullptr, &descPool);

    VkDescriptorSetAllocateInfo descAllocInfo = {};
    descAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descAllocInfo.descriptorPool = descPool;
    descAllocInfo.descriptorSetCount = 1;
    descAllocInfo.pSetLayouts = &descSetLayout;

    VkDescriptorSet descSet = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(computeDevice, &descAllocInfo, &descSet);

    // Update descriptor set
    VkDescriptorBufferInfo bufferDesc = {};
    bufferDesc.buffer = chainBuffer.buffer;
    bufferDesc.offset = 0;
    bufferDesc.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writeDesc = {};
    writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeDesc.dstSet = descSet;
    writeDesc.dstBinding = 0;
    writeDesc.descriptorCount = 1;
    writeDesc.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writeDesc.pBufferInfo = &bufferDesc;

    vkUpdateDescriptorSets(computeDevice, 1, &writeDesc, 0, nullptr);

    // 12. Create timestamp query pool
    VkQueryPoolCreateInfo queryPoolInfo = {};
    queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryPoolInfo.queryCount = 2;

    VkQueryPool queryPool = VK_NULL_HANDLE;
    vkCreateQueryPool(computeDevice, &queryPoolInfo, nullptr, &queryPool);

    // Fence for dispatch synchronization
    VkFence dispatchFence = VK_NULL_HANDLE;
    {
        VkFenceCreateInfo fenceCI = {};
        fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCreateFence(computeDevice, &fenceCI, nullptr, &dispatchFence);
    }

    // Push constants
    struct LatencyParams {
        uint32_t numChases;
        uint32_t startIndex;
    };
    LatencyParams params = { Constants::MEMORY_LATENCY_NUM_CHASES, 0 };

    // 13. Warmup dispatches
    if (g_app.config.debugLogging)
        Log("[DEBUG] Running " + std::to_string(Constants::MEMORY_LATENCY_WARMUP_DISPATCHES) + " warmup dispatches...");
    for (int w = 0; w < Constants::MEMORY_LATENCY_WARMUP_DISPATCHES && !ShouldAbortBenchmark(); w++) {
        vkResetCommandBuffer(computeCmdBuf, 0);

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(computeCmdBuf, &beginInfo);

        vkCmdBindPipeline(computeCmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(computeCmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
            pipelineLayout, 0, 1, &descSet, 0, nullptr);
        vkCmdPushConstants(computeCmdBuf, pipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
        vkCmdDispatch(computeCmdBuf, 1, 1, 1);

        vkEndCommandBuffer(computeCmdBuf);

        vkResetFences(computeDevice, 1, &dispatchFence);
        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &computeCmdBuf;
        vr = vkQueueSubmit(computeQueue, 1, &submitInfo, dispatchFence);
        if (vr != VK_SUCCESS) {
            Log("[ERROR] Warmup dispatch " + std::to_string(w) + " submit failed: " + std::to_string((int)vr));
        }
        vr = vkWaitForFences(computeDevice, 1, &dispatchFence, VK_TRUE, Constants::FENCE_WAIT_TIMEOUT_MS * 1000000ULL);
        if (vr != VK_SUCCESS) {
            Log("[ERROR] Warmup dispatch " + std::to_string(w) + " fence wait failed: " + std::to_string((int)vr));
            g_app.benchmarkAborted = true;  // fence still pending - it must not be reset or reused
            break;
        }

    }

    if (g_app.config.debugLogging)
        Log("[DEBUG] Warmup dispatches complete");

    if (ShouldAbortBenchmark()) goto cleanup;

    // 14. Measurement dispatches
    {
        if (g_app.config.debugLogging)
            Log("[DEBUG] Running " + std::to_string(Constants::MEMORY_LATENCY_MEASURE_DISPATCHES) + " measurement dispatches...");
        int totalDispatches = Constants::MEMORY_LATENCY_MEASURE_DISPATCHES;

        for (int m = 0; m < totalDispatches && !ShouldAbortBenchmark(); m++) {
            vkResetCommandBuffer(computeCmdBuf, 0);

            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(computeCmdBuf, &beginInfo);

            // Reset and write start timestamp
            vkCmdResetQueryPool(computeCmdBuf, queryPool, 0, 2);
            vkCmdWriteTimestamp(computeCmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);

            // Bind and dispatch
            vkCmdBindPipeline(computeCmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(computeCmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                pipelineLayout, 0, 1, &descSet, 0, nullptr);
            vkCmdPushConstants(computeCmdBuf, pipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
            vkCmdDispatch(computeCmdBuf, 1, 1, 1);

            // Write end timestamp
            vkCmdWriteTimestamp(computeCmdBuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queryPool, 1);

            vkEndCommandBuffer(computeCmdBuf);

            vkResetFences(computeDevice, 1, &dispatchFence);
            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &computeCmdBuf;
            vr = vkQueueSubmit(computeQueue, 1, &submitInfo, dispatchFence);
            if (vr != VK_SUCCESS) {
                Log("[ERROR] Measurement dispatch " + std::to_string(m) + " submit failed: " + std::to_string((int)vr));
                continue;
            }
            vr = vkWaitForFences(computeDevice, 1, &dispatchFence, VK_TRUE, Constants::FENCE_WAIT_TIMEOUT_MS * 1000000ULL);
            if (vr != VK_SUCCESS) {
                Log("[ERROR] Measurement dispatch " + std::to_string(m) + " fence wait failed: " + std::to_string((int)vr));
                g_app.benchmarkAborted = true;  // fence still pending - it must not be reset or reused
                break;
            }


            // Read timestamps
            uint64_t timestamps[2] = {};
            vr = vkGetQueryPoolResults(computeDevice, queryPool, 0, 2,
                sizeof(timestamps), timestamps, sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            timestamps[0] &= computeTsMask;  // strip undefined bits (timestampValidBits)
            timestamps[1] &= computeTsMask;

            if (g_app.config.debugLogging && m == 0) {
                Log("[DEBUG] Timestamp query result: " + std::to_string((int)vr) +
                    ", ts[0]=" + std::to_string(timestamps[0]) +
                    ", ts[1]=" + std::to_string(timestamps[1]) +
                    ", period=" + std::to_string(timestampPeriod) + " ns/tick");
            }

            if (vr == VK_SUCCESS && timestamps[1] > timestamps[0]) {
                double totalNs = static_cast<double>(timestamps[1] - timestamps[0]) * timestampPeriod;
                double perChaseNs = totalNs / Constants::MEMORY_LATENCY_NUM_CHASES;
                result.samples.push_back(perChaseNs);
            } else if (m == 0) {
                Log("[WARNING] Invalid timestamps on first measurement: vr=" + std::to_string((int)vr) +
                    " ts[0]=" + std::to_string(timestamps[0]) +
                    " ts[1]=" + std::to_string(timestamps[1]));
            }

            g_app.progress = static_cast<float>(m + 1) / totalDispatches;
        }
    }

    // 15. Calculate statistics
    if (g_app.config.debugLogging)
        Log("[DEBUG] Memory latency test collected " + std::to_string(result.samples.size()) + " valid samples");

    if (!result.samples.empty()) {
        std::sort(result.samples.begin(), result.samples.end());
        result.minValue = result.samples.front();
        result.maxValue = result.samples.back();
        double sum = 0;
        for (double s : result.samples) sum += s;
        result.avgValue = sum / result.samples.size();

        if (g_app.config.debugLogging)
            Log("[DEBUG] GPU Memory Latency: min=" + std::to_string(result.minValue) +
                " avg=" + std::to_string(result.avgValue) +
                " max=" + std::to_string(result.maxValue) + " ns");
    } else {
        Log("[WARNING] No valid memory latency samples collected");
    }

cleanup:
    // 16. Cleanup — destroy everything on the compute device, then destroy the device itself
    vkDeviceWaitIdle(computeDevice);
    vkDestroyFence(computeDevice, dispatchFence, nullptr);
    vkDestroyQueryPool(computeDevice, queryPool, nullptr);
    vkDestroyDescriptorPool(computeDevice, descPool, nullptr);
    chainBuffer.Destroy(computeDevice);
    vkDestroyPipeline(computeDevice, pipeline, nullptr);
    vkDestroyPipelineLayout(computeDevice, pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(computeDevice, descSetLayout, nullptr);
    vkDestroyShaderModule(computeDevice, shaderModule, nullptr);
    vkDestroyCommandPool(computeDevice, computeCommandPool, nullptr);
    vkDestroyDevice(computeDevice, nullptr);

    return result;
}

void BenchmarkThreadFunc() {
    g_app.benchmarkThreadRunning = true;
    g_app.benchmarkStartTime = std::chrono::steady_clock::now();
    g_app.benchmarkAborted = false;
    g_app.fenceTimeoutCount = 0;
    
    Log("=== Benchmark Started ===");
    Log("GPU: " + g_app.gpuList[g_app.config.selectedGPU].name);
    
    bool isIntegratedGPU = g_app.gpuList[g_app.config.selectedGPU].isIntegrated;
    if (isIntegratedGPU) {
        Log("GPU Type: Integrated (using GPU timestamps for upload measurement)");
    } else {
        Log("GPU Type: Discrete (using round-trip method for accurate upload measurement)");
    }
    
    // Keep an elevated (pkexec) read from startup: only re-detect when we
    // have no per-device data yet.
    if (g_app.systemMemory.totalSticks == 0) {
        g_app.systemMemory = DetectSystemMemory();
    }
    if (g_app.systemMemory.detected) {
        Log(FormatSystemMemoryInfo(g_app.systemMemory));
        
        if (g_app.systemMemory.configuredSpeedMT > 0 && 
            g_app.systemMemory.speedMT > 0 &&
            g_app.systemMemory.configuredSpeedMT < g_app.systemMemory.speedMT * 0.85) {
            Log("[WARNING] RAM running at " + std::to_string(g_app.systemMemory.configuredSpeedMT) + 
                " MT/s (rated for " + std::to_string(g_app.systemMemory.speedMT) + 
                " MT/s) - XMP/EXPO may not be enabled");
        }
        
        if (g_app.systemMemory.channelsUnverified) {
            // Soldered LPDDR: SMBIOS device count is not a channel count, so no
            // single-channel warning - state what was read and that it is an estimate.
            Log("[INFO] Soldered " + g_app.systemMemory.type + " memory: SMBIOS lists " +
                std::to_string(g_app.systemMemory.totalSticks) + " device(s)" +
                (g_app.systemMemory.busWidthBits > 0
                    ? ", " + std::to_string(g_app.systemMemory.busWidthBits) + "-bit data width"
                    : std::string()) +
                " - channel count (" + std::to_string(g_app.systemMemory.channels) + ") is an estimate");
        } else if (g_app.systemMemory.channels == 1) {
            Log("[WARNING] Single-channel RAM detected - this may bottleneck PCIe bandwidth");
        }
    } else {
        Log("[INFO] System memory detection: " + g_app.systemMemory.errorMessage);
    }
    
    size_t originalSize = g_app.config.bandwidthSize;
    g_app.config.bandwidthSize = ValidateBandwidthSize(originalSize, g_app.config.selectedGPU);
    
    Log("Size: " + FormatSize(g_app.config.bandwidthSize));
    if (g_app.config.bandwidthSize != originalSize) {
        Log("[INFO] Size was reduced from " + FormatSize(originalSize) + " to fit VRAM");
    }
    Log("Batches: " + std::to_string(g_app.config.bandwidthBatches));
    Log("Copies/Batch: " + std::to_string(g_app.config.copiesPerBatch));
    Log("Runs: " + std::to_string(g_app.config.numRuns));
    Log("Average Runs: " + std::string(g_app.config.averageRuns ? "Yes" : "No (individual)"));
    Log("=========================");

    if (!InitBenchmarkDevice(g_app.config.selectedGPU)) {
        Log("[ERROR] Failed to initialize benchmark device!");
        g_app.state = AppState::Idle;
        g_app.benchmarkThreadRunning = false;
        return;
    }
    
    // Log measurement methodology for reproducibility
    // These details map directly to D3D12 equivalents for backporting:
    //   Transfer queue → D3D12_COMMAND_LIST_TYPE_COPY
    //   GPU timestamps  → EndQuery(TIMESTAMP) on COPY queue
    //   CPU round-trip  → identical logic on COPY queue
    if (isIntegratedGPU) {
        Log("[METHOD] Upload: GPU timestamps, Download: GPU timestamps, Bidirectional: " +
            std::string(g_app.hasDualQueues ? "dual copy queues" : "single queue interleaved"));
    } else {
        Log("[METHOD] Upload: CPU round-trip, Download: GPU timestamps, Bidirectional: " +
            std::string(g_app.hasDualQueues ? "dual copy queues" : "single queue interleaved"));
    }

    std::vector<BenchmarkResult> allResults;

    // Estimate rated chip latency before benchmark starts
    EstimateRatedLatency(g_app.systemMemory);

    int testsPerRun = 2;  // Upload + Download
    if (g_app.config.runBidirectional) testsPerRun++;
    if (g_app.config.runLatency) testsPerRun += 3;
    g_app.totalTests = testsPerRun * g_app.config.numRuns;
    if (g_app.config.runMemoryLatency) g_app.totalTests++;  // Memory latency runs once (hardware constant)

    double avgUpload = 0, avgDownload = 0;
    double maxUpload = 0, maxDownload = 0;
    int uploadCount = 0, downloadCount = 0;
    
    auto getRunSuffix = [](int run, bool useAverage) -> std::string {
        return useAverage ? "" : " Run " + std::to_string(run);
    };

    for (int run = 1; run <= g_app.config.numRuns && !ShouldAbortBenchmark(); run++) {
        g_app.currentRun = run;
        Log("--- Run " + std::to_string(run) + " / " + std::to_string(g_app.config.numRuns) + " ---");
        
        bool runHadCriticalFailure = false;
        std::string runSuffix = getRunSuffix(run, g_app.config.averageRuns);
        
        bool isIntegrated = g_app.gpuList[g_app.config.selectedGPU].isIntegrated;
        bool useRoundTrip = !isIntegrated;
        
        // DOWNLOAD TEST FIRST
        double currentDownloadSpeed = 0.0;
        
        auto gpuSrc = CreateBuffer(VkBufferType::DeviceLocal, g_app.config.bandwidthSize);
        auto cpuReadback = CreateBuffer(VkBufferType::Readback, g_app.config.bandwidthSize);
        
        if (!gpuSrc || !cpuReadback) {
            Log("[CRITICAL] Failed to allocate download buffers - skipping download test");
            g_app.completedTests++;
            g_app.overallProgress = float(g_app.completedTests) / float(g_app.totalTests);
            gpuSrc.Destroy(g_app.benchDevice);
            cpuReadback.Destroy(g_app.benchDevice);
        } else {
            auto resDownload = RunBandwidthTest("GPU->CPU " + FormatSize(g_app.config.bandwidthSize) + runSuffix,
                gpuSrc, cpuReadback,
                g_app.config.bandwidthSize,
                g_app.config.copiesPerBatch,
                g_app.config.bandwidthBatches);
            
            if (!resDownload.samples.empty()) {
                allResults.push_back(resDownload);
                avgDownload += resDownload.avgValue;
                maxDownload = std::max(maxDownload, resDownload.maxValue);
                downloadCount++;
                currentDownloadSpeed = resDownload.avgValue;
                Log("  GPU->CPU: " + std::to_string(resDownload.avgValue).substr(0, 5) + " GB/s");
            } else {
                Log("[WARNING] Download test produced no valid samples");
            }
            
            gpuSrc.Destroy(g_app.benchDevice);
            cpuReadback.Destroy(g_app.benchDevice);
            
            g_app.completedTests++;
            g_app.overallProgress = float(g_app.completedTests) / float(g_app.totalTests);
        }
        
        if (ShouldAbortBenchmark()) break;
        
        // UPLOAD TEST
        auto cpuUpload = CreateBuffer(VkBufferType::Upload, g_app.config.bandwidthSize);
        if (!cpuUpload) {
            Log("[CRITICAL] Failed to allocate upload buffer - aborting run");
            runHadCriticalFailure = true;
        }
        
        auto gpuDefault = CreateBuffer(VkBufferType::DeviceLocal, g_app.config.bandwidthSize);
        if (!gpuDefault) {
            Log("[CRITICAL] Failed to allocate GPU default buffer - aborting run");
            runHadCriticalFailure = true;
        }
        
        if (runHadCriticalFailure) {
            cpuUpload.Destroy(g_app.benchDevice);
            gpuDefault.Destroy(g_app.benchDevice);
            g_app.completedTests += (testsPerRun - 1);
            g_app.overallProgress = float(g_app.completedTests) / float(g_app.totalTests);
            continue;
        }
        
        auto resUpload = RunBandwidthTest("CPU->GPU " + FormatSize(g_app.config.bandwidthSize) + runSuffix,
            cpuUpload, gpuDefault,
            g_app.config.bandwidthSize,
            g_app.config.copiesPerBatch,
            g_app.config.bandwidthBatches,
            useRoundTrip,
            currentDownloadSpeed);
        
        if (!resUpload.samples.empty()) {
            allResults.push_back(resUpload);
            avgUpload += resUpload.avgValue;
            maxUpload = std::max(maxUpload, resUpload.maxValue);
            uploadCount++;
            Log("  CPU->GPU: " + std::to_string(resUpload.avgValue).substr(0, 5) + " GB/s");
        } else {
            Log("[WARNING] Upload test produced no valid samples");
        }
        
        cpuUpload.Destroy(g_app.benchDevice);
        gpuDefault.Destroy(g_app.benchDevice);
        
        g_app.completedTests++;
        g_app.overallProgress = float(g_app.completedTests) / float(g_app.totalTests);
        if (ShouldAbortBenchmark()) break;

        // Bidirectional
        if (g_app.config.runBidirectional) {
            auto resBidir = RunBidirectionalTest(g_app.config.bandwidthSize, g_app.config.copiesPerBatch, g_app.config.bandwidthBatches);
            if (!resBidir.samples.empty()) {
                resBidir.testName += runSuffix;  // RunBidirectionalTest already named it after the ACTUAL buffer size
                allResults.push_back(resBidir);
                Log("  Bidirectional: " + std::to_string(resBidir.avgValue).substr(0, 5) + " GB/s");
            }
            g_app.completedTests++;
            g_app.overallProgress = float(g_app.completedTests) / float(g_app.totalTests);
            if (ShouldAbortBenchmark()) break;
        }

        // Latency tests
        if (g_app.config.runLatency) {
            auto latCpuUpload = CreateBuffer(VkBufferType::Upload, g_app.config.latencySize);
            auto latGpuDefault = CreateBuffer(VkBufferType::DeviceLocal, g_app.config.latencySize);
            auto latGpuSrc = CreateBuffer(VkBufferType::DeviceLocal, g_app.config.latencySize);
            auto latCpuReadback = CreateBuffer(VkBufferType::Readback, g_app.config.latencySize);

            if (!latCpuUpload || !latGpuDefault || !latGpuSrc || !latCpuReadback) {
                Log("[CRITICAL] Failed to allocate latency buffers - skipping latency tests");
                latCpuUpload.Destroy(g_app.benchDevice);
                latGpuDefault.Destroy(g_app.benchDevice);
                latGpuSrc.Destroy(g_app.benchDevice);
                latCpuReadback.Destroy(g_app.benchDevice);
                g_app.completedTests += 3;
                g_app.overallProgress = float(g_app.completedTests) / float(g_app.totalTests);
                continue;
            }

            // Warm-up passes
            Log("Running latency warm-up...");
            RunLatencyTest("Warm-up Upload", latCpuUpload, latGpuDefault, Constants::LATENCY_WARMUP_ITERATIONS);
            RunLatencyTest("Warm-up Download", latGpuSrc, latCpuReadback, Constants::LATENCY_WARMUP_ITERATIONS);
            RunCommandLatencyTest(Constants::LATENCY_WARMUP_ITERATIONS);

            if (ShouldAbortBenchmark()) {
                latCpuUpload.Destroy(g_app.benchDevice);
                latGpuDefault.Destroy(g_app.benchDevice);
                latGpuSrc.Destroy(g_app.benchDevice);
                latCpuReadback.Destroy(g_app.benchDevice);
                break;
            }

            auto resUpLat = RunLatencyTest("CPU->GPU Latency" + runSuffix, latCpuUpload, latGpuDefault, g_app.config.latencyIters);
            if (!resUpLat.samples.empty()) {
                allResults.push_back(resUpLat);
                Log("  CPU->GPU Latency: " + std::to_string(resUpLat.avgValue).substr(0, 6) + " us");
            }
            g_app.completedTests++;
            g_app.overallProgress = float(g_app.completedTests) / float(g_app.totalTests);
            if (ShouldAbortBenchmark()) {
                latCpuUpload.Destroy(g_app.benchDevice);
                latGpuDefault.Destroy(g_app.benchDevice);
                latGpuSrc.Destroy(g_app.benchDevice);
                latCpuReadback.Destroy(g_app.benchDevice);
                break;
            }

            auto resDownLat = RunLatencyTest("GPU->CPU Latency" + runSuffix, latGpuSrc, latCpuReadback, g_app.config.latencyIters);
            if (!resDownLat.samples.empty()) {
                allResults.push_back(resDownLat);
                Log("  GPU->CPU Latency: " + std::to_string(resDownLat.avgValue).substr(0, 6) + " us");
            }
            g_app.completedTests++;
            g_app.overallProgress = float(g_app.completedTests) / float(g_app.totalTests);
            if (ShouldAbortBenchmark()) {
                latCpuUpload.Destroy(g_app.benchDevice);
                latGpuDefault.Destroy(g_app.benchDevice);
                latGpuSrc.Destroy(g_app.benchDevice);
                latCpuReadback.Destroy(g_app.benchDevice);
                break;
            }

            auto resCmdLat = RunCommandLatencyTest(g_app.config.latencyIters);
            if (!resCmdLat.samples.empty()) {
                resCmdLat.testName = "Command Latency" + runSuffix;
                allResults.push_back(resCmdLat);
                Log("  Command Latency: " + std::to_string(resCmdLat.avgValue).substr(0, 6) + " us");
            }
            g_app.completedTests++;
            g_app.overallProgress = float(g_app.completedTests) / float(g_app.totalTests);
            
            latCpuUpload.Destroy(g_app.benchDevice);
            latGpuDefault.Destroy(g_app.benchDevice);
            latGpuSrc.Destroy(g_app.benchDevice);
            latCpuReadback.Destroy(g_app.benchDevice);

            if (ShouldAbortBenchmark()) break;
        }

        // GPU MEMORY LATENCY TEST (compute shader pointer-chase)
        // Only run on first run — measures hardware latency which doesn't vary between runs.
        if (g_app.config.runMemoryLatency && !ShouldAbortBenchmark() && run == 1) {
            auto resMemLat = RunMemoryLatencyTest();
            if (!resMemLat.samples.empty()) {
                resMemLat.testName = "GPU Memory Latency";
                allResults.push_back(resMemLat);
                Log("  GPU Memory Latency: " + std::to_string(resMemLat.avgValue).substr(0, 6) + " ns");
            }
            g_app.completedTests++;
            g_app.overallProgress = float(g_app.completedTests) / float(g_app.totalTests);
        }

    }

    CleanupBenchmarkDevice();

    if (g_app.cancelRequested) {
        Log("Benchmark cancelled by user");
        g_app.state = AppState::Idle;
    } else if (g_app.benchmarkAborted) {
        Log("[ERROR] Benchmark aborted due to critical errors");
        g_app.state = AppState::Idle;
    } else if (uploadCount == 0 || downloadCount == 0) {
        Log("[ERROR] Benchmark failed - no valid bandwidth measurements");
        g_app.state = AppState::Idle;
    } else {
        // Calculate values based on mode
        double reportUpload, reportDownload;
        
        if (g_app.config.averageRuns) {
            // Average mode: use average of all runs
            reportUpload = avgUpload / uploadCount;
            reportDownload = avgDownload / downloadCount;
        } else {
            // Individual mode: use best (max) values for interface comparison
            reportUpload = maxUpload;
            reportDownload = maxDownload;
        }
        
        DetectInterface(reportUpload, reportDownload, g_app.config.selectedGPU);
        
        // Check for eGPU - uses hardware detection if available, falls back to bandwidth
        const GPUInfo& gpu = g_app.gpuList[g_app.config.selectedGPU];
        bool isIntegrated = gpu.isIntegrated;
        DetectEGPU(reportUpload, reportDownload, gpu);

        Log("=== Benchmark Complete ===");
        
        // Generate summary comparing measured vs actual connection
        double measuredMax = std::max(reportUpload, reportDownload);
        
        if (isIntegrated) {
            // Integrated GPU - show UMA/shared memory info instead of PCIe
            Log("Memory Path: " + g_app.detectedInterface);
            Log("Fabric: " + g_app.integratedFabricType);
            Log("Memory Type: " + g_app.integratedMemoryType);
            Log("PCIe: Not Applicable (on-die GPU)");
            
            if (g_app.possibleEGPU) {
                Log("[INFO] This appears to be an eGPU connected via " + g_app.eGPUConnectionType);
            }

            // Log with percentages vs DDR bandwidth
            char uploadBuf[128], downloadBuf[128];
            const char* modeStr = g_app.config.averageRuns ? "Avg" : "Best";
            snprintf(uploadBuf, sizeof(uploadBuf), "CPU->GPU (%s): %.2f GB/s (%.0f%% of %s)",
                modeStr, reportUpload, g_app.uploadPercentage, g_app.closestUploadStandard.c_str());
            snprintf(downloadBuf, sizeof(downloadBuf), "GPU->CPU (%s): %.2f GB/s (%.0f%% of %s)",
                modeStr, reportDownload, g_app.downloadPercentage, g_app.closestDownloadStandard.c_str());
            Log(uploadBuf);
            Log(downloadBuf);
            
            // Generate explanation for integrated GPU
            std::ostringstream oss;
            oss << "INTEGRATED GPU (UMA Architecture)\n\n"
                << "This is an APU/integrated GPU that shares system memory with the CPU. "
                << "There are no PCIe lanes between the CPU and GPU cores - they communicate "
                << "through the on-die fabric (" << g_app.integratedFabricType << ").\n\n"
                << "Memory bandwidth is determined by:\n"
                << "- System RAM speed (" << g_app.integratedMemoryType << ")\n"
                << "- Memory controller configuration (channels, ranks)\n"
                << "- Fabric/interconnect bandwidth\n"
                << "- Contention with CPU memory access\n\n"
                << "The asymmetry between upload (" << std::fixed << std::setprecision(1) << reportUpload 
                << " GB/s) and download (" << reportDownload << " GB/s) is normal and reflects "
                << "differences in how the GPU reads vs writes to different memory heap types.";
            g_app.summaryExplanation = oss.str();
            
            // PCIe info is not meaningful for iGPUs
            g_app.actualPCIeConfig = "N/A (Integrated)";
            g_app.actualPCIeBandwidth = 0;
            
        } else {
            // Discrete GPU - show PCIe interface info
            // Show detected interface - for eGPUs, show the connection type
            if (g_app.possibleEGPU) {
                // Show detection method: hardware (device tree) vs bandwidth measurement
                if (gpu.isThunderbolt || gpu.isUSB4 || gpu.isUSB) {
                    Log("Connection: " + g_app.eGPUConnectionType + " (confirmed via device tree)");
                } else {
                    Log("Connection: " + g_app.eGPUConnectionType + " (detected via bandwidth)");
                }
                Log("eGPU Status: External GPU detected");
            } else {
                Log("Speed Comparable To: " + g_app.detectedInterface);
            }

            // Log with percentages - indicate if using best or average
            char uploadBuf[128], downloadBuf[128];
            const char* modeStr = g_app.config.averageRuns ? "Avg" : "Best";
            snprintf(uploadBuf, sizeof(uploadBuf), "CPU->GPU (%s): %.2f GB/s (%.0f%% of %s)",
                modeStr, reportUpload, g_app.uploadPercentage, g_app.closestUploadStandard.c_str());
            snprintf(downloadBuf, sizeof(downloadBuf), "GPU->CPU (%s): %.2f GB/s (%.0f%% of %s)",
                modeStr, reportDownload, g_app.downloadPercentage, g_app.closestDownloadStandard.c_str());
            Log(uploadBuf);
            Log(downloadBuf);
            
            if (gpu.pcieInfoValid) {
                g_app.actualPCIeConfig = FormatPCIeConfig(gpu.pcieGenCurrent, gpu.pcieLanesCurrent);
                double theoreticalBw = CalculatePCIeBandwidth(gpu.pcieGenCurrent, gpu.pcieLanesCurrent);
                double realisticBw = CalculateRealisticPCIeBandwidth(gpu.pcieGenCurrent, gpu.pcieLanesCurrent);
                g_app.actualPCIeBandwidth = realisticBw;  // Store realistic for comparisons
                
                if (g_app.possibleEGPU) {
                    // For eGPUs, the PCIe link is between GPU and enclosure, not to host
                    Log("=== PCIe Link (GPU to Enclosure) ===");
                } else {
                    Log("=== Actual PCIe Link ===");
                }
                char pcieBuf[256];
                snprintf(pcieBuf, sizeof(pcieBuf), "Connected as: %s (achievable: %.2f GB/s, theoretical: %.2f GB/s)",
                    g_app.actualPCIeConfig.c_str(), realisticBw, theoreticalBw);
                Log(pcieBuf);
                
                if (gpu.pcieGenMax > 0 && gpu.pcieLanesMax > 0) {
                    std::string maxConfig = FormatPCIeConfig(gpu.pcieGenMax, gpu.pcieLanesMax);
                    double maxTheoretical = CalculatePCIeBandwidth(gpu.pcieGenMax, gpu.pcieLanesMax);
                    double maxRealistic = CalculateRealisticPCIeBandwidth(gpu.pcieGenMax, gpu.pcieLanesMax);
                    snprintf(pcieBuf, sizeof(pcieBuf), "GPU capable of: %s (achievable: %.2f GB/s, theoretical: %.2f GB/s)",
                        maxConfig.c_str(), maxRealistic, maxTheoretical);
                    Log(pcieBuf);
                }
                
                // Generate explanation based on whether this is an eGPU
                if (g_app.possibleEGPU) {
                    // eGPU - bandwidth is limited by the external connection
                    std::ostringstream oss;
                    oss << "EXTERNAL GPU (" << g_app.eGPUConnectionType << ")\n\n"
                        << "This GPU is connected externally. The bandwidth is limited by the "
                        << g_app.eGPUConnectionType << " connection, not the GPU's PCIe capability.\n\n";
                    
                    if (gpu.thunderboltVersion == 4 || gpu.isUSB4) {
                        if (gpu.isThunderbolt) {
                            oss << "Thunderbolt 4 / USB4 provides:\n";
                        } else {
                            oss << "USB4 (PCIe Tunneling) provides:\n";
                        }
                        oss << "- 40 Gbps total bandwidth (~5 GB/s usable per direction)\n"
                            << "- Approximately PCIe 3.0 x4 equivalent\n"
                            << "- ~3.5 GB/s practical throughput per direction is normal\n\n";
                    } else if (gpu.thunderboltVersion == 3) {
                        oss << "Thunderbolt 3 provides:\n"
                            << "- 40 Gbps total bandwidth (shared with other devices)\n"
                            << "- Variable PCIe lane allocation\n"
                            << "- Bandwidth depends on enclosure and host controller\n\n";
                    } else if (gpu.isUSB) {
                        // Generic USB connection (unusual for GPUs)
                        oss << "USB Connection detected:\n"
                            << "- This is an unusual configuration for external GPUs\n"
                            << "- USB 3.2 Gen 2x2 provides up to 20 Gbps (~2.5 GB/s)\n"
                            << "- USB 3.1 Gen 2 provides up to 10 Gbps (~1.2 GB/s)\n"
                            << "- Performance may be limited compared to TB/USB4\n\n";
                    }
                    
                    oss << "Your measured bandwidth (" << std::fixed << std::setprecision(2) 
                        << measuredMax << " GB/s) is typical for this connection type.";
                    g_app.summaryExplanation = oss.str();
                } else {
                    // Internal GPU - compare against REALISTIC bandwidth
                    double efficiency = (measuredMax / realisticBw) * 100.0;
                    
                    if (measuredMax > realisticBw * 1.1) {
                        // Measured faster than realistic PCIe bandwidth
                        g_app.summaryExplanation = "FASTER THAN EXPECTED: Measured bandwidth exceeds typical "
                            "achievable PCIe bandwidth. This could indicate excellent driver optimization, "
                            "or the GPU uses a different bus (e.g., on-die connection).";
                    } else if (measuredMax < realisticBw * 0.7) {
                        // Significantly slower than expected
                        std::ostringstream oss;
                        oss << "SLOWER THAN EXPECTED: Measured " << std::fixed << std::setprecision(0) 
                            << efficiency << "% of achievable bandwidth. Possible causes:\n"
                            << "- PCIe slot may be sharing lanes with other devices (M.2 slots, USB controllers)\n"
                            << "- BIOS settings may limit PCIe lanes\n"
                            << "- Chipset limitations on this motherboard slot\n"
                            << "- CPU PCIe lane limitations\n"
                            << "- Thermal throttling or power limits";
                        g_app.summaryExplanation = oss.str();
                    } else if (measuredMax < realisticBw * 0.90) {
                        // Somewhat slower
                        std::ostringstream oss;
                        oss << "GOOD - " << std::fixed << std::setprecision(0) << efficiency 
                            << "% of achievable bandwidth. Minor overhead from:\n"
                            << "- Memory subsystem limitations\n"
                            << "- Driver/OS overhead\n"
                            << "- System load variations";
                        g_app.summaryExplanation = oss.str();
                    } else {
                        // Very good - close to achievable max
                        std::ostringstream oss;
                        oss << "EXCELLENT - " << std::fixed << std::setprecision(0) << efficiency 
                            << "% of achievable bandwidth! Your PCIe link is performing optimally.";
                        g_app.summaryExplanation = oss.str();
                    }
                }
                
                // Check if running below max capability (only relevant for internal GPUs)
                if (!g_app.possibleEGPU && 
                    (gpu.pcieGenCurrent < gpu.pcieGenMax || gpu.pcieLanesCurrent < gpu.pcieLanesMax)) {
                    std::ostringstream oss;
                    oss << g_app.summaryExplanation << "\n\nNOTE: GPU is running below its maximum capability. "
                        << "Current: " << FormatPCIeConfig(gpu.pcieGenCurrent, gpu.pcieLanesCurrent)
                        << ", Max: " << FormatPCIeConfig(gpu.pcieGenMax, gpu.pcieLanesMax) << ". "
                        << "Check BIOS settings and slot placement.";
                    g_app.summaryExplanation = oss.str();
                }
            } else {
                g_app.actualPCIeConfig = "Not detected";
                g_app.actualPCIeBandwidth = 0;
                g_app.summaryExplanation = "Could not query actual PCIe link configuration. "
                    "This may happen with some GPU drivers or on older systems.";
            }
        }
        

        std::lock_guard<std::mutex> lock(g_app.resultsMutex);
        
        // If averaging is enabled and we have multiple runs, aggregate the results
        std::vector<BenchmarkResult> newResults;
        if (g_app.config.averageRuns && g_app.config.numRuns > 1) {
            newResults = AggregateResults(allResults);
        } else {
            newResults = allResults;
        }
        
        // Append new results to existing results (accumulate across benchmark runs)
        g_app.results.insert(g_app.results.end(), newResults.begin(), newResults.end());
        
        // Only now (results inserted under the lock) let the UI open the
        // summary window that walks g_app.results.
        g_app.showSummaryWindow = true;
        g_app.state = AppState::Completed;
    }
    
    g_app.benchmarkThreadRunning = false;
}

// ============================================================================
//                             CSV EXPORT
// ============================================================================

// Default export file name: timestamped so repeated exports never overwrite.
std::string DefaultCsvFileName() {
    std::time_t t = std::time(nullptr);
    std::tm tmv = {};
    localtime_r(&t, &tmv);
    char buf[64];
    std::strftime(buf, sizeof(buf), "gpu-pcie-test_%Y%m%d-%H%M%S.csv", &tmv);
    return buf;
}

static std::string ShellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += (c == '\'') ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

// Ask the user where to save the CSV. Uses the desktop's native file dialog
// (kdialog on KDE, zenity elsewhere; whichever exists). Returns "" if the
// dialog was cancelled. Without either tool the file goes to the Documents
// folder (xdg-user-dir) or $HOME with a timestamped name, and the log states
// the full path. (Previously the file was written silently to the process's
// working directory, which for a launcher/AppImage start is not obvious.)
std::string ChooseCsvSavePath() {
    std::string dir = TrimString(ExecCommand("xdg-user-dir DOCUMENTS 2>/dev/null"));
    if (dir.empty() || access(dir.c_str(), W_OK) != 0) {
        const char* home = getenv("HOME");
        dir = home ? home : ".";
    }
    std::string def = dir + "/" + DefaultCsvFileName();

    bool haveKdialog = !TrimString(ExecCommand("command -v kdialog 2>/dev/null")).empty();
    bool haveZenity  = !TrimString(ExecCommand("command -v zenity 2>/dev/null")).empty();
    const char* desktop = getenv("XDG_CURRENT_DESKTOP");
    bool preferKde = desktop && std::string(desktop).find("KDE") != std::string::npos;

    std::string cmd;
    if (haveKdialog && (preferKde || !haveZenity)) {
        cmd = "kdialog --title 'Export benchmark results' --getsavefilename " + ShellQuote(def) + " 'text/csv' 2>/dev/null";
    } else if (haveZenity) {
        cmd = "zenity --file-selection --save --confirm-overwrite --title='Export benchmark results' --filename=" +
              ShellQuote(def) + " 2>/dev/null";
    }
    if (cmd.empty()) {
        Log("[INFO] No kdialog/zenity found for a save dialog - saving to " + def);
        return def;
    }
    return TrimString(ExecCommand(cmd));  // "" = cancelled
}

void ExportCSV(const std::string& filename) {
    std::ofstream file(filename);
    if (!file) {
        Log("[ERROR] Failed to open " + filename);
        return;
    }

    file << "Test,Min,Avg,Max,Unit\n";

    std::lock_guard<std::mutex> lock(g_app.resultsMutex);
    for (const auto& r : g_app.results) {
        file << r.testName << ","
            << std::fixed << std::setprecision(2) << r.minValue << ","
            << r.avgValue << ","
            << r.maxValue << ","
            << r.unit << "\n";
    }

    // Add interface detection info
    file << "\nSpeed Comparable To," << g_app.detectedInterface << "\n";
    file << "CPU->GPU," << g_app.uploadBW << " GB/s," << g_app.uploadPercentage << "% of " << g_app.closestUploadStandard << "\n";
    file << "GPU->CPU," << g_app.downloadBW << " GB/s," << g_app.downloadPercentage << "% of " << g_app.closestDownloadStandard << "\n";
    
    // Add system memory info
    if (g_app.systemMemory.detected) {
        file << "\nSystem Memory\n";
        file << "Type," << g_app.systemMemory.type << "\n";
        file << "Rated Speed," << g_app.systemMemory.speedMT << " MT/s\n";
        file << "Actual Speed," << g_app.systemMemory.configuredSpeedMT << " MT/s\n";
        file << "Channels," << g_app.systemMemory.channels << "\n";
        file << "DIMMs," << g_app.systemMemory.totalSticks << "\n";
        file << "Total Capacity," << g_app.systemMemory.totalCapacityGB << " GB\n";
        file << "Theoretical Bandwidth," << std::fixed << std::setprecision(1) << g_app.systemMemory.theoreticalBandwidth << " GB/s\n";
        if (g_app.systemMemory.latencyEstimated) {
            file << "Est. Chip Latency," << std::fixed << std::setprecision(1) << g_app.systemMemory.ratedLatencyNs << " ns (~CL" << g_app.systemMemory.estimatedCL << " typical for speed tier)\n";
        }
    }
    
    // Add eGPU detection info
    if (g_app.possibleEGPU) {
        file << "\neGPU Detection,Possible eGPU," << g_app.eGPUConnectionType << "\n";
    }

    file.close();
    Log("Results exported to " + filename);
}

// Menu / button entry point: pick a location, then write.
void ExportBenchmarkCsvInteractive() {
    std::string path = ChooseCsvSavePath();
    if (path.empty()) {
        Log("Export cancelled");
        return;
    }
    ExportCSV(path);
}

// ============================================================================
//                             GUI RENDERING
// ============================================================================

// ============================================================================
// ELEVATED SMBIOS READ (Linux)
// ============================================================================
// The SMBIOS memory table is root-only on Linux. Rather than relaunching a
// Vulkan GUI as root (fragile under Wayland, and far more privilege than
// needed), run dmidecode alone through pkexec: the desktop's polkit agent asks
// for the password and only that one read is elevated.
void ElevatedMemoryRead() {
    if (TrimString(ExecCommand("command -v pkexec 2>/dev/null")).empty()) {
        Log("[WARNING] pkexec not found - start the tool with sudo to read memory details");
        return;
    }
    Log("[INFO] Reading the SMBIOS memory table via pkexec (password prompt)...");
    std::string out = ExecCommand("pkexec dmidecode --type 17 2>/dev/null");
    if (out.find("Memory Device") == std::string::npos) {
        Log("[WARNING] Elevated dmidecode read was cancelled or failed");
        return;
    }
    g_app.systemMemory = DetectSystemMemoryFrom(out);
    g_app.systemMemory.needsElevation = false;
    EstimateRatedLatency(g_app.systemMemory);
    Log(FormatSystemMemoryInfo(g_app.systemMemory));
}

static bool g_memoryPromptOpened = false;
void RenderMemoryElevationPrompt() {
    if (!g_app.systemMemory.needsElevation || g_app.memoryPromptDismissed) return;
    const char* title = "Memory details need administrator access";
    if (!g_memoryPromptOpened) {
        ImGui::OpenPopup(title);
        g_memoryPromptOpened = true;
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextWrapped("Memory type, speed and channel count come from the SMBIOS table, "
                           "which Linux only exposes to root. Without it the tool cannot tell how "
                           "many memory channels this system has, and the integrated-GPU comparison "
                           "falls back to an estimate.");
        ImGui::Spacing();
        ImGui::TextWrapped("\"Read with admin rights\" runs dmidecode once through pkexec (you will be "
                           "asked for your password). Nothing else runs elevated. You can also start "
                           "the tool with sudo, or use Help > Read memory details later.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Read with admin rights (pkexec)", ImVec2(270, 0))) {
            g_app.memoryPromptDismissed = true;
            ImGui::CloseCurrentPopup();
            ElevatedMemoryRead();
        }
        ImGui::SameLine();
        if (ImGui::Button("Continue without", ImVec2(160, 0))) {
            g_app.memoryPromptDismissed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void RenderGUI() {
    RenderMemoryElevationPrompt();

    // Initialize docking once
    if (!g_app.dockingInitialized) {
        ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
        if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_None);
            ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

            ImGuiID dock_left = 0;
            ImGuiID dock_main = dockspace_id;
            ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, 0.25f, &dock_left, &dock_main);

            ImGuiID dock_bottom = 0;
            ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, 0.15f, &dock_bottom, &dock_main);

            ImGui::DockBuilderDockWindow("Configuration", dock_left);
            ImGui::DockBuilderDockWindow("Output Log", dock_main);
            ImGui::DockBuilderDockWindow("Progress", dock_bottom);

            ImGui::DockBuilderFinish(dockspace_id);
        }
        g_app.dockingInitialized = true;
    }

    // Main dockspace
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    // ========== Top Menu Bar (moved About here) ==========
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Export CSV", nullptr, false, g_app.state == AppState::Completed)) {
                ExportBenchmarkCsvInteractive();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                glfwSetWindowShouldClose(g_app.window, GLFW_TRUE);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Results", nullptr, false, g_app.state == AppState::Completed)) {
                g_app.showResultsWindow = true;
            }
            if (ImGui::MenuItem("Graphs", nullptr, false, g_app.state == AppState::Completed)) {
                g_app.showGraphsWindow = true;
            }
            if (ImGui::MenuItem("Compare to Standards", nullptr, false, g_app.state == AppState::Completed)) {
                g_app.showCompareWindow = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Read memory details (admin)...")) {
                ElevatedMemoryRead();
            }
            if (ImGui::MenuItem("About")) {
                g_app.showAboutDialog = true;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("MainDockSpaceWindow", nullptr, window_flags);
    ImGui::PopStyleVar();

    ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    ImGui::End();

    // ========== Configuration Window (Left) ==========
    float configWidth = viewport->WorkSize.x * 0.25f;
    float progressHeight = 145.0f;  // Taller for complete state with 3 progress bars

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(configWidth, viewport->WorkSize.y - progressHeight));
    ImGui::Begin("Configuration", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    ImGui::Text("GPU-PCIe-Test v3.4 GUI");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Select GPU:");
    ImGui::SetNextItemWidth(-1);
    
    // Check if we have valid GPUs
    bool hasValidGPU = !g_app.gpuList.empty() && g_app.gpuList[0].isValid;
    
    if (!hasValidGPU) {
        ImGui::BeginDisabled();
    }
    ImGui::Combo("##GPU", &g_app.config.selectedGPU, g_app.gpuComboPointers.data(), (int)g_app.gpuComboPointers.size());
    if (!hasValidGPU) {
        ImGui::EndDisabled();
    }
    
    // Display GPU details below the combo box
    if (g_app.config.selectedGPU >= 0 && g_app.config.selectedGPU < static_cast<int>(g_app.gpuList.size())) {
        const GPUInfo& selectedGPU = g_app.gpuList[g_app.config.selectedGPU];
        
        if (selectedGPU.isValid) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
            
            // VRAM info
            if (selectedGPU.isIntegrated) {
                ImGui::Text("  Type: Integrated GPU");
                ImGui::Text("  Shared Memory: %s", FormatMemory(selectedGPU.sharedMemory).c_str());
            } else {
                ImGui::Text("  Type: Discrete GPU");
                ImGui::Text("  VRAM: %s", FormatMemory(selectedGPU.dedicatedVRAM).c_str());
            }
            
            // PCIe link info
            if (selectedGPU.pcieInfoValid) {
                ImGui::PopStyleColor();
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "  PCIe: Gen%d x%d", 
                    selectedGPU.pcieGenCurrent, selectedGPU.pcieLanesCurrent);
                if (selectedGPU.pcieGenCurrent < selectedGPU.pcieGenMax || 
                    selectedGPU.pcieLanesCurrent < selectedGPU.pcieLanesMax) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "(max: Gen%d x%d)", 
                        selectedGPU.pcieGenMax, selectedGPU.pcieLanesMax);
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
            } else {
                ImGui::Text("  PCIe: Not detected");
            }
            
            // Vendor/Device ID
            ImGui::Text("  ID: %s", FormatVendorDeviceId(selectedGPU.vendorId, selectedGPU.deviceId).c_str());
            
            // Show max safe bandwidth size
            size_t maxSafe = GetSafeMaxBandwidthSize(g_app.config.selectedGPU);
            ImGui::Text("  Max Safe Test Size: %s", FormatSize(maxSafe).c_str());
            
            ImGui::PopStyleColor();
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "  No valid GPU detected!");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "  Check your graphics drivers.");
        }
        
        // System RAM info (useful for diagnosing bottlenecks)
        if (g_app.systemMemory.detected) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 0.6f, 1.0f), "System RAM:");
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
            
            // Format: "64GB DDR5 @ 6000 MT/s"
            char ramBuf[128];
            if (g_app.systemMemory.configuredSpeedMT > 0) {
                snprintf(ramBuf, sizeof(ramBuf), "  %" PRIu64 "GB %s @ %u MT/s",
                        g_app.systemMemory.totalCapacityGB,
                        g_app.systemMemory.type.c_str(),
                        g_app.systemMemory.configuredSpeedMT);
            } else {
                snprintf(ramBuf, sizeof(ramBuf), "  %" PRIu64 "GB %s",
                        g_app.systemMemory.totalCapacityGB,
                        g_app.systemMemory.type.c_str());
            }
            ImGui::Text("%s", ramBuf);
            
            // Channel config and theoretical bandwidth
            const char* channelStr = (g_app.systemMemory.channels == 1) ? "single" :
                                     (g_app.systemMemory.channels == 2) ? "dual" :
                                     (g_app.systemMemory.channels == 4) ? "quad" : "multi";
            snprintf(ramBuf, sizeof(ramBuf), "  %s-channel (~%.0f GB/s)",
                    channelStr, g_app.systemMemory.theoreticalBandwidth * 0.8);  // ~80% efficiency
            ImGui::Text("%s", ramBuf);

            // Estimated chip latency (from speed tier lookup, not hardware)
            if (g_app.systemMemory.latencyEstimated) {
                snprintf(ramBuf, sizeof(ramBuf), "  Est. Chip Latency: ~%.1f ns (~CL%d typical)",
                        g_app.systemMemory.ratedLatencyNs, g_app.systemMemory.estimatedCL);
                ImGui::Text("%s", ramBuf);
            }

            ImGui::PopStyleColor();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Bandwidth Test Size");
    ImGui::SetNextItemWidth(-1);

    // Bandwidth in MB - allow up to safe max based on GPU VRAM
    int maxBandwidthMB = static_cast<int>(GetSafeMaxBandwidthSize(g_app.config.selectedGPU) / (1024 * 1024));
    maxBandwidthMB = std::max(maxBandwidthMB, 16);  // Ensure minimum of 16 MB
    
    int bandwidth_mb = static_cast<int>(g_app.config.bandwidthSize / (1024 * 1024));
    bandwidth_mb = std::min(bandwidth_mb, maxBandwidthMB);  // Ensure current value doesn't exceed max
    
    if (ImGui::SliderInt("##BandwidthSize", &bandwidth_mb, 16, maxBandwidthMB, "%d MB", ImGuiSliderFlags_Logarithmic)) {
        g_app.config.bandwidthSize = static_cast<size_t>(bandwidth_mb) * 1024 * 1024;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Max safe size for this GPU: %d MB", maxBandwidthMB);
    }

    ImGui::Text("Latency Test Size");
    ImGui::SetNextItemWidth(-1);

    // Latency in bytes (already safe, but consistent style)
    int latency_bytes = static_cast<int>(g_app.config.latencySize);
    if (ImGui::SliderInt("##LatencySize", &latency_bytes, 1, 1024, "%d B", ImGuiSliderFlags_Logarithmic)) {
        g_app.config.latencySize = static_cast<size_t>(latency_bytes);
    }

    ImGui::Text("Latency Iterations");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##LatencyIters", &g_app.config.latencyIters, 500, 10000, "%d");

    ImGui::Text("Bandwidth Batches");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##BandwidthBatches", &g_app.config.bandwidthBatches, 8, 128);

    ImGui::Text("Copies per Batch");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##CopiesPerBatch", &g_app.config.copiesPerBatch, 1, 32);

    ImGui::Text("Number of Runs");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##Runs", &g_app.config.numRuns, 1, 10);

    ImGui::Spacing();
    ImGui::Checkbox("Run Bidirectional Test", &g_app.config.runBidirectional);
    ImGui::Checkbox("Run Latency Tests", &g_app.config.runLatency);
    ImGui::Checkbox("Run Memory Latency Test", &g_app.config.runMemoryLatency);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Measures GPU memory access latency using a compute shader pointer-chase.\n"
                         "On discrete GPUs, this measures VRAM latency.\n"
                         "On integrated GPUs (APUs), this measures system RAM latency\n"
                         "from the GPU's perspective (includes fabric overhead).");
    }
    ImGui::Checkbox("Debug Logging", &g_app.config.debugLogging);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Enable verbose diagnostic logging for memory latency test\n"
                         "and other internal operations. Useful for troubleshooting.");
    }

    ImGui::Spacing();
    
    // Track previous state to detect changes
    static bool prevAverageRuns = g_app.config.averageRuns;
    if (ImGui::Checkbox("Average Runs", &g_app.config.averageRuns)) {
        // Clear results when switching modes - can't compare averaged vs individual
        if (prevAverageRuns != g_app.config.averageRuns && !g_app.results.empty()) {
            std::lock_guard<std::mutex> lock(g_app.resultsMutex);
            g_app.results.clear();
            g_app.uploadBW = 0;
            g_app.downloadBW = 0;
            g_app.uploadPercentage = 0;
            g_app.downloadPercentage = 0;
            g_app.closestUploadStandard.clear();
            g_app.closestDownloadStandard.clear();
            g_app.detectedInterface.clear();
            if (g_app.state == AppState::Completed) {
                g_app.state = AppState::Idle;
            }
            Log("[INFO] Results cleared - switched between Average/Individual modes");
        }
        prevAverageRuns = g_app.config.averageRuns;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When enabled, combines all runs into averaged results.\nWhen disabled, shows each run individually.\nChanging this clears existing results.");
    }
    if (!g_app.config.averageRuns) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "(Individual)");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Quick mode - overrides settings for faster benchmark
    ImGui::Checkbox("Quick Mode", &g_app.config.quickMode);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Quick mode: 1 run, 16 batches, 500 latency iterations\nDisables manual settings above");
    }
    if (g_app.config.quickMode) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "(Fast)");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Can start when Idle OR when Completed (to run more tests)
    bool canStart = (g_app.state == AppState::Idle || g_app.state == AppState::Completed) && 
                    hasValidGPU && !g_app.vramTestRunning;
    bool canStop = (g_app.state == AppState::Running);
    bool hasResults = !g_app.results.empty();  // Changed: check if results exist, not state

    if (!canStart) ImGui::BeginDisabled();
    if (ImGui::Button("Start Benchmark", ImVec2(-1, 40))) {
        // Apply quick mode overrides (don't permanently change config)
        int actualRuns = g_app.config.quickMode ? 1 : g_app.config.numRuns;
        int actualBatches = g_app.config.quickMode ? 16 : g_app.config.bandwidthBatches;
        int actualLatencyIters = g_app.config.quickMode ? 500 : g_app.config.latencyIters;

        // Temporarily apply for this run
        g_app.config.numRuns = actualRuns;
        g_app.config.bandwidthBatches = actualBatches;
        g_app.config.latencyIters = actualLatencyIters;

        g_app.state = AppState::Running;
        g_app.progress = 0.0f;
        g_app.overallProgress = 0.0f;
        g_app.completedTests = 0;
        g_app.totalTests = 0;
        g_app.currentRun = 0;
        SetCurrentTest("Initializing...");
        g_app.cancelRequested = false;
        g_app.benchmarkAborted = false;
        g_app.possibleEGPU = false;
        g_app.eGPUConnectionType.clear();
        g_app.showResultsWindow = false;
        g_app.showGraphsWindow = false;
        g_app.showCompareWindow = false;
        // NOTE: Don't clear g_app.results - we want to accumulate results
        ClearLog();
        
        // Join previous thread if still somehow running
        if (g_app.benchmarkThread.joinable()) {
            g_app.benchmarkThread.join();
        }
        
        g_app.benchmarkThread = std::thread(BenchmarkThreadFunc);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (g_app.vramTestRunning) {
            ImGui::SetTooltip("Wait for VRAM scan to complete first");
        } else if (!hasValidGPU) {
            ImGui::SetTooltip("No valid GPU selected");
        }
    }
    if (!canStart) ImGui::EndDisabled();

    if (!canStop) ImGui::BeginDisabled();
    if (ImGui::Button("Cancel", ImVec2(-1, 30))) {
        g_app.cancelRequested = true;
        Log("Cancellation requested...");
    }
    if (!canStop) ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Results buttons - available when we have results
    if (!hasResults) ImGui::BeginDisabled();
    if (ImGui::Button("View Summary", ImVec2(-1, 30))) {
        g_app.showSummaryWindow = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("View detailed analysis comparing measured vs actual PCIe link");
    }
    if (ImGui::Button("View Results", ImVec2(-1, 30))) {
        g_app.showResultsWindow = true;
    }
    if (ImGui::Button("View Graphs", ImVec2(-1, 30))) {
        g_app.showGraphsWindow = true;
    }
    if (ImGui::Button("Compare to Standards", ImVec2(-1, 30))) {
        g_app.showCompareWindow = true;
    }
    if (ImGui::Button("Export to CSV", ImVec2(-1, 30))) {
        ExportBenchmarkCsvInteractive();
    }
    
    ImGui::Spacing();
    
    // Clear Charts button
    if (ImGui::Button("Clear Charts", ImVec2(-1, 30))) {
        std::lock_guard<std::mutex> lock(g_app.resultsMutex);
        g_app.results.clear();
        g_app.uploadBW = 0;
        g_app.downloadBW = 0;
        g_app.uploadPercentage = 0;
        g_app.downloadPercentage = 0;
        g_app.closestUploadStandard.clear();
        g_app.closestDownloadStandard.clear();
        g_app.detectedInterface.clear();
        g_app.detectedInterfaceDescription.clear();
        g_app.possibleEGPU = false;
        g_app.eGPUConnectionType.clear();
        if (g_app.state == AppState::Completed) {
            g_app.state = AppState::Idle;
        }
        Log("[INFO] Charts cleared");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Clear all benchmark results and graphs");
    }
    if (!hasResults) ImGui::EndDisabled();
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    // Reset Settings button (always available when not running)
    if (canStop) ImGui::BeginDisabled();
    if (ImGui::Button("Reset Settings", ImVec2(-1, 30))) {
        g_app.config.bandwidthSize = Constants::DEFAULT_BANDWIDTH_SIZE;
        g_app.config.latencySize = Constants::DEFAULT_LATENCY_SIZE;
        g_app.config.bandwidthBatches = Constants::DEFAULT_BANDWIDTH_BATCHES;
        g_app.config.copiesPerBatch = Constants::DEFAULT_COPIES_PER_BATCH;
        g_app.config.latencyIters = Constants::DEFAULT_LATENCY_ITERS;
        g_app.config.numRuns = Constants::DEFAULT_NUM_RUNS;
        g_app.config.runBidirectional = true;
        g_app.config.runLatency = true;
        g_app.config.runMemoryLatency = true;
        g_app.config.quickMode = false;
        g_app.config.averageRuns = true;
        g_app.config.debugLogging = false;
        // Reset VRAM scan options to the Standard preset
        ApplyVRAMScanPreset(VRAMScanPreset::Standard, g_app.config);
        // Don't reset selectedGPU or clear results
        Log("[INFO] Settings reset to defaults");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Reset all settings to default values\n(Does not clear chart data)");
    }
    if (canStop) ImGui::EndDisabled();
    
    // ========== VRAM Scan Section ==========
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "VRAM Diagnostics");
    ImGui::Spacing();
    
    bool isIntegratedGPU = g_app.gpuList[g_app.config.selectedGPU].isIntegrated;
    bool vramScanDisabled = canStop || g_app.vramTestRunning || isIntegratedGPU;
    
    // Explain why VRAM scan isn't available for iGPUs
    if (isIntegratedGPU) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        ImGui::TextWrapped("VRAM scan unavailable: Integrated GPUs use system RAM. "
                          "Test with MemTest86+ or memtester instead.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    
    // VRAM scan options (only when not running and not iGPU)
    if (!g_app.vramTestRunning && !isIntegratedGPU) {
        static const char* PRESET_NAMES[] = { "Quick", "Standard", "Deep", "Thorough", "Marathon", "Custom" };
        int presetIdx = static_cast<int>(g_app.config.vramScanPreset);
        if (presetIdx < 0 || presetIdx > 5) presetIdx = 1;
        ImGui::Text("Scan preset:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        if (ImGui::Combo("##VRAMPreset", &presetIdx, PRESET_NAMES, IM_ARRAYSIZE(PRESET_NAMES))) {
            ApplyVRAMScanPreset(static_cast<VRAMScanPreset>(presetIdx), g_app.config);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Quick: 4 patterns, 50%% coverage (~30s)\n"
                             "Standard: 8 patterns, 80%% coverage (~2-3min) - default\n"
                             "Deep: 8 patterns + refresh + non-seq + pre-heat 30s + GPU verify, 90%% (~5min)\n"
                             "Thorough: same as Deep with 10x re-reads + 60s pre-heat, 95%% (~12-15min)\n"
                             "Marathon: same as Thorough but loops forever (cancel to stop)\n"
                             "Custom: whatever you set individually below");
        }

        auto markCustomIfChanged = [](bool changed) {
            if (changed) g_app.config.vramScanPreset = DetectVRAMScanPreset(g_app.config);
        };

        int enabledCount = 0;
        for (bool b : g_app.config.vramPatternsEnabled) if (b) enabledCount++;
        char patternHeader[64];
        snprintf(patternHeader, sizeof(patternHeader), "Patterns (%d of 8 enabled)###VRAMPatHdr", enabledCount);
        if (ImGui::TreeNode(patternHeader)) {
            static const char* PATTERN_LABELS[8] = {
                "All Zeros", "All Ones", "Checkerboard", "Inverse Checkerboard",
                "Random", "Marching Ones", "Marching Zeros", "Address Pattern"
            };
            for (int row = 0; row < 4; ++row) {
                for (int col = 0; col < 2; ++col) {
                    int idx = row + col * 4;
                    bool v = g_app.config.vramPatternsEnabled[idx];
                    if (col > 0) ImGui::SameLine(220);
                    if (ImGui::Checkbox(PATTERN_LABELS[idx], &v)) {
                        g_app.config.vramPatternsEnabled[idx] = v;
                        markCustomIfChanged(true);
                    }
                }
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Stress checks###VRAMStressHdr")) {
            bool rr = g_app.config.vramRereadEnabled;
            if (ImGui::Checkbox("Refresh check (re-read pass)", &rr)) {
                g_app.config.vramRereadEnabled = rr;
                markCustomIfChanged(true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("After writing each chunk, re-read it multiple times\n"
                                 "with brief delays. Drift between reads = a\n"
                                 "data retention / refresh cycle error.");
            }
            if (g_app.config.vramRereadEnabled) {
                int iters = g_app.config.vramRereadIterations;
                ImGui::Indent();
                ImGui::SetNextItemWidth(200);
                if (ImGui::SliderInt("Iterations", &iters, 1, 20)) {
                    g_app.config.vramRereadIterations = iters;
                    markCustomIfChanged(true);
                }
                ImGui::Unindent();
            }

            bool ns = g_app.config.vramNonSequentialEnabled;
            if (ImGui::Checkbox("Address-bus check (non-sequential reads)", &ns)) {
                g_app.config.vramNonSequentialEnabled = ns;
                markCustomIfChanged(true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Read back chunks in randomized block order to defeat\n"
                                 "row buffer caching. Exposes address-bus errors and\n"
                                 "weakly-charged cells that sequential reads hide.");
            }
            if (g_app.config.vramNonSequentialEnabled) {
                static const char* BLOCK_LABELS[] = { "16 KB", "64 KB", "256 KB" };
                static const int   BLOCK_VALUES[] = { 16384, 65536, 262144 };
                int blockIdx = 1;
                for (int i = 0; i < 3; ++i) {
                    if (g_app.config.vramNonSequentialBlockSize == BLOCK_VALUES[i]) { blockIdx = i; break; }
                }
                ImGui::Indent();
                ImGui::SetNextItemWidth(120);
                if (ImGui::Combo("Block size##VRAMNSBlock", &blockIdx, BLOCK_LABELS, IM_ARRAYSIZE(BLOCK_LABELS))) {
                    g_app.config.vramNonSequentialBlockSize = BLOCK_VALUES[blockIdx];
                    markCustomIfChanged(true);
                }
                ImGui::Unindent();
            }

            bool ph = g_app.config.vramPreheatEnabled;
            if (ImGui::Checkbox("Pre-heat GPU before scan", &ph)) {
                g_app.config.vramPreheatEnabled = ph;
                markCustomIfChanged(true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Run continuous GPU memory traffic before the scan starts\n"
                                 "so the silicon reaches operating temperature.\n"
                                 "Exposes thermal-dependent errors that pass on a cold GPU.");
            }
            if (g_app.config.vramPreheatEnabled) {
                int psec = g_app.config.vramPreheatSeconds;
                ImGui::Indent();
                ImGui::SetNextItemWidth(200);
                if (ImGui::SliderInt("Pre-heat seconds", &psec, 10, 120)) {
                    g_app.config.vramPreheatSeconds = psec;
                    markCustomIfChanged(true);
                }
                ImGui::Unindent();
            }

            bool gv = g_app.config.vramGpuVerify;
            if (ImGui::Checkbox("GPU verify (compute shader)", &gv)) {
                g_app.config.vramGpuVerify = gv;
                markCustomIfChanged(true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Use a compute shader to compare patterns on the GPU\n"
                                 "instead of reading back to CPU. Much higher throughput\n"
                                 "(no PCIe round-trip on every chunk). Random pattern still\n"
                                 "uses CPU verify since its RNG cannot be replicated in GLSL.");
            }

            ImGui::TreePop();
        }

        static const char* COVERAGE_LABELS[] = { "50%%", "80%%", "90%%", "95%%" };
        static const int   COVERAGE_VALUES[] = { 50, 80, 90, 95 };
        int covIdx = 1;
        for (int i = 0; i < 4; ++i) {
            if (g_app.config.vramCoveragePercent == COVERAGE_VALUES[i]) { covIdx = i; break; }
        }
        ImGui::Text("Coverage:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        if (ImGui::Combo("##VRAMCoverage", &covIdx, COVERAGE_LABELS, IM_ARRAYSIZE(COVERAGE_LABELS))) {
            g_app.config.vramCoveragePercent = COVERAGE_VALUES[covIdx];
            markCustomIfChanged(true);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Percentage of VRAM to test.\n"
                             "Higher coverage = more thorough but may reduce stability\n"
                             "as it competes with the GPU driver / OS for memory.");
        }

        ImGui::TextDisabled("Bit-level error categorization is always on");
        ImGui::Spacing();
    }
    
    if (vramScanDisabled) ImGui::BeginDisabled();
    if (ImGui::Button("VRAM Scan", ImVec2(-1, 35))) {
        if (!g_app.vramTestRunning && !g_app.benchmarkThreadRunning) {
            // Clear any previous results immediately
            g_app.vramTestResult = {};
            SetVramPattern(std::string());
            
            g_app.vramTestCancelRequested = false;
            g_app.vramTestRunning = true;
            g_app.vramTestProgress = 0.0f;
            if (g_app.vramTestThread.joinable()) {
                g_app.vramTestThread.join();
            }
            g_app.vramTestThread = std::thread(VRAMTestThreadFunc);
        }
    }
    if (vramScanDisabled) ImGui::EndDisabled();
    
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (g_app.gpuList[g_app.config.selectedGPU].isIntegrated) {
            ImGui::SetTooltip("VRAM scan not available for integrated GPUs\n(uses shared system memory)");
        } else if (g_app.vramTestRunning) {
            ImGui::SetTooltip("VRAM scan in progress...");
        } else {
            ImGui::SetTooltip("Scan VRAM for errors using multiple test patterns.\n"
                             "This can help detect faulty video memory.\n\n"
                             "NOTE: This is a basic test, not a replacement for\n"
                             "vendor tools like NVIDIA MATS.");
        }
    }
    
    // Cancel button for VRAM test
    if (g_app.vramTestRunning) {
        if (ImGui::Button("Cancel VRAM Scan", ImVec2(-1, 30))) {
            g_app.vramTestCancelRequested = true;
            Log("[INFO] VRAM scan cancellation requested...");
        }
        
        // Show VRAM test progress
        ImGui::Spacing();
        ImGui::ProgressBar(g_app.vramTestProgress, ImVec2(-1, 20));
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Pattern: %s", 
                          GetVramPattern().c_str());
    }
    
    // Show VRAM test results button if test completed
    if (g_app.vramTestResult.completed || g_app.vramTestResult.cancelled) {
        if (ImGui::Button("View VRAM Results", ImVec2(-1, 30))) {
            g_app.showVRAMTestWindow = true;
        }
        
        // Quick status indicator
        if (g_app.vramTestResult.totalErrors == 0) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Last scan: PASS");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Last scan: %zu errors!", 
                              g_app.vramTestResult.totalErrors);
        }
    }

    ImGui::End();

    // ========== Log Window (Center) ==========
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + configWidth, viewport->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x - configWidth, viewport->WorkSize.y - progressHeight));
    ImGui::Begin("Output Log", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    // Scrollable log area
    ImGui::BeginChild("LogScroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lock(g_app.logMutex);
        for (const auto& line : g_app.logLines) {
            // Color code different types of messages
            if (line.find("===") != std::string::npos) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", line.c_str());
            }
            else if (line.find("---") != std::string::npos) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%s", line.c_str());
            }
            else if (line.find("ERROR") != std::string::npos || line.find("CRITICAL") != std::string::npos) {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", line.c_str());
            }
            else if (line.find("WARNING") != std::string::npos) {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", line.c_str());
            }
            else if (line.find("INFO") != std::string::npos) {
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s", line.c_str());
            }
            else if (line.find("GB/s") != std::string::npos || line.find(" us") != std::string::npos) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", line.c_str());
            }
            else if (line.find("eGPU") != std::string::npos) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 1.0f, 1.0f), "%s", line.c_str());
            }
            else {
                ImGui::Text("%s", line.c_str());
            }
        }
    }
    // Auto-scroll to bottom
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button("Copy Log to Clipboard", ImVec2(-1, 0))) {
        std::lock_guard<std::mutex> lock(g_app.logMutex);
        std::string allLog;
        for (const auto& line : g_app.logLines) {
            allLog += line + "\n";
        }
        ImGui::SetClipboardText(allLog.c_str());
    }

    ImGui::End();

    // ========== Progress Bar (Bottom) ==========
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - progressHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, progressHeight));
    ImGui::Begin("Progress", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);

    if (g_app.state == AppState::Running) {
        ImGui::Text("Current: %s", GetCurrentTest().c_str());

        // Overall progress bar
        char overlayBuf[64];
        snprintf(overlayBuf, sizeof(overlayBuf), "Overall: %d / %d tests (%.0f%%)",
            (int)g_app.completedTests, (int)g_app.totalTests, g_app.overallProgress * 100.0f);
        ImGui::ProgressBar(g_app.overallProgress, ImVec2(-1, 24), overlayBuf);

        // Per-test progress
        ImGui::ProgressBar(g_app.progress, ImVec2(-1, 24), "Test progress");

    }
    else if (g_app.state == AppState::Completed) {
        // Show summary with percentages
        char resultBuf[256];
        snprintf(resultBuf, sizeof(resultBuf),
            "Complete! | %s | CPU->GPU: %.2f GB/s (%.0f%% of %s) | GPU->CPU: %.2f GB/s (%.0f%% of %s)",
            g_app.detectedInterface.c_str(),
            g_app.uploadBW, g_app.uploadPercentage, g_app.closestUploadStandard.c_str(),
            g_app.downloadBW, g_app.downloadPercentage, g_app.closestDownloadStandard.c_str());
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", resultBuf);
        
        // Show eGPU detection if applicable
        if (g_app.possibleEGPU) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 1.0f, 1.0f), " | Possible eGPU via %s", g_app.eGPUConnectionType.c_str());
        }

        // All bars green for completed state
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));  // Green
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.3f, 0.1f, 1.0f));  // Dark green background

        // Full green "Complete" progress bar
        ImGui::ProgressBar(1.0f, ImVec2(-1, 28), "Benchmark Complete!");

        // Progress bars showing percentages
        char uploadBar[64], downloadBar[64];
        snprintf(uploadBar, sizeof(uploadBar), "CPU->GPU: %.0f%% of %s", g_app.uploadPercentage, g_app.closestUploadStandard.c_str());
        snprintf(downloadBar, sizeof(downloadBar), "GPU->CPU: %.0f%% of %s", g_app.downloadPercentage, g_app.closestDownloadStandard.c_str());

        // Normalize percentages (cap at 150% for display)
        float uploadPct = std::min(static_cast<float>(g_app.uploadPercentage) / 100.0f, 1.5f) / 1.5f;
        float downloadPct = std::min(static_cast<float>(g_app.downloadPercentage) / 100.0f, 1.5f) / 1.5f;

        float halfWidth = (viewport->WorkSize.x / 2) - 10;
        ImGui::ProgressBar(uploadPct, ImVec2(halfWidth, 28), uploadBar);
        ImGui::SameLine();
        ImGui::ProgressBar(downloadPct, ImVec2(-1, 28), downloadBar);

        ImGui::PopStyleColor(2);  // Pop both colors
    }
    else {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Ready - Click 'Start Benchmark' to begin");
        ImGui::ProgressBar(0.0f, ImVec2(-1, 24), "Idle");
    }

    ImGui::End();

    // ========== Summary Window (Auto-popup after benchmark) ==========
    if (g_app.showSummaryWindow && !g_app.results.empty()) {
        ImGui::SetNextWindowSize(ImVec2(700, 550), ImGuiCond_FirstUseEver);
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        
        ImGui::Begin("Benchmark Summary", &g_app.showSummaryWindow);
        // Worker threads append to g_app.results; hold the lock for the whole
        // window so the loops below never iterate a vector mid-reallocation.
        std::lock_guard<std::mutex> resultsLock(g_app.resultsMutex);
        
        const GPUInfo& gpu = g_app.gpuList[g_app.config.selectedGPU];
        
        // GPU Info header
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "GPU: %s", gpu.name.c_str());
        ImGui::Separator();
        ImGui::Spacing();
        
        // Measured Performance section
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "MEASURED PERFORMANCE");
        ImGui::Indent();
        
        const char* modeLabel = g_app.config.averageRuns ? "(Averaged)" : "(Best Run)";
        ImGui::Text("Mode: %s", modeLabel);
        
        ImGui::Text("CPU -> GPU: ");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%.2f GB/s", g_app.uploadBW);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(%.0f%% of %s)", 
            g_app.uploadPercentage, g_app.closestUploadStandard.c_str());
        
        ImGui::Text("GPU -> CPU: ");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%.2f GB/s", g_app.downloadBW);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(%.0f%% of %s)", 
            g_app.downloadPercentage, g_app.closestDownloadStandard.c_str());
        
        ImGui::Text("Inferred Interface: ");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "%s", g_app.detectedInterface.c_str());
        
        if (g_app.possibleEGPU) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 1.0f, 1.0f), 
                "Note: Possible eGPU via %s", g_app.eGPUConnectionType.c_str());
        }
        ImGui::Unindent();
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // Actual PCIe Link section
        ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "DETECTED PCIe LINK (from sysfs)");
        ImGui::Indent();
        
        if (gpu.pcieInfoValid) {
            ImGui::Text("Current Link: ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", g_app.actualPCIeConfig.c_str());
            
            ImGui::Text("Theoretical Max: ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%.2f GB/s", g_app.actualPCIeBandwidth);
            
            if (gpu.pcieGenMax > 0 && gpu.pcieLanesMax > 0) {
                std::string maxConfig = FormatPCIeConfig(gpu.pcieGenMax, gpu.pcieLanesMax);
                double maxBw = CalculatePCIeBandwidth(gpu.pcieGenMax, gpu.pcieLanesMax);
                
                ImGui::Text("GPU Maximum: ");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s (%.2f GB/s)", 
                    maxConfig.c_str(), maxBw);
            }
            
            if (!gpu.pcieLocationPath.empty()) {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Location: %s", 
                    gpu.pcieLocationPath.c_str());
            }
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Could not detect PCIe link configuration");
            if (gpu.isIntegrated) {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), 
                    "(Integrated GPUs may not report PCIe info)");
            }
        }
        ImGui::Unindent();
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // Analysis section
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "ANALYSIS");
        ImGui::Indent();
        
        // Determine analysis color based on content
        ImVec4 analysisColor = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
        if (g_app.summaryExplanation.find("EXCELLENT") != std::string::npos) {
            analysisColor = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
        } else if (g_app.summaryExplanation.find("GOOD") != std::string::npos) {
            analysisColor = ImVec4(0.8f, 1.0f, 0.4f, 1.0f);
        } else if (g_app.summaryExplanation.find("SLOWER") != std::string::npos) {
            analysisColor = ImVec4(1.0f, 0.7f, 0.3f, 1.0f);
        } else if (g_app.summaryExplanation.find("FASTER THAN") != std::string::npos) {
            analysisColor = ImVec4(0.4f, 0.8f, 1.0f, 1.0f);
        }
        
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(analysisColor, "%s", g_app.summaryExplanation.c_str());
        ImGui::PopTextWrapPos();

        ImGui::Unindent();

        // Memory Latency section
        {
            bool hasMemLatency = false;
            for (const auto& r : g_app.results) {
                if (r.testName.find("GPU Memory Latency") != std::string::npos) {
                    hasMemLatency = true;
                    break;
                }
            }
            if (hasMemLatency) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.8f, 0.6f, 1.0f, 1.0f), "MEMORY LATENCY");
                ImGui::Indent();

                for (const auto& r : g_app.results) {
                    if (r.testName.find("GPU Memory Latency") != std::string::npos) {
                        ImGui::Text("GPU VRAM Latency: ");
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%.1f ns", r.avgValue);
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                            "(min %.1f / max %.1f ns)", r.minValue, r.maxValue);
                        break;  // Show first/averaged result
                    }
                }

                if (g_app.systemMemory.latencyEstimated) {
                    ImGui::Text("Chip (estimated):");
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "~%.1f ns",
                        g_app.systemMemory.ratedLatencyNs);
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(~CL%d typical for speed tier)",
                        g_app.systemMemory.estimatedCL);
                }

                ImGui::Unindent();
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Close button
        float buttonWidth = 120.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - buttonWidth) * 0.5f);
        if (ImGui::Button("Close", ImVec2(buttonWidth, 30))) {
            g_app.showSummaryWindow = false;
        }
        
        ImGui::End();
    }

    // ========== Results Window (Popup, only when requested) ==========
    if (g_app.showResultsWindow && !g_app.results.empty()) {
        ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
        ImGui::Begin("Results", &g_app.showResultsWindow);

        std::lock_guard<std::mutex> lock(g_app.resultsMutex);

        if (ImGui::BeginTable("ResultsTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Test", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableHeadersRow();

            for (const auto& r : g_app.results) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s", r.testName.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.2f %s", r.minValue, r.unit.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.2f %s", r.avgValue, r.unit.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.2f %s", r.maxValue, r.unit.c_str());
            }

            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Interface detection with percentages
        ImGui::Text("Inferred Interface (from bandwidth):");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "%s", g_app.detectedInterface.c_str());
        
        const GPUInfo& gpu = g_app.gpuList[g_app.config.selectedGPU];
        ImGui::Text("Detected PCIe Link (from sysfs):");
        ImGui::SameLine();
        if (gpu.pcieInfoValid) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s (Theoretical %.2f GB/s)", g_app.actualPCIeConfig.c_str(), g_app.actualPCIeBandwidth);
        }
        else {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Not detected");
        }

        // eGPU detection
        if (g_app.possibleEGPU) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 1.0f, 1.0f), " (Possible eGPU via %s)", g_app.eGPUConnectionType.c_str());
        }

        ImGui::Spacing();

        char uploadStr[128], downloadStr[128];
        snprintf(uploadStr, sizeof(uploadStr), "CPU->GPU: %.2f GB/s (%.0f%% of %s)",
            g_app.uploadBW, g_app.uploadPercentage, g_app.closestUploadStandard.c_str());
        snprintf(downloadStr, sizeof(downloadStr), "GPU->CPU: %.2f GB/s (%.0f%% of %s)",
            g_app.downloadBW, g_app.downloadPercentage, g_app.closestDownloadStandard.c_str());

        // Color based on percentage
        ImVec4 uploadColor = g_app.uploadPercentage >= 90 ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) :
            g_app.uploadPercentage >= 70 ? ImVec4(1.0f, 1.0f, 0.4f, 1.0f) :
            ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        ImVec4 downloadColor = g_app.downloadPercentage >= 90 ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) :
            g_app.downloadPercentage >= 70 ? ImVec4(1.0f, 1.0f, 0.4f, 1.0f) :
            ImVec4(1.0f, 0.4f, 0.4f, 1.0f);

        ImGui::TextColored(uploadColor, "%s", uploadStr);
        ImGui::TextColored(downloadColor, "%s", downloadStr);

        ImGui::End();
    }

    // ========== Graphs Window (Popup, only when requested) ==========
    if (g_app.showGraphsWindow && !g_app.results.empty()) {
        ImGui::SetNextWindowSize(ImVec2(900, 700), ImGuiCond_FirstUseEver);
        ImGui::Begin("Graphs", &g_app.showGraphsWindow);

        std::lock_guard<std::mutex> lock(g_app.resultsMutex);

        // Bandwidth results - maintain order (oldest first = top of chart)
        std::vector<const BenchmarkResult*> bandwidthResults;
        std::vector<const BenchmarkResult*> latencyResults;
        for (const auto& r : g_app.results) {
            if (r.unit == "GB/s") bandwidthResults.push_back(&r);
            else if (r.unit == "us") latencyResults.push_back(&r);
        }

        if (!bandwidthResults.empty()) {
            // Show appropriate label based on mode
            if (g_app.config.averageRuns) {
                ImGui::Text("Bandwidth Tests (GB/s) - Min / Avg / Max  [Oldest at top]");
            } else {
                ImGui::Text("Bandwidth Tests (GB/s) - Min / Avg / Best  [Oldest at top]");
            }
            
            // Build data for grouped bars
            static std::vector<std::string> bwLabelStorage;
            static std::vector<const char*> bwLabels;
            bwLabelStorage.clear();
            bwLabels.clear();
            
            int numTests = static_cast<int>(bandwidthResults.size());
            std::vector<double> mins(numTests), avgs(numTests), maxs(numTests);
            
            for (int i = 0; i < numTests; ++i) {
                bwLabelStorage.push_back(bandwidthResults[i]->testName);
                mins[i] = bandwidthResults[i]->minValue;
                avgs[i] = bandwidthResults[i]->avgValue;
                maxs[i] = bandwidthResults[i]->maxValue;
            }
            for (const auto& s : bwLabelStorage) bwLabels.push_back(s.c_str());
            
            // Plot height based on number of tests
            float plotHeight = std::max(200.0f, numTests * 50.0f + 80.0f);
            
            if (ImPlot::BeginPlot("##Bandwidth", ImVec2(-1, plotHeight))) {
                // Invert Y axis so oldest (index 0) appears at TOP
                ImPlot::SetupAxes("GB/s", "", 0, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_Invert);
                
                // Calculate positions for grouped bars
                double barWidth = 0.25;
                
                std::vector<double> positions(numTests);
                for (int i = 0; i < numTests; ++i) positions[i] = static_cast<double>(i);
                
                // Create offset positions for each group
                std::vector<double> minPos(numTests), avgPos(numTests), maxPos(numTests);
                for (int i = 0; i < numTests; ++i) {
                    minPos[i] = positions[i] - barWidth;
                    avgPos[i] = positions[i];
                    maxPos[i] = positions[i] + barWidth;
                }
                
                // Setup Y axis with test names
                ImPlot::SetupAxisTicks(ImAxis_Y1, positions.data(), numTests, bwLabels.data());
                
                // Plot horizontal bars
                ImPlot::SetNextFillStyle(ImVec4(0.2f, 0.6f, 1.0f, 0.8f));  // Blue for Min
                ImPlot::PlotBars("Min", mins.data(), minPos.data(), numTests, barWidth * 0.9, ImPlotBarsFlags_Horizontal);
                
                ImPlot::SetNextFillStyle(ImVec4(0.2f, 0.9f, 0.2f, 0.8f));  // Green for Avg
                ImPlot::PlotBars("Avg", avgs.data(), avgPos.data(), numTests, barWidth * 0.9, ImPlotBarsFlags_Horizontal);
                
                ImPlot::SetNextFillStyle(ImVec4(1.0f, 0.4f, 0.2f, 0.8f));  // Orange for Max
                ImPlot::PlotBars("Max", maxs.data(), maxPos.data(), numTests, barWidth * 0.9, ImPlotBarsFlags_Horizontal);
                
                ImPlot::EndPlot();
            }
            
            // Legend explanation - change "Max" to "Best" in individual mode
            ImGui::TextColored(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), "Blue = Min");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "  Green = Avg");
            ImGui::SameLine();
            if (g_app.config.averageRuns) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), "  Orange = Max");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), "  Orange = Best");
            }
        }

        if (!latencyResults.empty()) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            
            // Show appropriate label based on mode
            if (g_app.config.averageRuns) {
                ImGui::Text("Latency Tests (microseconds) - Min / Avg / Max  [Oldest at top]");
            } else {
                ImGui::Text("Latency Tests (microseconds) - Min / Avg / Best  [Oldest at top]");
            }
            
            // Build data for grouped bars
            static std::vector<std::string> latLabelStorage;
            static std::vector<const char*> latLabels;
            latLabelStorage.clear();
            latLabels.clear();
            
            int numTests = static_cast<int>(latencyResults.size());
            std::vector<double> mins(numTests), avgs(numTests), maxs(numTests);
            
            for (int i = 0; i < numTests; ++i) {
                latLabelStorage.push_back(latencyResults[i]->testName);
                mins[i] = latencyResults[i]->minValue;
                avgs[i] = latencyResults[i]->avgValue;
                maxs[i] = latencyResults[i]->maxValue;
            }
            for (const auto& s : latLabelStorage) latLabels.push_back(s.c_str());
            
            float plotHeight = std::max(200.0f, numTests * 50.0f + 80.0f);
            
            if (ImPlot::BeginPlot("##Latency", ImVec2(-1, plotHeight))) {
                // Invert Y axis so oldest (index 0) appears at TOP
                ImPlot::SetupAxes("Microseconds (us)", "", 0, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_Invert);
                
                double barWidth = 0.25;
                
                std::vector<double> positions(numTests);
                for (int i = 0; i < numTests; ++i) positions[i] = static_cast<double>(i);
                
                std::vector<double> minPos(numTests), avgPos(numTests), maxPos(numTests);
                for (int i = 0; i < numTests; ++i) {
                    minPos[i] = positions[i] - barWidth;
                    avgPos[i] = positions[i];
                    maxPos[i] = positions[i] + barWidth;
                }
                
                ImPlot::SetupAxisTicks(ImAxis_Y1, positions.data(), numTests, latLabels.data());
                
                ImPlot::SetNextFillStyle(ImVec4(0.2f, 0.6f, 1.0f, 0.8f));
                ImPlot::PlotBars("Min", mins.data(), minPos.data(), numTests, barWidth * 0.9, ImPlotBarsFlags_Horizontal);
                
                ImPlot::SetNextFillStyle(ImVec4(0.2f, 0.9f, 0.2f, 0.8f));
                ImPlot::PlotBars("Avg", avgs.data(), avgPos.data(), numTests, barWidth * 0.9, ImPlotBarsFlags_Horizontal);
                
                ImPlot::SetNextFillStyle(ImVec4(1.0f, 0.4f, 0.2f, 0.8f));
                ImPlot::PlotBars("Max", maxs.data(), maxPos.data(), numTests, barWidth * 0.9, ImPlotBarsFlags_Horizontal);
                
                ImPlot::EndPlot();
            }
            
            ImGui::TextColored(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), "Blue = Min");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "  Green = Avg");
            ImGui::SameLine();
            if (g_app.config.averageRuns) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), "  Orange = Max");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), "  Orange = Best");
            }
        }

        ImGui::End();
    }

    // ========== Compare to Standards Window ==========
    if (g_app.showCompareWindow && !g_app.results.empty()) {
        ImGui::SetNextWindowSize(ImVec2(900, 700), ImGuiCond_FirstUseEver);
        ImGui::Begin("Compare to Interface Standards", &g_app.showCompareWindow);

        const char* modeLabel = g_app.config.averageRuns ? "Average" : "Best Run";
        ImGui::Text("Your %s Results vs Standard Interface Bandwidths", modeLabel);
        ImGui::Text("(Sorted from fastest to slowest, top to bottom)");
        ImGui::Separator();
        ImGui::Spacing();
        
        if (g_app.possibleEGPU) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 1.0f, 1.0f), "Note: Possible external GPU detected via %s", g_app.eGPUConnectionType.c_str());
            ImGui::Spacing();
        }

        // Build combined list of standards + user results, sorted by bandwidth
        struct BandwidthEntry {
            std::string name;
            double bandwidth;
            int type;  // 0 = standard, 1 = upload, 2 = download, 3 = memory standard
            std::string description;
        };

        std::vector<BandwidthEntry> entries;

        // Choose standards based on GPU type
        bool isIntegrated = g_app.gpuList[g_app.config.selectedGPU].isIntegrated;

        if (isIntegrated) {
            // Integrated GPU: show memory bandwidth standards
            for (int i = 0; i < NUM_MEMORY_STANDARDS; ++i) {
                entries.push_back({
                    MEMORY_STANDARDS[i].name,
                    MEMORY_STANDARDS[i].bandwidth,
                    3,
                    MEMORY_STANDARDS[i].description
                });
            }
        } else {
            // Discrete GPU: show PCIe/TB/USB4 interface standards
            for (int i = 0; i < NUM_INTERFACE_SPEEDS; ++i) {
                entries.push_back({
                    INTERFACE_SPEEDS[i].name,
                    INTERFACE_SPEEDS[i].bandwidth,
                    0,
                    INTERFACE_SPEEDS[i].description
                });
            }
        }
        
        // Add user results
        entries.push_back({
            "YOUR CPU->GPU",
            g_app.uploadBW,
            1,
            "Your measured upload bandwidth"
        });
        
        entries.push_back({
            "YOUR GPU->CPU",
            g_app.downloadBW,
            2,
            "Your measured download bandwidth"
        });
        
        // Sort by bandwidth (highest first = fastest at top)
        std::sort(entries.begin(), entries.end(), [](const BandwidthEntry& a, const BandwidthEntry& b) {
            return a.bandwidth > b.bandwidth;
        });
        
        // Build arrays for plotting
        int numEntries = static_cast<int>(entries.size());
        std::vector<double> bandwidths(numEntries);
        std::vector<double> positions(numEntries);
        std::vector<double> standardBW, uploadBW, downloadBW, memoryBW;
        std::vector<double> standardPos, uploadPos, downloadPos, memoryPos;

        static std::vector<std::string> labelStorage;
        static std::vector<const char*> labels;
        labelStorage.clear();
        labels.clear();

        for (int i = 0; i < numEntries; ++i) {
            positions[i] = static_cast<double>(i);
            bandwidths[i] = entries[i].bandwidth;
            labelStorage.push_back(entries[i].name);

            if (entries[i].type == 0) {
                standardPos.push_back(positions[i]);
                standardBW.push_back(entries[i].bandwidth);
            } else if (entries[i].type == 1) {
                uploadPos.push_back(positions[i]);
                uploadBW.push_back(entries[i].bandwidth);
            } else if (entries[i].type == 2) {
                downloadPos.push_back(positions[i]);
                downloadBW.push_back(entries[i].bandwidth);
            } else if (entries[i].type == 3) {
                memoryPos.push_back(positions[i]);
                memoryBW.push_back(entries[i].bandwidth);
            }
        }
        for (const auto& s : labelStorage) labels.push_back(s.c_str());
        
        // Calculate plot height based on number of entries
        float plotHeight = std::max(350.0f, numEntries * 22.0f + 60.0f);

        // Determine X-axis range based on data
        double maxBW = 70.0;
        for (int i = 0; i < numEntries; ++i) {
            if (entries[i].bandwidth > maxBW) maxBW = entries[i].bandwidth;
        }
        maxBW = maxBW * 1.1;  // 10% padding

        // Draw the horizontal bar chart
        if (ImPlot::BeginPlot("##InterfaceComparison", ImVec2(-1, plotHeight))) {
            ImPlot::SetupAxes("Bandwidth (GB/s)", "", 0, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_Invert);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, maxBW, ImPlotCond_Always);
            ImPlot::SetupAxisTicks(ImAxis_Y1, positions.data(), numEntries, labels.data());

            double barHeight = 0.7;

            // Plot PCIe/TB interface standards in gray
            if (!standardBW.empty()) {
                ImPlot::SetNextFillStyle(ImVec4(0.5f, 0.5f, 0.5f, 0.7f));
                ImPlot::PlotBars("Interface Standards", standardBW.data(), standardPos.data(),
                                  static_cast<int>(standardBW.size()), barHeight, ImPlotBarsFlags_Horizontal);
            }

            // Plot memory bandwidth standards in purple
            if (!memoryBW.empty()) {
                ImPlot::SetNextFillStyle(ImVec4(0.6f, 0.4f, 0.8f, 0.7f));
                ImPlot::PlotBars("Memory Standards", memoryBW.data(), memoryPos.data(),
                                  static_cast<int>(memoryBW.size()), barHeight, ImPlotBarsFlags_Horizontal);
            }

            // Plot user upload in green
            if (!uploadBW.empty()) {
                ImPlot::SetNextFillStyle(ImVec4(0.2f, 0.9f, 0.2f, 1.0f));
                ImPlot::PlotBars("Your CPU->GPU", uploadBW.data(), uploadPos.data(),
                                  static_cast<int>(uploadBW.size()), barHeight, ImPlotBarsFlags_Horizontal);
            }

            // Plot user download in cyan
            if (!downloadBW.empty()) {
                ImPlot::SetNextFillStyle(ImVec4(0.2f, 0.7f, 0.9f, 1.0f));
                ImPlot::PlotBars("Your GPU->CPU", downloadBW.data(), downloadPos.data(),
                                  static_cast<int>(downloadBW.size()), barHeight, ImPlotBarsFlags_Horizontal);
            }

            ImPlot::EndPlot();
        }
        
        // Legend
        ImGui::Spacing();
        const char* modeLbl = g_app.config.averageRuns ? "Avg" : "Best";
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Gray = Interface Standards");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "  Green = Your CPU->GPU %s (%.2f GB/s)", modeLbl, g_app.uploadBW);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.2f, 0.7f, 0.9f, 1.0f), "  Cyan = Your GPU->CPU %s (%.2f GB/s)", modeLbl, g_app.downloadBW);
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // Detailed ranking list
        ImGui::Text("Detailed Ranking (fastest to slowest):");
        ImGui::Spacing();
        
        if (ImGui::BeginTable("RankingTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, 
                              ImVec2(0, 200))) {
            ImGui::TableSetupColumn("Rank", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Interface", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Bandwidth", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            
            for (int i = 0; i < numEntries; ++i) {
                ImGui::TableNextRow();
                
                // Determine row color based on type
                ImVec4 textColor;
                if (entries[i].type == 1) {
                    textColor = ImVec4(0.2f, 0.9f, 0.2f, 1.0f);  // Green for upload
                } else if (entries[i].type == 2) {
                    textColor = ImVec4(0.2f, 0.7f, 0.9f, 1.0f);  // Cyan for download
                } else {
                    textColor = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);  // Light gray for standards
                }
                
                ImGui::TableNextColumn();
                if (entries[i].type != 0) {
                    ImGui::TextColored(textColor, "#%d", i + 1);
                } else {
                    ImGui::Text("#%d", i + 1);
                }
                
                ImGui::TableNextColumn();
                if (entries[i].type != 0) {
                    ImGui::TextColored(textColor, "%s", entries[i].name.c_str());
                } else {
                    ImGui::Text("%s", entries[i].name.c_str());
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", entries[i].description.c_str());
                }
                
                ImGui::TableNextColumn();
                if (entries[i].type != 0) {
                    ImGui::TextColored(textColor, "%.2f GB/s", entries[i].bandwidth);
                } else {
                    ImGui::Text("%.2f GB/s", entries[i].bandwidth);
                }
            }
            
            ImGui::EndTable();
        }
        
        // Summary percentages
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        char uploadSummary[128], downloadSummary[128];
        snprintf(uploadSummary, sizeof(uploadSummary), 
                 "CPU->GPU: %.2f GB/s = %.0f%% of %s",
                 g_app.uploadBW, g_app.uploadPercentage, g_app.closestUploadStandard.c_str());
        snprintf(downloadSummary, sizeof(downloadSummary),
                 "GPU->CPU: %.2f GB/s = %.0f%% of %s", 
                 g_app.downloadBW, g_app.downloadPercentage, g_app.closestDownloadStandard.c_str());
        
        ImVec4 uploadColor = g_app.uploadPercentage >= 90 ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) :
            g_app.uploadPercentage >= 70 ? ImVec4(1.0f, 1.0f, 0.4f, 1.0f) :
            ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        ImVec4 downloadColor = g_app.downloadPercentage >= 90 ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) :
            g_app.downloadPercentage >= 70 ? ImVec4(1.0f, 1.0f, 0.4f, 1.0f) :
            ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        
        ImGui::TextColored(uploadColor, "%s", uploadSummary);
        ImGui::TextColored(downloadColor, "%s", downloadSummary);
        
        if (!g_app.detectedInterface.empty() && g_app.detectedInterface != "Unknown") {
            ImGui::Spacing();
            ImGui::Text("Speed Comparable To:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "%s", g_app.detectedInterface.c_str());
        }

        ImGui::End();
    }

    // ========== VRAM Test Results Window ==========
    if (g_app.showVRAMTestWindow) {
        ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_FirstUseEver);
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        
        if (ImGui::Begin("VRAM Scan Results", &g_app.showVRAMTestWindow)) {
            const auto& result = g_app.vramTestResult;
            
            // Header with status
            if (result.cancelled) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "VRAM Scan: CANCELLED");
            } else if (result.totalErrors == 0) {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "VRAM Scan: PASS");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "VRAM Scan: FAIL");
            }
            
            ImGui::Separator();
            ImGui::Spacing();
            
            // Summary stats
            ImGui::Text("GPU: %s", g_app.gpuList[g_app.config.selectedGPU].name.c_str());
            ImGui::Text("Tested: %s", FormatSize(result.totalBytesTested).c_str());
            
            char durationBuf[64];
            snprintf(durationBuf, sizeof(durationBuf), "Duration: %.1f seconds", result.testDurationSeconds);
            ImGui::Text("%s", durationBuf);
            
            ImGui::Spacing();
            
            if (result.totalErrors > 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), 
                                  "Total Errors: %zu", result.totalErrors);
            } else {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), 
                                  "Total Errors: 0");
            }
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            
            // Pattern results
            ImGui::Text("Pattern Results:");
            ImGui::Spacing();
            
            for (const auto& patternResult : result.patternResults) {
                if (patternResult.find("PASS") != std::string::npos) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "  %s", patternResult.c_str());
                } else if (patternResult.find("FAIL") != std::string::npos) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "  %s", patternResult.c_str());
                } else {
                    ImGui::Text("  %s", patternResult.c_str());
                }
            }
            
            // Error details if any
            if (!result.errors.empty()) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // ----- Error Breakdown -----
                if (ImGui::CollapsingHeader("Error Breakdown", ImGuiTreeNodeFlags_DefaultOpen)) {
                    static const char* KIND_LABELS[6] = {
                        "single-bit", "multi-bit", "address-bus",
                        "stuck-at-0", "stuck-at-1", "refresh"
                    };
                    std::string kindLine = "Error kinds:";
                    bool anyKind = false;
                    for (size_t k = 0; k < result.errorKindCounts.size(); ++k) {
                        if (result.errorKindCounts[k] > 0) {
                            if (anyKind) kindLine += ",";
                            kindLine += " " + std::to_string(result.errorKindCounts[k])
                                      + " " + KIND_LABELS[k];
                            anyKind = true;
                        }
                    }
                    if (!anyKind) kindLine += " (uncategorized)";
                    ImGui::TextWrapped("%s", kindLine.c_str());

                    if (result.refreshPassErrors > 0) {
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                            "%zu errors detected during refresh check (data retention issue)",
                            result.refreshPassErrors);
                    }

                    {
                        std::vector<std::pair<size_t, int>> bitsSorted;
                        for (int b = 0; b < 32; ++b) {
                            if (result.bitFlipHistogram[b] > 0) {
                                bitsSorted.push_back({ result.bitFlipHistogram[b], b });
                            }
                        }
                        std::sort(bitsSorted.begin(), bitsSorted.end(),
                                  [](const auto& a, const auto& b){ return a.first > b.first; });
                        if (!bitsSorted.empty()) {
                            std::string topLine = "Most-flipped bits:";
                            size_t n = std::min<size_t>(3u, bitsSorted.size());
                            for (size_t i = 0; i < n; ++i) {
                                topLine += " bit " + std::to_string(bitsSorted[i].second)
                                         + " (" + std::to_string(bitsSorted[i].first) + ")";
                                if (i + 1 < n) topLine += ",";
                            }
                            ImGui::TextWrapped("%s", topLine.c_str());
                        }
                    }

                    {
                        size_t maxFlips = 0;
                        for (size_t v : result.bitFlipHistogram) if (v > maxFlips) maxFlips = v;
                        if (maxFlips > 0) {
                            ImGui::Spacing();
                            ImGui::Text("Bit position histogram:");
                            float floats[32];
                            for (int b = 0; b < 32; ++b) {
                                floats[b] = (float)result.bitFlipHistogram[b];
                            }
                            ImGui::PlotHistogram("##VRAMBitHist", floats, 32, 0,
                                                 nullptr, 0.0f, (float)maxFlips,
                                                 ImVec2(0, 80));
                        }
                    }
                }

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Error Regions:");
                ImGui::Spacing();

                ImGui::BeginChild("ErrorList", ImVec2(0, 150), true, ImGuiWindowFlags_HorizontalScrollbar);
                for (const auto& err : result.errors) {
                    char errBuf[320];
                    if (err.bitFlipCount > 0) {
                        snprintf(errBuf, sizeof(errBuf),
                                "0x%08zX - 0x%08zX: %zu errors (%s, %s)",
                                err.offsetStart, err.offsetEnd, err.errorCount,
                                GetPatternName(err.pattern).c_str(),
                                GetErrorKindName(err.kind));
                    } else {
                        snprintf(errBuf, sizeof(errBuf),
                                "0x%08zX - 0x%08zX: %zu errors (%s)",
                                err.offsetStart, err.offsetEnd, err.errorCount,
                                GetPatternName(err.pattern).c_str());
                    }
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", errBuf);
                }
                ImGui::EndChild();
            }
            
            // Disclaimer
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
            ImGui::TextWrapped(
                "DISCLAIMER: This is a basic VRAM integrity test using Vulkan. "
                "Error addresses shown are logical offsets in the test buffer, "
                "not physical VRAM addresses. For chip-level diagnosis and precise "
                "fault location, use vendor-specific tools such as:"
            );
            ImGui::Spacing();
            ImGui::BulletText("NVIDIA: MATS (Manufacturing Acceptance Test Software)");
            ImGui::BulletText("AMD: Memory diagnostics in driver utilities");
            ImGui::BulletText("Third-party: OCCT, FurMark (stress testing)");
            ImGui::PopStyleColor();
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            
            // Buttons
            float buttonWidth = 100.0f;
            float totalWidth = buttonWidth * 2 + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalWidth) * 0.5f);
            
            if (ImGui::Button("Copy", ImVec2(buttonWidth, 30))) {
                std::ostringstream ss;
                ss << "=== VRAM Scan Results ===\n";
                ss << "GPU: " << g_app.gpuList[g_app.config.selectedGPU].name << "\n";
                ss << "Tested: " << FormatSize(result.totalBytesTested) << "\n";
                ss << "Duration: " << std::fixed << std::setprecision(1) << result.testDurationSeconds << " seconds\n";
                ss << "Total Errors: " << result.totalErrors << "\n\n";
                ss << "Pattern Results:\n";
                for (const auto& pr : result.patternResults) {
                    ss << "  " << pr << "\n";
                }
                if (!result.errors.empty()) {
                    ss << "\nError Regions:\n";
                    for (const auto& err : result.errors) {
                        char errBuf[256];
                        snprintf(errBuf, sizeof(errBuf), 
                                "  0x%08zX - 0x%08zX: %zu errors (%s)\n",
                                err.offsetStart, err.offsetEnd, err.errorCount,
                                GetPatternName(err.pattern).c_str());
                        ss << errBuf;
                    }
                }
                ss << "\nNote: Use vendor tools (NVIDIA MATS, etc.) for chip-level diagnosis.\n";
                
                ImGui::SetClipboardText(ss.str().c_str());
                Log("[INFO] VRAM test results copied to clipboard");
            }
            
            ImGui::SameLine();
            
            if (ImGui::Button("Close", ImVec2(buttonWidth, 30))) {
                g_app.showVRAMTestWindow = false;
            }
        }
        ImGui::End();
    }

    // ========== About Dialog ==========
    if (g_app.showAboutDialog) {
        ImGui::OpenPopup("About GPU-PCIe-Test");
        g_app.showAboutDialog = false;  // Only open once
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("About GPU-PCIe-Test", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("GPU-PCIe-Test v3.4 GUI Edition (Vulkan)");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("A tool to benchmark GPU/PCIe bandwidth and latency.");
        ImGui::Text("Measures data transfer speeds between CPU and GPU.");
        ImGui::Text("Graphics API: Vulkan");
        ImGui::Text("Test Path: Dedicated copy/transfer queue (DMA engine)");
        ImGui::Spacing();
        ImGui::Text("Features:");
        ImGui::BulletText("Upload/Download bandwidth tests");
        ImGui::BulletText("Bidirectional bandwidth test");
        ImGui::BulletText("Latency measurements");
        ImGui::BulletText("VRAM integrity scanning");
        ImGui::BulletText("Interface detection (PCIe/TB/USB4/OCuLink)");
        ImGui::BulletText("eGPU auto-detection");
        ImGui::BulletText("System RAM detection");
        ImGui::BulletText("VRAM-aware buffer sizing");
        ImGui::BulletText("Average or individual run recording");
        ImGui::BulletText("Min/Avg/Max graphs");
        ImGui::BulletText("Ranked comparison to standards");
        ImGui::BulletText("CSV export");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("Author: David Janice");
        ImGui::Text("Email: djanice1980@gmail.com");
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "https://github.com/djanice1980/GPU-PCIe-Test");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float buttonWidth = 120.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - buttonWidth) * 0.5f);
        if (ImGui::Button("Close", ImVec2(buttonWidth, 30))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// ============================================================================
//                         RENDERING LOOP
// ============================================================================

void WaitForGPU() {
    if (g_app.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_app.device);
    }
}

void Render() {
    uint32_t frameIdx = g_app.frameIndex;

    // Wait for this frame's fence
    vkWaitForFences(g_app.device, 1, &g_app.inFlightFences[frameIdx], VK_TRUE, UINT64_MAX);

    // Acquire next image
    VkResult acquireResult = vkAcquireNextImageKHR(g_app.device, g_app.swapChain, UINT64_MAX,
        g_app.imageAvailableSemaphores[frameIdx], VK_NULL_HANDLE, &g_app.imageIndex);
    
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        // Swapchain needs recreation - schedule it explicitly rather than
        // hoping a framebuffer-size callback arrives (DPI/monitor changes can
        // produce OUT_OF_DATE without one, which previously stalled rendering).
        g_app.pendingWidth = g_app.windowWidth;
        g_app.pendingHeight = g_app.windowHeight;
        g_app.pendingResize = true;
        return;
    }

    vkResetFences(g_app.device, 1, &g_app.inFlightFences[frameIdx]);

    // Record command buffer
    VkCommandBuffer cmd = g_app.commandBuffers[frameIdx];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Begin render pass
    VkClearValue clearColor = {};
    clearColor.color = {{0.1f, 0.1f, 0.12f, 1.0f}};

    VkRenderPassBeginInfo rpInfo = {};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = g_app.renderPass;
    rpInfo.framebuffer = g_app.swapChainFramebuffers[g_app.imageIndex];
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = g_app.swapChainExtent;
    rpInfo.clearValueCount = 1;
    rpInfo.pClearValues = &clearColor;
    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Set viewport and scissor
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)g_app.swapChainExtent.width;
    viewport.height = (float)g_app.swapChainExtent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = g_app.swapChainExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Render ImGui
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    // Submit
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &g_app.imageAvailableSemaphores[frameIdx];
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &g_app.renderFinishedSemaphores[g_app.imageIndex];

    vkQueueSubmit(g_app.graphicsQueue, 1, &submitInfo, g_app.inFlightFences[frameIdx]);

    // Present
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &g_app.renderFinishedSemaphores[g_app.imageIndex];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &g_app.swapChain;
    presentInfo.pImageIndices = &g_app.imageIndex;

    VkResult presentResult = vkQueuePresentKHR(g_app.graphicsQueue, &presentInfo);
    
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        // Schedule swapchain recreation on the next loop iteration (the
        // pendingResize handler rebuilds it) instead of waiting for a
        // framebuffer-size callback.
        g_app.pendingWidth = g_app.windowWidth;
        g_app.pendingHeight = g_app.windowHeight;
        g_app.pendingResize = true;
    }

    g_app.frameIndex = (g_app.frameIndex + 1) % Constants::NUM_FRAMES_IN_FLIGHT;
}

void ResizeSwapChain(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (g_app.device == VK_NULL_HANDLE) return;

    WaitForGPU();
    DestroyRenderFinishedSemaphores();

    // Cleanup old swapchain resources
    for (auto fb : g_app.swapChainFramebuffers) {
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(g_app.device, fb, nullptr);
    }
    g_app.swapChainFramebuffers.clear();

    for (auto iv : g_app.swapChainImageViews) {
        if (iv != VK_NULL_HANDLE) vkDestroyImageView(g_app.device, iv, nullptr);
    }
    g_app.swapChainImageViews.clear();

    // Save old swapchain for reuse
    VkSwapchainKHR oldSwapChain = g_app.swapChain;

    // Get updated capabilities
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_app.renderPhysicalDevice, g_app.surface, &capabilities);

    VkExtent2D extent;
    if (capabilities.currentExtent.width != UINT32_MAX) {
        extent = capabilities.currentExtent;
    } else {
        extent.width = std::clamp((uint32_t)width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp((uint32_t)height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    g_app.swapChainExtent = extent;

    uint32_t imageCount = std::max(capabilities.minImageCount, (uint32_t)Constants::NUM_FRAMES_IN_FLIGHT);
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    // Create new swapchain
    VkSwapchainCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = g_app.surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = g_app.swapChainFormat;
    createInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = oldSwapChain;

    VkResult result = vkCreateSwapchainKHR(g_app.device, &createInfo, nullptr, &g_app.swapChain);
    
    // Destroy old swapchain
    if (oldSwapChain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(g_app.device, oldSwapChain, nullptr);
    }
    
    if (result != VK_SUCCESS) {
        Log("[ERROR] Failed to recreate swapchain: " + std::to_string((int)result));
        return;
    }

    // Get new swapchain images
    uint32_t swapImageCount;
    vkGetSwapchainImagesKHR(g_app.device, g_app.swapChain, &swapImageCount, nullptr);
    g_app.swapChainImages.resize(swapImageCount);
    vkGetSwapchainImagesKHR(g_app.device, g_app.swapChain, &swapImageCount, g_app.swapChainImages.data());

    // Recreate image views
    g_app.swapChainImageViews.resize(swapImageCount);
    for (uint32_t i = 0; i < swapImageCount; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = g_app.swapChainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = g_app.swapChainFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        vkCreateImageView(g_app.device, &viewInfo, nullptr, &g_app.swapChainImageViews[i]);
    }

    if (!CreateRenderFinishedSemaphores()) {
        Log("[ERROR] Failed to recreate render-finished semaphores");
        return;
    }

    // Recreate framebuffers
    CreateFramebuffers();

    g_app.windowWidth = width;
    g_app.windowHeight = height;
    g_app.frameIndex = 0;
}

// ============================================================================
//                        GLFW CALLBACKS
// ============================================================================

static void GlfwFramebufferSizeCallback(GLFWwindow* window, int width, int height) {
    if (width <= 0 || height <= 0) return;
    g_app.pendingWidth = width;
    g_app.pendingHeight = height;
    g_app.pendingResize = true;
}

static void GlfwErrorCallback(int error, const char* description) {
    fprintf(stderr, "[GLFW Error %d] %s\n", error, description);
}

// ============================================================================
//                              MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    // Initialize GLFW
    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    // Check Vulkan support
    if (!glfwVulkanSupported()) {
        fprintf(stderr, "GLFW: Vulkan not supported! Please install Vulkan drivers.\n");
        glfwTerminate();
        return 1;
    }

    // Create GLFW window (no OpenGL context - we use Vulkan)
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    g_app.window = glfwCreateWindow(
        Constants::WINDOW_WIDTH, Constants::WINDOW_HEIGHT,
        "GPU-PCIe-Test v3.4 GUI (Vulkan - Linux)", nullptr, nullptr
    );

    if (!g_app.window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }

    // Set callbacks
    glfwSetFramebufferSizeCallback(g_app.window, GlfwFramebufferSizeCallback);

    // Get actual framebuffer size
    glfwGetFramebufferSize(g_app.window, &g_app.windowWidth, &g_app.windowHeight);

    // Initialize Vulkan
    if (!InitVulkan()) {
        fprintf(stderr, "Failed to initialize Vulkan. Please ensure Vulkan drivers are installed.\n");
        glfwDestroyWindow(g_app.window);
        glfwTerminate();
        return 1;
    }

    // Enumerate GPUs
    EnumerateGPUs();

    // Detect system memory early
    g_app.systemMemory = DetectSystemMemory();
    EstimateRatedLatency(g_app.systemMemory);

    // Prepare GPU combo
    g_app.gpuComboNames.clear();
    g_app.gpuComboPointers.clear();
    for (const auto& gpu : g_app.gpuList) {
        std::string label;
        if (gpu.isValid) {
            label = gpu.vendor + " " + gpu.name +
                " (" + FormatMemory(gpu.dedicatedVRAM) +
                (gpu.isIntegrated ? " iGPU" : "") + ")";
        } else {
            label = gpu.name;
        }
        g_app.gpuComboNames.push_back(std::move(label));
        g_app.gpuComboPointers.push_back(g_app.gpuComboNames.back().c_str());
    }

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    // Initialize GLFW backend
    ImGui_ImplGlfw_InitForVulkan(g_app.window, true);

    // Get DPI scale from GLFW
    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(g_app.window, &xscale, &yscale);
    float dpiScale = std::max(xscale, yscale);
    io.FontGlobalScale = Constants::BASE_FONT_SCALE * dpiScale;

    io.DisplaySize = ImVec2((float)g_app.windowWidth, (float)g_app.windowHeight);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    ImGui::StyleColorsDark();

    // Custom style
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.15f, 0.15f, 0.18f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.1f, 0.1f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.2f, 0.2f, 0.25f, 1.0f);

    // Initialize Vulkan ImGui backend
    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.Instance = g_app.instance;
    initInfo.PhysicalDevice = g_app.renderPhysicalDevice;
    initInfo.Device = g_app.device;
    initInfo.QueueFamily = g_app.graphicsQueueFamily;
    initInfo.Queue = g_app.graphicsQueue;
    initInfo.DescriptorPool = g_app.imguiDescriptorPool;
    initInfo.MinImageCount = Constants::NUM_FRAMES_IN_FLIGHT;
    initInfo.ImageCount = static_cast<uint32_t>(g_app.swapChainImages.size());
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.RenderPass = g_app.renderPass;
    initInfo.Subpass = 0;
    ImGui_ImplVulkan_Init(&initInfo);

    // Main loop
    while (!glfwWindowShouldClose(g_app.window)) {
        glfwPollEvents();

        // Handle deferred resize
        if (g_app.pendingResize) {
            g_app.pendingResize = false;
            if (g_app.pendingWidth > 0 && g_app.pendingHeight > 0) {
                ResizeSwapChain(g_app.pendingWidth, g_app.pendingHeight);
            }
        }

        if (g_app.windowWidth <= 0 || g_app.windowHeight <= 0) {
            continue;
        }

        // Start ImGui frame
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Update display size each frame
        ImGuiIO& frameIO = ImGui::GetIO();
        frameIO.DisplaySize = ImVec2((float)g_app.windowWidth, (float)g_app.windowHeight);

        // Render GUI
        RenderGUI();

        // Render
        ImGui::Render();
        Render();
    }

    // Cleanup - request cancellation and wait for benchmark thread
    bool workerHung = false;
    g_app.cancelRequested = true;
    if (g_app.benchmarkThread.joinable()) {
        bool threadStopped = false;
        for (int i = 0; i < 50 && g_app.benchmarkThreadRunning; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        threadStopped = !g_app.benchmarkThreadRunning;

        if (threadStopped) {
            g_app.benchmarkThread.join();
        } else {
            g_app.benchmarkThread.detach();
            workerHung = true;
        }
    }

    // Cleanup VRAM test thread
    g_app.vramTestCancelRequested = true;
    if (g_app.vramTestThread.joinable()) {
        bool threadStopped = false;
        for (int i = 0; i < 50 && g_app.vramTestRunning; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        threadStopped = !g_app.vramTestRunning;

        if (threadStopped) {
            g_app.vramTestThread.join();
        } else {
            g_app.vramTestThread.detach();
            workerHung = true;
        }
    }

    if (workerHung) {
        // A detached worker is still live and using g_app's Vulkan objects.
        // Tearing down the devices/ImGui underneath it is a use-after-free, so
        // exit the process immediately instead - the OS reclaims everything.
        _exit(0);
    }

    WaitForGPU();

    // ImGui cleanup
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    // Vulkan cleanup
    DestroyRenderFinishedSemaphores();
    for (int i = 0; i < Constants::NUM_FRAMES_IN_FLIGHT; i++) {
        if (g_app.imageAvailableSemaphores[i] != VK_NULL_HANDLE) vkDestroySemaphore(g_app.device, g_app.imageAvailableSemaphores[i], nullptr);
        if (g_app.inFlightFences[i] != VK_NULL_HANDLE) vkDestroyFence(g_app.device, g_app.inFlightFences[i], nullptr);
    }

    if (g_app.commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(g_app.device, g_app.commandPool, nullptr);
    if (g_app.imguiDescriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(g_app.device, g_app.imguiDescriptorPool, nullptr);

    for (auto fb : g_app.swapChainFramebuffers) {
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(g_app.device, fb, nullptr);
    }
    for (auto iv : g_app.swapChainImageViews) {
        if (iv != VK_NULL_HANDLE) vkDestroyImageView(g_app.device, iv, nullptr);
    }

    if (g_app.renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(g_app.device, g_app.renderPass, nullptr);
    if (g_app.swapChain != VK_NULL_HANDLE) vkDestroySwapchainKHR(g_app.device, g_app.swapChain, nullptr);
    if (g_app.device != VK_NULL_HANDLE) vkDestroyDevice(g_app.device, nullptr);
    if (g_app.surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(g_app.instance, g_app.surface, nullptr);
#ifdef ENABLE_VULKAN_VALIDATION
    DestroyDebugMessenger(g_app.instance, g_app.debugMessenger);
#endif
    if (g_app.instance != VK_NULL_HANDLE) vkDestroyInstance(g_app.instance, nullptr);

    glfwDestroyWindow(g_app.window);
    glfwTerminate();

    return 0;
}

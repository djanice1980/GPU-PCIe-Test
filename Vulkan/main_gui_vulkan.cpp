// ============================================================================
// GPU-PCIe-Test v3.0 - GUI Edition (Vulkan)
// Dear ImGui + Vulkan Frontend
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
// D3D12 BACKPORT NOTES
// ─────────────────────────────────────────────────────────────────────────────
// To make D3D12 use the same methodology as this Vulkan version:
//   1. Change benchQueue from COMMAND_LIST_TYPE_DIRECT to COMMAND_LIST_TYPE_COPY
//   2. Create a second COPY queue + allocator + command list for bidirectional
//   3. Bidirectional: submit upload to COPY queue 1, download to COPY queue 2,
//      wait on both fences. (Currently uses DIRECT queue auto-routing which
//      gives higher numbers due to internal driver optimization.)
//   4. All bandwidth/latency tests already use CopyResource which works on COPY
//      queues - no changes needed to the copy commands themselves.
//   5. The round-trip upload method is API-agnostic, works identically.
//   6. Expected result: D3D12 numbers should converge with Vulkan numbers
//      since both will test through the same DMA copy engine hardware path.
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
// - Actual PCIe link detection via SetupAPI
// - System RAM detection via WMI (speed, channels, type)
// - Full Unicode support (UTF-8 internal)
// ============================================================================

// Uncomment to enable debug logging for external GPU detection
// #define DEBUG_EXTERNAL_DETECTION

#ifndef NOMINMAX
#define NOMINMAX
#endif

#define _WIN32_DCOM  // Required for CoInitializeEx
#define VK_USE_PLATFORM_WIN32_KHR

#include <windows.h>
#include <vulkan/vulkan.h>
#include <setupapi.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpropdef.h>
#include <comdef.h>
#include <wbemidl.h>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>
#include <chrono>
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
#include <functional>
#include <optional>
#include <cassert>

#pragma comment(lib, "vulkan-1.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// ImGui headers (must be in imgui/ subfolder)
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_vulkan.h"
#include "imgui/implot.h"
#include "imgui/imgui_internal.h"

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
    
    constexpr DWORD FENCE_WAIT_TIMEOUT_MS = 8000;
    constexpr int MAX_FENCE_RETRIES = 3;
    constexpr DWORD GLOBAL_BENCHMARK_TIMEOUT_MS = 300000;
    
    constexpr double VRAM_SAFETY_MARGIN = 0.8;
    constexpr size_t MIN_BANDWIDTH_SIZE = 16ull * 1024 * 1024;
    
    constexpr double EGPU_BANDWIDTH_THRESHOLD = 5.0;
    constexpr double TB3_MAX_BANDWIDTH = 3.5;
    constexpr double TB4_MAX_BANDWIDTH = 4.5;
}

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

// System RAM information (detected via WMI)
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

struct VRAMError {
    size_t offsetStart = 0;
    size_t offsetEnd = 0;
    uint32_t expected = 0;
    uint32_t actual = 0;
    VRAMTestPattern pattern;
    size_t errorCount = 0;
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

struct BenchmarkConfig {
    size_t bandwidthSize = Constants::DEFAULT_BANDWIDTH_SIZE;
    size_t latencySize = Constants::DEFAULT_LATENCY_SIZE;
    int    bandwidthBatches = Constants::DEFAULT_BANDWIDTH_BATCHES;
    int    copiesPerBatch = Constants::DEFAULT_COPIES_PER_BATCH;
    int    latencyIters = Constants::DEFAULT_LATENCY_ITERS;
    int    numRuns = Constants::DEFAULT_NUM_RUNS;
    bool   runBidirectional = true;
    bool   runLatency = true;
    bool   quickMode = false;
    bool   averageRuns = true;
    int    selectedGPU = 0;
};

struct InterfaceSpeed {
    const char* name;
    double      bandwidth;
    double      theoretical;
    const char* description;
};

static const InterfaceSpeed INTERFACE_SPEEDS[] = {
    {"PCIe 3.0 x4",    3.40,   3.94,  "Entry-level GPU slot"},
    {"PCIe 3.0 x8",    6.80,   7.88,  "Mid-range GPU slot"},
    {"PCIe 3.0 x16",   13.60,  15.75, "Standard discrete GPU"},
    {"PCIe 4.0 x4",    6.80,   7.88,  "NVMe / Entry eGPU"},
    {"PCIe 4.0 x8",    13.60,  15.75, "Mid-range PCIe 4.0"},
    {"PCIe 4.0 x16",   27.20,  31.51, "High-end discrete GPU"},
    {"PCIe 5.0 x8",    27.20,  31.51, "PCIe 5.0 mid-range"},
    {"PCIe 5.0 x16",   54.40,  63.02, "High-end PCIe 5.0 GPU slot"},
    {"PCIe 6.0 x16",   108.80, 126.03, "Next-gen PCIe 6.0 GPU slot"},
    {"OCuLink 1.0",    3.40,   3.94,  "PCIe 3.0 x4 external"},
    {"OCuLink 2.0",    6.80,   7.88,  "PCIe 4.0 x4 external"},
    {"Thunderbolt 3",  2.50,   2.80,  "40 Gbps (variable PCIe allocation)"},
    {"Thunderbolt 4",  3.00,   3.50,  "40 Gbps (guaranteed PCIe bandwidth)"},
    {"Thunderbolt 5",  10.00,  12.00, "80 Gbps bi-directional / 120 Gbps boost"},
    {"USB4 40Gbps",    4.00,   5.00,  "40 Gbps external"},
    {"USB4 80Gbps",    8.00,   10.00, "80 Gbps external"},
};

static const int NUM_INTERFACE_SPEEDS = sizeof(INTERFACE_SPEEDS) / sizeof(INTERFACE_SPEEDS[0]);

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
    // Window
    HWND hwnd = nullptr;
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
    VkSemaphore                renderFinishedSemaphores[Constants::NUM_FRAMES_IN_FLIGHT] = {};
    VkFence                    inFlightFences[Constants::NUM_FRAMES_IN_FLIGHT] = {};
    VkDescriptorPool           imguiDescriptorPool = VK_NULL_HANDLE;
    uint32_t                   frameIndex = 0;
    uint32_t                   imageIndex = 0;

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
    bool vramTestFullScan = false;
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

// ============================================================================
// UNICODE HELPER FUNCTIONS (Full UTF-8 Support)
// ============================================================================

// Convert wide string (Windows native) to UTF-8 string
std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), 
                                    nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string utf8(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), 
                        &utf8[0], size, nullptr, nullptr);
    return utf8;
}

// Convert UTF-8 string to wide string (Windows native)
std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), 
                                   nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring wide(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), 
                        &wide[0], size);
    return wide;
}

// Convert BSTR (COM) to UTF-8 string
std::string BstrToUtf8(BSTR bstr) {
    if (!bstr) return std::string();
    int len = SysStringLen(bstr);
    if (len == 0) return std::string();
    return WideToUtf8(std::wstring(bstr, len));
}

// ============================================================================
// SYSTEM MEMORY DETECTION (WMI)
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
        default: return "Unknown";
    }
}

// Form factor mapping
std::string GetFormFactorName(uint16_t formFactor) {
    switch (formFactor) {
        case 8:  return "DIMM";
        case 12: return "SODIMM";
        case 13: return "SRIMM";
        case 14: return "FB-DIMM";
        default: return "Unknown";
    }
}

// Detect system memory configuration via WMI
SystemMemoryInfo DetectSystemMemory() {
    SystemMemoryInfo info;
    
    HRESULT hr = CoInitializeEx(0, COINIT_MULTITHREADED);
    // Track if WE initialized COM (S_OK or S_FALSE means we added a reference)
    // RPC_E_CHANGED_MODE means COM was already init'd with different mode - we didn't init
    bool weInitializedCom = (hr == S_OK || hr == S_FALSE);
    bool comAvailable = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    
    if (!comAvailable) {
        info.errorMessage = "Failed to initialize COM";
        return info;
    }
    
    // Set security levels
    hr = CoInitializeSecurity(
        nullptr, -1, nullptr, nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE, nullptr
    );
    // Ignore security errors if already initialized
    
    IWbemLocator* pLoc = nullptr;
    hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, reinterpret_cast<void**>(&pLoc));
    if (FAILED(hr)) {
        info.errorMessage = "Failed to create WbemLocator";
        if (weInitializedCom) CoUninitialize();
        return info;
    }
    
    IWbemServices* pSvc = nullptr;
    hr = pLoc->ConnectServer(
        _bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr, 0, 0, 0, 0, &pSvc
    );
    if (FAILED(hr)) {
        info.errorMessage = "Failed to connect to WMI";
        pLoc->Release();
        if (weInitializedCom) CoUninitialize();
        return info;
    }
    
    // Set proxy security
    hr = CoSetProxyBlanket(
        pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE
    );
    
    // Query physical memory
    IEnumWbemClassObject* pEnumerator = nullptr;
    hr = pSvc->ExecQuery(
        _bstr_t(L"WQL"),
        _bstr_t(L"SELECT * FROM Win32_PhysicalMemory"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr, &pEnumerator
    );
    
    if (FAILED(hr)) {
        info.errorMessage = "WMI query failed";
        pSvc->Release();
        pLoc->Release();
        if (weInitializedCom) CoUninitialize();
        return info;
    }
    
    // Process results
    IWbemClassObject* pclsObj = nullptr;
    ULONG uReturn = 0;
    uint32_t maxSpeed = 0;
    uint32_t maxConfiguredSpeed = 0;
    uint64_t totalCapacity = 0;
    std::string detectedType;
    std::string detectedFormFactor;
    uint16_t detectedMemoryType = 0;  // SMBIOS memory type for DDR generation detection
    int stickCount = 0;
    std::set<std::string> uniqueBanks;  // To count channels
    
    while (pEnumerator) {
        hr = pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn);
        if (uReturn == 0) break;
        
        VARIANT vtProp;
        
        // Get speed (rated)
        hr = pclsObj->Get(L"Speed", 0, &vtProp, 0, 0);
        if (SUCCEEDED(hr) && vtProp.vt == VT_I4) {
            if (static_cast<uint32_t>(vtProp.lVal) > maxSpeed) {
                maxSpeed = static_cast<uint32_t>(vtProp.lVal);
            }
        }
        VariantClear(&vtProp);
        
        // Get configured clock speed (actual running speed)
        hr = pclsObj->Get(L"ConfiguredClockSpeed", 0, &vtProp, 0, 0);
        if (SUCCEEDED(hr) && vtProp.vt == VT_I4) {
            if (static_cast<uint32_t>(vtProp.lVal) > maxConfiguredSpeed) {
                maxConfiguredSpeed = static_cast<uint32_t>(vtProp.lVal);
            }
        }
        VariantClear(&vtProp);
        
        // Get capacity
        hr = pclsObj->Get(L"Capacity", 0, &vtProp, 0, 0);
        if (SUCCEEDED(hr) && vtProp.vt == VT_BSTR) {
            totalCapacity += _wtoi64(vtProp.bstrVal);
        }
        VariantClear(&vtProp);
        
        // Get memory type (SMBIOSMemoryType)
        hr = pclsObj->Get(L"SMBIOSMemoryType", 0, &vtProp, 0, 0);
        if (SUCCEEDED(hr) && vtProp.vt == VT_I4) {
            detectedMemoryType = static_cast<uint16_t>(vtProp.lVal);
            detectedType = GetDDRTypeFromSMBIOS(detectedMemoryType);
        }
        VariantClear(&vtProp);
        
        // Get form factor
        hr = pclsObj->Get(L"FormFactor", 0, &vtProp, 0, 0);
        if (SUCCEEDED(hr) && vtProp.vt == VT_I4) {
            detectedFormFactor = GetFormFactorName(static_cast<uint16_t>(vtProp.lVal));
        }
        VariantClear(&vtProp);
        
        // Get bank label (for channel counting)
        hr = pclsObj->Get(L"BankLabel", 0, &vtProp, 0, 0);
        if (SUCCEEDED(hr) && vtProp.vt == VT_BSTR) {
            uniqueBanks.insert(BstrToUtf8(vtProp.bstrVal));
        }
        VariantClear(&vtProp);
        
        stickCount++;
        pclsObj->Release();
    }
    
    pEnumerator->Release();
    pSvc->Release();
    pLoc->Release();
    if (weInitializedCom) CoUninitialize();
    
    // Fill in the info
    // DDR5 and LPDDR5 (SMBIOS types 34 and 35) report speed directly in MT/s
    // DDR4 and earlier report speed in MHz, which needs to be doubled for MT/s
    bool isDDR5orLater = (detectedMemoryType == 34 || detectedMemoryType == 35);
    
    if (isDDR5orLater) {
        // DDR5/LPDDR5: WMI already reports MT/s
        info.speedMT = maxSpeed;
        info.configuredSpeedMT = maxConfiguredSpeed;
    } else {
        // DDR4 and earlier: WMI reports MHz, double for MT/s
        info.speedMT = maxSpeed * 2;
        info.configuredSpeedMT = maxConfiguredSpeed * 2;
    }
    
    info.totalSticks = static_cast<uint32_t>(stickCount);
    info.totalCapacityGB = totalCapacity / (1024 * 1024 * 1024);
    info.type = detectedType;
    info.formFactor = detectedFormFactor;
    
    // Estimate channels from unique bank labels or stick count
    // This is approximate - some systems report banks differently
    if (uniqueBanks.size() >= 4u) {
        info.channels = 4;  // Quad channel
    } else if (uniqueBanks.size() >= 2u || stickCount >= 2) {
        info.channels = 2;  // Dual channel (typical)
    } else {
        info.channels = 1;  // Single channel
    }
    
    // Calculate theoretical bandwidth
    // DDR bandwidth = speed (MT/s) * 8 bytes * channels / 1000 = GB/s
    if (info.configuredSpeedMT > 0) {
        info.theoreticalBandwidth = (info.configuredSpeedMT * 8.0 * info.channels) / 1000.0;
    } else if (info.speedMT > 0) {
        info.theoreticalBandwidth = (info.speedMT * 8.0 * info.channels) / 1000.0;
    }
    
    info.detected = (stickCount > 0);
    return info;
}

// Format system memory info as a string for logging
std::string FormatSystemMemoryInfo(const SystemMemoryInfo& mem) {
    if (!mem.detected) {
        return "System Memory: Detection failed (" + mem.errorMessage + ")";
    }
    
    std::ostringstream oss;
    oss << "System Memory: ";
    oss << mem.totalCapacityGB << "GB " << mem.type;
    
    if (mem.configuredSpeedMT > 0) {
        oss << " @ " << mem.configuredSpeedMT << " MT/s";
        if (mem.speedMT > 0 && mem.speedMT != mem.configuredSpeedMT) {
            oss << " (rated " << mem.speedMT << " MT/s)";
        }
    } else if (mem.speedMT > 0) {
        oss << " @ " << mem.speedMT << " MT/s";
    }
    
    oss << ", " << mem.totalSticks << " stick" << (mem.totalSticks != 1 ? "s" : "");
    
    if (mem.channels > 0) {
        oss << ", ";
        if (mem.channels == 1) oss << "single";
        else if (mem.channels == 2) oss << "dual";
        else if (mem.channels == 4) oss << "quad";
        else oss << mem.channels;
        oss << "-channel";
    }
    
    if (mem.theoreticalBandwidth > 0) {
        oss << " (~" << std::fixed << std::setprecision(1) << mem.theoreticalBandwidth << " GB/s theoretical)";
    }
    
    return oss.str();
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
void FindClosestInterface(double measured, std::string& outName, double& outPercentage) {
    const InterfaceSpeed* best = nullptr;
    double bestDiff = 1e9;

    for (int i = 0; i < NUM_INTERFACE_SPEEDS; i++) {
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
        // Estimate DDR generation from bandwidth
        // DDR4-3200 dual channel: ~51 GB/s theoretical, ~40 GB/s real
        // DDR5-5600 dual channel: ~89 GB/s theoretical, ~70 GB/s real
        // DDR5-6400 dual channel: ~102 GB/s theoretical, ~80 GB/s real
        if (maxBandwidth > 60) {
            memoryType = "DDR5";
        } else if (maxBandwidth > 35) {
            memoryType = "DDR4/DDR5";
        } else {
            memoryType = "DDR4";
        }
    } else if (gpu.vendor == "Intel") {
        fabricType = "Intel Ring Bus / Mesh";
        if (maxBandwidth > 50) {
            memoryType = "DDR5";
        } else {
            memoryType = "DDR4";
        }
    } else {
        fabricType = "On-die Interconnect";
        memoryType = "System Memory";
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
        char buf[64];
        snprintf(buf, sizeof(buf), "%s @ %u MT/s (%.0f GB/s)", 
                g_app.systemMemory.type.c_str(),
                g_app.systemMemory.configuredSpeedMT > 0 ? 
                    g_app.systemMemory.configuredSpeedMT : g_app.systemMemory.speedMT,
                expectedBandwidth);
        bandwidthSource = buf;
    } else {
        // Fall back to estimates
        // DDR5 dual-channel realistic: ~70 GB/s
        // DDR4 dual-channel realistic: ~40 GB/s
        expectedBandwidth = (memoryType.find("DDR5") != std::string::npos) ? 70.0 : 40.0;
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
    const InterfaceSpeed* best = nullptr;
    double bestDiff = 1e9;

    for (int i = 0; i < NUM_INTERFACE_SPEEDS; i++) {
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

    g_app.uploadBW = upload;
    g_app.downloadBW = download;

    // Calculate percentages vs closest standards
    FindClosestInterface(upload, g_app.closestUploadStandard, g_app.uploadPercentage);
    FindClosestInterface(download, g_app.closestDownloadStandard, g_app.downloadPercentage);
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
            g_app.eGPUConnectionType = "Thunderbolt 4 / USB4";
        } else {
            g_app.eGPUConnectionType = "External (OCuLink / USB4 80Gbps)";
        }
        
        Log("[INFO] eGPU detected - bandwidth indicates " + g_app.eGPUConnectionType);
    }
}

// ============================================================================
// PCIe LINK DETECTION
// ============================================================================

// Define PCIe DEVPKEYs if not available in SDK
// These are the property keys for querying PCIe link status
DEFINE_DEVPROPKEY(DEVPKEY_PciDevice_CurrentLinkSpeed, 0x3ab22e31, 0x8264, 0x4b4e, 0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62, 9);
DEFINE_DEVPROPKEY(DEVPKEY_PciDevice_CurrentLinkWidth, 0x3ab22e31, 0x8264, 0x4b4e, 0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62, 10);
DEFINE_DEVPROPKEY(DEVPKEY_PciDevice_MaxLinkSpeed, 0x3ab22e31, 0x8264, 0x4b4e, 0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62, 11);
DEFINE_DEVPROPKEY(DEVPKEY_PciDevice_MaxLinkWidth, 0x3ab22e31, 0x8264, 0x4b4e, 0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62, 12);

// Query PCIe link information for a GPU using SetupAPI
bool DetectPCIeLink(uint32_t vendorId, uint32_t deviceId, GPUInfo& outInfo) {
    outInfo.pcieInfoValid = false;
    
    // Create device info set for display adapters
    HDEVINFO deviceInfoSet = SetupDiGetClassDevsW(
        nullptr,
        L"PCI",  // Enumerate PCI devices
        nullptr,
        DIGCF_PRESENT | DIGCF_ALLCLASSES
    );
    
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        Log("[DEBUG] SetupDiGetClassDevs failed");
        return false;
    }
    
    SP_DEVINFO_DATA deviceInfoData;
    deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    
    // Iterate through all PCI devices to find our GPU
    for (DWORD i = 0; SetupDiEnumDeviceInfo(deviceInfoSet, i, &deviceInfoData); i++) {
        // Get hardware ID to match vendor/device
        WCHAR hardwareId[512] = {0};
        DWORD requiredSize = 0;
        
        if (!SetupDiGetDeviceRegistryPropertyW(
                deviceInfoSet, &deviceInfoData,
                SPDRP_HARDWAREID, nullptr,
                (PBYTE)hardwareId, sizeof(hardwareId), &requiredSize)) {
            continue;
        }
        
        // Parse vendor and device ID from hardware ID string
        // Format: PCI\VEN_XXXX&DEV_XXXX&...
        WCHAR searchVen[32], searchDev[32];
        swprintf_s(searchVen, L"VEN_%04X", vendorId);
        swprintf_s(searchDev, L"DEV_%04X", deviceId);
        
        std::wstring hwIdStr(hardwareId);
        if (hwIdStr.find(searchVen) == std::wstring::npos ||
            hwIdStr.find(searchDev) == std::wstring::npos) {
            continue;
        }
        
        // Found our device - query PCIe properties
        DEVPROPTYPE propType;
        DWORD propSize;
        
        // Current Link Speed (returns PCIe generation)
        UINT32 currentSpeed = 0;
        if (SetupDiGetDevicePropertyW(deviceInfoSet, &deviceInfoData,
                &DEVPKEY_PciDevice_CurrentLinkSpeed, &propType,
                (PBYTE)&currentSpeed, sizeof(currentSpeed), &propSize, 0)) {
            outInfo.pcieGenCurrent = static_cast<int>(currentSpeed);
        }
        
        // Current Link Width
        UINT32 currentWidth = 0;
        if (SetupDiGetDevicePropertyW(deviceInfoSet, &deviceInfoData,
                &DEVPKEY_PciDevice_CurrentLinkWidth, &propType,
                (PBYTE)&currentWidth, sizeof(currentWidth), &propSize, 0)) {
            outInfo.pcieLanesCurrent = static_cast<int>(currentWidth);
        }
        
        // Max Link Speed
        UINT32 maxSpeed = 0;
        if (SetupDiGetDevicePropertyW(deviceInfoSet, &deviceInfoData,
                &DEVPKEY_PciDevice_MaxLinkSpeed, &propType,
                (PBYTE)&maxSpeed, sizeof(maxSpeed), &propSize, 0)) {
            outInfo.pcieGenMax = static_cast<int>(maxSpeed);
        }
        
        // Max Link Width
        UINT32 maxWidth = 0;
        if (SetupDiGetDevicePropertyW(deviceInfoSet, &deviceInfoData,
                &DEVPKEY_PciDevice_MaxLinkWidth, &propType,
                (PBYTE)&maxWidth, sizeof(maxWidth), &propSize, 0)) {
            outInfo.pcieLanesMax = static_cast<int>(maxWidth);
        }
        
        // Get location path for reference
        WCHAR locationPath[256] = {0};
        if (SetupDiGetDeviceRegistryPropertyW(
                deviceInfoSet, &deviceInfoData,
                SPDRP_LOCATION_INFORMATION, nullptr,
                (PBYTE)locationPath, sizeof(locationPath), nullptr)) {
            outInfo.pcieLocationPath = WideToUtf8(locationPath);
        }
        
        // Check if we got valid PCIe info
        if (outInfo.pcieGenCurrent > 0 && outInfo.pcieLanesCurrent > 0) {
            outInfo.pcieInfoValid = true;
            
            char logBuf[256];
            snprintf(logBuf, sizeof(logBuf), 
                "[INFO] Detected PCIe Gen%d x%d (Max: Gen%d x%d) for %s",
                outInfo.pcieGenCurrent, outInfo.pcieLanesCurrent,
                outInfo.pcieGenMax, outInfo.pcieLanesMax,
                outInfo.name.c_str());
            Log(logBuf);
        }
        
        SetupDiDestroyDeviceInfoList(deviceInfoSet);
        return outInfo.pcieInfoValid;
    }
    
    SetupDiDestroyDeviceInfoList(deviceInfoSet);
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

// Detect if a GPU is connected via Thunderbolt/USB4/USB by walking the device tree
void DetectExternalConnection(uint32_t gpuVendorId, uint32_t gpuDeviceId, GPUInfo& outInfo) {
    outInfo.isThunderbolt = false;
    outInfo.isUSB4 = false;
    outInfo.isUSB = false;
    outInfo.thunderboltVersion = 0;
    outInfo.externalConnectionType.clear();
    
    // Create device info set for PCI devices
    HDEVINFO deviceInfoSet = SetupDiGetClassDevsW(
        nullptr, L"PCI", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    
    if (deviceInfoSet == INVALID_HANDLE_VALUE) return;
    
    SP_DEVINFO_DATA deviceInfoData;
    deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    
    DEVINST gpuDevInst = 0;
    
    // Find our GPU in the device tree
    for (DWORD i = 0; SetupDiEnumDeviceInfo(deviceInfoSet, i, &deviceInfoData); i++) {
        WCHAR hardwareId[512] = {0};
        if (!SetupDiGetDeviceRegistryPropertyW(deviceInfoSet, &deviceInfoData,
                SPDRP_HARDWAREID, nullptr, (PBYTE)hardwareId, sizeof(hardwareId), nullptr)) {
            continue;
        }
        
        // Match vendor/device ID
        WCHAR searchVen[32], searchDev[32];
        swprintf_s(searchVen, L"VEN_%04X", gpuVendorId);
        swprintf_s(searchDev, L"DEV_%04X", gpuDeviceId);
        
        std::wstring hwIdStr(hardwareId);
        if (hwIdStr.find(searchVen) != std::wstring::npos &&
            hwIdStr.find(searchDev) != std::wstring::npos) {
            gpuDevInst = deviceInfoData.DevInst;
            break;
        }
    }
    
    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    
    if (gpuDevInst == 0) return;
    
    // Track if we found a USB connection (lower priority than TB/USB4)
    bool foundUSB3 = false;
    std::string usb3ControllerName;
    
    // Debug: Log device tree traversal (can be enabled for troubleshooting)
    #ifdef DEBUG_EXTERNAL_DETECTION
    Log("[DEBUG] Starting external connection detection for GPU");
    #endif
    
    // Walk up the device tree looking for Thunderbolt/USB4/USB3 controllers
    DEVINST parentDevInst = gpuDevInst;
    int depth = 0;
    const int maxDepth = 15;  // Go a bit further up for USB chains
    
    while (depth < maxDepth) {
        DEVINST nextParent;
        if (CM_Get_Parent(&nextParent, parentDevInst, 0) != CR_SUCCESS) {
            break;  // Reached the root
        }
        parentDevInst = nextParent;
        depth++;
        
        // Get device instance ID of parent
        WCHAR parentInstanceId[MAX_DEVICE_ID_LEN] = {0};
        if (CM_Get_Device_IDW(parentDevInst, parentInstanceId, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) {
            continue;
        }
        
        std::wstring instanceStr(parentInstanceId);
        std::wstring upperInstance = instanceStr;
        std::transform(upperInstance.begin(), upperInstance.end(), upperInstance.begin(), ::towupper);
        
        #ifdef DEBUG_EXTERNAL_DETECTION
        // Log the device path for debugging
        Log("[DEBUG] Depth " + std::to_string(depth) + ": " + WideToUtf8(parentInstanceId));
        #endif
        
        // ====== Check for Thunderbolt/USB4 in device path ======
        if (upperInstance.find(L"THUNDERBOLT") != std::wstring::npos ||
            upperInstance.find(L"\\TBT") != std::wstring::npos) {
            outInfo.isThunderbolt = true;
            outInfo.isUSB = true;  // TB is USB-based
            // Try to determine version from path
            if (upperInstance.find(L"TBT4") != std::wstring::npos ||
                upperInstance.find(L"USB4") != std::wstring::npos) {
                outInfo.thunderboltVersion = 4;
                outInfo.isUSB4 = true;
                outInfo.externalConnectionType = "Thunderbolt 4";
            } else if (upperInstance.find(L"TBT3") != std::wstring::npos) {
                outInfo.thunderboltVersion = 3;
                outInfo.externalConnectionType = "Thunderbolt 3";
            } else {
                outInfo.externalConnectionType = "Thunderbolt";
            }
            return;
        }
        
        // Check for USB4 in path (non-Thunderbolt branded)
        // Patterns: USB4\, USB4-, \USB4, ACPI\USB4, USB\ROOT_HUB40 (USB4 root hub)
        if (upperInstance.find(L"USB4") != std::wstring::npos ||
            upperInstance.find(L"ROOT_HUB40") != std::wstring::npos) {  // USB4 root hub pattern
            outInfo.isUSB4 = true;
            outInfo.isUSB = true;
            outInfo.externalConnectionType = "USB4 (PCIe Tunneling)";
            return;
        }
        
        // Check for USB/xHCI in path (indicates USB-based connection)
        if (upperInstance.find(L"USB\\") != std::wstring::npos ||
            upperInstance.find(L"XHCI") != std::wstring::npos ||
            upperInstance.find(L"USBROOT") != std::wstring::npos) {
            if (!foundUSB3) {
                foundUSB3 = true;
                usb3ControllerName = "USB";
            }
        }
        
        // ====== Check vendor/device IDs for known controllers ======
        if (instanceStr.find(L"PCI\\VEN_") != std::wstring::npos) {
            uint32_t parentVendor = 0, parentDevice = 0;
            size_t venPos = instanceStr.find(L"VEN_");
            size_t devPos = instanceStr.find(L"DEV_");
            
            if (venPos != std::wstring::npos && devPos != std::wstring::npos) {
                swscanf_s(instanceStr.c_str() + venPos + 4, L"%04X", &parentVendor);
                swscanf_s(instanceStr.c_str() + devPos + 4, L"%04X", &parentDevice);
                
                // Check for Intel Thunderbolt controllers (highest priority)
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
                
                // Check for Intel TB/USB4 PCIe SWITCHES (inside eGPU enclosures)
                // These are the PCIe fabric chips inside the tunnel - indicates eGPU
                if (IsThunderboltPCIeSwitch(parentVendor, parentDevice)) {
                    outInfo.isUSB4 = true;
                    outInfo.isUSB = true;
                    // These switches are used in both TB and USB4 enclosures
                    // We can't tell which from the switch alone, so report as USB4/TB
                    outInfo.externalConnectionType = "USB4 / Thunderbolt (PCIe Tunnel)";
                    #ifdef DEBUG_EXTERNAL_DETECTION
                    char dbgBuf[128];
                    snprintf(dbgBuf, sizeof(dbgBuf), "[DEBUG] Found Intel TB/USB4 PCIe Switch: %04X:%04X", parentVendor, parentDevice);
                    Log(dbgBuf);
                    #endif
                    return;
                }
                
                // Check for USB4 controllers (AMD, ASMedia, etc.)
                if (IsUSB4Controller(parentVendor, parentDevice)) {
                    outInfo.isUSB4 = true;
                    outInfo.isUSB = true;
                    std::string controllerName = GetUSBControllerName(parentVendor, parentDevice);
                    outInfo.externalConnectionType = controllerName + " (PCIe Tunneling)";
                    return;
                }
                
                // Check for USB 3.x controllers (track but keep looking for TB/USB4)
                if (IsUSB3Controller(parentVendor, parentDevice)) {
                    if (!foundUSB3) {
                        foundUSB3 = true;
                        usb3ControllerName = GetUSBControllerName(parentVendor, parentDevice);
                    }
                }
            }
        }
        
        // ====== Check device description for keywords ======
        HDEVINFO parentDevInfoSet = SetupDiGetClassDevsW(
            nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES | DIGCF_DEVICEINTERFACE);
        
        if (parentDevInfoSet != INVALID_HANDLE_VALUE) {
            SP_DEVINFO_DATA parentInfoData;
            parentInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
            
            if (SetupDiOpenDeviceInfoW(parentDevInfoSet, parentInstanceId, nullptr, 0, &parentInfoData)) {
                WCHAR deviceDesc[256] = {0};
                if (SetupDiGetDeviceRegistryPropertyW(parentDevInfoSet, &parentInfoData,
                        SPDRP_DEVICEDESC, nullptr, (PBYTE)deviceDesc, sizeof(deviceDesc), nullptr)) {
                    std::wstring descStr(deviceDesc);
                    std::wstring descUpper = descStr;
                    std::transform(descUpper.begin(), descUpper.end(), descUpper.begin(), ::towupper);
                    
                    #ifdef DEBUG_EXTERNAL_DETECTION
                    Log("[DEBUG]   Description: " + WideToUtf8(deviceDesc));
                    #endif
                    
                    // Check for Thunderbolt (Intel certified)
                    if (descUpper.find(L"THUNDERBOLT") != std::wstring::npos) {
                        outInfo.isThunderbolt = true;
                        outInfo.isUSB = true;
                        if (descUpper.find(L"USB4") != std::wstring::npos || 
                            descUpper.find(L" 4 ") != std::wstring::npos ||
                            descUpper.find(L" 4)") != std::wstring::npos) {
                            outInfo.thunderboltVersion = 4;
                            outInfo.isUSB4 = true;
                            outInfo.externalConnectionType = "Thunderbolt 4";
                        } else if (descUpper.find(L" 3 ") != std::wstring::npos ||
                                   descUpper.find(L" 3)") != std::wstring::npos) {
                            outInfo.thunderboltVersion = 3;
                            outInfo.externalConnectionType = "Thunderbolt 3";
                        } else {
                            outInfo.externalConnectionType = "Thunderbolt";
                        }
                        SetupDiDestroyDeviceInfoList(parentDevInfoSet);
                        return;
                    }
                    
                    // Check for USB4 - various patterns used by different vendors
                    // AMD: "USB4(TM) Router", "USB4 Router", "AMD USB4 Host Router"
                    // Generic: "USB4", "USB 4", "USB4 Host"
                    bool isUSB4Device = false;
                    std::string usb4Type;
                    
                    if (descUpper.find(L"USB4") != std::wstring::npos ||
                        descUpper.find(L"USB 4") != std::wstring::npos) {
                        isUSB4Device = true;
                        
                        // Determine the specific type
                        if (descUpper.find(L"ROUTER") != std::wstring::npos) {
                            // USB4 Router - this is a hub/dock/enclosure
                            if (descUpper.find(L"AMD") != std::wstring::npos) {
                                usb4Type = "AMD USB4 Router";
                            } else {
                                usb4Type = "USB4 Router";
                            }
                        } else if (descUpper.find(L"HOST") != std::wstring::npos) {
                            // USB4 Host controller
                            if (descUpper.find(L"AMD") != std::wstring::npos) {
                                usb4Type = "AMD USB4";
                            } else {
                                usb4Type = "USB4 Host";
                            }
                        } else {
                            // Generic USB4
                            usb4Type = "USB4";
                        }
                    }
                    
                    if (isUSB4Device) {
                        outInfo.isUSB4 = true;
                        outInfo.isUSB = true;
                        outInfo.externalConnectionType = usb4Type + " (PCIe Tunneling)";
                        SetupDiDestroyDeviceInfoList(parentDevInfoSet);
                        return;
                    }
                    
                    // Check for USB 3.x (SuperSpeed)
                    if (descUpper.find(L"USB 3") != std::wstring::npos ||
                        descUpper.find(L"SUPERSPEED") != std::wstring::npos ||
                        descUpper.find(L"XHCI") != std::wstring::npos) {
                        if (!foundUSB3) {
                            foundUSB3 = true;
                            // Try to determine USB version
                            if (descUpper.find(L"3.2") != std::wstring::npos ||
                                descUpper.find(L"GEN 2X2") != std::wstring::npos ||
                                descUpper.find(L"20GBPS") != std::wstring::npos) {
                                usb3ControllerName = "USB 3.2 Gen 2x2";
                            } else if (descUpper.find(L"3.1") != std::wstring::npos ||
                                       descUpper.find(L"GEN 2") != std::wstring::npos ||
                                       descUpper.find(L"10GBPS") != std::wstring::npos) {
                                usb3ControllerName = "USB 3.1 Gen 2";
                            } else {
                                usb3ControllerName = "USB 3.x";
                            }
                        }
                    }
                }
                
                // Also check the hardware ID for USB4 patterns
                WCHAR hardwareId[512] = {0};
                if (SetupDiGetDeviceRegistryPropertyW(parentDevInfoSet, &parentInfoData,
                        SPDRP_HARDWAREID, nullptr, (PBYTE)hardwareId, sizeof(hardwareId), nullptr)) {
                    std::wstring hwIdUpper(hardwareId);
                    std::transform(hwIdUpper.begin(), hwIdUpper.end(), hwIdUpper.begin(), ::towupper);
                    
                    // Check for USB4 in hardware ID (e.g., USB\USB4, ACPI\USB4)
                    if (hwIdUpper.find(L"USB4") != std::wstring::npos) {
                        outInfo.isUSB4 = true;
                        outInfo.isUSB = true;
                        if (outInfo.externalConnectionType.empty()) {
                            outInfo.externalConnectionType = "USB4 (PCIe Tunneling)";
                        }
                        SetupDiDestroyDeviceInfoList(parentDevInfoSet);
                        return;
                    }
                }
            }
            SetupDiDestroyDeviceInfoList(parentDevInfoSet);
        }
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
        appInfo.applicationVersion = VK_MAKE_VERSION(3, 0, 0);
        appInfo.pEngineName = "No Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;

        const char* extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
        createInfo.enabledExtensionCount = 2;
        createInfo.ppEnabledExtensionNames = extensions;

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
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
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
    safeMax = std::min(safeMax, 2ull * 1024 * 1024 * 1024);  // 2GB max
    
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

bool InitVulkan() {
    // Create Vulkan Instance
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "GPU-PCIe-Test";
    appInfo.applicationVersion = VK_MAKE_VERSION(3, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    const char* instanceExtensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME
    };

    VkInstanceCreateInfo instanceInfo = {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = 2;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;

    VK_CHECK_RETURN(vkCreateInstance(&instanceInfo, nullptr, &g_app.instance), false);

    // Create Win32 Surface
    VkWin32SurfaceCreateInfoKHR surfaceInfo = {};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hinstance = GetModuleHandle(nullptr);
    surfaceInfo.hwnd = g_app.hwnd;
    VK_CHECK_RETURN(vkCreateWin32SurfaceKHR(g_app.instance, &surfaceInfo, nullptr, &g_app.surface), false);

    // Pick physical device (use first discrete GPU, fallback to any)
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(g_app.instance, &deviceCount, nullptr);
    if (deviceCount == 0) return false;

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(g_app.instance, &deviceCount, devices.data());

    g_app.renderPhysicalDevice = devices[0];  // Default to first
    for (auto& d : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            g_app.renderPhysicalDevice = d;
            break;
        }
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
        VK_CHECK_RETURN(vkCreateSemaphore(g_app.device, &semInfo, nullptr, &g_app.renderFinishedSemaphores[i]), false);
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
    
    // Create logical device for benchmarking
    // Request 2 queues for bidirectional overlap (separate DMA engines)
    uint32_t maxQueues = queueFamilies[g_app.benchQueueFamily].queueCount;
    uint32_t requestedQueues = (maxQueues >= 2) ? 2 : 1;
    float queuePriorities[2] = { 1.0f, 1.0f };
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = g_app.benchQueueFamily;
    queueInfo.queueCount = requestedQueues;
    queueInfo.pQueuePriorities = queuePriorities;
    
    // Enable host query reset if available (Vulkan 1.2 feature)
    VkPhysicalDeviceHostQueryResetFeatures hostQueryResetFeatures = {};
    hostQueryResetFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES;
    hostQueryResetFeatures.hostQueryReset = VK_TRUE;
    
    VkDeviceCreateInfo deviceInfo = {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &hostQueryResetFeatures;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
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

VkBufferAllocation CreateBuffer(VkBufferType type, VkDeviceSize size) {
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
    
    VkResult result = vkCreateBuffer(g_app.benchDevice, &bufferInfo, nullptr, &alloc.buffer);
    if (result != VK_SUCCESS) {
        Log("[ERROR] vkCreateBuffer failed: " + std::to_string((int)result) + 
            " (Size: " + FormatSize(size) + ")");
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
    
    // Check global timeout
    if (!g_app.vramTestRunning && IsGlobalTimeoutExceeded()) {
        Log("[ERROR] Global benchmark timeout exceeded (5 minutes)");
        return FenceWaitResult::Timeout;
    }
    
    // Wait for fence
    uint64_t timeout = static_cast<uint64_t>(Constants::FENCE_WAIT_TIMEOUT_MS) * 1000000ULL; // ms -> ns
    VkResult result = vkWaitForFences(g_app.benchDevice, 1, &g_app.benchFence, VK_TRUE, timeout);
    
    if (result == VK_TIMEOUT) {
        g_app.fenceTimeoutCount++;
        Log("[WARNING] Benchmark fence wait timed out after " + 
            std::to_string(Constants::FENCE_WAIT_TIMEOUT_MS / 1000) + "s (timeout #" + 
            std::to_string(g_app.fenceTimeoutCount.load()) + ")");
        
        if (g_app.fenceTimeoutCount >= Constants::MAX_FENCE_RETRIES) {
            Log("[ERROR] Max fence timeouts exceeded - possible GPU hang. Aborting benchmark.");
            g_app.benchmarkAborted = true;
            return FenceWaitResult::Timeout;
        }
        return FenceWaitResult::Timeout;
    } else if (result != VK_SUCCESS) {
        Log("[ERROR] vkWaitForFences failed: " + std::to_string((int)result));
        return FenceWaitResult::Error;
    }
    
    // Reset fence for next use
    vkResetFences(g_app.benchDevice, 1, &g_app.benchFence);
    
    // Success - reset timeout counter
    g_app.fenceTimeoutCount = 0;
    g_app.benchFenceValue++;
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
    g_app.currentTest = name;
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
                if (fenceResult == FenceWaitResult::Error || g_app.benchmarkAborted) {
                    Log("[ERROR] Critical fence error - aborting bandwidth test");
                    break;
                }

                double totalSeconds = std::chrono::duration<double>(endTime - startTime).count();
                if (totalSeconds > 0) {
                    double sizeGB = static_cast<double>(size) * copies / (1024.0 * 1024.0 * 1024.0);
                    
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
                        double roundtripBw = (totalBytes / (1024.0 * 1024.0 * 1024.0)) / totalSeconds;
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
                if (fenceResult == FenceWaitResult::Error || g_app.benchmarkAborted) break;

                double seconds = std::chrono::duration<double>(endTime - startTime).count();
                if (seconds > 0) {
                    double bw = (static_cast<double>(size) * copies / (1024.0 * 1024.0 * 1024.0)) / seconds;
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
                if (fenceResult == FenceWaitResult::Error || g_app.benchmarkAborted) {
                    Log("[ERROR] Critical fence error - aborting bandwidth test");
                    break;
                }

                uint64_t timestamps[2] = {};
                VkResult qr = vkGetQueryPoolResults(g_app.benchDevice, queryPool, 0, 2,
                    sizeof(timestamps), timestamps, sizeof(uint64_t),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                
                if (qr == VK_SUCCESS && timestamps[1] > timestamps[0]) {
                    uint64_t delta = timestamps[1] - timestamps[0];
                    double seconds = static_cast<double>(delta) * static_cast<double>(g_app.benchTimestampPeriod) / 1e9;
                    if (seconds > 0) {
                        double bw = (static_cast<double>(size) * copies / (1024.0 * 1024.0 * 1024.0)) / seconds;
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
    g_app.currentTest = name;
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
        if (fenceResult == FenceWaitResult::Cancelled || g_app.benchmarkAborted) break;

        // Read timestamps
        std::vector<uint64_t> timestamps(opsThisBatch * 2);
        VkResult qr = vkGetQueryPoolResults(g_app.benchDevice, queryPool, 0, opsThisBatch * 2,
            timestamps.size() * sizeof(uint64_t), timestamps.data(), sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        
        if (qr == VK_SUCCESS) {
            for (int i = 0; i < opsThisBatch; ++i) {
                uint64_t tStart = timestamps[i * 2 + 0];
                uint64_t tEnd = timestamps[i * 2 + 1];
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
    g_app.currentTest = "Command Latency";
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
        if (fenceResult == FenceWaitResult::Cancelled || g_app.benchmarkAborted) break;

        std::vector<uint64_t> timestamps(opsThisBatch * 2);
        VkResult qr = vkGetQueryPoolResults(g_app.benchDevice, queryPool, 0, opsThisBatch * 2,
            timestamps.size() * sizeof(uint64_t), timestamps.data(), sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        
        if (qr == VK_SUCCESS) {
            for (int i = 0; i < opsThisBatch; ++i) {
                uint64_t tStart = timestamps[i * 2 + 0];
                uint64_t tEnd = timestamps[i * 2 + 1];
                double deltaSec = static_cast<double>(tEnd - tStart) * static_cast<double>(g_app.benchTimestampPeriod) / 1e9;
                double us = deltaSec * 1'000'000.0;
                latencies.push_back(us);
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
    g_app.currentTest = "Bidirectional " + FormatSize(size);
    BenchmarkResult result;
    result.testName = g_app.currentTest;
    result.unit = "GB/s";

    auto cpuUpload = CreateBuffer(VkBufferType::Upload, size);
    auto gpuDefault = CreateBuffer(VkBufferType::DeviceLocal, size);
    auto gpuSrc = CreateBuffer(VkBufferType::DeviceLocal, size);
    auto cpuReadback = CreateBuffer(VkBufferType::Readback, size);

    if (!cpuUpload || !gpuDefault || !gpuSrc || !cpuReadback) {
        Log("[ERROR] Failed to create resources for bidirectional test - likely out of VRAM");
        cpuUpload.Destroy(g_app.benchDevice);
        gpuDefault.Destroy(g_app.benchDevice);
        gpuSrc.Destroy(g_app.benchDevice);
        cpuReadback.Destroy(g_app.benchDevice);
        return result;
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
        vkWaitForFences(g_app.benchDevice, 2, warmupFences, VK_TRUE, UINT64_MAX);
        vkResetFences(g_app.benchDevice, 2, warmupFences);
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
            
            // Wait for both to complete
            VkFence fences[2] = { g_app.benchFence, g_app.benchFence2 };
            uint64_t timeout = static_cast<uint64_t>(Constants::FENCE_WAIT_TIMEOUT_MS) * 1000000ULL;
            VkResult waitResult = vkWaitForFences(g_app.benchDevice, 2, fences, VK_TRUE, timeout);
            
            auto endTime = std::chrono::high_resolution_clock::now();
            
            vkResetFences(g_app.benchDevice, 2, fences);
            
            if (waitResult == VK_TIMEOUT) {
                g_app.fenceTimeoutCount++;
                if (g_app.fenceTimeoutCount >= Constants::MAX_FENCE_RETRIES) {
                    g_app.benchmarkAborted = true;
                    break;
                }
                continue;
            } else if (waitResult != VK_SUCCESS) {
                break;
            }
            
            g_app.fenceTimeoutCount = 0;
            
            double seconds = std::chrono::duration<double>(endTime - startTime).count();
            if (seconds > 0) {
                double bw = (static_cast<double>(size) * copies * 2 / (1024.0 * 1024.0 * 1024.0)) / seconds;
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
            
            if (fenceResult == FenceWaitResult::Cancelled || g_app.benchmarkAborted) break;

            double seconds = std::chrono::duration<double>(endTime - startTime).count();
            if (seconds > 0) {
                double bw = (static_cast<double>(size) * copies * 2 / (1024.0 * 1024.0 * 1024.0)) / seconds;
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

// Compare buffers and find errors
void CompareBuffers(const uint32_t* expected, const uint32_t* actual, size_t count,
                   VRAMTestPattern pattern, std::vector<VRAMError>& errors,
                   size_t baseOffset, size_t& totalErrorCount) {
    
    const size_t CLUSTER_THRESHOLD = 256u;  // Merge errors within this range
    VRAMError currentCluster;
    bool inCluster = false;
    
    for (size_t i = 0; i < count; ++i) {
        if (expected[i] != actual[i]) {
            totalErrorCount++;
            size_t byteOffset = baseOffset + (i * sizeof(uint32_t));
            
            if (!inCluster) {
                // Start new cluster
                currentCluster = {};
                currentCluster.offsetStart = byteOffset;
                currentCluster.offsetEnd = byteOffset + sizeof(uint32_t);
                currentCluster.expected = expected[i];
                currentCluster.actual = actual[i];
                currentCluster.pattern = pattern;
                currentCluster.errorCount = 1;
                inCluster = true;
            } else if (byteOffset - currentCluster.offsetEnd <= CLUSTER_THRESHOLD * sizeof(uint32_t)) {
                // Extend current cluster
                currentCluster.offsetEnd = byteOffset + sizeof(uint32_t);
                currentCluster.errorCount++;
            } else {
                // Close current cluster and start new one
                errors.push_back(currentCluster);
                currentCluster = {};
                currentCluster.offsetStart = byteOffset;
                currentCluster.offsetEnd = byteOffset + sizeof(uint32_t);
                currentCluster.expected = expected[i];
                currentCluster.actual = actual[i];
                currentCluster.pattern = pattern;
                currentCluster.errorCount = 1;
            }
        }
    }
    
    // Close final cluster if any
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
    CompareBuffers(expectedData.data(), actualData, dwordCount, pattern, errors, regionOffset, totalErrors);
    
    vkUnmapMemory(g_app.benchDevice, readbackBuffer.memory);
    
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
    
    double targetPercent = g_app.vramTestFullScan ? 0.90 : Constants::VRAM_SAFETY_MARGIN;
    size_t targetTestSize = static_cast<size_t>(gpu.dedicatedVRAM * targetPercent);
    
    if (g_app.vramTestFullScan) {
        Log("FULL SCAN MODE: Attempting to test up to " + FormatSize(targetTestSize) + " (~90%) of VRAM");
        Log("[WARNING] Full scan allocates maximum possible VRAM - may cause instability");
    } else {
        Log("Testing up to " + FormatSize(targetTestSize) + " (~80%) of VRAM");
        Log("(Enable 'Full Scan' for more thorough testing)");
    }
    Log("");
    
    Log("NOTE: GPU drivers and OS reserve some VRAM for:");
    Log("  - Command buffers and page tables");
    Log("  - Desktop compositor (Windows DWM)");
    Log("  - Vulkan runtime scratch space");
    Log("We test as much as can be allocated via Vulkan.");
    Log("");
    
    std::vector<VRAMTestPattern> patterns = {
        VRAMTestPattern::AllZeros,
        VRAMTestPattern::AllOnes,
        VRAMTestPattern::Checkerboard,
        VRAMTestPattern::InverseCheckerboard,
        VRAMTestPattern::AddressPattern,
        VRAMTestPattern::Random
    };
    
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
    size_t patternsPerChunk = patterns.size() + 2;
    size_t totalSteps = numChunks * patternsPerChunk;
    size_t completedSteps = 0;
    
    double targetPercentDisplay = (static_cast<double>(targetTestSize) / gpu.dedicatedVRAM) * 100.0;
    char percentBuf[64];
    snprintf(percentBuf, sizeof(percentBuf), "%.0f%%", targetPercentDisplay);
    
    Log("");
    Log("Will test " + FormatSize(targetTestSize) + " (" + percentBuf + " of VRAM) in " + 
        std::to_string(numChunks) + " chunks");
    Log("Each chunk: 6 basic patterns + marching ones + marching zeros");
    Log("Reallocating between chunks to potentially hit different physical regions");
    Log("");
    
    size_t totalErrors = 0;
    std::vector<VRAMError> allErrors;
    bool hadCriticalFailure = false;
    size_t totalBytesTested = 0;
    
    for (size_t chunkNum = 0; chunkNum < numChunks && !g_app.vramTestCancelRequested && !hadCriticalFailure; ++chunkNum) {
        size_t chunkOffset = chunkNum * chunkSize;
        size_t thisChunkSize = std::min(chunkSize, targetTestSize - chunkOffset);
        
        Log("=== Chunk " + std::to_string(chunkNum + 1) + "/" + std::to_string(numChunks) + 
            " (" + FormatSize(thisChunkSize) + " at logical offset " + FormatSize(chunkOffset) + ") ===");
        
        auto uploadBuffer = CreateBuffer(VkBufferType::Upload, thisChunkSize);
        auto gpuBuffer = CreateBuffer(VkBufferType::DeviceLocal, thisChunkSize);
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
        
        for (const auto& pattern : patterns) {
            if (g_app.vramTestCancelRequested || chunkFailed) break;
            
            std::string patternName = GetPatternName(pattern);
            g_app.vramTestCurrentPattern = patternName + " [" + std::to_string(chunkNum + 1) + "/" + std::to_string(numChunks) + "]";
            
            g_app.fenceTimeoutCount = 0;
            size_t patternErrors = 0;
            
            if (!RunVRAMPatternTest(pattern, thisChunkSize, chunkOffset,
                                    uploadBuffer, gpuBuffer, readbackBuffer,
                                    allErrors, patternErrors)) {
                if (!g_app.vramTestCancelRequested) {
                    Log("  [WARNING] " + patternName + " failed");
                    chunkFailed = true;
                }
                break;
            }
            
            chunkErrors += patternErrors;
        }
        
        // Run marching patterns
        if (!g_app.vramTestCancelRequested && !chunkFailed) {
            g_app.vramTestCurrentPattern = "Marching [" + std::to_string(chunkNum + 1) + "/" + std::to_string(numChunks) + "]";
            g_app.fenceTimeoutCount = 0;
            
            for (int iter = 0; iter < MARCH_ITERATIONS && !g_app.vramTestCancelRequested && !chunkFailed; ++iter) {
                size_t marchErrors = 0;
                if (!RunVRAMPatternTest(VRAMTestPattern::MarchingOnes, thisChunkSize, chunkOffset,
                                  uploadBuffer, gpuBuffer, readbackBuffer,
                                  allErrors, marchErrors, iter)) {
                    chunkFailed = true;
                    break;
                }
                chunkErrors += marchErrors;
            }
            
            for (int iter = 0; iter < MARCH_ITERATIONS && !g_app.vramTestCancelRequested && !chunkFailed; ++iter) {
                size_t marchErrors = 0;
                if (!RunVRAMPatternTest(VRAMTestPattern::MarchingZeros, thisChunkSize, chunkOffset,
                                  uploadBuffer, gpuBuffer, readbackBuffer,
                                  allErrors, marchErrors, iter)) {
                    chunkFailed = true;
                    break;
                }
                chunkErrors += marchErrors;
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
    }
    
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
    
    g_app.systemMemory = DetectSystemMemory();
    if (g_app.systemMemory.detected) {
        Log(FormatSystemMemoryInfo(g_app.systemMemory));
        
        if (g_app.systemMemory.configuredSpeedMT > 0 && 
            g_app.systemMemory.speedMT > 0 &&
            g_app.systemMemory.configuredSpeedMT < g_app.systemMemory.speedMT * 0.85) {
            Log("[WARNING] RAM running at " + std::to_string(g_app.systemMemory.configuredSpeedMT) + 
                " MT/s (rated for " + std::to_string(g_app.systemMemory.speedMT) + 
                " MT/s) - XMP/EXPO may not be enabled");
        }
        
        if (g_app.systemMemory.channels == 1) {
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
    int successfulRuns = 0;

    int testsPerRun = 2;  // Upload + Download
    if (g_app.config.runBidirectional) testsPerRun++;
    if (g_app.config.runLatency) testsPerRun += 3;
    g_app.totalTests = testsPerRun * g_app.config.numRuns;

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
                resBidir.testName = "Bidirectional " + FormatSize(g_app.config.bandwidthSize) + runSuffix;
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
        
        successfulRuns++;
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
        
        // Show summary window
        g_app.showSummaryWindow = true;

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
        
        g_app.state = AppState::Completed;
    }
    
    g_app.benchmarkThreadRunning = false;
}

// ============================================================================
//                             CSV EXPORT
// ============================================================================

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
    }
    
    // Add eGPU detection info
    if (g_app.possibleEGPU) {
        file << "\neGPU Detection,Possible eGPU," << g_app.eGPUConnectionType << "\n";
    }

    file.close();
    Log("Results exported to " + filename);
}

// ============================================================================
//                             GUI RENDERING
// ============================================================================

void RenderGUI() {
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
                ExportCSV("gpu_benchmark_results.csv");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                PostQuitMessage(0);
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

    ImGui::Text("GPU-PCIe-Test v3.0 GUI");

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
                snprintf(ramBuf, sizeof(ramBuf), "  %lluGB %s @ %u MT/s",
                        g_app.systemMemory.totalCapacityGB,
                        g_app.systemMemory.type.c_str(),
                        g_app.systemMemory.configuredSpeedMT);
            } else {
                snprintf(ramBuf, sizeof(ramBuf), "  %lluGB %s",
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
        g_app.currentTest = "Initializing...";
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
        ExportCSV("gpu_benchmark_results.csv");
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
        g_app.config.quickMode = false;
        g_app.config.averageRuns = true;
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
                          "Test with MemTest86+ or Windows Memory Diagnostic instead.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    
    // Full Scan checkbox (only when not running and not iGPU)
    if (!g_app.vramTestRunning && !isIntegratedGPU) {
        ImGui::Checkbox("Full Scan (~90%)", &g_app.vramTestFullScan);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Test ~90%% of VRAM instead of default ~80%%.\n"
                             "More thorough but may cause instability.\n"
                             "Use if standard scan passes but you suspect issues.");
        }
        ImGui::Spacing();
    }
    
    if (vramScanDisabled) ImGui::BeginDisabled();
    if (ImGui::Button("VRAM Scan", ImVec2(-1, 35))) {
        if (!g_app.vramTestRunning && !g_app.benchmarkThreadRunning) {
            // Clear any previous results immediately
            g_app.vramTestResult = {};
            g_app.vramTestCurrentPattern.clear();
            
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
                          g_app.vramTestCurrentPattern.c_str());
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
        ImGui::Text("Current: %s", g_app.currentTest.c_str());

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
        ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "DETECTED PCIe LINK (from Windows)");
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
        ImGui::Text("Detected PCIe Link (from Windows):");
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
            int type;  // 0 = standard, 1 = upload, 2 = download
            std::string description;
        };
        
        std::vector<BandwidthEntry> entries;
        
        // Add all interface standards
        for (int i = 0; i < NUM_INTERFACE_SPEEDS; ++i) {
            entries.push_back({
                INTERFACE_SPEEDS[i].name,
                INTERFACE_SPEEDS[i].bandwidth,
                0,
                INTERFACE_SPEEDS[i].description
            });
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
        std::vector<double> standardBW, uploadBW, downloadBW;
        std::vector<double> standardPos, uploadPos, downloadPos;
        
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
            } else {
                downloadPos.push_back(positions[i]);
                downloadBW.push_back(entries[i].bandwidth);
            }
        }
        for (const auto& s : labelStorage) labels.push_back(s.c_str());
        
        // Calculate plot height based on number of entries
        float plotHeight = std::max(350.0f, numEntries * 22.0f + 60.0f);
        
        // Draw the horizontal bar chart
        if (ImPlot::BeginPlot("##InterfaceComparison", ImVec2(-1, plotHeight))) {
            ImPlot::SetupAxes("Bandwidth (GB/s)", "", 0, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_Invert);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, 70, ImPlotCond_Always);
            ImPlot::SetupAxisTicks(ImAxis_Y1, positions.data(), numEntries, labels.data());
            
            double barHeight = 0.7;
            
            // Plot standard interfaces in gray
            if (!standardBW.empty()) {
                ImPlot::SetNextFillStyle(ImVec4(0.5f, 0.5f, 0.5f, 0.7f));
                ImPlot::PlotBars("Interface Standards", standardBW.data(), standardPos.data(), 
                                  static_cast<int>(standardBW.size()), barHeight, ImPlotBarsFlags_Horizontal);
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
                
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Error Regions:");
                ImGui::Spacing();
                
                ImGui::BeginChild("ErrorList", ImVec2(0, 150), true, ImGuiWindowFlags_HorizontalScrollbar);
                for (const auto& err : result.errors) {
                    char errBuf[256];
                    snprintf(errBuf, sizeof(errBuf), 
                            "0x%08zX - 0x%08zX: %zu errors (%s)",
                            err.offsetStart, err.offsetEnd, err.errorCount,
                            GetPatternName(err.pattern).c_str());
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
        ImGui::Text("GPU-PCIe-Test v3.0 GUI Edition (Vulkan)");
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
        // Swapchain needs recreation
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
    submitInfo.pSignalSemaphores = &g_app.renderFinishedSemaphores[frameIdx];

    vkQueueSubmit(g_app.graphicsQueue, 1, &submitInfo, g_app.inFlightFences[frameIdx]);

    // Present
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &g_app.renderFinishedSemaphores[frameIdx];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &g_app.swapChain;
    presentInfo.pImageIndices = &g_app.imageIndex;

    VkResult presentResult = vkQueuePresentKHR(g_app.graphicsQueue, &presentInfo);
    
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        // Will handle on next frame
    }

    g_app.frameIndex = (g_app.frameIndex + 1) % Constants::NUM_FRAMES_IN_FLIGHT;
}

void ResizeSwapChain(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (g_app.device == VK_NULL_HANDLE) return;

    WaitForGPU();

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

    // Recreate framebuffers
    CreateFramebuffers();

    g_app.windowWidth = width;
    g_app.windowHeight = height;
    g_app.frameIndex = 0;
}

// ============================================================================
//                           WINDOW PROCEDURE
// ============================================================================

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_ENTERSIZEMOVE:
        g_app.isResizing = true;
        return 0;

    case WM_EXITSIZEMOVE:
        g_app.isResizing = false;
        g_app.pendingResize = true;
        return 0;

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_app.pendingWidth = LOWORD(lParam);
        g_app.pendingHeight = HIWORD(lParam);
        g_app.pendingResize = true;
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ============================================================================
//                              MAIN
// ============================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Enable DPI awareness
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        typedef BOOL(WINAPI* SetProcessDpiAwarenessContextFunc)(DPI_AWARENESS_CONTEXT);
        auto setDpiFunc = (SetProcessDpiAwarenessContextFunc)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setDpiFunc) {
            setDpiFunc(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GPUPCIeTestGUI";
    RegisterClassExW(&wc);

    // Calculate window size
    RECT windowRect = { 0, 0, Constants::WINDOW_WIDTH, Constants::WINDOW_HEIGHT };
    AdjustWindowRectEx(&windowRect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int windowWidth = windowRect.right - windowRect.left;
    int windowHeight = windowRect.bottom - windowRect.top;

    // Create window
    g_app.hwnd = CreateWindowExW(
        0, L"GPUPCIeTestGUI", L"GPU-PCIe-Test v3.0 GUI (Vulkan)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowWidth, windowHeight,
        nullptr, nullptr, hInstance, nullptr
    );

    // Get actual client size
    RECT clientRect;
    GetClientRect(g_app.hwnd, &clientRect);
    g_app.windowWidth = clientRect.right - clientRect.left;
    g_app.windowHeight = clientRect.bottom - clientRect.top;

    ShowWindow(g_app.hwnd, nCmdShow);
    UpdateWindow(g_app.hwnd);

    // Initialize Vulkan
    if (!InitVulkan()) {
        MessageBoxA(nullptr, "Failed to initialize Vulkan. Please ensure Vulkan drivers are installed.", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Enumerate GPUs
    EnumerateGPUs();
    
    // Detect system memory early
    g_app.systemMemory = DetectSystemMemory();

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

    // Initialize Win32 backend first to get DPI
    ImGui_ImplWin32_Init(g_app.hwnd);

    // Get DPI scale
    float dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(g_app.hwnd);
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
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        // Handle deferred resize
        if (g_app.pendingResize && !g_app.isResizing) {
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
        ImGui_ImplWin32_NewFrame();
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
        }
    }
    
    WaitForGPU();

    // ImGui cleanup
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    // Vulkan cleanup
    for (int i = 0; i < Constants::NUM_FRAMES_IN_FLIGHT; i++) {
        if (g_app.imageAvailableSemaphores[i] != VK_NULL_HANDLE) vkDestroySemaphore(g_app.device, g_app.imageAvailableSemaphores[i], nullptr);
        if (g_app.renderFinishedSemaphores[i] != VK_NULL_HANDLE) vkDestroySemaphore(g_app.device, g_app.renderFinishedSemaphores[i], nullptr);
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
    if (g_app.instance != VK_NULL_HANDLE) vkDestroyInstance(g_app.instance, nullptr);

    DestroyWindow(g_app.hwnd);
    UnregisterClassW(L"GPUPCIeTestGUI", hInstance);

    return 0;
}

// ============================================================================
// GPU Performance Benchmark Tool - Single Executable Edition v2.0
// Combined D3D12 + Vulkan - Everything in ONE file
// ============================================================================
// Features:
//   - Multi-run averaging (default 3 runs)
//   - Bidirectional bandwidth test
//   - GPU/PCIe detection and reporting
//   - D3D12 and Vulkan API support
// ============================================================================
// Build: cl /EHsc /std:c++17 /O2 /MD /I"%VULKAN_SDK%\Include" GPUPCIETest.cpp ^
//           d3d12.lib dxgi.lib /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib ^
//           /out:GPU-PCIe-Test.exe
// ============================================================================

// Prevent Windows.h from defining min/max macros that conflict with std::min/max
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <vulkan/vulkan.h>
#include <chrono>
#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <numeric>
#include <conio.h>
#include <string>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <cstring>
#include <thread>
#include <future>
#include <mutex>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "vulkan-1.lib")

using Microsoft::WRL::ComPtr;

// ============================================================================
//                              CONSTANTS
// ============================================================================
// Using constexpr for compile-time constants - provides type safety, scoping,
// and debugger visibility (superior to #define macros)
// ============================================================================

namespace Constants {
    // Default benchmark parameters
    constexpr size_t DEFAULT_BANDWIDTH_SIZE     = 256ull * 1024 * 1024;  // 256 MB
    constexpr size_t DEFAULT_LATENCY_SIZE       = 1;                     // 1 byte
    constexpr int    DEFAULT_COMMAND_LATENCY_ITERS = 100000;
    constexpr int    DEFAULT_TRANSFER_LATENCY_ITERS = 10000;
    constexpr int    DEFAULT_COPIES_PER_BATCH   = 8;
    constexpr int    DEFAULT_BANDWIDTH_BATCHES  = 32;
    constexpr int    DEFAULT_NUM_RUNS           = 3;    // Multi-run averaging
    constexpr int    MAX_NUM_RUNS               = 10;
    constexpr int    MIN_NUM_RUNS               = 1;
    
    // Display formatting
    constexpr int    DIVIDER_WIDTH              = 70;
    
    // Performance thresholds
    constexpr double REALISTIC_MIN_PERCENT      = 60.0;  // Minimum expected efficiency
    constexpr double REALISTIC_MAX_PERCENT      = 95.0;  // Maximum expected efficiency
    constexpr double EXCEPTIONAL_THRESHOLD      = 95.0;  // Above this is exceptional
}

// ============================================================================
//                          FORWARD DECLARATIONS
// ============================================================================

struct InterfaceSpeed;
struct BandwidthAnalysis;
struct TestResults;
struct GPUInfo;
struct BenchmarkConfig;
class CSVLogger;

// ============================================================================
//                          DATA STRUCTURES
// ============================================================================

// ----------------------------------------------------------------------------
// Interface Speed Reference Table
// Used to identify likely PCIe/Thunderbolt connection based on measured bandwidth
// ----------------------------------------------------------------------------
struct InterfaceSpeed {
    const char* name;
    double bandwidth_gbps;      // Theoretical maximum in GB/s
    const char* description;
};

// PCIe and Thunderbolt reference speeds
// Note: Real-world performance is typically 60-95% of theoretical due to:
//   - Protocol overhead (TLP headers, flow control)
//   - Encoding overhead (128b/130b for PCIe 3.0+)
//   - Driver and OS overhead
static constexpr InterfaceSpeed INTERFACE_SPEEDS[] = {
    // PCIe 3.0 (8 GT/s, 128b/130b encoding)
    {"PCIe 3.0 x1",  0.985,  "Single lane PCIe Gen 3"},
    {"PCIe 3.0 x4",  3.94,   "Common for M.2 SSDs, some GPUs"},
    {"PCIe 3.0 x8",  7.88,   "Older GPUs, some workstation cards"},
    {"PCIe 3.0 x16", 15.75,  "Standard GPU slot (older platforms)"},
    
    // PCIe 4.0 (16 GT/s, 128b/130b encoding)
    {"PCIe 4.0 x1",  1.97,   "Single lane PCIe Gen 4"},
    {"PCIe 4.0 x4",  7.88,   "Modern M.2 SSDs, OCuLink Gen 4"},
    {"PCIe 4.0 x8",  15.75,  "Some modern GPUs in x8 mode"},
    {"PCIe 4.0 x16", 31.5,   "Modern GPU slot (AMD Ryzen 3000+, Intel 11th gen+)"},
    
    // PCIe 5.0 (32 GT/s, 128b/130b encoding)
    {"PCIe 5.0 x1",  3.94,   "Single lane PCIe Gen 5"},
    {"PCIe 5.0 x4",  15.75,  "Next-gen M.2 SSDs, OCuLink Gen 5"},
    {"PCIe 5.0 x8",  31.5,   "High-end GPUs in x8 mode"},
    {"PCIe 5.0 x16", 63.0,   "Cutting-edge GPU slot (AMD Ryzen 7000+, Intel 12th gen+)"},
    
    // Thunderbolt (uses PCIe tunneling)
    {"Thunderbolt 3", 2.75,  "40 Gbps - Common eGPU connection"},
    {"Thunderbolt 4", 2.75,  "40 Gbps - Same speed as TB3, better specs"},
    {"Thunderbolt 5", 6.0,   "80 Gbps bidirectional - Latest standard"},
    {"TB5 Asymmetric", 12.0, "120 Gbps download / 40 Gbps upload"},
    
    // USB4 (uses PCIe tunneling, compatible with TB3)
    {"USB 3.2 Gen 2",  1.25, "10 Gbps - USB-C"},
    {"USB4 Gen 3x2",   4.8,  "40 Gbps - USB4"},
    
    // External PCIe cables
    {"OCuLink PCIe 3.0", 3.94,  "External PCIe cable (Gen 3 x4)"},
    {"OCuLink PCIe 4.0", 7.88,  "External PCIe cable (Gen 4 x4)"},
};

static constexpr int NUM_INTERFACES = sizeof(INTERFACE_SPEEDS) / sizeof(InterfaceSpeed);

// ----------------------------------------------------------------------------
// Bandwidth Analysis Result
// ----------------------------------------------------------------------------
struct BandwidthAnalysis {
    const InterfaceSpeed* likelyInterface;
    double percentOfTheoretical;
    bool isRealistic;   // True if within 60-95% of theoretical max
};

// ----------------------------------------------------------------------------
// GPU Information Structure
// Stores detected GPU properties from D3D12 or Vulkan
// ----------------------------------------------------------------------------
struct GPUInfo {
    std::string name;
    std::string vendor;
    std::string driverVersion;
    uint64_t    dedicatedVideoMemory;   // In bytes
    uint64_t    sharedSystemMemory;     // In bytes
    
    // PCIe info (if available)
    bool        hasPCIeInfo;
    int         pcieGen;                // 3, 4, 5, etc.
    int         pcieLanes;              // 1, 4, 8, 16
    double      pcieTheoreticalBW;      // Theoretical bandwidth in GB/s
    
    // Hardware IDs
    uint32_t    vendorId;
    uint32_t    deviceId;
    
    GPUInfo() : dedicatedVideoMemory(0), sharedSystemMemory(0), 
                hasPCIeInfo(false), pcieGen(0), pcieLanes(0), 
                pcieTheoreticalBW(0), vendorId(0), deviceId(0) {}
};

// ----------------------------------------------------------------------------
// Test Results Structure
// Stores results from a single test run
// ----------------------------------------------------------------------------
struct TestResults {
    std::string testName;
    double minValue;
    double avgValue;
    double maxValue;
    double p99Value;        // 99th percentile
    double p999Value;       // 99.9th percentile
    std::string unit;
    std::chrono::system_clock::time_point timestamp;
    
    TestResults() : minValue(0), avgValue(0), maxValue(0), 
                    p99Value(0), p999Value(0) {}
};

// ----------------------------------------------------------------------------
// Aggregated Results (for multi-run averaging)
// ----------------------------------------------------------------------------
struct AggregatedResults {
    std::string testName;
    std::string unit;
    
    // Averaged across runs
    double avgMin;
    double avgAvg;
    double avgMax;
    double avgP99;
    double avgP999;
    
    // Standard deviation of the averages (measure of consistency)
    double stdDevAvg;
    
    // Individual run results (for detailed logging)
    std::vector<TestResults> runs;
    
    AggregatedResults() : avgMin(0), avgAvg(0), avgMax(0), 
                          avgP99(0), avgP999(0), stdDevAvg(0) {}
};

// ----------------------------------------------------------------------------
// Benchmark Configuration
// ----------------------------------------------------------------------------
struct BenchmarkConfig {
    // Buffer sizes
    size_t largeBandwidthSize;      // For bandwidth tests (default 256 MB)
    size_t smallLatencySize;        // For latency tests (default 1 byte)
    
    // Iteration counts
    int commandLatencyIters;        // Empty command submission iterations
    int transferLatencyIters;       // Small transfer latency iterations
    int copiesPerBatch;             // Copies per bandwidth batch
    int bandwidthBatches;           // Number of bandwidth batches
    
    // Multi-run settings
    int numRuns;                    // Number of runs to average (default 3)
    bool logAllRuns;                // Log each run or just averages
    
    // Mode settings
    bool continuousMode;            // Allow ESC to stop tests early
    bool enableCSVLogging;          // Write results to CSV
    bool enableBidirectionalTest;   // Run bidirectional bandwidth test
    bool showDetailedGPUInfo;       // Show detailed GPU/PCIe info
    
    std::string csvFilename;
    
    BenchmarkConfig() :
        largeBandwidthSize(Constants::DEFAULT_BANDWIDTH_SIZE),
        smallLatencySize(Constants::DEFAULT_LATENCY_SIZE),
        commandLatencyIters(Constants::DEFAULT_COMMAND_LATENCY_ITERS),
        transferLatencyIters(Constants::DEFAULT_TRANSFER_LATENCY_ITERS),
        copiesPerBatch(Constants::DEFAULT_COPIES_PER_BATCH),
        bandwidthBatches(Constants::DEFAULT_BANDWIDTH_BATCHES),
        numRuns(Constants::DEFAULT_NUM_RUNS),
        logAllRuns(false),
        continuousMode(false),
        enableCSVLogging(true),
        enableBidirectionalTest(true),
        showDetailedGPUInfo(true),
        csvFilename("gpu_benchmark_results.csv")
    {}
};

// ----------------------------------------------------------------------------
// Graphics API Selection
// ----------------------------------------------------------------------------
enum class GraphicsAPI {
    D3D12,
    Vulkan,
    Both
};

// ============================================================================
//                          UTILITY FUNCTIONS
// ============================================================================

// ----------------------------------------------------------------------------
// Printing Helpers
// ----------------------------------------------------------------------------
inline void PrintDivider() {
    std::cout << std::string(Constants::DIVIDER_WIDTH, '-') << '\n';
}

inline void PrintDoubleDivider() {
    std::cout << std::string(Constants::DIVIDER_WIDTH, '=') << '\n';
}

// Format bytes into human-readable size
static std::string FormatSize(size_t bytes) {
    if (bytes >= 1024ull * 1024 * 1024) 
        return std::to_string(bytes / (1024ull * 1024 * 1024)) + " GB";
    if (bytes >= 1024 * 1024)           
        return std::to_string(bytes / (1024 * 1024)) + " MB";
    if (bytes >= 1024)                  
        return std::to_string(bytes / 1024) + " KB";
    return std::to_string(bytes) + " B";
}

// Format memory size with decimal precision
static std::string FormatMemorySize(uint64_t bytes) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    if (bytes >= 1024ull * 1024 * 1024) {
        oss << (bytes / (1024.0 * 1024.0 * 1024.0)) << " GB";
    } else if (bytes >= 1024 * 1024) {
        oss << (bytes / (1024.0 * 1024.0)) << " MB";
    } else {
        oss << bytes << " bytes";
    }
    return oss.str();
}

// Check if ESC key was pressed (for continuous mode)
static bool CheckForEscape() {
    if (_kbhit()) return _getch() == 27;
    return false;
}

// Get vendor name from vendor ID
static std::string GetVendorName(uint32_t vendorId) {
    switch (vendorId) {
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD";
        case 0x8086: return "Intel";
        case 0x1414: return "Microsoft";
        default:     return "Unknown";
    }
}

// Calculate standard deviation
static double CalculateStdDev(const std::vector<double>& values, double mean) {
    if (values.size() < 2) return 0.0;
    double sumSqDiff = 0.0;
    for (double v : values) {
        sumSqDiff += (v - mean) * (v - mean);
    }
    return std::sqrt(sumSqDiff / (values.size() - 1));
}

// ============================================================================
//                        BANDWIDTH ANALYSIS
// ============================================================================

BandwidthAnalysis AnalyzeBandwidth(double measuredBandwidth_gbps) {
    BandwidthAnalysis result;
    result.likelyInterface = nullptr;
    result.percentOfTheoretical = 0.0;
    result.isRealistic = false;
    
    double bestMatch = -1;
    double bestDiff = 1e9;
    
    // Find best match in realistic range (60-95% of theoretical)
    for (int i = 0; i < NUM_INTERFACES; i++) {
        double theoretical = INTERFACE_SPEEDS[i].bandwidth_gbps;
        double percent = (measuredBandwidth_gbps / theoretical) * 100.0;
        
        if (percent >= Constants::REALISTIC_MIN_PERCENT && 
            percent <= Constants::REALISTIC_MAX_PERCENT) {
            double diff = std::abs(theoretical - measuredBandwidth_gbps);
            if (diff < bestDiff) {
                bestDiff = diff;
                bestMatch = i;
            }
        }
    }
    
    if (bestMatch >= 0) {
        result.likelyInterface = &INTERFACE_SPEEDS[(int)bestMatch];
        result.percentOfTheoretical = (measuredBandwidth_gbps / result.likelyInterface->bandwidth_gbps) * 100.0;
        result.isRealistic = true;
        return result;
    }
    
    // No realistic match - find closest by absolute value
    for (int i = 0; i < NUM_INTERFACES; i++) {
        double theoretical = INTERFACE_SPEEDS[i].bandwidth_gbps;
        double diff = std::abs(theoretical - measuredBandwidth_gbps);
        if (diff < bestDiff) {
            bestDiff = diff;
            bestMatch = i;
        }
    }
    
    if (bestMatch >= 0) {
        result.likelyInterface = &INTERFACE_SPEEDS[(int)bestMatch];
        result.percentOfTheoretical = (measuredBandwidth_gbps / result.likelyInterface->bandwidth_gbps) * 100.0;
        result.isRealistic = false;
    }
    
    return result;
}

// Print interface guess based on upload and download bandwidth
void PrintInterfaceGuess(double uploadBW, double downloadBW) {
    double avgBW = (uploadBW + downloadBW) / 2.0;
    double maxBW = std::max(uploadBW, downloadBW);
    
    BandwidthAnalysis analysis = AnalyzeBandwidth(maxBW);
    
    std::cout << "\n";
    PrintDoubleDivider();
    std::cout << "Interface Detection\n";
    PrintDoubleDivider();
    std::cout << "\n";
    
    if (analysis.likelyInterface) {
        std::cout << "+-- Likely Connection Type ---------------------\n";
        std::cout << "|\n";
        std::cout << "|  " << analysis.likelyInterface->name << "\n";
        std::cout << "|  " << analysis.likelyInterface->description << "\n";
        std::cout << "|\n";
        
        double uploadPercent = (uploadBW / analysis.likelyInterface->bandwidth_gbps) * 100.0;
        double downloadPercent = (downloadBW / analysis.likelyInterface->bandwidth_gbps) * 100.0;
        double avgPercent = (avgBW / analysis.likelyInterface->bandwidth_gbps) * 100.0;
        
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "|  Upload:   " << uploadBW << " GB/s  (" 
                  << std::setprecision(1) << uploadPercent << "% of " 
                  << analysis.likelyInterface->name << ")\n";
        std::cout << "|  Download: " << std::setprecision(2) << downloadBW << " GB/s  (" 
                  << std::setprecision(1) << downloadPercent << "% of " 
                  << analysis.likelyInterface->name << ")\n";
        std::cout << "|\n";
        
        if (avgPercent >= Constants::EXCEPTIONAL_THRESHOLD) {
            std::cout << "|  [i] Exceptionally high efficiency - excellent!\n";
        } else if (analysis.isRealistic) {
            std::cout << "|  [OK] Performance is as expected for this interface\n";
        } else if (avgPercent < Constants::REALISTIC_MIN_PERCENT) {
            std::cout << "|  [!] Lower than expected - possible bottleneck\n";
        }
        
        std::cout << "|\n";
        std::cout << "+-----------------------------------------------\n";
    }
    
    PrintDoubleDivider();
}

// Print reference chart of interface speeds
void PrintInterfaceReferenceChart() {
    std::cout << "\n";
    PrintDoubleDivider();
    std::cout << "PCIe & Thunderbolt Bandwidth Reference Chart\n";
    PrintDoubleDivider();
    
    std::cout << "\nPCIe 3.0 (2010-2016 platforms):\n";
    PrintDivider();
    std::cout << "  x1   ~1.0 GB/s   | x4   ~4.0 GB/s\n";
    std::cout << "  x8   ~7.9 GB/s   | x16  ~15.8 GB/s\n";
    
    std::cout << "\nPCIe 4.0 (2019+ AMD, 2021+ Intel):\n";
    PrintDivider();
    std::cout << "  x1   ~2.0 GB/s   | x4   ~7.9 GB/s\n";
    std::cout << "  x8   ~15.8 GB/s  | x16  ~31.5 GB/s\n";
    
    std::cout << "\nPCIe 5.0 (2022+ latest platforms):\n";
    PrintDivider();
    std::cout << "  x1   ~3.9 GB/s   | x4   ~15.8 GB/s\n";
    std::cout << "  x8   ~31.5 GB/s  | x16  ~63.0 GB/s\n";
    
    std::cout << "\nThunderbolt / External:\n";
    PrintDivider();
    std::cout << "  TB3/4    ~2.75 GB/s  | TB5      ~6.0 GB/s\n";
    std::cout << "  USB4     ~4.8 GB/s   | OCuLink  ~7.9 GB/s (Gen4)\n";
    
    std::cout << "\nNote: Real-world is typically 60-90% of theoretical.\n";
    PrintDoubleDivider();
}


// ============================================================================
//                            CSV LOGGER
// ============================================================================

class CSVLogger {
public:
    CSVLogger(const std::string& filename) : filename_(filename) {
        std::ifstream check(filename);
        bool exists = check.good();
        check.close();

        file_.open(filename, std::ios::app);
        if (!exists && file_.is_open()) {
            // Header includes run number for multi-run logging
            file_ << "Timestamp,API,Test Name,Run,Min,Avg,Max,P99,P99.9,StdDev,Unit\n";
        }
    }

    ~CSVLogger() { 
        if (file_.is_open()) file_.close(); 
    }

    // Log a single test result
    void LogResult(const TestResults& r, const std::string& api, int runNumber = 0) {
        if (!file_.is_open()) return;
        auto t = std::chrono::system_clock::to_time_t(r.timestamp);
        std::tm tm; 
        localtime_s(&tm, &t);
        
        file_ << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << ","
              << api << "," 
              << r.testName << ","
              << runNumber << ","
              << r.minValue << "," 
              << r.avgValue << "," 
              << r.maxValue << ","
              << r.p99Value << "," 
              << r.p999Value << ","
              << "," // No stddev for single run
              << r.unit << "\n";
        file_.flush();
    }

    // Log aggregated results (averaged across multiple runs)
    void LogAggregated(const AggregatedResults& r, const std::string& api) {
        if (!file_.is_open()) return;
        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm; 
        localtime_s(&tm, &t);
        
        file_ << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << ","
              << api << "," 
              << r.testName << ","
              << "AVG," // Indicates this is an averaged result
              << r.avgMin << "," 
              << r.avgAvg << "," 
              << r.avgMax << ","
              << r.avgP99 << "," 
              << r.avgP999 << ","
              << r.stdDevAvg << ","
              << r.unit << "\n";
        file_.flush();
    }

private:
    std::string filename_;
    std::ofstream file_;
};

// ============================================================================
//                        D3D12 GPU DETECTION
// ============================================================================

GPUInfo GetD3D12GPUInfo(IDXGIAdapter1* adapter) {
    GPUInfo info;
    
    DXGI_ADAPTER_DESC1 desc;
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
        // Convert wide string to narrow string for GPU name
        char nameBuffer[256];
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nameBuffer, sizeof(nameBuffer), nullptr, nullptr);
        info.name = nameBuffer;
        
        info.vendorId = desc.VendorId;
        info.deviceId = desc.DeviceId;
        info.vendor = GetVendorName(desc.VendorId);
        info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
        info.sharedSystemMemory = desc.SharedSystemMemory;
    }
    
    // Try to get extended info via DXGI 1.6 (DXGI_ADAPTER_DESC3)
    // This requires QueryInterface to IDXGIAdapter4
    ComPtr<IDXGIAdapter4> adapter4;
    if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter4)))) {
        DXGI_ADAPTER_DESC3 desc3;
        if (SUCCEEDED(adapter4->GetDesc3(&desc3))) {
            // DXGI_ADAPTER_DESC3 doesn't directly expose PCIe info,
            // but we can check memory and other flags
            // The GraphicsPreemptionGranularity and ComputePreemptionGranularity 
            // can give hints about GPU capabilities
        }
    }
    
    // Note: To get actual PCIe link status on Windows, you would need to:
    // 1. Use SetupAPI to enumerate PCI devices
    // 2. Read PCI Express Capability Structure
    // 3. Parse Link Status Register (offset 0x12 from PCIe cap)
    // This is complex and requires admin privileges, so we estimate from bandwidth
    
    return info;
}

void PrintD3D12GPUInfo(const GPUInfo& info) {
    std::cout << "\n";
    PrintDoubleDivider();
    std::cout << "GPU Information (D3D12)\n";
    PrintDoubleDivider();
    
    std::cout << "  Name:        " << info.name << "\n";
    std::cout << "  Vendor:      " << info.vendor;
    std::cout << " (0x" << std::hex << info.vendorId << std::dec << ")\n";
    std::cout << "  Device ID:   0x" << std::hex << info.deviceId << std::dec << "\n";
    std::cout << "  Dedicated VRAM: " << FormatMemorySize(info.dedicatedVideoMemory) << "\n";
    std::cout << "  Shared Memory:  " << FormatMemorySize(info.sharedSystemMemory) << "\n";
    
    if (info.dedicatedVideoMemory == 0) {
        std::cout << "\n  [i] This appears to be an integrated GPU (APU/iGPU)\n";
        std::cout << "      Memory transfers may not traverse a real PCIe bus.\n";
    }
    
    PrintDoubleDivider();
}

// ============================================================================
//                        D3D12 HELPER FUNCTIONS
// ============================================================================

// Create a committed buffer resource
// heapType determines where the buffer lives:
//   D3D12_HEAP_TYPE_DEFAULT  - GPU-only memory (fastest for GPU access)
//   D3D12_HEAP_TYPE_UPLOAD   - CPU-writable, GPU-readable (for uploads)
//   D3D12_HEAP_TYPE_READBACK - GPU-writable, CPU-readable (for downloads)
static ComPtr<ID3D12Resource> MakeBuffer(
    ID3D12Device* dev, 
    D3D12_HEAP_TYPE heapType, 
    size_t size, 
    D3D12_RESOURCE_STATES state) 
{
    D3D12_HEAP_PROPERTIES heap{ heapType };
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width              = size;
    desc.Height             = 1;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.SampleDesc.Count   = 1;
    desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> res;
    dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res));
    return res;
}

// Wait for GPU to complete all queued work
static void WaitForFence(ID3D12CommandQueue* q, ID3D12Fence* f, HANDLE ev, UINT64& val) {
    q->Signal(f, val);
    if (f->GetCompletedValue() < val) {
        f->SetEventOnCompletion(val, ev);
        WaitForSingleObject(ev, INFINITE);
    }
    val++;
}

// GPU selection for D3D12
ComPtr<IDXGIAdapter1> SelectD3D12GPU(ComPtr<IDXGIFactory6> factory, bool showInfo, int gpuIndex = 0) {
    std::vector<ComPtr<IDXGIAdapter1>> adapters;
    ComPtr<IDXGIAdapter1> adapter;
    
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        adapters.push_back(adapter);
    }

    if (adapters.empty()) {
        std::cerr << "No DirectX 12 capable GPU found!\n";
        exit(1);
    }

    // CLI GPU selection
    if (gpuIndex > 0 && gpuIndex <= (int)adapters.size()) {
        GPUInfo info = GetD3D12GPUInfo(adapters[gpuIndex - 1].Get());
        std::cout << "Using GPU #" << gpuIndex << ": " << info.name << "\n";
        if (showInfo) PrintD3D12GPUInfo(info);
        return adapters[gpuIndex - 1];
    }

    if (adapters.size() == 1) {
        GPUInfo info = GetD3D12GPUInfo(adapters[0].Get());
        if (showInfo) {
            PrintD3D12GPUInfo(info);
        } else {
            std::cout << "Found 1 GPU: " << info.name << "\n";
            PrintDoubleDivider();
        }
        return adapters[0];
    }

    // Multiple GPUs - let user choose
    std::cout << "Found " << adapters.size() << " GPUs:\n";
    PrintDoubleDivider();
    
    for (size_t i = 0; i < adapters.size(); i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapters[i]->GetDesc1(&desc);
        
        char nameBuffer[256];
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nameBuffer, sizeof(nameBuffer), nullptr, nullptr);
        
        std::cout << (i + 1) << ". " << nameBuffer << "\n";
        std::cout << "   VRAM: " << (desc.DedicatedVideoMemory / (1024 * 1024)) << " MB\n";
        std::cout << "   Vendor: " << GetVendorName(desc.VendorId) << "\n\n";
    }

    std::cout << "Select GPU (1-" << adapters.size() << ", default=1): ";
    std::string input;
    std::getline(std::cin, input);
    
    int choice = 1;
    if (!input.empty()) {
        try { choice = std::stoi(input); } catch (...) { choice = 1; }
    }
    
    choice = std::max(1, std::min(choice, (int)adapters.size()));
    
    GPUInfo info = GetD3D12GPUInfo(adapters[choice - 1].Get());
    std::cout << "\nSelected: " << info.name << "\n";
    
    if (showInfo) {
        PrintD3D12GPUInfo(info);
    } else {
        PrintDoubleDivider();
    }
    
    return adapters[choice - 1];
}

// ============================================================================
//                      D3D12 BENCHMARK TESTS
// ============================================================================

// Bandwidth test - measures throughput for large transfers
TestResults RunBandwidthTestD3D12(
    const char* name, 
    ID3D12Device* device, 
    ID3D12CommandQueue* queue, 
    ID3D12CommandAllocator* allocator, 
    ID3D12GraphicsCommandList* list, 
    ID3D12Fence* fence, 
    HANDLE fenceEvent, 
    UINT64& fenceValue,
    ComPtr<ID3D12Resource> src, 
    ComPtr<ID3D12Resource> dst, 
    size_t bufferSize, 
    int copiesPerBatch, 
    int batches, 
    bool continuousMode) 
{
    std::cout << name << "\n";
    PrintDivider();

    // Create timestamp query heap for GPU-side timing
    D3D12_QUERY_HEAP_DESC qhd{}; 
    qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; 
    qhd.Count = 2;
    ComPtr<ID3D12QueryHeap> queryHeap; 
    device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&queryHeap));
    
    auto queryReadback = MakeBuffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(UINT64) * 2, D3D12_RESOURCE_STATE_COPY_DEST);
    
    UINT64 timestampFreq = 0; 
    queue->GetTimestampFrequency(&timestampFreq);

    std::vector<double> bandwidths;
    bandwidths.reserve(batches);
    int lastPercent = -1;
    bool escaped = false;

    for (int i = 0; i < batches && !escaped; i++) {
        allocator->Reset();
        list->Reset(allocator, nullptr);

        // Record GPU timestamp before copies
        list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        
        // Perform multiple copies in this batch for better accuracy
        for (int j = 0; j < copiesPerBatch; j++) {
            list->CopyResource(dst.Get(), src.Get());
        }
        
        // Record GPU timestamp after copies
        list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);

        list->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, queryReadback.Get(), 0);
        list->Close();

        ID3D12CommandList* cl[] = { list };
        queue->ExecuteCommandLists(1, cl);
        WaitForFence(queue, fence, fenceEvent, fenceValue);

        // Read back timestamps
        UINT64* ts = nullptr;
        D3D12_RANGE range{ 0, sizeof(UINT64) * 2 };
        queryReadback->Map(0, &range, (void**)&ts);
        double seconds = double(ts[1] - ts[0]) / double(timestampFreq);
        queryReadback->Unmap(0, nullptr);

        double bytes = double(bufferSize) * copiesPerBatch;
        bandwidths.push_back((bytes / (1024.0 * 1024.0 * 1024.0)) / seconds);

        int percent = int((i + 1) * 100 / batches);
        if (percent != lastPercent) { 
            std::cout << "\rProgress: " << percent << "% ";
            if (continuousMode) std::cout << "(Press ESC to stop) ";
            std::cout << std::flush;
            lastPercent = percent;
        }

        if (continuousMode && CheckForEscape()) {
            escaped = true;
            std::cout << "\n[Stopped by user]\n";
        }
    }
    std::cout << "\n";

    std::sort(bandwidths.begin(), bandwidths.end());

    TestResults results;
    results.testName = name;
    results.minValue = bandwidths.front();
    results.avgValue = std::accumulate(bandwidths.begin(), bandwidths.end(), 0.0) / bandwidths.size();
    results.maxValue = bandwidths.back();
    results.p99Value = bandwidths[static_cast<size_t>(bandwidths.size() * 0.99)];
    results.p999Value = bandwidths[static_cast<size_t>(bandwidths.size() * 0.999)];
    results.unit = "GB/s";
    results.timestamp = std::chrono::system_clock::now();

    PrintDivider();
    std::cout << "Results:\n";
    std::cout << "  Min : " << results.minValue << " GB/s\n";
    std::cout << "  Avg : " << results.avgValue << " GB/s\n";
    std::cout << "  Max : " << results.maxValue << " GB/s\n";

    return results;
}

// Transfer latency test - measures round-trip time for small transfers
TestResults RunLatencyTestD3D12(
    const char* name, 
    ID3D12Device* device, 
    ID3D12CommandQueue* queue, 
    ID3D12CommandAllocator* allocator, 
    ID3D12GraphicsCommandList* list, 
    ID3D12Fence* fence, 
    HANDLE fenceEvent, 
    UINT64& fenceValue, 
    ComPtr<ID3D12Resource> src, 
    ComPtr<ID3D12Resource> dst, 
    int iterations, 
    bool continuousMode) 
{
    std::cout << name << "\n";
    PrintDivider();

    std::vector<double> latencies;
    latencies.reserve(iterations);
    int lastPercent = -1;
    bool escaped = false;

    for (int i = 0; i < iterations && !escaped; i++) {
        allocator->Reset();
        list->Reset(allocator, nullptr);

        // Measure wall-clock time for full round-trip:
        // submit -> GPU execute -> fence signal -> CPU wait
        auto start = std::chrono::high_resolution_clock::now();
        
        list->CopyResource(dst.Get(), src.Get());
        list->Close();

        ID3D12CommandList* cl[] = { list };
        queue->ExecuteCommandLists(1, cl);
        WaitForFence(queue, fence, fenceEvent, fenceValue);
        
        auto end = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(us);

        int percent = int((i + 1) * 100 / iterations);
        if (percent != lastPercent) { 
            std::cout << "\rProgress: " << percent << "% ";
            if (continuousMode) std::cout << "(Press ESC to stop) ";
            std::cout << std::flush;
            lastPercent = percent;
        }

        if (continuousMode && CheckForEscape()) {
            escaped = true;
            std::cout << "\n[Stopped by user]\n";
        }
    }
    std::cout << "\n";

    std::sort(latencies.begin(), latencies.end());

    TestResults results;
    results.testName = name;
    results.minValue = latencies.front();
    results.avgValue = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    results.maxValue = latencies.back();
    results.p99Value = latencies[static_cast<size_t>(latencies.size() * 0.99)];
    results.p999Value = latencies[static_cast<size_t>(latencies.size() * 0.999)];
    results.unit = "us";
    results.timestamp = std::chrono::system_clock::now();

    PrintDivider();
    std::cout << "Results:\n";
    std::cout << "  Min        : " << std::fixed << std::setprecision(2) << results.minValue << " us\n";
    std::cout << "  Avg        : " << results.avgValue << " us\n";
    std::cout << "  Max        : " << results.maxValue << " us\n";

    return results;
}

// Command latency test - measures empty command submission overhead
TestResults RunCommandLatencyTestD3D12(
    ID3D12CommandQueue* queue, 
    ID3D12CommandAllocator* allocator, 
    ID3D12GraphicsCommandList* list, 
    ID3D12Fence* fence, 
    HANDLE fenceEvent, 
    UINT64& fenceValue, 
    int iterations, 
    bool continuousMode) 
{
    const char* name = "Command Latency";
    std::cout << name << "\n";
    PrintDivider();

    std::vector<double> latencies;
    latencies.reserve(iterations);
    int lastPercent = -1;
    bool escaped = false;

    for (int i = 0; i < iterations && !escaped; i++) {
        auto start = std::chrono::high_resolution_clock::now();

        allocator->Reset();
        list->Reset(allocator, nullptr);
        list->Close();  // Empty command list

        ID3D12CommandList* cl[] = { list };
        queue->ExecuteCommandLists(1, cl);
        WaitForFence(queue, fence, fenceEvent, fenceValue);

        auto end = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(us);

        int percent = int((i + 1) * 100 / iterations);
        if (percent != lastPercent) { 
            std::cout << "\rProgress: " << percent << "% ";
            if (continuousMode) std::cout << "(Press ESC to stop) ";
            std::cout << std::flush;
            lastPercent = percent;
        }

        if (continuousMode && CheckForEscape()) {
            escaped = true;
            std::cout << "\n[Stopped by user]\n";
        }
    }
    std::cout << "\n";

    std::sort(latencies.begin(), latencies.end());

    TestResults results;
    results.testName = name;
    results.minValue = latencies.front();
    results.avgValue = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    results.maxValue = latencies.back();
    results.p99Value = latencies[static_cast<size_t>(latencies.size() * 0.99)];
    results.p999Value = latencies[static_cast<size_t>(latencies.size() * 0.999)];
    results.unit = "us";
    results.timestamp = std::chrono::system_clock::now();

    PrintDivider();
    std::cout << "Results:\n";
    std::cout << "  Min        : " << std::fixed << std::setprecision(2) << results.minValue << " us\n";
    std::cout << "  Avg        : " << results.avgValue << " us\n";
    std::cout << "  Max        : " << results.maxValue << " us\n";

    return results;
}

// Bidirectional bandwidth test - simultaneous upload and download
TestResults RunBidirectionalTestD3D12(
    const char* name,
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12CommandAllocator* allocator,
    ID3D12GraphicsCommandList* list,
    ID3D12Fence* fence,
    HANDLE fenceEvent,
    UINT64& fenceValue,
    size_t bufferSize,
    int copiesPerBatch,
    int batches,
    bool continuousMode)
{
    std::cout << name << "\n";
    PrintDivider();
    std::cout << "(Simultaneous CPU->GPU and GPU->CPU transfers)\n";

    // Create buffers for both directions
    auto cpuUpload = MakeBuffer(device, D3D12_HEAP_TYPE_UPLOAD, bufferSize, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto gpuDefault = MakeBuffer(device, D3D12_HEAP_TYPE_DEFAULT, bufferSize, D3D12_RESOURCE_STATE_COPY_DEST);
    auto gpuSrc = MakeBuffer(device, D3D12_HEAP_TYPE_DEFAULT, bufferSize, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto cpuReadback = MakeBuffer(device, D3D12_HEAP_TYPE_READBACK, bufferSize, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_QUERY_HEAP_DESC qhd{}; 
    qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; 
    qhd.Count = 2;
    ComPtr<ID3D12QueryHeap> queryHeap; 
    device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&queryHeap));
    
    auto queryReadback = MakeBuffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(UINT64) * 2, D3D12_RESOURCE_STATE_COPY_DEST);
    
    UINT64 timestampFreq = 0; 
    queue->GetTimestampFrequency(&timestampFreq);

    std::vector<double> bandwidths;
    bandwidths.reserve(batches);
    int lastPercent = -1;
    bool escaped = false;

    for (int i = 0; i < batches && !escaped; i++) {
        allocator->Reset();
        list->Reset(allocator, nullptr);

        list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        
        // Interleave upload and download copies
        for (int j = 0; j < copiesPerBatch; j++) {
            list->CopyResource(gpuDefault.Get(), cpuUpload.Get());  // Upload
            list->CopyResource(cpuReadback.Get(), gpuSrc.Get());    // Download
        }
        
        list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, queryReadback.Get(), 0);
        list->Close();

        ID3D12CommandList* cl[] = { list };
        queue->ExecuteCommandLists(1, cl);
        WaitForFence(queue, fence, fenceEvent, fenceValue);

        UINT64* ts = nullptr;
        D3D12_RANGE range{ 0, sizeof(UINT64) * 2 };
        queryReadback->Map(0, &range, (void**)&ts);
        double seconds = double(ts[1] - ts[0]) / double(timestampFreq);
        queryReadback->Unmap(0, nullptr);

        // Total bytes = upload + download
        double bytes = double(bufferSize) * copiesPerBatch * 2;
        bandwidths.push_back((bytes / (1024.0 * 1024.0 * 1024.0)) / seconds);

        int percent = int((i + 1) * 100 / batches);
        if (percent != lastPercent) { 
            std::cout << "\rProgress: " << percent << "% ";
            if (continuousMode) std::cout << "(Press ESC to stop) ";
            std::cout << std::flush;
            lastPercent = percent;
        }

        if (continuousMode && CheckForEscape()) {
            escaped = true;
            std::cout << "\n[Stopped by user]\n";
        }
    }
    std::cout << "\n";

    std::sort(bandwidths.begin(), bandwidths.end());

    TestResults results;
    results.testName = name;
    results.minValue = bandwidths.front();
    results.avgValue = std::accumulate(bandwidths.begin(), bandwidths.end(), 0.0) / bandwidths.size();
    results.maxValue = bandwidths.back();
    results.p99Value = bandwidths[static_cast<size_t>(bandwidths.size() * 0.99)];
    results.p999Value = bandwidths[static_cast<size_t>(bandwidths.size() * 0.999)];
    results.unit = "GB/s";
    results.timestamp = std::chrono::system_clock::now();

    PrintDivider();
    std::cout << "Results (combined upload + download):\n";
    std::cout << "  Min : " << results.minValue << " GB/s\n";
    std::cout << "  Avg : " << results.avgValue << " GB/s\n";
    std::cout << "  Max : " << results.maxValue << " GB/s\n";

    return results;
}

// ============================================================================
//                         VULKAN SETUP
// ============================================================================

// Vulkan error checking macro
#define VK_CHECK(x) do { VkResult r = (x); if (r != VK_SUCCESS) { \
    std::cerr << "Vulkan error: " << r << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
    exit(1); } } while(0)

// Vulkan context - holds all Vulkan objects needed for benchmarking
struct VulkanContext {
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkQueue queue;
    VkCommandPool commandPool;
    uint32_t queueFamilyIndex;
    float timestampPeriod;  // Nanoseconds per timestamp tick
    
    GPUInfo gpuInfo;        // Detected GPU information

    void Init(int gpuIndex = 0) {
        // Create Vulkan instance
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "GPU-PCIe-Test";
        appInfo.applicationVersion = VK_MAKE_VERSION(2, 0, 0);
        appInfo.pEngineName = "None";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &appInfo;

        VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

        // Select physical device (GPU)
        SelectPhysicalDevice(gpuIndex);
        
        // Get queue family that supports transfer operations
        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfProps(qfCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qfCount, qfProps.data());

        queueFamilyIndex = UINT32_MAX;
        for (uint32_t i = 0; i < qfCount; i++) {
            // Prefer a queue with both graphics and transfer for best compatibility
            if (qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                queueFamilyIndex = i;
                break;
            }
        }
        if (queueFamilyIndex == UINT32_MAX) {
            // Fall back to any queue with transfer capability
            for (uint32_t i = 0; i < qfCount; i++) {
                if (qfProps[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
                    queueFamilyIndex = i;
                    break;
                }
            }
        }

        // Create logical device
        float priority = 1.0f;
        VkDeviceQueueCreateInfo dqci{};
        dqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        dqci.queueFamilyIndex = queueFamilyIndex;
        dqci.queueCount = 1;
        dqci.pQueuePriorities = &priority;

        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &dqci;

        VK_CHECK(vkCreateDevice(physicalDevice, &dci, nullptr, &device));
        vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

        // Create command pool
        // VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT allows reusing command buffers
        VkCommandPoolCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.queueFamilyIndex = queueFamilyIndex;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        VK_CHECK(vkCreateCommandPool(device, &cpci, nullptr, &commandPool));
    }

    void SelectPhysicalDevice(int gpuIndex = 0) {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        
        if (deviceCount == 0) {
            std::cerr << "No Vulkan-capable GPU found!\n";
            exit(1);
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        // CLI GPU selection
        if (gpuIndex > 0 && gpuIndex <= (int)deviceCount) {
            physicalDevice = devices[gpuIndex - 1];
            GetGPUInfo();
            std::cout << "Using GPU #" << gpuIndex << ": " << gpuInfo.name << "\n";
            return;
        }

        if (deviceCount == 1) {
            physicalDevice = devices[0];
            GetGPUInfo();
            std::cout << "Found 1 GPU: " << gpuInfo.name << "\n";
            PrintDoubleDivider();
            return;
        }

        // Multiple GPUs - let user choose
        std::cout << "Found " << deviceCount << " Vulkan GPUs:\n";
        PrintDoubleDivider();
        
        for (size_t i = 0; i < devices.size(); i++) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(devices[i], &props);
            std::cout << (i + 1) << ". " << props.deviceName << "\n";
        }

        std::cout << "\nSelect GPU (1-" << deviceCount << ", default=1): ";
        std::string input;
        std::getline(std::cin, input);
        
        int choice = 1;
        if (!input.empty()) {
            try { choice = std::stoi(input); } catch (...) { choice = 1; }
        }
        choice = std::max(1, std::min(choice, (int)deviceCount));
        
        physicalDevice = devices[choice - 1];
        GetGPUInfo();
        std::cout << "\nSelected: " << gpuInfo.name << "\n";
        PrintDoubleDivider();
    }

    void GetGPUInfo() {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physicalDevice, &props);
        
        gpuInfo.name = props.deviceName;
        gpuInfo.vendorId = props.vendorID;
        gpuInfo.deviceId = props.deviceID;
        gpuInfo.vendor = GetVendorName(props.vendorID);
        
        // Timestamp period for converting GPU ticks to nanoseconds
        timestampPeriod = props.limits.timestampPeriod;
        
        // Get memory information
        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
        
        gpuInfo.dedicatedVideoMemory = 0;
        gpuInfo.sharedSystemMemory = 0;
        
        for (uint32_t i = 0; i < memProps.memoryHeapCount; i++) {
            if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                gpuInfo.dedicatedVideoMemory += memProps.memoryHeaps[i].size;
            } else {
                gpuInfo.sharedSystemMemory += memProps.memoryHeaps[i].size;
            }
        }
        
        // Driver version (vendor-specific encoding)
        uint32_t driverVersion = props.driverVersion;
        if (gpuInfo.vendorId == 0x10DE) {
            // NVIDIA encoding
            gpuInfo.driverVersion = std::to_string((driverVersion >> 22) & 0x3FF) + "." +
                                    std::to_string((driverVersion >> 14) & 0xFF) + "." +
                                    std::to_string((driverVersion >> 6) & 0xFF);
        } else {
            // Standard Vulkan encoding
            gpuInfo.driverVersion = std::to_string(VK_VERSION_MAJOR(driverVersion)) + "." +
                                    std::to_string(VK_VERSION_MINOR(driverVersion)) + "." +
                                    std::to_string(VK_VERSION_PATCH(driverVersion));
        }
    }

    void PrintGPUInfo() {
        std::cout << "\n";
        PrintDoubleDivider();
        std::cout << "GPU Information (Vulkan)\n";
        PrintDoubleDivider();
        
        std::cout << "  Name:           " << gpuInfo.name << "\n";
        std::cout << "  Vendor:         " << gpuInfo.vendor;
        std::cout << " (0x" << std::hex << gpuInfo.vendorId << std::dec << ")\n";
        std::cout << "  Driver Version: " << gpuInfo.driverVersion << "\n";
        std::cout << "  Device Local Memory: " << FormatMemorySize(gpuInfo.dedicatedVideoMemory) << "\n";
        std::cout << "  Host Visible Memory: " << FormatMemorySize(gpuInfo.sharedSystemMemory) << "\n";
        std::cout << "  Timestamp Period: " << timestampPeriod << " ns/tick\n";
        
        if (gpuInfo.dedicatedVideoMemory == 0) {
            std::cout << "\n  [i] This appears to be an integrated GPU (APU/iGPU)\n";
        }
        
        PrintDoubleDivider();
    }

    void Cleanup() {
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
    }
};

// ============================================================================
//                       VULKAN BUFFER HELPER
// ============================================================================

// Vulkan buffer wrapper with automatic memory allocation
struct Buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size;

    // Create a buffer with specified usage and memory properties
    // memoryProperties flags:
    //   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT - GPU-only memory (fastest for GPU)
    //   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT - CPU can map and access this memory
    //   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT - CPU writes are automatically visible to GPU
    //                                          (without explicit flush/invalidate)
    //   VK_MEMORY_PROPERTY_HOST_CACHED_BIT - CPU reads are cached (faster reads, slower writes)
    void Create(const VulkanContext& ctx, VkDeviceSize bufferSize, VkBufferUsageFlags usage, VkMemoryPropertyFlags memoryProperties) {
        size = bufferSize;

        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bufferSize;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VK_CHECK(vkCreateBuffer(ctx.device, &bci, nullptr, &buffer));

        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(ctx.device, buffer, &memReq);

        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(ctx.physicalDevice, &memProps);

        uint32_t memTypeIndex = UINT32_MAX;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((memReq.memoryTypeBits & (1 << i)) &&
                (memProps.memoryTypes[i].propertyFlags & memoryProperties) == memoryProperties) {
                memTypeIndex = i;
                break;
            }
        }

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReq.size;
        mai.memoryTypeIndex = memTypeIndex;

        VK_CHECK(vkAllocateMemory(ctx.device, &mai, nullptr, &memory));
        VK_CHECK(vkBindBufferMemory(ctx.device, buffer, memory, 0));
    }

    void Destroy(const VulkanContext& ctx) {
        vkDestroyBuffer(ctx.device, buffer, nullptr);
        vkFreeMemory(ctx.device, memory, nullptr);
    }
};

// Command buffer helpers
VkCommandBuffer BeginCommandBuffer(const VulkanContext& ctx) {
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = ctx.commandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;

    VkCommandBuffer cb;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device, &ai, &cb));

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VK_CHECK(vkBeginCommandBuffer(cb, &bi));

    return cb;
}

void EndCommandBuffer(const VulkanContext& ctx, VkCommandBuffer cb) {
    VK_CHECK(vkEndCommandBuffer(cb));

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;

    VK_CHECK(vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.queue));

    vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &cb);
}

// ============================================================================
//                      VULKAN BENCHMARK TESTS
// ============================================================================

// Bandwidth test - measures throughput for large transfers
TestResults RunBandwidthTestVulkan(
    const char* name, 
    const VulkanContext& ctx, 
    Buffer& src, 
    Buffer& dst, 
    size_t size, 
    int copiesPerBatch, 
    int batches, 
    bool continuousMode) 
{
    std::cout << name << "\n";
    PrintDivider();

    VkCommandBuffer cb = BeginCommandBuffer(ctx);

    VkQueryPool queryPool;
    VkQueryPoolCreateInfo qpci{};
    qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = 2;

    VK_CHECK(vkCreateQueryPool(ctx.device, &qpci, nullptr, &queryPool));

    std::vector<double> bandwidths;
    bandwidths.reserve(batches);
    int lastPercent = -1;
    bool escaped = false;

    for (int i = 0; i < batches && !escaped; i++) {
        vkCmdResetQueryPool(cb, queryPool, 0, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);

        for (int j = 0; j < copiesPerBatch; j++) {
            VkBufferCopy copy{0, 0, size};
            vkCmdCopyBuffer(cb, src.buffer, dst.buffer, 1, &copy);
        }

        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);

        EndCommandBuffer(ctx, cb);
        cb = BeginCommandBuffer(ctx);

        uint64_t ts[2];
        VK_CHECK(vkGetQueryPoolResults(ctx.device, queryPool, 0, 2, sizeof(ts), ts, sizeof(uint64_t), 
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));

        double ns = double(ts[1] - ts[0]) * ctx.timestampPeriod;
        double seconds = ns / 1e9;
        double bytes = double(size) * copiesPerBatch;
        bandwidths.push_back((bytes / (1024.0 * 1024.0 * 1024.0)) / seconds);

        int percent = int((i + 1) * 100 / batches);
        if (percent != lastPercent) {
            std::cout << "\rProgress: " << percent << "% ";
            if (continuousMode) std::cout << "(Press ESC to stop) ";
            std::cout << std::flush;
            lastPercent = percent;
        }

        if (continuousMode && CheckForEscape()) {
            escaped = true;
            std::cout << "\n[Stopped by user]\n";
        }
    }

    std::cout << "\n";

    if (cb) EndCommandBuffer(ctx, cb);
    vkDestroyQueryPool(ctx.device, queryPool, nullptr);

    std::sort(bandwidths.begin(), bandwidths.end());

    TestResults results;
    results.testName = name;
    results.minValue = bandwidths.front();
    results.avgValue = std::accumulate(bandwidths.begin(), bandwidths.end(), 0.0) / bandwidths.size();
    results.maxValue = bandwidths.back();
    results.p99Value = bandwidths[static_cast<size_t>(bandwidths.size() * 0.99)];
    results.p999Value = bandwidths[static_cast<size_t>(bandwidths.size() * 0.999)];
    results.unit = "GB/s";
    results.timestamp = std::chrono::system_clock::now();

    PrintDivider();
    std::cout << "Results:\n";
    std::cout << "  Min : " << results.minValue << " GB/s\n";
    std::cout << "  Avg : " << results.avgValue << " GB/s\n";
    std::cout << "  Max : " << results.maxValue << " GB/s\n";

    return results;
}

// Transfer latency test - measures round-trip time for small transfers
TestResults RunLatencyTestVulkan(
    const char* name, 
    const VulkanContext& ctx, 
    Buffer& src, 
    Buffer& dst, 
    int iterations, 
    bool continuousMode) 
{
    std::cout << name << "\n";
    PrintDivider();

    VkCommandBuffer cmdBuf;
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = ctx.commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(ctx.device, &allocInfo, &cmdBuf);

    std::vector<double> latencies;
    latencies.reserve(iterations);
    int lastPercent = -1;
    bool escaped = false;

    for (int i = 0; i < iterations && !escaped; i++) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmdBuf, &beginInfo);

        VkBufferCopy copyRegion{0, 0, src.size};
        vkCmdCopyBuffer(cmdBuf, src.buffer, dst.buffer, 1, &copyRegion);
        vkEndCommandBuffer(cmdBuf);

        auto start = std::chrono::high_resolution_clock::now();
        
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBuf;
        vkQueueSubmit(ctx.queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(ctx.queue);
        
        auto end = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(us);

        int percent = int((i + 1) * 100 / iterations);
        if (percent != lastPercent) {
            std::cout << "\rProgress: " << percent << "% ";
            if (continuousMode) std::cout << "(Press ESC to stop) ";
            std::cout << std::flush;
            lastPercent = percent;
        }

        if (continuousMode && CheckForEscape()) {
            escaped = true;
            std::cout << "\n[Stopped by user]\n";
        }
    }

    std::cout << "\n";

    vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &cmdBuf);

    std::sort(latencies.begin(), latencies.end());

    TestResults results;
    results.testName = name;
    results.minValue = latencies.front();
    results.avgValue = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    results.maxValue = latencies.back();
    results.p99Value = latencies[static_cast<size_t>(latencies.size() * 0.99)];
    results.p999Value = latencies[static_cast<size_t>(latencies.size() * 0.999)];
    results.unit = "us";
    results.timestamp = std::chrono::system_clock::now();

    PrintDivider();
    std::cout << "Results:\n";
    std::cout << "  Min        : " << std::fixed << std::setprecision(2) << results.minValue << " us\n";
    std::cout << "  Avg        : " << results.avgValue << " us\n";
    std::cout << "  Max        : " << results.maxValue << " us\n";

    return results;
}

// Command latency test - measures empty command submission overhead
TestResults RunCommandLatencyTestVulkan(
    const VulkanContext& ctx, 
    int iterations, 
    bool continuousMode) 
{
    const char* name = "Command Latency";
    std::cout << name << "\n";
    PrintDivider();

    std::vector<double> latencies;
    latencies.reserve(iterations);
    int lastPercent = -1;
    bool escaped = false;

    for (int i = 0; i < iterations && !escaped; i++) {
        auto start = std::chrono::high_resolution_clock::now();

        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = ctx.commandPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;

        VkCommandBuffer cb;
        VK_CHECK(vkAllocateCommandBuffers(ctx.device, &ai, &cb));

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &bi));
        VK_CHECK(vkEndCommandBuffer(cb));

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        VK_CHECK(vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(ctx.queue));

        auto end = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(us);

        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &cb);

        int percent = int((i + 1) * 100 / iterations);
        if (percent != lastPercent) {
            std::cout << "\rProgress: " << percent << "% ";
            if (continuousMode) std::cout << "(Press ESC to stop) ";
            std::cout << std::flush;
            lastPercent = percent;
        }

        if (continuousMode && CheckForEscape()) {
            escaped = true;
            std::cout << "\n[Stopped by user]\n";
        }
    }

    std::cout << "\n";

    std::sort(latencies.begin(), latencies.end());

    TestResults results;
    results.testName = name;
    results.minValue = latencies.front();
    results.avgValue = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    results.maxValue = latencies.back();
    results.p99Value = latencies[static_cast<size_t>(latencies.size() * 0.99)];
    results.p999Value = latencies[static_cast<size_t>(latencies.size() * 0.999)];
    results.unit = "us";
    results.timestamp = std::chrono::system_clock::now();

    PrintDivider();
    std::cout << "Results:\n";
    std::cout << "  Min        : " << std::fixed << std::setprecision(2) << results.minValue << " us\n";
    std::cout << "  Avg        : " << results.avgValue << " us\n";
    std::cout << "  Max        : " << results.maxValue << " us\n";

    return results;
}

// Bidirectional bandwidth test - simultaneous upload and download
TestResults RunBidirectionalTestVulkan(
    const char* name,
    const VulkanContext& ctx,
    size_t bufferSize,
    int copiesPerBatch,
    int batches,
    bool continuousMode)
{
    std::cout << name << "\n";
    PrintDivider();
    std::cout << "(Simultaneous CPU->GPU and GPU->CPU transfers)\n";

    // Create buffers for both directions
    // Upload: HOST_VISIBLE -> DEVICE_LOCAL
    Buffer cpuSrc, gpuDst;
    cpuSrc.Create(ctx, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    gpuDst.Create(ctx, bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    // Download: DEVICE_LOCAL -> HOST_VISIBLE
    Buffer gpuSrc, cpuDst;
    gpuSrc.Create(ctx, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    cpuDst.Create(ctx, bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkCommandBuffer cb = BeginCommandBuffer(ctx);

    VkQueryPool queryPool;
    VkQueryPoolCreateInfo qpci{};
    qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = 2;

    VK_CHECK(vkCreateQueryPool(ctx.device, &qpci, nullptr, &queryPool));

    std::vector<double> bandwidths;
    bandwidths.reserve(batches);
    int lastPercent = -1;
    bool escaped = false;

    for (int i = 0; i < batches && !escaped; i++) {
        vkCmdResetQueryPool(cb, queryPool, 0, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);

        // Interleave upload and download copies
        for (int j = 0; j < copiesPerBatch; j++) {
            VkBufferCopy uploadCopy{0, 0, bufferSize};
            VkBufferCopy downloadCopy{0, 0, bufferSize};
            vkCmdCopyBuffer(cb, cpuSrc.buffer, gpuDst.buffer, 1, &uploadCopy);  // Upload
            vkCmdCopyBuffer(cb, gpuSrc.buffer, cpuDst.buffer, 1, &downloadCopy); // Download
        }

        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);

        EndCommandBuffer(ctx, cb);
        cb = BeginCommandBuffer(ctx);

        uint64_t ts[2];
        VK_CHECK(vkGetQueryPoolResults(ctx.device, queryPool, 0, 2, sizeof(ts), ts, sizeof(uint64_t), 
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));

        double ns = double(ts[1] - ts[0]) * ctx.timestampPeriod;
        double seconds = ns / 1e9;
        // Total bytes = upload + download
        double bytes = double(bufferSize) * copiesPerBatch * 2;
        bandwidths.push_back((bytes / (1024.0 * 1024.0 * 1024.0)) / seconds);

        int percent = int((i + 1) * 100 / batches);
        if (percent != lastPercent) {
            std::cout << "\rProgress: " << percent << "% ";
            if (continuousMode) std::cout << "(Press ESC to stop) ";
            std::cout << std::flush;
            lastPercent = percent;
        }

        if (continuousMode && CheckForEscape()) {
            escaped = true;
            std::cout << "\n[Stopped by user]\n";
        }
    }

    std::cout << "\n";

    if (cb) EndCommandBuffer(ctx, cb);
    vkDestroyQueryPool(ctx.device, queryPool, nullptr);
    
    cpuSrc.Destroy(ctx);
    gpuDst.Destroy(ctx);
    gpuSrc.Destroy(ctx);
    cpuDst.Destroy(ctx);

    std::sort(bandwidths.begin(), bandwidths.end());

    TestResults results;
    results.testName = name;
    results.minValue = bandwidths.front();
    results.avgValue = std::accumulate(bandwidths.begin(), bandwidths.end(), 0.0) / bandwidths.size();
    results.maxValue = bandwidths.back();
    results.p99Value = bandwidths[static_cast<size_t>(bandwidths.size() * 0.99)];
    results.p999Value = bandwidths[static_cast<size_t>(bandwidths.size() * 0.999)];
    results.unit = "GB/s";
    results.timestamp = std::chrono::system_clock::now();

    PrintDivider();
    std::cout << "Results (combined upload + download):\n";
    std::cout << "  Min : " << results.minValue << " GB/s\n";
    std::cout << "  Avg : " << results.avgValue << " GB/s\n";
    std::cout << "  Max : " << results.maxValue << " GB/s\n";

    return results;
}

// ============================================================================
//                     MULTI-RUN AGGREGATION
// ============================================================================

// Aggregate multiple test runs into a single result with statistics
AggregatedResults AggregateRuns(const std::vector<TestResults>& runs) {
    AggregatedResults agg;
    
    if (runs.empty()) return agg;
    
    agg.testName = runs[0].testName;
    agg.unit = runs[0].unit;
    agg.runs = runs;
    
    // Calculate averages
    double sumMin = 0, sumAvg = 0, sumMax = 0, sumP99 = 0, sumP999 = 0;
    std::vector<double> avgValues;
    
    for (const auto& r : runs) {
        sumMin += r.minValue;
        sumAvg += r.avgValue;
        sumMax += r.maxValue;
        sumP99 += r.p99Value;
        sumP999 += r.p999Value;
        avgValues.push_back(r.avgValue);
    }
    
    size_t n = runs.size();
    agg.avgMin = sumMin / n;
    agg.avgAvg = sumAvg / n;
    agg.avgMax = sumMax / n;
    agg.avgP99 = sumP99 / n;
    agg.avgP999 = sumP999 / n;
    
    // Calculate standard deviation of averages
    agg.stdDevAvg = CalculateStdDev(avgValues, agg.avgAvg);
    
    return agg;
}

// Print aggregated results
void PrintAggregatedResults(const AggregatedResults& agg, int numRuns) {
    std::cout << "\n";
    PrintDivider();
    std::cout << "Aggregated Results (" << numRuns << " runs):\n";
    PrintDivider();
    
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  " << agg.testName << ":\n";
    std::cout << "    Avg of Mins: " << agg.avgMin << " " << agg.unit << "\n";
    std::cout << "    Avg of Avgs: " << agg.avgAvg << " " << agg.unit;
    std::cout << " (StdDev: " << agg.stdDevAvg << ")\n";
    std::cout << "    Avg of Maxs: " << agg.avgMax << " " << agg.unit << "\n";
}

// ============================================================================
//                     CONFIGURATION MENU
// ============================================================================

void ShowConfigMenu(const BenchmarkConfig& config) {
    std::cout << "\n";
    PrintDoubleDivider();
    std::cout << "GPU Benchmark Configuration\n";
    PrintDoubleDivider();
    std::cout << "  1. Bandwidth buffer size: " << FormatSize(config.largeBandwidthSize) << "\n";
    std::cout << "  2. Latency buffer size:   " << FormatSize(config.smallLatencySize) << "\n";
    std::cout << "  3. Command iterations:    " << config.commandLatencyIters << "\n";
    std::cout << "  4. Transfer iterations:   " << config.transferLatencyIters << "\n";
    std::cout << "  5. Copies per batch:      " << config.copiesPerBatch << "\n";
    std::cout << "  6. Bandwidth batches:     " << config.bandwidthBatches << "\n";
    std::cout << "  7. Number of runs:        " << config.numRuns << " (multi-run averaging)\n";
    std::cout << "  8. Log all runs:          " << (config.logAllRuns ? "Yes" : "No (averages only)") << "\n";
    std::cout << "  9. Bidirectional test:    " << (config.enableBidirectionalTest ? "Enabled" : "Disabled") << "\n";
    std::cout << "  A. Detailed GPU info:     " << (config.showDetailedGPUInfo ? "Enabled" : "Disabled") << "\n";
    std::cout << "  C. Continuous mode:       " << (config.continuousMode ? "Enabled" : "Disabled") << "\n";
    std::cout << "  H. Show reference chart\n";
    std::cout << "  0. Start benchmark\n";
    PrintDivider();
}

void ConfigureSettings(BenchmarkConfig& config) {
    while (true) {
        ShowConfigMenu(config);
        std::cout << "Choice: ";
        
        char choice = (char)_getch();
        std::cout << choice << "\n";
        
        switch (choice) {
            case '1': {
                std::cout << "Enter bandwidth buffer size in MB: ";
                std::string input;
                std::getline(std::cin, input);
                try { config.largeBandwidthSize = std::stoull(input) * 1024 * 1024; } catch (...) {}
                break;
            }
            case '2': {
                std::cout << "Enter latency buffer size in bytes: ";
                std::string input;
                std::getline(std::cin, input);
                try { config.smallLatencySize = std::stoull(input); } catch (...) {}
                break;
            }
            case '3': {
                std::cout << "Enter command latency iterations: ";
                std::string input;
                std::getline(std::cin, input);
                try { config.commandLatencyIters = std::stoi(input); } catch (...) {}
                break;
            }
            case '4': {
                std::cout << "Enter transfer latency iterations: ";
                std::string input;
                std::getline(std::cin, input);
                try { config.transferLatencyIters = std::stoi(input); } catch (...) {}
                break;
            }
            case '5': {
                std::cout << "Enter copies per batch: ";
                std::string input;
                std::getline(std::cin, input);
                try { config.copiesPerBatch = std::stoi(input); } catch (...) {}
                break;
            }
            case '6': {
                std::cout << "Enter bandwidth batches: ";
                std::string input;
                std::getline(std::cin, input);
                try { config.bandwidthBatches = std::stoi(input); } catch (...) {}
                break;
            }
            case '7': {
                std::cout << "Enter number of runs (1-10): ";
                std::string input;
                std::getline(std::cin, input);
                try { 
                    int n = std::stoi(input); 
                    config.numRuns = std::max(Constants::MIN_NUM_RUNS, std::min(n, Constants::MAX_NUM_RUNS));
                } catch (...) {}
                break;
            }
            case '8':
                config.logAllRuns = !config.logAllRuns;
                break;
            case '9':
                config.enableBidirectionalTest = !config.enableBidirectionalTest;
                break;
            case 'A': case 'a':
                config.showDetailedGPUInfo = !config.showDetailedGPUInfo;
                break;
            case 'C': case 'c':
                config.continuousMode = !config.continuousMode;
                break;
            case 'H': case 'h':
                PrintInterfaceReferenceChart();
                std::cout << "\nPress any key to continue...\n";
                _getch();
                break;
            case '0':
            case '\r':
            case '\n':
                return;
            default:
                break;
        }
    }
}

// ============================================================================
//                          MAIN FUNCTION
// ============================================================================

// ============================================================================
//                        COMMAND LINE PARSING
// ============================================================================

struct CLIOptions {
    bool showHelp = false;
    bool hasOptions = false;
    bool quickMode = false;
    bool noBidir = false;
    bool noLatency = false;
    bool logAllRuns = false;
    int numRuns = 3;
    int gpuIndex = 0;        // 0 = prompt user, >0 = specific GPU
    size_t bufferSizeMB = 256;
    std::string outputFile = "gpu_benchmark_results.csv";
    GraphicsAPI api = GraphicsAPI::D3D12;  // Default API
};

void PrintHelp(const char* exeName) {
    std::cout << "\n";
    PrintDoubleDivider();
    std::cout << "GPU-PCIe-Test v2.0 - Combined D3D12 + Vulkan Benchmark\n";
    PrintDoubleDivider();
    std::cout << "\nUsage: " << exeName << " [options]\n\n";
    std::cout << "API Selection:\n";
    std::cout << "  --d3d12            Use Direct3D 12 only\n";
    std::cout << "  --vulkan           Use Vulkan only\n";
    std::cout << "  --both             Use both APIs (sequential)\n";
    std::cout << "\nGeneral Options:\n";
    std::cout << "  -h, --help, /?     Show this help message\n";
    std::cout << "  -r, --runs N       Number of runs (1-10, default: 3)\n";
    std::cout << "  -q, --quick        Quick mode (1 run, reduced iterations)\n";
    std::cout << "  -g, --gpu N        Select GPU by index (1-based)\n";
    std::cout << "  --no-bidir         Skip bidirectional bandwidth test\n";
    std::cout << "  --no-latency       Skip latency tests (bandwidth only)\n";
    std::cout << "  --size MB          Bandwidth buffer size in MB (default: 256)\n";
    std::cout << "  --log-all          Log all individual runs to CSV\n";
    std::cout << "  -o, --output FILE  CSV output filename\n";
    std::cout << "\nExamples:\n";
    std::cout << "  " << exeName << " --d3d12 -q             D3D12 quick benchmark\n";
    std::cout << "  " << exeName << " --both -r 5            Both APIs, 5 runs each\n";
    std::cout << "  " << exeName << " --vulkan -g 2 --size 512\n";
    std::cout << "\nNo arguments: Interactive menu\n";
    PrintDoubleDivider();
}

CLIOptions ParseArgs(int argc, char* argv[]) {
    CLIOptions opts;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help" || arg == "/?") {
            opts.showHelp = true; opts.hasOptions = true;
        } else if (arg == "--d3d12") {
            opts.api = GraphicsAPI::D3D12; opts.hasOptions = true;
        } else if (arg == "--vulkan") {
            opts.api = GraphicsAPI::Vulkan; opts.hasOptions = true;
        } else if (arg == "--both") {
            opts.api = GraphicsAPI::Both; opts.hasOptions = true;
        } else if (arg == "-q" || arg == "--quick") {
            opts.quickMode = true; opts.hasOptions = true;
            opts.numRuns = 1;
        } else if ((arg == "-r" || arg == "--runs") && i + 1 < argc) {
            opts.numRuns = std::max(1, std::min(10, std::atoi(argv[++i])));
            opts.hasOptions = true;
        } else if ((arg == "-g" || arg == "--gpu") && i + 1 < argc) {
            opts.gpuIndex = std::atoi(argv[++i]);
            opts.hasOptions = true;
        } else if (arg == "--no-bidir") {
            opts.noBidir = true; opts.hasOptions = true;
        } else if (arg == "--no-latency") {
            opts.noLatency = true; opts.hasOptions = true;
        } else if (arg == "--log-all") {
            opts.logAllRuns = true; opts.hasOptions = true;
        } else if (arg == "--size" && i + 1 < argc) {
            opts.bufferSizeMB = std::stoull(argv[++i]);
            opts.hasOptions = true;
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            opts.outputFile = argv[++i];
            opts.hasOptions = true;
        }
    }
    return opts;
}

// ============================================================================
//                             MAIN
// ============================================================================

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    CLIOptions cli = ParseArgs(argc, argv);
    
    if (cli.showHelp) {
        PrintHelp(argv[0]);
        return 0;
    }

    std::cout << "\nGPU-PCIe-Test v2.0\n";
    PrintDoubleDivider();
    std::cout << "Features: Multi-run averaging, Bidirectional test, PCIe detection\n";
    PrintDoubleDivider();
    
    GraphicsAPI api;
    BenchmarkConfig config;
    
    if (cli.hasOptions) {
        // Apply CLI options
        api = cli.api;
        config.numRuns = cli.numRuns;
        config.enableBidirectionalTest = !cli.noBidir;
        config.logAllRuns = cli.logAllRuns;
        config.largeBandwidthSize = cli.bufferSizeMB * 1024 * 1024;
        config.csvFilename = cli.outputFile;
        if (cli.quickMode) {
            config.commandLatencyIters = 10000;
            config.transferLatencyIters = 1000;
            config.bandwidthBatches = 16;
        }
        
        std::cout << "API: ";
        switch (api) {
            case GraphicsAPI::D3D12: std::cout << "D3D12"; break;
            case GraphicsAPI::Vulkan: std::cout << "Vulkan"; break;
            case GraphicsAPI::Both: std::cout << "Both"; break;
        }
        std::cout << " | Runs: " << config.numRuns;
        if (cli.quickMode) std::cout << " (quick)";
        std::cout << " | Buffer: " << FormatSize(config.largeBandwidthSize) << "\n";
        PrintDoubleDivider();
    } else {
        // Interactive menu
        std::cout << "Select Graphics API:\n";
        std::cout << "  1. Direct3D 12\n";
        std::cout << "  2. Vulkan\n";
        std::cout << "  3. Both (sequential)\n";
        PrintDivider();
        std::cout << "Choice (1-3, default=1): ";
        
        std::string input;
        std::getline(std::cin, input);
        
        api = GraphicsAPI::D3D12;
        if (input == "2") api = GraphicsAPI::Vulkan;
        else if (input == "3") api = GraphicsAPI::Both;
        
        std::cout << "\nSelected: ";
        switch (api) {
            case GraphicsAPI::D3D12: std::cout << "Direct3D 12\n"; break;
            case GraphicsAPI::Vulkan: std::cout << "Vulkan\n"; break;
            case GraphicsAPI::Both: std::cout << "Both APIs\n"; break;
        }
        PrintDoubleDivider();
        
        std::cout << "Press 'C' to configure or any key to start (use -h for CLI options)...\n";
        char key = (char)_getch();
        if (key == 'C' || key == 'c') {
            ConfigureSettings(config);
        }
    }
    
    CSVLogger logger(config.csvFilename);

    // ========================================================================
    // D3D12 Benchmarks
    // ========================================================================
    if (api == GraphicsAPI::D3D12 || api == GraphicsAPI::Both) {
        std::cout << "\n=== Direct3D 12 Benchmark ===\n";
        PrintDoubleDivider();

        ComPtr<IDXGIFactory6> factory;
        CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        auto adapter = SelectD3D12GPU(factory, config.showDetailedGPUInfo, cli.gpuIndex);
        
        ComPtr<ID3D12Device> device;
        D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
        
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        ComPtr<ID3D12CommandAllocator> allocator;
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        ComPtr<ID3D12GraphicsCommandList> list;
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
        list->Close();
        ComPtr<ID3D12Fence> fence;
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        UINT64 fenceValue = 1;

        // Storage for multi-run results
        std::vector<std::vector<TestResults>> allRunResults(6);  // Up to 6 test types
        
        for (int run = 0; run < config.numRuns; run++) {
            if (config.numRuns > 1) {
                std::cout << "\n--- Run " << (run + 1) << " of " << config.numRuns << " ---\n";
            }
            
            int testIdx = 0;

            // CPU->GPU bandwidth
            auto cpuUpload = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, config.largeBandwidthSize, D3D12_RESOURCE_STATE_GENERIC_READ);
            auto gpuDefault = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, config.largeBandwidthSize, D3D12_RESOURCE_STATE_COPY_DEST);
            allRunResults[testIdx++].push_back(RunBandwidthTestD3D12(
                ("CPU->GPU " + FormatSize(config.largeBandwidthSize) + " Bandwidth").c_str(),
                device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue,
                cpuUpload, gpuDefault, config.largeBandwidthSize, config.copiesPerBatch, config.bandwidthBatches, config.continuousMode));

            // GPU->CPU bandwidth
            auto gpuSrc = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, config.largeBandwidthSize, D3D12_RESOURCE_STATE_COPY_SOURCE);
            auto cpuReadback = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_READBACK, config.largeBandwidthSize, D3D12_RESOURCE_STATE_COPY_DEST);
            allRunResults[testIdx++].push_back(RunBandwidthTestD3D12(
                ("GPU->CPU " + FormatSize(config.largeBandwidthSize) + " Bandwidth").c_str(),
                device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue,
                gpuSrc, cpuReadback, config.largeBandwidthSize, config.copiesPerBatch, config.bandwidthBatches, config.continuousMode));

            // Bidirectional test
            if (config.enableBidirectionalTest) {
                allRunResults[testIdx++].push_back(RunBidirectionalTestD3D12(
                    ("Bidirectional " + FormatSize(config.largeBandwidthSize)).c_str(),
                    device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue,
                    config.largeBandwidthSize, config.copiesPerBatch, config.bandwidthBatches, config.continuousMode));
            }

            // Command latency
            if (!cli.noLatency) {
                allRunResults[testIdx++].push_back(RunCommandLatencyTestD3D12(
                    queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue, 
                    config.commandLatencyIters, config.continuousMode));

                // Transfer latency tests
                auto cpuSmall = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, config.smallLatencySize, D3D12_RESOURCE_STATE_GENERIC_READ);
                auto gpuSmall = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, config.smallLatencySize, D3D12_RESOURCE_STATE_COPY_DEST);
                allRunResults[testIdx++].push_back(RunLatencyTestD3D12(
                    ("CPU->GPU " + FormatSize(config.smallLatencySize) + " Latency").c_str(),
                    device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue,
                    cpuSmall, gpuSmall, config.transferLatencyIters, config.continuousMode));

                auto gpuSmallSrc = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, config.smallLatencySize, D3D12_RESOURCE_STATE_COPY_SOURCE);
                auto cpuSmallDst = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_READBACK, config.smallLatencySize, D3D12_RESOURCE_STATE_COPY_DEST);
                allRunResults[testIdx++].push_back(RunLatencyTestD3D12(
                    ("GPU->CPU " + FormatSize(config.smallLatencySize) + " Latency").c_str(),
                    device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue,
                    gpuSmallSrc, cpuSmallDst, config.transferLatencyIters, config.continuousMode));
            }
        }

        // Aggregate and display results
        std::cout << "\n";
        PrintDoubleDivider();
        std::cout << "D3D12 Summary\n";
        PrintDoubleDivider();
        
        double uploadBW = 0, downloadBW = 0;
        
        for (auto& runs : allRunResults) {
            if (runs.empty()) continue;
            
            if (config.numRuns > 1) {
                AggregatedResults agg = AggregateRuns(runs);
                PrintAggregatedResults(agg, config.numRuns);
                
                // Log to CSV
                if (config.logAllRuns) {
                    for (int i = 0; i < (int)runs.size(); i++) {
                        logger.LogResult(runs[i], "D3D12", i + 1);
                    }
                }
                logger.LogAggregated(agg, "D3D12");
                
                // Store for interface detection
                if (runs[0].testName.find("CPU->GPU") != std::string::npos && 
                    runs[0].testName.find("Bandwidth") != std::string::npos) {
                    uploadBW = agg.avgAvg;
                }
                if (runs[0].testName.find("GPU->CPU") != std::string::npos && 
                    runs[0].testName.find("Bandwidth") != std::string::npos) {
                    downloadBW = agg.avgAvg;
                }
            } else {
                logger.LogResult(runs[0], "D3D12", 1);
                
                if (runs[0].testName.find("CPU->GPU") != std::string::npos && 
                    runs[0].testName.find("Bandwidth") != std::string::npos) {
                    uploadBW = runs[0].avgValue;
                }
                if (runs[0].testName.find("GPU->CPU") != std::string::npos && 
                    runs[0].testName.find("Bandwidth") != std::string::npos) {
                    downloadBW = runs[0].avgValue;
                }
            }
        }

        // Interface detection
        if (uploadBW > 0 && downloadBW > 0) {
            PrintInterfaceGuess(uploadBW, downloadBW);
        }

        CloseHandle(fenceEvent);
    }

    // ========================================================================
    // Vulkan Benchmarks
    // ========================================================================
    if (api == GraphicsAPI::Vulkan || api == GraphicsAPI::Both) {
        if (api == GraphicsAPI::Both) { 
            std::cout << "\nPress any key for Vulkan...\n"; 
            _getch(); 
        }
        
        std::cout << "\n=== Vulkan Benchmark ===\n";
        PrintDoubleDivider();
        
        VulkanContext ctx;
        ctx.Init(cli.gpuIndex);
        
        if (config.showDetailedGPUInfo) {
            ctx.PrintGPUInfo();
        }

        // Storage for multi-run results
        std::vector<std::vector<TestResults>> allRunResults(6);
        
        for (int run = 0; run < config.numRuns; run++) {
            if (config.numRuns > 1) {
                std::cout << "\n--- Run " << (run + 1) << " of " << config.numRuns << " ---\n";
            }
            
            int testIdx = 0;

            // CPU->GPU bandwidth
            Buffer cpuSrc, gpuDst;
            cpuSrc.Create(ctx, config.largeBandwidthSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            gpuDst.Create(ctx, config.largeBandwidthSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            allRunResults[testIdx++].push_back(RunBandwidthTestVulkan(
                ("CPU->GPU " + FormatSize(config.largeBandwidthSize) + " Bandwidth").c_str(),
                ctx, cpuSrc, gpuDst, config.largeBandwidthSize, config.copiesPerBatch, 
                config.bandwidthBatches, config.continuousMode));
            cpuSrc.Destroy(ctx); gpuDst.Destroy(ctx);
            
            // GPU->CPU bandwidth
            Buffer gpuSrc, cpuDst;
            gpuSrc.Create(ctx, config.largeBandwidthSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            cpuDst.Create(ctx, config.largeBandwidthSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            allRunResults[testIdx++].push_back(RunBandwidthTestVulkan(
                ("GPU->CPU " + FormatSize(config.largeBandwidthSize) + " Bandwidth").c_str(),
                ctx, gpuSrc, cpuDst, config.largeBandwidthSize, config.copiesPerBatch, 
                config.bandwidthBatches, config.continuousMode));
            gpuSrc.Destroy(ctx); cpuDst.Destroy(ctx);

            // Bidirectional test
            if (config.enableBidirectionalTest) {
                allRunResults[testIdx++].push_back(RunBidirectionalTestVulkan(
                    ("Bidirectional " + FormatSize(config.largeBandwidthSize)).c_str(),
                    ctx, config.largeBandwidthSize, config.copiesPerBatch, 
                    config.bandwidthBatches, config.continuousMode));
            }

            // Command latency
            if (!cli.noLatency) {
                allRunResults[testIdx++].push_back(RunCommandLatencyTestVulkan(
                    ctx, config.commandLatencyIters, config.continuousMode));

                // Transfer latency tests
                Buffer smallCpuSrc, smallGpuDst;
                smallCpuSrc.Create(ctx, config.smallLatencySize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                smallGpuDst.Create(ctx, config.smallLatencySize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                allRunResults[testIdx++].push_back(RunLatencyTestVulkan(
                    ("CPU->GPU " + FormatSize(config.smallLatencySize) + " Latency").c_str(),
                    ctx, smallCpuSrc, smallGpuDst, config.transferLatencyIters, config.continuousMode));
                smallCpuSrc.Destroy(ctx); smallGpuDst.Destroy(ctx);

                Buffer smallGpuSrc, smallCpuDst;
                smallGpuSrc.Create(ctx, config.smallLatencySize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                smallCpuDst.Create(ctx, config.smallLatencySize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                allRunResults[testIdx++].push_back(RunLatencyTestVulkan(
                    ("GPU->CPU " + FormatSize(config.smallLatencySize) + " Latency").c_str(),
                    ctx, smallGpuSrc, smallCpuDst, config.transferLatencyIters, config.continuousMode));
                smallGpuSrc.Destroy(ctx); smallCpuDst.Destroy(ctx);
            }
        }

        // Aggregate and display results
        std::cout << "\n";
        PrintDoubleDivider();
        std::cout << "Vulkan Summary\n";
        PrintDoubleDivider();
        
        double uploadBW = 0, downloadBW = 0;
        
        for (auto& runs : allRunResults) {
            if (runs.empty()) continue;
            
            if (config.numRuns > 1) {
                AggregatedResults agg = AggregateRuns(runs);
                PrintAggregatedResults(agg, config.numRuns);
                
                if (config.logAllRuns) {
                    for (int i = 0; i < (int)runs.size(); i++) {
                        logger.LogResult(runs[i], "Vulkan", i + 1);
                    }
                }
                logger.LogAggregated(agg, "Vulkan");
                
                if (runs[0].testName.find("CPU->GPU") != std::string::npos && 
                    runs[0].testName.find("Bandwidth") != std::string::npos) {
                    uploadBW = agg.avgAvg;
                }
                if (runs[0].testName.find("GPU->CPU") != std::string::npos && 
                    runs[0].testName.find("Bandwidth") != std::string::npos) {
                    downloadBW = agg.avgAvg;
                }
            } else {
                logger.LogResult(runs[0], "Vulkan", 1);
                
                if (runs[0].testName.find("CPU->GPU") != std::string::npos && 
                    runs[0].testName.find("Bandwidth") != std::string::npos) {
                    uploadBW = runs[0].avgValue;
                }
                if (runs[0].testName.find("GPU->CPU") != std::string::npos && 
                    runs[0].testName.find("Bandwidth") != std::string::npos) {
                    downloadBW = runs[0].avgValue;
                }
            }
        }

        // Interface detection
        if (uploadBW > 0 && downloadBW > 0) {
            PrintInterfaceGuess(uploadBW, downloadBW);
        }
        
        ctx.Cleanup();
    }

    // Final summary
    std::cout << "\n=== Benchmark Complete! ===\n";
    std::cout << "Results saved to: " << config.csvFilename << "\n";
    if (config.numRuns > 1) {
        std::cout << "Multi-run averaging: " << config.numRuns << " runs per test\n";
    }
    
    if (!cli.hasOptions) {
        std::cout << "\nPress any key to exit...\n";
        _getch();
    }

    return 0;
}

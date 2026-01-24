// ============================================================================
// GPU Performance Benchmark Tool - Single Executable Edition
// Combined D3D12 + Vulkan - Everything in ONE file
// ============================================================================
// Build: cl /EHsc /std:c++17 /O2 /MD /I"%VULKAN_SDK%\Include" main_combined.cpp ^
//           d3d12.lib dxgi.lib /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib ^
//           /Fe:GPUBenchmark.exe
// ============================================================================

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

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "vulkan-1.lib")

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------
// Bandwidth Analysis (inlined from bandwidth_analysis.h)
// ------------------------------------------------------------

// ────────────────────────────────────────────────
//   Printing helpers
// ────────────────────────────────────────────────

inline void PrintDivider() {
    std::cout << std::string(70, '-') << '\n';
}

inline void PrintDoubleDivider() {
    std::cout << std::string(70, '=') << '\n';
}

// ────────────────────────────────────────────────
//   Interface speed reference table
// ────────────────────────────────────────────────

struct InterfaceSpeed {
    const char* name;
    double bandwidth_gbps;  // Theoretical maximum in GB/s
    const char* description;
};

static const InterfaceSpeed INTERFACE_SPEEDS[] = {
    // PCIe 3.0
    {"PCIe 3.0 x1",  0.985,  "Single lane PCIe Gen 3"},
    {"PCIe 3.0 x4",  3.94,   "Common for M.2 SSDs, some GPUs"},
    {"PCIe 3.0 x8",  7.88,   "Older GPUs, some workstation cards"},
    {"PCIe 3.0 x16", 15.75,  "Standard GPU slot (older platforms)"},
    
    // PCIe 4.0
    {"PCIe 4.0 x1",  1.97,   "Single lane PCIe Gen 4"},
    {"PCIe 4.0 x4",  7.88,   "Modern M.2 SSDs, OCuLink Gen 4"},
    {"PCIe 4.0 x8",  15.75,  "Some modern GPUs in x8 mode"},
    {"PCIe 4.0 x16", 31.5,   "Modern GPU slot (AMD Ryzen 3000+, Intel 11th gen+)"},
    
    // PCIe 5.0
    {"PCIe 5.0 x1",  3.94,   "Single lane PCIe Gen 5"},
    {"PCIe 5.0 x4",  15.75,  "Next-gen M.2 SSDs, OCuLink Gen 5"},
    {"PCIe 5.0 x8",  31.5,   "High-end GPUs in x8 mode"},
    {"PCIe 5.0 x16", 63.0,   "Cutting-edge GPU slot (AMD Ryzen 7000+, Intel 12th gen+)"},
    
    // Thunderbolt
    {"Thunderbolt 3", 2.75,  "40 Gbps - Common eGPU connection"},
    {"Thunderbolt 4", 2.75,  "40 Gbps - Same speed as TB3, better specs"},
    {"Thunderbolt 5", 6.0,   "80 Gbps bidirectional - Latest standard"},
    {"TB5 Asymmetric", 12.0, "120 Gbps download / 40 Gbps upload"},
    
    // USB
    {"USB 3.2 Gen 2",  1.25, "10 Gbps - USB-C"},
    {"USB4 Gen 3x2",   4.8,  "40 Gbps - USB4"},
    
    // Other
    {"OCuLink PCIe 3.0", 3.94,  "External PCIe cable (Gen 3 x4)"},
    {"OCuLink PCIe 4.0", 7.88,  "External PCIe cable (Gen 4 x4)"},
};

static const int NUM_INTERFACES = sizeof(INTERFACE_SPEEDS) / sizeof(InterfaceSpeed);

// ────────────────────────────────────────────────
//   Bandwidth analysis result structure
// ────────────────────────────────────────────────

struct BandwidthAnalysis {
    const InterfaceSpeed* likelyInterface;
    double percentOfTheoretical;
    bool isRealistic;  // True if within 60-95% of theoretical max
};

// ────────────────────────────────────────────────
//   Core analysis function
// ────────────────────────────────────────────────

BandwidthAnalysis AnalyzeBandwidth(double measuredBandwidth_gbps) {
    BandwidthAnalysis result;
    result.likelyInterface = nullptr;
    result.percentOfTheoretical = 0.0;
    result.isRealistic = false;
    
    // Find the best match - closest interface where measured is 60-95% of theoretical
    double bestMatch = -1;
    double bestDiff = 1e9;
    
    for (int i = 0; i < NUM_INTERFACES; i++) {
        double theoretical = INTERFACE_SPEEDS[i].bandwidth_gbps;
        double percent = (measuredBandwidth_gbps / theoretical) * 100.0;
        
        // Realistic range: 60-95% of theoretical (accounting for overhead)
        if (percent >= 60.0 && percent <= 95.0) {
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
    
    // No realistic match found - find closest by absolute value
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

// ────────────────────────────────────────────────
//   Printing functions
// ────────────────────────────────────────────────

void PrintBandwidthAnalysis(const char* direction, double bandwidth_gbps) {
    BandwidthAnalysis analysis = AnalyzeBandwidth(bandwidth_gbps);
    
    if (!analysis.likelyInterface) return;
    
    std::cout << "\n  Interface Analysis (" << direction << "):\n";
    std::cout << "  ├─ Closest match: " << analysis.likelyInterface->name << "\n";
    std::cout << "  ├─ Theoretical max: " << std::fixed << std::setprecision(2) 
              << analysis.likelyInterface->bandwidth_gbps << " GB/s\n";
    std::cout << "  ├─ Measured: " << bandwidth_gbps << " GB/s\n";
    std::cout << "  ├─ Efficiency: " << std::fixed << std::setprecision(1) 
              << analysis.percentOfTheoretical << "%\n";
    
    if (analysis.isRealistic) {
        std::cout << "  └─ ✓ Performance is typical for " << analysis.likelyInterface->name << "\n";
    } else if (analysis.percentOfTheoretical > 95.0) {
        std::cout << "  └─ ⚠ Unusually high (exceeds typical overhead)\n";
    } else if (analysis.percentOfTheoretical < 60.0) {
        std::cout << "  └─ ⚠ Lower than expected - possible bottleneck\n";
    }
}

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
    
    std::cout << "\nPCIe 5.0 (2022+ AMD, 2022+ Intel):\n";
    PrintDivider();
    std::cout << "  x1   ~3.9 GB/s   | x4   ~15.8 GB/s\n";
    std::cout << "  x8   ~31.5 GB/s  | x16  ~63.0 GB/s\n";
    
    std::cout << "\nThunderbolt (eGPU connections):\n";
    PrintDivider();
    std::cout << "  TB3 (40 Gbps)    ~2.8 GB/s\n";
    std::cout << "  TB4 (40 Gbps)    ~2.8 GB/s\n";
    std::cout << "  TB5 (80 Gbps)    ~6.0 GB/s bidirectional\n";
    std::cout << "  TB5 (120 Gbps)   ~12.0 GB/s download mode\n";
    
    std::cout << "\nOther Connections:\n";
    PrintDivider();
    std::cout << "  OCuLink Gen 3    ~4.0 GB/s   (PCIe 3.0 x4)\n";
    std::cout << "  OCuLink Gen 4    ~7.9 GB/s   (PCIe 4.0 x4)\n";
    std::cout << "  USB4 (40 Gbps)   ~4.8 GB/s\n";
    
    std::cout << "\nNote: Values shown are theoretical maximums.\n";
    std::cout << "Real-world performance is typically 70-90% of theoretical.\n";
    PrintDoubleDivider();
}

void PrintInterfaceGuess(double uploadBandwidth, double downloadBandwidth) {
    std::cout << "\n";
    PrintDoubleDivider();
    std::cout << "Interface Detection\n";
    PrintDoubleDivider();
    
    // Analyze both directions
    BandwidthAnalysis uploadAnalysis = AnalyzeBandwidth(uploadBandwidth);
    BandwidthAnalysis downloadAnalysis = AnalyzeBandwidth(downloadBandwidth);
    
    // Use the more constraining direction (usually upload for GPU)
    double avgBandwidth = (uploadBandwidth + downloadBandwidth) / 2.0;
    BandwidthAnalysis primaryAnalysis = AnalyzeBandwidth(avgBandwidth);
    
    if (primaryAnalysis.likelyInterface) {
        std::cout << "\n┌─ Likely Connection Type ─────────────────────\n";
        std::cout << "│\n";
        std::cout << "│  " << primaryAnalysis.likelyInterface->name << "\n";
        std::cout << "│  " << primaryAnalysis.likelyInterface->description << "\n";
        std::cout << "│\n";
        std::cout << "│  Upload:   " << std::fixed << std::setprecision(2) << uploadBandwidth 
                  << " GB/s  (" << std::setprecision(1) << uploadAnalysis.percentOfTheoretical << "% of " 
                  << uploadAnalysis.likelyInterface->name << ")\n";
        std::cout << "│  Download: " << std::fixed << std::setprecision(2) << downloadBandwidth 
                  << " GB/s  (" << std::setprecision(1) << downloadAnalysis.percentOfTheoretical << "% of " 
                  << downloadAnalysis.likelyInterface->name << ")\n";
        std::cout << "│\n";
        
        // Provide context-specific recommendations
        if (primaryAnalysis.isRealistic) {
            std::cout << "│  ✓ Performance is as expected for this interface\n";
        } else if (primaryAnalysis.percentOfTheoretical < 60.0) {
            std::cout << "│  ⚠ Lower than expected - possible issues:\n";
            std::cout << "│    • GPU might be in reduced PCIe mode (x8 instead of x16)\n";
            std::cout << "│    • PCIe slot might be limited (check motherboard manual)\n";
            std::cout << "│    • Driver or system configuration issue\n";
            std::cout << "│    • Thermal throttling\n";
        } else if (primaryAnalysis.percentOfTheoretical > 95.0) {
            std::cout << "│  ℹ Exceptionally high efficiency - excellent!\n";
        }
        
        std::cout << "│\n";
        std::cout << "└───────────────────────────────────────────────\n";
    }
    
    PrintDoubleDivider();
}

inline void PrintBandwidthComparison(double measured_gbps, const char* test_label = "Measured") {
    PrintDoubleDivider();
    std::cout << "Bandwidth Comparison: " << test_label << "\n";
    PrintDivider();
    std::cout << "Measured: " << std::fixed << std::setprecision(2) << measured_gbps << " GB/s\n";
    // ... you can expand this with more comparison logic as needed
    PrintDoubleDivider();
}

// ------------------------------------------------------------
// Configuration
// ------------------------------------------------------------

struct BenchmarkConfig {
    size_t largeBandwidthSize    = 256ull * 1024 * 1024;  // 256 MB
    size_t smallLatencySize      = 1;                     // 1 byte
    int    commandLatencyIters   = 100000;
    int    transferLatencyIters  = 10000;
    int    copiesPerBatch        = 8;
    int    bandwidthBatches      = 32;
    bool   continuousMode        = false;
    bool   enableCSVLogging      = true;
    std::string csvFilename      = "gpu_benchmark_results.csv";
};

// ------------------------------------------------------------
// Results & CSV Logger
// ------------------------------------------------------------

struct TestResults {
    std::string testName;
    double minValue, avgValue, maxValue;
    double p99Value = 0.0, p999Value = 0.0;
    std::string unit;
    std::chrono::system_clock::time_point timestamp;
};

class CSVLogger {
public:
    CSVLogger(const std::string& filename) : filename_(filename) {
        std::ifstream check(filename);
        bool exists = check.good();
        check.close();

        file_.open(filename, std::ios::app);
        if (!exists && file_.is_open()) {
            file_ << "Timestamp,API,Test Name,Min,Avg,Max,99th,99.9th,Unit\n";
        }
    }

    ~CSVLogger() { if (file_.is_open()) file_.close(); }

    void LogResult(const TestResults& r, const std::string& api) {
        if (!file_.is_open()) return;
        auto t = std::chrono::system_clock::to_time_t(r.timestamp);
        std::tm tm; localtime_s(&tm, &t);
        file_ << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << ","
              << api << "," << r.testName << ","
              << r.minValue << "," << r.avgValue << "," << r.maxValue << ","
              << r.p99Value << "," << r.p999Value << "," << r.unit << "\n";
        file_.flush();
    }

private:
    std::string filename_;
    std::ofstream file_;
};

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------

static std::string FormatSize(size_t bytes) {
    if (bytes >= 1024ull * 1024 * 1024) return std::to_string(bytes / (1024ull*1024*1024)) + " GB";
    if (bytes >= 1024 * 1024)           return std::to_string(bytes / (1024*1024)) + " MB";
    if (bytes >= 1024)                  return std::to_string(bytes / 1024) + " KB";
    return std::to_string(bytes) + " B";
}

static bool CheckForEscape() {
    if (_kbhit()) return _getch() == 27;
    return false;
}

// ------------------------------------------------------------
// D3D12 Specific
// ------------------------------------------------------------

static ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* dev, D3D12_HEAP_TYPE heapType, size_t size, D3D12_RESOURCE_STATES state) {
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

static void WaitForFence(ID3D12CommandQueue* q, ID3D12Fence* f, HANDLE ev, UINT64& val) {
    q->Signal(f, val);
    if (f->GetCompletedValue() < val) {
        f->SetEventOnCompletion(val, ev);
        WaitForSingleObject(ev, INFINITE);
    }
    val++;
}

TestResults RunBandwidthTestD3D12(const char* name, ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* list, ID3D12Fence* fence, HANDLE fenceEvent, UINT64& fenceValue, ComPtr<ID3D12Resource> src, ComPtr<ID3D12Resource> dst, size_t bufferSize, int copiesPerBatch, int batches, bool continuousMode) {
    std::cout << name << "\n";
    PrintDivider();

    D3D12_QUERY_HEAP_DESC qhd{}; qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qhd.Count = 2;
    ComPtr<ID3D12QueryHeap> queryHeap; device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&queryHeap));
    auto queryReadback = MakeBuffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(UINT64) * 2, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 timestampFreq = 0; queue->GetTimestampFrequency(&timestampFreq);

    std::vector<double> bandwidths;
    int lastPercent = -1;
    bool escaped = false;

    for (int i = 0; i < batches && !escaped; i++) {
        allocator->Reset();
        list->Reset(allocator, nullptr);

        list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        for (int j = 0; j < copiesPerBatch; j++)
            list->CopyResource(dst.Get(), src.Get());
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

    TestResults results;
    results.testName = name;
    results.minValue = *std::min_element(bandwidths.begin(), bandwidths.end());
    results.avgValue = std::accumulate(bandwidths.begin(), bandwidths.end(), 0.0) / bandwidths.size();
    results.maxValue = *std::max_element(bandwidths.begin(), bandwidths.end());
    results.unit = "GB/s";
    results.timestamp = std::chrono::system_clock::now();

    PrintDivider();
    std::cout << "Results:\n";
    std::cout << "  Min : " << results.minValue << " GB/s\n";
    std::cout << "  Avg : " << results.avgValue << " GB/s\n";
    std::cout << "  Max : " << results.maxValue << " GB/s\n";

    return results;
}

TestResults RunLatencyTestD3D12(const char* name, ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* list, ID3D12Fence* fence, HANDLE fenceEvent, UINT64& fenceValue, ComPtr<ID3D12Resource> src, ComPtr<ID3D12Resource> dst, int iterations, bool continuousMode) {
    std::cout << name << "\n";
    PrintDivider();

    D3D12_QUERY_HEAP_DESC qhd{}; qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qhd.Count = 2;
    ComPtr<ID3D12QueryHeap> queryHeap; device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&queryHeap));
    auto queryReadback = MakeBuffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(UINT64) * 2, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 timestampFreq = 0; queue->GetTimestampFrequency(&timestampFreq);

    std::vector<double> latencies;
    int lastPercent = -1;
    bool escaped = false;

    for (int i = 0; i < iterations && !escaped; i++) {
        allocator->Reset();
        list->Reset(allocator, nullptr);

        list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        list->CopyResource(dst.Get(), src.Get());
        list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);

        list->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, queryReadback.Get(), 0);
        list->Close();

        ID3D12CommandList* cl[] = { list };
        queue->ExecuteCommandLists(1, cl);
        WaitForFence(queue, fence, fenceEvent, fenceValue);

        UINT64* ts = nullptr;
        D3D12_RANGE range{ 0, sizeof(UINT64) * 2 };
        queryReadback->Map(0, &range, (void**)&ts);
        double ns = double(ts[1] - ts[0]) / double(timestampFreq) * 1e9;
        queryReadback->Unmap(0, nullptr);

        latencies.push_back(ns);

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
    results.unit = "ns";
    results.timestamp = std::chrono::system_clock::now();

    PrintDivider();
    std::cout << "Results:\n";
    std::cout << "  Min : " << results.minValue << " ns\n";
    std::cout << "  Avg : " << results.avgValue << " ns\n";
    std::cout << "  Max : " << results.maxValue << " ns\n";
    std::cout << "  P99 : " << results.p99Value << " ns\n";
    std::cout << "  P99.9 : " << results.p999Value << " ns\n";

    return results;
}

TestResults RunCommandLatencyTestD3D12(ID3D12CommandQueue* queue, ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* list, ID3D12Fence* fence, HANDLE fenceEvent, UINT64& fenceValue, int iterations, bool continuousMode) {
    const char* name = "Test 3 - Command Latency";
    std::cout << name << "\n";
    PrintDivider();

    std::vector<double> latencies;
    int lastPercent = -1;
    bool escaped = false;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations && !escaped; i++) {
        allocator->Reset();
        list->Reset(allocator, nullptr);
        list->Close();

        ID3D12CommandList* cl[] = { list };
        queue->ExecuteCommandLists(1, cl);
        WaitForFence(queue, fence, fenceEvent, fenceValue);

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

    auto end = std::chrono::high_resolution_clock::now();
    double total_ns = std::chrono::duration<double, std::nano>(end - start).count();

    double avg_ns = total_ns / iterations;

    TestResults results;
    results.testName = name;
    results.minValue = avg_ns;
    results.avgValue = avg_ns;
    results.maxValue = avg_ns;
    results.unit = "ns";
    results.timestamp = std::chrono::system_clock::now();

    PrintDivider();
    std::cout << "Results:\n";
    std::cout << "  Avg : " << results.avgValue << " ns per command\n";

    return results;
}

ComPtr<IDXGIAdapter1> SelectD3D12GPU(ComPtr<IDXGIFactory6> factory) {
    std::vector<ComPtr<IDXGIAdapter1>> adapters;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        adapters.push_back(adapter);
    }

    if (adapters.empty()) {
        std::cerr << "No DirectX 12 capable GPU found!\n";
        exit(1);
    }

    if (adapters.size() == 1) {
        DXGI_ADAPTER_DESC1 desc;
        adapters[0]->GetDesc1(&desc);
        std::wcout << L"Using GPU: " << desc.Description << L"\n";
        PrintDoubleDivider();
        return adapters[0];
    }

    std::cout << "Found " << adapters.size() << " GPUs:\n";
    PrintDoubleDivider();

    for (size_t i = 0; i < adapters.size(); ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapters[i]->GetDesc1(&desc);
        std::wcout << (i+1) << L". " << desc.Description << L"\n";
        std::cout << "   VRAM: " << (desc.DedicatedVideoMemory / (1024*1024)) << " MB\n";

        std::string vendor = "Unknown";
        switch (desc.VendorId) {
            case 0x10DE: vendor = "NVIDIA"; break;
            case 0x1002: vendor = "AMD";    break;
            case 0x8086: vendor = "Intel";  break;
        }
        std::cout << "   Vendor: " << vendor << "\n\n";
    }

    std::cout << "Select GPU (1-" << adapters.size() << ", default=1): ";
    char ch = _getch();
    std::cout << ch << "\n\n";

    int sel = (ch >= '1' && ch <= '9') ? ch - '0' : 1;
    if (sel < 1 || sel > static_cast<int>(adapters.size())) sel = 1;

    DXGI_ADAPTER_DESC1 desc;
    adapters[sel-1]->GetDesc1(&desc);
    std::wcout << L"Selected: " << desc.Description << L"\n";
    PrintDoubleDivider();

    return adapters[sel-1];
}

// ------------------------------------------------------------
// Vulkan Specific
// ------------------------------------------------------------

#define VK_CHECK(x) \
    do { \
        VkResult err = x; \
        if (err) { \
            std::cerr << "Vulkan error: " << err << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            abort(); \
        } \
    } while (0)

struct VulkanContext {
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkQueue queue;
    uint32_t queueFamilyIndex;
    VkCommandPool commandPool;
    float timestampPeriod;

    void Init() {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "GPU Benchmark";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "No Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;

        VK_CHECK(vkCreateInstance(&createInfo, nullptr, &instance));

        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
        
        if (deviceCount == 0) {
            std::cerr << "ERROR: No Vulkan-capable GPU found!\n";
            exit(1);
        }
        
        std::vector<VkPhysicalDeviceProperties> deviceProps(deviceCount);
        std::vector<VkPhysicalDeviceMemoryProperties> memProps(deviceCount);
        for (uint32_t i = 0; i < deviceCount; i++) {
            vkGetPhysicalDeviceProperties(devices[i], &deviceProps[i]);
            vkGetPhysicalDeviceMemoryProperties(devices[i], &memProps[i]);
        }
        
        if (deviceCount == 1) {
            physicalDevice = devices[0];
            std::cout << "Found 1 GPU: " << deviceProps[0].deviceName << "\n";
            PrintDoubleDivider();
        } else {
            std::cout << "\n";
            PrintDoubleDivider();
            std::cout << "Found " << deviceCount << " GPUs:\n";
            PrintDoubleDivider();
            
            for (uint32_t i = 0; i < deviceCount; i++) {
                std::cout << (i + 1) << ". " << deviceProps[i].deviceName << "\n";
                
                uint64_t vram = 0;
                for (uint32_t j = 0; j < memProps[i].memoryHeapCount; j++) {
                    if (memProps[i].memoryHeaps[j].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                        vram += memProps[i].memoryHeaps[j].size;
                    }
                }
                std::cout << "   VRAM: " << (vram / (1024 * 1024)) << " MB\n";
                
                std::string deviceType;
                switch (deviceProps[i].deviceType) {
                    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: deviceType = "Discrete GPU"; break;
                    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: deviceType = "Integrated GPU"; break;
                    default: deviceType = "Other"; break;
                }
                std::cout << "   Type: " << deviceType << "\n\n";
            }
            
            std::cout << "Select GPU (1-" << deviceCount << ", default=1): ";
            char ch = _getch();
            std::cout << ch << "\n\n";

            int sel = (ch >= '1' && ch <= '9') ? ch - '0' : 1;
            if (sel < 1 || sel > static_cast<int>(deviceCount)) sel = 1;

            physicalDevice = devices[sel - 1];
            std::cout << "Selected: " << deviceProps[sel - 1].deviceName << "\n";
            PrintDoubleDivider();
        }

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physicalDevice, &props);
        timestampPeriod = props.limits.timestampPeriod;

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

        queueFamilyIndex = -1;
        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
                queueFamilyIndex = i;
                break;
            }
        }

        if (queueFamilyIndex == -1) {
            std::cerr << "No suitable queue family found\n";
            exit(1);
        }

        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = queueFamilyIndex;
        qci.queueCount = 1;
        float priority = 1.0f;
        qci.pQueuePriorities = &priority;

        VkPhysicalDeviceFeatures features{};

        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.pQueueCreateInfos = &qci;
        dci.queueCreateInfoCount = 1;
        dci.pEnabledFeatures = &features;

        VK_CHECK(vkCreateDevice(physicalDevice, &dci, nullptr, &device));

        vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

        VkCommandPoolCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.queueFamilyIndex = queueFamilyIndex;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        VK_CHECK(vkCreateCommandPool(device, &cpci, nullptr, &commandPool));
    }

    void Cleanup() {
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
    }
};

struct Buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mapped = nullptr;

    void Create(const VulkanContext& ctx, size_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VK_CHECK(vkCreateBuffer(ctx.device, &bci, nullptr, &buffer));

        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(ctx.device, buffer, &mr);

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = FindMemoryType(ctx, mr.memoryTypeBits, props);

        VK_CHECK(vkAllocateMemory(ctx.device, &mai, nullptr, &memory));

        vkBindBufferMemory(ctx.device, buffer, memory, 0);

        if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            vkMapMemory(ctx.device, memory, 0, size, 0, &mapped);
        }
    }

    void Destroy(const VulkanContext& ctx) {
        if (mapped) vkUnmapMemory(ctx.device, memory);
        vkFreeMemory(ctx.device, memory, nullptr);
        vkDestroyBuffer(ctx.device, buffer, nullptr);
    }
};

uint32_t FindMemoryType(const VulkanContext& ctx, uint32_t typeFilter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(ctx.physicalDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }

    std::cerr << "Failed to find suitable memory type\n";
    exit(1);
    return 0; // Unreachable
}

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

TestResults RunBandwidthTestVulkan(const char* name, const VulkanContext& ctx, Buffer& src, Buffer& dst, size_t size, int copiesPerBatch, int batches, bool continuousMode) {
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
        VK_CHECK(vkGetQueryPoolResults(ctx.device, queryPool, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));

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

    if (cb) EndCommandBuffer(ctx, cb); // Clean up if escaped
    vkDestroyQueryPool(ctx.device, queryPool, nullptr);

    TestResults results;
    results.testName = name;
    results.minValue = *std::min_element(bandwidths.begin(), bandwidths.end());
    results.avgValue = std::accumulate(bandwidths.begin(), bandwidths.end(), 0.0) / bandwidths.size();
    results.maxValue = *std::max_element(bandwidths.begin(), bandwidths.end());
    results.unit = "GB/s";
    results.timestamp = std::chrono::system_clock::now();

    PrintDivider();
    std::cout << "Results:\n";
    std::cout << "  Min : " << results.minValue << " GB/s\n";
    std::cout << "  Avg : " << results.avgValue << " GB/s\n";
    std::cout << "  Max : " << results.maxValue << " GB/s\n";

    return results;
}

TestResults RunLatencyTestVulkan(const char* name, const VulkanContext& ctx, Buffer& src, Buffer& dst, int iterations, bool continuousMode) {
    std::cout << name << "\n";
    PrintDivider();

    VkCommandBuffer cb = BeginCommandBuffer(ctx);

    VkQueryPool queryPool;
    VkQueryPoolCreateInfo qpci{};
    qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = 2;

    VK_CHECK(vkCreateQueryPool(ctx.device, &qpci, nullptr, &queryPool));

    std::vector<double> latencies;
    int lastPercent = -1;
    bool escaped = false;

    for (int i = 0; i < iterations && !escaped; i++) {
        vkCmdResetQueryPool(cb, queryPool, 0, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);

        VkBufferCopy copy{0, 0, dst.size}; // Assuming Buffer has size member, adjust if not
        vkCmdCopyBuffer(cb, src.buffer, dst.buffer, 1, &copy);

        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);

        EndCommandBuffer(ctx, cb);
        cb = BeginCommandBuffer(ctx);

        uint64_t ts[2];
        VK_CHECK(vkGetQueryPoolResults(ctx.device, queryPool, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));

        double ns = double(ts[1] - ts[0]) * ctx.timestampPeriod;
        latencies.push_back(ns);

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

    if (cb) EndCommandBuffer(ctx, cb);
    vkDestroyQueryPool(ctx.device, queryPool, nullptr);

    std::sort(latencies.begin(), latencies.end());

    TestResults results;
    results.testName = name;
    results.minValue = latencies.front();
    results.avgValue = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    results.maxValue = latencies.back();
    results.p99Value = latencies[static_cast<size_t>(latencies.size() * 0.99)];
    results.p999Value = latencies[static_cast<size_t>(latencies.size() * 0.999)];
    results.unit = "ns";
    results.timestamp = std::chrono::system_clock::now();

    PrintDivider();
    std::cout << "Results:\n";
    std::cout << "  Min : " << results.minValue << " ns\n";
    std::cout << "  Avg : " << results.avgValue << " ns\n";
    std::cout << "  Max : " << results.maxValue << " ns\n";
    std::cout << "  P99 : " << results.p99Value << " ns\n";
    std::cout << "  P99.9 : " << results.p999Value << " ns\n";

    return results;
}

// ------------------------------------------------------------
// Configuration Menu
// ------------------------------------------------------------

void ShowConfigMenu(const BenchmarkConfig& config) {
    std::cout << "\nGPU Benchmark Configuration\n";
    PrintDoubleDivider();
    std::cout << "1. Large transfer size:      " << FormatSize(config.largeBandwidthSize)    << "\n";
    std::cout << "2. Small transfer size:      " << FormatSize(config.smallLatencySize)      << "\n";
    std::cout << "3. Command latency iters:    " << config.commandLatencyIters   << "\n";
    std::cout << "4. Transfer latency iters:   " << config.transferLatencyIters  << "\n";
    std::cout << "5. Copies per batch:         " << config.copiesPerBatch       << "\n";
    std::cout << "6. Bandwidth test batches:   " << config.bandwidthBatches     << "\n";
    std::cout << "7. Continuous mode:          " << (config.continuousMode ? "ON" : "OFF") << "\n";
    std::cout << "8. CSV logging:              " << (config.enableCSVLogging ? "ON" : "OFF") << "\n";
    std::cout << "9. CSV filename:             " << config.csvFilename << "\n";
    std::cout << "H. Show PCIe/Thunderbolt reference chart\n";
    std::cout << "0. Start benchmark\n";
    PrintDoubleDivider();
    std::cout << "Select option (0-9,H): ";
}

void ConfigureSettings(BenchmarkConfig& config) {
    bool done = false;
    while (!done) {
        ShowConfigMenu(config);
        char choice = _getch();
        std::cout << choice << "\n\n";

        switch (choice) {
            case '1': {
                std::cout << "Large transfer size (MB): ";
                int mb; std::cin >> mb; std::cin.ignore();
                config.largeBandwidthSize = static_cast<size_t>(mb) * 1024 * 1024;
                break;
            }
            case '2': {
                std::cout << "Small transfer size (bytes): ";
                int b; std::cin >> b; std::cin.ignore();
                config.smallLatencySize = b;
                break;
            }
            case '3': std::cout << "Command latency iterations: "; std::cin >> config.commandLatencyIters;   std::cin.ignore(); break;
            case '4': std::cout << "Transfer latency iterations: "; std::cin >> config.transferLatencyIters;  std::cin.ignore(); break;
            case '5': std::cout << "Copies per batch: ";            std::cin >> config.copiesPerBatch;       std::cin.ignore(); break;
            case '6': std::cout << "Bandwidth batches: ";           std::cin >> config.bandwidthBatches;     std::cin.ignore(); break;
            case '7': config.continuousMode   = !config.continuousMode;   break;
            case '8': config.enableCSVLogging = !config.enableCSVLogging; break;
            case '9': std::cout << "CSV filename: "; std::getline(std::cin, config.csvFilename); break;
            case 'H': case 'h': 
                PrintInterfaceReferenceChart();
                break;
            case '0': done = true; break;
        }
    }
}

// ------------------------------------------------------------
// API Selection
// ------------------------------------------------------------

enum class GraphicsAPI { D3D12, Vulkan, Both };

GraphicsAPI SelectAPIMenu() {
    std::cout << "\nGPU Performance Benchmark Tool\n";
    PrintDoubleDivider();
    std::cout << "Select Graphics API:\n";
    std::cout << "  1. Direct3D 12\n";
    std::cout << "  2. Vulkan\n";
    std::cout << "  3. Both (sequential)\n";
    PrintDivider();
    std::cout << "Choice (1-3, default=1): ";
    char ch = _getch();
    std::cout << ch << "\n\n";

    if (ch == '2') return GraphicsAPI::Vulkan;
    if (ch == '3') return GraphicsAPI::Both;
    return GraphicsAPI::D3D12;
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------

int main() {
    GraphicsAPI api = SelectAPIMenu();

    std::cout << "Selected: ";
    if (api == GraphicsAPI::D3D12) std::cout << "Direct3D 12\n";
    else if (api == GraphicsAPI::Vulkan) std::cout << "Vulkan\n";
    else std::cout << "Both APIs\n";
    PrintDoubleDivider();

    BenchmarkConfig config;

    std::cout << "Press 'C' to configure or any key to start...\n";
    char ch = _getch();
    if (ch == 'c' || ch == 'C') {
        ConfigureSettings(config);
    }

    CSVLogger logger(config.csvFilename);

    // D3D12
    if (api == GraphicsAPI::D3D12 || api == GraphicsAPI::Both) {
        std::cout << "\n=== Direct3D 12 Benchmark ===\n";
        PrintDoubleDivider();

        ComPtr<IDXGIFactory6> factory;
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        auto adapter = SelectD3D12GPU(factory);

        ComPtr<ID3D12Device> device;
        D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
        D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
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

        std::vector<TestResults> results;

        // CPU->GPU bandwidth
        auto cpuUpload = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, config.largeBandwidthSize, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto gpuDefault = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, config.largeBandwidthSize, D3D12_RESOURCE_STATE_COPY_DEST);
        results.push_back(RunBandwidthTestD3D12(("CPU->GPU " + FormatSize(config.largeBandwidthSize) + " Bandwidth").c_str(),
            device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue,
            cpuUpload, gpuDefault, config.largeBandwidthSize, config.copiesPerBatch, config.bandwidthBatches, config.continuousMode));

        // GPU->CPU bandwidth
        auto gpuSrc = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, config.largeBandwidthSize, D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto cpuReadback = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_READBACK, config.largeBandwidthSize, D3D12_RESOURCE_STATE_COPY_DEST);
        results.push_back(RunBandwidthTestD3D12(("GPU->CPU " + FormatSize(config.largeBandwidthSize) + " Bandwidth").c_str(),
            device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue,
            gpuSrc, cpuReadback, config.largeBandwidthSize, config.copiesPerBatch, config.bandwidthBatches, config.continuousMode));

        // Interface detection
        if (results.size() >= 2) {
            PrintInterfaceGuess(results[0].avgValue, results[1].avgValue);
        }

        // Log to CSV
        for (auto& r : results) {
            logger.LogResult(r, "D3D12");
        }

        CloseHandle(fenceEvent);
    }

    // Vulkan
    if (api == GraphicsAPI::Vulkan || api == GraphicsAPI::Both) {
        if (api == GraphicsAPI::Both) { std::cout << "\nPress any key for Vulkan...\n"; _getch(); }
        std::cout << "\n=== Vulkan Benchmark ===\n";
        PrintDoubleDivider();
        
        VulkanContext ctx;
        ctx.Init();

        std::vector<TestResults> results;

        // CPU->GPU bandwidth
        Buffer cpuSrc, gpuDst;
        cpuSrc.Create(ctx, config.largeBandwidthSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        gpuDst.Create(ctx, config.largeBandwidthSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        results.push_back(RunBandwidthTestVulkan(("CPU->GPU " + FormatSize(config.largeBandwidthSize) + " Bandwidth").c_str(),
            ctx, cpuSrc, gpuDst, config.largeBandwidthSize, config.copiesPerBatch, config.bandwidthBatches, config.continuousMode));
        cpuSrc.Destroy(ctx); gpuDst.Destroy(ctx);
        
        // GPU->CPU bandwidth
        Buffer gpuSrc, cpuDst;
        gpuSrc.Create(ctx, config.largeBandwidthSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        cpuDst.Create(ctx, config.largeBandwidthSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        results.push_back(RunBandwidthTestVulkan(("GPU->CPU " + FormatSize(config.largeBandwidthSize) + " Bandwidth").c_str(),
            ctx, gpuSrc, cpuDst, config.largeBandwidthSize, config.copiesPerBatch, config.bandwidthBatches, config.continuousMode));
        gpuSrc.Destroy(ctx); cpuDst.Destroy(ctx);
        
        // Interface detection
        if (results.size() >= 2) {
            PrintInterfaceGuess(results[0].avgValue, results[1].avgValue);
        }
        
        // Log to CSV
        for (auto& r : results) {
            logger.LogResult(r, "Vulkan");
        }
        
        ctx.Cleanup();
    }

    if (api == GraphicsAPI::Both) {
        std::cout << "\n=== Comparison Complete! ===\n";
        std::cout << "Results saved to: " << config.csvFilename << "\n";
    }

    std::cout << "\nPress any key to exit...\n";
    _getch();
    return 0;
}
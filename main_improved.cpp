#include <windows.h>   
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>
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

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

#include "bandwidth_analysis.h"

// #include "bandwidth_reference.h"

// ------------------------------------------------------------
// Configuration
// ------------------------------------------------------------
struct BenchmarkConfig {
    size_t largeBandwidthSize = 256ull * 1024 * 1024;  // 256MB default
    size_t smallLatencySize = 1;                        // 1 byte default
    int commandLatencyIters = 100000;
    int transferLatencyIters = 10000;
    int copiesPerBatch = 8;
    int bandwidthBatches = 32;
    bool continuousMode = false;
    bool enableCSVLogging = true;
    bool showReferenceChart = false;
    std::string csvFilename = "gpu_benchmark_results.csv";
};

// ------------------------------------------------------------
// Results Structure
// ------------------------------------------------------------
struct TestResults {
    std::string testName;
    double minValue;
    double avgValue;
    double maxValue;
    double p99Value;   // For latency tests
    double p999Value;  // For latency tests
    std::string unit;
    std::chrono::system_clock::time_point timestamp;
};

// ------------------------------------------------------------
// CSV Logger
// ------------------------------------------------------------
class CSVLogger {
public:
    CSVLogger(const std::string& filename) : filename_(filename) {
        // Check if file exists to determine if we need to write header
        std::ifstream checkFile(filename);
        bool fileExists = checkFile.good();
        checkFile.close();

        file_.open(filename, std::ios::app);
        if (!fileExists && file_.is_open()) {
            WriteHeader();
        }
    }

    ~CSVLogger() {
        if (file_.is_open()) {
            file_.close();
        }
    }

    void LogResult(const TestResults& result) {
        if (!file_.is_open()) return;

        auto time = std::chrono::system_clock::to_time_t(result.timestamp);
        std::tm tm;
        localtime_s(&tm, &time);
        
        file_ << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << ","
              << "D3D12,"  // API name
              << result.testName << ","
              << result.minValue << ","
              << result.avgValue << ","
              << result.maxValue << ","
              << result.p99Value << ","
              << result.p999Value << ","
              << result.unit << "\n";
        file_.flush();
    }

private:
    void WriteHeader() {
        file_ << "Timestamp,API,Test Name,Min,Avg,Max,99th Percentile,99.9th Percentile,Unit\n";
    }

    std::string filename_;
    std::ofstream file_;
};

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------
// static void PrintDivider() { std::cout << "----------------------------------------------\n"; }
// static void PrintDoubleDivider() { std::cout << "==============================================\n"; }

static std::string FormatSize(size_t bytes) {
    if (bytes >= 1024ull * 1024 * 1024) {
        return std::to_string(bytes / (1024ull * 1024 * 1024)) + "GB";
    } else if (bytes >= 1024 * 1024) {
        return std::to_string(bytes / (1024 * 1024)) + "MB";
    } else if (bytes >= 1024) {
        return std::to_string(bytes / 1024) + "KB";
    } else {
        return std::to_string(bytes) + "B";
    }
}

static bool CheckForEscape() {
    if (_kbhit()) {
        int ch = _getch();
        return (ch == 27); // ESC key
    }
    return false;
}

static void WaitForFence(ID3D12CommandQueue* queue, ID3D12Fence* fence, HANDLE eventHandle, UINT64& fenceValue) {
    queue->Signal(fence, fenceValue);
    if (fence->GetCompletedValue() < fenceValue) {
        fence->SetEventOnCompletion(fenceValue, eventHandle);
        WaitForSingleObject(eventHandle, INFINITE);
    }
    fenceValue++;
}

static ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heapType, size_t size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> res;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&res));
    return res;
}

// ------------------------------------------------------------
// Generic Bandwidth Test
// ------------------------------------------------------------
TestResults RunBandwidthTest(
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
    bool checkEscape = false)
{
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
            if (checkEscape) std::cout << "(Press ESC to stop) ";
            std::cout << std::flush;
            lastPercent = percent;
        }

        if (checkEscape && CheckForEscape()) {
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
    
    // Compare to known interface speeds
    PrintBandwidthComparison(results.avgValue, name);
    
    PrintDoubleDivider();

    return results;
}

// ------------------------------------------------------------
// Generic Latency Test
// ------------------------------------------------------------
TestResults RunLatencyTest(
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
    bool checkEscape = false)
{
    std::cout << name << "\n";
    PrintDivider();

    std::vector<double> latencies; latencies.reserve(iterations);
    int lastPercent = -1;
    bool escaped = false;

    for (int i = 0; i < iterations && !escaped; i++) {
        allocator->Reset();
        list->Reset(allocator, nullptr);

        auto start = std::chrono::high_resolution_clock::now();
        list->CopyResource(dst.Get(), src.Get());
        list->Close();

        ID3D12CommandList* cl[] = { list };
        queue->ExecuteCommandLists(1, cl);
        WaitForFence(queue, fence, fenceEvent, fenceValue);
        auto end = std::chrono::high_resolution_clock::now();

        latencies.push_back(std::chrono::duration<double, std::micro>(end - start).count());

        int percent = int((i + 1) * 100 / iterations);
        if (percent != lastPercent) { 
            std::cout << "\rProgress: " << percent << "% ";
            if (checkEscape) std::cout << "(Press ESC to stop) ";
            std::cout << std::flush;
            lastPercent = percent;
        }

        if (checkEscape && CheckForEscape()) {
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
    results.p99Value = latencies[size_t(latencies.size() * 0.99)];
    results.p999Value = latencies[size_t(latencies.size() * 0.999)];
    results.unit = "us";
    results.timestamp = std::chrono::system_clock::now();

    PrintDivider();
    std::cout << "Results:\n";
    std::cout << "  Min        : " << results.minValue << " us\n";
    std::cout << "  Avg        : " << results.avgValue << " us\n";
    std::cout << "  99% worst  : " << results.p99Value << " us\n";
    std::cout << "  99.9% worst: " << results.p999Value << " us\n";
    PrintDoubleDivider();

    return results;
}

// ------------------------------------------------------------
// Command Submission Latency (CPU -> GPU)
// ------------------------------------------------------------
TestResults RunCommandLatencyTest(
    ID3D12CommandQueue* queue,
    ID3D12CommandAllocator* allocator,
    ID3D12GraphicsCommandList* list,
    ID3D12Fence* fence,
    HANDLE fenceEvent,
    UINT64& fenceValue,
    int iterations,
    bool checkEscape = false)
{
    std::cout << "Test 3 - CPU -> GPU Command Submission Latency\n";
    PrintDivider();

    std::vector<double> latencies; latencies.reserve(iterations);
    int lastPercent = -1;
    bool escaped = false;

    for (int i = 0; i < iterations && !escaped; i++) {
        allocator->Reset();
        list->Reset(allocator, nullptr);
        list->Close();

        auto start = std::chrono::high_resolution_clock::now();
        ID3D12CommandList* cl[] = { list };
        queue->ExecuteCommandLists(1, cl);
        WaitForFence(queue, fence, fenceEvent, fenceValue);
        auto end = std::chrono::high_resolution_clock::now();

        latencies.push_back(std::chrono::duration<double, std::micro>(end - start).count());

        int percent = int((i + 1) * 100 / iterations);
        if (percent != lastPercent) { 
            std::cout << "\rProgress: " << percent << "% ";
            if (checkEscape) std::cout << "(Press ESC to stop) ";
            std::cout << std::flush;
            lastPercent = percent;
        }

        if (checkEscape && CheckForEscape()) {
            escaped = true;
            std::cout << "\n[Stopped by user]\n";
        }
    }
    std::cout << "\n";

    std::sort(latencies.begin(), latencies.end());
    
    TestResults results;
    results.testName = "CPU->GPU Command Submission Latency";
    results.minValue = latencies.front();
    results.avgValue = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    results.maxValue = latencies.back();
    results.p99Value = latencies[size_t(latencies.size() * 0.99)];
    results.p999Value = latencies[size_t(latencies.size() * 0.999)];
    results.unit = "us";
    results.timestamp = std::chrono::system_clock::now();

    PrintDivider();
    std::cout << "Results:\n";
    std::cout << "  Min        : " << results.minValue << " us\n";
    std::cout << "  Avg        : " << results.avgValue << " us\n";
    std::cout << "  99% worst  : " << results.p99Value << " us\n";
    std::cout << "  99.9% worst: " << results.p999Value << " us\n";
    PrintDoubleDivider();

    return results;
}

// ------------------------------------------------------------
// Configuration Menu
// ------------------------------------------------------------
void ShowConfigMenu(BenchmarkConfig& config) {
    std::cout << "\n";
    PrintDoubleDivider();
    std::cout << "GPU Benchmark Configuration\n";
    PrintDoubleDivider();
    std::cout << "1. Large transfer size: " << FormatSize(config.largeBandwidthSize) << "\n";
    std::cout << "2. Small transfer size: " << FormatSize(config.smallLatencySize) << "\n";
    std::cout << "3. Command latency iterations: " << config.commandLatencyIters << "\n";
    std::cout << "4. Transfer latency iterations: " << config.transferLatencyIters << "\n";
    std::cout << "5. Copies per batch: " << config.copiesPerBatch << "\n";
    std::cout << "6. Bandwidth test batches: " << config.bandwidthBatches << "\n";
    std::cout << "7. Continuous mode: " << (config.continuousMode ? "ON" : "OFF") << "\n";
    std::cout << "8. CSV logging: " << (config.enableCSVLogging ? "ON" : "OFF") << "\n";
    std::cout << "9. CSV filename: " << config.csvFilename << "\n";
    std::cout << "H. Show PCIe/Thunderbolt bandwidth reference chart\n";
    std::cout << "0. Start benchmark\n";
    PrintDoubleDivider();
    std::cout << "Select option (0-9, H): ";
}

void ConfigureSettings(BenchmarkConfig& config) {
    bool done = false;
    while (!done) {
        ShowConfigMenu(config);
        
        char choice = _getch();
        std::cout << choice << "\n\n";

        switch (choice) {
            case '1': {
                std::cout << "Enter large transfer size in MB: ";
                int mb;
                std::cin >> mb;
                config.largeBandwidthSize = static_cast<size_t>(mb) * 1024 * 1024;
                break;
            }
            case '2': {
                std::cout << "Enter small transfer size in bytes: ";
                int bytes;
                std::cin >> bytes;
                config.smallLatencySize = bytes;
                break;
            }
            case '3': {
                std::cout << "Enter command latency iterations: ";
                std::cin >> config.commandLatencyIters;
                break;
            }
            case '4': {
                std::cout << "Enter transfer latency iterations: ";
                std::cin >> config.transferLatencyIters;
                break;
            }
            case '5': {
                std::cout << "Enter copies per batch: ";
                std::cin >> config.copiesPerBatch;
                break;
            }
            case '6': {
                std::cout << "Enter bandwidth test batches: ";
                std::cin >> config.bandwidthBatches;
                break;
            }
            case '7': {
                config.continuousMode = !config.continuousMode;
                break;
            }
            case '8': {
                config.enableCSVLogging = !config.enableCSVLogging;
                break;
            }
            case '9': {
                std::cout << "Enter CSV filename: ";
                std::cin >> config.csvFilename;
                break;
            }
            case 'H':
            case 'h': {
                PrintInterfaceReferenceChart();
                std::cout << "Press any key to continue...";
                _getch();
                break;
            }
            case '0': {
                done = true;
                break;
            }
        }
    }
}

// ------------------------------------------------------------
// Run Full Benchmark Suite
// ------------------------------------------------------------
void RunBenchmarkSuite(
    BenchmarkConfig& config,
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12CommandAllocator* allocator,
    ID3D12GraphicsCommandList* list,
    ID3D12Fence* fence,
    HANDLE fenceEvent,
    UINT64& fenceValue,
    CSVLogger* logger)
{
    std::vector<TestResults> results;

    // Bandwidth Tests
    auto cpuUpload = MakeBuffer(device, D3D12_HEAP_TYPE_UPLOAD, config.largeBandwidthSize, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto gpuDefault = MakeBuffer(device, D3D12_HEAP_TYPE_DEFAULT, config.largeBandwidthSize, D3D12_RESOURCE_STATE_COPY_DEST);
    results.push_back(RunBandwidthTest(
        ("Test 1 - CPU -> GPU " + FormatSize(config.largeBandwidthSize) + " Transfer Bandwidth").c_str(),
        device, queue, allocator, list, fence, fenceEvent, fenceValue,
        cpuUpload, gpuDefault, config.largeBandwidthSize, config.copiesPerBatch, config.bandwidthBatches, config.continuousMode));

    auto gpuSrc = MakeBuffer(device, D3D12_HEAP_TYPE_DEFAULT, config.largeBandwidthSize, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto cpuReadback = MakeBuffer(device, D3D12_HEAP_TYPE_READBACK, config.largeBandwidthSize, D3D12_RESOURCE_STATE_COPY_DEST);
    results.push_back(RunBandwidthTest(
        ("Test 2 - GPU -> CPU " + FormatSize(config.largeBandwidthSize) + " Transfer Bandwidth").c_str(),
        device, queue, allocator, list, fence, fenceEvent, fenceValue,
        gpuSrc, cpuReadback, config.largeBandwidthSize, config.copiesPerBatch, config.bandwidthBatches, config.continuousMode));

    // Latency Tests
    results.push_back(RunCommandLatencyTest(queue, allocator, list, fence, fenceEvent, fenceValue, config.commandLatencyIters, config.continuousMode));

    auto cpuSmall = MakeBuffer(device, D3D12_HEAP_TYPE_UPLOAD, config.smallLatencySize, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto gpuSmall = MakeBuffer(device, D3D12_HEAP_TYPE_DEFAULT, config.smallLatencySize, D3D12_RESOURCE_STATE_COPY_DEST);
    results.push_back(RunLatencyTest(
        ("Test 4 - CPU -> GPU " + FormatSize(config.smallLatencySize) + " Transfer Latency").c_str(),
        device, queue, allocator, list, fence, fenceEvent, fenceValue,
        cpuSmall, gpuSmall, config.transferLatencyIters, config.continuousMode));

    auto gpuSmallSrc = MakeBuffer(device, D3D12_HEAP_TYPE_DEFAULT, config.smallLatencySize, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto cpuSmallDst = MakeBuffer(device, D3D12_HEAP_TYPE_READBACK, config.smallLatencySize, D3D12_RESOURCE_STATE_COPY_DEST);
    results.push_back(RunLatencyTest(
        ("Test 5 - GPU -> CPU " + FormatSize(config.smallLatencySize) + " Transfer Latency").c_str(),
        device, queue, allocator, list, fence, fenceEvent, fenceValue,
        gpuSmallSrc, cpuSmallDst, config.transferLatencyIters, config.continuousMode));

    // Analyze bandwidth and detect interface type
    if (results.size() >= 2) {
        double uploadBandwidth = results[0].avgValue;   // CPU->GPU
        double downloadBandwidth = results[1].avgValue; // GPU->CPU
        PrintInterfaceGuess(uploadBandwidth, downloadBandwidth);
    }

    // Log results to CSV
    if (logger) {
        for (const auto& result : results) {
            logger->LogResult(result);
        }
    }
}

// ------------------------------------------------------------
// GPU Selection
// ------------------------------------------------------------
ComPtr<IDXGIAdapter1> SelectGPU(ComPtr<IDXGIFactory6> factory) {
    std::vector<ComPtr<IDXGIAdapter1>> adapters;
    std::vector<DXGI_ADAPTER_DESC1> descs;
    
    // Enumerate all adapters
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        
        // Skip software adapters
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            adapters.push_back(adapter);
            descs.push_back(desc);
        }
    }
    
    if (adapters.empty()) {
        std::cout << "ERROR: No compatible GPU found!\n";
        exit(1);
    }
    
    if (adapters.size() == 1) {
        std::wcout << L"Found 1 GPU: " << descs[0].Description << L"\n";
        PrintDoubleDivider();
        return adapters[0];
    }
    
    // Multiple GPUs - let user choose
    std::cout << "\n";
    PrintDoubleDivider();
    std::wcout << L"Found " << adapters.size() << L" GPUs:\n";
    PrintDoubleDivider();
    
    for (size_t i = 0; i < adapters.size(); i++) {
        std::wcout << i + 1 << L". " << descs[i].Description << L"\n";
        std::wcout << L"   VRAM: " << (descs[i].DedicatedVideoMemory / (1024 * 1024)) << L" MB\n";
        
        // Decode vendor
        std::wstring vendor = L"Unknown";
        switch (descs[i].VendorId) {
            case 0x10DE: vendor = L"NVIDIA"; break;
            case 0x1002: vendor = L"AMD"; break;
            case 0x8086: vendor = L"Intel"; break;
        }
        std::wcout << L"   Vendor: " << vendor << L"\n";
        
        if (i < adapters.size() - 1) std::cout << "\n";
    }
    
    PrintDoubleDivider();
    std::cout << "Select GPU (1-" << adapters.size() << ", default=1): ";
    
    char ch = _getch();
    std::cout << ch << "\n\n";
    
    int selection = 1;
    if (ch >= '1' && ch <= '9') {
        selection = ch - '0';
    }
    
    if (selection < 1 || selection > (int)adapters.size()) {
        selection = 1;
    }
    
    std::wcout << L"Selected: " << descs[selection - 1].Description << L"\n";
    PrintDoubleDivider();
    
    return adapters[selection - 1];
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------
int main() {
    SetConsoleOutputCP(CP_UTF8);

    BenchmarkConfig config;

    std::cout << "==============================================\n";
    std::cout << "      GPU Performance Benchmark Tool\n";
    std::cout << "==============================================\n\n";
    std::cout << "Press 'C' to configure settings or any other key to use defaults...\n";
    
    char ch = _getch();
    if (ch == 'c' || ch == 'C') {
        ConfigureSettings(config);
    }

    ComPtr<IDXGIFactory6> factory; CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter = SelectGPU(factory);

    DXGI_ADAPTER_DESC1 ad; adapter->GetDesc1(&ad);
    std::wcout << L"\nUsing GPU: " << ad.Description << L"\n";
    PrintDoubleDivider();

    ComPtr<ID3D12Device> device; D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    ComPtr<ID3D12CommandAllocator> allocator; device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    ComPtr<ID3D12GraphicsCommandList> list; device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)); list->Close();
    ComPtr<ID3D12Fence> fence; device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr); UINT64 fenceValue = 1;

    CSVLogger* logger = config.enableCSVLogging ? new CSVLogger(config.csvFilename) : nullptr;

    if (config.continuousMode) {
        std::cout << "\nContinuous mode enabled. Press ESC during any test to stop.\n";
        std::cout << "Press any key to begin...";
        _getch();
        std::cout << "\n\n";

        int runNumber = 1;
        while (true) {
            std::cout << "\n*** RUN #" << runNumber << " ***\n";
            RunBenchmarkSuite(config, device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue, logger);
            
            std::cout << "\nRun #" << runNumber << " complete. Press ESC to exit or any other key to continue...\n";
            ch = _getch();
            if (ch == 27) break;
            runNumber++;
        }
    } else {
        RunBenchmarkSuite(config, device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue, logger);
    }

    delete logger;
    CloseHandle(fenceEvent);
    
    //std::cout << "\nPress any key to exit..."; 
    //_getch();
    return 0;
}

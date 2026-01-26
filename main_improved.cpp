// ============================================================================
// GPU-PCIe-Test v2.0 - Direct3D 12 Benchmark
// ============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>   
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <conio.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

#include "bandwidth_analysis.h"

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
    int gpuIndex = 0;  // 0 = prompt user
    size_t bufferSizeMB = 256;
    std::string outputFile = "gpu_benchmark_results.csv";
};

void PrintHelp(const char* exeName) {
    std::cout << "\n";
    PrintDoubleDivider();
    std::cout << "GPU-PCIe-Test v2.0 - Direct3D 12 Benchmark\n";
    PrintDoubleDivider();
    std::cout << "\nUsage: " << exeName << " [options]\n\n";
    std::cout << "Options:\n";
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
    std::cout << "  " << exeName << " -q                    Quick benchmark\n";
    std::cout << "  " << exeName << " -r 5 --no-bidir       5 runs, skip bidirectional\n";
    std::cout << "  " << exeName << " -g 2 --size 512       GPU #2, 512MB buffer\n";
    std::cout << "\nNo arguments: Interactive configuration menu\n";
    PrintDoubleDivider();
}

CLIOptions ParseArgs(int argc, char* argv[]) {
    CLIOptions opts;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help" || arg == "/?") {
            opts.showHelp = true; opts.hasOptions = true;
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
//                           HELPER FUNCTIONS
// ============================================================================

static bool CheckForEscape() { if (_kbhit()) return _getch() == 27; return false; }

GPUInfo GetD3D12GPUInfo(IDXGIAdapter1* adapter) {
    GPUInfo info;
    DXGI_ADAPTER_DESC1 desc;
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
        char nameBuffer[256];
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nameBuffer, sizeof(nameBuffer), nullptr, nullptr);
        info.name = nameBuffer;
        info.vendorId = desc.VendorId;
        info.deviceId = desc.DeviceId;
        info.vendor = GetVendorName(desc.VendorId);
        info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
        info.sharedSystemMemory = desc.SharedSystemMemory;
    }
    return info;
}

void PrintD3D12GPUInfo(const GPUInfo& info) {
    PrintDoubleDivider();
    std::cout << "GPU Information (D3D12)\n";
    PrintDoubleDivider();
    std::cout << "  Name:           " << info.name << "\n";
    std::cout << "  Vendor:         " << info.vendor << " (0x" << std::hex << info.vendorId << std::dec << ")\n";
    std::cout << "  Dedicated VRAM: " << FormatMemorySize(info.dedicatedVideoMemory) << "\n";
    std::cout << "  Shared Memory:  " << FormatMemorySize(info.sharedSystemMemory) << "\n";
    if (info.dedicatedVideoMemory == 0) std::cout << "  [i] This appears to be an integrated GPU (APU)\n";
    PrintDoubleDivider();
}

static ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* dev, D3D12_HEAP_TYPE heapType, size_t size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{ heapType };
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size; desc.Height = 1; desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> res;
    dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res));
    return res;
}

static void WaitForFence(ID3D12CommandQueue* q, ID3D12Fence* f, HANDLE ev, UINT64& val) {
    q->Signal(f, val);
    if (f->GetCompletedValue() < val) { f->SetEventOnCompletion(val, ev); WaitForSingleObject(ev, INFINITE); }
    val++;
}

ComPtr<IDXGIAdapter1> SelectD3D12GPU(ComPtr<IDXGIFactory6> factory, bool showInfo, int gpuIndex = 0) {
    std::vector<ComPtr<IDXGIAdapter1>> adapters;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) adapters.push_back(adapter);
    if (adapters.empty()) { std::cerr << "No DirectX 12 capable GPU found!\n"; exit(1); }
    
    if (gpuIndex > 0 && gpuIndex <= (int)adapters.size()) {
        GPUInfo info = GetD3D12GPUInfo(adapters[gpuIndex-1].Get());
        std::cout << "Using GPU #" << gpuIndex << ": " << info.name << "\n";
        if (showInfo) PrintD3D12GPUInfo(info);
        return adapters[gpuIndex-1];
    }
    
    if (adapters.size() == 1) {
        GPUInfo info = GetD3D12GPUInfo(adapters[0].Get());
        std::cout << "Found 1 GPU: " << info.name << "\n"; PrintDoubleDivider();
        if (showInfo) PrintD3D12GPUInfo(info);
        return adapters[0];
    }
    std::cout << "Found " << adapters.size() << " GPUs:\n"; PrintDoubleDivider();
    for (size_t i = 0; i < adapters.size(); i++) {
        DXGI_ADAPTER_DESC1 desc; adapters[i]->GetDesc1(&desc);
        char name[256]; WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, 256, nullptr, nullptr);
        std::cout << (i+1) << ". " << name << " (" << (desc.DedicatedVideoMemory/(1024*1024)) << " MB)\n";
    }
    std::cout << "Select GPU (1-" << adapters.size() << ", default=1): ";
    std::string input; std::getline(std::cin, input);
    int choice = 1; if (!input.empty()) try { choice = std::stoi(input); } catch (...) {}
    choice = std::max(1, std::min(choice, (int)adapters.size()));
    GPUInfo info = GetD3D12GPUInfo(adapters[choice-1].Get());
    std::cout << "\nSelected: " << info.name << "\n"; PrintDoubleDivider();
    if (showInfo) PrintD3D12GPUInfo(info);
    return adapters[choice-1];
}

// ============================================================================
//                          BENCHMARK TESTS
// ============================================================================

TestResults RunBandwidthTest(const char* name, ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* list, ID3D12Fence* fence,
    HANDLE fenceEvent, UINT64& fenceValue, ComPtr<ID3D12Resource> src, ComPtr<ID3D12Resource> dst,
    size_t bufferSize, int copiesPerBatch, int batches, bool checkEscape = false) {
    std::cout << name << "\n"; PrintDivider();
    D3D12_QUERY_HEAP_DESC qhd{}; qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qhd.Count = 2;
    ComPtr<ID3D12QueryHeap> queryHeap; device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&queryHeap));
    auto queryReadback = MakeBuffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(UINT64)*2, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 timestampFreq = 0; queue->GetTimestampFrequency(&timestampFreq);
    std::vector<double> bandwidths; bandwidths.reserve(batches);
    int lastPercent = -1; bool escaped = false;
    for (int i = 0; i < batches && !escaped; i++) {
        allocator->Reset(); list->Reset(allocator, nullptr);
        list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        for (int j = 0; j < copiesPerBatch; j++) list->CopyResource(dst.Get(), src.Get());
        list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, queryReadback.Get(), 0);
        list->Close();
        ID3D12CommandList* cl[] = { list }; queue->ExecuteCommandLists(1, cl);
        WaitForFence(queue, fence, fenceEvent, fenceValue);
        UINT64* ts = nullptr; D3D12_RANGE range{0, sizeof(UINT64)*2};
        queryReadback->Map(0, &range, (void**)&ts);
        double seconds = double(ts[1] - ts[0]) / double(timestampFreq);
        queryReadback->Unmap(0, nullptr);
        bandwidths.push_back((double(bufferSize) * copiesPerBatch / (1024.0*1024.0*1024.0)) / seconds);
        int percent = (i+1)*100/batches;
        if (percent != lastPercent) { std::cout << "\rProgress: " << percent << "% " << std::flush; lastPercent = percent; }
        if (checkEscape && CheckForEscape()) escaped = true;
    }
    std::cout << "\n";
    std::sort(bandwidths.begin(), bandwidths.end());
    TestResults results; results.testName = name;
    results.minValue = bandwidths.front();
    results.avgValue = std::accumulate(bandwidths.begin(), bandwidths.end(), 0.0) / bandwidths.size();
    results.maxValue = bandwidths.back();
    results.p99Value = bandwidths[size_t(bandwidths.size() * 0.99)];
    results.p999Value = bandwidths[size_t(bandwidths.size() * 0.999)];
    results.unit = "GB/s"; results.timestamp = std::chrono::system_clock::now();
    PrintDivider();
    std::cout << "Results:\n  Min : " << results.minValue << " GB/s\n  Avg : " << results.avgValue << " GB/s\n  Max : " << results.maxValue << " GB/s\n";
    PrintDoubleDivider();
    return results;
}

TestResults RunLatencyTest(const char* name, ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* list, ID3D12Fence* fence,
    HANDLE fenceEvent, UINT64& fenceValue, ComPtr<ID3D12Resource> src, ComPtr<ID3D12Resource> dst,
    int iterations, bool checkEscape = false) {
    std::cout << name << "\n"; PrintDivider();
    std::vector<double> latencies; latencies.reserve(iterations);
    int lastPercent = -1; bool escaped = false;
    for (int i = 0; i < iterations && !escaped; i++) {
        allocator->Reset(); list->Reset(allocator, nullptr);
        auto start = std::chrono::high_resolution_clock::now();
        list->CopyResource(dst.Get(), src.Get()); list->Close();
        ID3D12CommandList* cl[] = { list }; queue->ExecuteCommandLists(1, cl);
        WaitForFence(queue, fence, fenceEvent, fenceValue);
        auto end = std::chrono::high_resolution_clock::now();
        latencies.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        int percent = (i+1)*100/iterations;
        if (percent != lastPercent) { std::cout << "\rProgress: " << percent << "% " << std::flush; lastPercent = percent; }
        if (checkEscape && CheckForEscape()) escaped = true;
    }
    std::cout << "\n";
    std::sort(latencies.begin(), latencies.end());
    TestResults results; results.testName = name;
    results.minValue = latencies.front();
    results.avgValue = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    results.maxValue = latencies.back();
    results.p99Value = latencies[size_t(latencies.size() * 0.99)];
    results.p999Value = latencies[size_t(latencies.size() * 0.999)];
    results.unit = "us"; results.timestamp = std::chrono::system_clock::now();
    PrintDivider();
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Results:\n  Min        : " << results.minValue << " us\n  Avg        : " << results.avgValue << " us\n";
    std::cout << "  Max        : " << results.maxValue << " us\n";
    PrintDoubleDivider();
    return results;
}

TestResults RunCommandLatencyTest(ID3D12CommandQueue* queue, ID3D12CommandAllocator* allocator,
    ID3D12GraphicsCommandList* list, ID3D12Fence* fence, HANDLE fenceEvent, UINT64& fenceValue,
    int iterations, bool checkEscape = false) {
    const char* name = "Command Latency";
    std::cout << name << "\n"; PrintDivider();
    std::vector<double> latencies; latencies.reserve(iterations);
    int lastPercent = -1; bool escaped = false;
    for (int i = 0; i < iterations && !escaped; i++) {
        auto start = std::chrono::high_resolution_clock::now();
        allocator->Reset(); list->Reset(allocator, nullptr); list->Close();
        ID3D12CommandList* cl[] = { list }; queue->ExecuteCommandLists(1, cl);
        WaitForFence(queue, fence, fenceEvent, fenceValue);
        auto end = std::chrono::high_resolution_clock::now();
        latencies.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        int percent = (i+1)*100/iterations;
        if (percent != lastPercent) { std::cout << "\rProgress: " << percent << "% " << std::flush; lastPercent = percent; }
        if (checkEscape && CheckForEscape()) escaped = true;
    }
    std::cout << "\n";
    std::sort(latencies.begin(), latencies.end());
    TestResults results; results.testName = name;
    results.minValue = latencies.front();
    results.avgValue = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    results.maxValue = latencies.back();
    results.p99Value = latencies[size_t(latencies.size() * 0.99)];
    results.p999Value = latencies[size_t(latencies.size() * 0.999)];
    results.unit = "us"; results.timestamp = std::chrono::system_clock::now();
    PrintDivider();
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Results:\n  Min        : " << results.minValue << " us\n  Avg        : " << results.avgValue << " us\n";
    std::cout << "  Max        : " << results.maxValue << " us\n";
    PrintDoubleDivider();
    return results;
}

TestResults RunBidirectionalTest(const char* name, ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* list, ID3D12Fence* fence,
    HANDLE fenceEvent, UINT64& fenceValue, size_t bufferSize, int copiesPerBatch, int batches, bool checkEscape = false) {
    std::cout << name << "\n"; PrintDivider();
    std::cout << "(Simultaneous CPU->GPU and GPU->CPU transfers)\n";
    auto cpuUpload = MakeBuffer(device, D3D12_HEAP_TYPE_UPLOAD, bufferSize, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto gpuDefault = MakeBuffer(device, D3D12_HEAP_TYPE_DEFAULT, bufferSize, D3D12_RESOURCE_STATE_COPY_DEST);
    auto gpuSrc = MakeBuffer(device, D3D12_HEAP_TYPE_DEFAULT, bufferSize, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto cpuReadback = MakeBuffer(device, D3D12_HEAP_TYPE_READBACK, bufferSize, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_QUERY_HEAP_DESC qhd{}; qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qhd.Count = 2;
    ComPtr<ID3D12QueryHeap> queryHeap; device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&queryHeap));
    auto queryReadback = MakeBuffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(UINT64)*2, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 timestampFreq = 0; queue->GetTimestampFrequency(&timestampFreq);
    std::vector<double> bandwidths; bandwidths.reserve(batches);
    int lastPercent = -1; bool escaped = false;
    for (int i = 0; i < batches && !escaped; i++) {
        allocator->Reset(); list->Reset(allocator, nullptr);
        list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        for (int j = 0; j < copiesPerBatch; j++) {
            list->CopyResource(gpuDefault.Get(), cpuUpload.Get());
            list->CopyResource(cpuReadback.Get(), gpuSrc.Get());
        }
        list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, queryReadback.Get(), 0);
        list->Close();
        ID3D12CommandList* cl[] = { list }; queue->ExecuteCommandLists(1, cl);
        WaitForFence(queue, fence, fenceEvent, fenceValue);
        UINT64* ts = nullptr; D3D12_RANGE range{0, sizeof(UINT64)*2};
        queryReadback->Map(0, &range, (void**)&ts);
        double seconds = double(ts[1] - ts[0]) / double(timestampFreq);
        queryReadback->Unmap(0, nullptr);
        bandwidths.push_back((double(bufferSize) * copiesPerBatch * 2 / (1024.0*1024.0*1024.0)) / seconds);
        int percent = (i+1)*100/batches;
        if (percent != lastPercent) { std::cout << "\rProgress: " << percent << "% " << std::flush; lastPercent = percent; }
        if (checkEscape && CheckForEscape()) escaped = true;
    }
    std::cout << "\n";
    std::sort(bandwidths.begin(), bandwidths.end());
    TestResults results; results.testName = name;
    results.minValue = bandwidths.front();
    results.avgValue = std::accumulate(bandwidths.begin(), bandwidths.end(), 0.0) / bandwidths.size();
    results.maxValue = bandwidths.back();
    results.p99Value = bandwidths[size_t(bandwidths.size() * 0.99)];
    results.p999Value = bandwidths[size_t(bandwidths.size() * 0.999)];
    results.unit = "GB/s"; results.timestamp = std::chrono::system_clock::now();
    PrintDivider();
    std::cout << "Results (combined upload + download):\n  Min : " << results.minValue << " GB/s\n  Avg : " << results.avgValue << " GB/s\n  Max : " << results.maxValue << " GB/s\n";
    PrintDoubleDivider();
    return results;
}

// ============================================================================
//                        CONFIGURATION MENU
// ============================================================================

void ShowConfigMenu(BenchmarkConfig& config) {
    std::cout << "\n"; PrintDoubleDivider();
    std::cout << "D3D12 Benchmark Configuration\n"; PrintDoubleDivider();
    std::cout << "  1. Bandwidth size: " << FormatSize(config.largeBandwidthSize) << "\n";
    std::cout << "  2. Latency size:   " << FormatSize(config.smallLatencySize) << "\n";
    std::cout << "  3. Cmd iterations: " << config.commandLatencyIters << "\n";
    std::cout << "  4. Xfer iterations:" << config.transferLatencyIters << "\n";
    std::cout << "  5. Copies/batch:   " << config.copiesPerBatch << "\n";
    std::cout << "  6. Batches:        " << config.bandwidthBatches << "\n";
    std::cout << "  7. Num runs:       " << config.numRuns << "\n";
    std::cout << "  8. Log all runs:   " << (config.logAllRuns ? "Yes" : "No") << "\n";
    std::cout << "  9. Bidirectional:  " << (config.enableBidirectionalTest ? "Yes" : "No") << "\n";
    std::cout << "  0. Start\n"; PrintDivider();
}

void ConfigureSettings(BenchmarkConfig& config) {
    while (true) {
        ShowConfigMenu(config);
        std::cout << "Choice: "; char c = (char)_getch(); std::cout << c << "\n";
        std::string input;
        switch (c) {
            case '1': std::cout << "MB: "; std::getline(std::cin, input); try { config.largeBandwidthSize = std::stoull(input)*1024*1024; } catch (...) {} break;
            case '2': std::cout << "Bytes: "; std::getline(std::cin, input); try { config.smallLatencySize = std::stoull(input); } catch (...) {} break;
            case '3': std::cout << "Iters: "; std::getline(std::cin, input); try { config.commandLatencyIters = std::stoi(input); } catch (...) {} break;
            case '4': std::cout << "Iters: "; std::getline(std::cin, input); try { config.transferLatencyIters = std::stoi(input); } catch (...) {} break;
            case '5': std::cout << "Copies: "; std::getline(std::cin, input); try { config.copiesPerBatch = std::stoi(input); } catch (...) {} break;
            case '6': std::cout << "Batches: "; std::getline(std::cin, input); try { config.bandwidthBatches = std::stoi(input); } catch (...) {} break;
            case '7': std::cout << "Runs (1-10): "; std::getline(std::cin, input); try { config.numRuns = std::max(1, std::min(10, std::stoi(input))); } catch (...) {} break;
            case '8': config.logAllRuns = !config.logAllRuns; break;
            case '9': config.enableBidirectionalTest = !config.enableBidirectionalTest; break;
            case '0': case '\r': case '\n': return;
        }
    }
}

// ============================================================================
//                             MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    
    CLIOptions cli = ParseArgs(argc, argv);
    
    if (cli.showHelp) {
        PrintHelp(argv[0]);
        return 0;
    }
    
    BenchmarkConfig config;
    
    // Apply CLI options
    if (cli.hasOptions) {
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
    }
    
    std::cout << "\n"; PrintDoubleDivider();
    std::cout << "GPU-PCIe-Test v2.0 (D3D12)\n"; PrintDoubleDivider();
    
    if (!cli.hasOptions) {
        std::cout << "\nPress 'C' to configure or any key to start (use -h for CLI options)...\n";
        char key = (char)_getch();
        if (key == 'C' || key == 'c') ConfigureSettings(config);
    } else {
        std::cout << "Runs: " << config.numRuns;
        if (cli.quickMode) std::cout << " (quick)";
        std::cout << " | Buffer: " << FormatSize(config.largeBandwidthSize);
        std::cout << " | Bidir: " << (config.enableBidirectionalTest ? "Yes" : "No") << "\n";
        PrintDoubleDivider();
    }

    // Initialize D3D12
    ComPtr<IDXGIFactory6> factory; CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    auto adapter = SelectD3D12GPU(factory, config.showDetailedGPUInfo, cli.gpuIndex);
    ComPtr<ID3D12Device> device; D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    ComPtr<ID3D12CommandAllocator> allocator; device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    ComPtr<ID3D12GraphicsCommandList> list; device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)); list->Close();
    ComPtr<ID3D12Fence> fence; device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr); UINT64 fenceValue = 1;

    CSVLogger logger(config.csvFilename);
    std::vector<std::vector<TestResults>> allRuns(6);

    for (int run = 0; run < config.numRuns; run++) {
        if (config.numRuns > 1) std::cout << "\n--- Run " << (run+1) << " of " << config.numRuns << " ---\n";
        int idx = 0;
        
        // Bandwidth tests - consistent naming with single-exe
        auto up = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, config.largeBandwidthSize, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto gpuDst = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, config.largeBandwidthSize, D3D12_RESOURCE_STATE_COPY_DEST);
        allRuns[idx++].push_back(RunBandwidthTest(("CPU->GPU " + FormatSize(config.largeBandwidthSize) + " Bandwidth").c_str(), device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue, up, gpuDst, config.largeBandwidthSize, config.copiesPerBatch, config.bandwidthBatches, config.continuousMode));
        
        auto gpuSrc = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, config.largeBandwidthSize, D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto rb = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_READBACK, config.largeBandwidthSize, D3D12_RESOURCE_STATE_COPY_DEST);
        allRuns[idx++].push_back(RunBandwidthTest(("GPU->CPU " + FormatSize(config.largeBandwidthSize) + " Bandwidth").c_str(), device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue, gpuSrc, rb, config.largeBandwidthSize, config.copiesPerBatch, config.bandwidthBatches, config.continuousMode));
        
        if (config.enableBidirectionalTest) {
            allRuns[idx++].push_back(RunBidirectionalTest(("Bidirectional " + FormatSize(config.largeBandwidthSize)).c_str(), device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue, config.largeBandwidthSize, config.copiesPerBatch, config.bandwidthBatches, config.continuousMode));
        }
        
        // Latency tests
        if (!cli.noLatency) {
            allRuns[idx++].push_back(RunCommandLatencyTest(queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue, config.commandLatencyIters, config.continuousMode));
            
            auto smallUp = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, config.smallLatencySize, D3D12_RESOURCE_STATE_GENERIC_READ);
            auto smallGpu = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, config.smallLatencySize, D3D12_RESOURCE_STATE_COPY_DEST);
            allRuns[idx++].push_back(RunLatencyTest(("CPU->GPU " + FormatSize(config.smallLatencySize) + " Latency").c_str(), device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue, smallUp, smallGpu, config.transferLatencyIters, config.continuousMode));
            
            auto smallSrc = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, config.smallLatencySize, D3D12_RESOURCE_STATE_COPY_SOURCE);
            auto smallRb = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_READBACK, config.smallLatencySize, D3D12_RESOURCE_STATE_COPY_DEST);
            allRuns[idx++].push_back(RunLatencyTest(("GPU->CPU " + FormatSize(config.smallLatencySize) + " Latency").c_str(), device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue, smallSrc, smallRb, config.transferLatencyIters, config.continuousMode));
        }
    }

    // Summary
    std::cout << "\n"; PrintDoubleDivider();
    std::cout << "D3D12 Summary\n"; PrintDoubleDivider();
    
    double uploadBW = 0, downloadBW = 0;
    for (auto& runs : allRuns) {
        if (runs.empty()) continue;
        if (config.numRuns > 1) {
            auto agg = AggregateRuns(runs);
            PrintAggregatedResults(agg, config.numRuns);
            if (config.logAllRuns) for (int i = 0; i < (int)runs.size(); i++) logger.LogResult(runs[i], "D3D12", i+1);
            logger.LogAggregated(agg, "D3D12");
            if (runs[0].testName.find("CPU->GPU") != std::string::npos && runs[0].testName.find("Bandwidth") != std::string::npos) uploadBW = agg.avgAvg;
            if (runs[0].testName.find("GPU->CPU") != std::string::npos && runs[0].testName.find("Bandwidth") != std::string::npos) downloadBW = agg.avgAvg;
        } else {
            logger.LogResult(runs[0], "D3D12", 1);
            if (runs[0].testName.find("CPU->GPU") != std::string::npos && runs[0].testName.find("Bandwidth") != std::string::npos) uploadBW = runs[0].avgValue;
            if (runs[0].testName.find("GPU->CPU") != std::string::npos && runs[0].testName.find("Bandwidth") != std::string::npos) downloadBW = runs[0].avgValue;
        }
    }
    if (uploadBW > 0 && downloadBW > 0) PrintInterfaceGuess(uploadBW, downloadBW);
    CloseHandle(fenceEvent);
    
    std::cout << "\nResults saved to: " << config.csvFilename << "\n";
    if (!cli.hasOptions) {
        std::cout << "\nPress any key to exit...\n"; _getch();
    }
    return 0;
}

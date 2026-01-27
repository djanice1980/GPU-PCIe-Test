// ============================================================================
// GPU-PCIe-Test v2.2 - GUI Edition
// Dear ImGui + D3D12 Frontend
// ============================================================================
// This is a graphical frontend for the GPU/PCIe benchmark tool.
// Features:
// - Real-time progress visualization
// - Interactive configuration
// - Results graphs and charts with standard comparisons
// - CSV export
// ============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
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

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// ImGui headers (must be in imgui/ subfolder)
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx12.h"
#include "imgui/implot.h"
#include "imgui/imgui_internal.h"

using Microsoft::WRL::ComPtr;

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
    constexpr int DEFAULT_LATENCY_ITERS = 2000; // Reduced from 10000 - still accurate, much faster
    constexpr int DEFAULT_NUM_RUNS = 3;
    constexpr float BASE_FONT_SCALE = 1.0f;  // User's preferred 2x scaling
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
};

struct BenchmarkResult {
    std::string       testName;
    double            minValue = 0;
    double            avgValue = 0;
    double            maxValue = 0;
    std::string       unit;
    std::vector<double> samples;  // For graphing
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
    int    selectedGPU = 0;
};

struct InterfaceSpeed {
    const char* name;
    double      bandwidth;
    const char* description;
};

// Updated interface standards with OCuLink and corrected TB3/TB4 speeds
// TB3 has variable PCIe allocation (typically 2 lanes), TB4 guarantees more bandwidth
static const InterfaceSpeed INTERFACE_SPEEDS[] = {
    {"PCIe 3.0 x4",    3.94,  "Entry-level GPU slot"},
    {"PCIe 3.0 x8",    7.88,  "Mid-range GPU slot"},
    {"PCIe 3.0 x16",   15.75, "Standard discrete GPU"},
    {"PCIe 4.0 x4",    7.88,  "NVMe / Entry eGPU"},
    {"PCIe 4.0 x8",    15.75, "Mid-range PCIe 4.0"},
    {"PCIe 4.0 x16",   31.51, "High-end discrete GPU"},
    {"PCIe 5.0 x8",    31.51, "PCIe 5.0 mid-range"},
    {"PCIe 5.0 x16",   63.02, "Cutting-edge GPU slot"},
    {"OCuLink 1.0",    3.94,  "PCIe 3.0 x4 external"},
    {"OCuLink 2.0",    7.88,  "PCIe 4.0 x4 external"},
    {"Thunderbolt 3",  2.50,  "40 Gbps (variable PCIe allocation)"},
    {"Thunderbolt 4",  3.00,  "40 Gbps (guaranteed PCIe bandwidth)"},
    {"USB4 40Gbps",    4.00,  "40 Gbps external"},
    {"USB4 80Gbps",    8.00,  "80 Gbps external"},
};

static const int NUM_INTERFACE_SPEEDS = sizeof(INTERFACE_SPEEDS) / sizeof(INTERFACE_SPEEDS[0]);

// ============================================================================
// APPLICATION STATE
// ============================================================================
enum class AppState { Idle, Running, Completed };

struct AppContext {
    // Window
    HWND hwnd = nullptr;
    int  windowWidth = Constants::WINDOW_WIDTH;
    int  windowHeight = Constants::WINDOW_HEIGHT;

    // D3D12 Device (for rendering)
    ComPtr<ID3D12Device>              device;
    ComPtr<ID3D12CommandQueue>        commandQueue;
    ComPtr<IDXGISwapChain3>           swapChain;
    ComPtr<ID3D12DescriptorHeap>      rtvHeap;
    ComPtr<ID3D12DescriptorHeap>      srvHeap;
    ComPtr<ID3D12CommandAllocator>    commandAllocators[Constants::NUM_FRAMES_IN_FLIGHT];
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12Resource>            renderTargets[Constants::NUM_FRAMES_IN_FLIGHT];
    ComPtr<ID3D12Fence>               fence;
    HANDLE                            fenceEvent = nullptr;
    UINT64                            fenceValues[Constants::NUM_FRAMES_IN_FLIGHT] = {};
    UINT64                            currentFenceValue = 0;
    UINT                              frameIndex = 0;
    UINT                              rtvDescriptorSize = 0;

    // D3D12 Benchmark Device (separate for benchmarking)
    ComPtr<ID3D12Device>              benchDevice;
    ComPtr<ID3D12CommandQueue>        benchQueue;
    ComPtr<ID3D12CommandAllocator>    benchAllocator;
    ComPtr<ID3D12GraphicsCommandList> benchList;
    ComPtr<ID3D12Fence>               benchFence;
    HANDLE                            benchFenceEvent = nullptr;
    UINT64                            benchFenceValue = 1;

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
    std::string        currentTest;
    std::mutex         resultsMutex;
    std::vector<BenchmarkResult> results;
    std::thread        benchmarkThread;

    // UI State (removed g_ prefix - cleaner inside struct)
    bool showResultsWindow = false;
    bool showGraphsWindow = false;
    bool showCompareWindow = false;  // NEW: Compare to standards window
    bool showAboutDialog = false;
    bool dockingInitialized = false;
    bool isResizing = false;         // Was: g_isResizing
    bool pendingResize = false;      // Was: g_pendingResize
    int  pendingWidth = 0;           // Was: g_pendingWidth
    int  pendingHeight = 0;          // Was: g_pendingHeight

    // Log buffer
    std::mutex                 logMutex;
    std::vector<std::string>   logLines;

    // Detected interface results
    std::string detectedInterface;
    std::string detectedInterfaceDescription;
    double      uploadBW = 0;
    double      downloadBW = 0;
    double      uploadPercentage = 0;      // Percentage of closest standard
    double      downloadPercentage = 0;    // Percentage of closest standard
    std::string closestUploadStandard;     // Name of closest standard
    std::string closestDownloadStandard;   // Name of closest standard
};

static AppContext g_app;

// Helper to add log messages
void Log(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_app.logMutex);
    g_app.logLines.push_back(msg);
    // Keep last 500 lines
    if (g_app.logLines.size() > 500) {
        g_app.logLines.erase(g_app.logLines.begin());
    }
}

void ClearLog() {
    std::lock_guard<std::mutex> lock(g_app.logMutex);
    g_app.logLines.clear();
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
    if (bytes >= 1024ull * 1024 * 1024) return std::to_string(bytes / (1024 * 1024 * 1024)) + " GB";
    if (bytes >= 1024 * 1024)           return std::to_string(bytes / (1024 * 1024)) + " MB";
    if (bytes >= 1024)                  return std::to_string(bytes / 1024) + " KB";
    return std::to_string(bytes) + " B";
}

std::string FormatMemory(size_t bytes) {
    double gb = bytes / (1024.0 * 1024.0 * 1024.0);
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << gb << " GB";
    return ss.str();
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
        outName = "PCIe 5.0 x16";
        outPercentage = (measured / 63.02) * 100.0;
    } else {
        outName = "Unknown";
        outPercentage = 0;
    }
}

void DetectInterface(double upload, double download) {
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
        g_app.detectedInterfaceDescription = "Cutting-edge performance";
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

// ============================================================================
// GPU ENUMERATION
// ============================================================================
void EnumerateGPUs() {
    g_app.gpuList.clear();

    ComPtr<IDXGIFactory6> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        // Skip software adapters
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        GPUInfo info;
        char nameBuffer[256];
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nameBuffer, 256, nullptr, nullptr);
        info.name = nameBuffer;
        info.vendorId = desc.VendorId;
        info.deviceId = desc.DeviceId;
        info.vendor = GetVendorName(desc.VendorId);
        info.dedicatedVRAM = desc.DedicatedVideoMemory;
        info.sharedMemory = desc.SharedSystemMemory;
        info.isIntegrated = (desc.DedicatedVideoMemory == 0);

        g_app.gpuList.push_back(info);
    }
}

// ============================================================================
//                      D3D12 INITIALIZATION (Rendering)
// ============================================================================

bool InitD3D12() {
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;

    ComPtr<IDXGIAdapter1> adapter;
    factory->EnumAdapters1(0, &adapter);

    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_app.device)))) return false;

    // Command queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_app.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_app.commandQueue)))) return false;

    // Swap chain
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = Constants::NUM_FRAMES_IN_FLIGHT;
    swapChainDesc.Width = g_app.windowWidth;
    swapChainDesc.Height = g_app.windowHeight;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain1;
    if (FAILED(factory->CreateSwapChainForHwnd(g_app.commandQueue.Get(), g_app.hwnd, &swapChainDesc, nullptr, nullptr, &swapChain1))) return false;
    swapChain1.As(&g_app.swapChain);
    g_app.frameIndex = g_app.swapChain->GetCurrentBackBufferIndex();

    // RTV heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = Constants::NUM_FRAMES_IN_FLIGHT;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(g_app.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_app.rtvHeap)))) return false;
    g_app.rtvDescriptorSize = g_app.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // SRV heap (for ImGui fonts)
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 64;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_app.device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_app.srvHeap)))) return false;

    // Create RTVs and command allocators
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_app.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < Constants::NUM_FRAMES_IN_FLIGHT; i++) {
        g_app.swapChain->GetBuffer(i, IID_PPV_ARGS(&g_app.renderTargets[i]));
        g_app.device->CreateRenderTargetView(g_app.renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += g_app.rtvDescriptorSize;
        g_app.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_app.commandAllocators[i]));
    }

    // Command list
    g_app.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_app.commandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&g_app.commandList));
    g_app.commandList->Close();

    // Fence
    g_app.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_app.fence));
    g_app.currentFenceValue = 0;
    g_app.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    return true;
}

// ============================================================================
//                      D3D12 BENCHMARK DEVICE
// ============================================================================

bool InitBenchmarkDevice(int gpuIndex) {
    ComPtr<IDXGIFactory6> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));

    ComPtr<IDXGIAdapter1> adapter;
    int idx = 0;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (idx == gpuIndex) break;
        idx++;
    }

    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_app.benchDevice)))) return false;

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_app.benchDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_app.benchQueue)))) return false;
    if (FAILED(g_app.benchDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_app.benchAllocator)))) return false;
    if (FAILED(g_app.benchDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_app.benchAllocator.Get(), nullptr, IID_PPV_ARGS(&g_app.benchList)))) return false;
    g_app.benchList->Close();
    if (FAILED(g_app.benchDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_app.benchFence)))) return false;
    g_app.benchFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    g_app.benchFenceValue = 1;

    return true;
}

void CleanupBenchmarkDevice() {
    if (g_app.benchFenceEvent) { CloseHandle(g_app.benchFenceEvent); g_app.benchFenceEvent = nullptr; }
    g_app.benchFence.Reset();
    g_app.benchList.Reset();
    g_app.benchAllocator.Reset();
    g_app.benchQueue.Reset();
    g_app.benchDevice.Reset();
}

// ============================================================================
//                         BENCHMARK ENGINE
// ============================================================================

ComPtr<ID3D12Resource> CreateBuffer(D3D12_HEAP_TYPE heapType, size_t size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = heapType;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = g_app.benchDevice->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource));

    if (FAILED(hr)) {
        Log("[ERROR] CreateCommittedResource failed: HRESULT = 0x" + std::to_string(hr));
        return nullptr;
    }

    return resource;
}

void WaitForBenchFence() {
    g_app.benchQueue->Signal(g_app.benchFence.Get(), g_app.benchFenceValue);

    if (g_app.benchFence->GetCompletedValue() < g_app.benchFenceValue) {
        HRESULT hr = g_app.benchFence->SetEventOnCompletion(g_app.benchFenceValue, g_app.benchFenceEvent);
        if (FAILED(hr)) {
            Log("[ERROR] SetEventOnCompletion failed: " + std::to_string(hr));
            return;
        }

        DWORD waitResult = WaitForSingleObject(g_app.benchFenceEvent, 8000);
        if (waitResult == WAIT_TIMEOUT) {
            Log("[WARNING] Benchmark fence wait timed out after 8s - possible GPU hang");
        } else if (waitResult != WAIT_OBJECT_0) {
            Log("[ERROR] WaitForSingleObject failed: " + std::to_string(GetLastError()));
        }
    }

    g_app.benchFenceValue++;
}

BenchmarkResult RunBandwidthTest(const std::string& name, ComPtr<ID3D12Resource> src, ComPtr<ID3D12Resource> dst, size_t size, int copies, int batches) {
    g_app.currentTest = name;
    BenchmarkResult result;
    result.testName = name;
    result.unit = "GB/s";

    // Create timestamp query heap
    D3D12_QUERY_HEAP_DESC qhd = {};
    qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhd.Count = 2;
    ComPtr<ID3D12QueryHeap> queryHeap;
    if (FAILED(g_app.benchDevice->CreateQueryHeap(&qhd, IID_PPV_ARGS(&queryHeap)))) {
        Log("[ERROR] Failed to create timestamp query heap in bandwidth test");
        return result;
    }

    auto queryReadback = CreateBuffer(D3D12_HEAP_TYPE_READBACK, sizeof(UINT64) * 2, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!queryReadback) return result;

    UINT64 timestampFreq = 0;
    g_app.benchQueue->GetTimestampFrequency(&timestampFreq);
    if (timestampFreq == 0) {
        Log("[ERROR] Failed to get timestamp frequency");
        return result;
    }

    std::vector<double> bandwidths;
    bandwidths.reserve(batches);

    for (int i = 0; i < batches && !g_app.cancelRequested; ++i) {
        g_app.benchAllocator->Reset();
        g_app.benchList->Reset(g_app.benchAllocator.Get(), nullptr);

        // Record start timestamp
        g_app.benchList->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        
        // Perform copies
        for (int j = 0; j < copies; ++j) {
            g_app.benchList->CopyResource(dst.Get(), src.Get());
        }
        
        // Record end timestamp
        g_app.benchList->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);

        g_app.benchList->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, queryReadback.Get(), 0);
        g_app.benchList->Close();

        ID3D12CommandList* lists[] = { g_app.benchList.Get() };
        g_app.benchQueue->ExecuteCommandLists(1, lists);
        WaitForBenchFence();

        // Read timestamps
        UINT64* timestamps = nullptr;
        D3D12_RANGE readRange = { 0, sizeof(UINT64) * 2 };
        if (SUCCEEDED(queryReadback->Map(0, &readRange, reinterpret_cast<void**>(&timestamps)))) {
            double seconds = static_cast<double>(timestamps[1] - timestamps[0]) / static_cast<double>(timestampFreq);
            queryReadback->Unmap(0, nullptr);

            double bw = (static_cast<double>(size) * copies / (1024.0 * 1024.0 * 1024.0)) / seconds;
            bandwidths.push_back(bw);
        } else {
            Log("[ERROR] Failed to map query readback buffer in bandwidth test");
        }

        g_app.progress = static_cast<float>(i + 1) / static_cast<float>(batches);
    }

    if (!bandwidths.empty()) {
        std::sort(bandwidths.begin(), bandwidths.end());
        result.minValue = bandwidths.front();
        result.maxValue = bandwidths.back();
        result.avgValue = std::accumulate(bandwidths.begin(), bandwidths.end(), 0.0) / bandwidths.size();
        result.samples = std::move(bandwidths);
    }

    return result;
}

BenchmarkResult RunLatencyTest(const std::string& name, ComPtr<ID3D12Resource> src, ComPtr<ID3D12Resource> dst, int iterations) {
    g_app.currentTest = name;
    BenchmarkResult result;
    result.testName = name;
    result.unit = "us";

    if (iterations <= 0) return result;

    // ────────────────────────────────────────────────────────────────
    // Setup timestamp resources (create once per test)
    // ────────────────────────────────────────────────────────────────
    constexpr int QueriesPerBatch = 64;  // Adjust based on how many fit comfortably
    int batches = (iterations + QueriesPerBatch - 1) / QueriesPerBatch;

    D3D12_QUERY_HEAP_DESC qhd = {};
    qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhd.Count = QueriesPerBatch * 2;  // start + end per operation
    ComPtr<ID3D12QueryHeap> queryHeap;
    if (FAILED(g_app.benchDevice->CreateQueryHeap(&qhd, IID_PPV_ARGS(&queryHeap)))) {
        Log("[ERROR] Failed to create timestamp query heap");
        return result;
    }

    // Large enough readback buffer for all timestamps in one go
    size_t readbackSize = sizeof(UINT64) * QueriesPerBatch * 2;
    auto readbackBuffer = CreateBuffer(D3D12_HEAP_TYPE_READBACK, readbackSize, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readbackBuffer) return result;

    UINT64 timestampFreq = 0;
    g_app.benchQueue->GetTimestampFrequency(&timestampFreq);
    if (timestampFreq == 0) {
        Log("[ERROR] Failed to get timestamp frequency");
        return result;
    }

    std::vector<double> latencies;
    latencies.reserve(iterations);

    for (int b = 0; b < batches && !g_app.cancelRequested; ++b) {
        int opsThisBatch = std::min(QueriesPerBatch, iterations - (int)latencies.size());

        g_app.benchAllocator->Reset();
        g_app.benchList->Reset(g_app.benchAllocator.Get(), nullptr);

        // Start timestamps for all operations in this batch
        for (int i = 0; i < opsThisBatch; ++i) {
            UINT queryIndex = i * 2;
            g_app.benchList->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex);
            g_app.benchList->CopyResource(dst.Get(), src.Get());  // The small copy we're measuring
            g_app.benchList->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex + 1);
        }

        // Resolve all queries to readback buffer
        g_app.benchList->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, opsThisBatch * 2, readbackBuffer.Get(), 0);
        g_app.benchList->Close();

        ID3D12CommandList* lists[] = { g_app.benchList.Get() };
        g_app.benchQueue->ExecuteCommandLists(1, lists);
        WaitForBenchFence();

        // Map and extract deltas
        UINT64* timestamps = nullptr;
        D3D12_RANGE readRange = { 0, readbackSize };
        if (SUCCEEDED(readbackBuffer->Map(0, &readRange, reinterpret_cast<void**>(&timestamps)))) {
            for (int i = 0; i < opsThisBatch; ++i) {
                UINT64 tStart = timestamps[i * 2 + 0];
                UINT64 tEnd = timestamps[i * 2 + 1];
                double deltaSec = static_cast<double>(tEnd - tStart) / static_cast<double>(timestampFreq);
                double us = deltaSec * 1'000'000.0;
                latencies.push_back(us);
            }
            readbackBuffer->Unmap(0, nullptr);
        } else {
            Log("[ERROR] Failed to map readback buffer");
        }

        // Progress update (smooth over iterations)
        g_app.progress = static_cast<float>(latencies.size()) / static_cast<float>(iterations);
    }

    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        result.minValue = latencies.front();
        result.maxValue = latencies.back();
        result.avgValue = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
        result.samples = std::move(latencies);
    }

    return result;
}

BenchmarkResult RunCommandLatencyTest(int iterations) {
    g_app.currentTest = "Command Latency";
    BenchmarkResult result;
    result.testName = "Command Latency";
    result.unit = "us";

    if (iterations <= 0) return result;

    constexpr int QueriesPerBatch = 64;
    int batches = (iterations + QueriesPerBatch - 1) / QueriesPerBatch;

    D3D12_QUERY_HEAP_DESC qhd = {};
    qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhd.Count = QueriesPerBatch * 2;
    ComPtr<ID3D12QueryHeap> queryHeap;
    if (FAILED(g_app.benchDevice->CreateQueryHeap(&qhd, IID_PPV_ARGS(&queryHeap)))) {
        Log("[ERROR] Failed to create timestamp query heap for command latency");
        return result;
    }

    size_t readbackSize = sizeof(UINT64) * QueriesPerBatch * 2;
    auto readbackBuffer = CreateBuffer(D3D12_HEAP_TYPE_READBACK, readbackSize, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readbackBuffer) return result;

    UINT64 timestampFreq = 0;
    g_app.benchQueue->GetTimestampFrequency(&timestampFreq);
    if (timestampFreq == 0) {
        Log("[ERROR] Failed to get timestamp frequency");
        return result;
    }

    std::vector<double> latencies;
    latencies.reserve(iterations);

    for (int b = 0; b < batches && !g_app.cancelRequested; ++b) {
        int opsThisBatch = std::min(QueriesPerBatch, iterations - static_cast<int>(latencies.size()));

        g_app.benchAllocator->Reset();
        g_app.benchList->Reset(g_app.benchAllocator.Get(), nullptr);

        // Just measure timestamp pair - no actual work
        for (int i = 0; i < opsThisBatch; ++i) {
            UINT queryIndex = i * 2;
            g_app.benchList->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex);
            g_app.benchList->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex + 1);
        }

        g_app.benchList->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, opsThisBatch * 2, readbackBuffer.Get(), 0);
        g_app.benchList->Close();

        ID3D12CommandList* lists[] = { g_app.benchList.Get() };
        g_app.benchQueue->ExecuteCommandLists(1, lists);
        WaitForBenchFence();

        UINT64* timestamps = nullptr;
        D3D12_RANGE readRange = { 0, readbackSize };
        if (SUCCEEDED(readbackBuffer->Map(0, &readRange, reinterpret_cast<void**>(&timestamps)))) {
            for (int i = 0; i < opsThisBatch; ++i) {
                UINT64 tStart = timestamps[i * 2 + 0];
                UINT64 tEnd = timestamps[i * 2 + 1];
                double deltaSec = static_cast<double>(tEnd - tStart) / static_cast<double>(timestampFreq);
                double us = deltaSec * 1'000'000.0;
                latencies.push_back(us);
            }
            readbackBuffer->Unmap(0, nullptr);
        } else {
            Log("[ERROR] Failed to map readback buffer in command latency");
        }

        g_app.progress = static_cast<float>(latencies.size()) / static_cast<float>(iterations);
    }

    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        result.minValue = latencies.front();
        result.maxValue = latencies.back();
        result.avgValue = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
        result.samples = std::move(latencies);
    }

    return result;
}

BenchmarkResult RunBidirectionalTest(size_t size, int copies, int batches) {
    g_app.currentTest = "Bidirectional " + FormatSize(size);
    BenchmarkResult result;
    result.testName = g_app.currentTest;
    result.unit = "GB/s";

    auto cpuUpload = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto gpuDefault = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_COPY_DEST);
    auto gpuSrc = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto cpuReadback = CreateBuffer(D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);

    if (!cpuUpload || !gpuDefault || !gpuSrc || !cpuReadback) {
        Log("[ERROR] Failed to create resources for bidirectional test");
        return result;
    }

    D3D12_QUERY_HEAP_DESC qhd = {};
    qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhd.Count = 2;
    ComPtr<ID3D12QueryHeap> queryHeap;
    if (FAILED(g_app.benchDevice->CreateQueryHeap(&qhd, IID_PPV_ARGS(&queryHeap)))) {
        Log("[ERROR] Failed to create timestamp query heap in bidirectional test");
        return result;
    }

    auto queryReadback = CreateBuffer(D3D12_HEAP_TYPE_READBACK, sizeof(UINT64) * 2, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!queryReadback) return result;

    UINT64 timestampFreq = 0;
    g_app.benchQueue->GetTimestampFrequency(&timestampFreq);
    if (timestampFreq == 0) {
        Log("[ERROR] Failed to get timestamp frequency");
        return result;
    }

    std::vector<double> bandwidths;
    bandwidths.reserve(batches);

    for (int i = 0; i < batches && !g_app.cancelRequested; ++i) {
        g_app.benchAllocator->Reset();
        g_app.benchList->Reset(g_app.benchAllocator.Get(), nullptr);

        g_app.benchList->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        for (int j = 0; j < copies; ++j) {
            g_app.benchList->CopyResource(gpuDefault.Get(), cpuUpload.Get());
            g_app.benchList->CopyResource(cpuReadback.Get(), gpuSrc.Get());
        }
        g_app.benchList->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);

        g_app.benchList->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, queryReadback.Get(), 0);
        g_app.benchList->Close();

        ID3D12CommandList* lists[] = { g_app.benchList.Get() };
        g_app.benchQueue->ExecuteCommandLists(1, lists);
        WaitForBenchFence();

        UINT64* timestamps = nullptr;
        D3D12_RANGE readRange = { 0, sizeof(UINT64) * 2 };
        if (SUCCEEDED(queryReadback->Map(0, &readRange, reinterpret_cast<void**>(&timestamps)))) {
            double seconds = static_cast<double>(timestamps[1] - timestamps[0]) / static_cast<double>(timestampFreq);
            queryReadback->Unmap(0, nullptr);

            double bw = (static_cast<double>(size) * copies * 2 / (1024.0 * 1024.0 * 1024.0)) / seconds;
            bandwidths.push_back(bw);
        } else {
            Log("[ERROR] Failed to map query readback buffer in bidirectional test");
        }

        g_app.progress = static_cast<float>(i + 1) / static_cast<float>(batches);
    }

    if (!bandwidths.empty()) {
        std::sort(bandwidths.begin(), bandwidths.end());
        result.minValue = bandwidths.front();
        result.maxValue = bandwidths.back();
        result.avgValue = std::accumulate(bandwidths.begin(), bandwidths.end(), 0.0) / bandwidths.size();
        result.samples = std::move(bandwidths);
    }

    return result;
}

void BenchmarkThreadFunc() {
    Log("=== Benchmark Started ===");
    Log("GPU: " + g_app.gpuList[g_app.config.selectedGPU].name);
    Log("Size: " + FormatSize(g_app.config.bandwidthSize));
    Log("Batches: " + std::to_string(g_app.config.bandwidthBatches));
    Log("Copies/Batch: " + std::to_string(g_app.config.copiesPerBatch));
    Log("Runs: " + std::to_string(g_app.config.numRuns));
    Log("=========================");

    if (!InitBenchmarkDevice(g_app.config.selectedGPU)) {
        Log("[ERROR] Failed to initialize benchmark device!");
        g_app.state = AppState::Idle;
        return;
    }

    std::vector<BenchmarkResult> allResults;

    // Calculate total tests
    int testsPerRun = 2;  // Upload + Download
    if (g_app.config.runBidirectional) testsPerRun++;
    if (g_app.config.runLatency) testsPerRun += 3;  // Upload latency, Download latency, Command latency
    g_app.totalTests = testsPerRun * g_app.config.numRuns;

    double avgUpload = 0, avgDownload = 0;

    for (int run = 1; run <= g_app.config.numRuns && !g_app.cancelRequested; run++) {
        g_app.currentRun = run;
        Log("--- Run " + std::to_string(run) + " / " + std::to_string(g_app.config.numRuns) + " ---");

        // Upload (CPU to GPU)
        auto cpuUpload = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, g_app.config.bandwidthSize, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!cpuUpload) {
            Log("[CRITICAL] Failed to allocate upload buffer - skipping upload test");
            continue;
        }
        auto gpuDefault = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, g_app.config.bandwidthSize, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!gpuDefault) {
            Log("[CRITICAL] Failed to allocate GPU default buffer - skipping upload test");
            continue;
        }
        auto resUpload = RunBandwidthTest("CPU->GPU " + FormatSize(g_app.config.bandwidthSize),
            cpuUpload, gpuDefault,
            g_app.config.bandwidthSize,
            g_app.config.copiesPerBatch,
            g_app.config.bandwidthBatches);
        allResults.push_back(resUpload);
        avgUpload += resUpload.avgValue;
        Log("  CPU->GPU: " + std::to_string(resUpload.avgValue).substr(0, 5) + " GB/s");
        g_app.completedTests++;
        g_app.overallProgress = float(g_app.completedTests) / float(g_app.totalTests);
        if (g_app.cancelRequested) break;

        // Download (GPU to CPU)
        auto gpuSrc = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, g_app.config.bandwidthSize, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (!gpuSrc) {
            Log("[CRITICAL] Failed to allocate GPU source buffer - skipping download test");
            continue;
        }
        auto cpuReadback = CreateBuffer(D3D12_HEAP_TYPE_READBACK, g_app.config.bandwidthSize, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!cpuReadback) {
            Log("[CRITICAL] Failed to allocate readback buffer - skipping download test");
            continue;
        }
        auto resDownload = RunBandwidthTest("GPU->CPU " + FormatSize(g_app.config.bandwidthSize),
            gpuSrc, cpuReadback,
            g_app.config.bandwidthSize,
            g_app.config.copiesPerBatch,
            g_app.config.bandwidthBatches);
        allResults.push_back(resDownload);
        avgDownload += resDownload.avgValue;
        Log("  GPU->CPU: " + std::to_string(resDownload.avgValue).substr(0, 5) + " GB/s");
        g_app.completedTests++;
        g_app.overallProgress = float(g_app.completedTests) / float(g_app.totalTests);
        if (g_app.cancelRequested) break;

        // Bidirectional
        if (g_app.config.runBidirectional) {
            auto resBidir = RunBidirectionalTest(g_app.config.bandwidthSize, g_app.config.copiesPerBatch, g_app.config.bandwidthBatches);
            allResults.push_back(resBidir);
            Log("  Bidirectional: " + std::to_string(resBidir.avgValue).substr(0, 5) + " GB/s");
            g_app.completedTests++;
            g_app.overallProgress = float(g_app.completedTests) / float(g_app.totalTests);
            if (g_app.cancelRequested) break;
        }

        // Latency tests
        if (g_app.config.runLatency) {
            auto latCpuUpload = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, g_app.config.latencySize, D3D12_RESOURCE_STATE_GENERIC_READ);
            auto latGpuDefault = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, g_app.config.latencySize, D3D12_RESOURCE_STATE_COPY_DEST);
            auto latGpuSrc = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, g_app.config.latencySize, D3D12_RESOURCE_STATE_COPY_SOURCE);
            auto latCpuReadback = CreateBuffer(D3D12_HEAP_TYPE_READBACK, g_app.config.latencySize, D3D12_RESOURCE_STATE_COPY_DEST);

            if (!latCpuUpload || !latGpuDefault || !latGpuSrc || !latCpuReadback) {
                Log("[CRITICAL] Failed to allocate latency buffers - skipping latency tests");
                continue;
            }

            // Warm-up passes
            Log("Running latency warm-up...");
            RunLatencyTest("Warm-up Upload", latCpuUpload, latGpuDefault, Constants::LATENCY_WARMUP_ITERATIONS);
            RunLatencyTest("Warm-up Download", latGpuSrc, latCpuReadback, Constants::LATENCY_WARMUP_ITERATIONS);
            RunCommandLatencyTest(Constants::LATENCY_WARMUP_ITERATIONS);

            // Real measurements
            auto resUpLat = RunLatencyTest("CPU->GPU Latency", latCpuUpload, latGpuDefault, g_app.config.latencyIters);
            allResults.push_back(resUpLat);
            Log("  CPU->GPU Latency: " + std::to_string(resUpLat.avgValue).substr(0, 6) + " us");
            g_app.completedTests++;
            g_app.overallProgress = float(g_app.completedTests) / float(g_app.totalTests);
            if (g_app.cancelRequested) break;

            auto resDownLat = RunLatencyTest("GPU->CPU Latency", latGpuSrc, latCpuReadback, g_app.config.latencyIters);
            allResults.push_back(resDownLat);
            Log("  GPU->CPU Latency: " + std::to_string(resDownLat.avgValue).substr(0, 6) + " us");
            g_app.completedTests++;
            g_app.overallProgress = float(g_app.completedTests) / float(g_app.totalTests);
            if (g_app.cancelRequested) break;

            auto resCmdLat = RunCommandLatencyTest(g_app.config.latencyIters);
            allResults.push_back(resCmdLat);
            Log("  Command Latency: " + std::to_string(resCmdLat.avgValue).substr(0, 6) + " us");
            g_app.completedTests++;
            g_app.overallProgress = float(g_app.completedTests) / float(g_app.totalTests);
            if (g_app.cancelRequested) break;
        }
    }

    CleanupBenchmarkDevice();

    if (g_app.cancelRequested) {
        Log("Benchmark cancelled");
        g_app.state = AppState::Idle;
    } else {
        avgUpload /= g_app.config.numRuns;
        avgDownload /= g_app.config.numRuns;
        DetectInterface(avgUpload, avgDownload);

        Log("=== Benchmark Complete ===");
        Log("Detected Interface: " + g_app.detectedInterface);

        // Log with percentages
        char uploadBuf[128], downloadBuf[128];
        snprintf(uploadBuf, sizeof(uploadBuf), "CPU->GPU: %.2f GB/s (%.0f%% of %s)",
            avgUpload, g_app.uploadPercentage, g_app.closestUploadStandard.c_str());
        snprintf(downloadBuf, sizeof(downloadBuf), "GPU->CPU: %.2f GB/s (%.0f%% of %s)",
            avgDownload, g_app.downloadPercentage, g_app.closestDownloadStandard.c_str());
        Log(uploadBuf);
        Log(downloadBuf);

        std::lock_guard<std::mutex> lock(g_app.resultsMutex);
        g_app.results = allResults;
        g_app.state = AppState::Completed;
    }
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
    file << "\nDetected Interface," << g_app.detectedInterface << "\n";
    file << "CPU->GPU," << g_app.uploadBW << " GB/s," << g_app.uploadPercentage << "% of " << g_app.closestUploadStandard << "\n";
    file << "GPU->CPU," << g_app.downloadBW << " GB/s," << g_app.downloadPercentage << "% of " << g_app.closestDownloadStandard << "\n";

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

    ImGui::Text("GPU-PCIe-Test v2.0 GUI");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Select GPU:");
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##GPU", &g_app.config.selectedGPU, g_app.gpuComboPointers.data(), (int)g_app.gpuComboPointers.size());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Bandwidth Test Size");
    ImGui::SetNextItemWidth(-1);

    // Bandwidth in MB (safe int32 range: 16–1024 MB)
    int bandwidth_mb = static_cast<int>(g_app.config.bandwidthSize / (1024 * 1024));
    if (ImGui::SliderInt("##BandwidthSize", &bandwidth_mb, 16, 1024, "%d MB", ImGuiSliderFlags_Logarithmic)) {
        g_app.config.bandwidthSize = static_cast<size_t>(bandwidth_mb) * 1024 * 1024;
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

    bool canStart = (g_app.state == AppState::Idle);
    bool canStop = (g_app.state == AppState::Running);
    bool hasResults = (g_app.state == AppState::Completed);

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
        g_app.showResultsWindow = false;
        g_app.showGraphsWindow = false;
        g_app.showCompareWindow = false;
        g_app.results.clear();
        ClearLog();
        g_app.benchmarkThread = std::thread(BenchmarkThreadFunc);
        g_app.benchmarkThread.detach();
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

    // Results buttons - only available when complete
    if (!hasResults) ImGui::BeginDisabled();
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
    if (!hasResults) ImGui::EndDisabled();

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
            else if (line.find("GB/s") != std::string::npos || line.find(" us") != std::string::npos) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", line.c_str());
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
        ImGui::ProgressBar(g_app.progress, ImVec2(-1, 16), "Test progress");

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

    // ========== Results Window (Popup, only when requested) ==========
    if (g_app.showResultsWindow && g_app.state == AppState::Completed) {
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
        ImGui::Text("Detected Interface:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "%s", g_app.detectedInterface.c_str());

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
    if (g_app.showGraphsWindow && g_app.state == AppState::Completed) {
        ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
        ImGui::Begin("Graphs", &g_app.showGraphsWindow);

        std::lock_guard<std::mutex> lock(g_app.resultsMutex);

        // Bandwidth bar chart
        std::vector<double> bandwidths;
        std::vector<const char*> labels;
        for (const auto& r : g_app.results) {
            if (r.unit == "GB/s") {
                bandwidths.push_back(r.avgValue);
                labels.push_back(r.testName.c_str());
            }
        }

        if (!bandwidths.empty() && ImPlot::BeginPlot("Bandwidth (GB/s)", ImVec2(-1, 250))) {
            ImPlot::SetupAxes("Test", "GB/s");
            ImPlot::SetupAxisTicks(ImAxis_X1, 0, (double)bandwidths.size() - 1, (int)bandwidths.size(), labels.data());
            ImPlot::PlotBars("Bandwidth", bandwidths.data(), (int)bandwidths.size(), 0.67);
            ImPlot::EndPlot();
        }

        // Latency bar chart
        std::vector<double> latencies;
        std::vector<const char*> latLabels;
        for (const auto& r : g_app.results) {
            if (r.unit == "us") {
                latencies.push_back(r.avgValue);
                latLabels.push_back(r.testName.c_str());
            }
        }

        if (!latencies.empty() && ImPlot::BeginPlot("Latency (us)", ImVec2(-1, 250))) {
            ImPlot::SetupAxes("Test", "us");
            ImPlot::SetupAxisTicks(ImAxis_X1, 0, (double)latencies.size() - 1, (int)latencies.size(), latLabels.data());
            ImPlot::PlotBars("Latency", latencies.data(), (int)latencies.size(), 0.67);
            ImPlot::EndPlot();
        }

        ImGui::End();
    }

    // ========== Compare to Standards Window (NEW) ==========
    if (g_app.showCompareWindow && g_app.state == AppState::Completed) {
        ImGui::SetNextWindowSize(ImVec2(1100, 850), ImGuiCond_FirstUseEver);
        ImGui::Begin("Compare to Standards", &g_app.showCompareWindow);

        if (ImGui::BeginTabBar("CompareTabs")) {

            // ===== CPU->GPU Tab =====
            if (ImGui::BeginTabItem("CPU->GPU vs Standards")) {
                // Build sorted list with your result inserted in correct position
                struct BandwidthEntry {
                    std::string name;
                    double bandwidth;
                    bool isUserResult;
                };
                std::vector<BandwidthEntry> entries;

                // Add all standards
                for (int i = 0; i < NUM_INTERFACE_SPEEDS; i++) {
                    entries.push_back({ INTERFACE_SPEEDS[i].name, INTERFACE_SPEEDS[i].bandwidth, false });
                }
                // Add user result
                entries.push_back({ ">> YOUR RESULT <<", g_app.uploadBW, true });

                // Sort by bandwidth (slowest to fastest)
                std::sort(entries.begin(), entries.end(), [](const BandwidthEntry& a, const BandwidthEntry& b) {
                    return a.bandwidth < b.bandwidth;
                });

                // Build arrays for plotting
                std::vector<double> positions;
                std::vector<double> standardBW, userBW;
                std::vector<double> standardPos, userPos;
                static std::vector<std::string> sortedNamesUp;  // Static to persist for ImPlot
                sortedNamesUp.clear();

                for (size_t i = 0; i < entries.size(); i++) {
                    sortedNamesUp.push_back(entries[i].name);
                    if (entries[i].isUserResult) {
                        userPos.push_back((double)i);
                        userBW.push_back(entries[i].bandwidth);
                    } else {
                        standardPos.push_back((double)i);
                        standardBW.push_back(entries[i].bandwidth);
                    }
                }

                // Create label pointers
                static std::vector<const char*> labelPtrsUp;
                labelPtrsUp.clear();
                for (const auto& s : sortedNamesUp) labelPtrsUp.push_back(s.c_str());

                // Plot with extra bottom margin for angled labels
                if (ImPlot::BeginPlot("CPU->GPU Bandwidth vs Interface Standards", ImVec2(-1, 380))) {
                    ImPlot::SetupAxes("", "GB/s", ImPlotAxisFlags_NoTickLabels, 0);
                    ImPlot::SetupAxisLimits(ImAxis_X1, -0.5, (double)entries.size() - 0.5);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 70);

                    // Plot standards in gray
                    if (!standardBW.empty()) {
                        ImPlot::SetNextFillStyle(ImVec4(0.5f, 0.5f, 0.5f, 0.7f));
                        ImPlot::PlotBars("Standards", standardPos.data(), standardBW.data(), (int)standardBW.size(), 0.6);
                    }

                    // Plot user result in green
                    if (!userBW.empty()) {
                        ImPlot::SetNextFillStyle(ImVec4(0.2f, 0.9f, 0.2f, 1.0f));
                        ImPlot::PlotBars("Your Result", userPos.data(), userBW.data(), (int)userBW.size(), 0.6);
                    }

                    // Draw vertical labels below each bar
                    ImDrawList* drawList = ImPlot::GetPlotDrawList();
                    float fontSize = ImGui::GetFontSize() * 0.55f;
                    ImFont* font = ImGui::GetFont();

                    for (size_t i = 0; i < entries.size(); i++) {
                        ImPlotPoint plotPt((double)i, 0);
                        ImVec2 screenPt = ImPlot::PlotToPixels(plotPt);
                        screenPt.y += 5;  // Below axis

                        ImU32 textColor = entries[i].isUserResult ? 
                            IM_COL32(50, 230, 50, 255) : IM_COL32(180, 180, 180, 255);

                        // Draw text vertically (one character per line)
                        const char* text = entries[i].name.c_str();
                        float lineHeight = fontSize * 0.85f;
                        float xOffset = -fontSize * 0.25f;  // Center horizontally
                        int charIdx = 0;

                        while (*text && charIdx < 14) {  // Limit to 14 chars
                            char ch[2] = {*text, 0};
                            ImVec2 charPos(screenPt.x + xOffset, screenPt.y + charIdx * lineHeight);
                            drawList->AddText(font, fontSize, charPos, textColor, ch);
                            text++;
                            charIdx++;
                        }
                    }

                    ImPlot::EndPlot();
                }

                // Reference list (in case angled labels are hard to read)
                ImGui::Spacing();
                ImGui::Separator();

                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Reference (slowest to fastest):");
                ImGui::Indent();
                for (size_t i = 0; i < entries.size(); i++) {
                    if (entries[i].isUserResult) {
                        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "%zu. %s (%.2f GB/s)", i + 1, entries[i].name.c_str(), entries[i].bandwidth);
                    } else {
                        ImGui::Text("%zu. %s (%.2f GB/s)", i + 1, entries[i].name.c_str(), entries[i].bandwidth);
                    }
                }
                ImGui::Unindent();

                // Summary text
                ImGui::Spacing();
                char summary[128];
                snprintf(summary, sizeof(summary), "Your CPU->GPU bandwidth: %.2f GB/s = %.0f%% of %s",
                    g_app.uploadBW, g_app.uploadPercentage, g_app.closestUploadStandard.c_str());
                ImVec4 color = g_app.uploadPercentage >= 90 ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) :
                    g_app.uploadPercentage >= 70 ? ImVec4(1.0f, 1.0f, 0.4f, 1.0f) :
                    ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                ImGui::TextColored(color, "%s", summary);

                ImGui::EndTabItem();
            }

            // ===== GPU->CPU Tab =====
            if (ImGui::BeginTabItem("GPU->CPU vs Standards")) {
                // Build sorted list with your result inserted in correct position
                struct BandwidthEntry {
                    std::string name;
                    double bandwidth;
                    bool isUserResult;
                };
                std::vector<BandwidthEntry> entries;

                // Add all standards
                for (int i = 0; i < NUM_INTERFACE_SPEEDS; i++) {
                    entries.push_back({ INTERFACE_SPEEDS[i].name, INTERFACE_SPEEDS[i].bandwidth, false });
                }
                // Add user result
                entries.push_back({ ">> YOUR RESULT <<", g_app.downloadBW, true });

                // Sort by bandwidth (slowest to fastest)
                std::sort(entries.begin(), entries.end(), [](const BandwidthEntry& a, const BandwidthEntry& b) {
                    return a.bandwidth < b.bandwidth;
                });

                // Build arrays for plotting
                std::vector<double> standardBW, userBW;
                std::vector<double> standardPos, userPos;
                static std::vector<std::string> sortedNamesDn;
                sortedNamesDn.clear();

                for (size_t i = 0; i < entries.size(); i++) {
                    sortedNamesDn.push_back(entries[i].name);
                    if (entries[i].isUserResult) {
                        userPos.push_back((double)i);
                        userBW.push_back(entries[i].bandwidth);
                    } else {
                        standardPos.push_back((double)i);
                        standardBW.push_back(entries[i].bandwidth);
                    }
                }

                // Plot with extra bottom margin for angled labels
                if (ImPlot::BeginPlot("GPU->CPU Bandwidth vs Interface Standards", ImVec2(-1, 380))) {
                    ImPlot::SetupAxes("", "GB/s", ImPlotAxisFlags_NoTickLabels, 0);
                    ImPlot::SetupAxisLimits(ImAxis_X1, -0.5, (double)entries.size() - 0.5);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 70);

                    // Plot standards in gray
                    if (!standardBW.empty()) {
                        ImPlot::SetNextFillStyle(ImVec4(0.5f, 0.5f, 0.5f, 0.7f));
                        ImPlot::PlotBars("Standards", standardPos.data(), standardBW.data(), (int)standardBW.size(), 0.6);
                    }

                    // Plot user result in cyan
                    if (!userBW.empty()) {
                        ImPlot::SetNextFillStyle(ImVec4(0.2f, 0.7f, 0.9f, 1.0f));
                        ImPlot::PlotBars("Your Result", userPos.data(), userBW.data(), (int)userBW.size(), 0.6);
                    }

                    // Draw vertical labels below each bar
                    ImDrawList* drawList = ImPlot::GetPlotDrawList();
                    float fontSize = ImGui::GetFontSize() * 0.55f;
                    ImFont* font = ImGui::GetFont();

                    for (size_t i = 0; i < entries.size(); i++) {
                        ImPlotPoint plotPt((double)i, 0);
                        ImVec2 screenPt = ImPlot::PlotToPixels(plotPt);
                        screenPt.y += 5;

                        ImU32 textColor = entries[i].isUserResult ? 
                            IM_COL32(50, 180, 230, 255) : IM_COL32(180, 180, 180, 255);

                        // Draw text vertically (one character per line)
                        const char* text = entries[i].name.c_str();
                        float lineHeight = fontSize * 0.85f;
                        float xOffset = -fontSize * 0.25f;
                        int charIdx = 0;

                        while (*text && charIdx < 14) {
                            char ch[2] = {*text, 0};
                            ImVec2 charPos(screenPt.x + xOffset, screenPt.y + charIdx * lineHeight);
                            drawList->AddText(font, fontSize, charPos, textColor, ch);
                            text++;
                            charIdx++;
                        }
                    }

                    ImPlot::EndPlot();
                }

                // Reference list
                ImGui::Spacing();
                ImGui::Separator();

                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Reference (slowest to fastest):");
                ImGui::Indent();
                for (size_t i = 0; i < entries.size(); i++) {
                    if (entries[i].isUserResult) {
                        ImGui::TextColored(ImVec4(0.2f, 0.7f, 0.9f, 1.0f), "%zu. %s (%.2f GB/s)", i + 1, entries[i].name.c_str(), entries[i].bandwidth);
                    } else {
                        ImGui::Text("%zu. %s (%.2f GB/s)", i + 1, entries[i].name.c_str(), entries[i].bandwidth);
                    }
                }
                ImGui::Unindent();

                // Summary text
                ImGui::Spacing();
                char summary[128];
                snprintf(summary, sizeof(summary), "Your GPU->CPU bandwidth: %.2f GB/s = %.0f%% of %s",
                    g_app.downloadBW, g_app.downloadPercentage, g_app.closestDownloadStandard.c_str());
                ImVec4 color = g_app.downloadPercentage >= 90 ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) :
                    g_app.downloadPercentage >= 70 ? ImVec4(1.0f, 1.0f, 0.4f, 1.0f) :
                    ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                ImGui::TextColored(color, "%s", summary);

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    // ========== About Dialog ==========
    if (g_app.showAboutDialog) {
        ImGui::OpenPopup("About GPU-PCIe-Test");
        g_app.showAboutDialog = false;  // Reset so it doesn't keep opening
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_Appearing);  // Larger for scaled fonts

    if (ImGui::BeginPopupModal("About GPU-PCIe-Test", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "GPU-PCIe-Test v2.0 GUI");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("A GPU and PCIe bandwidth/latency benchmark tool.");
        ImGui::Text("Measures CPU<->GPU transfer speeds to help identify");
        ImGui::Text("PCIe lane configuration and potential bottlenecks.");

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Author:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "David Janice");

        ImGui::Text("Email:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "djanice1980@gmail.com");

        ImGui::Text("GitHub:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 1.0f, 1.0f), "https://github.com/djanice1980/GPU-PCIe-Test");

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Built with Dear ImGui, ImPlot, and Direct3D 12");

        ImGui::Spacing();
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
    // Flush the queue by signaling a new value and waiting for it
    g_app.currentFenceValue++;
    g_app.commandQueue->Signal(g_app.fence.Get(), g_app.currentFenceValue);

    if (g_app.fence->GetCompletedValue() < g_app.currentFenceValue) {
        g_app.fence->SetEventOnCompletion(g_app.currentFenceValue, g_app.fenceEvent);
        WaitForSingleObject(g_app.fenceEvent, INFINITE);
    }
}

void Render() {
    UINT frameIdx = g_app.frameIndex;

    // Wait for previous frame on this backbuffer
    if (g_app.fence->GetCompletedValue() < g_app.fenceValues[frameIdx]) {
        g_app.fence->SetEventOnCompletion(g_app.fenceValues[frameIdx], g_app.fenceEvent);
        WaitForSingleObject(g_app.fenceEvent, INFINITE);
    }

    // Reset allocator and command list
    g_app.commandAllocators[frameIdx]->Reset();
    g_app.commandList->Reset(g_app.commandAllocators[frameIdx].Get(), nullptr);

    // Transition to render target
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_app.renderTargets[frameIdx].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_app.commandList->ResourceBarrier(1, &barrier);

    // Set viewport and scissor rect to match window size
    D3D12_VIEWPORT viewport = {};
    viewport.Width = (float)g_app.windowWidth;
    viewport.Height = (float)g_app.windowHeight;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    g_app.commandList->RSSetViewports(1, &viewport);

    D3D12_RECT scissor = {};
    scissor.right = g_app.windowWidth;
    scissor.bottom = g_app.windowHeight;
    g_app.commandList->RSSetScissorRects(1, &scissor);

    // Clear
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_app.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += frameIdx * g_app.rtvDescriptorSize;
    float clearColor[] = { 0.1f, 0.1f, 0.12f, 1.0f };
    g_app.commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    g_app.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Render ImGui
    ID3D12DescriptorHeap* heaps[] = { g_app.srvHeap.Get() };
    g_app.commandList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_app.commandList.Get());

    // Transition to present
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_app.commandList->ResourceBarrier(1, &barrier);

    g_app.commandList->Close();

    // Execute
    ID3D12CommandList* lists[] = { g_app.commandList.Get() };
    g_app.commandQueue->ExecuteCommandLists(1, lists);

    // Present
    g_app.swapChain->Present(1, 0);

    // Signal fence with new value
    g_app.currentFenceValue++;
    g_app.commandQueue->Signal(g_app.fence.Get(), g_app.currentFenceValue);
    g_app.fenceValues[frameIdx] = g_app.currentFenceValue;

    g_app.frameIndex = g_app.swapChain->GetCurrentBackBufferIndex();
}

void ResizeSwapChain(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (!g_app.swapChain) return;

    WaitForGPU();

    // Release render targets
    for (UINT i = 0; i < Constants::NUM_FRAMES_IN_FLIGHT; i++) {
        g_app.renderTargets[i].Reset();
    }

    // Resize buffers with error checking
    HRESULT hr = g_app.swapChain->ResizeBuffers(
        Constants::NUM_FRAMES_IN_FLIGHT,
        width, height,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        0
    );

    if (FAILED(hr)) {
        Log("[ERROR] ResizeBuffers failed: HRESULT 0x" + std::to_string(static_cast<unsigned>(hr)));
        return;
    }

    g_app.frameIndex = g_app.swapChain->GetCurrentBackBufferIndex();

    // Recreate RTVs
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_app.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < Constants::NUM_FRAMES_IN_FLIGHT; i++) {
        g_app.swapChain->GetBuffer(i, IID_PPV_ARGS(&g_app.renderTargets[i]));
        g_app.device->CreateRenderTargetView(g_app.renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += g_app.rtvDescriptorSize;
        g_app.fenceValues[i] = 0;
    }

    g_app.windowWidth = width;
    g_app.windowHeight = height;
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
        0, L"GPUPCIeTestGUI", L"GPU-PCIe-Test v2.0 GUI",
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

    // Initialize D3D12
    if (!InitD3D12()) {
        MessageBoxA(nullptr, "Failed to initialize D3D12", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Enumerate GPUs
    EnumerateGPUs();

    // Prepare GPU combo (build once, not per frame)
    g_app.gpuComboNames.clear();
    g_app.gpuComboPointers.clear();
    for (const auto& gpu : g_app.gpuList) {
        std::string label = gpu.vendor + " " + gpu.name +
            " (" + FormatMemory(gpu.dedicatedVRAM) +
            (gpu.isIntegrated ? " iGPU" : "") + ")";
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

    // Get DPI scale and apply font scaling
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

    // Initialize DX12 backend
    ImGui_ImplDX12_InitInfo init_info = {};
    init_info.Device = g_app.device.Get();
    init_info.CommandQueue = g_app.commandQueue.Get();
    init_info.NumFramesInFlight = Constants::NUM_FRAMES_IN_FLIGHT;
    init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    init_info.SrvDescriptorHeap = g_app.srvHeap.Get();
    init_info.LegacySingleSrvCpuDescriptor = g_app.srvHeap->GetCPUDescriptorHandleForHeapStart();
    init_info.LegacySingleSrvGpuDescriptor = g_app.srvHeap->GetGPUDescriptorHandleForHeapStart();
    ImGui_ImplDX12_Init(&init_info);

    // Main loop
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        // Handle deferred resize (only after drag ends)
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
        ImGui_ImplDX12_NewFrame();
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

    // Cleanup
    WaitForGPU();

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    if (g_app.fenceEvent) CloseHandle(g_app.fenceEvent);

    DestroyWindow(g_app.hwnd);
    UnregisterClassW(L"GPUPCIeTestGUI", hInstance);

    return 0;
}

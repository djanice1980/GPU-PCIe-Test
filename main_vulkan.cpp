// ============================================================================
// GPU-PCIe-Test v2.0 - Vulkan Benchmark
// ============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <vulkan/vulkan.h>
#include <conio.h>

#pragma comment(lib, "vulkan-1.lib")

#include "bandwidth_analysis.h"

#define VK_CHECK(x) do { VkResult r = (x); if (r != VK_SUCCESS) { std::cerr << "Vulkan error: " << r << "\n"; exit(1); } } while(0)

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
    std::cout << "GPU-PCIe-Test v2.0 - Vulkan Benchmark\n";
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

struct VulkanContext {
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkQueue queue;
    VkCommandPool commandPool;
    uint32_t queueFamilyIndex;
    float timestampPeriod;
    GPUInfo gpuInfo;

    void Init(int gpuIndex = 0) {
        VkApplicationInfo appInfo{}; appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "GPU-PCIe-Test"; appInfo.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ici{}; ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ici.pApplicationInfo = &appInfo;
        VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));
        SelectPhysicalDevice(gpuIndex);
        uint32_t qfCount = 0; vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfProps(qfCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qfCount, qfProps.data());
        queueFamilyIndex = UINT32_MAX;
        for (uint32_t i = 0; i < qfCount; i++) if (qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { queueFamilyIndex = i; break; }
        if (queueFamilyIndex == UINT32_MAX) for (uint32_t i = 0; i < qfCount; i++) if (qfProps[i].queueFlags & VK_QUEUE_TRANSFER_BIT) { queueFamilyIndex = i; break; }
        float priority = 1.0f;
        VkDeviceQueueCreateInfo dqci{}; dqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        dqci.queueFamilyIndex = queueFamilyIndex; dqci.queueCount = 1; dqci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{}; dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &dqci;
        VK_CHECK(vkCreateDevice(physicalDevice, &dci, nullptr, &device));
        vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);
        VkCommandPoolCreateInfo cpci{}; cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.queueFamilyIndex = queueFamilyIndex; cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(device, &cpci, nullptr, &commandPool));
    }

    void SelectPhysicalDevice(int gpuIndex = 0) {
        uint32_t count = 0; vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (count == 0) { std::cerr << "No Vulkan GPU found!\n"; exit(1); }
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());
        
        if (gpuIndex > 0 && gpuIndex <= (int)count) {
            physicalDevice = devices[gpuIndex-1]; GetGPUInfo();
            std::cout << "Using GPU #" << gpuIndex << ": " << gpuInfo.name << "\n";
            return;
        }
        
        if (count == 1) {
            physicalDevice = devices[0]; GetGPUInfo();
            std::cout << "Found 1 GPU: " << gpuInfo.name << "\n"; PrintDoubleDivider();
            return;
        }
        std::cout << "Found " << count << " Vulkan GPUs:\n"; PrintDoubleDivider();
        for (size_t i = 0; i < devices.size(); i++) {
            VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(devices[i], &props);
            std::cout << (i+1) << ". " << props.deviceName << "\n";
        }
        std::cout << "Select GPU (1-" << count << ", default=1): ";
        std::string input; std::getline(std::cin, input);
        int choice = 1; if (!input.empty()) try { choice = std::stoi(input); } catch (...) {}
        choice = std::max(1, std::min(choice, (int)count));
        physicalDevice = devices[choice-1]; GetGPUInfo();
        std::cout << "\nSelected: " << gpuInfo.name << "\n"; PrintDoubleDivider();
    }

    void GetGPUInfo() {
        VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(physicalDevice, &props);
        gpuInfo.name = props.deviceName; gpuInfo.vendorId = props.vendorID;
        gpuInfo.deviceId = props.deviceID; gpuInfo.vendor = GetVendorName(props.vendorID);
        timestampPeriod = props.limits.timestampPeriod;
        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
        for (uint32_t i = 0; i < memProps.memoryHeapCount; i++) {
            if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                gpuInfo.dedicatedVideoMemory += memProps.memoryHeaps[i].size;
            else gpuInfo.sharedSystemMemory += memProps.memoryHeaps[i].size;
        }
    }

    void PrintGPUInfo() {
        PrintDoubleDivider();
        std::cout << "GPU Information (Vulkan)\n"; PrintDoubleDivider();
        std::cout << "  Name:   " << gpuInfo.name << "\n";
        std::cout << "  Vendor: " << gpuInfo.vendor << " (0x" << std::hex << gpuInfo.vendorId << std::dec << ")\n";
        std::cout << "  VRAM:   " << FormatMemorySize(gpuInfo.dedicatedVideoMemory) << "\n";
        if (gpuInfo.dedicatedVideoMemory == 0) std::cout << "  [i] Integrated GPU (APU)\n";
        PrintDoubleDivider();
    }

    void Cleanup() {
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
    }
};

struct Buffer {
    VkBuffer buffer; VkDeviceMemory memory; VkDeviceSize size;
    void Create(const VulkanContext& ctx, VkDeviceSize bufferSize, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps) {
        size = bufferSize;
        VkBufferCreateInfo bci{}; bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bufferSize; bci.usage = usage; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(ctx.device, &bci, nullptr, &buffer));
        VkMemoryRequirements memReq; vkGetBufferMemoryRequirements(ctx.device, buffer, &memReq);
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(ctx.physicalDevice, &mp);
        uint32_t memTypeIndex = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((memReq.memoryTypeBits & (1<<i)) && (mp.memoryTypes[i].propertyFlags & memProps) == memProps) { memTypeIndex = i; break; }
        VkMemoryAllocateInfo mai{}; mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReq.size; mai.memoryTypeIndex = memTypeIndex;
        VK_CHECK(vkAllocateMemory(ctx.device, &mai, nullptr, &memory));
        VK_CHECK(vkBindBufferMemory(ctx.device, buffer, memory, 0));
    }
    void Destroy(const VulkanContext& ctx) {
        vkDestroyBuffer(ctx.device, buffer, nullptr);
        vkFreeMemory(ctx.device, memory, nullptr);
    }
};

VkCommandBuffer BeginCommandBuffer(const VulkanContext& ctx) {
    VkCommandBufferAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = ctx.commandPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cb; VK_CHECK(vkAllocateCommandBuffers(ctx.device, &ai, &cb));
    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

void EndCommandBuffer(const VulkanContext& ctx, VkCommandBuffer cb) {
    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    VK_CHECK(vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.queue));
    vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &cb);
}

// ============================================================================
//                          BENCHMARK TESTS
// ============================================================================

TestResults RunBandwidthTest(const char* name, VulkanContext& ctx, Buffer& src, Buffer& dst, size_t size, int copiesPerBatch, int batches, bool checkEscape = false) {
    std::cout << name << "\n"; PrintDivider();
    VkCommandBuffer cb = BeginCommandBuffer(ctx);
    VkQueryPool queryPool; VkQueryPoolCreateInfo qpci{}; qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP; qpci.queryCount = 2;
    VK_CHECK(vkCreateQueryPool(ctx.device, &qpci, nullptr, &queryPool));
    std::vector<double> bandwidths; bandwidths.reserve(batches);
    int lastPercent = -1; bool escaped = false;
    for (int i = 0; i < batches && !escaped; i++) {
        vkCmdResetQueryPool(cb, queryPool, 0, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);
        for (int j = 0; j < copiesPerBatch; j++) { VkBufferCopy copy{0,0,size}; vkCmdCopyBuffer(cb, src.buffer, dst.buffer, 1, &copy); }
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
        EndCommandBuffer(ctx, cb); cb = BeginCommandBuffer(ctx);
        uint64_t ts[2]; VK_CHECK(vkGetQueryPoolResults(ctx.device, queryPool, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        double ns = double(ts[1] - ts[0]) * ctx.timestampPeriod;
        bandwidths.push_back((double(size) * copiesPerBatch / (1024.0*1024.0*1024.0)) / (ns / 1e9));
        int percent = (i+1)*100/batches;
        if (percent != lastPercent) { std::cout << "\rProgress: " << percent << "% " << std::flush; lastPercent = percent; }
        if (checkEscape && CheckForEscape()) escaped = true;
    }
    std::cout << "\n";
    if (cb) EndCommandBuffer(ctx, cb);
    vkDestroyQueryPool(ctx.device, queryPool, nullptr);
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

TestResults RunLatencyTest(const char* name, VulkanContext& ctx, Buffer& src, Buffer& dst, int iterations, bool checkEscape = false) {
    std::cout << name << "\n"; PrintDivider();
    VkCommandBuffer cmdBuf; VkCommandBufferAllocateInfo allocInfo{}; allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = ctx.commandPool; allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(ctx.device, &allocInfo, &cmdBuf);
    std::vector<double> latencies; latencies.reserve(iterations);
    int lastPercent = -1; bool escaped = false;
    for (int i = 0; i < iterations && !escaped; i++) {
        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmdBuf, &bi);
        VkBufferCopy copy{0,0,src.size}; vkCmdCopyBuffer(cmdBuf, src.buffer, dst.buffer, 1, &copy);
        vkEndCommandBuffer(cmdBuf);
        auto start = std::chrono::high_resolution_clock::now();
        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cmdBuf;
        vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(ctx.queue);
        auto end = std::chrono::high_resolution_clock::now();
        latencies.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        int percent = (i+1)*100/iterations;
        if (percent != lastPercent) { std::cout << "\rProgress: " << percent << "% " << std::flush; lastPercent = percent; }
        if (checkEscape && CheckForEscape()) escaped = true;
    }
    std::cout << "\n";
    vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &cmdBuf);
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

TestResults RunCommandLatencyTest(VulkanContext& ctx, int iterations, bool checkEscape = false) {
    const char* name = "Command Latency";
    std::cout << name << "\n"; PrintDivider();
    std::vector<double> latencies; latencies.reserve(iterations);
    int lastPercent = -1; bool escaped = false;
    for (int i = 0; i < iterations && !escaped; i++) {
        auto start = std::chrono::high_resolution_clock::now();
        VkCommandBufferAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = ctx.commandPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VkCommandBuffer cb; VK_CHECK(vkAllocateCommandBuffers(ctx.device, &ai, &cb));
        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &bi)); VK_CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        VK_CHECK(vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE)); VK_CHECK(vkQueueWaitIdle(ctx.queue));
        auto end = std::chrono::high_resolution_clock::now();
        latencies.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &cb);
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

TestResults RunBidirectionalTest(const char* name, VulkanContext& ctx, size_t bufferSize, int copiesPerBatch, int batches, bool checkEscape = false) {
    std::cout << name << "\n"; PrintDivider();
    std::cout << "(Simultaneous CPU->GPU and GPU->CPU transfers)\n";
    Buffer cpuSrc, gpuDst, gpuSrc, cpuDst;
    cpuSrc.Create(ctx, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    gpuDst.Create(ctx, bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    gpuSrc.Create(ctx, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    cpuDst.Create(ctx, bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkCommandBuffer cb = BeginCommandBuffer(ctx);
    VkQueryPool queryPool; VkQueryPoolCreateInfo qpci{}; qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP; qpci.queryCount = 2;
    VK_CHECK(vkCreateQueryPool(ctx.device, &qpci, nullptr, &queryPool));
    std::vector<double> bandwidths; bandwidths.reserve(batches);
    int lastPercent = -1; bool escaped = false;
    for (int i = 0; i < batches && !escaped; i++) {
        vkCmdResetQueryPool(cb, queryPool, 0, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);
        for (int j = 0; j < copiesPerBatch; j++) {
            VkBufferCopy upCopy{0,0,bufferSize}, dnCopy{0,0,bufferSize};
            vkCmdCopyBuffer(cb, cpuSrc.buffer, gpuDst.buffer, 1, &upCopy);
            vkCmdCopyBuffer(cb, gpuSrc.buffer, cpuDst.buffer, 1, &dnCopy);
        }
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
        EndCommandBuffer(ctx, cb); cb = BeginCommandBuffer(ctx);
        uint64_t ts[2]; VK_CHECK(vkGetQueryPoolResults(ctx.device, queryPool, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        double ns = double(ts[1] - ts[0]) * ctx.timestampPeriod;
        bandwidths.push_back((double(bufferSize) * copiesPerBatch * 2 / (1024.0*1024.0*1024.0)) / (ns / 1e9));
        int percent = (i+1)*100/batches;
        if (percent != lastPercent) { std::cout << "\rProgress: " << percent << "% " << std::flush; lastPercent = percent; }
        if (checkEscape && CheckForEscape()) escaped = true;
    }
    std::cout << "\n";
    if (cb) EndCommandBuffer(ctx, cb);
    vkDestroyQueryPool(ctx.device, queryPool, nullptr);
    cpuSrc.Destroy(ctx); gpuDst.Destroy(ctx); gpuSrc.Destroy(ctx); cpuDst.Destroy(ctx);
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
    std::cout << "Vulkan Benchmark Configuration\n"; PrintDoubleDivider();
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
    std::cout << "GPU-PCIe-Test v2.0 (Vulkan)\n"; PrintDoubleDivider();
    
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

    VulkanContext ctx; ctx.Init(cli.gpuIndex);
    if (config.showDetailedGPUInfo) ctx.PrintGPUInfo();
    CSVLogger logger(config.csvFilename);
    std::vector<std::vector<TestResults>> allRuns(6);

    for (int run = 0; run < config.numRuns; run++) {
        if (config.numRuns > 1) std::cout << "\n--- Run " << (run+1) << " of " << config.numRuns << " ---\n";
        int idx = 0;
        
        // Bandwidth tests - consistent naming with single-exe
        Buffer cpuSrc, gpuDst;
        cpuSrc.Create(ctx, config.largeBandwidthSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        gpuDst.Create(ctx, config.largeBandwidthSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        allRuns[idx++].push_back(RunBandwidthTest(("CPU->GPU " + FormatSize(config.largeBandwidthSize) + " Bandwidth").c_str(), ctx, cpuSrc, gpuDst, config.largeBandwidthSize, config.copiesPerBatch, config.bandwidthBatches, config.continuousMode));
        cpuSrc.Destroy(ctx); gpuDst.Destroy(ctx);
        
        Buffer gpuSrc, cpuDst;
        gpuSrc.Create(ctx, config.largeBandwidthSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        cpuDst.Create(ctx, config.largeBandwidthSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        allRuns[idx++].push_back(RunBandwidthTest(("GPU->CPU " + FormatSize(config.largeBandwidthSize) + " Bandwidth").c_str(), ctx, gpuSrc, cpuDst, config.largeBandwidthSize, config.copiesPerBatch, config.bandwidthBatches, config.continuousMode));
        gpuSrc.Destroy(ctx); cpuDst.Destroy(ctx);
        
        if (config.enableBidirectionalTest) {
            allRuns[idx++].push_back(RunBidirectionalTest(("Bidirectional " + FormatSize(config.largeBandwidthSize)).c_str(), ctx, config.largeBandwidthSize, config.copiesPerBatch, config.bandwidthBatches, config.continuousMode));
        }
        
        // Latency tests
        if (!cli.noLatency) {
            allRuns[idx++].push_back(RunCommandLatencyTest(ctx, config.commandLatencyIters, config.continuousMode));
            
            Buffer smallSrc, smallDst;
            smallSrc.Create(ctx, config.smallLatencySize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            smallDst.Create(ctx, config.smallLatencySize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            allRuns[idx++].push_back(RunLatencyTest(("CPU->GPU " + FormatSize(config.smallLatencySize) + " Latency").c_str(), ctx, smallSrc, smallDst, config.transferLatencyIters, config.continuousMode));
            smallSrc.Destroy(ctx); smallDst.Destroy(ctx);
            
            Buffer smallSrc2, smallDst2;
            smallSrc2.Create(ctx, config.smallLatencySize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            smallDst2.Create(ctx, config.smallLatencySize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            allRuns[idx++].push_back(RunLatencyTest(("GPU->CPU " + FormatSize(config.smallLatencySize) + " Latency").c_str(), ctx, smallSrc2, smallDst2, config.transferLatencyIters, config.continuousMode));
            smallSrc2.Destroy(ctx); smallDst2.Destroy(ctx);
        }
    }

    // Summary
    std::cout << "\n"; PrintDoubleDivider();
    std::cout << "Vulkan Summary\n"; PrintDoubleDivider();
    
    double uploadBW = 0, downloadBW = 0;
    for (auto& runs : allRuns) {
        if (runs.empty()) continue;
        if (config.numRuns > 1) {
            auto agg = AggregateRuns(runs);
            PrintAggregatedResults(agg, config.numRuns);
            if (config.logAllRuns) for (int i = 0; i < (int)runs.size(); i++) logger.LogResult(runs[i], "Vulkan", i+1);
            logger.LogAggregated(agg, "Vulkan");
            if (runs[0].testName.find("CPU->GPU") != std::string::npos && runs[0].testName.find("Bandwidth") != std::string::npos) uploadBW = agg.avgAvg;
            if (runs[0].testName.find("GPU->CPU") != std::string::npos && runs[0].testName.find("Bandwidth") != std::string::npos) downloadBW = agg.avgAvg;
        } else {
            logger.LogResult(runs[0], "Vulkan", 1);
            if (runs[0].testName.find("CPU->GPU") != std::string::npos && runs[0].testName.find("Bandwidth") != std::string::npos) uploadBW = runs[0].avgValue;
            if (runs[0].testName.find("GPU->CPU") != std::string::npos && runs[0].testName.find("Bandwidth") != std::string::npos) downloadBW = runs[0].avgValue;
        }
    }
    if (uploadBW > 0 && downloadBW > 0) PrintInterfaceGuess(uploadBW, downloadBW);
    ctx.Cleanup();
    
    std::cout << "\nResults saved to: " << config.csvFilename << "\n";
    if (!cli.hasOptions) {
        std::cout << "\nPress any key to exit...\n"; _getch();
    }
    return 0;
}

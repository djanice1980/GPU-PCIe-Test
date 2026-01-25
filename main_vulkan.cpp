#include <vulkan/vulkan.h>
#include <chrono>
#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <numeric>
#include <string>
#include <iomanip>
#include <sstream>
#include <cstring>

// Platform-specific includes
#ifdef _WIN32
    #include <windows.h>   
    #include <conio.h>
#else
    #include <termios.h>
    #include <unistd.h>
    #include <sys/select.h>
#endif

#include "bandwidth_analysis.h"



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
    double p99Value;
    double p999Value;
    std::string unit;
    std::chrono::system_clock::time_point timestamp;
};

// ------------------------------------------------------------
// CSV Logger
// ------------------------------------------------------------
class CSVLogger {
public:
    CSVLogger(const std::string& filename) : filename_(filename) {
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
        std::tm tm = *std::localtime(&time);
        
        file_ << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << ","
              << "Vulkan,"  // API name
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
// Terminal helpers (Cross-platform)
// ------------------------------------------------------------
#ifdef _WIN32
// Windows implementation
static void InitTerminal() {
    // No initialization needed on Windows
}

static void RestoreTerminal() {
    // No restoration needed on Windows
}

static int GetChar() {
    return _getch();
}

static bool CheckForEscape() {
    if (_kbhit()) {
        int ch = _getch();
        return (ch == 27); // ESC key
    }
    return false;
}

#else
// Linux implementation
static termios old_tio;

static void InitTerminal() {
    termios new_tio;
    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;
    new_tio.c_lflag &= (~ICANON & ~ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

static void RestoreTerminal() {
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
}

static int GetChar() {
    int c = getchar();
    return c;
}

static bool CheckForEscape() {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    
    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    
    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
        int ch = getchar();
        return (ch == 27); // ESC key
    }
    return false;
}
#endif

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

#define VK_CHECK(x) \
    do { \
        VkResult err = x; \
        if (err) { \
            std::cerr << "Vulkan error: " << err << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            abort(); \
        } \
    } while (0)

// ------------------------------------------------------------
// Vulkan Context
// ------------------------------------------------------------
struct VulkanContext {
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkQueue queue;
    uint32_t queueFamilyIndex;
    VkCommandPool commandPool;
    
    void Init() {
        // Create instance
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

        // Pick physical device
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
        
        if (deviceCount == 0) {
            std::cerr << "ERROR: No Vulkan-capable GPU found!\n";
            exit(1);
        }
        
        // Get properties for all devices
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
            // Multiple GPUs - let user choose
            std::cout << "\n";
            PrintDoubleDivider();
            std::cout << "Found " << deviceCount << " GPUs:\n";
            PrintDoubleDivider();
            
            for (uint32_t i = 0; i < deviceCount; i++) {
                std::cout << (i + 1) << ". " << deviceProps[i].deviceName << "\n";
                
                // Calculate total VRAM
                uint64_t vram = 0;
                for (uint32_t j = 0; j < memProps[i].memoryHeapCount; j++) {
                    if (memProps[i].memoryHeaps[j].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                        vram += memProps[i].memoryHeaps[j].size;
                    }
                }
                std::cout << "   VRAM: " << (vram / (1024 * 1024)) << " MB\n";
                
                // Device type
                std::string deviceType;
                switch (deviceProps[i].deviceType) {
                    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: deviceType = "Discrete GPU"; break;
                    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: deviceType = "Integrated GPU"; break;
                    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: deviceType = "Virtual GPU"; break;
                    case VK_PHYSICAL_DEVICE_TYPE_CPU: deviceType = "CPU"; break;
                    default: deviceType = "Other"; break;
                }
                std::cout << "   Type: " << deviceType << "\n";
                
                // Vendor
                std::string vendor = "Unknown";
                switch (deviceProps[i].vendorID) {
                    case 0x10DE: vendor = "NVIDIA"; break;
                    case 0x1002: vendor = "AMD"; break;
                    case 0x8086: vendor = "Intel"; break;
                }
                std::cout << "   Vendor: " << vendor << "\n";
                
                if (i < deviceCount - 1) std::cout << "\n";
            }
            
            PrintDoubleDivider();
            std::cout << "Select GPU (1-" << deviceCount << ", default=1): ";
            
            char ch = GetChar();
            std::cout << ch << "\n\n";
            
            int selection = 1;
            if (ch >= '1' && ch <= '9') {
                selection = ch - '0';
            }
            
            if (selection < 1 || selection > (int)deviceCount) {
                selection = 1;
            }
            
            physicalDevice = devices[selection - 1];
            std::cout << "Selected: " << deviceProps[selection - 1].deviceName << "\n";
            PrintDoubleDivider();
        }
        
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physicalDevice, &props);

        // Find queue family
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

        queueFamilyIndex = 0;
        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
                queueFamilyIndex = i;
                break;
            }
        }

        // Create logical device
        float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;

        VkDeviceCreateInfo deviceCreateInfo{};
        deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceCreateInfo.queueCreateInfoCount = 1;
        deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;

        VK_CHECK(vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device));
        vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

        // Create command pool
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool));
    }

    void Cleanup() {
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
    }
};

// ------------------------------------------------------------
// Buffer Helper
// ------------------------------------------------------------
struct Buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    size_t size;

    void Create(VulkanContext& ctx, size_t size_, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
        size = size_;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(ctx.device, &bufferInfo, nullptr, &buffer));

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(ctx.device, buffer, &memReqs);

        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(ctx.physicalDevice, &memProps);

        uint32_t memoryTypeIndex = 0;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((memReqs.memoryTypeBits & (1 << i)) && 
                (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
                memoryTypeIndex = i;
                break;
            }
        }

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = memoryTypeIndex;
        VK_CHECK(vkAllocateMemory(ctx.device, &allocInfo, nullptr, &memory));
        VK_CHECK(vkBindBufferMemory(ctx.device, buffer, memory, 0));
    }

    void Destroy(VulkanContext& ctx) {
        vkDestroyBuffer(ctx.device, buffer, nullptr);
        vkFreeMemory(ctx.device, memory, nullptr);
    }
};

// ------------------------------------------------------------
// Bandwidth Test
// ------------------------------------------------------------
TestResults RunBandwidthTest(
    const char* name,
    VulkanContext& ctx,
    Buffer& src,
    Buffer& dst,
    size_t bufferSize,
    int copiesPerBatch,
    int batches,
    bool checkEscape = false)
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

    VkQueryPool queryPool;
    VkQueryPoolCreateInfo queryPoolInfo{};
    queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryPoolInfo.queryCount = 2;
    vkCreateQueryPool(ctx.device, &queryPoolInfo, nullptr, &queryPool);

    std::vector<double> bandwidths;
    int lastPercent = -1;
    bool escaped = false;

    for (int i = 0; i < batches && !escaped; i++) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmdBuf, &beginInfo);

        vkCmdResetQueryPool(cmdBuf, queryPool, 0, 2);
        vkCmdWriteTimestamp(cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);

        VkBufferCopy copyRegion{};
        copyRegion.size = bufferSize;
        for (int j = 0; j < copiesPerBatch; j++) {
            vkCmdCopyBuffer(cmdBuf, src.buffer, dst.buffer, 1, &copyRegion);
        }

        vkCmdWriteTimestamp(cmdBuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
        vkEndCommandBuffer(cmdBuf);

        auto start = std::chrono::high_resolution_clock::now();
        
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBuf;
        vkQueueSubmit(ctx.queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(ctx.queue);
        
        auto end = std::chrono::high_resolution_clock::now();
        double seconds = std::chrono::duration<double>(end - start).count();

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

    vkDestroyQueryPool(ctx.device, queryPool, nullptr);
    vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &cmdBuf);

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
// Command Latency Test
// ------------------------------------------------------------
TestResults RunCommandLatencyTest(
    VulkanContext& ctx,
    int iterations,
    bool checkEscape = false)
{
    const char* name = "Test 3 - Command Latency";
    std::cout << name << "\n";
    PrintDivider();

    std::vector<double> latencies;
    int lastPercent = -1;
    bool escaped = false;

    for (int i = 0; i < iterations && !escaped; i++) {
        auto start = std::chrono::high_resolution_clock::now();

        // Allocate command buffer
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = ctx.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer cmdBuf;
        vkAllocateCommandBuffers(ctx.device, &allocInfo, &cmdBuf);

        // Begin and immediately end (empty command buffer)
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmdBuf, &beginInfo);
        vkEndCommandBuffer(cmdBuf);

        // Submit and wait
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBuf;
        vkQueueSubmit(ctx.queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(ctx.queue);

        auto end = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(us);

        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &cmdBuf);

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
    results.p99Value = latencies[static_cast<size_t>(latencies.size() * 0.99)];
    results.p999Value = latencies[static_cast<size_t>(latencies.size() * 0.999)];
    results.unit = "us";
    results.timestamp = std::chrono::system_clock::now();

    PrintDivider();
    std::cout << "Results:\n";
    std::cout << "  Min        : " << std::fixed << std::setprecision(2) << results.minValue << " us\n";
    std::cout << "  Avg        : " << results.avgValue << " us\n";
    std::cout << "  Max        : " << results.maxValue << " us\n";
    std::cout << "  P99        : " << results.p99Value << " us\n";
    std::cout << "  P99.9      : " << results.p999Value << " us\n";
    PrintDoubleDivider();

    return results;
}

// ------------------------------------------------------------
// Latency Test
// ------------------------------------------------------------
TestResults RunLatencyTest(
    const char* name,
    VulkanContext& ctx,
    Buffer& src,
    Buffer& dst,
    int iterations,
    bool checkEscape = false)
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
        vkBeginCommandBuffer(cmdBuf, &beginInfo);

        VkBufferCopy copyRegion{};
        copyRegion.size = src.size;
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

    vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &cmdBuf);

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
        
        char choice = GetChar();
        std::cout << choice << "\n\n";

        RestoreTerminal();
        switch (choice) {
            case '1': {
                std::cout << "Enter large transfer size in MB: ";
                int mb;
                std::cin >> mb;
                std::cin.ignore();
                config.largeBandwidthSize = static_cast<size_t>(mb) * 1024 * 1024;
                break;
            }
            case '2': {
                std::cout << "Enter small transfer size in bytes: ";
                int bytes;
                std::cin >> bytes;
                std::cin.ignore();
                config.smallLatencySize = bytes;
                break;
            }
            case '3': {
                std::cout << "Enter command latency iterations: ";
                std::cin >> config.commandLatencyIters;
                std::cin.ignore();
                break;
            }
            case '4': {
                std::cout << "Enter transfer latency iterations: ";
                std::cin >> config.transferLatencyIters;
                std::cin.ignore();
                break;
            }
            case '5': {
                std::cout << "Enter copies per batch: ";
                std::cin >> config.copiesPerBatch;
                std::cin.ignore();
                break;
            }
            case '6': {
                std::cout << "Enter bandwidth test batches: ";
                std::cin >> config.bandwidthBatches;
                std::cin.ignore();
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
                std::getline(std::cin, config.csvFilename);
                break;
            }
            case 'H':
            case 'h': {
                PrintInterfaceReferenceChart();
                std::cout << "Press any key to continue...";
                GetChar();
                break;
            }
            case '0': {
                done = true;
                break;
            }
        }
        InitTerminal();
    }
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------
int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    InitTerminal();
    
    BenchmarkConfig config;
    
    std::cout << "==============================================\n";
    std::cout << "      GPU Performance Benchmark Tool\n";
    std::cout << "           (Vulkan - Cross-Platform)\n";
    std::cout << "==============================================\n\n";
    std::cout << "Press 'C' to configure settings or any other key to use defaults...\n";
    
    char ch = GetChar();
    if (ch == 'c' || ch == 'C') {
        ConfigureSettings(config);
    }

    VulkanContext ctx;
    ctx.Init();

    CSVLogger* logger = config.enableCSVLogging ? new CSVLogger(config.csvFilename) : nullptr;

    auto RunBenchmarkSuite = [&]() {
        std::vector<TestResults> results;

        // CPU -> GPU bandwidth
        Buffer cpuSrc, gpuDst;
        cpuSrc.Create(ctx, config.largeBandwidthSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        gpuDst.Create(ctx, config.largeBandwidthSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        
        results.push_back(RunBandwidthTest(
            ("Test 1 - CPU -> GPU " + FormatSize(config.largeBandwidthSize) + " Transfer Bandwidth").c_str(),
            ctx, cpuSrc, gpuDst, config.largeBandwidthSize, config.copiesPerBatch, 
            config.bandwidthBatches, config.continuousMode));
        
        cpuSrc.Destroy(ctx);
        gpuDst.Destroy(ctx);

        // GPU -> CPU bandwidth
        Buffer gpuSrc, cpuDst;
        gpuSrc.Create(ctx, config.largeBandwidthSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        cpuDst.Create(ctx, config.largeBandwidthSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        results.push_back(RunBandwidthTest(
            ("Test 2 - GPU -> CPU " + FormatSize(config.largeBandwidthSize) + " Transfer Bandwidth").c_str(),
            ctx, gpuSrc, cpuDst, config.largeBandwidthSize, config.copiesPerBatch, 
            config.bandwidthBatches, config.continuousMode));
        
        gpuSrc.Destroy(ctx);
        cpuDst.Destroy(ctx);

        // Command latency test
        results.push_back(RunCommandLatencyTest(ctx, config.commandLatencyIters, config.continuousMode));

        // Small transfer latency tests
        Buffer smallCpuSrc, smallGpuDst;
        smallCpuSrc.Create(ctx, config.smallLatencySize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        smallGpuDst.Create(ctx, config.smallLatencySize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        
        results.push_back(RunLatencyTest(
            ("Test 4 - CPU -> GPU " + FormatSize(config.smallLatencySize) + " Transfer Latency").c_str(),
            ctx, smallCpuSrc, smallGpuDst, config.transferLatencyIters, config.continuousMode));
        
        smallCpuSrc.Destroy(ctx);
        smallGpuDst.Destroy(ctx);

        Buffer smallGpuSrc, smallCpuDst;
        smallGpuSrc.Create(ctx, config.smallLatencySize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        smallCpuDst.Create(ctx, config.smallLatencySize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        results.push_back(RunLatencyTest(
            ("Test 5 - GPU -> CPU " + FormatSize(config.smallLatencySize) + " Transfer Latency").c_str(),
            ctx, smallGpuSrc, smallCpuDst, config.transferLatencyIters, config.continuousMode));
        
        smallGpuSrc.Destroy(ctx);
        smallCpuDst.Destroy(ctx);

        // Analyze bandwidth and detect interface type
        if (results.size() >= 2) {
            double uploadBandwidth = results[0].avgValue;   // CPU->GPU
            double downloadBandwidth = results[1].avgValue; // GPU->CPU
            PrintInterfaceGuess(uploadBandwidth, downloadBandwidth);
        }

        // Log results
        if (logger) {
            for (const auto& result : results) {
                logger->LogResult(result);
            }
        }
    };

    if (config.continuousMode) {
        std::cout << "\nContinuous mode enabled. Press ESC during any test to stop.\n";
        std::cout << "Press any key to begin...";
        GetChar();
        std::cout << "\n\n";

        int runNumber = 1;
        while (true) {
            std::cout << "\n*** RUN #" << runNumber << " ***\n";
            RunBenchmarkSuite();
            
            std::cout << "\nRun #" << runNumber << " complete. Press ESC to exit or any other key to continue...\n";
            ch = GetChar();
            if (ch == 27) break;
            runNumber++;
        }
    } else {
        RunBenchmarkSuite();
    }

    delete logger;
    ctx.Cleanup();
    
    return 0;
}

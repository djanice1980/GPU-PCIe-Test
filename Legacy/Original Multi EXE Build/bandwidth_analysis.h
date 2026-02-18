// ============================================================================
// bandwidth_analysis.h - Shared header for GPU-PCIe-Test v2.0
// ============================================================================

#ifndef BANDWIDTH_ANALYSIS_H
#define BANDWIDTH_ANALYSIS_H

// Prevent Windows.h from defining min/max macros that conflict with std::min/max
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <string>
#include <vector>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <numeric>
#include <cmath>

// ============================================================================
//                              CONSTANTS
// ============================================================================

namespace Constants {
    constexpr size_t DEFAULT_BANDWIDTH_SIZE     = 256ull * 1024 * 1024;
    constexpr size_t DEFAULT_LATENCY_SIZE       = 1;
    constexpr int    DEFAULT_COMMAND_LATENCY_ITERS = 100000;
    constexpr int    DEFAULT_TRANSFER_LATENCY_ITERS = 10000;
    constexpr int    DEFAULT_COPIES_PER_BATCH   = 8;
    constexpr int    DEFAULT_BANDWIDTH_BATCHES  = 32;
    constexpr int    DEFAULT_NUM_RUNS           = 3;
    constexpr int    MAX_NUM_RUNS               = 10;
    constexpr int    MIN_NUM_RUNS               = 1;
    constexpr int    DIVIDER_WIDTH              = 70;
    constexpr double REALISTIC_MIN_PERCENT      = 60.0;
    constexpr double REALISTIC_MAX_PERCENT      = 95.0;
    constexpr double EXCEPTIONAL_THRESHOLD      = 95.0;
}

// ============================================================================
//                          DATA STRUCTURES
// ============================================================================

struct InterfaceSpeed {
    const char* name;
    double bandwidth_gbps;
    const char* description;
};

static constexpr InterfaceSpeed INTERFACE_SPEEDS[] = {
    {"PCIe 3.0 x1",  0.985,  "Single lane PCIe Gen 3"},
    {"PCIe 3.0 x4",  3.94,   "Common for M.2 SSDs"},
    {"PCIe 3.0 x8",  7.88,   "Older GPUs"},
    {"PCIe 3.0 x16", 15.75,  "Standard GPU slot (older)"},
    {"PCIe 4.0 x1",  1.97,   "Single lane PCIe Gen 4"},
    {"PCIe 4.0 x4",  7.88,   "Modern M.2 SSDs"},
    {"PCIe 4.0 x8",  15.75,  "Some modern GPUs"},
    {"PCIe 4.0 x16", 31.5,   "Modern GPU slot"},
    {"PCIe 5.0 x1",  3.94,   "Single lane PCIe Gen 5"},
    {"PCIe 5.0 x4",  15.75,  "Next-gen M.2 SSDs"},
    {"PCIe 5.0 x8",  31.5,   "High-end GPUs x8"},
    {"PCIe 5.0 x16", 63.0,   "Cutting-edge GPU slot"},
    {"Thunderbolt 3", 2.75,  "40 Gbps eGPU"},
    {"Thunderbolt 4", 2.75,  "40 Gbps"},
    {"Thunderbolt 5", 6.0,   "80 Gbps"},
    {"USB4 Gen 3x2",   4.8,  "40 Gbps USB4"},
};

static constexpr int NUM_INTERFACES = sizeof(INTERFACE_SPEEDS) / sizeof(InterfaceSpeed);

struct BandwidthAnalysis {
    const InterfaceSpeed* likelyInterface = nullptr;
    double percentOfTheoretical = 0;
    bool isRealistic = false;
};

struct GPUInfo {
    std::string name;
    std::string vendor;
    std::string driverVersion;
    uint64_t dedicatedVideoMemory = 0;
    uint64_t sharedSystemMemory = 0;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
};

struct TestResults {
    std::string testName;
    double minValue = 0, avgValue = 0, maxValue = 0;
    double p99Value = 0, p999Value = 0;
    std::string unit;
    std::chrono::system_clock::time_point timestamp;
};

struct AggregatedResults {
    std::string testName;
    std::string unit;
    double avgMin = 0, avgAvg = 0, avgMax = 0;
    double avgP99 = 0, avgP999 = 0, stdDevAvg = 0;
    std::vector<TestResults> runs;
};

struct BenchmarkConfig {
    size_t largeBandwidthSize = Constants::DEFAULT_BANDWIDTH_SIZE;
    size_t smallLatencySize = Constants::DEFAULT_LATENCY_SIZE;
    int commandLatencyIters = Constants::DEFAULT_COMMAND_LATENCY_ITERS;
    int transferLatencyIters = Constants::DEFAULT_TRANSFER_LATENCY_ITERS;
    int copiesPerBatch = Constants::DEFAULT_COPIES_PER_BATCH;
    int bandwidthBatches = Constants::DEFAULT_BANDWIDTH_BATCHES;
    int numRuns = Constants::DEFAULT_NUM_RUNS;
    bool logAllRuns = false;
    bool continuousMode = false;
    bool enableCSVLogging = true;
    bool enableBidirectionalTest = true;
    bool showDetailedGPUInfo = true;
    std::string csvFilename = "gpu_benchmark_results.csv";
};

// ============================================================================
//                          UTILITY FUNCTIONS
// ============================================================================

inline void PrintDivider() {
    std::cout << std::string(Constants::DIVIDER_WIDTH, '-') << '\n';
}

inline void PrintDoubleDivider() {
    std::cout << std::string(Constants::DIVIDER_WIDTH, '=') << '\n';
}

inline std::string FormatSize(size_t bytes) {
    if (bytes >= 1024ull * 1024 * 1024) return std::to_string(bytes / (1024ull * 1024 * 1024)) + " GB";
    if (bytes >= 1024 * 1024) return std::to_string(bytes / (1024 * 1024)) + " MB";
    if (bytes >= 1024) return std::to_string(bytes / 1024) + " KB";
    return std::to_string(bytes) + " B";
}

inline std::string FormatMemorySize(uint64_t bytes) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    if (bytes >= 1024ull * 1024 * 1024) oss << (bytes / (1024.0 * 1024.0 * 1024.0)) << " GB";
    else if (bytes >= 1024 * 1024) oss << (bytes / (1024.0 * 1024.0)) << " MB";
    else oss << bytes << " bytes";
    return oss.str();
}

inline std::string GetVendorName(uint32_t vendorId) {
    switch (vendorId) {
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD";
        case 0x8086: return "Intel";
        case 0x1414: return "Microsoft";
        default: return "Unknown";
    }
}

inline double CalculateStdDev(const std::vector<double>& values, double mean) {
    if (values.size() < 2) return 0.0;
    double sum = 0.0;
    for (double v : values) sum += (v - mean) * (v - mean);
    return std::sqrt(sum / (values.size() - 1));
}

// ============================================================================
//                        BANDWIDTH ANALYSIS
// ============================================================================

inline BandwidthAnalysis AnalyzeBandwidth(double measuredBandwidth_gbps) {
    BandwidthAnalysis result;
    double bestMatch = -1, bestDiff = 1e9;
    
    for (int i = 0; i < NUM_INTERFACES; i++) {
        double theoretical = INTERFACE_SPEEDS[i].bandwidth_gbps;
        double percent = (measuredBandwidth_gbps / theoretical) * 100.0;
        if (percent >= Constants::REALISTIC_MIN_PERCENT && percent <= Constants::REALISTIC_MAX_PERCENT) {
            double diff = std::abs(theoretical - measuredBandwidth_gbps);
            if (diff < bestDiff) { bestDiff = diff; bestMatch = i; }
        }
    }
    
    if (bestMatch >= 0) {
        result.likelyInterface = &INTERFACE_SPEEDS[(int)bestMatch];
        result.percentOfTheoretical = (measuredBandwidth_gbps / result.likelyInterface->bandwidth_gbps) * 100.0;
        result.isRealistic = true;
        return result;
    }
    
    for (int i = 0; i < NUM_INTERFACES; i++) {
        double diff = std::abs(INTERFACE_SPEEDS[i].bandwidth_gbps - measuredBandwidth_gbps);
        if (diff < bestDiff) { bestDiff = diff; bestMatch = i; }
    }
    
    if (bestMatch >= 0) {
        result.likelyInterface = &INTERFACE_SPEEDS[(int)bestMatch];
        result.percentOfTheoretical = (measuredBandwidth_gbps / result.likelyInterface->bandwidth_gbps) * 100.0;
    }
    return result;
}

inline void PrintInterfaceGuess(double uploadBW, double downloadBW) {
    double avgBW = (uploadBW + downloadBW) / 2.0;
    BandwidthAnalysis analysis = AnalyzeBandwidth(std::max(uploadBW, downloadBW));
    
    std::cout << "\n";
    PrintDoubleDivider();
    std::cout << "Interface Detection\n";
    PrintDoubleDivider();
    
    if (analysis.likelyInterface) {
        std::cout << "\n+-- Likely Connection Type ---------------------\n";
        std::cout << "|\n|  " << analysis.likelyInterface->name << "\n";
        std::cout << "|  " << analysis.likelyInterface->description << "\n|\n";
        
        double upPct = (uploadBW / analysis.likelyInterface->bandwidth_gbps) * 100.0;
        double dnPct = (downloadBW / analysis.likelyInterface->bandwidth_gbps) * 100.0;
        double avgPct = (avgBW / analysis.likelyInterface->bandwidth_gbps) * 100.0;
        
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "|  Upload:   " << uploadBW << " GB/s  (" << std::setprecision(1) << upPct << "% of " << analysis.likelyInterface->name << ")\n";
        std::cout << "|  Download: " << std::setprecision(2) << downloadBW << " GB/s  (" << std::setprecision(1) << dnPct << "% of " << analysis.likelyInterface->name << ")\n|\n";
        
        if (avgPct >= Constants::EXCEPTIONAL_THRESHOLD) std::cout << "|  [i] Exceptionally high efficiency - excellent!\n";
        else if (analysis.isRealistic) std::cout << "|  [OK] Performance is as expected for this interface\n";
        else if (avgPct < Constants::REALISTIC_MIN_PERCENT) std::cout << "|  [!] Lower than expected - possible bottleneck\n";
        
        std::cout << "|\n+-----------------------------------------------\n";
    }
    PrintDoubleDivider();
}

// ============================================================================
//                     MULTI-RUN AGGREGATION
// ============================================================================

inline AggregatedResults AggregateRuns(const std::vector<TestResults>& runs) {
    AggregatedResults agg;
    if (runs.empty()) return agg;
    
    agg.testName = runs[0].testName;
    agg.unit = runs[0].unit;
    agg.runs = runs;
    
    std::vector<double> avgValues;
    for (const auto& r : runs) {
        agg.avgMin += r.minValue;
        agg.avgAvg += r.avgValue;
        agg.avgMax += r.maxValue;
        agg.avgP99 += r.p99Value;
        agg.avgP999 += r.p999Value;
        avgValues.push_back(r.avgValue);
    }
    
    size_t n = runs.size();
    agg.avgMin /= n; agg.avgAvg /= n; agg.avgMax /= n;
    agg.avgP99 /= n; agg.avgP999 /= n;
    agg.stdDevAvg = CalculateStdDev(avgValues, agg.avgAvg);
    
    return agg;
}

inline void PrintAggregatedResults(const AggregatedResults& agg, int numRuns) {
    PrintDivider();
    std::cout << "Aggregated (" << numRuns << " runs): " << agg.testName << "\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Avg: " << agg.avgAvg << " " << agg.unit << " (StdDev: " << agg.stdDevAvg << ")\n";
    std::cout << "  Min: " << agg.avgMin << " | Max: " << agg.avgMax << " " << agg.unit << "\n";
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
        if (!exists && file_.is_open()) file_ << "Timestamp,API,Test Name,Run,Min,Avg,Max,P99,P99.9,StdDev,Unit\n";
    }
    ~CSVLogger() { if (file_.is_open()) file_.close(); }

    void LogResult(const TestResults& r, const std::string& api, int run = 0) {
        if (!file_.is_open()) return;
        auto t = std::chrono::system_clock::to_time_t(r.timestamp);
        std::tm tm; localtime_s(&tm, &t);
        file_ << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "," << api << "," << r.testName << ","
              << run << "," << r.minValue << "," << r.avgValue << "," << r.maxValue << ","
              << r.p99Value << "," << r.p999Value << ",," << r.unit << "\n";
        file_.flush();
    }

    void LogAggregated(const AggregatedResults& r, const std::string& api) {
        if (!file_.is_open()) return;
        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm; localtime_s(&tm, &t);
        file_ << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "," << api << "," << r.testName << ",AVG,"
              << r.avgMin << "," << r.avgAvg << "," << r.avgMax << ","
              << r.avgP99 << "," << r.avgP999 << "," << r.stdDevAvg << "," << r.unit << "\n";
        file_.flush();
    }

private:
    std::string filename_;
    std::ofstream file_;
};

#endif // BANDWIDTH_ANALYSIS_H

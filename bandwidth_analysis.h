// bandwidth_analysis.h
// PCIe/Thunderbolt Bandwidth Analysis and Printing Utilities

#ifndef BANDWIDTH_ANALYSIS_H
#define BANDWIDTH_ANALYSIS_H

#include <iostream>
#include <string>
#include <iomanip>

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
#endif // BANDWIDTH_ANALYSIS_H
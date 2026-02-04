// ============================================================================
// GPU-PCIe-Test v2.0 - Launcher
// Runs D3D12 and/or Vulkan benchmarks
// ============================================================================

// Prevent Windows.h from defining min/max macros that conflict with std::min/max
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <iostream>
#include <string>
#include <conio.h>

void PrintDoubleDivider() { std::cout << std::string(70, '=') << '\n'; }
void PrintDivider() { std::cout << std::string(70, '-') << '\n'; }

bool RunExecutable(const char* exeName) {
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    
    if (!CreateProcessA(exeName, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::cerr << "Failed to start " << exeName << " (error " << GetLastError() << ")\n";
        return false;
    }
    
    WaitForSingleObject(pi.hProcess, INFINITE);
    
    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    
    return exitCode == 0;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    
    std::cout << "\n";
    PrintDoubleDivider();
    std::cout << "      GPU Performance Benchmark Tool v2.0\n";
    std::cout << "         Combined D3D12 + Vulkan\n";
    PrintDoubleDivider();
    
    std::cout << "\nSelect Graphics API:\n";
    PrintDivider();
    std::cout << "  1. Direct3D 12 (Windows native)\n";
    std::cout << "  2. Vulkan (Cross-platform)\n";
    std::cout << "  3. Both (run sequentially for comparison)\n";
    PrintDivider();
    std::cout << "Choice (1-3, default=1): ";
    
    std::string input;
    std::getline(std::cin, input);
    
    int choice = 1;
    if (!input.empty()) {
        try { choice = std::stoi(input); } catch (...) { choice = 1; }
    }
    if (choice < 1 || choice > 3) choice = 1;
    
    std::cout << "\nSelected: ";
    switch (choice) {
        case 1: std::cout << "Direct3D 12\n"; break;
        case 2: std::cout << "Vulkan\n"; break;
        case 3: std::cout << "Both APIs (D3D12 first, then Vulkan)\n"; break;
    }
    
    bool success = true;
    
    if (choice == 1 || choice == 3) {
        std::cout << "\n";
        PrintDoubleDivider();
        std::cout << "       Running Direct3D 12 Benchmark\n";
        PrintDoubleDivider();
        std::cout << "\n";
        
        success = RunExecutable("GPU-PCIe-Test_D3D12.exe");
        
        if (!success) {
            std::cerr << "D3D12 benchmark failed!\n";
        } else {
            std::cout << "\nDirect3D 12 complete!\n";
        }
    }
    
    if (choice == 2 || choice == 3) {
        std::cout << "\n";
        PrintDoubleDivider();
        std::cout << "          Running Vulkan Benchmark\n";
        PrintDoubleDivider();
        std::cout << "\n";
        
        success = RunExecutable("GPU-PCIe-Test_Vulkan.exe") && success;
        
        if (!success) {
            std::cerr << "Vulkan benchmark failed!\n";
        }
    }
    
    // Final summary - same style as single-exe
    std::cout << "\n=== Benchmark Complete! ===\n";
    std::cout << "Results saved to: gpu_benchmark_results.csv\n";
    if (choice == 3) {
        std::cout << "APIs tested: D3D12 and Vulkan\n";
    }
    
    std::cout << "\nPress any key to exit...\n";
    _getch();
    
    return success ? 0 : 1;
}

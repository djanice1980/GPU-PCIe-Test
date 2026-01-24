// GPU Benchmark Launcher - Integrated Menu System
// Wraps separate D3D12 and Vulkan executables with unified menu

#include <windows.h>
#include <iostream>
#include <conio.h>
#include <string>

void PrintDivider() { std::cout << "----------------------------------------------\n"; }
void PrintDoubleDivider() { std::cout << "==============================================\n"; }

int SelectAPI() {
    std::cout << "\n";
    PrintDoubleDivider();
    std::cout << "      GPU Performance Benchmark Tool\n";
    std::cout << "         Combined D3D12 + Vulkan\n";
    PrintDoubleDivider();
    std::cout << "\nSelect Graphics API:\n";
    PrintDivider();
    std::cout << "  1. Direct3D 12 (Windows native)\n";
    std::cout << "  2. Vulkan (Cross-platform)\n";
    std::cout << "  3. Both (run sequentially for comparison)\n";
    PrintDivider();
    std::cout << "Choice (1-3, default=1): ";
    
    char ch = _getch();
    std::cout << ch << "\n\n";
    return (ch >= '1' && ch <= '3') ? (ch - '0') : 1;
}

bool CheckExecutableExists(const char* filename) {
    DWORD attr = GetFileAttributesA(filename);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

int RunExecutable(const char* exeName, const char* displayName) {
    if (!CheckExecutableExists(exeName)) {
        std::cerr << "ERROR: " << exeName << " not found!\n";
        std::cerr << "Please build the " << displayName << " version first.\n\n";
        return -1;
    }
    
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    
    // Use current console
    if (!CreateProcessA(exeName, NULL, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        std::cerr << "Failed to launch " << exeName << "\n";
        std::cerr << "Error code: " << GetLastError() << "\n\n";
        return 1;
    }
    
    // Wait for process to complete
    WaitForSingleObject(pi.hProcess, INFINITE);
    
    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    
    return exitCode;
}

int main() {
    int choice = SelectAPI();
    int result = 0;
    
    std::cout << "Selected: ";
    
    switch (choice) {
        case 1: {
            std::cout << "Direct3D 12\n\n";
            result = RunExecutable("GPU-PCIe-Test_D3D12.exe", "Direct3D 12");
            break;
        }
        
        case 2: {
            std::cout << "Vulkan\n\n";
            result = RunExecutable("GPU-PCIe-Test_Vulkan.exe", "Vulkan");
            break;
        }
        
        case 3: {
            std::cout << "Both APIs (D3D12 first, then Vulkan)\n\n";
            
            // Run D3D12 first
            PrintDoubleDivider();
            std::cout << "       Running Direct3D 12 Benchmark\n";
            PrintDoubleDivider();
            std::cout << "\n";
            
            result = RunExecutable("GPU-PCIe-Test_D3D12.exe", "Direct3D 12");
            
            if (result != 0) {
                if (result == -1) {
                    std::cout << "Skipping Vulkan benchmark.\n";
                } else {
                    std::cout << "\nDirect3D 12 benchmark failed!\n";
                    std::cout << "Skipping Vulkan benchmark.\n";
                }
                break;
            }
            
            std::cout << "\n";
            std::cout << "Direct3D 12 complete!\n\n";
            
            // Run Vulkan second
            PrintDoubleDivider();
            std::cout << "          Running Vulkan Benchmark\n";
            PrintDoubleDivider();
            std::cout << "\n";
            
            result = RunExecutable("GPU-PCIe-Test_Vulkan.exe", "Vulkan");
            
            if (result != 0) {
                if (result != -1) {
                    std::cout << "\nVulkan benchmark failed!\n";
                }
                break;
            }
            
            // Both completed successfully
            std::cout << "\n";
            PrintDoubleDivider();
            std::cout << "        Comparison Complete!\n";
            PrintDoubleDivider();
            std::cout << "\nBoth benchmarks finished successfully!\n";
            std::cout << "Results saved to: gpu_benchmark_results.csv\n\n";
            std::cout << "You can now compare D3D12 vs Vulkan performance\n";
            std::cout << "by opening the CSV file in Excel or a spreadsheet app.\n\n";
            std::cout << "Look for the \"API\" column to filter results.\n";
            PrintDoubleDivider();
            break;
        }
    }
    
    if (result != 0 && result != -1) {
        std::cout << "\nBenchmark exited with code: " << result << "\n";
    }
    
    std::cout << "\nPress any key to exit...";
    _getch();
    return (result < 0) ? 1 : result;
}

// ============================================================================
// PCIe Bandwidth Test - Simplified nvbandwidth-style tool
// Compares unidirectional vs bidirectional H2D/D2H bandwidth
// ============================================================================
// Build: nvcc -O3 pcie_bandwidth_test.cu -o pcie_bandwidth_test
// Run:   ./pcie_bandwidth_test
// ============================================================================

#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Default test parameters
#define DEFAULT_SIZE_MB 256
#define DEFAULT_ITERATIONS 10
#define WARMUP_ITERATIONS 3

// Error checking macro
#define CUDA_CHECK(call) do { \
    cudaError_t err = call; \
    if (err != cudaSuccess) { \
        fprintf(stderr, "CUDA Error at %s:%d - %s\n", __FILE__, __LINE__, cudaGetErrorString(err)); \
        exit(1); \
    } \
} while(0)

// Timing helper using CUDA events
double measureTransfer(void* dst, void* src, size_t size, int iterations, cudaStream_t stream) {
    cudaEvent_t start, stop;
    CUDA_CHECK(cudaEventCreate(&start));
    CUDA_CHECK(cudaEventCreate(&stop));
    
    // Warmup
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        CUDA_CHECK(cudaMemcpyAsync(dst, src, size, cudaMemcpyDefault, stream));
    }
    CUDA_CHECK(cudaStreamSynchronize(stream));
    
    // Timed runs
    CUDA_CHECK(cudaEventRecord(start, stream));
    for (int i = 0; i < iterations; i++) {
        CUDA_CHECK(cudaMemcpyAsync(dst, src, size, cudaMemcpyDefault, stream));
    }
    CUDA_CHECK(cudaEventRecord(stop, stream));
    CUDA_CHECK(cudaStreamSynchronize(stream));
    
    float ms = 0;
    CUDA_CHECK(cudaEventElapsedTime(&ms, start, stop));
    
    CUDA_CHECK(cudaEventDestroy(start));
    CUDA_CHECK(cudaEventDestroy(stop));
    
    double seconds = ms / 1000.0;
    double totalBytes = (double)size * iterations;
    return (totalBytes / (1024.0 * 1024.0 * 1024.0)) / seconds;  // GB/s
}

// Bidirectional test - measures one direction while other runs simultaneously
double measureBidirectional(void* h2d_dst, void* h2d_src, void* d2h_dst, void* d2h_src, 
                           size_t size, int iterations, bool measureH2D) {
    cudaStream_t streamMeasured, streamInterference;
    CUDA_CHECK(cudaStreamCreate(&streamMeasured));
    CUDA_CHECK(cudaStreamCreate(&streamInterference));
    
    cudaEvent_t start, stop;
    CUDA_CHECK(cudaEventCreate(&start));
    CUDA_CHECK(cudaEventCreate(&stop));
    
    // Warmup both directions
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        CUDA_CHECK(cudaMemcpyAsync(h2d_dst, h2d_src, size, cudaMemcpyDefault, streamMeasured));
        CUDA_CHECK(cudaMemcpyAsync(d2h_dst, d2h_src, size, cudaMemcpyDefault, streamInterference));
    }
    CUDA_CHECK(cudaDeviceSynchronize());
    
    // Timed runs - both directions simultaneously
    void* measured_dst = measureH2D ? h2d_dst : d2h_dst;
    void* measured_src = measureH2D ? h2d_src : d2h_src;
    void* interf_dst = measureH2D ? d2h_dst : h2d_dst;
    void* interf_src = measureH2D ? d2h_src : h2d_src;
    
    CUDA_CHECK(cudaEventRecord(start, streamMeasured));
    for (int i = 0; i < iterations; i++) {
        CUDA_CHECK(cudaMemcpyAsync(measured_dst, measured_src, size, cudaMemcpyDefault, streamMeasured));
        CUDA_CHECK(cudaMemcpyAsync(interf_dst, interf_src, size, cudaMemcpyDefault, streamInterference));
    }
    CUDA_CHECK(cudaEventRecord(stop, streamMeasured));
    CUDA_CHECK(cudaStreamSynchronize(streamMeasured));
    CUDA_CHECK(cudaStreamSynchronize(streamInterference));
    
    float ms = 0;
    CUDA_CHECK(cudaEventElapsedTime(&ms, start, stop));
    
    CUDA_CHECK(cudaEventDestroy(start));
    CUDA_CHECK(cudaEventDestroy(stop));
    CUDA_CHECK(cudaStreamDestroy(streamMeasured));
    CUDA_CHECK(cudaStreamDestroy(streamInterference));
    
    double seconds = ms / 1000.0;
    double totalBytes = (double)size * iterations;
    return (totalBytes / (1024.0 * 1024.0 * 1024.0)) / seconds;  // GB/s
}

// Sequential round-trip test (simulates our D3D12 benchmark method)
void measureRoundTrip(void* d_buf, void* h_upload, void* h_download, size_t size, 
                      int iterations, double* uploadBw, double* downloadBw) {
    cudaStream_t stream;
    CUDA_CHECK(cudaStreamCreate(&stream));
    
    cudaEvent_t start, stop;
    CUDA_CHECK(cudaEventCreate(&start));
    CUDA_CHECK(cudaEventCreate(&stop));
    
    // Warmup
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        CUDA_CHECK(cudaMemcpyAsync(d_buf, h_upload, size, cudaMemcpyHostToDevice, stream));
        CUDA_CHECK(cudaMemcpyAsync(h_download, d_buf, size, cudaMemcpyDeviceToHost, stream));
    }
    CUDA_CHECK(cudaStreamSynchronize(stream));
    
    // Measure round-trip
    CUDA_CHECK(cudaEventRecord(start, stream));
    for (int i = 0; i < iterations; i++) {
        CUDA_CHECK(cudaMemcpyAsync(d_buf, h_upload, size, cudaMemcpyHostToDevice, stream));
        CUDA_CHECK(cudaMemcpyAsync(h_download, d_buf, size, cudaMemcpyDeviceToHost, stream));
    }
    CUDA_CHECK(cudaEventRecord(stop, stream));
    CUDA_CHECK(cudaStreamSynchronize(stream));
    
    float ms = 0;
    CUDA_CHECK(cudaEventElapsedTime(&ms, start, stop));
    
    double seconds = ms / 1000.0;
    double totalBytes = (double)size * iterations * 2;  // Both directions
    double roundtripBw = (totalBytes / (1024.0 * 1024.0 * 1024.0)) / seconds;
    
    // Report as symmetric (divide by 2) - this is what our old D3D12 code did
    *uploadBw = roundtripBw / 2.0;
    *downloadBw = roundtripBw / 2.0;
    
    CUDA_CHECK(cudaEventDestroy(start));
    CUDA_CHECK(cudaEventDestroy(stop));
    CUDA_CHECK(cudaStreamDestroy(stream));
}

void printSeparator() {
    printf("================================================================\n");
}

int main(int argc, char** argv) {
    size_t sizeMB = DEFAULT_SIZE_MB;
    int iterations = DEFAULT_ITERATIONS;
    
    // Parse command line args
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            sizeMB = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            iterations = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-s size_mb] [-i iterations]\n", argv[0]);
            printf("  -s  Buffer size in MB (default: %d)\n", DEFAULT_SIZE_MB);
            printf("  -i  Number of iterations (default: %d)\n", DEFAULT_ITERATIONS);
            return 0;
        }
    }
    
    size_t size = sizeMB * 1024 * 1024;
    
    // Get device info
    cudaDeviceProp prop;
    CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    
    printSeparator();
    printf("PCIe Bandwidth Test (nvbandwidth-style)\n");
    printSeparator();
    printf("GPU: %s\n", prop.name);
    printf("Buffer Size: %zu MB\n", sizeMB);
    printf("Iterations: %d\n", iterations);
    printSeparator();
    
    // Allocate pinned host memory (critical for accurate PCIe measurement)
    void *h_upload, *h_download, *h_upload2, *h_download2;
    CUDA_CHECK(cudaMallocHost(&h_upload, size));
    CUDA_CHECK(cudaMallocHost(&h_download, size));
    CUDA_CHECK(cudaMallocHost(&h_upload2, size));
    CUDA_CHECK(cudaMallocHost(&h_download2, size));
    
    // Allocate device memory
    void *d_buf, *d_buf2;
    CUDA_CHECK(cudaMalloc(&d_buf, size));
    CUDA_CHECK(cudaMalloc(&d_buf2, size));
    
    // Initialize host memory with pattern
    memset(h_upload, 0xAA, size);
    memset(h_upload2, 0x55, size);
    
    cudaStream_t stream;
    CUDA_CHECK(cudaStreamCreate(&stream));
    
    // ========================================
    // TEST 1: Unidirectional H2D (Host to Device)
    // ========================================
    printf("\n[Test 1] Unidirectional Host -> Device (H2D)\n");
    double h2d_uni = measureTransfer(d_buf, h_upload, size, iterations, stream);
    printf("  Bandwidth: %.2f GB/s\n", h2d_uni);
    
    // ========================================
    // TEST 2: Unidirectional D2H (Device to Host)
    // ========================================
    printf("\n[Test 2] Unidirectional Device -> Host (D2H)\n");
    double d2h_uni = measureTransfer(h_download, d_buf, size, iterations, stream);
    printf("  Bandwidth: %.2f GB/s\n", d2h_uni);
    
    // ========================================
    // TEST 3: Bidirectional - Measure H2D with D2H interference
    // ========================================
    printf("\n[Test 3] Bidirectional H2D (with simultaneous D2H)\n");
    double h2d_bidir = measureBidirectional(d_buf, h_upload, h_download, d_buf2, 
                                            size, iterations, true);
    printf("  Bandwidth: %.2f GB/s\n", h2d_bidir);
    
    // ========================================
    // TEST 4: Bidirectional - Measure D2H with H2D interference
    // ========================================
    printf("\n[Test 4] Bidirectional D2H (with simultaneous H2D)\n");
    double d2h_bidir = measureBidirectional(d_buf, h_upload, h_download, d_buf2, 
                                            size, iterations, false);
    printf("  Bandwidth: %.2f GB/s\n", d2h_bidir);
    
    // ========================================
    // TEST 5: Sequential Round-Trip (our D3D12 method simulation)
    // ========================================
    printf("\n[Test 5] Sequential Round-Trip (D3D12 method simulation)\n");
    double rt_upload, rt_download;
    measureRoundTrip(d_buf, h_upload, h_download, size, iterations, &rt_upload, &rt_download);
    printf("  Upload (calculated): %.2f GB/s\n", rt_upload);
    printf("  Download (calculated): %.2f GB/s\n", rt_download);
    
    // ========================================
    // Summary
    // ========================================
    printSeparator();
    printf("SUMMARY\n");
    printSeparator();
    printf("%-40s %10s %10s %10s\n", "Test", "H2D", "D2H", "Ratio");
    printSeparator();
    printf("%-40s %9.2f GB/s %9.2f GB/s  %.2f:1\n", 
           "Unidirectional", h2d_uni, d2h_uni, h2d_uni / d2h_uni);
    printf("%-40s %9.2f GB/s %9.2f GB/s  %.2f:1\n", 
           "Bidirectional", h2d_bidir, d2h_bidir, h2d_bidir / d2h_bidir);
    printf("%-40s %9.2f GB/s %9.2f GB/s  %.2f:1\n", 
           "Sequential Round-Trip (div by 2)", rt_upload, rt_download, rt_upload / rt_download);
    printSeparator();
    
    printf("\nANALYSIS:\n");
    if (h2d_uni > d2h_uni * 0.9 && h2d_uni < d2h_uni * 1.1) {
        printf("- Unidirectional: SYMMETRIC (as expected for PCIe)\n");
    } else if (h2d_uni > d2h_uni) {
        printf("- Unidirectional: H2D faster than D2H (typical for GPUs)\n");
    } else {
        printf("- Unidirectional: D2H faster than H2D (unusual)\n");
    }
    
    if (h2d_bidir < h2d_uni * 0.7) {
        printf("- Bidirectional H2D dropped %.0f%% vs unidirectional (PCIe contention)\n", 
               (1.0 - h2d_bidir / h2d_uni) * 100);
    }
    
    if (d2h_bidir > d2h_uni * 0.9) {
        printf("- Bidirectional D2H held steady (typical behavior)\n");
    }
    
    double asymmetry = (rt_upload < rt_download) ? (rt_download / rt_upload) : (rt_upload / rt_download);
    if (asymmetry > 1.5) {
        printf("- Round-trip method shows %.1fx asymmetry - this may explain D3D12 results\n", asymmetry);
    } else {
        printf("- Round-trip method shows reasonable symmetry\n");
    }
    
    printSeparator();
    
    // Cleanup
    CUDA_CHECK(cudaStreamDestroy(stream));
    CUDA_CHECK(cudaFree(d_buf));
    CUDA_CHECK(cudaFree(d_buf2));
    CUDA_CHECK(cudaFreeHost(h_upload));
    CUDA_CHECK(cudaFreeHost(h_download));
    CUDA_CHECK(cudaFreeHost(h_upload2));
    CUDA_CHECK(cudaFreeHost(h_download2));
    
    return 0;
}

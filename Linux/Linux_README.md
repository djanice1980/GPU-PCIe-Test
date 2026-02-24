# GPU-PCIe-Test - Linux Vulkan Edition

Linux port of the GPU/PCIe bandwidth, latency, and VRAM integrity benchmarking tool.

## Features

- **PCIe Bandwidth Testing** - Upload (CPU→GPU) and Download (GPU→CPU) with accurate measurement
- **Bidirectional Testing** - Simultaneous upload/download using dual transfer queues
- **Latency Measurement** - Per-copy and command dispatch overhead
- **VRAM Integrity Scanning** - 8 test patterns, error clustering, fresh allocation per chunk
- **Hardware Detection** - PCIe link speed/width via sysfs, Thunderbolt/USB4/eGPU detection
- **System RAM Info** - Speed, channels, type via /proc/meminfo + dmidecode
- **Interactive GUI** - Dear ImGui with real-time progress, graphs, and CSV export
- **Multi-GPU Support** - Separate render and benchmark devices

## Requirements

### Build Dependencies

**Ubuntu/Debian:**
```bash
sudo apt install cmake g++ libvulkan-dev libglfw3-dev
```

**Fedora/RHEL:**
```bash
sudo dnf install cmake gcc-c++ vulkan-devel glfw-devel
```

**Arch Linux:**
```bash
sudo pacman -S cmake vulkan-devel glfw
```

### Runtime Requirements

- Vulkan-capable GPU with installed drivers (NVIDIA, AMD Mesa/AMDVLK, or Intel)
- X11 or Wayland display server
- Optional: `dmidecode` for detailed system RAM detection (requires root)

## Building

### Quick Build

```bash
cd Linux
chmod +x build_linux.sh
./build_linux.sh
```

### Manual CMake Build

```bash
cd Linux
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Debug Build with Vulkan Validation

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_VULKAN_VALIDATION=ON
make -j$(nproc)
```

Requires `vulkan-validationlayers` package for validation layer support.

## Running

```bash
./build/gpu-pcie-test-vulkan
```

### With System RAM Detection (Optional)

For full system memory info (speed, type, channels), run with root:

```bash
sudo ./build/gpu-pcie-test-vulkan
```

Without root, total RAM capacity is still detected via `/proc/meminfo`.

## Platform Differences from Windows Version

| Feature | Windows | Linux |
|---|---|---|
| Window System | Win32 API | GLFW (X11/Wayland) |
| Vulkan Surface | VK_KHR_win32_surface | glfwCreateWindowSurface |
| PCIe Link Info | SetupAPI (DEVPKEY) | sysfs (/sys/bus/pci/devices/) |
| eGPU Detection | cfgmgr32 device tree | sysfs + /sys/bus/thunderbolt/ |
| System RAM | WMI (Win32_PhysicalMemory) | /proc/meminfo + dmidecode |
| DPI Scaling | SetProcessDpiAwareness | glfwGetWindowContentScale |
| ImGui Backend | imgui_impl_win32 | imgui_impl_glfw |

## Benchmark Methodology

The benchmark methodology is identical to the Windows version:

- **Download (GPU→CPU):** GPU timestamps on the dedicated transfer queue
- **Upload (CPU→GPU) - Discrete:** CPU round-trip timing (ReBAR workaround)
- **Upload (CPU→GPU) - Integrated:** GPU timestamps (no bus transfer)
- **Bidirectional:** Dual transfer queues submitted simultaneously
- **Latency:** GPU timestamps per individual small copy

Results between Windows and Linux versions should be directly comparable since
both use the same Vulkan API calls and measurement approach.

## Troubleshooting

### "Failed to initialize Vulkan"
- Verify Vulkan drivers: `vulkaninfo | head`
- NVIDIA: Install `nvidia-driver` or `nvidia-dkms`
- AMD: Mesa Vulkan (`mesa-vulkan-drivers`) or AMDVLK
- Intel: `intel-media-va-driver` + `mesa-vulkan-drivers`

### No PCIe link info detected
- Check sysfs permissions: `ls -la /sys/bus/pci/devices/`
- Verify GPU is visible: `lspci | grep -i vga`

### dmidecode fails
- Requires root privileges: `sudo dmidecode --type memory`
- Install if missing: `sudo apt install dmidecode`

### GLFW errors on Wayland
- Ensure `libwayland-dev` and `libxkbcommon-dev` are installed
- GLFW 3.4+ recommended for native Wayland support
- Set `DISPLAY=:0` or `WAYLAND_DISPLAY=wayland-0` if needed

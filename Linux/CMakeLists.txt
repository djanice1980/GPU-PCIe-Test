cmake_minimum_required(VERSION 3.16)
project(GPU-PCIe-Test-Vulkan-Linux VERSION 3.0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type defaults to Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler warnings (match Windows /W4 /WX)
add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers)

# Release optimizations
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

# ==============================================================================
# Dependencies
# ==============================================================================

find_package(Vulkan REQUIRED)
find_package(glfw3 3.3 REQUIRED)
find_package(Threads REQUIRED)

# ==============================================================================
# Download ImGui and ImPlot (pinned versions matching Windows builds)
# ==============================================================================

include(FetchContent)

FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.8-docking.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_Declare(implot
    URL https://github.com/epezent/implot/archive/refs/tags/v0.16.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(imgui implot)

# ==============================================================================
# Main executable
# ==============================================================================

add_executable(gpu-pcie-test-vulkan
    main_gui_vulkan_linux.cpp

    # ImGui core
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp

    # ImGui backends
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp

    # ImPlot
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)

target_include_directories(gpu-pcie-test-vulkan PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${implot_SOURCE_DIR}
)

target_link_libraries(gpu-pcie-test-vulkan PRIVATE
    Vulkan::Vulkan
    glfw
    Threads::Threads
)

# Optional: Vulkan validation layers for debug builds
option(ENABLE_VULKAN_VALIDATION "Enable Vulkan validation layers" OFF)
if(ENABLE_VULKAN_VALIDATION)
    target_compile_definitions(gpu-pcie-test-vulkan PRIVATE ENABLE_VULKAN_VALIDATION)
endif()

# ==============================================================================
# Install
# ==============================================================================

install(TARGETS gpu-pcie-test-vulkan RUNTIME DESTINATION bin)

cmake_minimum_required(VERSION 3.16)
project(GPU-PCIe-Test-Vulkan-Linux VERSION 3.4.0 LANGUAGES CXX)

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
    # Pinned so a moved tag / tampered download fails loudly. If GitHub ever
    # regenerates its source archives (rare but has happened), re-pin after
    # verifying the new archive's contents match the tag.
    URL_HASH SHA256=b237f93d071bcbb4d948259bfb7ea1464e341f20a84c9e49c7fe260906ad9d09
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_Declare(implot
    URL https://github.com/epezent/implot/archive/refs/tags/v0.16.zip
    URL_HASH SHA256=24f772c688f6b8a6e19d7efc10e4923a04a915f13d487b08b83553aa62ae1708
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

# Suppress warnings in third-party ImGui/ImPlot code (they use memset on structs, etc.)
set_source_files_properties(
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
    PROPERTIES COMPILE_FLAGS "-Wno-error -Wno-class-memaccess"
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

# Desktop integration (used by the AppImage and the Arch/CachyOS package)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/gpu-pcie-test.desktop
        DESTINATION share/applications)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/gpu-pcie-test.png
        DESTINATION share/icons/hicolor/256x256/apps)

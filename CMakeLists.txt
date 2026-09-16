cmake_minimum_required(VERSION 3.22...3.31)

project(RealTimePathTracing VERSION 0.1 LANGUAGES C CXX)

include(CTest)

# ----------------------------------------------------------------------------------------------------------------------
# CMake setup
#

# Add our local cmake scripts folder
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

# Prefer modern C++
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_definitions(_USE_MATH_DEFINES)
endif()

# ----------------------------------------------------------------------------------------------------------------------
# Output folders
# - Build tree goes under ./Build
# - Runtime outputs go under ./Binaries/<Config>
# - Library archives go under ./Intermediate/Lib
#

set(_BinariesDir "${CMAKE_CURRENT_SOURCE_DIR}/Binaries")
set(_IntermediateDir "${CMAKE_CURRENT_SOURCE_DIR}/Intermediate")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${_BinariesDir}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${_BinariesDir}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${_IntermediateDir}/Lib")

foreach(_Cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${_Cfg}" _CfgUpper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CfgUpper} "${_BinariesDir}/${_Cfg}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_CfgUpper} "${_BinariesDir}/${_Cfg}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_CfgUpper} "${_IntermediateDir}/Lib/${_Cfg}")
endforeach()

# ----------------------------------------------------------------------------------------------------------------------
# Vulkan-Headers are pinned with the source dependencies. Volk loads the system
# Vulkan loader at runtime, so compiling never depends on a machine SDK version.
#

# ----------------------------------------------------------------------------------------------------------------------
# Direct third-party dependencies. Every revision is pinned because these libraries
# are part of the renderer's platform and GPU ABI, not a transitive SDK detail.
#

include(FetchContent)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VMA_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(VMA_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_GL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_VALIDATOR_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_HEADER_ONLY ON CACHE BOOL "" FORCE)
set(TINYGLTF_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
FetchContent_Declare(vulkan_headers GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git GIT_TAG vulkan-sdk-1.4.341.0 GIT_SHALLOW TRUE)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1 GIT_SHALLOW TRUE)
FetchContent_Declare(fmt GIT_REPOSITORY https://github.com/fmtlib/fmt.git GIT_TAG 12.2.0 GIT_SHALLOW TRUE)
FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk.git GIT_TAG vulkan-sdk-1.4.341.0 GIT_SHALLOW TRUE)
FetchContent_Declare(vma GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git GIT_TAG v3.3.0 GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.92.1-docking GIT_SHALLOW TRUE)
FetchContent_Declare(tinygltf GIT_REPOSITORY https://github.com/syoyo/tinygltf.git GIT_TAG v2.9.6 GIT_SHALLOW TRUE)
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG f0569113c93ad095470c54bf34a17b36646bbbb5)

FetchContent_MakeAvailable(vulkan_headers)
FetchContent_MakeAvailable(glfw glm fmt volk vma imgui tinygltf stb)

target_link_libraries(volk PUBLIC Vulkan::Headers)

add_library(imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
)
target_include_directories(imgui PUBLIC "${imgui_SOURCE_DIR}")
target_link_libraries(imgui PUBLIC glfw volk::volk Vulkan::Headers)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK VK_NO_PROTOTYPES)

add_library(tinygltf_headers INTERFACE)
target_include_directories(tinygltf_headers SYSTEM INTERFACE "${tinygltf_SOURCE_DIR}")
if(NOT TARGET stb)
    add_library(stb INTERFACE)
    target_include_directories(stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")
endif()

# ----------------------------------------------------------------------------------------------------------------------
# Dependencies (NRD)
# - Denoising is part of the renderer contract, so NRD is required.
# - Use the official NVIDIA NRD repository directly through CMake.
# - Keep the integration Vulkan-native for this repo; the runtime wrapper comes
#   from our side, not from copying the sample project.
#

set(REAL_TIME_PATH_TRACING_NRD_GIT_TAG "v4.17.3" CACHE STRING "Pinned NVIDIA NRD tag or commit for reproducible builds")

# The renderer and NRD use the same pinned DXC release. 1.9.2602 is the first
# release we rely on for the final vk::BufferPointer scalar-layout fixes used by
# the scene device-address ABI.
set(SHADERMAKE_DXC_VERSION "v1.9.2602" CACHE STRING "Pinned DXC release" FORCE)
set(SHADERMAKE_DXC_DATE "2026_02_20" CACHE STRING "Pinned DXC release asset date" FORCE)
set(SHADERMAKE_FIND_DXC_VK OFF CACHE BOOL "Use ShaderMake's pinned DXC for upstream NRD shaders" FORCE)
unset(SHADERMAKE_DXC_VK_PATH CACHE)

if(WIN32 AND (NOT DEFINED SHADERMAKE_FXC_PATH OR SHADERMAKE_FXC_PATH STREQUAL "" OR SHADERMAKE_FXC_PATH MATCHES "-NOTFOUND$"))
    file(GLOB _real_time_path_tracing_fxc_candidates "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/fxc.exe")
    list(SORT _real_time_path_tracing_fxc_candidates ORDER DESCENDING)
    list(LENGTH _real_time_path_tracing_fxc_candidates _real_time_path_tracing_fxc_candidate_count)
    if(_real_time_path_tracing_fxc_candidate_count GREATER 0)
        list(GET _real_time_path_tracing_fxc_candidates 0 _real_time_path_tracing_fxc_path)
        set(SHADERMAKE_FXC_PATH "${_real_time_path_tracing_fxc_path}" CACHE FILEPATH "Path to fxc.exe used by ShaderMake" FORCE)
    endif()
endif()

set(NRD_EMBEDS_DXIL_SHADERS OFF CACHE BOOL "NRD embeds DXIL shaders" FORCE)
set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "NRD embeds DXBC shaders" FORCE)

FetchContent_Declare(
    NRD
    GIT_REPOSITORY https://github.com/NVIDIA-RTX/NRD.git
    GIT_TAG        ${REAL_TIME_PATH_TRACING_NRD_GIT_TAG}
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(NRD)

if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(NRD PRIVATE -Wno-error=unused-parameter)
endif()

if(DEFINED nrd_SOURCE_DIR)
    set(REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR "${nrd_SOURCE_DIR}" CACHE INTERNAL "Resolved NRD source directory")
endif()

# ----------------------------------------------------------------------------------------------------------------------
# Dependencies (Streamline)
# - DLSS Ray Reconstruction is reached through NVIDIA Streamline. The NGX SDK itself only ships MSVC static libraries,
#   which an LLVM MinGW build cannot link; Streamline is a set of DLLs with a plain extern "C" API, so it can be.
# - Streamline's source is MIT, but the DLSS model (nvngx_dlssd.dll) is under the NVIDIA RTX SDKs license. Nothing is
#   committed: the pinned release is downloaded here, and only the runtime DLLs and their license texts are copied
#   next to the executable.
# - Optional and Windows-only, because Streamline has no Linux build. Without it the renderer falls back to NRD.
#

option(REAL_TIME_PATH_TRACING_WITH_STREAMLINE "Download NVIDIA Streamline and enable DLSS Ray Reconstruction" ${WIN32})

if(REAL_TIME_PATH_TRACING_WITH_STREAMLINE)
    if(NOT WIN32)
        message(FATAL_ERROR "REAL_TIME_PATH_TRACING_WITH_STREAMLINE requires Windows; Streamline has no Linux runtime.")
    endif()

    # SOURCE_SUBDIR points at a folder without a CMakeLists.txt, so the SDK is only downloaded and never configured.
    FetchContent_Declare(
        streamline
        URL      https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.1/streamline-sdk-v2.14.1.zip
        URL_HASH SHA256=92c4d954631a1710da86ca3fa8d5034f2b9503838c95fc4ae977ae149319781b
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR  _no_cmake
    )

    FetchContent_MakeAvailable(streamline)

    set(REAL_TIME_PATH_TRACING_STREAMLINE_SOURCE_DIR "${streamline_SOURCE_DIR}" CACHE INTERNAL "Resolved Streamline SDK directory")
endif()

# ----------------------------------------------------------------------------------------------------------------------
# Real-Time PathTracing helpers (shader compilation, runtime copying, etc.)
#

include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/CompileHlsl.cmake")

# ----------------------------------------------------------------------------------------------------------------------
# Source
#

add_subdirectory(Source)

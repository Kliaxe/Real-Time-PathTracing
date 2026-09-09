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
# nvpro_core2 download cache
# - nvpro_core2 defaults to creating "_downloaded_resources" in the source root.
# - Keep the repo clean by putting it under the active build tree instead.
#

if(NOT DEFINED NVPRO_CORE2_DOWNLOAD_DIR)
    set(NVPRO_CORE2_DOWNLOAD_DIR
        "${CMAKE_BINARY_DIR}/_downloaded_resources"
        CACHE PATH "nvpro_core2 download/cache directory"
    )
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
# Dependencies (nvpro_core2)
#

find_package(NvproCore2 REQUIRED)

# ----------------------------------------------------------------------------------------------------------------------
# Dependencies (NRD)
# - Denoising is part of the renderer contract, so NRD is required.
# - Use the official NVIDIA NRD repository directly through CMake.
# - Keep the integration Vulkan-native for this repo; the runtime wrapper comes
#   from our side, not from copying the sample project.
#

set(REAL_TIME_PATH_TRACING_NRD_GIT_TAG "v4.17.3" CACHE STRING "Pinned NVIDIA NRD tag or commit for reproducible builds")

include(FetchContent)

if(WIN32 AND (NOT DEFINED SHADERMAKE_FXC_PATH OR SHADERMAKE_FXC_PATH STREQUAL "" OR SHADERMAKE_FXC_PATH MATCHES "-NOTFOUND$"))
    file(GLOB _real_time_path_tracing_fxc_candidates "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/fxc.exe")
    list(SORT _real_time_path_tracing_fxc_candidates ORDER DESCENDING)
    list(LENGTH _real_time_path_tracing_fxc_candidates _real_time_path_tracing_fxc_candidate_count)
    if(_real_time_path_tracing_fxc_candidate_count GREATER 0)
        list(GET _real_time_path_tracing_fxc_candidates 0 _real_time_path_tracing_fxc_path)
        set(SHADERMAKE_FXC_PATH "${_real_time_path_tracing_fxc_path}" CACHE FILEPATH "Path to fxc.exe used by ShaderMake" FORCE)
    endif()
endif()

if(WIN32 AND DEFINED ENV{VULKAN_SDK}
   AND (NOT DEFINED NRD_DXC_CUSTOM_PATH OR NRD_DXC_CUSTOM_PATH STREQUAL "" OR NRD_DXC_CUSTOM_PATH MATCHES "-NOTFOUND$"))
    set(_real_time_path_tracing_vulkan_dxc "$ENV{VULKAN_SDK}/Bin/dxc.exe")
    if(EXISTS "${_real_time_path_tracing_vulkan_dxc}")
        set(NRD_DXC_CUSTOM_PATH "${_real_time_path_tracing_vulkan_dxc}" CACHE FILEPATH "Path to dxc.exe used by NRD" FORCE)
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
# Real-Time PathTracing helpers (shader compilation, runtime copying, etc.)
#

include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/RealTimePathTracingSamples.cmake")

# ----------------------------------------------------------------------------------------------------------------------
# Source
#

add_subdirectory(Source)

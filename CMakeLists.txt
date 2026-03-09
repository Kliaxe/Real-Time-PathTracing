cmake_minimum_required(VERSION 3.22...3.31)

project(RealTimePathTracing VERSION 0.1 LANGUAGES C CXX)

include(CTest)

# ----------------------------------------------------------------------------------------------------------------------
# CMake setup
#

set_property(GLOBAL PROPERTY USE_FOLDERS ON) # Keep Visual Studio solution tidy

# Add our local cmake scripts folder
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

# Prefer modern C++
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
# Thesis helpers (shader compilation, runtime copying, etc.)
#

include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/ThesisSamples.cmake")

# ----------------------------------------------------------------------------------------------------------------------
# Source
#

add_subdirectory(Source)

# Make Visual Studio use our app as the startup project
set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT RealTimePathTracing)


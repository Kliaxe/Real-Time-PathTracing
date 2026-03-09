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

# When CMake is launched from a plain shell instead of a Visual Studio developer
# prompt, cl.exe may be discoverable on PATH without the standard include/library
# environment. Bootstrap those paths so Ninja + MSVC still has a complete toolchain.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{INCLUDE})
    set(_vswhere "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe")
    set(_vs_install "")
    if(EXISTS "${_vswhere}")
        execute_process(
            COMMAND "${_vswhere}" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            OUTPUT_VARIABLE _vs_install
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    endif()

    file(GLOB _msvc_tool_roots LIST_DIRECTORIES true "${_vs_install}/VC/Tools/MSVC/*")
    list(SORT _msvc_tool_roots ORDER DESCENDING)
    list(GET _msvc_tool_roots 0 _msvc_root)

    file(GLOB _winsdk_include_roots LIST_DIRECTORIES true "C:/Program Files (x86)/Windows Kits/10/Include/*")
    list(SORT _winsdk_include_roots ORDER DESCENDING)
    list(GET _winsdk_include_roots 0 _winsdk_include_root)

    file(GLOB _winsdk_lib_roots LIST_DIRECTORIES true "C:/Program Files (x86)/Windows Kits/10/Lib/*")
    list(SORT _winsdk_lib_roots ORDER DESCENDING)
    list(GET _winsdk_lib_roots 0 _winsdk_lib_root)

    set(_msvc_include_dir "${_msvc_root}/include")
    set(_msvc_lib_dir "${_msvc_root}/lib/x64")
    set(_winsdk_ucrt_include_dir "${_winsdk_include_root}/ucrt")
    set(_winsdk_um_include_dir "${_winsdk_include_root}/um")
    set(_winsdk_shared_include_dir "${_winsdk_include_root}/shared")
    set(_winsdk_cppwinrt_include_dir "${_winsdk_include_root}/cppwinrt")
    set(_winsdk_winrt_include_dir "${_winsdk_include_root}/winrt")
    set(_winsdk_ucrt_lib_dir "${_winsdk_lib_root}/ucrt/x64")
    set(_winsdk_um_lib_dir "${_winsdk_lib_root}/um/x64")

    set(_bootstrap_include_dirs
        "${_msvc_include_dir}"
        "${_winsdk_ucrt_include_dir}"
        "${_winsdk_um_include_dir}"
        "${_winsdk_shared_include_dir}"
        "${_winsdk_cppwinrt_include_dir}"
        "${_winsdk_winrt_include_dir}"
    )

    set(_bootstrap_lib_dirs
        "${_msvc_lib_dir}"
        "${_winsdk_ucrt_lib_dir}"
        "${_winsdk_um_lib_dir}"
    )

    list(FILTER _bootstrap_include_dirs INCLUDE REGEX ".+")
    list(FILTER _bootstrap_lib_dirs INCLUDE REGEX ".+")

    set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES "${_bootstrap_include_dirs}")
    set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES "${_bootstrap_include_dirs}")
    link_directories(${_bootstrap_lib_dirs})
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
# Thesis helpers (shader compilation, runtime copying, etc.)
#

include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/ThesisSamples.cmake")

# ----------------------------------------------------------------------------------------------------------------------
# Source
#

add_subdirectory(Source)

# Make Visual Studio use our app as the startup project
set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT RealTimePathTracing)


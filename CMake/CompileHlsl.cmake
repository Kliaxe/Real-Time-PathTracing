include_guard(GLOBAL)

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

if(DEFINED SHADERMAKE_DXC_PATH AND EXISTS "${SHADERMAKE_DXC_PATH}")
    set(RTPT_DXC_EXECUTABLE "${SHADERMAKE_DXC_PATH}" CACHE FILEPATH "Pinned DXC used by renderer shaders" FORCE)
else()
    message(FATAL_ERROR "The pinned ShaderMake DXC executable is unavailable. Configure NRD before including CompileHlsl.cmake.")
endif()

execute_process(
    COMMAND "${RTPT_DXC_EXECUTABLE}" --version
    RESULT_VARIABLE _rtpt_dxc_version_result
    OUTPUT_VARIABLE RTPT_DXC_VERSION
    ERROR_VARIABLE _rtpt_dxc_version_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _rtpt_dxc_version_result EQUAL 0)
    message(FATAL_ERROR "DXC version query failed: ${_rtpt_dxc_version_error}")
endif()
if(NOT RTPT_DXC_VERSION MATCHES "1\\.9\\.2602\\.17")
    message(FATAL_ERROR "Renderer shaders require pinned DXC 1.9.2602.17, found: ${RTPT_DXC_VERSION}")
endif()
message(STATUS "Renderer DXC: ${RTPT_DXC_EXECUTABLE} (${RTPT_DXC_VERSION})")

function(rtpt_compile_hlsl)
    set(_options)
    set(_one_value_args TARGET SOURCE ENTRY PROFILE SYMBOL)
    set(_multi_value_args INCLUDE_DIRS DEFINES DEPENDS EXTENSIONS)
    cmake_parse_arguments(HLSL "${_options}" "${_one_value_args}" "${_multi_value_args}" ${ARGN})

    foreach(_required TARGET SOURCE PROFILE SYMBOL)
        if(NOT HLSL_${_required})
            message(FATAL_ERROR "rtpt_compile_hlsl: missing ${_required}")
        endif()
    endforeach()

    get_filename_component(_source_name "${HLSL_SOURCE}" NAME)
    set(_output_dir "${CMAKE_CURRENT_BINARY_DIR}/Generated/Shaders")
    if(HLSL_ENTRY)
        set(_entry_suffix "${HLSL_ENTRY}")
        set(_entry_flags -E "${HLSL_ENTRY}")
    else()
        set(_entry_suffix "library")
        set(_entry_flags)
    endif()
    set(_spirv "${_output_dir}/${_source_name}.${_entry_suffix}.spv")
    set(_header "${_output_dir}/${_source_name}.${_entry_suffix}.h")

    set(_include_flags)
    foreach(_include_dir IN LISTS HLSL_INCLUDE_DIRS)
        list(APPEND _include_flags -I "${_include_dir}")
    endforeach()
    set(_define_flags)
    foreach(_define IN LISTS HLSL_DEFINES)
        list(APPEND _define_flags -D "${_define}")
    endforeach()

    # Keep the compiler's extension inference from adding ray-query capability
    # for the acceleration-structure type shared by pipeline ray tracing. Every
    # extension emitted by project shaders must come from this explicit allowlist.
    set(_extension_flags
        -fspv-extension=SPV_KHR_ray_tracing
        -fspv-extension=SPV_KHR_physical_storage_buffer
        -fspv-extension=SPV_EXT_descriptor_indexing
    )
    foreach(_extension IN LISTS HLSL_EXTENSIONS)
        list(APPEND _extension_flags "-fspv-extension=${_extension}")
    endforeach()

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_optimization_flags -Od -Zi)
    else()
        set(_optimization_flags -O3)
    endif()

    add_custom_command(
        OUTPUT "${_spirv}" "${_header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_output_dir}"
        COMMAND "${RTPT_DXC_EXECUTABLE}"
            -spirv
            -T "${HLSL_PROFILE}"
            ${_entry_flags}
            -HV 202x
            -fspv-target-env=vulkan1.3
            ${_extension_flags}
            -fvk-use-scalar-layout
            -enable-16bit-types
            -Zpr
            -WX
            -D RTPT_HLSL=1
            ${_optimization_flags}
            ${_include_flags}
            ${_define_flags}
            -Fo "${_spirv}"
            "${HLSL_SOURCE}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/Scripts/embed_spirv.py"
            "${_spirv}" "${_header}" "${HLSL_SYMBOL}"
        DEPENDS "${HLSL_SOURCE}" ${HLSL_DEPENDS} "${CMAKE_SOURCE_DIR}/Scripts/embed_spirv.py"
        COMMENT "Compiling HLSL ${_source_name}:${_entry_suffix}"
        VERBATIM
        COMMAND_EXPAND_LISTS
    )

    target_sources(${HLSL_TARGET} PRIVATE "${HLSL_SOURCE}" "${_header}")
    target_include_directories(${HLSL_TARGET} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    source_group("Shaders/HLSL" FILES "${HLSL_SOURCE}")
    source_group("Shaders/Compiled" FILES "${_header}")
endfunction()

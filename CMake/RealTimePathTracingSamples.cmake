# RealTimePathTracingSamples.cmake
#
# Minimal, refactor-friendly sample/app setup helpers.
# This is inspired by vk_raytracing_tutorial_KHR/CMake/RtTutorial.cmake but adapted to our single-app layout.
#

include_guard(GLOBAL)

# ----------------------------------------------------------------------------------------------------------------------
# setup_real_time_path_tracing_app()
#
# Usage:
#   setup_real_time_path_tracing_app(
#     TARGET <name>
#     SOURCE_DIR <dir>            # e.g. ${CMAKE_CURRENT_LIST_DIR}
#     SHADER_DIR <dir>            # e.g. ${CMAKE_CURRENT_LIST_DIR}/shaders
#     [EXTRA_SHADER_INCLUDES ...]
#   )
#

function(setup_real_time_path_tracing_app)
    set(_Options)
    set(_OneValueArgs TARGET SOURCE_DIR SHADER_DIR)
    set(_MultiValueArgs EXTRA_SHADER_INCLUDES)
    cmake_parse_arguments(REAL_TIME_PATH_TRACING "${_Options}" "${_OneValueArgs}" "${_MultiValueArgs}" ${ARGN})

    if(NOT REAL_TIME_PATH_TRACING_TARGET)
        message(FATAL_ERROR "setup_real_time_path_tracing_app: missing TARGET")
    endif()
    if(NOT REAL_TIME_PATH_TRACING_SOURCE_DIR)
        message(FATAL_ERROR "setup_real_time_path_tracing_app: missing SOURCE_DIR")
    endif()
    if(NOT REAL_TIME_PATH_TRACING_SHADER_DIR)
        message(FATAL_ERROR "setup_real_time_path_tracing_app: missing SHADER_DIR")
    endif()

    # ------------------------------------------------------------------------------------------------------------------
    # Sources
    #

    file(GLOB _ExeSources
        "${REAL_TIME_PATH_TRACING_SOURCE_DIR}/*.cpp"
        "${REAL_TIME_PATH_TRACING_SOURCE_DIR}/*.hpp"
        "${REAL_TIME_PATH_TRACING_SOURCE_DIR}/*.h"
        "${REAL_TIME_PATH_TRACING_SOURCE_DIR}/Common/*.cpp"
        "${REAL_TIME_PATH_TRACING_SOURCE_DIR}/Common/*.hpp"
        "${REAL_TIME_PATH_TRACING_SOURCE_DIR}/Common/*.h"
    )

    add_executable(${REAL_TIME_PATH_TRACING_TARGET} ${_ExeSources})
    set_property(TARGET ${REAL_TIME_PATH_TRACING_TARGET} PROPERTY FOLDER "RealTimePathTracing")

    # ------------------------------------------------------------------------------------------------------------------
    # Dependencies (nvpro_core2 targets)
    #

    target_link_libraries(${REAL_TIME_PATH_TRACING_TARGET} PRIVATE
        nvpro2::nvapp
        nvpro2::nvgui
        nvpro2::nvslang
        nvpro2::nvutils
        nvpro2::nvvk
        nvpro2::nvshaders_host
        nvpro2::nvaftermath
        nvpro2::nvvkgltf
        nvpro2::nvvkglsl
    )

    # Adds useful compile definitions used by nvpro helpers (paths, NVSHADERS_DIR, etc.)
    add_project_definitions(${REAL_TIME_PATH_TRACING_TARGET})

    # ------------------------------------------------------------------------------------------------------------------
    # Includes
    #

    target_include_directories(${REAL_TIME_PATH_TRACING_TARGET} PRIVATE
        "${CMAKE_BINARY_DIR}"
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/Source/ShaderIncludes"
        "${REAL_TIME_PATH_TRACING_SOURCE_DIR}"
    )

    # ------------------------------------------------------------------------------------------------------------------
    # Shaders (Slang + GLSL -> generated headers under the build tree)
    #

    set(_ShaderOutputDir "${CMAKE_CURRENT_BINARY_DIR}/_autogen")
    file(GLOB_RECURSE _ShaderGlslFiles CONFIGURE_DEPENDS "${REAL_TIME_PATH_TRACING_SHADER_DIR}/*.glsl")
    file(GLOB_RECURSE _ShaderSlangFiles CONFIGURE_DEPENDS "${REAL_TIME_PATH_TRACING_SHADER_DIR}/*.slang")
    file(GLOB_RECURSE _ShaderHeaderFiles CONFIGURE_DEPENDS "${REAL_TIME_PATH_TRACING_SHADER_DIR}/*.h" "${REAL_TIME_PATH_TRACING_SHADER_DIR}/*.h.slang")

    # Standard nvshaders we rely on (tonemapping + simple sky)
    list(APPEND _ShaderSlangFiles
        "${NVSHADERS_DIR}/nvshaders/sky_simple.slang"
        "${NVSHADERS_DIR}/nvshaders/tonemapper.slang"
    )

    # Shader include flags
    set(_ShaderIncludeFlags
        "-I${NVSHADERS_DIR}"
        "-I${CMAKE_SOURCE_DIR}"
        "-I${CMAKE_SOURCE_DIR}/Source"
        "-I${CMAKE_SOURCE_DIR}/Source/ShaderIncludes"
        "-I${REAL_TIME_PATH_TRACING_SOURCE_DIR}"
        "-I${REAL_TIME_PATH_TRACING_SHADER_DIR}"
    )
    if(REAL_TIME_PATH_TRACING_EXTRA_SHADER_INCLUDES)
        foreach(_IncludeDir IN LISTS REAL_TIME_PATH_TRACING_EXTRA_SHADER_INCLUDES)
            list(APPEND _ShaderIncludeFlags "-I${_IncludeDir}")
        endforeach()
    endif()

    # Shader optimization is NOT implied by the build type: compile_slang defaults to
    # -O0 -g1 for every configuration, so a Release binary would still run
    # unoptimized SPIR-V. That is fine for debugging and actively misleading for
    # measurement - at -O0 nothing is inlined and register pressure is wildly
    # unrepresentative, which distorts the cost of long, divergent shaders far more
    # than short ones and makes two variants of the same pass incomparable.
    #
    # Debug info is left on in both cases: compile_slang treats a DEBUG_LEVEL of 0 as
    # "not set" and substitutes 1, so asking for it would be a lie in the build file.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_ShaderOptimizationLevel 0)
    else()
        set(_ShaderOptimizationLevel 3)
    endif()

    compile_slang(
        "${_ShaderSlangFiles}"
        "${_ShaderOutputDir}"
        _GeneratedShaderHeaders
        OPTIMIZATION_LEVEL ${_ShaderOptimizationLevel}
        EXTRA_FLAGS ${_ShaderIncludeFlags}
    )

    compile_glsl(
        "${_ShaderGlslFiles}"
        "${_ShaderOutputDir}"
        _GeneratedGlslHeaders
        EXTRA_FLAGS ${_ShaderIncludeFlags}
    )

    # Make shader sources visible in IDE
    source_group("Shaders" FILES ${_ShaderSlangFiles} ${_ShaderGlslFiles} ${_ShaderHeaderFiles})
    source_group("Shaders/Compiled" FILES ${_GeneratedShaderHeaders} ${_GeneratedGlslHeaders})

    # Make sure shader compilation is part of the target build
    target_sources(${REAL_TIME_PATH_TRACING_TARGET} PRIVATE
        ${_ShaderSlangFiles}
        ${_ShaderGlslFiles}
        ${_ShaderHeaderFiles}
        ${_GeneratedShaderHeaders}
        ${_GeneratedGlslHeaders}
    )

    # The code includes headers like: #include \"_autogen/foo.slang.h\"
    target_include_directories(${REAL_TIME_PATH_TRACING_TARGET} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

    # ------------------------------------------------------------------------------------------------------------------
    # Runtime copying (Content + shaders)
    #

    set(_RuntimeDir "$<TARGET_FILE_DIR:${REAL_TIME_PATH_TRACING_TARGET}>")

    # Copy Content/ next to the exe (so path utils can find it via exePath/Content)
    add_custom_command(TARGET ${REAL_TIME_PATH_TRACING_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Content"
            "${_RuntimeDir}/Content"
        VERBATIM
    )

    # Copy app shader folder next to the exe (for hot reload)
    add_custom_command(TARGET ${REAL_TIME_PATH_TRACING_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${REAL_TIME_PATH_TRACING_SHADER_DIR}"
            "${_RuntimeDir}/Shaders"
        VERBATIM
    )

    # Copy app-local shared headers next to the exe (for Slang includes like \"common/IoGltf.h\")
    add_custom_command(TARGET ${REAL_TIME_PATH_TRACING_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${REAL_TIME_PATH_TRACING_SOURCE_DIR}/common"
            "${_RuntimeDir}/common"
        VERBATIM
    )

    # Copy shared shader include folder (Source/ShaderIncludes) next to the exe
    add_custom_command(TARGET ${REAL_TIME_PATH_TRACING_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Source/ShaderIncludes"
            "${_RuntimeDir}/ShaderIncludes"
        VERBATIM
    )

    # Copy Aftermath DLLs + glslang helper if nvpro provides those lists
    if(DEFINED NsightAftermath_DLLS)
        foreach(_Dll IN LISTS NsightAftermath_DLLS)
            add_custom_command(TARGET ${REAL_TIME_PATH_TRACING_TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_Dll}"
                    "${_RuntimeDir}"
                VERBATIM
            )
        endforeach()
    endif()

    # Copy Slang runtime DLLs (required to run the exe)
    # - On Windows the app will fail to start if slang-compiler.dll is missing.
    if(DEFINED Slang_DLL)
        add_custom_command(TARGET ${REAL_TIME_PATH_TRACING_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${Slang_DLL}"
                "${_RuntimeDir}"
            VERBATIM
        )
    endif()

    if(DEFINED Slang_GLSLANG)
        add_custom_command(TARGET ${REAL_TIME_PATH_TRACING_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${Slang_GLSLANG}"
                "${_RuntimeDir}"
            VERBATIM
        )
    endif()

    if(DEFINED Slang_GLSL_MODULE)
        add_custom_command(TARGET ${REAL_TIME_PATH_TRACING_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${Slang_GLSL_MODULE}"
                "${_RuntimeDir}"
            VERBATIM
        )
    endif()
endfunction()

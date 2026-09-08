project(RealTimePathTracing_Source LANGUAGES CXX)

# ----------------------------------------------------------------------------------------------------------------------
# Single app lives directly under Source/
#

set(_real_time_path_tracing_extra_shader_includes
    "${CMAKE_SOURCE_DIR}/Source"
)

if(DEFINED REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR)
    list(APPEND _real_time_path_tracing_extra_shader_includes
        "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Shaders/Include"
    )
endif()

setup_real_time_path_tracing_app(
    TARGET     RealTimePathTracing
    SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}"
    SHADER_DIR "${CMAKE_CURRENT_LIST_DIR}/Shaders"
    EXTRA_SHADER_INCLUDES
        ${_real_time_path_tracing_extra_shader_includes}
)

# Keep scene-system sources explicit so they are compiled even though the shared
# helper currently only globs top-level Source/ and Source/Common/.
target_sources(RealTimePathTracing PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/Application.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Application.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneCatalog.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneCatalog.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneAssetCatalog.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneAssetCatalog.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneResolver.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneResolver.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneUploader.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneUploader.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneRuntime.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneRuntime.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneRenderer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneRenderer.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneTypes.h"
    "${CMAKE_CURRENT_LIST_DIR}/Denoising/DenoiserResources.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Denoising/DenoiserResources.h"
    "${CMAKE_CURRENT_LIST_DIR}/Denoising/NrdDenoiser.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Denoising/NrdDenoiser.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/Common/ResolveMode.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/PathTracer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/PathTracer.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/ReSTIRFrameContext.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/ReSTIRFrameContext.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/ReSTIRRenderPassUtils.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/ReSTIRRenderPassUtils.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/ReSTIRPTParameterContext.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/ReSTIRPTRenderer.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/ReSTIRPTRendererTypes.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/ReSTIRPTSettings.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/Common/RendererUi.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/ReSTIRPTResources.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/ReSTIRPTPairingTexture.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/ReSTIRPTUi.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/Internal/ReSTIRPTUi.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/Internal/ReSTIRPTParameterContext.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/Internal/ReSTIRPTResources.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/Internal/ReSTIRPTPairingTexture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/Internal/ReSTIRPTRenderer.cpp"
)

if(NOT TARGET NRD)
    message(FATAL_ERROR "NRD is required by the renderer denoising path, but the NRD target was not created.")
endif()

target_link_libraries(RealTimePathTracing PRIVATE NRD)
target_include_directories(RealTimePathTracing PRIVATE
    "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Include"
    "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Integration"
)

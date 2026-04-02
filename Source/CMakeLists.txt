project(RealTimePathTracing_Source LANGUAGES CXX)

# ----------------------------------------------------------------------------------------------------------------------
# Single app lives directly under Source/
#

set(_thesis_extra_shader_includes
    "${CMAKE_SOURCE_DIR}/Source"
)

if(DEFINED THESIS_NRD_SOURCE_DIR)
    list(APPEND _thesis_extra_shader_includes
        "${THESIS_NRD_SOURCE_DIR}/Shaders/Include"
    )
endif()

setup_thesis_app(
    TARGET     RealTimePathTracing
    SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}"
    SHADER_DIR "${CMAKE_CURRENT_LIST_DIR}/Shaders"
    EXTRA_SHADER_INCLUDES
        ${_thesis_extra_shader_includes}
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
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/PathTraceDenoiserResources.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/PathTraceDenoiserResources.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/PathTraceNrdDenoiser.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/PathTraceNrdDenoiser.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/Common/ResolveMode.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/PathTracer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/PathTracer.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/Common/ReSTIRContext.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/Common/ReSTIRContext.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/Common/ReSTIRRendererCommon.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/Common/ReSTIRRenderUtils.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/Common/ReSTIRRenderUtils.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/Common/ReSTIRResources.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/Common/ReSTIRResources.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/Common/ReSTIRSettings.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/Common/ReSTIRSharedResources.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/Common/ReSTIRSharedResources.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/DI/ReSTIRDIResources.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/DI/ReSTIRDIResources.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/DI/ReSTIRDISettings.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/DI/ReSTIRDIRenderer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/DI/ReSTIRDIRenderer.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/DI/Internal/ReSTIRDIContext.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/DI/Internal/ReSTIRUtils.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/GI/ReSTIRGIResources.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/GI/ReSTIRGIResources.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/GI/ReSTIRGISettings.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/GI/ReSTIRGIRenderer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/GI/ReSTIRGIRenderer.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/GI/Internal/ReSTIRGIContext.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/ReSTIRPTSettings.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/ReSTIRPTRenderer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/ReSTIRPTRenderer.h"
)

if(TARGET NRD)
    target_link_libraries(RealTimePathTracing PRIVATE NRD)
    target_include_directories(RealTimePathTracing PRIVATE
        "${THESIS_NRD_SOURCE_DIR}/Include"
        "${THESIS_NRD_SOURCE_DIR}/Integration"
    )
    target_compile_definitions(RealTimePathTracing PRIVATE THESIS_ENABLE_NRD=1)
endif()

if(BUILD_TESTING)
    add_executable(PathTracingDiagnostics
        "${CMAKE_CURRENT_LIST_DIR}/Diagnostics/PathTracingDiagnostics.cpp"
    )

    target_include_directories(PathTracingDiagnostics PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}"
    )

    add_test(NAME PathTracingDiagnostics COMMAND PathTracingDiagnostics)
    set_target_properties(PathTracingDiagnostics PROPERTIES FOLDER "Diagnostics")
endif()


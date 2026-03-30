project(RealTimePathTracing_Source LANGUAGES CXX)

# ----------------------------------------------------------------------------------------------------------------------
# Single app lives directly under Source/
#

setup_thesis_app(
    TARGET     RealTimePathTracing
    SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}"
    SHADER_DIR "${CMAKE_CURRENT_LIST_DIR}/Shaders"
    EXTRA_SHADER_INCLUDES
        "${CMAKE_SOURCE_DIR}/Source"
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
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/ReSTIRPTSettings.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/ReSTIRPTRenderer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/PT/ReSTIRPTRenderer.h"
)

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


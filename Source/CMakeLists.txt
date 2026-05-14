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
    "${CMAKE_CURRENT_LIST_DIR}/Experiments/ExperimentController.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Experiments/ExperimentController.h"
    "${CMAKE_CURRENT_LIST_DIR}/Experiments/ExperimentGpuTimer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Experiments/ExperimentGpuTimer.h"
    "${CMAKE_CURRENT_LIST_DIR}/Experiments/ExperimentMetadataWriter.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Experiments/ExperimentMetadataWriter.h"
    "${CMAKE_CURRENT_LIST_DIR}/Experiments/ExperimentPlan.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Experiments/ExperimentPlan.h"
    "${CMAKE_CURRENT_LIST_DIR}/Experiments/ExperimentTypes.h"
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
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/ReSTIRDIFrameContext.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/ReSTIRDIFrameContext.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/ReSTIRDIContext.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/ReSTIRDIRendererTypes.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/ReSTIRDIRenderPassUtils.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/ReSTIRDIRenderPassUtils.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/ReSTIRUtils.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/ReSTIRDIResources.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/ReSTIRDIResources.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/ReSTIRDISettings.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/ReSTIRDIRenderer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/ReSTIRDIRenderer.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/Internal/ReSTIRDIContext.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/ReSTIR/Internal/ReSTIRUtils.cpp"
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


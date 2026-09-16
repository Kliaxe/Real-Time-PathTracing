add_subdirectory(Framework)

# ----------------------------------------------------------------------------------------------------------------------
# Single app lives directly under Source/
#

add_executable(RealTimePathTracing)
set_property(TARGET RealTimePathTracing PROPERTY FOLDER "RealTimePathTracing")
target_include_directories(RealTimePathTracing PRIVATE
    "${CMAKE_BINARY_DIR}"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes"
)
target_compile_definitions(RealTimePathTracing PRIVATE
    GLM_ENABLE_EXPERIMENTAL
    GLM_FORCE_RADIANS
    GLM_FORCE_XYZW_ONLY
    VK_NO_PROTOTYPES
)
if(WIN32)
    target_compile_definitions(RealTimePathTracing PRIVATE NOMINMAX WIN32)
endif()

# Keep the production source set explicit. Adding a module requires an intentional
# target-composition change rather than silently expanding a recursive glob.
target_sources(RealTimePathTracing PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/Main.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Application.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Application.h"
    "${CMAKE_CURRENT_LIST_DIR}/ApplicationOptions.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ApplicationOptions.h"
    "${CMAKE_CURRENT_LIST_DIR}/Assets/ImageData.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Assets/ImageData.h"
    "${CMAKE_CURRENT_LIST_DIR}/Camera/CameraController.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Camera/CameraController.h"
    "${CMAKE_CURRENT_LIST_DIR}/Camera/CameraState.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Camera/CameraState.h"
    "${CMAKE_CURRENT_LIST_DIR}/Camera/CameraUi.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Camera/CameraUi.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneCatalog.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneCatalog.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/GltfImport.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/GltfImport.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/LightSampling.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/LightSampling.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneAssetCatalog.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneAssetCatalog.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneResolver.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneResolver.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneUploader.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneUploader.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneRuntime.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneRuntime.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneTypes.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneGpuResources.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneUi.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneUi.h"
    "${CMAKE_CURRENT_LIST_DIR}/Denoising/DenoiserResources.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Denoising/DenoiserResources.h"
    "${CMAKE_CURRENT_LIST_DIR}/Denoising/NrdComposePass.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Denoising/NrdComposePass.h"
    "${CMAKE_CURRENT_LIST_DIR}/Denoising/NrdDenoiser.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Denoising/NrdDenoiser.h"
    "${CMAKE_CURRENT_LIST_DIR}/Denoising/NrdDisocclusionMixPass.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Denoising/NrdDisocclusionMixPass.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/Common/ResolveHistory.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/Common/ResolveHistory.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/Common/ResolveHistoryTracker.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/Common/ResolveHistoryTracker.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/Common/ResolveMode.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/PathTracer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/PathTracer.h"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/PathTracerUi.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PathTracing/PathTracerUi.h"
    "${CMAKE_CURRENT_LIST_DIR}/PostProcessing/Tonemapper.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PostProcessing/Tonemapper.h"
    "${CMAKE_CURRENT_LIST_DIR}/PostProcessing/TonemapperUi.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PostProcessing/TonemapperUi.h"
    "${CMAKE_CURRENT_LIST_DIR}/Rendering/SkyRenderer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Rendering/SkyRenderer.h"
    "${CMAKE_CURRENT_LIST_DIR}/Rendering/FrameCapture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Rendering/FrameCapture.h"
    "${CMAKE_CURRENT_LIST_DIR}/Rendering/FrameTimingReport.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Rendering/FrameTimingReport.h"
    "${CMAKE_CURRENT_LIST_DIR}/Rendering/FrameTimingStatistics.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Rendering/FrameTimingStatistics.h"
    "${CMAKE_CURRENT_LIST_DIR}/Rendering/FrameTimingUi.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Rendering/FrameTimingUi.h"
    "${CMAKE_CURRENT_LIST_DIR}/Rendering/RasterRenderer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Rendering/RasterRenderer.h"
    "${CMAKE_CURRENT_LIST_DIR}/Rendering/RenderTargetView.h"
    "${CMAKE_CURRENT_LIST_DIR}/Rendering/ViewportTargets.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Rendering/ViewportTargets.h"
    "${CMAKE_CURRENT_LIST_DIR}/Sampling/SpatiotemporalBlueNoise.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Sampling/SpatiotemporalBlueNoise.h"
    "${CMAKE_CURRENT_LIST_DIR}/Sampling/README.md"
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

set(_blue_noise_binary "${CMAKE_CURRENT_LIST_DIR}/Sampling/Generated/BlueNoise64x64x32.bin")
set(_blue_noise_header "${CMAKE_CURRENT_BINARY_DIR}/Generated/Assets/BlueNoise64x64x32.h")
add_custom_command(
    OUTPUT "${_blue_noise_header}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/Scripts/embed_binary.py"
        "${_blue_noise_binary}" "${_blue_noise_header}" BlueNoise64x64x32
        --expected-size 131072
    DEPENDS "${_blue_noise_binary}" "${CMAKE_SOURCE_DIR}/Scripts/embed_binary.py"
    COMMENT "Embedding project-owned spatiotemporal blue-noise volume"
    VERBATIM
)
target_sources(RealTimePathTracing PRIVATE "${_blue_noise_binary}" "${_blue_noise_header}")
set_source_files_properties("${_blue_noise_binary}" PROPERTIES HEADER_FILE_ONLY TRUE)
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/Sampling/SpatiotemporalBlueNoise.cpp"
    PROPERTIES OBJECT_DEPENDS "${_blue_noise_header}")

target_link_libraries(RealTimePathTracing PRIVATE
    RtptPresentation RtptVulkan RtptPlatform
    fmt::fmt-header-only glm::glm tinygltf_headers stb
)

add_custom_command(TARGET RealTimePathTracing POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Content"
        "$<TARGET_FILE_DIR:RealTimePathTracing>/Content"
    VERBATIM
)

rtpt_compile_hlsl(
    TARGET  RealTimePathTracing
    SOURCE  "${CMAKE_CURRENT_LIST_DIR}/Shaders/NrdCompose.hlsl"
    ENTRY   main
    PROFILE cs_6_6
    SYMBOL  NrdCompose_hlsl
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/Shaders"
        "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes"
        "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Shaders"
        "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Shaders/Include"
    DEPENDS
        "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Shaders/NRD.hlsli"
        "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Shaders/Include/NRDConfig.hlsli"
)

rtpt_compile_hlsl(
    TARGET  RealTimePathTracing
    SOURCE  "${CMAKE_CURRENT_LIST_DIR}/Shaders/NrdDisocclusionMix.hlsl"
    ENTRY   main
    PROFILE cs_6_6
    SYMBOL  NrdDisocclusionMix_hlsl
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/Shaders"
        "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes"
        "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Shaders"
        "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Shaders/Include"
    DEPENDS
        "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Shaders/NRD.hlsli"
        "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Shaders/Include/NRDConfig.hlsli"
)

rtpt_compile_hlsl(
    TARGET  RealTimePathTracing
    SOURCE  "${CMAKE_CURRENT_LIST_DIR}/Shaders/Tonemap.hlsl"
    ENTRY   main
    PROFILE cs_6_6
    SYMBOL  Tonemap_hlsl
)

rtpt_compile_hlsl(
    TARGET  RealTimePathTracing
    SOURCE  "${CMAKE_CURRENT_LIST_DIR}/Shaders/Sky.hlsl"
    ENTRY   main
    PROFILE cs_6_6
    SYMBOL  Sky_hlsl
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes"
    DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/Common/ShaderTypes.h"
        "${CMAKE_CURRENT_LIST_DIR}/Common/SkyIo.h"
        "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes/Sky.hlsli"
)

set(_raster_hlsl_dependencies
    "${CMAKE_CURRENT_LIST_DIR}/Common/IoGltf.h"
    "${CMAKE_CURRENT_LIST_DIR}/Common/ShaderTypes.h"
    "${CMAKE_CURRENT_LIST_DIR}/Common/SkyIo.h"
    "${CMAKE_CURRENT_LIST_DIR}/Shaders/ShaderIo.h"
    "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes/Camera.hlsli"
    "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes/Math.hlsli"
    "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes/SceneAccess.hlsli"
    "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes/Sky.hlsli"
)

rtpt_compile_hlsl(
    TARGET  RealTimePathTracing
    SOURCE  "${CMAKE_CURRENT_LIST_DIR}/Shaders/Rasterizer.hlsl"
    ENTRY   vertexMain
    PROFILE vs_6_6
    SYMBOL  Rasterizer_vertex_hlsl
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes"
    DEPENDS ${_raster_hlsl_dependencies}
)

file(GLOB_RECURSE _path_tracing_hlsl_dependencies CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/Common/*.h"
    "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes/*.hlsli"
    "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes/PathTracing/*.hlsli"
)

rtpt_compile_hlsl(
    TARGET  RealTimePathTracing
    SOURCE  "${CMAKE_CURRENT_LIST_DIR}/Shaders/PathTracer.hlsl"
    PROFILE lib_6_6
    SYMBOL  PathTracer_hlsl
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/Shaders"
        "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes"
        "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Shaders"
        "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Shaders/Include"
    DEPENDS
        ${_path_tracing_hlsl_dependencies}
        "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Shaders/NRD.hlsli"
        "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Shaders/Include/NRDConfig.hlsli"
)

set(_restir_pt_hlsl_include_dirs
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/Shaders"
    "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes"
    "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Shaders"
    "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Shaders/Include"
)

set(_restir_pt_hlsl_dependencies
    ${_path_tracing_hlsl_dependencies}
    "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Shaders/NRD.hlsli"
    "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Shaders/Include/NRDConfig.hlsli"
)

foreach(_restir_pt_ray_module IN ITEMS
        InitialSampling
        TemporalResampling
        SpatialResampling
        SpatialPrepass)
    rtpt_compile_hlsl(
        TARGET  RealTimePathTracing
        SOURCE  "${CMAKE_CURRENT_LIST_DIR}/Shaders/ReSTIRPT/ReSTIRPT${_restir_pt_ray_module}.hlsl"
        PROFILE lib_6_6
        SYMBOL  "ReSTIRPT${_restir_pt_ray_module}_hlsl"
        INCLUDE_DIRS ${_restir_pt_hlsl_include_dirs}
        DEPENDS ${_restir_pt_hlsl_dependencies}
    )
endforeach()

foreach(_restir_pt_compute_module IN ITEMS
        FinalShading
        DuplicationMap
        LightTiles
        PrepassClassify
        PrepassOffsets)
    rtpt_compile_hlsl(
        TARGET  RealTimePathTracing
        SOURCE  "${CMAKE_CURRENT_LIST_DIR}/Shaders/ReSTIRPT/ReSTIRPT${_restir_pt_compute_module}.hlsl"
        ENTRY   main
        PROFILE cs_6_6
        SYMBOL  "ReSTIRPT${_restir_pt_compute_module}_hlsl"
        INCLUDE_DIRS ${_restir_pt_hlsl_include_dirs}
        DEPENDS ${_restir_pt_hlsl_dependencies}
    )
endforeach()

rtpt_compile_hlsl(
    TARGET  RealTimePathTracing
    SOURCE  "${CMAKE_CURRENT_LIST_DIR}/Shaders/Rasterizer.hlsl"
    ENTRY   fragmentMain
    PROFILE ps_6_6
    SYMBOL  Rasterizer_fragment_hlsl
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes"
    DEPENDS ${_raster_hlsl_dependencies}
)

if(NOT TARGET NRD)
    message(FATAL_ERROR "NRD is required by the renderer denoising path, but the NRD target was not created.")
endif()

target_link_libraries(RealTimePathTracing PRIVATE NRD)
target_include_directories(RealTimePathTracing PRIVATE
    "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Include"
    "${REAL_TIME_PATH_TRACING_NRD_SOURCE_DIR}/Integration"
)

if(BUILD_TESTING)
    add_executable(RtptImageDataTests
        "${CMAKE_SOURCE_DIR}/Tests/Cpu/ImageDataTests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/Assets/ImageData.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/Assets/ImageData.h"
    )
    target_include_directories(RtptImageDataTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_link_libraries(RtptImageDataTests PRIVATE stb)
    add_test(NAME ImageData COMMAND RtptImageDataTests)
    set_tests_properties(ImageData PROPERTIES LABELS "cpu")

    add_executable(RtptCameraTests
        "${CMAKE_SOURCE_DIR}/Tests/Cpu/CameraTests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/Camera/CameraController.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/Camera/CameraController.h"
        "${CMAKE_CURRENT_LIST_DIR}/Camera/CameraState.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/Camera/CameraState.h"
    )
    target_include_directories(RtptCameraTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_link_libraries(RtptCameraTests PRIVATE glm)
    add_test(NAME CameraParity COMMAND RtptCameraTests)
    set_tests_properties(CameraParity PROPERTIES LABELS "cpu")

    add_executable(RtptApplicationOptionsTests
        "${CMAKE_SOURCE_DIR}/Tests/Cpu/ApplicationOptionsTests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/ApplicationOptions.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/ApplicationOptions.h"
    )
    target_include_directories(RtptApplicationOptionsTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    add_test(NAME ApplicationOptions COMMAND RtptApplicationOptionsTests)
    set_tests_properties(ApplicationOptions PROPERTIES LABELS "cpu")

    add_executable(RtptLightSamplingTests
        "${CMAKE_SOURCE_DIR}/Tests/Cpu/LightSamplingTests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/Scene/LightSampling.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/Scene/LightSampling.h"
    )
    target_include_directories(RtptLightSamplingTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_link_libraries(RtptLightSamplingTests PRIVATE glm)
    add_test(NAME LightSampling COMMAND RtptLightSamplingTests)
    set_tests_properties(LightSampling PROPERTIES LABELS "cpu")

    add_executable(RtptResolveHistoryTests
        "${CMAKE_SOURCE_DIR}/Tests/Cpu/ResolveHistoryTests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/PathTracing/Common/ResolveHistoryTracker.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/PathTracing/Common/ResolveHistoryTracker.h"
    )
    target_include_directories(RtptResolveHistoryTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes")
    target_link_libraries(RtptResolveHistoryTests PRIVATE glm Vulkan::Headers)
    add_test(NAME ResolveHistory COMMAND RtptResolveHistoryTests)
    set_tests_properties(ResolveHistory PROPERTIES LABELS "cpu")

    add_executable(RtptFrameTimingStatisticsTests
        "${CMAKE_SOURCE_DIR}/Tests/Cpu/FrameTimingStatisticsTests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/Rendering/FrameTimingStatistics.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/Rendering/FrameTimingStatistics.h"
    )
    target_include_directories(RtptFrameTimingStatisticsTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    add_test(NAME FrameTimingStatistics COMMAND RtptFrameTimingStatisticsTests)
    set_tests_properties(FrameTimingStatistics PROPERTIES LABELS "cpu")

    add_executable(RtptVulkanBootstrapTests
        "${CMAKE_SOURCE_DIR}/Tests/Framework/VulkanBootstrapTests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/Rendering/ViewportTargets.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/Rendering/ViewportTargets.h"
        "${CMAKE_CURRENT_LIST_DIR}/Rendering/RenderTargetView.h"
    )
    target_include_directories(RtptVulkanBootstrapTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_link_libraries(RtptVulkanBootstrapTests PRIVATE RtptVulkan)
    rtpt_compile_hlsl(
        TARGET  RtptVulkanBootstrapTests
        SOURCE  "${CMAKE_SOURCE_DIR}/Tests/Shaders/RayTracingProbe.hlsl"
        PROFILE lib_6_6
        SYMBOL  RayTracingProbe_hlsl
    )
    add_test(NAME VulkanBootstrap COMMAND RtptVulkanBootstrapTests)

    add_executable(RtptPathSamplingTests
        "${CMAKE_SOURCE_DIR}/Tests/Framework/PathSamplingTests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/Sampling/SpatiotemporalBlueNoise.cpp"
        "${_blue_noise_header}"
    )
    target_include_directories(RtptPathSamplingTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes" "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(RtptPathSamplingTests PRIVATE RtptVulkan glm::glm)
    rtpt_compile_hlsl(
        TARGET RtptPathSamplingTests
        SOURCE "${CMAKE_SOURCE_DIR}/Tests/Shaders/PathSamplingProbe.hlsl"
        ENTRY main
        PROFILE cs_6_6
        SYMBOL PathSamplingProbe_hlsl
        INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/Shaders" "${CMAKE_CURRENT_LIST_DIR}/ShaderIncludes"
        DEPENDS ${_path_tracing_hlsl_dependencies}
    )
    add_test(NAME PathSampling COMMAND RtptPathSamplingTests)
    set_tests_properties(PathSampling PROPERTIES LABELS "gpu;sampling")

    add_executable(RtptPresentationTests "${CMAKE_SOURCE_DIR}/Tests/Framework/PresentationTests.cpp")
    target_link_libraries(RtptPresentationTests PRIVATE RtptPresentation RtptVulkan RtptPlatform)
    add_test(NAME Presentation COMMAND RtptPresentationTests)
    set_tests_properties(Presentation PROPERTIES LABELS "gpu;presentation")
    set(RTPT_VULKAN_VALIDATION_BIN "" CACHE PATH "Directory containing the Vulkan validation layer manifest and library")
    if(WIN32 AND RTPT_VULKAN_VALIDATION_BIN)
        set_tests_properties(VulkanBootstrap Presentation PathSampling PROPERTIES
            ENVIRONMENT "VK_LAYER_PATH=${RTPT_VULKAN_VALIDATION_BIN}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${RTPT_VULKAN_VALIDATION_BIN}"
        )
    endif()
endif()

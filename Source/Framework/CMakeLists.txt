add_library(RtptPlatform STATIC
    Platform/InputState.h
    Platform/Log.cpp
    Platform/Log.h
    Platform/Paths.cpp
    Platform/Paths.h
    Platform/Window.cpp
    Platform/Window.h
)
target_include_directories(RtptPlatform PUBLIC "${CMAKE_SOURCE_DIR}/Source")
target_link_libraries(RtptPlatform PUBLIC glfw volk::volk)
target_compile_definitions(RtptPlatform PUBLIC VK_NO_PROTOTYPES)
target_compile_definitions(RtptPlatform PRIVATE RTPT_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
set_property(TARGET RtptPlatform PROPERTY FOLDER "RealTimePathTracing/Framework")

add_library(RtptVulkan STATIC
    Vulkan/AccelerationStructures.cpp
    Vulkan/AccelerationStructures.h
    Vulkan/Barriers.cpp
    Vulkan/Barriers.h
    Vulkan/Descriptors.cpp
    Vulkan/Descriptors.h
    Vulkan/DeviceRequirements.cpp
    Vulkan/DeviceRequirements.h
    Vulkan/Diagnostics.cpp
    Vulkan/Diagnostics.h
    Vulkan/FrameContext.h
    Vulkan/GpuExecution.cpp
    Vulkan/GpuExecution.h
    Vulkan/GpuProfiler.cpp
    Vulkan/GpuProfiler.h
    Vulkan/GpuResources.cpp
    Vulkan/GpuResources.h
    Vulkan/Pipelines.cpp
    Vulkan/Pipelines.h
    Vulkan/PresentationSurface.cpp
    Vulkan/PresentationSurface.h
    Vulkan/ReadbackContext.cpp
    Vulkan/ReadbackContext.h
    Vulkan/ShaderBindingTable.cpp
    Vulkan/ShaderBindingTable.h
    Vulkan/Swapchain.cpp
    Vulkan/Swapchain.h
    Vulkan/UploadContext.cpp
    Vulkan/UploadContext.h
    Vulkan/VulkanDevice.cpp
    Vulkan/VulkanDevice.h
    Vulkan/VulkanInstance.cpp
    Vulkan/VulkanInstance.h
)
target_include_directories(RtptVulkan PUBLIC "${CMAKE_SOURCE_DIR}/Source")
target_link_libraries(RtptVulkan PUBLIC RtptPlatform GPUOpen::VulkanMemoryAllocator)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(RtptVulkan PRIVATE -Wno-nullability-completeness)
endif()
set_property(TARGET RtptVulkan PROPERTY FOLDER "RealTimePathTracing/Framework")

add_library(RtptPresentation STATIC
    Presentation/UiControls.h
    Presentation/UiRenderer.cpp
    Presentation/UiRenderer.h
    Presentation/WindowTitle.cpp
    Presentation/WindowTitle.h
)
target_include_directories(RtptPresentation PUBLIC "${CMAKE_SOURCE_DIR}/Source")
target_link_libraries(RtptPresentation PUBLIC RtptVulkan RtptPlatform imgui)
set_property(TARGET RtptPresentation PROPERTY FOLDER "RealTimePathTracing/Framework")

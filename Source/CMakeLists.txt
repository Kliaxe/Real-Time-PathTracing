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
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneCatalog.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneCatalog.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneAssetCatalog.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneAssetCatalog.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneBuilder.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneBuilder.h"
    "${CMAKE_CURRENT_LIST_DIR}/Scene/SceneTypes.h"
)


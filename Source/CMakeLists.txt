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


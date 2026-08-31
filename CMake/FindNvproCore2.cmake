# FindNvproCore2.cmake
#
# This is adapted from vk_raytracing_tutorial_KHR.
# Behavior:
# - Prefer a local nvpro_core2 checkout if present.
# - Otherwise (default) clone nvpro_core2 into the *build tree* under ${CMAKE_BINARY_DIR}/_deps/nvpro_core2.
#

if(NOT NvproCore2_FOUND)
    set(_PrintInfo TRUE)
endif()

# ----------------------------------------------------------------------------------------------------------------------
# Try to find a local installation first
#

find_path(NvproCore2_ROOT
    NAMES nvpro_core2/cmake/Setup.cmake
    PATHS
        "${CMAKE_BINARY_DIR}/_deps"
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/.."
        "${CMAKE_SOURCE_DIR}/../.."
)

if(NvproCore2_ROOT)
    set(NvproCore2_FOUND TRUE)
else()
    # ------------------------------------------------------------------------------------------------------------------
    # Option to allow downloading if not found
    #

    option(NVPROCORE2_DOWNLOAD "Download nvpro_core2 if not found" ON)

    if(NVPROCORE2_DOWNLOAD)
        include(FetchContent)

        # Pin the dependency to the revision validated with this renderer and Vulkan SDK.
        set(NVPRO_GIT_TAG "71e33ddc6920fe7ebdb3d3d7f4cd44b5b025c54e" CACHE STRING "Pinned nvpro_core2 revision" FORCE)

        # Try to determine nvpro_core2 location from the repo origin.
        execute_process(
            COMMAND git config --get remote.origin.url
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE GIT_REPO_URL
            RESULT_VARIABLE GIT_RESULT
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        if(NOT GIT_RESULT EQUAL 0)
            message(WARNING "Failed to get git remote URL. Defaulting to GitHub URL.")
            set(GIT_REPO_URL "https://github.com/nvpro-samples/nvpro_core2.git")
            set(GIT_BASE_URL "https://github.com")
            set(NVPRO_GIT_URL "${GIT_REPO_URL}")
        else()
            string(FIND "${GIT_REPO_URL}" "github.com" FOUND_INDEX)
            if(FOUND_INDEX GREATER -1)
                string(REGEX MATCH ".*github\\.com" GIT_BASE_URL "${GIT_REPO_URL}")
                if(NOT GIT_BASE_URL)
                    message(FATAL_ERROR "Failed to extract GitHub base URL from ${GIT_REPO_URL}")
                endif()

                string(FIND "${GIT_REPO_URL}" "git@" SSH_FOUND_INDEX)
                if(SSH_FOUND_INDEX GREATER -1)
                    set(NVPRO_GIT_URL "${GIT_BASE_URL}:nvpro-samples/nvpro_core2.git")
                else()
                    set(NVPRO_GIT_URL "${GIT_BASE_URL}/nvpro-samples/nvpro_core2.git")
                endif()

                message(STATUS "Using GitHub nvpro_core2 repository")
            else()
                # Internal repository - reconstruct base preserving protocol.
                string(REGEX MATCH "^[^/]+//[^/]+/" GIT_BASE_URL "${GIT_REPO_URL}")
                if(NOT GIT_BASE_URL)
                    message(FATAL_ERROR "Failed to extract base URL from ${GIT_REPO_URL}")
                endif()

                set(NVPRO_GIT_URL "${GIT_BASE_URL}devtechproviz/nvpro-samples/nvpro_core2.git")
                message(STATUS "Using internal nvpro_core2 repository")
            endif()
        endif()

        if(NOT NVPRO_GIT_URL)
            message(FATAL_ERROR "Failed to construct git URL for nvpro_core2")
        endif()

        message(STATUS "Will clone from: ${NVPRO_GIT_URL} (branch/tag: ${NVPRO_GIT_TAG})")

        # Fetch just the pinned revision so a clean build does not follow upstream main.
        execute_process(
            COMMAND git init "${CMAKE_BINARY_DIR}/_deps/nvpro_core2"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            RESULT_VARIABLE _nvpro_git_result
        )
        if(NOT _nvpro_git_result EQUAL 0)
            message(FATAL_ERROR "Failed to initialize the nvpro_core2 checkout.")
        endif()

        execute_process(
            COMMAND git -C "${CMAKE_BINARY_DIR}/_deps/nvpro_core2" remote add origin "${NVPRO_GIT_URL}"
            RESULT_VARIABLE _nvpro_git_result
        )
        if(NOT _nvpro_git_result EQUAL 0)
            message(FATAL_ERROR "Failed to configure the nvpro_core2 remote.")
        endif()

        execute_process(
            COMMAND git -C "${CMAKE_BINARY_DIR}/_deps/nvpro_core2" fetch --depth 1 origin "${NVPRO_GIT_TAG}"
            RESULT_VARIABLE _nvpro_git_result
        )
        if(NOT _nvpro_git_result EQUAL 0)
            message(FATAL_ERROR "Failed to fetch the pinned nvpro_core2 revision: ${NVPRO_GIT_TAG}")
        endif()

        execute_process(
            COMMAND git -C "${CMAKE_BINARY_DIR}/_deps/nvpro_core2" checkout --detach FETCH_HEAD
            RESULT_VARIABLE _nvpro_git_result
        )
        if(NOT _nvpro_git_result EQUAL 0)
            message(FATAL_ERROR "Failed to check out the pinned nvpro_core2 revision: ${NVPRO_GIT_TAG}")
        endif()

        # LLVM MinGW's libc++ does not implement std::execution policies.
        # Remove this patch when the pinned nvpro_core2 revision no longer uses them here.
        execute_process(
            COMMAND git -C "${CMAKE_BINARY_DIR}/_deps/nvpro_core2" apply --whitespace=nowarn
                "${CMAKE_SOURCE_DIR}/CMake/Patches/nvpro-core2-use-thread-pool.patch"
            RESULT_VARIABLE _nvpro_git_result
        )
        if(NOT _nvpro_git_result EQUAL 0)
            message(FATAL_ERROR "Failed to apply the nvpro_core2 LLVM MinGW compatibility patch.")
        endif()

        # Retry find after cloning
        find_path(NvproCore2_ROOT
            NAMES nvpro_core2/cmake/Setup.cmake
            PATHS
                "${CMAKE_BINARY_DIR}/_deps"
                "${CMAKE_SOURCE_DIR}"
                "${CMAKE_SOURCE_DIR}/.."
                "${CMAKE_SOURCE_DIR}/../.."
        )

        if(NvproCore2_ROOT)
            set(NvproCore2_FOUND TRUE)
        endif()
    endif()
endif()

if(_PrintInfo)
    message(STATUS "Found nvpro_core2 at: ${NvproCore2_ROOT}")
endif()

if(NvproCore2_FOUND)
    # Setup.cmake creates the nvpro2::* targets (nvvk, nvapp, nvslang, ...)
    include("${NvproCore2_ROOT}/nvpro_core2/cmake/Setup.cmake")
endif()

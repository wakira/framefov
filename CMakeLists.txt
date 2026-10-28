cmake_minimum_required(VERSION 3.16)

project(framefov VERSION 0.1.0 LANGUAGES CXX)

# framefov: a standalone Vulkan layer that injects fragment-shading-rate
# (foveated) rendering into an existing pipeline chain. Steam Frame variant:
# Vulkan only, no OpenXR, fixed foveation (eye tracking is a later milestone).

option(FRAMEFOV_BUILD_TESTS "Build the unit tests" ON)
option(FRAMEFOV_BUILD_GPU_TESTS "Build the GPU end-to-end test app (upstream test assumes RADV)" OFF)
option(FRAMEFOV_USE_SYSTEM_HEADERS "Use system Vulkan headers instead of FetchContent" OFF)
# The Nix dev shell exports this, so `cmake -B build` works there as documented.
if(DEFINED ENV{FRAMEFOV_USE_SYSTEM_HEADERS} AND NOT FRAMEFOV_USE_SYSTEM_HEADERS)
  set(FRAMEFOV_USE_SYSTEM_HEADERS $ENV{FRAMEFOV_USE_SYSTEM_HEADERS} CACHE BOOL "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies (headers only)
# ---------------------------------------------------------------------------
if(FRAMEFOV_USE_SYSTEM_HEADERS)
  find_path(FRAMEFOV_VULKAN_INCLUDE_DIR vulkan/vulkan.h
    HINTS ENV FRAMEFOV_VULKAN_INCLUDE_DIR PATH_SUFFIXES include)
  if(NOT FRAMEFOV_VULKAN_INCLUDE_DIR)
    message(FATAL_ERROR "vulkan/vulkan.h not found (set FRAMEFOV_VULKAN_INCLUDE_DIR)")
  endif()
  find_file(FRAMEFOV_VK_LAYER_HEADER vulkan/vk_layer.h HINTS ${FRAMEFOV_VULKAN_INCLUDE_DIR} NO_DEFAULT_PATH)
  if(NOT FRAMEFOV_VK_LAYER_HEADER)
    message(FATAL_ERROR "vulkan/vk_layer.h not found in ${FRAMEFOV_VULKAN_INCLUDE_DIR}")
  endif()
  # Optional: the SteamVR-native gaze source. Only the header is needed;
  # libopenvr_api.so is loaded at runtime with dlopen.
  find_path(FRAMEFOV_OPENVR_INCLUDE_DIR openvr/openvr.h
    HINTS ENV FRAMEFOV_OPENVR_INCLUDE_DIR PATH_SUFFIXES include)
else()
  include(FetchContent)
  # SOURCE_SUBDIR points at a directory without a CMakeLists.txt on purpose: we
  # only want the headers, never to build the loader / test suites.
  FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.3.280
    SOURCE_SUBDIR header-only)
  FetchContent_MakeAvailable(vulkan_headers)
  set(FRAMEFOV_VULKAN_INCLUDE_DIR ${vulkan_headers_SOURCE_DIR}/include)
endif()

if(NOT FRAMEFOV_OPENVR_INCLUDE_DIR)
  # Single header, pinned; downloaded rather than fetched as a project because we
  # only need the declarations (the library is resolved at runtime).
  set(FRAMEFOV_OPENVR_HEADER_DIR ${CMAKE_BINARY_DIR}/third_party/openvr)
  set(FRAMEFOV_OPENVR_HEADER ${FRAMEFOV_OPENVR_HEADER_DIR}/openvr/openvr.h)
  if(NOT EXISTS ${FRAMEFOV_OPENVR_HEADER})
    file(MAKE_DIRECTORY ${FRAMEFOV_OPENVR_HEADER_DIR}/openvr)
    file(DOWNLOAD
      https://raw.githubusercontent.com/ValveSoftware/openvr/v2.15.6/headers/openvr.h
      ${FRAMEFOV_OPENVR_HEADER} STATUS FRAMEFOV_OPENVR_DOWNLOAD)
    list(GET FRAMEFOV_OPENVR_DOWNLOAD 0 FRAMEFOV_OPENVR_DOWNLOAD_CODE)
    if(NOT FRAMEFOV_OPENVR_DOWNLOAD_CODE EQUAL 0)
      message(WARNING "could not download openvr.h; the OpenVR gaze source will be unavailable")
    endif()
  endif()
  if(EXISTS ${FRAMEFOV_OPENVR_HEADER})
    set(FRAMEFOV_OPENVR_INCLUDE_DIR ${FRAMEFOV_OPENVR_HEADER_DIR})
  endif()
endif()

# ---------------------------------------------------------------------------
# The layer
# ---------------------------------------------------------------------------
set(FRAMEFOV_COMMON_SOURCES
  src/common/log.cpp
  src/common/config.cpp
  src/common/shared_state.cpp
  src/common/pattern.cpp
  src/common/classify.cpp
  src/common/gaze_projection.cpp
  src/common/renderpass_convert.cpp
  src/common/spirv_scan.cpp)

set(FRAMEFOV_VULKAN_SOURCES
  src/vulkan/vk_layer.cpp
  src/vulkan/vk_dispatch.cpp
  src/vulkan/vk_device.cpp
  src/vulkan/vk_resources.cpp
  src/vulkan/vk_renderpass.cpp
  src/vulkan/vk_framebuffer.cpp
  src/vulkan/vk_pipeline.cpp
  src/vulkan/vk_commands.cpp)

# Out-of-process gaze publisher: the only component that talks to SteamVR.
add_executable(framefov-gaze
  src/gaze_publisher/main.cpp
  src/common/openvr_gaze.cpp
  src/common/log.cpp)
target_include_directories(framefov-gaze PRIVATE
  ${FRAMEFOV_OPENVR_INCLUDE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(framefov-gaze PRIVATE -Wall -Wextra)
target_link_libraries(framefov-gaze PRIVATE dl pthread)

add_library(framefov SHARED
  ${FRAMEFOV_COMMON_SOURCES}
  ${FRAMEFOV_VULKAN_SOURCES})

# The loader looks for libVkLayer_<name>.so per the manifest.
set_target_properties(framefov PROPERTIES OUTPUT_NAME VkLayer_framefov)
target_compile_features(framefov PRIVATE cxx_std_17)
set_target_properties(framefov PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON)

target_include_directories(framefov PRIVATE
  ${FRAMEFOV_OPENVR_INCLUDE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${FRAMEFOV_VULKAN_INCLUDE_DIR})

target_compile_options(framefov PRIVATE -Wall -Wextra)

target_link_options(framefov PRIVATE
  -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map
  -Wl,-Bsymbolic
  -Wl,--no-undefined)

target_link_libraries(framefov PRIVATE dl pthread)

foreach(src IN LISTS FRAMEFOV_VULKAN_SOURCES)
  set_source_files_properties(${src} PROPERTIES COMPILE_DEFINITIONS "FRAMEFOV_LOG_TAG=\"vk\"")
endforeach()

# ---------------------------------------------------------------------------
# Manifest / install
# ---------------------------------------------------------------------------
# For local testing the library path is the build directory path; scripts/install.sh
# rewrites the manifest with the real install prefix.
set(FRAMEFOV_LIBRARY_PATH ${CMAKE_BINARY_DIR}/libVkLayer_framefov.so)
configure_file(manifests/VkLayer_framefov.json.in ${CMAKE_BINARY_DIR}/VkLayer_framefov.json @ONLY)

include(GNUInstallDirs)
install(TARGETS framefov LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/framefov)
install(TARGETS framefov-gaze RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(FRAMEFOV_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "framefov: Vulkan headers: ${FRAMEFOV_VULKAN_INCLUDE_DIR}")
message(STATUS "framefov: OpenVR headers: ${FRAMEFOV_OPENVR_INCLUDE_DIR}")

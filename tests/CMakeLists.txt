# Unit tests: pure logic only, no Vulkan/OpenXR calls.
set(FRAMEFOV_TEST_COMMON_SOURCES
  ${CMAKE_SOURCE_DIR}/src/common/config.cpp
  ${CMAKE_SOURCE_DIR}/src/common/log.cpp
  ${CMAKE_SOURCE_DIR}/src/common/shared_state.cpp
  ${CMAKE_SOURCE_DIR}/src/common/pattern.cpp
  ${CMAKE_SOURCE_DIR}/src/common/classify.cpp
  ${CMAKE_SOURCE_DIR}/src/common/gaze_projection.cpp
  ${CMAKE_SOURCE_DIR}/src/common/overlay_texture.cpp
  ${CMAKE_SOURCE_DIR}/src/common/renderpass_convert.cpp
  ${CMAKE_SOURCE_DIR}/src/common/spirv_scan.cpp)

set(FRAMEFOV_UNIT_SOURCES
  unit/test_main.cpp
  unit/test_config.cpp
  unit/test_pattern.cpp
  unit/test_classify.cpp
  unit/test_gaze_projection.cpp
  unit/test_gaze_math.cpp
  unit/test_overlay_texture.cpp
  unit/test_renderpass_convert.cpp
  unit/test_spirv_scan.cpp)

add_executable(framefov_unit_tests ${FRAMEFOV_UNIT_SOURCES} ${FRAMEFOV_TEST_COMMON_SOURCES})
target_include_directories(framefov_unit_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${FRAMEFOV_VULKAN_INCLUDE_DIR})
target_compile_options(framefov_unit_tests PRIVATE -Wall -Wextra)

add_test(NAME unit COMMAND framefov_unit_tests)
set_tests_properties(unit PROPERTIES
  ENVIRONMENT "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-state;XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config")

# ---------------------------------------------------------------------------
# GPU end-to-end test: renders with the layer injected and checks the rate map.
# ---------------------------------------------------------------------------
if(FRAMEFOV_BUILD_GPU_TESTS)
  add_executable(vk_vrs_test vk_vrs_test/main.cpp)
  target_include_directories(vk_vrs_test PRIVATE
    ${FRAMEFOV_VULKAN_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/vk_vrs_test)
  target_compile_options(vk_vrs_test PRIVATE -Wall -Wextra)

  # The test app (not the layer!) links the Vulkan loader.
  find_package(PkgConfig QUIET)
  set(FRAMEFOV_LOADER_FOUND FALSE)
  if(PkgConfig_FOUND)
    pkg_check_modules(VULKAN_LOADER QUIET vulkan)
  endif()
  if(VULKAN_LOADER_FOUND)
    target_link_libraries(vk_vrs_test PRIVATE ${VULKAN_LOADER_LIBRARIES})
    target_include_directories(vk_vrs_test PRIVATE ${VULKAN_LOADER_INCLUDE_DIRS})
    target_link_directories(vk_vrs_test PRIVATE ${VULKAN_LOADER_LIBRARY_DIRS})
    set(FRAMEFOV_LOADER_FOUND TRUE)
  else()
    find_library(FRAMEFOV_VULKAN_LIBRARY NAMES vulkan vulkan-1)
    if(FRAMEFOV_VULKAN_LIBRARY)
      target_link_libraries(vk_vrs_test PRIVATE ${FRAMEFOV_VULKAN_LIBRARY})
      set(FRAMEFOV_LOADER_FOUND TRUE)
    endif()
  endif()
  if(NOT FRAMEFOV_LOADER_FOUND)
    message(WARNING "Vulkan loader not found; the GPU tests will not run")
  endif()

  # ---------------------------------------------------------------------------
  # Loader-protocol probe for the Vulkan layer: a layer must provide
  # vkCreateInstance for any instance argument, or the loader drops it and fails
  # instance creation (the Proton crash).
  # ---------------------------------------------------------------------------
  add_executable(vk_layer_probe vk_layer_probe/main.cpp)
  target_include_directories(vk_layer_probe PRIVATE ${FRAMEFOV_VULKAN_INCLUDE_DIR})
  target_compile_options(vk_layer_probe PRIVATE -Wall -Wextra)
  target_link_libraries(vk_layer_probe PRIVATE dl)
  add_test(NAME vk_layer_probe COMMAND vk_layer_probe ${CMAKE_BINARY_DIR}/libVkLayer_framefov.so)

  # Layer loading is opt-in and configured through the environment. The layer
  # must be findable by the loader: either installed (scripts/install-frame.sh) or via
  # VK_LAYER_PATH pointing at the build directory.
  set(FRAMEFOV_GPU_TEST_ENV
    "VK_LAYER_PATH=${CMAKE_BINARY_DIR}"
    "VK_INSTANCE_LAYERS=VK_LAYER_FRAMEFOV_foveation"
    "FRAMEFOV_ENABLE=1"
    "FRAMEFOV_FORCE_ACTIVE=1"
    "FRAMEFOV_FAKE_EYE=1024x1024"
    "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-state")
  foreach(mode dynamic rp1 rp2 multiview imageless nonEyeSize)
    add_test(NAME vk_vrs_test_${mode} COMMAND vk_vrs_test ${mode})
    set_tests_properties(vk_vrs_test_${mode} PROPERTIES ENVIRONMENT "${FRAMEFOV_GPU_TEST_ENV}")
  endforeach()

  # Standalone fixed foveation for apps without OpenXR (OpenVR games, plain
  # Vulkan apps): no FORCE_ACTIVE, no FAKE_EYE, just the config.
  set(FRAMEFOV_STANDALONE_ENV
    "VK_LAYER_PATH=${CMAKE_BINARY_DIR}"
    "VK_INSTANCE_LAYERS=VK_LAYER_FRAMEFOV_foveation"
    "FRAMEFOV_ENABLE=1"
    "FRAMEFOV_STANDALONE=1"
    "FRAMEFOV_EYE_SIZE=1024x1024"
    "FRAMEFOV_FIXED_INNER_RADIUS=0.25"
    "FRAMEFOV_FIXED_OUTER_RADIUS=0.45"
    "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-state"
    "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config")

  add_test(NAME vk_vrs_test_standalone_env COMMAND vk_vrs_test dynamic)
  set_tests_properties(vk_vrs_test_standalone_env PROPERTIES ENVIRONMENT "${FRAMEFOV_STANDALONE_ENV}")

  # draw_radii: the debug circles must appear in the rate map as a rate that is
  # neither the fovea rate nor the outer rate (the same path that puts the
  # circles in a game's eye image).
  set(FRAMEFOV_STANDALONE_RING_ENV ${FRAMEFOV_STANDALONE_ENV}
    "FRAMEFOV_DRAW_RADII=both" "FRAMEFOV_RING_WIDTH=2" "FRAMEFOV_EXPECT_RING=1")
  add_test(NAME vk_vrs_test_draw_radii COMMAND vk_vrs_test dynamic)
  set_tests_properties(vk_vrs_test_draw_radii PROPERTIES ENVIRONMENT "${FRAMEFOV_STANDALONE_RING_ENV}")

  # Same through the config file, in an [exe:vk_vrs_test] section, so the
  # per-executable sections and the Vulkan half's own config loading are covered.
  set(FRAMEFOV_STANDALONE_CONFIG_HOME ${CMAKE_BINARY_DIR}/test-config-standalone)
  file(MAKE_DIRECTORY ${FRAMEFOV_STANDALONE_CONFIG_HOME}/framefov)
  file(WRITE ${FRAMEFOV_STANDALONE_CONFIG_HOME}/framefov/settings.ini
    "; written by CMake for vk_vrs_test_standalone_file\n"
    "[global]\n"
    "enabled = 1\n"
    "[exe:vk_vrs_test]\n"
    "standalone = 1\n"
    "eye_size = 1024x1024\n"
    "fixed_inner_radius = 0.25\n"
    "fixed_outer_radius = 0.45\n")
  # Hot reload of the standalone config while a Vulkan-only app renders.
  add_test(NAME standalone_hot_reload
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/vk_vrs_test/standalone_hot_reload.sh)
  set_tests_properties(standalone_hot_reload PROPERTIES TIMEOUT 120)

  add_test(NAME vk_vrs_test_standalone_file COMMAND vk_vrs_test dynamic)
  set_tests_properties(vk_vrs_test_standalone_file PROPERTIES
    ENVIRONMENT
      "VK_LAYER_PATH=${CMAKE_BINARY_DIR};FRAMEFOV_ENABLE=1;XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-state;XDG_CONFIG_HOME=${FRAMEFOV_STANDALONE_CONFIG_HOME}")
endif()

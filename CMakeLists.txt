cmake_minimum_required(VERSION 3.16)

# Which application under examples/ to build.
#
# A CMake cache variable on purpose: the project CMakeLists is evaluated BEFORE
# sdkconfig is loaded, so CONFIG_* keys (and therefore a Kconfig `choice`) are
# not available here.
#
#   idf.py -DRING_APP=hello -B build/hello build
set(RING_APP "glyph-anime" CACHE STRING "Example app under examples/ to build")

set(_ring_app_dir "${CMAKE_CURRENT_LIST_DIR}/examples/${RING_APP}")
if(NOT EXISTS "${_ring_app_dir}/CMakeLists.txt")
    file(GLOB _ring_available RELATIVE "${CMAKE_CURRENT_LIST_DIR}/examples"
         "${CMAKE_CURRENT_LIST_DIR}/examples/*/CMakeLists.txt")
    string(REPLACE "/CMakeLists.txt" "" _ring_available "${_ring_available}")
    string(REPLACE ";" ", " _ring_available "${_ring_available}")
    message(FATAL_ERROR
        "RING_APP='${RING_APP}' is not an example: no "
        "examples/${RING_APP}/CMakeLists.txt.\n"
        "Available: ${_ring_available}")
endif()

# NOTE: examples are deliberately NOT added via EXTRA_COMPONENT_DIRS.
#
# In ESP-IDF, components from EXTRA_COMPONENT_DIRS are registered AFTER `main`,
# and __component_set_dependencies() silently drops any REQUIRES entry whose
# component target is not yet in __BUILD_COMPONENT_TARGETS. So `main` cannot
# list an example as a REQUIRES dependency — the header include path and the
# link would just quietly disappear. Instead, the selected example is a CMake
# fragment included by main/CMakeLists.txt and compiled into the `main`
# component. See examples/README.md.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smartring)

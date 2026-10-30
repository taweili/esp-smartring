# main is application-agnostic: it owns app_main() and pulls in whichever
# example under examples/ RING_APP selects.
#
# The example is a CMake *fragment*, not a component. It cannot be a component:
# EXTRA_COMPONENT_DIRS components are registered after `main`, and IDF silently
# drops REQUIRES entries pointing at not-yet-registered components, which would
# lose both the include path and the link. See the note in ../CMakeLists.txt.
#
# This file is evaluated TWICE by ESP-IDF:
#   1. the requirements-gathering pass (scripts/component_get_requirements.cmake),
#      a separate CMake run where cache variables are NOT set and CMAKE_SOURCE_DIR
#      points at the build directory, and
#   2. the real configure.
# So resolve the project root from CMAKE_CURRENT_LIST_DIR and fall back to the
# default app when RING_APP is unset — in pass (1) only REQUIRES is collected,
# so the fallback is harmless as long as the directory exists.

get_filename_component(_ring_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(_ring_app "${RING_APP}")
if(NOT _ring_app)
    set(_ring_app "glyph-anime")
endif()

set(_app_dir "${_ring_root}/examples/${_ring_app}")

set(RING_EXAMPLE_SRCS "")
set(RING_EXAMPLE_INCLUDE_DIRS "")
set(RING_EXAMPLE_REQUIRES "")
include("${_app_dir}/CMakeLists.txt")

set(_srcs "main.c")
set(_incs ".")
foreach(_s ${RING_EXAMPLE_SRCS})
    list(APPEND _srcs "${_app_dir}/${_s}")
endforeach()
foreach(_d ${RING_EXAMPLE_INCLUDE_DIRS})
    list(APPEND _incs "${_app_dir}/${_d}")
endforeach()

idf_component_register(
    SRCS ${_srcs}
    INCLUDE_DIRS ${_incs}
    REQUIRES ringkit ${RING_EXAMPLE_REQUIRES}
)

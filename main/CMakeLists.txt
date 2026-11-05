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

# Optional binary assets the selected example wants embedded (e.g. a test
# audio clip). Paths are relative to the example directory.
set(_ring_embed_files "")
foreach(_f ${RING_EXAMPLE_EMBED_FILES})
    list(APPEND _ring_embed_files "${_app_dir}/${_f}")
endforeach()

idf_component_register(
    SRCS ${_srcs}
    INCLUDE_DIRS ${_incs}
    REQUIRES ringkit ${RING_EXAMPLE_REQUIRES}
    EMBED_FILES ${_ring_embed_files}
)

# ---------------------------------------------------------------------------
# Local secrets (git-ignored): Wi-Fi credentials and the Qianwen API key.
#
# Preferred path: ./secrets.local (sourced by tools/dev.sh) is turned into a
# generated header, so the values never appear on a command line or in
# CMakeCache.txt — passing them as -D puts secrets into build/log/ and the
# cache, where they persist in plaintext.
#
# The legacy -DQIANWEN_API_KEY=... form still works; it is only honoured when
# secrets.local is absent.
# ---------------------------------------------------------------------------
# Written into the build's config directory: every component already has it on
# its include path, so ringkit (a dependency of main) sees the same values.
set(_ring_secrets_header "${CMAKE_BINARY_DIR}/config/ring_secrets.h")

file(WRITE "${_ring_secrets_header}"
     "/* Generated from the environment / secrets.local — do not commit. */\n"
     "#pragma once\n")

foreach(_pair
        "QIANWEN_API_KEY:QIANWEN_API_KEY"
        "RING_WIFI_DEFAULT_SSID:RING_WIFI_DEFAULT_SSID"
        "RING_WIFI_DEFAULT_PASSWORD:RING_WIFI_DEFAULT_PASSWORD")
    string(REPLACE ":" ";" _kv "${_pair}")
    list(GET _kv 0 _env)
    list(GET _kv 1 _macro)
    if(DEFINED ENV{${_env}} AND NOT "$ENV{${_env}}" STREQUAL "")
        set(_val "$ENV{${_env}}")
    elseif(DEFINED ${_env} AND NOT "${${_env}}" STREQUAL "")
        set(_val "${${_env}}")     # legacy -D form
    else()
        continue()
    endif()
    file(APPEND "${_ring_secrets_header}"
         "#ifndef ${_macro}\n#define ${_macro} \"${_val}\"\n#endif\n")
endforeach()

target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_BINARY_DIR}/config")

# 14_geodis_push_to_talk — application fragment (NOT a component; see examples/README.md).
#
# main/CMakeLists.txt includes this file and adds the sources to the `main`
# component, with "${_app_dir}" as the base for every path below.

set(RING_EXAMPLE_SRCS         "geodis_push_to_talk.c")
set(RING_EXAMPLE_INCLUDE_DIRS "include")
set(RING_EXAMPLE_REQUIRES     "")

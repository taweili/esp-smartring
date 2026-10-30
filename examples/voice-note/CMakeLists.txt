# voice-note — application fragment (NOT a component; see examples/README.md).
#
# main/CMakeLists.txt includes this file and adds the sources to the `main`
# component, with "${_app_dir}" as the base for every path below.
#
# `heap` is the ESP-IDF heap component: the example allocates the ~1.5 MB PCM
# capture buffer explicitly from PSRAM with heap_caps_malloc(MALLOC_CAP_SPIRAM).

set(RING_EXAMPLE_SRCS         "voice_note.c")
set(RING_EXAMPLE_INCLUDE_DIRS "include")
set(RING_EXAMPLE_REQUIRES     "heap")

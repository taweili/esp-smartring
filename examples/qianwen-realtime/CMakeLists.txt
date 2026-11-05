# qianwen-realtime — application fragment (NOT a component; see examples/README.md).
#
# main/CMakeLists.txt includes this file and adds the sources to the `main`
# component, with "${_app_dir}" as the base for every path below.

set(RING_EXAMPLE_SRCS          "qianwen_realtime.c")
set(RING_EXAMPLE_INCLUDE_DIRS  "include")
set(RING_EXAMPLE_REQUIRES      "")
# Known speech clip ("the capital of China", espeak -> 48 kHz / 16-bit mono)
# used as a deterministic model-interaction test input.
set(RING_EXAMPLE_EMBED_FILES   "assets/test_clip.pcm")

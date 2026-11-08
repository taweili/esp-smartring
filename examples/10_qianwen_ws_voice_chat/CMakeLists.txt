# 10_qianwen_ws_voice_chat — application fragment (NOT a component; see examples/README.md).
#
# main/CMakeLists.txt includes this file and adds the sources to the `main`
# component, with "${_app_dir}" as the base for every path below.

set(RING_EXAMPLE_SRCS         "qianwen_ws_voice_chat.c")
set(RING_EXAMPLE_INCLUDE_DIRS "include")
set(RING_EXAMPLE_REQUIRES     "")

# Bench self-test: run one complete turn from the embedded speech clip on boot
# instead of waiting for a finger on the button (README "Verifying a turn
# without touch"). Uncomment both lines to enable it.
set(RING_EXAMPLE_DEFINES     "QW_SELFTEST=1")
set(RING_EXAMPLE_EMBED_FILES "assets/qw_selftest.pcm")

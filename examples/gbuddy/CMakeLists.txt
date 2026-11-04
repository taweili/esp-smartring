# gbuddy — application fragment (NOT a component; see examples/README.md).
#
# main/CMakeLists.txt includes this file and adds the sources to the `main`
# component, with "${_app_dir}" as the base for every path below.
#
# The assets/*.c files are generated from medias/gbuddy-00N.png by
# assets/rebuild.sh — see notes/gbuddy.md §4.

set(RING_EXAMPLE_SRCS
    "gbuddy.c"
    "assets/eye_open_l.c"
    "assets/eye_open_r.c"
    "assets/eye_joy_l.c"
    "assets/eye_joy_r.c"
    "assets/eye_sad_l.c"
    "assets/eye_sad_r.c"
    "assets/mouth_smile.c"
    "assets/mouth_o.c"
    "assets/mouth_frown.c"
    "assets/mouth_grin.c"
)
set(RING_EXAMPLE_INCLUDE_DIRS "include")
set(RING_EXAMPLE_REQUIRES     "")

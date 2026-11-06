#!/usr/bin/env bash
# Regenerate every sprite .c file in this directory from medias/gbuddy-00N.png.
#
# Pipeline (see notes/gbuddy.md §4):
#   fixed crop window -> 118x150 / 110x80 panel-scale RGB
#   luminance matte -> feather the border -> RGB565 + A8 planes
#   -> tools/lvgl_img_pack.py -> <name>.c
#
# The crop windows are fixed, so every mood's eyes sit at the same place in the
# sprite and a mood swap never shifts the face. Re-run from anywhere:
#
#   examples/01_gbuddy_face/assets/rebuild.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRC="$ROOT/medias"
PACK="$ROOT/tools/lvgl_img_pack.py"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

MAGICK=${MAGICK:-magick}

# Fixed screen-space windows (2048x1152 originals).
EYE_L="180x230+769+356"    # left eye + brow, ~10 px margin
EYE_R="180x230+1093+356"   # right eye + brow
MOUTH="168x122+940+543"    # mouth, centred on (1024,604)

# make_sprite <window> <out_w> <out_h> <corner_r> <input.png> <name>
make_sprite() {
    local win="$1" ow="$2" oh="$3" rr="$4" in="$5" name="$6"
    local png="$HERE/$name.png"
    local rgb="$TMP/$name.rgb" a8="$TMP/$name.a8" rgba="$TMP/${name}_rgba.png"
    local feather="roundrectangle 6,6 $((ow - 7)),$((oh - 7)) ${rr},${rr}"

    $MAGICK "$in" -crop "$win" +repage -resize "${ow}x${oh}!" "$png"

    # matte from luminance: glowing art is bright, the navy screen is dark
    $MAGICK "$png" \( "$png" -colorspace gray -level 12%,45% \) \
        -alpha off -compose CopyOpacity -composite "$rgba"
    $MAGICK "$rgba" -depth 8 "rgb:$rgb"

    # feather the matte in from the border, or the sprite's rectangular glow
    # edge is visible against the background
    $MAGICK "$rgba" -alpha extract \
        \( -size "${ow}x${oh}" xc:black -fill white -draw "$feather" -blur 0x3 \) \
        -compose multiply -composite -depth 8 "gray:$a8"

    python3 "$PACK" "$rgb" "$ow" "$oh" "$HERE/$name.c" "$name" "$a8"

    # Rebuild the preview from the same feathered matte, otherwise the PNG
    # would show a rectangular edge the firmware does not have.
    $MAGICK "$rgba" -alpha extract \
        \( -size "${ow}x${oh}" xc:black -fill white -draw "$feather" -blur 0x3 \) \
        -compose multiply -composite "$TMP/${name}_pv.png"
    $MAGICK "$rgba" "$TMP/${name}_pv.png" -alpha off \
        -compose CopyOpacity -composite "$png"
}

# --- eyes (open / joy / sad, both sides) -----------------------------------
for side in l r; do
    if [ "$side" = l ]; then win="$EYE_L"; else win="$EYE_R"; fi
    make_sprite "$win" 118 150 14 "$SRC/gbuddy-001.png" "eye_open_$side"
    make_sprite "$win" 118 150 14 "$SRC/gbuddy-004.png" "eye_joy_$side"
    make_sprite "$win" 118 150 14 "$SRC/gbuddy-003.png" "eye_sad_$side"
done

# --- mouths ---------------------------------------------------------------
make_sprite "$MOUTH" 110 80 12 "$SRC/gbuddy-001.png" mouth_smile
make_sprite "$MOUTH" 110 80 12 "$SRC/gbuddy-002.png" mouth_o
make_sprite "$MOUTH" 110 80 12 "$SRC/gbuddy-003.png" mouth_frown
make_sprite "$MOUTH" 110 80 12 "$SRC/gbuddy-004.png" mouth_grin

echo "done: $(ls -1 "$HERE"/*.c | wc -l) asset files, $(du -ch "$HERE"/*.c | tail -1) of C source"

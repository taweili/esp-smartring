# 01_gbuddy_face — the GEODIS buddy face, animated

The stills in `medias/gbuddy-001..004.png` are four *expressions* of the same round
companion robot. This app turns them into a live face on the 360×360 panel: the eyes and
mouth are cut out of the artwork as alpha sprites and animated as small objects over a
flat navy "screen" band.

| Mood (tap to cycle) | Eyes | Mouth | Source |
|---|---|---|---|
| `happy` | open | smile | 001 |
| `surprised` | open | "o" | 002 |
| `sad` | droopy | frown | 003 |
| `joy` | closed `^` | grin | 004 |

On top of the moods:

- **blink** every 2.5–6 s (swap to the closed-eye sprite for 110 ms; 15 % double-blinks)
- **gaze** follows gravity — `ring_imu_tilt()` shifts both eyes up to ±8 px
- **twist** the ring (gyro Z) and the gaze flicks sideways, then decays back
- **long-press** → sleep (closed eyes, backlight 20 %); **tap** wakes it
- **battery < 15 %** → forced sad face

```
idf.py -DRING_APP=01_gbuddy_face -B build/01_gbuddy_face build
idf.py -DRING_APP=01_gbuddy_face -B build/01_gbuddy_face -p /dev/ttyACM0 flash monitor
```

## Assets

`assets/*.c` are generated — do not edit them. They are cut from `medias/` with fixed
screen-space windows so a mood swap never shifts the face:

```
assets/rebuild.sh          # needs ImageMagick (`magick`) only
```

That script + `tools/lvgl_img_pack.py` produce `lv_image_dsc_t` C files
(RGB565A8: colour plane then an A8 matte plane). Design rationale, crop windows, sprite
sizes and every measured number are in **`notes/gbuddy.md`**.

Notes for anyone touching the art:

- The `.png` previews next to the `.c` files carry the same feathered matte as the
  firmware (they are rebuilt from it), so what you see is what the panel draws.
- The background is a **flat** colour on purpose: a 222-px RGB565 gradient only has ~3
  quantisation steps and bands visibly.
- Adding moods costs ~53 KB (eye pair) + 26 KB (mouth) of flash per expression; the app
  currently uses 424 KB of assets with 382 KB of the partition free.

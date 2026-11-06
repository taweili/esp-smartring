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

Expected boot log: `app: 01_gbuddy_face`, `ringkit: display=ok battery=ok power=ok
audio=ok sd=ok imu=ok`, `01_gbuddy_face: mood happy`,
`ready: 4 moods, 6 eye + 4 mouth sprites`, then no panic/assert.

---

## 1. Assets

`assets/*.c` are generated — do not edit them. They are cut from `medias/` with fixed
screen-space windows so a mood swap never shifts the face:

```
assets/rebuild.sh          # needs ImageMagick (`magick`) only
```

That script + `tools/lvgl_img_pack.py` produce `lv_image_dsc_t` C files
(RGB565A8: colour plane then an A8 matte plane).

| Sprite | Source crop (2048×1152) | Size | Bytes |
|---|---|---|---|
| `eye_open` / `eye_joy` / `eye_sad` (L,R) | `180x230+769+356`, `180x230+1093+356` | 118×150 | 53,100 each |
| `mouth_smile` / `_o` / `_frown` / `_grin` | `168x122+940+543` | 110×80 | 26,400 each |

**Assets: 424,200 B** (6 eyes + 4 mouths). `smartring.bin` = **1,153,600 B**, leaving
**382,912 B** free in the 1500 KiB partition. Adding moods costs ~53 KB (eye pair) + 26 KB
(mouth) of flash per expression.

Labels live on a `face` container (360×222 at panel `y = 69`) holding `eye_l` at (15,24),
`eye_r` at (227,24) and `mouth` at (125,153) — container-relative, so a whole-face motion
is one handle and no ±69 offsets leak into timer callbacks.

### Cutting a sprite

The `.a8` matte must be feathered ~6 px in from the border, or the sprite's rectangular
edge shows against the background:

```bash
magick medias/gbuddy-001.png -crop 180x230+769+356 +repage -resize 118x150! eye_open.png
magick eye_open.png \( eye_open.png -colorspace gray -level 12%,45% \) \
  -alpha off -compose CopyOpacity -composite eye_open_rgba.png
magick eye_open_rgba.png -depth 8 rgb:eye_open.rgb
magick eye_open_rgba.png -alpha extract \
  \( -size 118x150 xc:black -fill white \
     -draw "roundrectangle 6,6 111,143 14,14" -blur 0x3 \) \
  -compose multiply -composite -depth 8 gray:eye_open.a8
python3 tools/lvgl_img_pack.py eye_open.rgb 118 150 assets/eye_open.c eye_open eye_open.a8
```

---

## 2. Why short sprites, not whole frames

- **The four stills are four expressions, not four animation frames.** Looping them with
  `lv_animimg` reads as a slideshow of moods, not motion.
- **A full-frame RGB565 still is 159,840 B.** Four of them plus the app exceeds what the
  partition comfortably allows. Sprites (~424 KB total) fit.
- **No PPA/DMA2D on the ESP32-S3.** Every scale/rotate lands on the CPU via
  `lv_draw_sw_transform`, with PSRAM at 40 MHz — so transforms are confined to sprites
  (a blink), never the background.
- **The 40-line BSP draw buffer** means a full-panel repaint is 9 flushes. Animating small
  objects instead of the frame is what keeps idle animation at ≥ 20 fps.

Palette (`I8`/`I4`) images are **not** drawable by this LVGL SW renderer, so
"palette-compress the stills" is not an option without a decode step.

---

## 3. Runtime design

| Animation | Period / duration | Mechanism |
|---|---|---|
| idle blink | every 2.5–6 s, 110 ms | swap to the `eye_joy` sprite |
| saccade | every 0.8–3 s, 60–140 ms | `translate_x/y`, ±6 px x / ±3 px y |
| breathing | 4.2 s | ±1.5 px y on eyes, ±1 px on the container |
| mood change | 180 ms | sprite swap + short scale/alpha |
| boot | 900 ms | eyes fade in, mouth scale 0.8→1.0 |

| Trigger | Effect |
|---|---|
| tap (`LV_EVENT_CLICKED`) | next mood; back to idle after 2.5 s |
| long-press | sleep: eyes shut, backlight 100→20 % |
| any press while asleep | wake, happy, restore backlight **before** the next mood change |
| IMU tilt | both eyes drift to the low side, low-passed, clamped |
| gyro twist (z) | fast glance sideways, returns on stop |
| battery ≤ 15 % | forced sad, polled every ~10 s |
| charging | glow pulse ×2 |

`lv_timer`/`lv_anim` callbacks run on the adapter task, which already holds the
(recursive) LVGL lock; the app still uses `RING_UI_LOCKED(-1)` for its own discipline.
Touch is single-contact (CST816S), so no pinch/two-finger gestures exist.

---

## 4. On-device findings (what the plan got wrong)

These were found with LVGL snapshots (§5) and are the reason the code looks the way it does.

| Planned | Shipped | Why |
|---|---|---|
| vertical gradient background | **flat `#091B36`** | a 222-px RGB565 gradient quantises to ~3 steps and bands visibly |
| glow blobs behind the eyes | **removed** | on device they render as dark discs with a visible outline |
| blink = pre-baked squashed sprite | **swap to `eye_joy` for 110 ms** | zero extra asset, perfectly aligned; a photo eye squashed to 18 % reads as a slit |
| `scale_y`/`mv` animations | **sprite swaps + `translate_x/y` only** | no transform cost, no pivot arithmetic, layout coordinates untouched |
| sad = open eyes pushed down, 0.78× | **real `eye_sad` sprites** (from 003) | it fit the budget and is the correct shape |

Three more lessons:

1. **The feather is what removes the sprite box, not the level.** `-level 12%,45%` on the
   luminance matte plus the 6 px feather makes the edge invisible. The `assets/*.png`
   previews are rebuilt *from* the feathered matte, so what you see is what the panel
   draws — an earlier version wrote previews pre-feather and sent debugging the wrong way.
2. **`lv_obj` shadows are not a glow.** The shadow draws only outside the shape and does
   not continue the interior fill, so `bg_opa` + `shadow_*` reads as a disc with a rim. A
   real soft glow needs a radial gradient
   (`CONFIG_LV_USE_DRAW_SW_COMPLEX_GRADIENTS` is off) or a baked asset.
3. **The face band has hard top/bottom edges** at y=69 and y=291. It reads as a screen
   inset (the still has one too). For a seamless version, paint two 360×180 gradient
   objects on the screen (black→`#0A1C38`, `#061124`→black) — 0 bytes, no seam, sprite
   coordinates unchanged.

---

## 5. Snapshot debugging (reusable)

Without a camera, use `CONFIG_LV_USE_SNAPSHOT=y`, call
`lv_snapshot_take(obj, LV_COLOR_FORMAT_RGB565)` **on the LVGL task**, and hex-dump
`buf->data` over the console (64 bytes/line, each line prefixed by its index so the host
can drop lines a log message interrupts). The host decodes it to RGB and you see exactly
what LVGL drew. This is how the three deltas in §4 were found.

Snapshotting is slow: a 360×360 RGB565 dump is 259 KB → ~14 s at console speed, and
blocking the LVGL task that long trips the task watchdog. Dump a sub-object
(`lv_snapshot_take(face, ...)` = 160 KB) and `vTaskDelay(1)` every 64 lines.

> Always drain `/dev/ttyACM0` while logging: console writes block and look like a hang
> (AGENTS.md "Gotchas").

---

## 6. Gotchas

- **RGB565 banding** in soft gradients — use a flat fill or bake a dithered asset.
- **The eye sprite contains the brow.** That is deliberate (they move together); split it
  out (+7.6 KB each) only if a blink's squash reads badly.
- **Flash headroom is real.** ~800 KB is the floor for *any* app because of NimBLE.
  Prefer sprites; never add Wi-Fi to this app.
- **Battery/IMU are stale after sleep** — clamp the first gaze frame after wake.
- **The AXP2101 `ESP_ERR_TIMEOUT` warning** in the log is pre-existing (it appears on
  `00_glyph_grid_reveal` too) and is rate-limited by the BSP.
- **Partition/flash settings are global** (`partitions.csv`, `sdkconfig.defaults`); an
  assets partition would affect every app.

---

## 7. Needs a human

Blink timing/feel, gaze response to real tilt and twist, sleep/wake, and how the face band
looks inside the round bezel (a snapshot is rectangular and cannot show the physical
cut-out). Touch cannot be synthesised from the host.

---

## 8. References

- `components/ringkit/include/ringkit/ring_theme.h` — panel geometry (360/180).
- `components/viewesmart__smartring_plus/smartring_bsp.c` — `buffer_height = 40`, RGB565 +
  flush byte swap.
- `notes/esp-smartring.md` §6 — display pitfalls (full-screen flush, QSPI + PSRAM).
- `notes/architecture.md` §6 — why examples are CMake fragments, hence a generated `.c` in
  `RING_EXAMPLE_SRCS` rather than `EMBED_FILES`.
- Vendored LVGL 9.4: `src/draw/lv_draw_buf.c` (RGB565A8 layout),
  `src/draw/sw/blend/lv_draw_sw_blend_to_rgb565.c` (I8/I4 not drawable),
  `src/draw/sw/lv_draw_sw_img.c` (CPU transform path).
- LVGL images (v9.5): <https://lvgl.io/docs/open/9.5/main-modules/images/sources>
- PPA is ESP32-P4 only: <https://github.com/lvgl/lvgl/issues/10260>

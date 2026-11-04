# GBuddy — animating `medias/gbuddy-001.png` on the Smart Ring Plus

**Date:** 2026-11-04
**Status:** implemented as `examples/gbuddy/` (§12 records what shipped and how it differs
from the plan below).
**Goal:** turn the round-companion still in `medias/gbuddy-001.png` (plus its three
siblings) into a **live face** on the 360×360 round panel: blinking, looking around,
changing expression, reacting to touch and tilt — within this project's flash, RAM and
draw-budget rules.

Everything in §1 is **measured** on the host against the actual files. §3 combines those
measurements with checks against the vendored LVGL/espressif sources. Anything that needs
the panel is marked **(device)**.

---

## 0. TL;DR

- `gbuddy-001..004.png` are **four expressions of the same robot**, not four animation
  frames. Looping them in `lv_animimg` would read as a slideshow of moods, not motion.
- The panel is round and the stills are 16:9 studio shots on **white** — only the *screen
  interior* is usable. Measured crop: `550x340+750+320`, rescaled to **360×222**, which
  fills the panel nicely (eyes 110×142 px on-target).
- A **full-frame RGB565 still is 159,840 B** and the app partition is **1500 KiB**;
  four stills (640 KB) technically fit next to `hello`'s 823 KB but leave only ~70 KB of
  headroom. Don't build on that.
- LVGL's SW renderer here draws **RGB565 / RGB565A8 / RGB888 / ARGB8888 / A8 / L8 / AL88 /
  I1** — **not I8/I4** palette images (§3.1). So "palette-compress the stills" is not an
  option without a decode step.
- **Recommended (Tier 2, §2):** keep the eyes and mouth as alpha sprites cut from the
  stills, draw the navy background as a **procedural gradient** (0 B, and a host composite
  of gradient + sprites is visually indistinguishable from the still), then animate *small
  objects* — blink, saccades, mood swap — never the whole frame. Asset cost ≈ **291 KB**,
  ~415 KB of flash headroom.
- **Fastest proof (Tier 1):** one 360×222 still + soft edge fade + slow y-bob + crossfade
  between the four moods. This already looks alive and needs ~4 h of work.
- No PPA/DMA2D on the ESP32-S3: every transform (scale/rotate) is CPU + PSRAM bandwidth.
  Image scaling is *allowed* but must be confined to sprites, not the background.

---

## 1. The source material (measured)

### 1.1 Files

| File | Size | Format |
|---|---|---|
| `medias/gbuddy-001.png` | 1,261,638 B | 2048×1152, 8-bit RGB, non-interlaced |
| `medias/gbuddy-002.png` | 1,680,077 B | same |
| `medias/gbuddy-003.png` | 1,582,650 B | same |
| `medias/gbuddy-004.png` | 1,467,396 B | same |

Content: a round blue/silver companion robot with a **rounded-rect emissive screen** for a
face, a blue "GEODIS" base with a speaker grille, on a **white studio background** with a
reflection. Bottom-right corner carries a generator watermark ("豆包AI生成") — the crop in
§1.3 excludes it.

### 1.2 The four moods

Blue-pixel connected-component analysis of the screen area (threshold
`B>90 ∧ B>R+40`, coordinates in the 2048×1152 original):

| # | Mood | Eyes | Mouth | Notes |
|---|---|---|---|---|
| 001 | **happy** | tall open ovals, 168×217, centres (858,472)/(1187,474) | smile, 159×90 at (1024,604) | the "hero" frame — glow rim + white highlights |
| 002 | **surprised** | wide ovals, 171–178 × 191–193 at (859,488)/(1188,490) | open "o", 124×104 at (1025,608) | brows raised (blob at y≈331) |
| 003 | **sad** | droopy, 176–182 × 161–173, lids pulled down | small frown, 76×29 at (1028,620) | separate lid arcs at (864,430)/(1183,428) |
| 004 | **joy** (closed) | closed "^" arcs, 150–154 wide | wide smile, 121×70 at (1024,618) | sparkle dot right of the mouth |

Nose: a small rounded triangle, 53×37 at (1023,530).

Because the mouth and the eye *shape* both change, the four frames are genuinely
different expressions — a crossfade between them is a mood transition, not animation.

### 1.3 Screen geometry and the crop that works

- Screen (rounded rect, radius ≈ 30 px): **x 728…1322, y 302…680 → 594×378** (aspect 1.57).
- Usable interior crop (keeps a margin inside the bezel, removes the watermark):
  **`550x340+750+320`**.
- Scaled to **360×222** (uniform: 0.6545 × 0.6529) the face fills the panel; the round
  bezel only clips the asset's corners, which are black after the edge fade below.

Resulting geometry. Coordinates are **relative to the 360×222 face container**, which
itself sits at panel `y = 69` (`(360-222)/2`); add 69 to y for absolute panel positions:

The shipped sprite windows (screen-space crop → sprite size):

| Sprite | Crop window (2048×1152) | Size | Top-left in container |
|---|---|---|---|
| eye L (open / joy / sad) | `180x230+769+356` | 118 × 150 | **(12, 24)** |
| eye R (open / joy / sad) | `180x230+1093+356` | 118 × 150 | **(224, 24)** |
| mouth (smile / o / frown / grin) | `168x122+940+543` | 110 × 80 | **(124, 146)** |

Inside an eye sprite the iris is ~110 × 142 with ~4 px of glow margin, and the **brow is
part of the sprite** (same still; they move together). Visible feature sizes in the
360-wide face: eye 110 × 142 at ±106 px from centre, mouth 104 × 59 (smile), brow 47 × 19.

### 1.4 Palette (dominant colours of the face)

| Role | Hex |
|---|---|
| screen background (navy) | `#091935`, `#061B4B`, `#0F203D` |
| deep glow / halo | `#052890`, `#0831AD` |
| iris / eye rim | `#76D4FE`, `#88D7FB`, `#7EE0FD` |
| brow (saturated blue) | `#1956E3` |
| highlight | `#DEF9FD`, `#AEECFD` |

### 1.5 Why the photo can't be shown whole

- The white background covers **68 % of the frame** (measured). Shown on a black round
  panel it would read as a glowing white card with a robot printed on it — the opposite of
  a face.
- Scaled whole into 360×360, the robot's *screen* would be only **~227 px** wide
  (the device is ~940 px of the 2048 px frame; measured). Showing a photo of the device
  inside the device is redundant: *this ring already is the robot*.
- So: show the **screen content only**. That is also what makes the app feel like the
  ring is "looking at you".

### 1.6 Fitting a rectangle into the round panel

Largest centred axis-aligned rectangle inside r = 180, by aspect:

| aspect | max w × h |
|---|---|
| 1.571 (the screen) | **303.7 × 193.3** |
| 1.600 | 305.3 × 190.8 |
| 1.500 | 299.5 × 199.7 |
| 1.000 | 254.6 × 254.6 |

So the *strictly-inside* still is ~304×193. The recommended **360×222** deliberately
overflows (corners off-panel) because the overflow is black after the edge fade — this
buys 18 % more eye size for free. Do the corner check on the device (§9) before freezing.

### 1.7 Edge fade — kill the rectangle seam

Even on a black panel the crop's navy background is lighter than pure black, so the
asset's top/bottom edges show as a faint band. Multiplying an outer ~16 px border down to
black removes the seam completely (**verified on the host render**):

```bash
magick medias/gbuddy-001.png -crop 550x340+750+320 +repage -resize 360x222! \
  \( -size 360x222 xc:black -fill white \
     -draw "rectangle 16,0 343,221" -draw "rectangle 0,16 359,205" -blur 0x10 \) \
  -compose multiply -composite -strip face_360x222.png
```

Bake the fade into the *background* layer only; sprites must not carry it.

---

## 2. What "animate" can mean — three tiers

| Tier | What moves | Assets | Flash | Effort | Reads as |
|---|---|---|---|---|---|
| **T1 — live photo** | whole still: y-bob, glow pulse, crossfade between 4 moods, edge fade | 4 × 360×222 RGB565 = **640 KB** (or 4 × 304×188 = 457 KB) | fits but tight | ½ day | "a photo that breathes" — no blink, no gaze |
| **T2 — marionette** (recommended) | gradient background + eye/mouth sprites: blink, saccade, mood swap, gaze from IMU | 0 B bg + 4 eye (2 variants × 2) + 4 mouth ≈ **291 KB** (§3.3) | comfortable | 2–3 days | "a creature" — the still is the art, LVGL moves it |
| **T3 — procedural** | eyes/mouth drawn with `lv_obj` circles, arcs, gradients + shadows | **0 B** | free | 3–4 days + tuning | "vector robot face" — expressive but not the render |

**Recommendation:** ship **T1 first** as `examples/gbuddy` (it is a real, verifiable
deliverable and derisks the panel look), then refactor the same file into **T2**. Keep
T3 as the fallback if the layered art never looks right at 360 px — the stills then serve
purely as the colour/geometry reference (they already give exact numbers, §1.3/§1.4).

Why not just T1 forever: without a blink, the face is uncanny within seconds. Blink is
the single highest-value animation, and it needs the eyes to be separate objects (or a
pre-baked closed-eye variant per mood — 8 stills, ~1.28 MB, out of budget).

Prior art worth mining: `0015/lvgl_kawaii_face` (LVGL 9 canvas face, emotion state
machine) and `txp666/otto-emoji-gif-component` (21 procedural expressions) — both solve
the same state-machine problem, neither ships photo-art sprites.

---

## 3. Assets on this board: what actually works

### 3.1 Colour formats the SW draw unit can render

Verified in `managed_components/lvgl__lvgl/src/draw/sw/blend/lv_draw_sw_blend_to_rgb565.c`
(src formats) — and the software unit is the **only** renderer in this build (no PPA:
that is ESP32-P4; `CONFIG_LV_USE_DRAW_SW_ASM=0`, `LV_USE_MATRIX` unset):

| Format | B/px | Drawable | Use for |
|---|---|---|---|
| `RGB565` | 2 | ✅ | background, opaque sprites |
| `RGB565A8` | 3 | ✅ | sprites with a real matte (eye/brow/mouth) |
| `RGB888` / `XRGB8888` | 3–4 | ✅ | never worth it here (display is 16-bit) |
| `ARGB8888` | 4 | ✅ | `lv_canvas` layers |
| `A8` | 1 | ✅ (recoloured) | masks, lids, glow stencils |
| `L8` / `AL88` | 1–2 | ✅ | monochrome marks |
| `I1` | 1/8 | ✅ | 1-bit only |
| **`I8` / `I4` / `I2`** | 1 / 0.5 / 0.25 | ❌ **not drawn** | — |

That last line kills a tempting idea: the art is nearly monochrome, and a 256-colour
palette reproduces it at **38.7 dB PSNR** (measured with a palette built from all four
frames and Floyd–Steinberg dithering). 1 B/px would put four stills in 320 KB. **LVGL 9.4's
software renderer has no I8 path** (only `I1`, guarded by `CONFIG_LV_DRAW_SW_SUPPORT_I1`),
so this would need a custom decoder that expands to RGB565 at load — 160 KB of PSRAM per
frame. Not worth it for ≤4 images; revisit only if the asset count grows.

### 3.2 Byte order and alignment

- `LV_COLOR_DEPTH=16`, display format `RGB565`; the BSP/adapter **swaps bytes on flush**
  (`smartring_bsp.c` `bsp_display_set_swap_bytes`). Store assets **native little-endian
  RGB565** — do *not* pre-swap.
- `header.stride` is the **bytes per row**: `w * 2` for RGB565. RGB565A8 occupies
  `stride * h` bytes of colour followed by a contiguous `w * h` A8 plane (see
  `_calculate_draw_buf_size()` in `lv_draw_buf.c`), so `data_size` is `w*h*3`.
- Put pixel arrays in flash as `const` with `LV_ATTRIBUTE_MEM_ALIGN`. They cost **no
  RAM**; LVGL reads them through the flash cache.

### 3.3 Measured asset sizes (crop of §1.3, panel scale)

Sprites carry margin so the matte can feather out (§4.2); the
"visible" size is the feature inside the sprite. **The eye sprite contains its brow** —
they are cut from the same still and move together.

| Asset | Pixels | RGB565 | RGB565A8 (used) |
|---|---|---|---|
| eye sprite 118×150 (visible eye 110×142 + brow) | 17,700 | 35,400 B | **53,100 B** |
| mouth sprite 110×65 (visible smile 104×59) | 7,150 | 14,300 B | **21,450 B** |
| *(optional)* full face background 360×222 | 79,920 | 159,840 B | — |
| *(optional)* face, strictly-inside 304×188 | 57,152 | 114,304 B | — |

Recommended T2 set: **no background asset** + 4 eye sprites (`eye_open`, `eye_joy` × 2)
+ 4 mouths (smile/o/frown/grin) = 212,400 + 85,800 = **298,200 B (291 KiB)**.
Adding a third eye variant (surprised, cut from 002) costs another 106,200 B; a real
droopy-lid sad variant likewise. Both still fit (≈310 KB headroom). Storing sprites at
half resolution and scaling 2× at runtime divides the sprite bytes by four (≈75 KB), at
the cost of a per-pixel transform on each eye/mouth draw.

Host-side PNG/QOI of the 360×222 crop (for comparison): PNG ≈ **94–102 KB**, QOI ≈
**115–127 KB** — the AI art is noisy, so *lossless compression saves only ~35 %*, and you
pay a decoder + a decoded-frame RAM buffer.

### 3.4 Flash budget — the binding constraint

`CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE` → factory **1500 KiB = 1,536,000 B**.

| Build | `smartring.bin` | headroom |
|---|---|---|
| `hello` | 823,088 B | **712,912 B** |
| `glyph-anime` | 750,576 B | 785,424 B |
| `bt-demo` | 1,080,096 B | 455,904 B |
| `wifi-demo` | 1,376,944 B | 159,056 B |

(`CONFIG_BT_ENABLED=y` is in `sdkconfig.defaults`, so *every* app already links NimBLE —
that is why even `hello` is 823 KB. Don't add Wi-Fi to gbuddy.)

Budget consequences:

- **T1 at 360×222**: 4 × 159,840 = 639,360 B → **~73 KB left**. Possible, no margin. At
  304×188: 457,216 B → **~255 KB left**. Recommended T1 resolution if all four moods ship.
- **T2**: 298,200 B → **~415 KB left**; with a third and fourth eye variant, ~310 KB left.
  The sprite split means *more* animation for *less* flash than T1.
- Add ~0 for code. Avoid pulling in `libpng`/`zlib` (decoder path) unless measured.

---

## 4. Asset pipeline (offline, no pip)

There is no image toolchain in this repo yet, and **PIL is not installed** on this machine.
`lvgl/scripts/LVGLImage.py` exists in the vendored LVGL but needs `pypng` + `lz4`. So the
zero-dependency route is **ImageMagick → raw RGB → tiny Python packer → C array**.

ImageMagick 7 is installed (`magick`), and so is the LVGL "binary image" idea without any
tool: we generate the `lv_image_dsc_t` ourselves.

### 4.1 The packer (`tools/lvgl_img_pack.py`, proposed)

```python
#!/usr/bin/env python3
"""Pack a raw RGB888 (+ optional A8) blob into an LVGL 9 lv_image_dsc_t C file."""
import struct, sys

def main(src, w, h, out, name, alpha=None):
    w, h = int(w), int(h)
    rgb = open(src, "rb").read()
    assert len(rgb) == w * h * 3, (len(rgb), w * h * 3)
    a8 = open(alpha, "rb").read() if alpha else None
    if a8 is not None:
        assert len(a8) == w * h, (len(a8), w * h)
        cf, px = "LV_COLOR_FORMAT_RGB565A8", bytearray()
    else:
        cf, px = "LV_COLOR_FORMAT_RGB565", bytearray()
    for i in range(0, len(rgb), 3):
        r, g, b = rgb[i], rgb[i + 1], rgb[i + 2]
        px += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    if a8:
        px += a8                       # alpha plane follows the colour plane
    with open(out, "w") as f:
        f.write(f"/* {w}x{h} {cf}, {len(px)} B, generated — do not edit */\n")
        f.write('#include "lvgl.h"\n\n')
        f.write(f"static const LV_ATTRIBUTE_MEM_ALIGN uint8_t {name}_map[] = {{\n")
        for i in range(0, len(px), 16):
            f.write("    " + ",".join(f"0x{b:02x}" for b in px[i:i + 16]) + ",\n")
        f.write("};\n\n")
        f.write(f"const lv_image_dsc_t {name} = {{\n"
                f"    .header.magic = LV_IMAGE_HEADER_MAGIC,\n"
                f"    .header.cf = {cf},\n"
                f"    .header.w = {w},\n"
                f"    .header.h = {h},\n"
                f"    .header.stride = {w * 2},\n"
                f"    .data_size = sizeof({name}_map),\n"
                f"    .data = {name}_map,\n}};\n")
    print(f"{out}: {len(px)} B ({cf})")

if __name__ == "__main__":
    main(*sys.argv[1:6], alpha=(sys.argv[6] if len(sys.argv) > 6 else None))
```

Usage:

```bash
# T1 asset: the whole face frame (used only if you skip the sprite split)
magick medias/gbuddy-001.png -crop 550x340+750+320 +repage -resize 360x222! \
       -depth 8 rgb:/tmp/face.rgb
python3 tools/lvgl_img_pack.py /tmp/face.rgb 360 222 \
        examples/gbuddy/assets/face_001.c face_001
```

A 160 KB asset becomes an ~850 KB `.c` file. That is fine for the compiler (~1–2 s,
ccache-friendly) but **do not** hand-edit it, and add a `/* generated */` banner.

> `target_add_binary_data()` / `EMBED_FILES` are *not* available to an example: the
> fragment contract only exposes `RING_EXAMPLE_SRCS/_INCLUDE_DIRS/_REQUIRES`, and it is
> `include()`d **before** `idf_component_register()` in `main/CMakeLists.txt`. A generated
> `.c` is the path of least resistance; adding binary embedding would mean changing
> `main/CMakeLists.txt` for all apps.

### 4.2 Cutting the sprites (T2) — validated on the host

Only two kinds of art are needed: a background you can **generate**, and alpha sprites cut
from the stills. Composited on the host at the §1.3 coordinates (gradient + two eyes +
mouth) the result is visually indistinguishable from the still — do that check before
writing any C.

**Background — generate it.** The screen background is a smooth navy gradient. Inpainting
the eye/mouth glow out of the still smears and leaves ghosts (tested); a vertical gradient
does not:

```bash
magick -size 1x222 gradient:'#0A1C38-#061124' -resize 360x222! bg_grad.png
```

In LVGL that is one plain object and **0 B of flash**:

```c
lv_obj_set_style_bg_color(obj, lv_color_hex(0x0A1C38), 0);
lv_obj_set_style_bg_grad_color(obj, lv_color_hex(0x061124), 0);
lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
```

**Sprites — cut with a feathered matte.** The `.a8` from `-alpha extract` must be
feathered ~6 px in from the border, or the sprite's rectangular glow edge shows against
the gradient (visible in the host mock until the feather was added):

```bash
# eye: bbox (779,366)-(938,583) + ~10 px margin
magick medias/gbuddy-001.png -crop 180x230+769+356 +repage -resize 118x150! eye_open.png
magick eye_open.png \( eye_open.png -colorspace gray -level 0%,30% \) \
  -alpha off -compose CopyOpacity -composite eye_open_rgba.png
magick eye_open_rgba.png -depth 8 rgb:eye_open.rgb
magick eye_open_rgba.png -alpha extract \
  \( -size 118x150 xc:black -fill white \
     -draw "roundrectangle 6,6 111,143 14,14" -blur 0x3 \) \
  -compose multiply -composite -depth 8 gray:eye_open.a8
python3 tools/lvgl_img_pack.py eye_open.rgb 118 150 assets/eye_open.c eye_open eye_open.a8

# mouth smile: bbox (945,559)-(1104,649) + ~3 px margin
magick medias/gbuddy-001.png -crop 165x96+942+556 +repage -resize 110x65! mouth_smile.png
# ... then the same CopyOpacity + feather + pack, as mouth_smile
```

Notes:

- The eye sprite **includes the brow** (cut from the same still; they move together). A
  blink that squashes the sprite (`lv_image_set_scale_y`) squashes the brow slightly too —
  invisible over 120 ms. If it bothers you, cut the brow as its own 63×40 sprite
  (+7.6 KB each) and zero its band in the eye's matte (brow bbox in 001:
  `820,369`–`904,419`, i.e. sprite y ≈ 0…44).
- Keep the **glow with the sprite**: the halo is part of the eye's identity, and the
  feathered matte keeps its edge soft.
- Sprite alpha from luminance is good enough for glowing art; the dark pupil becomes
  semi-transparent over a matching navy, which is fine. If the pupil disappears against a
  lighter background, matte on the blue channel instead (`-channel B -separate`).
- Second eye variant: the same crop from 004 (`eye_joy`, the closed "^" arcs). Sad reuses
  `eye_open` pushed down + `lv_image_set_scale_y(eye, 200)`; add an `eye_sad` cut from 003
  (+106 KB both eyes) only if that reads wrong.

`examples/gbuddy/CMakeLists.txt` lists every generated asset — the fragment supports a
list:

```cmake
set(RING_EXAMPLE_SRCS
    "gbuddy.c"
    "assets/eye_open.c"    "assets/eye_joy.c"
    "assets/mouth_smile.c" "assets/mouth_o.c"
    "assets/mouth_frown.c" "assets/mouth_grin.c")
set(RING_EXAMPLE_INCLUDE_DIRS "include")
set(RING_EXAMPLE_REQUIRES     "")
```

Regenerate assets with a small `examples/gbuddy/assets/rebuild.sh` that runs the
ImageMagick steps + the packer, so the checked-in `.c` files can be reproduced.

### 4.3 Alternatives (when art outgrows a C array)

| Path | Cost | When |
|---|---|---|
| `esp_lv_fs` + `esp_mmap_assets` (QOI/PNG on a flash partition) | new partition table + `CONFIG_ESPTOOLPY_FLASHSIZE` must go above today's **2 MB** setting + `CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS/DECODER` + `LV_CACHE_DEF_SIZE` | many frames, or art iteration without rebuilds |
| SD card via `ring_sd_*` + `esp_lv_fs` | zero flash, needs a card, 1–2 MB/s reads | demos/dev, not a product |
| PNG/QOI C array + decoder | decoder code (~tens of KB) + full decoded frame in PSRAM; needs a cache or it re-decodes every redraw | only if flash-bound |

Both `esp_lv_fs` and `esp_lv_decoder` are **already present** in `managed_components/`
(transitive deps of `esp_lvgl_adapter`), so enabling them is config, not new downloads.
The partition path also has to deal with the fact that `sdkconfig`/`sdkconfig.defaults`
are shared by every app (AGENTS.md) — a bigger flash setting is a project-wide change.

---

## 5. Runtime design (T2)

### 5.1 Object tree

```
screen (ring_ui_screen(), black)
└── face      lv_obj 360×222 at y=69, vertical gradient #0A1C38 → #061124
    ├── eye_l lv_image 118×150 (RGB565A8) at (15, 24)    [brow baked in]
    ├── eye_r lv_image 118×150 (RGB565A8) at (227, 24)
    └── mouth lv_image 110×65  (RGB565A8) at (125, 153)
```

The `face` container gives one handle for a whole-face bob/dip and keeps every animation
coordinate local (no ±69 sprinkled through the timer callbacks). Positions come from §1.3
and are set once; afterwards only the gaze offsets move anything. The gradient is repainted
only when the container is invalidated, so idle animation touches just the sprites.

### 5.2 Mood / event model

```c
typedef enum { MOOD_HAPPY, MOOD_SURPRISED, MOOD_SAD, MOOD_JOY, MOOD_COUNT } mood_t;

typedef struct {
    const lv_image_dsc_t *eye;     /* eye_open / eye_joy (sad = open, lids lower) */
    const lv_image_dsc_t *mouth;   /* smile / o / frown / grin                    */
    int16_t eye_dy;                /* sad: push the eyes down                     */
    uint16_t eye_zoom;             /* sad: 200 (0.78×) squint; 256 = none         */
} mood_def_t;

static const mood_def_t s_mood[MOOD_COUNT] = { /* ... one row per mood, §1.2 ... */ };
```

| Trigger | Effect |
|---|---|
| tap (`LV_EVENT_CLICKED`) | next mood, 180 ms sprite swap + scale, back to idle after 2.5 s |
| long-press (`LV_EVENT_LONG_PRESSED`) | **sleep**: eyes squashed shut, backlight 100→25 % |
| any press while asleep | wake, happy, restore backlight |
| idle 60 s | auto-sleep (optional, off by default during bring-up) |
| IMU tilt (`ring_imu_tilt`) | both eyes shift up to ±6 px, low-pass filtered |
| gyro twist (`ring_imu_gyro` z) | fast look left/right; sign verified on device (cf. `TURN_SIGN` in glyph-anime) |
| battery ≤ 15 % | forced sad mood every ~10 s |
| charging | glow pulse amplitude ×2 (heartbeat) |

Mood transitions are a **sprite swap plus a short scale/opa animation**, not a crossfade
of full frames — that is what keeps T2 cheap.

### 5.3 Animation table

| Animation | Period / duration | Cost (target) | Notes |
|---|---|---|---|
| idle blink | every 2.5–6 s, 120 ms `scale_y` 256→38→256; 15 % double-blink | 2 × 18 kpx transformed | brow squashes slightly (§4.2) |
| saccade | every 0.8–3 s, 60–140 ms move | move only | ±6 px x, ±3 px y, ease-out |
| breathing | 4.2 s, ±1.5 px y on eyes, ±1 px on the face container | move only | no scaling of the background |
| glow | 4.2 s, opa 200↔255 on the eyes | ~18 kpx, layer-buffered | keep small; `bg_opa` on a big object is expensive |
| mood change | 180 ms | swap + 2 small anims | |
| boot | 900 ms: eyes fade in (opa 0→255), mouth scale 0.8→1.0 | 1 sprite redraw | |

Blink is the one place a transform is justified: 17,700 px per eye, no extra asset, and at
120 ms the squashed brow is not perceptible. A hand-authored closed frame (a real lid rather
than a slit) would look better but costs 106 KB and an art pass — do it only if the squash
reads badly on the panel.

### 5.4 Code sketch (threading-correct)

```c
#include "ring_example.h"
#include "ringkit/ringkit.h"
#include "esp_log.h"

#define TAG "gbuddy"

#define FACE_Y 69
#define EYE_W 118
#define EYE_H 150
#define EYE_LX 15          /* sprite top-lefts, container coords (§1.3) */
#define EYE_RX 227
#define EYE_Y 24
#define MOUTH_X 125
#define MOUTH_Y 153

LV_IMAGE_DECLARE(eye_open);
LV_IMAGE_DECLARE(eye_joy);
LV_IMAGE_DECLARE(mouth_smile);
LV_IMAGE_DECLARE(mouth_o);
LV_IMAGE_DECLARE(mouth_frown);
LV_IMAGE_DECLARE(mouth_grin);

static lv_obj_t *s_face, *s_eye[2], *s_mouth;
static mood_t  s_cur = MOOD_HAPPY;
static int16_t s_gaze_x, s_gaze_y;

/* lv_timer / lv_anim callbacks run on the adapter task, which already holds the
 * (recursive) LVGL lock — keep ringkit's RING_UI_LOCKED discipline anyway. */

static void blink_exec(void *var, int32_t v)
{
    lv_image_set_scale_y((lv_obj_t *)var, (uint32_t)v);
}

static void blink_cb(lv_timer_t *t)
{
    RING_UI_LOCKED(-1) {
        for (int i = 0; i < 2; i++) {
            lv_anim_t a; lv_anim_init(&a);
            lv_anim_set_var(&a, s_eye[i]);
            lv_anim_set_exec_cb(&a, blink_exec);
            lv_anim_set_values(&a, LV_SCALE_NONE, 38);
            lv_anim_set_duration(&a, 55);
            lv_anim_set_reverse_duration(&a, 65);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
            lv_anim_start(&a);
        }
    }
    lv_timer_set_period(t, 2500 + ring_rand_below(3500));
}

static void gaze_cb(lv_timer_t *t)
{
    float gx, gy;
    ring_imu_tilt(&gx, &gy);                 /* m/s², screen plane */
    RING_UI_LOCKED(-1) {
        int32_t tx = LV_CLAMP(-6, (int32_t)(gx * 2.0f), 6);
        int32_t ty = LV_CLAMP(-3, (int32_t)(gy * 2.0f), 3);
        s_gaze_x += (tx - s_gaze_x) / 4;     /* cheap low-pass */
        s_gaze_y += (ty - s_gaze_y) / 4;
        lv_obj_set_pos(s_eye[0], EYE_LX + s_gaze_x, EYE_Y + s_gaze_y);
        lv_obj_set_pos(s_eye[1], EYE_RX + s_gaze_x, EYE_Y + s_gaze_y);
    }
}

static void apply_mood(mood_t m)
{
    s_cur = m;
    for (int i = 0; i < 2; i++) {
        lv_image_set_src(s_eye[i], s_mood[m].eye);
        lv_image_set_scale_y(s_eye[i], s_mood[m].eye_zoom);
        lv_obj_set_y(s_eye[i], EYE_Y + s_mood[m].eye_dy);
    }
    lv_image_set_src(s_mouth, s_mood[m].mouth);
}

static void click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    RING_UI_LOCKED(-1) { apply_mood((mood_t)((s_cur + 1) % MOOD_COUNT)); }
    ESP_LOGI(TAG, "mood -> %d", s_cur);
}

esp_err_t ring_example_start(void)
{
    RING_UI_LOCKED(-1) {
        lv_obj_t *scr = ring_ui_screen();

        s_face = lv_obj_create(scr);
        lv_obj_remove_style_all(s_face);
        lv_obj_set_size(s_face, 360, 222);
        lv_obj_set_pos(s_face, 0, FACE_Y);
        lv_obj_set_style_bg_opa(s_face, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_face, lv_color_hex(0x0A1C38), 0);
        lv_obj_set_style_bg_grad_color(s_face, lv_color_hex(0x061124), 0);
        lv_obj_set_style_bg_grad_dir(s_face, LV_GRAD_DIR_VER, 0);

        for (int i = 0; i < 2; i++) {
            s_eye[i] = lv_image_create(s_face);
            lv_image_set_src(s_eye[i], &eye_open);
            lv_image_set_pivot(s_eye[i], EYE_W / 2, EYE_H / 2);  /* shrink about the centre */
            lv_obj_set_pos(s_eye[i], i ? EYE_RX : EYE_LX, EYE_Y);
        }
        s_mouth = lv_image_create(s_face);
        lv_image_set_src(s_mouth, &mouth_smile);
        lv_obj_set_pos(s_mouth, MOUTH_X, MOUTH_Y);

        lv_obj_add_event_cb(scr, click_cb, LV_EVENT_CLICKED, NULL);
        lv_timer_create(blink_cb, 3200, NULL);
        lv_timer_create(gaze_cb, 40, NULL);
    }
    ESP_LOGI(TAG, "ready: mood %d, %d KB of sprites in flash", s_cur, 291);
    return ESP_OK;
}

const char *ring_example_name(void) { return "gbuddy"; }
```

Rules that this sketch obeys and new code must too:

- LVGL from the adapter task only, under `RING_UI_LOCKED(-1)`; `lv_timer` callbacks run
  inside `lv_timer_handler()` on that task and the mutex is **recursive**, so nesting is
  safe (same pattern as `imu-dash`).
- `ring_imu_*` / `ring_battery_*` are thread-safe and cheap; never block in a timer.
- `LV_DEF_REFR_PERIOD=33` caps the UI at ~30 Hz — no point animating faster.

---

## 6. Interaction contract (matches the other examples)

| Input | Behaviour |
|---|---|
| **tap** | cycle mood happy → surprised → sad → joy → happy; `ESP_LOGI` the mood |
| **long-press** | sleep / wake (eyes closed, backlight 25 % / 100 %) |
| **tilt** | eyes (with their baked-in brows) drift toward the low side (clamped, low-passed) |
| **twist** (gyro z) | fast glance left/right, returns to centre on stop |
| **battery low / charging** | sad mood / heartbeat glow (5 s poll, no blocking) |

No multi-touch: the CST816 reports a single contact (`notes/esp-smartring.md`), so pinch
and two-finger gestures are impossible here.

Accessibility of the app to a human tester: mood and gaze are visible; every transition is
logged so a serial capture proves the state machine even when the screen can't be seen.

---

## 7. Performance budget and how to measure it

### 7.1 What costs what (estimates, **verify on device**)

| Operation | Pixels touched | Relative cost |
|---|---|---|
| move an eye sprite | bounding box of the move (small) | cheapest — no pixel math |
| sprite swap (`set_src`) | sprite area | cheap — copy from flash cache |
| eye `scale_y` blink | sprite area, per-pixel transform | ~2–4 ms per eye (estimate) |
| mouth swap | 7 kpx | cheap |
| crossfade 2 × 360×222 stills | 80 kpx + layer chunking (`LV_DRAW_LAYER_SIMPLE_BUF_SIZE` 24 KB) | expensive, but only 300 ms |
| full-frame image `scale` (breathing the photo) | 80 kpx transform + 160 KB flash reads/frame | **avoid** |

The 40-line BSP draw buffer (2 × 28.8 KB internal RAM) means every flush is a 40-line
strip; a full-panel repaint is 9 flushes. That is why T2 animates objects, not the frame.

### 7.2 Measurement recipe (device)

Add a temporary probe to the app (not to ship):

```c
int64_t t0 = esp_timer_get_time();
RING_UI_LOCKED(-1) { lv_obj_invalidate(lv_screen_active()); lv_refr_now(NULL); }
ESP_LOGI(TAG, "full repaint %.2f ms", (esp_timer_get_time() - t0) / 1000.0);
```

Run it for: (a) static bg only, (b) bg + sprites, (c) one blink cycle. Acceptance target:
**idle animation ≥ 20 fps with no visible stutter**, i.e. one blink frame ≤ 50 ms.
Also watch `esp_get_free_heap_size()` before/after asset init (should not move — assets
live in flash).

> Always drain `/dev/ttyACM0` while logging: console writes block and look like a hang
> (AGENTS.md "Gotchas").

---

## 8. Risks and gotchas

1. **The face band has hard top/bottom edges.** A 360×222 gradient over a black screen
   leaves a visible horizontal boundary at y=69 and y=291. It reads as a "screen inset"
   (the still has one too), so it may be fine — but if you want it seamless, paint the
   background on the screen as two 360×180 gradient objects (black→`#0A1C38` on top,
   `#061124`→black below). Still 0 bytes, no seam, and the sprites keep their coordinates.
   **(device)** look at it once before deciding.
2. **Sprite matte edges.** Without the 6 px feather (§4.2) each sprite shows its
   rectangular glow edge against the gradient — clearly visible in the host mock.
3. **The eye sprite contains the brow.** Squashing it for a blink squashes the brow too;
   split the brow out (+7.6 KB each) if that reads badly. **(device)**
4. **RGB565 banding** in the soft glow. The art has smooth gradients; 5-6-5 gives visible
   steps at 360 px. Apply ordered dithering when reducing to RGB565 (the LVGL converter
   does it by default; with ImageMagick add a 4×4 Bayer noise pass before packing) or
   accept it and judge from a photo of the panel.
5. **Flash headroom is real.** 823 KB is the floor for *any* app because of NimBLE. Adding
   four 160 KB stills leaves 73 KB. Prefer sprites; never add Wi-Fi to this app.
6. **Partition/flash settings are global.** `CONFIG_ESPTOOLPY_FLASHSIZE="2MB"` today
   despite a 16 MB chip; an assets partition means changing shared config for every app.
7. **No PPA.** Scaled/rotated images land on the CPU (`lv_draw_sw_transform`), and PSRAM is
   only 40 MHz. Keep transforms on sprites and animate at ≤30 fps.
8. **`lv_animimg` is the wrong tool** for these stills — it cycles images as an animation.
   Use it only if you later bake a real frame sequence (e.g. a 12-frame blink strip).
9. **Auto-sleep must not strand the user.** If backlight goes to 25 % in sleep, a wake tap
   must restore it *before* the next mood change or the screen looks dead.
10. **Battery/IMU are stale after sleep.** `ring_imu_tilt()` keeps returning the last
    sample; the BSP task is free-running, but the first gaze after wake can jump — clamp
    the first frame.

---

## 9. Build & verify checklist (when the app is written)

```bash
# both apps must still build — gbuddy shares ringkit with everything else
idf.py -B build/glyph-anime -DRING_APP=glyph-anime build
idf.py -B build/gbuddy     -DRING_APP=gbuddy      build
idf.py -B build/gbuddy size                       # must fit 1500 KiB

# vendored overrides must still win (see AGENTS.md)
grep -oE "(components|managed_components)/(espressif__esp_codec_dev|viewesmart__smartring_plus)" \
     build/gbuddy/compile_commands.json | sort | uniq -c

idf.py -B build/gbuddy -p /dev/ttyACM0 flash monitor
```

Expected boot log: `app: gbuddy`, then `ringkit:` with all peripherals `ok`, then
`gbuddy: ready: ...`, and no `panic` / `assert` / `Guru Meditation`.

**Needs a human (cannot be verified from the log):** the face composition, the edge fade,
blink speed, gaze feel, mood transitions, and sleep/wake. Touch cannot be synthesised from
the host. Say so explicitly when reporting status — same rule as `glyph-anime`.

Also worth capturing once: a photo of the panel for the README, and a short capture of
`gbuddy: mood -> n` lines proving the state machine advanced on tap.

---

## 10. Open questions

1. **T1 or T2 for the first commit?** T1 is 4 h and de-risks the look; T2 is the real app.
   (Recommendation: T1.)
2. **Does the 360×222 asset look right when the bezel clips it?** Device check, §8.1.
3. **How does the glow pulse?** Baked halo + animated sprite `opa` (layer-buffered, small)
   vs a separate shadow object. Try the sprite-opa version first — no extra asset.
4. **How many moods at first?** 001 only + blink (2 eyes + smile mouth) = **~127 KB**; all
   four moods = **~291 KB** (T2). Both fit easily; the first ships sooner.
5. **Naming:** is "GBuddy" the intended product name (the art says GEODIS)? The example
   directory is `examples/gbuddy`; rename before the app ships if this is a brand asset.
6. **License of the stills.** They are AI-generated product renders in the repo's `medias/`.
   If they came from an external generator, confirm redistribution terms before baking them
   into firmware (the project is GPL-3.0-or-later, `LICENSE`).

---

## 11. References

**In this repo**

- `examples/glyph-anime/README.md` — the model for this kind of design note, and the
  source of the "approximate the still with primitives" approach.
- `components/ringkit/include/ringkit/ring_theme.h` — panel geometry (360/180).
- `components/viewesmart__smartring_plus/smartring_bsp.c` — `buffer_height = 40`,
  RGB565 + flush byte swap.
- `notes/esp-smartring.md` §6 — display pitfalls (full-screen flush, QSPI + PSRAM).
- `notes/architecture.md` §6 — why examples are CMake fragments, not components
  (so: generated `.c` in `RING_EXAMPLE_SRCS`, not `EMBED_FILES`).

**Vendored LVGL 9.4 (facts verified in the source)**

- `src/draw/lv_image_dsc.h` — `lv_image_dsc_t` layout (`header`, `data_size`, `data`).
- `src/draw/lv_draw_buf.c` — RGB565A8 = colour plane then A8 plane.
- `src/draw/sw/blend/lv_draw_sw_blend_to_rgb565.c` — supported source formats
  (**I8/I4 not drawable**).
- `src/draw/sw/lv_draw_sw_img.c` — transformed draw path (CPU).

**External**

- LVGL image sources (v9.5 docs): variable vs file vs symbol sources, `w/h ≤ 2048`,
  stride; converters. <https://lvgl.io/docs/open/9.5/main-modules/images/sources>
- LVGL image converter: <https://lvgl.io/tools/imageconverter>
- LVGL perf regression on ESP32-S3 v8→v9 (~37 %) and the advice to measure
  `lv_timer_handler` rather than an FPS counter: <https://github.com/lvgl/lvgl/issues/5459>
- PPA (hardware image scale/rotate) is ESP32-P4 only — on S3 all transforms are software:
  <https://github.com/lvgl/lvgl/issues/10260>
- `esp_lv_decoder` (PNG/JPG/QOI/SJPG/SPNG) and `esp_lv_fs` — already vendored via
  `esp_lvgl_adapter`; docs in `managed_components/espressif__esp_lv_decoder/README.md` and
  `.../esp_lv_fs/README.md`.
- Prior art for LVGL face state machines: `0015/lvgl_kawaii_face` (LVGL 9 canvas face),
  `txp666/otto-emoji-gif-component` (21 procedural expressions).

---

## 12. As built (`examples/gbuddy`, 2026-11-04)

### 12.1 What shipped

```
examples/gbuddy/
├── CMakeLists.txt          # gbuddy.c + 10 generated asset .c files
├── gbuddy.c                # ~450 lines: layout, moods, blink, gaze, sleep, battery
├── include/ring_example.h  # the standard contract header
├── README.md               # app-level summary
└── assets/
    ├── rebuild.sh          # regenerates everything below from medias/
    ├── eye_{open,joy,sad}_{l,r}.c   # 118x150 RGB565A8, 53,100 B each
    ├── mouth_{smile,o,frown,grin}.c # 110x80  RGB565A8, 26,400 B each
    └── *.png               # previews (NOTE: pre-feather, see 12.3)
tools/lvgl_img_pack.py      # RGB888(+A8) -> lv_image_dsc_t C file
```

Assets: **424,200 B** (6 eyes + 4 mouths). `smartring.bin` = **1,153,600 B**, 382,912 B
free in the 1500 KiB partition.

### 12.2 Deltas from the plan above

| Plan | Shipped | Why |
|---|---|---|
| vertical gradient background | **flat `#091B36`** | a 222-px RGB565 gradient quantises to ~3 steps and bands visibly on a dark screen (confirmed in a device snapshot) |
| glow blobs behind the eyes (`lv_obj` + shadow) | **removed** | on device they render as dark discs with a visible outline — worse than the problem they solved |
| blink = pre-baked squashed sprite | **swap to the `eye_joy` sprite for 110 ms** | zero extra asset, perfectly aligned, reads as a blink; a photo eye squashed to 18 % looks like a slit |
| eye `scale_y`/`mv` animations | **sprite swaps + `translate_x/y` only** | no transform cost, no pivot arithmetic; `translate` leaves layout coordinates alone |
| sad eye = open pushed down + 0.78× | **real `eye_sad` sprites** (cut from 003) | it fit the budget and is the correct shape |

Kept as designed: fixed crop windows (mood swaps never move the face), feathered mattes,
pointer-free gaze smoothing, `lv_timer` + `lv_anim` on the adapter task, mood cycling on
tap, long-press sleep with a 20 % backlight, and the battery → sad rule.

### 12.3 Findings worth remembering

1. **The feather is what removes the sprite box, not the level.** `-level 12%,45%` on the
   luminance matte plus a 6 px `roundrectangle` feather makes the sprite edge invisible on
   the flat background. While bringing this up the `assets/*.png` previews were written
   *before* the feather, which showed a rectangle the firmware did not have and sent the
   investigation the wrong way for an hour; `rebuild.sh` now re-applies the feathered matte
   to the preview so the PNGs match the firmware.
2. **RGB565 gradients band.** `#0A1C38 → #061124` over 222 px is 3 steps; use a flat fill
   or bake a dithered gradient asset.
3. **`lv_obj` shadows are not a glow.** The shadow is drawn only outside the shape and
   does not continue the interior fill, so `bg_opa` + `shadow_*` reads as a disc with a
   rim. A real soft glow needs a radial gradient (disabled here:
   `CONFIG_LV_USE_DRAW_SW_COMPLEX_GRADIENTS` is off) or a baked asset.
4. **Snapshot without a camera.** Temporarily set `CONFIG_LV_USE_SNAPSHOT=y` in
   `sdkconfig`, call `lv_snapshot_take(obj, LV_COLOR_FORMAT_RGB565)` on the LVGL task and
   hex-dump `buf->data` over the console (64 bytes per line, prefixed by the line index so
   the host can drop lines that a log message interrupts). The host decodes it to RGB and
   you can see exactly what LVGL drew. This is how the three deltas above were found — and
   it is worth reusing for any future UI work.
5. **Snapshotting is slow.** A 360×360 RGB565 dump is 259 KB → ~14 s at console speed, and
   blocking the LVGL task for that long trips the task watchdog. Dump a sub-object
   (`lv_snapshot_take(face, ...)` = 160 KB) and `vTaskDelay(1)` every 64 lines.

### 12.4 Verified

- Boot (device): `app: gbuddy`, `ringkit: display=ok battery=ok power=ok audio=ok sd=ok
  imu=ok`, `gbuddy: mood happy`, `ready: 4 moods, 6 eye + 4 mouth sprites`, no
  panic/assert; stable over 60 s.
- All four moods rendered correctly (verified from device snapshots).
- Both other apps still build (`glyph-anime` 750,576 B, `hello` 823,088 B).
- The AXP2101 `ESP_ERR_TIMEOUT` warning in the log is **pre-existing** (it appears on
  `glyph-anime` too) and is rate-limited by the BSP.

### 12.5 Still needs a human

Blink timing/feel, gaze response to real tilt and twist, sleep/wake, and how the face band
looks inside the round bezel (the snapshot is rectangular and cannot show the physical
cut-out). Touch cannot be synthesised from the host.

# 09_hello_smoke_test — bring-up smoke test

Deliberately the smallest possible UI: a white full-screen background and one
large black "Hello" glyph.

If this renders square, correctly oriented and without stripes, then the panel,
its colour order, the QSPI link, the LVGL adapter task and the backlight are all
sane — so when a richer example looks wrong, the fault is in that example, not
the board.

```
tools/dev.sh build 09_hello_smoke_test
tools/dev.sh flash 09_hello_smoke_test
```

## Implementation

Built entirely from the ringkit UI helpers — no raw LVGL screen setup, no
`bsp_*` calls:

```c
RING_UI_LOCKED(-1) {
    lv_obj_t *scr = ring_ui_screen();
    lv_obj_set_style_bg_color(scr, RING_COLOR_FG, LV_PART_MAIN);   /* white */
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *label = ring_ui_add_label(scr, "Hello",
                                        &lv_font_montserrat_48, RING_COLOR_BG);
    ring_ui_place_centered(label, RING_CX, RING_CY);
}
```

The palette is the theme inverted (`RING_COLOR_FG` background, `RING_COLOR_BG`
glyph) and the geometry comes from `ring_theme.h` (`RING_CX`/`RING_CY`).
`ring_ui_place_centered()` is used instead of `lv_obj_center()` so the glyph is
pixel-exact — the same helper every other example uses.

On success it logs one line with the display facts that matter during
bring-up:

```
I (1946) 09_hello_smoke_test: display 360x360, color format 18, swap_bytes 1 — Hello on screen
```

(a wrong `swap_bytes` value is what turns the screen into vertical stripes).

## Notes

- The white background is the point: it exercises every pixel.
- This is the only app that pulls in `lv_font_montserrat_48`, which is why its
  image is larger than `00_glyph_grid_reveal`'s (~804 KB vs ~733 KB). Not a
  regression — see `AGENTS.md` § Gotchas.

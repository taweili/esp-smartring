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

## Notes

- The white background is the point: it exercises every pixel.
- This is the only app that pulls in `lv_font_montserrat_48`, which is why its
  image is larger than `00_glyph_grid_reveal`'s (~804 KB vs ~733 KB). Not a
  regression — see `AGENTS.md` § Gotchas.

Read this when tuning the panel, changing the boot splash, or checking internal-RAM headroom.

# Display and memory

## Display tuning

The RGB panel configuration in `src/hal/crowpanel.cpp` is Elecrow's own 7.0"
description. Symptoms and where to look if a board is reworked or is a
different revision:

| Symptom | Fix |
|---|---|
| Colours swapped (red shows as blue) | build with `-DDASH_USE_50_PINMAP=1`, or permute `kDataPins` |
| Image shifted / wrapped | `LCD_HSYNC_*` / `LCD_VSYNC_*` porches in `board_config.h` |
| Whole image jitters / shifts | panel DMA starved of PSRAM bandwidth: raise `LCD_BOUNCE_LINES`, then lower `LCD_PCLK_HZ` |
| Image tears or rolls | LVGL is drawing into the buffer being scanned: keep `LCD_USE_DOUBLE_FB 1` and `LV_DISPLAY_RENDER_MODE_DIRECT` |
| Panel blank, no backlight | the PCA9557 was not found — check the I2C scan line in the boot log |

### The real limit is PSRAM bandwidth

Whatever else is tuned, this is the number that decides how the dash behaves.
The panel reads its frame buffer out of PSRAM continuously. On the pre-0.3.0
bring-up build (the firmware had no version string yet), this board measured
only ~35–44 MB/s in total, with ~19 MB/s of that left for the CPU while the
panel is scanning:

| pclk | refresh | panel needs |
|---|---|---|
| 24 MHz | 49 Hz | 37.9 MB/s — starves everything else |
| 20 MHz | 41 Hz | 31.6 MB/s |
| **16 MHz** | **32 Hz** | **25.3 MB/s — what this build uses** |
| 12 MHz | 24.6 Hz | 19.0 MB/s |

(One frame is `htotal x vtotal` = 928 x 525 = 487 200 pixel clocks, so the refresh
is `pclk / 487 200` — which is why the boot line prints `32 Hz` for 16 MHz.)

Two things make it worse. WiFi's buffers are allocated in PSRAM
(`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`, baked into the prebuilt IDF
libraries), so network traffic competes with the panel directly — on the same
pre-0.3.0 build, pausing the HTTP polling alone took the render rate from 16 fps
to 42 fps on the bench.

Symptoms are easy to misread: with a starved DMA the whole image rolls, which
looks like a badly tuned television rather than a software bug.

### Two frame buffers, and DIRECT mode

LVGL renders into one full-screen PSRAM buffer while the RGB peripheral scans
the other, and `esp_lcd_panel_draw_bitmap()` only returns once the swap has
happened at a frame boundary — so a frame is never half old and half new.

The render mode that pairs with it is **DIRECT**: a full-screen buffer, but
only the invalidated rectangles are drawn.
`LV_DISPLAY_RENDER_MODE_FULL` looks tempting but forces a whole 800x480 render
*and* a 768 KB PSRAM write every frame, which both wastes the CPU and starves
the panel.

When the DMA's prefetch falls behind, the peripheral loses sync with the DMA
and the picture shifts. Espressif ships `esp_lcd_rgb_panel_restart()` for it
and `CONFIG_LCD_RGB_RESTART_IN_VSYNC` is enabled in this build, which turns a
permanent shift into a jitter. `LCD_BOUNCE_LINES` is the margin against that:
the driver refills an internal-SRAM bounce buffer from PSRAM in bursts, and
the deeper it is, the longer a WiFi burst can run without emptying it. Two of
them are allocated, so the shipped 24 lines cost 2 x 38 400 B = 76 800 B.
(The `b` console key still toggles between 0 and 16 lines for a quick A/B
comparison; 16 lines would be 2 x 25 600 B.)

The pixel clock is **16 MHz taken from PLL_D2 (240 MHz)**, which it divides by an
exact 15 — `[board] ... 16000000 Hz pclk (32 Hz refresh, PLL_D2/240M, exact)`.
On `PLL_F160M` the same rate needs a fractional divider, and the code falls back
to that rather than leave the screen dark; the exact divider removes any
pixel-clock jitter from the picture. The source is printed at boot, and `p` on
the serial console cycles the clock through 24 → 20 → 16 → 12 MHz at runtime to
test the bandwidth theory live — the first press goes *up* to 24 MHz, which is
right at the outer edge for this panel, so treat anything above 16 as a bench
experiment.

### Where the layout lives

Bar cell size, the big number's row, tile widths and padding are all derived
in `include/theme.h`, so geometry is adjusted in one place. The reasoning
behind the top band is in `docs/redesign-bars.md`, and the mock-up tool that
draws the layout at panel size before it is coded is `tools/mockup_dash.py`.

---

## Fonts

Every value on the dash is drawn with Montserrat. Which sizes are compiled is
decided in `include/lv_conf.h`, which face serves which role in
`include/theme.h`, and the generated faces live in `src/ui/fonts`:

| Face | Role | Source |
|---|---|---|
| montserrat_12 | tile captions | stock LVGL |
| montserrat_14 | sub lines, peaks, lamps, rev-bar ticks, the keyboard | stock LVGL; also `LV_FONT_DEFAULT` |
| montserrat_16 | status bar, lock rows | stock LVGL |
| `lv_font_dash_val_22` | coolant / oil / fuel / lock % | subset of montserrat_22 |
| `lv_font_dash_mode_28` | Haldex mode name, SETTINGS heading | subset of montserrat_28 |
| `lv_font_dash_big_48` | speed value | subset of montserrat_48 |
| `lv_font_dash_rpm_96` | rev digits | Montserrat Bold via `tools/make_fonts.sh` |
| `lv_font_dash_num_56` | boost value | Montserrat Bold via `tools/make_fonts.sh` |
| `lv_font_dash_arrow_52` | indicator block arrows | FontAwesome 4.7 via `tools/make_fonts.sh` |

The three subsets replaced LVGL's stock montserrat_22/28/48 in 0.8.1; the stock
faces cost 147.7 KB more flash than the subsets for glyphs the dash never
draws. They are generated from the same Montserrat-Medium.ttf LVGL builds its
stock faces from, with the options recorded in the stock font headers
(`--no-compress --no-prefilter --bpp 4 --force-fast-kern-format`), so each kept
glyph is **byte-identical** to the stock bitmap — verified glyph for glyph
against LVGL 9.3.0's own font sources with `tools/check_font_subsets.py`. Of
the tool's versions, **1.5.2** reproduces the stock bitmaps exactly; 1.5.3
rasterizes a handful of pixels one alpha step differently, which is why
`tools/make_fonts.sh` takes a separate `LV_FONT_CONV_SUBSETS`. What each
subset carries:

| Face | Code points | Draws |
|---|---|---|
| `lv_font_dash_big_48` | `0-9`, `-` | speed `%d`, `--` |
| `lv_font_dash_val_22` | `0-9`, `-`, `.`, `%`, `C`, `F`, `L`, space, `°` | `79°C` / `175°F`, `49.6 L`, `63%`, `--` |
| `lv_font_dash_mode_28` | `0-9`, `:`, `C D E F G I K N O P R S T W X`, `-` | `STOCK FWD 50:50 60:40 75:25 EXPERT`, `SETTINGS`, `--` |

The FontAwesome symbol range the stock faces also carry is dropped: none of
those roles ever draws an `LV_SYMBOL_*`, and the keyboard's symbols come from
montserrat_14, which stays stock. One caveat the generator will not tell you
about: `lv_font_conv` derives `line_height` and `base_line` from the glyphs
kept, and digit sets have no descenders, so the generated metrics collapse
(48 px: line 34, base 0 against the stock 52/9) and every label would sit
several px higher than today — the tile geometry assumes the stock baselines
(the shared-baseline note in `dash_ui.cpp` explains one such placement).
`tools/make_fonts.sh` pins them back (52/9, 24/4, 30/5) after generating. A
new character on one of these roles therefore means: add its code point to
the `-r` list in `tools/make_fonts.sh`, regenerate, and re-run
`tools/check_font_subsets.py`.

---

## LVGL's feature set

LVGL 9.3.0 defaults to compiling in nearly every widget, both layout engines
and every colour-format blend path; the dash's `include/lv_conf.h` overrides
that to exactly what the UI draws, with the reason written beside each define.
The compiled-in set is `lv_label`, `lv_bar`, `lv_button`, `lv_textarea`,
`lv_keyboard` (which creates an `lv_buttonmatrix` internally), `lv_image` and
plain `lv_obj` — no flex, no grid, no other widget — rendering RGB565 with A8
glyph masks, and RGB565A8 / ARGB8888 / premultiplied ARGB8888 kept for a
blended layer, should one ever be introduced.

Adding a widget therefore means: enable it — and anything the comment beside
its define says it needs — in `include/lv_conf.h`, rebuild, check the flash
line, and verify the widget on the glass. Using a widget without enabling it
is a compile error; relying on a default that is not written down is the
mistake this configuration exists to prevent. The same applies to images,
transforms, opacity and clipping: they can pull in intermediate colour formats
even though the panel is RGB565, so check the `LV_DRAW_SW_SUPPORT_*` block in
the same header before assuming the renderer can draw it.

---

## The boot splash's picture

The splash is a pre-rendered 800x480 JPEG (`assets/candash-splash-base-92.jpg`,
30 395 B) with the progress rule, the tagline and the version drawn over it by
LVGL. It is put in the app image, not the file system, by

```ini
board_build.embed_files = assets/candash-splash-base-92.jpg
```

which the linker turns into `_binary_assets_candash_splash_base_92_jpg_start`
/ `_end` symbols ([`src/ui/splash_img.cpp`](../src/ui/splash_img.cpp) is the only
thing that refers to them; without a reference the linker drops the data again).
In the app image rather than LittleFS on purpose: it can then be replaced by an
over-the-air update, and the OTA page cannot write LittleFS.

Decoding is the **ESP32-S3 mask ROM's** TJpgDec, reached through
`esp32s3/rom/tjpgd.h`. The S3 has no JPEG peripheral (`esp_driver_jpeg` is gated
on `CONFIG_SOC_JPEG_CODEC_SUPPORTED`, which the S3 does not set) and linking a
decoder would cost tens of KB of a slot that is 92 % full, but the ROM ships the
algorithm for free — `jd_prepare` and `jd_decomp` are in `esp32s3.rom.ld`. The
ROM's build is fixed at RGB888 output, so each MCU band is converted to RGB565 as
it arrives and a full-size RGB888 image is never materialised. Measured on the
board: 800x480 in **416 ms**, 768 000 B of PSRAM held for the 3 s the splash is up
and freed with it.

The rule is the picture's own, at y 284, x 210-589 (380x3). Its gradient is fixed
across the full 380 px and *revealed*, not stretched: a 1 px-wide parent is grown
to 380 px and clips a child that is always full width. A child that grew instead
would recompute the gradient over its own width, so the colour at the leading edge
would crawl as the bar filled.

---

### Memory, exactly

The ESP32-S3 has **512 KB of SRAM**, but only 327 680 B (320 KB) of it is
addressable as the application's DRAM region — the rest is IRAM and ROM-reserved.
So the `327 680` in PlatformIO's `RAM:` line is the DRAM the application can
actually run out of, and reporting a percentage of it (`17.9 %` in 0.6.0) is
normal for this chip, not a sign that something is missing.

The 8 MB of **PSRAM** is the part that matters here: the two 800x480x16 bpp
frame buffers are 1 536 000 B and they must live there. The build confirms the
`qio_opi` variant is linked (`CONFIG_SPIRAM_MODE_OCT 1`).

### Internal RAM

Internal RAM is what runs out first: WiFi, ESP-NOW and the task stacks all have to
have it, while PSRAM has megabytes free. Read from the 0.5.0 build's `firmware.elf`,
with the 0.5.1 moves to PSRAM marked:

| What | Internal RAM | |
|---|---|---|
| Panel bounce buffers | 76.8 KB | has to be internal; `LCD_BOUNCE_LINES` in `board_config.h` |
| Code that must run from RAM | 67 KB | framework; WiFi and lwIP's speed-ups are already off in this build |
| LVGL's pool | 256 KB (248 472 B usable) | **in PSRAM since 0.5.1** — it was 96 KB of internal RAM in 0.5.0 |
| CAN relay tables | 7.6 KB | **in PSRAM since 0.5.1** |
| Everything else fixed | ~58 KB | framework, WiFi, the other small variables |

Also since 0.5.1: ordinary memory requests of 512 B or more come from PSRAM first,
and the JSON from `/api/live` and `/api/dashboard` is parsed in PSRAM.

Every 5 s the console prints, on the 0.6.0 board with the panel up and no
controller in range (WiFi down, relay carrying nothing — the exact byte counts
move by a few hundred between boots):

```
[ram] internal 163476 B free, lowest 162904 B, biggest piece 122868 B | stack spare: ui 3332 B, net 6936 B
[ram] lvgl pool 46% of 248472 B used, peak 114988 B, biggest piece 134804 B
```

**lowest** is the one to watch: of the 320 KB DRAM the application starts with, the
panel's bounce buffers are allocated first, and 163 KB is what is left for WiFi,
ESP-NOW, the two task stacks and every runtime allocation. A failed request prints
`[ram] OUT OF MEMORY ...` at once, says so in the 5 s report, and is reported again
at the next boot if it ended in a reset.

To undo the pool move, set `DASH_LVGL_POOL_IN_PSRAM` to 0 in `include/lv_conf.h`. Do
that if the render rate drops noticeably, since widgets are slower to reach in PSRAM.
The pool is 256 KB and the bench peak was 115 KB, so it could be cut to suit if PSRAM
is ever tight enough to matter — it is not today.

If LVGL ever does run out - it has its own pool, so the warning above does not cover
it - the panel no longer goes quiet about it. `src/app/lv_assert_glue.cpp` replaces
LVGL's stock assert handler, which is a bare `while(1);`, with one that prints the
file and line, a backtrace, and then reboots:

```
[lvgl] *** ASSERT at src/misc/lv_mem.c:123 ***
```

That is the difference between a fault and a mystery: the CAN DATA page hanging on
open and on close looked exactly like this before the pool moved to PSRAM.

### One caveat worth knowing

The precompiled Arduino/IDF libraries in this platform were built assuming
`CONFIG_ESPTOOLPY_FLASHSIZE="16MB"`. This board has 4 MB. That is harmless in
practice — every partition is inside the first 4 MB, `board_upload.flash_size`
writes the correct size into the app image header, and esptool detects the real
chip — but if a future change ever needs libs compiled for exactly 4 MB, it
requires `custom_sdkconfig` *plus* a project-local `core_dir`, because
rebuilding the libs writes into the shared packages folder.

---

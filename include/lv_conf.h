/**
 * @file lv_conf.h
 * LVGL v9 configuration for the OpenHaldex S3 dash.
 *
 * Deliberately partial: LVGL's lv_conf_internal.h fills in a default for
 * every symbol that is not defined here, so this file only lists the
 * settings that differ from stock.
 *
 * Selected by -DLV_CONF_INCLUDE_SIMPLE (see platformio.ini).
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   COLOUR / RENDERING
 *====================*/
/* The RGB panel is wired as RGB565 and LovyanGFX hands LVGL its own
   16-bit frame buffers, so LVGL must render RGB565 too. */
#define LV_COLOR_DEPTH 16

/* 800 px * 2 bytes = 1600 B per line, already 4-byte aligned; keep LVGL
   from choosing a stride that the LCD DMA engine cannot scan. */
#define LV_DRAW_BUF_STRIDE_ALIGN 4

/* The software blender compiles a blend path per colour format, both as a
   destination and as an image source. The panel is RGB565, and the only image
   the dash ever draws is the splash JPEG decoded to RGB565
   (src/ui/splash_img.cpp), so every other source format is dead weight. Kept:
   RGB565 (the screen), RGB565A8 and ARGB8888 / ARGB8888_PREMULTIPLIED (the
   formats LVGL would pick for a blended layer if one were ever introduced),
   and A8, which is what glyph masks are drawn through - not an image path at
   all, and text would break without it. All of these default to 1
   (lv_conf_internal.h), so each one is written down either way. */
#define LV_DRAW_SW_SUPPORT_RGB565 1
#define LV_DRAW_SW_SUPPORT_RGB565A8 1
#define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED 0
#define LV_DRAW_SW_SUPPORT_RGB888 0
#define LV_DRAW_SW_SUPPORT_XRGB8888 0
#define LV_DRAW_SW_SUPPORT_ARGB8888 1
#define LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED 1
#define LV_DRAW_SW_SUPPORT_A8 1
#define LV_DRAW_SW_SUPPORT_L8 0
#define LV_DRAW_SW_SUPPORT_AL88 0
#define LV_DRAW_SW_SUPPORT_I1 0

/*====================
   MEMORY
 *====================*/
/* ** CAN CHANGE THIS ** Where LVGL keeps its memory: every widget on every page,
   plus the temporary buffers it draws text through.
   1 = a 256 KB pool in PSRAM (0.5.1). Hands 96 KB of internal RAM back to WiFi and
       the tasks. can2gauge moved its pool for the same reason (2026-10-14), and
       later grew it because 96 KB ran out (0.31.0).
   0 = the old 96 KB pool in internal RAM. Widgets are a little faster to reach, so
       go back to this if [stats] render fps drops noticeably on the glass.
   [ram] lvgl pool on the serial console shows how much of it is really in use. */
#define DASH_LVGL_POOL_IN_PSRAM 1

/* LVGL's own memory manager, so its use of memory stays predictable. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#if DASH_LVGL_POOL_IN_PSRAM
#define LV_MEM_SIZE (256U * 1024U)
#define LV_MEM_POOL_INCLUDE <esp_heap_caps.h>  /* for heap_caps_malloc below */
/* LVGL calls this once, in lv_init(), to get its pool. The panel has already taken
   its frame buffers from PSRAM by then, so PSRAM is known to be working. */
#define LV_MEM_POOL_ALLOC(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define LV_MEM_SIZE (96U * 1024U)
#endif

/* An LVGL assert stops the whole panel. The stock handler is a bare `while(1)`
   and LV_USE_LOG is off, so anything that asserts - most importantly
   LV_ASSERT_MALLOC, when the pool above runs out - shows up as a dark screen, a
   silent console and no clue which allocation failed. Print where it happened,
   with a backtrace, then reboot: a fault on the console instead of a hang with
   nothing to go on. src/app/lv_assert_glue.cpp implements it. */
#ifndef __ASSEMBLER__  /* lv_conf.h is included by LVGL's .S files too */
void ohx_lv_assert_handler(const char *file, int line);
#endif
#define LV_ASSERT_HANDLER ohx_lv_assert_handler(__FILE__, __LINE__);

/*====================
   HAL / TICK
 *====================*/
/* lv_tick_set_cb(millis) is installed in board_display.cpp instead, so the
   HAL tick source here is unused. */
#define LV_USE_OS LV_OS_NONE

/*====================
   LOGGING
 *====================*/
#define LV_USE_LOG 0

/*====================
   FONTS
 *====================*/
/* Only the Montserrat sizes the dashboard actually uses are compiled in;
   every extra size costs flash. 18 px has no user anywhere in the UI.
   22/28/48 px are replaced by subsets of those very faces, generated from the
   same Montserrat-Medium.ttf with the options LVGL itself used, so the kept
   glyphs are byte-identical to these stock ones (verified in
   tools/make_fonts.sh). The subsets live in src/ui/fonts and are bound in
   theme.h; they carry only the code points each role draws and drop the
   FontAwesome symbol range, which only the keyboard's montserrat_14 needed. */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_48 0

#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* The two big numbers (src/ui/fonts) are RLE-compressed: without this,
   lv_font_get_glyph_bitmap() returns NULL for every glyph of a compressed
   font and the letter drawing code dereferences it. That is a hard fault,
   not a fallback, which is exactly what it did. */
#define LV_USE_FONT_COMPRESSED 1

/*====================
   WIDGETS
 *====================*/
/* The UI builds with exactly these: lv_label (every value and caption),
   lv_bar (rev bar and gauges), lv_button + lv_textarea + lv_keyboard (the
   settings page), lv_image (the splash picture) and lv_obj (cards, the
   settings sheet, list rows). lv_buttonmatrix is what the keyboard creates
   internally. Every LVGL widget not listed here is compiled out - the stock
   default for any symbol not defined is 1 (lv_conf_internal.h), so leaving
   them undefined would still link their code and styles. */
#define LV_USE_LABEL 1
#define LV_USE_BAR 1
#define LV_USE_BUTTON 1
#define LV_USE_TEXTAREA 1
#define LV_USE_KEYBOARD 1
#define LV_USE_BUTTONMATRIX 1  /* required by the keyboard */
#define LV_USE_IMAGE 1         /* the splash picture */

/* Not used by this UI - each one costs flash. */
#define LV_USE_ANIMIMG 0
#define LV_USE_ARC 0
#define LV_USE_CANVAS 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_LED 0
#define LV_USE_LINE 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 0
#define LV_USE_ROLLER 0
#define LV_USE_SCALE 0
#define LV_USE_SLIDER 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_SWITCH 0
#define LV_USE_TABVIEW 0
#define LV_USE_TABLE 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0
#define LV_USE_CALENDAR 0
#define LV_USE_CHART 0
#define LV_USE_COLORWHEEL 0
#define LV_USE_OBJ_ID 0

/* Neither layout engine is used: every widget is placed with absolute pixel
   coordinates from include/theme.h. Checked against LVGL 9.3.0 sources - the
   kept widgets (label/bar/button/textarea/keyboard/image/theme default) and
   lv_obj.c never call flex or grid functions, and neither does this UI. */
#define LV_USE_FLEX 0
#define LV_USE_GRID 0

/*====================
   TIMING
 *====================*/
/* LVGL's own refresh timer. It only redraws when something has been
   invalidated, so this is a ceiling rather than a guaranteed frame rate. */
#define LV_DEF_REFR_PERIOD 16

#endif /* LV_CONF_H */

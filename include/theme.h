// =====================================================================
// theme.h - colours, fonts and the layout model
//
// The palette is a direct port of dash.html (the browser reference), and so
// is the tile band. dash.html sized everything from one unit:
//
//     --u = min(viewport_w / 100, viewport_h / 56)
//
// At 800x480 that gives u = 8 px, which is where the tile padding comes from.
//
// The top band is no longer the ported tach ring: the ring, needle and hub
// cost ~58 ms a frame on this board and were replaced by a bar, which is a
// redesign rather than a port (see docs/redesign-bars.md and the gauge
// section below).
// =====================================================================
#pragma once

#include <lvgl.h>

#include "board_config.h"

// ------------------------------------------------------------- palette
// Every colour on the glass is a *role*, not a hex value: a scheme is a table
// of role -> colour, and switching scheme re-points the roles. Midnight is the
// palette dash.html was ported from, and it is what every build before 0.9.4
// drew, so it stays the first entry and the default.
//
// The neutrals below are shared by every scheme on purpose. The panel is a
// 16-bit TN RGB panel read at a glance, and a scheme that moved the black
// would lose the contrast the dashboard is designed around; only the accent
// family moves. That is also the shape of the reference gauge's themes
// (ESP32-S3-Gauge, src/ui/theme.cpp: one accent per scheme).
//
// Two rules the non-Midnight schemes follow:
//   warn  is a lighter shade of the scheme's own accent, never amber. In a
//         green dash an amber number reads as a different system rather than
//         as a hotter version of the same reading.
//   crit  is the accent as a solid plate with contrasting text (critText).
//         Outside Midnight there is no red: "maximum" is shown by inverting
//         the scheme, which cannot be confused with the warn shade.
namespace theme {

enum class Scheme : uint8_t { Midnight = 0, Orange, Green, Blue, Purple, White, Count };

constexpr uint8_t kSchemeCount = (uint8_t)Scheme::Count;

/// The semantic roles. A palette is one colour per role; COL_* below are the
/// names the rest of the UI uses, so a role is renamed in one place.
enum class Role : uint8_t {
  Bg,        // the screen behind everything
  Panel,     // status bar, raw-page cards
  Line,      // rules, tile separators, card borders
  Fg,        // values, the big number, the mode
  Dim,       // captions, units, "peak", "--"
  Accent,    // "normal" values, live bars, the LIVE dot
  AccentDim, // cruise lamp "armed" letters
  AccentDimmer,  // cruise lamp "armed" plate
  Warn,      // warning values and the stale dot
  Crit,      // critical values, plates and the redline flash
  CritText,  // text drawn on a Crit plate
  AccentText,  // text drawn on an Accent plate (the cruise lamp when it regulates)
  Cyan,      // the second reading (lock actual)
  Shift,     // the shift flash
  Track,     // unfilled bar background
  LampOff,
  LampEdge,
  LampTxtOff,
  ZoneAccent,  // unlit rev-bar tints
  ZoneWarn,
  ZoneCrit,
  White,       // the big rev digits, boost bar fill, raw-page heads
  ButtonFill,
  FieldFill,
  PageBg,
  SoftLabel,
  Count
};

/// One scheme. The neutrals are the defaults (dash.html's :root values); a
/// scheme therefore only has to name the accent family it changes.
struct Palette {
  uint32_t bg = 0x000000;
  uint32_t panel = 0x0b0d10;
  uint32_t line = 0x1c2128;
  uint32_t fg = 0xe8edf2;
  uint32_t dim = 0x7c8894;
  uint32_t white = 0xf2f5f8;
  uint32_t track = 0x14181d;
  uint32_t lampOff = 0x0e1114;
  uint32_t lampEdge = 0x2a2f36;
  uint32_t lampTxtOff = 0x3c444d;
  uint32_t buttonFill = 0x12161b;
  uint32_t fieldFill = 0x0e1114;
  uint32_t pageBg = 0x080a0d;
  uint32_t softLabel = 0xc6ced6;

  // --- the accent family: the only part a scheme changes ---
  uint32_t accent;
  uint32_t accentDim;
  uint32_t accentDimmer;
  uint32_t zoneAccent;
  uint32_t warn;
  uint32_t zoneWarn;
  uint32_t crit;
  uint32_t critText;
  // Text on an accent plate. The same colour as critText in every scheme but
  // Midnight, where the accent is green (black letters) and the crit plate is
  // red (white letters).
  uint32_t accentText = 0x000000;
  uint32_t zoneCrit;
  uint32_t cyan;
  uint32_t shift;
};

/// A colour together with the role it came from. The role is what lets a
/// build-time colour be attached as a shared style, so a scheme change
/// repaints it without walking the tree. A computed colour (a value's
/// warn/crit colour, say) has no role and is written locally - which is
/// correct, because the code that computes it re-computes it every pass.
struct Col {
  lv_color_t v;
  Role role;
  constexpr Col(lv_color_t c) : v(c), role(Role::Count) {}
  constexpr Col(Role r, lv_color_t c) : v(c), role(r) {}
  operator lv_color_t() const { return v; }
};

Scheme scheme();
const char *schemeName(Scheme s);
Scheme schemeAt(uint8_t i);

/// The current palette's colour for a role.
Col c(Role role);

/// Add a role colour to an object as a shared style, so setScheme() repaints
/// it. A colour without a role (a computed one) is written as an ordinary
/// local property instead, exactly as before.
void styleText(lv_obj_t *obj, Col col);
void styleBg(lv_obj_t *obj, Col col, lv_style_selector_t sel = 0);
void styleBorder(lv_obj_t *obj, Col col, lv_style_selector_t sel = 0);

/// Switch palette and repaint. True if the scheme actually changed. The UI
/// wraps this in ui::applyTheme(), which is also where the choice is stored.
bool setScheme(Scheme s);

}  // namespace theme

// The names the UI already uses. Each is a role lookup, so a scheme change is
// visible to the next call without touching a call site.
#define COL_BG theme::c(theme::Role::Bg)
#define COL_PANEL theme::c(theme::Role::Panel)
#define COL_LINE theme::c(theme::Role::Line)
#define COL_FG theme::c(theme::Role::Fg)
#define COL_DIM theme::c(theme::Role::Dim)
#define COL_GREEN theme::c(theme::Role::Accent)
#define COL_GREEN_DIM theme::c(theme::Role::AccentDim)  // lamp "armed" state: on, but not doing anything
// The cruise lamp's "armed" background, taken much darker than COL_GREEN_DIM so
// a switch that is merely on cannot compete with the lamp that means the car is
// actually holding a speed. Deliberately a colour, not an opacity: lampPaint()
// compares packed colours to decide whether a repaint is needed. The panel is
// 16-bit, so this lands as (0, 40, 16); 0x0a3d1f landed as (8, 60, 24), which on
// the glass is an ordinary dark green rather than a hint. The armed letters are
// COL_GREEN_DIM (see lampStateCruise) - white on this plate was the brightest
// thing in the status bar, which inverted the hierarchy the comment above is
// about.
#define COL_GREEN_DIMMER theme::c(theme::Role::AccentDimmer)
#define COL_AMBER theme::c(theme::Role::Warn)
#define COL_RED theme::c(theme::Role::Crit)
#define COL_CRIT_TEXT theme::c(theme::Role::CritText)
#define COL_ACCENT_TEXT theme::c(theme::Role::AccentText)
#define COL_CYAN theme::c(theme::Role::Cyan)
#define COL_SHIFT theme::c(theme::Role::Shift)  // the rev bar's shift flash
#define COL_TRACK theme::c(theme::Role::Track)  // unfilled bar background
#define COL_LAMP_OFF theme::c(theme::Role::LampOff)
#define COL_LAMP_EDGE theme::c(theme::Role::LampEdge)
#define COL_LAMP_TXT_OFF theme::c(theme::Role::LampTxtOff)
#define COL_ZONE_GREEN theme::c(theme::Role::ZoneAccent)  // unlit zone tints
#define COL_ZONE_AMBER theme::c(theme::Role::ZoneWarn)
#define COL_ZONE_RED theme::c(theme::Role::ZoneCrit)
#define COL_WHITE theme::c(theme::Role::White)  // the big rev digits, boost tile bar
#define COL_BUTTON_FILL theme::c(theme::Role::ButtonFill)
#define COL_FIELD_FILL theme::c(theme::Role::FieldFill)
#define COL_PAGE_BG theme::c(theme::Role::PageBg)
#define COL_SOFT_LABEL theme::c(theme::Role::SoftLabel)

// --------------------------------------------------------------- fonts
// montserrat_12/14/16 are the stock LVGL faces, enabled in lv_conf.h. The
// larger sizes are subsets of the same Montserrat-Medium faces, generated by
// tools/make_fonts.sh: same rasterizer options as LVGL's own build of the
// stock faces, so every kept glyph is byte-identical to the stock bitmap and
// the stock line metrics (52/9, 24/4, 30/5) are pinned afterwards - those
// baselines are what the tile geometry assumes. Only the code points each
// role draws are in the subset; anything else would draw nothing.
#define FONT_CAP (&lv_font_montserrat_12)     // tile captions
#define FONT_SMALL (&lv_font_montserrat_14)   // peaks, sub lines, lamps
#define FONT_BODY (&lv_font_montserrat_16)    // status bar, lock rows
#define FONT_TICK (&lv_font_montserrat_14)    // numbers under the rev bar

// The two big numbers come from Montserrat Bold. LVGL ships Montserrat only
// up to 48 px, so they are generated from the TTF into src/ui/fonts by
// tools/make_fonts.sh. Only the glyphs they need are in there (digits, '-',
// '.'), which is why 96 px costs 43 KB rather than the ~150 KB a full size
// would. '-' is what draws "--" for a value that has never arrived.
//
// There was a third: an 80 px face for the boot splash's "CAN DASH". The splash
// is a pre-rendered picture since 0.6.2 and the wordmark is in it, so the face
// went with it - it had no other user, and it was 2.8 KB of flash.
LV_FONT_DECLARE(lv_font_dash_rpm_96);
LV_FONT_DECLARE(lv_font_dash_num_56);
LV_FONT_DECLARE(lv_font_dash_arrow_52);
LV_FONT_DECLARE(lv_font_dash_val_22);
LV_FONT_DECLARE(lv_font_dash_mode_28);
LV_FONT_DECLARE(lv_font_dash_big_48);
#define FONT_RPM (&lv_font_dash_rpm_96)       // the big rev readout
#define FONT_NUM (&lv_font_dash_num_56)       // the boost tile's value
#define FONT_TEMP (&lv_font_dash_val_22)      // coolant / oil / fuel / lock %
#define FONT_MODE (&lv_font_dash_mode_28)     // Haldex mode name, SETTINGS
#define FONT_BIG (&lv_font_dash_big_48)       // speed value
// The indicator repeaters: two solid block arrows (FontAwesome 4.7 U+F060/F061).
// LVGL ships no filled arrow - LV_SYMBOL_LEFT/RIGHT are chevrons - so the two
// glyphs are converted from FontAwesome into a face of their own.
#define FONT_ARROW (&lv_font_dash_arrow_52)

// ------------------------------------------------------------- layout
// All in pixels, evaluated once for the 800x480 panel.
namespace layout {

// --u = min(w/100, h/56) = min(8.0, 8.571) = 8
constexpr int U = (LCD_H_RES / 100 < LCD_V_RES / 56) ? (LCD_H_RES / 100) : (LCD_V_RES / 56);

constexpr int W = LCD_H_RES;
constexpr int H = LCD_V_RES;

// #status is flex 0 0 auto at height 2.8u, and #root is a column of
// [#top 56%, #bottom 44%, #status]. So the status bar is taken off first.
// dash.html gives 2.8u = 22 px here; that is a 22 px touch target on a
// 7" panel, so the bar is 30 px instead and the band above it absorbs the
// difference. Everything below is derived, so nothing else needs editing.
constexpr int STATUS_H = 30;                          // was 2.8u = 22
constexpr int CONTENT_H = H - STATUS_H;               // 450

// The band under the rev bar: the four original tiles, at the widths
// dash.html gives them (186 / 186 / 204 / 224 = 800). BOOST is one of them
// again - only REVS is full width.
constexpr int TILE_Y = 230;
constexpr int TILE_H = CONTENT_H - TILE_Y;            // 220

constexpr int COL0_X = 0;                             // SPEED
constexpr int COL0_W = (W * 186) / 800;               // 186
constexpr int COL1_X = COL0_X + COL0_W;               // 186
constexpr int COL1_W = COL0_W;                        // 186  BOOST
constexpr int COL2_X = COL1_X + COL1_W;               // 372
constexpr int COL2_W = (W * 204) / 800;               // 204  TEMPERATURES
constexpr int COL3_X = COL2_X + COL2_W;               // 576
constexpr int COL3_W = W - COL3_X;                    // 224  HALDEX

// .tile padding: 0.9u vertical, 1.1u horizontal
constexpr int TILE_PAD_V = (U * 9) / 10;  // 7
constexpr int TILE_PAD_H = (U * 11) / 10; // 8

// ---------------------------------------------------------------- gauge
// One bar across the whole panel, with the number it belongs to on a row of
// its own underneath. It replaces the old tach ring, needle and 9 ticks: a
// bar's moving edge is an opaque rectangle, so a change repaints only the
// cells that crossed the edge instead of a 279 px anti-aliased arc.
//
// The bar is BAR_SEG opaque cells, drawn edge to edge so it reads as one
// continuous bar. 80 cells over the full 800 px is 10 px per cell: the moving
// edge lands within 5 px of the real fraction (20 px cells were visibly steppy),
// and because a change repaints only the cell that crossed the edge, narrower
// cells are cheaper, not dearer (about 1k px per step against 1.9k). It also
// makes a cell exactly 100 rpm on the default 8000 scale, which is the step the
// settings page uses.
constexpr int BAR_X = 0;
constexpr int BAR_W = W;                  // 0 .. 800: the only full-width item
constexpr int BAR_SEG = 80;

constexpr int REV_BAR_Y = 6;
constexpr int REV_BAR_H = 104;            // fat: it is the one the eye tracks
constexpr int TICK_ROW_Y = 114;           // numbers under the rev bar
constexpr int TICK_ROW_H = 18;

// The big number gets its own row, centred, in the 96 px Montserrat Bold the
// bar's scale is read against. Its label is line-height tall, so the glyphs
// stay centred if the font size ever changes.
constexpr int RPM_NUM_Y = 132;
constexpr int RPM_NUM_H = TILE_Y - RPM_NUM_Y;  // 98

// ---------------------------------------------------------- tile band
// The tile band is the one part of the dashboard whose shape can change.
// Two geometries exist, both approved as mock-ups:
//
//   Full     SPEED | BOOST | TEMPERATURES | HALDEX, the widths dash.html
//            gives them (186 / 186 / 204 / 224) - docs/mockups/B
//   NoBoost  SPEED | TEMPERATURES | HALDEX, three equal panels - mock-up A,
//            for a car whose owner turns the BOOST panel off
//
// Which one is used follows the Boost tile setting (config::Data::boostTile),
// rather than a capability decision. It is changed from the Settings page; the
// view is re-placed, not rebuilt.
enum class Tile : uint8_t { Speed, Boost, Temps, Haldex };

struct TileCol {
  Tile role;
  int x, w;
};

struct TileBand {
  const TileCol *cols;
  int count;
  const char *name;
};

constexpr TileCol kTilesFull[] = {
    {Tile::Speed, COL0_X, COL0_W},
    {Tile::Boost, COL1_X, COL1_W},
    {Tile::Temps, COL2_X, COL2_W},
    {Tile::Haldex, COL3_X, COL3_W},
};

// Three equal panels: 266 / 266 / 268, so the third takes the rounding
// remainder and the band still ends exactly at the panel edge.
constexpr int NOBOOST_W = W / 3;
constexpr TileCol kTilesNoBoost[] = {
    {Tile::Speed, 0, NOBOOST_W},
    {Tile::Temps, NOBOOST_W, NOBOOST_W},
    {Tile::Haldex, 2 * NOBOOST_W, W - 2 * NOBOOST_W},
};

enum class Variant : uint8_t { Full, NoBoost };

constexpr TileBand kBandFull{kTilesFull, 4, "Full (B)"};
constexpr TileBand kBandNoBoost{kTilesNoBoost, 3, "NoBoost (A)"};

inline const TileBand &bandFor(Variant v) {
  return v == Variant::NoBoost ? kBandNoBoost : kBandFull;
}

}  // namespace layout

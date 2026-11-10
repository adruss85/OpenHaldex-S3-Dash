// =====================================================================
// dash_ui.cpp - the LVGL dashboard
//
// Widget map (see theme.h for the numbers):
//
//   +----------------------------------------------------------------+
//   | [###############################################.............]  |  REVS bar
//   |  1    2    3    4    5    6    7                     x1000      |  full width
//   |                        6800                                     |  big number
//   +---------+----------+--------------+---------------------------+
//   | SPEED   | BOOST    | TEMPERATURES | HALDEX                    |  4 tiles
//   | 123 km/h| 1.55 bar | cool / oil   | mode, lock req/act, thr   |
//   +---------+----------+--------------+---------------------------+
//   | * LIVE  BRAKE HAND TC ASR CC rpm peak  RESET PEAKS DAY SETTINGS|  status bar
//   +----------------------------------------------------------------+
//
// The top band used to be a tach ring (3 anti-aliased arcs, 9 ticks, a
// needle and a hub). On this board that cost ~58 ms of a 61 ms frame - it
// was the entire frame budget - so it was replaced by a bar: a bar's moving
// edge is an opaque rectangle, and only the cells that crossed it are
// repainted. REVS is the only full-width item and boost is a tile again, so
// the same value never appears twice. See docs/redesign-bars.md.
//
// Values come from net/datafeed.h, which merges the C6's ESP-NOW broadcast
// with its HTTP API, and nothing is drawn for a value that has never
// arrived ("--" instead of 0).
//
// Sections, by the function that starts each one (line numbers would rot):
//
//   helpers                 setText, setBar, colFor, plain/opaque/label,
//                           bar, button, lamp, lampPaint
//   mode switch             cycleMode, onHaldexTap, updateModeCmd
//   gauge / rev bar         syncGauge, buildGauge, buildIndicatorArrow,
//                           setIndicator, updateRevBar, updateGaugeText
//   tiles / band placement  buildTiles, placeSpeedTile/placeBoostTile/
//                           placeTempsTile/placeHaldexTile, placeBand
//   status bar / lamps      buildStatusBar, lampState, lampStateCruise,
//                           updateConn
//   settings page           buildSettings, addStepRow, addField, taEvent,
//                           refreshSteppers, the on* handlers, showSettings
//   boot splash             buildSplash, updateSplash, hideSplash
//   update loop             updateReadouts, updateDayNight, tick
//   public entry points     begin, applyLayout, applyDebugMask, setDebugMask,
//                           takeTimings, staleEvents, maxStaleAgeMs, setPage,
//                           page, layoutVariant, cycleHaldexMode,
//                           settingsOpen
// =====================================================================
#include "ui/dash_ui.h"

#include <Arduino.h>
#include <cmath>
#include <cstring>

#include "hal/crowpanel.h"
#include "net/telemetry.h"
#include "net/ohx_now.h"
#include "net/datafeed.h"
#include "app/config.h"
#include "app/logbook.h"
#include "app/ota.h"
#include "theme.h"
#include "ohx_proto.h"
#include "version.h"
#include "ui/can_ui.h"
#include "ui/raw_ui.h"
#include "ui/splash_img.h"

namespace ui {
namespace {

namespace L = layout;

constexpr float kPi = 3.14159265358979f;
constexpr float kTau = 0.11f;  // interpolation time constant, as in dash.html

// The shift flash and the traction-intervention flash, at 4 Hz. Slower than the
// 60 ms it was (8.3 Hz): on a 32 Hz panel a 125 ms phase is exactly 8 refreshes,
// so there is no beating and no half-painted phase, and a 96 px bar pulsing at
// 8 Hz was a strobe rather than a cue (#10).
constexpr uint32_t kFlashMs = 125;

// The indicator repeaters blink at the car's own rate, not the shift light's.
// A VAG indicator flashes at about 1.5 Hz, so a 333 ms half-period matches what
// the cluster beside it is doing; the earlier 125 ms (4 Hz) read as a strobe.
// Kept separate from kFlashMs because the shift cue and a turn signal are not
// the same cue and must be tunable apart (#5).
constexpr uint32_t kIndicatorFlashMs = 333;
// How long a seen indicator bit keeps the repeater alive. A VW indicator's dark
// phase is ~330 ms, so a 1 s latch rides through it, and a lost frame (the relay
// is 9/s and lossy) cannot cancel a signal that is still going.
constexpr uint32_t kIndicatorHoldMs = 1000;

// The repeater glyph box. FONT_ARROW is a 52 px face whose arrow glyphs measure
// 44 x 46, so the box is that plus a little slack for the label to centre in.
constexpr int kArrowW = 56;
constexpr int kArrowH = 56;

// Fuel smoothing (#4): the tank sloshes, so the displayed level is a 5 s
// exponential low-pass (kFuelTau) and a sample that would move it faster than
// 8 l/s is dropped rather than latched. The car's own low-fuel warning is NOT
// smoothed - it is read straight off the frame.
constexpr float kFuelTau = 5.0f;
constexpr float kFuelMaxRate = 8.0f;  // l/s

const char *const kModeNames[6] = {"STOCK", "FWD", "50:50", "60:40", "75:25", "EXPERT"};

// ------------------------------------------------------ mode switch state
// Tap the HALDEX panel to step the mode on. One tap, one step, no list and no
// modal: the panel already shows the mode, so the tap's effect is visible in
// the next packet. The request goes out over ESP-NOW and the controller
// answers in the ack fields of its next status broadcast - it never replies
// directly - so this small state machine is what turns that into a line on
// the panel.
//
// The open-access-point warning in settings is a warning, not a gate. An
// empty password is still a key (the HMAC is computed over an empty string),
// and whether a command is obeyed is the controller's decision, reported
// honestly here as a result code.
constexpr uint32_t kAckWaitMs = 1500;     // sent, no answer: stop waiting
constexpr uint32_t kResultHoldMs = 5000;  // how long the answer stays up
constexpr uint8_t kAckNoLink = 0xFE;      // ours, not a car result: not even sent
constexpr uint8_t kAckNoReply = 0xFF;     // ours: sent, never answered

struct ModeCmd {
  bool pending = false;     // sent, waiting for the controller's ack
  bool haveResult = false;  // an answer (or our own failure) is on screen
  bool retried = false;     // the one silent retry a STALE result earns
  uint8_t want = 0;         // the mode we asked for
  uint8_t result = 0;       // ohxResult_t, or kAckNoLink / kAckNoReply
  uint32_t sentMs = 0, resultMs = 0;
};
ModeCmd g_modeCmd;
int g_curMode = -1;  // the mode the panel is showing, for the next step

const char *ackResultText(uint8_t r) {
  switch (r) {
    case OHX_R_OK: return "ok";
    case OHX_R_STALE: return "stale";
    case OHX_R_AUTH: return "wrong key";
    case OHX_R_DISABLED: return "disabled";
    case OHX_R_MOVING: return "car moving";
    case OHX_R_NO_HALDEX: return "no haldex";
    case OHX_R_BUSY: return "busy";
    case OHX_R_INVALID: return "invalid";
    case OHX_R_UNSUPPORTED: return "unsupported";
    case OHX_R_CTRL_OFF: return "control off";
    case kAckNoLink: return "no link";
    case kAckNoReply: return "no reply";
    default: return "?";
  }
}

// One step: STOCK -> FWD -> 50:50 -> 60:40 -> 75:25 -> EXPERT -> STOCK (or back
// to FWD on a standalone controller, which has no STOCK). While
// a command is still in flight the step counts from the mode we just asked
// for, so three quick taps move three modes instead of re-sending one.
void cycleMode() {
  const int base = g_modeCmd.pending
                       ? g_modeCmd.want
                       : (g_curMode >= 0 && g_curMode < 6 ? g_curMode : 0);
  uint8_t want = (uint8_t)((base + 1) % 6);
  // Standalone has no stock behaviour to fall back on, so the controller answers
  // a request for STOCK by staying on its last mode, and still reports ok. Ask
  // for it from EXPERT and the panel never moves again, so step over it.
  if (want == 0 && ohxnow::last().standalone) want = 1;

  ModeCmd &m = g_modeCmd;
  m.want = want;
  m.sentMs = millis();
  m.pending = true;
  m.haveResult = false;
  m.retried = false;
  m.result = 0;

  if (ohxnow::sendMode(want)) {
    Serial.printf("[ui] mode -> %s (sent)\n", kModeNames[want]);
  } else {
    // Nothing to sign against, or the radio refused: say so on the panel
    // rather than looking like a tap that did nothing.
    m.pending = false;
    m.haveResult = true;
    m.result = kAckNoLink;
    m.resultMs = millis();
    Serial.printf("[ui] mode -> %s: no link to the controller\n", kModeNames[want]);
  }
}

void onHaldexTap(lv_event_t *) { cycleMode(); }

// ------------------------------------------------------------- handles
struct Widgets {
  // The dashboard's own root: bar, number, tiles and status bar live in it so
  // the whole thing can be hidden while the settings page is up. LVGL 9 has no
  // "covered by an opaque object" skip, so anything left visible under the
  // settings card would be redrawn with it on every scroll step.
  lv_obj_t *root = nullptr;
  lv_obj_t *top = nullptr;  // the whole top band; long-press opens settings
  // The boot splash, a sibling of root and on top of it. It is built once and
  // deleted when the hold elapses, so it costs nothing for the rest of the
  // drive. Its picture is a child of it; the rule, tagline and version are
  // siblings drawn over that.
  lv_obj_t *splash = nullptr;

  // The rev bar, its cells, the row of numbers under it and the big number's
  // row. A group each, so the debug mask can hide them one at a time.
  lv_obj_t *revGrp = nullptr, *ticksGrp = nullptr, *numGrp = nullptr;
  lv_obj_t *revCell[L::BAR_SEG] = {};
  lv_obj_t *tickLabel[12] = {};  // one per thousand rpm, minus the top slot
  lv_obj_t *tickUnit = nullptr;  // "x1000", at the right-hand end
  int tickLabels = 0;

  lv_obj_t *rpmVal = nullptr;  // the big number, centred on its own row

  // The indicator repeaters: a left and a right arrow flanking that number
  // (#5). One glyph each, from the FontAwesome face (see FONT_ARROW), because
  // LVGL's built-in LEFT/RIGHT symbols are chevrons and its symbol font carries
  // no filled block arrow - the shape the mockup draws. Both are hidden when
  // nothing is blinking, so a dark dash shows no arrow furniture at all; a
  // blink shows the arrow in green and hides it again for the "off" phase.
  lv_obj_t *indLeft = nullptr, *indRight = nullptr;

  lv_obj_t *speedVal = nullptr, *speedPeak = nullptr, *speedUnit = nullptr;
  lv_obj_t *boostVal = nullptr, *boostUnit = nullptr, *boostBar = nullptr, *boostPeak = nullptr;
  lv_obj_t *coolVal = nullptr, *coolBar = nullptr, *oilVal = nullptr, *oilBar = nullptr;
  lv_obj_t *fuelVal = nullptr, *fuelBar = nullptr;
  lv_obj_t *modeVal = nullptr, *haldexSub = nullptr;
  lv_obj_t *lockReqVal = nullptr, *lockActVal = nullptr, *thrBar = nullptr;

  lv_obj_t *dot = nullptr, *connTxt = nullptr, *odoTxt = nullptr;
  lv_obj_t *lampBrake = nullptr, *lampHand = nullptr, *lampTc = nullptr, *lampAsr = nullptr,
           *lampCc = nullptr;
  lv_obj_t *btnReset = nullptr, *btnCfg = nullptr;

  lv_obj_t *settings = nullptr, *kb = nullptr;
  // What the controller will accept from us, and with what key. Shown in
  // settings because the credential is a settings field, not a diagnostic.
  lv_obj_t *ctrlLine = nullptr, *ctrlWarn = nullptr;
  // Where to send a new image, and which slot this one came from.
  lv_obj_t *otaLine = nullptr;
  // What automatic day / night has decided, and the number it produced, so the
  // mode can be judged on the glass rather than trusted.
  lv_obj_t *dayNightLine = nullptr;
  // The drive log: a session is started by hand, so it is a button and a state
  // line rather than a setting (no NVS field, nothing to leave on by accident).
  lv_obj_t *btnLog = nullptr, *logLine = nullptr;
} w;

struct Values {
  float rpm, speed, throttle, boost, coolant, oil, lockTarget, lockActual, fuel;
};
constexpr Values kBlank = {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};

Values g_target = kBlank;  // latest from the API
Values g_shown = kBlank;   // interpolated, what is drawn
struct {
  float speed = NAN, boost = NAN;
} g_peaks;

bool g_flashOn = false;    // the shift / traction flash phase
bool g_indOn = false;      // the indicator repeater phase (its own timer)
uint32_t g_lastIndMs = 0;
int g_indMask = 0;         // latched indicator bits (bit 0 left, bit 1 right)
uint32_t g_indMaskMs = 0;  // when a non-zero mask was last seen

// 0xFFFF = draw everything (see dash_ui.h).
uint16_t g_dbgMask = 0xFFFF;

// Bench counters for "the screen went stale": how often, and how old the data
// was when it did.
uint32_t g_staleEvents = 0;
uint32_t g_maxStaleAgeMs = 0;
bool dbg(uint16_t bit) { return (g_dbgMask & bit) != 0; }

uint32_t g_gaugeUs = 0, g_readoutsUs = 0, g_connUs = 0, g_calls = 0;
bool g_dbgHideAll = false;
uint32_t g_lastFlashMs = 0;
uint32_t g_lastTickMs = 0;
// Which page is on the glass; Page::Dashboard means the dashboard is.
Page g_page = Page::Dashboard;
bool g_stale = true;

// The boot splash owns the glass until the first real data arrives, then it is
// removed for good. While it is up the dashboard is hidden underneath it, so the
// panel draws one static mark instead of a full dashboard it has no data for.
bool g_splashUp = false;

// How long the splash holds before the dashboard takes over. Time-based, not
// data-based: the panel always reaches the dashboard, controller or not.
constexpr uint32_t kSplashMs = 3000;
uint32_t g_splashStartMs = 0;
lv_obj_t *g_splashRule = nullptr;  // the clip whose width is the progress fill

// The rule's geometry, taken from the artwork rather than chosen: the splash
// picture paints the empty track at exactly this place, and the fill has to land
// inside it. 380 px wide, rows 284 to 286, starting at x 210.
constexpr int kRuleX = 210;
constexpr int kRuleY = 284;
constexpr int kRuleW = 380;
constexpr int kRuleH = 3;

// Which cells of the rev bar are currently lit: 1 lit, 0 dark, -1 unknown.
// -1 is the state after start-up and after the zone colours change, so the
// first update after that repaints every cell instead of assuming dark.
int8_t g_revLit[L::BAR_SEG];

// Lit/unlit colour per rev cell, from the rpm zones. Fixed until the
// redline moves, so a cell's colour is decided once per settings change, not
// once per frame.
struct CellColour {
  lv_color_t lit, unlit;
};
CellColour g_revCol[L::BAR_SEG];

// ------------------------------------------------------------- helpers
void setText(lv_obj_t *label, const char *text) {
  const char *cur = lv_label_get_text(label);
  if (!cur || strcmp(cur, text) != 0) lv_label_set_text(label, text);
}

bool sameColor(lv_color_t a, lv_color_t b) {
  return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

// Setting a style always invalidates the object, even when the value is
// identical. On a 30 Hz dashboard that is pure repaint cost, so every style
// write goes through one of these guards.
void setTextColor(lv_obj_t *label, lv_color_t c) {
  if (!sameColor(lv_obj_get_style_text_color(label, 0), c)) {
    lv_obj_set_style_text_color(label, c, 0);
  }
}

void setBgColor(lv_obj_t *obj, lv_color_t c, lv_style_selector_t sel) {
  if (!sameColor(lv_obj_get_style_bg_color(obj, sel), c)) {
    lv_obj_set_style_bg_color(obj, c, sel);
  }
}

void setBorderColor(lv_obj_t *obj, lv_color_t c) {
  if (!sameColor(lv_obj_get_style_border_color(obj, 0), c)) {
    lv_obj_set_style_border_color(obj, c, 0);
  }
}

void setBar(lv_obj_t *bar, int32_t value) {
  if (lv_bar_get_value(bar) != value) lv_bar_set_value(bar, value, LV_ANIM_OFF);
}

lv_color_t colFor(float v, float warn, float crit) {
  if (isnan(v)) return COL_DIM;
  if (v >= crit) return COL_RED;
  if (v >= warn) return COL_AMBER;
  return COL_GREEN;
}

lv_obj_t *plain(lv_obj_t *parent, int x, int y, int width, int height) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, width, height);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  return o;
}

// Like plain() but paints its own background, so a repaint stays inside the
// container instead of reaching back to whatever is underneath.
lv_obj_t *opaque(lv_obj_t *parent, int x, int y, int width, int height, lv_color_t bg) {
  lv_obj_t *o = plain(parent, x, y, width, height);
  lv_obj_set_style_bg_color(o, bg, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  return o;
}

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_remove_flag(l, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, text);
  return l;
}

// A fixed-size box with centred text: avoids depending on measured label
// width when placing text at an exact point.
lv_obj_t *boxedLabel(lv_obj_t *parent, int cx, int cy, int boxW, int boxH,
                     const lv_font_t *font, lv_color_t color, const char *text) {
  lv_obj_t *l = label(parent, font, color, text);
  lv_obj_set_size(l, boxW, boxH);
  lv_obj_set_pos(l, cx - boxW / 2, cy - boxH / 2);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_all(l, 0, 0);
  return l;
}

// Dynamic text must never use LV_SIZE_CONTENT: a changing self-size marks
// the layout dirty, and LVGL then repaints the whole parent tile. Every
// label whose text changes gets a fixed box instead.
lv_obj_t *fixedLabel(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *font,
                     lv_color_t color, const char *text) {
  lv_obj_t *l = label(parent, font, color, text);
  lv_obj_set_size(l, w, h);
  lv_obj_set_pos(l, x, y);
  lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
  return l;
}

// A 1 px horizontal rule. On the settings page this is what keeps the
// structure the card's border used to give, for one thin object instead of
// 12 px of padding on every side of the page.
lv_obj_t *rule(lv_obj_t *parent, int x, int y, int w) {
  lv_obj_t *r = plain(parent, x, y, w, 1);
  lv_obj_set_style_bg_color(r, COL_LINE, 0);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  return r;
}

lv_obj_t *bar(lv_obj_t *parent, int x, int y, int width, int height, lv_color_t fill,
              int32_t min, int32_t max) {
  lv_obj_t *b = lv_bar_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, width, height);
  lv_obj_set_style_radius(b, height / 2, 0);
  lv_obj_set_style_bg_color(b, COL_TRACK, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(b, height / 2, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(b, fill, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_bar_set_range(b, min, max);
  lv_bar_set_value(b, min, LV_ANIM_OFF);
  return b;
}

lv_obj_t *button(lv_obj_t *parent, int x, int y, int width, int height, const char *text,
                 lv_event_cb_t cb) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, width, height);
  lv_obj_set_style_radius(b, 4, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x12161b), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, COL_LAMP_EDGE, 0);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *l = label(b, FONT_CAP, COL_FG, text);
  lv_obj_center(l);
  return b;
}

lv_obj_t *lamp(lv_obj_t *parent, int x, int y, int width, const char *text) {
  lv_obj_t *l = label(parent, FONT_CAP, COL_LAMP_TXT_OFF, text);
  lv_obj_set_size(l, width, 20);
  lv_obj_set_pos(l, x, y);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_all(l, 2, 0);
  lv_obj_set_style_radius(l, 3, 0);
  lv_obj_set_style_bg_color(l, COL_LAMP_OFF, 0);
  lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(l, 1, 0);
  lv_obj_set_style_border_color(l, COL_LAMP_EDGE, 0);
  return l;
}

// Called every frame for the status-bar lamps; keep the previous state so an
// unchanged lamp costs nothing at all. The colour is part of the state, because
// the cruise lamp has two different "on" colours.
struct LampState {
  lv_obj_t *obj = nullptr;
  bool valid = false;
  bool on = false;
  lv_color_t bg = lv_color_black();
  lv_color_t fg = lv_color_black();
};
LampState g_lampState[8];

void lampPaint(lv_obj_t *l, bool on, lv_color_t bg, lv_color_t fg) {
  LampState *st = nullptr;
  for (LampState &c : g_lampState) {
    if (c.obj == l) {
      st = &c;
      break;
    }
    if (c.obj == nullptr) {
      c.obj = l;
      st = &c;
      break;
    }
  }
  if (!st) return;
  const bool same = st->valid && st->on == on && st->bg.red == bg.red && st->bg.green == bg.green &&
                    st->bg.blue == bg.blue;
  if (same) return;
  st->valid = true;
  st->on = on;
  st->bg = bg;
  st->fg = fg;

  setBgColor(l, on ? bg : COL_LAMP_OFF, 0);
  setBorderColor(l, on ? bg : COL_LAMP_EDGE);
  setTextColor(l, on ? fg : COL_LAMP_TXT_OFF);
}

// The tell-tale lamps: amber, or red for the ones that mean something is wrong.
void lampState(lv_obj_t *l, bool on, bool red) {
  lampPaint(l, on, red ? COL_RED : COL_AMBER, red ? lv_color_white() : lv_color_black());
}

// The cruise lamp: three states rather than two, because "armed" (the column
// switch is on) and "regulating" (the engine is holding a speed) are different
// facts and telling them apart is the whole point of showing it. A very dark
// green when armed, the indicator green when it is actually working: the armed
// state is a reminder that the system exists, not a call to look at it.
//
// Armed used to take white letters, which is 255 luma - the brightest thing in
// the status bar, brighter per pixel than the regulating lamp's fill - so the
// idle state outshouted the working one no matter how dark its plate was. Dim
// green letters (100 luma) on the dark plate, and black on the lit green plate
// when regulating: the only thing that lights up is the state that matters.
void lampStateCruise(lv_obj_t *l, const int state) {  // 0 off, 1 armed, 2 regulating
  if (state <= 0) {
    lampPaint(l, false, COL_GREEN, lv_color_black());
  } else if (state == 1) {
    lampPaint(l, true, COL_GREEN_DIMMER, COL_GREEN_DIM);
  } else {
    lampPaint(l, true, COL_GREEN, lv_color_black());
  }
}

// ---------------------------------------------------------------- gauge
// Everything that depends on the rpm/boost scale: which zone each rev cell
// belongs to, and the numbers along the bar. Called once at build time and
// again whenever the settings change the scale.
void syncGauge() {
  const cfg::Data &c = cfg::get();
  const float fAmber = c.rpmAmber / c.rpmMax;
  const float fRed = c.rpmRedline / c.rpmMax;

  for (int i = 0; i < L::BAR_SEG; i++) {
    // Each cell takes the colour of the zone its centre falls in, so the
    // amber and red zones are visible as dark tints even before they light.
    const float f = (i + 0.5f) / (float)L::BAR_SEG;
    CellColour col{COL_GREEN, COL_ZONE_GREEN};
    if (f >= fRed) {
      col = {COL_RED, COL_ZONE_RED};
    } else if (f >= fAmber) {
      col = {COL_AMBER, COL_ZONE_AMBER};
    }
    g_revCol[i] = col;
    g_revLit[i] = -1;  // force the next update to paint this cell
  }

  // One number per 1000 rpm. The left end (0) has no number and the top of
  // the scale is where the unit tag sits, so the count is perK - 1.
  const int perK = (int)lroundf(c.rpmMax / 1000.0f);
  int n = perK - 1;
  if (n < 1) n = 1;
  if (n > (int)(sizeof(w.tickLabel) / sizeof(w.tickLabel[0]))) {
    n = (int)(sizeof(w.tickLabel) / sizeof(w.tickLabel[0]));
  }
  const int labelW = 40;
  char buf[8];
  for (int i = 0; i < n; i++) {
    snprintf(buf, sizeof(buf), "%d", i + 1);  // thousands
    setText(w.tickLabel[i], buf);
    const float f = (float)((i + 1) * 1000) / c.rpmMax;
    lv_obj_set_x(w.tickLabel[i], (int)lroundf(f * L::BAR_W) - labelW / 2);
    lv_obj_remove_flag(w.tickLabel[i], LV_OBJ_FLAG_HIDDEN);
  }
  for (int i = n; i < (int)(sizeof(w.tickLabel) / sizeof(w.tickLabel[0])); i++) {
    lv_obj_add_flag(w.tickLabel[i], LV_OBJ_FLAG_HIDDEN);
  }
  w.tickLabels = n;

  // "x1000" only means something if the scale lands on whole thousands.
  if (fmodf(c.rpmMax, 1000.0f) == 0.0f) {
    lv_obj_remove_flag(w.tickUnit, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(w.tickUnit, LV_OBJ_FLAG_HIDDEN);
  }
}

// Indicator repeaters. LVGL's built-in LV_SYMBOL_LEFT/RIGHT are chevrons (0xF053
// / 0xF054), and its symbol font carries no filled block arrow, which is the
// shape the approved layout C mockup draws. FontAwesome 4.7 has exactly that
// shape at 0xF060 / 0xF061, so those two glyphs are converted into their own
// face (FONT_ARROW) and drawn as plain labels - a glyph blit, which is the
// cheapest thing the panel can draw, and nothing like the cost of the six
// lv_line objects the open-outline version needed.
constexpr const char *kArrowLeft = "\xEF\x81\xA0";   // U+F060, FA arrow-left
constexpr const char *kArrowRight = "\xEF\x81\xA1";  // U+F061, FA arrow-right

// One arrow label. Hidden at build: an arrow is on the glass only while its
// side is actually blinking, so the number's row carries no permanent
// grey furniture.
lv_obj_t *buildIndicatorArrow(lv_obj_t *parent, int x, int y, const char *glyph) {
  lv_obj_t *a = fixedLabel(parent, x, y, kArrowW, kArrowH, FONT_ARROW, COL_GREEN, glyph);
  lv_obj_set_style_text_align(a, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
  return a;
}

// Lit -> the arrow is on the glass in green. Dark -> it is gone entirely. The
// flag is checked first because LVGL invalidates on the way in either way, and
// this runs every frame.
void setIndicator(lv_obj_t *arrow, bool lit) {
  if (!arrow) return;
  const bool hidden = lv_obj_has_flag(arrow, LV_OBJ_FLAG_HIDDEN);
  if (lit == !hidden) return;
  if (lit) {
    setTextColor(arrow, COL_GREEN);
    lv_obj_remove_flag(arrow, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(arrow, LV_OBJ_FLAG_HIDDEN);
  }
}

// The rev bar: 40 opaque cells, painted once at build and then only ever
// recoloured where the fill edge has moved. Nothing is resized, moved or
// re-laid-out, which is what keeps a frame cheap.
void buildGauge(lv_obj_t *parent) {
  w.top = opaque(parent, 0, 0, L::W, L::TILE_Y, COL_BG);

  // One group per element so the debug mask can switch each one off.
  w.revGrp = plain(w.top, L::BAR_X, L::REV_BAR_Y, L::BAR_W, L::REV_BAR_H);
  w.ticksGrp = plain(w.top, L::BAR_X, L::TICK_ROW_Y, L::BAR_W, L::TICK_ROW_H);
  w.numGrp = plain(w.top, 0, L::RPM_NUM_Y, L::W, L::RPM_NUM_H);

  for (int i = 0; i < L::BAR_SEG; i++) {
    const int x0 = (i * L::BAR_W) / L::BAR_SEG;
    const int x1 = ((i + 1) * L::BAR_W) / L::BAR_SEG - L::BAR_GAP;
    w.revCell[i] = opaque(w.revGrp, x0, 0, x1 - x0, L::REV_BAR_H, COL_ZONE_GREEN);
  }

  // Numbers along the rev bar, plus the unit tag at its right-hand end.
  for (int i = 0; i < (int)(sizeof(w.tickLabel) / sizeof(w.tickLabel[0])); i++) {
    w.tickLabel[i] = fixedLabel(w.ticksGrp, 0, 0, 40, L::TICK_ROW_H, FONT_TICK, COL_DIM, "");
    lv_obj_set_style_text_align(w.tickLabel[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(w.tickLabel[i], LV_OBJ_FLAG_HIDDEN);
  }
  w.tickUnit =
      fixedLabel(w.ticksGrp, L::BAR_W - 48, 0, 48, L::TICK_ROW_H, FONT_CAP, COL_DIM, "x1000");
  lv_obj_set_style_text_align(w.tickUnit, LV_TEXT_ALIGN_RIGHT, 0);

  // The big number, centred on a row of its own. The label box is exactly one
  // line high, so "centred" is arithmetic instead of a layout pass, and it
  // stays centred if the generated font changes size.
  const int lh = FONT_RPM->line_height > 0 ? FONT_RPM->line_height : L::RPM_NUM_H;
  w.rpmVal = fixedLabel(w.numGrp, 0, (L::RPM_NUM_H - lh) / 2, L::W, lh, FONT_RPM, COL_FG, "--");
  lv_obj_set_style_text_align(w.rpmVal, LV_TEXT_ALIGN_CENTER, 0);

  // The indicator repeaters: an arrow at each end of the number's row, as in
  // layout C. Both are created hidden and left that way unless a side is
  // blinking, so the row shows nothing but the number at rest (#5).
  const int arrowY = (L::RPM_NUM_H - kArrowH) / 2;
  w.indLeft = buildIndicatorArrow(w.numGrp, 2 * L::TILE_PAD_H, arrowY, kArrowLeft);
  w.indRight = buildIndicatorArrow(w.numGrp, L::W - 2 * L::TILE_PAD_H - kArrowW, arrowY,
                                   kArrowRight);

  syncGauge();
}

// --------------------------------------------------------------- tiles
// The four original sections, at the widths dash.html gives them. BOOST is
// back as a tile of its own: boost the bar is gone, so the value is shown
// once, here, with its own small bar and its peak.
//
// The four panels are always built; which of them are on the glass, and where
// they sit, is decided by applyLayout() below from what the car can feed.
lv_obj_t *g_tileSpeed = nullptr, *g_tileBoost = nullptr;
lv_obj_t *g_tileTemps = nullptr, *g_tileHaldex = nullptr;
layout::Variant g_variant = layout::Variant::Full;
bool g_placed = false;

// The temperatures panel's three rows (coolant / oil / fuel) share one pitch,
// so the build pass and the placement pass cannot drift apart.
//
// The card carries no title of its own - "coolant", "oil" and "fuel" say what
// the card is - so its first row has to sit where every other card's caption
// sits, TILE_PAD_V, and the three rows then fill the tile (#11). The anchor per
// row is therefore the *caption's* box top, not a notional row top.
//
// The caption and the value share a baseline. At montserrat_12 (FONT_CAP) the
// caption's baseline is 12 px below its box top; at montserrat_22 (FONT_TEMP)
// the value's is 20 px below its. So a value box 8 px above the caption's puts
// the two on one line - the relationship the Haldex tile's LOCK REQ / LOCK ACT
// rows already use. The bar runs 30 px below the caption.
constexpr int kTempRows = 3;
constexpr int kTempPitch = 80;
constexpr int kTempCapY[kTempRows] = {L::TILE_PAD_V, L::TILE_PAD_V + kTempPitch,
                                      L::TILE_PAD_V + 2 * kTempPitch};
constexpr int kTempValDY = -8;   // value box top, from the caption's
constexpr int kTempValH = 30;
constexpr int kTempBarDY = 30;   // bar top, from the caption's

void buildTiles(lv_obj_t *parent) {
  const int y = L::TILE_Y;
  const int h = L::TILE_H;

  // ---- SPEED ------------------------------------------------------
  lv_obj_t *t0 = opaque(parent, L::COL0_X, y, L::COL0_W, h, COL_BG);
  g_tileSpeed = t0;
  lv_obj_set_style_border_side(t0, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_border_width(t0, 1, 0);
  lv_obj_set_style_border_color(t0, COL_LINE, 0);

  lv_obj_t *cap = label(t0, FONT_CAP, COL_DIM, "SPEED");
  lv_obj_set_pos(cap, L::TILE_PAD_H, L::TILE_PAD_V);

  w.speedVal = fixedLabel(t0, L::TILE_PAD_H - 2, 44, 132, 58, FONT_BIG, COL_FG, "--");

  // Dynamic: km/h or mph, from the metric / imperial setting (#7).
  w.speedUnit = fixedLabel(t0, L::TILE_PAD_H + 4, h - 44, 70, 18, FONT_CAP, COL_DIM, "km/h");

  w.speedPeak = fixedLabel(t0, L::TILE_PAD_H, h - 24, L::COL0_W - 2 * L::TILE_PAD_H, 18,
                           FONT_SMALL, COL_DIM, "peak --");

  // ---- BOOST ------------------------------------------------------
  lv_obj_t *t1 = opaque(parent, L::COL1_X, y, L::COL1_W, h, COL_BG);
  g_tileBoost = t1;
  lv_obj_set_style_border_side(t1, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_border_width(t1, 1, 0);
  lv_obj_set_style_border_color(t1, COL_LINE, 0);

  cap = label(t1, FONT_CAP, COL_DIM, "BOOST");
  lv_obj_set_pos(cap, L::TILE_PAD_H, L::TILE_PAD_V);

  const int bw = L::COL1_W - 2 * L::TILE_PAD_H;
  w.boostVal = fixedLabel(t1, L::TILE_PAD_H, 40, bw, 52, FONT_NUM, COL_FG, "--");
  w.boostUnit = fixedLabel(t1, L::TILE_PAD_H, 96, bw, 18, FONT_SMALL, COL_DIM, "bar");
  w.boostBar = bar(t1, L::TILE_PAD_H, 128, bw, 10, COL_WHITE, 0, 100);
  w.boostPeak = fixedLabel(t1, L::TILE_PAD_H, h - 24, bw, 18, FONT_SMALL, COL_DIM, "peak --");

  // ---- TEMPERATURES -----------------------------------------------
  lv_obj_t *t2 = opaque(parent, L::COL2_X, y, L::COL2_W, h, COL_BG);
  g_tileTemps = t2;
  lv_obj_set_style_border_side(t2, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_border_width(t2, 1, 0);
  lv_obj_set_style_border_color(t2, COL_LINE, 0);

  // Three rows: coolant, oil and fuel (layout C), with no caption above them -
  // the card is named by its contents, so the first row starts where the cards
  // beside it put their captions. The unit rides in the value label - "79 °C",
  // "49.6 L" - so a reading is never ambiguous: the temperatures follow the C/F
  // toggle and the fuel is always litres (the metric/imperial toggle is about
  // distance). The 80 px pitch is what fills the tile without scrolling and
  // without shrinking FONT_TEMP (#11).
  const int tw = L::COL2_W - 2 * L::TILE_PAD_H;
  const int *const rowY = kTempCapY;

  lv_obj_t *cl = label(t2, FONT_CAP, COL_DIM, "COOLANT");
  lv_obj_set_pos(cl, L::TILE_PAD_H, rowY[0]);
  w.coolVal = fixedLabel(t2, L::TILE_PAD_H + tw - 100, rowY[0] + kTempValDY, 100, kTempValH, FONT_TEMP, COL_FG, "--");
  lv_obj_set_style_text_align(w.coolVal, LV_TEXT_ALIGN_RIGHT, 0);
  w.coolBar = bar(t2, L::TILE_PAD_H, rowY[0] + kTempBarDY, tw, 10, COL_GREEN, 0, 100);

  lv_obj_t *ol = label(t2, FONT_CAP, COL_DIM, "OIL");
  lv_obj_set_pos(ol, L::TILE_PAD_H, rowY[1]);
  w.oilVal = fixedLabel(t2, L::TILE_PAD_H + tw - 100, rowY[1] + kTempValDY, 100, kTempValH, FONT_TEMP, COL_FG, "--");
  lv_obj_set_style_text_align(w.oilVal, LV_TEXT_ALIGN_RIGHT, 0);
  w.oilBar = bar(t2, L::TILE_PAD_H, rowY[1] + kTempBarDY, tw, 10, COL_GREEN, 0, 100);

  lv_obj_t *fl = label(t2, FONT_CAP, COL_DIM, "FUEL");
  lv_obj_set_pos(fl, L::TILE_PAD_H, rowY[2]);
  w.fuelVal = fixedLabel(t2, L::TILE_PAD_H + tw - 100, rowY[2] + kTempValDY, 100, kTempValH, FONT_TEMP, COL_FG, "--");
  lv_obj_set_style_text_align(w.fuelVal, LV_TEXT_ALIGN_RIGHT, 0);
  w.fuelBar = bar(t2, L::TILE_PAD_H, rowY[2] + kTempBarDY, tw, 10, COL_GREEN, 0, 100);

  // ---- HALDEX -----------------------------------------------------
  lv_obj_t *t3 = opaque(parent, L::COL3_X, y, L::COL3_W, h, COL_BG);
  g_tileHaldex = t3;

  cap = label(t3, FONT_CAP, COL_DIM, "HALDEX");
  lv_obj_set_pos(cap, L::TILE_PAD_H, L::TILE_PAD_V);

  const int hw = L::COL3_W - 2 * L::TILE_PAD_H;
  w.modeVal = fixedLabel(t3, L::TILE_PAD_H, 40, hw, 36, FONT_MODE, COL_FG, "--");

  w.haldexSub = fixedLabel(t3, L::TILE_PAD_H, 76, hw, 20, FONT_SMALL, COL_DIM, "coupling --");

  lv_obj_t *lr = label(t3, FONT_CAP, COL_DIM, "LOCK REQ");
  lv_obj_set_pos(lr, L::TILE_PAD_H, 102);
  w.lockReqVal = fixedLabel(t3, L::COL3_W - 110, 94, 100, 30, FONT_TEMP, COL_FG, "--");
  lv_obj_set_style_text_align(w.lockReqVal, LV_TEXT_ALIGN_RIGHT, 0);

  lv_obj_t *la = label(t3, FONT_CAP, COL_DIM, "LOCK ACT");
  lv_obj_set_pos(la, L::TILE_PAD_H, 136);
  w.lockActVal = fixedLabel(t3, L::COL3_W - 110, 128, 100, 30, FONT_TEMP, COL_FG, "--");
  lv_obj_set_style_text_align(w.lockActVal, LV_TEXT_ALIGN_RIGHT, 0);

  lv_obj_t *thr = label(t3, FONT_CAP, COL_FG, "THR");
  lv_obj_set_pos(thr, L::TILE_PAD_H + 4, 170);
  w.thrBar = bar(t3, L::TILE_PAD_H, 186, hw, 12, COL_GREEN, 0, 100);
}

// ------------------------------------------------------ tile geometry
// Each panel is built once, above, and then *placed* here. That distinction
// is the whole trick: changing the band's shape moves and resizes objects
// that already exist, so nothing is destroyed, no pointer goes stale, and a
// panel that is not in the chosen band is hidden rather than deleted - which
// also means the code that fills it in never has to ask whether it exists.
//
// Only width-dependent items are touched. A caption sits at the padding edge
// and a bar starts at it, whatever the panel's width, so those do not move.
void placeSpeedTile(lv_obj_t *t, int x, int width, int height) {
  lv_obj_set_pos(t, x, L::TILE_Y);
  lv_obj_set_size(t, width, height);
  lv_obj_set_pos(w.speedVal, L::TILE_PAD_H - 2, 44);
  lv_obj_set_pos(w.speedPeak, L::TILE_PAD_H, height - 24);
  lv_obj_set_width(w.speedPeak, width - 2 * L::TILE_PAD_H);
}

void placeBoostTile(lv_obj_t *t, int x, int width, int height) {
  lv_obj_set_pos(t, x, L::TILE_Y);
  lv_obj_set_size(t, width, height);
  const int bw = width - 2 * L::TILE_PAD_H;
  lv_obj_set_width(w.boostVal, bw);
  lv_obj_set_width(w.boostUnit, bw);
  lv_obj_set_width(w.boostBar, bw);
  lv_obj_set_pos(w.boostPeak, L::TILE_PAD_H, height - 24);
  lv_obj_set_width(w.boostPeak, bw);
}

void placeTempsTile(lv_obj_t *t, int x, int width, int height) {
  lv_obj_set_pos(t, x, L::TILE_Y);
  lv_obj_set_size(t, width, height);
  const int tw = width - 2 * L::TILE_PAD_H;
  lv_obj_set_pos(w.coolVal, L::TILE_PAD_H + tw - 100, kTempCapY[0] + kTempValDY);
  lv_obj_set_width(w.coolBar, tw);
  lv_obj_set_pos(w.oilVal, L::TILE_PAD_H + tw - 100, kTempCapY[1] + kTempValDY);
  lv_obj_set_width(w.oilBar, tw);
  lv_obj_set_pos(w.fuelVal, L::TILE_PAD_H + tw - 100, kTempCapY[2] + kTempValDY);
  lv_obj_set_width(w.fuelBar, tw);
}

void placeHaldexTile(lv_obj_t *t, int x, int width, int height) {
  lv_obj_set_pos(t, x, L::TILE_Y);
  lv_obj_set_size(t, width, height);
  const int hw = width - 2 * L::TILE_PAD_H;
  lv_obj_set_width(w.modeVal, hw);
  lv_obj_set_width(w.haldexSub, hw);
  lv_obj_set_pos(w.lockReqVal, width - 110, 94);
  lv_obj_set_pos(w.lockActVal, width - 110, 128);
  lv_obj_set_width(w.thrBar, hw);
}

// Called once, when the capability decision settles, and by hand from the
// console for bench work. Idempotent: re-applying the same band re-places the
// same pixels and repaints nothing.
void placeBand(layout::Variant v) {
  const layout::TileBand &band = layout::bandFor(v);
  const bool changed = (v != g_variant) || !g_placed;
  bool boostShown = false;

  for (int i = 0; i < band.count; i++) {
    const layout::TileCol &c = band.cols[i];
    switch (c.role) {
      case layout::Tile::Speed: placeSpeedTile(g_tileSpeed, c.x, c.w, L::TILE_H); break;
      case layout::Tile::Boost:
        placeBoostTile(g_tileBoost, c.x, c.w, L::TILE_H);
        boostShown = true;
        break;
      case layout::Tile::Temps: placeTempsTile(g_tileTemps, c.x, c.w, L::TILE_H); break;
      case layout::Tile::Haldex: placeHaldexTile(g_tileHaldex, c.x, c.w, L::TILE_H); break;
    }
  }
  // Hidden, not deleted: a panel the car cannot fill must stop drawing, but
  // the widgets in it are still updated by the readout pass, which is cheaper
  // and simpler than making every readout ask whether its panel exists.
  if (g_tileBoost) {
    if (boostShown) {
      lv_obj_remove_flag(g_tileBoost, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(g_tileBoost, LV_OBJ_FLAG_HIDDEN);
    }
  }
  g_variant = v;
  g_placed = true;

  if (changed) {
    // The panels that just moved or appeared have to be repainted, and so
    // does the strip the hidden one used to occupy.
    if (w.root) lv_obj_invalidate(w.root);
    Serial.printf("[ui] tile band %s (%d panels)\n", band.name, band.count);
  }
}

// ---------------------------------------------------------- status bar
void onResetPeaks(lv_event_t *) {
  // The rpm peak is gone (the odometer took its slot, #6), so this is now just
  // the two tiles that show a peak of their own.
  g_peaks.speed = g_peaks.boost = NAN;
}

void onOpenSettings(lv_event_t *);

void buildStatusBar(lv_obj_t *parent) {
  const int y = L::H - L::STATUS_H;
  lv_obj_t *s = plain(parent, 0, y, L::W, L::STATUS_H);
  lv_obj_set_style_bg_color(s, COL_PANEL, 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_set_style_border_side(s, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(s, 1, 0);
  lv_obj_set_style_border_color(s, COL_LINE, 0);

  w.dot = plain(s, 9, (L::STATUS_H - 11) / 2, 11, 11);
  lv_obj_set_style_radius(w.dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(w.dot, COL_DIM, 0);
  lv_obj_set_style_bg_opa(w.dot, LV_OPA_COVER, 0);

  // 92 px holds the widest state word ("CONNECTING") without resizing.
  w.connTxt = fixedLabel(s, 26, 8, 62, 18, FONT_CAP, COL_DIM, "CONNECT");

  const int lampY = 5;
  w.lampBrake = lamp(s, 92, lampY, 50, "BRAKE");
  w.lampHand = lamp(s, 146, lampY, 50, "HAND");
  w.lampTc = lamp(s, 200, lampY, 32, "TC");
  w.lampAsr = lamp(s, 236, lampY, 40, "ASR");
  // Where the AWD lamp was. That flag is HTTP-only and its meaning is not
  // settled; the cruise lamp reads the car's own bus and says something the
  // driver can act on.
  w.lampCc = lamp(s, 280, lampY, 42, "CC");

  // The odometer, where the rpm peak used to be (#6). It is not "live data" and
  // is deliberately not part of the LIVE/STALE decision: an odometer that is ten
  // seconds old is not stale in any useful sense.
  //
  // The headlight lamp of #14 has no CAN bit to read and is not built. If it
  // ever lands it takes the right-hand end of this lamp cluster, and this label
  // moves up to meet RESET PEAKS.
  w.odoTxt = fixedLabel(s, 332, 6, 180, 18, FONT_SMALL, COL_DIM, "ODO --");

  w.btnReset = button(s, 516, 3, 104, 24, "RESET PEAKS", onResetPeaks);
  // The DAY button that used to sit here is gone: day / night is a setting, not
  // something to toggle by accident from a moving car (#8/#11).
  w.btnCfg = button(s, 626, 3, 166, 24, "SETTINGS", onOpenSettings);
}

// ------------------------------------------------------------ settings
// Two columns, no scrolling (#9). The page is 780x420 and every row is placed
// to fit: scrolling this on a 32 Hz panel was a full-width repaint per step.
cfg::Data g_edit;

const char *const kBoostUnitNames[3] = {"bar", "mbar", "psi"};
const char *const kDayNightNames[3] = {"AUTO", "DAY", "NIGHT"};

// One value, drawn as [-] value [+]. The cell is created already showing the
// stored number and registers its context, so the page can re-read the whole
// config when it opens - which is what stops a threshold from showing "-"
// until it has been touched (#12).
struct StepCtx {
  float *field;
  float step, min, max;
  int decimals;
  lv_obj_t *valueLabel;
};
StepCtx g_steps[24];
int g_stepCount = 0;

void fmtStepper(char *buf, size_t n, const StepCtx &c) {
  if (c.decimals == 0) {
    snprintf(buf, n, "%d", (int)lroundf(*c.field));
  } else {
    snprintf(buf, n, "%.*f", c.decimals, (double)*c.field);
  }
}

void nudge(StepCtx *ctx, float dir) {
  float v = *ctx->field + dir * ctx->step;
  if (v < ctx->min) v = ctx->min;
  if (v > ctx->max) v = ctx->max;
  *ctx->field = v;
  char buf[16];
  fmtStepper(buf, sizeof(buf), *ctx);
  setText(ctx->valueLabel, buf);
}

// One [-] value [+] cell, `w` px wide. The value box is a label with a border,
// not a text area: it never takes focus, so the keyboard cannot appear over the
// half of the page it does not belong to.
int addStepCell(lv_obj_t *parent, int x, int y, int w, float *field, float step, float min,
                float max, int decimals) {
  if (g_stepCount >= (int)(sizeof(g_steps) / sizeof(g_steps[0]))) return x + w;
  StepCtx &c = g_steps[g_stepCount++];
  c.field = field;
  c.step = step;
  c.min = min;
  c.max = max;
  c.decimals = decimals;

  const int btnW = 28;
  const int valW = w - 2 * btnW - 8;

  lv_obj_t *minus = button(parent, x, y, btnW, 24, "-", nullptr);
  lv_obj_add_event_cb(
      minus, [](lv_event_t *e) { nudge((StepCtx *)lv_event_get_user_data(e), -1.0f); },
      LV_EVENT_CLICKED, &c);

  char buf[16];
  fmtStepper(buf, sizeof(buf), c);
  c.valueLabel = fixedLabel(parent, x + btnW + 4, y, valW, 24, FONT_BODY, COL_FG, "");
  lv_obj_set_style_text_align(c.valueLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(c.valueLabel, 3, 0);
  lv_obj_set_style_bg_color(c.valueLabel, lv_color_hex(0x0e1114), 0);
  lv_obj_set_style_bg_opa(c.valueLabel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(c.valueLabel, 1, 0);
  lv_obj_set_style_border_color(c.valueLabel, COL_LAMP_EDGE, 0);
  lv_obj_set_style_radius(c.valueLabel, 3, 0);
  setText(c.valueLabel, buf);

  lv_obj_t *plus = button(parent, x + btnW + 4 + valW + 4, y, btnW, 24, "+", nullptr);
  lv_obj_add_event_cb(
      plus, [](lv_event_t *e) { nudge((StepCtx *)lv_event_get_user_data(e), 1.0f); },
      LV_EVENT_CLICKED, &c);
  return x + w;
}

struct StepSpec {
  float *field;
  float step, min, max;
  int decimals;
};

// A settings row: a name, then one or two value cells. Two cells make the paired
// [VALUE]/[VALUE] rows of the layout C settings page, which is how the eleven
// thresholds fit in half the card.
void addStepRow(lv_obj_t *parent, int x, int y, const char *name, const StepSpec *specs, int n) {
  lv_obj_t *l = label(parent, FONT_SMALL, lv_color_hex(0xc6ced6), name);
  lv_obj_set_pos(l, x, y + 4);
  int cx = x + 148;
  for (int i = 0; i < n; i++) {
    const StepSpec &s = specs[i];
    addStepCell(parent, cx, y, 116, s.field, s.step, s.min, s.max, s.decimals);
    cx += 116 + 8;
  }
}

void refreshSteppers() {
  char buf[16];
  for (int i = 0; i < g_stepCount; i++) {
    fmtStepper(buf, sizeof(buf), g_steps[i]);
    setText(g_steps[i].valueLabel, buf);
  }
}


void taEvent(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *ta = lv_event_get_target_obj(e);
  if (code == LV_EVENT_FOCUSED) {
    lv_obj_remove_flag(w.kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(w.kb, ta);
    lv_obj_move_foreground(w.kb);
  } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    lv_keyboard_set_textarea(w.kb, nullptr);
    lv_obj_add_flag(w.kb, LV_OBJ_FLAG_HIDDEN);
  }
}

lv_obj_t *addField(lv_obj_t *parent, int y, const char *name, const char *value, bool password,
                   int width) {
  lv_obj_t *l = label(parent, FONT_SMALL, lv_color_hex(0xc6ced6), name);
  lv_obj_set_pos(l, 0, y + 8);

  lv_obj_t *ta = lv_textarea_create(parent);
  lv_obj_set_pos(ta, 110, y);
  lv_obj_set_size(ta, width, 34);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_max_length(ta, password ? 64 : 47);
  lv_textarea_set_password_mode(ta, password);
  lv_textarea_set_text(ta, value);
  lv_obj_set_style_bg_color(ta, lv_color_hex(0x0e1114), 0);
  lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(ta, COL_FG, 0);
  lv_obj_set_style_text_font(ta, FONT_BODY, 0);
  lv_obj_set_style_border_width(ta, 1, 0);
  lv_obj_set_style_border_color(ta, COL_LAMP_EDGE, 0);
  lv_obj_set_style_radius(ta, 3, 0);
  lv_obj_add_event_cb(ta, taEvent, LV_EVENT_ALL, nullptr);
  return ta;
}

lv_obj_t *g_taSsid = nullptr, *g_taPass = nullptr, *g_taHost = nullptr;
lv_obj_t *g_btnTempUnits = nullptr, *g_btnBoostUnits = nullptr;
lv_obj_t *g_btnDistUnits = nullptr, *g_btnDayNight = nullptr;

void onTempUnits(lv_event_t *) {
  g_edit.tempF = !g_edit.tempF;
  setText(lv_obj_get_child(g_btnTempUnits, 0), g_edit.tempF ? "\xC2\xB0" "F" : "\xC2\xB0" "C");
}

void onBoostUnits(lv_event_t *) {
  g_edit.boostUnits = (g_edit.boostUnits + 1) % 3;
  setText(lv_obj_get_child(g_btnBoostUnits, 0), kBoostUnitNames[g_edit.boostUnits]);
}

// Distance only: km/h and km against mph and miles. The temperature toggle above
// is a separate setting and stays separate (#7).
void onDistUnits(lv_event_t *) {
  g_edit.unitsMetric = g_edit.unitsMetric ? 0 : 1;
  setText(lv_obj_get_child(g_btnDistUnits, 0), g_edit.unitsMetric ? "km" : "mi");
}

// Auto / Day / Night. Auto is what reads Terminal 58s off the car; the two
// overrides hold the backlight where it is put (#8).
void onDayNight(lv_event_t *) {
  g_edit.dayNight = (uint8_t)((g_edit.dayNight + 1) % 3);
  setText(lv_obj_get_child(g_btnDayNight, 0), kDayNightNames[g_edit.dayNight]);
}

void onCfgSave(lv_event_t *) {
  strncpy(g_edit.ssid, lv_textarea_get_text(g_taSsid), sizeof(g_edit.ssid) - 1);
  strncpy(g_edit.pass, lv_textarea_get_text(g_taPass), sizeof(g_edit.pass) - 1);
  strncpy(g_edit.host, lv_textarea_get_text(g_taHost), sizeof(g_edit.host) - 1);
  // A new SSID means a different controller, so the remembered one is dropped.
  // A new password on the same SSID is still the same controller.
  const bool ssidChanged = strcmp(g_edit.ssid, cfg::get().ssid) != 0;
  cfg::apply(g_edit);
  if (ssidChanged) ohxnow::forgetController();
  // No setBacklight() here: the backlight is now decided from the config *and*
  // the car's Terminal 58s reading, and tick() applies it a frame later. Writing
  // the configured brightness straight to the panel would flash the day value at
  // night for that frame.
  syncGauge();  // the zones and the numbers move with the scale
  showSettings(false);
}

void onCfgDefaults(lv_event_t *) {
  const cfg::Data keep = cfg::get();
  cfg::Data fresh = cfg::defaults();
  strncpy(fresh.ssid, keep.ssid, sizeof(fresh.ssid) - 1);
  strncpy(fresh.pass, keep.pass, sizeof(fresh.pass) - 1);
  strncpy(fresh.host, keep.host, sizeof(fresh.host) - 1);
  fresh.port = keep.port;
  fresh.netUseStatic = keep.netUseStatic;  // learnt on this car, not a preference
  cfg::apply(fresh);
  ohxnow::forgetController();  // also how a replaced controller gets picked up
  g_edit = cfg::get();
  lv_textarea_set_text(g_taSsid, g_edit.ssid);
  lv_textarea_set_text(g_taPass, g_edit.pass);
  lv_textarea_set_text(g_taHost, g_edit.host);
  refreshSteppers();
  // The toggles above are buttons, not steppers, so nothing re-reads them: label
  // them here or DEFAULTS leaves them showing the settings it just replaced.
  setText(lv_obj_get_child(g_btnTempUnits, 0), g_edit.tempF ? "\xC2\xB0" "F" : "\xC2\xB0" "C");
  setText(lv_obj_get_child(g_btnBoostUnits, 0), kBoostUnitNames[g_edit.boostUnits]);
  setText(lv_obj_get_child(g_btnDistUnits, 0), g_edit.unitsMetric ? "km" : "mi");
  setText(lv_obj_get_child(g_btnDayNight, 0), kDayNightNames[g_edit.dayNight]);
  syncGauge();  // the zones and the numbers move with the scale
}

void onCfgClose(lv_event_t *) { showSettings(false); }

void onCfgRaw(lv_event_t *) {
  // One tap from settings. setPage() closes settings on the way in, so the
  // raw page is never stacked on top of the card.
  rawui::show(true);
}

void onCfgCan(lv_event_t *) { canui::show(true); }

// ------------------------------------------------------------ drive log
// Next to the page it belongs to. setText()/setBgColor() do nothing when the
// value has not changed, so refreshing this once a second costs nothing while
// the page sits idle.
void updateLogStatus() {
  if (!w.btnLog) return;
  const bool rec = logbook::recording();
  setText(lv_obj_get_child(w.btnLog, 0), rec ? "STOP LOG" : "START LOG");
  setBgColor(w.btnLog, rec ? COL_RED : lv_color_hex(0x12161b), 0);
  if (w.logLine) {
    // With no filesystem there is nothing this button or its counter can say,
    // so it says that instead of a confident "stored 0 KB" (seen on the bench:
    // the partition was blank and the button did nothing).
    if (!logbook::ready()) {
      setText(w.logLine, "no filesystem");
      setTextColor(w.logLine, COL_AMBER);
      return;
    }
    char b[40];
    snprintf(b, sizeof b, "%s %u KB", rec ? "REC" : "stored",
             (unsigned)((logbook::fileBytes() + 512) / 1024));
    setText(w.logLine, b);
    setTextColor(w.logLine, rec ? COL_RED : COL_DIM);
  }
}

void onLogToggle(lv_event_t *) {
  logbook::toggle();
  updateLogStatus();
}

void buildSettings() {
  w.settings = lv_obj_create(lv_layer_top());
  lv_obj_remove_flag(w.settings, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(w.settings, L::W, L::H);
  lv_obj_set_pos(w.settings, 0, 0);
  // Opaque, not the 214/255 it was: a translucent backdrop is blended over
  // the entire card area every frame and then completely covered by the card,
  // which is pure cost. It looked like a dimmed dashboard before; with the
  // dashboard hidden behind it, near-black is the same picture.
  lv_obj_set_style_bg_color(w.settings, lv_color_hex(0x080a0d), 0);
  lv_obj_set_style_bg_opa(w.settings, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(w.settings, 0, 0);
  lv_obj_set_style_pad_all(w.settings, 0, 0);
  lv_obj_add_flag(w.settings, LV_OBJ_FLAG_HIDDEN);

  // The page body is the whole screen, not a 780x420 card floating on it.
  // The card was a border, 12 px of padding on every side and 10 px of margin,
  // and it bought nothing: this page already owns the glass (the dashboard is
  // hidden, not merely covered), so the frame was decoration drawn in the one
  // currency that is scarce here - pixels. Removing it gave the two columns
  // 60 px of height and 44 px of width, which is what the connection block
  // needs now that its lines wrap instead of being clipped.
  //
  // Two 1 px rules (under the title and above the buttons) keep the structure
  // the border used to give, for the cost of two thin objects.
  lv_obj_t *card = lv_obj_create(w.settings);
  lv_obj_set_size(card, L::W, L::H);
  lv_obj_set_pos(card, 0, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);  // the page behind it is opaque
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_radius(card, 0, 0);
  lv_obj_set_style_pad_all(card, 0, 0);
  lv_obj_set_style_pad_left(card, 12, 0);
  lv_obj_set_style_pad_right(card, 12, 0);
  lv_obj_set_style_pad_top(card, 10, 0);
  // No LV_DIR_VER and no SCROLLABLE flag: the page is laid out to fit as it is
  // (#9). Scrolling it on a 32 Hz panel is a full-width repaint per step, which
  // is exactly what made this page feel slow.
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  // ---- left column: connection, then display ------------------------
  int y = 0;
  lv_obj_t *h = label(card, FONT_MODE, COL_FG, "SETTINGS");
  lv_obj_set_pos(h, 0, y);
  y += 32;

  rule(card, 0, y, L::W - 24);
  y += 10;

  lv_obj_t *net = label(card, FONT_BODY, COL_DIM, "CONNECTION TO OPENHALDEX");
  lv_obj_set_pos(net, 0, y);
  y += 20;

  g_taSsid = addField(card, y, "SSID", g_edit.ssid, false, 220);
  y += 36;
  g_taPass = addField(card, y, "AP PASSWORD", g_edit.pass, true, 220);
  y += 36;
  g_taHost = addField(card, y, "HOST", g_edit.host, false, 220);
  y += 48;  // 14 px of air under the HOST box: see the note below

  // The password above is two things at once: the WPA key the station joins
  // the access point with, and the HMAC key every ESP-NOW command has to be
  // signed with. Empty is therefore not "no password needed" - it is a key
  // anyone in range can compute, against a nonce the controller broadcasts.

  // The status block sat 2 px under the HOST box and, being 340 px wide, ran
  // the whole way past the field's edge, so on the glass it read as part of the
  // field (seen in a photo of 0.6.2). It now has 14 px of air.
  //
  // The two long lines get two rows each and wrap. They were 18 px boxes with
  // LV_LABEL_LONG_CLIP, so their ends were cut off: measured against
  // lv_font_montserrat_14's advance widths, the open-access-point warning is
  // 484 px and the trial OTA line 349 px, against a 340 px box. The warning is
  // the one that matters - it was losing "can send commands".
  w.ctrlLine = fixedLabel(card, 0, y, 368, 18, FONT_SMALL, COL_DIM, "control: waiting");
  y += 18;
  w.ctrlWarn = fixedLabel(card, 0, y, 368, 36, FONT_SMALL, COL_AMBER, "");
  lv_label_set_long_mode(w.ctrlWarn, LV_LABEL_LONG_WRAP);
  y += 36;
  w.otaLine = fixedLabel(card, 0, y, 368, 36, FONT_SMALL, COL_DIM, "OTA: --");
  lv_label_set_long_mode(w.otaLine, LV_LABEL_LONG_WRAP);
  y += 46;

  lv_obj_t *disp = label(card, FONT_BODY, COL_DIM, "DISPLAY");
  lv_obj_set_pos(disp, 0, y);
  y += 20;

  {
    const StepSpec bright[] = {{&g_edit.brightness, 5, 10, 100, 0}};
    addStepRow(card, 0, y, "Brightness (%)", bright, 1);
    y += 30;
  }

  lv_obj_t *ul = label(card, FONT_SMALL, lv_color_hex(0xc6ced6), "Units");
  lv_obj_set_pos(ul, 0, y + 4);
  g_btnTempUnits =
      button(card, 110, y, 44, 24, g_edit.tempF ? "\xC2\xB0" "F" : "\xC2\xB0" "C", onTempUnits);
  g_btnBoostUnits = button(card, 162, y, 60, 24, kBoostUnitNames[g_edit.boostUnits], onBoostUnits);
  g_btnDistUnits = button(card, 230, y, 56, 24, g_edit.unitsMetric ? "km" : "mi", onDistUnits);
  y += 30;

  lv_obj_t *dn = label(card, FONT_SMALL, lv_color_hex(0xc6ced6), "Day / night");
  lv_obj_set_pos(dn, 0, y + 4);
  g_btnDayNight = button(card, 110, y, 90, 24, kDayNightNames[g_edit.dayNight], onDayNight);
  // What automatic mode has actually decided, and the number it produced, so the
  // two board_config numbers can be judged on the glass rather than trusted.
  w.dayNightLine = fixedLabel(card, 208, y + 4, 148, 20, FONT_SMALL, COL_DIM, "");
  y += 30;

  // ---- right column: thresholds, two values to a row ----------------
  const int rx = 386;
  lv_obj_t *thr = label(card, FONT_BODY, COL_DIM, "THRESHOLDS");
  lv_obj_set_pos(thr, rx, 42);

  // 60 rather than 26: the column now starts level with CONNECTION, and the
  // seven rows plus the note below have to end above the button rule at 404.
  // Measured: 6 rows at 40 + one at 34 = 274, and the note wraps to 4 lines at
  // font 12 in 388 px, so 338 + 60 = 398.
  int ty = 60;
  {
    const StepSpec a[] = {{&g_edit.rpmMax, 250, 3000, 12000, 0}, {&g_edit.rpmAmber, 250, 0, 12000, 0}};
    addStepRow(card, rx, ty, "RPM max / amber", a, 2);
    ty += 40;
  }
  {
    const StepSpec a[] = {{&g_edit.rpmRedline, 250, 1000, 12000, 0},
                          {&g_edit.rpmShift, 250, 1000, 12000, 0}};
    addStepRow(card, rx, ty, "RPM redline / shift", a, 2);
    ty += 40;
  }
  {
    const StepSpec a[] = {{&g_edit.coolWarn, 1, -40, 200, 0}, {&g_edit.coolCrit, 1, -40, 250, 0}};
    addStepRow(card, rx, ty, "Coolant warn / crit", a, 2);
    ty += 40;
  }
  {
    const StepSpec a[] = {{&g_edit.oilWarn, 1, -40, 200, 0}, {&g_edit.oilCrit, 1, -40, 250, 0}};
    addStepRow(card, rx, ty, "Oil warn / crit", a, 2);
    ty += 40;
  }
  {
    const StepSpec a[] = {{&g_edit.boostMax, 100, 200, 5000, 0},
                          {&g_edit.boostWarn, 100, 0, 5000, 0}};
    addStepRow(card, rx, ty, "Boost max / warn", a, 2);
    ty += 40;
  }
  {
    const StepSpec a[] = {{&g_edit.boostCrit, 100, 0, 5000, 0}};
    addStepRow(card, rx, ty, "Boost crit", a, 1);
    ty += 40;
  }
  {
    const StepSpec a[] = {{&g_edit.tankLitres, 1, 20, 120, 0}};
    addStepRow(card, rx, ty, "Fuel tank (l)", a, 1);
    ty += 34;
  }

  // The clamping rule, said out loud: it is what keeps a stored threshold set
  // self-consistent, and before the shift light was fixed, the shift point was
  // free to sit above the redline (#10).
  lv_obj_t *note = label(card, FONT_CAP, COL_DIM,
                         "Saved values are clamped into order: amber <= shift <= "
                         "redline <= max, warn <= crit.\nTemperature thresholds are "
                         "in C; the temperature rows above follow the units toggle.");
  lv_obj_set_pos(note, rx, ty);
  lv_obj_set_width(note, 388);

  // ---- the buttons, along the bottom of the screen -------------------
  // Pinned to the bottom edge with a rule above them, so the page reads as a
  // page: content above, actions below.
  rule(card, 0, 404, L::W - 24);
  button(card, 0, 414, 90, 30, "DEFAULTS", onCfgDefaults);
  button(card, 100, 414, 82, 30, "RAW DATA", onCfgRaw);
  button(card, 192, 414, 82, 30, "CAN DATA", onCfgCan);
  // The drive log is a session, not a setting: started here or with 'l' on the
  // console, and it stops itself at 100 KB. It sits between the page buttons
  // and SAVE, with the line that says whether a session is running.
  w.btnLog = button(card, 290, 414, 96, 30, "START LOG", onLogToggle);
  w.logLine = fixedLabel(card, 396, 420, 216, 18, FONT_SMALL, COL_DIM, "");
  button(card, 620, 414, 74, 30, "SAVE", onCfgSave);
  button(card, 702, 414, 74, 30, "CLOSE", onCfgClose);

  // Every cell above was created showing the stored value, so there is nothing
  // to refresh before the page is first opened: the "-" placeholder is gone
  // (#12). refreshSteppers() is still what puts the truth back after DEFAULTS or
  // after an edit that was abandoned by closing the page.
  refreshSteppers();

  w.kb = lv_keyboard_create(lv_layer_top());
  lv_obj_set_size(w.kb, L::W, 200);
  lv_obj_align(w.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(w.kb, LV_OBJ_FLAG_HIDDEN);
}

void onOpenSettings(lv_event_t *) { showSettings(true); }
void onLongPress(lv_event_t *) { showSettings(true); }

// -------------------------------------------------------------- update
float easeValue(float shown, float target, float dt) {
  if (isnan(target)) return NAN;
  if (isnan(shown)) return target;
  const float k = 1.0f - expf(-dt / kTau);
  return shown + (target - shown) * k;
}

void setSuffix(lv_obj_t *valueLabel, const char *text, lv_color_t color) {
  setText(valueLabel, text);
  setTextColor(valueLabel, color);
}

// ---- units -----------------------------------------------------------
// One place that decides how a distance, a speed or a fuel volume is written, so
// the metric / imperial setting cannot be applied twice or forgotten somewhere
// (#7). Distance and speed only: the temperature toggle is its own setting.
//
// U+00B0 (degree) in UTF-8, written escaped and split into two literals: the
// compiler would otherwise read "\xB0C" as one over-long hex escape.
constexpr const char *kDegC = "\xC2\xB0" "C";
constexpr const char *kDegF = "\xC2\xB0" "F";
constexpr float kMilesPerKm = 0.621371f;

bool metric() { return cfg::get().unitsMetric != 0; }
const char *speedUnitText() { return metric() ? "km/h" : "mph"; }
float toSpeed(float kmh) { return metric() ? kmh : kmh * kMilesPerKm; }
float toDistance(float km) { return metric() ? km : km * kMilesPerKm; }

/// Write n with thousands separators: 138618 -> "138,618".
void groupDigits(char *buf, size_t n, long v) {
  char d[24];
  snprintf(d, sizeof(d), "%ld", v < 0 ? -v : v);
  char out[32];
  int o = 0;
  const int len = (int)strlen(d);
  for (int i = 0; i < len && o < (int)sizeof(out) - 2; i++) {
    if (i > 0 && ((len - i) % 3) == 0) out[o++] = ',';
    out[o++] = d[i];
  }
  out[o] = '\0';
  snprintf(buf, n, "%s%s", v < 0 ? "-" : "", out);
}

/// The temperature, with its unit: "79°C" / "175°F", or "--" for a value that
/// has never arrived. One helper feeds the coolant and oil rows, so the unit can
/// never disagree with the setting.
void fmtTemp(char *buf, size_t n, float celsius) {
  if (isnan(celsius)) {
    snprintf(buf, n, "--");
    return;
  }
  const bool f = cfg::get().tempF;
  const float v = f ? celsius * 9.0f / 5.0f + 32.0f : celsius;
  snprintf(buf, n, "%d%s", (int)lroundf(v), f ? kDegF : kDegC);
}

/// A speed in the current units, no suffix.
void fmtSpeed(char *buf, size_t n, float kmh) {
  if (isnan(kmh)) {
    snprintf(buf, n, "--");
    return;
  }
  snprintf(buf, n, "%d", (int)lroundf(toSpeed(kmh)));
}

/// A distance with its unit: "138,618 km" / "86,133 mi".
void fmtDistance(char *buf, size_t n, float km) {
  char num[24];
  groupDigits(num, sizeof(num), lroundf(toDistance(km)));
  snprintf(buf, n, "%s %s", num, metric() ? "km" : "mi");
}

/// The fuel level, always in litres: "49.6 L".
///
/// Deliberately NOT tied to the metric / imperial toggle. The car reports litres
/// (TankContents is one litre per count) and a tank size is a metric fact here,
/// and the toggle is about *distance* - so switching to miles moves the speed and
/// the odometer and leaves the fuel alone. Converting it would also put the fuel
/// bar's full-scale (a litre figure) at odds with the number beside it.
void fmtFuel(char *buf, size_t n, float litres) {
  if (isnan(litres)) {
    snprintf(buf, n, "--");
    return;
  }
  snprintf(buf, n, "%.1f L", (double)litres);
}

void fmtBoost(char *buf, size_t n, float mbar) {
  if (isnan(mbar)) {
    snprintf(buf, n, "--");
    return;
  }
  switch (cfg::get().boostUnits) {
    case cfg::BOOST_MBAR:
      snprintf(buf, n, "%d", (int)lroundf(mbar));
      break;
    case cfg::BOOST_PSI:
      snprintf(buf, n, "%.1f", (double)(mbar / 68.9476f));
      break;
    default:
      snprintf(buf, n, "%.2f", (double)(mbar / 1000.0f));
      break;
  }
}

const char *boostUnitText() {
  switch (cfg::get().boostUnits) {
    case cfg::BOOST_MBAR: return "mbar";
    case cfg::BOOST_PSI: return "psi";
    default: return "bar";
  }
}

// The rev bar, at the full render rate: the eye tracks its edge the way it
// used to track the needle.
//
// A cell is only touched when its state actually changes, so a frame that
// moves the fill edge by one cell repaints exactly one 18x104 px rectangle
// instead of an arc. setBgColor() is the same-value guard used everywhere
// else, so even the state comparison is belt and braces.
//
// The shift cue lives here rather than on the number (#10): at or above
// rpmShift the lit cells alternate between the shift purple and a flat dark, and
// at the limiter between red and that same dark. Flashing to a flat off - not
// back to the zone colour - is deliberate: pulsing between two lit colours read
// as a colour change, and pulsing to the per-zone tints still carried a
// green/amber/red gradient through the blink. A 96 px number strobing was hard
// to read; a bar pulsing is not. The colour is only recomputed while a shift is
// on, so a steady bar costs exactly what it did before.
void updateRevBar(const cfg::Data &c, float rpm) {
  if (!dbg(1)) return;

  const float frac = isnan(rpm) ? 0.0f : (rpm / c.rpmMax);
  int lit = (int)lroundf(frac * L::BAR_SEG);
  if (lit < 0) lit = 0;
  if (lit > L::BAR_SEG) lit = L::BAR_SEG;

  const bool have = !isnan(rpm);
  const bool shifting = have && rpm >= c.rpmShift;
  const bool limiter = have && rpm >= c.rpmRedline;
  const lv_color_t flashCol = limiter ? COL_RED : COL_SHIFT;

  for (int i = 0; i < L::BAR_SEG; i++) {
    // 0 = empty (faint zone tint), 1 = lit in its zone colour, 2 = lit in the
    // flash colour, 3 = the dark half of a shift flash: a flat dark, not the
    // zone tint, so nothing of the zone gradient shows through the blink.
    int8_t want;
    if (i >= lit) {
      want = 0;
    } else if (!shifting) {
      want = 1;
    } else {
      want = g_flashOn ? 2 : 3;
    }
    if (g_revLit[i] != want) {
      g_revLit[i] = want;
      const lv_color_t col = (want == 0)   ? g_revCol[i].unlit
                             : (want == 1) ? g_revCol[i].lit
                             : (want == 2) ? flashCol
                                           : COL_TRACK;
      setBgColor(w.revCell[i], col, 0);
    }
  }
}

// The big number. It is only repainted when its text or colour changes, so
// this costs nothing on the frames where the value has not moved. It does NOT
// flash any more: the shift cue is the bar (#10).
void updateGaugeText(const cfg::Data &c, float rpm) {
  (void)c;
  if (!dbg(4)) return;

  // A missing rpm shows as "--"; a stale one keeps its last value, because
  // going stale is the status light's job.
  char buf[24];
  if (!isnan(rpm)) {
    snprintf(buf, sizeof(buf), "%d", (int)lroundf(rpm));
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  setSuffix(w.rpmVal, buf, COL_FG);
}

void updateReadouts(const cfg::Data &c, const telemetry::Live &live, const telemetry::Dash &d) {
  (void)live;  // only the interpolated copy is drawn
  if (!dbg(8)) return;
  // The last known values stay on the glass when the data goes stale (the
  // status light says so); "--" is reserved for values never received, which
  // is what NaN means.
  const Values v = g_shown;
  char buf[32];

  // ---- indicator repeaters (#5). The frame gives the direction and the fact
  // that something is blinking; the flash phase is ours, at the car's own ~1.5 Hz,
  // and the latch means a dropped packet cannot cancel a signal. Left and right
  // are separate arrows, so hazards (both bits) light both. An arrow is on the
  // glass only during its side's lit phase - the dark phase hides it, so there
  // is no grey arrow sitting on the dash between blinks.
  if (d.indicators > 0) {
    g_indMask = d.indicators;
    g_indMaskMs = millis();
  }
  const bool indOn = g_indMask != 0 && (uint32_t)(millis() - g_indMaskMs) < kIndicatorHoldMs;
  const bool indLit = indOn && g_indOn;
  setIndicator(w.indLeft, indLit && (g_indMask & 1));
  setIndicator(w.indRight, indLit && (g_indMask & 2));

  // ---- odometer, where the rpm peak used to be (#6)
  //
  // While a drive log is recording the slot says so instead: a session writes
  // to flash, and a dash that does that silently is a bad dash. One word, so
  // the cost is a single small repaint per state change, not per second.
  if (logbook::recording()) {
    setText(w.odoTxt, "REC");
    setTextColor(w.odoTxt, COL_RED);
  } else if (isnan(d.odometerKm)) {
    setText(w.odoTxt, "ODO --");
    setTextColor(w.odoTxt, COL_DIM);
  } else {
    char ob[24];
    fmtDistance(ob, sizeof(ob), d.odometerKm);
    snprintf(buf, sizeof(buf), "ODO %s", ob);
    setText(w.odoTxt, buf);
    setTextColor(w.odoTxt, COL_DIM);
  }

  // ---- speed
  fmtSpeed(buf, sizeof(buf), v.speed);
  setSuffix(w.speedVal, buf, isnan(v.speed) ? COL_DIM : COL_FG);
  setText(w.speedUnit, speedUnitText());
  if (isnan(g_peaks.speed)) {
    setText(w.speedPeak, "peak --");
  } else {
    char sb[16];
    fmtSpeed(sb, sizeof(sb), g_peaks.speed);
    snprintf(buf, sizeof(buf), "peak %s", sb);
    setText(w.speedPeak, buf);
  }

  // ---- boost
  fmtBoost(buf, sizeof(buf), v.boost);
  setSuffix(w.boostVal, buf, colFor(v.boost, c.boostWarn, c.boostCrit));
  setText(w.boostUnit, boostUnitText());
  {
    int pct = 0;
    if (!isnan(v.boost)) {
      pct = (int)((v.boost / c.boostMax) * 100.0f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
    }
    setBar(w.boostBar, pct);
  }
  if (isnan(g_peaks.boost)) {
    setText(w.boostPeak, "peak --");
  } else {
    char b[16];
    fmtBoost(b, sizeof(b), g_peaks.boost);
    snprintf(buf, sizeof(buf), "peak %s", b);
    setText(w.boostPeak, buf);
  }

  // ---- temperatures
  fmtTemp(buf, sizeof(buf), v.coolant);
  setSuffix(w.coolVal, buf, colFor(v.coolant, c.coolWarn, c.coolCrit));
  {
    int pct = 0;
    if (!isnan(v.coolant)) {
      pct = (int)(((v.coolant - 40.0f) / 90.0f) * 100.0f);  // 40..130 C scale
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
    }
    setBar(w.coolBar, pct);
    setBgColor(w.coolBar, colFor(v.coolant, c.coolWarn, c.coolCrit), LV_PART_INDICATOR);
  }

  fmtTemp(buf, sizeof(buf), v.oil);
  setSuffix(w.oilVal, buf, colFor(v.oil, c.oilWarn, c.oilCrit));
  {
    int pct = 0;
    if (!isnan(v.oil)) {
      pct = (int)(((v.oil - 40.0f) / 90.0f) * 100.0f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
    }
    setBar(w.oilBar, pct);
    setBgColor(w.oilBar, colFor(v.oil, c.oilWarn, c.oilCrit), LV_PART_INDICATOR);
  }

  // ---- fuel, the third row of this panel in both tile bands (#4/#11). The bar
  // is a percentage of the configured tank size; the number is the car's own
  // litres. The value and the bar go amber on the car's low-fuel warning, which
  // is read straight off the frame and never smoothed - the warning is the one
  // thing on this row that must not lag.
  fmtFuel(buf, sizeof(buf), v.fuel);
  setSuffix(w.fuelVal, buf, isnan(v.fuel) ? COL_DIM : (d.fuelLow ? COL_AMBER : COL_FG));
  {
    int pct = 0;
    if (!isnan(v.fuel)) {
      pct = (int)lroundf(100.0f * v.fuel / c.tankLitres);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
    }
    setBar(w.fuelBar, pct);
    setBgColor(w.fuelBar, d.fuelLow ? COL_AMBER : COL_GREEN, LV_PART_INDICATOR);
  }

  // ---- haldex
  if (d.mode >= 0 && d.mode < 6) {
    setText(w.modeVal, kModeNames[d.mode]);
    g_curMode = d.mode;  // where the next tap steps from
  } else {
    setText(w.modeVal, "--");
  }

  // Dim is the resting colour: a mode command borrows this line (see
  // updateModeCmd) and dimming here is what hands it back afterwards.
  setTextColor(w.haldexSub, COL_DIM);
  if (d.haldexCAN) {
    const char *state = d.couplingOpen ? "coupling open" : "coupling closed";
    setText(w.haldexSub, d.tempProtection ? "temp protection" : state);
  } else {
    setText(w.haldexSub, "Haldex CAN --");
  }

  if (isnan(v.lockTarget)) {
    setSuffix(w.lockReqVal, "--", COL_DIM);
  } else {
    snprintf(buf, sizeof(buf), "%d%%", (int)lroundf(v.lockTarget));
    setSuffix(w.lockReqVal, buf, COL_FG);
  }
  if (isnan(v.lockActual)) {
    setSuffix(w.lockActVal, "--", COL_DIM);
  } else {
    snprintf(buf, sizeof(buf), "%d%%", (int)lroundf(v.lockActual));
    setSuffix(w.lockActVal, buf, COL_CYAN);
  }

  {
    int pct = 0;
    if (!isnan(v.throttle)) {
      pct = (int)v.throttle;
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
    }
    setBar(w.thrBar, pct);
  }

  // ---- lamps. Last known state is held, like the readouts above: the amber
  // status light is what tells the driver the data is no longer fresh.
  //
  // Two independent things report the pedals on this platform:
  // the C6's own wired inputs (brakeIn / handbrakeIn) and the chassis CAN
  // (brakeFromCAN / handbrakeFromCAN, decoded from Bremse_1 and Kombi_1).
  // Reading only the wired pair is why both lamps sat dark on a car where the
  // CAN brake bit works and the handbrake bit has read 1 while parked. The
  // lamp lights when either source says the pedal is down; the raw page shows
  // the two side by side, so a dead input can be told from a live one.
  const bool brakeOn = d.brakeIn || d.brakeFromCAN == 1;
  const bool handOn = d.handbrakeIn || d.handbrakeFromCAN == 1;
  lampState(w.lampBrake, brakeOn, false);
  lampState(w.lampHand, handOn, false);
  // The traction tell-tales now come from the car's own lamp rather than the
  // controller's flags inverted, which had them lit while the system was on (#1).
  // The cluster lights BR1_Lamp_ASR (0x1A0 bit 9) when traction control is
  // switched off and sets BR1_ASR_Req (bit 0) while it intervenes. So: dark when
  // it is on, lit when it is off, flashing while it works. The flash is ours -
  // the same 60 ms toggle the shift cue uses - because the frame arrives at 9/s
  // and lossily, so mirroring the lamp bit's own flicker would alias. On this car
  // TC and ASR are one system with one tell-tale, so both lamps move together;
  // the packet's off-flags are only the fallback while the frame is quiet.
  const bool haveLamp = d.asrLamp >= 0;
  // Without chassis CAN the packet's flags mean nothing either, so the lamp
  // stays dark rather than guessing "off means disabled".
  const bool tractionOff = haveLamp ? d.asrLamp == 1 : (d.chassisCAN && (!d.tcOn || !d.asrOn));
  const bool intervening = d.asrActive == 1;
  const bool tractionLit = intervening ? g_flashOn : tractionOff;
  lampState(w.lampTc, tractionLit, false);
  lampState(w.lampAsr, tractionLit, false);
  // The cruise lamp: dark green with the column switch on, bright green while
  // the engine says it is regulating. Off when the frame is missing, so a quiet
  // link cannot read as "armed".
  lampStateCruise(w.lampCc, d.cruise);
}

// The line under the mode on the HALDEX panel: normally the coupling state,
// which updateReadouts has just written. While a mode command is in flight -
// and for a few seconds after it lands - this writes over it with the request
// and then the controller's answer, so a tap is never silent. Called after
// updateReadouts so its answer is the one left on the panel.
void updateModeCmd(uint32_t now) {
  if (!w.haldexSub) return;
  ModeCmd &m = g_modeCmd;
  const char *mode = kModeNames[m.want];

  if (m.pending) {
    const ohxnow::Ack a = ohxnow::lastAck();
    if (a.valid && a.result != OHX_R_NONE) {
      // STALE means another command (the web UI, a second gauge) got in first
      // and used the nonce up. The packet refills it every 100 ms, so one
      // silent retry is usually enough; more than one would just race again.
      if (a.result == OHX_R_STALE && !m.retried) {
        m.retried = true;
        if (ohxnow::sendMode(m.want)) {
          m.sentMs = now;
          return;
        }
      }
      m.pending = false;
      m.haveResult = true;
      m.result = a.result;
      m.resultMs = now;
      Serial.printf("[ui] mode %s: %s (ack after %lu ms)\n", mode, ackResultText(a.result),
                    (unsigned long)a.ageMs);
    } else if ((uint32_t)(now - m.sentMs) > kAckWaitMs) {
      m.pending = false;
      m.haveResult = true;
      m.result = kAckNoReply;
      m.resultMs = now;
      Serial.printf("[ui] mode %s: no reply\n", mode);
    } else {
      char buf[24];
      snprintf(buf, sizeof buf, "%s ...", mode);
      setText(w.haldexSub, buf);
      setTextColor(w.haldexSub, COL_AMBER);
      return;
    }
  }

  if (m.haveResult) {
    if ((uint32_t)(now - m.resultMs) > kResultHoldMs) {
      m.haveResult = false;  // the readout pass owns the line again
      return;
    }
    char buf[32];
    snprintf(buf, sizeof buf, "%s %s", mode, ackResultText(m.result));
    setText(w.haldexSub, buf);
    setTextColor(w.haldexSub, m.result == OHX_R_OK           ? COL_GREEN
                             : m.result == kAckNoLink || m.result == kAckNoReply ? COL_RED
                                                                                : COL_AMBER);
  }
}

float g_fuelAcc = NAN;     // the smoothed target: the last sample the gate let through
float g_fuelPrev = NAN;    // the previous raw sample, whatever it was
uint32_t g_fuelPrevMs = 0; // when that sample arrived

// Automatic day / night (#8). The car's Terminal 58s carries both facts at
// once: 0 with the lights off, 7..99 % with them on following the dash dimmer
// wheel. So one reading decides the mode and, at night, the backlight.
//
//   * 58s == 0 is day, 58s > 0 is night. Terminal_58d is ignored: it sits at a
//     non-zero 24 % with the lights off, so it cannot mean night.
//   * a *change* has to hold for kNightDebounceMs before it is believed, so one
//     dropped or error-flagged frame cannot strobe the screen.
//   * an absent reading (no relay) never moves the mode - the last one is held.
constexpr uint32_t kNightDebounceMs = 400;
bool g_night = false;
bool g_nightValid = false;
bool g_nightWanted = false;
uint32_t g_nightWantedMs = 0;
int g_term58s = -1;
uint8_t g_backlight = 0;
bool g_backlightValid = false;

// Write the backlight, but only when the number changes: it is a PWM write and
// the result is compared every tick.
void applyBacklight() {
  const cfg::Data &c = cfg::get();
  float pct = c.brightness;
  if (g_night) {
    const float s = g_term58s > 0 ? (float)g_term58s : 0.0f;
    pct = (float)DASH_NIGHT_MIN_PCT + (float)DASH_NIGHT_SPAN_PCT * s / 100.0f;
    if (pct < (float)DASH_NIGHT_MIN_PCT) pct = (float)DASH_NIGHT_MIN_PCT;
    if (pct > 100.0f) pct = 100.0f;
  }
  const uint8_t want = (uint8_t)lroundf(pct);
  if (g_backlightValid && want == g_backlight) return;
  g_backlight = want;
  g_backlightValid = true;
  crowpanel::setBacklight(want);
}

void updateDayNight(const telemetry::Dash &d, uint32_t now) {
  g_term58s = d.term58s;  // -1 = not heard
  bool moved = false;
  const uint8_t mode = cfg::get().dayNight;
  if (mode == cfg::DN_DAY) {
    moved = g_nightValid && g_night;
    g_night = false;
    g_nightValid = true;
  } else if (mode == cfg::DN_NIGHT) {
    moved = g_nightValid && !g_night;
    g_night = true;
    g_nightValid = true;
  } else if (g_term58s >= 0) {
    const bool want = g_term58s > 0;
    if (want != g_nightWanted) {
      g_nightWanted = want;
      g_nightWantedMs = now;
    }
    if (!g_nightValid || want == g_night) {
      g_night = want;
      g_nightValid = true;
    } else if ((uint32_t)(now - g_nightWantedMs) >= kNightDebounceMs) {
      g_night = want;
      moved = true;
    }
  }
  applyBacklight();
  if (moved) {
    Serial.printf("[ui] backlight %s: 58s %d%% -> %u%%\n", g_night ? "night" : "day", g_term58s,
                  (unsigned)g_backlight);
  }
}

void updateConn(bool stale) {
  if (!dbg(16)) return;
  const telemetry::Stats s = telemetry::stats();

  // Staleness now lives entirely in the status light: the dashboard keeps
  // showing the last values it had, and this is the only thing that changes.
  //
  // Two sources feed it now, so "no WiFi" means *neither* of them is alive:
  // ESP-NOW needs no association, so packets arriving prove the C6 is there
  // even when our own station has dropped off its access point.
  if (!s.wifiUp && !feed::onNow()) {
    setText(w.connTxt, "NO WIFI");
    setTextColor(w.connTxt, COL_RED);
    setBgColor(w.dot, COL_RED, 0);
  } else if (stale) {
    // "WAIT" until one of the two has delivered something at all.
    setText(w.connTxt, feed::source() == feed::Source::None ? "WAIT" : "STALE");
    setTextColor(w.connTxt, COL_AMBER);
    setBgColor(w.dot, COL_AMBER, 0);
  } else {
    setText(w.connTxt, "LIVE");
    setTextColor(w.connTxt, COL_GREEN);
    setBgColor(w.dot, COL_GREEN, 0);
  }
}

// ---------------------------------------------------------- boot splash
// The panel is blank for the second between power-on and the first frame. The
// splash covers that gap and then hands over after a fixed three seconds -
// always, controller or not, so the panel can never be left sitting on it.
//
// The field, the wordmark and the rule's empty track are one pre-rendered JPEG,
// decoded at boot by the mask ROM (src/ui/splash_img.cpp). That is not a
// shortcut: the gradients in that art are exactly what the S3's software
// renderer bands and paints slowly enough for the fill to outrun the paint, and
// the 0.6.1 splash dropped them and drew a flat black field instead.
//
// Only the parts that must not be baked into a picture are drawn here: the
// progress fill, and the version, which would go stale in the art the first time
// it moved.
//
// It is built once and deleted when it drops, so its widgets go back to the LVGL
// pool and the picture goes back to PSRAM, costing nothing for the rest of the
// drive.
void buildSplash(lv_obj_t *parent) {
  w.splash = opaque(parent, 0, 0, L::W, L::H, COL_BG);

  // The picture is the size of the panel and sits underneath everything else
  // here. If the decode failed, decode() has said why and the flat field stays -
  // the rule, tagline and version below are drawn either way, so even that
  // fallback says what is running.
  if (splashimg::decode()) {
    lv_obj_t *pic = lv_image_create(w.splash);
    lv_obj_remove_style_all(pic);
    lv_obj_remove_flag(pic, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(pic, splashimg::image());
    lv_obj_set_pos(pic, 0, 0);
  }

  // ---- the rule, at the picture's own coordinates: y 284, x 210 to 589.
  //
  // The gradient is fixed across the full 380 px and *revealed*, not stretched:
  // the parent grows and clips a child that is always full width. A child that
  // grew instead would recompute the gradient over its own width, so the colour
  // at the leading edge would crawl as the bar filled.
  lv_obj_t *clip = plain(w.splash, kRuleX, kRuleY, 1, kRuleH);
  lv_obj_t *grad = lv_obj_create(clip);
  lv_obj_remove_style_all(grad);
  lv_obj_remove_flag(grad, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(grad, 0, 0);
  lv_obj_set_style_bg_opa(grad, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(grad, lv_color_hex(0x22c55e), 0);
  lv_obj_set_style_bg_grad_color(grad, lv_color_hex(0x22c0f0), 0);
  lv_obj_set_style_bg_grad_dir(grad, LV_GRAD_DIR_HOR, 0);
  lv_obj_set_pos(grad, 0, 0);
  lv_obj_set_size(grad, kRuleW, kRuleH);
  g_splashRule = clip;

  // ---- tagline under the rule, and the version in the bottom-right corner,
  // where it is out of the artwork's way. The version is the one thing here
  // worth reading off the glass at boot: it is what says which build an
  // over-the-air trial actually booted.
  lv_obj_t *tag =
      fixedLabel(w.splash, 0, 302, L::W, 20, FONT_BODY, COL_DIM, "TRACK DASH FOR OPENHALDEX");
  lv_obj_set_style_text_align(tag, LV_TEXT_ALIGN_CENTER, 0);
  char vbuf[32];
  snprintf(vbuf, sizeof(vbuf), "v%s", OHX_DASH_VERSION);
  lv_obj_t *ver =
      fixedLabel(w.splash, L::W - 172, L::H - 30, 160, 18, FONT_SMALL, COL_DIM, vbuf);
  lv_obj_set_style_text_align(ver, LV_TEXT_ALIGN_RIGHT, 0);
}

// Hide the splash and hand the glass to the dashboard. Called once, when the
// three-second hold elapses. The splash is never shown again, so it is deleted
// rather than left hidden: its widgets - logo, rule, labels - go straight back
// to the LVGL pool, which is what keeps it from costing anything for the rest of
// the drive.
void hideSplash() {
  if (!g_splashUp) return;
  g_splashUp = false;
  g_splashRule = nullptr;
  if (w.splash) {
    lv_obj_delete(w.splash);  // takes the picture widget with it
    w.splash = nullptr;
  }
  // The picture's buffer is ours, not LVGL's: the widget only ever pointed at it.
  // Delete the widget first, then the buffer, so nothing is left holding a
  // pointer into freed PSRAM.
  splashimg::release();
  if (w.root) lv_obj_remove_flag(w.root, LV_OBJ_FLAG_HIDDEN);
  Serial.println("[ui] splash down: dashboard up");
}

// Advance the splash: fill the rule with the elapsed time and, at three seconds,
// hand the glass to the dashboard. Called every tick while the splash is up.
void updateSplash(uint32_t now) {
  if ((uint32_t)(now - g_splashStartMs) >= kSplashMs) {
    hideSplash();
    return;
  }
  if (g_splashRule) {
    const int w = (int)(((uint32_t)kRuleW * (now - g_splashStartMs)) / kSplashMs);
    lv_obj_set_width(g_splashRule, w < 1 ? 1 : w);
  }
}

}  // namespace

// ---------------------------------------------------------------- API
void begin() {
  lv_obj_t *scr = lv_screen_active();
  if (!scr) {
    // No display was registered (panel bring-up failed). Nothing in LVGL
    // is safe to touch, so leave the screen unbuilt.
    return;
  }
  lv_obj_remove_style_all(scr);
  lv_obj_set_style_bg_color(scr, COL_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Everything on the glass goes in one container: hiding it is what makes
  // the settings page cheap to scroll.
  w.root = opaque(scr, 0, 0, L::W, L::H, COL_BG);

  buildGauge(w.root);
  buildTiles(w.root);
  placeBand(g_variant);  // the panels are built with the full band's geometry;
                         // this is what makes the log line and g_placed true
  buildStatusBar(w.root);

  // Long-press the top band opens settings, as the tach ring used to: it is
  // the whole band above the tiles, which beats the 24 px status-bar buttons
  // as a target in a moving car. The rev bar is inside it.
  lv_obj_add_flag(w.top, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(w.top, onLongPress, LV_EVENT_LONG_PRESSED, nullptr);

  // Tap anywhere on the HALDEX panel to step the mode. A plain tap on
  // purpose: no confirm, because the panel shows the mode and the controller's
  // answer, so a stray tap is visible and one tap reverses it. The panel is
  // the target, so a tap that lands on any of its labels still counts.
  if (g_tileHaldex) {
    lv_obj_add_flag(g_tileHaldex, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_tileHaldex, onHaldexTap, LV_EVENT_CLICKED, nullptr);
  }

  g_edit = cfg::get();
  buildSettings();

  // The backlight is decided by the day/night rule, which needs the config it
  // was just loaded with, so it is applied from the first tick rather than here.
  applyBacklight();

  g_lastTickMs = millis();
  g_lastFlashMs = millis();
  g_lastIndMs = millis();

  // The splash goes up last, on top of the whole dashboard, and the dashboard
  // stays hidden underneath it until the hold elapses. Nothing else needs to
  // know: tick() takes both back down together.
  buildSplash(scr);
  g_splashUp = true;
  // The hold's clock starts *here*, not before buildSplash(). Decoding the
  // picture is the head of the wait, so counting it would put the rule already
  // part filled on the splash's first visible frame and then drop it early.
  g_lastTickMs = g_splashStartMs = millis();
  if (w.root) lv_obj_add_flag(w.root, LV_OBJ_FLAG_HIDDEN);
  Serial.printf("[ui] splash up (v%s), %lu ms\n", OHX_DASH_VERSION, (unsigned long)kSplashMs);
}

void applyDebugMask() {
  // The gauge parts are groups, so a group is one hide/show rather than 40
  // of them.
  struct Group {
    lv_obj_t *obj;
    uint16_t bit;
  };
  const Group groups[3] = {{w.revGrp, 1}, {w.ticksGrp, 2}, {w.numGrp, 4}};
  for (const Group &g : groups) {
    if (!g.obj) continue;
    if (dbg(g.bit)) {
      lv_obj_remove_flag(g.obj, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(g.obj, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (!w.top) return;
  if (g_dbgHideAll) {
    lv_obj_add_flag(w.top, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(w.top, LV_OBJ_FLAG_HIDDEN);
  }
}

void tick() {
  const uint32_t now = millis();
  const float dt = (float)(now - g_lastTickMs) / 1000.0f;
  g_lastTickMs = now;

  // Shift-light flash, independent of the data rate.
  if ((uint32_t)(now - g_lastFlashMs) >= kFlashMs) {
    g_lastFlashMs = now;
    g_flashOn = !g_flashOn;
  }

  // The indicator repeaters blink at the car's own rate, slower than the shift
  // cue, so the two cadences are tuned apart (#5).
  if ((uint32_t)(now - g_lastIndMs) >= kIndicatorFlashMs) {
    g_lastIndMs = now;
    g_indOn = !g_indOn;
  }

  const cfg::Data &c = cfg::get();
  // The merge point: ESP-NOW while its packets are arriving, HTTP for
  // coolant and for the lamps the packet does not carry. See net/datafeed.h.
  const telemetry::Live l = feed::live();
  const telemetry::Dash d = feed::dash();
  // The backlight runs on every page, not just the dashboard: it is a property
  // of the panel, and automatic mode has to keep working while the settings page
  // is open - which is where the number it produced is shown.
  updateDayNight(d, now);
  const bool stale = !feed::fresh(c.staleMs);
  // The screen's own idea of "stale" is what the user sees, so make every
  // transition visible on the console together with how old the data was.
  static bool lastStale = false;
  static uint32_t staleEvents = 0;
  static uint32_t maxAgeMs = 0;
  g_maxStaleAgeMs = maxAgeMs;
  if (stale) {
    const uint32_t age = l.ageMs;
    if (age != UINT32_MAX && age > maxAgeMs) maxAgeMs = age;
  }
  if (stale != lastStale) {
    lastStale = stale;
    if (stale) {
      staleEvents++;
      g_staleEvents = staleEvents;
    }
    Serial.printf("[ui] %s (data age %lu ms, source %s, event %lu)\n",
                  stale ? "STALE" : "LIVE", (unsigned long)l.ageMs, feed::sourceName(),
                  (unsigned long)staleEvents);
  }
  g_stale = stale;

  // The splash owns the glass for a fixed three seconds, controller or not, so
  // the panel always reaches the dashboard. updateSplash() fills the rule and,
  // at the end of the hold, deletes the splash and reveals the dashboard.
  if (g_splashUp) {
    updateSplash(now);
    return;
  }

  // Targets -> interpolated values.
  g_target.rpm = l.rpm;
  g_target.speed = l.speed;
  g_target.throttle = l.throttle;
  g_target.boost = l.boost;
  g_target.coolant = l.coolant;
  g_target.oil = l.oil;
  g_target.lockTarget = l.lockTarget;
  g_target.lockActual = l.lockActual;

  if (!stale) {
    g_shown.rpm = easeValue(g_shown.rpm, g_target.rpm, dt);
    g_shown.speed = easeValue(g_shown.speed, g_target.speed, dt);
    g_shown.throttle = easeValue(g_shown.throttle, g_target.throttle, dt);
    g_shown.boost = easeValue(g_shown.boost, g_target.boost, dt);
    g_shown.coolant = easeValue(g_shown.coolant, g_target.coolant, dt * 0.25f);
    g_shown.oil = easeValue(g_shown.oil, g_target.oil, dt * 0.25f);
    g_shown.lockTarget = easeValue(g_shown.lockTarget, g_target.lockTarget, dt);
    g_shown.lockActual = easeValue(g_shown.lockActual, g_target.lockActual, dt);

    // Fuel gets the hardest smoothing on the panel: the tank sloshes, and the
    // level is read at a glance rather than followed (#4). The gate drops any
    // single sample that moves the level faster than kFuelMaxRate l/s - a
    // one-sample spike is thrown away, while a real step (a fill-up) is believed
    // on its second sample, 100 ms later. On top of that the displayed level is a
    // 5 s low-pass. The car's own low-fuel warning is not touched by any of this.
    if (!isnan(l.fuel)) {
      const bool ok = isnan(g_fuelPrev) ||
                      fabsf(l.fuel - g_fuelPrev) <=
                          kFuelMaxRate * (float)(now - g_fuelPrevMs) / 1000.0f;
      if (ok) g_fuelAcc = l.fuel;
      g_fuelPrev = l.fuel;
      g_fuelPrevMs = now;
    }
    g_target.fuel = g_fuelAcc;
    g_shown.fuel = easeValue(g_shown.fuel, g_target.fuel, dt * (kTau / kFuelTau));

    // Peaks come from the raw values, not the eased ones. Speed and boost only:
    // the rpm peak lived in the status bar and the odometer has that slot now.
    if (!isnan(l.speed))
      g_peaks.speed = isnan(g_peaks.speed) ? l.speed : fmaxf(g_peaks.speed, l.speed);
    if (!isnan(l.boost))
      g_peaks.boost = isnan(g_peaks.boost) ? l.boost : fmaxf(g_peaks.boost, l.boost);
  }

  // The settings page is modal and this is the only point below which nothing
  // runs while it owns the glass, so its log row is refreshed here. Once a
  // second: a session's percentage and clock do not need more.
  static uint32_t lastLogUi = 0;
  if (w.settings && !lv_obj_has_flag(w.settings, LV_OBJ_FLAG_HIDDEN) &&
      (uint32_t)(now - lastLogUi) >= 1000) {
    lastLogUi = now;
    updateLogStatus();
  }

  // A modal page is on the glass and the dashboard is hidden behind it, so
  // there is nothing to update: leaving the tick running would invalidate the
  // bar, the number, the tiles and the status bar 30+ times a second under
  // it, and each of those areas drags a full repaint with it.
  if (g_page != Page::Dashboard) return;

  uint32_t t0 = micros();
  // The rev bar and the big number run at the full rate: the bar's edge is
  // what the eye tracks now that the needle is gone.
  updateRevBar(c, g_shown.rpm);
  updateGaugeText(c, g_shown.rpm);
  uint32_t t1 = micros();
  // The tiles carry values that only arrive at 10 Hz and are already eased,
  // so repainting them 30 times a second buys nothing.
  static uint32_t lastReadouts = 0;
  if ((uint32_t)(now - lastReadouts) >= 100) {
    lastReadouts = now;
    if (dbg(8)) updateReadouts(c, l, d);
    updateModeCmd(now);  // after the coupling line, so its answer wins
  }
  uint32_t t2 = micros();
  if (dbg(16)) updateConn(stale);
  uint32_t t3 = micros();

  g_gaugeUs += t1 - t0;
  g_readoutsUs += t2 - t1;
  g_connUs += t3 - t2;
  g_calls++;
}

uint32_t staleEvents() { return g_staleEvents; }
uint32_t maxStaleAgeMs() { return g_maxStaleAgeMs; }

void setDebugMask(uint16_t mask) {
  const bool wasHidden = g_dbgHideAll;
  g_dbgMask = mask;
  g_dbgHideAll = (mask == 0);
  (void)wasHidden;
  applyDebugMask();
  lv_obj_invalidate(lv_screen_active());
}

uint16_t debugMask() { return g_dbgMask; }

Timings takeTimings() {
  Timings t{g_gaugeUs, g_readoutsUs, g_connUs, g_calls};
  g_gaugeUs = g_readoutsUs = g_connUs = g_calls = 0;
  return t;
}

// One line about whether the controller would accept a command from this
// dash, and whether the key it would have to be signed with is set. The
// credential doubles as the station's join password, so this is the same
// field the operator just typed.
void updateControlStatus() {
  if (!w.ctrlLine) return;
  if (w.otaLine) {
    char ubuf[128];
    snprintf(ubuf, sizeof ubuf, "%s | page %s", ota::stateLine(), ota::url());
    setText(w.otaLine, ubuf);
    setTextColor(w.otaLine, ota::onTrial() ? COL_AMBER : COL_DIM);
  }
  // What automatic day / night has decided, and the number it produced, so the
  // two board_config numbers can be judged on the glass (#8).
  if (w.dayNightLine) {
    char db[48];
    if (g_term58s < 0) {
      snprintf(db, sizeof db, "58s -- / %s", g_night ? "night" : "day");
    } else {
      snprintf(db, sizeof db, "58s %d%% / %s %u%%", g_term58s, g_night ? "night" : "day",
               (unsigned)g_backlight);
    }
    setText(w.dayNightLine, db);
    setTextColor(w.dayNightLine, g_night ? COL_AMBER : COL_DIM);
  }
  const ohxnow::Status s = ohxnow::last();
  char buf[160];
  if (!s.valid) {
    setText(w.ctrlLine, "control: no packet from the controller yet");
    setTextColor(w.ctrlLine, COL_DIM);
    setText(w.ctrlWarn, "");
    return;
  }
  // "signing key" does not fit: this line is 300 px at font 14 and the long
  // form was 357-411 px, so its end was cut off in both states. The warning
  // below says what the key is for.
  snprintf(buf, sizeof buf, "control: %s | key: %s",
           s.ctrlAccept ? "accepted over ESP-NOW" : "refused (controller off)",
           s.keyed ? "set" : "NONE");
  setText(w.ctrlLine, buf);
  setTextColor(w.ctrlLine, s.ctrlAccept ? COL_GREEN : COL_AMBER);
  if (!s.keyed) {
    setText(w.ctrlWarn, "Open access point: set one, or anyone in range can send commands");
  } else {
    setText(w.ctrlWarn, "");
  }
}

void showSettings(bool visible) {
  if (visible) {
    updateControlStatus();
    updateLogStatus();
    g_edit = cfg::get();
    lv_textarea_set_text(g_taSsid, g_edit.ssid);
    lv_textarea_set_text(g_taPass, g_edit.pass);
    lv_textarea_set_text(g_taHost, g_edit.host);
    // The steppers are the same story as the text areas: g_edit has just been
    // reloaded from the config, so the numbers on screen have to be redrawn to
    // match, or an edit abandoned by closing the page leaves stale values in
    // front of the fields they no longer describe (#12). The three toggles and
    // the day/night button are not steppers, so they are re-labelled here too.
    refreshSteppers();
    setText(lv_obj_get_child(g_btnTempUnits, 0), g_edit.tempF ? "\xC2\xB0" "F" : "\xC2\xB0" "C");
    setText(lv_obj_get_child(g_btnBoostUnits, 0), kBoostUnitNames[g_edit.boostUnits]);
    setText(lv_obj_get_child(g_btnDistUnits, 0), g_edit.unitsMetric ? "km" : "mi");
    setText(lv_obj_get_child(g_btnDayNight, 0), kDayNightNames[g_edit.dayNight]);
  } else if (w.kb) {
    lv_keyboard_set_textarea(w.kb, nullptr);
    lv_obj_add_flag(w.kb, LV_OBJ_FLAG_HIDDEN);
  }
  setPage(visible ? Page::Settings : Page::Dashboard);
}

// One place that decides what is on the glass. Everything else here and in
// raw_ui goes through it, so the dashboard can never be left drawn under a
// page that is on top of it.
void setPage(Page p) {
  g_page = p;
  const bool settings = (p == Page::Settings);

  if (w.settings) {
    if (settings) {
      lv_obj_remove_flag(w.settings, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(w.settings);
    } else {
      lv_obj_add_flag(w.settings, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (w.kb && !settings) {
    lv_keyboard_set_textarea(w.kb, nullptr);
    lv_obj_add_flag(w.kb, LV_OBJ_FLAG_HIDDEN);
  }
  // Hidden rather than merely covered: a covered object is still drawn
  // first, which is the whole cost this avoids. While the boot splash is up the
  // dashboard stays hidden whatever page is asked for, so a stray console
  // command cannot put it on the glass behind the splash.
  if (w.root) {
    if (p == Page::Dashboard && !g_splashUp) {
      lv_obj_remove_flag(w.root, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(w.root, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

Page page() { return g_page; }

// Which tile band the glass is showing, and a way to change it. The capability
// decision calls this once when it settles; the console can call it by hand.
void applyLayout(layout::Variant v) { placeBand(v); }

layout::Variant layoutVariant() { return g_variant; }

void cycleHaldexMode() { cycleMode(); }

bool settingsOpen() { return g_page == Page::Settings; }

}  // namespace ui

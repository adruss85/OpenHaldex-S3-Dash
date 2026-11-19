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
#include "net/can_relay.h"
#include "app/capability.h"
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
// The two halves of "write NVS without losing the picture" (defined with the
// backlight below, used by SAVE and DEFAULTS first).
void nvsGuardBegin();
void nvsGuardEnd();
}  // namespace

// Forward declarations at UI scope, not inside the anonymous namespace below:
// these two are public entry points defined further down, and a second
// declaration in the anonymous namespace would make every call ambiguous.
void applyTheme(theme::Scheme s);
void applyDebugMask();

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

// Fuel smoothing (#4): the tank sloshes, so the displayed level is a 60 s
// exponential low-pass (kFuelTau) - a minute, not the original 5 s, because a
// hill or a stop-start crawl moved a 5 s filter enough to read as a real
// change - and a sample that would move it faster than 8 l/s is dropped rather
// than latched. The car's own low-fuel warning is NOT smoothed - it is read
// straight off the frame.
constexpr float kFuelTau = 60.0f;
constexpr float kFuelMaxRate = 8.0f;  // l/s
constexpr float kOilPressMaxBar = 10.0f;  // PST-F 1 full scale; only sizes the bar

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
  // The tile band, as one container above the status bar. It is its own object
  // so a view can take it off the glass with one flag instead of four.
  lv_obj_t *bandGrp = nullptr;
  // The boot splash, a sibling of root and on top of it. It is built once and
  // deleted when the hold elapses, so it costs nothing for the rest of the
  // drive. Its picture is a child of it; the rule, tagline and version are
  // siblings drawn over that.
  lv_obj_t *splash = nullptr;

  // The rev bar, its cells, the row of numbers under it and the big number's
  // row. A group each, so the debug mask can hide them one at a time.
  lv_obj_t *revGrp = nullptr, *ticksGrp = nullptr, *numGrp = nullptr;
  lv_obj_t *revCell[L::BAR_SEG] = {};
  // The shift/limiter flash: one full-width opaque plate over the cells, not a
  // recolour of each lit cell. See updateRevBar() for why (40 invalid areas in
  // one refresh overflow LVGL's invalidation buffer and turn into a full-screen
  // repaint) and for why it is the whole width.
  lv_obj_t *revFlash = nullptr;
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
  // The ForbesAutomotive patch's second reading, in the temperatures card under
  // the oil-temperature bar. It belongs with the engine readings and stays on
  // the glass when the BOOST panel is switched off.
  lv_obj_t *oilPressCap = nullptr, *oilPressVal = nullptr, *oilPressBar = nullptr;
  lv_obj_t *oilPressUnit = nullptr;
  // The temperatures card's captions move when the card gains or loses the
  // oil-pressure row, so they are kept.
  lv_obj_t *tempCapCool = nullptr, *tempCapOil = nullptr, *tempCapFuel = nullptr;
  lv_obj_t *coolVal = nullptr, *coolBar = nullptr, *coolUnit = nullptr;
  lv_obj_t *oilVal = nullptr, *oilBar = nullptr, *oilUnit = nullptr;
  lv_obj_t *fuelVal = nullptr, *fuelBar = nullptr, *fuelUnit = nullptr;
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
  // The drive log: a session is started by hand, so it is a button and a state
  // line rather than a setting (no NVS field, nothing to leave on by accident).
  lv_obj_t *btnLog = nullptr, *logLine = nullptr;
  // The palette and view buttons live in the settings header, the only strip of
  // that page with room left (the columns below it are full).
  lv_obj_t *btnTheme = nullptr, *btnView = nullptr;

  // ---- the other two views (View in dash_ui.h) ----------------------
  // Built once and hidden. They take the glass where the rev bar and tile band
  // would be; the status bar below them is the same object in every view, which
  // is why switching a view never rebuilds a lamp or re-decides LIVE/STALE.
  lv_obj_t *minGrp = nullptr, *tempsGrp = nullptr;
  struct {
    lv_obj_t *speed = nullptr, *speedUnit = nullptr, *rpm = nullptr;
    lv_obj_t *rpmBar = nullptr;
    lv_obj_t *cap[4] = {}, *val[4] = {}, *unit[4] = {};
  } mn;
  // Rows 0..3 are coolant, oil, oil pressure, fuel. Row 2 is hidden on a car
  // without the ME7 oil-pressure patch, exactly as it is in the tile.
  struct {
    lv_obj_t *cap[4] = {}, *val[4] = {}, *unit[4] = {}, *bar[4] = {}, *note[4] = {};
  } tv;
} w;

struct Values {
  float rpm, speed, throttle, boost, coolant, oil, lockTarget, lockActual, fuel, oilPressure;
};
constexpr Values kBlank = {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};

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
// Which dashboard view the dashboard is showing. It shares the status bar with
// the other views, so it is not a Page: only the band above the status bar
// changes.
View g_view = View::Race;
bool g_stale = true;
// Set when a palette change has invalidated the *computed* colours (a value's
// warn/crit shade, a crit plate). Those are written locally every readout, so
// the way to bring them onto the new palette is to run that pass again now
// rather than wait for the next 100 ms tick - which the flag does by making the
// tick's "100 ms since the last pass" test true on the next frame.
bool g_forceReadout = false;
// When the deferred theme/view write is due. Zero = nothing pending. A tap must
// not touch flash: the write is what starves the panel's DMA, so it is moved to
// a moment when the glass has been quiet for a second and a half - which also
// means cycling through all six schemes writes once, not six times.
constexpr uint32_t kDisplaySaveMs = 1500;
uint32_t g_displaySaveAt = 0;

// Defined below with the views; the band placement needs both, and it is
// declared up here so a capability change can re-place a live view.
void clearPlates();
void placeTempsView();


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

// The flash plate's last painted state: 0 hidden, 2 flash colour, 3 dark half.
// Kept so a steady bar costs no style writes; the colour itself is re-asserted
// through setBgColor(), which ignores an unchanged value.
int8_t g_revFlash = 0;

// Bench only: 0 normal, 1 the flash half, 2 the dark half. The shift cue is the
// one thing that cannot be produced on a bench, because it needs rpm and rpm
// needs the car; this is how the plate's geometry, its full width and its cost
// are inspected with the frame dump `d`. Console `z` cycles it.
int8_t g_forceShift = 0;

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
lv_obj_t *opaque(lv_obj_t *parent, int x, int y, int width, int height, theme::Col bg) {
  lv_obj_t *o = plain(parent, x, y, width, height);
  theme::styleBg(o, bg, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  return o;
}

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, theme::Col color, const char *text) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_remove_flag(l, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_style_text_font(l, font, 0);
  theme::styleText(l, color);
  lv_label_set_text(l, text);
  return l;
}

// A fixed-size box with centred text: avoids depending on measured label
// width when placing text at an exact point.
lv_obj_t *boxedLabel(lv_obj_t *parent, int cx, int cy, int boxW, int boxH,
                     const lv_font_t *font, theme::Col color, const char *text) {
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
                     theme::Col color, const char *text) {
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
  theme::styleBg(r, COL_LINE, 0);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  return r;
}

lv_obj_t *bar(lv_obj_t *parent, int x, int y, int width, int height, theme::Col fill,
              int32_t min, int32_t max) {
  lv_obj_t *b = lv_bar_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, width, height);
  lv_obj_set_style_radius(b, height / 2, 0);
  theme::styleBg(b, COL_TRACK, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(b, height / 2, LV_PART_INDICATOR);
  theme::styleBg(b, fill, LV_PART_INDICATOR);
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
  theme::styleBg(b, COL_BUTTON_FILL, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  theme::styleBorder(b, COL_LAMP_EDGE, 0);
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
  theme::styleBg(l, COL_LAMP_OFF, 0);
  lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(l, 1, 0);
  theme::styleBorder(l, COL_LAMP_EDGE, 0);
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

  setBgColor(l, on ? bg : (lv_color_t)COL_LAMP_OFF, 0);
  setBorderColor(l, on ? bg : (lv_color_t)COL_LAMP_EDGE);
  setTextColor(l, on ? fg : (lv_color_t)COL_LAMP_TXT_OFF);
}

// The tell-tale lamps: warn, or crit for the ones that mean something is wrong.
// A crit plate is the scheme's own accent with the palette's contrasting text
// on it (theme.h), so a lamp in the orange scheme is black-on-amber where
// Midnight is white-on-red.
void lampState(lv_obj_t *l, bool on, bool red) {
  lampPaint(l, on, red ? COL_RED : COL_AMBER,
            red ? (lv_color_t)COL_CRIT_TEXT : lv_color_black());
}

// The cruise lamp: three states rather than two, because "armed" (the column
// switch is on) and "engaged" (a speed is stored) are different facts and
// telling them apart is the whole point of showing it. A very dark green when
// armed, the indicator green when a speed is stored: the armed state is a
// reminder that the system exists, not a call to look at it.
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
    // Text on the accent plate is the scheme's own: black on green/amber/
    // white, white on blue/purple (theme.h, Role::AccentText).
    lampPaint(l, true, COL_GREEN, COL_ACCENT_TEXT);
  }
}

// ---------------------------------------------------------------- gauge
// How many cells the bar is filled to, for a given rpm. Shared by the two
// places that need it: the normal per-frame step, and the whole-bar repaint.
int revLitCount(const cfg::Data &c, float rpm) {
  const float frac = isnan(rpm) ? 0.0f : (rpm / c.rpmMax);
  int lit = (int)lroundf(frac * L::BAR_SEG);
  if (lit < 0) lit = 0;
  if (lit > L::BAR_SEG) lit = L::BAR_SEG;
  return lit;
}

// Recolour every cell in one pass, and repaint the bar as ONE invalid area
// instead of eighty.
//
// Measured on this board: eighty separate invalid areas cost about as much as
// a full-screen repaint (174 ms against 190-230 ms), because what is expensive
// is the per-area draw pass over the object tree, not the pixels - 80 cells are
// only 83k pixels. One invalidation for the whole 800x104 bar costs about 15 ms.
// So the colours are written with the display's invalidation suspended (a
// counter in LVGL, documented as "temporarily"), and the bar is then
// invalidated once. Everything written here is read back at draw time, so
// nothing is lost by not invalidating per object.
//
// This is the palette-change and scale-change path. A normal rpm step still
// goes through updateRevBar()'s per-cell compare, which repaints one cell.
void repaintBarAll(const cfg::Data &c, float rpm) {
  if (!w.revGrp) return;
  const int lit = revLitCount(c, rpm);
  lv_display_t *disp = lv_display_get_default();
  if (disp) lv_display_enable_invalidation(disp, false);
  for (int i = 0; i < L::BAR_SEG; i++) {
    const int8_t want = (i >= lit) ? 0 : 1;
    g_revLit[i] = want;
    setBgColor(w.revCell[i], want ? g_revCol[i].lit : g_revCol[i].unlit, 0);
  }
  if (disp) lv_display_enable_invalidation(disp, true);
  // An invalidation of a hidden object is dropped by LVGL, so this costs
  // nothing while another view or page owns the glass; the cells keep the new
  // colours and are painted when the bar is shown again.
  // An invalidation of a hidden object is dropped by LVGL (it walks the parent
  // chain), so this costs nothing while another view or page owns the glass:
  // the cells keep their new colours and are painted when the bar is shown.
  lv_obj_invalidate(w.revGrp);
}

// Everything that depends on the rpm/boost scale: which zone each rev cell
// belongs to, and the numbers along the bar. Called once at build time and
// again whenever the settings change the scale - or the palette.
void syncGauge() {
  const cfg::Data &c = cfg::get();
  // The tick numbers only move when the scale does. A palette change comes
  // through here too, and rewriting twelve label positions that have not moved
  // would add twelve invalid areas - text areas, the expensive kind - to a
  // repaint that is trying to stay small.
  static uint16_t lastMax = 0, lastAmber = 0, lastRed = 0;
  const bool scaleMoved =
      lastMax != c.rpmMax || lastAmber != c.rpmAmber || lastRed != c.rpmRedline;
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
  }

  // One number per 1000 rpm, placed in proportion to the scale, so a 7000 scale
  // spreads them wider than an 8000 one. The left end (0) has no number, and a
  // label that would run under the "x1000" tag at the right-hand end is dropped:
  // with a free scale the top label can land there (12000 hides the "11").
  if (!scaleMoved) {
    repaintBarAll(c, g_shown.rpm);
    return;
  }
  lastMax = c.rpmMax;
  lastAmber = c.rpmAmber;
  lastRed = c.rpmRedline;

  const int perK = (int)lroundf(c.rpmMax / 1000.0f);
  int n = perK - 1;
  if (n < 1) n = 1;
  if (n > (int)(sizeof(w.tickLabel) / sizeof(w.tickLabel[0]))) {
    n = (int)(sizeof(w.tickLabel) / sizeof(w.tickLabel[0]));
  }
  const int labelW = 40;
  const int tagW = 48;  // the unit tag, drawn at BAR_W - tagW
  char buf[8];
  int shown = 0;
  for (int i = 0; i < n; i++) {
    const float f = (float)((i + 1) * 1000) / c.rpmMax;
    const int x = (int)lroundf(f * L::BAR_W) - labelW / 2;
    if (x + labelW > L::BAR_W - tagW) {
      lv_obj_add_flag(w.tickLabel[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    snprintf(buf, sizeof(buf), "%d", i + 1);  // thousands
    setText(w.tickLabel[i], buf);
    lv_obj_set_x(w.tickLabel[i], x);
    lv_obj_remove_flag(w.tickLabel[i], LV_OBJ_FLAG_HIDDEN);
    shown++;
  }
  for (int i = n; i < (int)(sizeof(w.tickLabel) / sizeof(w.tickLabel[0])); i++) {
    lv_obj_add_flag(w.tickLabel[i], LV_OBJ_FLAG_HIDDEN);
  }
  w.tickLabels = shown;

  // The unit tag is always shown now: the scale is free, so the labels are
  // rarely whole thousands and bare "1..7" would mean nothing without it.
  lv_obj_remove_flag(w.tickUnit, LV_OBJ_FLAG_HIDDEN);

  // The zones or the palette have moved: repaint the bar itself. g_shown.rpm is
  // the value the glass is showing, so the fill edge is right for the new scale
  // rather than for the last frame's.
  repaintBarAll(c, g_shown.rpm);
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

// Lit -> the arrow is on the glass in the accent colour. Dark -> it is gone
// entirely. The colour is re-asserted while lit (the setter is a no-op when it
// already matches), because that is what re-colours a lit arrow after a theme
// change; the flag is still only touched on a real transition, since every flag
// write invalidates.
void setIndicator(lv_obj_t *arrow, bool lit) {
  if (!arrow) return;
  const bool hidden = lv_obj_has_flag(arrow, LV_OBJ_FLAG_HIDDEN);
  if (lit) {
    setTextColor(arrow, COL_GREEN);
    if (hidden) lv_obj_remove_flag(arrow, LV_OBJ_FLAG_HIDDEN);
  } else if (!hidden) {
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
    const int x1 = ((i + 1) * L::BAR_W) / L::BAR_SEG;
    // untracked: the palette change repaints the whole bar as one area
    // (repaintBarAll), so these 80 cells must not join the per-object palette
    // list - filling it would push a scheme change back to a full-screen
    // repaint, which is what that helper exists to avoid.
    w.revCell[i] = opaque(w.revGrp, x0, 0, x1 - x0, L::REV_BAR_H,
                          theme::untracked(COL_ZONE_GREEN));
  }
  // Created after the cells so it is on top of them, hidden until a shift.
  w.revFlash = opaque(w.revGrp, 0, 0, L::BAR_W, L::REV_BAR_H, COL_SHIFT);
  lv_obj_add_flag(w.revFlash, LV_OBJ_FLAG_HIDDEN);

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
// The four panels are always built; whether BOOST is on the glass, and where
// the other three sit, follows the Boost tile setting and is applied by
// applyLayout() below.
lv_obj_t *g_tileSpeed = nullptr, *g_tileBoost = nullptr;
lv_obj_t *g_tileTemps = nullptr, *g_tileHaldex = nullptr;
layout::Variant g_variant = layout::Variant::Full;
bool g_placed = false;
bool g_oilShown = false;  // the OIL PRESS row is on the glass (capability, not layout)

// The temperatures panel's rows share one pitch, so the build pass and the
// placement pass cannot drift apart. Four rows are usual (coolant / oil / oil
// pressure / fuel) at 55 px; a car without the ME7 patch loses the pressure row
// and its three rows go back to the wide 80 px pitch, so nothing is left empty.
//
// The card carries no title of its own - the captions say what the card is - so
// its first row has to sit where every other card's caption sits, TILE_PAD_V.
// The anchor per row is therefore the *caption's* box top, not a notional row
// top.
//
// The caption and the value share a baseline. At montserrat_12 (FONT_CAP) the
// caption's baseline is 12 px below its box top; at montserrat_22 (FONT_TEMP)
// the value's is 20 px below its. So a value box 8 px above the caption's puts
// the two on one line - the relationship the Haldex tile's LOCK REQ / LOCK ACT
// rows already use. The bar runs 30 px below the caption.
constexpr int kTempY0 = L::TILE_PAD_V;
constexpr int kTempPitchTight = 55;  // four rows: coolant, oil, oil press, fuel
constexpr int kTempPitchWide = 80;   // three rows, no oil-pressure sensor
constexpr int kTempValDY = -8;   // value box top, from the caption's
constexpr int kTempValH = 30;
constexpr int kTempBarDY = 30;   // bar top, from the caption's
// Every reading in the card ends with a unit label, and "bar"/"psi" needs 24 px
// where "°C" and "L" need 16. One unit column for all rows keeps the numbers on
// one right edge; the labels are right-aligned so the units themselves finish
// flush with the bar.
constexpr int kTempUnitW = 24;

// The tile unit labels are single letters: C, F, L, V all start on the same
// left edge. The Settings button still spells the choice as °C/°F.
constexpr const char *kDegC = "C";
constexpr const char *kDegF = "F";

// The speed and boost panels put their unit and peak on the same two lines, so
// the two tiles read across. The speed value is one font size taller, but its
// drawn line ends at 96 px, where the unit starts.
constexpr int kTileUnitY = 96;
constexpr int kTilePeakY = 116;

void buildTiles(lv_obj_t *parent) {
  // Band-relative: the tiles live in w.bandGrp, which sits at L::TILE_Y.
  const int y = 0;
  const int h = L::TILE_H;

  // ---- SPEED ------------------------------------------------------
  lv_obj_t *t0 = opaque(parent, L::COL0_X, y, L::COL0_W, h, COL_BG);
  g_tileSpeed = t0;
  lv_obj_set_style_border_side(t0, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_border_width(t0, 1, 0);
  theme::styleBorder(t0, COL_LINE, 0);

  lv_obj_t *cap = label(t0, FONT_CAP, COL_DIM, "SPEED");
  lv_obj_set_pos(cap, L::TILE_PAD_H, L::TILE_PAD_V);

  w.speedVal = fixedLabel(t0, L::TILE_PAD_H - 2, 44, 132, 58, FONT_BIG, COL_FG, "--");

  // The unit and peak sit directly under the big number, as they do on the
  // boost side.
  w.speedUnit = fixedLabel(t0, L::TILE_PAD_H, kTileUnitY, 70, 18, FONT_SMALL, COL_DIM, "km/h");

  w.speedPeak = fixedLabel(t0, L::TILE_PAD_H, kTilePeakY, L::COL0_W - 2 * L::TILE_PAD_H, 18,
                           FONT_SMALL, COL_DIM, "peak --");

  // ---- BOOST ------------------------------------------------------
  lv_obj_t *t1 = opaque(parent, L::COL1_X, y, L::COL1_W, h, COL_BG);
  g_tileBoost = t1;
  lv_obj_set_style_border_side(t1, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_border_width(t1, 1, 0);
  theme::styleBorder(t1, COL_LINE, 0);

  cap = label(t1, FONT_CAP, COL_DIM, "BOOST");
  lv_obj_set_pos(cap, L::TILE_PAD_H, L::TILE_PAD_V);

  const int bw = L::COL1_W - 2 * L::TILE_PAD_H;
  w.boostVal = fixedLabel(t1, L::TILE_PAD_H, 40, bw, 52, FONT_NUM, COL_FG, "--");
  w.boostUnit = fixedLabel(t1, L::TILE_PAD_H, kTileUnitY, bw, 18, FONT_SMALL, COL_DIM, "bar");
  // Peak under the unit, then the bar. The oil-pressure row that used to share
  // this tile now lives in the temperatures card, with the other engine
  // readings, so this panel is boost alone again.
  w.boostPeak = fixedLabel(t1, L::TILE_PAD_H, kTilePeakY, bw, 18, FONT_SMALL, COL_DIM, "peak --");
  w.boostBar = bar(t1, L::TILE_PAD_H, 140, bw, 10, COL_WHITE, 0, 100);

  // ---- TEMPERATURES -----------------------------------------------
  lv_obj_t *t2 = opaque(parent, L::COL2_X, y, L::COL2_W, h, COL_BG);
  g_tileTemps = t2;
  lv_obj_set_style_border_side(t2, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_border_width(t2, 1, 0);
  theme::styleBorder(t2, COL_LINE, 0);

  // Four rows: coolant, oil, oil pressure, fuel. The card is named by its
  // contents, so the first row starts where the cards beside it put their
  // captions. Values are the 22 px face; every unit is the same small grey
  // stock font on the caption's baseline, right-aligned in one unit column so
  // "°C", "L", "bar" and "psi" all finish flush with the bar.
  const int tw = L::COL2_W - 2 * L::TILE_PAD_H;
  const int valX = L::TILE_PAD_H + tw - 104;
  const int unitX = L::TILE_PAD_H + tw - kTempUnitW;
  const int rowCool = kTempY0;
  const int rowOil = kTempY0 + kTempPitchTight;
  const int rowPress = kTempY0 + 2 * kTempPitchTight;
  const int rowFuel = kTempY0 + 3 * kTempPitchTight;

  w.tempCapCool = label(t2, FONT_CAP, COL_DIM, "COOLANT");
  lv_obj_set_pos(w.tempCapCool, L::TILE_PAD_H, rowCool);
  w.coolVal = fixedLabel(t2, valX, rowCool + kTempValDY, 80, kTempValH, FONT_TEMP, COL_FG, "--");
  lv_obj_set_style_text_align(w.coolVal, LV_TEXT_ALIGN_RIGHT, 0);
  w.coolUnit = fixedLabel(t2, unitX, rowCool, kTempUnitW, 18, FONT_SMALL, COL_DIM, kDegC);
  lv_obj_set_style_text_align(w.coolUnit, LV_TEXT_ALIGN_RIGHT, 0);
  w.coolBar = bar(t2, L::TILE_PAD_H, rowCool + kTempBarDY, tw, 10, COL_GREEN, 0, 100);

  w.tempCapOil = label(t2, FONT_CAP, COL_DIM, "OIL");
  lv_obj_set_pos(w.tempCapOil, L::TILE_PAD_H, rowOil);
  w.oilVal = fixedLabel(t2, valX, rowOil + kTempValDY, 80, kTempValH, FONT_TEMP, COL_FG, "--");
  lv_obj_set_style_text_align(w.oilVal, LV_TEXT_ALIGN_RIGHT, 0);
  w.oilUnit = fixedLabel(t2, unitX, rowOil, kTempUnitW, 18, FONT_SMALL, COL_DIM, kDegC);
  lv_obj_set_style_text_align(w.oilUnit, LV_TEXT_ALIGN_RIGHT, 0);
  w.oilBar = bar(t2, L::TILE_PAD_H, rowOil + kTempBarDY, tw, 10, COL_GREEN, 0, 100);

  // The ME7 patch's oil pressure, directly under the oil-temperature bar. It
  // has a bar of its own (a fraction of kOilPressMaxBar) and the shared
  // pressure unit. Hidden until capability detection proves the patch, and
  // placeTempsTile() then either keeps the four-row pitch or closes the gap.
  w.oilPressCap = label(t2, FONT_CAP, COL_DIM, "OIL PRES");
  lv_obj_set_pos(w.oilPressCap, L::TILE_PAD_H, rowPress);
  lv_obj_add_flag(w.oilPressCap, LV_OBJ_FLAG_HIDDEN);
  w.oilPressVal =
      fixedLabel(t2, valX, rowPress + kTempValDY, 80, kTempValH, FONT_TEMP, COL_FG, "--");
  lv_obj_set_style_text_align(w.oilPressVal, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_add_flag(w.oilPressVal, LV_OBJ_FLAG_HIDDEN);
  w.oilPressUnit = fixedLabel(t2, unitX, rowPress, kTempUnitW, 18, FONT_SMALL, COL_DIM, "bar");
  lv_obj_set_style_text_align(w.oilPressUnit, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_add_flag(w.oilPressUnit, LV_OBJ_FLAG_HIDDEN);
  w.oilPressBar = bar(t2, L::TILE_PAD_H, rowPress + kTempBarDY, tw, 10, COL_GREEN, 0, 100);
  lv_obj_add_flag(w.oilPressBar, LV_OBJ_FLAG_HIDDEN);

  // FUEL is the bottom row, where the diagnostic battery voltage used to be.
  // BATT is no longer drawn: it stays on the raw page as a diagnostic.
  w.tempCapFuel = label(t2, FONT_CAP, COL_DIM, "FUEL");
  lv_obj_set_pos(w.tempCapFuel, L::TILE_PAD_H, rowFuel);
  w.fuelVal = fixedLabel(t2, valX, rowFuel + kTempValDY, 80, kTempValH, FONT_TEMP, COL_FG, "--");
  lv_obj_set_style_text_align(w.fuelVal, LV_TEXT_ALIGN_RIGHT, 0);
  w.fuelUnit = fixedLabel(t2, unitX, rowFuel, kTempUnitW, 18, FONT_SMALL, COL_DIM, "L");
  lv_obj_set_style_text_align(w.fuelUnit, LV_TEXT_ALIGN_RIGHT, 0);
  w.fuelBar = bar(t2, L::TILE_PAD_H, rowFuel + kTempBarDY, tw, 10, COL_GREEN, 0, 100);

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
  lv_obj_set_pos(t, x, 0);
  lv_obj_set_size(t, width, height);
  lv_obj_set_pos(w.speedVal, L::TILE_PAD_H - 2, 44);
  lv_obj_set_pos(w.speedUnit, L::TILE_PAD_H, kTileUnitY);
  lv_obj_set_pos(w.speedPeak, L::TILE_PAD_H, kTilePeakY);
  lv_obj_set_width(w.speedPeak, width - 2 * L::TILE_PAD_H);
}

void placeBoostTile(lv_obj_t *t, int x, int width, int height) {
  lv_obj_set_pos(t, x, 0);
  lv_obj_set_size(t, width, height);
  const int bw = width - 2 * L::TILE_PAD_H;
  lv_obj_set_width(w.boostVal, bw);
  lv_obj_set_width(w.boostUnit, bw);
  lv_obj_set_width(w.boostBar, bw);
  lv_obj_set_pos(w.boostPeak, L::TILE_PAD_H, kTilePeakY);
  lv_obj_set_width(w.boostPeak, bw);
}

void placeTempsTile(lv_obj_t *t, int x, int width, int height) {
  lv_obj_set_pos(t, x, 0);
  lv_obj_set_size(t, width, height);
  const int tw = width - 2 * L::TILE_PAD_H;
  const int valX = L::TILE_PAD_H + tw - 104;
  const int unitX = L::TILE_PAD_H + tw - kTempUnitW;

  // Four rows on a patched car: coolant, oil, oil pressure, fuel. Without the
  // patch the pressure row is hidden and the three remaining rows use the wide
  // pitch, so a stock car gets no gap where a reading it cannot make would be.
  const bool press = cap::get().oilPressure;
  const int pitch = press ? kTempPitchTight : kTempPitchWide;
  const int rowOil = kTempY0 + pitch;
  const int rowPress = rowOil + pitch;
  const int rowFuel = press ? rowPress + pitch : rowOil + pitch;

  lv_obj_set_pos(w.tempCapOil, L::TILE_PAD_H, rowOil);
  lv_obj_set_pos(w.tempCapFuel, L::TILE_PAD_H, rowFuel);

  lv_obj_set_width(w.coolBar, tw);
  lv_obj_set_pos(w.coolVal, valX, kTempY0 + kTempValDY);
  lv_obj_set_pos(w.coolUnit, unitX, kTempY0);
  lv_obj_set_pos(w.oilVal, valX, rowOil + kTempValDY);
  lv_obj_set_pos(w.oilUnit, unitX, rowOil);
  lv_obj_set_width(w.oilBar, tw);
  lv_obj_set_pos(w.oilBar, L::TILE_PAD_H, rowOil + kTempBarDY);
  lv_obj_set_pos(w.oilPressVal, valX, rowPress + kTempValDY);
  lv_obj_set_pos(w.oilPressUnit, unitX, rowPress);
  lv_obj_set_pos(w.oilPressBar, L::TILE_PAD_H, rowPress + kTempBarDY);
  lv_obj_set_width(w.oilPressBar, tw);
  lv_obj_set_pos(w.fuelVal, valX, rowFuel + kTempValDY);
  lv_obj_set_pos(w.fuelUnit, unitX, rowFuel);
  lv_obj_set_width(w.fuelBar, tw);
  lv_obj_set_pos(w.fuelBar, L::TILE_PAD_H, rowFuel + kTempBarDY);

  if (press) {
    lv_obj_remove_flag(w.oilPressCap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(w.oilPressVal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(w.oilPressUnit, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(w.oilPressBar, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(w.oilPressCap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(w.oilPressVal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(w.oilPressUnit, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(w.oilPressBar, LV_OBJ_FLAG_HIDDEN);
  }
}

void placeHaldexTile(lv_obj_t *t, int x, int width, int height) {
  lv_obj_set_pos(t, x, 0);
  lv_obj_set_size(t, width, height);
  const int hw = width - 2 * L::TILE_PAD_H;
  lv_obj_set_width(w.modeVal, hw);
  lv_obj_set_width(w.haldexSub, hw);
  lv_obj_set_pos(w.lockReqVal, width - 110, 94);
  lv_obj_set_pos(w.lockActVal, width - 110, 128);
  lv_obj_set_width(w.thrBar, hw);
}

// Called when the Boost tile setting or a capability-derived row changes, and
// by hand from the console for bench work. Idempotent: re-applying the same
// band re-places the same pixels and repaints nothing.
void placeBand(layout::Variant v) {
  const layout::TileBand &band = layout::bandFor(v);
  bool boostShown = false;

  // A plate has grown the box it belongs to; put it back before the placement
  // pass writes the same coordinates again.
  clearPlates();
  // The Temps view has its own row geometry and the same capability input.
  placeTempsView();

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
  // The oil-pressure row is the one capability still on the glass. It lives in
  // the temperatures card, and placeTempsTile() has already shown or hidden it
  // and chosen the pitch, so here it only counts as a repaint-worthy change.
  const bool oilShown = cap::get().oilPressure;
  const bool oilChanged = oilShown != g_oilShown;
  g_oilShown = oilShown;

  const bool changed = (v != g_variant) || !g_placed || oilChanged;
  g_variant = v;
  g_placed = true;

  if (changed) {
    // The panels that just moved or appeared have to be repainted, and so
    // does the strip the hidden one used to occupy.
    if (w.root) lv_obj_invalidate(w.root);
    Serial.printf("[ui] tile band %s (%d panels), oil pressure %s\n", band.name, band.count,
                  oilShown ? "shown" : "hidden");
  }
}

// ------------------------------------------------------------- views
// Two more ways to read the same numbers. Both are groups inside root, above
// the status bar, and both are built once: see View in dash_ui.h for why they
// are not pages. They draw the values the Race view already eases (g_shown), so
// switching costs a hide/show and never a re-read of a transport.
//
// The geometry is fixed pixels like everything else here (theme.h), and the
// reading rows are at whole-pixel positions because the fonts' line heights
// are pinned (52 for FONT_NUM, 30 for FONT_TEMP).

/// The crit plate (theme.h): outside Midnight "critical" has no red, so the
/// reading inverts instead - a solid accent plate with the palette's
/// contrasting text. The box grows by kPlatePad on every side so the digits do
/// not move; the state is remembered per object, so a steady reading writes no
/// styles at all, and clearPlates() restores the geometry when a tile is
/// re-placed.
constexpr int kPlatePad = 4;

struct PlateState {
  lv_obj_t *obj = nullptr;
  bool valid = false;
  bool on = false;
  int x = 0, y = 0, w = 0, h = 0;
};
PlateState g_plates[10];

void plateApply(const PlateState &st, bool on) {
  lv_obj_t *l = st.obj;
  if (on) {
    // The plate is the label's own background, grown by kPlatePad on every
    // side with matching padding, so the digits do not move. Where the label
    // sits at the very top of its parent (the temperatures card's values are
    // at y -1) growing upwards would be clipped by the parent, and a plate
    // with two cut corners reads as a broken edge: the whole pad goes below
    // instead. The text cannot move either way, because the pads move with the
    // box and the content area stays exactly the old rect.
    int padTop = kPlatePad, padBottom = kPlatePad;
    int y = st.y - kPlatePad;
    const lv_obj_t *par = lv_obj_get_parent(l);
    const int maxH = par ? lv_obj_get_content_height(par) : 0;
    if (y < 0) {
      padTop = 0;
      padBottom = 2 * kPlatePad;
      y = st.y;
    } else if (maxH > 0 && y + st.h + 2 * kPlatePad > maxH) {
      padTop = 2 * kPlatePad;
      padBottom = 0;
      y = st.y - 2 * kPlatePad;
    }
    lv_obj_set_style_pad_top(l, padTop, 0);
    lv_obj_set_style_pad_bottom(l, padBottom, 0);
    lv_obj_set_style_pad_left(l, kPlatePad, 0);
    lv_obj_set_style_pad_right(l, kPlatePad, 0);
    lv_obj_set_pos(l, st.x - kPlatePad, y);
    lv_obj_set_size(l, st.w + 2 * kPlatePad, st.h + 2 * kPlatePad);
    setBgColor(l, COL_RED, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, 3, 0);
  } else {
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_obj_set_pos(l, st.x, st.y);
    lv_obj_set_size(l, st.w, st.h);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
  }
}

/// Figure out the box a label had before it was plated. A plated label has
/// grown by kPlatePad on every side, so the stored rect is the truth; an
/// unplated one is still at its base rect and can be read straight off it.
void setPlate(lv_obj_t *l, bool on) {
  if (!l) return;
  // Midnight keeps the ported look: red text, no plate. The other schemes
  // invert, which is what replaces the red.
  if (theme::scheme() == theme::Scheme::Midnight) on = false;
  PlateState *st = nullptr;
  for (PlateState &c : g_plates) {
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
  if (st->valid && st->on == on) return;
  if (!st->on) {  // not plated: the object's box is still the base rect
    st->x = lv_obj_get_x(l);
    st->y = lv_obj_get_y(l);
    st->w = lv_obj_get_width(l);
    st->h = lv_obj_get_height(l);
  }
  st->valid = true;
  st->on = on;
  plateApply(*st, on);
}

/// One numeric reading, with the palette's rules in one place: NaN is "--" in
/// dim, at or above crit inverts (non-Midnight) or takes the crit colour, at or
/// above warn takes the warn shade, and everything else the accent. The text
/// colour is written here, not by the caller, so a plate and the colour on it
/// cannot disagree.
void setReading(lv_obj_t *l, const char *text, float v, float warn, float crit) {
  const bool hit = !isnan(v) && v >= crit;
  setPlate(l, hit);
  setText(l, text);
  setTextColor(l, (hit && theme::scheme() != theme::Scheme::Midnight)
                      ? (lv_color_t)COL_CRIT_TEXT
                      : colFor(v, warn, crit));
}

/// Undo every plate and forget its state. Called before a tile is re-placed,
/// because place*() sets the same geometry the plate had grown from.
void clearPlates() {
  for (PlateState &c : g_plates) {
    if (!c.obj) continue;
    if (c.on) plateApply(c, false);
    c.valid = false;
    c.on = false;
  }
}

// ---- minimal: speed and rpm, large, and the three readings that matter.
constexpr int kMinRowY = 330;

void buildMinimal(lv_obj_t *parent) {
  w.minGrp = plain(parent, 0, 0, L::W, L::CONTENT_H);
  lv_obj_add_flag(w.minGrp, LV_OBJ_FLAG_HIDDEN);

  // The speed, in the 96 px face the rev number already pays for. Centred on a
  // fixed box: a self-sizing label would dirty the layout on every change.
  w.mn.speed = fixedLabel(w.minGrp, 0, 40, L::W, 100, FONT_RPM, COL_FG, "--");
  lv_obj_set_style_text_align(w.mn.speed, LV_TEXT_ALIGN_CENTER, 0);
  w.mn.speedUnit = fixedLabel(w.minGrp, 0, 142, L::W, 20, FONT_BODY, COL_DIM, "km/h");
  lv_obj_set_style_text_align(w.mn.speedUnit, LV_TEXT_ALIGN_CENTER, 0);

  // rpm: the number, then one plain bar. One lv_bar is one invalid area; the
  // 80-cell bar in the Race view is cheap because a *step* only repaints the
  // cell it crossed, which is a different problem from redrawing a whole bar.
  w.mn.rpm = fixedLabel(w.minGrp, 0, 184, L::W, 60, FONT_NUM, COL_FG, "--");
  lv_obj_set_style_text_align(w.mn.rpm, LV_TEXT_ALIGN_CENTER, 0);
  w.mn.rpmBar = bar(w.minGrp, 0, 252, L::W, 18, COL_GREEN, 0, 1000);

  // Three readings and the mode, in four 200 px columns.
  static const char *const caps[4] = {"COOLANT", "OIL", "FUEL", "HALDEX"};
  for (int i = 0; i < 4; i++) {
    const int x = i * (L::W / 4);
    w.mn.cap[i] = label(w.minGrp, FONT_CAP, COL_DIM, caps[i]);
    lv_obj_set_pos(w.mn.cap[i], x + 16, kMinRowY);
    if (i == 3) continue;  // the mode is text, not a reading with a unit
    w.mn.val[i] = fixedLabel(w.minGrp, x + 16, kMinRowY + 18, 140, 30, FONT_TEMP, COL_FG, "--");
    w.mn.unit[i] = fixedLabel(w.minGrp, x + 160, kMinRowY + 24, 32, 18, FONT_SMALL, COL_DIM, "");
  }
  w.mn.val[3] = fixedLabel(w.minGrp, 3 * (L::W / 4) + 16, kMinRowY + 16, 180, 36, FONT_MODE,
                           COL_FG, "--");
}

// ---- temps: the four engine readings, full width, each with a bar and the
// thresholds that colour it. This is the view to read while changing them.
constexpr int kTempsRowY0 = 10;
constexpr int kTempsRowH = 108;
constexpr int kTempsBarW = L::W - 48;

void buildTemps(lv_obj_t *parent) {
  w.tempsGrp = plain(parent, 0, 0, L::W, L::CONTENT_H);
  lv_obj_add_flag(w.tempsGrp, LV_OBJ_FLAG_HIDDEN);

  static const char *const caps[4] = {"COOLANT", "OIL TEMPERATURE", "OIL PRESSURE", "FUEL"};
  for (int i = 0; i < 4; i++) {
    const int y = kTempsRowY0 + i * kTempsRowH;
    w.tv.cap[i] = label(w.tempsGrp, FONT_CAP, COL_DIM, caps[i]);
    lv_obj_set_pos(w.tv.cap[i], 24, y + 2);
    w.tv.note[i] = fixedLabel(w.tempsGrp, 200, y + 2, 400, 16, FONT_CAP, COL_DIM, "");
    w.tv.val[i] = fixedLabel(w.tempsGrp, 24, y + 18, 260, 60, FONT_NUM, COL_FG, "--");
    w.tv.unit[i] = fixedLabel(w.tempsGrp, 292, y + 42, 48, 22, FONT_BODY, COL_DIM, "");
    w.tv.bar[i] = bar(w.tempsGrp, 24, y + 82, kTempsBarW, 14, COL_GREEN, 0, 100);
  }
  // The oil-pressure row is a capability, exactly as it is in the tile: a car
  // without the ME7 patch has no sensor, so its row goes and the three that
  // remain spread over the space (see placeTempsView).
}

/// Spread the temps rows: four on a patched car, three otherwise. Idempotent,
/// like placeBand.
void placeTempsView() {
  const bool press = cap::get().oilPressure;
  const int shown = press ? 4 : 3;
  const int pitch = L::CONTENT_H / shown;
  for (int i = 0; i < 4; i++) {
    lv_obj_t *objs[4] = {w.tv.cap[i], w.tv.val[i], w.tv.unit[i], w.tv.bar[i]};
    if (!objs[0]) continue;
    const bool hide = (i == 2) && !press;
    for (lv_obj_t *o : objs) {
      if (hide) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
      }
    }
    if (hide) {
      lv_obj_add_flag(w.tv.note[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_remove_flag(w.tv.note[i], LV_OBJ_FLAG_HIDDEN);
    // Rows 0,1,3 keep their order; without the pressure row, fuel takes slot 2.
    const int slot = (i == 3 && !press) ? 2 : i;
    const int y = kTempsRowY0 + slot * pitch;
    lv_obj_set_pos(w.tv.cap[i], 24, y + 2);
    lv_obj_set_pos(w.tv.note[i], 200, y + 2);
    lv_obj_set_pos(w.tv.val[i], 24, y + 18);
    lv_obj_set_pos(w.tv.unit[i], 292, y + 42);
    lv_obj_set_pos(w.tv.bar[i], 24, y + 82);
  }
}

// Put one view's content on the glass and take the others off. Hidden, not
// deleted: every widget in the other views is still updated by the readout
// pass, which is cheaper than making each update ask whether its view is up.
//
// force is for start-up, where g_view already holds the stored value and the
// object flags still have to be set from it.
void applyView(View v, bool force) {
  if ((uint8_t)v >= (uint8_t)View::Count) v = View::Race;
  const bool changed = (v != g_view) || force;
  g_view = v;
  if (!w.root) return;

  const bool race = (v == View::Race);
  // Each group names the view it belongs to. (An earlier version compared a
  // "belongs to Race" boolean, which made Minimal and Temps both visible at
  // once: they share the answer "not Race", and Temps is built later, so it
  // drew over Minimal.)
  struct Group {
    lv_obj_t *obj;
    View view;
  };
  const Group groups[4] = {{w.top, View::Race},
                           {w.bandGrp, View::Race},
                           {w.minGrp, View::Minimal},
                           {w.tempsGrp, View::Temps}};
  for (const Group &g : groups) {
    if (!g.obj) continue;
    if (g.view == v) {
      lv_obj_remove_flag(g.obj, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(g.obj, LV_OBJ_FLAG_HIDDEN);
    }
  }
  // The debug mask has the last word over w.top (and its three groups), so a
  // view switch back to Race re-applies it instead of undoing a bench mask.
  if (race) applyDebugMask();
  if (v == View::Temps) placeTempsView();
  // The rev cells are only painted while this view is up, and the first pass
  // after a switch must repaint all of them (they kept the colours of a view
  // that is no longer being updated).
  if (race && changed) syncGauge();
  if (changed) {
    if (w.root) lv_obj_invalidate(w.root);
    // Names the groups as well as the view: on the bench the question after a
    // switch is always "what is actually on the glass", and a hidden flag is
    // the honest answer (the dump only shows luminance).
    auto vis = [](lv_obj_t *o) { return o && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) ? "on" : "off"; };
    Serial.printf("[ui] view %s: top %s band %s min %s temps %s\n", viewName(v), vis(w.top),
                  vis(w.bandGrp), vis(w.minGrp), vis(w.tempsGrp));
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
  theme::styleBg(s, COL_PANEL, 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_set_style_border_side(s, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(s, 1, 0);
  theme::styleBorder(s, COL_LINE, 0);

  w.dot = plain(s, 9, (L::STATUS_H - 11) / 2, 11, 11);
  lv_obj_set_style_radius(w.dot, LV_RADIUS_CIRCLE, 0);
  theme::styleBg(w.dot, COL_DIM, 0);
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
  theme::styleBg(c.valueLabel, COL_FIELD_FILL, 0);
  lv_obj_set_style_bg_opa(c.valueLabel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(c.valueLabel, 1, 0);
  theme::styleBorder(c.valueLabel, COL_LAMP_EDGE, 0);
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
  lv_obj_t *l = label(parent, FONT_SMALL, COL_SOFT_LABEL, name);
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
  lv_obj_t *l = label(parent, FONT_SMALL, COL_SOFT_LABEL, name);
  lv_obj_set_pos(l, 0, y + 8);

  lv_obj_t *ta = lv_textarea_create(parent);
  lv_obj_set_pos(ta, 110, y);
  lv_obj_set_size(ta, width, 34);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_max_length(ta, password ? 64 : 47);
  lv_textarea_set_password_mode(ta, password);
  lv_textarea_set_text(ta, value);
  theme::styleBg(ta, COL_FIELD_FILL, 0);
  lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
  theme::styleText(ta, COL_FG);
  lv_obj_set_style_text_font(ta, FONT_BODY, 0);
  lv_obj_set_style_border_width(ta, 1, 0);
  theme::styleBorder(ta, COL_LAMP_EDGE, 0);
  lv_obj_set_style_radius(ta, 3, 0);
  // One line of FONT_BODY (20 px) in the 34 px box: equal top/bottom padding
  // centres it vertically, as the HOST field has always looked.
  lv_obj_set_style_pad_top(ta, 6, 0);
  lv_obj_set_style_pad_bottom(ta, 6, 0);
  lv_obj_add_event_cb(ta, taEvent, LV_EVENT_ALL, nullptr);
  return ta;
}

lv_obj_t *g_taSsid = nullptr, *g_taPass = nullptr, *g_taHost = nullptr;
lv_obj_t *g_btnTempUnits = nullptr, *g_btnPressure = nullptr;
lv_obj_t *g_btnDistUnits = nullptr, *g_btnDayNight = nullptr, *g_btnBoostTile = nullptr;
lv_obj_t *g_btnNetRole = nullptr, *g_btnNetCh = nullptr;

const char *pressureUnitName(uint8_t units) {
  return units == cfg::PRESSURE_PSI ? "psi" : "bar";
}

const char *netRoleName(uint8_t role) {
  return role == cfg::NET_ANCHOR ? "ANCHOR" : "FOLLOWER";
}

void setNetChLabel() {
  if (!g_btnNetCh) return;
  char b[8];
  snprintf(b, sizeof b, "%u", (unsigned)g_edit.fleetChannel);
  setText(lv_obj_get_child(g_btnNetCh, 0), b);
}

void onNetRole(lv_event_t *) {
  g_edit.netRole = (g_edit.netRole == cfg::NET_ANCHOR) ? cfg::NET_FOLLOWER : cfg::NET_ANCHOR;
  setText(lv_obj_get_child(g_btnNetRole, 0), netRoleName(g_edit.netRole));
}

void onNetChannel(lv_event_t *) {
  g_edit.fleetChannel = (uint8_t)((g_edit.fleetChannel % 13) + 1);
  setNetChLabel();
}

void onTempUnits(lv_event_t *) {
  g_edit.tempF = !g_edit.tempF;
  setText(lv_obj_get_child(g_btnTempUnits, 0), g_edit.tempF ? "\xC2\xB0" "F" : "\xC2\xB0" "C");
}

void onPressureUnits(lv_event_t *) {
  g_edit.pressureUnits =
      (g_edit.pressureUnits == cfg::PRESSURE_PSI) ? cfg::PRESSURE_BAR : cfg::PRESSURE_PSI;
  setText(lv_obj_get_child(g_btnPressure, 0), pressureUnitName(g_edit.pressureUnits));
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

// The BOOST panel's on/off switch. Off hands the fourth tile's width to SPEED /
// TEMPERATURES / HALDEX (the three-panel band), on restores the four-panel one.
// The dashboard is re-placed on SAVE, with the rest of the settings.
void onBoostTile(lv_event_t *) {
  g_edit.boostTile = g_edit.boostTile ? 0 : 1;
  setText(lv_obj_get_child(g_btnBoostTile, 0), g_edit.boostTile ? "ON" : "OFF");
}

// ---- theme and view: the two display preferences that apply on the tap.
// They are stored immediately rather than on SAVE: SAVE is at the bottom of the
// page, and CLOSE would otherwise leave a theme on the glass that NVS does not
// have. cfg::apply() writes only when the value actually changed, so cycling
// back to the current scheme costs no flash page.
char g_btnText[2][24];
const char *themeButtonText() {
  snprintf(g_btnText[0], sizeof g_btnText[0], "THEME %s", theme::schemeName(theme::scheme()));
  return g_btnText[0];
}
const char *viewButtonText() {
  snprintf(g_btnText[1], sizeof g_btnText[1], "VIEW %s", viewName(g_view));
  return g_btnText[1];
}
void relabelDisplayButtons() {
  if (w.btnTheme) setText(lv_obj_get_child(w.btnTheme, 0), themeButtonText());
  if (w.btnView) setText(lv_obj_get_child(w.btnView, 0), viewButtonText());
}
void onThemeBtn(lv_event_t *) {
  nextTheme();
  relabelDisplayButtons();
}
void onViewBtn(lv_event_t *) {
  nextView();
  relabelDisplayButtons();
}

void onCfgSave(lv_event_t *) {
  strncpy(g_edit.ssid, lv_textarea_get_text(g_taSsid), sizeof(g_edit.ssid) - 1);
  strncpy(g_edit.pass, lv_textarea_get_text(g_taPass), sizeof(g_edit.pass) - 1);
  strncpy(g_edit.host, lv_textarea_get_text(g_taHost), sizeof(g_edit.host) - 1);
  // A new SSID means a different controller, so the remembered one is dropped.
  // A new password on the same SSID is still the same controller.
  const bool ssidChanged = strcmp(g_edit.ssid, cfg::get().ssid) != 0;
  const uint8_t oldRole = cfg::get().netRole;
  const uint8_t oldChannel = cfg::get().fleetChannel;
  // The theme and the view are applied and stored on the tap, not on SAVE, so
  // g_edit (a snapshot taken when this page opened) is not the truth for them.
  // Without this, SAVE would quietly put back the scheme and view that were up
  // when the page was opened - including one changed from the console with the
  // page already open.
  {
    const cfg::Data live = cfg::get();
    g_edit.theme = live.theme;
    g_edit.view = live.view;
  }
  nvsGuardBegin();
  cfg::apply(g_edit);
  nvsGuardEnd();
  if (ssidChanged) ohxnow::forgetController();
  // The WiFi mode (AP+STA vs AP-only) cannot be switched safely with ESP-NOW
  // live, so a role or fleet-channel change is applied by rebooting. The
  // values are already in NVS; this is only the restart.
  if (g_edit.netRole != oldRole || g_edit.fleetChannel != oldChannel) {
    Serial.printf("[net] role/channel changed (%s ch%u) - rebooting to apply\n",
                  netRoleName(g_edit.netRole), (unsigned)g_edit.fleetChannel);
    Serial.flush();
    delay(150);
    esp_restart();
  }
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
  // Network role and fleet channel are deployment choices, like the connection
  // itself: DEFAULTS restores the dashboard thresholds, not the car's network.
  fresh.netRole = keep.netRole;
  fresh.fleetChannel = keep.fleetChannel;
  nvsGuardBegin();
  cfg::apply(fresh);
  nvsGuardEnd();
  ohxnow::forgetController();  // also how a replaced controller gets picked up
  g_edit = cfg::get();
  lv_textarea_set_text(g_taSsid, g_edit.ssid);
  lv_textarea_set_text(g_taPass, g_edit.pass);
  lv_textarea_set_text(g_taHost, g_edit.host);
  refreshSteppers();
  // The toggles above are buttons, not steppers, so nothing re-reads them: label
  // them here or DEFAULTS leaves them showing the settings it just replaced.
  setText(lv_obj_get_child(g_btnTempUnits, 0), g_edit.tempF ? "\xC2\xB0" "F" : "\xC2\xB0" "C");
  setText(lv_obj_get_child(g_btnPressure, 0), pressureUnitName(g_edit.pressureUnits));
  setText(lv_obj_get_child(g_btnDistUnits, 0), g_edit.unitsMetric ? "km" : "mi");
  setText(lv_obj_get_child(g_btnDayNight, 0), kDayNightNames[g_edit.dayNight]);
  setText(lv_obj_get_child(g_btnBoostTile, 0), g_edit.boostTile ? "ON" : "OFF");
  setText(lv_obj_get_child(g_btnNetRole, 0), netRoleName(g_edit.netRole));
  setNetChLabel();
  // DEFAULTS restores the display preferences too (they are on the same page),
  // so the glass is put back to what cfg::defaults() chose.
  applyTheme((theme::Scheme)g_edit.theme);
  applyView((View)g_edit.view, false);
  relabelDisplayButtons();
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
  setBgColor(w.btnLog, rec ? COL_RED : COL_BUTTON_FILL, 0);
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
  theme::styleBg(w.settings, COL_PAGE_BG, 0);
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

  // The header's right-hand strip: the only place on this page with room left
  // (both columns and the button row are full), and the two settings that are
  // about the display itself rather than the car.
  w.btnTheme = button(card, 386, 2, 200, 24, themeButtonText(), onThemeBtn);
  w.btnView = button(card, 594, 2, 182, 24, viewButtonText(), onViewBtn);
  y += 32;

  rule(card, 0, y, L::W - 24);
  y += 10;

  lv_obj_t *net = label(card, FONT_BODY, COL_DIM, "NETWORK");
  lv_obj_set_pos(net, 0, y);
  y += 20;

  // Role and fleet channel. Changing either saves and reboots (the WiFi mode
  // is AP+STA vs AP only, which cannot be swapped with ESP-NOW live); the
  // console w/W do the same. In FOLLOWER the AP follows the controller once
  // joined, so the channel is the fallback/standby value.
  lv_obj_t *roleLbl = label(card, FONT_SMALL, COL_DIM, "Role");
  lv_obj_set_pos(roleLbl, 0, y + 4);
  g_btnNetRole = button(card, 110, y, 104, 24, netRoleName(g_edit.netRole), onNetRole);
  lv_obj_t *chLbl = label(card, FONT_SMALL, COL_DIM, "Fleet ch");
  lv_obj_set_pos(chLbl, 222, y + 4);
  g_btnNetCh = button(card, 292, y, 38, 24, "1", onNetChannel);
  setNetChLabel();
  y += 30;

  g_taSsid = addField(card, y, "SSID", g_edit.ssid, false, 220);
  y += 32;
  g_taPass = addField(card, y, "PASSWORD", g_edit.pass, true, 220);
  y += 32;
  g_taHost = addField(card, y, "HOST", g_edit.host, false, 220);
  y += 44;  // air under the HOST box: the green status line starts below it

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
  w.ctrlWarn = fixedLabel(card, 0, y, 368, 30, FONT_SMALL, COL_AMBER, "");
  lv_label_set_long_mode(w.ctrlWarn, LV_LABEL_LONG_WRAP);
  y += 30;
  w.otaLine = fixedLabel(card, 0, y, 368, 30, FONT_SMALL, COL_DIM, "OTA: --");
  lv_label_set_long_mode(w.otaLine, LV_LABEL_LONG_WRAP);
  y += 32;

  lv_obj_t *disp = label(card, FONT_BODY, COL_DIM, "DISPLAY");
  lv_obj_set_pos(disp, 0, y);
  y += 20;

  {
    const StepSpec bright[] = {{&g_edit.brightness, 5, 10, 100, 0}};
    addStepRow(card, 0, y, "Brightness (%)", bright, 1);
    y += 30;
  }

  lv_obj_t *ul = label(card, FONT_SMALL, COL_SOFT_LABEL, "Units");
  lv_obj_set_pos(ul, 0, y + 4);
  g_btnTempUnits =
      button(card, 110, y, 44, 24, g_edit.tempF ? "\xC2\xB0" "F" : "\xC2\xB0" "C", onTempUnits);
  g_btnPressure =
      button(card, 162, y, 64, 24, pressureUnitName(g_edit.pressureUnits), onPressureUnits);
  g_btnDistUnits = button(card, 230, y, 56, 24, g_edit.unitsMetric ? "km" : "mi", onDistUnits);
  y += 30;

  lv_obj_t *dn = label(card, FONT_SMALL, COL_SOFT_LABEL, "Day / Night");
  lv_obj_set_pos(dn, 0, y + 4);
  g_btnDayNight = button(card, 110, y, 90, 24, kDayNightNames[g_edit.dayNight], onDayNight);
  // The BOOST panel is a preference now, not a capability decision. It shares
  // the Day / Night row: the left column ends 14 px above the button rule, so
  // there is no room for a row of its own, and this row has 180 px of space
  // after its button.
  lv_obj_t *btl = label(card, FONT_SMALL, COL_SOFT_LABEL, "Boost tile");
  lv_obj_set_pos(btl, 208, y + 4);
  g_btnBoostTile = button(card, 288, y, 42, 24, g_edit.boostTile ? "ON" : "OFF", onBoostTile);
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
    // The scale is free again: every rpm threshold moves in 100 rpm, and the
    // tick numbers and the bar's zone tints are recomputed from it on save
    // (syncGauge). A zone edge quantises to one bar cell - 100 rpm at the
    // default 8000, since BAR_SEG is 80 - so at ordinary scales a 100 rpm step
    // is effectively exact.
    const StepSpec a[] = {{&g_edit.rpmMax, 100, 3000, 12000, 0},
                          {&g_edit.rpmAmber, 100, 0, 12000, 0}};
    addStepRow(card, rx, ty, "RPM max / amber", a, 2);
    ty += 40;
  }
  {
    // Shift only starts the bar flash: it sits on no cell and is independent of
    // amber and redline, so it is free anywhere on the scale.
    const StepSpec a[] = {{&g_edit.rpmRedline, 100, 1000, 12000, 0},
                          {&g_edit.rpmShift, 100, 0, 12000, 0}};
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
  // self-consistent. The shift point is deliberately not in it - it marks no
  // cell, so it is free anywhere on the scale.
  lv_obj_t *note = label(card, FONT_CAP, COL_DIM,
                         "Saved values are clamped into order: amber <= redline <= max, "
                         "warn <= crit.\nRPM shift is independent. Temperatures are in C "
                         "per the units toggle.");
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
  w.btnLog = button(card, 284, 414, 96, 30, "START LOG", onLogToggle);
  w.logLine = fixedLabel(card, 396, 420, 216, 18, FONT_SMALL, COL_DIM, "");
  button(card, 618, 414, 74, 30, "SAVE", onCfgSave);
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

/// The temperature number only; the small unit label beside it is set from
/// tempUnitText(), so the two can never disagree about C/F.
void fmtTempValue(char *buf, size_t n, float celsius) {
  if (isnan(celsius)) {
    snprintf(buf, n, "--");
    return;
  }
  const bool f = cfg::get().tempF;
  const float v = f ? celsius * 9.0f / 5.0f + 32.0f : celsius;
  snprintf(buf, n, "%d", (int)lroundf(v));
}

const char *tempUnitText() { return cfg::get().tempF ? kDegF : kDegC; }

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

/// The fuel level number only; the small "L" label beside it is fixed.
///
/// Deliberately NOT tied to the metric / imperial toggle. The car reports litres
/// (TankContents is one litre per count) and a tank size is a metric fact here,
/// and the toggle is about *distance* - so switching to miles moves the speed and
/// the odometer and leaves the fuel alone. Converting it would also put the fuel
/// bar's full-scale (a litre figure) at odds with the number beside it.
void fmtFuelValue(char *buf, size_t n, float litres) {
  if (isnan(litres)) {
    snprintf(buf, n, "--");
    return;
  }
  snprintf(buf, n, "%.1f", (double)litres);
}

void fmtBoost(char *buf, size_t n, float mbar) {
  if (isnan(mbar)) {
    snprintf(buf, n, "--");
    return;
  }
  switch (cfg::get().pressureUnits) {
    case cfg::PRESSURE_PSI:
      snprintf(buf, n, "%.1f", (double)(mbar / 68.9476f));
      break;
    default:
      snprintf(buf, n, "%.2f", (double)(mbar / 1000.0f));
      break;
  }
}

const char *boostUnitText() {
  return cfg::get().pressureUnits == cfg::PRESSURE_PSI ? "psi" : "bar";
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
// rpmShift the whole bar alternates between the shift purple and a flat dark,
// and at the limiter between red and that same dark. Flashing to a flat off -
// not back to the zone colour - is deliberate: pulsing between two lit colours
// read as a colour change, and pulsing to the per-zone tints still carried a
// green/amber/red gradient through the blink. A 96 px number strobing was hard
// to read; a bar pulsing is not.
//
// The flash is ONE full-width plate, not a recolour of every lit cell. LVGL's
// cost of the flash is what forced it: forty cell writes per phase were forty
// invalid areas, and forty areas cost about what a whole screen does, because
// the per-area draw pass - not the pixels - is what is expensive here. One
// 800x104 fill is one area, and it is also the cue the eye expects: the full
// width, not just the part below the shift point. (Same measurement as
// repaintBarAll(), and the reason LV_INV_BUF_SIZE in lv_conf.h is 128 rather
// than LVGL's 32: eighty cells plus a handful of readings must not silently
// turn into a full-screen repaint.)
void updateRevBar(const cfg::Data &c, float rpm) {
  if (!dbg(1)) return;

  const int lit = revLitCount(c, rpm);
  const bool have = !isnan(rpm);
  // The forced state is the bench stand-in for rpm: the cue is the one thing
  // that cannot happen here, because rpm comes from the car.
  const bool shifting = g_forceShift != 0 || (have && rpm >= c.rpmShift);
  const bool limiter = g_forceShift == 0 && have && rpm >= c.rpmRedline;
  const lv_color_t flashCol = limiter ? COL_RED : COL_SHIFT;

  // 2 = flash colour, 3 = the dark half, 0 = hidden. Only a phase change
  // writes a style, so a steady bar costs what it did before.
  // Forced states: 1 = flash half, 2 = dark half, 3 = forced on but still
  // toggling, which is the live cue and is how its cost is measured on a bench.
  const bool flashOn = (g_forceShift == 1) ? true
                       : (g_forceShift == 2) ? false
                                             : g_flashOn;
  const int8_t wantFlash = shifting ? (flashOn ? 2 : 3) : 0;
  if (wantFlash != g_revFlash) {
    if (wantFlash == 0) {
      lv_obj_add_flag(w.revFlash, LV_OBJ_FLAG_HIDDEN);
      // The cells under the plate were not kept up to date while it covered
      // them, and every one of them has to be repainted - as one area, not
      // eighty (see repaintBarAll() for the measurement).
      repaintBarAll(c, rpm);
    } else {
      setBgColor(w.revFlash, wantFlash == 2 ? flashCol : (lv_color_t)COL_TRACK, 0);
      if (g_revFlash == 0) lv_obj_remove_flag(w.revFlash, LV_OBJ_FLAG_HIDDEN);
    }
    g_revFlash = wantFlash;
  }

  // While the plate is up it covers the bar, so the cells are left alone: this
  // is what keeps the cue at one invalid area instead of forty.
  if (shifting) return;

  for (int i = 0; i < L::BAR_SEG; i++) {
    // 0 = empty (faint zone tint), 1 = lit in its zone colour.
    const int8_t want = (i >= lit) ? 0 : 1;
    if (g_revLit[i] != want) {
      g_revLit[i] = want;
      setBgColor(w.revCell[i], want ? g_revCol[i].lit : g_revCol[i].unlit, 0);
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
  if (!dbg(8)) return;
  // The battery reading was the only thing here drawn from the raw copy; every
  // other value is the interpolated g_shown. Kept in the signature because the
  // caller has both and a future slow-moving row will want the same source.
  (void)live;
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
  setReading(w.boostVal, buf, v.boost, c.boostWarn, c.boostCrit);
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
  fmtTempValue(buf, sizeof(buf), v.coolant);
  setReading(w.coolVal, buf, v.coolant, c.coolWarn, c.coolCrit);
  setText(w.coolUnit, tempUnitText());
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

  fmtTempValue(buf, sizeof(buf), v.oil);
  setReading(w.oilVal, buf, v.oil, c.oilWarn, c.oilCrit);
  setText(w.oilUnit, tempUnitText());
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

  // ---- oil pressure: the other half of the ForbesAutomotive ME7 patch, from
  // Motor_7 bytes 5-6. It is the temperatures card's third row, directly under
  // the oil-temperature bar, and it is hidden (with FUEL moved up) unless the
  // car was detected with the patch. Negative is drawn as it is: it means the
  // input is sitting below the sensor's 0.5 V zero (unwired or unpowered), and
  // a fake 0.0 bar on an oil gauge would be worse than that.
  setText(w.oilPressUnit, c.pressureUnits == cfg::PRESSURE_PSI ? "psi" : "bar");
  if (isnan(v.oilPressure)) {
    setSuffix(w.oilPressVal, "--", COL_DIM);
  } else if (c.pressureUnits == cfg::PRESSURE_PSI) {
    snprintf(buf, sizeof(buf), "%.1f", (double)(v.oilPressure * 14.5038f));
    setSuffix(w.oilPressVal, buf, COL_FG);
  } else {
    snprintf(buf, sizeof(buf), "%.1f", (double)v.oilPressure);
    setSuffix(w.oilPressVal, buf, COL_FG);
  }
  {
    int pct = 0;
    if (!isnan(v.oilPressure)) {
      pct = (int)((v.oilPressure / kOilPressMaxBar) * 100.0f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
    }
    setBar(w.oilPressBar, pct);
  }

  // ---- fuel, the bottom row of this panel in both tile bands (#4/#11). The bar
  // is a percentage of the configured tank size; the number is the car's own
  // litres. The value and the bar go amber on the car's low-fuel warning, which
  // is read straight off the frame and never smoothed - the warning is the one
  // thing on this row that must not lag.
  fmtFuelValue(buf, sizeof(buf), v.fuel);
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

  // Battery voltage is no longer drawn on the dashboard (0.9.1): BATT's row
  // became the oil-pressure row, and FUEL took the bottom slot. The reading is
  // still on the raw page and in the drive log.

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
  // The cruise lamp: dark green with the column switch on (armed), bright green
  // once a speed is stored (engaged). Off when the frames are missing, so a
  // quiet link cannot read as "armed".
  lampStateCruise(w.lampCc, d.cruise);
}

// ---- the Minimal and Temps views -------------------------------------
// Same values, same easing, same NaN rule: a view is a second way to read the
// dashboard, not a second source. Called from the 100 ms block in tick(), only
// for the view that is on the glass, so a hidden view costs nothing.
void updateMinimal(const cfg::Data &c, const telemetry::Dash &d) {
  if (!w.minGrp) return;
  char buf[32];
  const Values v = g_shown;

  fmtSpeed(buf, sizeof buf, v.speed);
  setSuffix(w.mn.speed, buf, isnan(v.speed) ? COL_DIM : COL_FG);
  setText(w.mn.speedUnit, speedUnitText());

  if (isnan(v.rpm)) {
    setSuffix(w.mn.rpm, "--", COL_DIM);
  } else {
    snprintf(buf, sizeof buf, "%d", (int)lroundf(v.rpm));
    setSuffix(w.mn.rpm, buf, COL_FG);
  }
  {
    int pct = 0;
    if (!isnan(v.rpm)) {
      pct = (int)lroundf(1000.0f * v.rpm / c.rpmMax);
      if (pct < 0) pct = 0;
      if (pct > 1000) pct = 1000;
    }
    setBar(w.mn.rpmBar, pct);
    setBgColor(w.mn.rpmBar, colFor(v.rpm, c.rpmAmber, c.rpmRedline), LV_PART_INDICATOR);
  }

  // coolant (column 0)
  fmtTempValue(buf, sizeof buf, v.coolant);
  setReading(w.mn.val[0], buf, v.coolant, c.coolWarn, c.coolCrit);
  setText(w.mn.unit[0], tempUnitText());
  // oil (column 1)
  fmtTempValue(buf, sizeof buf, v.oil);
  setReading(w.mn.val[1], buf, v.oil, c.oilWarn, c.oilCrit);
  setText(w.mn.unit[1], tempUnitText());
  // fuel (column 2): litres, and the car's own low-fuel warning, exactly as in
  // the tile (never smoothed, never inferred).
  fmtFuelValue(buf, sizeof buf, v.fuel);
  setSuffix(w.mn.val[2], buf,
            isnan(v.fuel) ? (lv_color_t)COL_DIM
                          : (d.fuelLow ? (lv_color_t)COL_AMBER : (lv_color_t)COL_FG));
  setText(w.mn.unit[2], "L");
  // mode (column 3)
  setText(w.mn.val[3], (d.mode >= 0 && d.mode < 6) ? kModeNames[d.mode] : "--");
}

void updateTemps(const cfg::Data &c, const telemetry::Dash &d) {
  if (!w.tempsGrp) return;
  char buf[32], note[48];
  const Values v = g_shown;

  struct Row {
    float v, warn, crit;
    const char *unit;
    int pct;
  };
  Row r[4] = {};
  // coolant and oil use the same 40..130 C bar scale as the tile.
  for (int i = 0; i < 2; i++) {
    const float val = i == 0 ? v.coolant : v.oil;
    r[i].v = val;
    r[i].warn = i == 0 ? c.coolWarn : c.oilWarn;
    r[i].crit = i == 0 ? c.coolCrit : c.oilCrit;
    r[i].unit = tempUnitText();
    int pct = 0;
    if (!isnan(val)) {
      pct = (int)(((val - 40.0f) / 90.0f) * 100.0f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
    }
    r[i].pct = pct;
  }
  r[2].v = v.oilPressure;
  r[2].unit = c.pressureUnits == cfg::PRESSURE_PSI ? "psi" : "bar";
  if (!isnan(v.oilPressure)) {
    int pct = (int)((v.oilPressure / kOilPressMaxBar) * 100.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    r[2].pct = pct;
  }
  r[3].v = v.fuel;
  r[3].unit = "L";
  if (!isnan(v.fuel)) {
    int pct = (int)lroundf(100.0f * v.fuel / c.tankLitres);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    r[3].pct = pct;
  }

  for (int i = 0; i < 4; i++) {
    if (!w.tv.val[i]) continue;
    // The number: same formats as the tiles, so the two views can never
    // disagree about a value.
    lv_color_t col = COL_FG;
    if (i < 2) {
      fmtTempValue(buf, sizeof buf, r[i].v);
      setReading(w.tv.val[i], buf, r[i].v, r[i].warn, r[i].crit);
      col = colFor(r[i].v, r[i].warn, r[i].crit);
    } else if (i == 2) {
      if (isnan(r[i].v)) {
        snprintf(buf, sizeof buf, "--");
        col = COL_DIM;
      } else if (c.pressureUnits == cfg::PRESSURE_PSI) {
        snprintf(buf, sizeof buf, "%.1f", (double)(r[i].v * 14.5038f));
        col = COL_FG;
      } else {
        snprintf(buf, sizeof buf, "%.1f", (double)r[i].v);
        col = COL_FG;
      }
    } else {
      fmtFuelValue(buf, sizeof buf, r[i].v);
      col = isnan(r[i].v) ? (lv_color_t)COL_DIM : (d.fuelLow ? (lv_color_t)COL_AMBER : (lv_color_t)COL_FG);
    }
    // setReading() owns the two temperature rows' colour and plate; the
    // pressure and fuel rows have no warn/crit, so they use the plain setter.
    if (i >= 2) setSuffix(w.tv.val[i], buf, col);
    setText(w.tv.unit[i], r[i].unit);
    setBar(w.tv.bar[i], r[i].pct);
    setBgColor(w.tv.bar[i], (i == 3 && d.fuelLow) ? (lv_color_t)COL_AMBER : col,
               LV_PART_INDICATOR);

    // The thresholds this row is coloured by - the settings are a page away, so
    // the view that depends on them carries them.
    if (i < 2) {
      char a[12], b[12];
      fmtTempValue(a, sizeof a, r[i].warn);
      fmtTempValue(b, sizeof b, r[i].crit);
      snprintf(note, sizeof note, "warn %s  crit %s %s", a, b, r[i].unit);
    } else if (i == 2) {
      snprintf(note, sizeof note, "scale 0 - 10 %s", r[i].unit);
    } else {
      snprintf(note, sizeof note, "tank %d L", (int)lroundf(c.tankLitres));
    }
    setText(w.tv.note[i], note);
  }
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
// True while an OTA upload owns the panel: the glass is blank and ui::tick()
// does nothing. See tick() for why.
bool g_uploadBlank = false;

// ---- NVS writes and the glass -------------------------------------------
// Measured on this board, and the reason 0.9.2 blanks the glass for an OTA
// upload: a flash write disables the cache, the RGB panel refills its bounce
// buffer from PSRAM through that cache, and a missed refill underruns the FIFO.
// The panel can come back with its scan-out shifted - the picture "loses sync".
// An NVS write is short (1.5-2 ms usually, ~60 ms when NVS erases a page), so
// every write the UI makes goes through here: the backlight is blanked for the
// duration, and the panel's DMA is re-armed afterwards whether or not the write
// was long enough to matter. The blank is at most a frame or two and hides the
// re-arm; the alternative is a screen that stays broken.
uint32_t g_nvsGuardUs = 0;

void nvsGuardBegin() {
  crowpanel::setBacklight(0);
  g_nvsGuardUs = micros();
}

void nvsGuardEnd() {
  crowpanel::resync();
  g_backlightValid = false;  // applyBacklight() re-asserts the real value
  Serial.printf("[ui] NVS write %lu us (glass blanked)\n",
                (unsigned long)(micros() - g_nvsGuardUs));
}

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
  // Since 0.9.0 the dash runs its own AP in both roles, so "no WiFi" is a
  // different failure from "no controller". Keep them apart:
  //   NO WIFI  both radio interfaces are down and nothing is arriving (a real
  //            radio failure, or the AP never started);
  //   NO CTRL  FOLLOWER cannot reach the controller and nothing else (the
  //            bodycan, OHX, HTTP) is arriving - what used to read NO WIFI;
  //   WAIT / STALE  something has arrived before, but not recently.
  const feed::Source src = feed::source();
  const bool noData = src == feed::Source::None;
  const bool follower = cfg::get().netRole == cfg::NET_FOLLOWER;

  if (!s.apUp && !s.wifiUp && noData) {
    setText(w.connTxt, "NO WIFI");
    setTextColor(w.connTxt, COL_RED);
    setBgColor(w.dot, COL_RED, 0);
  } else if (follower && !s.wifiUp && noData) {
    setText(w.connTxt, "NO CTRL");
    setTextColor(w.connTxt, COL_RED);
    setBgColor(w.dot, COL_RED, 0);
  } else if (stale) {
    // "WAIT" until one of the transports has delivered something at all.
    setText(w.connTxt, noData ? "WAIT" : "STALE");
    setTextColor(w.connTxt, COL_AMBER);
    setBgColor(w.dot, COL_AMBER, 0);
  } else {
    // The number is the relay senders heard in the last 3 s. It is the same
    // set the CAN DATA page names, so LIVE(2) there and "C6, BCAN" on that
    // page describe one state, not two. No senders: plain LIVE, as before.
    const int online = canrelay::sendersOnline(nullptr, 0);
    char b[16];
    if (online > 0) snprintf(b, sizeof b, "LIVE(%d)", online); else snprintf(b, sizeof b, "LIVE");
    setText(w.connTxt, b);
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
  // A real lv_bar, not a clipped custom object: the fixed-width gradient child
  // this used to reveal was never repainted in its newly exposed part on this
  // LVGL build (the width advanced in the log while the glass showed no fill;
  // bench, 13 Nov 2026). The bar's track is transparent so the artwork's empty
  // track shows through; only the green indicator is drawn.
  lv_obj_t *splashBar = lv_bar_create(w.splash);
  lv_obj_remove_style_all(splashBar);
  lv_obj_remove_flag(splashBar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(splashBar, kRuleX, kRuleY);
  lv_obj_set_size(splashBar, kRuleW, kRuleH);
  lv_bar_set_range(splashBar, 0, 100);
  lv_bar_set_value(splashBar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_opa(splashBar, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(splashBar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(splashBar, lv_color_hex(0x22c55e), LV_PART_INDICATOR);
  lv_obj_set_style_radius(splashBar, 0, LV_PART_INDICATOR);
  lv_obj_move_foreground(splashBar);  // above the full-panel picture
  g_splashRule = splashBar;

  // ---- tagline under the wordmark, and the version in the bottom-right
  // corner, where it is out of the artwork's way. The new artwork carries the
  // name and its "d a s h" subtitle down to y 331, so the tagline sits below
  // that. The version is the one thing here worth reading off the glass at
  // boot: it is what says which build an over-the-air trial actually booted.
  lv_obj_t *tag =
      fixedLabel(w.splash, 0, 360, L::W, 20, FONT_BODY, COL_DIM, "TRACK DASH FOR OPENHALDEX");
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
    const int pct = (int)(((uint32_t)100 * (now - g_splashStartMs)) / kSplashMs);
    lv_bar_set_value(g_splashRule, pct, LV_ANIM_OFF);
    // The bar's own invalidation is a few pixels wide; in DIRECT mode a
    // partial flush does not reach the panel until a full frame is flushed,
    // so the splash would keep scanning the first picture with no fill. A
    // full-splash invalidation makes the flush a complete frame and swaps the
    // buffer. Three seconds of full-screen redraw at boot is the cost.
    lv_obj_invalidate(w.splash);
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
  // The stored scheme decides every role colour from here on. It goes in
  // before the first object exists, so the whole tree is built in the right
  // palette rather than recoloured afterwards.
  theme::setScheme(theme::schemeAt(cfg::get().theme));
  lv_obj_remove_style_all(scr);
  theme::styleBg(scr, COL_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Everything on the glass goes in one container: hiding it is what makes
  // the settings page cheap to scroll.
  w.root = opaque(scr, 0, 0, L::W, L::H, COL_BG);

  buildGauge(w.root);
  // The tile band is one container so a view can take it off the glass with a
  // single flag; the tiles are positioned inside it, starting at y 0.
  w.bandGrp = plain(w.root, 0, L::TILE_Y, L::W, L::TILE_H);
  buildTiles(w.bandGrp);
  buildMinimal(w.root);
  buildTemps(w.root);
  placeBand(g_variant);  // the panels are built with the full band's geometry;
                         // this is what makes the log line and g_placed true
  buildStatusBar(w.root);
  applyView((View)cfg::get().view, true);

  // Long-press the top band opens settings, as the tach ring used to: it is
  // the whole band above the tiles, which beats the 24 px status-bar buttons
  // as a target in a moving car. The rev bar is inside it.
  lv_obj_add_flag(w.top, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(w.top, onLongPress, LV_EVENT_LONG_PRESSED, nullptr);
  // The other two views have no top band, so the same long-press target is the
  // whole content area. Without it, Settings would only be one 24 px button on
  // a moving car - the reason the long-press exists at all.
  lv_obj_t *const pressTargets[2] = {w.minGrp, w.tempsGrp};
  for (lv_obj_t *t : pressTargets) {
    if (!t) continue;
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(t, onLongPress, LV_EVENT_LONG_PRESSED, nullptr);
  }

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
  if (g_dbgHideAll || g_view != View::Race) {
    lv_obj_add_flag(w.top, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(w.top, LV_OBJ_FLAG_HIDDEN);
  }
}

void tick() {
  // An OTA upload writes flash with the cache disabled. The RGB panel's DMA
  // prefetches out of PSRAM through that cache, so it underruns and the
  // picture tears, shifts and flickers - the corruption is in the scan-out,
  // not in our buffer, so no amount of drawing can avoid it (seen on 0.9.1:
  // the update itself was fine, the glass looked broken). Blank the backlight
  // and stop drawing for the duration. The update page in the browser is the
  // progress display; a success reboots into the new image's splash, and only
  // a failed or aborted upload brings the glass back, through the normal
  // day/night path.
  if (ota::busy()) {
    if (!g_uploadBlank) {
      g_uploadBlank = true;
      g_backlight = 0;
      g_backlightValid = true;
      crowpanel::setBacklight(0);
      Serial.println("[ui] OTA upload: glass blanked, drawing suspended");
    }
    return;
  }
  if (g_uploadBlank) {
    g_uploadBlank = false;
    g_backlightValid = false;  // applyBacklight() re-asserts the real value
    Serial.println("[ui] OTA upload ended: drawing resumed");
  }

  const uint32_t now = millis();
  const float dt = (float)(now - g_lastTickMs) / 1000.0f;
  g_lastTickMs = now;

  // The deferred display preference - last in the frame's work, and only after
  // the quiet period, so the flash write cannot land on top of the repaint a
  // tap caused.
  if (g_displaySaveAt != 0 && (int32_t)(now - g_displaySaveAt) >= 0) {
    g_displaySaveAt = 0;
    nvsGuardBegin();
    cfg::flushPending();
    nvsGuardEnd();
  }

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
  g_target.oilPressure = l.oilPressure;

  if (!stale) {
    g_shown.rpm = easeValue(g_shown.rpm, g_target.rpm, dt);
    g_shown.speed = easeValue(g_shown.speed, g_target.speed, dt);
    g_shown.throttle = easeValue(g_shown.throttle, g_target.throttle, dt);
    g_shown.boost = easeValue(g_shown.boost, g_target.boost, dt);
    g_shown.coolant = easeValue(g_shown.coolant, g_target.coolant, dt * 0.25f);
    g_shown.oil = easeValue(g_shown.oil, g_target.oil, dt * 0.25f);
    g_shown.lockTarget = easeValue(g_shown.lockTarget, g_target.lockTarget, dt);
    g_shown.lockActual = easeValue(g_shown.lockActual, g_target.lockActual, dt);
    // Oil pressure moves with rpm, so it gets half the boost's rate: quick
    // enough to follow a pull, slow enough that a lossy 7/s frame does not
    // make the last digit flicker.
    g_shown.oilPressure = easeValue(g_shown.oilPressure, g_target.oilPressure, dt * 0.5f);

    // Fuel gets the hardest smoothing on the panel: the tank sloshes, and the
    // level is read at a glance rather than followed (#4). The gate drops any
    // single sample that moves the level faster than kFuelMaxRate l/s - a
    // one-sample spike is thrown away, while a real step (a fill-up) is believed
    // on its second sample, 100 ms later. On top of that the displayed level is a
    // 60 s low-pass. The car's own low-fuel warning is not touched by any of this.
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
  // what the eye tracks now that the needle is gone. The other two views have
  // nothing that moves per frame, so they cost nothing outside the 100 ms pass.
  if (g_view == View::Race) {
    updateRevBar(c, g_shown.rpm);
    updateGaugeText(c, g_shown.rpm);
  }
  uint32_t t1 = micros();
  // The tiles carry values that only arrive at 10 Hz and are already eased,
  // so repainting them 30 times a second buys nothing.
  static uint32_t lastReadouts = 0;
  if (g_forceReadout) {
    lastReadouts = now - 100;
    g_forceReadout = false;
  }
  if ((uint32_t)(now - lastReadouts) >= 100) {
    lastReadouts = now;
    if (dbg(8)) updateReadouts(c, l, d);
    if (g_view == View::Minimal) updateMinimal(c, d);
    else if (g_view == View::Temps) updateTemps(c, d);
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
    setText(lv_obj_get_child(g_btnPressure, 0), pressureUnitName(g_edit.pressureUnits));
    setText(lv_obj_get_child(g_btnDistUnits, 0), g_edit.unitsMetric ? "km" : "mi");
    setText(lv_obj_get_child(g_btnDayNight, 0), kDayNightNames[g_edit.dayNight]);
    setText(lv_obj_get_child(g_btnBoostTile, 0), g_edit.boostTile ? "ON" : "OFF");
    setText(lv_obj_get_child(g_btnNetRole, 0), netRoleName(g_edit.netRole));
    setNetChLabel();
    relabelDisplayButtons();
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

// ---------------------------------------------------------------- views
const char *viewName(View v) {
  switch (v) {
    case View::Minimal: return "MINIMAL";
    case View::Temps: return "TEMPS";
    default: return "RACE";
  }
}

View view() { return g_view; }

void setView(View v) {
  if ((uint8_t)v >= (uint8_t)View::Count) v = View::Race;
  if (v == g_view) return;
  applyView(v, false);
  cfg::Data d = cfg::get();
  d.view = (uint8_t)v;
  // Live now, NVS once the glass has been quiet: a flash write in the same
  // frame as the repaint is what makes the panel's DMA underrun.
  cfg::applyDeferred(d);
  g_displaySaveAt = millis() + kDisplaySaveMs;
}

void nextView() { setView((View)(((uint8_t)g_view + 1) % (uint8_t)View::Count)); }

// ---------------------------------------------------------------- theme
// One place a palette change goes through, whether it came from the Settings
// button, the console or DEFAULTS. Persisting is the caller's choice: DEFAULTS
// has already written its config, the button has not.
void applyTheme(theme::Scheme s) {
  if (!theme::setScheme(s)) return;
  // The rev cells hold colours computed from the old palette, and a crit plate
  // holds a background it will not re-apply while its state is unchanged - so
  // unplate them and let the next readout pass put them back in the new
  // palette (g_forceReadout makes that pass the next frame, not the next tick).
  syncGauge();
  clearPlates();
  g_forceReadout = true;
  // No full-screen invalidation: everything that carries an accent colour
  // repaints itself - the rev cells through syncGauge(), the values, plates and
  // lamps through the readout pass forced above, the bars through the styles
  // theme::setScheme() invalidated. Invalidating w.root here was what made a
  // palette change a 190-230 ms full-screen render (see lv_conf.h).
  Serial.printf("[ui] theme %s\n", theme::schemeName(s));
}

void nextTheme() {
  const theme::Scheme s = theme::schemeAt((uint8_t)theme::scheme() + 1);
  applyTheme(s);
  cfg::Data d = cfg::get();
  d.theme = (uint8_t)s;
  cfg::applyDeferred(d);
  g_displaySaveAt = millis() + kDisplaySaveMs;
}

// Which tile band the glass is showing, and a way to change it. The capability
// decision calls this once when it settles; the console can call it by hand.
void applyLayout(layout::Variant v) { placeBand(v); }

void forceShift(int state) {
  g_forceShift = (int8_t)(state < 0 ? 0 : (state > 3 ? 3 : state));
}

layout::Variant layoutVariant() { return g_variant; }

void cycleHaldexMode() { cycleMode(); }

bool settingsOpen() { return g_page == Page::Settings; }

}  // namespace ui

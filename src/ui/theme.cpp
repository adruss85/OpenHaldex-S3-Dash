// =====================================================================
// theme.cpp - the six runtime palettes
//
// Only the accent family moves between schemes (see the block comment in
// theme.h): the background, the rules, the text and the lamp greys are shared,
// because this is a 16-bit TN panel read at a glance and moving the black
// would move the contrast the dashboard is designed around.
//
// The colours are the S3-gauge accents (green / orange / red / blue / white /
// purple), with red kept as Midnight's critical colour and replaced by the
// scheme's own accent-as-a-plate in the other five. Midnight is the palette
// every build before this one drew. Its readings are now drawn in the same
// white as the rest of the text (the green stays on the bars), which is the one
// deliberate change to it; see docs/Colour instructions.txt.
//
// A scheme change must be cheap and complete. Complete is the hard part: the
// dashboard has ~250 objects whose colour was chosen once at build time.
// Rather than keep a registry of them (and a delete hook per object), the
// build-time colours are attached as *shared* LVGL styles - one style per
// role - so changing the palette is a few style writes plus one
// lv_obj_report_style_change() sweep. Colours that are computed per pass (a
// value's warn/crit colour, the live dot) are written locally as before and
// re-apply themselves on the next tick, because every one of those writes is
// guarded by "does the object already have this colour".
// =====================================================================
#include "theme.h"

#include <Arduino.h>  // Serial: one line if the accent-object list ever fills

namespace theme {
namespace {

// Midnight is dash.html's :root, plus the four greys the LVGL port added
// (button fill, field fill, page background, the soft settings labels) which
// are part of the neutral set now.
// Each entry names the scheme it *is*, not just its position. That is not
// decoration: the 0.9.4 Red scheme was first added in the wrong slot, so the
// table silently disagreed with the enum - the THEME button said "White" while
// the glass was red, and the other way round. The static_assert below turns
// that into a build error.
struct Definition {
  Scheme id;
  const char *name;
  Palette palette;
};

// Midnight is dash.html's :root, plus the four greys the LVGL port added
// (button fill, field fill, page background, the soft settings labels) which
// are part of the neutral set now. It is the one scheme left exactly as it was.
//
// The single-colour schemes are one hue in three steps: accent (base) -> warn
// (lighter) -> crit (brightest). That ordering is what the rev bar draws left
// to right, so the redline is always the most vivid third of the bar rather
// than a repeat of the left end, which is all a single accent could give. Their
// text is the hue itself - saturated, not lifted towards white (fg) - with a
// darkened version for captions and units (dim), so the whole face carries the
// scheme instead of the shared neutral grey. White keeps near-white text,
// because that is what the scheme is called. "shift" stays a different hue on purpose: it is the full-width
// shift flash, and a flash in the family's own colour cannot be told from the
// redline flash at a glance.
constexpr Definition kSchemes[kSchemeCount] = {
    {Scheme::Midnight, "Midnight",
     {.accent = 0x22c55e,
      .accentDim = 0x15803d,
      .accentDimmer = 0x072a14,
      .zoneAccent = 0x123a22,
      .warn = 0xf59e0b,
      .zoneWarn = 0x3a2a10,
      .crit = 0xef4444,
      .critText = 0xffffff,
      .zoneCrit = 0x3a1414,
      .cyan = 0x22d3ee,
      .shift = 0x8b5cf6}},

    // Orange - amber base, brightest crit is yellow. Black on the plate: white
    // on amber is the one pairing this panel cannot hold at a glance.
    {Scheme::Orange, "Orange",
     {.fg = 0xffc247,
      .dim = 0xb97400,
      .accent = 0xf59e0b,
      .accentDim = 0xb45309,
      .accentDimmer = 0x2a1a05,
      .zoneAccent = 0x2e1f08,
      .warn = 0xfbbf24,
      .zoneWarn = 0x4a3410,
      .crit = 0xfde047,
      .critText = 0x000000,
      .accentText = 0x000000,
      .zoneCrit = 0x5c4a14,
      .cyan = 0x22d3ee,
      .shift = 0xa78bfa}},

    // Green - Midnight's own hue, but stepped instead of going to amber/red.
    {Scheme::Green, "Green",
     {.fg = 0x3ee87b,
      .dim = 0x1f9e57,
      .accent = 0x22c55e,
      .accentDim = 0x15803d,
      .accentDimmer = 0x072a14,
      .zoneAccent = 0x0d2f1b,
      .warn = 0x4ade80,
      .zoneWarn = 0x174a2c,
      .crit = 0x86efac,
      .critText = 0x000000,
      .accentText = 0x000000,
      .zoneCrit = 0x22603a,
      .cyan = 0x22d3ee,
      .shift = 0xa78bfa}},

    {Scheme::Blue, "Blue",
     {.fg = 0x5fa8ff,
      .dim = 0x2f6fbf,
      .accent = 0x3b82f6,
      .accentDim = 0x1d4ed8,
      .accentDimmer = 0x0b1a33,
      .zoneAccent = 0x122a45,
      .warn = 0x60a5fa,
      .zoneWarn = 0x1d3a63,
      .crit = 0x93c5fd,
      .critText = 0x000000,
      .accentText = 0xffffff,
      .zoneCrit = 0x2b4d7d,
      .cyan = 0x22d3ee,
      .shift = 0xf472b6}},

    {Scheme::Purple, "Purple",
     {.fg = 0xc98dff,
      .dim = 0x8b46d6,
      .accent = 0xa855f7,
      .accentDim = 0x7e22ce,
      .accentDimmer = 0x1f0a33,
      .zoneAccent = 0x2a1240,
      .warn = 0xc084fc,
      .zoneWarn = 0x3a1f5c,
      .crit = 0xd8b4fe,
      .critText = 0x000000,
      .accentText = 0xffffff,
      .zoneCrit = 0x4a2a73,
      .cyan = 0xe879f9,
      .shift = 0xf472b6}},

    // White - dark grey, light grey, white: the ramp this scheme was missing
    // (both its ends used to be the same near-white). The shift flash is amber,
    // because in a grey dash it is the one cue that must not read as "the bar
    // is full".
    {Scheme::White, "White",
     {.fg = 0xeef2f6,
      .dim = 0x9aa3ad,
      .accent = 0x7c8894,
      .accentDim = 0x4b555f,
      .accentDimmer = 0x1a1e22,
      .zoneAccent = 0x1a1d21,
      .warn = 0xc6ced6,
      .zoneWarn = 0x2f353c,
      .crit = 0xffffff,
      .critText = 0x000000,
      .accentText = 0x000000,
      .zoneCrit = 0x4a525a,
      .cyan = 0x8ab4d8,
      .shift = 0xf59e0b}},

    // Red - listed last because a stored theme is a byte in NVS: the numbers
    // 0..6 are a wire format and may not move.
    {Scheme::Red, "Red",
     {.fg = 0xff5f5f,
      .dim = 0xb03333,
      .accent = 0xef4444,
      .accentDim = 0xb91c1c,
      .accentDimmer = 0x2a0b0b,
      .zoneAccent = 0x33100f,
      .warn = 0xf87171,
      .zoneWarn = 0x4a1a1a,
      .crit = 0xfea3a3,
      .critText = 0x000000,
      .accentText = 0xffffff,
      .zoneCrit = 0x5c2626,
      .cyan = 0xfda4af,
      .shift = 0xc084fc}},
};

constexpr bool tableOrderOk() {
  for (int i = 0; i < kSchemeCount; i++) {
    if ((uint8_t)kSchemes[i].id != (uint8_t)i) return false;
  }
  return true;
}
static_assert(tableOrderOk(), "kSchemes order must match theme::Scheme");
static_assert(sizeof(kSchemes) / sizeof(kSchemes[0]) == kSchemeCount,
              "palette table and Scheme::Count disagree");


Scheme g_scheme = Scheme::Midnight;

uint32_t rgb(Scheme s, Role r) {
  const Palette &p = kSchemes[(uint8_t)s].palette;
  switch (r) {
    case Role::Bg: return p.bg;
    case Role::Panel: return p.panel;
    case Role::Line: return p.line;
    case Role::Fg: return p.fg;
    case Role::Dim: return p.dim;
    case Role::Accent: return p.accent;
    case Role::AccentDim: return p.accentDim;
    case Role::AccentDimmer: return p.accentDimmer;
    case Role::Warn: return p.warn;
    case Role::Crit: return p.crit;
    case Role::CritText: return p.critText;
    case Role::AccentText: return p.accentText;
    case Role::Cyan: return p.cyan;
    case Role::Shift: return p.shift;
    case Role::Track: return p.track;
    case Role::LampOff: return p.lampOff;
    case Role::LampEdge: return p.lampEdge;
    case Role::LampTxtOff: return p.lampTxtOff;
    case Role::ZoneAccent: return p.zoneAccent;
    case Role::ZoneWarn: return p.zoneWarn;
    case Role::ZoneCrit: return p.zoneCrit;
    case Role::White: return p.white;
    case Role::ButtonFill: return p.buttonFill;
    case Role::FieldFill: return p.fieldFill;
    case Role::PageBg: return p.pageBg;
    case Role::SoftLabel: return p.softLabel;
    case Role::Count: break;
  }
  return 0;
}

// One shared style per (role, property kind). A style carrying a text colour
// must not be the same object as one carrying a background colour, or adding
// it to a label would paint a box behind the text.
struct RoleStyles {
  lv_style_t text;
  lv_style_t bg;
  lv_style_t border;
};
RoleStyles g_styles[(int)Role::Count];
uint32_t g_inited = 0;  // one bit per role, set when its styles exist

// The objects styled with a colour a scheme actually changes. setScheme() needs
// to repaint exactly these: everything else on the glass is drawn in a neutral
// role, and repainting it would cost a full-screen redraw (190-230 ms measured
// on this board) to produce an identical picture.
//
// A registry rather than lv_obj_report_style_change(NULL), which refreshes every
// property of every object on every screen: measured at 205 ms for a palette
// change. The objects here are the handful of bars and readings that carry an
// accent at build time; the rev bar's cells and the values are recoloured by
// their own passes (syncGauge() and the 100 ms readout), which is where their
// colours already lived. The list is fixed and small because every object in
// it is built once and never deleted - the pages that are deleted (the splash)
// carry no accent style. lv_obj_is_valid() is still checked, so a stale entry
// costs a scan rather than a crash.
constexpr int kMaxTracked = 64;
lv_obj_t *g_tracked[kMaxTracked];
int g_trackedCount = 0;
bool g_trackOverflow = false;

void track(lv_obj_t *obj, Role role) {
  if (!varies(role)) return;
  for (int i = 0; i < g_trackedCount; i++) {
    if (g_tracked[i] == obj) return;
  }
  if (g_trackedCount < kMaxTracked) {
    g_tracked[g_trackedCount++] = obj;
  } else if (!g_trackOverflow) {
    // Not fatal, but it means a scheme change has to fall back to repainting the
    // screen, so say so on the console rather than quietly costing frames.
    g_trackOverflow = true;
    Serial.println("[theme] accent-object list full: palette changes repaint the screen");
  }
}

void initRole(Role r) {
  const uint32_t bit = 1u << (int)r;
  if (g_inited & bit) return;
  g_inited |= bit;
  RoleStyles &s = g_styles[(int)r];
  lv_style_init(&s.text);
  lv_style_init(&s.bg);
  lv_style_init(&s.border);
  const lv_color_t v = lv_color_hex(rgb(g_scheme, r));
  lv_style_set_text_color(&s.text, v);
  lv_style_set_bg_color(&s.bg, v);
  lv_style_set_border_color(&s.border, v);
}

}  // namespace

Scheme scheme() { return g_scheme; }

const char *schemeName(Scheme s) {
  return kSchemes[(uint8_t)s < kSchemeCount ? (uint8_t)s : 0].name;
}

Scheme schemeAt(uint8_t i) {
  return static_cast<Scheme>(i % kSchemeCount);
}

Col c(Role role) { return Col(role, lv_color_hex(rgb(g_scheme, role))); }

void styleText(lv_obj_t *obj, Col col) {
  if (col.role == Role::Count) {
    lv_obj_set_style_text_color(obj, col.v, 0);
    return;
  }
  initRole(col.role);
  track(obj, col.role);
  lv_obj_add_style(obj, &g_styles[(int)col.role].text, 0);
}

void styleBg(lv_obj_t *obj, Col col, lv_style_selector_t sel) {
  if (col.role == Role::Count) {
    lv_obj_set_style_bg_color(obj, col.v, sel);
    return;
  }
  initRole(col.role);
  track(obj, col.role);
  lv_obj_add_style(obj, &g_styles[(int)col.role].bg, sel);
}

void styleBorder(lv_obj_t *obj, Col col, lv_style_selector_t sel) {
  if (col.role == Role::Count) {
    lv_obj_set_style_border_color(obj, col.v, sel);
    return;
  }
  initRole(col.role);
  track(obj, col.role);
  lv_obj_add_style(obj, &g_styles[(int)col.role].border, sel);
}

bool varies(Role role) {
  switch (role) {
    case Role::Bg:
    case Role::Panel:
    case Role::Line:
    case Role::Fg:
    case Role::Dim:
    case Role::Track:
    case Role::LampOff:
    case Role::LampEdge:
    case Role::LampTxtOff:
    case Role::White:
    case Role::ButtonFill:
    case Role::FieldFill:
    case Role::PageBg:
    case Role::SoftLabel:
      return false;
    default:
      return role != Role::Count;
  }
}

bool setScheme(Scheme s) {
  if ((uint8_t)s >= kSchemeCount || s == g_scheme) return false;
  g_scheme = s;
  for (int r = 0; r < (int)Role::Count; r++) {
    if (!(g_inited & (1u << r))) continue;
    RoleStyles &st = g_styles[r];
    const lv_color_t v = lv_color_hex(rgb(s, static_cast<Role>(r)));
    lv_style_set_text_color(&st.text, v);
    lv_style_set_bg_color(&st.bg, v);
    lv_style_set_border_color(&st.border, v);
  }
  // Repaint what actually moved: the objects styled in an accent role. There is
  // no lv_obj_report_style_change() here - a colour is read from the style at
  // draw time, so invalidating the object is enough, and the sweep costs 205 ms
  // on this board to re-parse properties that no scheme changes. The dynamic
  // colours re-apply themselves on the next tick through the guarded setters,
  // and syncGauge() (the caller's job) repaints the rev cells.
  if (g_trackOverflow) {
    if (lv_obj_t *scr = lv_screen_active()) lv_obj_invalidate(scr);
  } else {
    for (int i = 0; i < g_trackedCount; i++) {
      if (lv_obj_is_valid(g_tracked[i])) lv_obj_invalidate(g_tracked[i]);
    }
  }
  return true;
}

}  // namespace theme

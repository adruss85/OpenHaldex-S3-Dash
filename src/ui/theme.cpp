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
// scheme's own accent-as-a-plate in the other five. Midnight is therefore the
// palette every build before this one drew, and it is untouched.
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
constexpr Palette kSchemes[kSchemeCount] = {
    // Midnight - the ported palette. The only scheme with a red critical.
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
     .shift = 0x8b5cf6},

    // Orange - amber base. BLACK text on the critical plate: white on amber
    // is the one pairing this panel cannot hold at a glance.
    {.accent = 0xf59e0b,
     .accentDim = 0xb45309,
     .accentDimmer = 0x2a1a05,
     .zoneAccent = 0x3a2a10,
     .warn = 0xfbbf24,
     .zoneWarn = 0x4a3410,
     .crit = 0xf59e0b,
     .critText = 0x000000,
     .zoneCrit = 0x3a2a10,
     .cyan = 0xfcd34d,
     .shift = 0xfcd34d},

    // Green - the same accent as Midnight, so warn and crit have to move
    // instead: a lighter green, then the accent plate with black letters.
    {.accent = 0x22c55e,
     .accentDim = 0x15803d,
     .accentDimmer = 0x072a14,
     .zoneAccent = 0x123a22,
     .warn = 0x4ade80,
     .zoneWarn = 0x14361f,
     .crit = 0x22c55e,
     .critText = 0x000000,
     .zoneCrit = 0x123a22,
     .cyan = 0x2dd4bf,
     .shift = 0x86efac},

    // Blue
    {.accent = 0x3b82f6,
     .accentDim = 0x1d4ed8,
     .accentDimmer = 0x0b1a33,
     .zoneAccent = 0x122a45,
     .warn = 0x60a5fa,
     .zoneWarn = 0x1a3358,
     .crit = 0x3b82f6,
     .critText = 0xffffff,
     .accentText = 0xffffff,
     .zoneCrit = 0x1a2f4d,
     .cyan = 0x22d3ee,
     .shift = 0x93c5fd},

    // Purple
    {.accent = 0xa855f7,
     .accentDim = 0x7e22ce,
     .accentDimmer = 0x1f0a33,
     .zoneAccent = 0x2a1240,
     .warn = 0xc084fc,
     .zoneWarn = 0x331a4d,
     .crit = 0xa855f7,
     .critText = 0xffffff,
     .accentText = 0xffffff,
     .zoneCrit = 0x2a1240,
     .cyan = 0xe879f9,
     .shift = 0xd8b4fe},

    // Red. The one scheme where the critical plate and the accent are the same
    // hue, exactly as Green has it: a critical reading is still told apart from
    // a normal one because it inverts (solid red, white letters) rather than
    // being red letters. "Warn" is the same red lifted towards pink, which on
    // this panel measures 0xFB8E against the accent's 0xEA28 - the lighter of
    // the two, and the only direction left to warn in. The shift cue stays
    // violet, deliberately outside the family: in a red dash a red shift flash
    // would be indistinguishable from the redline.
    {.accent = 0xef4444,
     .accentDim = 0xb91c1c,
     .accentDimmer = 0x2a0b0b,
     .zoneAccent = 0x3a1414,
     .warn = 0xf87171,
     .zoneWarn = 0x4a1a1a,
     .crit = 0xef4444,
     .critText = 0xffffff,
     .accentText = 0xffffff,
     .zoneCrit = 0x3a1414,
     .cyan = 0xfda4af,
     .shift = 0xc084fc},

    // White - monochrome. "Warn" is a dimmer grey rather than a different hue:
    // there is no second colour to lean on, so the warning is the shade that
    // recedes and the critical is the one that inverts (black on near-white).
    {.accent = 0xf2f5f8,
     .accentDim = 0x9aa4ae,
     .accentDimmer = 0x1a1d21,
     .zoneAccent = 0x24282d,
     .warn = 0x8f9aa6,
     .zoneWarn = 0x2a2e33,
     .crit = 0xf2f5f8,
     .critText = 0x000000,
     .zoneCrit = 0x24282d,
     .cyan = 0xc6ced6,
     .shift = 0xffffff},
};

static_assert(sizeof(kSchemes) / sizeof(kSchemes[0]) == kSchemeCount,
              "palette table and Scheme::Count disagree");

const char *const kNames[kSchemeCount] = {"Midnight", "Orange", "Green", "Blue",
                                          "Purple",   "White",  "Red"};

Scheme g_scheme = Scheme::Midnight;

uint32_t rgb(Scheme s, Role r) {
  const Palette &p = kSchemes[(uint8_t)s];
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
  return kNames[(uint8_t)s < kSchemeCount ? (uint8_t)s : 0];
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

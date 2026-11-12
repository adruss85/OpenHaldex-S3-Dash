// =====================================================================
// capability.cpp - see capability.h for why any of this is a decision at all
// =====================================================================
#include "app/capability.h"

#include <Arduino.h>

#include <math.h>
#include <stdio.h>

#include "net/chassis.h"
#include "net/ohx_now.h"

namespace cap {
namespace {

Caps g;

// Latched detections: a field is "available" if it has *ever* carried a
// value, not if it carries one right now. A cold engine sends no oil
// temperature for a while, and that must not take a widget off the glass.
bool g_sawSupply = false;
bool g_sawTemps = false;
bool g_sawSteer = false;
// The ForbesAutomotive ME7 patch, latched from Motor_7 itself. One long frame
// is all it takes; losing it afterwards does not remove the row.
bool g_sawEngine7 = false;
bool g_sawOilPressure = false;

uint32_t g_firstPacketMs = 0;
uint32_t g_settledMs = 0;
char g_summary[192] = "waiting for data";

// 20 packets is ~2 s at 10 Hz: long enough that a sentinel-only field has had
// a fair chance to prove otherwise, short enough that the layout settles
// before the driver has pulled away.
constexpr uint16_t kSettlePackets = 20;
// If the packets stop or never start, decide anyway rather than sit
// undecided: an unsettled dashboard would keep whatever shape it booted with.
constexpr uint32_t kSettleMs = 8000;
// The decision is not final immediately after it settles. The relay can start
// a second or two behind the controller's first packet, and on a Gen1 the
// patch's own frames are what say whether it has boost. Without this window a
// Gen1 would keep NoBoost for the whole drive. After it, poll() does nothing.
constexpr uint32_t kLateMs = 15000;

// Boost comes from the generation table, except that a Gen1 can prove the table
// wrong by broadcasting a lengthened Motor_7 - which is exactly what the
// ForbesAutomotive ME7 patch does. Evidence can only add a widget: any
// generation the table already says has boost keeps it, so a car whose CAN
// relay is switched off does not regress to the pre-0.8.0 NoBoost band.
bool boostFor(const bool genKnown, const uint8_t gen, const bool sawEngine7) {
  return !genKnown || gen != 1 || sawEngine7;
}

}  // namespace

void poll() {
#if OHX_USE_ESP_NOW
  // Settled, but still inside the revision window: catch a generation or a
  // patched Motor_7 that turned up after the decision. Outside the window this
  // is the first line that runs, so a settled dash reads no frames at all.
  if (g.settled) {
    if ((uint32_t)(millis() - g_settledMs) > kLateMs) return;

    const ohxnow::Status s = ohxnow::last();
    const chassis::PatchedEngine pe = chassis::patchedEngine();
    if (pe.boost) g_sawEngine7 = true;
    if (pe.oilPressure) g_sawOilPressure = true;

    bool changed = false;
    if (!g.genKnown && s.valid && s.generation != 0) {
      g.genKnown = true;
      g.generation = s.generation;
    }
    const bool boost = boostFor(g.genKnown, g.generation, g_sawEngine7);
    if (boost != g.boost) {
      g.boost = boost;
      g.variant = boost ? layout::Variant::Full : layout::Variant::NoBoost;
      changed = true;
    }
    if (g_sawOilPressure != g.oilPressure) {
      g.oilPressure = g_sawOilPressure;
      changed = true;
    }
    if (changed) {
      g.revision++;
      Serial.printf("[cap] revised after late data: %s\n", summary());
    }
    return;
  }
#else
  if (g.settled) return;
#endif

  const uint32_t now = millis();
#if OHX_USE_ESP_NOW
  const ohxnow::Status s = ohxnow::last();
  // What the chassis frames say about the patch, before the generation table is
  // consulted. A stock PQ car sends Motor_7 three bytes long; a patched one
  // sends eight and is the only source of oil pressure.
  const chassis::PatchedEngine pe = chassis::patchedEngine();
  if (pe.boost) g_sawEngine7 = true;
  if (pe.oilPressure) g_sawOilPressure = true;

  if (s.valid) {
    // 0 is not a Haldex generation (the controller sends 1, 2, 4, 41, 50, 51,
    // 52 ...), so a packet carrying it has not told us anything yet. Latching it
    // would answer "not a Gen1" and hand the car a boost tile it has no source
    // for. Waiting costs nothing: the real value follows, and the sentinel
    // checks below still run meanwhile.
    if (!g.genKnown && s.generation != 0) {
      g.genKnown = true;
      g.generation = s.generation;
      g_firstPacketMs = now;
    }
    // A sentinel reaches us as NaN: the same "no value" the JSON spells null.
    if (!isnan(s.supplyV)) g_sawSupply = true;
    if (!isnan(s.clutchTemp) || !isnan(s.oilTemp) || !isnan(s.moduleTemp)) g_sawTemps = true;
    if (!isnan(s.steering)) g_sawSteer = true;
    g.packetsSeen++;
  }
  const bool havePackets = g.packetsSeen > 0;
#else
  const bool havePackets = false;
#endif

  const bool enough = g.packetsSeen >= kSettlePackets ||
                      (havePackets && (uint32_t)(now - g_firstPacketMs) >= kSettleMs);
  const bool givenUp = !havePackets && now >= kSettleMs;
  if (!enough && !givenUp) return;

  // With no packet there is no generation, so nothing is ruled out: the full
  // band is used rather than hiding a widget on a guess.
  g.boost = boostFor(g.genKnown, g.generation, g_sawEngine7);
  // Oil pressure has no table fallback: it exists only on a car whose Motor_7
  // carries the patch's bytes 5-6, so it is evidence or nothing.
  g.oilPressure = g_sawOilPressure;
  g.supplyVoltage = g_sawSupply;
  g.haldexTemps = g_sawTemps;
  g.steering = g_sawSteer;
  g.variant = g.boost ? layout::Variant::Full : layout::Variant::NoBoost;
  g.settled = true;
  g_settledMs = now;

  Serial.printf("[cap] %s\n", summary());
}

const Caps &get() { return g; }

const char *summary() {
  // No Arduino String here: this runs on the UI task and a heap allocation
  // in a diagnostic path is a way to make the diagnostic the problem.
  char genTxt[12];
  if (g.genKnown) {
    snprintf(genTxt, sizeof(genTxt), "%u", (unsigned)g.generation);
  } else {
    snprintf(genTxt, sizeof(genTxt), "unknown");
  }
  // On a Gen1 the table says no; if the answer is yes there, the lengthened
  // Motor_7 is what changed it, and saying so keeps the console honest.
  const char *boostHow = !g.boost ? "no (table)"
                         : (g.genKnown && g.generation == 1) ? "yes (motor_7)"
                                                            : "yes (table)";
  snprintf(g_summary, sizeof(g_summary),
           "gen %s | boost %s | oil pressure %s | supply %s | haldex temps %s | steering %s | %s"
           " | %u pkts",
           genTxt, boostHow, g.oilPressure ? "yes (motor_7)" : "no",
           g.supplyVoltage ? "yes (detected)" : "no (detected)",
           g.haldexTemps ? "yes (detected)" : "no (detected)",
           g.steering ? "yes (detected)" : "no (detected)",
           layout::bandFor(g.variant).name, (unsigned)g.packetsSeen);
  return g_summary;
}

}  // namespace cap

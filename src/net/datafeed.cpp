// =====================================================================
// datafeed.cpp - merge ESP-NOW and HTTP into the snapshot the UI draws.
// =====================================================================
#include "net/datafeed.h"

#include <Arduino.h>
#include <math.h>

#include "app/config.h"
#include "app/capability.h"
#include "net/can_relay.h"
#include "net/ohx_now.h"

namespace feed {
namespace {

using ohxnow::Status;

constexpr uint32_t kLiveSlowMs = 5000;  // /api/live while it only has temperatures to add
// /api/dashboard once the relay is carrying the values it gates. It is still
// polled: the C6 shuts its access point down after ~5 minutes without a web
// request, and this handler is what resets that clock. See telemetry.h.
constexpr uint32_t kDashSlowMs = 2000;

// The last source we logged, so a transition is printed exactly once.
Source g_lastLogged = Source::None;
uint32_t g_lastChangeMs = 0;
uint32_t g_changes = 0;

/// ESP-NOW wins when it has a number; otherwise keep the HTTP value.
inline float pick(const float packet, const float http) {
  return isnan(packet) ? http : packet;
}

inline int pickInt(const float packet, const int http) {
  return isnan(packet) ? http : (int)lroundf(packet);
}

const char *label(const Source s) {
  switch (s) {
    case Source::EspNow: return "esp-now";
    case Source::Http: return "http";
    default: return "none";
  }
}

}  // namespace

uint32_t nowAgeMs() {
  // The newest of the two ESP-NOW streams. In ANCHOR mode there is no
  // OHX_STATUS at all; the relay's age is what the dashboard can honestly
  // report, and what the console's source line should print.
  const uint32_t now = ohxnow::stats().ageMs;
  if (now != UINT32_MAX) return now;
  return canrelay::stats().ageMs;
}

bool onNow() {
  const uint32_t age = ohxnow::stats().ageMs;
  return age != UINT32_MAX && age <= kHoldMs;
}

// The CAN relay is the other ESP-NOW stream. In ANCHOR mode there is no
// OHX_STATUS at all (no controller), so without this the dash would call a
// perfectly live CAN relay "stale" and show nothing on the status light. It is
// deliberately separate from onNow(): the OHX packet fields are still only
// merged when a controller is actually speaking.
bool onRelay() {
  const canrelay::Stats r = canrelay::stats();
  return r.ageMs != UINT32_MAX && r.ageMs <= kHoldMs;
}

Source source() {
  if (onNow()) return Source::EspNow;
  if (onRelay()) return Source::EspNow;  // the relay rides the same radio
  // "HTTP" only once the poll has actually produced something; before that
  // the honest answer is "nothing has arrived yet".
  return telemetry::live().ageMs == UINT32_MAX ? Source::None : Source::Http;
}

const char *sourceName() { return label(source()); }

void poll() {
#if OHX_USE_ESP_NOW
  ohxnow::poll();
  canrelay::poll();
#endif

  const Source s = source();
  if (s != g_lastLogged) {
    g_lastLogged = s;
    g_changes++;
    Serial.printf("[feed] values now come from %s (change %lu)\n", label(s),
                  (unsigned long)g_changes);
  }
}

telemetry::Live live() {
  const telemetry::Live http = telemetry::live();
  const bool now = onNow();
  const bool relayNow = onRelay();
  if (!now && !relayNow) return http;

  telemetry::Live v = http;  // keeps coolant and engine oil unless the relay has them
  int generation = 0;        // no controller: the relay decodes as PQ

  if (now) {
    const Status p = ohxnow::last();
    v.rpm = pick(p.rpm, http.rpm);
    v.speed = pick(p.speed, http.speed);
    v.throttle = pick(p.throttle, http.throttle);
    v.boost = pick(p.boost, http.boost);
    v.lockTarget = pick(p.lockTarget, http.lockTarget);
    v.lockActual = pick(p.lockActual, http.lockActual);
    // The controller fills this on Gen5 (and Gen4 with KWP); on a Gen1 it is
    // NAN and the body-CAN box's 0x570 below is the source.
    if (!isnan(p.supplyV)) v.batteryV = p.supplyV;
    generation = p.generation;
  }

  // Boost is decided from the Haldex generation, with a lengthened Motor_7
  // (0x588) as evidence that can add it on a Gen1 (capability.h): a stock PQ
  // car's frame is three bytes long, so the controller's own decode reads a
  // confident 0.00 over bytes that are not there. Gate the *value* on what the
  // car can feed, not just the tile band, and until the decision has been made
  // read "--" rather than a guess: the band is Full for those first seconds, so
  // without this the tile that is meant to be hidden shows a number first (#2).
  const cap::Caps &caps = cap::get();
  if (!caps.settled || !caps.boost) v.boost = NAN;
  // Not the packet's oilTemp: that is the Haldex's own oil (Gen4, over KWP), and
  // the OIL tile is engine oil from Kombi_2 with engine thresholds on it. They
  // are different fluids, so one is no fallback for the other.
  const canrelay::Values relay = canrelay::values(generation);
  if (!isnan(relay.oil)) v.oil = relay.oil;
  if (!isnan(relay.coolant)) v.coolant = relay.coolant;
  // Fuel level, same story as the temperatures: the relay is the only source
  // (the API has never carried it), and NAN means the frame is quiet.
  if (!isnan(relay.fuelLitres)) v.fuel = relay.fuelLitres;
  // Oil pressure is the dash's own decode of Motor_7 bytes 5-6, which only
  // exist with the ForbesAutomotive ME7 patch. Same gate as boost: a value with
  // no capability behind it must not reach the row.
  v.oilPressure = (caps.settled && caps.oilPressure) ? relay.oilPressureBar : NAN;
  // The body-CAN box's 0x570 is the battery source on a Gen1; if both it and
  // the controller's supply field are present, the direct body reading wins.
  if (!isnan(relay.batteryV)) v.batteryV = relay.batteryV;

  // Report the age of whichever sample the fields above actually came from,
  // so the console's "live data age" stays truthful.
  const uint32_t age = now ? ohxnow::stats().ageMs : canrelay::stats().ageMs;
  v.ageMs = age < http.ageMs ? age : http.ageMs;
  v.everOk = true;
  return v;
}

telemetry::Dash dash() {
  const telemetry::Dash http = telemetry::dash();
  const bool now = onNow();
  const bool relayNow = onRelay();
  if (!now && !relayNow) return http;

  telemetry::Dash v = http;  // keeps awdWarning, which only HTTP has
  int generation = 0;        // no controller: the relay decodes as PQ
  if (now) {
    const Status p = ohxnow::last();
    v.mode = (int)p.mode;  // always a real byte in the packet
    v.haldexState = (int)p.haldexState;
    v.lockTarget = pickInt(p.lockTarget, http.lockTarget);
    v.lockActual = pickInt(p.lockActual, http.lockActual);
    // Steering: packet first, because /api/dashboard gates steeringAngle on the
    // Haldex generation and reports null on Gen1 while the same angle is in every
    // packet (see docs/upstream-steering-report.md). Merged here, not in the
    // transport, so the JSON's own null is still what 'e' compares.
    v.steeringAngle = pick(p.steering, http.steeringAngle);
    v.chassisCAN = p.chassisCan;
    v.haldexCAN = p.haldexCan;
    v.brakeIn = p.brake;
    v.handbrakeIn = p.handbrake;
    v.couplingOpen = p.couplingOpen;
    v.tempProtection = p.tempProtection;
    // The API works these out from the same two flags: on = not forced off, and
    // null (so off here) without chassis CAN.
    v.tcOn = p.chassisCan && !p.tcForce;
    v.asrOn = p.chassisCan && !p.asrOff;
    generation = p.generation;
  }
  const canrelay::Values relay = canrelay::values(generation);
  if (relay.brake >= 0) v.brakeFromCAN = relay.brake;
  if (relay.handbrake >= 0) v.handbrakeFromCAN = relay.handbrake;
  // The cluster's own traction tell-tale and its intervention bit, straight off
  // Brake_1. Left at -1 (unknown) while the frame is quiet, so the lamps fall
  // back to the packet flags rather than a stale lamp (#1).
  if (relay.asrLamp >= 0) v.asrLamp = relay.asrLamp;
  if (relay.asrActive >= 0) v.asrActive = relay.asrActive;
  // Already gated on BR5_PressureValid in can_relay.cpp: NAN unless the frame
  // says the pressure is real (#3).
  v.brakeBar = relay.brakeBar;
  // Cruise state for the CC lamp, straight off the car's own bus.
  if (relay.cruise >= 0) v.cruise = relay.cruise;
  // The fuel warning, the odometer and the indicator bits: all relay-only, so
  // they are taken as they come (#4/#5/#6).
  v.fuelLow = relay.fuelLow;
  if (!isnan(relay.odometerKm)) v.odometerKm = relay.odometerKm;
  if (relay.indicators >= 0) v.indicators = relay.indicators;
  // Terminal 58s, the automatic day/night source.
  if (relay.term58s >= 0) v.term58s = relay.term58s;
  const uint32_t age = now ? ohxnow::stats().ageMs : canrelay::stats().ageMs;
  v.ageMs = age < http.ageMs ? age : http.ageMs;
  v.everOk = true;
  return v;
}

uint32_t livePeriodMs() {
  const uint32_t normal = cfg::get().pollLiveMs;
  if (!onNow()) return normal;
  if (!isnan(canrelay::values(ohxnow::last().generation).coolant)) return 0;
  return kLiveSlowMs;
}

uint32_t dashPeriodMs() {
  const uint32_t normal = cfg::get().pollDashMs;
  if (!onNow()) return normal;
  const canrelay::Values relay = canrelay::values(ohxnow::last().generation);
  if (relay.brake >= 0 && relay.handbrake >= 0 && normal < kDashSlowMs) return kDashSlowMs;
  return normal;
}

bool fresh(uint32_t staleMs) {
  const uint32_t age = nowAgeMs();
  if (age != UINT32_MAX && age <= staleMs) return true;
  // No controller, but the relay is arriving: the dash is live in ANCHOR mode.
  const canrelay::Stats r = canrelay::stats();
  if (r.ageMs != UINT32_MAX && r.ageMs <= staleMs) return true;
  return telemetry::liveFresh(staleMs);
}

}  // namespace feed

// =====================================================================
// telemetry.h - the OpenHaldex-C6 client
//
// Two independent HTTP pollers run in a dedicated task on core 0:
//
// How often each one actually runs is decided by feed::livePeriodMs() and
// dashPeriodMs(): these are the rates while HTTP is carrying the glass.
//
//   GET /api/live        pollLiveMs (default 1000 ms)  rpm, speed, throttle,
//                                                      boost, coolantTemp,
//                                                      oilTemp, lockTarget,
//                                                      lockActual
//   GET /api/dashboard   pollDashMs (default 1000 ms)  mode, Haldex state,
//                                                      brake / lamp flags,
//                                                      CAN health
//
// Why two rates: /api/dashboard on the C6 calls getCPUUsagePercent(), which
// mallocs and walks every task on the single C6 core. Polling that at 10 Hz
// would cost roughly ten times the CPU for no extra visual resolution.
//
// Why /api/dashboard is polled at all, and never slower than ~2 s: it is the
// only handler that calls otaNoteWebActivity() on the C6, and the C6 shuts
// its access point down after ~5 minutes without it.
//
// A field that the C6 reports as JSON null is stored as NAN, so "sensor not
// present" and "sensor reads zero" stay distinguishable all the way to the UI.
// =====================================================================
#pragma once

#include <math.h>
#include <stdint.h>

namespace telemetry {

struct Live {
  float rpm, speed, throttle, boost;
  float coolant, oil;
  float lockTarget, lockActual;
  // Fuel level in litres, from the cluster's tank contents over the relay. NAN
  // = not heard, or a reading the car says is not real. The relay is the only
  // source; /api/live has never carried it.
  float fuel;
  bool everOk;
  uint32_t ageMs;  // ms since the last successful parse
};

struct Dash {
  int mode;          // -1 when unknown
  int haldexState;   // -1 when unknown
  int lockTarget;
  int lockActual;
  bool chassisCAN;
  bool haldexCAN;
  bool brakeIn;
  bool handbrakeIn;
  bool tcOn;
  bool asrOn;
  bool awdWarning;
  bool couplingOpen;
  bool tempProtection;
  bool everOk;
  uint32_t ageMs;

  // Raw fields the C6 publishes that the dashboard does not use yet. They
  // live here rather than being parsed inside the UI so the raw page and the
  // lamp logic that follows it read the same value from the same place.
  //
  // brakeFromCAN / handbrakeFromCAN are the *chassis CAN* view of the pedals
  // (-1 = the C6 reported null). The wired inputs above them are a different
  // thing: on this car the wired handbrake input is off while the CAN says
  // the handbrake is up, which is the working theory for the dark lamps.
  float steeringAngle;    // deg, NAN when the C6 reports null (Gen1 gating)
  int brakeFromCAN;       // -1 unknown, else 0/1
  int handbrakeFromCAN;   // -1 unknown, else 0/1

  // The cluster's own traction tell-tales, off the relayed chassis bus
  // (0x1A0 Brake_1). asrLamp is the ASR/ESP lamp the car lights when the
  // system is switched off; asrActive is set while it is intervening now.
  // -1 = the frame has not been heard, so the lamps fall back to the packet's
  // tcForce / asrOff flags - which is the inversion that was wrong before.
  int asrLamp = -1;    // -1 unknown, else 0/1
  int asrActive = -1;  // -1 unknown, else 0/1

  // Brake pressure off the relayed chassis bus (0x4A8 Brake_5), already gated
  // on the frame's own BR5_PressureValid: NAN unless the car says the reading is
  // real, so "no braking" and "1.7 bar of braking at a standstill" cannot be
  // confused. Shown on the raw page beside the CAN brake switch (#3).
  float brakeBar = NAN;  // bar

  // Cruise, from the car's own bus: -1 not heard, 0 off, 1 armed (the column
  // switch is on but nothing is being held), 2 regulating. Drives the CC lamp.
  int cruise = -1;

  // The cluster's low-fuel warning (0x320 Cluster_1 bit 23) and its odometer
  // (0x520 Cluster_3, whole km). fuelLow is a plain flag with no sentinel to
  // gate on: it is only read from a frame that has been heard.
  bool fuelLow = false;
  float odometerKm = NAN;

  // The indicator bits as a mask (bit 0 left, bit 1 right; -1 = no frame). The
  // UI latches these and runs its own flash phase - the bits themselves toggle
  // too slowly under packet loss to follow an edge (see docs/upgrades.md #5).
  int indicators = -1;

  // Terminal 58s off Cluster_2: 0 with the lights off, 7..99 % with the dash
  // dimmer. This is the automatic day/night source. -1 = not heard. Terminal_58d
  // is deliberately not read: it sits at a non-zero 24 % with the lights off.
  int term58s = -1;
};

struct Stats {
  uint32_t liveOk;
  uint32_t liveFail;
  uint32_t dashOk;
  uint32_t dashFail;
  uint32_t reconnects;
  int16_t rssi;
  bool wifiUp;
  uint32_t ip;
};

/// Start the network task (core 0). Called once from setup().
void begin();

/// Consistent snapshots.
Live live();
Dash dash();
Stats stats();

/// True when the last live sample is recent enough to be trusted.
bool liveFresh(uint32_t staleMs);

/// The least the network task's stack has ever had free, in bytes. 0 before it starts.
uint32_t netStackSpareBytes();

/// Force a reconnect + interval reset (called after settings change).
void restart();

/// Stop/start the HTTP polling without dropping the association, so the cost
/// of our own requests can be told apart from the radio receiving everything
/// else on the channel.
void setPaused(bool paused);

}  // namespace telemetry

#pragma once
// =====================================================================
// chassis - the car's own bus, read as values.
//
// The controller relays the frames it sees on the chassis CAN (see can_relay.h
// for the wire format). This module is the one place that turns those bytes into
// the numbers the dashboard wants, so adding a value later is a line here rather
// than a new branch somewhere in the UI.
//
// Two rules it exists to enforce, both learned on a car:
//
//   * a signal is only a number when the frame says it is real. Brake_5 reads
//     1.7 bar at a standstill with the pedal up, and its validity bit says so;
//     the tank contents read 127 when they have no value. Every field below
//     carries a *Valid flag or is NAN on its own, and the UI decides how loud to
//     be about it.
//   * nothing here invents timing. An indicator does not flash because this
//     module says so - the frame's bit is mirrored, and if a lamp has to blink
//     the UI blinks it (the relay is 9/s and lossy, so edge-following aliases).
//
// The bytes are read explicitly, with the signal name and bit in a comment,
// rather than through dbc::read(). That is deliberate: these are the same bytes
// the controller has proved on a car, and the DBC is not always right about PQ -
// vw_pq lumps the handbrake into a 2-bit "BrakeInfo" it cannot name, for one.
// The CAN DATA page uses the DBC, and the two being readable side by side is how
// a disagreement gets found rather than argued about.
// =====================================================================
#include <math.h>
#include <stdint.h>

namespace chassis {

/// Everything the dashboard reads off the relayed chassis bus. NAN / -1 = not
/// heard recently, or not fitted on this car.
struct Chassis {
  // --- indicators, mirrored from the cluster (never generated here)
  bool indLeft = false, indRight = false;
  bool indValid = false;

  // --- fuel, from the cluster's own tank contents
  float fuelLitres = NAN;  // l
  bool fuelLow = false;    // the cluster's low-fuel warning
  bool fuelValid = false;

  // --- lighting: Terminal 58, the lighting switch and dimmer wheel
  int term58s = -1;      // 0 = lights off; 1..99 = dimmer %, from Cluster_2
  bool termValid = false;

  // --- cluster
  float odometerKm = NAN;
  float outsideTempC = NAN;  // filtered ambient, the one the car itself shows
  bool odometerValid = false, outsideValid = false;

  // --- engine temperatures, from the cluster frame the C6 also reads
  float coolant = NAN;  // C
  float oil = NAN;      // C, engine oil

  // --- pedals, the chassis CAN's view (the C6's wired inputs are separate)
  int brake = -1, handbrake = -1;  // 0 / 1

  // --- brakes and driver aids
  float brakeBar = NAN;  // bar, only when the frame's validity bit is set
  bool lampAsr = false, lampAbs = false, lampBk = false;  // the cluster's own lamps
  bool lampValid = false;
  bool asrActive = false;  // BR1_ASR_Req: intervention happening now

  // --- cruise control
  bool cruiseMain = false;        // the column stalk's main switch
  bool cruiseRegulating = false;  // the engine says it is regulating to a set speed
  bool cruiseValid = false;
};

/// Read the latest copy of every frame this car has. Cheap enough for the UI
/// task: it copies a handful of frames, all of which are already in PSRAM.
/// `generation` picks the table, as dbc::forGeneration() does.
Chassis read(uint8_t generation);

/// Print the whole structure to the console, one line per group, with "--" where
/// a value is absent or invalid. This is how the decode is checked on a car
/// before anything is drawn from it: a value that is wrong here would be wrong
/// on the glass too.
void print(uint8_t generation);

}  // namespace chassis

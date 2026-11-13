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

  // --- battery voltage, from BodyECU_Load (0x570) byte 2 on the comfort CAN.
  // The body-CAN box is usually the sender on this car; the controller's
  // powertrain bus does not carry 0x570. NAN = never heard.
  float batteryV = NAN;  // V, BatteryVoltage_VehicleBattery: x0.05 + 5
  bool batteryValid = false;

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

  // --- Engine_7 (0x588) as the ForbesAutomotive ME7 patch sends it
  //
  // A stock PQ car broadcasts Motor_7 three bytes long. The patch extends it to
  // eight: byte 4 is the boost sensor, bytes 5-6 the raw 10-bit ADC of the
  // rear-O2 input (now an oil-pressure sensor), byte 7 a copy of Motor_3's IAT.
  // Both values are NAN unless the frame is long enough, because reading past a
  // 3-byte frame is exactly how a missing sensor becomes a confident 0.00.
  float manifoldBar = NAN;     // absolute bar, byte 4; the BOOST tile does not use this
  float oilPressureBar = NAN;  // bar, bytes 5-6 through the PST-F 1 transfer

  // --- pedals, the chassis CAN's view (the C6's wired inputs are separate)
  int brake = -1, handbrake = -1;  // 0 / 1

  // --- brakes and driver aids
  float brakeBar = NAN;  // bar, only when the frame's validity bit is set
  bool lampAsr = false, lampAbs = false, lampBk = false;  // the cluster's own lamps
  bool lampValid = false;
  bool asrActive = false;  // BR1_ASR_Req: intervention happening now

  // --- cruise control
  uint8_t cruiseStatus = 0;       // MO2_Status_GRA: 0 off, 1 activated, 2 overdriven, 3 fault
  bool cruiseEngaged = false;     // a speed is stored: status 1 or 2 (cluster lamp on)
  bool cruiseArmed = false;       // the system is switched on: the column switch, or engaged
  bool cruiseMain = false;        // 0x38A GRA_MainSwitch, the column stalk's on/off
  bool cruiseRegulating = false;  // MO2_GRAregelt: the engine is adding torque now (not a lamp state)
  float cruiseSetKmh = NAN;       // MO2_GRA_Target: the set speed, NaN when none is stored
  bool cruiseValid = false;
};

/// Read the latest copy of every frame this car has. Cheap enough for the UI
/// task: it copies a handful of frames, all of which are already in PSRAM.
/// `generation` picks the table, as dbc::forGeneration() does.
Chassis read(uint8_t generation);

/// What a lengthened Motor_7 says about the car, for capability detection.
///
/// `boost` is the frame carrying byte 4 at all (length >= 5), which a stock PQ
/// car never does. `oilPressure` additionally wants the 8-byte form whose byte 7
/// copies Motor_3's IAT - the patch's own signature - so a different long frame
/// cannot light up an oil row nothing is feeding.
struct PatchedEngine {
  bool boost = false;
  bool oilPressure = false;
};

PatchedEngine patchedEngine();

/// Print the whole structure to the console, one line per group, with "--" where
/// a value is absent or invalid. This is how the decode is checked on a car
/// before anything is drawn from it: a value that is wrong here would be wrong
/// on the glass too.
void print(uint8_t generation);

}  // namespace chassis

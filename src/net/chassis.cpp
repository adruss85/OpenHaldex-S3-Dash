// =====================================================================
// chassis.cpp - see chassis.h for why any of this is a decision at all
// =====================================================================
#include "net/chassis.h"

#include <Arduino.h>

#include <stdio.h>

#include "net/can_relay.h"
#include "net/dbc.h"

namespace chassis {
namespace {

// How long a value read from a frame is believed: long enough to ride out a
// burst of packet loss at 9/s, short enough that a frame which has stopped is
// treated as gone rather than as current. It is the same 2 s the controller
// gives Kombi_2 before its own API reports null.
constexpr uint32_t kFreshMs = 2000;

// The latest copy of an id, if it is recent enough and long enough to hold the
// signal the caller is about to read. The length test is not politeness: the
// boost signal on a Gen1's Engine_7 simply is not in the frame, and reading past
// the end of it is how the controller reports a confident 0.00 bar.
bool recent(uint32_t id, uint8_t minLength, canrelay::Frame &out) {
  return canrelay::find(id, false, out) && out.ageMs <= kFreshMs && out.length >= minLength;
}

void readPq(Chassis &c) {
  canrelay::Frame f;

  // ---- Cluster_1 (0x320). Fuel, indicators, handbrake.
  if (recent(0x320, 3, f)) {
    // TankContents 16|7, 1 l per count, and 127 is what it reads when it has no
    // value. ErrorStatus_Tank is bit 1. Either one means the fuel bar must show
    // nothing rather than an empty tank.
    const uint8_t raw = f.data[2] & 0x7F;
    c.fuelValid = raw != 0x7F && ((f.data[0] >> 1) & 0x01) == 0;
    c.fuelLitres = c.fuelValid ? (float)raw : NAN;
    c.fuelLow = ((f.data[2] >> 7) & 0x01) != 0;  // LowFuelWarning, bit 23
  }
  if (recent(0x320, 6, f)) {
    // The cluster's own indicator bits, next to the speed it displays. Mirrored,
    // not generated: the dashboard decides how to draw a blink from these.
    c.indLeft = ((f.data[5] >> 4) & 0x01) != 0;   // Indicator_Left_4_1, bit 44
    c.indRight = ((f.data[5] >> 5) & 0x01) != 0;  // Indicator_Right_4_1, bit 45
    c.indValid = true;
  }
  if (recent(0x320, 2, f)) {
    // The 2-bit "BrakeInfo" at bits 8..9. vw_pq can only call it BrakeInfo, but
    // bit 1 is the handbrake and has read 1 while parked - proven on the car, so
    // it is read by bit rather than by name.
    c.handbrake = (f.data[1] >> 1) & 0x01;
  }

  // ---- Cluster_2 (0x420). Temperatures, ambient, and the lighting switch.
  if (recent(0x420, 5, f)) {
    // The same two bytes the controller reads, so the dashboard and the
    // controller's own API cannot disagree about them.
    c.oil = (float)f.data[3] - 60.0f;             // OilTemperature_4_1, 24|8 x1 -60
    c.coolant = (float)f.data[4] * 0.75f - 48.0f; // CoolantTemp_4_1, 32|8 x0.75 -48
  }
  if (recent(0x420, 2, f)) {
    // OutsideTemperature_Filtered, 8|8 x0.5 -50. The filtered one on purpose:
    // it is what the car's own display shows. ErrorStatus is bit 0.
    if ((f.data[0] & 0x01) == 0) {
      c.outsideTempC = (float)f.data[1] * 0.5f - 50.0f;
      c.outsideValid = true;
    }
  }
  if (recent(0x420, 7, f)) {
    // Terminal_58s, 48|7: 0 with the light switch off, 7..99 % with it on and the
    // dimmer wheel wherever it is left. 127 is what the field reads when it has
    // no value at all - the whole frame is sentinels with the car switched off
    // (byte 5 reads 127 too) - and 127 > 0 would read as "lights on", which is
    // exactly the wrong answer at night. ErrorStatus_Term__58_s is bit 55, and a
    // flagged reading is treated as "not heard" so the day/night rule holds its
    // last mode rather than strobing off a bad frame.
    const int s = f.data[6] & 0x7F;
    if (((f.data[6] >> 7) & 0x01) == 0 && s != 0x7F) {
      c.term58s = s;
      c.termValid = true;
    }
  }

  // ---- Cluster_3 (0x520). Odometer, 40|20 km: the low 20 bits of bytes 5..7.
  if (recent(0x520, 8, f)) {
    c.odometerKm = (float)(f.data[5] | (f.data[6] << 8) | ((f.data[7] & 0x0F) << 16));
    c.odometerValid = true;
  }

  // ---- Brake_1 (0x1A0). The cluster's tell-tales and the intervention bit.
  if (recent(0x1A0, 2, f)) {
    c.lampAbs = (f.data[1] & 0x01) != 0;          // BR1_Lamp_ABS, bit 8
    c.lampAsr = ((f.data[1] >> 1) & 0x01) != 0;   // BR1_Lamp_ASR, bit 9 (ASR/ESP)
    c.lampBk = ((f.data[1] >> 2) & 0x01) != 0;    // BR1_Lamp_BK,  bit 10
    c.asrActive = (f.data[0] & 0x01) != 0;        // BR1_ASR_Req,   bit 0
    c.lampValid = true;
  }

  // ---- Brake_5 (0x4A8). Brake pressure, only when it says it is real.
  if (recent(0x4A8, 4, f)) {
    // BR5_BrakePressure 16|12 x0.1 bar, gated on BR5_PressureValid (bit 29).
    // Without the gate a car at a standstill with the pedal up reads 1.7 bar.
    const bool valid = ((f.data[3] >> 5) & 0x01) != 0;
    if (valid) c.brakeBar = (float)(f.data[2] | ((f.data[3] & 0x0F) << 8)) * 0.1f;
  }

  // ---- Engine_2 (0x288). The brake light switch, and cruise state.
  if (recent(0x288, 4, f)) {
    c.brake = f.data[2] & 0x01;  // MO2_BLS, bit 16
    // MO2_Status_GRA 22|2 (0 off, 1 activated, 2 overdriven, 3 faulted). Its
    // value table is the cluster's lamp: 1/2 are the lamp-on states, and that is
    // a stored set speed, not merely the column switch being on. On the car,
    // arming cruise leaves this at 0 and the cluster lamp dark; the first SET
    // (+ or -) takes it to 1. So this is "engaged", and the armed state comes
    // from the switch in 0x38A.
    const uint8_t gra = (uint8_t)((f.data[2] >> 6) & 0x03);
    c.cruiseStatus = gra;
    c.cruiseEngaged = (gra == 1 || gra == 2);
    // MO2_GRAregelt 26|1 is decoded for the log and the CAN DATA page, but it is
    // NOT the regulating state the lamp used to take its brightness from: on the
    // car it read 1 while the + side of the rocker was in use and 0 with the -,
    // so a lamp keyed off it went bright or dark depending on which way the set
    // speed was being nudged.
    c.cruiseRegulating = ((f.data[3] >> 2) & 0x01) != 0;
    c.cruiseValid = true;
  }
  if (recent(0x38A, 2, f)) {
    // GRA_MainSwitch 8|1: the column switch's on/off, which is the armed state.
    // It reads 1 whenever the system is switched on, including on a parked car
    // with no speed stored - that is what armed means.
    c.cruiseMain = (f.data[1] & 0x01) != 0;
    c.cruiseValid = true;
  }

  // Three lamp states from two facts, both the car's own:
  //   off      - the column switch is off and no speed is stored
  //   armed    - the column switch is on, no speed stored (the cluster's own lamp
  //              stays dark here; the dim plate is this dash's reminder)
  //   engaged  - MO2_Status_GRA says a speed is stored (the cluster's lamp is on)
  // Decided after both frames have been read, because either can arrive first.
  c.cruiseArmed = c.cruiseEngaged || c.cruiseMain;
}

void readMqb(Chassis &c) {
  canrelay::Frame f;

  // MQB is thinner here on purpose: only what the controller itself reads has
  // been on a car (see can_relay.cpp). 254 and 255 mean "initialising" and
  // "fault", so they are treated as no value.
  if (recent(0x640, 4, f)) {
    if (f.data[2] < 0xFE) c.oil = (float)f.data[2] - 60.0f;
    if (f.data[3] < 0xFE) c.coolant = (float)f.data[3] * 0.75f - 48.0f;
  }
  if (recent(0x106, 4, f)) c.brake = (f.data[3] >> 2) & 0x01;       // ESP_05: ESP_Fahrer_bremst
  if (recent(0x30B, 3, f)) c.handbrake = (f.data[2] >> 7) & 0x01;   // Kombi_01: KBI_Handbremse
}

}  // namespace

Chassis read(const uint8_t generation) {
  Chassis c;
  const dbc::Table *table = dbc::forGeneration(generation);
  if (table == nullptr) return c;  // another make: its ids mean something else
  if (table == &dbc::kVwPq) {
    readPq(c);
  } else {
    readMqb(c);
  }
  return c;
}

void print(const uint8_t generation) {
  const Chassis c = read(generation);

  // "num unit" when present, "--" when not: an absent value must never print as
  // 0, which is how a missing tank reading becomes an empty tank.
  char fuel[24], odo[24], amb[24], cool[16], oil[16], bar[16];
  auto num = [](char *out, size_t n, float v, int dec, const char *unit) {
    if (isnan(v)) {
      snprintf(out, n, "--");
    } else {
      snprintf(out, n, "%.*f %s", dec, (double)v, unit);
    }
  };
  if (c.fuelValid) {
    num(fuel, sizeof fuel, c.fuelLitres, 1, "l");
  } else {
    snprintf(fuel, sizeof fuel, "--");
  }
  if (c.odometerValid) {
    num(odo, sizeof odo, c.odometerKm, 0, "km");
  } else {
    snprintf(odo, sizeof odo, "--");
  }
  if (c.outsideValid) {
    num(amb, sizeof amb, c.outsideTempC, 1, "C");
  } else {
    snprintf(amb, sizeof amb, "--");
  }
  num(cool, sizeof cool, c.coolant, 0, "C");
  num(oil, sizeof oil, c.oil, 0, "C");
  num(bar, sizeof bar, c.brakeBar, 1, "bar");

  Serial.printf("[chassis] gen %u | fuel %s low %d %s | odo %s | 58s %d %s | outside %s\n",
                (unsigned)generation, fuel, c.fuelLow ? 1 : 0, c.fuelValid ? "" : "(invalid)", odo,
                c.term58s, c.termValid ? "" : "(no frame)", amb);
  Serial.printf("[chassis] coolant %s | oil %s | brake %s | hand %s\n", cool, oil,
                c.brake < 0 ? "--" : (c.brake ? "1" : "0"),
                c.handbrake < 0 ? "--" : (c.handbrake ? "1" : "0"));
  Serial.printf("[chassis] pressure %s | lamps asr/abs/bk %d/%d/%d %s | intervening %d\n", bar,
                c.lampAsr ? 1 : 0, c.lampAbs ? 1 : 0, c.lampBk ? 1 : 0,
                c.lampValid ? "" : "(no frame)", c.asrActive ? 1 : 0);
  Serial.printf("[chassis] indicators L%d R%d %s | cruise main %d status %d armed %d "
                "engaged %d regelt %d %s\n",
                c.indLeft ? 1 : 0, c.indRight ? 1 : 0, c.indValid ? "" : "(no frame)",
                c.cruiseMain ? 1 : 0, (int)c.cruiseStatus, c.cruiseArmed ? 1 : 0,
                c.cruiseEngaged ? 1 : 0, c.cruiseRegulating ? 1 : 0,
                c.cruiseValid ? "" : "(no frame)");
}

}  // namespace chassis

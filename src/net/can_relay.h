#pragma once
// =====================================================================
// can_relay - the car's CAN frames, relayed by the controller over ESP-NOW.
//
// With "Relay CAN Frames to Gauges" switched on in its web UI, the controller
// broadcasts every frame it receives on its chassis bus (bus 0), batched into
// packets, in the wire format of include/cannow_proto.h. This keeps the latest
// copy of each id, which is what the CAN DATA page shows, and reads the few values
// the dashboard wants straight from it, so they stop needing an HTTP request.
//
// Not in the relay: bus 1, so the Haldex's own frames (its lock and status arrive
// in the OHX_STATUS packet instead), and anything the controller transmits itself.
//
// Broadcast has no retransmission, so this is a live view, not a lossless log. The
// counters say how much went missing, in the air and at the sender.
// =====================================================================
#include <math.h>
#include <stdint.h>

namespace canrelay {

constexpr int kMaxFrames = 200;  // distinct ids kept; PQ and MQB both use well under this

/// The latest copy of one id.
struct Frame {
  uint32_t id;
  bool extended;
  uint8_t length;
  uint8_t data[8];
  uint32_t count;   // copies received
  uint16_t perSec;  // received per second, over the last second
  uint32_t ageMs;   // since the last copy
};

struct Stats {
  uint32_t packets;   // relay packets accepted
  uint32_t frames;    // frames in them
  uint32_t airLost;   // packets missed in the air (gaps in each sender's sequence)
  uint32_t srcLost;   // frames the senders say they could not relay
  uint32_t rejects;   // malformed packets
  uint32_t tableFull; // frames dropped because kMaxFrames ids are already known
  uint32_t perSec;    // frames per second, over the last second
  int ids;            // distinct ids heard
  uint32_t ageMs;     // since the last packet (UINT32_MAX = never)
  uint8_t senders;    // distinct source MACs heard (the controller plus, e.g., the body-CAN box)
};

/// The values the dashboard reads from the relay. NAN / -1 = not heard recently,
/// or not known for this car.
struct Values {
  float coolant = NAN;  // C
  float oil = NAN;      // C, engine oil
  int brake = -1;       // 0 / 1
  int handbrake = -1;   // 0 / 1
  // The cluster's own tell-tales, from 0x1A0 Brake_1. asrLamp is BR1_Lamp_ASR,
  // the lamp the car lights when traction control is switched off (and flashes
  // while it intervenes); asrActive is BR1_ASR_Req, set while it is intervening
  // right now. The dashboard mirrors these rather than inverting the packet's
  // tcForce / asrOff flags, so the lamps match the car.
  int asrLamp = -1;    // 0 / 1
  int asrActive = -1;  // 0 / 1
  // Brake pressure from 0x4A8 Brake_5, filled in only when the frame's own
  // BR5_PressureValid is set: a car at a standstill with the pedal up otherwise
  // reads 1.7 bar of braking at all times. NAN = not valid or not heard.
  float brakeBar = NAN;  // bar

  // Oil pressure from Engine_7 (0x588) bytes 5-6, which only exist on a car with
  // the ForbesAutomotive ME7 patch. The ECU sends the sensor's raw ADC, so the
  // bar figure is scaled here from the PST-F 1 transfer. NAN = no long frame.
  float oilPressureBar = NAN;  // bar
  // Cruise, as one code because the lamp wants exactly three answers: -1 not
  // heard, 0 off, 1 armed (the column switch is on, no speed stored), 2 engaged
  // (the engine says a speed is stored). See chassis.h for the two facts it is
  // built from.
  int cruise = -1;

  // Fuel, from the cluster's tank contents. NAN = no reading, or one the car
  // says is not real, so an empty tank cannot be shown for a missing sensor.
  float fuelLitres = NAN;
  bool fuelLow = false;

  // The cluster's odometer, and the indicator bits as a mask (bit 0 left, bit 1
  // right; -1 = no frame). The bits are mirrored raw - the blink the driver sees
  // is the dashboard's own, because 9/s and lossy cannot follow an edge.
  float odometerKm = NAN;
  int indicators = -1;

  // Terminal 58s, the lighting switch and dash dimmer: 0 with the lights off,
  // 7..99 % with them on. -1 = not heard. One signal carries both the day/night
  // decision and the night brightness (see board_config.h).
  int term58s = -1;
};

/// Makes the frame tables in PSRAM. Call in setup(), before the network starts.
void begin();

/// One relay packet, from the ESP-NOW receive callback (the WiFi task). The caller
/// has already checked the CANN magic bytes. Any sender is accepted: the relay
/// carries display-only data, and cannow_proto.h arbitrates per id between
/// senders (the first one heard keeps each id until it goes quiet). Short, and
/// never blocks.
void onPacket(const uint8_t *data, int length, const uint8_t src[6]);

/// Call from the main loop: works out the per-second rates.
void poll();

/// True while relay packets are arriving.
bool fresh();

Stats stats();

/// How many distinct ids have been heard, and the frame in each slot, in the order
/// they were first heard. False if there is nothing in that slot.
int count();
bool frameAt(int index, Frame &out);

/// The latest copy of one id. False if it has never been heard.
bool find(uint32_t id, bool extended, Frame &out);

/// Read the dashboard's values for this Haldex generation's car.
Values values(uint8_t generation);

}  // namespace canrelay

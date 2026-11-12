// =====================================================================
// capability.h - what this car can actually fill
//
// The dashboard's shape is chosen, once at boot, from what the controller
// says it can feed. Two ways of finding out, in this order of preference:
//
//   detected   the packet marks a field it has no value for with a sentinel
//              (OHX_NONE8 / OHX_NONE16 / OHX_NONE_T), which reaches us as NaN.
//              supply voltage, the Haldex-side temperatures and the steering
//              angle are all detectable this way, and detection is preferred
//              because it is the car telling us rather than us assuming.
//
//   table      boost is the exception: the controller has no absence sentinel
//              for it, so a car whose chassis CAN carries no boost frame
//              reads a confident 0.00 rather than "no value" and cannot be
//              detected through the packet at all. It is therefore decided
//              from the Haldex generation, which is the only thing we have that
//              correlates: measured on the Gen1 car this was written against,
//              Motor_7 (0x588) is the only PQ boost frame and a stock car does
//              not broadcast it, so boost never moves.
//
//   frame      the exception to the exception. A Gen1 can still prove it has a
//              boost source by lengthening Motor_7 (0x588), which is exactly
//              what the ForbesAutomotive ME7 patch does: the stock 3-byte frame
//              becomes 8, with byte 4 the boost sensor and bytes 5-6 the raw ADC
//              of an oil-pressure sensor on the rear-O2 input. A length >= 5
//              frame therefore upgrades boost on a Gen1 and is the only way oil
//              pressure is ever offered. Evidence can only add a widget, never
//              take one away: a generation the table says has boost keeps it
//              even if the relay is switched off.
//
// The decision is made once and latched. It is not re-evaluated per frame: a
// layout that changed shape while the car was moving would be worse than a
// widget that reads "--". It is, however, revised for a short window after the
// decision settles if the generation or the patch's frames only arrive then -
// see poll().
// =====================================================================
#pragma once

#include <stdint.h>

#include "theme.h"

namespace cap {

struct Caps {
  bool settled;            // the decision has been made and latched
  bool genKnown;           // a packet has told us the Haldex generation
  uint8_t generation;
  uint16_t packetsSeen;

  // What the car can feed. "false" means the dashboard must not reserve
  // space for it, not merely that it reads "--".
  bool boost;
  bool oilPressure;        // Motor_7 bytes 5-6: the ForbesAutomotive ME7 patch
  bool supplyVoltage;
  bool haldexTemps;
  bool steering;

  layout::Variant variant;  // which tile band the dashboard should use

  uint32_t revision;        // bumped if a capability changes after `settled`
};

/// Feed this from loop(). Cheap: it does nothing at all once settled.
void poll();

const Caps &get();

/// One line describing the decision, for the console and the raw page.
const char *summary();

}  // namespace cap

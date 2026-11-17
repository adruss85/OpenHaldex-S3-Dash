// =====================================================================
// capability.h - what this car can actually fill
//
// The BOOST panel is a setting now (config::Data::boostTile), not something
// inferred from the car. What capability still decides on the glass is the
// oil-pressure row in the temperatures card: it is offered only on a car whose
// Motor_7 carries the ForbesAutomotive ME7 patch. Boost detection is kept
// because the raw page and the console summary should say whether the car has
// a boost source at all, but it no longer moves the layout.
//
// Two ways of finding out, in this order of preference:
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
// widget that reads "--". What *is* still watched after it settles is evidence
// that only ever adds something - a generation, or the patched Motor_7 that
// offers oil pressure - because the CAN relay can come up long after the
// controller's first packet (0.9.x makes the body-CAN box scan for the dash's
// AP first). There is no time limit on that: see poll().
// =====================================================================
#pragma once

#include <stdint.h>

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

  uint32_t revision;        // bumped if a capability changes after `settled`
};

/// Feed this from loop(). Cheap: after it settles it only compares a couple of
/// latched booleans, and does so for the life of the boot - the relay can start
/// long after the controller.
void poll();

const Caps &get();

/// One line describing the decision, for the console and the raw page.
const char *summary();

}  // namespace cap

// =====================================================================
// config.h - persisted dashboard configuration (NVS)
//
// Mirrors DEFAULTS in dash.html so the two UIs behave identically:
//   rpmMax 8000  rpmAmber 6000  rpmRedline 6800  rpmShift 7300
//   coolWarn 100 coolCrit 110  oilWarn 120       oilCrit 135
//   boostMax 2500 boostWarn 1800 boostCrit 2200 (millibar gauge)
//   tempUnits "C"  pressureUnits "bar"  brightness 1.0
//   pollLiveMs 100  pollDashMs 1000  staleMs 500
// =====================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cfg {

// Legacy pre-v7 boost-units values, kept only for the append-only NVS layout
// and migration. The live setting is PressureUnits, shared by boost and oil.
enum BoostUnits : uint8_t { BOOST_BAR = 0, BOOST_MBAR = 1, BOOST_PSI = 2 };

/// How the backlight is decided. Auto reads Terminal 58s off the car's own bus
/// (0 = lights off, 7..99 % = the dash dimmer wheel), which carries both the
/// mode and the night brightness in one signal.
enum DayNightMode : uint8_t { DN_AUTO = 0, DN_DAY = 1, DN_NIGHT = 2 };

/// Which side of the fleet the dash is on. In both roles the dash runs its own
/// access point, so the fleet always has an anchor to scan for (see
/// docs/architecture.md, "The pseudo wireless CAN network"):
///   FOLLOWER  AP+STA: the AP rides the controller's channel, the STA joins it.
///   ANCHOR    AP only: the dash is the fleet's channel, no controller needed.
enum NetRole : uint8_t { NET_FOLLOWER = 0, NET_ANCHOR = 1 };

/// One pressure unit for the whole dash: the boost value/peak and the oil
/// pressure row. The small unit labels carry the choice, because the 22 px
/// number face has no letters.
enum PressureUnits : uint8_t { PRESSURE_BAR = 0, PRESSURE_PSI = 1 };

struct Data {
  // Connection to the OpenHaldex-C6. Stored in NVS, never in git.
  char ssid[33];
  char pass[65];
  char host[48];
  uint16_t port;

  // Tachometer / warning thresholds.
  float rpmMax;
  float rpmAmber;
  float rpmRedline;
  float rpmShift;
  float coolWarn;
  float coolCrit;
  float oilWarn;
  float oilCrit;
  float boostMax;
  float boostWarn;
  float boostCrit;

  bool tempF;           // false = degC, true = degF
  uint8_t boostUnits;   // legacy BoostUnits; folded into pressureUnits (v7)
  float brightness;     // 0..100 %

  // Polling. pollDashMs must stay <= 2000 to keep the C6's WiFi awake:
  // only /api/dashboard calls otaNoteWebActivity() on the C6, and it drops
  // the access point after ~5 minutes of inactivity.
  uint16_t pollLiveMs;
  uint16_t pollDashMs;
  uint16_t staleMs;

  // 1 = skip DHCP and go straight to the static address. Learnt at runtime:
  // set after the first successful static join, because the C6's AP leases no
  // address while it also sits on a home LAN with the same 192.168.1.0/24.
  // Waiting 15 s for a lease that never comes costs 15 s of a blank dashboard
  // on every boot and on every reconnect.
  uint8_t netUseStatic;

  // --- added in version 5. These sit at the END of Data on purpose: an older
  // NVS blob is then a byte-for-byte prefix of this one, so it can be read back
  // into the same struct (config.cpp) instead of being thrown away, which would
  // cost the connection settings that were learnt on the car.

  // Tank size, only for the fuel bar's percentage and the low-fuel threshold:
  // the litres themselves come from the car (TankContents, 1 l per count).
  float tankLitres;

  // 1 = km/h and km (metric), 0 = mph and miles. Distance only: the
  // temperature toggle above is a separate setting and stays separate.
  uint8_t unitsMetric;

  uint8_t dayNight;  // DayNightMode

  // --- added in version 6. Network role and the channel the dash's own AP
  // starts on. In FOLLOWER the AP follows the controller's channel once the
  // STA joins; this channel is the fallback while it is not joined. In ANCHOR
  // it is the fleet's channel. Appended for the same prefix-compatible reason
  // as the version 5 fields above.
  uint8_t netRole;      // NetRole
  uint8_t fleetChannel; // 1..13

  // --- added in version 7. System pressure units (PressureUnits): boost and
  // oil pressure share it. The legacy boostUnits field above stays for the
  // NVS layout and migration only.
  uint8_t pressureUnits;
};

/// Load from NVS (or defaults + any build-time secrets on first boot).
void begin();

/// Validated, clamped, saved to NVS.
void apply(const Data &d);

/// The shipped defaults, validated. Nothing is applied or saved: the caller
/// decides what to keep (the DEFAULTS button keeps the connection) and hands
/// the result to apply().
Data defaults();

/// Current values. The UI thread owns writes; scalars are read directly by
/// the network task, strings are copied under a lock.
const Data &get();

/// Copy of the connection fields only, safe against a concurrent apply().
void connectionCopy(char *ssid, size_t ssidLen, char *pass, size_t passLen, char *host,
                    size_t hostLen, uint16_t &port);

/// Bumped when the connection settings change (ssid, password, host, port), so
/// the network task knows to rejoin. Nothing else moves it: rejoining means a
/// few seconds of stale data, which a brightness change must not cost.
uint32_t revision();

/// Ask the network task to rejoin even though nothing changed.
void requestReconnect();

}  // namespace cfg

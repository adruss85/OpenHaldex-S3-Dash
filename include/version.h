// =====================================================================
// version.h - the dash's own version
//
// The firmware had no version string at all, which matters for three
// reasons: a boot log that cannot say what is running, an OTA client with
// nothing to compare against, and a rollback with nothing to report. One
// place to change, printed at boot and in the raw page's title.
// =====================================================================
#pragma once

#define OHX_DASH_VERSION "0.8.2"  // Relay receive: any sender, per-sender seq, bench channel pin
// 0.8.2 - The relay is no longer one-sender-only. The body-CAN box
// (ESP32-bodycan, a second CANN sender on the rig's channel 11) was invisible
// to this dash three times over. The CAN-relay branch of the ESP-NOW callback
// took packets from the one remembered controller MAC only, which a body-CAN
// box can never become - it sends no OHX_STATUS and hosts no access point - so
// every packet was dropped without a counter. With no controller on the air the
// station sat wherever the last scan left it (measured: channels 1/4/12/13 in
// one boot), so channel-11 frames were never even received. And with no
// generation in any status packet, forGeneration() returned no DBC table, so
// the CAN DATA page decoded nothing. Now: relay packets are taken from any
// sender heard on the channel (cannow_proto.h arbitrates per id between
// senders; the first heard keeps each id until it goes quiet), which is what
// lets the dash sit in the middle of several ESP-NOW devices each broadcasting
// one thing; air-lost and seq are tracked per source MAC so interleaved
// senders do not read as phantom losses (8-sender table, beyond it senders
// fold into the first slot); the listen channel is pinned to 11 after every
// failed join attempt while nothing is joined, never while associated; and a
// missing generation decodes as PQ so a relay-only bench still gets names and
// scaling. The command path, controller identification and the dashboard's
// source table are untouched. Verified on the bench, 12 Nov 2026, against
// ESP32-bodycan (synthetic frames): `C` lists 0x570 BodyECU_Load, 0x470
// BodyECU_Cluster and 0x578 BattMan_1 with DBC names and decodes
// BatteryVoltage_VehicleBattery 14.00 V (0xB4 = 180, x0.05+5). Not verified
// here: the C6 path itself (no controller on this bench). Reception while no
// access point is present is bursty - each join attempt spends ~8 s hopping
// channels, so roughly half the sender's packets are missed and `air lost`
// reads honestly high; staying parked on the pinned channel between attempts
// is the next lever. Senders are counted (N sender(s)) but not yet named or
// shown - TODO 1.6.
// 0.8.1 - Binary size, with nothing changed on the glass or the wire. The
// stock montserrat_22/28/48 faces are replaced by subsets of those very faces,
// generated from the same Montserrat-Medium.ttf with the options LVGL itself
// used and verified byte-identical glyph for glyph, with the stock line
// metrics pinned (tools/make_fonts.sh). lv_conf.h compiles out every LVGL
// widget, layout engine and colour-format blend path the dash never uses, and
// the two HTTP pollers bind a persistent plain NetworkClient so the
// URL-sniffing begin() cannot link the mbedTLS SSL client for a controller
// that is always plain http. Measured against the 0.8.0 tree:
// 1,847,186 -> 1,525,962 B of the 1,966,080 B OTA slot (94.0% -> 77.6%),
// static RAM 60,412 -> 60,196 B. What was switched off, and why it is safe,
// is written beside each define in lv_conf.h and in docs/display-and-memory.md.
// Also 0.8.1: the night-brightness floor comes down. 0.6.0's night curve
// (20 % + 45 % × 58s/100) never went below 23 % with the dimmer wheel at its
// bottom stop (58s 7 %), which was still too bright in the car; it is now
// 5 % + 60 % × 58s/100 - 9 % at the bottom stop, the ~64 % ceiling unchanged
// (include/board_config.h). Confirmed on the car, 12 Nov 2026.
// 0.8.0 - Boost and oil pressure on cars with the ForbesAutomotive ME7 patch.
// The patch lengthens Motor_7 (0x588) from a stock PQ car's three bytes to
// eight: byte 4 is the boost sensor, bytes 5-6 the raw ADC of the rear-O2 input
// now carrying an oil-pressure sensor, byte 7 a copy of Motor_3's IAT. Boost on
// a Gen1 is therefore no longer decided by the generation table alone - a
// length-5 frame is evidence enough - so a patched car gets the Full (B) band
// while a stock Gen1 keeps NoBoost. Oil pressure is detected from the 8-byte
// frame whose byte 7 matches Engine_3 byte 1 (the patch's own signature) and is
// drawn as a quiet OIL row in the boost tile, scaled by the PST-F 1 transfer.
// Measured on this car: 8 bytes at 7/s, byte 4 ~99 key-off and moving under
// load, byte 7 matching 0x380 byte 1.
// 0.7.5 - The CC lamp's three states confirmed on the car (0.7.4), and the
// cruise set speed decoded: MO2_GRA_Target (0x288 byte 4, x1.28 km/h) now
// appears in the `k` dump as "set", NaN when nothing is stored. It has never
// been seen non-zero on this car, so whether the engine populates it is exactly
// what the next `k` dump settles.
// 0.7.4 - The CC lamp's three states, corrected on the car.
// 0.7.3 lit the lamp from MO2_Status_GRA and took its brightness from
// MO2_GRAregelt. Driving showed GRAregelt follows the +/- side of the rocker -
// bright with +, dark with - - and that Status_GRA is 0 while the system is
// armed with no speed stored. The lamp is now: off, dim green while the column
// switch (0x38A) is on with no speed stored, bright green once Status_GRA says a
// speed is stored. GRAregelt is still decoded for the log but drives nothing.
// 0.7.3 - Four faults from the 11 Nov drive. The CC lamp
// now mirrors the cluster's own MO2_Status_GRA instead of the stalk switch and
// MO2_GRAregelt, which lit it at random and at steady speed. A Gen1 car's boost
// tile disappears when the Haldex generation only arrives after the capability
// decision settled. The shift flash is one full-width plate instead of 80 cell
// writes, which overflowed LVGL's 32-slot invalidation buffer into an 800x480
// repaint twice per flash (~20 ms of flush, 16 fps on the car), and it covers the
// whole bar rather than only the lit part. Console `V` prints the version, build
// and running slot on demand, because the tools that watch a running dash open
// the port without resetting it and so never see the banner; `z` forces the shift
// flash for one bench check of that plate.
// 0.7.2 - The rev bar again: 80 cells drawn edge to edge (was 40
// with a 2 px gap) and the scale a setting once more - max, amber, redline and shift all
// in 100 rpm, with the tick numbers placed from the scale on save. One cell is exactly
// 100 rpm at the default 8000. The 0.7.1 clamp that held shift between amber and redline
// is gone: shift only starts the flash and marks no cell, so it is free on the scale.
// 0.7.1 - The cruise lamp's armed state, quietened: dim green
// letters on a darker plate. It was white text on 0x0a3d1f, which a 16-bit panel
// renders as (255,255,255) on (8,60,24) - brighter per pixel than the regulating
// lamp's fill, so the reminder outshouted the state worth looking at. 0.7.0's code
// with one lamp's colours changed, not a new feature.
// 0.7.0 - The drive log: a PSRAM ring that always runs (last
// 40 s, no flash touched) and a session started by hand, written to the LittleFS
// partition as CSV and downloadable from the update page. Started from SETTINGS or
// 'l'; the status bar's odometer slot reads REC while it is writing.
// 0.6.2 - the splash is a pre-rendered 800x480 JPEG decoded by the mask ROM's
// TJpgDec, with the progress rule, tagline and version drawn over it; the 80 px
// "CAN DASH" face the drawn splash needed is gone with it. The version moved to
// the bottom-right corner, where the artwork does not fight it.
// 0.6.1 - Layout C tweaks: boot splash (fixed 3 s hold, 80 px
// CAN DASH, a flat fill that tracks the hold - gradients dropped, the panel bands
// them); indicator repeaters are solid block arrows, hidden when idle and
// flashing green/off at the car's ~1.5 Hz; shift flash purple/off, not
// purple/gradient; the CC "armed" lamp is much darker; the temperatures card
// loses its title so its three rows fill the tile
// 0.6.0 - Layout C: fuel + odometer + indicator repeaters, metric/imperial,
// auto day/night, shift flash on the bar, settings page without scrolling
// 0.5.1 - internal RAM: LVGL pool and relay tables in PSRAM, [ram] reports
// 0.5.0 - CAN relay receiver, DBC tables, CAN DATA page, less HTTP

// Date and time the image was built, which is how two builds of the same
// version are told apart on the bench.
#define OHX_DASH_BUILD __DATE__ " " __TIME__

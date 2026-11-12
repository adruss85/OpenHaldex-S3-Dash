// =====================================================================
// version.h - the dash's own version
//
// The firmware had no version string at all, which matters for three
// reasons: a boot log that cannot say what is running, an OTA client with
// nothing to compare against, and a rollback with nothing to report. One
// place to change, printed at boot and in the raw page's title.
// =====================================================================
#pragma once

#define OHX_DASH_VERSION "0.8.0"  // Boost and oil pressure from the ForbesAutomotive ME7 patch
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

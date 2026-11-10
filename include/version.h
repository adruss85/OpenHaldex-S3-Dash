// =====================================================================
// version.h - the dash's own version
//
// The firmware had no version string at all, which matters for three
// reasons: a boot log that cannot say what is running, an OTA client with
// nothing to compare against, and a rollback with nothing to report. One
// place to change, printed at boot and in the raw page's title.
// =====================================================================
#pragma once

#define OHX_DASH_VERSION "0.6.2"  // The splash is a pre-rendered 800x480 JPEG decoded by
// the mask ROM's TJpgDec, with the progress rule, tagline and version drawn over
// it; the 80 px "CAN DASH" face the drawn splash needed is gone with it. The
// version moved to the bottom-right corner, where the artwork does not fight it.
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

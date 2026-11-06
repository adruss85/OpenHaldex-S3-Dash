// =====================================================================
// lv_assert_glue - makes LVGL's assert handler say where it happened
//
// LVGL's stock LV_ASSERT_HANDLER is `while(1);`, and this build has LV_USE_LOG 0,
// so an assert produces a frozen panel, a silent console and no clue which
// allocation failed. LV_ASSERT_MALLOC is the one that matters here: it fires when
// LVGL's object pool runs out, which is exactly how the CAN DATA page hanging on
// open and on close looked before the pool moved to PSRAM (0.5.1).
//
// Printed through Serial and not LVGL's log, because LVGL is the thing that has
// just failed. The panel reboots itself afterwards, so a fault in the car leaves
// a trace in whatever is capturing the console instead of a dead screen.
// =====================================================================
#include <Arduino.h>
#include <esp_debug_helpers.h>  // esp_backtrace_print
#include <esp_system.h>         // esp_restart

extern "C" void ohx_lv_assert_handler(const char *file, int line) {
  // One trace is useful, a repeating one buries it: a second assert (from the
  // reboot path, say) waits here rather than printing over the first.
  static volatile bool printed = false;
  if (printed) {
    while (true) delay(1000);
  }
  printed = true;

  Serial.printf("\n[lvgl] *** ASSERT at %s:%d ***\n", file, line);
  Serial.flush();
  esp_backtrace_print(40);
  Serial.flush();
  delay(3000);  // let the console drain before the reboot wipes the screen
  esp_restart();
}

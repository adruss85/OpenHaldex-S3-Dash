#pragma once
// =====================================================================
// logbook - a drive trace the car can hand back without a laptop
//
// Two things, deliberately separated by cost:
//
//  1. A RAM ring that always runs. One row every 200 ms in PSRAM, no flash
//     touched, no stall, nothing to switch on. It exists so "the boost reading
//     looked wrong just then" has an answer for the last 40 seconds.
//
//  2. A session, started by hand. While it runs the same rows go to
//     /littlefs/drive.csv, batched into ~1 KB writes so an erase is rare, and
//     stopping at 100 KB or ten minutes. Starting dumps the ring first, so the
//     seconds before the button was pressed are in the file too.
//
// Why not log to flash all the time: the partition is 128 KB. At 5 Hz that is
// about six minutes of data, so a rolling log would rewrite the whole
// partition every six minutes - roughly 7 000 erase cycles per block per year
// of two-hour driving days - for a trace nobody asked for. A session costs
// nothing when it is not running, and the ring covers the case it does not.
//
// Every flash write stalls both cores while the cache is off (tens of ms), so
// batches are ~1 KB and the writer gives up its turn rather than waiting for
// the HTTP side: a download in progress delays a flush by one tick, it does
// not corrupt the file or block the dashboard.
//
// A value the car does not have is an empty CSV field, never a fake 0 - the
// same rule as the glass. The header says what each column is; `src` says
// which transport the row's fast values came from (0 none, 1 ESP-NOW, 2 HTTP).
// =====================================================================
#include <Arduino.h>

#include <stddef.h>
#include <stdint.h>

class WebServer;

namespace logbook {

/// Mount the data partition and allocate the ring. Call once, early in setup().
/// Mount failure is reported and disables the feature: it does *not* format,
/// because formatting is what would destroy the log being rescued.
void begin();

/// Sample, flush and enforce the session limits. Call from loop().
void tick();

void start();
void stop();
void toggle();

bool recording();

/// Bytes in the current (or last) file.
size_t fileBytes();

/// One line for the console and the settings page.
const char *stateLine();

/// Console: state, ring depth, file size, rows lost to a busy filesystem.
void printStatus();

/// Register /log.csv and the session routes on the update page's server.
void attach(WebServer &server);

/// HTML fragment for the update page.
String logHtml();

}  // namespace logbook

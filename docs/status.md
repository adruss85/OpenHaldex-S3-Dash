Read this when you need to know what has actually been verified, on which firmware version.

# Status

<!-- status:start -->
Firmware version: 0.7.0
Flash: 1 843 374 / 1 966 080 B (93.8%)
Internal RAM (static): 59 868 / 327 680 B
Measured: 2026-11-10, build 6e87ed6
<!-- status:end -->

Measured figures are baselines for the firmware version they name, not claims
about the current build. `tools/doc_status.py` regenerates the block above from
a real `pio run` (or a saved log / a `firmware.elf`).

This is **0.7.0**: layout C (0.6.0), the boot splash (0.6.1), the splash's move
to a pre-rendered picture (0.6.2) and the drive log (0.7.0). The status below
separates what was verified in the car, what has been exercised on the bench,
and what is still unproven; measured figures carry the version they came from.

**0.7.0 is compiled, not exercised.** The drive log — the LittleFS mount, 5 Hz
sampling against a real frame rate, the flash stall and the phone download — has
not been run on a board. See [Drive logging](logging.md).

**In the car** (0.5.x):

* Panel, touch and the PCA9557 expander all come up; tearing and jitter are
  dealt with (two frame buffers, DIRECT mode, push only on
  `lv_display_flush_is_last()`, PLL_D2 clock, 24-line bounce buffer, 16 MHz).
* Both transports are live: HTTP at 1 Hz and the ESP-NOW broadcast at ~10 Hz,
  with the source printed and switchable.
* Stale behaviour, the settings page, the touch mapping and all five redraw
  groups were measured on the bench.
* The raw telemetry page renders and every field matches what the controller
  reports; the BRAKE and HAND lamps light from the source that actually moves on
  this car; the capability decision settles to the NoBoost band; and **the mode
  switch works** — a tap on the HALDEX panel, and `m` on the console, each got
  `ok` back from the controller in ~60 ms.
* **Over the air**: a real 1.7 MB upload from a browser wrote the other slot,
  rebooted into it *on trial*, and stood the trial down once the panel had
  held for 20 s. The fall-back was then exercised for real — a trial told to
  fail handed the car back to the previous slot in both directions
  (app0 → app1 → app0). A file that is not a firmware image is refused with
  HTTP 500 and nothing written. Confirmed from a phone on the controller's own
  access point as well.

**On the bench** (panel attached, controller powered off, so every relay-fed
value correctly reads `--`). Figures are 0.6.0 unless the line names a later
version:

* The **boot splash** (0.6.1, re-done in 0.6.2): the picture decodes in 416 ms and
  the panel is never blank; the rule fills over a fixed 3 s and the version is on
  the glass, bottom-right. The hold's clock starts *after* the decode, so the rule
  begins empty - counting the decode showed it already ~13 % filled on its first
  frame and dropped the splash 0.4 s early. Its storage cost is in
  [the splash section](display-and-memory.md#the-boot-splashs-picture).
* Flashed over USB and left running for minutes: the fuel row, the two indicator
  repeaters, the odometer in the status bar, the day/night backlight and the
  rewritten settings page all draw as intended, and the settings page no longer
  scrolls.
* The settings blob was **migrated in place, v4 → v5** — SSID, brightness, the
  metric flag and the static-IP flag all survived the update (read back out of the
  NVS partition to confirm). The stored shift point was 5800 rpm, below the amber
  zone, so the new `amber ≤ shift ≤ redline` clamp raised it to 6000 on load.
* Render rate is **61.8 fps** idle (`0.74 ms/loop, 1.19 ms/busy of a 16.2 ms
  period`); while the station is still hunting for the controller's access point it
  settles at 32 fps, which is the WiFi buffers competing for PSRAM as described in
  [Display tuning](display-and-memory.md#display-tuning).
* Internal RAM holds at **163.5 KB free, 162.9 KB lowest**, with LVGL's pool at
  46 % of its 256 KB. See [Internal RAM](display-and-memory.md#internal-ram).

**Still to be exercised in the car:** the CAN relay and the CAN DATA page
([test procedure](can-relay-test.md)) — which now also covers the fuel,
odometer, indicator and 58s values, since those arrive the same way; and the value
scaling for `boost` — none of the reference car's data has come from a boost source
yet, since a Gen1 Haldex has no boost frame at all, and the Full band itself
remains unverified. One unexplained chip reset was seen 8 s after the first
post-update boot while the station was re-associating (no panic, clean ROM banner);
180 s of idle running afterwards showed no repeats, and the boot banner now prints
the reason so it can be caught if it happens again.

Next: [TODO.md](../TODO.md) — what is deliberately not built yet, and why.

Build result, measured on 0.7.0:

```
RAM:   59 868 / 327 680 B   (18.3%)   <- LVGL's pool and the relay tables are in PSRAM
Flash: 1 843 290 / 1 966 080 B (93.8%) <- of the 1.875 MB OTA slot
```

The RAM line came down from 0.5.0's 50.1 % (164 060 B) when the pool and the relay
tables moved to PSRAM (see [Internal RAM](display-and-memory.md#internal-ram)); 0.7.0
adds 1.3 KB of it (the 1 KB log batch buffer, the open `File` and the mutex), and
the 19 KB ring lives in PSRAM. The flash line is within ~120 KB of the OTA slot's
limit, which is the number to watch if another page or table is added; 28 KB of it
is the splash's JPEG, bought back by dropping the 80 px "CAN DASH" face the drawn
splash needed (see [the splash section](display-and-memory.md#the-boot-splashs-picture)).
The drive log cost 38 KB of flash, which is the largest single addition since layout
C and the reason the headroom is now the thing to check before the next feature.

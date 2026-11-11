Read this when you need to know what has actually been verified, on which firmware version.

# Status

<!-- status:start -->
Firmware version: 0.7.2
Flash: 1 844 758 / 1 966 080 B (93.8%)
Internal RAM (static): 60 308 / 327 680 B
Measured: 2026-11-10, build a84c801
<!-- status:end -->

Measured figures are baselines for the firmware version they name, not claims
about the current build. `tools/doc_status.py` regenerates the block above from
a real `pio run` (or a saved log / a `firmware.elf`).

This is **0.7.2**: layout C (0.6.0), the boot splash (0.6.1), the splash's move
to a pre-rendered picture (0.6.2), the drive log (0.7.0), a quieter cruise lamp
(0.7.1) and the rev bar with a settable scale (0.7.2). The status below separates
what was verified in the car, what has been exercised on the bench, and what is
still unproven; measured figures carry the version they came from.

**0.7.0 has been run on the bench and against a live controller, not driven.**
The drive log works end to end: the blank partition formats itself, a session
starts, writes and stops, the CSV reads back correctly out of a raw flash dump —
including a session with the car powered up, where the rows carry `src=1` and
real values — and the HTTP controls were exercised from a phone on the
controller's access point (download, start, stop, delete). What is still unproven
is a trace taken while the car is actually moving. See
[Drive logging](logging.md) for the list.

**0.7.1 changes one lamp's colours and nothing else** — the cruise lamp's armed
plate and lettering, plus the version string. Every 0.7.0 observation below
therefore still describes this image, and the lamp itself was checked on the
glass in 0.7.1.

**0.7.2 changes the rev bar and the rpm thresholds and nothing else** — 80 cells
drawn edge to edge in place of 40 with gaps, and the scale settable again, with
max, amber, redline and shift all in 100 rpm. It was flashed over USB and booted
clean on the bench, with the bar and the new settings rows looked at on the
glass. No controller was in range, so the bar sat at `--`: neither the moving
edge against live rpm nor a shorter scale's tick placement has been watched
there yet. Every earlier observation still describes the rest of the image.

**0.7.3 is the four faults from the 11 Nov drive** (CC lamp source, late Haldex
generation, shift-flash cost and its width) plus console `V`. Verified on the
bench only, from a parked car with no controller in range: it boots clean, `V`
answers, `z` + `d` show the flash plate covering all 800 px with no gaps, and the
alternating plate costs 1.5–2.1 ms of flush at 57–60 fps against 0.00 ms at 65 fps
idle. What has **not** been on a car: which bit was actually lying in the CC lamp,
the late-generation revision itself, and the cue against live rpm. See
[TODO.md](../TODO.md) §2.6.

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
* The **drive log** (0.7.0), flashed over USB with no controller in range: the
  erased partition is formatted once at boot and mounts; the PSRAM ring samples
  at 5 Hz; a session writes its header, 19 rows of pre-trigger history and then
  1544 B in ten seconds with nothing dropped; `F` formats it back to a clean
  8192/131072 B. The file dumped straight out of flash holds the header and one
  row per sample with empty fields rather than zeros. The dashboard held 68 fps
  throughout.
* The **drive log against the live controller** (0.7.0), car powered up with the
  engine off: the dash joined the controller's access point at −59…−68 dBm,
  ESP-NOW ran at 7–9 Hz and the CAN relay at 104–124 frames/s over 17 ids. A
  session wrote 122 rows of pre-trigger and closed at 10 057 B with **0 rows
  lost**, at 66–68 fps. The CSV out of flash carries real values with `src=1`
  and an empty `boost_mbar` (Gen1). Measured row: 42.0 B including the newline
  against a 126 B header, samples every 199–316 ms — so 100 KB is about eight
  minutes, and that cap stops the session before the clock does.
* The **update page** (0.7.0) was exercised from a phone on the controller's
  access point: **Download drive.csv**, **Start log**, **Stop log** and
  **Delete** all worked. The **Format partition** button was correctly absent —
  it is only drawn when the partition holds no readable filesystem — and that
  path was checked on the console instead (`F`, above).
* The **cruise lamp** (0.7.1) was looked at on the glass with the car reporting
  `cruise main 1 regulating 0`, i.e. in the armed state that was changed: dim
  green letters on a dark plate, with regulating still the only lit block.
* The **settings page** (0.7.0) was checked on the glass with `d` (the ASCII
  frame dump), which is how the two defects were found: the connection status
  overlapped the HOST field, and its warning and OTA lines were being clipped
  mid-sentence by a 340 px box. It is now a full-screen page rather than a card,
  the long lines wrap, and every string was measured against the font's advance
  widths before being placed.
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

Build result, measured on 0.7.1:

```
RAM:   59 868 / 327 680 B   (18.3%)   <- LVGL's pool and the relay tables are in PSRAM
Flash: 1 845 186 / 1 966 080 B (93.9%) <- of the 1.875 MB OTA slot
```

The RAM line came down from 0.5.0's 50.1 % (164 060 B) when the pool and the relay
tables moved to PSRAM (see [Internal RAM](display-and-memory.md#internal-ram)); 0.7.0
adds 1.3 KB of it (the 1 KB log batch buffer, the open `File` and the mutex), and
the 19 KB ring lives in PSRAM. The flash line is within ~120 KB of the OTA slot's
limit, which is the number to watch if another page or table is added; 28 KB of it
is the splash's JPEG, bought back by dropping the 80 px "CAN DASH" face the drawn
splash needed (see [the splash section](display-and-memory.md#the-boot-splashs-picture)).
The drive log cost 40 KB of flash, which is the largest single addition since layout
C and the reason the headroom is now the thing to check before the next feature.

Read this when you need to know what has actually been verified, on which firmware version.

# Status

<!-- status:start -->
Firmware version: 0.8.2
Flash: 1 526 670 / 1 966 080 B (77.7%)
Internal RAM (static): 60 268 / 327 680 B
Measured: 2026-11-12, build abd51b1
<!-- status:end -->

Measured figures are baselines for the firmware version they name, not claims
about the current build. `tools/doc_status.py` regenerates the block above from
a real `pio run` (or a saved log / a `firmware.elf`).

This is **0.8.2**: layout C (0.6.0), the boot splash (0.6.1), the splash's move
to a pre-rendered picture (0.6.2), the drive log (0.7.0), a quieter cruise lamp
(0.7.1), the rev bar with a settable scale (0.7.2), the four faults from the 11
Nov drive (0.7.3), the CC lamp's three states (0.7.4), the cruise set speed in
the `k` dump (0.7.5), boost plus oil pressure on the ForbesAutomotive ME7
patch (0.8.0), the binary-size audit with the night-brightness floor lowered
(0.8.1), and relay reception from any CANN sender with a bench listen-channel
pin (0.8.2). The status below separates what was verified in the car, what has
been exercised on the bench, and what is still unproven; measured figures carry
the version they came from.

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
idle. It was driven that evening; what that showed is the 0.7.4 note below.

**0.7.4 corrects the CC lamp's three states, from the evening drive on 11 Nov**
(the 0.7.3 image). That drive proved two things on the car: the late Haldex
generation revision fires (`[cap] gen 1 | boost no (table) | … | NoBoost (A)`),
and the lamp's brightness was following `MO2_GRAregelt`, which tracks the +/−
side of the rocker — bright with **+**, dark with **−** — while arming the system
left the lamp dark because `MO2_Status_GRA` is 0 until a speed is stored. The
lamp is now off, dim green while the column switch is on with no speed stored,
bright green once `MO2_Status_GRA` says a speed is stored; `MO2_GRAregelt` is
decoded for the log but drives nothing.

**The CC tell-tale is confirmed working on the car** (0.7.4), in a second session
on 11 Nov: all four states behave — off with cruise off, dim green armed with no
speed stored, bright green whether the speed was set or nudged with **+** *or*
**−**. The coolant warn and critical thresholds were exercised in that session too
and do what they say. Still unverified in the car: the shift cue against live rpm;
and the render-rate regression under live CAN traffic (~30 fps, 0.7.1 measured 53
at the same packet rate) — [TODO.md](../TODO.md) §2.6.

**0.7.5 adds the cruise set speed to the `k` dump.** `MO2_GRA_Target` (0x288 byte
4, ×1.28 km/h) is the set speed, in the same frame as the lamp bit; `k` now prints
it as `set`, `NaN` (drawn `--`) when nothing is stored. No stored log has ever
contained it — no raw CAN dump was ever taken — so whether this engine populates
the byte is what the next `k` dump with a speed set settles. The `k` line for
cruise is now `main / status / armed / engaged / regelt / set`.

**0.8.0 adds boost and oil pressure for cars running the ForbesAutomotive ME7
patch**, and was verified on that car. The patch lengthens `Motor_7` (0x588)
from three bytes to eight, with the boost sensor in byte 4 and the oil-pressure
ADC in bytes 5–6. On the reference car, with the patch flashed and the rear O2
still connected, the board reported:

```
[cap] gen 1 | boost yes (motor_7) | oil pressure yes (motor_7) | supply no
(detected) | haldex temps no (detected) | steering yes (detected) | Full (B) | 20 pkts
[chassis] engine_7 manifold 0.98 bar abs | oil pressure -0.1 bar
```

That is the whole path proved in the car: the 8-byte frame was on the bus at
7/s, byte 4 read ~99 key-off, byte 7 matched `Motor_3` byte 1, the Full band was
built, the OIL row appeared, and `k` decoded both values. The negative oil
figure is expected with the rear O2 still connected — the row is deliberately
not clamped — and the honest zero arrives when the PST-F 1 is wired. Idle render
held ~72 fps with internal RAM at 152.8 KB free / 151.3 KB lowest.

**0.8.1 is a size reduction with nothing changed on the glass or the wire.**
The stock montserrat_22/28/48 faces became subsets of those very faces —
generated from the same Montserrat-Medium.ttf with the options LVGL itself
used, verified byte-identical glyph for glyph (`tools/check_font_subsets.py`),
with the stock line metrics pinned so label placement is untouched.
`lv_conf.h` now compiles out every LVGL widget, layout engine and
colour-format blend path the dash never uses, and the HTTP pollers bind a
plain `NetworkClient` so the mbedTLS SSL client is no longer linked for a
controller that is always plain http. Measured against a build of the
unmodified 0.8.0 tree: 1,847,186 → 1,525,962 B of the 1,966,080 B OTA slot
(94.0% → 77.6%), static RAM 60,412 → 60,196 B. The per-stage deltas, the
disabled-feature list and the fidelity evidence are in
[the size brief](binary-size-brief.md); the font inventory is in
[display-and-memory.md](display-and-memory.md#fonts). Measurement note: the
stage-4 row is the incremental build; every clean build of this tree — with
or without the floor constants — measures 1,525,962 B of sections (the .bin
image can carry a 32-byte alignment pad on top).

The image was then flashed over USB and verified on the bench on 12 Nov 2026
— boot and `V` (0.8.1, app0 confirmed, fallback ready), the splash at 411 ms,
the dashboard and settings pages on glass at two dump thresholds with the
subset fonts (big `--`, captions, the 28 px `SETTINGS` heading), `o`, `e`,
`x` and `k`, 62–67 fps idle and internal RAM at 159.9 KB free / 159.2 KB
lowest, the same figures 0.8.0 gave on this bench — and then driven in the
car the same day: the dashboard live on both transports (mode names and lock
% with real data), HTTP polling against the controller, ESP-NOW packets, and
the night-brightness floor confirmed with the wheel down (below). Still not
re-verified: the render rate under radio traffic was not re-measured, OTA
upload/trial/fall-back was not re-exercised (unchanged code since 0.8.0), and
the units toggles were not re-checked on the glass.

**0.8.1 also lowers the night-brightness floor.** 0.6.0's night curve
(`20 % + 45 % × 58s/100`) never went below 23 % with the dimmer wheel at its
bottom stop (58s 7 %), which was still too bright in the car; it is now
`5 % + 60 % × 58s/100` — **9 %** at the bottom stop, the ~64 % ceiling
unchanged, day untouched. Confirmed in the car on 12 Nov 2026.

**0.8.2 accepts CAN-relay packets from any sender, and the bench proved the
whole path against a second device** (12 Nov 2026): the ESP32-bodycan devkit
broadcasting synthetic comfort-bus frames (0x570 / 0x470 / 0x578) on channel
11, the dash hearing them with no controller on the air (`[stats] relay ...
1 sender(s)`, the listen channel pinned to 11 after every failed join
attempt) and the `C` snapshot listing all three ids with DBC names, decoding
`BatteryVoltage_VehicleBattery` **14.00 V** from the fabricated 0xB4 byte.
Recorded honestly: with no access point to join, reception is bursty — the
join loop hops channels ~8 s of every ~13 s cycle and about half the
sender's packets are missed (`air lost` ~44%); staying parked on the pinned
channel between attempts is the named next lever. Not verified here: the C6
path itself (no controller on this bench), and the two-sender interleave
(only one sender was transmitting).

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
odometer, indicator and 58s values, since those arrive the same way; the Full
band and boost decode on a Gen4/5 car, where both come from the generation table
and the controller's own source rather than the 0.8.0 patch; and oil pressure
with the PST-F 1 actually wired. One unexplained chip reset was seen 8 s after
the first post-update boot while the station was re-associating (no panic, clean
ROM banner); 180 s of idle running afterwards showed no repeats, and the boot
banner now prints the reason so it can be caught if it happens again.

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
(0.8.1 bought the headroom back: 440,086 B free of the slot, from the font
subsets, the pruned LVGL configuration and a plain-HTTP client — see
[the size brief](binary-size-brief.md).)

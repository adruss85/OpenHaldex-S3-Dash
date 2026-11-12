# TODO

What is deliberately not built, not verified, or not decided. Kept separate
from the README so the README can describe what *is*, and this can describe
what *might be*, without either one lying.

Ordering inside each group is roughly "what would hurt most if it bit us"
downwards.

---

## 1. Waiting on a decision

### 1.1 Rescue access point
If the dash's SSID or password is set wrongly, it cannot join the controller's
access point — so it cannot fetch data, and the over-the-air update page is
unreachable. USB is the only way back.

The fix: if the station has failed to join for ~3 minutes, raise our own access
point (`ohx-dash-rescue`), put the address on the glass, and serve the same
update page plus a way to fix the stored credentials. Roughly a softAP, a
small DHCP server and one more page; maybe 60–80 KB of flash.

**Deferred, not rejected.** An edge case you have to walk into on purpose.
Worth doing if this ever ships to someone who cannot reach the USB port.

### 1.2 Authenticate the update page
Right now anyone on the controller's access point can flash the dash. That is
a different risk class from the mode switch — a stranger changing the Haldex
map is annoying, a stranger writing your firmware is not — and the open access
point is broadcast, so "who is on it" is everyone in range.

Cheapest fix: a 6-digit code in the URL, shown on the dash's SETTINGS page next
to the address. Not a login, just enough that the page cannot be *found* by a
casual passer-by. Alternatives: leave it (consistent with treating an open
access point as a warning, which is the current stance on the mode switch), or
require the AP password as a token.

### 1.3 ~~Build a `firmware.factory.bin` release asset too?~~ — **done, 0.8.0**
Releases carried only `firmware.bin` (what the update page wants). From 0.8.0
both files ship: `...-firmware.bin` for OTA and `...-factory.bin`, the merged
image written at `0x0`, for a board that has never been flashed or whose app
slots have been scrambled.

### 1.4 Automatic FWD while cruise control is active — **paused**
The **read-only half is built**: `chassis.cpp` decodes `MO2_Status_GRA` (0x288,
bits 22-23, the cluster's own cruise lamp: 1 activated, 2 overdriven) and the
column switch `GRA_MainSwitch` (0x38A, bit 8), and they drive the **CC** lamp:
off, dark green while the switch is on with no speed stored (armed), bright green
once `MO2_Status_GRA` says a speed is stored (engaged). `MO2_GRAregelt` (0x288,
bit 26) is decoded for the log but does not drive the lamp: on the car it read 1
with the + side of the rocker and 0 with the −, so it tracked a nudge rather than
a held speed. That is what `can_relay.cpp`'s `cruise` value carries.

The half that **acts is not built**: nothing switches the Haldex mode to FWD when
cruise engages, and there is no setting for it. It would be the only feature here
that changes how the car puts power down without the driver asking, so it was
paused by decision — and the safety requirements were agreed at that point, which
is why it is a short job when it is picked up rather than a fresh design:

* opt-in setting, off by default, and it says on the glass when it is on;
* 2 s of debounce to enter *and* to leave;
* capture the mode on entry and restore exactly that — never "cycle to the next";
* any manual mode change wins, and switches it off for the rest of the ignition
  cycle;
* every change announced on the glass and on the console.

The signal evidence, the reasoning and the full requirements are in
[`docs/upgrades.md`](docs/upgrades.md) §13. It starts at the debounce, not at the
research.

### 1.5 adc2can as a relay sender — acceptance built (0.8.2), product decision still open
The Forbes Automotive **adc2can** (8× 0–5 V sensors to CAN) shares the CAN relay
wire format with OpenHaldex by design: `cannow_proto.h` here is one of three
identical copies (see its header comment) and already names `CANN_K_ADC2CAN`.
So `can_relay.cpp` decodes its relayed car-bus frames and 29-bit sensor
frames today, and the CAN DATA page can show its sensors from a DBC for the
`0AD2C000` base id. No WiFi join is needed either — the relay is an unpaired
ESP-NOW broadcast and both sides sit on the rig's channel.

What changed in 0.8.2, and what is still deliberate:

* **Sender acceptance was OpenHaldex-coupled — now it is not.** `ohx_now.cpp`
  used to learn "its controller" from an `OhxStatus` packet and drop every CANN
  packet that was not from that MAC — the `kind` byte was not even consulted.
  An adc2can or body-CAN box never sends `OhxStatus`, so it was filtered out.
  From 0.8.2 relay packets are accepted from **any** sender, by explicit owner
  decision: the direction is a pseudo wireless CAN network — several ESP32
  devices each broadcasting one thing, the dash a silent listener in the
  middle. The old gate's intent — stopping the dash hearing a stranger's car
  in a paddock — is now a documented tradeoff, not a protection
  ([architecture.md](docs/architecture.md) §The CAN relay): any sender on the
  channel can paint the CAN DATA page and fill relay-carried values your own
  senders are not carrying. The command path stays OpenHaldex-keyed, so
  nothing in the noise floor can steer the car.
* **No Haldex-shaped feed without a controller.** The Full (F) band, the mode
  tile and `/api/dashboard` polling all assume the C6; on an adc2can-only car
  the main band would stay `--`. What a dash without OpenHaldex *is* — the CAN
  DATA page plus gauges, or nothing beyond raw/CAN — is a product question,
  since can2gauge already covers the display-only road-car use well.
* **No command path, and none to build.** adc2can is one-way over the air; the
  dash would be display-only, which matches the house rule but means the mode
  tile must hide rather than transmit into silence.

Perspective: the interesting version is **both on one car** — adc2can sensors
alongside OpenHaldex data where the controller cannot help (Gen1 has no oil
temp/pressure; a race car wants exactly those). Making the dash a general
can2gauge replacement is not the goal. The acceptance gate is done; the decode
side is free.

---

### 1.6 Sender identification and liveness — who is on the air, who dropped out

The direction is a pseudo wireless CAN network: several ESP32 devices each
broadcasting one thing (the controller's chassis relay, the body-CAN box,
later an adc2can), with the dash a silent listener in the middle — 0.8.2
accepts them all. Today the dash *counts* them (`[stats] relay ... N sender(s)`)
but cannot *show* them: the CAN DATA page has no idea which sender carries
which id, and a sender that drops out is visible only as its ids going stale
and the `air lost` counter creeping up — easy to misread as radio loss (seen
on the bench, 12 Nov 2026).

Wanted: a user-friendly way to identify senders and see liveness at a glance
— which are transmitting now, which has gone quiet, ideally by a name the
owner chose rather than a MAC.

What exists to build on:

* `can_relay.cpp` tracks per-sender seq under its own lock (8 slots) and
  exposes `Stats.senders`; the source MACs are in that table but not surfaced.
* The CANN `kind` byte (`CANN_K_*`) rides in every packet header and is
  ignored today; it is the natural *type* label ("the Haldex controller", "a
  body/sensor box") with no pairing at all. A friendly *name* per MAC would
  need NVS (append-only, config.h rules) or an adoption key, like the
  controller's own.
* Per-sender last-heard age is derivable where the packet is accepted.

Shape to weigh, cheapest first — this page shares PSRAM bandwidth with the
panel, so glass cost is the constraint:

* console first: `C` (and the `[stats] relay` line) print one line per sender —
  MAC, kind, packets, f/s, age. No glass cost, immediate bench and road value;
* then a compact sender row on the CAN DATA page (or the raw page): chips that
  grey out when quiet, e.g. `C6 ● bodycan ●`, inside the existing redraw
  groups (mask 1..5) and with no new polling;
* NVS-remembered labels per MAC, adopted like the controller is, as the last
  piece — the only one that needs a settings decision.

Do not let it become per-frame sender attribution: the wire format has no
sender field per frame, and per-id arbitration (first heard keeps an id until
it goes quiet) is the contract the whole network runs on.

---

### 1.7 One shared channel for the pseudo wireless CAN network — direction chosen, architecture deferred

The owner set the direction on 12 Nov 2026 and deliberately stopped before
any code, to think the architecture through properly. The direction: the
pseudo network's channel becomes **1 unless a C6 is on the air, then the
C6's channel** — senders that hear `OHX_STATUS` follow the `channel` field
it already carries (`ohx_proto.h:132`, added for exactly this) and revert to
1 when the C6 goes quiet. The dash side of that is a one-line change
(`OHX_NOW_IDLE_CHANNEL` 11→1 in `telemetry.cpp`, its comment block
rewritten); the sender side is the bodycan's follow upgrade (its TODO 3.4).
Nothing is built. This entry is the problem statement for that pass.

Why this is a decision and not a config knob: every device here has one
radio, so the whole network shares a single channel, and today that channel
is agreed in three places that only match through per-rig edits — the C6's
AP (whatever its configuration or home join makes it), this dash's idle pin
(a 0.8.2 bench patch, currently 11), and each sender's own constant (bodycan
11, adc2can 1). The family default is channel 1, recorded twice:
`ohx_proto.h` ("in the car both APs default to channel 1") and measured on
the original ESP-NOW bench day ([esp-now-plan.md](docs/esp-now-plan.md) —
the C6, the same MAC this dash remembers, broadcasting on channel 1 with the
station riding it). This rig's 11 is the anomaly.

Open questions for the architecture pass:

* **Why is this C6 on 11?** Its own configuration, or the hazard the shared
  header itself names — "if either joins a home network" (ESP32 is one
  radio: an AP+STA device's AP is dragged onto the home network's channel,
  and the strong home APs here sit exactly on 11). First step: the C6's web
  UI — is there an AP channel setting, and is it joining the home WiFi? If
  the C6 cannot hold 1, the follow design absorbs it; that is the point of
  the choice.
* **Where the follow logic lives.** `cannow_tx.h` is a verbatim shared copy
  and stays untouched (rule 1); the follow is sender-local glue around it.
  The bodycan needs a receive path it does not have today: an ESP-NOW recv
  callback, a verbatim copy of `ohx_proto.h`, and follow/revert glue.
* **Revert policy.** How quiet is "quiet" (a timeout on no `OHX_STATUS`),
  and the behaviour on a rig with two C6s — the dash already carries that
  ambiguity; a sender would follow the first or strongest heard.
* **adc2can cannot follow** (Forbes design, fixed channel 1): on a rig whose
  C6 sits elsewhere, the joined dash is deaf to it in-car. An upstream
  report, or a per-rig constant for that one device — decision to make.
* **The flip is coordinated or nothing.** This pin, the bodycan's default
  and anything the C6 needs land in one bench session and are verified
  together — a half-moved fleet is worse than today. Verify with a pinned
  steady-state loss measurement on 1 vs 11: every bench number so far is
  polluted by join-attempt hopping, and on this rig channel 1 is the
  measurably quieter half (strong home APs -48 dBm on 11, their ch1 copies
  -75..-79 dBm).
* **What this decision does not touch.** The bursty ~50% bench reception is
  join hopping, not channel choice — the stronger fix there is remembering
  the C6's BSSID and channel and joining directly (`WiFi.begin` takes
  both), which belongs in the same architecture pass. The wire format and
  the 6 Mbit/s rate are channel-independent.

---

## 2. Verification gaps

Things built and believed, but not yet proven on the right hardware.

### 2.1 Other Haldex generations — **needs testers**
Everything layout-related was decided from a **Gen1** car, now running the
ForbesAutomotive ME7 patch (0.8.0). That car fills BOOST from the patch and OIL
pressure from its ADC bytes, so the **Full (B)** band has been rendered on a
car and its boost decode has been seen moving — but only through this patch:

* the Full band has **not** been seen on a Gen4/5 car, where it comes from the
  generation table and the controller's own boost source rather than a patched
  `Motor_7`;
* the packet's `boost` (documented as *mbar above ambient*) has still only been
  cross-checked against the patch's byte 4 on this one engine;
* Gen2/4/50/52 differences still unknown: which generations fill `slip[4]`,
  and whether steering arrives both ways as it does on Gen1. (`oilTemp` is
  settled: the packet's is Haldex oil, Gen4 only, and the OIL tile no longer
  uses it. HTTP oil is engine oil from the Kombi.)
* oil pressure is offered only where the patch's signature is present, so a
  stock car of any generation is expected to read a dash there — unverified,
  because no stock car has been flashed with 0.8.0.

The raw telemetry page (`x`) exists for exactly this: it shows every field
raw, so a tester can report back without a laptop and without this agent.

### 2.2 The CAN relay in the car
Built and believed, not yet measured. What it costs in render rate, how much it
loses in the air at this car's −76 to −86 dBm (the controller sends at 6 Mbit/s
while relaying, which has less margin than the 1 Mbit/s the link was measured at),
and which relay setting to recommend. MQB coolant and oil from `Motor_07` have
never been on a car through this path. The whole test is in
[`docs/can-relay-test.md`](docs/can-relay-test.md).

The bench half of this is now done (0.6.0): with no controller in range the panel
runs at 61.8 fps and internal RAM holds at 163 KB free, so the PSRAM moves are no
longer the worry they were. What the *relay* costs — render rate and loss with the frames
actually arriving — can only be measured on the car, and the procedure covers the
fuel, odometer, indicator and 58s values too, since those arrive the same way.

### 2.3 A wrong *project* image is accepted
The IDF refuses an image for another chip (verified in its source:
`mismatch chip ID, expected %d, found %d`, on the same verify path
`Update.end()` uses). It does **not** care whose project the image is, so a
valid ESP32-S3 image that is not this dashboard would be written, would boot,
and would never stand the trial down — leaving a trial armed in NVS for the
next real boot to trip over.

The fix, in `handleUploadDone()`: after `Update.end(true)`, read the app
descriptor out of the target partition (`esp_partition_read` at
`16 + 8 * segment_count`, rounded up) and compare `project_name` with this
project. On a mismatch, point the boot partition back at the running slot and
report it. The running image is untouched either way.

**Not done because it cannot be tested from here** — the update page is only
reachable from the controller's access point, and the controller's access
point is out of range of the development machine. Until then the README says
plainly: take the file from this repository's releases and nothing else.

### 2.4 The unexplained reset
One reset, 8 s after the first post-update boot, while the station was
re-associating. No panic, a clean ROM banner, no repeats in 180 s of idle
running afterwards, and the trial logic handled it correctly in passing.

Leading hypothesis: a supply sag. The panel and the WiFi share a rail with
very little margin on a USB port, and the reset landed in the middle of
association retries. The other candidate has since been removed: until 0.4.2 a
rejoin shut the WiFi driver down with ESP-NOW still running on top of it. The boot banner now prints `reset : <reason>`
(power-on / software / PANIC / watchdog / BROWNOUT), so the next occurrence
answers the question instead of prompting another guess. Watch it on the car's
supply, which is a different animal.

### 2.5 Link margin
The ESP-NOW link sits at −76 to −86 dBm in the car and delivers 6–9 Hz rather
than the full 10, with occasional gaps. Nothing is wrong with the code —
broadcast has no retransmission — but the dash would look better with the S3
moved ~20 cm away from whatever it is currently sitting next to.

Worth trying when someone has the car apart: a position that gets to −70 dBm,
and measuring `[stats] esp-now … Hz` before and after.

### 2.6 The drive-report fixes (lamp and band confirmed on the car)
Reported from the car on 11 Nov 2026. Two sessions on 11 Nov: the 0.7.3 image
proved the boost-tile revision fires and showed the CC lamp's +/− asymmetry; the
0.7.4 image, in a second session, confirmed all four lamp states and the coolant
warn/crit. Still unverified anywhere: the shift cue against live rpm, and the
render-rate regression below.

* **CC lamp lighting at random — confirmed fixed on the car (0.7.4).** The lamp
  has three states from two facts: off, dim while the column switch is on with no
  speed stored (armed), bright once `MO2_Status_GRA` says a speed is stored
  (engaged). `MO2_GRAregelt` no longer drives anything on the glass. The 0.7.3
  drive had shown the fault it was hiding: with a speed set, the **+** side of the
  rocker lit the lamp bright and the **−** side left it dark, because GRAregelt
  tracks the nudge, not the held speed; and arming the system left it dark because
  `MO2_Status_GRA` is 0 until a speed is stored. All four states behave on the car.
* **Cruise set speed.** `MO2_GRA_Target` (0x288 byte 4, ×1.28 km/h) is the set
  speed, in the same frame as the lamp bit. It is already on the CAN DATA page via
  the DBC and is now in the `k` dump as `set` (`NaN` when nothing is stored). No
  stored log has ever contained it, because no raw CAN dump (`C`) was ever taken —
  so whether this engine populates the byte is exactly what one `k` dump with a
  speed set settles. Not shown on the glass yet.
* **Boost tile on a Gen1 car.** `cap::poll()` now revises the band once if the
  Haldex generation only arrives after the decision settled by timeout. A cold
  boot with the controller off, then `[cap] revised after late generation`.
* **Slowdown while the bar flashes.** The flash is one full-width plate instead
  of one style write per lit cell (80 of them on the 0.7.2 bar, which overflowed
  LVGL's 32-slot invalidation buffer into a whole-screen repaint), and
  `LV_INV_BUF_SIZE` is 64. **Open, and wider than the cue:** the 0.7.3 drive
  measured 33 fps mean / 31 median across the whole run with 8.9 ms of flush per
  frame, at 709-2703 rpm, i.e. with the bar nowhere near the shift point. 0.7.1
  measured 53 fps at the same relay rate. The cost vanishes within 5 s of live
  packets stopping (63-67 fps, 0.00 ms flush, while `e`/`k` still print), so it
  is repaint area driven by data, not the serial console. Next: park with the
  controller talking, `9` then `1`/`2`/`3`/`4`/`5` to name the redraw group.
* **Flash should span the full width.** Confirmed by `d` on the bench: the plate
  is solid across all 120 columns in both halves.

---

## 3. Requested, and worth doing

### 3.1 Display scaling / other panel sizes
Currently out of scope, and worth stating precisely why: `theme.h` is a
list of absolute pixel positions derived from the browser page's geometry
(`u = min(w/100, h/56)`, 8 px at 800×480). Nothing scales.

The **5.0" CrowPanel** is the same 800×480, so it should fit as-is and needs
only a board variant — different pins, touch controller and possibly less
PSRAM, which matters more than the resolution: the two 800×480×16bpp
framebuffers are 1.5 MB and cannot live in internal RAM. Check the 5" board's
PSRAM *before* anything else.

For a genuinely different resolution, the honest options are (a) express
`theme.h` in terms of a scale factor and multiply, (b) convert the layout to
LVGL flex/grid, or (c) accept per-resolution variants. (a) is the smallest
change and keeps the "one file holds the geometry" property.

### 3.2 More of the controller's own controls
The mode switch is the thin end of a wedge. The same signed-command path
already carries learn start/cancel/clear, generation, and controller on/off
(see `ohx_proto.h`). Each one is a decision, not a port: generation and learn
need the car stopped, and "controller off" while driving is a conversation.

### 3.3 Things that arrive for free and have nowhere to go
Per-wheel slip (`slip[4]`), steering angle (packet-only on Gen1), supply
voltage, Haldex clutch/oil/module temperatures. All decoded, all shown on the
raw page, none on the dashboard — because this car cannot fill them. Space
them once a Gen4/5 tester exists (see 2.1).

### 3.4 Smaller polish
* Scale up the big SPEED number — noted as not urgent, since speed matters
  least on a racecar-style dash, but it is inconsistent with rpm.
* A QR code on the SETTINGS page for the update URL, to kill the last bit of
  typing on a phone.

---

## 4. Technical debt

### 4.1 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`
The IDF's own rollback is the right home for the update trial. Enabling it
means `custom_sdkconfig`, which forces a full framework rebuild against the
shared core directory — measured, and it does not currently succeed for this
platform release (`libsodium` fails to compile once the regenerated sdkconfig
drops its defines). Revisit if upstream fixes the rebuild; the application's
trial can be deleted the same day.

The application-level version cannot catch a crash that happens before our
code runs. The IDF's version can.

### 4.2 The `coredump` partition is unused
It exists in the partition table and nothing writes to it. Enabling core
dumps, and a way to read one back out (over the update page would be neat),
would turn a field crash from "it rebooted" into a stack trace.

### 4.3 Render performance
The loop is PSRAM-bandwidth bound, not HTTP bound (see
[`docs/display-and-memory.md`](docs/display-and-memory.md)), so the
rate follows the radio: 16–35 fps while a phone or the controller is pushing
traffic through WiFi, and **61.8 fps on the bench with nothing in range** (0.6.0,
which also cut the polling to a single cross-check request a second). What it does
in the car with the relay arriving has not been measured. The pixel clock is
deliberately left where it is known to be stable. The `p` and `b` console keys
remain the fastest way to explore it if anyone wants more.

### 4.4 Report upstream: the steering inconsistency
`/api/dashboard` gates `steeringAngle` on `steerSupported = (gen == 2 || 4 ||
50 || 52)`, so on Gen1 the JSON field is `null` — while the ESP-NOW packet
carries the same `received_steering_angle` for the same car, and so does the
BLE characteristic. The CAN decoder supports PQ `LW_1` (0x0C2) *and* MQB
`LWI_01`, so the platform is deliberately supported one layer down and then
excluded at the API. It shows in the controller's own web UI too
(`data/app.js` renders `steeringAngle`), and `steeringHealthy` is `false` on
Gen1, so a client that trusts the health flag is misinformed rather than
merely uninformed.

The dash uses the packet and is unaffected; the raw page and `e` on the
console show both transports side by side. The evidence, the three-path
comparison and a suggested fix are written up ready to paste in
[`docs/upstream-steering-report.md`](docs/upstream-steering-report.md).

### 4.5 `ohx_proto.h` and `cannow_proto.h` must be refreshed from upstream
They are verbatim copies of the controller's shared headers. If the C6's struct
changes and this copy does not, the `static_assert` on the packet size will
catch it — but only if someone looks. Worth a note in the release checklist.

# Planned upgrades — functional first, cosmetic after

Drawn up from the drive of 2026-11-06 (0.5.1) and the code as it stands. Nothing here
is started. Priorities are proposed, not fixed: read the "Decisions wanted" section
at the end before this turns into code.

The short version:

| # | Item | Kind | Size | Priority |
|---|---|---|---|---|
| 1 | TC/ASR lamp logic is inverted | correctness | S | **P1** |
| 2 | Boost shown on Gen1, where it does not exist | correctness | S | **P1** |
| 3 | Brake pressure needs its validity bit | correctness | S | **P1** |
| 4 | Fuel level + low fuel warning | feature | M | **P2** |
| 5 | Turn indicator repeaters | feature | M | **P2** |
| 6 | Odometer in the status bar, rpm peak dropped | feature | S | **P2** |
| 7 | Metric / imperial toggle | feature | S | **P2** |
| 8 | Automatic day / night mode and brightness | feature | M | **P2** |
| 9 | Settings page: static, no scrolling, paired rows | usability | M/L | **P2** |
| 10 | Shift light: flash the bar, not the numbers | usability | S | **P3** |
| 11 | Automatic FWD while cruise control is active | feature | M | **P3, last** |

S = under half a day, M = about a day, L = more. Sizes assume the relay path below
is built once and reused, which is why #4–#7 are cheap.

---

## 0. The one piece of architecture this batch needs

Today the relay feeds exactly five values, hand-picked bytes in
`can_relay.cpp::values()` — coolant, oil, brake, handbrake — on the grounds that the
DBC gets the handbrake wrong for PQ. Everything in this batch wants *more* of the
same bus, and wants it **named**, so:

**New: `src/net/chassis.{h,cpp}`** — decodes the relayed frames into one `Chassis`
structure, using `dbc::read()` on `dbc/vw_pq.dbc` / `dbc/vw_mqb.dbc` rather than
hand-picked bytes, so adding a value later is a table entry and not a new branch.

```cpp
struct Chassis {
  // indicators, mirrored not generated
  bool indLeft, indRight;
  // fuel
  float fuelLitres;  bool fuelLow;  bool fuelValid;
  // lighting
  int  term58s;      // 0 = lights off; >0 = lights on
  int  term58d;      // dimmer wheel, %
  bool termValid;
  // cluster
  float odometerKm;  float outsideTempC;
  // brakes and driver aids
  float brakeBar;    bool brakeValid;
  bool  lampAsr, lampAbs, lampBk;  // the cluster's own lamps
  bool  asrActive;                 // BR1_ASR_Req: intervention happening now
  // cruise
  bool  cruiseRegulating;
  // cadence
  uint32_t seq;      // bumped when anything changed, so the UI refreshes cheaply
};
Chassis read();            // merged view, thread-safe enough for the UI task
bool    fresh();           // any of the above seen inside kChassisFreshMs (2000)
```

Rules it implements, so the UI does not have to:

* every signal carries its own freshness window; a stale signal reports its last
  value **plus** a `*Valid`/stale flag, and the UI decides how loud to be about it
* `Needs`/validity/error bits from the frames are respected (see #3)
* no timing is generated anywhere in this module — it mirrors the car

`can_relay.cpp::values()` then either calls into this or is folded into it, so there
is one place that knows how a bus value becomes a shown value.

---

## 1. TC / ASR lamps are inverted — P1

**Wanted:** off when TC is on, lit when TC is disabled, flashing while TC is
intervening.

**Today:** `dash_ui.cpp:1223` `lampState(w.lampTc, d.tcOn, false)`, where
`datafeed.cpp` computes `tcOn = p.chassisCan && !p.tcForce` — the lamp is lit when
traction control is *enabled*, which is backwards.

**The car agrees with the request, and we can read the lamp directly.** In the
engine-running capture, `0x1A0 Brake_1` decodes as `BR1_Lamp_ASR = 1` at the same
time as the controller's status packet reports `ASR_OFF`. The cluster is telling us
the lamp is on *because the system is off*.

**Implementation:** take the lamp from the cluster instead of inferring it —
`BR1_Lamp_ASR` (0x1A0) for the lamp, and `BR1_ASR_Req` for "intervening right now",
which is the flash. `BR1_Lamp_ABS`/`BR1_Lamp_BK` come free for the same treatment
later. Fall back to the packet flags (`tcForce`, `asrOff`) only while the frame is
stale, and show nothing rather than a guess.

**Verify:** with the engine running, ASR on: lamp dark. Press the ASR button off:
lamp lit within a second. Wheelslip: it flashes — bench-check that part by watching
`BR1_ASR_Req` on the RAW page instead.

---

## 2. Boost is shown on Gen1, where it does not exist — P1

**Confirmed on the drive, and by the DBC: there is no boost pressure in the CAN
frames on Gen1.** Two independent reasons:

* `0x588 Engine_7` arrives from this car as a **3-byte** frame (`08 00 7E`), while
  `dbc/vw_pq.dbc` declares 8 bytes with `BoostPressure` at byte 4. Byte 4 is not in
  the frame, so that signal cannot exist here. The relay sends the controller's own
  `data_length_code`, so this is the car, not the relay.
* `0x280 Engine_1` is where the two databases disagree: ours has the pedal at bit 40,
  the controller's own decoder expects boost (`MO_Ladedruck`) at bit 39. On this car
  those bits read 0, which is a valid "pedal 0 %" and an impossible "0 bar absolute"
  for an idling engine.

**Why it appeared anyway:** `capability.cpp:74` assumes boost *is* available while the
generation is unknown (`g.boost = g.genKnown ? … : true`), which puts the tile band in
`Full` for the first seconds after every boot; and `datafeed.cpp:85`
(`v.boost = pick(p.boost, http.boost)`) passes the packet's boost straight through
with no capability gate. So a Gen1 car shows a boost number from the controller's own
dubious decode.

**Implementation:** gate the *value* on the capability, not just the layout —
`if (!capability::get().boost) v.boost = NAN;` beside the existing band decision, and
keep the panel hidden as it is. Then `--` is what a Gen1 shows, always. The clamp in
`datafeed.cpp` is the one-line fix; the band rule stays for the layout.

**Report upstream:** our sample (`docs/can-relay-sample-engine-running.txt`) has the
3-byte frame and the bit-39 conflict written up. Worth a note to Forbes: on a Gen1 car
the controller's `0x588` boost read is past the end of the frame.

---

## 3. Brake pressure needs its validity bit — P1

`0x4A8 Brake_5` reads **1.70 bar while stationary with the pedal up**, and
`BR5_PressureValid` (bit 29) is **0**. A naive decode would show "1.7 bar of braking"
at all times. `BR5_Status_Pressure` (bit 30) and `BR5_Sign_Pressure` (bit 31) come
with it.

**Implementation:** in `chassis.cpp`, `brakeBar` is only published when
`BR5_PressureValid` is 1, and the tile shows `--` otherwise. **Verify on the drive:**
press the pedal and watch for the pressure to become valid and rise; that also tells
us whether a valid *zero* is distinguishable from "invalid".

---

## 4. Fuel level and the low fuel warning — P2

**Litres, not a percentage, confirmed from the DBC** —
`SG_ TankContents : 16|7@1+ (1,0) [0|126] "l"` in `0x320 Cluster_1`, so 1 litre per
count, 7 bits, maximum 126. Live: **22 l** with the engine idling, 25 l in the
engine-off capture. **Low fuel is bit 23 of the same frame** (`LowFuelWarning`, 1 =
warning active) and there is no offset or scaling anywhere in it.

So no capacity conversion is needed for litres — only for a **percentage**, which is
where a `tankLitres` config constant comes in (default 55, range 20–120, settings
row). Show all three: bar (percentage of `tankLitres`), litres, and the amber warning.

**Smoothing:** reports say slosh, especially on track, so the *displayed* level is a
1 Hz-friendly low-pass: an exponential filter with a 5 s time constant, plus a
"reject anything that moves more than 8 l in one second" gate. The raw value still
drives the low-fuel lamp, so the warning is never smoothed away. Method goes in the
README next to the value.

---

## 5. Turn indicator repeaters — P2

**Source confirmed:** `0x320 Cluster_1`, `Indicator_Left_4_1` at bit 44 and
`Indicator_Right_4_1` at bit 45, one bit each, in a frame we already receive at
10/s. DBC comment: `[DE: Blinker_links_4_1]` / `[DE: Blinker_rechts_4_1]`.

**Mirror, do not generate.** Whether the bit is the *lamp* state or a steady
"indicator active" status, I could not confirm from the captures we hold: both were
taken with the indicators off (byte 5 = `0x00`). It is a 30-second check — indicators
on, one `C` press, count the transitions of bit 44 in the dump. If it toggles, mirror
it; if it is steady, we generate the blink ourselves at the car's rate. Default is to
mirror whatever arrives.

**Aliasing at 10 Hz: not a problem, with one caveat.** A VW indicator flashes at about
1.5 Hz, so each phase lasts ~330 ms — the relay samples it ~3.3 times per phase, well
above the 2 needed, and hazards use the same rate. The caveat is loss: `air lost` was
8–11 % on the drive, and a missed frame delays an edge by up to 100–200 ms, so an
occasional blink can look a touch lopsided. Harmless for a repeater; it is why we
mirror rather than re-time. (The analysis goes in this document, since there is no
`ANALYSIS.md` — say the word and it becomes one.)

**Placement:** left and right of the RPM bar, top corners, green when lit. Both on
together (hazards) lights both. Stale data: both off, and the NO DATA overlay covers
it as it does now. The controller also reports a `hazard` flag in its status packet —
worth showing as a cross-check on the RAW page, but the repeaters themselves read the
bus.

---

## 6. Odometer in the status bar — P2

`0x520 Cluster_3` `Odometer` decodes as **138 618 km** on this car, in a frame we
already receive at 4/s. Drop the rpm peak (and the `g_peaks.rpm` field, and its update
at `dash_ui.cpp:1446`) and use the 180 px slot the peak occupied. RESET PEAKS then
only covers speed and boost, both of which are shown in their own tiles.

Rounded to whole units, in miles when #7 says so, and it must not count as "data" for
the LIVE/STALE logic — an odometer that is 10 s old is not stale in any useful sense.

---

## 7. Metric / imperial toggle — P2

One setting, `unitsMetric` (default metric), in the existing units area of the
settings page next to the C/F and bar/psi toggles. It affects speed (km/h ↔ mph),
odometer (km ↔ miles) and the odometer on the status bar; distance only — the
temperature toggle already exists and stays separate. All formatting goes through one
helper (`fmtSpeed`, `fmtDistance`) so nothing is converted twice.

---

## 8. Automatic day / night mode and brightness — P2

**Sources, both present in `0x420 Cluster_2`, both already relayed at 4–10/s:**

| Signal | Where | Live value (drive, lights off) |
|---|---|---|
| `Terminal_58s__Cluster_2_` | bit 48, 7 bits, % | **0** |
| `Terminal_58d__Cluster_2_` | bit 40, 7 bits, % | **24** |
| `ErrorStatus_Term__58_s` / `…_d` | bits 55 / 47 | 0 (valid) |

Two corrections to the brief, both from the live data:

1. **Use 58s for the mode decision, 58d for the brightness scale.** 58d sat at 24 %
   with the lights off on the drive — it follows the dimmer wheel, so it does not mean
   "night" on its own, while 58s was a clean 0. Rule as asked: 58s > 0 → night, 58s
   == 0 or absent → day.
2. **There is no `localStorage`** — the dashboard persists settings in NVS
   (`Preferences`, namespace `ohxdash`). The override goes there with everything else.

**Behaviour:** mode is Auto (default) / Day / Night, with the existing DAY button
moving off the status bar into the settings page (which also frees the space #6
needs). The 300–500 ms debounce is applied to *changes* only, using a 400 ms
hysteresis: a new reading must hold for 400 ms before the mode moves. Missing or
error-flagged values are treated as 0 (day) for a mode *decision*, but never move the
mode — the last mode is held — so a dropped frame cannot strobe the screen.

**Brightness.** Day: the configured brightness (default 100). Night: scaled from the
dimmer,

```
night = kNightMinPct + (kNightSpanPct * term58d / 100)      // clamped to kNightMinPct..100
kNightMinPct  = 20    // never darker than this: readable at night
kNightSpanPct = 45    // so 58d 0 % -> 20 %, 58d 100 % -> 65 %
```

Those are my picks — 20 % floor for legibility, 65 % ceiling so night never
dazzles. Both are `#define`s in `board_config.h` beside the other tunables, and the
settings page shows the resulting number so it can be judged on the glass.

**Palette.** `include/theme.h` is a wall of compile-time `#define`s used at ~100 call
sites, so a true second palette means converting them into a runtime `Palette` and
re-applying colours to every widget — real work for a modest gain, since the backlight
is what actually stops the glare. **Proposal: do the brightness in this batch, and
defer the palette swap to its own change** (it can then be done properly with a
`theme::apply(bool night)` that re-colours the visible widgets once per mode change).
If you want the palette now, say so and it moves up — the LVGL pool has room.

---

## 9. Settings page: static, no scrolling — P2

**Why it scrolls:** `dash_ui.cpp:910` makes the 780×420 card `LV_DIR_VER` +
`SCROLLABLE`, and about 20 rows are stacked at 30–44 px each (three text fields,
brightness, two unit toggles, eleven steppers, five buttons). Scrolling this by touch
on a 32 Hz panel is what feels slow — each scroll step repaints, and every repaint
costs a full-width redraw.

**Plan — lay it out in two columns and pair related values**, so it fits 420 px with
no scrolling at all:

```
CONNECTION                        THRESHOLDS
 SSID    [___________]             RPM  max/amber      [8000/6000]
 PASSWORD[___________]             RPM  redline/shift  [6800/7300]
 HOST    [___________]             Coolant warn/crit   [105/115]
 control: …  ota: …                Oil     warn/crit   [130/145]
                                   Boost   max/warn    [1800/1500]
DISPLAY                            Boost   crit        [2000/--]
 brightness/C-F/bar-psi/units      Fuel tank   [55 l]
 day-night Auto/Day/Night
 fuel warn %  […]
                                   [DEFAULTS] [RAW] [CAN] [SAVE] [CLOSE]
```

Each paired row shows `VALUE/VALUE` with its own `-` `+` per value — two buttons each,
four per row, 24 px, big enough for a fingertip on this panel. Steppers keep the
existing `addStepper()` look and the existing `clampv()` validation in
`config.cpp`, so pairing them is presentation, not new arithmetic.

The three text fields keep the on-screen keyboard, which currently overlays the
bottom 200 px — so the fields stay in the top half of the layout to avoid being typed
through.

This is the largest single item in the batch and it is mostly layout, so it is worth
doing once, after the values it has to display (#4–#8) exist.

---

## 10. Shift light: flash the bar, not the numbers — P3

**Today:** `dash_ui.cpp:1084` flashes the *rev number* red at `rpm >= rpmShift`, at
`kFlashMs = 60` (8.3 Hz). You are right that a 96 px number strobing is hard to read.

**Wanted:** flash the bar, 4 Hz, blue or purple at the shift point, red at the
limiter.

* `kFlashMs = 125` for a clean 4 Hz — the panel's vsync gives 32–33 refreshes/s, so
  every flash phase is 8 refreshes: no beating, no partial phases.
* The bar has per-cell colours already (`g_revCol[i]`, set from `rpmAmber`/`rpmRedline`
  at `dash_ui.cpp:413-417`), and only the cells that cross a boundary are repainted.
  Flashing means repainting the *lit* cells between two colours, so it costs one pass
  over `BAR_SEG` cells per phase instead of a text change — no cheaper and no dearer
  than today's number flash.
* Colours: I would take **`COL_PURPLE 0x8b5cf6` for the shift flash** (it survives a
  daylight drive better than blue, and cannot be confused with the green/amber/red
  zones) and `COL_RED` at the limiter. Both go in `theme.h`.
* The number keeps its colour: no flash there at all.

**The thresholds need fixing first — they currently overlap.** Defaults are
`amber 6000 / redline 6800 / shift 7300` and `config.cpp:68-71` only enforces
`amber ≤ redline ≤ max` with `shift` free, so **the shift light fires 500 rpm after
the limiter**, and "red at the limiter" can never happen. Proposed semantics, which
is also the answer to your question:

| Setting | Means | Default now | Proposed |
|---|---|---|---|
| `rpmMax` | the full scale of the bar (what 100 % means); also sets the tick spacing | 8000 | 8000 |
| `rpmAmber` | where the bar turns amber (colour only) | 6000 | 6000 |
| `rpmRedline` | where the bar turns red **and** the limiter flash | 6800 | 6800 |
| `rpmShift` | where the shift flash starts — should be *below* the redline | 7300 | **6500** |

and `validate()` gains `amber ≤ shift ≤ redline ≤ max`, so stored values are clamped
into a sensible order on load rather than silently disagreeing with each other.

---

## 11. Automatic FWD while cruise control is active — P3, last

**Feasible, and the signal is in a frame we already have.** `0x288 Engine_2`:

* `MO2_GRAregelt` — bit 26, 1 bit — "cruise control is regulating", i.e. **active**
* `MO2_Status_GRA` — bit 22, 2 bits — cruise availability/status
* `MO2_GRA_Target` — bit 32, 8 bits, 1.28 km/h — the target speed

Live on the drive: `GRAregelt = 0` (not in cruise), and `0x38A GRA_New`
(the stalk) reads `GRA_MainSwitch = 1`, so the main switch is already on and the
"active" bit is the one to watch. Both frames arrive at 10/s, and the controller's
own status packet does not carry a cruise flag, so this is a CAN-only feature — which
is the first thing in this batch that *depends* on the relay being on.

**This is the highest-risk item in the document** and I would do it last, after the
battery of the display work. It is the only one that *acts on the car*: it sends real
mode commands to the controller, so a mis-decode changes how the car puts power down
mid-corner rather than just showing a wrong number. Requirements before it ships:

* **opt-in setting**, off by default, and it says on the glass when it is on
* **debounce**: cruise must be active for 2 s before the mode moves, and inactive for
  2 s before it returns
* **remember and restore**: capture the mode at entry, restore exactly that on exit —
  never "cycle to the next one"
* **never fight the driver**: any manual mode change wins, and switches auto-FWD off
  for the rest of the ignition cycle
* **announce**: the mode-change banner already exists for the mode switch; reuse it,
  plus a console line, so a mode change is never silent
* **still subject to the open-access-point warning** — that stays a warning and does
  not block this or anything else

If any of those are unwanted, the feature is not worth having: an automatic AWD
change that the driver cannot see is worse than the fuel it saves.

---

## Cross-cutting

* **Order of work:** 1–3 first (small, and they are wrong values on the glass today),
  then the `chassis.cpp` relay path, then 4–8, then 9 (which displays 4–8), then 10,
  then 11.
* **What this does *not* need:** flash or RAM. The relay tables and the LVGL pool are
  in PSRAM since 0.5.1, internal RAM is flat at 156 KB, and the DBC decoder is
  already linked. These are display and logic changes.
* **Rate reality:** everything above reads frames we already receive at 10/s with
  8–11 % packet loss, which is fine for lamps, fuel and cruise state and marginal for
  nothing in this batch. Nothing here needs a faster relay setting, so the
  controller's setting can stay where it is.
* **Every new value needs a stale story.** The policy, applied per value: hold the
  last number for a slow value (fuel, odometer, temperatures), go dark for a binary
  (indicators, lamps), and never move a mode (day/night) because a frame was missed.

## Decisions wanted

1. **Priorities** — the brief ends "prioritise functional improvements vs", and the
   rest was cut off. Taken as functional first, everything else after. Correct me if
   it was "vs cosmetic" with a different split in mind.
2. **Fuel:** `tankLitres` default 55 — is that the right tank for the car? Only used
   for the percentage; litres are litres.
3. **Night brightness:** `20 % + 45 % × dimmer` as above — pick different numbers now
   if you want them, they are one-line constants.
4. **Shift flash colour:** purple `0x8b5cf6` preferred over blue; fine?
5. **`rpmShift` default 7300 → 6500**, and the clamp order `amber ≤ shift ≤ redline`
   — this changes behaviour with no settings change by the user, which is the point.
6. **Indicators:** mirror the bit as it arrives (default) — do you want the check that
   settles lamp-vs-status first, or should I just build mirroring and see it in the car?
7. **Night palette:** brightness only in this batch (my recommendation), or the full
   `theme.h` conversion now?
8. **Auto-FWD:** confirm the safety requirements in #11, or drop it to a later batch.

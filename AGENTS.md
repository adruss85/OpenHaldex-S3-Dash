# AGENTS.md

Guidance for AI coding agents working on **OpenHaldex S3 Dash**. Read this first; it is
deliberately short. Detail lives in the files it points to.

## What this is

Native dashboard firmware for the Elecrow **CrowPanel 7.0"** (ESP32-S3, 800x480 RGB panel,
8 MB OPI PSRAM, 4 MB flash). It is a **display client** for the separate OpenHaldex-C6
Haldex AWD controller: it reads what the controller publishes (ESP-NOW broadcast, CAN relay,
HTTP `/api/*`) and sends one thing back, the signed Haldex mode-change command.

**The C6 controller firmware is out of scope and must not need changes.** Do not propose
edits to it; report upstream problems in `docs/upstream-steering-report.md` style instead.

Stack: PlatformIO, Arduino framework on pioarduino, LVGL 9.3, ESP-IDF `esp_lcd` (no display
driver library), ArduinoJson 7, TAMC_GT911. C++ in `src/`, config headers in `include/`.

## Ignore these directories

Do not read, grep, index or edit: `.pio/`, `.venv/`, `.backup/`, `.shots/`, `.dummy/`,
`compile_commands.json` (27 MB), `tools/__pycache__/`. They are build output, vendored
libraries (LVGL, ArduinoJson), local tooling, or a factory flash dump. Scope searches to
`src/`, `include/`, `tools/`, `dbc/` and the project `*.md` files.

## Build, flash, verify

```bash
# one-off: create the venv and install PlatformIO plus pyserial/esptool
python3 -m venv .venv && ./.venv/bin/pip install platformio pyserial esptool

./.venv/bin/pio run -e s3-dash                 # build (first run downloads the toolchain)
./.venv/bin/pio run -e s3-dash -t upload       # flash over USB (board in download mode)
```

- Upload speed is pinned to **460800**; the CH340 fails at 921600.
- Serial console: 115200 baud on UART0 (`/dev/ttyUSB0`).
- **Opening the serial port resets the board.** Use `tools/serial_shell.py` (opens once,
  no reset) or `tools/serial_watch.py` (read only). `tools/serial_capture.py` and
  `tools/serial_cmd.py` reset deliberately.
- There is no unit-test suite. **A successful `pio run` is the only check you can do
  without hardware.** Anything touching the panel, WiFi, ESP-NOW, CAN relay or OTA needs the
  board or a car. Say clearly in your handoff what was and wasn't verified on hardware.
- After a build, check the size lines (see Budgets). Report them.

If the platform vanishes on first build (`IncompatiblePlatform`), see README section 3.

## Hard rules (these have all caused bugs or were deliberate decisions)

1. **Never edit `include/ohx_proto.h` or `include/cannow_proto.h`.** They are verbatim
   copies of the controller's headers. Refresh from upstream only. Their `static_assert`s
   will catch size drift.
2. **Generated files are not hand-edited.** `src/net/dbc_tables.cpp` comes from
   `tools/make_dbc_tables.py` (inputs in `dbc/`); `src/ui/fonts/*.c` come from
   `tools/make_fonts.sh`. Change the source and regenerate.
3. **Unknown is `NaN`, drawn as `--`. Never substitute a fake zero.** A JSON `null` or a
   packet "no data" sentinel travels to the screen as unknown. A genuine `0` draws `0`.
4. **Settings struct is append-only.** New fields go at the **end** of `config::Data`
   (`src/app/config.h`) so older NVS blobs stay a byte prefix of newer ones and migrate in
   place. Bump the blob version, extend the migration in `config.cpp`, and keep the clamps
   (`amber <= shift <= redline <= max`, `warn <= crit`) valid on load and save.
5. **Provenance is decided in one place:** `src/net/datafeed.cpp`. Do not read a transport
   directly from UI code. The source table is in `docs/architecture.md`.
6. **Tiles and lamps do not go through the DBC.** They decode the same raw bytes the
   controller does (`src/net/chassis.cpp`), because those are proven on a car and the DBC
   can be wrong. The DBC powers only the CAN DATA page.
7. **Fuel is always litres.** The km/mi toggle is distance only.
8. **Telltales mirror the car; do not infer.** CC lamp: armed vs regulating are different
   signals (see comment in `chassis.cpp`).
9. **No automatic action on the car.** The only transmission is the user-initiated mode
   step. Automatic FWD-on-cruise is deliberately paused; its agreed safety requirements are
   in `TODO.md` 1.4 and `docs/upgrades.md` section 13. Do not build it without being asked.
10. **No credentials in git.** `include/wifi_secrets.h` is gitignored; use
    `wifi_secrets.h.example` as the template. Never commit `.backup/` or device dumps.
11. **Layout geometry lives in `include/theme.h`.** Nothing scales; it is absolute pixels
    derived from `u = min(w/100, h/56)` = 8 px at 800x480. Change it there, not inline.

## Budgets: do not regress these

The panel streams its frame buffer out of PSRAM continuously, and WiFi buffers share that
memory. Most performance problems here are **PSRAM bandwidth**, not CPU.

| Resource | Limit / baseline | Why it matters |
|---|---|---|
| App flash | OTA slot is **1,966,080 B** (0x1e0000). 0.6.2 was 91.8% used; **re-measure after your change** | Over the limit breaks OTA and the build |
| Internal RAM | ~163 KB free, ~163 KB lowest at idle (0.6.0). Watch `lowest` in the `[ram]` line | WiFi, ESP-NOW and task stacks need it |
| Render rate | ~62 fps idle on the bench (0.6.0), 16-35 fps with radio traffic | Extra HTTP or full-screen redraws cost frames |
| Pixel clock | 16 MHz (32 Hz refresh), deliberately conservative | Raising it starves the CPU |

Rules of thumb: no unconditional `lv_obj_invalidate`; cache per-cell state like the rev bar
does; keep LVGL in `DIRECT` render mode with two frame buffers; do not add polling (HTTP
is already the minimum that keeps the controller's AP alive, `/api/dashboard` is polled
at least every 2 s for that reason); avoid scrolling containers (a scroll step repaints
full width); avoid gradients (the panel bands them). Arcs and large anti-aliased shapes
were measured as very expensive (`docs/redesign-bars.md`).

Measured numbers in the docs are dated by firmware version. Treat them as baselines,
not facts about the current build.

## Where things are (task to files)

| Task | Touch |
|---|---|
| Add or change a **setting** | `src/app/config.h`/`.cpp` (field, default, clamp, migration), settings page `buildSettings()` and the `on*` handlers in `src/ui/dash_ui.cpp`, README section 6 defaults table |
| Add a **dashboard widget or lamp** | `src/ui/dash_ui.cpp` (`buildTiles`, `buildStatusBar`, `syncGauge`, `place*Tile`), geometry in `include/theme.h` |
| Add a **value from the car** | Decode in `src/net/chassis.cpp`, carry it through `can_relay`/`datafeed`, show on the raw page `src/ui/raw_ui.cpp`, then the dashboard |
| Change **where a value comes from** | `src/net/datafeed.cpp` only, plus the table in `docs/architecture.md` |
| **ESP-NOW** receive, decode, mode command | `src/net/ohx_now.cpp` (wire format: `include/ohx_proto.h`, read-only) |
| **CAN relay** receive | `src/net/can_relay.cpp` (wire format: `include/cannow_proto.h`, read-only) |
| **HTTP polling** and WiFi join | `src/net/telemetry.cpp` (core 0 "ohxnet" task) |
| **Capability detection** (what this car can fill) | `src/app/capability.cpp`; band choice in `dash_ui.cpp` (`placeBand`) |
| **OTA** page and boot trial | `src/app/ota.cpp`; partitions in `partitions_4mb_ota.csv` |
| **CAN DATA page** names and scaling | `dbc/*.dbc`, regenerate with `tools/make_dbc_tables.py` |
| **Panel, touch, backlight** | `src/hal/crowpanel.cpp`, pins and timings in `include/board_config.h` |
| **Boot splash** | in `src/ui/dash_ui.cpp`; background JPEG is `assets/candash-splash-base-92.jpg`, embedded via `board_build.embed_files` in `platformio.ini` |
| **Console key** | the `switch` in `src/main.cpp` and the table in `docs/console.md` |
| **Version bump** | `include/version.h` (the history comment is the changelog) |

`src/ui/dash_ui.cpp` is ~2,200 lines (dashboard, settings page, splash). Search by function
name from the table above instead of reading it top to bottom.

Threading: `ohxnet` task on core 0 does WiFi, HTTP and the ESP-NOW callback; `loop()` on
core 1 does UI tick, `feed::poll()` and `lv_timer_handler()`. Data crosses cores as copied
structs; keep it that way and do not touch LVGL from core 0.

## How to verify a change

| Change type | Check |
|---|---|
| Any | `pio run -e s3-dash` succeeds; report flash and RAM lines |
| Layout or UI | On hardware: `tools/serial_cmd.py d 34` prints the live frame buffer as an ASCII map. Redraw groups can be masked with console keys `1`-`5` to find costly widgets (`tools/serial_tune.py`) |
| Settings or migration | Flash over an older build and confirm values survive; check clamps on load |
| Data path | Console `e` (ESP-NOW and HTTP side by side), `k` (chassis decode), `x` (raw page on glass) |
| CAN | `tools/decode_can_snapshot.py` against `docs/can-relay-sample*.txt`; console `C` and the CAN DATA page |
| Stale/fallback behaviour | Stop both transports: `E` then `n` |
| OTA | Console `o` shows slot and trial state; `O` arms a deliberately failing trial |

Console keys are listed in `docs/console.md`.

## Conventions

- Match the existing style: comments explain **why** and record measured evidence; keep that
  habit. Prefer editing in place over new abstractions.
- Console output is a first-class debugging interface. New runtime behaviour should log one
  clear line, and anything that changes state should say so on the console.
- Prefer a small, honest change over a wide one. Several things in this repo are decisions,
  not oversights (see `TODO.md` "Waiting on a decision"). Ask before reversing one.
- If you change behaviour, update the relevant doc and `TODO.md` in the same change. Do not
  leave shipped items in TODO or unbuilt items in the docs.
- Commit style so far: short imperative subject, version-prefixed for releases
  (`0.6.1: ...`), body explaining the reason.

## Documentation map

| File | Read when |
|---|---|
| `README.md` | Overview, hardware, build/flash, backup, OTA, configuration defaults and the repository layout |
| `docs/architecture.md` | Where each value comes from, what the car can fill, ESP-NOW/CAN relay, and what the dash cannot show |
| `docs/display-and-memory.md` | Display tuning, the boot splash picture, frame buffers, internal RAM and PSRAM |
| `docs/console.md` | Serial console keys, the raw/CAN pages and the serial tools |
| `docs/status.md` | What is verified where, its firmware version, and the current build size |
| `TODO.md` | Before proposing new work: what is deliberately unbuilt, unverified, or awaiting a decision |
| `docs/upgrades.md` | History and evidence behind the 0.6.0 batch; section 13 (cruise-FWD) and 14 (headlights) are the only live items |
| `docs/esp-now-plan.md` | Why ESP-NOW is the primary transport; all three phases are built, with their verification state |
| `docs/redesign-bars.md` | Measurements behind the rev-bar design. **Historical:** its layout sketch predates layout C |
| `docs/can-relay-test.md` | The in-car test procedure for the CAN relay |
| `docs/upstream-steering-report.md` | Ready-to-paste upstream issue about Gen1 steering angle |
| `dbc/SOURCE.md` | Provenance and licence of the DBC files |

## Known hardware and environment facts

- Reference car is **Gen1 (PQ)**: no boost frame, no supply voltage, no Haldex temperatures,
  CAN-only handbrake. The Full (B) tile band, `boost` scaling, and other Haldex generations
  are **unverified** on real cars.
- The 5.0" CrowPanel needs `-DDASH_USE_50_PINMAP=1`; untested.
- This repository is AI-generated and human-tested on one board. Keep claims about
  verification honest.

# OpenHaldex S3 Dash

A native dashboard for the **[OpenHaldex](#credits)** Haldex AWD controller,
running directly on a **7" ESP32-S3 touch panel**. It replaces the browser
dashboard — `dash.html`, served by the controller itself — with standalone
firmware, so the display boots straight to the gauge in the car. No phone, no
tablet, no browser, no WiFi client to keep awake.

The controller firmware is **not modified**. This project only reads what the
controller already publishes, so it can keep taking upstream updates.

> ### ⚠️ 100% AI-generated project
>
> Every line of this repository — firmware, build scripts, mock-ups and
> documentation — was written by **goose**, an open-source AI coding agent.
> Nothing here was typed by hand. The human role was direction, design
> approval, hardware bring-up and testing in the car.
>
> The models behind it: **`deepseek-v4.1-flash`** did the heavy lifting, with
> **`gpt-oss-20b`** running under **local inference** on the simpler tasks.
> `goose` orchestrates them and does the work on the wire.
>
> Treat it accordingly: it was built against one specific board (a CrowPanel
> 7.0" ESP32-S3), it is not security-audited, and the API it consumes is a
> hobby project's own. Read the code before you flash it.

---



## Contents

| | |
|---|---|
| [Credits](#credits) | [Architecture and data flow](docs/architecture.md) |
| [1. What it shows](#1-what-it-shows) | [Display and memory](docs/display-and-memory.md) |
| [2. Hardware](#2-hardware) | [Serial console](docs/console.md) |
| [3. Build](#3-build) | [Status and what is verified](docs/status.md) |
| [4. Before you flash: back up the board](#4-before-you-flash-back-up-the-board) | [What is not built yet](TODO.md) |
| [5. Updating it in the car](#5-updating-it-in-the-car-over-the-air) | |
| [6. Configuration](#6-configuration) | |
| [7. Repository layout](#7-repository-layout) | [Licence](#licence) |

---



## Credits

**[OpenHaldex](https://github.com/Forbes-Automotive/OpenHaldex-C6)** is an
open-source Haldex AWD controller for Volkswagen and Audi Group Gen1, Gen2 and
Gen4 differentials, built by **[Forbes Automotive](https://forbes-automotive.com)**.

Everything this dashboard displays originates there. The CAN decoding, the
Haldex control logic, the HTTP API and the ESP-NOW broadcast are all the
OpenHaldex-C6's work — this repository is only a **display client** for it.

If you are looking for the controller rather than a display for it, that is
the project you want.

The two sides communicate using interfaces the C6 already exposed, so neither
needed changing: `include/ohx_proto.h` and `include/cannow_proto.h` here are
**verbatim copies** of the controller's shared wire-format headers. Refresh them
from upstream; never edit them.

The CAN database behind the CAN DATA page is **[opendbc](https://github.com/commaai/opendbc)**
by comma.ai (MIT), with the VW names translated to English by Forbes Automotive:
see [`dbc/SOURCE.md`](dbc/SOURCE.md). Bug fixes, the CAN relay receiver and the CAN
DATA page were contributed by **Adam Forbes**.

---



## 1. What it shows

![The dashboard layout](docs/mockups/C.%20Updated%20layout.png)

*The layout the glass is built to (0.6.0). The earlier mock-ups in
`docs/mockups/` — A, B and `g-fullwidth.png`, the one the band widths come from —
predate the fuel row, the repeaters and the odometer.*

| Band | Contents |
|---|---|
| Rev bar | One continuous bar across the full 800 px (80 cells drawn edge to edge), one number per 1000 rpm above it and `x1000` at the right, then the rpm as a large centred number on its own row — with a **turn-indicator arrow at each end of that row**, hidden until the car blinks, then flashing green |
| Tiles | `SPEED` (km/h or mph + peak) · `BOOST` (value, unit, a bar, peak) · the **temperatures card** (`COOLANT`, `OIL` and `FUEL`, each with its unit and a bar; no caption of its own — its first row lines up with the other cards' captions) · `HALDEX` (mode, coupling state, lock requested / actual, throttle) — **tap it to step the Haldex mode** |
| Status bar | Data-state dot, the BRAKE / HAND / TC / ASR / **CC** lamps, the **odometer**, and the RESET PEAKS / SETTINGS buttons |

The tile band follows the same geometry as the browser page, from one unit:

```
u = min(width / 100, height / 56)         ->  8 px at 800x480
```

REVS is the only full-width item; everything else is a tile, so no value
appears twice.

**Tell-tales mirror the car, they are not inferred.** BRAKE and HAND light from
the chassis CAN bit (or the controller's wired input, whichever moves — on this
car the CAN brake works and the wired handbrake never moves). TC and ASR mirror
the cluster's own `BR1_Lamp_ASR` and flash while it intervenes. CC mirrors the
cluster's own cruise lamp bit (`MO2_Status_GRA`): lit when that says the car's
lamp is lit, dark green when the engine is set but not regulating, bright green
while it is regulating to a set speed. The column switch is only a fallback for
a bus that has no `MO2_Status_GRA`. The shift cue is the bar, not the number: at
the shift point the whole bar flashes purple, at the redline red, covering the
full width rather than only the part below the shift point.

**Unmeasurable values show `--`, never a fake zero.** A JSON `null` from the
controller is carried all the way to the screen as "unknown", with a dash
(`NaN`) as the sentinel. Only the dash glyph draws `--`; a genuine `0` draws
`0`.

**Staleness moves the status light, not the data.** When live data stops
arriving the dashboard keeps the last values it had — bars stop where they
were, tiles keep their numbers — and the status dot turns amber with `STALE`.
Before the first sample ever arrives it reads `WAIT`, and `NO WIFI` in red if
the link itself is down. Nothing is blanked and no overlay covers the screen.

**Design choices specific to this display:**

* The status bar is **30 px** rather than the page's `2.8u` (22 px), so the
  buttons are a usable touch target. The band above absorbs the difference.
* **Long-press anywhere on the top band opens Settings**, because the
  status-bar buttons are small to hit in a moving car.
* Boost, speed and rpm are drawn with the *eased* value — a 110 ms time
  constant, matching the page — so they sweep rather than snap. Fuel is eased
  much harder (5 s, behind a slosh gate), because a tank level is read at a
  glance rather than followed.
* **The backlight follows the car.** Terminal 58s — the lighting switch and dash
  dimmer wheel, one signal that reads 0 with the lights off and 7-99 % with them
  on — decides day or night and, at night, the brightness. Auto / Day / Night is
  a setting, not a status-bar button.
* Settings holds every threshold, the units, the day/night mode, the brightness
  and the connection, and is stored in NVS so it survives a power cycle. It is
  a full-screen page in two columns with no scrolling: on a 32 Hz panel, a
  scroll step repaints the full screen width, which is what made the old page
  feel slow.

---



## 2. Hardware

| | |
|---|---|
| Board | Elecrow **CrowPanel 7.0"** HMI, `ESP32 Display-7.0` |
| MCU | ESP32-S3 QFN56 rev v0.2, dual core @ 240 MHz |
| PSRAM | 8 MB **OPI** (octal) — mandatory, see §3 |
| Flash | 4 MB quad |
| Panel | 800x480 RGB565 parallel (16 data lines + DE/VSYNC/HSYNC/PCLK) |
| Touch | GT911 capacitive, I2C on SDA 19 / SCL 20 at 400 kHz |
| Expander | PCA9557 I2C: IO0 = panel reset, IO1 = input |
| Backlight | GPIO 2 |
| USB | CH340 UART bridge (`/dev/ttyUSB0`), 460800 baud ceiling |

Nothing about the firmware is board-specific beyond `include/board_config.h`,
which holds the pin map, panel timings, I2C pins and the expander.

### The 5.0" variant

The CrowPanel **5.0"** uses the *same* 16 GPIOs but wired to the panel's RGB
inputs in a **different order**, and swaps DE/VSYNC. Building for it is one
flag:

```ini
build_flags = -DDASH_USE_50_PINMAP=1
```

A wrong pin map shows up immediately as a garbled or colour-swapped image. It
damages nothing.

### Touch axes

The GT911 library's `ROTATION_NORMAL` returns `(width - x, height - y)` — it
mirrors *both* axes — which puts every press 180° from the finger. This build
sets `ROTATION_INVERTED`, and the console's `r` key cycles the four mappings
if a differently-mounted panel needs another one.

---



## 3. Build

```bash
cd OpenHaldex-S3-Dash
./.venv/bin/pio run -e s3-dash
```

This is a plain [PlatformIO](https://platformio.org) project. The first
build downloads the pioarduino `espressif32@55.3.312` platform — the same
release the OpenHaldex-C6 firmware uses, so both projects build against an
identical toolchain — plus the framework, which takes a few minutes, once.

**If the platform disappears on the first build**, the installed PlatformIO
core is reporting itself as a post-release of 6.2.0 (`6.2.0.post1`), which
PlatformIO turns into the semver *pre-release* `6.2.0-post.1`. The platform
declares `engines >= 6.2.0`, the pre-release fails that test, and Platform
Manager deletes the platform — leaving `~/.platformio/platforms` empty and a
build log that ends with `IncompatiblePlatform`. The core is a post-release,
not a pre-release, so report it as plain 6.2.0: in `platformio/__init__.py`,
set

```python
VERSION = (6, 2, 0)
```

**PSRAM must be OPI.** The panel driver needs two 800x480x16 bpp frame buffers
(1.50 MB) reachable by the LCD DMA engine. `platformio.ini` sets
`board_build.arduino.memory_type = qio_opi`; without it `lcd.begin()` fails
and nothing is drawn.

### Flash

```bash
# with the board in download mode:
./.venv/bin/pio run -e s3-dash -t upload
```

`upload_speed` is pinned to **460800**. The CH340 on this board fails at
921600 with `Invalid SLIP escape (0xdb, 0xBD)`.

---



## 4. Before you flash: back up the board

The CrowPanel ships with a factory application. Reading the flash is
read-only and risk-free, and it is worth having:

```bash
./.venv/bin/esptool -p /dev/ttyUSB0 -b 460800 read_flash 0x0 0x400000 \
    .backup/s3_factory_full_4MB.bin
```

Restore the original image at any time with:

```bash
./.venv/bin/esptool -p /dev/ttyUSB0 -b 460800 write_flash 0x0 \
    .backup/s3_factory_full_4MB.bin
```

The board's factory partition table, as read from that image:

| Label | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 0x5000 |
| otadata | data/ota | 0xe000 | 0x2000 |
| app0 | app/ota_0 | 0x10000 | 0x3c0000 |
| spiffs | data/spiffs | 0x3d0000 | 0x20000 |
| coredump | data/coredump | 0x3f0000 | 0x10000 |

This firmware replaces it with `partitions_4mb_ota.csv`, which trades app size
for **two app slots (OTA)**, a 128 KB LittleFS partition and a coredump
partition:

| Label | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 0x5000 |
| otadata | data/ota | 0xe000 | 0x2000 |
| app0 | app/ota_0 | 0x10000 | 0x1e0000 |
| app1 | app/ota_1 | 0x1f0000 | 0x1e0000 |
| littlefs | data/spiffs | 0x3d0000 | 0x20000 |
| coredump | data/coredump | 0x3f0000 | 0x10000 |

OTA matters once the dash is mounted behind the dashboard, where there is no
USB cable. It is deliberately **after** the mode switch: a dash that can
change one thing on the car is worth more than one that can be reflashed, and
the flashing side of OTA needs no new partition to be useful.

---



## 5. Updating it in the car (over the air)

Once the dash is screwed into a pod there is no USB cable, so it can update
itself from a phone. The dash is **already** a station on the controller's
access point — that is how it gets its data — so this needs no new networking
at all:

1. Download `firmware.bin` from the
   [releases page](https://github.com/adruss85/OpenHaldex-S3-Dash/releases)
   onto the phone.
2. Join the controller's access point (`OpenHaldex-C6` by default) — the same
   thing you do to see the controller's own dashboard.
3. Open the address shown on the dash's SETTINGS page, under **UPDATE**:
   `http://192.168.1.203/` (or `http://ohx-dash.local/`).
4. Choose the file and press **Upload and reboot**. It takes about 15 s for
   the ~1.8 MB image, and the page reports the version, the slot and how many
   bytes went in.

Both addresses work because the dash takes a **static** address after the
first join: the controller's access point hands out no leases when it is also
sitting on a home LAN with the same subnet, which is a quirk of this setup
rather than a fault, and the dash learns its way around it.

### The way back, which is the part that matters

Anything written over the air is a **trial** until the dash proves it works.

```
old image  ->  records "the next boot is on trial" in NVS, then reboots
new image  ->  boots, brings the panel up, holds it for 20 s
           ->  stands the trial down: this image is now the confirmed one
```

If that does not happen, the dash puts the previous image back by itself:

| Give-up condition | Why |
|---|---|
| the panel never comes up within 90 s | the image runs, but the thing you look at does not |
| the trial image has already tried to boot 3 times | a crash loop |
| a trial told to fail, from the console (`O`) | how the path is tested without a bad build |

The trial lives in its own NVS namespace because the *outgoing* image is the
one that writes it. The image that is being replaced is never modified: a
power cut in the middle of a write leaves the running slot untouched and the
boot selection unchanged, so the dash comes back exactly as it was.

### What it checks, and what it does not

| Check | Enforced |
|---|---|
| the file is a real ESP image (magic, checksum) | yes, by the IDF |
| it is an image for **this chip** (ESP32-S3) | yes, by the IDF — a `.bin` for another chip is refused |
| it is an image for **this project** | **no** — a valid ESP32-S3 image that is not this dashboard would be accepted, boot, and never stand the trial down. Until that is closed, take the file from this repository's releases page and nothing else |

Nothing is fetched from the internet. The dash never checks for updates: it
has no idea a newer version exists, which is deliberate — nothing on this
device reaches out on its own.

### From the console

| Key | |
|---|---|
| `o` | where the update page is, which slot is running, whether it is on trial |
| `O` | bench test: arm the next boot as a *failing* trial and reboot, so the fall-back path is exercised for real. Refused unless the other slot actually holds an image |

---



## 6. Configuration

Defaults match the browser page:

| Setting | Default |
|---|---|
| SSID / password | `OpenHaldex-C6` / *(empty — the controller's AP is open by default)* |
| Host | `192.168.1.1` (the access-point address; `openhaldex.local` also works) |
| rpm max / amber / redline / shift | 8000 / 6000 / 6800 / 6500 |
| Coolant warn / crit (C) | 100 / 110 |
| Oil warn / crit (C) | 120 / 135 |
| Boost max / warn / crit (mbar gauge) | 2500 / 1800 / 2200 |
| Fuel tank | 62 l |
| Temperature / boost units | C / bar |
| Distance units | km (metric) |
| Day / night | Auto (from the car's Terminal 58s) |
| Brightness (day) | 100 % |
| Poll live / dash / stale | 1000 / 1000 / 1500 ms |

The rev bar's scale is a setting: **max, amber, redline and shift all move in
100 rpm** on the Settings page (max 1000–12000). The tick numbers are placed
from the scale when it is saved, so a 7000 max spreads them wider than an 8000
one, and `x1000` at the right-hand end is always shown — a tick label that would
run under it is dropped rather than overprinted. The bar is 80 cells drawn edge
to edge, so a cell is exactly 100 rpm at the default 8000; the zone tints
quantise to a cell, which at other scales is at most ~75 rpm (about 5 px) away
from the requested edge and not visible at arm's length.

The thresholds are clamped into a sensible order on load and on save —
`amber ≤ redline ≤ max`, `warn ≤ crit` — so a stored set can never disagree with
itself. The shift point is deliberately **independent** and only clamped to the
scale: it starts the bar flash rather than marking a cell, so it can sit
anywhere on the scale, including above the redline. (Up to 0.6.x it was clamped
between amber and redline, which was protecting a default that sat above the
redline — the default was the bug, not the freedom.)

All of it is editable on the device's Settings page — two columns, no scrolling —
and stored in NVS (`Preferences`, namespace `ohxdash`) as one versioned blob.
New fields are appended to the **end** of that struct, so an older blob is a
byte-for-byte prefix of a newer one and is migrated rather than thrown away:
a firmware update that adds a setting keeps everything else, including the static
address the network task learnt on the car. Saving only rejoins the WiFi if the
SSID, password, host or port actually changed.

**No credentials are in git.** For bench work, copy
`include/wifi_secrets.h.example` to `include/wifi_secrets.h` (gitignored) and
fill it in. It is only a first-boot seed — the settings page always wins
afterwards.

### Units

Boost is **millibar gauge** (above ambient). The controller decodes both MQB
`Ladedruck` (0x107) and PQ (0x588) as `(raw * 10) - 1000` and clamps to
`>= 0`, so 1000 mbar ≈ 1.00 bar ≈ 14.5 psi.

There are three toggles in the Settings page's **Units** row and they are
deliberately not one:

* **Temperature** — `°C` or `°F`, shown on the COOLANT and OIL rows and used for
  the two warning thresholds.
* **Boost** — bar, mbar or psi.
* **Distance** — the `km` / `mi` button, which moves the speed, the speed peak and
  the odometer.

**Fuel is always litres.** The car reports litres (`TankContents` is one litre
per count), a tank size is a metric figure here, and "switch to miles" is a
statement about distance rather than about the tank. Converting it would also
leave the number disagreeing with the bar beside it, whose full scale is a litre
figure. So the fuel row reads `22.4 L` whichever distance unit is chosen.

---



## 7. Repository layout

```
platformio.ini              build: ESP32-S3, OPI PSRAM, 4 MB OTA layout
partitions_4mb_ota.csv      two app slots + LittleFS + coredump
LICENSE                     GPL-3.0
AGENTS.md                   short guidance for AI agents working in this repo
README.md                   this file
TODO.md                     deliberately unbuilt, unverified or undecided
assets/
  candash-splash-base-92.jpg  the boot splash's picture, embedded in the app

include/
  board_config.h            panel pins, timing, I2C, expander, touch
  theme.h                   palette, fonts, all layout constants
  lv_conf.h                 LVGL v9 config (partial; defaults fill the rest)
  ohx_proto.h               ESP-NOW wire format - verbatim copy of the
                            controller's shared header; refresh, never edit
  cannow_proto.h            CAN relay wire format - verbatim copy, likewise
  version.h                 version string and its history comment
  wifi_secrets.h.example    optional bench credentials

src/
  main.cpp                  boot banner, loop: ui tick + lv_timer_handler
  hal/
    crowpanel.h/.cpp        RGB panel, LVGL display+indev, backlight, touch
    pca9557.h/.cpp          expander driver + I2C probe
  net/
    telemetry.h/.cpp        WiFi + /api/live + /api/dashboard, core 0 task
    ohx_now.h/.cpp          ESP-NOW listener: decode + counters, which controller
    can_relay.h/.cpp        CAN relay receiver: latest copy of each id, counters
    chassis.h/.cpp          decodes the relayed frames into one Chassis view
    dbc.h/.cpp              look up and decode signals from the tables
    dbc_tables.cpp          the tables, generated from dbc/
    datafeed.h/.cpp         source selection, and how often HTTP is worth asking
  app/
    config.h/.cpp           NVS-backed settings, validation, revision counter
    capability.h/.cpp       what the controller says this car can feed
    ota.h/.cpp              over-the-air update page and the boot trial
    logbook.h/.cpp          RAM ring + the drive-log session and its download
    ram.h/.cpp              5 s internal-RAM and LVGL-pool reporting
    lv_assert_glue.cpp      LVGL's assert handler: print, backtrace, reboot
  ui/
    dash_ui.h/.cpp          the whole dashboard in LVGL, plus settings/splash
    raw_ui.h/.cpp           raw telemetry page: both transports, no
                            interpretation, "--" for a field with no value
    can_ui.h/.cpp           CAN DATA page: every id heard, decoded from the DBC
    splash_img.h/.cpp       the boot splash's JPEG, decoded by the mask ROM
    fonts/
      font_rpm_96.c         96 px rev digits, generated (tools/make_fonts.sh)
      font_num_56.c         56 px boost value, generated
      font_arrow_52.c       52 px indicator arrows, generated

tools/
  make_fonts.sh             regenerate src/ui/fonts from tools/fonts/*.ttf
  make_dbc_tables.py        regenerate src/net/dbc_tables.cpp from dbc/
  decode_can_snapshot.py    decode a console CAN dump against docs/can-relay-sample*.txt
  mockup_dash.py            draw the layout at panel size before coding it
  serial_capture.py         reset + read the console (read-only)
  serial_watch.py           read the console without resetting the board
  serial_shell.py           hold the port open: log + send keys, no reset
  serial_cmd.py             send one console command
  serial_seq.py             drive a timed sequence of console commands
  serial_tune.py            mask sweep: render cost per redraw group
  fonts/                    Montserrat Bold (OFL), FontAwesome 4.7 + licences

dbc/
  vw_pq.dbc, vw_mqb.dbc     from opendbc (MIT), with English names
  SOURCE.md                 provenance, commit and licence
  LICENSE                   opendbc's MIT licence

docs/
  architecture.md           data flow, capability, transports, limits
  display-and-memory.md     panel tuning, the splash picture, internal RAM
  console.md                serial console keys and the tools around them
  status.md                 what is verified where, and the build size
  logging.md                the drive log: what it records, and what it costs
  can-relay-test.md         how to measure the CAN relay in the car
  can-relay-sample*.txt     recorded relay dumps for decode_can_snapshot.py
  redesign-bars.md          why the top band is a bar, and its measurements
  esp-now-plan.md           the ESP-NOW transport, its risks and phases
  upgrades.md               history and evidence behind the 0.6.0 batch
  upstream-steering-report.md  ready-to-paste report about Gen1 steering
  mockups/                  design history: the layout mock-ups and the
                            splash's source JPEGs
```

`tools/make_fonts.sh` is only needed to *regenerate* the three generated fonts.
LVGL ships Montserrat up to 48 px and no filled arrow, so the 96 px rev digits,
56 px boost value and two FontAwesome block arrows are generated from the TTFs
in `tools/fonts/`; the output is committed, so a normal build needs neither node
nor the script.

## Licence

This project is licensed under the **GNU General Public License v3.0 (GPL‑v3)**, which requires that any derivative work also be distributed under the same license. The full text is in the `LICENSE` file.

It depends on **[OpenHaldex-C6](https://github.com/Forbes-Automotive/OpenHaldex-C6)**
at runtime and copies its wire-format header verbatim — see that project for
its terms. The bundled Montserrat Bold font is under the SIL Open Font
Licence, reproduced in `tools/fonts/OFL.txt`. LVGL, the GT911 library,
ArduinoJson and the ESP-IDF are all used under their own licences.

Read this when you want a drive trace off the dash, or want to know why it is
built the way it is.

# Drive logging

The dash keeps two kinds of history, and they are deliberately different
things.

| | RAM ring | Session |
|---|---|---|
| Runs | always | only when started |
| Where | PSRAM, ~19 KB | `/littlefs/drive.csv` in the 128 KB data partition |
| Rate | 5 Hz | 5 Hz |
| Depth | last ~40 s | up to 100 KB, ~8 minutes of driving |
| Cost | nothing measurable: no flash, no stall | one ~900 B flash write every ~4 s while running |
| Start / stop | nothing to do | SETTINGS → **START LOG**, or `l` on the console |

Starting a session dumps the ring into the file first, so the file begins with
the seconds *before* the button was pressed.

## Why not log to flash all the time

The partition is **128 KB** (`partitions_4mb_ota.csv`, offset `0x3d0000`). A row
from the car measures **42 B** including its newline, so a rolling log would
hold about eight minutes and then rewrite the whole partition — some five
thousand rewrites of the partition a year for a two-hour driving day, on a
100 000-cycle part, for a trace nobody asked for.

The second cost is stalls, not wear: every flash erase or program turns the
instruction cache off for both cores for tens of ms. That is a dropped frame on
the panel and a late ESP-NOW or CAN-frame interrupt. Batching the writes into
~1 KB makes it rare; nothing makes it free. So the ring (PSRAM only) is always
on and the flash writes are something a person starts.

While a session runs, the status bar's odometer slot reads **REC** instead of
`ODO`, and the settings page shows the percentage and clock. A dash writing to
flash without saying so would be a bad dash.

## The file

`/littlefs/drive.csv` (the partition's root, `drive.csv` to the filesystem
API), comma separated, one row per sample, `\n` line endings, a header row
first:

```
ms,rpm,speed_kmh,throttle_pct,boost_mbar,coolant_c,oil_c,fuel_l,mode,lock_tgt,lock_act,steer_deg,brake_can,hand_can,cruise,src
```

* `ms` — milliseconds since boot, not wall clock. The dash has no RTC.
* A value the car does not have is an **empty field**, never a `0`. NaN on the
  glass and an empty CSV field are the same statement.
* `brake_can` / `hand_can` / `cruise` / `mode` / locks are integers; empty where
  the source has no value, and `cruise` carries the car's own states (0 off,
  1 armed, 2 regulating) rather than a boolean.
* `src` is which transport the fast values in that row came from: `0` nothing
  yet, `1` ESP-NOW, `2` HTTP. A row that steps because the source changed is
  readable as such instead of looking like a fault.
* `steer_deg` is the merged value ([datafeed.cpp](../src/net/datafeed.cpp)):
  the packet wins, because `/api/dashboard` gates the field off on a Gen1 car.
  `src` still says which transport the row's *fast* values came from.

VALUES ARE NOT UNIT-CONVERTED: speed is km/h and boost is mbar whatever the
settings page says, so one file from two cars stays comparable.

## Getting the file out

The update page already runs an HTTP server on the dash's own address, so a
phone on the controller's access point can take the file:

1. SETTINGS shows the address under **UPDATE** (`http://ohx-dash.local/` or the
   IP).
2. Open it, find **Drive log**, press **Download drive.csv**.
3. `Start log`, `Stop log` and `Delete` are on the same page.

`/log.csv` serves the file directly if a script wants it. Nothing needs a USB
cable, and downloading does not stop the session: the writer gives up its turn
while the download holds the filesystem, then carries on.

## Limits and failure behaviour

* A session stops itself at **100 KB** or **ten minutes**, whichever comes
  first — 100 KB of a 128 KB partition, because the filesystem needs room to
  append without compacting on every write.
* A partition that will not mount is **left alone**, because it may still hold
  the log somebody is trying to rescue. The console says so and the settings
  page shows **no filesystem** next to the button. The one exception is a
  partition that reads back blank: there is nothing to lose, so it is formatted
  once at boot and the console says `[log] data partition is blank - formatting
  it once`. This is what a new board needs — it ships with the region erased,
  and LittleFS reports that as corruption (`Corrupted dir pair at {0x0,0x1}`).
* To format on purpose — a partition holding something that is not a readable
  filesystem — use **`F`** on the console (always available) or the **Format
  partition** button on the update page (`POST /log/format`). The button is only
  drawn in that state: while the filesystem is mounted the page offers Start,
  Stop, Delete and Download instead, because erasing a partition that may hold
  the log you are about to download should not be one tap away. The route
  itself is always registered.
* A short write (the partition is full) closes the session cleanly and reports
  it rather than retrying the tail forever.
* Rows lost while the filesystem was busy are counted and printed when the
  session ends (`[log] ... row(s) lost to a busy filesystem`). Losing a tick is
  preferred to stalling the panel.

## Console

| Key | Effect |
|---|---|
| `l` | start / stop a session — same as the SETTINGS button |
| `L` | state, file size, ring depth, rows dropped |

## What has been checked, and on what

Two runs on a CrowPanel 7.0, both flashed over USB.

**On the bench, with no controller in range** (every value absent, every `src`
`0`):

* The blank partition formatted itself at boot and mounted; 8 KB of the 128 KB
  is metadata, which is why `usedBytes()` reads 8192 with no log in it.
* The ring filled at 5 Hz in PSRAM — 48 rows after ten seconds.
* A session wrote the header plus **19 rows of pre-trigger**, then 1544 B in
  ten seconds with **0 rows dropped**; `l` again closed it and reported the
  size.
* The file read back out of flash (a raw dump of `0x3d0000`) contains the
  header and one row per sample with **empty fields**, not zeros.
* `F` formatted the partition back to a clean 8192/131072 B.
* The dashboard kept 68 fps while the ring was sampling, and internal RAM did
  not move (160 KB free).

**With the car powered up** (ignition on, engine off, so `rpm` and road speed
are 0 but the controller and the CAN bus are live):

* The dash joined `OpenHaldex-C6` on its fallback static address
  (`192.168.1.203`, no DHCP on that access point) at rssi −59…−68 dBm.
* ESP-NOW ran at 7–9 Hz and the CAN relay at 104–124 frames/s over 17 ids, with
  17–34 % air loss through a wall — enough for this test, not a car test.
* A session wrote **122 rows of pre-trigger** (the ring held no more: opening
  the console port resets the board), then 8853 B by 0:19, and closed at
  **10 057 B with 0 rows lost**. Render rate stayed at 66–68 fps throughout.
* The CSV dumped out of flash has `src=1` on the sampled rows and an **empty**
  `boost_mbar` — a Gen1 car has no boost frame, and the empty field says so
  instead of the row reading `0`.
* Row size: header 126 B, mean row **42.0 B** including the newline. The sample
  interval measured 199–316 ms, mean **207 ms**, so the real rate is ~4.8 Hz and
  100 KB is about **8 minutes** — which stops the session before the ten-minute
  cap does. With nothing connected the rows shrink to ~20 B, and then the clock
  binds instead, at about 60 KB.
* A 42 B row is a row with short numbers. A moving car has a 4-digit rpm, a
  3-digit speed and 2-digit temperatures, so expect rows a little longer and a
  session a little shorter than eight minutes — not longer.

**From a phone on the controller's access point** (`http://192.168.1.203/`, the
owner's check, same 0.7.0 build):

* **Download drive.csv** — the file came down with the rows in it.
* **Start log** and **Stop log** — the session started and stopped from the page.
* **Delete** — the stored file went away and the page said so.
* **Format partition** was not offered, which is correct rather than missing: the
  button is only drawn when the partition holds no readable filesystem, and this
  one was mounted and holding a log. The format path itself was exercised on the
  console (`F`, bench run above) and the route is registered unconditionally.

The bench machine still cannot reach the dash's access point (its own LAN is
`192.168.1.0/24`, the same subnet as the dash's fallback address), so every HTTP
check above was a person with a phone, not a script. Unmeasured: the flash stall
with a heavier frame stream under it — 66–68 fps is what a car sending 17 CAN
ids produced, not the worst case — and nothing has yet been logged while the car
is actually moving.

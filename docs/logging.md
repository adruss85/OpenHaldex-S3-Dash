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
| Depth | last ~40 s | up to 100 KB, ~5 minutes of driving |
| Cost | nothing measurable: no flash, no stall | one ~1 KB flash write every ~3 s while running |
| Start / stop | nothing to do | SETTINGS → **START LOG**, or `l` on the console |

Starting a session dumps the ring into the file first, so the file begins with
the seconds *before* the button was pressed.

## Why not log to flash all the time

The partition is **128 KB** (`partitions_4mb_ota.csv`, offset `0x3d0000`). At
5 Hz a row is ~65 bytes, so a rolling log would hold about six minutes and then
rewrite the whole partition — roughly 7 000 erase cycles per block for a year of
two-hour driving days, on a 100 000-cycle part, for a trace nobody asked for.

The second cost is stalls, not wear: every flash erase or program turns the
instruction cache off for both cores for tens of ms. That is a dropped frame on
the panel and a late ESP-NOW or CAN-frame interrupt. Batching the writes into
~1 KB makes it rare; nothing makes it free. So the ring (PSRAM only) is always
on and the flash writes are something a person starts.

While a session runs, the status bar's odometer slot reads **REC** instead of
`ODO`, and the settings page shows the percentage and clock. A dash writing to
flash without saying so would be a bad dash.

## The file

`drive.csv`, comma separated, one row per sample, `\n` line endings, a header
row first:

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
* If the partition will not mount at boot, logging is **disabled and the
  partition is not formatted**. A partition that will not mount may still hold
  the log somebody is trying to rescue; the console says so
  (`[log] littlefs will not mount`).
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

## What is not verified

Everything in this page was built and compiled but has **not** been run on a
board yet: the LittleFS mount, the 5 Hz sampling against a real frame rate, the
flash stall in practice, and the download from a phone. The numbers above are
arithmetic on the partition layout and the row size, not measurements.

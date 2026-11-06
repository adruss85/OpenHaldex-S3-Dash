# Testing the CAN relay in the car

What has to be measured before the relay can be called finished, and how. Nothing
here can be answered on the bench: the frame rate depends on real radio traffic,
and the losses depend on where the boards sit in the car.

## Before you start

* Controller on `V11.00.x` or later.
* In the controller's web UI: **Controller Options > Relay CAN Frames to Gauges**.
* Serial console on the dash at 115200 (`tools/serial_shell.py`, so the port is only
  opened once). The lines that matter come every 5 s:

```
[stats] render 28.4 fps | ...
[stats] source esp-now (packet age 27 ms, http up, live every 0 ms, dash every 2000 ms)
[stats] esp-now 418 pkt (9.7 Hz), age live, rej 0 dup 0 gap 3, ignored 0, ch 1, rssi -76 dBm
[stats] relay 1843 f/s, 61 ids, 4210 pkt, air lost 12 (0.3%), sender lost 0, rej 0, full 0
```

The same numbers are on the glass: **SETTINGS > RAW DATA** (`CAN RELAY`) and
**SETTINGS > CAN DATA** (the title line).

## 1. Frame rate and losses, at each relay setting

Engine running, dashboard on the glass, 2 minutes at each setting. Change it in the
controller's web UI; the dash needs no change. Read the last `[stats]` block.

| Relay setting | render fps | esp-now Hz | rssi | relay f/s | air lost % | sender lost |
|---|---|---|---|---|---|---|
| Off | | | | - | - | - |
| Each ID up to 10 per second | | | | | | |
| Each ID up to 20 per second | | | | | | |
| Each ID up to 50 per second | | | | | | |
| Every frame | | | | | | |

What the numbers mean:

* **render fps** is the one to watch. The panel's memory is shared with the WiFi
  buffers, so more packets can mean fewer frames. If *Every frame* costs too much,
  the dashboard only needs *Each ID up to 20 per second*, and that goes in the README
  as the recommended setting.
* **esp-now Hz** should not drop when the relay is on. If it does, the switch to
  6 Mbit/s is costing range: the controller sends everything at that rate while
  relaying, including the status packet.
* **air lost** is packets that never arrived. Broadcast is not resent.
* **sender lost** is frames the controller could not fit in its queue. Anything but
  0 here means the controller was too busy to relay, and that is the number to send
  back to OpenHaldex.

## 2. The HTTP reduction

On a PQ car with the relay on, the `source` line should read `live every 0 ms, dash
every 2000 ms`. Turn the relay off and within a second or so it should read `live
every 5000 ms, dash every 1000 ms`, and the coolant and oil tiles should carry on
without a gap.

## 3. The values against the controller's own

Relay on, compare the dash's tiles with the controller's web UI:

| Value | On the dash | In the web UI | Match? |
|---|---|---|---|
| Coolant | | | |
| Engine oil | | | |
| BRAKE lamp, pedal on and off | | | |
| HAND lamp, lever up and down | | | |

**MQB only:** the coolant and oil here come from `Motor_07` (0x640), which the
controller does not read, so there is nothing in its web UI to compare with. Compare
with the car's own cluster, and report what you see.

## 4. The CAN DATA page

* The list fills with the car's ids, and names appear beside the ones the DBC knows.
* `Cluster_2` (PQ, 0x420) or `Engine_07` (MQB, 0x640): the coolant signal matches the
  tile.
* The page stays usable with the engine revving, and the dashboard's frame rate comes
  back when the page closes.

## 5. Recorded so far (2026-11-06)

Gen1 PQ, controller fw 11.0.4, relay at **Each ID up to 10 per second**, dash on
0.5.1 with all three pages resident.

| Measured | Result |
|---|---|
| Ids relayed | 17, holding steady; `full 0` — no id got dropped |
| Frame rate | 121-153 f/s, with 0x0C2 capped just under 10/s as the setting intends |
| Sender lost | 0, rejected 0 — the controller always had queue room |
| Air lost | 5.2% parked beside the panel; 16-26% with a 3D printer between the car and the panel (both links, ESP-NOW status included, degraded together) |
| HTTP reduction | `live every 0 ms, dash every 2000 ms` with the relay on, with no gap in the coolant and oil tiles |
| Values vs the controller's API | Coolant 30 C, oil 8 C, handbrake 1, brake 0 — all matched |
| Gen1 steering | 0x0C2 `LW1_LRW` arrives over the relay while `/api/dashboard` returns null |
| Render rate | Dashboard 60 fps, CAN DATA page 41-45 fps, RAW page 13-30 fps |
| CAN DATA page | Opens and closes repeatedly with the panel staying up |

Two captures from the same car, with the console dump in full, are kept so the
frames can be re-read without a car, controller or CAN hardware:

* `docs/can-relay-sample.txt` — engine off, ignition on (17 ids, fuel 25 l)
* `docs/can-relay-sample-engine-running.txt` — engine idling (17 ids, 754 rpm,
  coolant 77 °C, fuel 22 l) — same ids as above, so a running engine changes the
  values in those messages rather than adding new ones

Each has a `-decoded.txt` beside it and `tools/decode_can_snapshot.py` re-decodes
either one against `dbc/vw_pq.dbc`.

## 6. Fixes in the same release, worth checking while you are there

* Change the brightness, power-cycle, and it stays changed.
* Press **DAY**. The status light stays LIVE, with no `[net] joining` on the console.
* Drop the controller's access point (change its WiFi settings, or restart it). The
  packets come back without a reboot of the dash, and `[feed]` returns to `esp-now`.

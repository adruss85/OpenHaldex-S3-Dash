Read this when you need to know where each value comes from, which transport carries it, and what this car can supply.

# Architecture

## Data flow

Three things feed the dashboard. **ESP-NOW is the primary** — the
controller broadcasts a packed status packet every 100 ms with no web stack
involved, so there is no association, no socket and no TCP in that path. With
its **CAN relay** switched on, it broadcasts the car's frames the same way. HTTP
is the fallback, and it still carries what neither of those has.

```
        core 0                                 core 1
+-----------------------------+        +---------------------------+
| "ohxnet" task               |        | loop()                    |
|   WiFi STA -> C6 HTTP API   | struct |   ui::tick()     ~30 Hz   |
|   GET /api/live     0-1 Hz  | --+    |   feed::poll()            |
|   GET /api/dashboard ~1 Hz  |   |    |   lv_timer_handler()      |
| ESP-NOW receive (radio, no  |   +--> |---------------------------|
|   association, no sockets)  |        | net/datafeed.cpp: choose  |
+-----------------------------+        |   source, merge fields    |
      ^  ~10 Hz, no HTTP              | LVGL -> 2x 800x480 PSRAM  |
      |                               | frame buffers -> RGB DMA  |
   C6 broadcasts OHX_STATUS           +---------------------------+
   (and CAN relay packets)
   to FF:FF:FF:FF:FF:FF
```

`src/net/datafeed.{h,cpp}` is the single place that decides provenance:

| Field | Source |
|---|---|
| `rpm`, `speed`, `throttle`, `boost`, `lockTarget`, `lockActual` | ESP-NOW while packets arrive (holds 500 ms), else HTTP |
| `mode`, `haldexState`, `chassisCAN`, `haldexCAN`, `brakeIn`, `handbrakeIn`, `couplingOpen`, `tempProtection` | ESP-NOW — the packet has real bits for all of these |
| `coolant`, `oil`, `fuel`, `odometerKm`, `brakeFromCAN`, `handbrakeFromCAN` | the CAN relay while it is carrying them, else HTTP. The packet has none of these: its `oilTemp` is the Haldex's oil, not the engine's |
| `fuelLow`, `indicators`, `term58s`, `asrLamp`, `asrActive`, `cruise` | the CAN relay only — the cluster's own frame, with no HTTP equivalent at all |
| `tcOn`, `asrOn` | the relay's `BR1_Lamp_ASR`; the packet's `TC_FORCE` / `ASR_OFF` flags (which is how the controller's API works them out) are only the fallback while the frame is quiet |
| `awdWarning` | always HTTP — it comes from Haldex data, which is on the bus the relay does not carry. Kept and shown on the raw page; nothing on the dashboard uses it |
| packets stopped | everything falls back to HTTP, and says so on the console |

A numeric field the packet marks as "no data" falls back to the HTTP value if
HTTP has one, so a field this Haldex generation never fills in cannot make the
dashboard worse than it was. The source in use is printed in the stats line
and on every change (`[feed] values now come from esp-now`), and `e` on the
console dumps both transports side by side.

HTTP is only asked for what nothing else is delivering, because every request
costs the panel frames and costs the controller's single core time:

| | `/api/live` | `/api/dashboard` |
|---|---|---|
| Packets stopped | 1000 ms: everything | 1000 ms: everything |
| Packets, no relay | 5000 ms: coolant and oil | 1000 ms: brake, handbrake, AWD flag |
| Packets and relay | 1000 ms: coolant, as a cross-check | 2000 ms: nothing the glass needs |

The rate in force is on the `[stats] source` line. The fallback starts within
100 ms of the packets stopping, however slow the previous rate was.

**With the relay on, `/api/live` is still polled at 1 Hz for one reason: it is
the cross-check.** Everything on the glass arrives over the relay, so the HTTP
value is only there for `e` to compare against — and it costs one request a
second, against the eleven a second (10 + 1) the 0.5.0 build made.

`/api/dashboard` is polled at 2 s even though the glass needs nothing from it:
it is the only handler that calls `otaNoteWebActivity()` on the controller, and
the controller drops its access point after about five minutes without it.

Because ESP-NOW needs no association, the dashboard goes **LIVE before the WiFi
join completes** — `[ui] LIVE (data age 21 ms, source esp-now)` while
`http down`.

### Why HTTP is still there

Three reasons, all deliberate: without the relay, coolant, engine oil, the fuel
level and the odometer are not on the air at all; if the controller stops
broadcasting (older firmware, ESP-NOW disabled, out of range) the dashboard
keeps working at 1 Hz instead of going dark; and the station has to stay joined
because the over-the-air update page lives on the controller's address.

### Why two poll rates

`/api/dashboard` on the controller calls `getCPUUsagePercent()`, which mallocs
and walks every task on the controller's **single** core. Polling that faster
buys no extra visual resolution. Splitting fast and slow data keeps the same
look for roughly 90 % less CPU on the controller.

`/api/dashboard` is also never polled slower than **2 s**: it is the only
handler that calls `otaNoteWebActivity()`, and the controller drops its access
point after about five minutes without it. The settings page clamps
`pollDashMs` to 250..2000 ms for exactly this reason.

### Joining the controller's access point

The OpenHaldex-C6 publishes an **open** access point by default, which two
defaults in the Arduino core have to be worked around:

* `WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK)` is the core's default, so an open
  network is *below* the threshold. The station is therefore told
  `WIFI_AUTH_OPEN` before `WiFi.begin()`.
* Association and DHCP are separate problems. The board associates fine and
  then never gets a lease, which looks identical to "AP is dead" from outside.
  The cause is that the controller is usually *also* on a home LAN already
  using `192.168.1.0/24` — its softAP and station interfaces then share a
  subnet, and its DHCP server cannot hand out an address that looks free. The
  firmware logs `associated but no DHCP lease` and retries with a static
  address derived from the configured host (`192.168.1.203`), skipping DHCP
  entirely. That suits a fixed install: the dash talks to one AP at one
  address.

The join loop prints BSSID, channel and RSSI, which only read back once the
station is associated, so the two failure modes can be told apart.

### Connection reuse

Both pollers use `HTTPClient::setReuse(true)`, so the two requests per second
share one TCP connection instead of opening two. That matters: the controller
runs with `CONFIG_LWIP_MAX_SOCKETS=8`, and a handshake-per-request pattern
would fill its socket table with `TIME_WAIT` entries.

---

## What the car can fill

The dashboard's shape is decided once at boot from what the controller says it
can feed, not fixed for every car. Two ways of finding out, and which was used
is shown on the raw page so it can be argued with:

| | How | Why |
|---|---|---|
| **detected** | the packet marks a field it has no value for with a sentinel, which arrives as NaN | the car is telling us, rather than us assuming |
| **table** | the Haldex generation | boost is the one field with no sentinel: a car with no boost frame reads a confident `0.00`, so it cannot be detected |

Measured on the Gen1 car this was built against, and logged as one line at
boot:

```
[cap] gen 1 | boost no (table) | supply no (detected) | haldex temps no
(detected) | steering yes (detected) | NoBoost (A) | 20 pkts
```

| Capability | This car | Consequence |
|---|---|---|
| boost | absent | the BOOST panel is not built into the band |
| supply voltage | absent | not shown |
| Haldex oil / clutch / module temps | absent | not shown |
| steering angle | **packet only** | the packet is authoritative; the JSON path gates it off for Gen1 |
| handbrake | **chassis CAN only** | the lamp lights from the CAN bit; the wired input never moves on this car |
| brake | wire, and CAN | the lamp lights from either |

Two tile bands exist, both approved as mock-ups in `docs/mockups/`:

* **NoBoost (A)** — SPEED | TEMPERATURES | HALDEX, three equal panels (266 / 266
  / 268), for a car with no boost source. What this car gets.
* **Full (B)** — SPEED | BOOST | TEMPERATURES | HALDEX, the widths `dash.html`
  gives them, for a car that has one. **Unverified**: no Gen4/5 car has been
  available to test it on.

The decision latches. It is not re-evaluated per frame, because a layout that
changed shape while the car was moving would be worse than a widget that reads
`--`, and an unsettled dashboard keeps the full band rather than hiding a widget
on a guess. `v` on the console forces either band, which is how the unverified
one gets looked at.

## ESP-NOW

The controller broadcasts a packed `OhxStatus` struct to the broadcast address
every 100 ms with **no web stack involved** — no access-point association, no
sockets, no HTTP. This firmware listens to it and it carries most of the numbers
on the glass; HTTP stays for the values the packet has no field for — coolant,
oil, the fuel level and the odometer arrive over the CAN relay instead
(see [Data flow](#data-flow)) — and as the fallback if the packets stop.

| | |
|---|---|
| Wire format | `include/ohx_proto.h`, a **verbatim copy** of the controller's shared header (`b54d9a1d`). Refresh it from upstream; never edit it. |
| Listener | `src/net/ohx_now.{h,cpp}` — init, receive callback, decode, counters |
| Source choice | `src/net/datafeed.{h,cpp}` — ESP-NOW while packets arrive, HTTP otherwise |
| Build switch | `-DOHX_USE_ESP_NOW=1`; `0` compiles a build that only ever uses HTTP, and the listener is not initialised at all |
| Console | `e` = every field, ESP-NOW beside HTTP. `E` = enable/disable the listener at runtime |
| Stats | `[stats] source esp-now (packet age 27 ms, http up)` and `[stats] esp-now 418 pkt (9.7 Hz), rej 0 dup 0 gap 3, ch 1, rssi -33 dBm` |
| Every change | `[feed] values now come from esp-now` / `... from http` |

84-byte packets arrive at 9.7–10.6 Hz with 0 rejects and ~0.5 % sequence gaps
— broadcast has no retransmission, so gaps are expected. Listening costs
nothing in render rate: the loop stays at ~60.2 ms busy out of a 61.0 ms
period with the listener on or off.

### Which controller

A broadcast reaches every dash in range, and two controllers can share the
default SSID. So the first one heard whose name matches the configured SSID is
remembered by MAC, in NVS, and from then on:

* packets from anyone else are counted (`ignored`) and dropped, with one console
  line per MAC;
* commands are only ever sent to that MAC;
* the station's join is aimed at that MAC, so the HTTP values cannot come from a
  different car than the ESP-NOW ones.

A replaced controller is picked up by changing the SSID, pressing **DEFAULTS** in
Settings, or `f` on the console. The first boot takes whichever matching
controller it hears first, so do it away from other cars.

### In the car

`e` is the cross-check that matters: it prints both transports
field by field, so any field whose scaling differs — `boost` is the one to
watch, since the packet documents it as "mbar above ambient" — is visible
rather than guessed at.

### Sending: the mode switch

The same wire format carries the other way. **Tap anywhere on the HALDEX
panel** and the mode steps on: Stock → FWD → 50:50 → 60:40 → 75:25 → Expert →
Stock. A standalone controller has no Stock to go to (it answers by staying on
its last mode), so there the step goes from Expert back to FWD. One tap, one
step — no list, no confirmation, no modal. `m` on the
console does exactly the same thing, which is how the path gets exercised
without touching the glass.

The command is 22 bytes — magic, type, proto, the controller's nonce, a
per-dash sequence, the command byte and its arguments — signed with
**HMAC-SHA256**, truncated to 8 bytes. The key is the controller's
access-point password: the same string the station joins with. It is unicast
to the [remembered controller](#which-controller), and this is the only thing
this firmware ever transmits.

The controller does not reply directly. It publishes the answer in the ack
fields of its next broadcast (`ackSrc`, `ackSeq`, `ackResult`), so the
verdict arrives with the same 10 Hz stream as the data, and the panel's
sub-line reports both halves of the exchange:

```
[ui] mode -> FWD (sent)
[ui] mode FWD: ok (ack after 60 ms)
```

| On the panel | Meaning |
|---|---|
| `FWD ...` | sent, waiting for the controller |
| `FWD ok` | accepted and applied |
| `FWD control off` | ESP-NOW commands are switched off on the controller |
| `FWD wrong key` | the password here differs from the one on the controller |
| `FWD no link` | nothing was sent: no packet heard from the controller yet |
| `FWD no reply` | sent, but nothing came back within 1.5 s |

A `stale` result (another command — the web UI, a second gauge — used the
nonce first) earns **one** silent retry, because the nonce is refilled every
100 ms and a second race would just lose the same way.

**An open access point is a warning, not a gate.** With no password the HMAC
is keyed with an empty string, and the controller accepts commands signed that
way if it has commands enabled — this is the state the reference car was in
when the table above was captured. Settings says so in plain words (`signing
key: NONE` plus a warning line) and leaves the switch working, because whether
a command is obeyed is the controller's decision and the result code reports
it honestly. The nonce is broadcast, so on an open access point anyone in
range can compute a signature; the visible, immediate mode change on the panel
is what makes that noticeable.

Full plan, risks and phase notes: [`esp-now-plan.md`](esp-now-plan.md).

### The CAN relay

Switch on **Relay CAN Frames to Gauges** in the controller's web UI (Controller
Options; it is off by default) and it broadcasts the frames it receives on its
chassis bus, batched into packets. The dash needs no setting: it picks them up
as soon as they arrive, from its own controller only. That gives it:

* **less HTTP**, because the tiles and lamps can read the car directly (see
  [Data flow](#data-flow));
* **coolant and oil on MQB**, which the controller's API does not report. Read from
  `Motor_07` (0x640), and not yet checked on a car;
* **what the packet and the API have no field for at all**: the fuel level and its
  low-fuel warning, the odometer, the turn-indicator bits and Terminal 58s (the
  lighting switch and dimmer, which drives [day/night](display-and-memory.md#display-tuning));
* **the CAN DATA page**: **SETTINGS → CAN DATA**, or `c` on the console. Every id on
  the bus with its name and rate. Tap one to see every signal in it, decoded.

The names and scaling come from tables compiled into the firmware: `vw_pq` (Gen1,
Gen2, Gen4, 0AY) and `vw_mqb` (Gen5), about 72 KB of flash for 199 messages and 2612
signals. They are generated by `tools/make_dbc_tables.py` from `dbc/`. Other makes
show raw bytes. A compiled table beats a file on the LittleFS partition because the
update page only updates the app.

The tiles and lamps do **not** go through the DBC. They read the same bytes the
controller reads, because those have been proved on a car and a DBC can be wrong:
`vw_pq` lumps the handbrake in with the brake-fluid warning as one 2-bit field.

What the relay does not carry:

* **Bus 1**, so the Haldex's own frames. Its lock and status arrive in the status
  packet instead.
* Anything the controller transmits itself, including the edited frames it sends
  the Haldex. What is shown is what the car sent.

Broadcast has no retransmission, so this is a live view, not a lossless log. The
counters say how much went missing: `air lost` is packets that never arrived,
`sender lost` is frames the controller could not fit in its queue.

**Not yet measured in a car**: what the relay costs in render rate and how much it
loses. The test is written up in [`can-relay-test.md`](can-relay-test.md).

---

## What this device cannot show

Data the controller does not expose, or has no room for here:

* On the dashboard itself: engine oil **pressure**, fuel pressure, individual wheel
  speeds, gear position and clutch state. Where the car's powertrain CAN has them,
  the CAN DATA page shows them with the relay on.
* Battery voltage — this *is* available, but only in the ESP-NOW broadcast
  (`supplyMv`), not over HTTP. The dash receives it; it has no space on the
  layout yet.
* Anything at all while the chassis CAN is silent: on the bench with no
  vehicle, the controller reports `rpm`/`speed`/`throttle`/`boost` as JSON
  `null`, and this firmware correctly shows `--`.

---

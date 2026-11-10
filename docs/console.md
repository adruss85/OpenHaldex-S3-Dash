Read this when you need to drive or diagnose the board from the serial port.

# Serial console

115200 baud on UART0 (the CH340). Besides the boot banner:

| Key | Effect |
|---|---|
| `1`..`5` | toggle redraw groups: rev bar, tick numbers, big number, tile readouts, status bar |
| `0` / `9` | all redraw groups on / off |
| `p` | cycle the pixel clock: 24 → 20 → 16 → 12 MHz (the first press goes up from 16) |
| `d` | print the frame buffer the panel is scanning as an ASCII luminance map |
| `n` | pause / resume the HTTP polling |
| `e` | ESP-NOW vs HTTP, every field side by side |
| `f` | forget the remembered controller (see [Which controller](architecture.md#which-controller)) |
| `x` | open / close the raw telemetry page (all of the above on the glass) |
| `c` | open / close the CAN DATA page |
| `C` | print every CAN id heard, and the selected one's signals |
| `v` | force the other tile band, for bench work (see 'What the car can fill') |
| `m` | step the Haldex mode — the same as tapping the HALDEX panel |
| `E` | enable / disable the ESP-NOW listener (A/B its cost on one boot) |
| `k` | decode the latest chassis CAN frames: fuel, indicators, odometer, 58s, brake, lamps, cruise |
| `b` | toggle the bounce buffer between 0 and 16 lines |
| `r` | try the next GT911 axis mapping |
| `t` | reset the touch probe |
| `s` | open / close the settings page (the one page with no other way in but touch) |
| `l` | start / stop a drive-log session (see [Drive logging](logging.md)) |
| `L` | drive log: state, file size, ring depth, rows dropped |
| `F` | format the data partition — erases it; only needed when it holds no readable filesystem |
| `o` | where the update page is, which slot is running, whether it is on trial |
| `O` | arm the next boot as a deliberately failing trial (see [OTA](../README.md#5-updating-it-in-the-car-over-the-air)) |
| `h` | the list above |

`d` is the bench substitute for a camera: `tools/serial_cmd.py d 34` prints
what is on the glass, which is how the layout was verified without a screen in
front of you. `tools/serial_seq.py "35:d,42:n,48:d,56:n"` drives a whole timed
scene in one run.

`x` (or **SETTINGS → RAW DATA**) opens the **raw telemetry page**: every value
the dash can see, on both transports, with a field the C6 marks as "no value"
drawn as `--` rather than `0`. It is the instrument for a road test — what the
packet carries versus what the JSON carries, side by side — so a problem can be
read off the panel or pasted from the console instead of being described.

The page prints itself to the console when it opens and whenever **FREEZE** is
pressed, which holds the last reading on the glass long enough to photograph.

To stage a **stale** screen, both transports have to be stopped — `E` (stop
listening) then `n` (stop polling). Either one alone leaves the dashboard live,
which is the point of the fallback; `[stats] source ...` says which one is
carrying it.

> **Opening this board's serial port reboots the ESP32.** That is measured,
> not assumed: with a 12 s gap between two runs, the second still begins with a
> full boot banner, so the reset comes from the kernel asserting DTR/RTS as the
> port is opened, before any userspace code can touch the lines. Nothing held in
> RAM survives it. `tools/serial_shell.py` therefore opens the port **once** and
> keeps it, so a whole session of gestures and console keys is not reset halfway
> through, and `tools/serial_capture.py` is the one to use when a reset is
> exactly what you want.

---

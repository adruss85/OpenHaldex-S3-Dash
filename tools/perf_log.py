#!/usr/bin/env python3
"""Log the dash console with a wall clock, for a drive.

The 5 s `[stats]` block is the only place the dash reports what it is doing:
render fps, CAN relay f/s, ESP-NOW Hz, source, RAM. A raw serial_shell log has
no clock in it, so "it stuttered about ten minutes in" cannot be checked
afterwards. This prefixes every line with the time of arrival, survives the port
disappearing (a car USB socket re-enumerates on an ignition cycle), and writes
lines that tools/perf_report.py can read back.

usage: perf_log.py <logfile> [seconds] [port] [baud]
       seconds = 0 logs until stopped (Ctrl-C closes the port cleanly).
       --keys K --every N    send a console key every N seconds. Repeat the pair
                             for more than one key: --keys k --every 30 --keys e
                             --every 5. The port is held open by this tool, so
                             nobody can type; "k" every 30 s is how a lamp decoded
                             wrongly is caught after the fact, and "e" every 5 s
                             puts rpm beside the fps lines so a dip can be tied to
                             the shift flash.
"""
import sys
import time

import serial

from serial_port import default_port

# (payload, period seconds) pairs, in the order they were given. A --keys with
# no --every after it is dropped: one-shot keys are not what this is for.
keys = []
args = []
i = 1
while i < len(sys.argv):
    if sys.argv[i] == "--keys" and i + 1 < len(sys.argv):
        if i + 3 < len(sys.argv) and sys.argv[i + 2] == "--every":
            keys.append(((sys.argv[i + 1] + "\n").encode(), float(sys.argv[i + 3])))
            i += 4
        else:
            keys.append(((sys.argv[i + 1] + "\n").encode(), 0.0))
            i += 2
    else:
        args.append(sys.argv[i])
        i += 1
keys = [(k, p) for k, p in keys if p > 0.0]

path = args[0] if len(args) > 0 else "ohx-drive.log"
secs = float(args[1]) if len(args) > 1 else 0.0
port = args[2] if len(args) > 2 else default_port()
baud = int(args[3]) if len(args) > 3 else 115200


def open_port():
    # DTR/RTS de-asserted before and after open: the board's auto-reset circuit
    # watches them, and the point of this tool is to watch a running dash.
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = False
    return ser


def stamp():
    return time.strftime("%H:%M:%S")


end = time.monotonic() + secs
partial = b""
with open(path, "a", buffering=1, encoding="utf-8", errors="replace") as log:
    log.write("=== perf_log open %s\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
    while secs == 0.0 or time.monotonic() < end:
        try:
            ser = open_port()
        except (serial.SerialException, OSError) as exc:
            log.write("=== %s port unavailable (%s), retrying\n" % (stamp(), exc))
            time.sleep(2.0)
            continue
        try:
            # Record what is running with the log. This tool holds the port, so
            # the boot banner is never available, and a drive log whose firmware
            # version is unknown cannot be compared with another one later.
            try:
                ser.write(b"V\n")
                ser.flush()
            except (serial.SerialException, OSError):
                pass
            # One due time per schedule, reset on every (re)open.
            schedule = [[time.monotonic() + p, k, p] for k, p in keys]
            while secs == 0.0 or time.monotonic() < end:
                for item in schedule:
                    if time.monotonic() >= item[0]:
                        item[0] = time.monotonic() + item[2]
                        try:
                            ser.write(item[1])
                            ser.flush()
                            log.write("=== %s sent %s\n" % (stamp(), item[1].decode().strip()))
                        except (serial.SerialException, OSError) as exc:
                            log.write("=== %s key write failed (%s)\n" % (stamp(), exc))
                data = ser.read(4096)
                if not data:
                    continue
                partial += data
                *lines, partial = partial.split(b"\n")
                for line in lines:
                    text = line.decode("utf-8", "replace").rstrip("\r")
                    log.write("%s %s\n" % (stamp(), text))
        except (serial.SerialException, OSError) as exc:
            log.write("=== %s port lost (%s), reopening\n" % (stamp(), exc))
            partial = b""
            time.sleep(1.0)
        finally:
            try:
                ser.close()
            except Exception:
                pass
    log.write("=== perf_log closed %s\n" % time.strftime("%Y-%m-%d %H:%M:%S"))

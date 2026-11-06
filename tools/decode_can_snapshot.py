#!/usr/bin/env python3
"""Decode a captured CAN relay snapshot with the DBC in dbc/.

The snapshot is the text the dash prints for 'C' on its console: one line per id
it has heard, with the rate and the latest bytes of each. This reads that text
back and prints every signal in every frame, so a capture taken in a car can be
read on a laptop with no dash, no controller and no CAN hardware.

It uses the same dbc tables and the same bit arithmetic as the firmware, because
it imports the DBC parsing and the bit arithmetic from tools/make_dbc_tables.py,
the generator that produces the firmware's tables and checks them against the
controller's own hand decodes, so the two share their provenance.

It does not currently agree with the firmware's own per-signal dump for every
signal: see the open question at the end of docs/can-relay-sample.txt. The tile
values are unaffected - those read fixed bytes in can_relay.cpp, not the DBC.

    python tools/decode_can_snapshot.py docs/can-relay-sample.txt
    python tools/decode_can_snapshot.py --nonzero docs/can-relay-sample.txt
    python tools/decode_can_snapshot.py --each docs/can-relay-sample.txt
    python tools/decode_can_snapshot.py --table dbc/vw_pq.dbc <snapshot>

A snapshot may hold several presses of 'C', which are captures a few seconds
apart. By default an id is decoded from its **first** capture, so every line in
the output describes one moment, and an id that never moved is not printed twice.
--each prints every capture instead, oldest first, which is how you see a value
move.

Only the Python standard library is needed, but it decodes with the same tables
the firmware links, so it needs dbc/*.dbc and tools/make_dbc_tables.py beside it -
both of which arrive with the CAN relay work, not before it.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_dbc_tables import MUX_PLAIN, MUX_SELECTOR, decode, parse_dbc  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# "[can]     4A0     9/s  00 00 00                Brake_3", optionally prefixed by
# a capture timestamp ("t+  8.0s  "). Two hex digits that are not a byte count
# cannot appear before the name, so the split is unambiguous.
FRAME = re.compile(r"\[can\]\s+([0-9A-Fa-f]{1,8})\s+(\d+)/s\s+(.*)$")
BYTE = re.compile(r"^[0-9A-Fa-f]{2}$")


def parse_snapshot(path):
    """-> [(id, per_sec, dlc, bytes, name_as_printed)], one entry per capture.

    A later capture of the same id is a later entry, not a duplicate: main()
    decides whether to collapse them.
    """
    frames = []
    with open(path, encoding="utf-8", errors="replace") as text:
        for line in text:
            m = FRAME.search(line)
            if not m:
                continue
            ident = int(m.group(1), 16)
            if ident == 0:  # "--- 17 ids, ..." summary lines
                continue
            rate = int(m.group(2))
            tokens = m.group(3).split()
            data = []
            while tokens and BYTE.match(tokens[0]) and len(data) < 8:
                data.append(int(tokens.pop(0), 16))
            name = " ".join(tokens)
            frames.append((ident, rate, len(data), data, name))
    return frames


def signals_of(message, data):
    """-> [(name, value, unit)] for every signal present in this frame.

    Multiplexed signals follow dbc.cpp: the selector's value decides which of
    them a frame carries.
    """
    out = []
    signals = message[4]
    selector = None
    for signal in signals:
        if signal["mux"] == MUX_SELECTOR:
            if len(data) * 8 > signal["start"]:
                selector = int(decode(signal, data))
            break
    for signal in signals:
        if signal["mux"] != MUX_PLAIN:
            if signal["mux"] == MUX_SELECTOR or selector is None or signal["mux"] != selector:
                continue
        if signal["length"] == 0 or (signal["start"] + signal["length"] - 1) // 8 >= len(data):
            continue
        out.append((signal["name"], decode(signal, data), signal["unit"]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("snapshot", help="a captured console dump (text with '[can]' lines)")
    ap.add_argument("--table", default=os.path.join(ROOT, "dbc", "vw_pq.dbc"),
                    help="the DBC to decode with (default: dbc/vw_pq.dbc)")
    ap.add_argument("--nonzero", action="store_true", help="hide signals that are 0")
    ap.add_argument("--each", action="store_true",
                    help="decode every capture, not just the last of each id")
    args = ap.parse_args()

    by_id = {(m[0], m[1]): m for m in parse_dbc(args.table)}
    frames = parse_snapshot(args.snapshot)
    if not frames:
        sys.exit("%s: no '[can]' frame lines found" % args.snapshot)
    if not args.each:
        # The first capture of each id, in the order it was heard. The firmware
        # decoded the same moment in the same file, so the two can be compared.
        first, order = {}, []
        for frame in frames:
            if frame[0] in first:
                continue
            first[frame[0]] = frame
            order.append(frame[0])
        frames = [first[i] for i in order]

    unknown = 0
    print("%s  decoded with %s" % (args.snapshot, os.path.basename(args.table)))
    print()
    for ident, rate, dlc, data, printed in frames:
        message = by_id.get((ident, False))
        hexed = " ".join("%02X" % b for b in data)
        if message is None:
            unknown += 1
            print("0x%03X  %-18s %2d/s  %s   (not in the DBC)" % (ident, printed, rate, hexed))
            continue
        label = message[2] if printed in ("", message[2]) else "%s (%s)" % (message[2], printed)
        print("0x%03X  %-18s %2d/s  %s" % (ident, label, rate, hexed))
        if message[3] != len(data):
            print("        (DBC says %d bytes, the frame carried %d)" % (message[3], len(data)))
        for name, value, unit in signals_of(message, data):
            if args.nonzero and value == 0:
                continue
            print("        %-38s %14.2f %s" % (name, value, unit.strip()))
        print()
    print("%d ids, %d not in the DBC" % (len(frames), unknown))


if __name__ == "__main__":
    main()

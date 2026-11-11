#!/usr/bin/env python3
"""Pick the console port for the serial tools in this directory.

Every serial tool used to default to /dev/ttyUSB0, which is right on the Linux
bench and wrong everywhere else: Windows gives the CH340 a COM number that moves
with the USB socket, so ask pyserial for the adapter that is actually plugged in
rather than hard-coding one. Pass a port explicitly to override it.
"""
import sys

LINUX_PORT = "/dev/ttyUSB0"
WCH_VID = 0x1A86  # CH340/CH341 USB-UART bridge on this board


def default_port():
    if sys.platform != "win32":
        return LINUX_PORT
    from serial.tools import list_ports

    ports = list(list_ports.comports())
    for p in ports:
        if p.vid == WCH_VID:
            return p.device
    if ports:
        return ports[0].device
    raise SystemExit("no serial port found: is the dash plugged in?")

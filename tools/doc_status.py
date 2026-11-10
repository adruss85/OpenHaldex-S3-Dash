#!/usr/bin/env python3
"""Rewrite the build-status block in docs/status.md from a real build.

The numbers in a status page rot the moment the next commit lands. This reads
the version from include/version.h, runs the same size report PlatformIO prints
after a build, and replaces only the text between the two marker comments.

It refuses to guess: if the build output has no RAM/Flash lines it exits
non-zero and leaves the file untouched. A missing build is a failure, not a
reason to keep an old number.

Usage:
    tools/doc_status.py                 # build, then update docs/status.md
    tools/doc_status.py --log build.txt # parse a saved `pio run` log instead
    tools/doc_status.py --elf path.elf  # use the toolchain size on an ELF

The ELF path uses the size tool's section list, not its one-line summary: the
summary counts .ext_ram.bss as RAM, which is where the 320 KB DRAM figure would
go wrong. The section names here match what PlatformIO reports.
"""

import argparse
import datetime as dt
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STATUS = ROOT / "docs" / "status.md"
VERSION_H = ROOT / "include" / "version.h"
START = "<!-- status:start -->"
END = "<!-- status:end -->"

SIZE_RE = {
    "flash": re.compile(
        r"^Flash:\s+\[[^\]]*\]\s+([\d.]+)%\s+\(used (\d+) bytes from (\d+) bytes\)", re.M
    ),
    "ram": re.compile(
        r"^RAM:\s+\[[^\]]*\]\s+([\d.]+)%\s+\(used (\d+) bytes from (\d+) bytes\)", re.M
    ),
}


def version() -> str:
    match = re.search(r'#define\s+OHX_DASH_VERSION\s+"([^"]+)"', VERSION_H.read_text())
    if not match:
        raise SystemExit(f"{VERSION_H}: no OHX_DASH_VERSION")
    return match.group(1)


def git_hash() -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"], text=True
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def build_output() -> str:
    pio = ROOT / ".venv" / "bin" / "pio"
    command = [str(pio) if pio.exists() else "pio", "run", "-e", "s3-dash"]
    print("running:", " ".join(command), file=sys.stderr)
    result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if result.returncode != 0:
        raise SystemExit("build failed:\n" + result.stdout[-4000:])
    return result.stdout


def parse_output(output: str) -> dict:
    sizes = {}
    for key, pattern in SIZE_RE.items():
        match = pattern.search(output)
        if not match:
            raise SystemExit(
                f"could not find the {key.upper()} size line in the build output; "
                "was the build interrupted before PlatformIO printed its report?"
            )
        pct, used, total = match.groups()
        sizes[key] = {"pct": float(pct), "used": int(used), "total": int(total)}
    return sizes


def parse_elf(path: Path) -> dict:
    """Read the same sections PlatformIO counts, using the project toolchain."""
    candidates = sorted(Path.home().glob(
        ".platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-size"))
    if not candidates:
        raise SystemExit("xtensa-esp32s3-elf-size not found under ~/.platformio")
    result = subprocess.run([str(candidates[-1]), "-A", str(path)], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        raise SystemExit(f"size failed on {path}:\n{result.stdout}")

    sections = {}
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith("."):
            try:
                sections[parts[0]] = int(parts[1])
            except ValueError:
                pass

    # These are exactly the sections PlatformIO's espressif32 SIZEPROGREGEXP /
    # SIZEDATAREGEXP count, so this reproduces its RAM and Flash lines. In
    # particular .flash.appdesc, .eh_frame and the PSRAM .ext_ram.* sections are
    # deliberately not in either total.
    ram_names = (".dram0.data", ".dram0.bss", ".noinit")
    flash_names = (".iram0.text", ".iram0.vectors", ".dram0.data",
                   ".flash.text", ".flash.rodata")
    missing = [n for n in ram_names + flash_names if n not in sections]
    if missing:
        raise SystemExit("ELF is missing expected sections: " + ", ".join(missing))

    ram_used = sum(sections[n] for n in ram_names)
    flash_used = sum(sections[n] for n in flash_names)
    return {
        "flash": {"pct": flash_used * 100.0 / 1966080, "used": flash_used, "total": 1966080},
        "ram": {"pct": ram_used * 100.0 / 327680, "used": ram_used, "total": 327680},
    }


def spaced(value: int) -> str:
    return f"{value:,}".replace(",", " ")


def block(sizes: dict, measured: dt.date) -> str:
    flash = sizes["flash"]
    ram = sizes["ram"]
    return "\n".join([
        START,
        f"Firmware version: {version()}",
        f"Flash: {spaced(flash['used'])} / {spaced(flash['total'])} B ({flash['pct']:.1f}%)",
        f"Internal RAM (static): {spaced(ram['used'])} / {spaced(ram['total'])} B",
        f"Measured: {measured.isoformat()}, build {git_hash()}",
        END,
    ])


def replace_block(text: str, new: str) -> str:
    start = text.find(START)
    end = text.find(END)
    if start < 0 or end < 0 or end < start:
        raise SystemExit(f"{STATUS}: status markers missing or out of order")
    if text.find(START, start + 1) >= 0 or text.find(END, end + 1) >= 0:
        raise SystemExit(f"{STATUS}: status markers appear more than once")
    return text[:start] + new + text[end + len(END):]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--log", type=Path, help="saved `pio run -e s3-dash` output")
    source.add_argument("--elf", type=Path, help="firmware.elf to size instead of building")
    parser.add_argument("--date", type=dt.date.fromisoformat, default=dt.date.today(),
                        help="measurement date (YYYY-MM-DD), defaults to today")
    args = parser.parse_args()

    if args.log:
        output = args.log.read_text()
        sizes = parse_output(output)
    elif args.elf:
        sizes = parse_elf(args.elf)
    else:
        sizes = parse_output(build_output())

    text = STATUS.read_text()
    STATUS.write_text(replace_block(text, block(sizes, args.date)))
    flash, ram = sizes["flash"], sizes["ram"]
    print(f"updated {STATUS.relative_to(ROOT)}: "
          f"flash {flash['used']}/{flash['total']} B ({flash['pct']:.1f}%), "
          f"RAM {ram['used']}/{ram['total']} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())

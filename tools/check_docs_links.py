#!/usr/bin/env python3
"""Check every relative link and #anchor in the project's Markdown files.

Why a script rather than a one-off grep: the README is split by audience, and a
moved section leaves a valid-looking link that GitHub only reveals as a 404 when
someone clicks it. This resolves each target against the file it appears in and
checks every fragment against the headings of the target file.

External URLs are deliberately not fetched: the point is the local graph, and a
network check would make the result depend on the day.

Usage:
    tools/check_docs_links.py [--root PATH]

Exits non-zero, listing every broken link, if anything does not resolve.
"""

import argparse
import re
import sys
from pathlib import Path
from urllib.parse import unquote

SKIP_DIRS = {".pio", ".venv", ".backup", ".shots", ".dummy", "__pycache__", ".git"}
LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
HEADING_RE = re.compile(r"^#{1,6}\s+(.*?)\s*#*\s*$")


def slug(text: str) -> str:
    """GitHub's heading anchor, near enough for this repository's headings."""
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)  # [text](url) -> text
    text = text.replace("`", "").replace("*", "").replace("~", "")
    text = text.lower()
    # Drop punctuation; keep letters, digits, spaces, hyphens and underscores.
    text = re.sub(r"[^\w\s-]", "", text, flags=re.UNICODE)
    text = re.sub(r"\s+", "-", text.strip())
    return text


def markdown_files(root: Path):
    for path in sorted(root.rglob("*.md")):
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        yield path


def headings(path: Path):
    found = set()
    in_fence = False
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.lstrip()
        if stripped.startswith("```") or stripped.startswith("~~~"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        match = HEADING_RE.match(line)
        if match:
            found.add(slug(match.group(1)))
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    root = args.root.resolve()

    heading_cache = {}
    errors = []
    checked = 0

    for source in markdown_files(root):
        in_fence = False
        for lineno, line in enumerate(
            source.read_text(encoding="utf-8", errors="replace").splitlines(), start=1
        ):
            stripped = line.lstrip()
            if stripped.startswith("```") or stripped.startswith("~~~"):
                in_fence = not in_fence
                continue
            if in_fence:
                continue

            for raw in LINK_RE.findall(line):
                target = raw.strip()
                if target.startswith("<") and target.endswith(">"):
                    target = target[1:-1]
                else:
                    target = target.split()[0]  # drop an optional "title"
                if not target or target.startswith(("http://", "https://", "mailto:", "tel:", "#")):
                    if not target.startswith("#"):
                        continue
                checked += 1

                path_part, _, anchor = target.partition("#")
                path_part = unquote(path_part)
                anchor = unquote(anchor)
                if path_part == "":
                    dest = source
                else:
                    dest = (source.parent / path_part).resolve()

                if not dest.exists():
                    errors.append(f"{source.relative_to(root)}:{lineno}: missing {path_part}")
                    continue
                if anchor and dest.suffix.lower() == ".md":
                    if dest not in heading_cache:
                        heading_cache[dest] = headings(dest)
                    if anchor not in heading_cache[dest]:
                        errors.append(
                            f"{source.relative_to(root)}:{lineno}: "
                            f"no #{anchor} in {dest.relative_to(root)}"
                        )

    if errors:
        print("Broken documentation links:")
        for error in errors:
            print(f"  {error}")
        return 1

    print(f"Checked {checked} relative link(s) in {len(list(markdown_files(root)))} Markdown file(s): all resolve.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Prove that the subset fonts in src/ui/fonts are the stock LVGL faces.

Every glyph a subset keeps must be byte-identical to the same glyph in LVGL
9.3.0's own committed font source: same bitmap bytes, same per-glyph metrics,
same line_height / base_line (which tools/make_fonts.sh pins to the stock
values). The stock sources are fetched from the LVGL GitHub tag on first run
and cached in /tmp.

Exit status is the verdict; run it after any change to tools/make_fonts.sh.
"""
import os, re, sys, urllib.request

LVGL_TAG = "v9.3.0"
RAW = "https://raw.githubusercontent.com/lvgl/lvgl/%s/src/font/%%s" % LVGL_TAG
CACHE = "/tmp/opencode/stockfonts"

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
fonts = os.path.join(root, "src", "ui", "fonts")

# (subset file, stock file, codepoints kept) - mirrors tools/make_fonts.sh
SETS = [
    ("lv_font_dash_big_48", "lv_font_montserrat_48",
     [0x2D] + list(range(0x30, 0x3A))),
    ("lv_font_dash_val_22", "lv_font_montserrat_22",
     [0x20, 0x25, 0x2D, 0x2E] + list(range(0x30, 0x3A)) + [0x43, 0x46, 0x4C, 0xB0]),
    ("lv_font_dash_mode_28", "lv_font_montserrat_28",
     [0x2D] + list(range(0x30, 0x3B)) + [0x43, 0x44, 0x45, 0x46, 0x47, 0x49, 0x4B,
                                         0x4E, 0x4F, 0x50, 0x52, 0x53, 0x54, 0x57, 0x58]),
]

def bitmap_bytes(text):
    m = re.search(r'glyph_bitmap\[\]\s*=\s*\{(.*?)\};', text, re.S)
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
    return [int(t, 0) for t in re.findall(r'0[xX][0-9a-fA-F]+|\d+', body)]

def glyph_dsc(text):
    m = re.search(r'glyph_dsc\[\]\s*=\s*\{(.*?)\};', text, re.S)
    out = []
    for e in re.finditer(r'\{\.bitmap_index = (\d+), \.adv_w = (-?\d+), \.box_w = (\d+), '
                         r'\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}', m.group(1)):
        out.append(dict(zip(("idx", "adv", "w", "h", "ox", "oy"),
                            map(int, e.groups()))))
    return out

def font_fields(text):
    f = {}
    for key in ("line_height", "base_line"):
        m = re.search(r'\.%s = (-?\d+)' % key, text)
        if m: f[key] = int(m.group(1))
    return f

def stock_glyph_id(cp):
    """The stock faces map 0x20..0x7F to glyph ids 1..96, then 0xB0, then 0x2022
    and the FontAwesome range."""
    if cp == 0x20: return 1
    if cp == 0xB0: return 96
    assert 0x21 <= cp <= 0x7F, "codepoint 0x%X is not in the stock faces' first cmap" % cp
    return cp - 0x20 + 1

os.makedirs(CACHE, exist_ok=True)
ok = True
for sub_name, stock_name, cps in SETS:
    sub_path = os.path.join(fonts, sub_name + ".c")
    stock_path = os.path.join(CACHE, stock_name + ".c")
    if not os.path.exists(stock_path):
        print("fetching %s ..." % stock_name, file=sys.stderr)
        urllib.request.urlretrieve(RAW % (stock_name + ".c"), stock_path)
    with open(sub_path) as f: sub_text = f.read()
    with open(stock_path) as f: stock_text = f.read()
    sub_bm, stock_bm = bitmap_bytes(sub_text), bitmap_bytes(stock_text)
    sub_dsc, stock_dsc = glyph_dsc(sub_text), glyph_dsc(stock_text)
    print("== %s vs %s ==" % (sub_name, stock_name))
    if font_fields(sub_text) != font_fields(stock_text):
        print("   FONT METRICS DIFFER: %s vs %s"
              % (font_fields(sub_text), font_fields(stock_text)))
        ok = False
        continue
    if len(sub_dsc) != len(cps) + 1:
        print("   ENTRY COUNT: %d entries for %d codepoints" % (len(sub_dsc), len(cps)))
        ok = False
        continue
    for i, cp in enumerate(cps):
        s, t = sub_dsc[i + 1], stock_dsc[stock_glyph_id(cp)]
        g = stock_glyph_id(cp)
        hi_s = sub_dsc[i + 2]["idx"] if i + 2 < len(sub_dsc) else len(sub_bm)
        hi_t = stock_dsc[g + 1]["idx"] if g + 1 < len(stock_dsc) else len(stock_bm)
        if sub_bm[s["idx"]:hi_s] != stock_bm[t["idx"]:hi_t]:
            print("   U+%04X bitmap bytes differ" % cp)
            ok = False
        elif {k: s[k] for k in ("adv", "w", "h", "ox", "oy")} != \
             {k: t[k] for k in ("adv", "w", "h", "ox", "oy")}:
            print("   U+%04X metrics differ: sub %s stock %s" % (cp, s, t))
            ok = False
    if ok:
        print("   all %d kept glyphs byte-identical to stock" % len(cps))
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)

#!/usr/bin/env bash
# Regenerate this project's hand-built LVGL fonts: the rev digits, the boost
# value, the indicator arrows, and the three subsets that replace LVGL's stock
# montserrat_22/28/48 faces.
#
# LVGL ships Montserrat only up to 48 px and ships no filled arrow at all, so
# the dashboard's 96 px rev digits, 56 px boost value and the two FontAwesome
# block arrows are generated here. The output is committed, so a normal build
# does not need this script or node.
#
# There used to be a fourth, an 80 px "CAN DASH" for the boot splash. 0.6.2 made
# the splash a pre-rendered picture with the wordmark in it, so that face was
# dropped; do not add it back without a user.
#
# Needs node (no npm) plus lv_font_conv. If you have neither, fetch the
# tarball it ships in and point LV_FONT_CONV at the CLI inside it:
#
#   curl -sSL -o /tmp/lvfc.tgz \
#     https://registry.npmjs.org/lv_font_conv/-/lv_font_conv-1.5.3.tgz
#   mkdir -p /tmp/lvfc && tar xzf /tmp/lvfc.tgz -C /tmp/lvfc
#   LV_FONT_CONV="node /tmp/lvfc/package/lv_font_conv.js" tools/make_fonts.sh
#
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
conv="${LV_FONT_CONV:-node $here/lv_font_conv.js}"
# The subset section wants lv_font_conv 1.5.2 specifically: that version's
# output is byte-identical to LVGL 9.3.0's stock montserrat faces, which is
# what makes "subset of the same glyphs" provable. The Bold/arrow faces above
# were generated with 1.5.3 (the first with --lv-font-name), so a full
# regeneration needs both. Defaults to the same toolchain if unset.
subconv="${LV_FONT_CONV_SUBSETS:-$conv}"
ttf="tools/fonts/Montserrat-Bold.ttf"
out="src/ui/fonts"

# Generate from the repo root with relative paths: lv_font_conv records the
# command in the file header, and an absolute path there would bake one
# developer's checkout directory into the committed source.
cd "$root"

[ -f "$ttf" ] || { echo "missing $ttf" >&2; exit 1; }
mkdir -p "$out"

# Digits and the dash: '-' is what draws '--' when a field is absent.
$conv --font "$ttf" --size 96 --bpp 4 --format lvgl \
      --symbols "0123456789-" --lv-font-name lv_font_dash_rpm_96 \
      -o "$out/font_rpm_96.c"

# Boost value, e.g. "1.55".
$conv --font "$ttf" --size 56 --bpp 4 --format lvgl \
      --symbols "0123456789." --lv-font-name lv_font_dash_num_56 \
      -o "$out/font_num_56.c"

# The indicator repeaters' block arrows. LVGL ships no filled arrow - its
# LV_SYMBOL_LEFT/RIGHT are chevrons - so the two arrows come from FontAwesome
# 4.7, whose U+F060 / U+F061 are exactly the solid shape the mockup draws. Only
# two glyphs, so the face costs a couple of KB; see FontAwesome-LICENSE.txt.
fa="tools/fonts/FontAwesome-4.7.ttf"
[ -f "$fa" ] || { echo "missing $fa" >&2; exit 1; }
$conv --font "$fa" --size 52 --bpp 4 --format lvgl \
      -r 0xF060-0xF061 --lv-font-name lv_font_dash_arrow_52 \
      -o "$out/font_arrow_52.c"

echo "wrote $out/font_rpm_96.c, $out/font_num_56.c and $out/font_arrow_52.c"

# ---------------------------------------------------------------- subsets
# The three stock Montserrat faces, subset to the code points each role draws.
# LVGL 9.3.0 built them from this same Montserrat-Medium.ttf with the options
# recorded in their headers (--no-compress --no-prefilter --bpp 4
# --force-fast-kern-format), and regenerating a full stock face with those
# options and lv_font_conv 1.5.2 reproduces LVGL's committed font sources
# byte-for-byte - so a subset's glyphs are the same bitmaps the dash drew
# before it, just without the code points no role ever shows:
#
#   lv_font_dash_big_48.c  speed:      "%d" or "--"              -> 0-9, '-'
#   lv_font_dash_val_22.c  temps:      "79°C"/"175°F", fuel      -> 0-9,
#                          "49.6 L", locks "%d%%", "--"    '-','.','%',
#                                                          'C','F','L',
#                                                          space, '°'
#   lv_font_dash_mode_28.c mode names: STOCK/FWD/50:50/60:40/75:25/EXPERT, the
#                          SETTINGS heading, and "--"  -> 0-9, ':', and
#                                                          C D E F G I K N O
#                                                          P R S T W X
#
# The FontAwesome symbol range the stock faces also carry is dropped: none of
# those roles ever draws an LV_SYMBOL_* (the keyboard's symbols come from
# montserrat_14, which stays stock).
#
# Byte-fidelity was checked with lv_font_conv 1.5.2; 1.5.3 rasterizes a few
# anti-aliased pixels one alpha step differently. If you regenerate with a
# newer lv_font_conv, expect small rasterization drift unless the bitmaps
# still match. tools/check_font_subsets.py re-proves the byte-identity
# against LVGL 9.3.0's committed font sources after any regeneration.
med="tools/fonts/Montserrat-Medium.ttf"
[ -f "$med" ] || { echo "missing $med" >&2; exit 1; }
sub="--no-compress --no-prefilter --force-fast-kern-format --bpp 4 --format lvgl"

$subconv --font "$med" --size 48 $sub -r 0x2D,0x30-0x39 \
      -o "$out/lv_font_dash_big_48.c"

$subconv --font "$med" --size 22 $sub -r 0x20,0x25,0x2D,0x2E,0x30-0x39,0x43,0x46,0x4C,0xB0 \
      -o "$out/lv_font_dash_val_22.c"

$subconv --font "$med" --size 28 $sub -r 0x2D,0x30-0x3A,0x43-0x47,0x49,0x4B,0x4E-0x50,0x52-0x54,0x57,0x58 \
      -o "$out/lv_font_dash_mode_28.c"

# No --lv-font-name here: 1.5.2 (the version whose output is byte-identical to
# LVGL's stock faces) predates that option and names the struct after the
# output file's basename, hence the lv_font_dash_* file names above.
#
# lv_font_conv derives line_height and base_line from the glyphs kept: digit
# and dash sets have no descenders, so the metrics collapse (48 px: line 34,
# base 0) and every label would sit ~9 px higher than it does today. The dash's
# tile geometry assumes the stock faces' baselines (see the shared-baseline
# note in dash_ui.cpp), so pin them to what LVGL 9.3.0's montserrat_22/28/48
# carry: 52/9, 24/4 and 30/5. Checked against the upstream font sources.
pin_metrics() { # <file> <line_height> <base_line>
  sed -i -e "s/^\(    \.line_height = \)[0-9][0-9]*,/\1$2,/" \
         -e "s/^\(    \.base_line = \)[0-9][0-9]*,/\1$3,/" "$1"
}
pin_metrics "$out/lv_font_dash_big_48.c"  52 9
pin_metrics "$out/lv_font_dash_val_22.c"  24 4
pin_metrics "$out/lv_font_dash_mode_28.c" 30 5

echo "wrote $out/lv_font_dash_big_48.c, $out/lv_font_dash_val_22.c and $out/lv_font_dash_mode_28.c"
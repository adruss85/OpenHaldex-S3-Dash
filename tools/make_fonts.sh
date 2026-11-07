#!/usr/bin/env bash
# Regenerate this project's hand-built LVGL fonts: the rev digits, the boost
# value, the boot splash's name and the indicator arrows.
#
# LVGL ships Montserrat only up to 48 px and ships no filled arrow at all, so
# the dashboard's 96 px rev digits, 56 px boost value, 80 px "CAN DASH" name and
# the two FontAwesome block arrows are generated here. The output is committed,
# so a normal build does not need this script or node.
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

# The boot splash's name, "CAN DASH", one size up. It needs letters, which the
# two numeric fonts above deliberately do not carry, so it is its own face. 80 px
# is the compromise: bigger and bolder than the 48 px Montserrat LVGL ships, and
# about as wide as the mockup's 96 px fits across an 800 px panel.
$conv --font "$ttf" --size 80 --bpp 4 --format lvgl \
      --symbols "CAND SH" --lv-font-name lv_font_dash_name_80 \
      -o "$out/font_name_80.c"

# The indicator repeaters' block arrows. LVGL ships no filled arrow - its
# LV_SYMBOL_LEFT/RIGHT are chevrons - so the two arrows come from FontAwesome
# 4.7, whose U+F060 / U+F061 are exactly the solid shape the mockup draws. Only
# two glyphs, so the face costs a couple of KB; see FontAwesome-LICENSE.txt.
fa="tools/fonts/FontAwesome-4.7.ttf"
[ -f "$fa" ] || { echo "missing $fa" >&2; exit 1; }
$conv --font "$fa" --size 52 --bpp 4 --format lvgl \
      -r 0xF060-0xF061 --lv-font-name lv_font_dash_arrow_52 \
      -o "$out/font_arrow_52.c"

echo "wrote $out/font_rpm_96.c, $out/font_num_56.c, $out/font_name_80.c and $out/font_arrow_52.c"
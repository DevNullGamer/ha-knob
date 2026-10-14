#!/usr/bin/env sh
# Regenerates the LVGL fonts in src/ui/fonts/ (Latin-1 + Latin Extended-A, for Danish etc.).
# Requires Node.js (npx) and the LVGL package downloaded by PlatformIO
# (run `pio pkg install -e t-encoder-pro` first). Run from the repository root.
set -eu

LV_FONT_CONV="lv_font_conv@1.5.3"
FONT_DIR=".pio/libdeps/t-encoder-pro/lvgl/scripts/built_in_font"   # LVGL 9.2.2 (OFL fonts)
TEXT="$FONT_DIR/Montserrat-Medium.ttf"
SYMBOL_FONT="$FONT_DIR/FontAwesome5-Solid+Brands+Regular.woff"
OUT="src/ui/fonts"

# ASCII, Latin-1 Supplement (incl. degree sign, æ ø å Æ Ø Å), Latin Extended-A, bullet
TEXT_RANGE="0x20-0x7E,0xA0-0xFF,0x100-0x17F,0x2022"
# The LV_SYMBOL_* glyphs of LVGL's built-in fonts (from scripts/built_in_font/built_in_font_gen.py)
SYMBOLS="61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650"

COMMON="--no-compress --no-prefilter --bpp 4 --format lvgl --force-fast-kern-format"

# 20 and 28 px: text + LV_SYMBOL_* glyphs (used by LVGL widgets and status icons)
for size in 20 28; do
    npx -y "$LV_FONT_CONV" $COMMON --size "$size" \
        --font "$TEXT" -r "$TEXT_RANGE" \
        --font "$SYMBOL_FONT" -r "$SYMBOLS" \
        -o "$OUT/ui_font_$size.c"
done

# 56 px: large centre value, text only (symbols would cost flash for no use)
npx -y "$LV_FONT_CONV" $COMMON --size 56 \
    --font "$TEXT" -r "$TEXT_RANGE" \
    -o "$OUT/ui_font_56.c"

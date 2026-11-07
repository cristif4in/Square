#!/usr/bin/env bash
# Square Browser Engine — Linux build script
set -e
cd "$(dirname "$0")"
OUT=${OUT:-build}
QJS=thirdparty/quickjs
WEBP=thirdparty/libwebp
CXX=${CXX:-g++}
CC=${CC:-gcc}
CXXFLAGS="-std=c++17 -O2 -g -fPIC -Wall -Wno-unused-variable -Wno-unused-function -Iinclude -Isrc -Ithirdparty -Ithirdparty/libwebp/src"
QFLAGS="-I$QJS -D_GNU_SOURCE -DCONFIG_VERSION=\"$(cat $QJS/VERSION)\" -O2 -fPIC"
WEBPFLAGS="-O2 -fPIC -I$WEBP -I$WEBP/src -Wno-unused-function -Wno-unused-variable -Wno-attributes -Wno-sign-compare"
LDFLAGS="-shared -lz -lssl -lcrypto -lpthread -ldl -lm"
mkdir -p $OUT $OUT/webp

echo "[1/7] QuickJS (C)"
for f in quickjs libregexp libunicode cutils dtoa; do
  $CC -c $QFLAGS $QJS/$f.c -o $OUT/qjs_$f.o &
done
wait

echo "[2/7] libwebp (vendored decoder — dec/utils/dsp, encoder excluded)"
WP_SRCS=""
for f in $WEBP/src/dec/*.c $WEBP/src/utils/*.c $WEBP/src/dsp/*.c; do
  b=$(basename $f .c)
  case $b in cost|cost_*|enc|enc_*|cost_mips32|cost_mips_dsp_r2|cost_neon|cost_sse2|enc_mips32|enc_mips_dsp_r2|enc_msa|enc_neon|enc_sse2|enc_sse41) continue;; esac
  WP_SRCS="$WP_SRCS $f"
done
N=0
for f in $WP_SRCS; do
  $CC -c $WEBPFLAGS $f -o $OUT/webp/$(basename $f .c).o &
  N=$((N+1)); if [ $((N % 16)) -eq 0 ]; then wait; fi
done
wait

echo "[3/7] Embedded fonts"
$CC -c -fPIC thirdparty/fonts_embed.c -o $OUT/fonts_embed.o

echo "[4/7] Engine modules (C++)"
for f in square_content square_css square_style square_layout_common square_layout_inline square_layout_block square_layout_flex square_layout_grid square_layout_table square_layout_paint square_css_selectors square_css_values square_font square_image square_svg square_gfx square_net square_platform square_platform_api; do
  $CXX -c $CXXFLAGS src/$f.cpp -o $OUT/$f.o &
done
wait

echo "[5/7] libsquare.so"
$CXX -shared -o $OUT/libsquare.so \
  $OUT/qjs_*.o $OUT/fonts_embed.o $OUT/webp/*.o \
  $OUT/square_css.o $OUT/square_style.o $OUT/square_content.o $OUT/square_layout_common.o $OUT/square_layout_inline.o $OUT/square_layout_block.o $OUT/square_layout_flex.o $OUT/square_layout_grid.o $OUT/square_layout_table.o $OUT/square_layout_paint.o $OUT/square_css_selectors.o $OUT/square_css_values.o $OUT/square_font.o $OUT/square_image.o $OUT/square_svg.o $OUT/square_gfx.o \
  $OUT/square_net.o $OUT/square_platform.o $OUT/square_platform_api.o \
  -lz -lssl -lcrypto -lpthread -ldl -lm

echo "[6/7] square_engine (engine test harness)"
$CXX -std=c++17 -O2 -Iinclude test/engine_main.cpp -o $OUT/square_engine \
  -L$OUT -lsquare -Wl,-rpath,'$ORIGIN' -lpthread

echo "[7/7] square_browser (browser app)"
$CXX -std=c++17 -O2 -Iinclude app/browser_main.cpp app/browser_x11.cpp -o $OUT/square_browser \
  -L$OUT -lsquare -Wl,-rpath,'$ORIGIN' -lpthread -lX11

echo "Done: $OUT/libsquare.so $OUT/square_engine $OUT/square_browser"

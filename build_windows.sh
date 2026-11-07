#!/usr/bin/env bash
# Square Browser Engine — Windows x64 cross-build (zig cc), runnable from Linux.
# Produces build-win/square.dll + square_engine.exe + square_browser.exe
set -e
cd "$(dirname "$0")"
ZIG=${ZIG:-../tools/zig/zig}
OUT=build-win
QJS=thirdparty/quickjs
ZLIB=thirdparty/zlib
WEBP=thirdparty/libwebp
QVER=$(cat $QJS/VERSION)
T=x86_64-windows-gnu
mkdir -p $OUT/webp

CFLAGS="-O2 -I$QJS -Ithirdparty/winshim -DCONFIG_VERSION=\"\\\"$QVER\\\"\""
echo "[1/6] QuickJS (C)"
$ZIG cc -target $T -O1 -I$QJS -Ithirdparty/winshim -DCONFIG_VERSION="\"$QVER\"" -c $QJS/quickjs.c -o $OUT/qjs_quickjs.o
for f in libregexp libunicode cutils dtoa; do
  $ZIG cc -target $T -O2 -I$QJS -Ithirdparty/winshim -DCONFIG_VERSION="\"$QVER\"" -c $QJS/$f.c -o $OUT/qjs_$f.o
done

echo "[2/6] zlib"
for f in adler32 crc32 inflate inffast inftrees zutil deflate trees; do
  $ZIG cc -target $T -O2 -I$ZLIB -c $ZLIB/$f.c -o $OUT/z_$f.o
done

echo "[3/6] libwebp (vendored decoder)"
WP_SRCS=""
for f in $WEBP/src/dec/*.c $WEBP/src/utils/*.c $WEBP/src/dsp/*.c; do
  b=$(basename $f .c)
  case $b in cost|cost_*|enc|enc_*|cost_mips32|cost_mips_dsp_r2|cost_neon|cost_sse2|enc_mips32|enc_mips_dsp_r2|enc_msa|enc_neon|enc_sse2|enc_sse41) continue;; esac
  WP_SRCS="$WP_SRCS $f"
done
for f in $WP_SRCS; do
  $ZIG cc -target $T -O2 -I$WEBP -I$WEBP/src -Wno-unused-function -Wno-unused-variable -Wno-attributes -Wno-sign-compare -c $f -o $OUT/webp/$(basename $f .c).o &
done
wait

echo "[4/6] fonts"
$ZIG cc -target $T -O2 -c thirdparty/fonts_embed.c -o $OUT/fonts_embed.o

echo "[5/6] engine modules (C++)"
for f in square_content square_css square_style square_layout_common square_layout_inline square_layout_block square_layout_flex square_layout_grid square_layout_paint square_css_selectors square_css_values square_font square_image square_svg square_gfx square_net square_platform square_platform_api; do
  $ZIG c++ -target $T -std=c++17 -O2 -DSQUARE_BUILD_DLL -Iinclude -Isrc -Ithirdparty -I$WEBP/src -I$ZLIB -c src/$f.cpp -o $OUT/$f.o
done
$ZIG c++ -target $T -std=c++17 -O2 -Iinclude -c test/engine_main.cpp -o $OUT/engine_main.o
$ZIG c++ -target $T -std=c++17 -O2 -Iinclude -c app/browser_main.cpp -o $OUT/browser_main.o
$ZIG c++ -target $T -std=c++17 -O2 -Iinclude -c app/browser_win32.cpp -o $OUT/browser_win32.o

echo "[6/6] square.dll + executables"
$ZIG c++ -target $T -shared -o $OUT/square.dll \
  $OUT/qjs_*.o $OUT/z_*.o $OUT/webp/*.o $OUT/fonts_embed.o $OUT/square_*.o \
  -static-libgcc -static-libstdc++ -lws2_32 -lcrypt32 -ladvapi32 -lsecur32 -lm
$ZIG dlltool -d build-win/square.def -l $OUT/libsquare.a -m i386:x86-64
$ZIG c++ -target $T -std=c++17 -O2 -Iinclude $OUT/engine_main.o $OUT/libsquare.a -o $OUT/square_engine.exe -lws2_32 -lcrypt32 -ladvapi32 -lsecur32 -lwsock32 -lm
$ZIG c++ -target $T -std=c++17 -O2 -Iinclude $OUT/browser_main.o $OUT/browser_win32.o $OUT/libsquare.a -o $OUT/square_browser.exe -lws2_32 -lcrypt32 -ladvapi32 -lsecur32 -lwsock32 -lgdi32 -luser32 -lshell32 -lm

echo "Done: $OUT/square.dll $OUT/square_engine.exe $OUT/square_browser.exe"

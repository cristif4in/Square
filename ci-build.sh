#!/usr/bin/env bash
# Square Browser Engine — CI build (Linux g++ + OpenSSL, or Windows MinGW-w64
# with Schannel and vendored zlib/libwebp). Produces build/libsquare(.so|dll),
# build/square_engine(.exe) and build/square_browser(.exe).
set -e
cd "$(dirname "$0")"
OUT=build
QJS=thirdparty/quickjs
ZLIB=thirdparty/zlib
WEBP=thirdparty/libwebp
QVER=$(cat $QJS/VERSION)
CXX=${CXX:-g++}
CC=${CC:-gcc}
mkdir -p $OUT/webp $OUT/z

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) WINDOWS=1 ;;
  *) WINDOWS=0 ;;
esac
PIC=""; [ "$WINDOWS" = "0" ] && PIC="-fPIC"

echo "[1/6] QuickJS (C)"
pids=""
for f in quickjs libregexp libunicode cutils dtoa; do
  $CC -c -O2 $PIC -I$QJS -D_GNU_SOURCE -DCONFIG_VERSION="\"$QVER\"" $QJS/$f.c -o $OUT/qjs_$f.o &
  pids="$pids $!"
done
for p in $pids; do wait $p || exit $?; done

echo "[2/6] Embedded fonts"
$CC -c -O2 $PIC thirdparty/fonts_embed.c -o $OUT/fonts_embed.o

if [ "$WINDOWS" = "1" ]; then
  echo "[3/6] zlib (vendored)"
  pids=""
  for f in adler32 crc32 inflate inffast inftrees zutil deflate trees; do
    $CC -c -O2 $PIC -I$ZLIB $ZLIB/$f.c -o $OUT/z/$f.o &
    pids="$pids $!"
  done
  for p in $pids; do wait $p || exit $?; done
else
  echo "[3/6] zlib (system)"
fi

echo "[4/6] libwebp (vendored decoder)"
WP_SRCS=""
for f in $WEBP/src/dec/*.c $WEBP/src/utils/*.c $WEBP/src/dsp/*.c; do
  b=$(basename $f .c)
  case $b in cost|cost_*|enc|enc_*|cost_mips32|cost_mips_dsp_r2|cost_neon|cost_sse2|enc_mips32|enc_mips_dsp_r2|enc_msa|enc_neon|enc_sse2|enc_sse41) continue;; esac
  WP_SRCS="$WP_SRCS $f"
done
N=0
pids=""
for f in $WP_SRCS; do
  $CC -c -O2 $PIC -I$WEBP -I$WEBP/src -Wno-unused-function -Wno-unused-variable -Wno-attributes -Wno-sign-compare $f -o $OUT/webp/$(basename $f .c).o &
  pids="$pids $!"
  N=$((N+1)); if [ $((N % 16)) -eq 0 ]; then for p in $pids; do wait $p || exit $?; done; pids=""; fi
done
for p in $pids; do wait $p || exit $?; done

echo "[5/6] Engine modules (C++)"
CXXFLAGS="-std=c++17 -O2 -fPIC -Wall -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable -DSQUARE_BUILD_DLL -Iinclude -Isrc -Ithirdparty -I$WEBP/src"
if [ "$WINDOWS" = "1" ]; then
  # engine includes <zlib.h> → use the vendored zlib headers
  CXXFLAGS="$CXXFLAGS -I$ZLIB"
fi
pids=""
for f in square_content square_css square_style square_layout_common square_layout_inline square_layout_block square_layout_flex square_layout_grid square_layout_table square_layout_paint square_css_selectors square_css_values square_font square_image square_svg square_gfx square_net square_platform square_platform_dom square_platform_js square_platform_api; do
  $CXX -c $CXXFLAGS src/$f.cpp -o $OUT/$f.o &
  pids="$pids $!"
done
for p in $pids; do wait $p || exit $?; done

echo "[6/6] Link"
if [ "$WINDOWS" = "1" ]; then
  $CXX -shared -o $OUT/square.dll \
    $OUT/qjs_*.o $OUT/fonts_embed.o $OUT/z/*.o $OUT/webp/*.o \
    $OUT/square_css.o $OUT/square_style.o $OUT/square_content.o $OUT/square_layout_common.o $OUT/square_layout_inline.o $OUT/square_layout_block.o $OUT/square_layout_flex.o $OUT/square_layout_grid.o $OUT/square_layout_table.o $OUT/square_layout_paint.o $OUT/square_css_selectors.o $OUT/square_css_values.o $OUT/square_font.o $OUT/square_image.o $OUT/square_svg.o $OUT/square_gfx.o \
    $OUT/square_net.o $OUT/square_platform.o $OUT/square_platform_dom.o $OUT/square_platform_js.o $OUT/square_platform_api.o \
    -static -Wl,--out-implib,$OUT/libsquare.a \
    -lpthread -lm -lws2_32 -lsecur32 -lcrypt32 -ladvapi32
  if [ -f test/engine_main.cpp ]; then
    $CXX -std=c++17 -O2 -Iinclude test/engine_main.cpp -o $OUT/square_engine.exe \
      -L$OUT -lsquare -static -lpthread
  fi
  $CXX -std=c++17 -O2 -Iinclude app/browser_main.cpp app/browser_win32.cpp \
    -o $OUT/square_browser.exe -L$OUT -lsquare -static -lgdi32 -luser32 -lshell32 -lpthread
  echo "Done: $OUT/square.dll $OUT/square_browser.exe"
else
  $CXX -shared -o $OUT/libsquare.so \
    $OUT/qjs_*.o $OUT/fonts_embed.o $OUT/webp/*.o \
    $OUT/square_css.o $OUT/square_style.o $OUT/square_content.o $OUT/square_layout_common.o $OUT/square_layout_inline.o $OUT/square_layout_block.o $OUT/square_layout_flex.o $OUT/square_layout_grid.o $OUT/square_layout_table.o $OUT/square_layout_paint.o $OUT/square_css_selectors.o $OUT/square_css_values.o $OUT/square_font.o $OUT/square_image.o $OUT/square_svg.o $OUT/square_gfx.o \
    $OUT/square_net.o $OUT/square_platform.o $OUT/square_platform_dom.o $OUT/square_platform_js.o $OUT/square_platform_api.o \
    -lz -lssl -lcrypto -lpthread -ldl -lm
  if [ -f test/engine_main.cpp ]; then
    $CXX -std=c++17 -O2 -Iinclude test/engine_main.cpp -o $OUT/square_engine \
      -L$OUT -lsquare -Wl,-rpath,'$ORIGIN' -lpthread
  fi
  $CXX -std=c++17 -O2 -Iinclude app/browser_main.cpp app/browser_x11.cpp \
    -o $OUT/square_browser -L$OUT -lsquare -Wl,-rpath,'$ORIGIN' -lpthread -lX11
  echo "Done: $OUT/libsquare.so $OUT/square_browser"
fi

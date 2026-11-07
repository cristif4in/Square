@echo off
rem ============================================================
rem  Square Browser Engine - Windows build using zig (recommended)
rem   1. install zig (https://zig.download) or use the bundled path below
rem   2. run:  build_windows_zig.cmd [path\to\zig.exe]
rem  Output: build-win\square.dll + square_engine.exe + square_browser.exe
rem ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

set ZIG=%1
if "%ZIG%"=="" set ZIG=zig
set OUT=build-win
set QJS=thirdparty\quickjs
set ZLIB=thirdparty\zlib
for /f "usebackq delims=" %%v in ("%QJS%\VERSION") do set QVER=%%v

if not exist %OUT% mkdir %OUT

echo [1/5] QuickJS (C)
%ZIG% cc -target x86_64-windows-gnu -O1 -I%QJS% -Ithirdparty\winshim -DCONFIG_VERSION="\"!QVER!\"" -c %QJS%\quickjs.c -o %OUT%\qjs_quickjs.o || goto :err
for %%f in (libregexp libunicode cutils dtoa) do (
  %ZIG% cc -target x86_64-windows-gnu -O2 -I%QJS% -Ithirdparty\winshim -DCONFIG_VERSION="\"!QVER!\"" -c %QJS%\%%f.c -o %OUT%\qjs_%%f.o || goto :err
)

echo [2/5] zlib
for %%f in (adler32 crc32 inflate inffast inftrees zutil deflate trees) do (
  %ZIG% cc -target x86_64-windows-gnu -O2 -I%ZLIB% -c %ZLIB%\%%f.c -o %OUT%\z_%%f.o || goto :err
)

echo [3/5] fonts
%ZIG% cc -target x86_64-windows-gnu -O2 -c thirdparty\fonts_embed.c -o %OUT%\fonts_embed.o || goto :err

echo [4/5] engine modules (C++)
for %%f in (square_content square_css square_style square_layout square_gfx square_net square_platform square_platform_api) do (
  %ZIG% c++ -target x86_64-windows-gnu -std=c++17 -O2 -DSQUARE_BUILD_DLL -Iinclude -Isrc -Ithirdparty -I%ZLIB% -c src\%%f.cpp -o %OUT%\%%f.o || goto :err
)
%ZIG% c++ -target x86_64-windows-gnu -std=c++17 -O2 -Iinclude -c test\engine_main.cpp -o %OUT%\engine_main.o || goto :err
%ZIG% c++ -target x86_64-windows-gnu -std=c++17 -O2 -Iinclude -c app\browser_main.cpp -o %OUT%\browser_main.o || goto :err
%ZIG% c++ -target x86_64-windows-gnu -std=c++17 -O2 -Iinclude -c app\browser_win32.cpp -o %OUT%\browser_win32.o || goto :err

echo [5/5] square.dll + executables
%ZIG% c++ -target x86_64-windows-gnu -shared -o %OUT%\square.dll %OUT%\qjs_*.o %OUT%\z_*.o %OUT%\fonts_embed.o %OUT%\square_*.o -static-libgcc -static-libstdc++ -lws2_32 -lcrypt32 -ladvapi32 -lsecur32 -lm || goto :err
%ZIG% dlltool -d build-win\square.def -l %OUT%\libsquare.a -m i386:x86-64 || goto :err
%ZIG% c++ -target x86_64-windows-gnu -std=c++17 -O2 -Iinclude %OUT%\engine_main.o %OUT%\libsquare.a -o %OUT%\square_engine.exe -lws2_32 -lcrypt32 -ladvapi32 -lsecur32 -lwsock32 -lm || goto :err
%ZIG% c++ -target x86_64-windows-gnu -std=c++17 -O2 -Iinclude %OUT%\browser_main.o %OUT%\browser_win32.o %OUT%\libsquare.a -o %OUT%\square_browser.exe -lws2_32 -lcrypt32 -ladvapi32 -lsecur32 -lwsock32 -lgdi32 -luser32 -lshell32 -lm || goto :err

echo Done: %OUT%\square.dll %OUT%\square_engine.exe %OUT%\square_browser.exe
goto :eof
:err
echo BUILD FAILED
exit /b 1

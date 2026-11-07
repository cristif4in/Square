# Square Browser Engine — Windows build (MSVC or MinGW-w64)
# -----------------------------------------------------------
# MinGW-w64 (recommended):
#   build.bat mingw
# MSVC (run from a Developer Command Prompt):
#   build.bat msvc
# Output: build\square.dll, build\square_engine.exe, build\square_browser.exe
@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
set OUT=build
if not exist %OUT% mkdir %OUT
set QJS=thirdparty\quickjs
set ZLIB=thirdparty\zlib
set WEBP=thirdparty\libwebp
for /f "usebackq delims=" %%v in ("thirdparty\quickjs\VERSION") do set QVER=%%v

if "%1"=="msvc" goto msvc

:minGW
echo [1/4] QuickJS + fonts + zlib (C)
for %%f in (quickjs libregexp libunicode cutils dtoa) do (
  gcc -c -O2 -I%QJS% -D_GNU_SOURCE -DCONFIG_VERSION=\"!QVER!\" %QJS%\%%f.c -o %OUT%\qjs_%%f.o
)
gcc -c -O2 thirdparty\fonts_embed.c -o %OUT%\fonts_embed.o
for %%f in (adler32 crc32 inflate inffast inftrees zutil deflate trees) do (
  gcc -c -O2 -I%ZLIB% %ZLIB%\%%f.c -o %OUT%\z_%%f.o
)
echo [2/4] libwebp (vendored decoder)
for %%f in (%WEBP%\src\dec\*.c %WEBP%\src\utils\*.c %WEBP%\src\dsp\*.c) do (
  gcc -c -O2 -I%WEBP% -I%WEBP%\src -Wno-unused-function -Wno-unused-variable -Wno-attributes -Wno-sign-compare %%f -o %OUT%\webp_%%~nf.o
)
echo [3/4] Engine modules (C++)
for %%f in (square_content square_css square_style square_layout square_gfx square_net square_platform square_platform_api) do (
  g++ -c -std=c++17 -O2 -Wall -DSQUARE_BUILD_DLL -Iinclude -Isrc -Ithirdparty -I%WEBP%\src src\%%f.cpp -o %OUT%\%%f.o
)
echo [4/4] square.dll + executables
g++ -shared -o %OUT%\square.dll %OUT%\qjs_*.o %OUT%\fonts_embed.o %OUT%\z_*.o %OUT%\webp_*.o %OUT%\square_content.o %OUT%\square_layout.o %OUT%\square_gfx.o %OUT%\square_net.o %OUT%\square_platform.o ^
  -static -Wl,--output-def,%OUT%\square.def ^
  -Wl,--out-implib,%OUT%\libsquare.a -lpthread -lm -lws2_32 -lsecur32 -lcrypt32 -ladvapi32
g++ -std=c++17 -O2 -Iinclude test\engine_main.cpp -o %OUT%\square_engine.exe -L%OUT% -lsquare -static -lpthread
g++ -std=c++17 -O2 -Iinclude app\browser_main.cpp app\browser_win32.cpp -o %OUT%\square_browser.exe -L%OUT% -lsquare -static -lgdi32 -luser32 -lshell32 -lpthread
echo Done: %OUT%\square.dll %OUT%\square_engine.exe %OUT%\square_browser.exe
goto :eof

:msvc
echo [1/4] QuickJS + fonts (C)
for %%f in (quickjs libregexp libunicode cutils dtoa) do (
  cl /c /O2 /I%QJS% /D_GNU_SOURCE /DCONFIG_VERSION=\"!QVER!\" /Fo%OUT%\qjs_%%f.obj %QJS%\%%f.c
)
cl /c /O2 /Fo%OUT%\fonts_embed.obj thirdparty\fonts_embed.c
echo [2/4] libwebp (vendored decoder)
for %%f in (%WEBP%\src\dec\*.c %WEBP%\src\utils\*.c %WEBP%\src\dsp\*.c) do (
  cl /c /O2 /I%WEBP% /I%WEBP%\src /Fo%OUT%\webp_%%~nf.obj %%f
)
echo [3/4] Engine modules (C++)
for %%f in (square_content square_css square_style square_layout square_gfx square_net square_platform square_platform_api) do (
  cl /c /std:c++17 /O2 /W3 /DSQUARE_BUILD_DLL /Iinclude /Isrc /Ithirdparty /I%WEBP%\src /Fo%OUT%\%%f.obj src\%%f.cpp
)
echo [4/4] square.dll + executables
link /DLL /OUT:%OUT%\square.dll /IMPLIB:%OUT%\square.lib %OUT%\qjs_*.obj %OUT%\fonts_embed.obj %OUT%\webp_*.obj %OUT%\square_*.obj ws2_32.lib secur32.lib crypt32.lib advapi32.lib
cl /std:c++17 /O2 /Iinclude test\engine_main.cpp /Fe%OUT%\square_engine.exe /link /LIBPATH:%OUT% square.lib
cl /std:c++17 /O2 /Iinclude app\browser_main.cpp app\browser_win32.cpp /Fe%OUT%\square_browser.exe /link /LIBPATH:%OUT% square.lib user32.lib gdi32.lib shell32.lib
echo Done: %OUT%\square.dll %OUT%\square_engine.exe %OUT%\square_browser.exe

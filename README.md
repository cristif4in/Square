# Square Browser Engine

[![build](https://github.com/cristif4in/Square/actions/workflows/build.yml/badge.svg)](https://github.com/cristif4in/Square/actions/workflows/build.yml)

Square is a from-scratch browser engine written in C++17. It ships as a shared
library (`libsquare.so` on Linux, `square.dll` on Windows).

## Ai disclaimer
This project may use the usage of AI in the code but also screenshots and relative stuff to this project.

## Versioning

Each release adds +11 patch number to the patch section: 1.a.0001 → 1.a.0012 → 1.a.0023 → …
If it would pass 9999/9998, the major goes up and the patch restarts: 1.a.9990 → 2.a.0001 → 2.a.0012. the second letter which right now is ".a" indicates the milestone, which as of now is Alpha. it may change as we reach milestones

## What's powering this browser engine:

- **CSS engine** - tokenizer, selectors (classes/ids/attrs/pseudo-classes/
  `:nth-child`/`:not`), specificity cascade, inheritance, custom properties
- **Renderer** - software rasterizer: might not run as fast as chromium.
- **JavaScript** - embedded QuickJS + a API layers and additions.
- **Networking** - **HTTP/2** (RFC 7540/7541: ALPN negotiation, HPACK with
  static + with automatic HTTP/1.1 fallback.
  **TLS 1.2/1.3 via OpenSSL** (Linux) or **Schannel** (Windows), certificate
  verification.
- **JIT Engine** - Flame is our own JIT engine, a tiered JIT for QuickJS. ARM64 is currently not fully tested and verified yet.


## Usage

### Browser (GUI)
```
square_browser [url] [--width N] [--height N]
square_browser --headless [out.png] [--out page.png] [--full-page] [url]
```
Linux needs an X11 display; without one it falls back to headless mode
(loads the page, renders, saves a PNG and exits). On Windows it opens a
Win32 window with an address bar.

### Engine harness
```
square_engine [url] [--out page.png] [--viewport 1440x900] [--full] [--log]
```

### From your own code (the DLL)
```c
#include <square.h>
square_instance* inst = square_create();
square_set_viewport(inst, 1440, 900);
square_load_url(inst, "https://example.com/");
square_wait_state(inst, SQ_STATE_READY, 45000);
square_save_png(inst, "page.png", 1);
square_destroy(inst);
```
Link against `libsquare.so` / `square.dll` (import lib `libsquare.a` included).

## Building

### Linux
```
./build.sh          # needs: g++, libssl-dev, zlib1g-dev, libx11-dev
```
Outputs to `build/`: `libsquare.so`, `square_engine`, `square_browser`.

### Windows
**With MinGW-w64:** `build.bat mingw` (needs zlib dev + pthreads shim is
bundled under `thirdparty/winshim`). MSVC: `build.bat msvc`.

Licensed: project code MIT. Vendored: QuickJS (MIT), stb (public domain),
Inter font (SIL OFL 1.1), zlib (zlib license).

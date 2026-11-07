# Square Browser Engine

Square is a from-scratch browser engine written in C++17. It ships as a shared
library (`libsquare.so` on Linux, `square.dll` on Windows).

## Versioning

Each release adds 11 to the patch: 1.a.0001 → 1.a.0012 → 1.a.0023 → …
If it would pass 9999, the major goes up and the patch restarts: 1.a.9990 → 2.a.0001 → 2.a.0012.

## What's inside (all written for this project)

- **HTML5 tokenizer + tree builder** — void elements, implied tags, tables,
  raw text, entities, SVG foreign content
- **CSS engine** — tokenizer, selectors (classes/ids/attrs/pseudo-classes/
  `:nth-child`/`:not`), specificity cascade, inheritance, custom properties
  (`var()` with fallbacks), `calc()`, media queries, `@font-face`/`@import`
- **Layout** — block flow with margin collapsing, inline layout with line
  breaking, **flexbox** (wrap, grow/shrink, gap, justify, align), **CSS grid**
  (fr/repeat/minmax/span), absolute + fixed positioning, replaced elements
- **Renderer** — software rasterizer: anti-aliased rounded rects, linear/radial
  gradients, box shadows (real gaussian-ish blur), PNG/JPEG/WebP images with
  cover/contain, backdrop blur, transforms, opacity layers, inline SVG
  (paths, arcs, gradients, transforms), glyph rasterizer with embedded
  **Inter** fonts (400–800) and system font fallback
- **JavaScript** — embedded QuickJS + a DOM/Web API layer built for real apps:
  runs **React** apps unmodified (DOM bindings, events, `localStorage`,
  `fetch`, `matchMedia`, `MessageChannel`, dynamic `import()` with a module
  loader over the network stack, timers, `requestAnimationFrame`)
- **Networking** — **HTTP/2** (RFC 7540/7541: ALPN negotiation, HPACK with
  static + dynamic tables and Huffman coding, flow control, SETTINGS/PING/
  GOAWAY handling) with automatic HTTP/1.1 fallback; redirects, chunked
  encoding, gzip/deflate, cookies, data: / file: URLs;
  **TLS 1.2/1.3 via OpenSSL** (Linux) or **Schannel** (Windows), certificate
  verification on by default
- **Interaction** — pointer input drives `:hover` styling and `click` event
  listeners (with `preventDefault()`), links navigate in-place; the browser
  shell adds an URL bar, back/reload buttons and session history
- **Multi-threaded by default** — a worker pool sized to every available core
  drives resource fetching, image decoding and JS fetch tasks; the page runs
  on its own thread with a big stack; painting is thread-safe and can be
  called from any thread

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

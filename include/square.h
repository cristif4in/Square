/* =====================================================================
 *  Square Browser Engine — public C API  (v1.0)
 *  ---------------------------------------------------------------------
 *  Square is a from-scratch browser engine:
 *    - HTML5 parser, CSS parser + cascade (selectors, custom properties)
 *    - layout: block / inline / flexbox / grid / absolute positioning
 *    - software rasterizer: gradients, rounded corners, shadows,
 *      anti-aliased glyphs (embedded Inter), PNG/JPEG/WebP images, SVG
 *    - JavaScript: embedded QuickJS + DOM/Web API bindings (React-ready)
 *    - networking: HTTP/1.1 + TLS (OpenSSL on Linux, Schannel on Windows)
 *      with gzip, chunked encoding, redirects, cookies
 *    - multi-threaded by default: a worker pool sized to the machine
 *      (all cores) drives resource fetching, image decoding and JS tasks
 *
 *  The engine builds as a shared library (libsquare.so / square.dll).
 *  The "square_browser" application is a thin window shell that uses
 *  Square exclusively through this header.
 * ===================================================================== */
#ifndef SQUARE_H
#define SQUARE_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  ifdef SQUARE_BUILD_DLL
#    define SQRL_API __declspec(dllexport)
#  else
#    define SQRL_API __declspec(dllimport)
#  endif
#else
#  define SQRL_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct square_instance square_instance;

/* ---------------- logging ---------------- */
enum { SQ_LOG_DEBUG = 0, SQ_LOG_INFO = 1, SQ_LOG_WARN = 2, SQ_LOG_ERROR = 3 };
typedef void (*square_log_fn)(void* userdata, int level, const char* message);

/* ---------------- instance state ----------------
 * lifecycle of the current page load:
 *   IDLE -> CONNECTING -> DOCUMENT -> STYLES -> ASSETS -> SCRIPT -> READY
 *                                              \--> ERROR (fatal)      */
enum {
  SQ_STATE_IDLE = 0,
  SQ_STATE_CONNECTING = 1,  /* resolving / TLS / requesting main document */
  SQ_STATE_DOCUMENT = 2,    /* main document downloading / parsing       */
  SQ_STATE_STYLES = 3,      /* stylesheets downloading / parsing         */
  SQ_STATE_ASSETS = 4,      /* images & other subresources loading       */
  SQ_STATE_SCRIPT = 5,      /* javascript executing                      */
  SQ_STATE_READY = 6,       /* page rendered; engine keeps living and
                                 repaints when late assets/timers land    */
  SQ_STATE_ERROR = 7        /* fatal (DNS / TLS / HTTP failure)          */
};
typedef void (*square_state_fn)(void* userdata, int state, float progress,
                                const char* info);

/* ---------------- instance management ---------------- */
SQRL_API square_instance* square_create(void);
SQRL_API void             square_destroy(square_instance* inst);

/* viewport in CSS pixels; default 1280x800. Safe to change between paints. */
SQRL_API void square_set_viewport(square_instance* inst, int width, int height);
SQRL_API void square_get_viewport(square_instance* inst, int* out_w, int* out_h);

/* default UA mimics a modern Chrome on the host OS */
SQRL_API void square_set_user_agent(square_instance* inst, const char* ua);

SQRL_API void square_set_log_callback(square_instance* inst, square_log_fn fn, void* ud);
SQRL_API void square_set_state_callback(square_instance* inst, square_state_fn fn, void* ud);

/* number of worker threads in the engine pool (>= 2, scales with cores) */
SQRL_API int  square_get_thread_count(square_instance* inst);

/* ---------------- navigation ---------------- */
/* async: returns immediately, work happens on the engine's page thread */
SQRL_API void square_load_url(square_instance* inst, const char* url);
SQRL_API void square_load_html(square_instance* inst, const char* html_utf8,
                               const char* base_url);
SQRL_API void square_stop(square_instance* inst);

/* ---------------- page info ---------------- */
SQRL_API int         square_get_state(square_instance* inst);
SQRL_API void        square_wait_state(square_instance* inst, int state, int timeout_ms);
SQRL_API const char* square_get_title(square_instance* inst);
SQRL_API const char* square_get_url(square_instance* inst); /* final URL */
SQRL_API int         square_get_page_size(square_instance* inst, int* out_w, int* out_h);

/* ---------------- rendering ----------------
 * BGRA (little-endian ARGB), top-down rows, straight alpha.
 * Thread-safe: may be called from any thread; renders the newest layout.
 * dst_h_px bounds the destination height: if the engine's last frame is
 * taller (the viewport was just resized), only the first dst_h_px rows are
 * copied — without this, a window shrink overflows the caller's buffer.
 * Returns 1 on success, 0 when nothing has been parsed yet. */
SQRL_API int  square_render_bgra(square_instance* inst, uint8_t* dst,
                                 int dst_stride_bytes, int dst_h_px);
/* Save a PNG. full_page=0 -> viewport, 1 -> whole document height.
 * Returns 1 on success. */
SQRL_API int  square_save_png(square_instance* inst, const char* path,
                              int full_page);

/* ---------------- interaction ----------------
 * Feed pointer input; drives :hover styles, click listeners and link
 * navigation. Coordinates are CSS px relative to the viewport.
 * square_send_click returns 1 when the click triggered a navigation. */
SQRL_API void square_send_mouse_move(square_instance* inst, float x, float y);
SQRL_API void square_send_mouse_leave(square_instance* inst);
SQRL_API int  square_send_click(square_instance* inst, float x, float y, int button);

#ifdef __cplusplus
}
#endif
#endif /* SQUARE_H */

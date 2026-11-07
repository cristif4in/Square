
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include <square.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

typedef void (*square_log_fn_t)(void*, int, const char*);

#ifdef __linux__
int run_x11(square_instance* inst, const std::string& url, int vw, int vh);
#endif
#ifdef _WIN32
int run_win32(square_instance* inst, const std::string& url, int vw, int vh);
#endif

static void log_cb(void*, int level, const char* msg) {
  (void)level;
  fprintf(stderr, "[square] %s\n", msg);
}

static std::string g_last_title;
static void state_cb(void* win, int state, float progress, const char* info) {
  (void)progress;
  (void)info;
#ifdef _WIN32
  HWND hwnd = (HWND)win;
  if (state == SQ_STATE_READY || state == SQ_STATE_ERROR) {
    const char* title = square_get_title((square_instance*)GetWindowLongPtr(hwnd, GWLP_USERDATA));
    static wchar_t wtitle[512];
    const char* t = title && *title ? title : "Square";
    char buf[512];
    snprintf(buf, sizeof(buf), "%s — Square", t);
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, wtitle, 512);
    SetWindowTextW(hwnd, wtitle);
  }
  InvalidateRect(hwnd, nullptr, FALSE);
#else
  (void)win;
  (void)state;
#endif
}

static bool is_url(const std::string& s) {
  return s.find("://") != std::string::npos || s.rfind("data:", 0) == 0;
}

/* ============================ headless ============================ */
int run_headless(square_instance* inst, const std::string& url, int vw, int vh,
                        const std::string& out, int full_page) {
  square_set_viewport(inst, vw, vh);
  printf("Square headless: %s -> %s (%dx%d)\n", url.c_str(), out.c_str(), vw, vh);
  square_load_url(inst, url.c_str());
  square_wait_state(inst, SQ_STATE_READY, 45000);
  int st = square_get_state(inst);
  if (st == SQ_STATE_ERROR) {
    fprintf(stderr, "load error\n");
    return 2;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  if (square_save_png(inst, out.c_str(), full_page)) {
    int pw, ph;
    square_get_page_size(inst, &pw, &ph);
    printf("Saved: %s (%dx%d)\n", out.c_str(), vw, full_page ? ph : vh);
    return 0;
  }
  fprintf(stderr, "png save failed\n");
  return 3;
}

/* ============================ main ============================ */
int main(int argc, char** argv) {
  std::string url = "https://example.com/";
  int vw = 1280, vh = 800;
  bool headless = false;
  std::string out = "square_page.png";
  int full_page = 0;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--width" && i + 1 < argc) vw = atoi(argv[++i]);
    else if (a == "--height" && i + 1 < argc) vh = atoi(argv[++i]);
    else if (a == "--headless") {
      headless = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') out = argv[++i];
    } else if (a == "--out" && i + 1 < argc) out = argv[++i];
    else if (a == "--full-page") full_page = 1;
    else url = a;
  }
  /* local file → file:// */
  if (!is_url(url)) {
    FILE* f = fopen(url.c_str(), "rb");
    if (f) {
      fclose(f);
#ifdef _WIN32
      char full[_MAX_PATH];
      _fullpath(full, url.c_str(), _MAX_PATH);
      url = std::string("file://") + full;
#else
      char* full = realpath(url.c_str(), nullptr);
      if (full) {
        url = std::string("file://") + full;
        free(full);
      }
#endif
    } else {
      url = "https://" + url;
    }
  }

  square_instance* inst = square_create();
  square_set_log_callback(inst, log_cb, nullptr);
  printf("Square Browser 1.0 — engine threads: %d\n", square_get_thread_count(inst));

#ifdef _WIN32
  if (!headless) return run_win32(inst, url, vw, vh);
#elif defined(__linux__)
  if (!headless && getenv("DISPLAY")) return run_x11(inst, url, vw, vh);
#endif
  int rc = run_headless(inst, url, vw, vh, out, full_page);
  square_destroy(inst);
  return rc;
}

/* keep log_cb referenced on gui paths */
square_log_fn_t square_browser_log_cb() { return log_cb; }

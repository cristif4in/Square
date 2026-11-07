
#ifdef __linux__

#include <square.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <unistd.h>
#include <string>
#include <vector>

#define TOOLBAR_H 40

int run_headless(square_instance* inst, const std::string& url, int vw, int vh,
                   const std::string& out, int full_page);

struct BrowserUI {
  Display* dpy = nullptr;
  Window win = 0;
  GC gc = nullptr;
  XImage* img = nullptr;
  std::vector<uint8_t> buffer;
  int vw = 0, vh = 0;
  int win_w = 0, win_h = 0;
  std::string address = "https://example.com/";
  bool focused = true;
  square_instance* inst = nullptr;
  int last_state = -1;
  double last_repaint = 0;
  std::vector<std::string> history;
  size_t history_pos = 0;
  Font font = 0;
  XFontStruct* font_struct = nullptr;

  void push_history(const std::string& url) {
    if (!history.empty() && history[history_pos] == url) return;
    history.resize(history_pos + 1);
    history.push_back(url);
    history_pos = history.size() - 1;
    if (history.size() > 64) {
      history.erase(history.begin());
      history_pos = history.size() - 1;
    }
  }
  bool can_back() const { return history_pos > 0; }
  void go_back() {
    if (can_back()) {
      history_pos--;
      address = history[history_pos];
      square_load_url(inst, address.c_str());
    }
  }
  void reload() { square_load_url(inst, address.c_str()); }
};

static void ui_render_frame(BrowserUI& ui) {
  if (ui.vw != ui.win_w || ui.vh != ui.win_h - TOOLBAR_H) {
    ui.vw = ui.win_w;
    ui.vh = std::max(50, ui.win_h - TOOLBAR_H);
    square_set_viewport(ui.inst, ui.vw, ui.vh);
    ui.buffer.assign((size_t)ui.vw * ui.vh * 4, 0);
    if (ui.img) XDestroyImage(ui.img);
    ui.img = XCreateImage(ui.dpy, DefaultVisual(ui.dpy, DefaultScreen(ui.dpy)),
                          24, ZPixmap, 0, (char*)ui.buffer.data(), ui.vw, ui.vh, 32, ui.vw * 4);
  }
  if (!ui.img) return;
  if (square_render_bgra(ui.inst, ui.buffer.data(), ui.vw * 4, ui.vh)) {
    /* blit the engine frame below the toolbar */
    XPutImage(ui.dpy, ui.win, ui.gc, ui.img, 0, 0, 0, TOOLBAR_H, ui.vw, ui.vh);
  }
}

static void ui_draw_toolbar(BrowserUI& ui) {
  Display* dpy = ui.dpy;
  Window w = ui.win;
  XSetForeground(dpy, ui.gc, 0x101820);
  XFillRectangle(dpy, w, ui.gc, 0, 0, ui.win_w, TOOLBAR_H);
  XSetForeground(dpy, ui.gc, 0x233240);
  XDrawLine(dpy, w, ui.gc, 0, TOOLBAR_H - 1, ui.win_w, TOOLBAR_H - 1);
  if (!ui.font) {
    const char* cands[] = {"fixed", "6x13", "8x13", "7x13", "cursor", nullptr};
    for (int i = 0; cands[i]; i++) {
      XFontStruct* fs = XLoadQueryFont(dpy, cands[i]);
      if (fs) {
        ui.font = fs->fid;
        ui.font_struct = fs;
        break;
      }
    }
  }
  XSetForeground(dpy, ui.gc, 0xe6edf3);
  if (ui.font) XSetFont(dpy, ui.gc, ui.font);
  /* back button */
  bool can_back = ui.can_back();
  XSetForeground(dpy, ui.gc, can_back ? 0xe6edf3 : 0x4a5a68);
  XDrawString(dpy, w, ui.gc, 14, 26, "<", 1);
  /* reload button */
  XSetForeground(dpy, ui.gc, 0xe6edf3);
  XDrawString(dpy, w, ui.gc, 38, 26, "r", 1);
  /* address field */
  int addr_x = 58;
  XSetForeground(dpy, ui.gc, 0x1a2430);
  XFillRectangle(dpy, w, ui.gc, addr_x, 8, ui.win_w - addr_x - 12, TOOLBAR_H - 16);
  XSetForeground(dpy, ui.gc, 0x31414f);
  XDrawRectangle(dpy, w, ui.gc, addr_x, 8, ui.win_w - addr_x - 12, TOOLBAR_H - 16);
  if (ui.focused) {
    XSetForeground(dpy, ui.gc, 0x5282ff);
    XDrawRectangle(dpy, w, ui.gc, addr_x - 1, 7, ui.win_w - addr_x - 10, TOOLBAR_H - 14);
  }
  XSetForeground(dpy, ui.gc, 0xe6edf3);
  if (!ui.address.empty()) {
    XDrawString(dpy, w, ui.gc, addr_x + 8, 26, ui.address.c_str(), (int)ui.address.size());
    if (ui.focused) {
      int tw = ui.font_struct
                   ? XTextWidth(ui.font_struct, ui.address.c_str(), (int)ui.address.size())
                   : (int)ui.address.size() * 6;
      XFillRectangle(dpy, w, ui.gc, addr_x + 8 + tw + 1, 12, 7, TOOLBAR_H - 24);
    }
  } else {
    XSetForeground(dpy, ui.gc, 0x6a7a88);
    XDrawString(dpy, w, ui.gc, addr_x + 8, 26, "Enter URL and press Enter", 25);
  }
}

static void ui_navigate(BrowserUI& ui, std::string url) {
  if (url.find("://") == std::string::npos) {
    FILE* f = fopen(url.c_str(), "rb");
    if (f) {
      fclose(f);
      char* full = realpath(url.c_str(), nullptr);
      if (full) {
        url = std::string("file://") + full;
        free(full);
      }
    } else if (!url.empty() && (url.rfind("localhost", 0) == 0 || url.rfind("127.", 0) == 0 ||
                                url.find('.') != std::string::npos)) {
      url = "https://" + url;
    } else {
      url = "https://" + url;
    }
  }
  ui.address = url;
  ui.push_history(url);
  square_load_url(ui.inst, url.c_str());
}

int run_x11(square_instance* inst, const std::string& url, int vw, int vh) {
  BrowserUI ui;
  ui.inst = inst;
  XInitThreads();
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy) return run_headless(inst, url, vw, vh, "square_page.png", 0);
  ui.dpy = dpy;
  int screen = DefaultScreen(dpy);
  Visual* visual = DefaultVisual(dpy, screen);
  /* require TrueColor */
  if (visual->c_class != TrueColor) {
    XCloseDisplay(dpy);
    return run_headless(inst, url, vw, vh, "square_page.png", 0);
  }
  ui.win_w = vw;
  ui.win_h = vh + TOOLBAR_H;
  Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 40, 40, ui.win_w, ui.win_h, 1,
                                   BlackPixel(dpy, screen), WhitePixel(dpy, screen));
  ui.win = win;
  XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                        PointerMotionMask | StructureNotifyMask);
  XStoreName(dpy, win, "Square Browser");
  XMapWindow(dpy, win);
  ui.gc = XCreateGC(dpy, win, 0, nullptr);
  ui.address = url;
  ui.push_history(url);
  ui_navigate(ui, url); /* kick off the initial load */

  Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(dpy, win, &wm_delete, 1);

  bool quit = false;
  while (!quit) {
    while (XPending(dpy)) {
      XEvent ev;
      XNextEvent(dpy, &ev);
      if (ev.type == Expose) {
        ui_render_frame(ui);
      } else if (ev.type == ConfigureNotify) {
        if (ev.xconfigure.width != ui.win_w || ev.xconfigure.height != ui.win_h) {
          ui.win_w = ev.xconfigure.width;
          ui.win_h = ev.xconfigure.height;
        }
      } else if (ev.type == KeyPress && ui.focused) {
        char buf[32];
        KeySym sym;
        int n = XLookupString(&ev.xkey, buf, sizeof(buf), &sym, nullptr);
        if (sym == XK_Return) {
          ui_navigate(ui, ui.address);
        } else if (sym == XK_BackSpace) {
          if (!ui.address.empty()) ui.address.pop_back();
        } else if (sym == XK_Escape) {
          ui.address.clear();
        } else if ((ev.xkey.state & ControlMask) && (sym == XK_u || sym == XK_l)) {
          ui.address.clear();
        } else if (n > 0 && buf[0] >= 32 && buf[0] < 127 && ui.address.size() < 2000) {
          ui.address += buf[0];
        }
      } else if (ev.type == MotionNotify) {
        /* hover styles + JS mousemove tracking (page coords below toolbar) */
        if (ev.xmotion.y >= TOOLBAR_H)
          square_send_mouse_move(ui.inst, (float)ev.xmotion.x, (float)(ev.xmotion.y - TOOLBAR_H));
      } else if (ev.type == ButtonRelease && ev.xbutton.button == Button1) {
        if (ev.xbutton.y < TOOLBAR_H) {
          int mx = ev.xbutton.x;
          if (mx >= 8 && mx <= 26 && ui.can_back()) ui.go_back();
          else if (mx >= 32 && mx <= 50) ui.reload();
        } else {
          square_send_click(ui.inst, (float)ev.xbutton.x, (float)(ev.xbutton.y - TOOLBAR_H), 0);
        }
      } else if (ev.type == ButtonPress) {
        if (ev.xbutton.y < TOOLBAR_H) ui.focused = true;
      } else if (ev.type == ClientMessage) {
        if ((Atom)ev.xclient.data.l[0] == wm_delete) quit = true;
      }
    }
    /* periodic repaint while loading / on state change */
    int st = square_get_state(inst);
    double now = (double)clock() / CLOCKS_PER_SEC;
    if (st != ui.last_state || now - ui.last_repaint > 0.15 || st == SQ_STATE_READY) {
      if (st != ui.last_state) {
        ui.last_state = st;
        if (st == SQ_STATE_READY) {
          const char* title = square_get_title(inst);
          std::string t = (title && *title ? title : "Square") + std::string(" — Square");
          XStoreName(dpy, win, t.c_str());
          /* keep the address bar in sync with in-page link navigation */
          const char* cur = square_get_url(inst);
          if (cur && *cur) ui.address = cur;
        }
      }
      ui_render_frame(ui);
      ui_draw_toolbar(ui);
      XFlush(dpy);
      ui.last_repaint = now;
    }
    usleep(15000);
  }

  if (ui.img) XDestroyImage(ui.img);
  XFreeGC(dpy, ui.gc);
  XDestroyWindow(dpy, win);
  XCloseDisplay(dpy);
  return 0;
}

#endif /* __linux__ */

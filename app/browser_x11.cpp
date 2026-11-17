
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
#include <algorithm>

#define TOOLBAR_H 68
#define TAB_STRIP_H 30
#define TAB_Y 4
#define TAB_H 26
#define TAB_W 220
#define TAB_GAP 4
#define TAB_PAD_L 8
#define ADDR_X 58
#define ADDR_Y (TAB_STRIP_H + 3)
#define ADDR_H 28

int run_headless(square_instance* inst, const std::string& url, int vw, int vh,
                   const std::string& out, int full_page);

struct Tab {
  square_instance* inst = nullptr;
  std::string title = "New Tab";
  std::string url;
  int last_state = -1;
  std::vector<std::string> history;
  size_t hpos = 0;

  void push_history(const std::string& u) {
    if (!history.empty() && history[hpos] == u) return;
    history.resize(hpos + 1);
    history.push_back(u);
    hpos = history.size() - 1;
    if (history.size() > 64) {
      history.erase(history.begin());
      hpos = history.size() - 1;
    }
  }
  bool can_back() const { return hpos > 0; }
  void go_back(square_instance* i) {
    if (!can_back()) return;
    hpos--;
    url = history[hpos];
    square_load_url(i, url.c_str());
  }
};

struct BrowserUI {
  bool drag_sb = false;
  Display* dpy = nullptr;
  Window win = 0;
  GC gc = nullptr;
  XImage* img = nullptr;
  std::vector<uint8_t> buffer;
  int vw = 0, vh = 0;
  int win_w = 0, win_h = 0;
  std::string address;
  bool addr_focused = true;
  size_t caret = 0;
  size_t sel_a = 0, sel_b = 0; /* byte offsets; selection active when a != b */
  square_instance* inst = nullptr;
  int last_state = -1;
  double last_repaint = 0;
  std::vector<Tab> tabs;
  size_t active = 0;
  Font font = 0;
  XFontStruct* font_struct = nullptr;

  Tab& cur() { return tabs[active]; }
  square_instance* cur_inst() { return tabs[active].inst; }
};

static void addr_clear_sel(BrowserUI& ui) { ui.sel_a = ui.sel_b = 0; }
static void addr_caret_end(BrowserUI& ui) {
  ui.caret = ui.address.size();
  addr_clear_sel(ui);
}
static void addr_select_all(BrowserUI& ui) { ui.sel_a = 0; ui.sel_b = ui.address.size(); }
static bool addr_has_sel(BrowserUI& ui) { return ui.sel_a != ui.sel_b; }
static void addr_delete_sel(BrowserUI& ui) {
  size_t a = std::min(ui.sel_a, ui.sel_b), b = std::max(ui.sel_a, ui.sel_b);
  if (a == b) return;
  ui.address.erase(a, b - a);
  ui.caret = a;
  addr_clear_sel(ui);
}
static void addr_insert(BrowserUI& ui, const std::string& t) {
  if (addr_has_sel(ui)) {
    addr_delete_sel(ui);
  } else {
    ui.address.erase(ui.caret);
  }
  ui.address.insert(ui.caret, t);
  ui.caret += t.size();
}

static void ui_render_frame(BrowserUI& ui) {
  if (ui.vw != ui.win_w || ui.vh != ui.win_h - TOOLBAR_H) {
    ui.vw = ui.win_w;
    ui.vh = std::max(50, ui.win_h - TOOLBAR_H);
    square_set_viewport(ui.cur_inst(), ui.vw, ui.vh);
    ui.buffer.assign((size_t)ui.vw * ui.vh * 4, 0);
    if (ui.img) XDestroyImage(ui.img);
    ui.img = XCreateImage(ui.dpy, DefaultVisual(ui.dpy, DefaultScreen(ui.dpy)),
                          24, ZPixmap, 0, (char*)ui.buffer.data(), ui.vw, ui.vh, 32, ui.vw * 4);
  }
  if (!ui.img) return;
  if (square_render_bgra(ui.cur_inst(), ui.buffer.data(), ui.vw * 4, ui.vh)) {
    /* blit the engine frame below the toolbar */
    XPutImage(ui.dpy, ui.win, ui.gc, ui.img, 0, 0, 0, TOOLBAR_H, ui.vw, ui.vh);
  }
}

static void ui_draw_tabs(BrowserUI& ui) {
  Display* dpy = ui.dpy;
  Window w = ui.win;
  for (size_t i = 0; i < ui.tabs.size(); i++) {
    int x = TAB_PAD_L + (int)i * (TAB_W + TAB_GAP);
    bool act = i == ui.active;
    if (act) {
      XSetForeground(dpy, ui.gc, 0x1e2a38);
      XFillRectangle(dpy, w, ui.gc, x + 4, TAB_Y, TAB_W - 8, TAB_H);
      XFillArc(dpy, w, ui.gc, x, TAB_Y, TAB_H, TAB_H, 0, 360 * 64);
      XFillArc(dpy, w, ui.gc, x + TAB_W - TAB_H, TAB_Y, TAB_H, TAB_H, 0, 360 * 64);
    }
    XSetForeground(dpy, ui.gc, act ? 0xe6edf3 : 0x9aa8b4);
    if (ui.font) XSetFont(dpy, ui.gc, ui.font);
    const std::string& ti = ui.tabs[i].title;
    int tw = ui.font_struct
                 ? XTextWidth(ui.font_struct, ti.c_str(), (int)ti.size())
                 : (int)ti.size() * 6;
    int maxw = TAB_W - 40;
    XRectangle clip = {(short)(x + 10), (short)TAB_Y, (unsigned short)maxw,
                       (unsigned short)TAB_H};
    XSetClipRectangles(dpy, ui.gc, 0, 0, &clip, 1, Unsorted);
    XDrawString(dpy, w, ui.gc, x + 10, TAB_Y + TAB_H / 2 + 5, ti.c_str(), (int)ti.size());
    XSetClipMask(dpy, ui.gc, None);
    (void)tw;
    /* close mark */
    int cx = x + TAB_W - 14, cy = TAB_Y + TAB_H / 2;
    XSetForeground(dpy, ui.gc, 0x8a96a2);
    XDrawLine(dpy, w, ui.gc, cx - 4, cy - 4, cx + 4, cy + 4);
    XDrawLine(dpy, w, ui.gc, cx - 4, cy + 4, cx + 4, cy - 4);
  }
  /* new-tab mark */
  int px = TAB_PAD_L + (int)ui.tabs.size() * (TAB_W + TAB_GAP) + 13;
  int py = TAB_Y + TAB_H / 2;
  XSetForeground(dpy, ui.gc, 0xe6edf3);
  XDrawLine(dpy, w, ui.gc, px - 6, py, px + 6, py);
  XDrawLine(dpy, w, ui.gc, px, py - 6, px, py + 6);
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
  if (ui.font) XSetFont(dpy, ui.gc, ui.font);
  ui_draw_tabs(ui);

  /* back / reload buttons sit left of the address field */
  bool can_back = !ui.tabs.empty() && ui.cur().can_back();
  XSetForeground(dpy, ui.gc, can_back ? 0xe6edf3 : 0x4a5a68);
  XDrawString(dpy, w, ui.gc, 14, ADDR_Y + ADDR_H / 2 + 5, "<", 1);
  XSetForeground(dpy, ui.gc, 0xe6edf3);
  XDrawString(dpy, w, ui.gc, 40, ADDR_Y + ADDR_H / 2 + 5, "r", 1);

  /* address field: pill-shaped, magnifier glyph, placeholder, blinking caret */
  int ax = ADDR_X, ay = ADDR_Y, aw = ui.win_w - ax - 12, ah = ADDR_H;
  unsigned long fill = ui.addr_focused ? 0x0c141e : 0x1e2a38;
  XSetForeground(dpy, ui.gc, fill);
  XFillRectangle(dpy, w, ui.gc, ax + 4, ay, aw - 8, ah);
  XFillArc(dpy, w, ui.gc, ax, ay, ah, ah, 0, 360 * 64);
  XFillArc(dpy, w, ui.gc, ax + aw - ah, ay, ah, ah, 0, 360 * 64);
  if (ui.addr_focused) {
    XSetForeground(dpy, ui.gc, 0x3a4a5c);
    XDrawArc(dpy, w, ui.gc, ax, ay, ah, ah, 0, 360 * 64);
    XDrawArc(dpy, w, ui.gc, ax + aw - ah, ay, ah, ah, 0, 360 * 64);
    XDrawLine(dpy, w, ui.gc, ax + ah / 2, ay, ax + aw - ah / 2, ay);
    XDrawLine(dpy, w, ui.gc, ax + ah / 2, ay + ah - 1, ax + aw - ah / 2, ay + ah - 1);
  }
  /* magnifier glyph */
  int icx = ax + 20, icy = ay + ah / 2;
  XSetForeground(dpy, ui.gc, 0x8a96a2);
  XDrawArc(dpy, w, ui.gc, icx - 6, icy - 6, 9, 9, 0, 360 * 64);
  XDrawLine(dpy, w, ui.gc, icx + 4, icy + 4, icx + 8, icy + 8);

  int tx = ax + 34;
  int tclip_w = ax + aw - tx - 8;
  if (!ui.address.empty()) {
    XSetForeground(dpy, ui.gc, 0xe6edf3);
    int tw = ui.font_struct
                 ? XTextWidth(ui.font_struct, ui.address.c_str(), (int)ui.address.size())
                 : (int)ui.address.size() * 6;
    int avail = tclip_w;
    int x = tx;
    if (tw > avail) x = tx + avail - tw; /* keep the tail (caret end) visible */

    /* selection highlight */
    bool has_sel = addr_has_sel(ui);
    size_t sa = std::min(ui.sel_a, ui.sel_b), sb = std::max(ui.sel_a, ui.sel_b);
    if (has_sel && ui.font_struct) {
      int w0 = XTextWidth(ui.font_struct, ui.address.c_str(), (int)sa);
      int w1 = XTextWidth(ui.font_struct, ui.address.c_str(), (int)sb);
      int sx0 = x + w0, sx1 = x + w1;
      if (sx1 > sx0) {
        XSetForeground(dpy, ui.gc, 0x2b5ca8);
        int rx0 = std::max(sx0, tx), rx1 = std::min(sx1, tx + avail);
        if (rx1 > rx0) XFillRectangle(dpy, w, ui.gc, rx0, ay + 4, rx1 - rx0, ah - 8);
      }
    }

    XRectangle clip = {(short)tx, (short)ay, (unsigned short)tclip_w, (unsigned short)ah};
    XSetClipRectangles(dpy, ui.gc, 0, 0, &clip, 1, Unsorted);
    XSetForeground(dpy, ui.gc, 0xe6edf3);
    XDrawString(dpy, w, ui.gc, x, ay + ah / 2 + 5, ui.address.c_str(), (int)ui.address.size());
    if (ui.addr_focused && !has_sel) {
      long nowl = (long)clock();
      if ((nowl / (CLOCKS_PER_SEC / 2)) % 2 == 0) {
        int cw = ui.font_struct
                     ? XTextWidth(ui.font_struct, ui.address.c_str(), (int)ui.caret)
                     : (int)ui.caret * 6;
        int cx = x + cw + 1;
        if (cx > tx + avail) cx = tx + avail;
        XSetForeground(dpy, ui.gc, 0xe6edf3);
        XFillRectangle(dpy, w, ui.gc, cx, ay + 7, 2, ah - 14);
      }
    }
    XSetClipMask(dpy, ui.gc, None);
  } else {
    XSetForeground(dpy, ui.gc, 0x8a96a2);
    const char* ph = "Search or type a URL";
    XRectangle clip = {(short)tx, (short)ay, (unsigned short)tclip_w, (unsigned short)ah};
    XSetClipRectangles(dpy, ui.gc, 0, 0, &clip, 1, Unsorted);
    XDrawString(dpy, w, ui.gc, tx, ay + ah / 2 + 5, ph, (int)strlen(ph));
    XSetClipMask(dpy, ui.gc, None);
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
    } else {
      /* localhost / 127.x dev servers speak plain http */
      if (url.rfind("localhost", 0) == 0 || url.rfind("127.", 0) == 0 || url.rfind("::1", 0) == 0)
        url = "http://" + url;
      else
        url = "https://" + url;
    }
  }
  ui.cur().url = url;
  ui.cur().push_history(url);
  square_load_url(ui.cur_inst(), url.c_str());
}

static void tab_activate(BrowserUI& ui, size_t i) {
  if (i >= ui.tabs.size()) return;
  ui.active = i;
  ui.address = ui.tabs[i].url;
  addr_caret_end(ui);
  ui.addr_focused = false;
}

static void tab_new(BrowserUI& ui) {
  Tab t;
  t.inst = square_create();
  ui.tabs.push_back(t);
  tab_activate(ui, ui.tabs.size() - 1);
  ui.addr_focused = true;
  addr_caret_end(ui);
  /* blank page until the user navigates */
  square_load_html(t.inst, "<html><body></body></html>", "about:blank");
}

static void tab_close(BrowserUI& ui, size_t i) {
  if (i >= ui.tabs.size()) return;
  square_destroy(ui.tabs[i].inst);
  ui.tabs.erase(ui.tabs.begin() + (long)i);
  if (ui.tabs.empty()) exit(0); /* closing the last tab closes the window */
  if (ui.active >= ui.tabs.size()) ui.active = ui.tabs.size() - 1;
  else if (ui.active > i) ui.active--;
  tab_activate(ui, ui.active);
}

int run_x11(square_instance* inst, const std::string& url, int vw, int vh) {
  BrowserUI ui;
  ui.inst = inst;
  Tab t0;
  t0.inst = inst;
  ui.tabs.push_back(t0);
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
  addr_caret_end(ui);
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
      } else if (ev.type == KeyPress) {
        char buf[32];
        KeySym sym;
        int n = XLookupString(&ev.xkey, buf, sizeof(buf), &sym, nullptr);
        bool ctrl = (ev.xkey.state & ControlMask) != 0;
        if (ctrl && sym == XK_t) {
          tab_new(ui);
        } else if (ctrl && sym == XK_w) {
          tab_close(ui, ui.active);
        } else if (ctrl && sym == XK_Tab) {
          if (!ui.tabs.empty()) tab_activate(ui, (ui.active + 1) % ui.tabs.size());
        } else if (ctrl && sym == XK_l) {
          ui.addr_focused = true;
          addr_select_all(ui);
        } else if (ui.addr_focused) {
          if (sym == XK_Return) {
            ui_navigate(ui, ui.address);
          } else if (sym == XK_BackSpace) {
            if (addr_has_sel(ui)) {
              addr_delete_sel(ui);
            } else if (ui.caret > 0) {
              /* strip the last UTF-8 code point before the caret */
              while (ui.caret > 0 && (ui.address[ui.caret - 1] & 0xC0) == 0x80) ui.caret--;
              if (ui.caret > 0) ui.caret--;
              ui.address.erase(ui.caret);
            }
          } else if (sym == XK_Escape) {
            ui.address.clear();
            addr_caret_end(ui);
          } else if (ctrl && sym == XK_a) {
            addr_select_all(ui);
          } else if (n > 0 && buf[0] >= 32 && buf[0] < 127 && ui.address.size() < 2000) {
            addr_insert(ui, std::string(buf, 1));
          }
        }
      } else if (ev.type == MotionNotify) {
        if (ui.drag_sb) {
          square_scrollbar_drag(ui.cur_inst(), ev.xmotion.y - TOOLBAR_H);
        } else if (ev.xmotion.y >= TOOLBAR_H) {
          /* hover styles + JS mousemove tracking (page coords below toolbar) */
          square_send_mouse_move(ui.cur_inst(), (float)ev.xmotion.x,
                                 (float)(ev.xmotion.y - TOOLBAR_H));
        }
      } else if (ev.type == ButtonRelease && ev.xbutton.button == Button1) {
        ui.drag_sb = false;
      } else if (ev.type == ButtonPress) {
        if (ev.xbutton.y < TOOLBAR_H) {
          int mx = ev.xbutton.x, my = ev.xbutton.y;
          if (my < TAB_STRIP_H) {
            /* tab strip: switch, close, or open */
            int px = TAB_PAD_L + (int)ui.tabs.size() * (TAB_W + TAB_GAP) + 13;
            if (mx >= px - 13 && mx <= px + 13) {
              tab_new(ui);
            } else {
              for (size_t i = 0; i < ui.tabs.size(); i++) {
                int x = TAB_PAD_L + (int)i * (TAB_W + TAB_GAP);
                int cx = x + TAB_W - 14;
                if (mx >= cx - 6 && mx <= cx + 6) {
                  tab_close(ui, i);
                  break;
                }
                if (mx >= x && mx <= x + TAB_W) {
                  tab_activate(ui, i);
                  break;
                }
              }
            }
          } else {
            /* back / reload buttons */
            if (mx >= 8 && mx <= 26) {
              if (ui.cur().can_back()) {
                ui.cur().go_back(ui.cur_inst());
                ui.address = ui.cur().url;
                addr_caret_end(ui);
              }
            } else if (mx >= 34 && mx <= 52) {
              square_load_url(ui.cur_inst(), ui.cur().url.c_str());
            } else {
              bool in_field = mx >= ADDR_X && mx < ui.win_w - 12;
              ui.addr_focused = in_field;
              if (in_field) addr_caret_end(ui);
            }
            }
        } else if (ev.xbutton.button == Button1 &&
                   square_scrollbar_hit(ui.cur_inst(), ev.xbutton.x)) {
          ui.addr_focused = false;
          ui.drag_sb = true;
          square_scrollbar_drag(ui.cur_inst(), ev.xbutton.y - TOOLBAR_H);
        } else if (ev.xbutton.button == Button1) {
          ui.addr_focused = false; /* clicking the page blurs the field */
          square_send_mouse_down(ui.cur_inst(), (float)ev.xbutton.x,
                                 (float)(ev.xbutton.y - TOOLBAR_H), 0);
        } else if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5) {
          /* wheel up/down */
          int pw = 0, ph = 0;
          square_get_page_size(ui.cur_inst(), &pw, &ph);
          int scroll_max = ph - ui.vh;
          if (scroll_max < 0) scroll_max = 0;
          int step = 3 * (ev.xbutton.button == Button4 ? -1 : 1) * 40;
          int sy = square_get_scroll(ui.cur_inst()) + step;
          if (sy < 0) sy = 0;
          if (sy > scroll_max) sy = scroll_max;
          square_set_scroll(ui.cur_inst(), sy);
        }
      } else if (ev.type == ClientMessage) {
        if ((Atom)ev.xclient.data.l[0] == wm_delete) quit = true;
      }
    }
    /* periodic repaint while loading / on state change; also drives caret blink */
    double now = (double)clock() / CLOCKS_PER_SEC;
    bool changed = false;
    for (size_t i = 0; i < ui.tabs.size(); i++) {
      Tab& t = ui.tabs[i];
      int st = square_get_state(t.inst);
      if (st == t.last_state) continue;
      t.last_state = st;
      changed = true;
      if (st == SQ_STATE_READY || st == SQ_STATE_ERROR) {
        const char* title = square_get_title(t.inst);
        if (title && *title) t.title = title;
        const char* cur = square_get_url(t.inst);
        if (cur && *cur) t.url = cur;
        if (i == ui.active && !ui.addr_focused) {
          ui.address = t.url;
          addr_caret_end(ui);
        }
        if (i == ui.active) {
          const std::string& ti = ui.tabs[ui.active].title;
          std::string tt = (ti.empty() ? "Square" : ti) + std::string(" — Square");
          XStoreName(dpy, win, tt.c_str());
        }
      }
    }
    if (changed || now - ui.last_repaint > 0.15 || square_get_state(ui.cur_inst()) == SQ_STATE_READY) {
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

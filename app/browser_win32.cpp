
#ifdef _WIN32

#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>

#include <square.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>

#ifndef GET_WHEEL_DELTA_WPARAM
#  define GET_WHEEL_DELTA_WPARAM(w) ((short)HIWORD(w))
#endif

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
#define ADDR_PAD_R 12
#define TIMER_PAINT 1

struct Tab {
  square_instance* inst = nullptr;
  std::string title = "New Tab";
  std::string url;
  int last_state = -1;
};

struct BrowserUI {
  HWND hwnd = nullptr;
  HBRUSH bg_brush = nullptr;
  HFONT font = nullptr;
  HBITMAP dib = nullptr;
  uint8_t* dib_bits = nullptr;
  int dib_w = 0, dib_h = 0;
  int vw = 0, vh = 0;
  int last_state = -1;
  int last_repaint_tick = 0;
  int scroll_y = 0;
  bool drag_sb = false;
  /* tabs */
  std::vector<Tab> tabs;
  size_t active = 0;
  /* address field */
  std::string address;
  bool addr_focused = true;
  size_t caret = 0;
  size_t sel_a = 0, sel_b = 0; /* byte offsets; selection active when a != b */
};

static BrowserUI g_ui;

static Tab& cur_tab() { return g_ui.tabs[g_ui.active]; }
static square_instance* cur_inst() { return g_ui.tabs.empty() ? nullptr : g_ui.tabs[g_ui.active].inst; }

static void addr_clear_sel() { g_ui.sel_a = g_ui.sel_b = 0; }
static void addr_caret_end() {
  g_ui.caret = g_ui.address.size();
  addr_clear_sel();
}
static void addr_select_all() { g_ui.sel_a = 0; g_ui.sel_b = g_ui.address.size(); }
static bool addr_has_sel() { return g_ui.sel_a != g_ui.sel_b; }
static void addr_delete_sel() {
  size_t a = std::min(g_ui.sel_a, g_ui.sel_b), b = std::max(g_ui.sel_a, g_ui.sel_b);
  if (a == b) return;
  g_ui.address.erase(a, b - a);
  g_ui.caret = a;
  addr_clear_sel();
}
static void addr_insert(const std::string& t) {
  if (addr_has_sel()) {
    addr_delete_sel();
  } else {
    g_ui.address.erase(g_ui.caret);
  }
  g_ui.address.insert(g_ui.caret, t);
  g_ui.caret += t.size();
}

static void ui_ensure_dib(int w, int h) {
  if (g_ui.dib && g_ui.dib_w == w && g_ui.dib_h == h) return;
  if (g_ui.dib) { DeleteObject(g_ui.dib); g_ui.dib = nullptr; }
  BITMAPINFO bi;
  memset(&bi, 0, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h; /* top-down */
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  HDC dc = GetDC(nullptr);
  g_ui.dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&g_ui.dib_bits, nullptr, 0);
  ReleaseDC(nullptr, dc);
  g_ui.dib_w = w;
  g_ui.dib_h = h;
}

static void ui_navigate(const std::string& url_in) {
  std::string url = url_in;
  if (url.find("://") == std::string::npos) {
    FILE* f = fopen(url.c_str(), "rb");
    if (f) {
      fclose(f);
      char full[_MAX_PATH];
      _fullpath(full, url.c_str(), _MAX_PATH);
      url = std::string("file:///") + full;
    } else {
      /* localhost / 127.x dev servers speak plain http */
      if (url.rfind("localhost", 0) == 0 || url.rfind("127.", 0) == 0 || url.rfind("::1", 0) == 0)
        url = "http://" + url;
      else
        url = "https://" + url;
    }
  }
  square_load_url(cur_inst(), url.c_str());
}

static void ui_navigate_current() { ui_navigate(g_ui.address); }

static void tab_activate(size_t i) {
  if (i >= g_ui.tabs.size()) return;
  g_ui.active = i;
  g_ui.address = g_ui.tabs[i].url;
  addr_caret_end();
  g_ui.addr_focused = false;
  RECT tb = {0, 0, g_ui.vw, TOOLBAR_H};
  RECT page = {0, TOOLBAR_H, g_ui.vw, TOOLBAR_H + g_ui.vh};
  InvalidateRect(g_ui.hwnd, &tb, FALSE);
  InvalidateRect(g_ui.hwnd, &page, FALSE);
}

static void tab_new() {
  Tab t;
  t.inst = square_create();
  g_ui.tabs.push_back(t);
  tab_activate(g_ui.tabs.size() - 1);
  g_ui.addr_focused = true;
  addr_caret_end();
  /* blank page until the user navigates */
  square_load_html(t.inst, "<html><body></body></html>", "about:blank");
}

static void tab_close(size_t i) {
  if (i >= g_ui.tabs.size()) return;
  square_instance* dead = g_ui.tabs[i].inst;
  /* tear the engine down off the UI thread so the window stays responsive */
  std::thread([dead]() { square_destroy(dead); }).detach();
  g_ui.tabs.erase(g_ui.tabs.begin() + (long)i);
  if (g_ui.tabs.empty()) {
    DestroyWindow(g_ui.hwnd); /* closing the last tab closes the window */
    return;
  }
  if (g_ui.active >= g_ui.tabs.size()) g_ui.active = g_ui.tabs.size() - 1;
  else if (g_ui.active > i) g_ui.active--;
  tab_activate(g_ui.active);
}

static RECT addr_rect(HWND hwnd) {
  RECT rc;
  GetClientRect(hwnd, &rc);
  RECT r = {ADDR_X, ADDR_Y, rc.right - ADDR_PAD_R, ADDR_Y + ADDR_H};
  return r;
}

static RECT tab_rect(size_t i) {
  RECT r = {TAB_PAD_L + (int)i * (TAB_W + TAB_GAP), TAB_Y,
            TAB_PAD_L + (int)i * (TAB_W + TAB_GAP) + TAB_W, TAB_Y + TAB_H};
  return r;
}

static bool ctrl_down() { return (GetKeyState(VK_CONTROL) & 0x8000) != 0; }

/* rounded pill-shaped address field with magnifier glyph, placeholder,
 * selection highlight and blinking caret */
static void ui_draw_address(HDC dc) {
  RECT field = addr_rect(g_ui.hwnd);
  bool focused = g_ui.addr_focused;

  COLORREF fill = focused ? RGB(0x0c, 0x14, 0x1e) : RGB(0x1e, 0x2a, 0x38);
  COLORREF edge = focused ? RGB(0x3a, 0x4a, 0x5c) : RGB(0x1e, 0x2a, 0x38);

  HRGN rgn = CreateRoundRectRgn(field.left, field.top, field.right + 1, field.bottom + 1,
                                ADDR_H, ADDR_H);
  FillRgn(dc, rgn, CreateSolidBrush(fill));
  DeleteObject(rgn);
  HPEN pen = CreatePen(PS_SOLID, 1, edge);
  HGDIOBJ old_pen = SelectObject(dc, pen);
  HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  RoundRect(dc, field.left, field.top, field.right, field.bottom, ADDR_H, ADDR_H);
  SelectObject(dc, old_pen);
  SelectObject(dc, old_brush);
  DeleteObject(pen);

  /* magnifier glyph */
  int icx = field.left + 18, icy = (field.top + field.bottom) / 2 - 1;
  HPEN ipen = CreatePen(PS_SOLID, 2, RGB(0x8a, 0x96, 0xa2));
  HGDIOBJ oldi = SelectObject(dc, ipen);
  HGDIOBJ oldb2 = SelectObject(dc, GetStockObject(NULL_BRUSH));
  Ellipse(dc, icx - 5, icy - 5, icx + 4, icy + 4);
  MoveToEx(dc, icx + 6, icy + 6, nullptr);
  LineTo(dc, icx + 10, icy + 10);
  SelectObject(dc, oldi);
  SelectObject(dc, oldb2);
  DeleteObject(ipen);

  int tx = field.left + 34;
  int ty = field.top + (ADDR_H - 16) / 2; /* 16px font vertical centering */
  RECT inner = {tx + 2, field.top + 2, field.right - 10, field.bottom - 2};

  SetBkMode(dc, TRANSPARENT);
  if (g_ui.address.empty()) {
    SetTextColor(dc, RGB(0x8a, 0x96, 0xa2));
    static wchar_t wph[64];
    MultiByteToWideChar(CP_UTF8, 0, "Search or type a URL", -1, wph, 64);
    DrawTextW(dc, wph, -1, &inner, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    return;
  }

  static wchar_t wtext[2048];
  MultiByteToWideChar(CP_UTF8, 0, g_ui.address.c_str(), -1, wtext, 2048);
  int len = (int)wcslen(wtext);
  SetTextColor(dc, RGB(0xe6, 0xed, 0xf3));
  SIZE sz;
  GetTextExtentPoint32W(dc, wtext, len, &sz);
  int avail = inner.right - tx;
  int x = tx;
  if (sz.cx > avail) x = inner.right - sz.cx; /* keep the tail (caret end) visible */

  /* selection highlight (byte offsets -> wide char indices -> pixels) */
  bool has_sel = addr_has_sel();
  size_t sa = std::min(g_ui.sel_a, g_ui.sel_b), sb = std::max(g_ui.sel_a, g_ui.sel_b);
  int sx0 = 0, sx1 = 0;
  if (has_sel) {
    int w0 = MultiByteToWideChar(CP_UTF8, 0, g_ui.address.c_str(), (int)sa, nullptr, 0);
    int w1 = MultiByteToWideChar(CP_UTF8, 0, g_ui.address.c_str(), (int)sb, nullptr, 0);
    SIZE s0, s1;
    GetTextExtentPoint32W(dc, wtext, w0, &s0);
    GetTextExtentPoint32W(dc, wtext, w1, &s1);
    sx0 = x + s0.cx;
    sx1 = x + s1.cx;
    RECT selr = {std::max(sx0, tx), field.top + 4, std::min(sx1, (int)inner.right),
                 field.bottom - 4};
    if (selr.right > selr.left) FillRect(dc, &selr, CreateSolidBrush(RGB(0x2b, 0x5c, 0xa8)));
  }

  RECT clip = {tx, field.top + 2, field.right - 8, field.bottom - 2};
  ExtTextOutW(dc, x, ty, ETO_CLIPPED, &clip, wtext, len, nullptr);

  if (focused && !has_sel && (GetTickCount() / 530) % 2 == 0) {
    int widx = MultiByteToWideChar(CP_UTF8, 0, g_ui.address.c_str(), (int)g_ui.caret, nullptr, 0);
    SIZE sw;
    GetTextExtentPoint32W(dc, wtext, widx, &sw);
    int cx = x + sw.cx + 1;
    if (cx > inner.right) cx = inner.right;
    RECT caret = {cx, field.top + 7, cx + 2, field.bottom - 7};
    FillRect(dc, &caret, CreateSolidBrush(RGB(0xe6, 0xed, 0xf3)));
  }
}

static void ui_draw_tabs(HDC dc) {
  HGDIOBJ oldf = SelectObject(dc, g_ui.font);
  SetBkMode(dc, TRANSPARENT);
  for (size_t i = 0; i < g_ui.tabs.size(); i++) {
    RECT r = tab_rect(i);
    bool act = i == g_ui.active;
    if (act) {
      HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, 8, 8);
      FillRgn(dc, rgn, CreateSolidBrush(RGB(0x1e, 0x2a, 0x38)));
      DeleteObject(rgn);
    }
    SetTextColor(dc, act ? RGB(0xe6, 0xed, 0xf3) : RGB(0x9a, 0xa8, 0xb4));
    RECT tr = {r.left + 10, r.top, r.right - 24, r.bottom};
    static wchar_t wt[512];
    MultiByteToWideChar(CP_UTF8, 0, g_ui.tabs[i].title.c_str(), -1, wt, 512);
    DrawTextW(dc, wt, -1, &tr, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    /* close mark */
    RECT cr = {r.right - 22, r.top, r.right - 6, r.bottom};
    SetTextColor(dc, RGB(0x8a, 0x96, 0xa2));
    DrawTextW(dc, L"\u00D7", -1, &cr, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
  }
  /* new-tab mark */
  int px = TAB_PAD_L + (int)g_ui.tabs.size() * (TAB_W + TAB_GAP);
  SetTextColor(dc, RGB(0xe6, 0xed, 0xf3));
  RECT pr = {px, TAB_Y, px + 26, TAB_Y + TAB_H};
  DrawTextW(dc, L"+", -1, &pr, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
  SelectObject(dc, oldf);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      g_ui.hwnd = hwnd;
      SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cur_inst());
      g_ui.font = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
      SetTimer(hwnd, TIMER_PAINT, 90, nullptr);
      SetFocus(hwnd);
      return 0;
    }
    case WM_SIZE: {
      int w = LOWORD(lp), h = HIWORD(lp);
      g_ui.vw = w;
      g_ui.vh = std::max(50, h - TOOLBAR_H);
      for (auto& t : g_ui.tabs) square_set_viewport(t.inst, g_ui.vw, g_ui.vh);
      RECT rc = {0, TOOLBAR_H, w, TOOLBAR_H + g_ui.vh};
      InvalidateRect(hwnd, &rc, FALSE);
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT tb = {0, 0, g_ui.vw, TOOLBAR_H};
      FillRect(dc, &tb, g_ui.bg_brush);
      ui_draw_tabs(dc);
      ui_draw_address(dc);
      /* engine frame: render straight into the DIB, then blit once */
      if (g_ui.vw > 0 && g_ui.vh > 0 && cur_inst()) {
        ui_ensure_dib(g_ui.vw, g_ui.vh);
        if (square_render_bgra(cur_inst(), g_ui.dib_bits, g_ui.vw * 4, g_ui.vh)) {
          HDC mem = CreateCompatibleDC(dc);
          HBITMAP old = (HBITMAP)SelectObject(mem, g_ui.dib);
          BitBlt(dc, 0, TOOLBAR_H, g_ui.vw, g_ui.vh, mem, 0, 0, SRCCOPY);
          SelectObject(mem, old);
          DeleteDC(mem);
        }
      }
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_TIMER: {
      /* poll every tab: keep titles/urls fresh, sync the field for the active one */
      for (size_t i = 0; i < g_ui.tabs.size(); i++) {
        Tab& t = g_ui.tabs[i];
        int st = square_get_state(t.inst);
        if (st == t.last_state) continue;
        t.last_state = st;
        if (st == SQ_STATE_READY || st == SQ_STATE_ERROR) {
          const char* title = square_get_title(t.inst);
          if (title && *title) t.title = title;
          const char* cur = square_get_url(t.inst);
          if (cur && *cur) t.url = cur;
          if (i == g_ui.active && !g_ui.addr_focused) {
            g_ui.address = t.url;
            addr_caret_end();
          }
        }
      }
      if (!g_ui.tabs.empty()) {
        const std::string& ti = cur_tab().title;
        char buf[600];
        snprintf(buf, sizeof(buf), "%s — Square", ti.c_str());
        static wchar_t wtitle[600];
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, wtitle, 600);
        SetWindowTextW(hwnd, wtitle);
      }
      RECT rc = {0, TOOLBAR_H, g_ui.vw, TOOLBAR_H + g_ui.vh};
      InvalidateRect(hwnd, &rc, FALSE);
      RECT tb = {0, 0, g_ui.vw, TOOLBAR_H}; /* caret blink + field state */
      InvalidateRect(hwnd, &tb, FALSE);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
      ScreenToClient(hwnd, &pt);
      if (pt.y < TOOLBAR_H) return 0; /* over the toolbar: nothing to scroll */
      int delta = (int)GET_WHEEL_DELTA_WPARAM(wp);
      int pw = 0, ph = 0;
      square_get_page_size(cur_inst(), &pw, &ph);
      int scroll_max = ph - g_ui.vh;
      if (scroll_max < 0) scroll_max = 0;
      g_ui.scroll_y -= delta; /* wheel up = scroll up */
      if (g_ui.scroll_y < 0) g_ui.scroll_y = 0;
      if (g_ui.scroll_y > scroll_max) g_ui.scroll_y = scroll_max;
      square_set_scroll(cur_inst(), g_ui.scroll_y);
      RECT rc = {0, TOOLBAR_H, g_ui.vw, TOOLBAR_H + g_ui.vh};
      InvalidateRect(hwnd, &rc, FALSE);
      return 0;
    }
    case WM_MOUSEMOVE: {
      float x = (float)(short)LOWORD(lp), y = (float)(short)HIWORD(lp);
      if (g_ui.drag_sb) {
        square_scrollbar_drag(cur_inst(), (int)(y - TOOLBAR_H));
        RECT rc = {0, TOOLBAR_H, g_ui.vw, TOOLBAR_H + g_ui.vh};
        InvalidateRect(hwnd, &rc, FALSE);
        return 0;
      }
      if (y >= TOOLBAR_H) square_send_mouse_move(cur_inst(), x, y - TOOLBAR_H);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      float x = (float)(short)LOWORD(lp), y = (float)(short)HIWORD(lp);
      if (y >= TOOLBAR_H && square_scrollbar_hit(cur_inst(), (int)x)) {
        SetFocus(hwnd);
        SetCapture(hwnd);
        g_ui.drag_sb = true;
        square_scrollbar_drag(cur_inst(), (int)(y - TOOLBAR_H));
        RECT rc = {0, TOOLBAR_H, g_ui.vw, TOOLBAR_H + g_ui.vh};
        InvalidateRect(hwnd, &rc, FALSE);
        return 0;
      }
      if (y >= TOOLBAR_H) {
        g_ui.addr_focused = false; /* clicking the page blurs the field */
        RECT tb = {0, 0, g_ui.vw, TOOLBAR_H};
        InvalidateRect(hwnd, &tb, FALSE);
        square_send_mouse_down(cur_inst(), x, y - TOOLBAR_H, 0);
      } else {
        RECT tb = {0, 0, g_ui.vw, TOOLBAR_H};
        if (y < TAB_STRIP_H) {
          /* tab strip: switch, close, or open */
          int px = TAB_PAD_L + (int)g_ui.tabs.size() * (TAB_W + TAB_GAP);
          if (x >= px && x <= px + 26) {
            tab_new();
            InvalidateRect(hwnd, &tb, FALSE);
            return 0;
          }
          for (size_t i = 0; i < g_ui.tabs.size(); i++) {
            RECT r = tab_rect(i);
            if (x >= r.right - 22 && x <= r.right - 6) {
              tab_close(i);
              InvalidateRect(hwnd, &tb, FALSE);
              return 0;
            }
            if (x >= r.left && x <= r.right) {
              tab_activate(i);
              InvalidateRect(hwnd, &tb, FALSE);
              return 0;
            }
          }
          g_ui.addr_focused = false;
        } else {
          RECT field = addr_rect(hwnd);
          POINT pt = {(int)x, (int)y};
          g_ui.addr_focused = PtInRect(&field, pt) != 0;
          if (g_ui.addr_focused) addr_caret_end();
        }
        InvalidateRect(hwnd, &tb, FALSE);
      }
      break; /* normal page click: handled on WM_LBUTTONUP */
    }
    case WM_LBUTTONUP: {
      float x = (float)(short)LOWORD(lp), y = (float)(short)HIWORD(lp);
      if (g_ui.drag_sb) {
        g_ui.drag_sb = false;
        ReleaseCapture();
        return 0; /* end of scrollbar drag, not a page click */
      }
      if (y >= TOOLBAR_H) {
        SetFocus(hwnd); /* wheel events go to the focused window */
        square_send_click(cur_inst(), x, y - TOOLBAR_H, 0);
      }
      return 0;
    }
    case WM_KEYDOWN: {
      if (ctrl_down() && wp == 'T') { tab_new(); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
      if (ctrl_down() && wp == 'W') { tab_close(g_ui.active); return 0; }
      if (ctrl_down() && wp == VK_TAB) {
        if (!g_ui.tabs.empty()) tab_activate((g_ui.active + 1) % g_ui.tabs.size());
        return 0;
      }
      if (ctrl_down() && wp == 'L') {
        g_ui.addr_focused = true;
        addr_select_all();
        RECT tb = {0, 0, g_ui.vw, TOOLBAR_H};
        InvalidateRect(hwnd, &tb, FALSE);
        return 0;
      }
      if (!g_ui.addr_focused) break;
      if (wp == VK_RETURN) ui_navigate_current();
      else if (wp == VK_ESCAPE) {
        g_ui.address.clear();
        addr_caret_end();
        RECT tb = {0, 0, g_ui.vw, TOOLBAR_H};
        InvalidateRect(hwnd, &tb, FALSE);
      } else if (ctrl_down() && wp == 'A') {
        addr_select_all();
        RECT tb = {0, 0, g_ui.vw, TOOLBAR_H};
        InvalidateRect(hwnd, &tb, FALSE);
      } else if (wp == VK_DELETE && addr_has_sel()) {
        addr_delete_sel();
        RECT tb = {0, 0, g_ui.vw, TOOLBAR_H};
        InvalidateRect(hwnd, &tb, FALSE);
      }
      return 0;
    }
    case WM_CHAR: {
      if (!g_ui.addr_focused) break;
      wchar_t ch = (wchar_t)wp;
      if (ch == L'\b') {
        if (addr_has_sel()) addr_delete_sel();
        else if (g_ui.caret > 0) {
          /* strip the last UTF-8 code point before the caret */
          while (g_ui.caret > 0 && (g_ui.address[g_ui.caret - 1] & 0xC0) == 0x80) g_ui.caret--;
          if (g_ui.caret > 0) g_ui.caret--;
          g_ui.address.erase(g_ui.caret);
        }
      } else if (ch >= 32 && ch != 0x7f && g_ui.address.size() < 2000) {
        char enc[4];
        int n;
        if (ch < 0x80) { enc[0] = (char)ch; n = 1; }
        else if (ch < 0x800) {
          enc[0] = (char)(0xC0 | (ch >> 6));
          enc[1] = (char)(0x80 | (ch & 0x3F));
          n = 2;
        } else {
          enc[0] = (char)(0xE0 | (ch >> 12));
          enc[1] = (char)(0x80 | ((ch >> 6) & 0x3F));
          enc[2] = (char)(0x80 | (ch & 0x3F));
          n = 3;
        }
        addr_insert(std::string(enc, n));
      } else {
        break;
      }
      RECT tb = {0, 0, g_ui.vw, TOOLBAR_H};
      InvalidateRect(hwnd, &tb, FALSE);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_DESTROY:
      KillTimer(hwnd, TIMER_PAINT);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

int run_win32(square_instance* inst, const std::string& url, int vw, int vh) {
  Tab t0;
  t0.inst = inst;
  g_ui.tabs.push_back(t0);
  g_ui.address = url;
  addr_caret_end();
  g_ui.bg_brush = CreateSolidBrush(RGB(0x10, 0x1a, 0x24));
  WNDCLASSW wc;
  memset(&wc, 0, sizeof(wc));
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"SquareBrowserWnd";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = g_ui.bg_brush;
  wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  RegisterClassW(&wc);

  RECT r = {0, 0, vw, vh + TOOLBAR_H};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"Square Browser", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                            nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) return 3;
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);
  ui_navigate(url);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}

#endif /* _WIN32 */

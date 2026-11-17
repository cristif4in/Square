
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

#ifndef GET_WHEEL_DELTA_WPARAM
#  define GET_WHEEL_DELTA_WPARAM(w) ((short)HIWORD(w))
#endif

#define TOOLBAR_H 40
#define TIMER_PAINT 1

/* address field metrics (toolbar-local) */
#define ADDR_X 58
#define ADDR_Y 6
#define ADDR_H 28
#define ADDR_PAD_R 12

struct BrowserUI {
  HWND hwnd = nullptr;
  HBRUSH bg_brush = nullptr;
  HFONT font = nullptr;
  HBITMAP dib = nullptr;
  uint8_t* dib_bits = nullptr;
  int dib_w = 0, dib_h = 0;
  int vw = 0, vh = 0;
  square_instance* inst = nullptr;
  int last_state = -1;
  int last_repaint_tick = 0;
  int scroll_y = 0;
  bool drag_sb = false;
  std::string address = "https://example.com/";
  bool addr_focused = true;
};

static BrowserUI g_ui;

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
  square_load_url(g_ui.inst, url.c_str());
}

static RECT addr_rect(HWND hwnd) {
  RECT rc;
  GetClientRect(hwnd, &rc);
  RECT r = {ADDR_X, ADDR_Y, rc.right - ADDR_PAD_R, ADDR_Y + ADDR_H};
  return r;
}

static void ui_navigate_current() { ui_navigate(g_ui.address); }

/* rounded pill-shaped address field with magnifier glyph, placeholder and
 * blinking caret (colors follow the toolbar's dark palette) */
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
  int ty = field.top + (ADDR_H - 18) / 2; /* 18px font baseline compensation */
  RECT inner = {tx + 2, field.top + 2, field.right - 10, field.bottom - 2};

  SetBkMode(dc, TRANSPARENT);
  if (g_ui.address.empty()) {
    SetTextColor(dc, RGB(0x8a, 0x96, 0xa2));
    static wchar_t wph[64];
    MultiByteToWideChar(CP_UTF8, 0, "Search or type a URL", -1, wph, 64);
    DrawTextW(dc, wph, -1, &inner, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
  } else {
    static wchar_t wtext[2048];
    MultiByteToWideChar(CP_UTF8, 0, g_ui.address.c_str(), -1, wtext, 2048);
    int len = (int)wcslen(wtext);
    SetTextColor(dc, RGB(0xe6, 0xed, 0xf3));
    SIZE sz;
    GetTextExtentPoint32W(dc, wtext, len, &sz);
    int avail = inner.right - tx;
    int x = tx;
    if (sz.cx > avail) x = inner.right - sz.cx; /* keep the tail (caret end) visible */
    RECT clip = {tx, field.top + 2, field.right - 8, field.bottom - 2};
    ExtTextOutW(dc, x, ty, ETO_CLIPPED, &clip, wtext, len, nullptr);

    if (focused && (GetTickCount() / 530) % 2 == 0) {
      int cx = sz.cx > avail ? inner.right : x + sz.cx + 1;
      RECT caret = {cx, field.top + 7, cx + 2, field.bottom - 7};
      FillRect(dc, &caret, CreateSolidBrush(RGB(0xe6, 0xed, 0xf3)));
    }
  }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      g_ui.hwnd = hwnd;
      SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)g_ui.inst);
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
      square_set_viewport(g_ui.inst, g_ui.vw, g_ui.vh);
      /* invalidate only the page area — repainting the toolbar every frame
       * is what made it flicker */
      RECT rc = {0, TOOLBAR_H, w, TOOLBAR_H + g_ui.vh};
      InvalidateRect(hwnd, &rc, FALSE);
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      /* toolbar bg */
      RECT tb = {0, 0, g_ui.vw, TOOLBAR_H};
      FillRect(dc, &tb, g_ui.bg_brush);
      ui_draw_address(dc);
      /* engine frame: render straight into the DIB, then blit once */
      if (g_ui.vw > 0 && g_ui.vh > 0) {
        ui_ensure_dib(g_ui.vw, g_ui.vh);
        if (square_render_bgra(g_ui.inst, g_ui.dib_bits, g_ui.vw * 4, g_ui.vh)) {
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
      int st = square_get_state(g_ui.inst);
      if (st != g_ui.last_state) {
        g_ui.last_state = st;
        if (st == SQ_STATE_READY || st == SQ_STATE_ERROR) {
          const char* title = square_get_title(g_ui.inst);
          char buf[600];
          snprintf(buf, sizeof(buf), "%s — Square", title && *title ? title : "Square");
          static wchar_t wtitle[600];
          MultiByteToWideChar(CP_UTF8, 0, buf, -1, wtitle, 600);
          SetWindowTextW(hwnd, wtitle);
          /* keep the address field in sync with in-page link navigation */
          const char* cur = square_get_url(g_ui.inst);
          if (cur && *cur && !g_ui.addr_focused) g_ui.address = cur;
        }
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
      square_get_page_size(g_ui.inst, &pw, &ph);
      int scroll_max = ph - g_ui.vh;
      if (scroll_max < 0) scroll_max = 0;
      g_ui.scroll_y -= delta; /* wheel up = scroll up */
      if (g_ui.scroll_y < 0) g_ui.scroll_y = 0;
      if (g_ui.scroll_y > scroll_max) g_ui.scroll_y = scroll_max;
      square_set_scroll(g_ui.inst, g_ui.scroll_y);
      RECT rc = {0, TOOLBAR_H, g_ui.vw, TOOLBAR_H + g_ui.vh};
      InvalidateRect(hwnd, &rc, FALSE);
      return 0;
    }
    case WM_MOUSEMOVE: {
      float x = (float)(short)LOWORD(lp), y = (float)(short)HIWORD(lp);
      if (g_ui.drag_sb) {
        square_scrollbar_drag(g_ui.inst, (int)(y - TOOLBAR_H));
        RECT rc = {0, TOOLBAR_H, g_ui.vw, TOOLBAR_H + g_ui.vh};
        InvalidateRect(hwnd, &rc, FALSE);
        return 0;
      }
      if (y >= TOOLBAR_H) square_send_mouse_move(g_ui.inst, x, y - TOOLBAR_H);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      float x = (float)(short)LOWORD(lp), y = (float)(short)HIWORD(lp);
      if (y >= TOOLBAR_H && square_scrollbar_hit(g_ui.inst, (int)x)) {
        SetFocus(hwnd);
        SetCapture(hwnd);
        g_ui.drag_sb = true;
        square_scrollbar_drag(g_ui.inst, (int)(y - TOOLBAR_H));
        RECT rc = {0, TOOLBAR_H, g_ui.vw, TOOLBAR_H + g_ui.vh};
        InvalidateRect(hwnd, &rc, FALSE);
        return 0;
      }
      if (y >= TOOLBAR_H) {
        g_ui.addr_focused = false; /* clicking the page blurs the field */
        RECT tb = {0, 0, g_ui.vw, TOOLBAR_H};
        InvalidateRect(hwnd, &tb, FALSE);
        square_send_mouse_down(g_ui.inst, x, y - TOOLBAR_H, 0);
      } else {
        RECT field = addr_rect(hwnd);
        POINT pt = {(int)x, (int)y};
        bool in = PtInRect(&field, pt);
        g_ui.addr_focused = in;
        if (in) SetFocus(hwnd);
        RECT tb = {0, 0, g_ui.vw, TOOLBAR_H};
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
        square_send_click(g_ui.inst, x, y - TOOLBAR_H, 0);
      }
      return 0;
    }
    case WM_KEYDOWN: {
      if (!g_ui.addr_focused) break;
      if (wp == VK_RETURN) ui_navigate_current();
      else if (wp == VK_ESCAPE) {
        g_ui.address.clear();
        RECT tb = {0, 0, g_ui.vw, TOOLBAR_H};
        InvalidateRect(hwnd, &tb, FALSE);
      }
      return 0;
    }
    case WM_CHAR: {
      if (!g_ui.addr_focused) break;
      wchar_t ch = (wchar_t)wp;
      if (ch == L'\b') {
        /* strip the last UTF-8 code point */
        while (!g_ui.address.empty() && (g_ui.address.back() & 0xC0) == 0x80) g_ui.address.pop_back();
        if (!g_ui.address.empty()) g_ui.address.pop_back();
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
        g_ui.address.append(enc, n);
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
  g_ui.inst = inst;
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

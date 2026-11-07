
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

#define TOOLBAR_H 40
#define IDC_ADDRESS 1001
#define IDC_GO 1002
#define TIMER_PAINT 1

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
      url = "https://" + url;
    }
  }
  square_load_url(g_ui.inst, url.c_str());
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      g_ui.hwnd = hwnd;
      SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)g_ui.inst);
      RECT rc;
      GetClientRect(hwnd, &rc);
      g_ui.font = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
      CreateWindowExW(0, L"EDIT", L"https://example.com/",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                      8, 8, rc.right - 100, TOOLBAR_H - 16, hwnd, (HMENU)IDC_ADDRESS, nullptr, nullptr);
      CreateWindowExW(0, L"BUTTON", L"Go",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      rc.right - 84, 8, 76, TOOLBAR_H - 16, hwnd, (HMENU)IDC_GO, nullptr, nullptr);
      SendMessageW(GetDlgItem(hwnd, IDC_ADDRESS), WM_SETFONT, (WPARAM)g_ui.font, TRUE);
      SendMessageW(GetDlgItem(hwnd, IDC_GO), WM_SETFONT, (WPARAM)g_ui.font, TRUE);
      SetTimer(hwnd, TIMER_PAINT, 120, nullptr);
      SetFocus(GetDlgItem(hwnd, IDC_ADDRESS));
      return 0;
    }
    case WM_SIZE: {
      int w = LOWORD(lp), h = HIWORD(lp);
      MoveWindow(GetDlgItem(hwnd, IDC_ADDRESS), 8, 8, w - 100, TOOLBAR_H - 16, TRUE);
      MoveWindow(GetDlgItem(hwnd, IDC_GO), w - 84, 8, 76, TOOLBAR_H - 16, TRUE);
      g_ui.vw = w;
      g_ui.vh = std::max(50, h - TOOLBAR_H);
      square_set_viewport(g_ui.inst, g_ui.vw, g_ui.vh);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      /* toolbar bg */
      RECT tb = {0, 0, g_ui.vw, TOOLBAR_H};
      FillRect(dc, &tb, g_ui.bg_brush);
      /* engine frame */
      if (g_ui.vw > 0 && g_ui.vh > 0) {
        ui_ensure_dib(g_ui.vw, g_ui.vh);
        std::vector<uint8_t> tmp((size_t)g_ui.vw * g_ui.vh * 4);
        if (square_render_bgra(g_ui.inst, tmp.data(), g_ui.vw * 4)) {
          for (int y = 0; y < g_ui.vh; y++)
            memcpy(g_ui.dib_bits + (size_t)y * g_ui.vw * 4, tmp.data() + (size_t)y * g_ui.vw * 4, g_ui.vw * 4);
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
          /* keep the address bar in sync with in-page link navigation */
          const char* cur = square_get_url(g_ui.inst);
          if (cur && *cur) {
            static wchar_t wurl[2048];
            MultiByteToWideChar(CP_UTF8, 0, cur, -1, wurl, 2048);
            SetWindowTextW(GetDlgItem(hwnd, IDC_ADDRESS), wurl);
          }
        }
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEMOVE: {
      float x = (float)(short)LOWORD(lp), y = (float)(short)HIWORD(lp);
      if (y >= TOOLBAR_H) square_send_mouse_move(g_ui.inst, x, y - TOOLBAR_H);
      return 0;
    }
    case WM_LBUTTONUP: {
      float x = (float)(short)LOWORD(lp), y = (float)(short)HIWORD(lp);
      if (y >= TOOLBAR_H) square_send_click(g_ui.inst, x, y - TOOLBAR_H, 0);
      return 0;
    }
    case WM_COMMAND: {
      if (LOWORD(wp) == IDC_GO) {
        wchar_t wurl[2048] = {0};
        GetWindowTextW(GetDlgItem(hwnd, IDC_ADDRESS), wurl, 2048);
        char url[2048];
        WideCharToMultiByte(CP_UTF8, 0, wurl, -1, url, 2048, nullptr, nullptr);
        ui_navigate(url);
      }
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
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
        GetFocus() == GetDlgItem(hwnd, IDC_ADDRESS)) {
      SendMessageW(hwnd, WM_COMMAND, IDC_GO, 0);
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}

#endif /* _WIN32 */

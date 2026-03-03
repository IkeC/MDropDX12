/*
  ToolWindow — base class implementation for standalone tool windows on their own threads.
  Handles: thread lifecycle, message pump, dark theme painting, pin button,
  font +/- with cross-window sync, window size persistence, owner-draw controls.
*/

#include "tool_window.h"
#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

namespace mdrop {

extern Engine g_engine;

//----------------------------------------------------------------------
// Constructor / Destructor
//----------------------------------------------------------------------

ToolWindow::ToolWindow(Engine* pEngine, int defaultW, int defaultH)
  : m_pEngine(pEngine), m_nDefaultW(defaultW), m_nDefaultH(defaultH),
    m_nWndW(defaultW), m_nWndH(defaultH) {}

ToolWindow::~ToolWindow() {
  Close();
}

//----------------------------------------------------------------------
// Open / Close
//----------------------------------------------------------------------

void ToolWindow::Open() {
  if (m_hWnd && IsWindow(m_hWnd)) {
    SetForegroundWindow(m_hWnd);
    return;
  }
  if (m_bThreadRunning.load()) return;

  if (m_thread.joinable())
    m_thread.join();

  m_thread = std::thread(&ToolWindow::CreateOnThread, this);
}

void ToolWindow::Close() {
  if (m_hWnd && IsWindow(m_hWnd))
    PostMessage(m_hWnd, WM_CLOSE, 0, 0);
  if (m_thread.joinable())
    m_thread.join();
}

bool ToolWindow::IsOpen() const {
  return m_hWnd && IsWindow(m_hWnd);
}

//----------------------------------------------------------------------
// Font sync broadcast — notifies all windows except the sender
//----------------------------------------------------------------------

void Engine::BroadcastFontSync(HWND hSender) {
  if (m_hSettingsWnd && IsWindow(m_hSettingsWnd) && m_hSettingsWnd != hSender)
    PostMessage(m_hSettingsWnd, WM_MW_REBUILD_FONTS, 0, 0);
  if (m_displaysWindow && m_displaysWindow->IsOpen() && m_displaysWindow->GetHWND() != hSender)
    PostMessage(m_displaysWindow->GetHWND(), WM_MW_REBUILD_FONTS, 0, 0);
  if (m_songInfoWindow && m_songInfoWindow->IsOpen() && m_songInfoWindow->GetHWND() != hSender)
    PostMessage(m_songInfoWindow->GetHWND(), WM_MW_REBUILD_FONTS, 0, 0);
}

//----------------------------------------------------------------------
// Thread + Window Creation
//----------------------------------------------------------------------

void ToolWindow::LoadWindowPosition() {
  const wchar_t* ini = m_pEngine->GetConfigIniFile();
  const wchar_t* sec = GetINISection();
  m_nWndW = GetPrivateProfileIntW(sec, L"WndW", m_nDefaultW, ini);
  m_nWndH = GetPrivateProfileIntW(sec, L"WndH", m_nDefaultH, ini);
  m_nPosX = GetPrivateProfileIntW(sec, L"PosX", -1, ini);
  m_nPosY = GetPrivateProfileIntW(sec, L"PosY", -1, ini);
  m_bOnTop = GetPrivateProfileIntW(sec, L"OnTop", 0, ini) != 0;
  if (m_nWndW < GetMinWidth()) m_nWndW = GetMinWidth();
  if (m_nWndH < GetMinHeight()) m_nWndH = GetMinHeight();
}

void ToolWindow::SaveWindowPosition() {
  if (!m_hWnd) return;
  const wchar_t* ini = m_pEngine->GetConfigIniFile();
  const wchar_t* sec = GetINISection();
  wchar_t buf[16];
  RECT rc;
  GetWindowRect(m_hWnd, &rc);
  swprintf(buf, 16, L"%d", rc.right - rc.left);
  WritePrivateProfileStringW(sec, L"WndW", buf, ini);
  swprintf(buf, 16, L"%d", rc.bottom - rc.top);
  WritePrivateProfileStringW(sec, L"WndH", buf, ini);
  swprintf(buf, 16, L"%d", (int)rc.left);
  WritePrivateProfileStringW(sec, L"PosX", buf, ini);
  swprintf(buf, 16, L"%d", (int)rc.top);
  WritePrivateProfileStringW(sec, L"PosY", buf, ini);
  WritePrivateProfileStringW(sec, L"OnTop", m_bOnTop ? L"1" : L"0", ini);
}

void ToolWindow::CreateOnThread() {
  m_bThreadRunning.store(true);
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

  // Register window class (idempotent — RegisterClassEx fails silently if already registered)
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = BaseWndProc;
  wc.hInstance = GetModuleHandle(NULL);
  wc.lpszClassName = GetWindowClass();
  wc.hbrBackground = m_pEngine->m_bSettingsDarkTheme
    ? CreateSolidBrush(m_pEngine->m_colSettingsBg)
    : (HBRUSH)(COLOR_BTNFACE + 1);
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
  RegisterClassExW(&wc);

  // Init common controls
  INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_BAR_CLASSES | ICC_UPDOWN_CLASS | ICC_TAB_CLASSES };
  InitCommonControlsEx(&icex);

  // Ensure theme brushes are ready
  m_pEngine->LoadSettingsThemeFromINI();

  // Load persisted size/position
  LoadWindowPosition();

  int posX, posY;
  if (m_nPosX >= 0 && m_nPosY >= 0) {
    posX = m_nPosX;
    posY = m_nPosY;
  } else {
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    posX = (screenW - m_nWndW) / 2;
    posY = (screenH - m_nWndH) / 2;
  }

  DWORD exStyle = WS_EX_TOOLWINDOW;
  if (m_bOnTop) exStyle |= WS_EX_TOPMOST;

  m_hWnd = CreateWindowExW(
    exStyle,
    GetWindowClass(), GetWindowTitle(),
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
    posX, posY, m_nWndW, m_nWndH,
    NULL, NULL, GetModuleHandle(NULL), (LPVOID)this);

  if (!m_hWnd) {
    CoUninitialize();
    m_bThreadRunning.store(false);
    return;
  }

  DoBuildControls();
  ApplyDarkTheme();

  ShowWindow(m_hWnd, SW_SHOW);
  UpdateWindow(m_hWnd);

  // Own message pump on this thread
  MSG msg;
  while (GetMessage(&msg, NULL, 0, 0)) {
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
      PostMessage(m_hWnd, WM_CLOSE, 0, 0);
      continue;
    }
    if (!IsDialogMessage(m_hWnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }

  m_hWnd = NULL;
  CoUninitialize();
  m_bThreadRunning.store(false);
}

//----------------------------------------------------------------------
// Dark Theme
//----------------------------------------------------------------------

void ToolWindow::ApplyDarkTheme() {
  if (!m_hWnd) return;

  m_pEngine->LoadSettingsThemeFromINI();

  BOOL bDark = m_pEngine->m_bSettingsDarkTheme ? TRUE : FALSE;
  DwmSetWindowAttribute(m_hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &bDark, sizeof(bDark));
  if (m_pEngine->m_bSettingsDarkTheme) {
    DwmSetWindowAttribute(m_hWnd, 35 /* DWMWA_CAPTION_COLOR */, &m_pEngine->m_colSettingsBg, sizeof(COLORREF));
    DwmSetWindowAttribute(m_hWnd, 34 /* DWMWA_BORDER_COLOR */, &m_pEngine->m_colSettingsBorder, sizeof(COLORREF));
    DwmSetWindowAttribute(m_hWnd, 36 /* DWMWA_TEXT_COLOR */, &m_pEngine->m_colSettingsText, sizeof(COLORREF));
  }

  for (HWND hChild : m_childCtrls) {
    if (hChild && IsWindow(hChild)) {
      wchar_t szClass[32];
      GetClassNameW(hChild, szClass, 32);
      if (_wcsicmp(szClass, WC_TABCONTROLW) == 0)
        SetWindowTheme(hChild, m_pEngine->m_bSettingsDarkTheme ? L"" : NULL,
                                m_pEngine->m_bSettingsDarkTheme ? L"" : NULL);
      else
        SetWindowTheme(hChild, m_pEngine->m_bSettingsDarkTheme ? L"DarkMode_Explorer" : NULL, NULL);
    }
  }

  RedrawWindow(m_hWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
}

//----------------------------------------------------------------------
// Font helpers
//----------------------------------------------------------------------

int ToolWindow::GetLineHeight() {
  if (!m_hFont || !m_hWnd) return 26;
  HDC hdc = GetDC(m_hWnd);
  if (!hdc) return 26;
  HFONT hOld = (HFONT)SelectObject(hdc, m_hFont);
  TEXTMETRIC tm = {};
  GetTextMetrics(hdc, &tm);
  SelectObject(hdc, hOld);
  ReleaseDC(m_hWnd, hdc);
  int h = tm.tmHeight + tm.tmExternalLeading + 6;
  return max(h, 20);
}

ToolWindow::BaseLayout ToolWindow::BuildBaseControls() {
  HWND hw = m_hWnd;

  // Create fonts from shared font size
  if (m_hFont) DeleteObject(m_hFont);
  m_hFont = CreateFontW(m_pEngine->m_nSettingsFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  if (m_hFontBold) DeleteObject(m_hFontBold);
  m_hFontBold = CreateFontW(m_pEngine->m_nSettingsFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  RECT rcWnd;
  GetClientRect(hw, &rcWnd);
  int clientW = rcWnd.right;

  int lineH = GetLineHeight();
  int gap = 6, x = 16;
  int rw = clientW - x * 2;
  int y = 8;

  // Font +/- buttons (top-left)
  {
    int btnW = lineH;
    TrackControl(CreateBtn(hw, L"\u2212", GetFontMinusControlID(), x, y, btnW, lineH, m_hFont));
    TrackControl(CreateBtn(hw, L"+", GetFontPlusControlID(), x + btnW + 4, y, btnW, lineH, m_hFont));
  }

  // Pin button (top-right)
  {
    if (m_hPinFont) DeleteObject(m_hPinFont);
    int pinSize = lineH;
    m_hPinFont = CreateFontW(-pinSize + 4, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");

    int pinX = clientW - pinSize - x;
    HWND hPin = CreateWindowExW(0, L"BUTTON", L"\xE718",
      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
      pinX, y, pinSize, pinSize, hw,
      (HMENU)(INT_PTR)GetPinControlID(), GetModuleHandle(NULL), NULL);
    if (hPin) {
      if (m_hPinFont) SendMessage(hPin, WM_SETFONT, (WPARAM)m_hPinFont, TRUE);
      SetPropW(hPin, L"IsPinBtn", (HANDLE)(intptr_t)1);
      HWND hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hw, NULL, GetModuleHandle(NULL), NULL);
      if (hTip) {
        TTTOOLINFOW ti = { sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
        ti.hwnd = hw;
        ti.uId = (UINT_PTR)hPin;
        ti.lpszText = (LPWSTR)L"Always on top";
        SendMessageW(hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
      }
    }
    TrackControl(hPin);
  }

  y += lineH + gap + 4;
  return { y, lineH, gap, x, rw, clientW };
}

void ToolWindow::ResetPosition() {
  if (!m_hWnd || !IsWindow(m_hWnd)) return;
  int screenW = GetSystemMetrics(SM_CXSCREEN);
  int screenH = GetSystemMetrics(SM_CYSCREEN);
  int posX = (screenW - m_nDefaultW) / 2;
  int posY = (screenH - m_nDefaultH) / 2;
  m_nWndW = m_nDefaultW;
  m_nWndH = m_nDefaultH;
  m_bOnTop = false;
  SetWindowPos(m_hWnd, HWND_NOTOPMOST, posX, posY, m_nDefaultW, m_nDefaultH, SWP_SHOWWINDOW);
  RebuildFonts();
}

void ToolWindow::RebuildFonts() {
  if (!m_hWnd) return;

  // Destroy all child windows
  HWND hChild = GetWindow(m_hWnd, GW_CHILD);
  while (hChild) {
    HWND hNext = GetWindow(hChild, GW_HWNDNEXT);
    DestroyWindow(hChild);
    hChild = hNext;
  }
  m_childCtrls.clear();

  DoBuildControls();
  ApplyDarkTheme();
}

//----------------------------------------------------------------------
// Base WndProc — handles common messages, delegates to subclass virtuals
//----------------------------------------------------------------------

LRESULT CALLBACK ToolWindow::BaseWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  // Store 'this' pointer on creation
  if (uMsg == WM_NCCREATE) {
    CREATESTRUCTW* pcs = (CREATESTRUCTW*)lParam;
    if (pcs && pcs->lpCreateParams)
      SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)pcs->lpCreateParams);
  }
  ToolWindow* tw = (ToolWindow*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
  if (!tw) return DefWindowProcW(hWnd, uMsg, wParam, lParam);

  Engine* p = tw->m_pEngine;

  switch (uMsg) {
  case WM_CLOSE:
    DestroyWindow(hWnd);
    return 0;

  case WM_DESTROY:
    {
      tw->SaveWindowPosition();

      // Let subclass clean up
      tw->DoDestroy();

      // Clean up base resources
      tw->m_hWnd = NULL;
      tw->m_childCtrls.clear();
      if (tw->m_hFont) { DeleteObject(tw->m_hFont); tw->m_hFont = NULL; }
      if (tw->m_hFontBold) { DeleteObject(tw->m_hFontBold); tw->m_hFontBold = NULL; }
      if (tw->m_hPinFont) { DeleteObject(tw->m_hPinFont); tw->m_hPinFont = NULL; }
    }
    PostQuitMessage(0);
    return 0;

  case WM_SIZE:
    if (wParam != SIZE_MINIMIZED) {
      RECT rc;
      GetWindowRect(hWnd, &rc);
      tw->m_nWndW = rc.right - rc.left;
      tw->m_nWndH = rc.bottom - rc.top;
      tw->RebuildFonts();
    }
    return 0;

  case WM_GETMINMAXINFO:
  {
    MINMAXINFO* mmi = (MINMAXINFO*)lParam;
    mmi->ptMinTrackSize.x = tw->GetMinWidth();
    mmi->ptMinTrackSize.y = tw->GetMinHeight();
    HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfo(hMon, &mi)) {
      mmi->ptMaxTrackSize.x = mi.rcWork.right - mi.rcWork.left;
      mmi->ptMaxTrackSize.y = mi.rcWork.bottom - mi.rcWork.top;
    }
    return 0;
  }

  case WM_MW_REBUILD_FONTS:
    tw->RebuildFonts();
    return 0;

  case WM_MW_RESET_WINDOW:
    tw->ResetPosition();
    return 0;

  // ── Sliders ──
  case WM_HSCROLL:
  {
    HWND hTrack = (HWND)lParam;
    int id = GetDlgCtrlID(hTrack);
    int pos = (int)SendMessage(hTrack, TBM_GETPOS, 0, 0);
    LRESULT r = tw->DoHScroll(hWnd, id, pos);
    if (r != -1) return r;
    break;
  }

  // ── Notifications ──
  case WM_NOTIFY:
  {
    NMHDR* pnm = (NMHDR*)lParam;
    LRESULT r = tw->DoNotify(hWnd, pnm);
    if (r != -1) return r;
    break;
  }

  // ── Commands ──
  case WM_COMMAND:
  {
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);

    // Pin button (common)
    if (id == tw->GetPinControlID() && code == BN_CLICKED) {
      tw->m_bOnTop = !tw->m_bOnTop;
      SetWindowPos(hWnd, tw->m_bOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      InvalidateRect((HWND)lParam, NULL, TRUE);
      return 0;
    }

    // Font + (common)
    if (id == tw->GetFontPlusControlID() && code == BN_CLICKED) {
      if (p->m_nSettingsFontSize > -24) {
        p->m_nSettingsFontSize -= 2;
        tw->RebuildFonts();
        p->BroadcastFontSync(hWnd);
      }
      return 0;
    }

    // Font - (common)
    if (id == tw->GetFontMinusControlID() && code == BN_CLICKED) {
      if (p->m_nSettingsFontSize < -12) {
        p->m_nSettingsFontSize += 2;
        tw->RebuildFonts();
        p->BroadcastFontSync(hWnd);
      }
      return 0;
    }

    // Owner-draw BN_CLICKED: toggle checkbox/radio state
    if (code == BN_CLICKED) {
      HWND hCtrl = (HWND)lParam;
      bool bIsCheckbox = (bool)(intptr_t)GetPropW(hCtrl, L"IsCheckbox");
      bool bIsRadio = (bool)(intptr_t)GetPropW(hCtrl, L"IsRadio");

      if (bIsRadio || bIsCheckbox) {
        // Subclass handles the actual toggle + state update
        // We just dispatch — the subclass gets the raw id/code
      }
    }

    // Delegate to subclass
    LRESULT r = tw->DoCommand(hWnd, id, code, lParam);
    if (r != -1) return r;
    break;
  }

  // ── Dark theme painting ──
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
    if (p->m_bSettingsDarkTheme && p->m_hBrSettingsCtrlBg) {
      SetTextColor((HDC)wParam, p->m_colSettingsText);
      SetBkColor((HDC)wParam, p->m_colSettingsCtrlBg);
      return (LRESULT)p->m_hBrSettingsCtrlBg;
    }
    break;

  case WM_CTLCOLORSTATIC:
    if (p->m_bSettingsDarkTheme && p->m_hBrSettingsBg) {
      HDC hdc = (HDC)wParam;
      HWND hCtrl = (HWND)lParam;
      wchar_t szClass[32];
      GetClassNameW(hCtrl, szClass, 32);
      if (_wcsicmp(szClass, L"Edit") == 0) {
        SetTextColor(hdc, p->m_colSettingsText);
        SetBkColor(hdc, p->m_colSettingsCtrlBg);
        return (LRESULT)p->m_hBrSettingsCtrlBg;
      }
      SetTextColor(hdc, p->m_colSettingsText);
      SetBkColor(hdc, p->m_colSettingsBg);
      SetBkMode(hdc, TRANSPARENT);
      return (LRESULT)p->m_hBrSettingsBg;
    }
    break;

  case WM_CTLCOLORBTN:
    if (p->m_bSettingsDarkTheme && p->m_hBrSettingsBg) {
      SetTextColor((HDC)wParam, p->m_colSettingsText);
      SetBkColor((HDC)wParam, p->m_colSettingsBg);
      return (LRESULT)p->m_hBrSettingsBg;
    }
    break;

  case WM_DRAWITEM:
  {
    DRAWITEMSTRUCT* pDIS = (DRAWITEMSTRUCT*)lParam;
    if (pDIS && pDIS->CtlType == ODT_TAB) {
      bool bSelected = (pDIS->itemState & ODS_SELECTED) != 0;
      HDC hdc = pDIS->hDC;
      RECT rc = pDIS->rcItem;
      if (p->m_bSettingsDarkTheme) {
        COLORREF bg = bSelected ? p->m_colSettingsCtrlBg : p->m_colSettingsBtnFace;
        HBRUSH hBr = CreateSolidBrush(bg);
        FillRect(hdc, &rc, hBr);
        DeleteObject(hBr);
        if (bSelected) {
          HPEN hiPen = CreatePen(PS_SOLID, 1, p->m_colSettingsBtnHi);
          HPEN shPen = CreatePen(PS_SOLID, 1, p->m_colSettingsBtnShadow);
          HPEN oldPen = (HPEN)SelectObject(hdc, hiPen);
          MoveToEx(hdc, rc.left, rc.top, NULL);
          LineTo(hdc, rc.right - 1, rc.top);
          MoveToEx(hdc, rc.left, rc.top, NULL);
          LineTo(hdc, rc.left, rc.bottom);
          SelectObject(hdc, shPen);
          MoveToEx(hdc, rc.right - 1, rc.top, NULL);
          LineTo(hdc, rc.right - 1, rc.bottom);
          SelectObject(hdc, oldPen);
          DeleteObject(hiPen);
          DeleteObject(shPen);
        } else {
          HPEN shPen = CreatePen(PS_SOLID, 1, p->m_colSettingsBtnShadow);
          HPEN oldPen = (HPEN)SelectObject(hdc, shPen);
          MoveToEx(hdc, rc.left, rc.bottom - 1, NULL);
          LineTo(hdc, rc.right, rc.bottom - 1);
          SelectObject(hdc, oldPen);
          DeleteObject(shPen);
        }
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, bSelected ? p->m_colSettingsHighlightText : p->m_colSettingsText);
      } else {
        FillRect(hdc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
      }
      wchar_t szText[64] = {};
      TCITEMW tci = {};
      tci.mask = TCIF_TEXT;
      tci.pszText = szText;
      tci.cchTextMax = 64;
      SendMessageW(pDIS->hwndItem, TCM_GETITEMW, pDIS->itemID, (LPARAM)&tci);
      DrawTextW(hdc, szText, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      return TRUE;
    }
    if (pDIS && pDIS->CtlType == ODT_BUTTON) {
      // Pin button
      bool bIsPinBtn = (bool)(intptr_t)GetPropW(pDIS->hwndItem, L"IsPinBtn");
      if (bIsPinBtn) {
        HDC hdc = pDIS->hDC;
        RECT rc = pDIS->rcItem;
        bool pressed = (pDIS->itemState & ODS_SELECTED) != 0;
        bool pinned = tw->m_bOnTop;
        COLORREF bg = p->m_bSettingsDarkTheme ? p->m_colSettingsBg : GetSysColor(COLOR_BTNFACE);
        HBRUSH hBr = CreateSolidBrush(bg);
        FillRect(hdc, &rc, hBr);
        DeleteObject(hBr);
        SetBkMode(hdc, TRANSPARENT);
        COLORREF pinCol = pinned
          ? (p->m_bSettingsDarkTheme ? RGB(100, 180, 255) : RGB(0, 100, 200))
          : (p->m_bSettingsDarkTheme ? RGB(120, 120, 120) : RGB(160, 160, 160));
        SetTextColor(hdc, pinCol);
        HFONT hOld = tw->m_hPinFont ? (HFONT)SelectObject(hdc, tw->m_hPinFont) : NULL;
        RECT textRc = rc;
        if (pressed) OffsetRect(&textRc, 1, 1);
        DrawTextW(hdc, L"\xE718", 1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (hOld) SelectObject(hdc, hOld);
        return TRUE;
      }
      // Checkbox, radio, regular button
      bool bIsCheckbox = (bool)(intptr_t)GetPropW(pDIS->hwndItem, L"IsCheckbox");
      bool bIsRadio = (bool)(intptr_t)GetPropW(pDIS->hwndItem, L"IsRadio");
      if (bIsCheckbox) {
        DrawOwnerCheckbox(pDIS, p->m_bSettingsDarkTheme,
          p->m_colSettingsBg, p->m_colSettingsCtrlBg, p->m_colSettingsBorder, p->m_colSettingsText);
      } else if (bIsRadio) {
        DrawOwnerRadio(pDIS, p->m_bSettingsDarkTheme,
          p->m_colSettingsBg, p->m_colSettingsCtrlBg, p->m_colSettingsBorder, p->m_colSettingsText);
      } else {
        DrawOwnerButton(pDIS, p->m_bSettingsDarkTheme,
          p->m_colSettingsBtnFace, p->m_colSettingsBtnHi, p->m_colSettingsBtnShadow, p->m_colSettingsText);
      }
      return TRUE;
    }
    break;
  }

  case WM_ERASEBKGND:
    if (p->m_bSettingsDarkTheme && p->m_hBrSettingsBg) {
      HDC hdc = (HDC)wParam;
      RECT rc;
      GetClientRect(hWnd, &rc);
      FillRect(hdc, &rc, p->m_hBrSettingsBg);
      return 1;
    }
    break;
  }

  return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

} // namespace mdrop

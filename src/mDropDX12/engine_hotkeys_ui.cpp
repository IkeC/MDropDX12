/*
  HotkeysWindow — Keyboard Shortcuts window (ToolWindow subclass).
  Shows all configurable hotkey bindings in a ListView, with a hotkey
  capture control, Set/Clear buttons, scope toggle, and Reset to Defaults.
  Launched from "Hotkeys..." button on the Settings System tab, or Ctrl+F7.
*/

#include "tool_window.h"
#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include <commctrl.h>

namespace mdrop {

extern Engine g_engine;

//----------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------

HotkeysWindow::HotkeysWindow(Engine* pEngine)
  : ToolWindow(pEngine, 580, 520) {}

//----------------------------------------------------------------------
// Engine bridge: Open/Close via Engine members
//----------------------------------------------------------------------

void Engine::OpenHotkeysWindow() {
  if (!m_hotkeysWindow)
    m_hotkeysWindow = std::make_unique<HotkeysWindow>(this);
  m_hotkeysWindow->Open();
}

void Engine::CloseHotkeysWindow() {
  if (m_hotkeysWindow)
    m_hotkeysWindow->Close();
}

//----------------------------------------------------------------------
// Common control flags
//----------------------------------------------------------------------

DWORD HotkeysWindow::GetCommonControlFlags() const {
  return ICC_HOTKEY_CLASS | ICC_LISTVIEW_CLASSES;
}

//----------------------------------------------------------------------
// Helper: refresh ListView contents from m_hotkeys[]
//----------------------------------------------------------------------

static bool IsLaunchAppAction(int actionId) {
  return actionId >= HK_LAUNCH_APP_1 && actionId <= HK_LAUNCH_APP_4;
}
static int LaunchAppSlot(int actionId) {
  return actionId - HK_LAUNCH_APP_1;  // 0-3
}

static void RefreshHotkeyList(HWND hList, Engine* p)
{
  if (!hList) return;
  ListView_DeleteAllItems(hList);

  for (int i = 0; i < NUM_HOTKEYS; i++) {
    // For Launch App rows, append configured exe name
    std::wstring actionName = p->m_hotkeys[i].szAction;
    if (IsLaunchAppAction(p->m_hotkeys[i].id)) {
      int slot = LaunchAppSlot(p->m_hotkeys[i].id);
      if (p->m_szLaunchApp[slot][0] != L'\0') {
        const wchar_t* exeName = wcsrchr(p->m_szLaunchApp[slot], L'\\');
        if (!exeName) exeName = wcsrchr(p->m_szLaunchApp[slot], L'/');
        exeName = exeName ? exeName + 1 : p->m_szLaunchApp[slot];
        actionName += L" (";
        actionName += exeName;
        actionName += L")";
      }
    }

    LVITEMW lvi = {};
    lvi.mask = LVIF_TEXT;
    lvi.iItem = i;
    lvi.iSubItem = 0;
    lvi.pszText = (LPWSTR)actionName.c_str();
    SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);

    // Shortcut column
    std::wstring shortcut = p->FormatHotkeyDisplay(p->m_hotkeys[i].modifiers, p->m_hotkeys[i].vk);
    lvi.iSubItem = 1;
    lvi.pszText = (LPWSTR)shortcut.c_str();
    SendMessageW(hList, LVM_SETITEMTEXTW, i, (LPARAM)&lvi);

    // Scope column
    const wchar_t* scope = (p->m_hotkeys[i].vk == 0) ? L"-" :
      (p->m_hotkeys[i].scope == HKSCOPE_GLOBAL ? L"Global" : L"Local");
    lvi.iSubItem = 2;
    lvi.pszText = (LPWSTR)scope;
    SendMessageW(hList, LVM_SETITEMTEXTW, i, (LPARAM)&lvi);
  }
}

//----------------------------------------------------------------------
// Helper: save bindings and re-register global hotkeys
//----------------------------------------------------------------------

static void SaveAndReRegister(Engine* p)
{
  p->SaveHotkeySettings();
  HWND hRender = p->GetPluginWindow();
  if (hRender)
    PostMessage(hRender, WM_MW_REGISTER_HOTKEYS, 0, 0);
}

//----------------------------------------------------------------------
// Build Controls
//----------------------------------------------------------------------

void HotkeysWindow::DoBuildControls()
{
  HWND hw = m_hWnd;
  if (!hw) return;

  auto L = BuildBaseControls();
  int y = L.y, lineH = L.lineH, gap = L.gap, x = L.x, rw = L.rw;
  HFONT hFont = GetFont();
  HFONT hFontBold = GetFontBold();

  // Title label
  TrackControl(CreateLabel(hw, L"Keyboard Shortcuts", x, y, rw, lineH, hFontBold));
  y += lineH + gap;

  // ListView (report mode, full row select, single select)
  int listH = lineH * (NUM_HOTKEYS + 2);  // enough for all rows + header
  HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE,
    WC_LISTVIEWW, NULL,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP |
    LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
    x, y, rw, listH, hw,
    (HMENU)(INT_PTR)IDC_MW_HOTKEYS_LIST, GetModuleHandle(NULL), NULL);
  if (hList) {
    ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    if (hFont) SendMessage(hList, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Columns: Action, Shortcut, Scope
    int colAction = MulDiv(rw, 45, 100);
    int colScope = MulDiv(rw, 18, 100);
    int colShortcut = rw - colAction - colScope - GetSystemMetrics(SM_CXVSCROLL) - 4;

    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = (LPWSTR)L"Action";
    col.cx = colAction;
    SendMessageW(hList, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Shortcut";
    col.cx = colShortcut;
    SendMessageW(hList, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Scope";
    col.cx = colScope;
    SendMessageW(hList, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);

    RefreshHotkeyList(hList, m_pEngine);
    ListView_SetItemState(hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  }
  TrackControl(hList);
  y += listH + gap + 4;

  // "Press key combo:" label
  TrackControl(CreateLabel(hw, L"Press key combo:", x, y, rw, lineH, hFont));
  y += lineH + 2;

  // HOTKEY_CLASS control + Set + Clear buttons
  int btnW = MulDiv(60, lineH, 26);
  int btnGap = 8;
  int editW = rw - 2 * btnW - 2 * btnGap;

  HWND hHotkey = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, NULL,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
    x, y, editW, lineH, hw,
    (HMENU)(INT_PTR)IDC_MW_HOTKEYS_EDIT, GetModuleHandle(NULL), NULL);
  if (hHotkey && hFont) SendMessage(hHotkey, WM_SETFONT, (WPARAM)hFont, TRUE);
  TrackControl(hHotkey);

  int bx = x + editW + btnGap;
  TrackControl(CreateBtn(hw, L"Set", IDC_MW_HOTKEYS_SET, bx, y, btnW, lineH, hFont));
  bx += btnW + btnGap;
  TrackControl(CreateBtn(hw, L"Clear", IDC_MW_HOTKEYS_CLEAR, bx, y, btnW, lineH, hFont));
  y += lineH + gap;

  // "Global (system-wide)" checkbox
  TrackControl(CreateCheck(hw, L"Global (system-wide)", IDC_MW_HOTKEYS_SCOPE,
    x, y, rw, lineH, hFont, false));
  y += lineH + gap;

  // "App path:" label + edit + Browse button (for Launch App rows, initially hidden)
  m_hPathLabel = CreateLabel(hw, L"App path:", x, y, MulDiv(70, lineH, 26), lineH, hFont, false);
  TrackControl(m_hPathLabel);
  int labelW = MulDiv(70, lineH, 26);
  int browseW = MulDiv(80, lineH, 26);
  int pathEditW = rw - labelW - browseW - 8;
  TrackControl(CreateEdit(hw, L"", IDC_MW_HOTKEYS_PATH,
    x + labelW, y, pathEditW, lineH, hFont, ES_AUTOHSCROLL, false));
  TrackControl(CreateBtn(hw, L"Browse...", IDC_MW_HOTKEYS_BROWSE,
    x + labelW + pathEditW + 8, y, browseW, lineH, hFont, false));
  y += lineH + gap + 8;

  // "Reset to Defaults" button
  int resetW = MulDiv(160, lineH, 26);
  TrackControl(CreateBtn(hw, L"Reset to Defaults", IDC_MW_HOTKEYS_RESET,
    x, y, resetW, lineH, hFont));

  // Update scope checkbox and path controls to match first selected item
  if (hList) {
    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (sel >= 0 && sel < NUM_HOTKEYS) {
      CheckDlgButton(hw, IDC_MW_HOTKEYS_SCOPE,
        m_pEngine->m_hotkeys[sel].scope == HKSCOPE_GLOBAL ? BST_CHECKED : BST_UNCHECKED);
      ShowPathControls(hw, sel);
    }
  }
}

//----------------------------------------------------------------------
// DoCommand — button clicks
//----------------------------------------------------------------------

LRESULT HotkeysWindow::DoCommand(HWND hWnd, int id, int code, LPARAM /*lParam*/)
{
  Engine* p = m_pEngine;
  HWND hList = GetDlgItem(hWnd, IDC_MW_HOTKEYS_LIST);

  if (id == IDC_MW_HOTKEYS_SET && code == BN_CLICKED) {
    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= NUM_HOTKEYS) {
      p->AddNotification(L"Select a hotkey action first");
      return 0;
    }
    HWND hHotkey = GetDlgItem(hWnd, IDC_MW_HOTKEYS_EDIT);
    if (!hHotkey) return 0;

    DWORD hk = (DWORD)SendMessage(hHotkey, HKM_GETHOTKEY, 0, 0);
    UINT vk = LOBYTE(LOWORD(hk));
    UINT hkMod = HIBYTE(LOWORD(hk));
    if (vk == 0) {
      p->AddNotification(L"Press a key combination first");
      return 0;
    }

    // Convert HOTKEYF_* to MOD_*
    UINT mod = 0;
    if (hkMod & HOTKEYF_ALT)     mod |= MOD_ALT;
    if (hkMod & HOTKEYF_CONTROL) mod |= MOD_CONTROL;
    if (hkMod & HOTKEYF_SHIFT)   mod |= MOD_SHIFT;

    // Conflict detection: clear any other binding with the same key+mod
    for (int i = 0; i < NUM_HOTKEYS; i++) {
      if (i != sel && p->m_hotkeys[i].vk == vk && p->m_hotkeys[i].modifiers == mod) {
        p->m_hotkeys[i].vk = 0;
        p->m_hotkeys[i].modifiers = 0;
      }
    }

    // Assign
    p->m_hotkeys[sel].modifiers = mod;
    p->m_hotkeys[sel].vk = vk;

    // Use current scope checkbox state
    bool bGlobal = (IsDlgButtonChecked(hWnd, IDC_MW_HOTKEYS_SCOPE) == BST_CHECKED);
    p->m_hotkeys[sel].scope = bGlobal ? HKSCOPE_GLOBAL : HKSCOPE_LOCAL;

    SaveAndReRegister(p);
    RefreshHotkeyList(hList, p);
    ListView_SetItemState(hList, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);

    std::wstring notif = L"Hotkey set: " + p->FormatHotkeyDisplay(mod, vk);
    p->AddNotification((wchar_t*)notif.c_str());
    return 0;
  }

  if (id == IDC_MW_HOTKEYS_CLEAR && code == BN_CLICKED) {
    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (sel >= 0 && sel < NUM_HOTKEYS) {
      p->m_hotkeys[sel].vk = 0;
      p->m_hotkeys[sel].modifiers = 0;
      SaveAndReRegister(p);
      RefreshHotkeyList(hList, p);
      ListView_SetItemState(hList, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);

      HWND hHotkey = GetDlgItem(hWnd, IDC_MW_HOTKEYS_EDIT);
      if (hHotkey) SendMessageW(hHotkey, HKM_SETHOTKEY, 0, 0);
    }
    return 0;
  }

  if (id == IDC_MW_HOTKEYS_SCOPE && code == BN_CLICKED) {
    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (sel >= 0 && sel < NUM_HOTKEYS && p->m_hotkeys[sel].vk != 0) {
      bool bGlobal = (IsDlgButtonChecked(hWnd, IDC_MW_HOTKEYS_SCOPE) == BST_CHECKED);
      p->m_hotkeys[sel].scope = bGlobal ? HKSCOPE_GLOBAL : HKSCOPE_LOCAL;
      SaveAndReRegister(p);
      RefreshHotkeyList(hList, p);
      ListView_SetItemState(hList, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    return 0;
  }

  if (id == IDC_MW_HOTKEYS_BROWSE && code == BN_CLICKED) {
    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (sel >= 0 && sel < NUM_HOTKEYS && IsLaunchAppAction(p->m_hotkeys[sel].id)) {
      int slot = LaunchAppSlot(p->m_hotkeys[sel].id);
      wchar_t szFile[MAX_PATH] = {};
      wcsncpy_s(szFile, p->m_szLaunchApp[slot], _TRUNCATE);
      OPENFILENAMEW ofn = {};
      ofn.lStructSize = sizeof(ofn);
      ofn.hwndOwner = hWnd;
      ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
      ofn.lpstrFile = szFile;
      ofn.nMaxFile = MAX_PATH;
      ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
      ofn.lpstrTitle = L"Select Application";
      if (GetOpenFileNameW(&ofn)) {
        wcsncpy_s(p->m_szLaunchApp[slot], szFile, _TRUNCATE);
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_HOTKEYS_PATH), szFile);
        SaveAndReRegister(p);
        RefreshHotkeyList(hList, p);
        ListView_SetItemState(hList, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      }
    }
    return 0;
  }

  if (id == IDC_MW_HOTKEYS_PATH && code == EN_KILLFOCUS) {
    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (sel >= 0 && sel < NUM_HOTKEYS && IsLaunchAppAction(p->m_hotkeys[sel].id)) {
      int slot = LaunchAppSlot(p->m_hotkeys[sel].id);
      wchar_t szPath[MAX_PATH] = {};
      GetWindowTextW(GetDlgItem(hWnd, IDC_MW_HOTKEYS_PATH), szPath, MAX_PATH);
      wcsncpy_s(p->m_szLaunchApp[slot], szPath, _TRUNCATE);
      SaveAndReRegister(p);
      RefreshHotkeyList(hList, p);
      ListView_SetItemState(hList, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    return 0;
  }

  if (id == IDC_MW_HOTKEYS_RESET && code == BN_CLICKED) {
    p->ResetHotkeyDefaults();
    SaveAndReRegister(p);
    RefreshHotkeyList(hList, p);
    ListView_SetItemState(hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    CheckDlgButton(hWnd, IDC_MW_HOTKEYS_SCOPE,
      p->m_hotkeys[0].scope == HKSCOPE_GLOBAL ? BST_CHECKED : BST_UNCHECKED);

    HWND hHotkey = GetDlgItem(hWnd, IDC_MW_HOTKEYS_EDIT);
    if (hHotkey) SendMessageW(hHotkey, HKM_SETHOTKEY, 0, 0);

    ShowPathControls(hWnd, 0);  // update path visibility for new selection
    p->AddNotification(L"Hotkeys reset to defaults");
    return 0;
  }

  return -1;
}

//----------------------------------------------------------------------
// DoNotify — ListView selection change
//----------------------------------------------------------------------

LRESULT HotkeysWindow::DoNotify(HWND hWnd, NMHDR* pnm)
{
  if (pnm->idFrom == IDC_MW_HOTKEYS_LIST && pnm->code == LVN_ITEMCHANGED) {
    NMLISTVIEW* pnmlv = (NMLISTVIEW*)pnm;
    if (pnmlv->uNewState & LVIS_SELECTED) {
      int sel = pnmlv->iItem;
      if (sel >= 0 && sel < NUM_HOTKEYS) {
        CheckDlgButton(hWnd, IDC_MW_HOTKEYS_SCOPE,
          m_pEngine->m_hotkeys[sel].scope == HKSCOPE_GLOBAL ? BST_CHECKED : BST_UNCHECKED);
        ShowPathControls(hWnd, sel);
      }
    }
    return 0;
  }
  return -1;
}

//----------------------------------------------------------------------
// ShowPathControls — show/hide path edit + browse for Launch App rows
//----------------------------------------------------------------------

void HotkeysWindow::ShowPathControls(HWND hWnd, int sel)
{
  bool show = (sel >= 0 && sel < NUM_HOTKEYS && IsLaunchAppAction(m_pEngine->m_hotkeys[sel].id));
  int cmd = show ? SW_SHOW : SW_HIDE;
  if (m_hPathLabel) ShowWindow(m_hPathLabel, cmd);
  HWND hPath = GetDlgItem(hWnd, IDC_MW_HOTKEYS_PATH);
  HWND hBrowse = GetDlgItem(hWnd, IDC_MW_HOTKEYS_BROWSE);
  if (hPath) ShowWindow(hPath, cmd);
  if (hBrowse) ShowWindow(hBrowse, cmd);

  // Populate path edit with current value
  if (show && hPath) {
    int slot = LaunchAppSlot(m_pEngine->m_hotkeys[sel].id);
    SetWindowTextW(hPath, m_pEngine->m_szLaunchApp[slot]);
  }
}

} // namespace mdrop

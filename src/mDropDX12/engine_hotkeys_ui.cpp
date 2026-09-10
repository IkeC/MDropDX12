/*
  HotkeysWindow — Keyboard Shortcuts window (ToolWindow subclass).
  Shows all configurable hotkey bindings in a ListView, with Add/Edit/Delete
  buttons for dynamic user entries (Script Commands and Launch Apps).
  Key assignment and editing happen in a modal dialog.
  Launched from "Hotkeys..." button on the Settings System tab, or Ctrl+F7.
*/

#include "tool_window.h"
#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include "format_to.h"
#include <commctrl.h>
#include <uxtheme.h>   // SetWindowTheme for the dark-mode list

namespace mdrop {

extern Engine g_engine;
extern int g_nHelpLineCount;

//----------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------

HotkeysWindow::HotkeysWindow(Engine* pEngine)
  : ToolWindow(pEngine, 680, 580) {}

//----------------------------------------------------------------------
// Common control flags
//----------------------------------------------------------------------

DWORD HotkeysWindow::GetCommonControlFlags() const {
  return ICC_HOTKEY_CLASS | ICC_LISTVIEW_CLASSES;
}

//----------------------------------------------------------------------
// lParam encoding: bit 31 distinguishes built-in from user entries
//   built-in: lParam = index into m_hotkeys[]   (bit 31 = 0)
//   user:     lParam = 0x80000000 | index into m_userHotkeys[]
//----------------------------------------------------------------------

static constexpr LPARAM USER_LPARAM_BIT = 0x80000000;

static bool IsUserLParam(LPARAM lp) { return (lp & USER_LPARAM_BIT) != 0; }
static int  UserIndex(LPARAM lp)    { return (int)(lp & 0x7FFFFFFF); }

//----------------------------------------------------------------------
// AvailableKeysDialog — which global combinations are actually free
//
// A child of the Hotkeys window, on the same terms as the edit dialog: it
// takes the window as its parent, so it opens where that window is rather
// than on whatever Windows considers the main display, and closes with it.
//
// Windows offers no way to ask what other applications hold, so the list is
// built by TRYING each combination -- see Engine::ScanGlobalHotkeyAvailability.
// Two consequences the user can see, and one they cannot:
//
//   * a combination reading "free" was genuinely claimable a moment ago, which
//     is a stronger statement than any lookup table could make;
//   * "in use" cannot name the application holding it, because Windows does
//     not say and there is no way to ask;
//   * every free combination was held for an instant during the scan. A key
//     pressed in exactly that instant would be swallowed. Unavoidable: the
//     probe IS the measurement.
//----------------------------------------------------------------------

namespace {

enum { AK_COL_COMBO = 0, AK_COL_STATUS, AK_COL_ACTION };

class AvailableKeysDialog : public ModalDialog {
public:
  // targetName is empty when the Hotkeys list had no selection: the bind
  // button then has nothing to name and stays disabled.
  AvailableKeysDialog(Engine* pEngine, int targetBuiltIn, int targetUserIdx,
                      std::wstring targetName)
    : ModalDialog(pEngine), m_targetBuiltIn(targetBuiltIn),
      m_targetUserIdx(targetUserIdx), m_targetName(std::move(targetName)) {}

  // Set when a binding was applied, so the caller knows to refresh its list.
  bool m_bBound = false;

protected:
  const wchar_t* GetDialogTitle() const override { return L"Available Global Hotkeys"; }
  const wchar_t* GetDialogClass() const override { return L"MDropAvailKeysDlg"; }

  void DoBuildControls(int clientW, int clientH) override {
    HWND hDlg = GetHWND();
    HFONT hFont = GetFont();
    auto L = GetBaseLayout();
    int x = L.margin, y = L.margin;
    int cw = clientW - L.margin * 2;

    m_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
      LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
      x, y, cw, L.lineH * 14, hDlg,
      (HMENU)(INT_PTR)IDC_AK_LIST, GetModuleHandle(NULL), NULL);
    if (m_hList) {
      ListView_SetExtendedListViewStyle(m_hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
      SendMessageW(m_hList, WM_SETFONT, (WPARAM)hFont, TRUE);
      if (m_pEngine->IsDarkTheme()) {
        SetWindowTheme(m_hList, L"DarkMode_Explorer", NULL);
        ListView_SetBkColor(m_hList, m_pEngine->m_colSettingsCtrlBg);
        ListView_SetTextBkColor(m_hList, m_pEngine->m_colSettingsCtrlBg);
        ListView_SetTextColor(m_hList, m_pEngine->m_colSettingsText);
      }
      // Combination gets the most room: "CTRL+ALT+SHIFT+F12" is the longest
      // thing in the table and it is also the only column whose truncation
      // makes the row useless -- "CTRL+ALT+SHI..." names no key at all.
      // Action takes the REMAINDER of the two computed widths rather than a
      // third percentage, so rounding cannot push the total past the list and
      // raise a horizontal scrollbar.
      const int scrollW  = GetSystemMetrics(SM_CXVSCROLL) + 4;
      const int comboW   = MulDiv(cw, 42, 100);
      const int statusW  = MulDiv(cw, 26, 100);
      LVCOLUMNW col = {};
      col.mask = LVCF_TEXT | LVCF_WIDTH;
      col.pszText = (LPWSTR)L"Combination"; col.cx = comboW;
      SendMessageW(m_hList, LVM_INSERTCOLUMNW, AK_COL_COMBO, (LPARAM)&col);
      col.pszText = (LPWSTR)L"Status";      col.cx = statusW;
      SendMessageW(m_hList, LVM_INSERTCOLUMNW, AK_COL_STATUS, (LPARAM)&col);
      col.pszText = (LPWSTR)L"Action";      col.cx = cw - comboW - statusW - scrollW;
      SendMessageW(m_hList, LVM_INSERTCOLUMNW, AK_COL_ACTION, (LPARAM)&col);

      TrackControl(m_hList);
    }
    y += L.lineH * 14 + L.gap;

    TrackControl(CreateCheck(hDlg, L"Free only", IDC_AK_FREEONLY,
                             x, y, MulDiv(cw, 24, 100), L.lineH, hFont, m_bFreeOnly));
    // Two ROWS, not four controls crammed into one.
    //
    // "Use for <action>" carries a preset name inside it, so its width is not
    // knowable at layout time -- squeezing it into the leftovers of a shared
    // row clipped it at both ends however the other three were sized. The
    // filter and Rescan take the first row; the two buttons that CHANGE
    // something get the second, split evenly, which also separates reading the
    // list from acting on it.
    const int bGap = MulDiv(8, L.lineH, 26);
    TrackControl(CreateBtn(hDlg, L"Rescan", IDC_AK_RESCAN,
                           x + cw - MulDiv(90, L.lineH, 26), y,
                           MulDiv(90, L.lineH, 26), L.lineH, hFont));
    y += L.lineH + L.gap;

    // Fixed meanings rather than one button that changes what it does with the
    // selection: a control has to look like what it does, and a button that is
    // "Use" on one row and "Clear" on the next is one you read twice before
    // trusting it.
    const int halfW = (cw - bGap) / 2;
    m_hClear = CreateBtn(hDlg, L"Clear binding", IDC_AK_CLEAR,
                         x, y, halfW, L.lineH, hFont);
    TrackControl(m_hClear);

    std::wstring useLabel = m_targetName.empty()
        ? std::wstring(L"Use for (nothing selected)")
        : (L"Use for \"" + m_targetName + L"\"");
    m_hUse = CreateBtn(hDlg, useLabel.c_str(), IDC_AK_USE,
                       x + halfW + bGap, y, halfW, L.lineH, hFont);
    TrackControl(m_hUse);
    y += L.lineH + L.gap;

    // The caveat, stated rather than discovered. The shell reserves many
    // Win+key combinations and still swallows them after RegisterHotKey has
    // said yes, so "free" is honest about what Windows granted and cannot
    // promise the key will arrive.
    // Three lines, because at this width the sentence wraps to three. Sized
    // for two, the last line is simply cut off -- which is how the first
    // version of this shipped a caveat that stopped mid-word.
    TrackControl(CreateLabel(hDlg,
      L"\"Free\" means Windows granted it just now. Windows reserves many "
      L"Win+key combinations and may still swallow them.",
      x, y, cw, L.lineH * 3, hFont));
    y += L.lineH * 3 + L.gap;

    TrackControl(CreateBtn(hDlg, L"Close", IDCANCEL,
                           x + (cw - 90) / 2, y, 90, L.lineH + 4, hFont));
    y += L.lineH + 4 + L.margin;

    FitToContent(clientW, y);
    Rescan();
  }

  LRESULT DoCommand(int id, int code, LPARAM lParam) override {
    if (id == IDC_AK_RESCAN && code == BN_CLICKED) { Rescan(); return 0; }
    if (id == IDC_AK_FREEONLY) { m_bFreeOnly = IsChecked(IDC_AK_FREEONLY); Fill(); return 0; }
    if (id == IDC_AK_USE && code == BN_CLICKED) { Bind(); return 0; }
    if (id == IDC_AK_CLEAR && code == BN_CLICKED) { ClearBinding(); return 0; }
    if (id == IDCANCEL && code == BN_CLICKED) { EndDialog(m_bBound); return 0; }
    return -1;
  }

  LRESULT DoNotify(NMHDR* pnm) override {
    if (pnm->idFrom == IDC_AK_LIST &&
        (pnm->code == LVN_ITEMCHANGED || pnm->code == NM_CLICK))
      UpdateUseButton();
    if (pnm->idFrom == IDC_AK_LIST && pnm->code == NM_DBLCLK) { Bind(); return 0; }
    return -1;
  }

private:
  HWND m_hList = NULL;
  HWND m_hUse   = NULL;
  HWND m_hClear = NULL;
  bool m_bFreeOnly = true;   // the useful default: the question is "what can I take?"
  int  m_targetBuiltIn;
  int  m_targetUserIdx;
  std::wstring m_targetName;
  std::vector<HotkeyScanRow> m_rows;

  void Rescan() {
    // Safe from this thread: the probes pass hWnd = NULL, so they belong to
    // whichever thread asks rather than to a window somebody else owns.
    m_rows = m_pEngine->ScanGlobalHotkeyAvailability();
    Fill();
  }

  void Fill() {
    if (!m_hList) return;
    SendMessageW(m_hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(m_hList);
    int row = 0;
    // Ours first, then free, then taken -- the two answers a person came here
    // for are "what have I got" and "what can I add"; the rest is background.
    for (int pass = 0; pass < 3; pass++) {
      const int want = (pass == 0) ? HKAVAIL_OURS
                     : (pass == 1) ? HKAVAIL_FREE : HKAVAIL_TAKEN;
      if (m_bFreeOnly && want == HKAVAIL_TAKEN) continue;
      for (size_t i = 0; i < m_rows.size(); i++) {
        const HotkeyScanRow& r = m_rows[i];
        if (r.status != want) continue;
        const std::wstring combo = m_pEngine->FormatHotkeyDisplay(r.modifiers, r.vk);
        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = row;
        lvi.iSubItem = AK_COL_COMBO;
        lvi.lParam = (LPARAM)i;      // index into m_rows, not the display order
        lvi.pszText = (LPWSTR)combo.c_str();
        int idx = (int)SendMessageW(m_hList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);

        const wchar_t* status = (r.status == HKAVAIL_FREE) ? L"free"
                              : (r.status == HKAVAIL_OURS) ? L"yours"
                                                           : L"in use by another app";
        lvi.mask = LVIF_TEXT;
        lvi.iItem = idx;
        lvi.iSubItem = AK_COL_STATUS;
        lvi.pszText = (LPWSTR)status;
        SendMessageW(m_hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

        std::wstring action = r.ownerId >= 0 ? m_pEngine->HotkeyOwnerName(r.ownerId)
                                             : std::wstring();
        // A binding of ours that Windows did not actually grant is the one
        // case where "yours" alone would mislead.
        if (r.status == HKAVAIL_OURS && r.ownerId >= 0 &&
            m_pEngine->HasGlobalHotkeyFailure(r.ownerId))
          action += L"  (NOT held)";
        lvi.iSubItem = AK_COL_ACTION;
        lvi.pszText = (LPWSTR)action.c_str();
        SendMessageW(m_hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);
        row++;
      }
    }
    SendMessageW(m_hList, WM_SETREDRAW, TRUE, 0);

    // Size the last column HERE, not at creation. GetClientRect on an empty
    // list reports the full width because no vertical scrollbar exists yet, so
    // a width computed then is too wide the moment rows arrive -- and the list
    // grows a horizontal scrollbar across content that visibly fits.
    RECT rcList;
    GetClientRect(m_hList, &rcList);
    const int combo  = (int)SendMessageW(m_hList, LVM_GETCOLUMNWIDTH, AK_COL_COMBO, 0);
    const int status = (int)SendMessageW(m_hList, LVM_GETCOLUMNWIDTH, AK_COL_STATUS, 0);
    const int fill   = (rcList.right - rcList.left) - combo - status;
    if (fill > 40)
      SendMessageW(m_hList, LVM_SETCOLUMNWIDTH, AK_COL_ACTION, fill);

    InvalidateRect(m_hList, NULL, TRUE);
    UpdateUseButton();
  }

  const HotkeyScanRow* Selected() const {
    if (!m_hList) return nullptr;
    int sel = ListView_GetNextItem(m_hList, -1, LVNI_SELECTED);
    if (sel < 0) return nullptr;
    LVITEMW lvi = {};
    lvi.mask = LVIF_PARAM;
    lvi.iItem = sel;
    if (!SendMessageW(m_hList, LVM_GETITEMW, 0, (LPARAM)&lvi)) return nullptr;
    size_t i = (size_t)lvi.lParam;
    return (i < m_rows.size()) ? &m_rows[i] : nullptr;
  }

  void UpdateUseButton() {
    const HotkeyScanRow* r = Selected();
    if (m_hUse) {
      // Only a FREE combination, and only with something to bind it to.
      // Binding a taken one would fail, and binding one of ours to itself is
      // a no-op.
      EnableWindow(m_hUse, r && r->status == HKAVAIL_FREE && !m_targetName.empty());
    }
    if (m_hClear) {
      // Only one of OURS can be cleared. A combination another application
      // holds is not ours to give back, and a free one has nothing to clear.
      EnableWindow(m_hClear, r && r->status == HKAVAIL_OURS && r->ownerId >= 0);
    }
  }

  // Unbind whatever holds the selected combination, freeing it up.
  //
  // It acts on the row's OWNER, not on the dialog's bind target: the question
  // this answers is "what is sitting on the key I want", so clearing anything
  // other than the thing named in the row would be a surprise.
  //
  // No confirmation. Shane's standing rule is that a control the user pressed
  // deliberately is itself the confirmation, and this one is trivially undone
  // -- the row turns from "yours" to "free" in front of you, which is better
  // feedback than a dialog asking whether you meant it.
  void ClearBinding() {
    const HotkeyScanRow* r = Selected();
    if (!r || r->status != HKAVAIL_OURS || r->ownerId < 0) return;

    Engine* p = m_pEngine;
    bool changed = false;
    for (int i = 0; i < NUM_HOTKEYS; i++)
      if (p->m_hotkeys[i].id == r->ownerId) {
        p->m_hotkeys[i].globalMod = 0;
        p->m_hotkeys[i].globalVK  = 0;
        changed = true;
        break;
      }
    if (!changed)
      for (auto& uh : p->m_userHotkeys)
        if (uh.id == r->ownerId) {
          // A user hotkey has ONE binding, so clearing its global key clears
          // the entry's key outright rather than leaving a local one behind.
          uh.modifiers = 0;
          uh.vk = 0;
          changed = true;
          break;
        }
    if (!changed) return;

    ApplyAndReRegister();
    m_bBound = true;      // the parent list is now stale either way
    Rescan();
  }

  void ApplyAndReRegister() {
    Engine* p = m_pEngine;
    p->SaveHotkeySettings();
    p->GenerateHelpText();
    g_nHelpLineCount = p->m_nHelpLineCount;
    p->InvalidateHelpTexture();
    if (HWND hRender = p->GetPluginWindow())
      PostMessage(hRender, WM_MW_REGISTER_HOTKEYS, 0, 0);
  }

  void Bind() {
    const HotkeyScanRow* r = Selected();
    if (!r || r->status != HKAVAIL_FREE || m_targetName.empty()) return;

    Engine* p = m_pEngine;
    const int selfBuiltIn = (m_targetBuiltIn >= 0) ? p->m_hotkeys[m_targetBuiltIn].id : -1;
    const int selfUser = (m_targetUserIdx >= 0 &&
                          m_targetUserIdx < (int)p->m_userHotkeys.size())
                       ? p->m_userHotkeys[m_targetUserIdx].id : -1;

    // The SAME confirm the edit dialog raises. Binding from here is a second
    // route to the same change, not a second set of rules -- a free
    // combination cannot clash with one of ours, but the check costs nothing
    // and means there is no path that quietly unbinds something.
    int count = 0;
    std::wstring warn = p->DescribeHotkeyConflicts(selfBuiltIn, selfUser,
                            r->modifiers, r->vk, HKSCOPE_GLOBAL, &count);
    if (!warn.empty()) {
      warn += L"\n\nUse it here and unbind it there?";
      if (MessageBoxW(GetHWND(), warn.c_str(), L"Reassign hotkey?",
                      MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
      for (int i = 0; i < NUM_HOTKEYS; i++)
        if (p->m_hotkeys[i].id != selfBuiltIn &&
            p->m_hotkeys[i].globalVK == r->vk &&
            p->m_hotkeys[i].globalMod == r->modifiers) {
          p->m_hotkeys[i].globalVK = 0;
          p->m_hotkeys[i].globalMod = 0;
        }
      for (auto& uh : p->m_userHotkeys)
        if (uh.id != selfUser && uh.vk == r->vk &&
            uh.modifiers == r->modifiers && uh.scope == HKSCOPE_GLOBAL) {
          uh.vk = 0;
          uh.modifiers = 0;
        }
    }

    if (m_targetBuiltIn >= 0) {
      p->m_hotkeys[m_targetBuiltIn].globalMod = r->modifiers;
      p->m_hotkeys[m_targetBuiltIn].globalVK  = r->vk;
    } else if (selfUser >= 0) {
      // A user hotkey carries ONE binding plus a scope, not a local/global
      // pair, so assigning a global key here also makes it global.
      p->m_userHotkeys[m_targetUserIdx].modifiers = r->modifiers;
      p->m_userHotkeys[m_targetUserIdx].vk        = r->vk;
      p->m_userHotkeys[m_targetUserIdx].scope     = HKSCOPE_GLOBAL;
    } else {
      return;
    }

    ApplyAndReRegister();

    m_bBound = true;
    // Rescan rather than patch the row: the binding we just made is now one
    // of ours, and anything else that changed underneath is picked up too.
    Rescan();
  }
};

} // namespace

//----------------------------------------------------------------------
// Helper: refresh ListView contents from m_hotkeys[] + m_userHotkeys
//----------------------------------------------------------------------

// Column indices for the ListView
enum { COL_CATEGORY = 0, COL_ACTION, COL_LOCAL_KEY, COL_GLOBAL_KEY };

// Sort state
static int  s_sortColumn = COL_CATEGORY;  // default: sort by category
static bool s_sortAscending = true;

// What to append to a global binding's cell so the row shows whether the key
// is actually ours.
//
// Only ever a suffix on a combination that IS configured -- an action with no
// global binding has nothing to hold and must not be marked. The distinction
// matters: "not held" and "not asked for" look identical in a table that only
// prints the request, and only the first is a problem.
//
// ERROR_HOTKEY_ALREADY_REGISTERED gets its own words, because "in use" is
// something a user can act on and a bare code is not. Windows never says WHICH
// application holds it and there is no way to ask -- saying "not ours" is the
// whole job.
static std::wstring HotkeyHeldSuffix(Engine* p, int id)
{
  DWORD err = 0;
  if (!p->HasGlobalHotkeyFailure(id, &err)) return L"";
  if (err == ERROR_HOTKEY_ALREADY_REGISTERED) return L"  (in use)";
  wchar_t buf[64];
  FormatTo(buf, L"  (failed: %lu)", (unsigned long)err);
  return buf;
}

static void RefreshHotkeyList(HWND hList, Engine* p)
{
  if (!hList) return;
  ListView_DeleteAllItems(hList);

  int row = 0;

  // Built-in hotkeys
  for (int i = 0; i < NUM_HOTKEYS; i++) {
    LVITEMW lvi = {};
    lvi.mask = LVIF_TEXT | LVIF_PARAM;
    lvi.iItem = row;
    lvi.iSubItem = 0;
    lvi.lParam = (LPARAM)i;  // bit 31 = 0 → built-in
    const wchar_t* catName = (p->m_hotkeys[i].category < HKCAT_COUNT)
        ? kCategoryNames[p->m_hotkeys[i].category] : L"?";
    lvi.pszText = (LPWSTR)catName;
    int idx = (int)SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);

    // Action column
    lvi.mask = LVIF_TEXT;
    lvi.iItem = idx;
    lvi.iSubItem = COL_ACTION;
    lvi.pszText = (LPWSTR)p->m_hotkeys[i].szAction;
    SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

    // Local Key column
    std::wstring localKey = p->FormatHotkeyDisplay(p->m_hotkeys[i].modifiers, p->m_hotkeys[i].vk);
    lvi.iSubItem = COL_LOCAL_KEY;
    lvi.pszText = (LPWSTR)localKey.c_str();
    SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

    // Global Key column, marked with what is actually HELD.
    //
    // The column used to show only what was configured, which is the request
    // and not the answer -- RegisterHotKey is first-come-first-served across
    // the machine, so a combination another application owns looked exactly
    // like one that works (forgejo#72). Live state is the interesting fact.
    std::wstring globalKey = p->FormatHotkeyDisplay(p->m_hotkeys[i].globalMod, p->m_hotkeys[i].globalVK);
    if (p->m_hotkeys[i].globalVK != 0)
      globalKey += HotkeyHeldSuffix(p, p->m_hotkeys[i].id);
    lvi.iSubItem = COL_GLOBAL_KEY;
    lvi.pszText = (LPWSTR)globalKey.c_str();
    SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);
    row++;
  }

  // User hotkeys (Script Commands + Launch Apps)
  for (int i = 0; i < (int)p->m_userHotkeys.size(); i++) {
    const auto& uh = p->m_userHotkeys[i];

    const wchar_t* catName = (uh.type == USER_HK_SCRIPT)
        ? kCategoryNames[HKCAT_SCRIPT] : kCategoryNames[HKCAT_LAUNCH];

    LVITEMW lvi = {};
    lvi.mask = LVIF_TEXT | LVIF_PARAM;
    lvi.iItem = row;
    lvi.iSubItem = 0;
    lvi.lParam = (LPARAM)(USER_LPARAM_BIT | i);
    lvi.pszText = (LPWSTR)catName;
    int idx = (int)SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);

    // Action: label + command preview in parens
    std::wstring actionName = uh.label;
    if (!uh.command.empty()) {
      if (uh.type == USER_HK_SCRIPT) {
        actionName += L" (" + uh.command + L")";
      } else {
        const wchar_t* exeName = wcsrchr(uh.command.c_str(), L'\\');
        if (!exeName) exeName = wcsrchr(uh.command.c_str(), L'/');
        exeName = exeName ? exeName + 1 : uh.command.c_str();
        actionName += L" (";
        actionName += exeName;
        actionName += L")";
      }
    }
    lvi.mask = LVIF_TEXT;
    lvi.iItem = idx;
    lvi.iSubItem = COL_ACTION;
    lvi.pszText = (LPWSTR)actionName.c_str();
    SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

    // User hotkeys use single binding — show in local or global column based on scope
    std::wstring shortcut = p->FormatHotkeyDisplay(uh.modifiers, uh.vk);
    if (uh.scope == HKSCOPE_LOCAL || uh.vk == 0) {
      lvi.iSubItem = COL_LOCAL_KEY;
      lvi.pszText = (LPWSTR)shortcut.c_str();
      SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);
      lvi.iSubItem = COL_GLOBAL_KEY;
      lvi.pszText = (LPWSTR)L"(none)";
      SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);
    } else {
      lvi.iSubItem = COL_LOCAL_KEY;
      lvi.pszText = (LPWSTR)L"(none)";
      SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);
      std::wstring globalShortcut = shortcut + HotkeyHeldSuffix(p, uh.id);
      lvi.iSubItem = COL_GLOBAL_KEY;
      lvi.pszText = (LPWSTR)globalShortcut.c_str();
      SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);
    }
    row++;
  }
}

//----------------------------------------------------------------------
// Sort comparison callback
//----------------------------------------------------------------------

struct SortContext {
  HWND hList;
  Engine* pEngine;
};

// Helpers to extract sort fields from either built-in or user entry
struct HKSortData {
  int category;
  const wchar_t* action;
  UINT vk, mod;
  UINT globalVK, globalMod;
};

static HKSortData GetSortData(LPARAM lp, Engine* p)
{
  HKSortData d = {};
  if (IsUserLParam(lp)) {
    int idx = UserIndex(lp);
    if (idx >= 0 && idx < (int)p->m_userHotkeys.size()) {
      const auto& uh = p->m_userHotkeys[idx];
      d.category = (uh.type == USER_HK_SCRIPT) ? HKCAT_SCRIPT : HKCAT_LAUNCH;
      d.action = uh.label.c_str();
      if (uh.scope == HKSCOPE_GLOBAL) {
        d.globalVK = uh.vk; d.globalMod = uh.modifiers;
      } else {
        d.vk = uh.vk; d.mod = uh.modifiers;
      }
    }
  } else {
    int idx = (int)lp;
    if (idx >= 0 && idx < NUM_HOTKEYS) {
      d.category = (int)p->m_hotkeys[idx].category;
      d.action = p->m_hotkeys[idx].szAction;
      d.vk = p->m_hotkeys[idx].vk;
      d.mod = p->m_hotkeys[idx].modifiers;
      d.globalVK = p->m_hotkeys[idx].globalVK;
      d.globalMod = p->m_hotkeys[idx].globalMod;
    }
  }
  return d;
}

static int CALLBACK HotkeyListCompare(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort)
{
  SortContext* ctx = (SortContext*)lParamSort;
  HKSortData a = GetSortData(lParam1, ctx->pEngine);
  HKSortData b = GetSortData(lParam2, ctx->pEngine);

  int cmp = 0;
  switch (s_sortColumn) {
  case COL_CATEGORY:
    cmp = a.category - b.category;
    if (cmp == 0) cmp = _wcsicmp(a.action, b.action);
    break;
  case COL_ACTION:
    cmp = _wcsicmp(a.action, b.action);
    break;
  case COL_LOCAL_KEY:
    cmp = (int)a.vk - (int)b.vk;
    if (cmp == 0) cmp = (int)a.mod - (int)b.mod;
    break;
  case COL_GLOBAL_KEY:
    cmp = (int)a.globalVK - (int)b.globalVK;
    if (cmp == 0) cmp = (int)a.globalMod - (int)b.globalMod;
    break;
  }
  return s_sortAscending ? cmp : -cmp;
}

//----------------------------------------------------------------------
// Helper: save bindings and re-register global hotkeys
//----------------------------------------------------------------------

static void SaveAndReRegister(Engine* p)
{
  p->SaveHotkeySettings();
  p->GenerateHelpText();
  g_nHelpLineCount = p->m_nHelpLineCount;
  p->InvalidateHelpTexture();
  HWND hRender = p->GetPluginWindow();
  if (hRender)
    PostMessage(hRender, WM_MW_REGISTER_HOTKEYS, 0, 0);
}

//----------------------------------------------------------------------
// Helper: get lParam from ListView item
//----------------------------------------------------------------------

static LPARAM GetItemLParam(HWND hList, int iItem)
{
  LVITEMW lvi = {};
  lvi.mask = LVIF_PARAM;
  lvi.iItem = iItem;
  if (SendMessageW(hList, LVM_GETITEMW, 0, (LPARAM)&lvi))
    return lvi.lParam;
  return -1;
}

static LPARAM GetSelectedLParam(HWND hList)
{
  int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
  if (sel < 0) return -1;
  return GetItemLParam(hList, sel);
}

//----------------------------------------------------------------------
// Edit Hotkey — uses shared ShowActionEditDialog
//----------------------------------------------------------------------

void HotkeysWindow::OpenEditDialog(int lvItem)
{
  HWND hList = GetDlgItem(m_hWnd, IDC_MW_HOTKEYS_LIST);
  if (!hList || lvItem < 0) return;
  LPARAM lp = GetItemLParam(hList, lvItem);
  if (lp == (LPARAM)-1) return;

  Engine* p = m_pEngine;

  // Populate ActionEditData from the hotkey entry
  ActionEditData data;
  data.pEngine = p;
  data.showKeyBinding = true;

  bool isBuiltIn = !IsUserLParam(lp);
  int  entryIndex = isBuiltIn ? (int)lp : UserIndex(lp);

  if (isBuiltIn) {
    if (entryIndex < 0 || entryIndex >= NUM_HOTKEYS) return;
    data.isBuiltInHotkey = true;
    data.selfHotkeyId = p->m_hotkeys[entryIndex].id;
    data.actionName = p->m_hotkeys[entryIndex].szAction;
    data.modifiers  = p->m_hotkeys[entryIndex].modifiers;
    data.vk         = p->m_hotkeys[entryIndex].vk;
    data.globalMod  = p->m_hotkeys[entryIndex].globalMod;
    data.globalVK   = p->m_hotkeys[entryIndex].globalVK;
  } else {
    if (entryIndex < 0 || entryIndex >= (int)p->m_userHotkeys.size()) return;
    const auto& uh = p->m_userHotkeys[entryIndex];
    data.isBuiltInHotkey = false;
    data.selfUserId = uh.id;
    data.actionType = (uh.type == USER_HK_LAUNCH) ? ButtonAction::LaunchApp : ButtonAction::ScriptCommand;
    data.label      = uh.label;
    data.payload    = uh.command;
    data.modifiers  = uh.modifiers;
    data.vk         = uh.vk;
    data.scope      = uh.scope;
  }

  if (!ShowActionEditDialog(m_hWnd, data)) return;

  // Assigning a key another action already has UNBINDS that action. That is
  // very likely the right default -- it is what the user is asking for -- but
  // it used to happen in silence, so the older shortcut simply stopped working
  // with no message at the time and no record afterwards (forgejo#72).
  //
  // The sentence comes from Engine::DescribeHotkeyConflicts, which is also
  // what DIAG_HOTKEY_CONFLICTS answers with, so what the dialog says and what
  // the test asserts cannot drift apart.
  {
    const int selfBuiltIn = isBuiltIn ? p->m_hotkeys[entryIndex].id : -1;
    const int selfUser    = (!isBuiltIn && entryIndex >= 0 &&
                             entryIndex < (int)p->m_userHotkeys.size())
                          ? p->m_userHotkeys[entryIndex].id : -1;
    std::wstring warn;
    if (isBuiltIn) {
      // Both bindings are edited on one trip through the dialog, so both are
      // checked before anything is written -- answering No must leave the
      // local binding alone as well, not half-apply the edit.
      const std::wstring l = p->DescribeHotkeyConflicts(selfBuiltIn, selfUser,
                                 data.modifiers, data.vk, HKSCOPE_LOCAL);
      const std::wstring g = p->DescribeHotkeyConflicts(selfBuiltIn, selfUser,
                                 data.globalMod, data.globalVK, HKSCOPE_GLOBAL);
      warn = l;
      if (!g.empty()) warn = warn.empty() ? g : (l + L"\n\n" + g);
    } else {
      warn = p->DescribeHotkeyConflicts(selfBuiltIn, selfUser,
                 data.modifiers, data.vk, data.scope);
    }
    if (!warn.empty()) {
      // The question belongs here, not in the shared sentence -- the inline
      // label in the edit dialog shows the same statement and must not ask
      // anything. Defaulting to No: the destructive answer should never be
      // the one a stray Enter picks.
      warn += L"\n\nUse it here and unbind it there?";
      if (MessageBoxW(m_hWnd, warn.c_str(), L"Reassign hotkey?",
                      MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    }
  }

  // Apply changes back
  if (isBuiltIn) {
    // Conflict detection for local binding
    if (data.vk != 0) {
      for (int i = 0; i < NUM_HOTKEYS; i++) {
        if (i != entryIndex && p->m_hotkeys[i].vk == data.vk && p->m_hotkeys[i].modifiers == data.modifiers) {
          p->m_hotkeys[i].vk = 0;
          p->m_hotkeys[i].modifiers = 0;
        }
      }
      for (auto& uh : p->m_userHotkeys) {
        if (uh.vk == data.vk && uh.modifiers == data.modifiers && uh.scope == HKSCOPE_LOCAL) {
          uh.vk = 0;
          uh.modifiers = 0;
        }
      }
    }
    // Conflict detection for global binding
    if (data.globalVK != 0) {
      for (int i = 0; i < NUM_HOTKEYS; i++) {
        if (i != entryIndex && p->m_hotkeys[i].globalVK == data.globalVK && p->m_hotkeys[i].globalMod == data.globalMod) {
          p->m_hotkeys[i].globalVK = 0;
          p->m_hotkeys[i].globalMod = 0;
        }
      }
      for (auto& uh : p->m_userHotkeys) {
        if (uh.vk == data.globalVK && uh.modifiers == data.globalMod && uh.scope == HKSCOPE_GLOBAL) {
          uh.vk = 0;
          uh.modifiers = 0;
        }
      }
    }
    p->m_hotkeys[entryIndex].modifiers = data.modifiers;
    p->m_hotkeys[entryIndex].vk = data.vk;
    p->m_hotkeys[entryIndex].globalMod = data.globalMod;
    p->m_hotkeys[entryIndex].globalVK = data.globalVK;
  } else {
    if (entryIndex >= 0 && entryIndex < (int)p->m_userHotkeys.size()) {
      // Conflict detection
      if (data.vk != 0) {
        for (int i = 0; i < NUM_HOTKEYS; i++) {
          if (p->m_hotkeys[i].vk == data.vk && p->m_hotkeys[i].modifiers == data.modifiers) {
            p->m_hotkeys[i].vk = 0;
            p->m_hotkeys[i].modifiers = 0;
          }
        }
        for (int i = 0; i < (int)p->m_userHotkeys.size(); i++) {
          if (i != entryIndex && p->m_userHotkeys[i].vk == data.vk && p->m_userHotkeys[i].modifiers == data.modifiers) {
            p->m_userHotkeys[i].vk = 0;
            p->m_userHotkeys[i].modifiers = 0;
          }
        }
      }
      auto& uh = p->m_userHotkeys[entryIndex];
      uh.modifiers = data.modifiers;
      uh.vk        = data.vk;
      uh.scope     = data.scope;
      uh.label     = data.label;
      uh.command   = data.payload;
      // Map action type back to UserHotkeyType
      uh.type = (data.actionType == ButtonAction::LaunchApp) ? USER_HK_LAUNCH : USER_HK_SCRIPT;
    }
  }

  SaveAndReRegister(p);
  RefreshHotkeyList(hList, p);
  // Re-select the edited item
  for (int i = 0; i < ListView_GetItemCount(hList); i++) {
    if (GetItemLParam(hList, i) == lp) {
      ListView_SetItemState(hList, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      ListView_EnsureVisible(hList, i, FALSE);
      break;
    }
  }
}

//----------------------------------------------------------------------
// Update Delete button enable state
//----------------------------------------------------------------------

void HotkeysWindow::UpdateDeleteButton()
{
  if (!m_hList) return;
  LPARAM lp = GetSelectedLParam(m_hList);
  if (m_hBtnDelete)
    EnableWindow(m_hBtnDelete, (lp != (LPARAM)-1 && IsUserLParam(lp)) ? TRUE : FALSE);

  // Clear Key enabled when any entry is selected and has a key bound
  if (m_hBtnClearKey) {
    bool hasKey = false;
    if (lp != (LPARAM)-1) {
      if (IsUserLParam(lp)) {
        int idx = UserIndex(lp);
        if (idx >= 0 && idx < (int)m_pEngine->m_userHotkeys.size())
          hasKey = m_pEngine->m_userHotkeys[idx].vk != 0;
      } else {
        int idx = (int)lp;
        if (idx >= 0 && idx < NUM_HOTKEYS)
          hasKey = m_pEngine->m_hotkeys[idx].vk != 0 || m_pEngine->m_hotkeys[idx].globalVK != 0;
      }
    }
    EnableWindow(m_hBtnClearKey, hasKey ? TRUE : FALSE);
  }
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

  RECT rc;
  GetClientRect(hw, &rc);

  const wchar_t* tabNames[] = { L"Key Bindings", L"Help Display" };
  RECT rcTab = BuildTabControl(IDC_MW_HOTKEYS_TAB, tabNames, HOTKEYS_NUM_PAGES,
                                0, y, rc.right, rc.bottom - y);

  int tabX = rcTab.left + x;
  int tabY = rcTab.top + 4;
  int tabRW = rcTab.right - rcTab.left - x * 2;

  BuildBindingsPage(tabX, tabY, tabRW, lineH, gap);
  BuildHelpOrderPage(tabX, tabY, tabRW, lineH, gap);

  SelectInitialTab();
  UpdateDeleteButton();
}

void HotkeysWindow::BuildBindingsPage(int x, int y, int rw, int lineH, int gap)
{
  HWND hw = m_hWnd;
  HFONT hFont = GetFont();

  #define PAGE_TC(page, expr) TrackPageControl(page, (expr))

  m_headerH = y;  // save for LayoutControls

  // Button bar height (at bottom)
  m_buttonBarH = lineH + gap + 8;

  // ListView fills remaining vertical space
  RECT rc;
  GetClientRect(hw, &rc);
  int listH = (rc.bottom - y - m_buttonBarH - gap);
  if (listH < lineH * 5) listH = lineH * 5;

  m_hList = CreateThemedListView(IDC_MW_HOTKEYS_LIST, x, y, rw, listH,
                                  /*visible=*/true, /*sortable=*/true);
  PAGE_TC(HOTKEYS_PAGE_BINDINGS, m_hList);
  if (m_hList) {
    // Global Key takes the remainder and now needs more of it, because the
    // cell carries the combination AND whether Windows granted it. At the old
    // 16/30/26 split "CTRL+ALT+SHIFT+F24  (in use)" ellipsised to
    // "CTRL+ALT+SHIFT+F24  ..." -- the marker was painted and then clipped,
    // which is worse than not having it at all.
    //
    // Captured both ways at the default window size to be sure, because the
    // first attempt at this measurement was a lie: the capture process set its
    // DPI awareness too late, so PrintWindow drew a 1107px window into a
    // 738px bitmap and everything looked clipped, including things that were
    // not. On a 1.5x display the list is 991px wide, and these four columns
    // come to 1000.
    //
    // The width comes off Category and Local Key, which have the slack: the
    // longest category is "Navigation" and local keys are short ("SHIFT+DOWN").
    int scrollW = GetSystemMetrics(SM_CXVSCROLL) + 4;
    int colCategory = MulDiv(rw, 14, 100);
    int colAction   = MulDiv(rw, 28, 100);
    int colLocal    = MulDiv(rw, 22, 100);
    int colGlobal   = rw - colCategory - colAction - colLocal - scrollW;

    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = (LPWSTR)L"Category";
    col.cx = colCategory;
    SendMessageW(m_hList, LVM_INSERTCOLUMNW, COL_CATEGORY, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Action";
    col.cx = colAction;
    SendMessageW(m_hList, LVM_INSERTCOLUMNW, COL_ACTION, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Local Key";
    col.cx = colLocal;
    SendMessageW(m_hList, LVM_INSERTCOLUMNW, COL_LOCAL_KEY, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Global Key";
    col.cx = colGlobal;
    SendMessageW(m_hList, LVM_INSERTCOLUMNW, COL_GLOBAL_KEY, (LPARAM)&col);

    RefreshHotkeyList(m_hList, m_pEngine);
    ListView_SetItemState(m_hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  }
  y += listH + gap;

  // Button row
  int btnW = MulDiv(60, lineH, 26);
  int addW = MulDiv(30, lineH, 26);
  int btnGap = 8;

  m_hBtnAdd = CreateBtn(hw, L"+", IDC_MW_HOTKEYS_ADD, x, y, addW, lineH, hFont);
  PAGE_TC(HOTKEYS_PAGE_BINDINGS, m_hBtnAdd);
  int bx = x + addW + btnGap;

  m_hBtnEdit = CreateBtn(hw, L"Edit", IDC_MW_HOTKEYS_EDITBTN, bx, y, btnW, lineH, hFont);
  PAGE_TC(HOTKEYS_PAGE_BINDINGS, m_hBtnEdit);
  bx += btnW + btnGap;

  m_hBtnDelete = CreateBtn(hw, L"Delete", IDC_MW_HOTKEYS_DELETE, bx, y, btnW, lineH, hFont);
  PAGE_TC(HOTKEYS_PAGE_BINDINGS, m_hBtnDelete);
  bx += btnW + btnGap;

  int clearKeyW = MulDiv(80, lineH, 26);
  m_hBtnClearKey = CreateBtn(hw, L"Clear Key", IDC_MW_HOTKEYS_CLEARKEY, bx, y, clearKeyW, lineH, hFont);
  PAGE_TC(HOTKEYS_PAGE_BINDINGS, m_hBtnClearKey);
  bx += clearKeyW + btnGap;

  int availW = MulDiv(130, lineH, 26);
  PAGE_TC(HOTKEYS_PAGE_BINDINGS,
          CreateBtn(hw, L"Available keys...", IDC_MW_HOTKEYS_AVAIL, bx, y,
                    availW, lineH, hFont));

  int resetW = MulDiv(160, lineH, 26);
  m_hBtnReset = CreateBtn(hw, L"Reset to Defaults", IDC_MW_HOTKEYS_RESET,
    x + rw - resetW, y, resetW, lineH, hFont);
  PAGE_TC(HOTKEYS_PAGE_BINDINGS, m_hBtnReset);

  #undef PAGE_TC
}

void HotkeysWindow::BuildHelpOrderPage(int x, int y, int rw, int lineH, int gap)
{
  HWND hw = m_hWnd;
  HFONT hFont = GetFont();
  HFONT hFontBold = GetFontBold();

  #define PAGE_TC(page, expr) TrackPageControl(page, (expr))

  PAGE_TC(HOTKEYS_PAGE_HELPORDER,
    CreateLabel(hw, L"F1 Help Category Order", x, y, rw, lineH, hFontBold));
  y += lineH + gap;

  PAGE_TC(HOTKEYS_PAGE_HELPORDER,
    CreateLabel(hw, L"Set the order categories appear on the F1 help overlay:", x, y, rw, lineH, hFont));
  y += lineH + gap;

  // Category list
  RECT rc;
  GetClientRect(hw, &rc);
  int listH = rc.bottom - y - lineH - gap * 3;
  if (listH < lineH * 5) listH = lineH * 5;

  int btnColW = MulDiv(80, lineH, 26) + 16;
  int listW = rw - btnColW;

  m_hCatList = CreateWindowExW(0, WC_LISTBOXW, NULL,
    WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
    x, y, listW, listH, hw, (HMENU)(INT_PTR)IDC_MW_HOTKEYS_CATLIST,
    GetModuleHandle(NULL), NULL);
  SendMessageW(m_hCatList, WM_SETFONT, (WPARAM)hFont, TRUE);
  PAGE_TC(HOTKEYS_PAGE_HELPORDER, m_hCatList);

  // Up/Down/Reset buttons to the right of the list
  int btnX = x + listW + 12;
  int btnW = btnColW - 12;

  // One word each. They are a column beside the list they reorder, so "Move"
  // was repeating what the arrangement already says, and "Move Down" no
  // longer fitted its button at a large font (issue 159).
  m_hBtnCatUp = CreateBtn(hw, L"Up", IDC_MW_HOTKEYS_CAT_UP, btnX, y, btnW, lineH, hFont);
  PAGE_TC(HOTKEYS_PAGE_HELPORDER, m_hBtnCatUp);

  m_hBtnCatDown = CreateBtn(hw, L"Down", IDC_MW_HOTKEYS_CAT_DOWN, btnX, y + lineH + gap, btnW, lineH, hFont);
  PAGE_TC(HOTKEYS_PAGE_HELPORDER, m_hBtnCatDown);

  m_hBtnCatReset = CreateBtn(hw, L"Reset", IDC_MW_HOTKEYS_CAT_RESET, btnX, y + (lineH + gap) * 2, btnW, lineH, hFont);
  PAGE_TC(HOTKEYS_PAGE_HELPORDER, m_hBtnCatReset);

  RefreshCatOrderList();

  #undef PAGE_TC
}

void HotkeysWindow::RefreshCatOrderList()
{
  if (!m_hCatList) return;
  int sel = (int)SendMessageW(m_hCatList, LB_GETCURSEL, 0, 0);
  SendMessageW(m_hCatList, LB_RESETCONTENT, 0, 0);
  for (int i = 0; i < HKCAT_COUNT; i++) {
    int cat = m_pEngine->m_helpCatOrder[i];
    if (cat >= 0 && cat < HKCAT_COUNT)
      SendMessageW(m_hCatList, LB_ADDSTRING, 0, (LPARAM)kCategoryNames[cat]);
  }
  if (sel >= 0 && sel < HKCAT_COUNT)
    SendMessageW(m_hCatList, LB_SETCURSEL, sel, 0);
}

//----------------------------------------------------------------------
// OnResize — reposition controls without full rebuild
//----------------------------------------------------------------------

void HotkeysWindow::OnResize()
{
  LayoutControls();
}

void HotkeysWindow::LayoutControls()
{
  if (!m_hWnd || !m_hList) return;

  RECT rc;
  GetClientRect(m_hWnd, &rc);

  int lineH = GetLineHeight();
  int gap = MulDiv(6, lineH, 26);
  int margin = 12;
  int x = margin;
  int rw = rc.right - 2 * margin;

  // Recalculate list and button positions
  int listY = m_headerH;
  int buttonY = rc.bottom - m_buttonBarH;
  int listH = buttonY - listY - gap;
  if (listH < lineH * 5) listH = lineH * 5;

  MoveWindow(m_hList, x, listY, rw, listH, TRUE);

  // Reposition button row
  int btnW = MulDiv(60, lineH, 26);
  int addW = MulDiv(30, lineH, 26);
  int btnGap = 8;

  if (m_hBtnAdd)    MoveWindow(m_hBtnAdd, x, buttonY, addW, lineH, TRUE);
  int bx = x + addW + btnGap;
  if (m_hBtnEdit)   MoveWindow(m_hBtnEdit, bx, buttonY, btnW, lineH, TRUE);
  bx += btnW + btnGap;
  if (m_hBtnDelete) MoveWindow(m_hBtnDelete, bx, buttonY, btnW, lineH, TRUE);
  bx += btnW + btnGap;
  int clearKeyW = MulDiv(80, lineH, 26);
  if (m_hBtnClearKey) MoveWindow(m_hBtnClearKey, bx, buttonY, clearKeyW, lineH, TRUE);

  int resetW = MulDiv(160, lineH, 26);
  if (m_hBtnReset) MoveWindow(m_hBtnReset, x + rw - resetW, buttonY, resetW, lineH, TRUE);
}

//----------------------------------------------------------------------
// DoCommand — button clicks
//----------------------------------------------------------------------

LRESULT HotkeysWindow::DoCommand(HWND hWnd, int id, int code, LPARAM /*lParam*/)
{
  Engine* p = m_pEngine;

  if (id == IDC_MW_HOTKEYS_ADD && code == BN_CLICKED) {
    // Show popup menu: Script Command / Launch App
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, 1, L"Script Command");
    AppendMenuW(hMenu, MF_STRING, 2, L"Launch App");
    RECT rc;
    GetWindowRect(m_hBtnAdd, &rc);
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
                              rc.left, rc.bottom, 0, hWnd, NULL);
    DestroyMenu(hMenu);

    if (cmd == 1 || cmd == 2) {
      UserHotkeyType type = (cmd == 1) ? USER_HK_SCRIPT : USER_HK_LAUNCH;
      int idx = p->AddUserHotkey(type);
      SaveAndReRegister(p);
      RefreshHotkeyList(m_hList, p);

      // Select the new entry and open edit dialog
      LPARAM newLp = USER_LPARAM_BIT | idx;
      for (int i = 0; i < ListView_GetItemCount(m_hList); i++) {
        if (GetItemLParam(m_hList, i) == newLp) {
          ListView_SetItemState(m_hList, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
          ListView_EnsureVisible(m_hList, i, FALSE);
          OpenEditDialog(i);
          break;
        }
      }
      UpdateDeleteButton();
    }
    return 0;
  }

  if (id == IDC_MW_HOTKEYS_EDITBTN && code == BN_CLICKED) {
    int sel = ListView_GetNextItem(m_hList, -1, LVNI_SELECTED);
    if (sel >= 0) OpenEditDialog(sel);
    return 0;
  }

  if (id == IDC_MW_HOTKEYS_AVAIL && code == BN_CLICKED) {
    // The row selected HERE is what the child offers to bind to, so the
    // selection is read before the dialog opens rather than being asked for
    // again inside it. No selection is not an error: the list is worth
    // reading on its own, and the bind button simply stays disabled.
    int builtIn = -1, userIdx = -1;
    std::wstring name;
    int sel = ListView_GetNextItem(m_hList, -1, LVNI_SELECTED);
    if (sel >= 0) {
      LPARAM lp = GetItemLParam(m_hList, sel);
      if (lp != (LPARAM)-1) {
        if (!IsUserLParam(lp)) {
          builtIn = (int)lp;
          if (builtIn >= 0 && builtIn < NUM_HOTKEYS)
            name = p->m_hotkeys[builtIn].szAction;
          else
            builtIn = -1;
        } else {
          userIdx = UserIndex(lp);
          if (userIdx >= 0 && userIdx < (int)p->m_userHotkeys.size())
            name = p->m_userHotkeys[userIdx].label;
          else
            userIdx = -1;
        }
      }
    }
    AvailableKeysDialog dlg(p, builtIn, userIdx, name);
    // Wide by default, and deliberately wider than the Hotkeys window itself.
    // Three columns, one holding an action name, one holding a combination
    // that must never be the thing that gets clipped, and a button carrying a
    // preset name inside its label. Every narrower value tried clipped one of
    // them; the width is the fix, not another round of column percentages.
    dlg.Show(m_hWnd, 860, 600);
    if (dlg.m_bBound) RefreshHotkeyList(m_hList, p);
    return 0;
  }

  if (id == IDC_MW_HOTKEYS_DELETE && code == BN_CLICKED) {
    LPARAM lp = GetSelectedLParam(m_hList);
    if (lp != (LPARAM)-1 && IsUserLParam(lp)) {
      int idx = UserIndex(lp);
      p->RemoveUserHotkey(idx);
      SaveAndReRegister(p);
      RefreshHotkeyList(m_hList, p);
      // Select first item if any
      if (ListView_GetItemCount(m_hList) > 0)
        ListView_SetItemState(m_hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      UpdateDeleteButton();
    }
    return 0;
  }

  if (id == IDC_MW_HOTKEYS_CLEARKEY && code == BN_CLICKED) {
    LPARAM lp = GetSelectedLParam(m_hList);
    if (lp == (LPARAM)-1) return 0;

    if (IsUserLParam(lp)) {
      int idx = UserIndex(lp);
      if (idx >= 0 && idx < (int)p->m_userHotkeys.size()) {
        p->m_userHotkeys[idx].vk = 0;
        p->m_userHotkeys[idx].modifiers = 0;
      }
    } else {
      int idx = (int)lp;
      if (idx >= 0 && idx < NUM_HOTKEYS) {
        p->m_hotkeys[idx].vk = 0;
        p->m_hotkeys[idx].modifiers = 0;
        p->m_hotkeys[idx].globalVK = 0;
        p->m_hotkeys[idx].globalMod = 0;
      }
    }
    SaveAndReRegister(p);
    RefreshHotkeyList(m_hList, p);
    // Re-select the item
    for (int i = 0; i < ListView_GetItemCount(m_hList); i++) {
      if (GetItemLParam(m_hList, i) == lp) {
        ListView_SetItemState(m_hList, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(m_hList, i, FALSE);
        break;
      }
    }
    return 0;
  }

  if (id == IDC_MW_HOTKEYS_RESET && code == BN_CLICKED) {
    int result = MessageBoxW(hWnd, L"Reset all built-in hotkeys to their default bindings?\n\nUser-added entries will be kept.",
                             L"Reset Hotkeys", MB_YESNO | MB_ICONQUESTION);
    if (result == IDYES) {
      p->ResetHotkeyDefaults();
      SaveAndReRegister(p);
      RefreshHotkeyList(m_hList, p);
      ListView_SetItemState(m_hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      p->AddNotification(L"Built-in hotkeys reset to defaults (user entries kept)");
    }
    return 0;
  }

  // ── Help Display page: category order buttons ──

  if (id == IDC_MW_HOTKEYS_CAT_UP && code == BN_CLICKED) {
    int sel = (int)SendMessageW(m_hCatList, LB_GETCURSEL, 0, 0);
    if (sel > 0) {
      int tmp = p->m_helpCatOrder[sel - 1];
      p->m_helpCatOrder[sel - 1] = p->m_helpCatOrder[sel];
      p->m_helpCatOrder[sel] = tmp;
      p->SaveHelpCatOrder();
      SendMessageW(m_hCatList, LB_SETCURSEL, sel - 1, 0);
      RefreshCatOrderList();
      SendMessageW(m_hCatList, LB_SETCURSEL, sel - 1, 0);
      p->GenerateHelpText();
      g_nHelpLineCount = p->m_nHelpLineCount;
      p->InvalidateHelpTexture();
    }
    return 0;
  }

  if (id == IDC_MW_HOTKEYS_CAT_DOWN && code == BN_CLICKED) {
    int sel = (int)SendMessageW(m_hCatList, LB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < HKCAT_COUNT - 1) {
      int tmp = p->m_helpCatOrder[sel + 1];
      p->m_helpCatOrder[sel + 1] = p->m_helpCatOrder[sel];
      p->m_helpCatOrder[sel] = tmp;
      p->SaveHelpCatOrder();
      SendMessageW(m_hCatList, LB_SETCURSEL, sel + 1, 0);
      RefreshCatOrderList();
      SendMessageW(m_hCatList, LB_SETCURSEL, sel + 1, 0);
      p->GenerateHelpText();
      g_nHelpLineCount = p->m_nHelpLineCount;
      p->InvalidateHelpTexture();
    }
    return 0;
  }

  if (id == IDC_MW_HOTKEYS_CAT_RESET && code == BN_CLICKED) {
    p->ResetHelpCatOrder();
    p->SaveHelpCatOrder();
    RefreshCatOrderList();
    p->GenerateHelpText();
    g_nHelpLineCount = p->m_nHelpLineCount;
    p->InvalidateHelpTexture();
    p->AddNotification(L"Help category order reset to default");
    return 0;
  }

  return -1;
}

//----------------------------------------------------------------------
// DoNotify — ListView selection change, column click sorting, double-click
//----------------------------------------------------------------------

LRESULT HotkeysWindow::DoNotify(HWND hWnd, NMHDR* pnm)
{
  // Tab control handled by base class (TCN_SELCHANGE → ShowPage)

  if (pnm->idFrom != IDC_MW_HOTKEYS_LIST) return -1;

  if (pnm->code == LVN_ITEMCHANGED) {
    NMLISTVIEW* pnmlv = (NMLISTVIEW*)pnm;
    if (pnmlv->uNewState & LVIS_SELECTED)
      UpdateDeleteButton();
    return 0;
  }

  if (pnm->code == NM_DBLCLK) {
    NMITEMACTIVATE* pnma = (NMITEMACTIVATE*)pnm;
    if (pnma->iItem >= 0)
      OpenEditDialog(pnma->iItem);
    return 0;
  }

  if (pnm->code == LVN_COLUMNCLICK) {
    NMLISTVIEW* pnmlv = (NMLISTVIEW*)pnm;
    if (pnmlv->iSubItem == s_sortColumn)
      s_sortAscending = !s_sortAscending;
    else {
      s_sortColumn = pnmlv->iSubItem;
      s_sortAscending = true;
    }
    SortContext ctx = { m_hList, m_pEngine };
    ListView_SortItems(m_hList, HotkeyListCompare, (LPARAM)&ctx);
    return 0;
  }

  return -1;
}

} // namespace mdrop

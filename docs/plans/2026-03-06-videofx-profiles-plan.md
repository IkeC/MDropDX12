# Video FX Profiles Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Save/load video effects as JSON profiles with a dedicated profile picker window and startup auto-load.

**Architecture:** Engine-level serialize/deserialize methods using `json_utils.h`. Profiles stored as `.json` files in `resources/videofx/`. New `VFXProfileWindow` (ToolWindow subclass) with listbox for instant switching. Startup pattern mirrors existing preset startup (`bEnableVFXStartup` + `szVFXStartup` + save-on-close).

**Tech Stack:** C++17, Win32, json_utils.h (JsonWriter/JsonParse/JsonLoadFile/JsonSaveFile)

---

### Task 1: Engine Members and Method Declarations

**Files:**
- Modify: `src/mDropDX12/engine.h:975-978` (after Video Effects Window block)

**Step 1: Add profile members and method declarations**

After `void CloseVideoEffectsWindow();` (line 978), add:

```cpp
  // Video FX Profiles
  wchar_t m_szCurrentVFXProfile[MAX_PATH] = {};  // currently loaded profile (empty = none)
  bool    m_bEnableVFXStartup = false;
  wchar_t m_szVFXStartup[MAX_PATH] = {};
  bool    m_bEnableVFXStartupSavingOnClose = true;
  void    SaveVideoFXProfile(const wchar_t* path);
  bool    LoadVideoFXProfile(const wchar_t* path);
  void    GetVideoFXProfileDir(wchar_t* out, size_t len);

  // VFX Profile Picker Window
  class VFXProfileWindow* m_pVFXProfileWindow = nullptr;
  void OpenVFXProfileWindow();
  void CloseVFXProfileWindow();
```

**Step 2: Build to verify**

Run: `powershell -ExecutionPolicy Bypass -File build.ps1 Release x64`
Expected: 0 errors (new members declared but not yet used)

**Step 3: Commit**

```
git add src/mDropDX12/engine.h
git commit -m "Add VFX profile members and method declarations"
```

---

### Task 2: Profile Serialize/Deserialize + Directory Helper

**Files:**
- Modify: `src/mDropDX12/engine_spout_input.cpp:839` (before closing `} // namespace mdrop`)

**Step 1: Add GetVideoFXProfileDir**

Before the closing `} // namespace mdrop` at end of file, add:

```cpp
// ---------------------------------------------------------------------------
// Video FX Profile I/O
// ---------------------------------------------------------------------------
void Engine::GetVideoFXProfileDir(wchar_t* out, size_t len)
{
    swprintf_s(out, len, L"%svideofx\\", m_szMilkdrop2Path);
}

void Engine::SaveVideoFXProfile(const wchar_t* path)
{
    // Ensure directory exists
    wchar_t dir[MAX_PATH];
    GetVideoFXProfileDir(dir, MAX_PATH);
    CreateDirectoryW(dir, NULL);

    JsonWriter w;
    w.BeginObject();
    w.String(L"name", std::wstring(path));  // store filename for display
    w.Int(L"version", 1);

    w.BeginObject(L"transform");
    w.Float(L"posX", m_videoFX.posX);
    w.Float(L"posY", m_videoFX.posY);
    w.Float(L"scale", m_videoFX.scale);
    w.Float(L"rotation", m_videoFX.rotation);
    w.Bool(L"mirrorH", m_videoFX.mirrorH);
    w.Bool(L"mirrorV", m_videoFX.mirrorV);
    w.EndObject();

    w.BeginObject(L"color");
    w.Float(L"tintR", m_videoFX.tintR);
    w.Float(L"tintG", m_videoFX.tintG);
    w.Float(L"tintB", m_videoFX.tintB);
    w.Float(L"brightness", m_videoFX.brightness);
    w.Float(L"contrast", m_videoFX.contrast);
    w.Float(L"saturation", m_videoFX.saturation);
    w.Float(L"hueShift", m_videoFX.hueShift);
    w.Bool(L"invert", m_videoFX.invert);
    w.EndObject();

    w.BeginObject(L"effects");
    w.Float(L"pixelation", m_videoFX.pixelation);
    w.Float(L"chromatic", m_videoFX.chromatic);
    w.Bool(L"edgeDetect", m_videoFX.edgeDetect);
    w.EndObject();

    w.Int(L"blendMode", m_videoFX.blendMode);

    auto writeAR = [&](const wchar_t* key, const AudioLink& ar) {
        w.BeginObject(key);
        w.Int(L"source", ar.source);
        w.Float(L"intensity", ar.intensity);
        w.EndObject();
    };
    w.BeginObject(L"audio");
    writeAR(L"posX",       m_videoFX.arPosX);
    writeAR(L"posY",       m_videoFX.arPosY);
    writeAR(L"scale",      m_videoFX.arScale);
    writeAR(L"rotation",   m_videoFX.arRotation);
    writeAR(L"brightness", m_videoFX.arBrightness);
    writeAR(L"saturation", m_videoFX.arSaturation);
    writeAR(L"chromatic",  m_videoFX.arChromatic);
    w.EndObject();

    w.EndObject();
    w.SaveToFile(path);
}

bool Engine::LoadVideoFXProfile(const wchar_t* path)
{
    JsonValue root = JsonLoadFile(path);
    if (root.isNull()) return false;

    auto& t = root[L"transform"];
    if (!t.isNull()) {
        m_videoFX.posX     = t[L"posX"].asFloat(0);
        m_videoFX.posY     = t[L"posY"].asFloat(0);
        m_videoFX.scale    = t[L"scale"].asFloat(1.0f);
        m_videoFX.rotation = t[L"rotation"].asFloat(0);
        m_videoFX.mirrorH  = t[L"mirrorH"].asBool(false);
        m_videoFX.mirrorV  = t[L"mirrorV"].asBool(false);
    }

    auto& c = root[L"color"];
    if (!c.isNull()) {
        m_videoFX.tintR      = c[L"tintR"].asFloat(1);
        m_videoFX.tintG      = c[L"tintG"].asFloat(1);
        m_videoFX.tintB      = c[L"tintB"].asFloat(1);
        m_videoFX.brightness = c[L"brightness"].asFloat(0);
        m_videoFX.contrast   = c[L"contrast"].asFloat(1.0f);
        m_videoFX.saturation = c[L"saturation"].asFloat(1.0f);
        m_videoFX.hueShift   = c[L"hueShift"].asFloat(0);
        m_videoFX.invert     = c[L"invert"].asBool(false);
    }

    auto& e = root[L"effects"];
    if (!e.isNull()) {
        m_videoFX.pixelation = e[L"pixelation"].asFloat(0);
        m_videoFX.chromatic  = e[L"chromatic"].asFloat(0);
        m_videoFX.edgeDetect = e[L"edgeDetect"].asBool(false);
    }

    m_videoFX.blendMode = root[L"blendMode"].asInt(0);

    auto readAR = [&](const JsonValue& obj, AudioLink& ar) {
        if (!obj.isNull()) {
            ar.source    = obj[L"source"].asInt(0);
            ar.intensity = obj[L"intensity"].asFloat(0.5f);
        }
    };
    auto& a = root[L"audio"];
    if (!a.isNull()) {
        readAR(a[L"posX"],       m_videoFX.arPosX);
        readAR(a[L"posY"],       m_videoFX.arPosY);
        readAR(a[L"scale"],      m_videoFX.arScale);
        readAR(a[L"rotation"],   m_videoFX.arRotation);
        readAR(a[L"brightness"], m_videoFX.arBrightness);
        readAR(a[L"saturation"], m_videoFX.arSaturation);
        readAR(a[L"chromatic"],  m_videoFX.arChromatic);
    }

    wcscpy_s(m_szCurrentVFXProfile, path);
    return true;
}
```

**Step 2: Add `#include "json_utils.h"` at top of engine_spout_input.cpp** (if not already present)

**Step 3: Build to verify**

Run: `powershell -ExecutionPolicy Bypass -File build.ps1 Release x64`
Expected: 0 errors

**Step 4: Commit**

```
git add src/mDropDX12/engine_spout_input.cpp
git commit -m "Add VFX profile JSON serialize/deserialize methods"
```

---

### Task 3: INI Persistence for Startup Settings

**Files:**
- Modify: `src/mDropDX12/engine_spout_input.cpp` — inside `LoadSpoutInputSettings()` (after line ~764) and `SaveSpoutInputSettings()` (after line ~838)

**Step 1: Add INI load for startup settings**

At the end of `LoadSpoutInputSettings()`, after the `readAR` block (before the closing `}`), add:

```cpp
    // VFX Profile startup settings
    m_bEnableVFXStartup = GetPrivateProfileIntW(L"VideoFX", L"bEnableVFXStartup", 0, pIni) != 0;
    GetPrivateProfileStringW(L"VideoFX", L"szVFXStartup", L"", m_szVFXStartup, MAX_PATH, pIni);
    m_bEnableVFXStartupSavingOnClose = GetPrivateProfileIntW(L"VideoFX", L"bEnableVFXStartupSavingOnClose", 1, pIni) != 0;
    GetPrivateProfileStringW(L"VideoFX", L"szCurrentVFXProfile", L"", m_szCurrentVFXProfile, MAX_PATH, pIni);
```

**Step 2: Add INI save for startup settings**

At the end of `SaveSpoutInputSettings()`, after the `writeAR` block (before the closing `}`), add:

```cpp
    // VFX Profile startup settings
    WritePrivateProfileStringW(L"VideoFX", L"bEnableVFXStartup", m_bEnableVFXStartup ? L"1" : L"0", pIni);
    WritePrivateProfileStringW(L"VideoFX", L"szVFXStartup", m_szVFXStartup, pIni);
    WritePrivateProfileStringW(L"VideoFX", L"bEnableVFXStartupSavingOnClose", m_bEnableVFXStartupSavingOnClose ? L"1" : L"0", pIni);
    WritePrivateProfileStringW(L"VideoFX", L"szCurrentVFXProfile", m_szCurrentVFXProfile, pIni);
```

**Step 3: Build and commit**

```
git add src/mDropDX12/engine_spout_input.cpp
git commit -m "Persist VFX profile startup settings in INI"
```

---

### Task 4: Startup Load + Save-on-Close

**Files:**
- Modify: `src/mDropDX12/engine.cpp:2995` (after preset startup load)
- Modify: `src/mDropDX12/engine_spout_input.cpp` — in `SaveSpoutInputSettings()` add save-on-close logic

**Step 1: Add startup profile load**

In `engine.cpp`, after the startup preset block (around line ~3020, after the `else { }` of the startup preset section), add:

```cpp
    // Load VFX profile on startup
    if (m_bEnableVFXStartup && wcslen(m_szVFXStartup) > 0)
        LoadVideoFXProfile(m_szVFXStartup);
```

**Step 2: Add save-on-close**

In `SaveSpoutInputSettings()`, at the very end (before closing `}`), add:

```cpp
    // Auto-save current VFX profile on close
    if (m_bEnableVFXStartupSavingOnClose && wcslen(m_szCurrentVFXProfile) > 0)
        SaveVideoFXProfile(m_szCurrentVFXProfile);
```

**Step 3: Build and commit**

```
git add src/mDropDX12/engine.cpp src/mDropDX12/engine_spout_input.cpp
git commit -m "Add VFX profile startup load and save-on-close"
```

---

### Task 5: Control IDs + VFXProfileWindow Declaration

**Files:**
- Modify: `src/mDropDX12/engine_helpers.h:693` (after `IDC_MW_OPEN_VFX`)
- Modify: `src/mDropDX12/tool_window.h:504` (after VideoEffectsWindow class)
- Modify: `src/mDropDX12/hotkeys.h:132` (before HK_OPEN_VIDEO_FX line, or after it)
- Modify: `src/mDropDX12/engine_hotkeys.cpp:125,671` (HK_DEF + dispatch)

**Step 1: Add control IDs in engine_helpers.h**

After line 693 (`IDC_MW_OPEN_VFX`), add:

```cpp
// VFX Profile window controls (9290-9310)
#define IDC_MW_VFXP_PIN           9290
#define IDC_MW_VFXP_FONT_PLUS     9291
#define IDC_MW_VFXP_FONT_MINUS    9292
#define IDC_MW_VFXP_LIST          9293  // Listbox: profile list
#define IDC_MW_VFXP_SAVE          9294  // Button: Save As...
#define IDC_MW_VFXP_DELETE        9295  // Button: Delete
#define IDC_MW_VFXP_STARTUP       9296  // Checkbox: Load on startup
#define IDC_MW_VFXP_SAVECLOSE     9297  // Checkbox: Save on close

// Save/Load buttons on Video Effects window
#define IDC_MW_VFX_SAVE_PROFILE   9281
#define IDC_MW_VFX_LOAD_PROFILE   9282  // Opens profile picker
```

**Step 2: Add VFXProfileWindow class declaration in tool_window.h**

After the `VideoEffectsWindow` closing `};` (line 504), before `// ── Channel input sources`, add:

```cpp
// ── Concrete subclass: VFX Profile Picker window ──

class VFXProfileWindow : public ToolWindow {
public:
  VFXProfileWindow(Engine* pEngine);

protected:
  const wchar_t* GetWindowTitle() const override { return L"VFX Profiles"; }
  const wchar_t* GetWindowClass() const override { return L"MDropDX12VFXProfileWnd"; }
  const wchar_t* GetINISection() const override  { return L"VFXProfiles"; }
  int GetPinControlID() const override       { return IDC_MW_VFXP_PIN; }
  int GetFontPlusControlID() const override  { return IDC_MW_VFXP_FONT_PLUS; }
  int GetFontMinusControlID() const override { return IDC_MW_VFXP_FONT_MINUS; }
  int GetMinWidth() const override  { return 280; }
  int GetMinHeight() const override { return 350; }

  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  void    DoDestroy() override;

private:
  void RefreshProfileList();
  void ApplySelectedProfile();
  std::vector<std::wstring> m_profilePaths;  // full paths, indexed parallel to listbox
};
```

**Step 3: Add hotkey enum**

In `hotkeys.h`, after `HK_OPEN_VIDEO_FX` (line 132), add:

```cpp
    HK_OPEN_VFX_PROFILES,       // (unbound) — open VFX Profile Picker window
```

**Step 4: Add HK_DEF and dispatch in engine_hotkeys.cpp**

After the `HK_OPEN_VIDEO_FX` HK_DEF line (line 125), add:

```cpp
    HK_DEF(i++, HK_OPEN_VFX_PROFILES,0,                   0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open VFX Profiles",    L"OpenVFXProfiles");
```

After the `case HK_OPEN_VIDEO_FX:` dispatch (line ~671), add:

```cpp
    case HK_OPEN_VFX_PROFILES:
        OpenVFXProfileWindow();
        return true;
```

**Step 5: Build and commit**

```
git add src/mDropDX12/engine_helpers.h src/mDropDX12/tool_window.h src/mDropDX12/hotkeys.h src/mDropDX12/engine_hotkeys.cpp
git commit -m "Add VFXProfileWindow class declaration, control IDs, and hotkey"
```

---

### Task 6: VFX Profile Picker Window Implementation

**Files:**
- Create: `src/mDropDX12/engine_vfx_profiles_ui.cpp`
- Modify: `src/mDropDX12/engine.vcxproj:384` (add new .cpp after engine_video_effects_ui.cpp)

**Step 1: Create engine_vfx_profiles_ui.cpp**

```cpp
// engine_vfx_profiles_ui.cpp — VFX Profile Picker window
//
// Listbox-based instant profile switching with save/delete/startup controls.

#include "engine.h"
#include "tool_window.h"
#include "engine_helpers.h"
#include "json_utils.h"
#include <shlwapi.h>  // PathFindFileNameW, PathRemoveExtensionW

namespace mdrop {

VFXProfileWindow::VFXProfileWindow(Engine* pEngine)
    : ToolWindow(pEngine, 300, 400) {}

// ---------------------------------------------------------------------------
// Open / Close wrappers on Engine
// ---------------------------------------------------------------------------
void Engine::OpenVFXProfileWindow()
{
    if (!m_pVFXProfileWindow)
        m_pVFXProfileWindow = new VFXProfileWindow(this);
    m_pVFXProfileWindow->Open();
}

void Engine::CloseVFXProfileWindow()
{
    if (m_pVFXProfileWindow) {
        m_pVFXProfileWindow->Close();
        delete m_pVFXProfileWindow;
        m_pVFXProfileWindow = nullptr;
    }
}

// ---------------------------------------------------------------------------
// DoBuildControls
// ---------------------------------------------------------------------------
void VFXProfileWindow::DoBuildControls()
{
    auto base = BuildBaseControls();
    int y = base.y, lineH = base.lineH, gap = base.gap;
    int x = base.x, rw = base.rw;

    // Profile listbox
    CreateLabel(m_hWnd, L"Profiles:", x, y, rw, lineH, m_hFontBold);
    y += lineH + gap;

    int listH = lineH * 6;
    HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        x, y, rw, listH, m_hWnd,
        (HMENU)(INT_PTR)IDC_MW_VFXP_LIST, GetModuleHandle(NULL), NULL);
    if (hList && m_hFont) SendMessage(hList, WM_SETFONT, (WPARAM)m_hFont, TRUE);
    y += listH + gap;

    // Save As / Delete buttons
    int btnW = MulDiv(90, lineH, 26);
    int btnGap = 8;
    CreateBtn(m_hWnd, L"Save As...", IDC_MW_VFXP_SAVE, x, y, btnW, lineH, m_hFont);
    CreateBtn(m_hWnd, L"Delete", IDC_MW_VFXP_DELETE, x + btnW + btnGap, y, btnW, lineH, m_hFont);
    y += lineH + gap;

    // Startup checkboxes
    CreateCheck(m_hWnd, L"Load on startup", IDC_MW_VFXP_STARTUP, x, y, rw, lineH, m_hFont,
                m_pEngine->m_bEnableVFXStartup);
    y += lineH + gap;
    CreateCheck(m_hWnd, L"Save on close", IDC_MW_VFXP_SAVECLOSE, x, y, rw, lineH, m_hFont,
                m_pEngine->m_bEnableVFXStartupSavingOnClose);

    RefreshProfileList();
}

// ---------------------------------------------------------------------------
// RefreshProfileList — scan resources/videofx/*.json, populate listbox
// ---------------------------------------------------------------------------
void VFXProfileWindow::RefreshProfileList()
{
    HWND hList = GetDlgItem(m_hWnd, IDC_MW_VFXP_LIST);
    if (!hList) return;
    SendMessage(hList, LB_RESETCONTENT, 0, 0);
    m_profilePaths.clear();

    // (None) entry
    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)L"(None)");
    m_profilePaths.push_back(L"");

    // Scan directory
    wchar_t dir[MAX_PATH];
    m_pEngine->GetVideoFXProfileDir(dir, MAX_PATH);

    wchar_t pattern[MAX_PATH];
    swprintf_s(pattern, MAX_PATH, L"%s*.json", dir);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern, &fd);
    int selIdx = 0;
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            wchar_t fullPath[MAX_PATH];
            swprintf_s(fullPath, MAX_PATH, L"%s%s", dir, fd.cFileName);

            // Display name = filename without .json extension
            wchar_t displayName[MAX_PATH];
            wcscpy_s(displayName, fd.cFileName);
            wchar_t* dot = wcsrchr(displayName, L'.');
            if (dot) *dot = 0;

            int idx = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)displayName);
            m_profilePaths.push_back(fullPath);

            // Select if this is the current profile
            if (_wcsicmp(fullPath, m_pEngine->m_szCurrentVFXProfile) == 0)
                selIdx = idx;
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    SendMessage(hList, LB_SETCURSEL, selIdx, 0);
}

// ---------------------------------------------------------------------------
// ApplySelectedProfile
// ---------------------------------------------------------------------------
void VFXProfileWindow::ApplySelectedProfile()
{
    HWND hList = GetDlgItem(m_hWnd, IDC_MW_VFXP_LIST);
    int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)m_profilePaths.size()) return;

    if (m_profilePaths[sel].empty()) {
        // (None) — reset to defaults
        m_pEngine->m_videoFX = Engine::VideoEffectParams{};
        m_pEngine->m_szCurrentVFXProfile[0] = 0;
    } else {
        m_pEngine->LoadVideoFXProfile(m_profilePaths[sel].c_str());
    }

    // Update startup path if startup is enabled
    if (m_pEngine->m_bEnableVFXStartup)
        wcscpy_s(m_pEngine->m_szVFXStartup, m_pEngine->m_szCurrentVFXProfile);

    m_pEngine->SaveSpoutInputSettings();

    // Refresh effects window if open
    if (m_pEngine->m_pVideoEffectsWindow)
        m_pEngine->m_pVideoEffectsWindow->RebuildFonts();
}

// ---------------------------------------------------------------------------
// DoCommand
// ---------------------------------------------------------------------------
LRESULT VFXProfileWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam)
{
    switch (id) {
    case IDC_MW_VFXP_LIST:
        if (code == LBN_SELCHANGE)
            ApplySelectedProfile();
        return 0;

    case IDC_MW_VFXP_SAVE: {
        // Save As — prompt for name with GetSaveFileNameW
        wchar_t dir[MAX_PATH];
        m_pEngine->GetVideoFXProfileDir(dir, MAX_PATH);
        CreateDirectoryW(dir, NULL);

        wchar_t filePath[MAX_PATH] = {};
        // Pre-fill with current profile name
        if (wcslen(m_pEngine->m_szCurrentVFXProfile) > 0) {
            const wchar_t* fname = PathFindFileNameW(m_pEngine->m_szCurrentVFXProfile);
            wcscpy_s(filePath, fname);
        }

        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_hWnd;
        ofn.lpstrFilter = L"VFX Profile (*.json)\0*.json\0All Files\0*.*\0";
        ofn.lpstrFile = filePath;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = dir;
        ofn.lpstrDefExt = L"json";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

        if (GetSaveFileNameW(&ofn)) {
            m_pEngine->SaveVideoFXProfile(filePath);
            wcscpy_s(m_pEngine->m_szCurrentVFXProfile, filePath);
            if (m_pEngine->m_bEnableVFXStartup)
                wcscpy_s(m_pEngine->m_szVFXStartup, filePath);
            m_pEngine->SaveSpoutInputSettings();
            RefreshProfileList();
        }
        return 0;
    }

    case IDC_MW_VFXP_DELETE: {
        HWND hList = GetDlgItem(m_hWnd, IDC_MW_VFXP_LIST);
        int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
        if (sel <= 0 || sel >= (int)m_profilePaths.size()) return 0;  // can't delete (None)

        // Confirm deletion
        wchar_t msg[512];
        wchar_t name[MAX_PATH];
        SendMessageW(hList, LB_GETTEXT, sel, (LPARAM)name);
        swprintf_s(msg, L"Delete profile \"%s\"?", name);
        if (MessageBoxW(m_hWnd, msg, L"Delete VFX Profile", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;

        DeleteFileW(m_profilePaths[sel].c_str());

        // If this was the current profile, clear it
        if (_wcsicmp(m_profilePaths[sel].c_str(), m_pEngine->m_szCurrentVFXProfile) == 0)
            m_pEngine->m_szCurrentVFXProfile[0] = 0;
        if (_wcsicmp(m_profilePaths[sel].c_str(), m_pEngine->m_szVFXStartup) == 0)
            m_pEngine->m_szVFXStartup[0] = 0;

        m_pEngine->SaveSpoutInputSettings();
        RefreshProfileList();
        return 0;
    }

    case IDC_MW_VFXP_STARTUP:
        m_pEngine->m_bEnableVFXStartup = (SendMessage((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED);
        if (m_pEngine->m_bEnableVFXStartup)
            wcscpy_s(m_pEngine->m_szVFXStartup, m_pEngine->m_szCurrentVFXProfile);
        m_pEngine->SaveSpoutInputSettings();
        return 0;

    case IDC_MW_VFXP_SAVECLOSE:
        m_pEngine->m_bEnableVFXStartupSavingOnClose = (SendMessage((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED);
        m_pEngine->SaveSpoutInputSettings();
        return 0;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// DoDestroy
// ---------------------------------------------------------------------------
void VFXProfileWindow::DoDestroy()
{
    m_pEngine->SaveSpoutInputSettings();
}

} // namespace mdrop
```

**Step 2: Add to vcxproj**

After `<ClCompile Include="engine_video_effects_ui.cpp" />` (line 384), add:

```xml
    <ClCompile Include="engine_vfx_profiles_ui.cpp" />
```

**Step 3: Build and commit**

```
git add src/mDropDX12/engine_vfx_profiles_ui.cpp src/mDropDX12/engine.vcxproj
git commit -m "Add VFX Profile Picker window implementation"
```

---

### Task 7: Save/Load Buttons on Video Effects Window

**Files:**
- Modify: `src/mDropDX12/engine_video_effects_ui.cpp` — in `DoBuildControls()` and `DoCommand()`

**Step 1: Add Save/Load buttons before the tab control**

In `DoBuildControls()`, after `int rw = base.rw;` (line ~40) and before the tab control creation, add:

```cpp
    // Profile save/load buttons (above tabs)
    int btnW = MulDiv(100, lineH, 26);
    int btnGap = 8;
    CreateBtn(hw, L"Save Profile...", IDC_MW_VFX_SAVE_PROFILE, x, y, btnW, lineH, hFont);
    CreateBtn(hw, L"Profiles...", IDC_MW_VFX_LOAD_PROFILE, x + btnW + btnGap, y, btnW, lineH, hFont);
    y += lineH + gap;
```

Note: `hw` needs to be set — either extract from `m_hWnd` before the tab, or use `m_hWnd`. Check that `hFont` etc. are available. The base controls have already set these up. Use `m_hWnd`, `m_hFont`.

Actually, looking at the existing code, `DoBuildControls` uses `auto base = BuildBaseControls()` which returns x,y,rw,lineH,gap. The `hw` and `hFont` variables are set inside the Build*Page methods. So add the buttons using `m_hWnd` and `m_hFont` directly:

```cpp
    // Profile save/load buttons (above tabs)
    int profileBtnW = MulDiv(100, lineH, 26);
    CreateBtn(m_hWnd, L"Save Profile...", IDC_MW_VFX_SAVE_PROFILE, x, y, profileBtnW, lineH, m_hFont);
    CreateBtn(m_hWnd, L"Profiles...", IDC_MW_VFX_LOAD_PROFILE, x + profileBtnW + 8, y, profileBtnW, lineH, m_hFont);
    y += lineH + gap;
```

**Step 2: Add command handlers in DoCommand**

Before the `return -1;` at the end of DoCommand, add cases:

```cpp
    case IDC_MW_VFX_SAVE_PROFILE: {
        // Save current profile using GetSaveFileNameW
        wchar_t dir[MAX_PATH];
        m_pEngine->GetVideoFXProfileDir(dir, MAX_PATH);
        CreateDirectoryW(dir, NULL);

        wchar_t filePath[MAX_PATH] = {};
        if (wcslen(m_pEngine->m_szCurrentVFXProfile) > 0) {
            const wchar_t* fname = PathFindFileNameW(m_pEngine->m_szCurrentVFXProfile);
            wcscpy_s(filePath, fname);
        }

        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_hWnd;
        ofn.lpstrFilter = L"VFX Profile (*.json)\0*.json\0All Files\0*.*\0";
        ofn.lpstrFile = filePath;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = dir;
        ofn.lpstrDefExt = L"json";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

        if (GetSaveFileNameW(&ofn)) {
            m_pEngine->SaveVideoFXProfile(filePath);
            wcscpy_s(m_pEngine->m_szCurrentVFXProfile, filePath);
            m_pEngine->SaveSpoutInputSettings();
        }
        return 0;
    }

    case IDC_MW_VFX_LOAD_PROFILE:
        m_pEngine->OpenVFXProfileWindow();
        return 0;
```

**Step 3: Add `#include <shlwapi.h>` at top** (for `PathFindFileNameW`)

**Step 4: Build and commit**

```
git add src/mDropDX12/engine_video_effects_ui.cpp
git commit -m "Add Save Profile / Profiles buttons on Video Effects window"
```

---

### Task 8: Final Build + Verification

**Step 1: Full build**

Run: `powershell -ExecutionPolicy Bypass -File build.ps1 Release x64`
Expected: 0 errors

**Step 2: Manual verification checklist**

1. Open Video Effects window — Save Profile / Profiles buttons visible above tabs
2. Click "Save Profile..." — save dialog opens in `resources/videofx/`, save a profile
3. Click "Profiles..." — VFX Profile Picker window opens
4. Listbox shows "(None)" + saved profile(s)
5. Click a profile — effects apply instantly, effects window sliders update
6. Click "(None)" — resets all effects to defaults
7. Save As in profile picker — creates new profile
8. Delete — removes profile file, selects (None)
9. Check "Load on startup" — close and reopen app — profile loads on startup
10. Check "Save on close" — modify effects, close app, reopen — changes persisted to profile
11. VFX Profiles appears in Settings > Tools tab

**Step 3: Final commit**

```
git add -A
git commit -m "Add VFX JSON profile system with profile picker window and startup auto-load"
```

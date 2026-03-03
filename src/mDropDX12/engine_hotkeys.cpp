// engine_hotkeys.cpp — Configurable hotkey load/save/register/dispatch
//
// Part of the MDropDX12 configurable hotkeys system.
// Supports local (render-window-focus) and global (system-wide) bindings.

#include "engine.h"
#include "utility.h"
#include <TlHelp32.h>

namespace mdrop {

void Engine::ResetHotkeyDefaults()
{
    m_hotkeys[0] = { HK_TOGGLE_FULLSCREEN, 0, 0, HKSCOPE_LOCAL,
                     L"Toggle Fullscreen", L"ToggleFullscreen", 0, 0, HKSCOPE_LOCAL };
    m_hotkeys[1] = { HK_TOGGLE_STRETCH, 0, 0, HKSCOPE_LOCAL,
                     L"Toggle Stretch/Mirror", L"ToggleStretch", 0, 0, HKSCOPE_LOCAL };
    m_hotkeys[2] = { HK_OPEN_SETTINGS, 0, 0, HKSCOPE_LOCAL,
                     L"Open Settings", L"OpenSettings", 0, 0, HKSCOPE_LOCAL };
    m_hotkeys[3] = { HK_OPEN_DISPLAYS, MOD_CONTROL, VK_F8, HKSCOPE_LOCAL,
                     L"Open Spout/Displays", L"OpenDisplays", MOD_CONTROL, VK_F8, HKSCOPE_LOCAL };
    m_hotkeys[4] = { HK_OPEN_SONGINFO, MOD_SHIFT | MOD_CONTROL, VK_F8, HKSCOPE_LOCAL,
                     L"Open Song Info", L"OpenSongInfo", MOD_SHIFT | MOD_CONTROL, VK_F8, HKSCOPE_LOCAL };
    m_hotkeys[5] = { HK_OPEN_HOTKEYS, MOD_CONTROL, VK_F7, HKSCOPE_LOCAL,
                     L"Open Hotkeys", L"OpenHotkeys", MOD_CONTROL, VK_F7, HKSCOPE_LOCAL };
    m_hotkeys[6] = { HK_LAUNCH_APP_1, 0, 0, HKSCOPE_GLOBAL,
                     L"Launch App 1", L"LaunchApp1", 0, 0, HKSCOPE_GLOBAL };
    m_hotkeys[7] = { HK_LAUNCH_APP_2, 0, 0, HKSCOPE_GLOBAL,
                     L"Launch App 2", L"LaunchApp2", 0, 0, HKSCOPE_GLOBAL };
    m_hotkeys[8] = { HK_LAUNCH_APP_3, 0, 0, HKSCOPE_GLOBAL,
                     L"Launch App 3", L"LaunchApp3", 0, 0, HKSCOPE_GLOBAL };
    m_hotkeys[9] = { HK_LAUNCH_APP_4, 0, 0, HKSCOPE_GLOBAL,
                     L"Launch App 4", L"LaunchApp4", 0, 0, HKSCOPE_GLOBAL };
    for (int i = 0; i < 4; i++)
        m_szLaunchApp[i][0] = L'\0';
}

void Engine::LoadHotkeySettings()
{
    wchar_t* pIni = GetConfigIniFile();

    // Start from defaults
    ResetHotkeyDefaults();

    // Migration: if old "Enabled" key exists, migrate scope for configured bindings
    int oldEnabled = GetPrivateProfileIntW(L"Hotkeys", L"Enabled", -1, pIni);

    // Read overrides from INI
    for (int i = 0; i < NUM_HOTKEYS; i++) {
        wchar_t modKey[128], vkKey[128], scopeKey[128];
        swprintf(modKey, 128, L"%s_Mod", m_hotkeys[i].szIniKey);
        swprintf(vkKey, 128, L"%s_VK", m_hotkeys[i].szIniKey);
        swprintf(scopeKey, 128, L"%s_Scope", m_hotkeys[i].szIniKey);

        m_hotkeys[i].modifiers = (UINT)GetPrivateProfileIntW(L"Hotkeys", modKey, (int)m_hotkeys[i].modifiers, pIni);
        m_hotkeys[i].vk = (UINT)GetPrivateProfileIntW(L"Hotkeys", vkKey, (int)m_hotkeys[i].vk, pIni);
        m_hotkeys[i].scope = (HotkeyScope)GetPrivateProfileIntW(L"Hotkeys", scopeKey, (int)m_hotkeys[i].scope, pIni);
    }

    // Migration: old system had master enable toggle for first 2 bindings
    if (oldEnabled >= 0) {
        if (oldEnabled == 1) {
            // Promote any configured bindings to global scope
            for (int i = 0; i < 2 && i < NUM_HOTKEYS; i++) {
                if (m_hotkeys[i].vk != 0)
                    m_hotkeys[i].scope = HKSCOPE_GLOBAL;
            }
        }
        // Delete the old key and re-save in new format
        WritePrivateProfileStringW(L"Hotkeys", L"Enabled", NULL, pIni);
        SaveHotkeySettings();
    }

    // Load Launch App paths
    for (int i = 0; i < 4; i++) {
        wchar_t key[64];
        swprintf(key, 64, L"LaunchApp%d_Path", i + 1);
        GetPrivateProfileStringW(L"Hotkeys", key, L"", m_szLaunchApp[i], MAX_PATH, pIni);
    }
}

void Engine::SaveHotkeySettings()
{
    wchar_t* pIni = GetConfigIniFile();
    wchar_t buf[64];

    for (int i = 0; i < NUM_HOTKEYS; i++) {
        wchar_t modKey[128], vkKey[128], scopeKey[128];
        swprintf(modKey, 128, L"%s_Mod", m_hotkeys[i].szIniKey);
        swprintf(vkKey, 128, L"%s_VK", m_hotkeys[i].szIniKey);
        swprintf(scopeKey, 128, L"%s_Scope", m_hotkeys[i].szIniKey);

        swprintf(buf, 64, L"%u", m_hotkeys[i].modifiers);
        WritePrivateProfileStringW(L"Hotkeys", modKey, buf, pIni);
        swprintf(buf, 64, L"%u", m_hotkeys[i].vk);
        WritePrivateProfileStringW(L"Hotkeys", vkKey, buf, pIni);
        swprintf(buf, 64, L"%d", (int)m_hotkeys[i].scope);
        WritePrivateProfileStringW(L"Hotkeys", scopeKey, buf, pIni);
    }

    // Save Launch App paths
    for (int i = 0; i < 4; i++) {
        wchar_t key[64];
        swprintf(key, 64, L"LaunchApp%d_Path", i + 1);
        WritePrivateProfileStringW(L"Hotkeys", key, m_szLaunchApp[i], pIni);
    }
}

void Engine::RegisterGlobalHotkeys(HWND hwnd)
{
    if (!hwnd) return;
    for (int i = 0; i < NUM_HOTKEYS; i++) {
        if (m_hotkeys[i].vk != 0 && m_hotkeys[i].scope == HKSCOPE_GLOBAL) {
            RegisterHotKey(hwnd, m_hotkeys[i].id, m_hotkeys[i].modifiers | MOD_NOREPEAT, m_hotkeys[i].vk);
        }
    }
}

void Engine::UnregisterGlobalHotkeys(HWND hwnd)
{
    if (!hwnd) return;
    for (int i = 0; i < NUM_HOTKEYS; i++) {
        UnregisterHotKey(hwnd, m_hotkeys[i].id);
    }
}

bool Engine::DispatchHotkeyAction(int actionId)
{
    switch (actionId) {
    case HK_OPEN_SETTINGS:
        OpenSettingsWindow();
        return true;
    case HK_OPEN_DISPLAYS:
        OpenDisplaysWindow();
        return true;
    case HK_OPEN_SONGINFO:
        OpenSongInfoWindow();
        return true;
    case HK_OPEN_HOTKEYS:
        OpenHotkeysWindow();
        return true;
    case HK_TOGGLE_FULLSCREEN:
    case HK_TOGGLE_STRETCH: {
        // These need App.cpp-level dispatch (ToggleFullScreen etc.)
        HWND hRender = GetPluginWindow();
        if (hRender)
            PostMessage(hRender, WM_MW_HOTKEY_ACTION, (WPARAM)actionId, 0);
        return true;
    }
    case HK_LAUNCH_APP_1: LaunchOrFocusApp(0); return true;
    case HK_LAUNCH_APP_2: LaunchOrFocusApp(1); return true;
    case HK_LAUNCH_APP_3: LaunchOrFocusApp(2); return true;
    case HK_LAUNCH_APP_4: LaunchOrFocusApp(3); return true;
    default:
        return false;
    }
}

void Engine::LaunchOrFocusApp(int slot)
{
    if (slot < 0 || slot >= 4) return;
    if (m_szLaunchApp[slot][0] == L'\0') {
        AddNotification(L"No app configured for this slot");
        return;
    }

    // Extract exe filename from full path
    const wchar_t* exeName = wcsrchr(m_szLaunchApp[slot], L'\\');
    if (!exeName) exeName = wcsrchr(m_szLaunchApp[slot], L'/');
    exeName = exeName ? exeName + 1 : m_szLaunchApp[slot];

    // Search for a running process with matching exe name
    DWORD targetPID = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                    targetPID = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    // If running, find and focus its main window
    if (targetPID != 0) {
        struct FindData { DWORD pid; HWND hwnd; } fd = { targetPID, NULL };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* d = (FindData*)lp;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == d->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == NULL) {
                d->hwnd = h;
                return FALSE;
            }
            return TRUE;
        }, (LPARAM)&fd);

        if (fd.hwnd) {
            if (IsIconic(fd.hwnd))
                ShowWindow(fd.hwnd, SW_RESTORE);
            SetForegroundWindow(fd.hwnd);
            return;
        }
    }

    // Not running — launch it
    HINSTANCE hr = ShellExecuteW(NULL, L"open", m_szLaunchApp[slot],
                                  NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)hr > 32) {
        wchar_t msg[MAX_PATH + 32];
        swprintf(msg, MAX_PATH + 32, L"Launching %s", exeName);
        AddNotification(msg);
    } else {
        wchar_t msg[MAX_PATH + 64];
        swprintf(msg, MAX_PATH + 64, L"Could not launch %s", exeName);
        AddError(msg, 3.0f, ERR_MISC, false);
    }
}

bool Engine::LookupLocalHotkey(UINT vk, UINT modifiers)
{
    for (int i = 0; i < NUM_HOTKEYS; i++) {
        if (m_hotkeys[i].vk == vk && m_hotkeys[i].vk != 0 &&
            m_hotkeys[i].modifiers == modifiers &&
            m_hotkeys[i].scope == HKSCOPE_LOCAL)
        {
            return DispatchHotkeyAction(m_hotkeys[i].id);
        }
    }
    return false;
}

void Engine::LoadIdleTimerSettings()
{
    wchar_t* pIni = GetConfigIniFile();
    m_bIdleTimerEnabled = GetPrivateProfileIntW(L"IdleTimer", L"Enabled", 0, pIni) != 0;
    m_nIdleTimeoutMinutes = GetPrivateProfileIntW(L"IdleTimer", L"TimeoutMinutes", 5, pIni);
    if (m_nIdleTimeoutMinutes < 1) m_nIdleTimeoutMinutes = 1;
    if (m_nIdleTimeoutMinutes > 60) m_nIdleTimeoutMinutes = 60;
    m_nIdleAction = GetPrivateProfileIntW(L"IdleTimer", L"Action", 0, pIni);
    if (m_nIdleAction < 0 || m_nIdleAction > 1) m_nIdleAction = 0;
    m_bIdleAutoRestore = GetPrivateProfileIntW(L"IdleTimer", L"AutoRestore", 1, pIni) != 0;
}

void Engine::SaveIdleTimerSettings()
{
    wchar_t* pIni = GetConfigIniFile();
    wchar_t buf[64];

    WritePrivateProfileStringW(L"IdleTimer", L"Enabled", m_bIdleTimerEnabled ? L"1" : L"0", pIni);
    swprintf(buf, 64, L"%d", m_nIdleTimeoutMinutes);
    WritePrivateProfileStringW(L"IdleTimer", L"TimeoutMinutes", buf, pIni);
    swprintf(buf, 64, L"%d", m_nIdleAction);
    WritePrivateProfileStringW(L"IdleTimer", L"Action", buf, pIni);
    WritePrivateProfileStringW(L"IdleTimer", L"AutoRestore", m_bIdleAutoRestore ? L"1" : L"0", pIni);
}

std::wstring Engine::FormatHotkeyDisplay(UINT modifiers, UINT vk)
{
    if (vk == 0) return L"(none)";

    std::wstring result;
    if (modifiers & MOD_CONTROL) result += L"CTRL+";
    if (modifiers & MOD_ALT)     result += L"ALT+";
    if (modifiers & MOD_SHIFT)   result += L"SHIFT+";
    if (modifiers & MOD_WIN)     result += L"WIN+";

    // Map virtual key to name
    switch (vk) {
    case VK_RETURN:  result += L"ENTER"; break;
    case VK_ESCAPE:  result += L"ESC"; break;
    case VK_SPACE:   result += L"SPACE"; break;
    case VK_TAB:     result += L"TAB"; break;
    case VK_BACK:    result += L"BACKSPACE"; break;
    case VK_DELETE:  result += L"DELETE"; break;
    case VK_INSERT:  result += L"INSERT"; break;
    case VK_HOME:    result += L"HOME"; break;
    case VK_END:     result += L"END"; break;
    case VK_PRIOR:   result += L"PGUP"; break;
    case VK_NEXT:    result += L"PGDN"; break;
    case VK_UP:      result += L"UP"; break;
    case VK_DOWN:    result += L"DOWN"; break;
    case VK_LEFT:    result += L"LEFT"; break;
    case VK_RIGHT:   result += L"RIGHT"; break;
    default:
        if (vk >= VK_F1 && vk <= VK_F24) {
            wchar_t fbuf[8];
            swprintf(fbuf, 8, L"F%d", vk - VK_F1 + 1);
            result += fbuf;
        } else if (vk >= 'A' && vk <= 'Z') {
            result += (wchar_t)vk;
        } else if (vk >= '0' && vk <= '9') {
            result += (wchar_t)vk;
        } else {
            wchar_t kbuf[16];
            swprintf(kbuf, 16, L"0x%02X", vk);
            result += kbuf;
        }
        break;
    }
    return result;
}

} // namespace mdrop

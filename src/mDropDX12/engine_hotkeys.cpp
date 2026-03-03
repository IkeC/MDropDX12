// engine_hotkeys.cpp — Configurable hotkey load/save/register/dispatch
//
// Part of the MDropDX12 configurable hotkeys system.
// Supports local (render-window-focus) and global (system-wide) bindings.

#include "engine.h"
#include "utility.h"

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
    default:
        return false;
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

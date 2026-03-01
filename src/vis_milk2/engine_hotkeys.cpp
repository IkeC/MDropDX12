// engine_hotkeys.cpp — Global hotkey load/save/register/unregister
//
// Part of the MDropDX12 configurable global hotkeys system.

#include "engine.h"
#include "utility.h"

namespace mdrop {

void Engine::LoadHotkeySettings()
{
    wchar_t* pIni = GetConfigIniFile();

    // Initialize defaults
    m_hotkeys[0] = { HK_TOGGLE_FULLSCREEN, MOD_ALT, VK_RETURN, L"Toggle Fullscreen", L"ToggleFullscreen" };
    m_hotkeys[1] = { HK_TOGGLE_STRETCH,    MOD_ALT, 'S',       L"Toggle Stretch/Mirror", L"ToggleStretch" };

    // Read from INI
    for (int i = 0; i < HK_COUNT - 1; i++) {
        wchar_t modKey[128], vkKey[128];
        swprintf(modKey, 128, L"%s_Mod", m_hotkeys[i].szIniKey);
        swprintf(vkKey, 128, L"%s_VK", m_hotkeys[i].szIniKey);

        m_hotkeys[i].modifiers = (UINT)GetPrivateProfileIntW(L"Hotkeys", modKey, (int)m_hotkeys[i].modifiers, pIni);
        m_hotkeys[i].vk = (UINT)GetPrivateProfileIntW(L"Hotkeys", vkKey, (int)m_hotkeys[i].vk, pIni);
    }
}

void Engine::SaveHotkeySettings()
{
    wchar_t* pIni = GetConfigIniFile();
    wchar_t buf[64];

    for (int i = 0; i < HK_COUNT - 1; i++) {
        wchar_t modKey[128], vkKey[128];
        swprintf(modKey, 128, L"%s_Mod", m_hotkeys[i].szIniKey);
        swprintf(vkKey, 128, L"%s_VK", m_hotkeys[i].szIniKey);

        swprintf(buf, 64, L"%u", m_hotkeys[i].modifiers);
        WritePrivateProfileStringW(L"Hotkeys", modKey, buf, pIni);
        swprintf(buf, 64, L"%u", m_hotkeys[i].vk);
        WritePrivateProfileStringW(L"Hotkeys", vkKey, buf, pIni);
    }
}

void Engine::RegisterGlobalHotkeys(HWND hwnd)
{
    if (!hwnd) return;
    for (int i = 0; i < HK_COUNT - 1; i++) {
        if (m_hotkeys[i].vk != 0) {
            RegisterHotKey(hwnd, m_hotkeys[i].id, m_hotkeys[i].modifiers | MOD_NOREPEAT, m_hotkeys[i].vk);
        }
    }
}

void Engine::UnregisterGlobalHotkeys(HWND hwnd)
{
    if (!hwnd) return;
    for (int i = 0; i < HK_COUNT - 1; i++) {
        UnregisterHotKey(hwnd, m_hotkeys[i].id);
    }
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

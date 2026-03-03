#pragma once

#include <windows.h>

// Action IDs for configurable hotkey bindings
enum HotkeyAction : int {
    HK_NONE = 0,
    HK_TOGGLE_FULLSCREEN = 1,   // existing
    HK_TOGGLE_STRETCH,           // existing
    HK_OPEN_SETTINGS,            // additional shortcut (F8 stays hardcoded)
    HK_OPEN_DISPLAYS,            // default Ctrl+F8
    HK_OPEN_SONGINFO,            // default Shift+Ctrl+F8
    HK_OPEN_HOTKEYS,             // default Ctrl+F7
    HK_LAUNCH_APP_1,             // Launch/focus external app slot 1
    HK_LAUNCH_APP_2,             // Launch/focus external app slot 2
    HK_LAUNCH_APP_3,             // Launch/focus external app slot 3
    HK_LAUNCH_APP_4,             // Launch/focus external app slot 4
    HK_COUNT
};

static constexpr int NUM_HOTKEYS = HK_COUNT - 1;  // 6 bindings (IDs start at 1)

enum HotkeyScope : int {
    HKSCOPE_LOCAL  = 0,   // Only when render window has focus (WM_KEYDOWN lookup)
    HKSCOPE_GLOBAL = 1,   // System-wide via RegisterHotKey
};

// A single configurable hotkey binding
struct HotkeyBinding {
    int id;                    // HotkeyAction ID (for RegisterHotKey)
    UINT modifiers;            // MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN
    UINT vk;                   // Virtual key code (0 = unbound)
    HotkeyScope scope;
    wchar_t szAction[80];      // Action name (e.g., L"Toggle Fullscreen")
    wchar_t szIniKey[64];      // INI key prefix (e.g., L"ToggleFullscreen")
    UINT defaultMod;           // for Reset to Defaults
    UINT defaultVK;
    HotkeyScope defaultScope;
};

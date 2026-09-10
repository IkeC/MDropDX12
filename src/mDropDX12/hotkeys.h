#pragma once

#include <windows.h>
#include <string>

// Category groupings for the Hotkeys window and F1 help overlay
enum HotkeyCategory : int {
    HKCAT_NAVIGATION = 0,    // Preset navigation
    HKCAT_VISUAL,            // Visual adjustments
    HKCAT_MEDIA,             // Media control
    HKCAT_WINDOW,            // Window management
    HKCAT_TOOLS,             // Tool windows
    HKCAT_SHADER,            // Shader/effect control
    HKCAT_MISC,              // Miscellaneous
    HKCAT_SCRIPT,            // User script commands
    HKCAT_LAUNCH,            // Launch App slots
    HKCAT_COUNT
};

static const wchar_t* const kCategoryNames[HKCAT_COUNT] = {
    L"Navigation", L"Visual", L"Media", L"Window",
    L"Tools", L"Shader", L"Misc", L"Script", L"Launch"
};

// Action IDs for configurable hotkey bindings.
// IDs start at 1 (0 = none/unbound).
enum HotkeyAction : int {
    HK_NONE = 0,

    // ── Navigation ──
    HK_NEXT_PRESET = 1,          // Space — soft cut
    HK_PREV_PRESET,              // Backspace
    HK_HARD_CUT,                 // H/h — instant hard cut
    HK_RANDOM_MASHUP,            // A/a — random warp+comp+preset
    HK_LOCK_PRESET,              // `/~ — lock/unlock
    HK_TOGGLE_RANDOM,            // R/r — sequential/random order
    HK_OPEN_PRESET_LIST,         // L/l — open preset browser
    HK_SAVE_PRESET,              // S/s — save-as dialog
    HK_OPEN_MENU,                // M/m — toggle menu

    // ── Visual ──
    HK_OPACITY_UP,               // Shift+Up
    HK_OPACITY_DOWN,             // Shift+Down
    HK_OPACITY_25,               // (unbound by default)
    HK_OPACITY_50,               // (unbound by default)
    HK_OPACITY_75,               // (unbound by default)
    HK_OPACITY_100,              // (unbound by default)
    HK_WAVE_MODE_NEXT,           // W
    HK_WAVE_MODE_PREV,           // Shift+W
    HK_WAVE_ALPHA_DOWN,          // E
    HK_WAVE_ALPHA_UP,            // Shift+E
    HK_WAVE_SCALE_DOWN,          // J
    HK_WAVE_SCALE_UP,            // Shift+J
    HK_ZOOM_IN,                  // I
    HK_ZOOM_OUT,                 // Shift+I
    HK_WARP_AMOUNT_DOWN,         // O
    HK_WARP_AMOUNT_UP,           // Shift+O
    HK_WARP_SCALE_DOWN,          // U
    HK_WARP_SCALE_UP,            // Shift+U
    HK_ECHO_ALPHA_DOWN,          // P
    HK_ECHO_ALPHA_UP,            // Shift+P
    HK_ECHO_ZOOM_DOWN,           // Q
    HK_ECHO_ZOOM_UP,             // Shift+Q
    HK_ECHO_ORIENT,              // F
    HK_GAMMA_DOWN,               // G
    HK_GAMMA_UP,                 // Shift+G
    HK_PUSH_X_NEG,               // [
    HK_PUSH_X_POS,               // ]
    HK_PUSH_Y_NEG,               // {
    HK_PUSH_Y_POS,               // }
    HK_ROTATE_LEFT,              // <
    HK_ROTATE_RIGHT,             // >
    HK_BRIGHTNESS_DOWN,          // -
    HK_BRIGHTNESS_UP,            // +
    HK_HUE_FORWARD,              // Ctrl+H
    HK_HUE_BACKWARD,             // Ctrl+Shift+H

    // ── Media ──
    HK_MEDIA_PLAY_PAUSE,         // Down (UI_REGULAR)
    HK_MEDIA_STOP,               // Up (UI_REGULAR)
    HK_MEDIA_PREV_TRACK,         // Left
    HK_MEDIA_NEXT_TRACK,         // Right
    HK_MEDIA_REWIND,             // Ctrl+Left
    HK_MEDIA_FAST_FORWARD,       // Ctrl+Right

    // ── Window ──
    HK_TOGGLE_FULLSCREEN,        // (configurable, was F9/Alt+Enter in App)
    HK_TOGGLE_STRETCH,           // (configurable) — stretch or mirror depending on toggle
    HK_MIRROR_ONLY,              // (unbound) — always mirror
    HK_STRETCH_ONLY,             // (unbound) — always stretch
    HK_ALWAYS_ON_TOP,            // F7
    HK_TRANSPARENCY_MODE,        // F12
    HK_BLACK_MODE,               // Ctrl+F12
    HK_FPS_CYCLE,                // F3
    HK_SHOW_PRESET_INFO,         // F4
    HK_SHOW_FPS,                 // F5
    HK_SHOW_BATTERY,             // (unbound) — headset battery % beside the FPS
    HK_SHOW_RATING,              // F6
    HK_SHOW_SHADER_HELP,         // F9

    // ── Tools ──
    HK_OPEN_SETTINGS,            // F8
    HK_OPEN_DISPLAYS,            // Ctrl+F8
    HK_OPEN_SONGINFO,            // Shift+Ctrl+F8
    HK_OPEN_HOTKEYS,             // Ctrl+F7
    HK_OPEN_MIDI,                // (unbound by default)

    // ── Shader/Effects ──
    HK_INJECT_EFFECT_CYCLE,      // F11
    HK_HARDCUT_MODE_CYCLE,       // Shift+F11
    HK_QUALITY_DOWN,             // Ctrl+Q
    HK_QUALITY_UP,               // Ctrl+Shift+Q
    HK_SPOUT_TOGGLE,             // Ctrl+Z / F10
    HK_SPOUT_FIXED_SIZE,         // Shift+F10 / Ctrl+Shift+Z
    HK_SCREENSHOT,               // Ctrl+X
    HK_SHADER_LOCK_CYCLE,        // D/d — comp/warp/all/none
    HK_SONG_TITLE,               // T/t — launch song title anim
    HK_KILL_SPRITES,             // Ctrl+K
    HK_KILL_SUPERTEXTS,          // Ctrl+T
    HK_AUTO_PRESET_CHANGE,       // Ctrl+A
    HK_SCRAMBLE_WARP,            // ! — randomize warp shader
    HK_SCRAMBLE_COMP,            // @ — randomize comp shader
    HK_QUICKSAVE,                // Ctrl+S
    HK_SCROLL_LOCK,              // Scroll Lock — lock + toggle playlist
    HK_RELOAD_MESSAGES,          // * — re-read custom messages

    // ── Misc ──
    HK_DEBUG_INFO,               // N/n
    HK_SPRITE_MODE,              // K/k — toggle sprite/message mode
    HK_OPEN_BOARD,               // Ctrl+F9 — open Button Board window
    HK_OPEN_PRESETS,             // (unbound) — open Presets window
    HK_OPEN_SPRITES,             // (unbound) — open Sprites window
    HK_OPEN_MESSAGES,            // (unbound) — open Messages window
    HK_OPEN_SHADER_IMPORT,       // (unbound) — open Shader Import window
    HK_OPEN_VIDEO_FX,            // (unbound) — open Video Effects window
    HK_OPEN_VFX_PROFILES,        // (unbound) — open VFX Profile Picker window
    HK_OPEN_CUSTOM_SHADERS,      // (unbound) — open Custom Shaders window
    HK_TOGGLE_SHADER_OVERRIDES,  // (unbound) — master enable for shader overrides
    HK_OPEN_WORKSPACE_LAYOUT,    // (unbound) — open Workspace Layout window
    HK_APPLY_WORKSPACE_LAYOUT,   // (unbound) — apply saved workspace layout
    HK_OPEN_TEXT_ANIM,            // (unbound) — open Text Animations window
    HK_OPEN_REMOTE,               // (unbound) — open/launch Milkwave Remote
    HK_OPEN_VISUAL,               // (unbound) — open Visual window
    HK_OPEN_COLORS,               // (unbound) — open Colors window
    HK_OPEN_CONTROLLER,           // (unbound) — open Controller window
    HK_OPEN_ANNOTATIONS,          // (unbound) — open Annotations window
    HK_OPEN_SCRIPT,               // (unbound) — open Script window
    HK_POLL_TRACK_INFO,           // Middle Mouse — force track info poll
    HK_MIRROR_WATERMARK,          // (unbound) — toggle mirror watermark mode
    HK_WATERMARK,                 // (unbound) — toggle clickthrough/watermark mode
    HK_TOGGLE_MESSAGES,           // (unbound) — toggle messages.ini messages on/off
    HK_MIRROR_INDEPENDENT,        // (unbound) — toggle independent per-display mirror render
    HK_OPEN_PRESET_EDITOR,        // (unbound) — open Preset Editor on the running preset
    HK_MIXER_GROUP1_UP,           // (unbound) — audio mixer: group 1 louder
    HK_MIXER_GROUP1_DOWN,         // (unbound) — audio mixer: group 1 quieter
    HK_MIXER_GROUP1_MUTE,         // (unbound) — audio mixer: group 1 mute toggle
    HK_MIXER_SONAR_RESTART,       // (unbound) — restart the SteelSeries process tree
    HK_OPEN_AUDIO_MIXER,          // (unbound) — open Audio Mixer window
    HK_SAVE_DISPLAY_SNAPSHOT,     // (unbound) — bank every screen's current preset
    HK_LOAD_DEFAULT_PROFILE,      // (unbound) — load the default display profile
    HK_NEXT_DISPLAY_PROFILE,      // (unbound) — step to the next display profile
    HK_PREV_DISPLAY_PROFILE,      // (unbound) — step to the previous one
    HK_SETTINGS_OVERLAY,          // (unbound) — the in-frame settings screen

    HK_COUNT  // = number of built-in actions + 1 (HK_NONE)
};

static constexpr int NUM_HOTKEYS = HK_COUNT - 1;  // built-in actions (excludes HK_NONE)

enum HotkeyScope : int {
    HKSCOPE_LOCAL  = 0,   // Only when render window has focus (WM_KEYDOWN lookup)
    HKSCOPE_GLOBAL = 1,   // System-wide via RegisterHotKey
};

// A single configurable hotkey binding.
// Each action supports both a local binding (vk/modifiers) and a separate
// global binding (globalVK/globalMod). Either can be unbound (vk=0).
struct HotkeyBinding {
    int id;                    // HotkeyAction ID (for RegisterHotKey)
    UINT modifiers;            // Local binding: MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN
    UINT vk;                   // Local binding: virtual key code (0 = unbound)
    HotkeyScope scope;         // (legacy, unused for built-in — kept for INI compat)
    HotkeyCategory category;   // Grouping for UI and F1 help
    wchar_t szAction[80];      // Action name (e.g., L"Toggle Fullscreen")
    wchar_t szIniKey[64];      // INI key prefix (e.g., L"ToggleFullscreen")
    UINT defaultMod;           // for Reset to Defaults (local)
    UINT defaultVK;
    HotkeyScope defaultScope;  // (legacy)
    UINT globalMod;            // Global binding: MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN
    UINT globalVK;             // Global binding: virtual key code (0 = unbound)
    UINT defaultGlobalMod;     // for Reset to Defaults (global)
    UINT defaultGlobalVK;
};

// Dynamic user-added hotkeys (Script Commands and Launch Apps)
static constexpr int USER_HOTKEY_ID_BASE = 1000;

enum UserHotkeyType : int {
    USER_HK_SCRIPT = 0,   // IPC command string (e.g. "NEXT", "OPACITY=0.5")
    USER_HK_LAUNCH = 1,   // Launch/focus application path
};

struct UserHotkey {
    int            id;          // unique ID for RegisterHotKey (>= USER_HOTKEY_ID_BASE)
    UserHotkeyType type;
    UINT           modifiers;   // MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN
    UINT           vk;          // virtual key code (0 = unbound)
    HotkeyScope    scope;
    std::wstring   label;       // display name (e.g. "My Script", "Launch OBS")
    std::wstring   command;     // IPC command (Script) or full exe path (Launch)
};

// ── Live registration state ─────────────────────────────────────────────
//
// A global binding is a REQUEST, not a fact. RegisterHotKey is
// first-come-first-served across the whole machine, so the combination in
// HotkeyBinding is what the user asked for and this is what Windows granted.
// Keeping them apart is the whole point: the Hotkeys window used to show the
// request and call it the answer, so a combination another application owned
// looked configured, did nothing, and explained nothing (forgejo#72).
//
// Rebuilt from scratch on every registration pass, so a binding that comes
// back after the other application releases it reports held again. Nothing
// here is persisted -- it describes this process at this moment.
struct GlobalHotkeyState {
    int   id;          // HotkeyAction id, or a UserHotkey id (>= USER_HOTKEY_ID_BASE)
    UINT  modifiers;   // what was actually offered to RegisterHotKey (sans MOD_NOREPEAT)
    UINT  vk;
    bool  held;        // RegisterHotKey returned TRUE
    DWORD lastError;   // GetLastError() when it did not; 0 when it did
    bool  probed;      // asked and released again -- see GHKMODE_PROBE
};

// What a global registration pass is allowed to do in this process.
//
// ONE value, read both by the pass that acts on it and by the diagnostics
// that report it. Splitting those apart is how a report drifts from the
// behaviour it claims to describe -- which is exactly what happened here:
// registration was skipped on a test instance while DIAG_HOTKEY_BIND went on
// answering held=1, because it inferred "held" from the ABSENCE of a recorded
// failure and a pass that never ran records no failures (#173).
enum GlobalHotkeyMode {
    // Claim the combinations and keep them. The only mode that ends with this
    // process owning a key.
    GHKMODE_REGISTER,

    // Claim, record what Windows answered, and release again immediately --
    // the same try-and-let-go the availability scan uses.
    //
    // This is a test instance. RegisterHotKey is machine-wide and
    // first-come-first-served, so a harness copy that KEPT what it claimed
    // would take the user's keys away from the visualizer he is looking at
    // and act on them itself. Refusing to register at all was the first
    // answer to that and it cost more than it saved: collision handling
    // became untestable, and the diagnostics reported a table nothing had
    // filled in.
    //
    // Probing gives back both. The collision against whatever already owns
    // the combination -- another application, or the user's own running copy
    // -- is real and is detected, while nothing is held for longer than the
    // call takes, so the harness still cannot swallow a keystroke.
    GHKMODE_PROBE,

    // A -child instance registers nothing system-wide and does not ask
    // either. It is driven over the pipe and takes no direct input.
    GHKMODE_NONE
};

// The wire spelling, for the diagnostic replies. Stable: tests match on it.
inline const wchar_t* GlobalHotkeyModeName(GlobalHotkeyMode m)
{
    return m == GHKMODE_REGISTER ? L"register"
         : m == GHKMODE_PROBE    ? L"probe"
                                 : L"none";
}

// ── Availability scan ───────────────────────────────────────────────────
//
// Windows offers no way to ask what other applications hold, so the only
// honest test of a combination is to TRY it: register, look at the answer,
// release it again. That is the same standard a real binding is held to,
// which is what makes the result worth putting in front of somebody.
//
// "Ours" is a third answer and not a flavour of "taken". A second
// registration of a combination fails even inside one process, so a naive
// probe reports our own bindings as owned by somebody else -- precisely the
// confusion the window exists to remove. They are identified from the binding
// table instead of being probed at all.
enum HotkeyAvailability : int {
    HKAVAIL_FREE  = 0,   // registered cleanly, and was released again
    HKAVAIL_TAKEN = 1,   // another process holds it
    HKAVAIL_OURS  = 2,   // one of this app's own bindings
};

struct HotkeyScanRow {
    UINT modifiers;
    UINT vk;
    int  status;         // HotkeyAvailability
    int  ownerId;        // when OURS: the action / user hotkey id, else -1
    DWORD lastError;     // when TAKEN: why the registration was refused
};

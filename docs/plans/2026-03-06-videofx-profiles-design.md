# Video FX Profiles Design

## Overview
JSON profile system for video effects: save/load from the Video Effects window, separate profile picker window for quick switching, and auto-load on startup mirroring the existing startup preset pattern.

## Storage
- Profiles saved as `.json` files in `resources/videofx/` (created on first save if missing)
- Uses existing `json_utils.h` (`JsonWriter` to serialize, `JsonParse` to deserialize)

## JSON Schema
```json
{
  "name": "My Profile",
  "version": 1,
  "transform": {
    "posX": 0, "posY": 0, "scale": 1.0, "rotation": 0,
    "mirrorH": false, "mirrorV": false
  },
  "color": {
    "tintR": 1, "tintG": 1, "tintB": 1, "brightness": 0,
    "contrast": 1, "saturation": 1, "hueShift": 0, "invert": false
  },
  "effects": {
    "pixelation": 0, "chromatic": 0, "edgeDetect": false
  },
  "blendMode": 0,
  "audio": {
    "posX":       { "source": 0, "intensity": 0.5 },
    "posY":       { "source": 0, "intensity": 0.5 },
    "scale":      { "source": 0, "intensity": 0.5 },
    "rotation":   { "source": 0, "intensity": 0.5 },
    "brightness": { "source": 0, "intensity": 0.5 },
    "saturation": { "source": 0, "intensity": 0.5 },
    "chromatic":  { "source": 0, "intensity": 0.5 }
  }
}
```

## Startup Pattern (mirrors preset startup)
Three new INI settings in `[VideoFX]`:
- `bEnableVFXStartup` - load a profile on startup (default: false)
- `szVFXStartup` - path to the startup profile .json
- `bEnableVFXStartupSavingOnClose` - save current state as startup profile on close (default: true)

## Two Windows

### Video Effects Window (existing, modified)
Add Save/Load buttons at the bottom or top of the window:
- **[Save Profile...]** button - prompts for name, saves current `m_videoFX` to `.json`
- **[Load Profile...]** button - opens the VFX Profile Picker window

### VFX Profile Picker Window (new ToolWindow)
```
+-- VFX Profiles ----------------------+
| +----------------------------------+ |
| | (None)                           | |
| | Webcam Center                    | |
| | PiP Bottom Right                 | |
| | Audio Reactive Pulse             | |
| +----------------------------------+ |
| [Save As...] [Delete]               |
| [ ] Load on startup  [ ] Save on close|
+--------------------------------------+
```
- Listbox with single-click instant profile loading
- "(None)" entry resets to defaults
- Save As prompts for name (pre-fills selected profile name)
- Delete removes file, selects "(None)"
- Startup checkboxes persist to INI
- Accessible from Settings > Tools hotkey entry and from Effects window button
- Both windows can be open simultaneously; profile switch refreshes effects window if open

## Engine Methods
- `SaveVideoFXProfile(const wchar_t* path)` - serialize `m_videoFX` to JSON
- `LoadVideoFXProfile(const wchar_t* path)` - parse JSON into `m_videoFX`
- `GetVideoFXProfileDir(wchar_t* out, size_t len)` - returns `m_szMilkdrop2Path + "videofx\\"`
- `OpenVFXProfileWindow()` / `CloseVFXProfileWindow()`

## Engine Members
- `wchar_t m_szCurrentVFXProfile[MAX_PATH]` - currently loaded profile path (empty = none)
- `bool m_bEnableVFXStartup` - load profile on startup
- `wchar_t m_szVFXStartup[MAX_PATH]` - startup profile path
- `bool m_bEnableVFXStartupSavingOnClose` - auto-save on close
- `VFXProfileWindow* m_pVFXProfileWindow` - profile picker window pointer

## Startup Flow
In engine init (after `LoadSpoutInputSettings()`):
```cpp
if (m_bEnableVFXStartup && wcslen(m_szVFXStartup) > 0)
    LoadVideoFXProfile(m_szVFXStartup);
```

On close (in cleanup/save):
```cpp
if (m_bEnableVFXStartupSavingOnClose && wcslen(m_szCurrentVFXProfile) > 0)
    SaveVideoFXProfile(m_szCurrentVFXProfile);
```

## Files to Modify
| File | Changes |
|------|---------|
| engine.h | Startup members, profile methods, window pointer |
| engine_helpers.h | ~10 new control IDs (listbox, buttons, checkboxes, save/load on effects window) |
| tool_window.h | VFXProfileWindow class declaration |
| engine_spout_input.cpp | SaveVideoFXProfile, LoadVideoFXProfile, GetVideoFXProfileDir, INI load/save for startup settings |
| engine_video_effects_ui.cpp | Add Save/Load profile buttons, open profile picker handler |
| engine_vfx_profiles_ui.cpp | NEW: VFXProfileWindow implementation |
| engine.vcxproj | Add new .cpp |
| hotkeys.h | HK_OPEN_VFX_PROFILES enum |
| engine_hotkeys.cpp | HK_DEF + dispatch for profile window |

// display_presets.cpp -- which preset a non-primary display starts on, and
// which directory it cycles through.
//
// These lived in child_instances.cpp, because when they were written the only
// way a display could hold a preset of its own was a second process to hold it
// in. #186 moved that in process -- a display is now a mirror surface with its
// own simulation -- and phase 6 deleted the child subsystem, so the resolution
// rules outlived the mechanism they were written for and moved here.
//
// They answer for the RENDER thread now, not a worker: the surface that owns
// the display asks at the point it adopts a preset.

#include "engine.h"
#include "utility.h"
#include "format_to.h"

#include <algorithm>
#include <vector>

// Engine lives in namespace mdrop (engine.h), so its members must be
// defined there too -- every other engine_*.cpp does the same.
namespace mdrop {

// One random preset from a directory, top level only -- the same set the
// display's own surface will cycle through, so a startup pick and a later
// cycle are choosing from the same list.
//
// Modelled on PickRandomTextureFile in dxcontext.cpp, and deliberately a plain
// directory walk rather than a call into the engine's preset list: m_presets
// belongs to the render thread and describes the MAIN window's directory,
// while this answers for whatever directory the display was given.
template <size_t N>
static bool PickRandomPresetFile(const wchar_t* szDir, wchar_t (&szOut)[N]) {
    if (!szDir || !szDir[0]) return false;
    std::wstring base = szDir;
    if (base.back() != L'\\' && base.back() != L'/') base += L'\\';

    // One pass with an explicit extension test, rather than a mask per
    // extension. Three masks would need dedup afterwards -- FindFirstFile
    // matches 8.3 short names as well as long ones, so which files a mask
    // like "*.milk" also catches depends on whether short-name generation is
    // on for the volume. Deciding that per machine is not worth it when the
    // suffix can simply be compared.
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const wchar_t* dot = wcsrchr(fd.cFileName, L'.');
        if (!dot) continue;
        if (_wcsicmp(dot, L".milk")  == 0 ||
            _wcsicmp(dot, L".milk2") == 0 ||
            _wcsicmp(dot, L".milk3") == 0)
            files.push_back(base + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (files.empty()) return false;
    CopyTo(szOut, files[rand() % files.size()].c_str());
    return true;
}

// One random IMMEDIATE subfolder of szDir, or false if it has none.
//
// Immediate rather than recursive on purpose: <Random> for a directory exists
// so several displays land on DIFFERENT folders, and a recursive walk of a
// library organised by artist would keep picking leaf folders holding two
// presets each. One level down is the level a person actually organises at.
template <size_t N>
static bool PickRandomSubdir(const wchar_t* szDir, wchar_t (&szOut)[N]) {
    if (!szDir || !szDir[0]) return false;
    std::wstring base = szDir;
    if (base.back() != L'\\' && base.back() != L'/') base += L'\\';

    std::vector<std::wstring> dirs;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        dirs.push_back(base + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (dirs.empty()) return false;
    CopyTo(szOut, dirs[rand() % dirs.size()].c_str());
    return true;
}

// ─── Preset mode sentinels → real values ──────────────────────────────────────
//
// The two per-display preset fields each hold a literal path or one of the
// kDispMode* sentinels (display_output.h). These are the ONLY two functions
// that know that; everything downstream -- the surface, the profile writer,
// GET_CHILDREN's reply -- deals in resolved paths, so a sentinel can never be
// taken for a filename.
//
// Resolution happens when the display STARTS holding its own preset, not when
// the field is set, which is the whole point of a mode: <Current> means the
// preset the main window is showing when this display starts, not the one it
// happened to be showing when the combo was last touched.

static bool IsDispMode(const wchar_t* s, const wchar_t* mode) {
    return s && _wcsicmp(s, mode) == 0;
}

std::wstring Engine::ResolveDisplayPresetDir(const DisplayOutputConfig& cfg) const {
    const wchar_t* v = cfg.szPresetDir;

    // Empty is the pre-v3 form of <Default>, and always meant "inherit".
    if (!v[0] || IsDispMode(v, kDispModeDefault))
        return m_szPresetDir;

    if (IsDispMode(v, kDispModeRandom)) {
        wchar_t pick[MAX_PATH] = {};
        if (PickRandomSubdir(m_szPresetDir, pick))
            return pick;
        // A library with no subfolders is not an error, it is a library with
        // nothing to choose between. Fall back rather than leave the display
        // with no directory at all.
        DLOG_INFO("display preset dir <Random>: %ls has no subfolders; using it "
                  "directly", m_szPresetDir);
        return m_szPresetDir;
    }

    if (IsDispMode(v, kDispModeCurrent)) {
        // m_szCurrentPresetFile is documented "w/o path" but is a full path on
        // the paths that matter here (LoadPreset stores what it was given), so
        // it is tested rather than trusted -- with recursive preset lists on,
        // the folder it names can be a subfolder of m_szPresetDir, which is
        // exactly what makes <Current> different from <Default>.
        return CurrentPresetDir();
    }

    return v;   // a literal path
}

std::wstring Engine::ResolveDisplayStartupPreset(const DisplayOutputConfig& cfg,
                                                 const wchar_t* dir) const {
    const wchar_t* v = cfg.szStartupPreset;

    auto randomFrom = [dir]() -> std::wstring {
        wchar_t pick[MAX_PATH] = {};
        if (dir && PickRandomPresetFile(dir, pick)) return pick;
        return std::wstring();   // caller keeps whatever the child loaded
    };

    // Empty is the pre-v3 form, and it meant a random pick -- NOT <Default>.
    // Mapping it anywhere else would silently change what every existing
    // settings.ini and display profile does.
    if (!v[0] || IsDispMode(v, kDispModeRandom))
        return randomFrom();

    if (IsDispMode(v, kDispModeDefault)) {
        // The global startup preset, on the same terms the main window uses it:
        // the checkbox has to be on and the path non-empty, else it is random.
        if (m_bEnablePresetStartup && m_szPresetStartup[0])
            return m_szPresetStartup;
        return randomFrom();
    }

    if (IsDispMode(v, kDispModeCurrent)) {
        if (!m_szCurrentPresetFile[0])
            return randomFrom();
        // A bare filename is joined to the directory this display will use, so
        // <Current> still names a file that can actually be opened.
        if (wcschr(m_szCurrentPresetFile, L'\\') ||
            wcschr(m_szCurrentPresetFile, L'/'))
            return m_szCurrentPresetFile;
        std::wstring base = (dir && dir[0]) ? dir : m_szPresetDir;
        if (!base.empty() && base.back() != L'\\' && base.back() != L'/')
            base += L'\\';
        return base + m_szCurrentPresetFile;
    }

    return v;   // a literal path
}

// \\.\DISPLAY3 -> 3, and 0 for anything that is not a DISPLAYn device.
int Engine::DisplayNumberFromDevice(const wchar_t* deviceName) {
    if (!deviceName) return 0;
    const wchar_t* d = deviceName;
    while (*d && (*d < L'0' || *d > L'9')) d++;
    return *d ? _wtoi(d) : 0;
}

// What a display is showing right now (#186 phase 6).
//
// This is the replacement for GetChildInfo, and the difference is worth
// stating: that answer came from polling a second process over its pipe once a
// second, so it was up to a second stale and could say "starting" or "failed".
// A display is a mirror surface now, so the answer is the surface's, and the
// only states left are the ones a surface can be in.
Engine::DisplayPresetInfo Engine::DisplayPresetStatus(const wchar_t* device) const {
    DisplayPresetInfo info;
    MirrorSurface* s = MirrorSurfaceFor(device);
    if (!s) return info;
    info.haveSurface = true;
    info.ready = s->imageReady.load();
    std::lock_guard<std::mutex> lk(s->requestMutex);
    info.preset = s->shownPreset;
    return info;
}

void Engine::SetUserPresetLock(bool bLocked) {
    // Early out on no change, because HK_SCROLL_LOCK assigns the key state on
    // every press and the auto-lock-on-silence timer re-evaluates constantly.
    // It used to queue a pipe message to every child per call, which is what
    // made the guard worth having; the guard is kept because the property it
    // states -- this is called far more often than it changes -- is still true.
    if (m_bPresetLockedByUser == bLocked) return;
    m_bPresetLockedByUser = bLocked;
}

} // namespace mdrop

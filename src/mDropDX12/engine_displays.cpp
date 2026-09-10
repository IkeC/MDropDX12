// engine_displays.cpp — Display output management (monitor mirrors + Spout senders)
//
// Part of the MDropDX12 unified display output system.
// Manages enumeration, INI persistence, init/destroy, and per-frame send.

#include "engine.h"
#include "tool_window.h"
#include "engine_helpers.h"
#include "json_utils.h"
#include "profile_paths.h"
#include "kv_store.h"
#include "utility.h"
#include "format_to.h"
#include <algorithm>
#include <dxgi1_4.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM for mirror mouse input
#include <process.h>
#include <mutex>
#include <chrono>
#include "config_store.h"
#include "pipe_server.h"

namespace mdrop {

// Used by InitDisplayOutput and ApplyMirrorWindowStyles (defined later)
static void ComputeMirrorLayout(const DisplayOutputConfig& cfg,
                                int& outX, int& outY, int& outW, int& outH);

// Aspect-preserving long-edge cap, 16-aligned. Shared by the mirror swap-chain
// cap and the feedback-canvas limit: they want identical geometry rules, and
// having two copies would let them drift.
void CapDimToLongEdge(int& w, int& h, int maxDim)
{
    if (maxDim > 0 && (w > maxDim || h > maxDim)) {
        float sc = (float)maxDim / (float)((w > h) ? w : h);
        w = max(1, (int)(w * sc + 0.5f));
        h = max(1, (int)(h * sc + 0.5f));
    }
    w = ((w + 15) / 16) * 16;
    h = ((h + 15) / 16) * 16;
}

// Never allocate a mirror SC larger than 1920 on a side. 2160x3840 primary
// plus matching mirror SCs TDRs (idle-timer FS at 12:22).
static void CapMirrorSwapChainDim(int& w, int& h)
{
    CapDimToLongEdge(w, h, 1920);
}

// Copy-mode: primary BB size (capped) -- the panel shows the primary's image,
// so the primary's shape is the right shape.
// Opposite-aspect independent, or a display holding its OWN PRESET: the
// monitor's own aspect, long side <= 1920.
static void MirrorSwapChainSize(const DisplayOutput& out, int primW, int primH,
                                int& outW, int& outH)
{
    int x = 0, y = 0, layW = 0, layH = 0;
    ComputeMirrorLayout(out.config, x, y, layW, layH);
    const bool primPortrait = primH > primW;
    const bool panelPortrait = layH > layW;

    // bOwnProcess means "this display holds its own preset" (#186 phase 4), and
    // it is NOT paired with bIndependentRender -- the two were mutually
    // exclusive when one meant "spawn a child". So an own-preset display fell
    // into the else below and was given the PRIMARY's dimensions.
    //
    // Measured, with a portrait primary and two landscape own-preset panels:
    // the swap chain came out 1088x1920 (portrait) while the surface rendered
    // 1920x1088, so BlitOrientOutputToMirror letterboxed by
    // ny = dstAr/srcAr = 0.5667/1.7647 = 0.3211 -- the reported "too wide"
    // band, content in 32% of the panel height. A following mirror on the same
    // run got 1920x1088 and ny = 1.0.
    //
    // Orientation is not part of the own-preset test: that test asks whether
    // the primary's image can be stretched onto this panel, and for a display
    // rendering a DIFFERENT PRESET the answer is no at any orientation.
    const bool ownPreset = out.config.bOwnProcess;
    const bool wantsOwnShape =
        ownPreset || (out.config.bIndependentRender && panelPortrait != primPortrait);

    if (wantsOwnShape && layW > 0 && layH > 0) {
        outW = layW;
        outH = layH;
    } else {
        outW = primW;
        outH = primH;
    }
    CapMirrorSwapChainDim(outW, outH);
}

// ─── Mirror Window Proc ──────────────────────────────────────────────────────
// GWLP_USERDATA holds the primary render HWND (set at CreateWindow).
// Mirrors use WS_EX_NOACTIVATE so they never become the key window; without
// explicit focus hand-off, clicks on a mirror leave local hotkeys dead.

static void RequestPrimaryFocus(HWND hMirror)
{
    HWND primary = (HWND)GetWindowLongPtrW(hMirror, GWLP_USERDATA);
    if (!primary || !IsWindow(primary))
        return;
    // Cross-thread: primary is owned by the UI thread — never SetFocus here.
    PostMessageW(primary, WM_MW_FOCUS_PRIMARY, 0, 0);
}

extern Engine g_engine;   // declared in App.cpp

// Map a click on a mirror panel into the shadertoy iMouse space.
//
// iMouse lives in PRIMARY-target pixels, and the orient pass rescales it by
// texsize when it renders the mirror — so feeding this one value keeps a single
// source of truth and makes the centre of a mirror land on the centre of the
// primary's view, with no second code path. The panel blit is a straight
// stretch of the orient render, so normalising across the client rect is the
// whole mapping; there are no letterbox bars to compensate for.
// Returns false if the sizes are not usable yet.
static bool MirrorClientToShadertoyMouse(HWND hMirror, POINT pt, float& outX, float& outY)
{
    RECT rc;
    if (!GetClientRect(hMirror, &rc))
        return false;
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;
    if (cw <= 1 || ch <= 1)
        return false;

    int targetW = 1, targetH = 1;
    if (g_engine.m_lpDX && g_engine.m_lpDX->m_backbuffer_width > 0 &&
        g_engine.m_lpDX->m_backbuffer_height > 0) {
        targetW = g_engine.m_lpDX->m_backbuffer_width;
        targetH = g_engine.m_lpDX->m_backbuffer_height;
    } else if (g_engine.m_WindowWidth > 0 && g_engine.m_WindowHeight > 0) {
        targetW = g_engine.m_WindowWidth;
        targetH = g_engine.m_WindowHeight;
    }
    if (targetW <= 1 || targetH <= 1)
        return false;

    float fx = (float)pt.x / (float)(cw - 1);
    float fy = (float)pt.y / (float)(ch - 1);
    fx = fx < 0.f ? 0.f : (fx > 1.f ? 1.f : fx);
    fy = fy < 0.f ? 0.f : (fy > 1.f ? 1.f : fy);

    outX = fx * (float)(targetW - 1);
    outY = (1.0f - fy) * (float)(targetH - 1);  // iMouse origin is bottom-left
    return true;
}

static LRESULT CALLBACK MirrorWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_CLOSE:
        return 0; // Prevent user from closing the mirror window
    case WM_ERASEBKGND:
        return 1; // DX12 handles rendering; skip GDI erase

    case WM_MOUSEACTIVATE:
        // Do not activate the mirror; ask primary to take keyboard focus.
        RequestPrimaryFocus(hWnd);
        return MA_NOACTIVATE;

    case WM_LBUTTONDOWN:
    {
        // Drive the shadertoy mouse from the panel so mouse-steered presets
        // respond to a drag on a mirror. Shared on purpose (Shane, 2026-08-23):
        // one iMouse, so the primary and every mirror swing together.
        // Focus still goes to the primary and MA_NOACTIVATE still stands, so
        // clicking a mirror steers without stealing activation or keystrokes.
        RequestPrimaryFocus(hWnd);
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        float mx, my;
        if (MirrorClientToShadertoyMouse(hWnd, pt, mx, my)) {
            g_engine.m_stMouseX = mx;
            g_engine.m_stMouseY = my;
            g_engine.m_stClickX = mx;
            g_engine.m_stClickY = my;
            g_engine.m_stMouseDown = true;
            g_engine.m_stMouseJustClicked = true;
            SetCapture(hWnd);   // keep tracking if the drag leaves the panel
        }
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        // Position updates only while held — same drag-only rule as the primary.
        if (g_engine.m_stMouseDown && GetCapture() == hWnd) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            float mx, my;
            if (MirrorClientToShadertoyMouse(hWnd, pt, mx, my)) {
                g_engine.m_stMouseX = mx;
                g_engine.m_stMouseY = my;
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (GetCapture() == hWnd)
            ReleaseCapture();
        g_engine.m_stMouseDown = false;
        return 0;

    case WM_CAPTURECHANGED:
        // Capture yanked away mid-drag (alt-tab, another window): end the drag
        // rather than leaving iMouse.z stuck down forever.
        g_engine.m_stMouseDown = false;
        return 0;

    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        RequestPrimaryFocus(hWnd);
        return 0;

    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    {
        // If keys ever land here, forward to primary so hotkeys still work.
        HWND primary = (HWND)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
        if (primary && IsWindow(primary))
            PostMessageW(primary, uMsg, wParam, lParam);
        return 0;
    }
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// Helper: find a monitor by device name and return its current rect
struct FindMonitorCtx {
    const wchar_t* szDeviceName;
    RECT rcResult;
    bool bFound;
};

static BOOL CALLBACK FindMonitorCB(HMONITOR hMon, HDC, LPRECT, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindMonitorCtx*>(lp);
    MONITORINFOEXW mi = { sizeof(MONITORINFOEXW) };
    if (GetMonitorInfoW(hMon, &mi)) {
        if (wcscmp(mi.szDevice, ctx->szDeviceName) == 0) {
            ctx->rcResult = mi.rcMonitor;
            ctx->bFound = true;
            return FALSE; // stop enumeration
        }
    }
    return TRUE;
}

// ─── Monitor Enumeration ──────────────────────────────────────────────────────

struct EnumMonitorCtx {
    Engine* engine;
    HMONITOR hRenderMonitor; // the monitor hosting the render window (for bSkippedSameMonitor)
};

static BOOL CALLBACK EnumMonitorCB(HMONITOR hMon, HDC, LPRECT, LPARAM lp)
{
    auto* ctx = reinterpret_cast<EnumMonitorCtx*>(lp);

    MONITORINFOEXW mi = { sizeof(MONITORINFOEXW) };
    if (!GetMonitorInfoW(hMon, &mi))
        return TRUE;

    DisplayOutput out;
    out.config.type = DisplayOutputType::Monitor;
    out.config.bEnabled = false;
    out.config.bFullscreen = true;
    out.config.rcMonitor = mi.rcMonitor;
    wcsncpy_s(out.config.szDeviceName, mi.szDevice, _TRUNCATE);

    // Mark the render window's monitor as skipped (dynamic check in SendToDisplayOutputs
    // will update this at runtime when the render window moves between monitors)
    if (hMon == ctx->hRenderMonitor)
        out.bSkippedSameMonitor = true;

    // Get friendly display name from DISPLAY_DEVICEW
    DISPLAY_DEVICEW dd = { sizeof(dd) };
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0))
        wcsncpy_s(out.config.szName, dd.DeviceString, _TRUNCATE);
    else
        wcsncpy_s(out.config.szName, mi.szDevice, _TRUNCATE);

    // Append device name for disambiguation
    wchar_t label[128];
    swprintf(label, 128, L"%s (%s)", out.config.szName, mi.szDevice);
    wcsncpy_s(out.config.szName, label, _TRUNCATE);

    ctx->engine->m_displayOutputs.push_back(std::move(out));
    return TRUE;
}

void Engine::EnumerateDisplayOutputs()
{
    // m_displayOutputsMutex, not just render-thread confinement: this function
    // erases every monitor entry and repopulates it in several steps (erase,
    // re-enumerate, restore saved config), and ChildConfigureNow reads the
    // vector from the child-worker thread. Without the lock a read landing
    // mid-rebuild could see a reallocated buffer, or a monitor entry that
    // exists but has not yet had its saved settings restored. #27.
    std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);

    // Save existing monitor configs so we can preserve enabled state after re-enumeration
    // Mirrors are session-lived: carry the live MonitorMirrorState across
    // re-enumeration instead of letting the erase below destroy it. A plain
    // unique_ptr reset never runs DestroyMonitorMirror, so every enumeration
    // used to leak that mirror's reserved RTV block (16 slots total → mirrors
    // stopped coming back after ~4 idle "Mirror all" cycles) and its HWND.
    //
    // The whole config is carried, not a hand-picked list of fields. It used to
    // name five -- bEnabled, bFullscreen, nOpacity, bClickThrough,
    // bIndependentRender -- and none of the five child fields added later, so a
    // hotplug, a resolution change or a rearrangement silently reset
    // bOwnProcess, szPresetDir, szStartupPreset, fTimeBetweenPresets and
    // bSequentialOrder for every display (forgejo#35). Nothing killed the child
    // it had just disowned, so a full second instance kept rendering while
    // GET_CHILDREN -- which filters on bOwnProcess -- stopped listing it, and
    // no record was left associating it with a display. Copying the struct
    // means the next field added to DisplayOutputConfig is carried by default
    // instead of being lost until someone notices.
    struct SavedMonitorConfig {
        DisplayOutputConfig config;
        std::unique_ptr<MonitorMirrorState> monitorState;
        bool bMatched = false;   // a monitor that never came back
    };
    std::vector<SavedMonitorConfig> saved;
    for (auto& o : m_displayOutputs) {
        if (o.config.type == DisplayOutputType::Monitor) {
            SavedMonitorConfig s;
            s.config = o.config;
            s.monitorState = std::move(o.monitorState);
            saved.push_back(std::move(s));
        }
    }

    // Remove existing monitor entries (keep Spout outputs)
    m_displayOutputs.erase(
        std::remove_if(m_displayOutputs.begin(), m_displayOutputs.end(),
            [](const DisplayOutput& o) { return o.config.type == DisplayOutputType::Monitor; }),
        m_displayOutputs.end());

    // Determine which monitor hosts the render window
    HMONITOR hRenderMon = nullptr;
    if (m_lpDX && m_lpDX->GetHwnd())
        hRenderMon = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);

    EnumMonitorCtx ctx = { this, hRenderMon };
    EnumDisplayMonitors(NULL, NULL, EnumMonitorCB, reinterpret_cast<LPARAM>(&ctx));

    // Restore saved config for monitors that still exist (match by display rect first,
    // fall back to device name — Windows can reassign device names across enumerations)
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor) continue;
        for (auto& s : saved) {
            bool rectMatch = (out.config.rcMonitor.left == s.config.rcMonitor.left &&
                              out.config.rcMonitor.top == s.config.rcMonitor.top &&
                              out.config.rcMonitor.right == s.config.rcMonitor.right &&
                              out.config.rcMonitor.bottom == s.config.rcMonitor.bottom);
            if (rectMatch || wcscmp(out.config.szDeviceName, s.config.szDeviceName) == 0) {
                // Everything the USER chose comes back; the four fields the
                // MACHINE owns stay as this enumeration just discovered them,
                // because that is the entire point of re-enumerating.
                const DisplayOutputType type = out.config.type;
                const RECT rc = out.config.rcMonitor;
                wchar_t dev[32], name[128];
                wcsncpy_s(dev, out.config.szDeviceName, _TRUNCATE);
                wcsncpy_s(name, out.config.szName, _TRUNCATE);

                out.config = s.config;

                out.config.type = type;
                out.config.rcMonitor = rc;
                wcsncpy_s(out.config.szDeviceName, dev, _TRUNCATE);
                wcsncpy_s(out.config.szName, name, _TRUNCATE);
                s.bMatched = true;
                // Same physical rect → reuse the live window + swap chain.
                // Rect changed (resolution / rearrange) → let it be rebuilt at
                // the new size; the old one is destroyed on the render thread.
                if (s.monitorState && rectMatch)
                    out.monitorState = std::move(s.monitorState);
                break;
            }
        }
    }

    // Anything not carried over (monitor unplugged, rect changed) still owns a
    // swap chain, an RTV block and an HWND. WaitForGpu/DestroyWindow are
    // render-thread-only, so hand them off instead of destroying them here.
    {
        std::lock_guard<std::mutex> lk(m_orphanMirrorMutex);
        for (auto& s : saved) {
            if (s.monitorState)
                m_orphanMirrors.push_back(std::move(s.monitorState));
        }
    }

    // A monitor that did not come back takes its child process with it. Losing
    // the record and leaving the process running are two separate bugs, and
    // carrying the fields above only fixes the first: without this, unplugging
    // a display leaves a full instance rendering to nothing, unlisted by
    // GET_CHILDREN and addressable by nothing (forgejo#35).
    // A display that did not come back used to leave a full second process
    // rendering to nothing, unlisted and unaddressable (forgejo#35), so it was
    // killed here. Its surface is retired by RetireMirrorSurfacesExcept on the
    // next SendToDisplayOutputs pass instead -- the display is not in `wanted`,
    // so nothing keeps it.
    for (auto& s : saved) {
        if (!s.bMatched && s.config.bOwnProcess && s.config.szDeviceName[0])
            DebugLogWFmt(LOG_WARN, L"display %ls is gone; its surface is retired",
                         s.config.szDeviceName);
    }
}

// ─── Mirror Activation Failsafe ──────────────────────────────────────────────

Engine::MirrorActivateResult Engine::TryActivateMirrors(HWND hRenderWnd)
{
    // Which monitor hosts the render window (mirrors of that one are skipped)
    wchar_t renderDevice[32] = {};
    if (hRenderWnd) {
        HMONITOR hRenderMon = MonitorFromWindow(hRenderWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi = { sizeof(mi) };
        if (hRenderMon && GetMonitorInfoW(hRenderMon, &mi))
            CopyTo(renderDevice, mi.szDevice);
    }

    int totalOther = 0;   // monitors that are not the render surface
    int enabledOther = 0;
    for (auto& o : m_displayOutputs) {
        if (o.config.type != DisplayOutputType::Monitor)
            continue;
        const bool isPrimary = renderDevice[0] &&
            wcscmp(o.config.szDeviceName, renderDevice) == 0;
        if (isPrimary)
            continue;
        totalOther++;
        if (o.config.bEnabled)
            enabledOther++;
    }

    // Case 1: User already enabled one or more non-primary monitors — keep that selection
    if (enabledOther > 0)
        return MirrorActivated;

    // Case 2: No other monitors at all — fullscreen only
    if (totalOther == 0)
        return MirrorFullscreenOnly;

    // Case 3: Other monitors exist but none enabled — auto-enable only non-primary
    // (do NOT force-enable the render monitor; that never gets a mirror window)
    for (auto& o : m_displayOutputs) {
        if (o.config.type != DisplayOutputType::Monitor)
            continue;
        const bool isPrimary = renderDevice[0] &&
            wcscmp(o.config.szDeviceName, renderDevice) == 0;
        o.config.bEnabled = !isPrimary;
    }
    SaveDisplayOutputSettings();
    return MirrorActivated;
}

// ─── INI Persistence ──────────────────────────────────────────────────────────

// [DisplayOutputs] and [DisplayOutput_*] -> resources/displays.json, once.
//
// 62 keys, and the last big block in settings.ini. The reader and writer above
// were not rewritten to speak json -- they map some sixty fields across two
// output types and decide whether a child process takes over a real monitor,
// and a field misread there puts a window on a screen the user is using. Only
// the STORE changed; see kv_store.h.
//
// Every key is found by enumeration, never from a list written here. The
// section is deleted straight afterwards, so a forgotten key would mean a lost
// one -- which is why ConfigBackend grew KeyNames().
void Engine::MigrateDisplaySettings()
{
    std::vector<std::wstring> secs;
    for (const std::wstring& sec : Config().SectionNames()) {
        if (_wcsicmp(sec.c_str(), L"DisplayOutputs") == 0 ||
            _wcsnicmp(sec.c_str(), L"DisplayOutput_", 14) == 0)
            secs.push_back(sec);
    }
    if (secs.empty()) return;               // already moved, or a fresh install

    size_t copied = 0;
    for (const std::wstring& sec : secs) {
        // A key already in the json wins: it is newer than anything still
        // sitting in the INI, and this runs before the loader.
        for (const std::wstring& key : Config().KeyNames(sec.c_str())) {
            if (DisplayCfg().Has(sec.c_str(), key.c_str())) continue;
            wchar_t val[512] = {};
            Config().GetStringTo(sec.c_str(), key.c_str(), L"", val, 512);
            DisplayCfg().SetString(sec.c_str(), key.c_str(), val);
            copied++;
        }
    }

    // Read every key back and compare before removing anything.
    for (const std::wstring& sec : secs) {
        for (const std::wstring& key : Config().KeyNames(sec.c_str())) {
            wchar_t was[512] = {}, now[512] = {};
            Config().GetStringTo(sec.c_str(), key.c_str(), L"", was, 512);
            DisplayCfg().GetStringTo(sec.c_str(), key.c_str(), L"\x01", now, 512);
            if (wcscmp(was, now) == 0) continue;
            DLOG_ERROR("displays: %ls/%ls did not survive ('%ls' vs '%ls'); "
                       "keeping the INI sections", sec.c_str(), key.c_str(), was, now);
            return;
        }
    }

    for (const std::wstring& sec : secs) Config().RemoveSection(sec.c_str());
    ConfigFlushAll();
    DLOG_INFO("displays: moved %zu key(s) in %zu section(s) to %ls",
              copied, secs.size(), DisplayCfg().Path().c_str());
}

void Engine::LoadDisplayOutputSettings()
{
    // Startup-only (no other thread touches m_displayOutputs yet at this
    // point), but taking the lock here too means every structural mutator
    // holds it with no carved-out exception to remember. #27.
    std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);

    int count = DisplayCfg().GetInt(L"DisplayOutputs", L"Count", -1);
    int legacyOpacity = DisplayCfg().GetInt(L"DisplayOutputs", L"MirrorOpacity", 100);
    if (legacyOpacity < 1) legacyOpacity = 1;
    if (legacyOpacity > 100) legacyOpacity = 100;
    // AltSMode replaced the MirrorModeForAltS bool. Read the old key as the
    // default for the new one so an existing "use mirrors for ALT-S" keeps
    // working without the user setting it again.
    m_nDisplayViewMode = DisplayCfg().GetInt(L"DisplayOutputs", L"ViewMode", 0);
    if (m_nDisplayViewMode < 0 || m_nDisplayViewMode > 1) m_nDisplayViewMode = 0;
    m_nAltSMode = DisplayCfg().GetInt(L"DisplayOutputs", L"AltSMode",
        DisplayCfg().GetBool(L"DisplayOutputs", L"MirrorModeForAltS", false)
            ? ALTS_MIRROR : ALTS_STRETCH);
    if (m_nAltSMode < ALTS_STRETCH || m_nAltSMode > ALTS_PROFILE)
        m_nAltSMode = ALTS_STRETCH;
    m_bMirrorPromptDisabled = DisplayCfg().GetBool(L"DisplayOutputs", L"MirrorPromptDisabled", false);
    m_bMirrorIndependentDefault = DisplayCfg().GetBool(L"DisplayOutputs", L"MirrorIndependentDefault", false);
    {
        int fps = DisplayCfg().GetInt(L"DisplayOutputs", L"MirrorMaxFps", 0);
        if (fps < 0) fps = 0;
        if (fps > 0 && fps < 5) fps = 5;
        if (fps > 240) fps = 240;
        m_nMirrorMaxFps.store(fps);
    }

    if (count < 0) {
        // Legacy migration: no [DisplayOutputs] section yet.
        // Create a default Spout output from the old settings.
        DisplayOutput spout;
        spout.config.type = DisplayOutputType::Spout;
        spout.config.bEnabled = bSpoutOut;
        spout.config.bFixedSize = bSpoutFixedSize;
        spout.config.nWidth = nSpoutFixedWidth;
        spout.config.nHeight = nSpoutFixedHeight;
        CopyTo(spout.config.szName, L"MDropDX12");
        m_displayOutputs.insert(m_displayOutputs.begin(), std::move(spout));
        return;
    }

    for (int i = 0; i < count; i++) {
        wchar_t section[64];
        swprintf(section, 64, L"DisplayOutput_%d", i);

        wchar_t typeBuf[32] = {};
        DisplayCfg().GetStringTo(section, L"Type", L"Spout", typeBuf, 32);

        DisplayOutput out;
        if (wcscmp(typeBuf, L"Monitor") == 0)
            out.config.type = DisplayOutputType::Monitor;
        else
            out.config.type = DisplayOutputType::Spout;

        out.config.bEnabled = DisplayCfg().GetBool(section, L"Enabled", false);

        wchar_t nameBuf[128] = {};
        DisplayCfg().GetStringTo(section, L"Name", L"MDropDX12", nameBuf, 128);
        wcsncpy_s(out.config.szName, nameBuf, _TRUNCATE);

        if (out.config.type == DisplayOutputType::Monitor) {
            wchar_t devBuf[32] = {};
            DisplayCfg().GetStringTo(section, L"DeviceName", L"", devBuf, 32);
            wcsncpy_s(out.config.szDeviceName, devBuf, _TRUNCATE);
            out.config.bFullscreen = DisplayCfg().GetBool(section, L"Fullscreen", true);
            out.config.nOpacity = DisplayCfg().GetInt(section, L"Opacity", 100);
            if (out.config.nOpacity < 1) out.config.nOpacity = 1;
            if (out.config.nOpacity > 100) out.config.nOpacity = 100;
            out.config.bClickThrough = DisplayCfg().GetBool(section, L"ClickThrough", false);
            out.config.bIndependentRender = DisplayCfg().GetBool(section, L"IndependentRender", m_bMirrorIndependentDefault);
            // Per-display child settings (forgejo#22)
            out.config.bOwnProcess = DisplayCfg().GetBool(section, L"OwnProcess", false);

            // OwnProcess=1 now comes back as an in-process own-preset
            // display, and is therefore persisted like any other setting.
            //
            // It used to be DOWNGRADED here to an independent mirror, because
            // it meant "spawn a second MDropDX12.exe" and a stale flag nobody
            // remembered setting brought up borderless-fullscreen instances on
            // displays somebody was working on. That reason is gone with the
            // spawn: since #186 phase 4 the flag means the display holds its
            // own preset, rendered by this process, so restoring it starts no
            // process and covers no monitor that was not already an output.
            //
            // The key keeps its now-inaccurate name deliberately -- see
            // DisplayHoldsOwnPreset.
            {
                wchar_t pathBuf[MAX_PATH] = {};
                DisplayCfg().GetStringTo(section, L"PresetDir", L"", pathBuf, MAX_PATH);
                wcsncpy_s(out.config.szPresetDir, pathBuf, _TRUNCATE);
                pathBuf[0] = L'\0';
                DisplayCfg().GetStringTo(section, L"StartupPreset", L"", pathBuf, MAX_PATH);
                wcsncpy_s(out.config.szStartupPreset, pathBuf, _TRUNCATE);
            }
            out.config.fTimeBetweenPresets = DisplayCfg().GetFloat(section, L"TimeBetweenPresets", -1.0f);
            out.config.bSequentialOrder = DisplayCfg().GetBool(section, L"SequentialOrder", false);
            out.config.nPresetLock = DisplayCfg().GetInt(section, L"PresetLock", -1);
            if (out.config.nPresetLock < -1) out.config.nPresetLock = -1;
            if (out.config.nPresetLock > 1)  out.config.nPresetLock = 1;
        }
        else {
            out.config.bFixedSize = DisplayCfg().GetBool(section, L"FixedSize", false);
            out.config.nWidth = DisplayCfg().GetInt(section, L"Width", 1920);
            out.config.nHeight = DisplayCfg().GetInt(section, L"Height", 1080);
        }

        // For monitors, try to match to an already-enumerated monitor by DeviceName
        if (out.config.type == DisplayOutputType::Monitor) {
            bool matched = false;
            for (auto& existing : m_displayOutputs) {
                if (existing.config.type == DisplayOutputType::Monitor &&
                    wcscmp(existing.config.szDeviceName, out.config.szDeviceName) == 0) {
                    // Update the enumerated entry with saved settings
                    existing.config.bEnabled = out.config.bEnabled;
                    existing.config.bFullscreen = out.config.bFullscreen;
                    existing.config.nOpacity = out.config.nOpacity;
                    existing.config.bClickThrough = out.config.bClickThrough;
                    existing.config.bIndependentRender = out.config.bIndependentRender;
                    existing.config.bOwnProcess = out.config.bOwnProcess;
                    CopyTo(existing.config.szPresetDir, out.config.szPresetDir);
                    CopyTo(existing.config.szStartupPreset, out.config.szStartupPreset);
                    existing.config.fTimeBetweenPresets = out.config.fTimeBetweenPresets;
                    existing.config.bSequentialOrder = out.config.bSequentialOrder;
                    existing.config.nPresetLock = out.config.nPresetLock;
                    matched = true;
                    break;
                }
            }
            // If monitor not currently connected, skip it
            if (!matched) continue;
        }
        else {
            // Spout outputs: insert at the beginning (before monitors)
            m_displayOutputs.insert(m_displayOutputs.begin(), std::move(out));
        }
    }

    // Sync legacy variables from first Spout output (backward compat)
    for (auto& out : m_displayOutputs) {
        if (out.config.type == DisplayOutputType::Spout) {
            bSpoutOut = out.config.bEnabled;
            bSpoutFixedSize = out.config.bFixedSize;
            nSpoutFixedWidth = out.config.nWidth;
            nSpoutFixedHeight = out.config.nHeight;
            break;
        }
    }
}

void Engine::SaveDisplayOutputSettings()
{

    int count = (int)m_displayOutputs.size();
    wchar_t buf[64];
    swprintf(buf, 64, L"%d", count);
    DisplayCfg().SetString(L"DisplayOutputs", L"Count", buf);
    swprintf(buf, 64, L"%d", m_nAltSMode);
    DisplayCfg().SetString(L"DisplayOutputs", L"AltSMode", buf);
    // The old key stays written and truthful, so downgrading a build does not
    // silently turn ALT-S back into stretch for someone using mirrors.
    swprintf(buf, 64, L"%d", m_nAltSMode == ALTS_MIRROR ? 1 : 0);
    DisplayCfg().SetString(L"DisplayOutputs", L"MirrorModeForAltS", buf);
    swprintf(buf, 64, L"%d", m_bMirrorPromptDisabled ? 1 : 0);
    DisplayCfg().SetString(L"DisplayOutputs", L"MirrorPromptDisabled", buf);
    swprintf(buf, 64, L"%d", m_bMirrorIndependentDefault ? 1 : 0);
    DisplayCfg().SetString(L"DisplayOutputs", L"MirrorIndependentDefault", buf);
    swprintf(buf, 64, L"%d", m_nMirrorMaxFps.load());
    DisplayCfg().SetString(L"DisplayOutputs", L"MirrorMaxFps", buf);

    for (int i = 0; i < count; i++) {
        auto& cfg = m_displayOutputs[i].config;
        wchar_t section[64];
        swprintf(section, 64, L"DisplayOutput_%d", i);

        DisplayCfg().SetString(section, L"Type", cfg.type == DisplayOutputType::Monitor ? L"Monitor" : L"Spout");
        swprintf(buf, 64, L"%d", cfg.bEnabled ? 1 : 0);
        DisplayCfg().SetString(section, L"Enabled", buf);
        DisplayCfg().SetString(section, L"Name", cfg.szName);

        if (cfg.type == DisplayOutputType::Monitor) {
            DisplayCfg().SetString(section, L"DeviceName", cfg.szDeviceName);
            swprintf(buf, 64, L"%d", cfg.bFullscreen ? 1 : 0);
            DisplayCfg().SetString(section, L"Fullscreen", buf);
            swprintf(buf, 64, L"%d", cfg.nOpacity);
            DisplayCfg().SetString(section, L"Opacity", buf);
            swprintf(buf, 64, L"%d", cfg.bClickThrough ? 1 : 0);
            DisplayCfg().SetString(section, L"ClickThrough", buf);
            swprintf(buf, 64, L"%d", cfg.bIndependentRender ? 1 : 0);
            DisplayCfg().SetString(section, L"IndependentRender", buf);
            // Per-display child settings (forgejo#22)
            swprintf(buf, 64, L"%d", cfg.bOwnProcess ? 1 : 0);
            DisplayCfg().SetString(section, L"OwnProcess", buf);
            DisplayCfg().SetString(section, L"PresetDir", cfg.szPresetDir);
            DisplayCfg().SetString(section, L"StartupPreset", cfg.szStartupPreset);
            swprintf(buf, 64, L"%.3f", cfg.fTimeBetweenPresets);
            DisplayCfg().SetString(section, L"TimeBetweenPresets", buf);
            swprintf(buf, 64, L"%d", cfg.bSequentialOrder ? 1 : 0);
            DisplayCfg().SetString(section, L"SequentialOrder", buf);
            swprintf(buf, 64, L"%d", cfg.nPresetLock);
            DisplayCfg().SetString(section, L"PresetLock", buf);
        }
        else {
            swprintf(buf, 64, L"%d", cfg.bFixedSize ? 1 : 0);
            DisplayCfg().SetString(section, L"FixedSize", buf);
            swprintf(buf, 64, L"%d", cfg.nWidth);
            DisplayCfg().SetString(section, L"Width", buf);
            swprintf(buf, 64, L"%d", cfg.nHeight);
            DisplayCfg().SetString(section, L"Height", buf);
        }
    }

    // Also sync legacy INI keys from first Spout output
    for (auto& out : m_displayOutputs) {
        if (out.config.type == DisplayOutputType::Spout) {
            bSpoutOut = out.config.bEnabled;
            bSpoutFixedSize = out.config.bFixedSize;
            nSpoutFixedWidth = out.config.nWidth;
            nSpoutFixedHeight = out.config.nHeight;
            break;
        }
    }
}

// ─── Display Output Init / Destroy ────────────────────────────────────────────

void Engine::InitDisplayOutput(DisplayOutput& out)
{
    if (out.config.type == DisplayOutputType::Spout) {
        if (out.spoutState && out.spoutState->bReady)
            return; // already live — never thrash Spout wraps mid-session
        out.spoutState = std::make_unique<SpoutOutputState>();
        auto& ss = *out.spoutState;

        if (!m_lpDX || !m_lpDX->m_device.Get())
            return;

        // Convert wide name to ANSI for Spout API
        char senderNameA[256] = {};
        WideCharToMultiByte(CP_ACP, 0, out.config.szName, -1, senderNameA, 256, NULL, NULL);
        ss.sender.SetName(senderNameA);

        if (!ss.sender.Open(m_lpDX->m_device.Get(),
                            m_lpDX->m_commandQueue.Get())) {
            char logBuf[512];
            sprintf(logBuf, "InitDisplayOutput: Spout Open failed for '%s'\n", senderNameA);
            DebugLogA(logBuf, LOG_ERROR);
            return;
        }

        for (int n = 0; n < DXC_FRAME_COUNT; n++) {
            if (!ss.sender.WrapResource(
                    m_lpDX->m_renderTargets[n].Get(),
                    &ss.wrappedBackBuffers[n],
                    D3D12_RESOURCE_STATE_RENDER_TARGET)) {
                char logBuf[512];
                sprintf(logBuf, "InitDisplayOutput: Spout WrapResource failed [%d]\n", n);
                DebugLogA(logBuf, LOG_ERROR);
                // Cleanup partial wraps
                for (int j = 0; j < n; j++) {
                    if (ss.wrappedBackBuffers[j]) {
                        ss.wrappedBackBuffers[j]->Release();
                        ss.wrappedBackBuffers[j] = nullptr;
                    }
                }
                ss.sender.Close();
                return;
            }
        }
        ss.bReady = true;
        { char logBuf[512]; sprintf(logBuf, "InitDisplayOutput: Spout sender '%s' ready\n", senderNameA); DebugLogA(logBuf); }
    }
    else if (out.config.type == DisplayOutputType::Monitor) {
        if (!m_lpDX || !m_lpDX->m_device.Get() || !m_lpDX->m_swapChain.Get())
            return;

        // Idempotent: if a session-lived mirror already exists, never recreate.
        // Destroying + CreateSwapChain while the GPU still references back buffers
        // is what TDRs the device after a couple of enable/disable toggles.
        if (out.monitorState && out.monitorState->bReady && out.monitorState->swapChain) {
            out.monitorState->bSoftDisabled = false;
            out.monitorState->nFramesUntilHardDestroy = 0;
            if (out.monitorState->hWnd && !IsWindowVisible(out.monitorState->hWnd))
                ShowWindow(out.monitorState->hWnd, SW_SHOWNOACTIVATE);
            return;
        }
        // Stale half-init (failed earlier) — drop without WaitForGpu (no live SC)
        if (out.monitorState)
            out.monitorState.reset();

        {
            char logBuf[512];
            sprintf(logBuf, "InitDisplayOutput: Starting init for %ls (%ls)\n",
                out.config.szName, out.config.szDeviceName);
            DebugLogA(logBuf, LOG_WARN);
        }

        // Safety: don't mirror the monitor hosting the render window.
        // In watermark mode, use the stored target device name for deterministic detection.
        {
            wchar_t renderDevice[32] = {};
            if (m_bMirrorWatermarkActive && m_szWatermarkRenderDevice[0]) {
                CopyTo(renderDevice, m_szWatermarkRenderDevice);
            } else if (m_lpDX->GetHwnd()) {
                HMONITOR hRenderMon = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);
                if (hRenderMon) {
                    MONITORINFOEXW mi = { sizeof(mi) };
                    if (GetMonitorInfoW(hRenderMon, &mi))
                        CopyTo(renderDevice, mi.szDevice);
                }
            }
            if (renderDevice[0] && wcscmp(renderDevice, out.config.szDeviceName) == 0) {
                char logBuf[256];
                sprintf(logBuf, "InitDisplayOutput: Skipping %ls — render window's monitor\n",
                    out.config.szDeviceName);
                DebugLogA(logBuf, LOG_WARN);
                out.bSkippedSameMonitor = true;
                return;
            }
        }

        // Do NOT WaitForGpu here — it freezes the whole machine when called often
        // (enable/disable cycles). Soft-destroy already waits multiple frames before
        // releasing swap chains. PeekMessage/Dispatch is also avoided: re-entering
        // the message pump from the render thread can deadlock DXGI.

        out.monitorState = std::make_unique<MonitorMirrorState>();
        auto& ms = *out.monitorState;
        ms.bSoftDisabled = false;
        ms.nFramesUntilHardDestroy = 0;

        // Register mirror window class (once)
        if (!m_bMirrorClassRegistered) {
            WNDCLASSEXW wc = { sizeof(wc) };
            wc.lpfnWndProc = MirrorWndProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
            wc.lpszClassName = L"MDropDX12_Mirror";
            if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
                m_bMirrorClassRegistered = true;
        }

        // Re-query the monitor rect fresh by device name (cached rect may be stale)
        FindMonitorCtx fmc = { out.config.szDeviceName, {}, false };
        EnumDisplayMonitors(NULL, NULL, FindMonitorCB, reinterpret_cast<LPARAM>(&fmc));
        if (fmc.bFound) {
            out.config.rcMonitor = fmc.rcResult;
        } else {
            char logBuf[256];
            sprintf(logBuf, "InitDisplayOutput: WARNING — monitor %ls not found by EnumDisplayMonitors!\n",
                out.config.szDeviceName);
            DebugLogA(logBuf, LOG_WARN);
        }

        RECT rc = out.config.rcMonitor;
        int monW = rc.right - rc.left;
        int monH = rc.bottom - rc.top;
        {
            char logBuf[512];
            sprintf(logBuf, "InitDisplayOutput: %ls rect = (%d,%d)-(%d,%d) size %dx%d\n",
                out.config.szDeviceName,
                (int)rc.left, (int)rc.top, (int)rc.right, (int)rc.bottom, monW, monH);
            DebugLogA(logBuf, LOG_WARN);
        }

        if (monW <= 0 || monH <= 0) {
            DebugLogA("InitDisplayOutput: Invalid monitor rect, skipping\n", LOG_WARN);
            out.monitorState.reset();
            return;
        }

        // Create borderless popup covering the target monitor.
        // IMPORTANT: Do NOT use WS_EX_LAYERED when opacity is 100%. Flip-model
        // swap chains + layered windows let DWM composite other monitors' content
        // through the mirror (landscape "ghost" strip on portrait, often flickering).
        // Layered is only for partial opacity or click-through.
        const bool needLayered = (out.config.nOpacity < 100) || out.config.bClickThrough;
        DWORD exStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        if (needLayered)
            exStyle |= WS_EX_LAYERED;
        if (out.config.bClickThrough)
            exStyle |= WS_EX_TRANSPARENT;
        // Fullscreen checkbox: full monitor vs centered windowed layout
        int layX = rc.left, layY = rc.top, layW = monW, layH = monH;
        ComputeMirrorLayout(out.config, layX, layY, layW, layH);

        ms.hWnd = CreateWindowExW(
            exStyle,
            L"MDropDX12_Mirror",
            L"MDropDX12 Mirror",
            WS_POPUP,
            layX, layY, layW, layH,
            nullptr, nullptr, GetModuleHandle(NULL), nullptr);
        if (!ms.hWnd) {
            DebugLogA("InitDisplayOutput: CreateWindowExW failed for mirror\n", LOG_ERROR);
            out.monitorState.reset();
            return;
        }
        // Primary HWND for focus hand-off on click (local hotkeys need primary focus).
        HWND hPrimary = m_lpDX ? m_lpDX->GetHwnd() : nullptr;
        SetWindowLongPtrW(ms.hWnd, GWLP_USERDATA, (LONG_PTR)hPrimary);
        if (needLayered) {
            BYTE alpha = (BYTE)(out.config.nOpacity * 255 / 100);
            if (alpha < 3) alpha = 3;
            SetLayeredWindowAttributes(ms.hWnd, 0, alpha, LWA_ALPHA);
        }
        // Mirrors must stay above other windows on their monitor; otherwise the
        // landscape primary (or desktop) can show through as a ghost.
        SetWindowPos(ms.hWnd, HWND_TOPMOST,
                     layX, layY, layW, layH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!m_bAlwaysOnTop)
            SetWindowPos(ms.hWnd, HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(ms.hWnd, SW_SHOWNOACTIVATE);

        // Get DXGI factory from existing swap chain
        ComPtr<IDXGIFactory4> factory;
        HRESULT hr = m_lpDX->m_swapChain->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            DebugLogA("InitDisplayOutput: GetParent(IDXGIFactory4) failed\n", LOG_ERROR);
            DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
            out.monitorState.reset();
            return;
        }

        // Copy mode: SC = primary BB (CopyResource + DXGI stretch).
        // Independent opposite-aspect: SC = monitor aspect, long side capped at
        // 1920 (full 2160x3840 + a second classic pass TDRs). Same-aspect
        // independent still uses primary size and blits.
        const int primW = (m_lpDX->m_client_width > 0) ? m_lpDX->m_client_width : layW;
        const int primH = (m_lpDX->m_client_height > 0) ? m_lpDX->m_client_height : layH;
        MirrorSwapChainSize(out, primW, primH, ms.width, ms.height);
        ms.bIndependentSized = (ms.width != primW || ms.height != primH);

        DXGI_SWAP_CHAIN_DESC1 scDesc = {};
        scDesc.Width = (UINT)ms.width;
        scDesc.Height = (UINT)ms.height;
        scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scDesc.SampleDesc.Count = 1;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.BufferCount = DXC_FRAME_COUNT;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        scDesc.Flags = 0;
        scDesc.Scaling = DXGI_SCALING_STRETCH;

        ComPtr<IDXGISwapChain1> sc1;
        hr = factory->CreateSwapChainForHwnd(
            m_lpDX->m_commandQueue.Get(), ms.hWnd, &scDesc, nullptr, nullptr, &sc1);
        ms.bufferCount = scDesc.BufferCount;
        if (FAILED(hr)) {
            char logBuf[256]; sprintf(logBuf, "InitDisplayOutput: CreateSwapChainForHwnd failed (0x%08X)\n", (unsigned)hr);
            DebugLogA(logBuf, LOG_ERROR);
            DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
            out.monitorState.reset();
            return;
        }
        factory->MakeWindowAssociation(ms.hWnd, DXGI_MWA_NO_ALT_ENTER);
        hr = sc1.As(&ms.swapChain);
        if (FAILED(hr)) {
            DebugLogA("InitDisplayOutput: QueryInterface IDXGISwapChain4 failed\n", LOG_ERROR);
            DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
            out.monitorState.reset();
            return;
        }

        // Get back buffers (+ permanent-reserve RTVs that survive resize rewinds)
        ms.bHasRtv = false;
        ms.rtvSlotBase = m_lpDX->AllocateMirrorRtvBlock(ms.bufferCount);
        if (ms.rtvSlotBase == UINT_MAX) {
            DebugLogA("InitDisplayOutput: no reserved RTV slots for mirror\n", LOG_ERROR);
            ms.swapChain.Reset();
            DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
            out.monitorState.reset();
            return;
        }
        for (UINT i = 0; i < ms.bufferCount && i < MIRROR_BUFFER_COUNT; i++) {
            hr = ms.swapChain->GetBuffer(i, IID_PPV_ARGS(&ms.backBuffers[i]));
            if (FAILED(hr)) {
                char logBuf[256]; sprintf(logBuf, "InitDisplayOutput: GetBuffer(%d) failed\n", i);
                DebugLogA(logBuf, LOG_ERROR);
                m_lpDX->FreeMirrorRtvBlock(ms.rtvSlotBase, ms.bufferCount);
                ms.rtvSlotBase = UINT_MAX;
                ms.swapChain.Reset();
                DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
                out.monitorState.reset();
                return;
            }
            ms.rtvHandles[i] = m_lpDX->GetRtvCpuHandleAt(ms.rtvSlotBase + i);
            m_lpDX->m_device->CreateRenderTargetView(ms.backBuffers[i].Get(), nullptr, ms.rtvHandles[i]);
        }
        ms.bHasRtv = true;
        ms.rtvEpoch = m_lpDX->m_descriptorEpoch;
        ms.bNeedsFullChainClear = true;
        ms.paintedBufferMask = 0;
        for (UINT bi = 0; bi < MIRROR_BUFFER_COUNT; bi++)
            ms.bbState[bi] = D3D12_RESOURCE_STATE_COMMON;

        ms.bReady = true;
        {
            char logBuf[256];
            sprintf(logBuf, "InitDisplayOutput: Mirror %ls READY (hwnd=%p, swapchain=%p, %dx%d copy-size)\n",
                out.config.szDeviceName, (void*)ms.hWnd, (void*)ms.swapChain.Get(),
                ms.width, ms.height);
            DebugLogA(logBuf, LOG_WARN);
        }
    }
}

void Engine::DestroyDisplayOutput(DisplayOutput& out)
{
    if (out.spoutState) {
        auto& ss = *out.spoutState;
        for (int n = 0; n < DXC_FRAME_COUNT; n++) {
            if (ss.wrappedBackBuffers[n]) {
                ss.wrappedBackBuffers[n]->Release();
                ss.wrappedBackBuffers[n] = nullptr;
            }
        }
        if (ss.bReady) {
            ss.sender.Close();
            ss.bReady = false;
        }
        out.spoutState.reset();
    }
    if (out.monitorState) {
        DestroyMonitorMirror(*out.monitorState);
        out.monitorState.reset();
    }
}

// Single teardown path for a mirror's GPU + window resources. Callers must be
// on the render thread: WaitForGpu freezes the UI thread, and DestroyWindow
// only works from the thread that created the window.
void Engine::DestroyMonitorMirror(MonitorMirrorState& ms)
{
    ms.bReady = false;
    ms.bSoftDisabled = true;
    // Must idle the GPU before releasing swap-chain buffers. Without this,
    // in-flight mirror command lists TDR the device (seen as lockup after
    // 1–2 toggles when destroy ran mid-session). Only called on profile
    // load / refresh / app shutdown — never on enable/disable toggle.
    if (m_lpDX && ms.swapChain)
        m_lpDX->WaitForGpu();
    if (m_lpDX && ms.rtvSlotBase != UINT_MAX) {
        m_lpDX->FreeMirrorRtvBlock(ms.rtvSlotBase, ms.bufferCount ? ms.bufferCount : MIRROR_BUFFER_COUNT);
        ms.rtvSlotBase = UINT_MAX;
    }
    ms.bHasRtv = false;
    for (UINT i = 0; i < MIRROR_BUFFER_COUNT; i++)
        ms.backBuffers[i].Reset();
    ms.swapChain.Reset();
    if (ms.hWnd) {
        DestroyWindow(ms.hWnd);
        ms.hWnd = nullptr;
    }
}

// Render-thread drain for mirrors detached by EnumerateDisplayOutputs.
void Engine::DrainOrphanedMirrors()
{
    std::vector<std::unique_ptr<MonitorMirrorState>> orphans;
    {
        std::lock_guard<std::mutex> lk(m_orphanMirrorMutex);
        if (m_orphanMirrors.empty())
            return;
        orphans.swap(m_orphanMirrors);
    }
    for (auto& ms : orphans) {
        if (ms)
            DestroyMonitorMirror(*ms);
    }
    char logBuf[128];
    sprintf(logBuf, "DrainOrphanedMirrors: destroyed %d re-enumerated mirror(s)\n", (int)orphans.size());
    DebugLogA(logBuf, LOG_WARN);
}

void Engine::ReleaseDisplayOutputWraps()
{
    for (auto& out : m_displayOutputs) {
        if (out.spoutState) {
            auto& ss = *out.spoutState;
            for (int n = 0; n < DXC_FRAME_COUNT; n++) {
                if (ss.wrappedBackBuffers[n]) {
                    ss.wrappedBackBuffers[n]->Release();
                    ss.wrappedBackBuffers[n] = nullptr;
                }
            }
            ss.sender.Close();
            ss.bReady = false;
            out.spoutState.reset();
        }
    }
}

void Engine::DestroyAllDisplayOutputs()
{
    StopMirrorThread();
    DrainOrphanedMirrors();
    for (auto& out : m_displayOutputs)
        DestroyDisplayOutput(out);

    // Release mirror command objects
    m_mirrorCmdList.Reset();
    for (int i = 0; i < DXC_FRAME_COUNT; i++)
        m_mirrorCmdAllocators[i].Reset();
    if (m_lpDX && !LagIndepFenceIdle())
        m_lpDX->WaitForGpu();
    ReleaseLagIndepObjects();
}

bool Engine::LagIndepFenceIdle() const
{
    if (!m_lagIndepFence)
        return true;
    return m_lagIndepFence->GetCompletedValue() >= m_lagIndepSubmitted;
}

bool Engine::EnsureLagIndepObjects()
{
    if (!m_lpDX || !m_lpDX->m_device)
        return false;
    if (!m_lagIndepFence) {
        HRESULT hr = m_lpDX->m_device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_lagIndepFence));
        if (FAILED(hr)) {
            DebugLogA("EnsureLagIndepObjects: CreateFence failed\n", LOG_ERROR);
            return false;
        }
        m_lagIndepSubmitted = 0;
        m_lagIndepSignal = 0;
    }
    if (!m_lagIndepAlloc) {
        HRESULT hr = m_lpDX->m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_lagIndepAlloc));
        if (FAILED(hr)) {
            DebugLogA("EnsureLagIndepObjects: CreateCommandAllocator failed\n", LOG_ERROR);
            return false;
        }
    }
    if (!m_lagIndepList) {
        HRESULT hr = m_lpDX->m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_lagIndepAlloc.Get(), nullptr, IID_PPV_ARGS(&m_lagIndepList));
        if (FAILED(hr)) {
            DebugLogA("EnsureLagIndepObjects: CreateCommandList failed\n", LOG_ERROR);
            return false;
        }
        m_lagIndepList->Close();
    }
    return true;
}

void Engine::ReleaseLagIndepObjects()
{
    m_lagIndepList.Reset();
    m_lagIndepAlloc.Reset();
    m_lagIndepFence.Reset();
    m_lagIndepSubmitted = 0;
    m_lagIndepSignal = 0;
    m_lagIndepAuxFrame = UINT_MAX;
}

void Engine::LockMirrorEngine() {
    // No yield. A hand-rolled one stood here for two years in two flavours,
    // and both were wrong in opposite directions -- see the note on
    // m_mirrorEngineMutex. The workers block on the mutex properly now, so
    // they are in its wait list and the OS hands it to them.
    m_mirrorEngineMutex.lock();
}
void Engine::UnlockMirrorEngine() { m_mirrorEngineMutex.unlock(); }

namespace {
// One per worker: the engine plus the surface it serves, owned by the thread.
struct MirrorWorkerArg {
    mdrop::Engine*                                engine;
    std::shared_ptr<mdrop::Engine::MirrorSurface> surf;
};
}

static unsigned __stdcall MirrorThreadThunk(void* arg)
{
    // The shared_ptr is what keeps the surface alive for the thread's whole
    // life. A retire can drop the engine's reference while this thread is
    // mid-frame; it must not free the sim and the pipe under it.
    std::unique_ptr<MirrorWorkerArg> a(reinterpret_cast<MirrorWorkerArg*>(arg));
    a->engine->MirrorThreadMain(a->surf);
    return 0;
}

bool Engine::StartSurfaceWorker(const std::shared_ptr<MirrorSurface>& surf)
{
    if (!surf)
        return false;
    if (surf->hThread)
        return true;
    if (!m_lpDX || !m_lpDX->m_device || !m_mirrorQueue)
        return false;

    surf->quit.store(false);
    if (!surf->hWake)
        surf->hWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!surf->hWake)
        return false;

    // SUSPENDED, because a worker MUST own an upload ring before it runs a
    // single frame and a ring is claimed per THREAD ID -- which does not exist
    // until the thread does. Sharing the aux ring let a worker reset the
    // render thread's offsets mid-frame (black presets, then TDR); that is why
    // phase 5a made the ring per thread and why this cannot simply start.
    auto* arg = new MirrorWorkerArg{ this, surf };
    unsigned tid = 0;
    HANDLE h = (HANDLE)_beginthreadex(nullptr, 0, MirrorThreadThunk, arg,
                                      CREATE_SUSPENDED, &tid);
    if (!h) {
        delete arg;
        DebugLogA("StartSurfaceWorker: _beginthreadex failed\n", LOG_ERROR);
        return false;
    }
    // No ring, no worker. Running one anyway would put it on the shared aux
    // ring, which is the exact failure the per-thread rings exist to prevent --
    // and IsMirrorWorkerThread() reads the ring registry, so a ringless worker
    // would also not be recognised as one by EnsureOrientPipeline's guard.
    if (!m_lpDX->SetMirrorUploadThreadId((DWORD)tid) ||
        !m_lpDX->CreateMirrorUploadBuffer((DWORD)tid)) {
        DebugLogA("StartSurfaceWorker: no upload ring available - surface stays idle\n",
                  LOG_ERROR);
        surf->quit.store(true);
        ResumeThread(h);            // let it observe the quit flag and exit
        WaitForSingleObject(h, 2000);
        CloseHandle(h);
        m_lpDX->ReleaseMirrorUploadBuffer((DWORD)tid);
        if (DXContext::MirrorUploadRing* r = m_lpDX->MirrorRingForThread((DWORD)tid))
            r->threadId.store(0, std::memory_order_release);
        return false;
    }
    surf->hThread = h;
    surf->threadId = tid;
    ResumeThread(h);
    char buf[192];
    sprintf(buf, "mirror worker started for %ls (tid %u)\n",
            surf->device.empty() ? L"(none)" : surf->device.c_str(), tid);
    DebugLogA(buf, LOG_INFO);
    return true;
}

void Engine::StopSurfaceWorker(MirrorSurface& surf)
{
    if (surf.hThread) {
        surf.quit.store(true);
        if (surf.hWake)
            SetEvent(surf.hWake);
        // The join TIMES OUT on purpose. This runs on the render thread, which
        // may be holding the engine mutex the worker is spinning for; the
        // worker's try_lock loop checks the quit flag every spin, so it gets
        // out -- but if anything else has it parked, waiting forever here would
        // hang the render thread instead.
        WaitForSingleObject(surf.hThread, 4000);
        CloseHandle(surf.hThread);
        surf.hThread = nullptr;
    }
    if (surf.hWake) {
        CloseHandle(surf.hWake);
        surf.hWake = nullptr;
    }
    if (m_lpDX && surf.threadId) {
        // Buffer first, then the slot -- releasing the slot would make the ring
        // unfindable and leak the 32 MB it holds.
        m_lpDX->ReleaseMirrorUploadBuffer((DWORD)surf.threadId);
        if (DXContext::MirrorUploadRing* r = m_lpDX->MirrorRingForThread((DWORD)surf.threadId))
            r->threadId.store(0, std::memory_order_release);
    }
    surf.threadId = 0;
}

void Engine::StartMirrorThread()
{
    if (!m_lpDX || !m_lpDX->m_device)
        return;

    // The queue is shared by every worker: ID3D12CommandQueue is free-threaded,
    // so concurrent ExecuteCommandLists and Signal on it are legal.
    if (!m_mirrorQueue) {
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        if (FAILED(m_lpDX->m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_mirrorQueue)))) {
            DebugLogA("StartMirrorThread: CreateCommandQueue failed\n", LOG_ERROR);
            return;
        }
    }

    m_bMirrorThreadQuit.store(false);
    for (auto& sp : SnapshotMirrorSurfaces()) {
        sp->publishedIdx.store(-1);
        sp->lastWrite = -1;
        sp->pendingWrite = -1;
        StartSurfaceWorker(sp);
    }
}

void Engine::StopMirrorThread()
{
    m_bMirrorThreadQuit.store(true);
    for (auto& sp : SnapshotMirrorSurfaces()) {
        StopSurfaceWorker(*sp);
        sp->publishedIdx.store(-1);
        sp->lastWrite = -1;
        sp->pendingWrite = -1;
        // Sim context: states + per-context EEL storage die with the thread.
        MirrorSimFree(sp->sim);
    }
    m_mirrorQueue.Reset();
}

void Engine::MirrorThreadMain(const std::shared_ptr<MirrorSurface>& surf)
{
    MirrorSurface& s = *surf;
    {
        char buf[160];
        sprintf(buf, "MirrorThreadMain: enter (%ls)\n",
                s.device.empty() ? L"(none)" : s.device.c_str());
        DebugLogA(buf, LOG_INFO);
    }
    while (!m_bMirrorThreadQuit.load() && !s.quit.load()) {
        // Poll at 2 ms: nothing signals the wake event, so this timeout IS the
        // worker's loop pacer - 50 ms capped mirrors at ~20 fps (11 fps
        // backgrounded, when a fence-busy miss cost another 50 ms). The actual
        // rate comes from the orient GPU fence and the pacing gate below.
        WaitForSingleObject(s.hWake, 2);
        if (m_bMirrorThreadQuit.load() || s.quit.load())
            break;
        if (!m_bMirrorsActive)
            continue;
        try {
            MirrorThreadDrawAndPresent(s);
        } catch (...) {
            DebugLogA("MirrorThreadMain: exception - skip frame\n", LOG_ERROR);
        }
    }
    DebugLogA("MirrorThreadMain: exit\n", LOG_INFO);
}

void Engine::MirrorThreadDrawAndPresent(MirrorSurface& surf)
{
    // ONE surface, on its own thread (#186 phase 5c). What used to be a loop
    // over every surface inside one worker is now this function running N
    // times concurrently, which is the whole change: the simulation below is
    // ctx-pure and lock-free, so N of them genuinely overlap. The RECORD still
    // takes m_mirrorEngineMutex exclusively -- see the note on MirrorSurface
    // for why that is the right split and not a shortcut.
    //
    // Quit checks: StopSurfaceWorker can run on the render thread WHILE that
    // thread holds the engine mutex - its join times out if we are blocked on
    // that mutex, and it then frees the sim context and D3D objects. Every
    // resume point below must bail on quit before touching anything.
    if (m_bMirrorThreadQuit.load() || surf.quit.load())
        return;
    if (!m_lpDX || !m_lpDX->m_device || !m_mirrorQueue)
        return;

    // Nothing to draw for? Then do not simulate.
    //
    // Each worker runs a FULL independent MilkDrop simulation -- its own CState,
    // its own 50 EEL VMs, per-vertex EEL over the same warp grid as the primary
    // -- and it had no test for whether any consumer existed. When every mirror
    // target is taken over by a -child, SendToDisplayOutputs correctly stops
    // copying the source and the mirror swap chains stop presenting, but this
    // kept running flat out into a surface nobody read.
    //
    // Measured on a 3-display setup with both mirror targets child-owned:
    // orientFrames advanced 1283 in five seconds (~256fps) while per-monitor
    // draws advanced 0, having accumulated 165,000 discarded frames. At
    // sim 1.8ms + lock 1.5ms + record 0.2ms per frame that is most of a core,
    // and it took m_mirrorEngineMutex ~256 times a second in contention with
    // the render thread -- which is the more expensive half, because the
    // primary blocks on that same mutex.
    //
    // Safe against the handover it might look like it breaks: ChildOwnsDisplay
    // is gated on the child being READY, not merely requested, so the mirror is
    // held until there is something to hand the display over to. And the sim's
    // animation clock is the primary's (mirror_sim.cpp), so a mirror that
    // starts again after a child is retired resumes at the correct time rather
    // than replaying stale simulation.
    if (!m_bAnyMirrorTargetLive.load(std::memory_order_acquire))
        return;

    // Jerkiness instrumentation. thread_local since 5c: each worker measures
    // ITS OWN surface, which is what these numbers were always trying to
    // describe. The published values are therefore one surface's -- whichever
    // closed its window last -- and no longer a total over all of them.
    static LONGLONG s_qpf = [] {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart;
    }();
    struct DiagWin {
        LONGLONG lastDone = 0, winStart = 0;
        int n = 0, fence = 0;
        LONGLONG dtSum = 0, lockSum = 0, recSum = 0, simSum = 0;
        int dtMin = INT_MAX, dtMax = 0, lockMax = 0, recMax = 0, simMax = 0;
    };
    thread_local DiagWin s_dw;
    auto usSince = [&](LONGLONG a, LONGLONG b) {
        return (int)((b - a) * 1000000 / s_qpf);
    };

    // This surface's own previous frame must be off the GPU. Wait briefly
    // rather than returning: a hard return cost a full primary frame per miss
    // and put backgrounded mirrors at 11 fps. Nothing here waits on ANOTHER
    // surface any more -- which is precisely what 5b's per-surface fences were
    // for, and what lets these threads run independently.
    if (!surf.WorkIdle()) {
        s_dw.fence++;
        bool waited = false;
        if (surf.workFence && surf.fenceEvt &&
            SUCCEEDED(surf.workFence->SetEventOnCompletion(surf.workSubmitted,
                                                           surf.fenceEvt)))
            waited = WaitForSingleObject(surf.fenceEvt, 20) == WAIT_OBJECT_0;
        if (!waited)
            return;
    }

    const int needW = surf.needW.load(), needH = surf.needH.load();
    if (needW <= 0 || needH <= 0 || !surf.pipe.ready)
        return;

    // -- Pacing ---------------------------------------------------------------
    // The orient pipeline integrates its feedback once per orient frame, so the
    // worker's rate IS the mirror's animation SPEED, not just its smoothness.
    // Free-running it (the old parity mode) only looked right while engine-lock
    // contention happened to throttle the worker to the primary's rate: an
    // fps-capped primary sleeps between frames and releases the lock, so the
    // worker hit 160 fps against a 40 fps primary and its warp/zoom advanced
    // four times per primary frame - mirrors visibly faster than the window
    // they mirror (Shane, 2026-08-23, "martin - Gin Tonic on Ice 004.milk2").
    //
    //   maxFps == 0 (parity, default): one sim frame per PRIMARY frame. Same
    //     integration rate as the primary is what makes the motion match.
    //   maxFps  > 0: explicit ceiling instead (Displays dropdown /
    //     SET_MIRROR_MAXFPS). Set it high for the old unbounded free-run.
    //
    // Both gates run BEFORE the sim step: the cap used to step the ctx EEL and
    // then discard the frame, advancing the simulation with no render behind it.
    //
    // Per SURFACE since 5c. These were engine members when one worker paced
    // every surface; shared, whichever thread ran first would satisfy the gate
    // on behalf of all of them and the rest would starve.
    LARGE_INTEGER qpcNow;
    QueryPerformanceCounter(&qpcNow);
    const int maxFps = m_nMirrorMaxFps.load(std::memory_order_relaxed);
    if (maxFps > 0) {
        // -1.5 ms so the 2 ms wake quantization lands ON the target period
        // instead of one poll past it (a 60 fps cap otherwise runs ~53).
        const double minMs = 1000.0 / (double)maxFps - 1.5;
        if (surf.lastOrientQpc != 0 &&
            (double)(qpcNow.QuadPart - surf.lastOrientQpc) * 1000.0 / (double)s_qpf < minMs)
            return;
    } else {
        const unsigned pf = m_nPrimaryFrameSeq.load(std::memory_order_acquire);
        if (pf == surf.seenPrimaryFrame)
            return; // primary has not advanced - nothing new to keep up with
        surf.seenPrimaryFrame = pf;
    }

    // Independent simulation: adopt the latest preset bundle and advance the
    // context OUTSIDE the engine lock - ctx-pure work (own states/VMs/mesh),
    // imports serialize on m_presetLoadMutex only. Never nest those locks
    // (see engine.h at m_presetLoadMutex).
    //
    // THIS is the part 5c parallelises, and the measurement that chose it:
    // two surfaces free-running spend sim 1.6 ms, record 0.4 ms and lock-wait
    // 0.2 ms of a 3.8 ms iteration.
    int simUs = 0;
    if (!m_bShadertoyMode) {
        const int gw = surf.pipe.ready ? surf.pipe.w : needW;
        const int gh = surf.pipe.ready ? surf.pipe.h : needH;
        MirrorSimEnsureGrid(surf.sim, gw, gh,
                            surf.panelW.load(), surf.panelH.load());
        // Take any preset the render or IPC thread asked for. The worker owns
        // sim.ownPresetPath; nothing else may assign to it.
        DrainSurfacePreset(surf);
        // No bundle yet (startup): keep the last presented image.
        if (!MirrorSimAdoptPreset(surf.sim))
            return;
        // Publish what this display is now showing. Adoption is rare, so the
        // string copy is free; the alternative is every reporting path reading
        // the worker's own sim.loadedOwnPath while the worker assigns it.
        {
            std::lock_guard<std::mutex> lk(surf.requestMutex);
            surf.shownPreset = surf.sim.loadedOwnPath;
        }
        LARGE_INTEGER simA, simB;
        QueryPerformanceCounter(&simA);
        MirrorSimStepFrame(surf.sim);
        QueryPerformanceCounter(&simB);
        simUs = usSince(simA.QuadPart, simB.QuadPart);
    }

    // Block on the lock, with a timeout that exists ONLY to re-check the quit
    // flag. An untimed lock could park us here across StopSurfaceWorker (which
    // runs on the render thread WHILE it holds this mutex): its join timed out
    // and it freed everything under us (0xC0000409 on mirror disable,
    // 2026-08-21). The predecessor of this went the other way and used
    // try_lock, which never enqueues -- see the note on m_mirrorEngineMutex for
    // why that starved the worker and what the yield it needed then cost.
    LARGE_INTEGER lockA;
    QueryPerformanceCounter(&lockA);
    std::unique_lock<std::timed_mutex> lk(m_mirrorEngineMutex, std::defer_lock);
    while (!lk.try_lock_for(std::chrono::milliseconds(50))) {
        if (m_bMirrorThreadQuit.load() || surf.quit.load())
            return;
    }
    LARGE_INTEGER lockB;
    QueryPerformanceCounter(&lockB);
    const int lockUs = usSince(lockA.QuadPart, lockB.QuadPart);
    // Re-check after acquisition: a Stop may have freed members while we spun.
    if (m_bMirrorThreadQuit.load() || surf.quit.load())
        return;
    surf.lastOrientQpc = qpcNow.QuadPart;

    // The fence gate above proved last iteration's orient frame complete on
    // the GPU - publish its disp[] index for the render thread's blit and
    // advance the triple rotation. All panel blits/presents stay on the
    // render thread (worker-side main-queue submits deadlocked D3D12Core's
    // queue mutex; mirror-queue writes to flip buffers TDR'd - 2026-08-21).
    if (surf.lastWrite >= 0 && surf.WorkIdle()) {
        surf.publishedIdx.store(surf.lastWrite, std::memory_order_release);
        // Slot first, then the counter: a reader that sees the new count is
        // guaranteed to read at least this slot (and a slot one newer is just
        // as valid - the triple rotation keeps both fence-complete).
        surf.publishedSeq.fetch_add(1, std::memory_order_release);
        surf.pipe.dispWrite = (surf.lastWrite + 1) % 3;
    }

    m_bOrientOppositeAspect = true;
    m_lpDX->BeginAuxUpload();
    // This surface records into ITS OWN list and signals ITS OWN fence
    // (#186 phase 5b) -- which is what makes a thread per surface possible at
    // all: two threads cannot record into one list, and one shared fence
    // cannot say whose frame is done.
    surf.pendingWrite = -1;
    bool recorded = false;
    ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
    if (surf.workList && surf.workAlloc && surf.workFence &&
        SUCCEEDED(surf.workAlloc->Reset()) &&
        SUCCEEDED(surf.workList->Reset(surf.workAlloc.Get(), nullptr))) {
        ID3D12GraphicsCommandList* cmd = surf.workList.Get();
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

        const bool ok = m_bShadertoyMode
            ? RenderMilk3OrientPipeline(surf, cmd, needW, needH)
            : RenderClassicOrientPipeline(surf, cmd, needW, needH);
        if (FAILED(cmd->Close()) || !ok) {
            surf.imageReady.store(false);
        } else {
            ID3D12CommandList* lists[] = { cmd };
            m_mirrorQueue->ExecuteCommandLists(1, lists);
            surf.workSubmitted++;
            m_mirrorQueue->Signal(surf.workFence.Get(), surf.workSubmitted);
            // Which disp[] face this frame wrote; published at the NEXT
            // iteration, once this surface's own fence proves it done.
            surf.pendingWrite = surf.pipe.dispWrite % 3;
            surf.lastWrite = surf.pendingWrite;
            if (surf.pipe.frames >= 2)
                surf.imageReady.store(true);
            recorded = true;
        }
    }
    m_lpDX->EndAuxUpload();
    m_bOrientOppositeAspect = false;

    if (!recorded)
        return;
    m_nOrientFrameAccum.fetch_add(1, std::memory_order_relaxed);

    // -- Window accounting: dt = interval between completed worker frames --
    LARGE_INTEGER doneQpc;
    QueryPerformanceCounter(&doneQpc);
    const int recUs = usSince(lockB.QuadPart, doneQpc.QuadPart);
    if (s_dw.lastDone != 0) {
        const int dtUs = usSince(s_dw.lastDone, doneQpc.QuadPart);
        s_dw.n++;
        s_dw.dtSum += dtUs;
        if (dtUs < s_dw.dtMin) s_dw.dtMin = dtUs;
        if (dtUs > s_dw.dtMax) s_dw.dtMax = dtUs;
        s_dw.lockSum += lockUs;
        if (lockUs > s_dw.lockMax) s_dw.lockMax = lockUs;
        s_dw.recSum += recUs;
        if (recUs > s_dw.recMax) s_dw.recMax = recUs;
        s_dw.simSum += simUs;
        if (simUs > s_dw.simMax) s_dw.simMax = simUs;
    }
    s_dw.lastDone = doneQpc.QuadPart;
    if (s_dw.winStart == 0)
        s_dw.winStart = doneQpc.QuadPart;
    if (usSince(s_dw.winStart, doneQpc.QuadPart) >= 1000000) {
        const int n = s_dw.n > 0 ? s_dw.n : 1;
        m_diagOrientDtMinUs.store(s_dw.dtMin == INT_MAX ? 0 : s_dw.dtMin, std::memory_order_relaxed);
        m_diagOrientDtMaxUs.store(s_dw.dtMax, std::memory_order_relaxed);
        m_diagOrientDtAvgUs.store((int)(s_dw.dtSum / n), std::memory_order_relaxed);
        m_diagOrientLockMaxUs.store(s_dw.lockMax, std::memory_order_relaxed);
        m_diagOrientLockAvgUs.store((int)(s_dw.lockSum / n), std::memory_order_relaxed);
        m_diagOrientRecMaxUs.store(s_dw.recMax, std::memory_order_relaxed);
        m_diagOrientRecAvgUs.store((int)(s_dw.recSum / n), std::memory_order_relaxed);
        m_diagOrientSimMaxUs.store(s_dw.simMax, std::memory_order_relaxed);
        m_diagOrientSimAvgUs.store((int)(s_dw.simSum / n), std::memory_order_relaxed);
        m_diagOrientFenceWaits.store(s_dw.fence, std::memory_order_relaxed);
        s_dw = DiagWin{};
        s_dw.lastDone = doneQpc.QuadPart;
        s_dw.winStart = doneQpc.QuadPart;
    }
}

void Engine::CopyPrimaryToMirrorSrc()
{
    m_bMirrorSrcCopiedThisFrame = false;
    if (!m_bMirrorsActive || !m_lpDX || !m_lpDX->m_commandList || !m_lpDX->m_device)
        return;

    bool anyMirror = false;
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor || !out.config.bEnabled)
            continue;
        if (out.bSkippedSameMonitor)
            continue;
        if (out.monitorState && out.monitorState->bReady && !out.monitorState->bSoftDisabled)
            anyMirror = true;
        else if (!out.monitorState)
            anyMirror = true; // will Init this frame
    }
    if (!anyMirror)
        return;

    auto* cmd = m_lpDX->m_commandList.Get();
    ID3D12Resource* bb = m_lpDX->m_renderTargets[m_lpDX->m_frameIndex].Get();
    if (!cmd || !bb)
        return;

    // CopyResource requires exact size match. Client size can disagree with the
    // flip BB for a frame after fullscreen (that mismatch TDRs the GPU).
    const D3D12_RESOURCE_DESC bbDesc = bb->GetDesc();
    const UINT bw = (UINT)bbDesc.Width;
    const UINT bh = bbDesc.Height;
    if (bw == 0 || bh == 0)
        return;

    static UINT s_mirrorSrcEpoch = UINT_MAX;
    const bool srcStale = !m_mirrorSrcTex.IsValid() ||
        m_mirrorSrcTex.width != bw ||
        m_mirrorSrcTex.height != bh ||
        s_mirrorSrcEpoch != m_lpDX->m_descriptorEpoch;
    if (srcStale) {
        m_mirrorSrcTex.Reset();
        m_mirrorSrcTex = m_lpDX->CreateRenderTargetTexture(
            bw, bh, DXGI_FORMAT_R8G8B8A8_UNORM);
        s_mirrorSrcEpoch = m_lpDX->m_descriptorEpoch;
        if (!m_mirrorSrcTex.IsValid())
            return;
    }
    if (!m_mirrorSrcTex.resource ||
        m_mirrorSrcTex.width != bw || m_mirrorSrcTex.height != bh)
        return;

    D3D12_RESOURCE_BARRIER bars[2] = {};
    bars[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[0].Transition.pResource = bb;
    bars[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bars[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bars[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bars[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[1].Transition.pResource = m_mirrorSrcTex.resource.Get();
    bars[1].Transition.StateBefore = m_mirrorSrcTex.currentState;
    bars[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    bars[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (bars[1].Transition.StateBefore == bars[1].Transition.StateAfter)
        cmd->ResourceBarrier(1, &bars[0]);
    else
        cmd->ResourceBarrier(2, bars);

    cmd->CopyResource(m_mirrorSrcTex.resource.Get(), bb);
    m_mirrorSrcTex.currentState = D3D12_RESOURCE_STATE_COPY_DEST;

    bars[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bars[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bars[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bars[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmd->ResourceBarrier(2, bars);
    m_mirrorSrcTex.currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_bMirrorSrcCopiedThisFrame = true;
}

void Engine::ResizeMirrorSwapChain(MonitorMirrorState& ms, int newW, int newH)
{
    // Prefer soft-recreate (caller sets bSoftDisabled) over ResizeBuffers+WaitForGpu.
    // Kept for rare callers; never block the GPU for 5s here.
    if (!ms.swapChain || !m_lpDX || newW <= 0 || newH <= 0) {
        ms.bReady = false;
        return;
    }
    if (ms.width == newW && ms.height == newH && ms.bHasRtv)
        return;

    if (m_lpDX->m_device && m_lpDX->m_device->GetDeviceRemovedReason() != S_OK) {
        m_lpDX->DumpDredToLog("MirrorResize/pre");
        ms.bReady = false;
        return;
    }

    // GPU must be idle before releasing back buffers
    m_lpDX->WaitForGpu();
    if (m_lpDX->m_device && m_lpDX->m_device->GetDeviceRemovedReason() != S_OK) {
        m_lpDX->DumpDredToLog("MirrorResize/postWait");
        ms.bReady = false;
        return;
    }

    for (UINT i = 0; i < MIRROR_BUFFER_COUNT; i++)
        ms.backBuffers[i].Reset();
    ms.bHasRtv = false;

    // Must match CreateSwapChain flags (copy-mode SCs are created with Flags=0).
    UINT scFlags = 0;
    UINT nBuf = ms.bufferCount ? ms.bufferCount : MIRROR_BUFFER_COUNT;
    HRESULT hr = ms.swapChain->ResizeBuffers(
        nBuf, (UINT)newW, (UINT)newH,
        DXGI_FORMAT_R8G8B8A8_UNORM, scFlags);
    if (FAILED(hr)) {
        char logBuf[256]; sprintf(logBuf, "ResizeMirrorSwapChain: ResizeBuffers failed (0x%08X)\n", (unsigned)hr);
        DebugLogA(logBuf, LOG_ERROR);
        ms.bReady = false;
        return;
    }

    for (UINT i = 0; i < nBuf && i < MIRROR_BUFFER_COUNT; i++) {
        hr = ms.swapChain->GetBuffer(i, IID_PPV_ARGS(&ms.backBuffers[i]));
        if (FAILED(hr)) {
            ms.bReady = false;
            return;
        }
        if (ms.rtvSlotBase != UINT_MAX) {
            ms.rtvHandles[i] = m_lpDX->GetRtvCpuHandleAt(ms.rtvSlotBase + i);
            m_lpDX->m_device->CreateRenderTargetView(
                ms.backBuffers[i].Get(), nullptr, ms.rtvHandles[i]);
        }
    }
    ms.bHasRtv = (ms.rtvSlotBase != UINT_MAX);
    ms.rtvEpoch = m_lpDX->m_descriptorEpoch;
    ms.width = newW;
    ms.height = newH;
    ms.bEverPresented = false;
    ms.bNeedsFullChainClear = true;
    ms.paintedBufferMask = 0;
    for (UINT bi = 0; bi < MIRROR_BUFFER_COUNT; bi++)
        ms.bbState[bi] = D3D12_RESOURCE_STATE_COMMON;
}

// ─── Per-Frame Send ───────────────────────────────────────────────────────────

// One preset from a display's directory.
//
// A plain directory walk rather than a call into the engine's preset list, for
// the same reason PickRandomPresetFile is one: m_presets belongs to the render
// thread and describes the PARENT's directory, while a display may be pointed
// at any other. Top level only, matching what a child was given.
void Engine::StepOwnPresetDisplays(bool next)
{
    for (auto& out : m_displayOutputs) {
        if (!DisplayHoldsOwnPreset(out.config)) continue;
        const wchar_t* err = nullptr;
        std::wstring detail;
        // Failure is ordinary here and is not reported: a display whose folder
        // is empty, or whose surface has not been created yet, simply does not
        // step. The addressed verb says so to its caller; a global Next has no
        // one to say it to.
        AdvanceDisplayPreset(out, next, err, detail);
    }
}

bool Engine::AdvanceDisplayPreset(DisplayOutput& out, bool next,
                                  const wchar_t*& err, std::wstring& detail)
{
    // Sequential order steps one either way; random order ignores the
    // direction, which is what "previous" means when there is no order to step
    // back through.
    MirrorSurface* ps = MirrorSurfaceFor(out.config.szDeviceName);
    if (!ps) {
        err = L"no_surface";
        detail = L"display has no render surface yet; enable it first";
        return false;
    }
    const std::wstring dir = ResolveDisplayPresetDir(out.config);
    int idx = ps->seqIndex.load();
    if (out.config.bSequentialOrder && !next)
        idx -= 2;               // PickDisplayPreset advances by one
    const std::wstring pick =
        PickDisplayPreset(dir, out.config.bSequentialOrder, idx);
    ps->seqIndex.store(idx);
    if (pick.empty()) {
        err = L"no_presets";
        detail = dir;
        return false;
    }
    PostSurfacePreset(*ps, pick);
    // Restart the cycle clock, so stepping by hand does not leave the display
    // advancing again a fraction of a second later.
    const float interval = EffectiveChildInterval(out.config);
    ps->nextPresetAt = interval > 0.0f ? GetTime() + interval : -1.0;
    return true;
}

std::wstring Engine::PickDisplayPreset(const std::wstring& dir, bool sequential,
                                       int& seqIndex) const
{
    if (dir.empty())
        return std::wstring();
    std::wstring base = dir;
    if (base.back() != L'\\' && base.back() != L'/')
        base += L'\\';

    // One pass with an explicit extension test rather than a mask per
    // extension: FindFirstFile matches 8.3 short names too, so three masks
    // would need dedup afterwards.
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"*.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return std::wstring();
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        const wchar_t* dot = wcsrchr(fd.cFileName, L'.');
        if (!dot)
            continue;
        if (_wcsicmp(dot, L".milk") == 0 || _wcsicmp(dot, L".milk2") == 0 ||
            _wcsicmp(dot, L".milk3") == 0)
            files.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (files.empty())
        return std::wstring();

    // Sorted, so "sequential" means the same order every run and on every
    // display -- an unsorted FindFirstFile order is stable in practice but is
    // not promised, and a display that reordered itself between runs would
    // look like it had skipped presets.
    std::sort(files.begin(), files.end(),
              [](const std::wstring& a, const std::wstring& b) {
                  return _wcsicmp(a.c_str(), b.c_str()) < 0;
              });

    size_t idx;
    if (sequential) {
        seqIndex = (seqIndex < 0) ? 0 : seqIndex + 1;
        if ((size_t)seqIndex >= files.size())
            seqIndex = 0;
        idx = (size_t)seqIndex;
    } else {
        idx = (size_t)(rand() % (int)files.size());
        seqIndex = (int)idx;   // so a later switch to sequential carries on here
    }
    return base + files[idx];
}

// One surface's recording objects. Idempotent, render thread.
//
// DRED attribution names the list after the display, so a device-removed dump
// says which surface was recording rather than "MirrorWorkList".
bool Engine::EnsureSurfaceWorkObjects(MirrorSurface& surf)
{
    if (!m_lpDX || !m_lpDX->m_device)
        return false;
    if (surf.workList && surf.workAlloc && surf.workFence)
        return true;

    if (!surf.workFence &&
        FAILED(m_lpDX->m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                             IID_PPV_ARGS(&surf.workFence)))) {
        DebugLogA("mirror surface: CreateFence failed\n", LOG_ERROR);
        return false;
    }
    if (!surf.workAlloc &&
        FAILED(m_lpDX->m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&surf.workAlloc)))) {
        DebugLogA("mirror surface: CreateCommandAllocator failed\n", LOG_ERROR);
        return false;
    }
    if (!surf.workList &&
        FAILED(m_lpDX->m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            surf.workAlloc.Get(), nullptr, IID_PPV_ARGS(&surf.workList)))) {
        DebugLogA("mirror surface: CreateCommandList failed\n", LOG_ERROR);
        return false;
    }
    std::wstring name = L"MirrorWork:" + (surf.device.empty() ? L"(none)" : surf.device);
    surf.workList->SetName(name.c_str());
    surf.workList->Close();
    if (!surf.fenceEvt)
        surf.fenceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    surf.workSubmitted = 0;
    return true;
}

void Engine::ReleaseSurfaceWorkObjects(MirrorSurface& surf)
{
    surf.workList.Reset();
    surf.workAlloc.Reset();
    surf.workFence.Reset();
    surf.workSubmitted = 0;
    if (surf.fenceEvt) {
        CloseHandle(surf.fenceEvt);
        surf.fenceEvt = nullptr;
    }
}

std::shared_ptr<Engine::MirrorSurface> Engine::EnsureMirrorSurface(const wchar_t* device)
{
    // ONE locked section, and deliberately not via MirrorSurfaceFor: that takes
    // the same non-recursive mutex, so calling it from in here would deadlock.
    std::lock_guard<std::mutex> lock(m_mirrorSurfacesMutex);
    for (auto& sp : m_mirrorSurfaces)
        if (_wcsicmp(sp->device.c_str(), device ? device : L"") == 0)
            return sp;
    // An unkeyed surface may already exist: OnlyMirrorSurface() materialises
    // one lazily for the diagnostics, and it is the same object this display
    // should adopt rather than a second one beside it.
    for (auto& sp : m_mirrorSurfaces) {
        if (sp->device.empty()) {
            sp->device = device ? device : L"";
            return sp;
        }
    }
    m_mirrorSurfaces.emplace_back(std::make_shared<MirrorSurface>());
    m_mirrorSurfaces.back()->device = device ? device : L"";
    return m_mirrorSurfaces.back();
}

void Engine::RetireMirrorSurfacesExcept(const std::vector<std::wstring>& keep)
{
    // Detach under the lock, release outside it. WaitForGpu can block for a
    // whole frame, and holding the surfaces mutex across it would stall the
    // worker's snapshot -- which is taken every wake.
    std::vector<std::shared_ptr<MirrorSurface>> retired;
    {
        std::lock_guard<std::mutex> lock(m_mirrorSurfacesMutex);
        for (size_t i = m_mirrorSurfaces.size(); i-- > 0 && m_mirrorSurfaces.size() > 1; ) {
            auto& sp = m_mirrorSurfaces[i];
            // Never retire the last one, and never an unkeyed one: the
            // diagnostics read a surface unconditionally through
            // OnlyMirrorSurface(), so an empty list would mean an empty-check
            // at every one of those sites for no gain. An idle surface holds no
            // pipe, and therefore no VRAM.
            bool wanted = sp->device.empty();
            for (const auto& k : keep)
                if (_wcsicmp(k.c_str(), sp->device.c_str()) == 0) { wanted = true; break; }
            if (wanted)
                continue;
            retired.push_back(sp);
            m_mirrorSurfaces.erase(m_mirrorSurfaces.begin() + i);
        }
    }
    if (retired.empty())
        return;
    // Stop the threads FIRST, before the WaitForGpu and before anything is
    // freed. A worker mid-record would otherwise have its pipe released under
    // it -- and it is holding a shared_ptr, so the surface itself survives,
    // which makes the failure a use-after-free of the D3D objects rather than
    // an obvious null.
    for (auto& sp : retired)
        StopSurfaceWorker(*sp);
    // The shared_ptrs above keep these alive even if a worker snapshotted
    // them a moment ago; their pipes hold D3D objects the GPU may still read.
    m_lpDX->WaitForGpu();
    for (auto& sp : retired) {
        ReleaseOrientPipeline(*sp);
        ReleaseSurfaceWorkObjects(*sp);
    }
}

void Engine::SendToDisplayOutputs()
{
    if (!m_lpDX) return;

    // Free any mirror handed over by a re-enumeration before doing anything
    // else — their RTV blocks are needed by the InitDisplayOutput below.
    DrainOrphanedMirrors();

    // Mirror throughput, sampled once a second. orient is the opposite-orient
    // worker (its own thread, own rate); present is mirror swap-chain flips.
    // A large gap between them means panels are re-showing the same orient frame.
    {
        // Primary frame interval (render thread) — the jitter baseline the
        // mirror is compared against.
        static LONGLONG s_pqf = [] {
            LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart;
        }();
        static LONGLONG s_pLast = 0, s_pWinStart = 0, s_pSum = 0;
        static int s_pN = 0, s_pMin = INT_MAX, s_pMax = 0;
        // Parity pacing token for the mirror worker (see m_nPrimaryFrameSeq),
        // plus a direct wake. Left to its 2 ms poll the worker found the new
        // count anywhere inside the primary's period, so its step interval
        // wobbled 9.5-40 ms around a 25 ms mean — the right number of frames,
        // at the wrong moments. This lands it right after the primary frame,
        // while the fps limiter is sleeping and the engine lock is free.
        m_nPrimaryFrameSeq.fetch_add(1, std::memory_order_release);
        if (m_bMirrorsActive) {
            // One event per surface since 5c: every worker is waiting on its
            // own, and all of them are pacing off the count just published.
            for (auto& sp : SnapshotMirrorSurfaces())
                if (sp->hWake)
                    SetEvent(sp->hWake);
        }
        LARGE_INTEGER pNow;
        QueryPerformanceCounter(&pNow);
        if (s_pLast != 0) {
            const int dtUs = (int)((pNow.QuadPart - s_pLast) * 1000000 / s_pqf);
            s_pN++; s_pSum += dtUs;
            if (dtUs < s_pMin) s_pMin = dtUs;
            if (dtUs > s_pMax) s_pMax = dtUs;
        }
        s_pLast = pNow.QuadPart;
        if (s_pWinStart == 0) s_pWinStart = pNow.QuadPart;
        if ((pNow.QuadPart - s_pWinStart) * 1000000 / s_pqf >= 1000000) {
            const int n = s_pN > 0 ? s_pN : 1;
            m_diagPrimDtMinUs.store(s_pMin == INT_MAX ? 0 : s_pMin, std::memory_order_relaxed);
            m_diagPrimDtMaxUs.store(s_pMax, std::memory_order_relaxed);
            m_diagPrimDtAvgUs.store((int)(s_pSum / n), std::memory_order_relaxed);
            s_pSum = 0; s_pN = 0; s_pMin = INT_MAX; s_pMax = 0;
            s_pWinStart = pNow.QuadPart;
        }

        const DWORD nowMs = GetTickCount();
        if (m_dwMirrorFpsTick == 0)
            m_dwMirrorFpsTick = nowMs;
        const DWORD elapsed = nowMs - m_dwMirrorFpsTick;
        if (elapsed >= 1000) {
            const float secs = (float)elapsed / 1000.0f;
            const unsigned o = m_nOrientFrameAccum.exchange(0, std::memory_order_relaxed);
            const unsigned pr = m_nMirrorPresentAccum.exchange(0, std::memory_order_relaxed);
            // PER SURFACE, which is what this always meant. One worker used to
            // record every surface and bump the counter once, so the raw total
            // WAS the per-surface rate; with a thread per surface (#186 phase
            // 5c) each bumps it for itself, and undivided the same two mirrors
            // would read as a doubled frame rate. That exact mistake is already
            // recorded against mirrorFps in bench_children_vs_mirrors, where a
            // summed counter "proved" a 2x speedup.
            const unsigned surfaces = (unsigned)MirrorSurfaceCount();
            m_fOrientFps = (float)o / secs / (float)(surfaces > 0 ? surfaces : 1);
            m_fMirrorPresentFps = (float)pr / secs;
            m_dwMirrorFpsTick = nowMs;
            if (m_bMirrorsActive) {
                char logBuf[160];
                sprintf(logBuf, "MirrorFPS: orient=%.1f present=%.1f\n",
                        m_fOrientFps, m_fMirrorPresentFps);
                DebugLogA(logBuf, LOG_INFO);
            }
        }
    }

    // Primary geom changed (resize / Windows primary-monitor switch): never leave
    // orient SizeGuard aspect or opposite-orient warp mesh on the primary path.
    if (m_bPrimaryGeomDirty.exchange(false)) {
        ClearOutputSizeOverride();
        RestorePrimaryTexSizeFromVS();
        // Soft orient reset — keep RTs if size still matches; force warmup clear.
        for (auto& sp : m_mirrorSurfaces) {
            if (sp->pipe.ready) {
                sp->pipe.frames = 0;
                sp->pipe.fbIdx = 0;
            }
        }
        // Drop primary-BB copy so next frame rebuilds at the new size (stale
        // m_mirrorSrcTex after landscape↔portrait caused wrong stretch samples).
        m_mirrorSrcTex.Reset();
        m_bMirrorSrcCopiedThisFrame = false;
        m_bMirrorResetOrientNextFrame.store(true);
        m_nDeferMirrorResize.store(8);
        DebugLogA("SendToDisplayOutputs: primary geom dirty — skip mirror init/draw this frame\n", LOG_INFO);
        return;
    }

    // Finish deferred activate only on a stable-size frame (not the FS resize).
    {
        int defer = m_nDeferMirrorActivate.load();
        if (defer > 0) {
            if (m_nDeferMirrorActivate.fetch_sub(1) == 1 && !m_bMirrorsActive) {
                m_bMirrorsActive = true;
                m_bMirrorStylesDirty.store(true);
                m_bRaiseMirrorsNextFrame.store(true);
                DebugLogA("SendToDisplayOutputs: deferred mirror activate\n", LOG_INFO);
            } else {
                return;
            }
        }
    }

    // Apply deferred mirror style changes (set by UI thread via m_bMirrorStylesDirty)
    if (m_bMirrorStylesDirty.exchange(false))
        ApplyMirrorWindowStyles();

    // Apply pending fullscreen/windowed layout (size may require SC resize)
    {
        bool anyLayout = false;
        for (auto& out : m_displayOutputs) {
            if (out.monitorState && out.monitorState->bPendingLayout) {
                anyLayout = true;
                break;
            }
        }
        if (anyLayout) {
            for (auto& out : m_displayOutputs) {
                if (!out.monitorState || !out.monitorState->bPendingLayout)
                    continue;
                auto& ms = *out.monitorState;
                ms.bPendingLayout = false;
                if (!ms.hWnd || !ms.swapChain) continue;
                const int x = ms.pendingX, y = ms.pendingY;
                const int w = ms.pendingW, h = ms.pendingH;
                if (w <= 0 || h <= 0) continue;
                SetWindowPos(ms.hWnd, m_bAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                             x, y, w, h, SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                if (ms.width != w || ms.height != h)
                    ResizeMirrorSwapChain(ms, w, h);
            }
        }
    }

    // Soft reset orient history when re-entering independent (do NOT free RTs —
    // CreateRenderTargetTexture RTV slots are never reclaimed; free+recreate
    // exhausted the dynamic RTV heap → permanent black until full app restart.
    if (m_bMirrorResetOrientNextFrame.exchange(false)) {
        for (auto& sp : m_mirrorSurfaces) {
            if (sp->pipe.ready) {
                sp->pipe.frames = 0;
                sp->pipe.fbIdx = 0;
            }
        }
    }

    // After mirror on (and first Init of HWNDs), bring surfaces to front once.
    const bool raiseThisFrame = m_bRaiseMirrorsNextFrame.exchange(false);

    // Force reinit: tear down session-lived monitor mirrors so SC/window style
    // changes (layered, flip model, buffer count) actually take effect.
    // Without this, InitDisplayOutput early-returns and leaves the old SC forever.
    // NOTE: Independent toggle must NOT set this flag — only opacity/layer style.
    if (m_bMirrorForceReinit.exchange(false)) {
        for (auto& out : m_displayOutputs) {
            if (out.config.type == DisplayOutputType::Monitor && out.monitorState)
                DestroyDisplayOutput(out);
        }
        ReleaseAllOrientPipelines();
        m_bRaiseMirrorsNextFrame.store(true);
        DebugLogA("SendToDisplayOutputs: force-reinit destroyed monitor mirrors for recreate\n", LOG_WARN);
    }

    // After resize / ResetDynamicDescriptors the SRV bump rewinds — letterbox block
    // must be reallocated (RTVs use a permanent reserve and only need re-CreateRTV).
    if (m_mirrorLetterboxSrvEpoch != m_lpDX->m_descriptorEpoch) {
        m_mirrorLetterboxSrvBase = UINT_MAX;
        m_mirrorLetterboxSrvEpoch = m_lpDX->m_descriptorEpoch;
    }

    int mainW = m_lpDX->m_client_width;
    int mainH = m_lpDX->m_client_height;
    UINT fi = m_lpDX->m_frameIndex;
    bool hasActiveMonitors = false;

    // Primary monitor detection (never mirror onto the render window's display)
    wchar_t renderDevice[32] = {};
    if (m_lpDX->GetHwnd()) {
        if (m_bMirrorWatermarkActive && m_szWatermarkRenderDevice[0]) {
            CopyTo(renderDevice, m_szWatermarkRenderDevice);
        } else {
            HMONITOR hRenderMon = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW rmi = { sizeof(rmi) };
            if (hRenderMon && GetMonitorInfoW(hRenderMon, &rmi))
                CopyTo(renderDevice, rmi.szDevice);
        }
    }
    const bool gotDevice = renderDevice[0] != L'\0';

    // ── Session-lived mirrors: show/hide only — never Destroy on per-monitor toggle ──
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor)
            continue;

        const bool isPrimary = gotDevice &&
            wcscmp(out.config.szDeviceName, renderDevice) == 0;
        if (isPrimary)
            out.bSkippedSameMonitor = true;
        else if (m_bMirrorsActive && out.config.bEnabled)
            out.bSkippedSameMonitor = false;

        // A display holding its own preset used to be driven by a SEPARATE
        // PROCESS, so the mirror had to stand off its pixels or the primary's
        // frame landed on top of what the child was rendering (forgejo#38) --
        // and only once the child was READY, so the monitor never went blank
        // waiting for a spawn.
        //
        // #186 makes that suppression meaningless in the best way: an
        // own-preset display is the same mirror surface as any other, holding a
        // different preset. There is nothing to stand off from, and no handover
        // to time.
        const bool wantVisible = m_bMirrorsActive && out.config.bEnabled &&
            !out.bSkippedSameMonitor;

        if (!out.monitorState) {
            // Create once when first needed
            if (wantVisible)
                InitDisplayOutput(out);
        }

        if (!out.monitorState || !out.monitorState->bReady)
            continue;

        auto& ms = *out.monitorState;
        if (wantVisible) {
            ms.bSoftDisabled = false;
            ms.nFramesUntilHardDestroy = 0;
            // Re-bind RTVs after any descriptor epoch change (window resize rebuilds
            // dynamic RTVs and used to overwrite bump-allocated mirror slots → SEH crash).
            if (ms.bHasRtv && ms.rtvSlotBase != UINT_MAX &&
                ms.rtvEpoch != m_lpDX->m_descriptorEpoch) {
                for (UINT i = 0; i < ms.bufferCount && i < MIRROR_BUFFER_COUNT; i++) {
                    if (!ms.backBuffers[i])
                        continue;
                    ms.rtvHandles[i] = m_lpDX->GetRtvCpuHandleAt(ms.rtvSlotBase + i);
                    m_lpDX->m_device->CreateRenderTargetView(
                        ms.backBuffers[i].Get(), nullptr, ms.rtvHandles[i]);
                }
                ms.rtvEpoch = m_lpDX->m_descriptorEpoch;
                DebugLogA("SendToDisplayOutputs: refreshed mirror RTVs after descriptor rewind\n", LOG_WARN);
            }
            if (ms.hWnd) {
                // Raise only on hide→show (monitor just enabled). Never every frame
                // (DWM thrashing floors FPS at ~30). Same TOPMOST→NOTOPMOST hop as
                // RaiseMirrorSurfaces so the surface is frontmost without sticky AOT
                // unless Always On Top is on.
                const bool wasHidden = !IsWindowVisible(ms.hWnd);
                if (wasHidden) {
                    ShowWindow(ms.hWnd, SW_SHOWNOACTIVATE);
                    const UINT zflags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
                    SetWindowPos(ms.hWnd, HWND_TOPMOST, 0, 0, 0, 0, zflags);
                    if (!m_bAlwaysOnTop)
                        SetWindowPos(ms.hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, zflags);
                }
            }
            hasActiveMonitors = true;
        } else {
            // Park the mirror: hide + skip draws. Keep SC alive for the session.
            ms.bSoftDisabled = true;
            if (ms.hWnd && IsWindowVisible(ms.hWnd))
                ShowWindow(ms.hWnd, SW_HIDE);
        }
    }

    // Independent opposite-aspect SCs are capped native; copy-mode is primary
    // size. Resize on the render thread only (UI must not WaitForGpu).
    if (m_nDeferMirrorResize.load() > 0)
        m_nDeferMirrorResize.fetch_sub(1);
    if (m_nDeferMirrorResize.load() <= 0 && m_bMirrorIndepSizeDirty.exchange(false)) {
        // m_mirrorWorkFence stood here, and phase 5b stopped signalling it
        // when each surface got its own -- so this gate had quietly become
        // "always idle" and resizes went ahead over in-flight orient work.
        if (!AllSurfacesIdle()) {
            m_bMirrorIndepSizeDirty.store(true); // a worker is still presenting
        } else {
        const int primW = m_lpDX->m_client_width;
        const int primH = m_lpDX->m_client_height;
        const bool lagIdleForResize = LagIndepFenceIdle();
        for (auto& out : m_displayOutputs) {
            if (out.config.type != DisplayOutputType::Monitor || !out.monitorState)
                continue;
            if (!out.monitorState->bReady || !out.monitorState->swapChain)
                continue;
            int wantW = primW, wantH = primH;
            MirrorSwapChainSize(out, primW, primH, wantW, wantH);
            if (wantW > 0 && wantH > 0 &&
                (out.monitorState->width != wantW || out.monitorState->height != wantH)) {
                if (!lagIdleForResize && out.config.bIndependentRender) {
                    m_bMirrorIndepSizeDirty.store(true); // retry when lag fence is idle
                    continue;
                }
                ResizeMirrorSwapChain(*out.monitorState, wantW, wantH);
            }
        }
        }
    }

    // Mirror on: raise mirror HWNDs here (render-owned). Ask UI thread to raise primary
    // — never SetWindowPos the primary from this thread (cross-thread deadlock).
    if (raiseThisFrame) {
        RaiseMirrorSurfaces(nullptr); // mirrors only
        HWND hPrimary = m_lpDX->GetHwnd();
        if (hPrimary)
            PostMessage(hPrimary, WM_MW_RAISE_PRIMARY, 0, 0);
    }

    // Never hard-destroy monitor mirrors while the app is running — CreateSwapChain
    // thrash is what locked the PC after two toggles. Park = hide only.

    // Spout cleanup when disabled
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor &&
            !out.config.bEnabled && out.spoutState)
            DestroyDisplayOutput(out);
    }

    // Spout send
    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Spout)
            continue;
        if (!out.spoutState || !out.spoutState->bReady) {
            if (!out.spoutState)
                InitDisplayOutput(out);
            if (!out.spoutState || !out.spoutState->bReady)
                continue;
        }
        out.spoutState->sender.Send(out.spoutState->wrappedBackBuffers[fi]);
    }

    // Tell the orient worker whether anything still wants its frames. It has no
    // consumer test of its own, and without one it keeps simulating into a
    // surface nobody reads -- see MirrorThreadDrawAndPresent.
    m_bAnyMirrorTargetLive.store(hasActiveMonitors, std::memory_order_release);

    if (!hasActiveMonitors) {
        // Restore default DXGI latency after a multi-mirror session (stuck high
        // latency was observed as a permanent ~30–33 FPS ceiling).
        m_lpDX->m_bSerializeWithMirrors = false;
        m_lpDX->EnsureMultiSwapChainFrameLatency(1, false);
        return;
    }

    // Stretch/copy on the *same* queue (pre-July-29 path). CopyResource when
    // the mirror SC matches the flip BB; stretch-blit otherwise. Present is
    // non-blocking. Independent re-render is not in this path.
    ID3D12Resource* flipBB = m_lpDX->m_renderTargets[fi].Get();
    if (!flipBB)
        return;
    const D3D12_RESOURCE_DESC flipDesc = flipBB->GetDesc();
    const int bbW = (int)flipDesc.Width;
    const int bbH = (int)flipDesc.Height;
    if (bbW <= 0 || bbH <= 0)
        return;

    if (!m_mirrorCmdAllocators[0]) {
        for (int i = 0; i < DXC_FRAME_COUNT; i++) {
            if (FAILED(m_lpDX->m_device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_mirrorCmdAllocators[i]))))
                return;
        }
        if (FAILED(m_lpDX->m_device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                m_mirrorCmdAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_mirrorCmdList))))
            return;
        m_mirrorCmdList->Close();
    }

    HRESULT hrAlloc = m_mirrorCmdAllocators[fi]->Reset();
    if (FAILED(hrAlloc)) {
        m_lpDX->WaitForGpu();
        hrAlloc = m_mirrorCmdAllocators[fi]->Reset();
    }
    if (FAILED(hrAlloc))
        return;
    HRESULT hrList = m_mirrorCmdList->Reset(m_mirrorCmdAllocators[fi].Get(), nullptr);
    if (FAILED(hrList)) {
        m_lpDX->WaitForGpu();
        hrList = m_mirrorCmdList->Reset(m_mirrorCmdAllocators[fi].Get(), nullptr);
    }
    if (FAILED(hrList))
        return;

    ID3D12GraphicsCommandList* cmd = m_mirrorCmdList.Get();
    ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());
    m_lpDX->BeginAuxUpload();

    // Prefer the pre-Execute snapshot (same size as flip BB). Else copy the flip BB.
    ID3D12Resource* src = flipBB;
    D3D12_RESOURCE_STATES srcState = D3D12_RESOURCE_STATE_PRESENT;
    int srcW = bbW, srcH = bbH;
    const bool haveSnap = m_bMirrorSrcCopiedThisFrame && m_mirrorSrcTex.IsValid() &&
        m_mirrorSrcTex.resource &&
        (int)m_mirrorSrcTex.width == bbW && (int)m_mirrorSrcTex.height == bbH;
    if (haveSnap) {
        src = m_mirrorSrcTex.resource.Get();
        srcState = m_mirrorSrcTex.currentState;
        srcW = (int)m_mirrorSrcTex.width;
        srcH = (int)m_mirrorSrcTex.height;
    }

    auto isPortrait = [](int w, int h) { return h > w; };
    const bool mainPortrait = isPortrait(bbW, bbH);

    // ── One surface per display that needs one (#186 phase 3) ────────────
    //
    // This loop used to pick a single WINNER -- the largest opposite-orient
    // panel -- size the one shared pipe to it, and let every other panel blit
    // that same image stretched to fit. Two consequences it is worth naming,
    // because they are what the collection fixes:
    //
    //  * a portrait and a landscape mirror alternately failed the pipe's w/h
    //    test, so EnsureOrientPipeline tore the pipe down and rebuilt it on
    //    every frame -- WaitForGpu included;
    //  * a panel of a different shape from the winner showed a stretched
    //    image, never one rendered at its own aspect.
    //
    // Each qualifying display now gets its own surface, sized to itself. They
    // all still follow the primary, so they render the same preset and the
    // picture is unchanged -- phase 4 is what gives them presets of their own.
    // #186 phase 4: a display that holds its OWN PRESET needs a surface
    // whatever its orientation. The opposite-orientation test above is about
    // whether the primary's image can simply be stretched onto the panel; a
    // display running a different preset cannot use the primary's image at any
    // orientation, so it renders for itself even when the shapes match.
    bool anyOppositeIndep = false;
    std::vector<std::wstring> wanted;
    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
            continue;
        const bool ownPreset = DisplayHoldsOwnPreset(out.config);
        if (!out.config.bIndependentRender && !ownPreset)
            continue;
        if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
            continue;
        int lx = 0, ly = 0, lw = 0, lh = 0;
        ComputeMirrorLayout(out.config, lx, ly, lw, lh);
        const bool port = (lw > 0 && lh > 0) ? isPortrait(lw, lh)
                                            : isPortrait(out.monitorState->width, out.monitorState->height);
        if (port == mainPortrait && !ownPreset)
            continue;
        anyOppositeIndep = true;
        wanted.push_back(out.config.szDeviceName);

        std::shared_ptr<MirrorSurface> msp = EnsureMirrorSurface(out.config.szDeviceName);
        MirrorSurface& ms = *msp;
        EnsureSurfaceWorkObjects(ms);
        // A display that starts mirroring mid-session gets its thread here --
        // StartMirrorThread only covers the surfaces that existed when mirrors
        // were switched on. Idempotent, so this is a no-op on every later pass.
        if (m_bMirrorsActive && !m_bMirrorThreadQuit.load())
            StartSurfaceWorker(msp);
        // Resolve the display's preset ONCE, not every frame: <Random> picks a
        // new file each time it is asked, so re-resolving per frame would
        // reload a different preset sixty times a second. After this the
        // surface owns the answer, and cycling replaces it deliberately.
        if (ownPreset) {
            const std::wstring dir = ResolveDisplayPresetDir(out.config);
            const float interval = EffectiveChildInterval(out.config);
            const bool  locked   = EffectiveChildPresetLock(out.config);
            const double now = GetTime();

            if (ms.nextPresetAt == 0.0 && ms.sim.ownPresetPath.empty() &&
                !ms.requestPending) {
                // First preset for this display. Resolved ONCE -- <Random>
                // answers differently every time it is asked, so resolving per
                // frame would reload a new preset sixty times a second.
                const std::wstring pick =
                    ResolveDisplayStartupPreset(out.config, dir.c_str());
                if (!pick.empty()) {
                    PostSurfacePreset(ms, pick);
                    ms.nextPresetAt = (interval > 0.0f) ? now + interval : -1.0;
                    DLOG_INFO("display %ls holds its own preset: %ls",
                              out.config.szDeviceName, pick.c_str());
                }
            } else if (!locked && interval > 0.0f && ms.nextPresetAt > 0.0 &&
                       now >= ms.nextPresetAt && !ms.requestPending) {
                // Its own timer, its own order. The lock is the RESOLVED one:
                // this display's if it has pinned a value, otherwise the main
                // window's, so the global lock still stops every display.
                int idx = ms.seqIndex.load();
                const std::wstring pick =
                    PickDisplayPreset(dir, out.config.bSequentialOrder, idx);
                ms.seqIndex.store(idx);
                if (!pick.empty()) {
                    PostSurfacePreset(ms, pick);
                    ms.nextPresetAt = now + interval;
                }
            } else if (interval > 0.0f && ms.nextPresetAt < 0.0) {
                // Cycling was off when this display started and has been turned
                // on since: give it a deadline rather than waiting for a
                // restart.
                ms.nextPresetAt = now + interval;
            } else if (interval <= 0.0f && ms.nextPresetAt > 0.0) {
                ms.nextPresetAt = -1.0;    // cycling switched off
            }
        } else if (!ms.sim.ownPresetPath.empty() || ms.requestPending) {
            // Turned back into a follower: an empty request puts it back on the
            // primary's bundle. Posted, not assigned -- the worker owns the sim.
            PostSurfacePreset(ms, std::wstring());
            ms.nextPresetAt = 0.0;
            ms.seqIndex.store(-1);
        }
        int rawW = (lw > 0) ? lw : out.monitorState->width;
        int rawH = (lh > 0) ? lh : out.monitorState->height;
        int tw = rawW, th = rawH;
        CapMirrorSwapChainDim(tw, th);
        ms.panelW.store(rawW);
        ms.panelH.store(rawH);

        // Latch the preset's canvas limit ONLY while this surface has no pipe,
        // then clamp with it. Adopting a new limit under a live pipe changes
        // the computed size and triggers a recreate, which hangs the GPU.
        if (!ms.pipe.ready)
            ms.canvasLimit = m_nCanvasLimitApplied;
        ClampOrientCanvas(ms, tw, th);
        ms.needW.store(tw);
        ms.needH.store(th);
    }
    // Displays that stopped qualifying give their VRAM back -- about 45 MB per
    // classic surface at 1920x1088, and roughly 270 MB for a .milk3 one.
    RetireMirrorSurfacesExcept(wanted);
    const bool workerIdle = AllSurfacesIdle();
    if (anyOppositeIndep && workerIdle) {
        // ONE WaitForGpu for the whole pass, however many surfaces need a pipe.
        // Per-surface it would be N stalls in a frame, and the wait is the
        // expensive half of a recreate.
        bool anyNeedsPipe = false;
        for (auto& sp : m_mirrorSurfaces) {
            const int nw = sp->needW.load(), nh = sp->needH.load();
            if (nw <= 0 || nh <= 0)
                continue;
            // Mode is part of "need": the worker is barred from rebuilding, so
            // if this test misses a reason the pipe is unusable, nobody
            // rebuilds it and the mirrors freeze. OrientPipeReadyForMode covers
            // ready-ness and the per-mode texture set together.
            if (!OrientPipeReadyForMode(*sp) ||
                sp->pipe.w != nw || sp->pipe.h != nh ||
                sp->pipe.bindEpoch != m_lpDX->m_descriptorEpoch) {
                anyNeedsPipe = true;
                break;
            }
        }
        if (anyNeedsPipe) {
            m_lpDX->WaitForGpu();
            for (auto& sp : m_mirrorSurfaces) {
                const int nw = sp->needW.load(), nh = sp->needH.load();
                if (nw <= 0 || nh <= 0)
                    continue;
                if (!OrientPipeReadyForMode(*sp) ||
                    sp->pipe.w != nw || sp->pipe.h != nh ||
                    sp->pipe.bindEpoch != m_lpDX->m_descriptorEpoch) {
                    EnsureOrientPipeline(*sp, nw, nh);
                    sp->imageReady.store(false);
                }
            }
        }
        // The worker owns a dedicated upload ring (StartMirrorThread), so it
        // cannot stomp the render thread's offsets. Start it once an
        // opposite-orient independent panel exists; it free-runs from here and
        // is deliberately not frame-locked to the primary.
        if (!m_mirrorQueue)
            StartMirrorThread();
    }

    // Readiness is per surface now -- see the panel loop, which asks the
    // surface serving THAT display. A single flag would have let one panel's
    // warmup hold every other panel on the stretched fallback.
    //
    // No workerIdle requirement: the blit reads the PUBLISHED (double-buffered,
    // fence-proven) display face, never the one in flight. Requiring idle here
    // made the path fall back to a stretched primary whenever the worker was
    // busy — the panels alternated two different images (~20 Hz flicker).
    auto surfaceReady = [](const MirrorSurface* sp) {
        return sp && sp->imageReady.load() && sp->pipe.ready &&
               sp->pipe.frames >= 2 &&
               sp->publishedIdx.load(std::memory_order_acquire) >= 0;
    };

    if (anyOppositeIndep && m_nDeferMirrorResize.load() <= 0 && workerIdle) {
        for (auto& out : m_displayOutputs) {
            if (!out.config.bIndependentRender || out.config.type != DisplayOutputType::Monitor)
                continue;
            if (!out.monitorState || !out.monitorState->bReady || !out.monitorState->swapChain)
                continue;
            int wantW = bbW, wantH = bbH;
            MirrorSwapChainSize(out, bbW, bbH, wantW, wantH);
            if (wantW > 0 && wantH > 0 &&
                (out.monitorState->width != wantW || out.monitorState->height != wantH))
                ResizeMirrorSwapChain(*out.monitorState, wantW, wantH);
        }
    }

    bool anyCopy = false;
    bool anyBlit = false;
    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
            continue;
        if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
            continue;
        auto& ms = *out.monitorState;
        int lx = 0, ly = 0, lw = 0, lh = 0;
        ComputeMirrorLayout(out.config, lx, ly, lw, lh);
        const bool panelPort = (lw > 0 && lh > 0) ? isPortrait(lw, lh)
                                                 : isPortrait(ms.width, ms.height);
        const bool oppositeIndep = out.config.bIndependentRender &&
            (panelPort != mainPortrait);
        if (oppositeIndep || ms.width != srcW || ms.height != srcH)
            anyBlit = true;
        else
            anyCopy = true;
    }

    if (anyCopy && srcState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src;
        b.Transition.StateBefore = srcState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        srcState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    if (anyBlit && srcState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src;
        b.Transition.StateBefore = srcState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        srcState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    // Whether messages and HUD were held OUT of the snapshot this frame.
    //
    // The decision is made once for the whole set: RenderFrame skips drawing
    // supertexts into the backbuffer when ANY mirror is independent, so each
    // panel can draw them at its own size instead of receiving them stretched.
    // Redrawing them, though, used to be keyed to the panel's OWN
    // bIndependentRender -- so a copy/stretch mirror standing next to an
    // independent one got a snapshot the messages had been taken out of and
    // nothing to put them back. That is how a mirror went silent while the
    // render window kept its text, and it needs only ONE independent entry to
    // happen: in the reported case DISPLAY2, the render window's own monitor,
    // which is skipped as a mirror and so was not even drawing.
    //
    // The flag that removes the overlays has to be the flag that restores them.
    const bool overlaysDeferred = AnyIndependentMirrorEnabled();

    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
            continue;
        if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
            continue;
        auto& ms = *out.monitorState;
        const bool wantOverlays = out.config.bIndependentRender || overlaysDeferred;
        UINT mirrorFI = ms.swapChain->GetCurrentBackBufferIndex();
        if (mirrorFI >= ms.bufferCount || mirrorFI >= MIRROR_BUFFER_COUNT ||
            !ms.backBuffers[mirrorFI])
            continue;
        ID3D12Resource* mirrorBB = ms.backBuffers[mirrorFI].Get();
        const D3D12_RESOURCE_STATES before = ms.bEverPresented
            ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COMMON;

        int lx = 0, ly = 0, lw = 0, lh = 0;
        ComputeMirrorLayout(out.config, lx, ly, lw, lh);
        const bool panelPort = (lw > 0 && lh > 0) ? isPortrait(lw, lh)
                                                 : isPortrait(ms.width, ms.height);
        // A display holding its own preset renders for itself whatever its
        // shape: the primary's image is the WRONG PRESET, so stretching it is
        // not an option the way it is for a same-orient follower (#186 ph 4).
        const bool ownPreset = DisplayHoldsOwnPreset(out.config);
        const bool oppositeIndep = (out.config.bIndependentRender &&
            (panelPort != mainPortrait)) || ownPreset;
        // THIS display's surface, not "the" surface: each one renders at its
        // own size into its own pipe.
        MirrorSurface* psurf = oppositeIndep
            ? MirrorSurfaceFor(out.config.szDeviceName) : nullptr;
        const bool useOrient = oppositeIndep && surfaceReady(psurf);
        ms.bFreshDraw = true;
        // Present gating: no new sim frame published since this panel's last
        // draw → keep the presented image (skip draw AND present). Re-blitting
        // and re-presenting identical content every render frame was ~2x the
        // sim rate in pure waste.
        if (useOrient && ms.bEverPresented &&
            ms.lastPubSeq == psurf->publishedSeq.load(std::memory_order_acquire)) {
            ms.bFreshDraw = false;
            continue;
        }
        // Never 1:1-copy a portrait primary into a landscape SC (or vice versa).
        const bool doCopy = !oppositeIndep && (ms.width == srcW && ms.height == srcH);
        D3D12_RESOURCE_BARRIER mb = {};
        mb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        mb.Transition.pResource = mirrorBB;
        mb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        if (doCopy) {
            if (before != D3D12_RESOURCE_STATE_COPY_DEST) {
                mb.Transition.StateBefore = before;
                mb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                cmd->ResourceBarrier(1, &mb);
            }
            cmd->CopyResource(mirrorBB, src);
            if (wantOverlays && ms.bHasRtv) {
                mb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                mb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                cmd->ResourceBarrier(1, &mb);
                // Bind the panel's RTV. Nothing else on this branch does: the
                // blit paths bind it inside BlitMainToMirror /
                // BlitOrientOutputToMirror, and a straight CopyResource needs
                // no render target at all -- so the overlay draw here inherited
                // whatever target was last set on the list.
                D3D12_CPU_DESCRIPTOR_HANDLE mirrorRtv = ms.rtvHandles[mirrorFI];
                cmd->OMSetRenderTargets(1, &mirrorRtv, FALSE, nullptr);
                DrawOverlaysToMirror(cmd, ms.width, ms.height, false);
                mb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                mb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                cmd->ResourceBarrier(1, &mb);
            } else {
                mb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                mb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                cmd->ResourceBarrier(1, &mb);
            }
            ms.lastPath = 4;
        } else {
            if (before != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                mb.Transition.StateBefore = before;
                mb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                cmd->ResourceBarrier(1, &mb);
            }
            if (useOrient) {
                if (!BlitOrientOutputToMirror(*psurf, cmd,
                                              ms.rtvHandles[mirrorFI],
                                              ms.width, ms.height))
                    BlitMainToMirror(cmd, src, srcW, srcH,
                                     ms.rtvHandles[mirrorFI], ms.width, ms.height, 0);
                // Orient pass is warp+comp only — draw messages/sprites at panel size.
                DrawOverlaysToMirror(cmd, ms.width, ms.height, true);
                ms.lastPath = 1;
                ms.lastPubSeq = psurf->publishedSeq.load(std::memory_order_acquire);
            } else {
                // Opposite independent without a ready orient image: if this
                // panel EVER showed a sim frame, hold it — a stretch-filled
                // portrait frame here is exactly the "infrequent whole-screen
                // stretched flash" Shane pinned down (fires for 1-2 worker
                // periods on every orient-pipe recreate: descriptor-heap
                // rewind, preset mode switch, resize). Stretch only serves the
                // very first light-up, before any sim frame exists.
                if (oppositeIndep && ms.bEverPresented) {
                    ms.bFreshDraw = false;
                    ms.lastPath = 6;
                    // Undo the RT transition recorded above for this face.
                    mb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    mb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                    cmd->ResourceBarrier(1, &mb);
                    if (mirrorFI < MIRROR_BUFFER_COUNT)
                        ms.bbState[mirrorFI] = D3D12_RESOURCE_STATE_PRESENT;
                    continue;
                }
                // Stretch-fill (letterbox looked like an unstretched portrait copy).
                const int scale = oppositeIndep ? 0 : (out.config.bIndependentRender ? 1 : 0);
                BlitMainToMirror(cmd, src, srcW, srcH,
                                 ms.rtvHandles[mirrorFI], ms.width, ms.height, scale);
                if (wantOverlays)
                    DrawOverlaysToMirror(cmd, ms.width, ms.height, false);
                ms.lastPath = 2;
            }
            mb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            mb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            cmd->ResourceBarrier(1, &mb);
        }
        if (mirrorFI < MIRROR_BUFFER_COUNT)
            ms.bbState[mirrorFI] = D3D12_RESOURCE_STATE_PRESENT;
        ms.lastDrawFI = mirrorFI;
        ms.drawCount++;
        if (ms.hWnd && !IsWindowVisible(ms.hWnd))
            ShowWindow(ms.hWnd, SW_SHOWNOACTIVATE);
    }

    if (haveSnap)
        m_mirrorSrcTex.currentState = srcState;
    else if (srcState != D3D12_RESOURCE_STATE_PRESENT) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src;
        b.Transition.StateBefore = srcState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
    }

    cmd->Close();
    m_lpDX->EndAuxUpload();
    ID3D12CommandList* lists[] = { cmd };
    m_lpDX->m_commandQueue->ExecuteCommandLists(1, lists);

    // DXGI frame latency is DEVICE-wide. At the default (2-3), multi-swapchain
    // flip queues can re-scan a STALE face every other refresh — visible as a
    // two-image flicker on the panels that composition-level captures
    // (PrintWindow/ImageGrab) can NOT see, since DWM composes the newest
    // frame while scanout flips the queue. The latency=1 call lived only in
    // the dead slot-path since the July rework; restore it here (2026-08-21).
    {
        int nMirrorPresents = 0;
        for (auto& out : m_displayOutputs)
            if (out.config.bEnabled && out.config.type == DisplayOutputType::Monitor &&
                out.monitorState && out.monitorState->bReady && !out.monitorState->bSoftDisabled)
                nMirrorPresents++;
        m_lpDX->EnsureMultiSwapChainFrameLatency(1 + nMirrorPresents, true);
    }

    // SC is created with Flags=0 — ALLOW_TEARING is invalid and Present fails
    // (invisible HWND). First present is blocking so flip-model actually shows.
    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
            continue;
        if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
            continue;
        auto& ms = *out.monitorState;
        if (!ms.bFreshDraw)
            continue; // nothing new drawn this frame — keep the shown image
        if (ms.hWnd && !IsWindowVisible(ms.hWnd))
            ShowWindow(ms.hWnd, SW_SHOWNOACTIVATE);
        const UINT flags = ms.bEverPresented ? DXGI_PRESENT_DO_NOT_WAIT : 0u;
        HRESULT hr = ms.swapChain->Present(0, flags);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            hr = ms.swapChain->Present(0, 0);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                ms.presentSkipCount++;
                ms.lastPresentHr = hr;
                continue;
            }
        }
        ms.lastPresentHr = hr;
        if (SUCCEEDED(hr)) {
            ms.bEverPresented = true;
            ms.presentOkCount++;
            m_nMirrorPresentAccum.fetch_add(1, std::memory_order_relaxed);
        } else {
            char logBuf[256];
            sprintf(logBuf, "Mirror Present failed (0x%08X) on %ls\n",
                    (unsigned)hr, out.config.szDeviceName);
            DebugLogA(logBuf, LOG_ERROR);
            ms.presentFailCount++;
        }
    }

    return;

#if 0 // old same-thread independent path — kept for reference, not compiled
    bool anyIndepActive = false;
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor || !out.config.bEnabled)
            continue;
        if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
            continue;
        if (out.config.bIndependentRender)
            anyIndepActive = true;
    }

    // Independent off: copy when SC == primary, else blit (SC is capped at 1920
    // so a 2160x3840 primary must not ResizeBuffers to 4K — that TDRs).
    if (!anyIndepActive) {
        if (m_nDeferMirrorResize.load() <= 0) {
            for (auto& out : m_displayOutputs) {
                if (out.config.type != DisplayOutputType::Monitor || !out.config.bEnabled)
                    continue;
                if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
                    continue;
                int wantW = mainW, wantH = mainH;
                MirrorSwapChainSize(out, mainW, mainH, wantW, wantH);
                auto& ms = *out.monitorState;
                if (wantW > 0 && wantH > 0 && (ms.width != wantW || ms.height != wantH))
                    ResizeMirrorSwapChain(ms, wantW, wantH);
            }
        }

        bool allSameSize = true;
        for (auto& out : m_displayOutputs) {
            if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
                continue;
            if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
                continue;
            if (out.monitorState->width != mainW || out.monitorState->height != mainH) {
                allSameSize = false;
                break;
            }
        }
        // Same-size CopyResource of the flip BB after Execute used to be the
        // July path. It freezes when client size != BB desc, and the cheap
        // blit below already covers copy-mode. Always fall through.
        if (false && allSameSize) {
        if (!m_mirrorCmdAllocators[0]) {
            for (int i = 0; i < DXC_FRAME_COUNT; i++) {
                HRESULT hr = m_lpDX->m_device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_mirrorCmdAllocators[i]));
                if (FAILED(hr)) {
                    DebugLogA("SendToDisplayOutputs: CreateCommandAllocator failed\n", LOG_ERROR);
                    return;
                }
            }
            HRESULT hr = m_lpDX->m_device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                m_mirrorCmdAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_mirrorCmdList));
            if (FAILED(hr)) {
                DebugLogA("SendToDisplayOutputs: CreateCommandList failed\n", LOG_ERROR);
                return;
            }
            m_mirrorCmdList->Close();
        }

        HRESULT hrAlloc = m_mirrorCmdAllocators[fi]->Reset();
        if (FAILED(hrAlloc)) {
            m_lpDX->WaitForGpu();
            hrAlloc = m_mirrorCmdAllocators[fi]->Reset();
        }
        if (FAILED(hrAlloc))
            return;
        HRESULT hrList = m_mirrorCmdList->Reset(m_mirrorCmdAllocators[fi].Get(), nullptr);
        if (FAILED(hrList)) {
            m_lpDX->WaitForGpu();
            hrList = m_mirrorCmdList->Reset(m_mirrorCmdAllocators[fi].Get(), nullptr);
        }
        if (FAILED(hrList))
            return;

        ID3D12Resource* mainBB = m_lpDX->m_renderTargets[fi].Get();
        D3D12_RESOURCE_STATES mainBefore = D3D12_RESOURCE_STATE_PRESENT;
        if (m_bDisableMirrorHud && m_bMirrorSrcCopiedThisFrame &&
            m_mirrorSrcTex.IsValid() && m_mirrorSrcTex.resource &&
            m_mirrorSrcTex.width == (UINT)mainW && m_mirrorSrcTex.height == (UINT)mainH) {
            mainBB = m_mirrorSrcTex.resource.Get();
            mainBefore = m_mirrorSrcTex.currentState;
        }
        if (!mainBB) {
            m_mirrorCmdList->Close();
            return;
        }

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = mainBB;
        barrier.Transition.StateBefore = mainBefore;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_mirrorCmdList->ResourceBarrier(1, &barrier);

        for (auto& out : m_displayOutputs) {
            if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
                continue;
            if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
                continue;
            auto& ms = *out.monitorState;
            if (ms.width != mainW || ms.height != mainH)
                continue;
            UINT mirrorFI = ms.swapChain->GetCurrentBackBufferIndex();
            if (mirrorFI >= ms.bufferCount || mirrorFI >= MIRROR_BUFFER_COUNT ||
                !ms.backBuffers[mirrorFI])
                continue;
            ID3D12Resource* mirrorBB = ms.backBuffers[mirrorFI].Get();
            const D3D12_RESOURCE_STATES before = ms.bEverPresented
                ? D3D12_RESOURCE_STATE_PRESENT
                : D3D12_RESOURCE_STATE_COMMON;

            D3D12_RESOURCE_BARRIER mirrorBarrier = {};
            mirrorBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            mirrorBarrier.Transition.pResource = mirrorBB;
            mirrorBarrier.Transition.StateBefore = before;
            mirrorBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            mirrorBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_mirrorCmdList->ResourceBarrier(1, &mirrorBarrier);

            m_mirrorCmdList->CopyResource(mirrorBB, mainBB);

            mirrorBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            mirrorBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            m_mirrorCmdList->ResourceBarrier(1, &mirrorBarrier);
            if (mirrorFI < MIRROR_BUFFER_COUNT)
                ms.bbState[mirrorFI] = D3D12_RESOURCE_STATE_PRESENT;
            ms.lastPath = 4;
            ms.lastDrawFI = mirrorFI;
            ms.drawCount++;
        }

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = mainBefore;
        m_mirrorCmdList->ResourceBarrier(1, &barrier);
        if (mainBB == m_mirrorSrcTex.resource.Get())
            m_mirrorSrcTex.currentState = mainBefore;
        m_mirrorCmdList->Close();

        ID3D12CommandList* lists[] = { m_mirrorCmdList.Get() };
        m_lpDX->m_commandQueue->ExecuteCommandLists(1, lists);

        int nMirrorPresents = 0;
        for (auto& out : m_displayOutputs) {
            if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
                continue;
            if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
                continue;
            nMirrorPresents++;
            HRESULT hr = out.monitorState->swapChain->Present(0, 0);
            if (FAILED(hr)) {
                char logBuf[256];
                sprintf(logBuf, "Mirror Present failed (0x%08X) on %ls (no destroy)\n",
                        (unsigned)hr, out.config.szDeviceName);
                DebugLogA(logBuf, LOG_ERROR);
                out.monitorState->presentFailCount++;
            } else {
                out.monitorState->bEverPresented = true;
                out.monitorState->presentOkCount++;
            }
        }
        m_lpDX->m_bSerializeWithMirrors = true;
        m_lpDX->EnsureMultiSwapChainFrameLatency(1 + nMirrorPresents, true);
        return;
        } // allSameSize: else fall through and blit
    }

    // Collect active monitor slots; decide main-BB sampling needs before opening the list.
    struct MirrorSlot {
        DisplayOutput* out = nullptr;
        MonitorMirrorState* ms = nullptr;
        UINT mirrorFI = 0;
        ID3D12Resource* bb = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
        bool doIndepReRender = false; // milk3 or classic independent re-render
        bool portrait = false;
        bool oppositeOrient = false; // independent re-render at opposite aspect
        int  pipeW = 0, pipeH = 0;   // capped panel size (aspect of the HWND)
    };
    auto isPortrait = [](int w, int h) { return h > w; };

    std::vector<MirrorSlot> slots;
    slots.reserve(m_displayOutputs.size());
    bool needMainAsCopySrc = false;
    bool needMainAsSrv = false;
    bool anyIndepMilk3 = false;
    bool anyIndepReRender = false;
    bool anyOppositeOrient = false;
    const bool mainPortrait = isPortrait(mainW, mainH);
    // Independent: milk3 (orient pipe Image) OR classic .milk/.milk2 (warp+comp;
    // milk2 is frozen dual-preset blend on the classic path, not Shadertoy).
    const bool canIndepMilk3 = m_bShadertoyMode && m_dx12CompPSO;
    const bool canIndepClassic = !m_bShadertoyMode && m_lpDX && m_lpDX->m_device;

    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
            continue;
        if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
            continue;
        auto& ms = *out.monitorState;
        UINT mirrorFI = ms.swapChain->GetCurrentBackBufferIndex();
        if (mirrorFI >= ms.bufferCount || mirrorFI >= MIRROR_BUFFER_COUNT ||
            !ms.backBuffers[mirrorFI] || !ms.bHasRtv)
            continue;

        MirrorSlot s;
        s.out = &out;
        s.ms = &ms;
        s.mirrorFI = mirrorFI;
        s.bb = ms.backBuffers[mirrorFI].Get();
        s.rtv = ms.rtvHandles[mirrorFI];
        // Aspect from the HWND/panel, not the SC. Copy-mode SCs match the
        // primary; using ms.size then never saw opposite-orient milk2.
        int layX = 0, layY = 0, layW = 0, layH = 0;
        ComputeMirrorLayout(out.config, layX, layY, layW, layH);
        s.portrait = (layW > 0 && layH > 0) ? isPortrait(layW, layH)
                                            : isPortrait(ms.width, ms.height);
        s.pipeW = (layW > 0) ? layW : ms.width;
        s.pipeH = (layH > 0) ? layH : ms.height;
        CapMirrorSwapChainDim(s.pipeW, s.pipeH);
        s.doIndepReRender = out.config.bIndependentRender &&
            (canIndepMilk3 || canIndepClassic);
        s.oppositeOrient = out.config.bIndependentRender && (s.portrait != mainPortrait);
        if (s.oppositeOrient)
            anyOppositeOrient = true;
        if (s.doIndepReRender) {
            anyIndepReRender = true;
            if (canIndepMilk3)
                anyIndepMilk3 = true;
        } else if (ms.width == mainW && ms.height == mainH && !out.config.bIndependentRender) {
            needMainAsCopySrc = true;
        } else {
            needMainAsSrv = true;
        }
        slots.push_back(s);
    }
    if (slots.empty())
        return;

    // Same-orient independent always blits primary (cheap). Opposite uses the
    // lagged pass — never a second warp+comp on this list.
    if (anyIndepReRender) {
        for (const auto& s : slots) {
            if (s.doIndepReRender && s.portrait == mainPortrait)
                needMainAsSrv = true;
        }
    }

    const bool lagIdle = LagIndepFenceIdle();
    const bool auxSlotBusy = !lagIdle && m_lagIndepAuxFrame == (UINT)fi;

    // Pre-create opposite-orient pipe only when we can record a lag pass.
    // WaitForGpu here is rare (size/epoch change) and only while the lag fence is idle.
    if (anyOppositeOrient && anyIndepReRender && lagIdle && !auxSlotBusy) {
        int needW = 0, needH = 0;
        for (const auto& s : slots) {
            if (!s.doIndepReRender || !s.oppositeOrient) continue;
            long long a = (long long)s.pipeW * (long long)s.pipeH;
            long long best = (long long)needW * (long long)needH;
            if (a > best) {
                needW = s.pipeW;
                needH = s.pipeH;
            }
        }
        if (needW > 0 && needH > 0) {
            int capW = needW, capH = needH;
            const int maxDim = 1920; // match EnsureOrientPipeline (2560 TDRd with 4K primary)
            if (capW > maxDim || capH > maxDim) {
                float sc = (float)maxDim / (float)((capW > capH) ? capW : capH);
                capW = max(1, (int)(capW * sc + 0.5f));
                capH = max(1, (int)(capH * sc + 0.5f));
            }
            capW = ((capW + 15) / 16) * 16;
            capH = ((capH + 15) / 16) * 16;
            const bool needCreate = !OnlyMirrorSurface().pipe.ready ||
                OnlyMirrorSurface().pipe.w != capW || OnlyMirrorSurface().pipe.h != capH ||
                OnlyMirrorSurface().pipe.bindEpoch != m_lpDX->m_descriptorEpoch;
            if (needCreate) {
                m_lpDX->WaitForGpu();
                EnsureOrientPipeline(needW, needH);
            }
        }
    }

    // Lazy-create mirror command objects
    if (!m_mirrorCmdAllocators[0]) {
        for (int i = 0; i < DXC_FRAME_COUNT; i++) {
            HRESULT hr = m_lpDX->m_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_mirrorCmdAllocators[i]));
            if (FAILED(hr)) {
                DebugLogA("SendToDisplayOutputs: CreateCommandAllocator failed\n", LOG_ERROR);
                return;
            }
        }
        HRESULT hr = m_lpDX->m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_mirrorCmdAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_mirrorCmdList));
        if (FAILED(hr)) {
            DebugLogA("SendToDisplayOutputs: CreateCommandList failed\n", LOG_ERROR);
            return;
        }
        m_mirrorCmdList->Close();
    }

    // Cheap blit list. Retry once with WaitForGpu so a busy allocator cannot
    // freeze the last presented frame.
    bool cheapOk = false;
    if (!auxSlotBusy) {
        HRESULT hrAlloc = m_mirrorCmdAllocators[fi]->Reset();
        if (FAILED(hrAlloc)) {
            m_lpDX->WaitForGpu();
            hrAlloc = m_mirrorCmdAllocators[fi]->Reset();
        }
        HRESULT hrList = FAILED(hrAlloc) ? E_FAIL
            : m_mirrorCmdList->Reset(m_mirrorCmdAllocators[fi].Get(), nullptr);
        if (FAILED(hrList)) {
            m_lpDX->WaitForGpu();
            hrList = m_mirrorCmdList->Reset(m_mirrorCmdAllocators[fi].Get(), nullptr);
        }
        if (SUCCEEDED(hrAlloc) && SUCCEEDED(hrList))
            cheapOk = true;
        else
            m_mirrorDiagSkipFrames++;
    }

    // Isolate mirror-path failures so they cannot cascade into safe mode.
    // Outer size guard: whatever SizeGuard/ImageToMirror does, leave primary texsize clean.
    struct PrimarySizeGuard {
        Engine* e;
        explicit PrimarySizeGuard(Engine* eng) : e(eng) {}
        ~PrimarySizeGuard() { e->RestorePrimaryTexSizeFromVS(); }
    } primarySizeGuard(this);

    try {

    ID3D12Resource* mainBB = m_lpDX->m_renderTargets[fi].Get();
    ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };

    bool beganAux = false;
    auto ensureAux = [&]() {
        if (!beganAux) {
            m_lpDX->BeginAuxUpload();
            beganAux = true;
        }
    };

    // Snapshot size is the flip-BB desc, which can disagree with client
    // (DPI / FS). Using client as the match left mainBB null → no draw →
    // no Present → invisible click-blocking HWND.
    ID3D12Resource* snapBB = nullptr;
    D3D12_RESOURCE_STATES snapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    int snapW = 0, snapH = 0;
    if (m_bMirrorSrcCopiedThisFrame && m_mirrorSrcTex.IsValid() &&
        m_mirrorSrcTex.resource) {
        snapBB = m_mirrorSrcTex.resource.Get();
        snapState = m_mirrorSrcTex.currentState;
        snapW = (int)m_mirrorSrcTex.width;
        snapH = (int)m_mirrorSrcTex.height;
    }
    (void)needMainAsSrv;
    (void)needMainAsCopySrc;
    (void)mainBB;

    bool processed[32] = {};
    bool cheapDrew[32] = {};
    bool lagDrewOpposite = false;
    const size_t nSlots = slots.size();
    if (nSlots > 32)
        DebugLogA("SendToDisplayOutputs: too many mirrors for coalesce mask\n", LOG_WARN);

    auto transitionOn = [&](ID3D12GraphicsCommandList* cmd, MirrorSlot& s,
                            D3D12_RESOURCE_STATES after) {
        if (!cmd || s.mirrorFI >= MIRROR_BUFFER_COUNT)
            return;
        D3D12_RESOURCE_STATES before = s.ms->bbState[s.mirrorFI];
        if (before == after)
            return;
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = s.bb;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        s.ms->bbState[s.mirrorFI] = after;
    };

    // ── Cheap path: copy / letterbox primary. Never warp+comp here. ──
    if (cheapOk) {
        m_mirrorCmdList->SetDescriptorHeaps(1, heaps);
        m_mirrorCmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());
        ensureAux();

        for (size_t i = 0; i < nSlots; i++) {
            MirrorSlot& s = slots[i];
            // Blit every face, including opposite-orient. Lag may overwrite
            // those after; if it skips they stay the live primary (not black).

            if (s.mirrorFI < MIRROR_BUFFER_COUNT) {
                s.ms->bbState[s.mirrorFI] = s.ms->bEverPresented
                    ? D3D12_RESOURCE_STATE_PRESENT
                    : D3D12_RESOURCE_STATE_COMMON;
            }

            const bool sameSize = snapBB &&
                (s.ms->width == snapW && s.ms->height == snapH);
            const bool doCopy = sameSize && !s.out->config.bIndependentRender &&
                snapState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            const bool doBlit = snapBB && snapW > 0 && snapH > 0 && !doCopy;

            processed[i] = true;
            if (doCopy) {
                D3D12_RESOURCE_BARRIER toCopy = {};
                toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                toCopy.Transition.pResource = snapBB;
                toCopy.Transition.StateBefore = snapState;
                toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                if (snapState != D3D12_RESOURCE_STATE_COPY_SOURCE)
                    m_mirrorCmdList->ResourceBarrier(1, &toCopy);
                snapState = D3D12_RESOURCE_STATE_COPY_SOURCE;
                transitionOn(m_mirrorCmdList.Get(), s, D3D12_RESOURCE_STATE_COPY_DEST);
                m_mirrorCmdList->CopyResource(s.bb, snapBB);
                transitionOn(m_mirrorCmdList.Get(), s, D3D12_RESOURCE_STATE_PRESENT);
                s.ms->lastPath = 4;
                cheapDrew[i] = true;
            } else if (doBlit) {
                if (snapState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
                    D3D12_RESOURCE_BARRIER toSrv = {};
                    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    toSrv.Transition.pResource = snapBB;
                    toSrv.Transition.StateBefore = snapState;
                    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    m_mirrorCmdList->ResourceBarrier(1, &toSrv);
                    snapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                }
                transitionOn(m_mirrorCmdList.Get(), s, D3D12_RESOURCE_STATE_RENDER_TARGET);
                const int scaleMode = s.out->config.bIndependentRender ? 1 : 0;
                BlitMainToMirror(m_mirrorCmdList.Get(), snapBB, snapW, snapH,
                                 s.rtv, s.ms->width, s.ms->height, scaleMode);
                transitionOn(m_mirrorCmdList.Get(), s, D3D12_RESOURCE_STATE_PRESENT);
                s.ms->lastPath = scaleMode == 1 ? 3 : 2;
                cheapDrew[i] = true;
            } else {
                continue;
            }
            s.ms->lastDrawFI = s.mirrorFI;
            s.ms->drawCount++;
        }

        if (snapBB && snapState != m_mirrorSrcTex.currentState)
            m_mirrorSrcTex.currentState = snapState;

        m_mirrorCmdList->Close();
        ID3D12CommandList* cheapLists[] = { m_mirrorCmdList.Get() };
        m_lpDX->m_commandQueue->ExecuteCommandLists(1, cheapLists);
    }

    // ── Lagged opposite-orient: skip if previous fence still in flight. ──
    if (anyOppositeOrient && anyIndepReRender && lagIdle && !auxSlotBusy &&
        EnsureLagIndepObjects()) {
        HRESULT hrA = m_lagIndepAlloc->Reset();
        HRESULT hrL = FAILED(hrA) ? E_FAIL
            : m_lagIndepList->Reset(m_lagIndepAlloc.Get(), nullptr);
        if (SUCCEEDED(hrA) && SUCCEEDED(hrL)) {
            m_lagIndepList->SetDescriptorHeaps(1, heaps);
            m_lagIndepList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());
            ensureAux();

            int leaderIdx = -1;
            long long bestArea = 0;
            for (size_t i = 0; i < nSlots; i++) {
                if (!slots[i].doIndepReRender || !slots[i].oppositeOrient)
                    continue;
                long long a = (long long)slots[i].pipeW * (long long)slots[i].pipeH;
                if (leaderIdx < 0 || a > bestArea) {
                    bestArea = a;
                    leaderIdx = (int)i;
                }
            }

            bool ok = false;
            if (leaderIdx >= 0) {
                MirrorSlot& leader = slots[leaderIdx];
                const int rw = leader.pipeW > 0 ? leader.pipeW : leader.ms->width;
                const int rh = leader.pipeH > 0 ? leader.pipeH : leader.ms->height;
                m_bOrientOppositeAspect = true;
                ok = canIndepMilk3
                    ? RenderMilk3OrientPipeline(m_lagIndepList.Get(), rw, rh)
                    : RenderClassicOrientPipeline(m_lagIndepList.Get(), rw, rh);
                m_bOrientOppositeAspect = false;
            }
            const bool showHud = ok && OnlyMirrorSurface().pipe.frames >= 2;
            // Warmup clears are black — do not overwrite the cheap snapshot blit.
            const bool useLagImage = ok && OnlyMirrorSurface().pipe.frames >= 2;

            if (useLagImage) {
            for (size_t i = 0; i < nSlots; i++) {
                MirrorSlot& s = slots[i];
                if (!s.doIndepReRender || !s.oppositeOrient)
                    continue;
                if (s.mirrorFI < MIRROR_BUFFER_COUNT) {
                    s.ms->bbState[s.mirrorFI] = s.ms->bEverPresented
                        ? D3D12_RESOURCE_STATE_PRESENT
                        : D3D12_RESOURCE_STATE_COMMON;
                }
                processed[i] = true;
                transitionOn(m_lagIndepList.Get(), s, D3D12_RESOURCE_STATE_RENDER_TARGET);
                bool drew = false;
                if (BlitOrientOutputToMirror(m_lagIndepList.Get(), s.rtv,
                                             s.ms->width, s.ms->height)) {
                    drew = true;
                    if (showHud)
                        DrawOverlaysToMirror(m_lagIndepList.Get(),
                                             s.ms->width, s.ms->height);
                }
                transitionOn(m_lagIndepList.Get(), s, D3D12_RESOURCE_STATE_PRESENT);
                s.ms->lastPath = drew ? 1 : 6;
                s.ms->lastDrawFI = s.mirrorFI;
                s.ms->drawCount++;
            }
            }

            m_lagIndepList->Close();
            ID3D12CommandList* lagLists[] = { m_lagIndepList.Get() };
            m_lpDX->m_commandQueue->ExecuteCommandLists(1, lagLists);
            m_lagIndepSignal++;
            m_lpDX->m_commandQueue->Signal(m_lagIndepFence.Get(), m_lagIndepSignal);
            m_lagIndepSubmitted = m_lagIndepSignal;
            m_lagIndepAuxFrame = fi;
            lagDrewOpposite = useLagImage;
        } else {
            m_mirrorDiagSkipFrames++;
        }
    } else if (anyOppositeOrient && anyIndepReRender) {
        m_mirrorDiagSkipFrames++;
        for (size_t i = 0; i < nSlots; i++) {
            if (slots[i].doIndepReRender && slots[i].oppositeOrient)
                processed[i] = true; // keep last presented image
        }
    }

    if (beganAux)
        m_lpDX->EndAuxUpload();

    // DIAG snapshot (observability only)
    m_mirrorDiagMainW = mainW;
    m_mirrorDiagMainH = mainH;
    m_mirrorDiagMainPortrait = mainPortrait ? 1 : 0;
    m_mirrorDiagNeedMainSrv = snapBB ? 1 : 0;
    m_mirrorDiagCanSampleMain = snapBB ? 1 : 0;
    m_mirrorDiagAnyOpposite = anyOppositeOrient ? 1 : 0;
    m_mirrorDiagAnyIndepMilk3 = anyIndepMilk3 ? 1 : 0;
    m_mirrorDiagShadertoy = m_bShadertoyMode ? 1 : 0;
    m_mirrorDiagCompPso = m_dx12CompPSO ? 1 : 0;
    m_mirrorDiagSlotCount = (int)nSlots;
    m_mirrorDiagFrameCounter++;
    if (m_lpDX->m_frameIndex < DXC_FRAME_COUNT)
        m_mirrorDiagAuxUsed = m_lpDX->m_auxUploadOffset[m_lpDX->m_frameIndex];

    // Device-wide present queue: 1 main + N mirrors. Keep latency modest; also
    // call with 1 when no mirrors so we restore default after multi-SC sessions.
    int nMirrorPresents = 0;
    for (auto& out : m_displayOutputs) {
        if (out.config.bEnabled && out.config.type == DisplayOutputType::Monitor &&
            out.monitorState && out.monitorState->bReady && !out.monitorState->bSoftDisabled)
            nMirrorPresents++;
    }
    // Latency 1 whenever mirrors are up. Latency 2 kept a stale landscape
    // flip face on the portrait primary (ghost strip) at high focused FPS.
    m_lpDX->m_bSerializeWithMirrors = true;
    m_lpDX->EnsureMultiSwapChainFrameLatency(1 + nMirrorPresents, true);

    // First Present must complete or the HWND stays a transparent click overlay.
    // After that, skip Present if we did not draw (keep last image). Hide until
    // the first successful present so an empty popup cannot eat clicks.
    const UINT tearFlag = m_lpDX->m_tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0;
    for (size_t si = 0; si < nSlots; si++) {
        MirrorSlot& s = slots[si];
        auto& ms = *s.ms;
        const bool oppositeIndep = s.doIndepReRender && s.oppositeOrient;
        const bool drewThis = oppositeIndep ? lagDrewOpposite : cheapDrew[si];
        ms.lastOppositeIndep = oppositeIndep;
        if (!drewThis) {
            if (!ms.bEverPresented && ms.hWnd && IsWindowVisible(ms.hWnd))
                ShowWindow(ms.hWnd, SW_HIDE);
            ms.presentSkipCount++;
            continue;
        }
        if (ms.hWnd && !IsWindowVisible(ms.hWnd))
            ShowWindow(ms.hWnd, SW_SHOWNOACTIVATE);
        if (ms.hWnd) {
            LONG_PTR ex = GetWindowLongPtrW(ms.hWnd, GWL_EXSTYLE);
            ms.lastLayered = (ex & WS_EX_LAYERED) != 0;
        }
        const UINT nBuf = ms.bufferCount ? ms.bufferCount : MIRROR_BUFFER_COUNT;
        const UINT allBits = (nBuf >= 32) ? 0xFFFFFFFFu : ((1u << nBuf) - 1u);
        const bool firstPresent = !ms.bEverPresented;
        ms.lastMustBlock = firstPresent;
        const UINT flags = firstPresent ? 0u : (tearFlag | DXGI_PRESENT_DO_NOT_WAIT);
        const UINT curIdx = ms.swapChain->GetCurrentBackBufferIndex();
        ms.lastPresentFI = curIdx;
        HRESULT hr = ms.swapChain->Present(0, flags);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING && !firstPresent) {
            ms.presentSkipCount++;
            ms.lastPresentHr = hr;
            continue;
        }
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            hr = ms.swapChain->Present(0, 0);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                ms.presentSkipCount++;
                ms.lastPresentHr = hr;
                continue;
            }
        }
        ms.lastPresentHr = hr;
        if (FAILED(hr)) {
            char logBuf[256];
            sprintf(logBuf, "Mirror Present failed (0x%08X) on %ls (no destroy)\n",
                    (unsigned)hr, s.out->config.szDeviceName);
            DebugLogA(logBuf, LOG_ERROR);
            ms.presentFailCount++;
        } else {
            ms.bEverPresented = true;
            ms.presentOkCount++;
            if (curIdx < 32)
                ms.paintedBufferMask |= (1u << curIdx);
            if ((ms.paintedBufferMask & allBits) == allBits)
                ms.bNeedsFullChainClear = false;
        }
    }
    } catch (const std::exception& e) {
        char buf[256];
        sprintf(buf, "SendToDisplayOutputs: exception: %s\n", e.what());
        DebugLogA(buf, LOG_ERROR);
        m_lpDX->EndAuxUpload();
        RestorePrimaryTexSizeFromVS();
        if (m_mirrorCmdList)
            m_mirrorCmdList->Close();
        if (m_lagIndepList)
            m_lagIndepList->Close();
        if (m_lpDX)
            m_lpDX->WaitForGpu();
        m_mirrorCmdList.Reset();
        for (int i = 0; i < DXC_FRAME_COUNT; i++)
            m_mirrorCmdAllocators[i].Reset();
        ReleaseLagIndepObjects();
        ReleaseAllOrientPipelines();
        m_bMirrorForceReinit.store(true);
    } catch (...) {
        DebugLogA("SendToDisplayOutputs: unknown exception — force mirror reinit\n", LOG_ERROR);
        m_lpDX->EndAuxUpload();
        RestorePrimaryTexSizeFromVS();
        if (m_mirrorCmdList)
            m_mirrorCmdList->Close();
        if (m_lagIndepList)
            m_lagIndepList->Close();
        if (m_lpDX)
            m_lpDX->WaitForGpu();
        m_mirrorCmdList.Reset();
        for (int i = 0; i < DXC_FRAME_COUNT; i++)
            m_mirrorCmdAllocators[i].Reset();
        ReleaseLagIndepObjects();
        ReleaseAllOrientPipelines();
        m_bMirrorForceReinit.store(true);
    }
#endif
}

// ─── Independent mirror rendering ────────────────────────────────────────────

bool Engine::AnyIndependentMirrorEnabled() const
{
    if (!m_bMirrorsActive)
        return false;
    for (const auto& out : m_displayOutputs) {
        if (out.config.bEnabled &&
            out.config.type == DisplayOutputType::Monitor &&
            out.config.bIndependentRender)
            return true;
    }
    return false;
}

void Engine::DrawDeferredMessages()
{
    if (!MessagesEnabled() || !m_lpDX)
        return;
    for (int i = 0; i < NUM_SUPERTEXTS; i++) {
        if (m_supertexts[i].fStartTime >= 0 && !m_supertexts[i].bRedrawSuperText) {
            float fProgress = (GetTime() - m_supertexts[i].fStartTime) / m_supertexts[i].fDuration;
            if (fProgress <= 1.0f)
                ShowSongTitleAnim(GetWidth(), GetHeight(), min(fProgress, 0.9999f), i);
        }
    }
}

void Engine::DrawOverlaysToMirror(ID3D12GraphicsCommandList* cmdList, int monW, int monH,
                                  bool drawSprites)
{
    if (!cmdList || !m_lpDX || monW <= 0 || monH <= 0)
        return;

    // Viewport must match mirror RT (milk3 path already set it; re-assert for safety)
    SetViewportAndScissor(cmdList, (UINT)monW, (UINT)monH);

    ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

    // messages.ini / song-title anims stay on mirrors even when HUD is off.
    if (MessagesEnabled()) {
        for (int i = 0; i < NUM_SUPERTEXTS; i++) {
            if (m_supertexts[i].fStartTime >= 0 && !m_supertexts[i].bRedrawSuperText) {
                float fProgress = (GetTime() - m_supertexts[i].fStartTime) / m_supertexts[i].fDuration;
                if (fProgress <= 1.0f)
                    ShowSongTitleAnim(monW, monH, min(fProgress, 0.9999f), i, cmdList);
            }
        }
    }

    if (drawSprites && SpritesEnabled()) {
        DrawUserSprites(0, cmdList);
        DrawUserSprites(1, cmdList);
    }

    if (m_bDisableMirrorHud)
        return;

    // HUD / preset / notifications: queue was laid out in primary client pixels.
    // Pass primary as layout size so positions and font scale map to monW×monH.
    const int layoutW = m_lpDX->m_client_width > 0 ? m_lpDX->m_client_width : monW;
    const int layoutH = m_lpDX->m_client_height > 0 ? m_lpDX->m_client_height : monH;
    m_text.DrawNow(cmdList, monW, monH, false, layoutW, layoutH);
}

void Engine::RenderMilk3ImageToMirror(ID3D12GraphicsCommandList* cmdList,
                                      D3D12_CPU_DESCRIPTOR_HANDLE rtv, int monW, int monH)
{
    if (!cmdList || !m_lpDX || monW <= 0 || monH <= 0)
        return;

    // RAII: restore primary VS texsize — portrait dims must not stick into next primary frame.
    struct SizeOverrideGuard {
        Engine* e;
        explicit SizeOverrideGuard(Engine* eng, int w, int h) : e(eng) {
            e->m_nTexSizeX = w;
            e->m_nTexSizeY = h;
            e->SetOutputSizeOverride(w, h);
            e->m_fAspectX = (h > w) ? w / (float)h : 1.0f;
            e->m_fAspectY = (w > h) ? h / (float)w : 1.0f;
            e->m_fInvAspectX = 1.0f / e->m_fAspectX;
            e->m_fInvAspectY = 1.0f / e->m_fAspectY;
        }
        ~SizeOverrideGuard() { e->RestorePrimaryTexSizeFromVS(); }
    } sizeGuard(this, monW, monH);

    float black[] = { 0.f, 0.f, 0.f, 1.f };
    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList->ClearRenderTargetView(rtv, black, 0, nullptr);
    SetViewportAndScissor(cmdList, (UINT)monW, (UINT)monH);

    if (!m_dx12CompPSO)
        return;
    cmdList->SetPipelineState(m_dx12CompPSO.Get());

    PShaderInfo* compSI = &m_shaders.comp;
    if (compSI->CT) {
        ApplyShaderParams(&compSI->params, compSI->CT, m_pState);
        DX12ConstantTable* ct = static_cast<DX12ConstantTable*>(compSI->CT);
        if (ct->GetShadowSize() > 0) {
            D3D12_GPU_VIRTUAL_ADDRESS cbAddr =
                m_lpDX->UploadConstantBuffer(ct->GetShadowData(), ct->GetShadowSize());
            if (cbAddr)
                cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
        }
    } else {
        BYTE zeros[256] = {};
        D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_lpDX->UploadConstantBuffer(zeros, 256);
        if (cbAddr)
            cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
    }

    // Bindings = primary Image pass (audio shared). Do not write feedback here —
    // OM is the mirror RT only.
    cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetCompBindingGpuHandle());

    MYVERTEX v[4];
    ZeroMemory(v, sizeof(v));
    const float cx[4] = { -1.f, 1.f, -1.f, 1.f };
    const float cy[4] = { 1.f, 1.f, -1.f, -1.f };
    const float cu[4] = { 0.f, 1.f, 0.f, 1.f };
    const float cv[4] = { 0.f, 0.f, 1.f, 1.f };
    for (int i = 0; i < 4; i++) {
        v[i].x = cx[i]; v[i].y = cy[i]; v[i].z = 0.f;
        v[i].Diffuse = 0xFFFFFFFFu;
        v[i].tu = cu[i]; v[i].tv = cv[i];
        v[i].tu_orig = cu[i]; v[i].tv_orig = cv[i];
    }
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, v, 4, sizeof(MYVERTEX), cmdList);
}

void Engine::BlitMainToMirror(ID3D12GraphicsCommandList* cmdList,
                              ID3D12Resource* mainBB, int mainW, int mainH,
                              D3D12_CPU_DESCRIPTOR_HANDLE mirrorRtv, int monW, int monH,
                              int scaleMode)
{
    if (!cmdList || !m_lpDX || !mainBB || monW <= 0 || monH <= 0 || mainW <= 0 || mainH <= 0)
        return;

    // Root table is 32 SRVs — reuse one permanent block (allocate once).
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    if (m_mirrorLetterboxSrvBase == UINT_MAX) {
        D3D12_CPU_DESCRIPTOR_HANDLE first = m_lpDX->AllocateSrvCpu();
        m_mirrorLetterboxSrvBase = m_lpDX->m_nextFreeSrvSlot;
        m_lpDX->AllocateSrvGpu();
        for (UINT i = 1; i < DXContext::BINDING_BLOCK_SIZE; i++) {
            m_lpDX->AllocateSrvCpu();
            m_lpDX->AllocateSrvGpu();
        }
        (void)first;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE blockCpu = m_lpDX->GetSrvCpuHandleAt(m_mirrorLetterboxSrvBase);
    for (UINT i = 0; i < DXContext::BINDING_BLOCK_SIZE; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE slot = blockCpu;
        slot.ptr += (SIZE_T)i * m_lpDX->m_srvDescriptorSize;
        m_lpDX->m_device->CreateShaderResourceView(mainBB, &srvDesc, slot);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE blockGpu =
        m_lpDX->GetBindingBlockGpuHandleByIndex(m_mirrorLetterboxSrvBase);

    float black[] = { 0.f, 0.f, 0.f, 1.f };
    cmdList->OMSetRenderTargets(1, &mirrorRtv, FALSE, nullptr);
    cmdList->ClearRenderTargetView(mirrorRtv, black, 0, nullptr);
    SetViewportAndScissor(cmdList, (UINT)monW, (UINT)monH);

    cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
    cmdList->SetGraphicsRootDescriptorTable(1, blockGpu);
    BYTE zeros[256] = {};
    // A dedicated 256 KB buffer stood here for the one worker thread, chosen by
    // thread id. Phase 5a gave every worker its own upload ring and taught
    // SelectUploadRing to find it, so the ordinary path is already per-thread
    // correct -- and a thread-id test against ONE worker would now be wrong.
    D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_lpDX->UploadConstantBuffer(zeros, 256);
    if (cbAddr)
        cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);

    // scaleMode 0 = stretch (full UV, full NDC)
    // scaleMode 1 = letterbox/fit (shrink NDC, full UV — bars)
    // scaleMode 2 = cover/crop (full NDC, center-crop UV — fills screen)
    float nx = 1.f, ny = 1.f;
    float u0 = 0.f, u1 = 1.f, v0 = 0.f, v1 = 1.f;
    const float srcAr = (float)mainW / (float)mainH;
    const float dstAr = (float)monW / (float)monH;
    if (scaleMode == 1) {
        if (srcAr > dstAr)
            ny = dstAr / srcAr; // source wider → letterbox top/bottom
        else
            nx = srcAr / dstAr; // source taller → pillarbox left/right
    } else if (scaleMode == 2) {
        if (srcAr > dstAr) {
            // Source wider than dest — crop left/right, fill height
            const float visible = dstAr / srcAr;
            u0 = (1.f - visible) * 0.5f;
            u1 = 1.f - u0;
        } else {
            // Source taller than dest — crop top/bottom, fill width
            const float visible = srcAr / dstAr;
            v0 = (1.f - visible) * 0.5f;
            v1 = 1.f - v0;
        }
    }

    MYVERTEX v[4];
    ZeroMemory(v, sizeof(v));
    const float px[4] = { -nx, nx, -nx, nx };
    const float py[4] = { ny, ny, -ny, -ny };
    // TRIANGLESTRIP order: TL, TR, BL, BR
    const float pu[4] = { u0, u1, u0, u1 };
    const float pv[4] = { v0, v0, v1, v1 };
    for (int i = 0; i < 4; i++) {
        v[i].x = px[i]; v[i].y = py[i]; v[i].z = 0.f;
        v[i].Diffuse = 0xFFFFFFFFu;
        v[i].tu = pu[i]; v[i].tv = pv[i];
        v[i].tu_orig = pu[i]; v[i].tv_orig = pv[i];
    }
    // DrawVertices suballocates from the CALLING THREAD's ring since phase 5a,
    // so the hand-rolled vertex upload that used to sit here for the worker is
    // simply what this already does.
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, v, 4, sizeof(MYVERTEX), cmdList);
}

void Engine::ClearMirrorSwapChainAllBuffers(ID3D12GraphicsCommandList* cmdList,
                                            MonitorMirrorState& ms, UINT keepAsRtvIndex)
{
    // Intentionally only clears the current back buffer. Transitioning non-current
    // flip-model buffers (assumed PRESENT while DWM still owns them) TDRs the GPU.
    // Reset painted mask so Present path will block-cycle until every face is redrawn.
    (void)keepAsRtvIndex;
    if (!cmdList || !ms.bHasRtv)
        return;
    float black[] = { 0.f, 0.f, 0.f, 1.f };
    UINT cur = ms.swapChain ? ms.swapChain->GetCurrentBackBufferIndex() : 0;
    if (cur < ms.bufferCount && cur < MIRROR_BUFFER_COUNT && ms.backBuffers[cur])
        cmdList->ClearRenderTargetView(ms.rtvHandles[cur], black, 0, nullptr);
    ms.bNeedsFullChainClear = true;
    ms.paintedBufferMask = 0;
}

void Engine::SetMirrorIndependentRender(bool enable)
{
    if (m_bMirrorIndependentDefault == enable) {
        // Already at requested state — still report so remote clients get a response path
        return;
    }
    m_bMirrorIndependentDefault = enable;
    for (auto& out : m_displayOutputs) {
        if (out.config.type == DisplayOutputType::Monitor)
            out.config.bIndependentRender = m_bMirrorIndependentDefault;
    }
    SaveDisplayOutputSettings();

    // Independent is a DRAW PATH only. Do not destroy swap chains (that froze
    // the UI). Copy mode resizes any leftover native-sized SC to primary size
    // on the render thread (see SendToDisplayOutputs).
    for (auto& out : m_displayOutputs) {
        if (!out.monitorState) continue;
        auto& ms = *out.monitorState;
        ms.bNeedsFullChainClear = true;
        ms.paintedBufferMask = 0;
        ms.lastPath = 0;
        ms.lastDrawFI = UINT_MAX;
        ms.drawCount = 0;
        ms.presentOkCount = 0;
        ms.presentFailCount = 0;
        ms.presentSkipCount = 0;
        for (UINT bi = 0; bi < MIRROR_BUFFER_COUNT; bi++)
            ms.bbState[bi] = ms.bEverPresented
                ? D3D12_RESOURCE_STATE_PRESENT
                : D3D12_RESOURCE_STATE_COMMON;
    }

    if (m_bMirrorIndependentDefault)
        m_bMirrorResetOrientNextFrame.store(true);
    else
        ClearOutputSizeOverride();
    m_bMirrorIndepSizeDirty.store(true);

    // Ensure windows stay visible after mode flip (no hide/recreate)
    m_bRaiseMirrorsNextFrame.store(true);
    m_bMirrorStylesDirty.store(true);
    RefreshDisplaysTab();
    AddNotification(m_bMirrorIndependentDefault
        ? L"Mirrors: independent (own feedback per orientation)"
        : L"Mirrors: copy mode (stretch)");
}

void Engine::ToggleMirrorIndependentRender()
{
    SetMirrorIndependentRender(!m_bMirrorIndependentDefault);
}

// ─── Mirror Window Style Updates ─────────────────────────────────────────────

// Compute desired mirror HWND rect from config (fullscreen = full monitor,
// windowed = centered ~80% of work area, min 640x360).
static void ComputeMirrorLayout(const DisplayOutputConfig& cfg,
                                int& outX, int& outY, int& outW, int& outH)
{
    RECT mon = cfg.rcMonitor;
    // Refresh monitor rect by device name when possible
    struct FindCtx { const wchar_t* name; RECT rc; bool found; };
    FindCtx ctx = { cfg.szDeviceName, mon, false };
    EnumDisplayMonitors(NULL, NULL,
        [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<FindCtx*>(lp);
            MONITORINFOEXW mi = { sizeof(mi) };
            if (GetMonitorInfoW(hMon, &mi) &&
                wcscmp(mi.szDevice, c->name) == 0) {
                c->rc = mi.rcMonitor;
                c->found = true;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found)
        mon = ctx.rc;

    const int monW = mon.right - mon.left;
    const int monH = mon.bottom - mon.top;
    if (cfg.bFullscreen || monW <= 0 || monH <= 0) {
        outX = mon.left;
        outY = mon.top;
        outW = max(1, monW);
        outH = max(1, monH);
        return;
    }
    // Windowed: use work area if available, else 80% of monitor
    RECT work = mon;
    {
        HMONITOR hMon = MonitorFromRect(&mon, MONITOR_DEFAULTTONULL);
        if (hMon) {
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfoW(hMon, &mi))
                work = mi.rcWork;
        }
    }
    int workW = work.right - work.left;
    int workH = work.bottom - work.top;
    outW = max(640, (workW * 4) / 5);
    outH = max(360, (workH * 4) / 5);
    if (outW > workW) outW = workW;
    if (outH > workH) outH = workH;
    outX = work.left + (workW - outW) / 2;
    outY = work.top + (workH - outH) / 2;
}

void Engine::ApplyMirrorWindowStyles()
{
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor || !out.monitorState)
            continue;
        auto& ms = *out.monitorState;
        HWND hWnd = ms.hWnd;
        if (!hWnd) continue;

        // Layered only when needed — flip + WS_EX_LAYERED ghosts other monitors' pixels.
        const bool needLayered = (out.config.nOpacity < 100) || out.config.bClickThrough;
        LONG_PTR ex = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
        if (needLayered)
            ex |= WS_EX_LAYERED;
        else
            ex &= ~WS_EX_LAYERED;
        if (out.config.bClickThrough)
            ex |= WS_EX_TRANSPARENT;
        else
            ex &= ~WS_EX_TRANSPARENT;
        SetWindowLongPtrW(hWnd, GWL_EXSTYLE, ex);
        if (needLayered) {
            BYTE alpha = (BYTE)(out.config.nOpacity * 255 / 100);
            if (alpha < 3) alpha = 3;
            SetLayeredWindowAttributes(hWnd, 0, alpha, LWA_ALPHA);
        }

        // Fullscreen checkbox: full monitor vs windowed layout
        int x = 0, y = 0, w = 0, h = 0;
        ComputeMirrorLayout(out.config, x, y, w, h);
        if (w != ms.width || h != ms.height) {
            ms.pendingX = x;
            ms.pendingY = y;
            ms.pendingW = w;
            ms.pendingH = h;
            ms.bPendingLayout = true;
        } else {
            const UINT zflags = SWP_NOACTIVATE | SWP_FRAMECHANGED;
            SetWindowPos(hWnd, m_bAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                         x, y, w, h, zflags);
        }
    }
}

// ─── Displays Tab Refresh ─────────────────────────────────────────────────────

void Engine::RefreshDisplaysTab()
{
    HWND hWnd = m_displaysWindow ? m_displaysWindow->GetHWND() : NULL;
    if (!hWnd) return;

    HWND hList = GetDlgItem(hWnd, IDC_MW_DISP_LIST);
    if (!hList) return;

    // Remember the selection across the rebuild. LB_RESETCONTENT drops it, and
    // all twenty-six callers of this function therefore cleared whichever
    // display the user had picked. That was survivable while the only refreshes
    // were user actions; it stops being survivable the moment this is also
    // called on a timer, which is what makes a starting child's status update.
    const LRESULT keepSel = SendMessage(hList, LB_GETCURSEL, 0, 0);

    SendMessage(hList, LB_RESETCONTENT, 0, 0);

    // Resolve primary (render) monitor for clearer list status
    wchar_t renderDevice[32] = {};
    if (m_lpDX && m_lpDX->GetHwnd()) {
        HMONITOR hMon = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi = { sizeof(mi) };
        if (hMon && GetMonitorInfoW(hMon, &mi))
            CopyTo(renderDevice, mi.szDevice);
    }

    // Build every row's text FIRST, under the lock, into a local buffer --
    // then release the lock before touching hList at all. #27's lock must
    // never be held across a SendMessage(W) to a control owned by the
    // Displays ToolWindow thread: when this runs on the render thread (most
    // RenderCmd handlers call it), that SendMessage blocks waiting for the
    // ToolWindow thread's own message pump to answer, and if that thread is
    // itself blocked trying to take this same (recursive, but only within
    // ONE thread) lock -- which any of its own field-edit handlers can be,
    // this rapidly -- the two threads deadlock each other. Measured: this
    // exact shape hung the process solid inside a couple of seconds of
    // concurrent Add Spout/Remove clicks and IPC traffic, caught by
    // test_display_outputs_thread_safety.py.
    std::vector<std::wstring> rows;
    {
        std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);
        rows.reserve(m_displayOutputs.size());
        for (size_t i = 0; i < m_displayOutputs.size(); i++) {
            auto& out = m_displayOutputs[i];
            auto& cfg = out.config;
            wchar_t label[256];
            const wchar_t* prefix = (cfg.type == DisplayOutputType::Monitor) ? L"[Monitor]" : L"[Spout]";
            const wchar_t* status;
            if (cfg.type == DisplayOutputType::Monitor) {
                const bool isPrimary = renderDevice[0] &&
                    wcscmp(cfg.szDeviceName, renderDevice) == 0;
                if (isPrimary)
                    status = L"PRIMARY (no mirror)";
                else if (!cfg.bEnabled)
                    status = L"OFF";
                else if (!m_bMirrorsActive)
                    status = L"ON (not active)";
                else if (out.bSkippedSameMonitor)
                    status = L"SKIPPED";
                else
                    status = L"MIRRORING";
            }
            else {
                status = cfg.bEnabled ? L"ON" : L"OFF";
            }
            // A display holding its own preset says so, and says what it is
            // playing. Not being able to tell which preset was on which screen was
            // half of what made this feature hard to use (forgejo#22).
            if (cfg.type == DisplayOutputType::Monitor && cfg.bOwnProcess) {
                const DisplayPresetInfo di = DisplayPresetStatus(cfg.szDeviceName);
                // "CHILD" named the mechanism; it is "OWN" now, and FAILED is
                // gone with the process that could fail to start (#186 phase 6).
                const wchar_t* state = di.ready ? L"OWN" : L"starting";
                std::wstring leaf;
                if (!di.preset.empty()) {
                    const size_t sep = di.preset.find_last_of(L"\\/");
                    leaf = (sep == std::wstring::npos) ? di.preset
                                                      : di.preset.substr(sep + 1);
                }
                const wchar_t* preset = leaf.c_str();
                if (preset[0])
                    swprintf(label, 256, L"%s %s  (%s)  %s", prefix, cfg.szName,
                             state, preset);
                else
                    swprintf(label, 256, L"%s %s  (%s)", prefix, cfg.szName, state);
            } else {
                swprintf(label, 256, L"%s %s  (%s)", prefix, cfg.szName, status);
            }
            rows.emplace_back(label);
        }
    }
    for (const auto& row : rows)
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)row.c_str());

    if (keepSel != LB_ERR &&
        keepSel < (LRESULT)SendMessage(hList, LB_GETCOUNT, 0, 0))
        SendMessage(hList, LB_SETCURSEL, (WPARAM)keepSel, 0);

    // How far the horizontal scrollbar reaches: the widest row actually in the
    // list, measured in the list's own font.
    //
    // Without this the bar exists but does nothing, and a detail line naming a
    // long preset is simply cut off at the right edge -- which is what made the
    // window keep growing to fit names that have no length limit.
    {
        HDC hdc = GetDC(hList);
        HFONT hf = (HFONT)SendMessage(hList, WM_GETFONT, 0, 0);
        HFONT old = hf ? (HFONT)SelectObject(hdc, hf) : NULL;
        int widest = 0;
        const int count = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
        for (int i = 0; i < count; i++) {
            wchar_t row[256] = {};
            if (SendMessageW(hList, LB_GETTEXTLEN, i, 0) >= 256) continue;
            SendMessageW(hList, LB_GETTEXT, i, (LPARAM)row);
            SIZE sz = {};
            GetTextExtentPoint32W(hdc, row, (int)wcslen(row), &sz);
            if (sz.cx > widest) widest = sz.cx;
        }
        if (old) SelectObject(hdc, old);
        ReleaseDC(hList, hdc);
        SendMessage(hList, LB_SETHORIZONTALEXTENT, (WPARAM)(widest + 12), 0);
    }

    // The arrangement is the same data drawn differently, so it is stale the
    // moment the list is not.
    if (HWND hPanel = GetDlgItem(hWnd, IDC_MW_DISP_PANEL))
        InvalidateRect(hPanel, NULL, FALSE);

    // Sync Activate Mirrors button text
    HWND hBtn = GetDlgItem(hWnd, IDC_MW_DISP_ACTIVATE);
    // "Mirror/Children" named a mechanism that no longer exists. Every enabled
    // display is a mirror surface now, whatever preset it holds (#186 phase 6).
    if (hBtn) SetWindowTextW(hBtn, m_bMirrorsActive ? L"Deactivate Displays"
                                                    : L"Activate Displays");

}

// What the list would SAY about the own-preset displays right now, cheaply.
//
// The Displays list is only redrawn when something asks it to, so a display
// switched to its own preset sat on "(starting)" until the user happened to
// click something else -- it was up in well under a second and the list had no
// way to know. A timer polls this instead of rebuilding every second: the
// rebuild flickers and costs a LB_RESETCONTENT, and nothing changes between
// ticks for almost all of them.
std::wstring Engine::DisplayListSignature() const
{
    std::wstring sig;
    for (auto& out : m_displayOutputs) {
        const auto& cfg = out.config;
        if (cfg.type != DisplayOutputType::Monitor || !cfg.bOwnProcess) continue;
        const DisplayPresetInfo di = DisplayPresetStatus(cfg.szDeviceName);
        sig += cfg.szDeviceName;
        sig += !di.haveSurface ? L"|-" : di.ready ? L"|R" : L"|S";
        sig += di.preset;
        sig += L';';
    }
    return sig;
}

// Clear and disable every control in the "Selected display" group that only
// means anything for a display running its own process.
//
// Split out because the "nothing selected" branch of UpdateDisplaysTabSelection
// below used to RETURN before reaching them. Nine controls were disabled there
// and eleven were left exactly as DoBuildControls had constructed them: enabled,
// and showing their construction defaults. So with no row selected the group
// read as a live, editable panel -- and every handler behind it is gated on
// having a selection, so the "..." browse buttons and the preset combos looked
// usable and silently did nothing when used. The buttons are owner-drawn and DO
// honour ODS_DISABLED, so once they are actually disabled they look it.
static void ClearDisplayChildControls(HWND hWnd)
{
    auto uncheck = [](HWND h) {
        if (!h) return;
        SetPropW(h, L"Checked", (HANDLE)(intptr_t)0);
        InvalidateRect(h, NULL, TRUE);
    };
    const int kRadios[] = {
        IDC_MW_DISP_MODE_MIRROR, IDC_MW_DISP_MODE_CHILD,
        IDC_MW_DISP_ORDER_RANDOM, IDC_MW_DISP_ORDER_SEQ,
        IDC_MW_DISP_LOCK_INHERIT, IDC_MW_DISP_LOCK_OFF, IDC_MW_DISP_LOCK_ON,
        IDC_MW_DISP_CYCLE_ON,
    };
    for (int id : kRadios) {
        HWND h = GetDlgItem(hWnd, id);
        if (!h) continue;
        uncheck(h);
        EnableWindow(h, FALSE);
    }
    for (int id : { IDC_MW_DISP_PRESETDIR, IDC_MW_DISP_STARTUP }) {
        HWND h = GetDlgItem(hWnd, id);
        if (!h) continue;
        SetModeComboValue(h, L"");
        EnableWindow(h, FALSE);
    }
    if (HWND h = GetDlgItem(hWnd, IDC_MW_DISP_CYCLE_SECS)) {
        SetWindowTextW(h, L"");
        EnableWindow(h, FALSE);
    }
    for (int id : { IDC_MW_DISP_PRESETDIR_BR, IDC_MW_DISP_STARTUP_BR }) {
        if (HWND h = GetDlgItem(hWnd, id)) EnableWindow(h, FALSE);
    }
}

void Engine::UpdateDisplaysTabSelection(int sel)
{
    HWND hWnd = m_displaysWindow ? m_displaysWindow->GetHWND() : NULL;
    if (!hWnd) return;
    m_nDisplaysTabSel = sel;

    HWND hEnable    = GetDlgItem(hWnd, IDC_MW_DISP_ENABLE);
    HWND hFullscr   = GetDlgItem(hWnd, IDC_MW_DISP_FULLSCREEN);
    HWND hClickThru = GetDlgItem(hWnd, IDC_MW_DISP_CLICKTHRU);
    HWND hOpacity   = GetDlgItem(hWnd, IDC_MW_DISP_OPACITY);
    HWND hOpSpin    = GetDlgItem(hWnd, IDC_MW_DISP_OPACITY_SPIN);
    HWND hName      = GetDlgItem(hWnd, IDC_MW_DISP_SPOUT_NAME);
    HWND hFixed     = GetDlgItem(hWnd, IDC_MW_DISP_SPOUT_FIXED);
    HWND hW         = GetDlgItem(hWnd, IDC_MW_DISP_SPOUT_W);
    HWND hH         = GetDlgItem(hWnd, IDC_MW_DISP_SPOUT_H);

    // Helper: sync custom owner-drawn checkbox property + visual state.
    //
    // BOTH mechanisms, because this group contains both kinds of control. The
    // checkboxes come from CreateCheck and are owner-drawn from a "Checked"
    // window property; the Random/Sequential and Inherit/Unlocked/Locked
    // radios are created straight from CreateWindowExW as plain
    // BS_AUTORADIOBUTTONs and paint from the real button state.
    //
    // Setting only the property meant those five radios never showed what was
    // selected -- measured, not assumed: setting the property on "Unlocked"
    // left it hollow, while clearing the button state on "Inherit" emptied it,
    // so the paint follows the state and not the property. That affected every
    // display, not merely the main render, which is why it is fixed here
    // rather than worked around at the call site.
    //
    // BM_SETCHECK on an owner-drawn checkbox is harmless: it never reads the
    // real state when painting.
    auto SetCheckbox = [](HWND hCtrl, bool checked) {
        if (!hCtrl) return;
        SetPropW(hCtrl, L"Checked", (HANDLE)(intptr_t)(checked ? 1 : 0));
        SendMessageW(hCtrl, BM_SETCHECK,
                     (WPARAM)(checked ? BST_CHECKED : BST_UNCHECKED), 0);
        InvalidateRect(hCtrl, NULL, TRUE);
    };

    bool haveSel;
    {
        // m_displayOutputsMutex: just the size check, released immediately
        // after -- nothing below needs the lock held. #27.
        std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);
        haveSel = sel >= 0 && sel < (int)m_displayOutputs.size();
    }
    if (!haveSel) {
        // Nothing selected — clear/disable controls
        if (hEnable)    { SetCheckbox(hEnable, false); EnableWindow(hEnable, FALSE); }
        if (hFullscr)   { SetCheckbox(hFullscr, false); EnableWindow(hFullscr, FALSE); }
        if (hClickThru) { SetCheckbox(hClickThru, false); EnableWindow(hClickThru, FALSE); }
        if (hOpacity)   { SetWindowTextW(hOpacity, L""); EnableWindow(hOpacity, FALSE); }
        if (hOpSpin)    { EnableWindow(hOpSpin, FALSE); }
        if (hName)      { SetWindowTextW(hName, L""); EnableWindow(hName, FALSE); }
        if (hFixed)     { SetCheckbox(hFixed, false); EnableWindow(hFixed, FALSE); }
        if (hW)         { SetWindowTextW(hW, L""); EnableWindow(hW, FALSE); }
        if (hH)         { SetWindowTextW(hH, L""); EnableWindow(hH, FALSE); }
        // The rest of the group, which this branch used to walk straight past.
        ClearDisplayChildControls(hWnd);
        return;
    }

    // A COPY, not the live reference this used to bind here -- taken under
    // the lock and used for the rest of the function with the lock released.
    // #27's lock must never be held across a SendMessage(W)/EnableWindow/
    // SetWindowTextW to a control owned by the Displays ToolWindow thread,
    // which is everything below this point: when this function runs on the
    // render thread (most RenderCmd handlers call it), those calls block
    // waiting for the ToolWindow thread's own message pump, and if that
    // thread is itself blocked trying to take this same lock -- which any of
    // its own field-edit handlers can be -- the two threads deadlock each
    // other. Measured: this exact shape hung the process solid within
    // seconds of concurrent Add Spout/Remove clicks and IPC traffic, caught
    // by test_display_outputs_thread_safety.py. DisplayOutputConfig is
    // POD-ish (fixed-size members, no owned pointers), so the copy is cheap.
    DisplayOutputConfig cfg;
    {
        std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);
        cfg = m_displayOutputs[sel].config;
    }
    bool isSpout = (cfg.type == DisplayOutputType::Spout);
    bool isMon = !isSpout;

    // Enable checkbox — always available
    if (hEnable)  { SetCheckbox(hEnable, cfg.bEnabled); EnableWindow(hEnable, TRUE); }

    // Fullscreen — only for monitors
    if (hFullscr) { SetCheckbox(hFullscr, cfg.bFullscreen); EnableWindow(hFullscr, isMon); }

    // Click-through and opacity — only for monitors
    if (hClickThru) { SetCheckbox(hClickThru, cfg.bClickThrough); EnableWindow(hClickThru, isMon); }
    if (hOpacity) {
        wchar_t buf[8];
        swprintf(buf, 8, L"%d", cfg.nOpacity);
        SetWindowTextW(hOpacity, isMon ? buf : L"");
        EnableWindow(hOpacity, isMon);
    }
    if (hOpSpin) {
        if (isMon) SendMessage(hOpSpin, UDM_SETPOS32, 0, cfg.nOpacity);
        EnableWindow(hOpSpin, isMon);
    }

    // Spout-specific fields
    if (hName) { SetWindowTextW(hName, isSpout ? cfg.szName : L""); EnableWindow(hName, isSpout); }
    if (hFixed) { SetCheckbox(hFixed, cfg.bFixedSize); EnableWindow(hFixed, isSpout); }
    wchar_t buf[32];
    if (hW) { swprintf(buf, 32, L"%d", cfg.nWidth); SetWindowTextW(hW, isSpout ? buf : L""); EnableWindow(hW, isSpout); }
    if (hH) { swprintf(buf, 32, L"%d", cfg.nHeight); SetWindowTextW(hH, isSpout ? buf : L""); EnableWindow(hH, isSpout); }

    // ── Per-display child settings (forgejo#22) ──────────────────────────
    //
    // Only a monitor can be a child, and only a child has a preset directory
    // of its own -- a mirror renders the primary's preset. Grey the rest out
    // rather than leave controls that silently do nothing.
    {
        HWND hModeMir  = GetDlgItem(hWnd, IDC_MW_DISP_MODE_MIRROR);
        HWND hModeChi  = GetDlgItem(hWnd, IDC_MW_DISP_MODE_CHILD);
        HWND hDir      = GetDlgItem(hWnd, IDC_MW_DISP_PRESETDIR);
        HWND hDirBr    = GetDlgItem(hWnd, IDC_MW_DISP_PRESETDIR_BR);
        HWND hStartup  = GetDlgItem(hWnd, IDC_MW_DISP_STARTUP);
        HWND hStartBr  = GetDlgItem(hWnd, IDC_MW_DISP_STARTUP_BR);
        HWND hCycleOn  = GetDlgItem(hWnd, IDC_MW_DISP_CYCLE_ON);
        HWND hCycleSec = GetDlgItem(hWnd, IDC_MW_DISP_CYCLE_SECS);
        HWND hOrdRnd   = GetDlgItem(hWnd, IDC_MW_DISP_ORDER_RANDOM);
        HWND hOrdSeq   = GetDlgItem(hWnd, IDC_MW_DISP_ORDER_SEQ);

        const bool isChild = isMon && cfg.bOwnProcess;

        // The monitor the main window is on is a display too, and selecting it
        // now edits the MAIN RENDER's preset settings -- for the profile being
        // saved, not for the app.
        //
        // It is the same tile the panel already labels "MAIN WINDOW", so there
        // is nothing new to find: pick the screen the visualiser is on and the
        // preset controls describe it. Before this they greyed out on that one
        // tile, because only an own-process child had a preset of its own --
        // so a profile could restore every screen except the largest, and the
        // main window's preset could only be changed from the Presets window,
        // where a change is global rather than part of a setup.
        //
        // Two important differences from a child, both of which follow from
        // the main render not being a child:
        //
        //   * these values live in m_primaryProfile, not in out.config, and
        //     nothing here writes settings.ini -- so a profile that is never
        //     loaded changes nothing;
        //   * "(default)" means inherit the global rather than inherit the
        //     main window, because for this row the main window IS the thing
        //     being described.
        wchar_t renderDev[32] = {};
        const bool isRenderHost =
            isMon && GetRenderMonitorDevice(renderDev) && renderDev[0] &&
            wcscmp(renderDev, cfg.szDeviceName) == 0;
        // Either kind can carry preset settings; which STORE they come from is
        // what differs below.
        const bool hasPresets = isChild || isRenderHost;
        const PrimaryProfileCfg& pp = m_primaryProfile;

        if (hModeMir) {
            SetCheckbox(hModeMir, isMon && !cfg.bOwnProcess);
            EnableWindow(hModeMir, isMon);
        }
        if (hModeChi) {
            SetCheckbox(hModeChi, isChild);
            EnableWindow(hModeChi, isMon);
        }
        // Both are mode combos now (CreateModeCombo), so an empty stored value
        // is shown as the mode it has always meant rather than as a blank the
        // user would have to guess at.
        const wchar_t* showDir = L"";
        const wchar_t* showPreset = L"";
        if (isRenderHost) {
            showDir    = pp.szPresetDir[0] ? pp.szPresetDir : kDispModeDefault;
            // <Default> rather than <Random>: an unset child picks a random
            // preset because that is what a child with no pin does, but an
            // unset MAIN render means "whatever the profile captures", which
            // is the running preset. Showing <Random> here would claim the
            // profile had asked for a random one.
            showPreset = pp.szStartupPreset[0] ? pp.szStartupPreset
                                               : kDispModeDefault;
        } else if (isChild) {
            showDir    = cfg.szPresetDir[0] ? cfg.szPresetDir : kDispModeDefault;
            showPreset = cfg.szStartupPreset[0] ? cfg.szStartupPreset
                                                : kDispModeRandom;
        }
        if (hDir)     { SetModeComboValue(hDir, showDir);
                        EnableWindow(hDir, hasPresets); }
        if (hDirBr)   EnableWindow(hDirBr, hasPresets);
        if (hStartup) { SetModeComboValue(hStartup, showPreset);
                        EnableWindow(hStartup, hasPresets); }
        if (hStartBr) EnableWindow(hStartBr, hasPresets);

        // fTimeBetweenPresets: -1 inherits the parent's, 0 means no cycling.
        // The checkbox is "does this display cycle at all", so an inherited
        // value shows as ticked with the parent's number in the box.
        //
        // For the main render, -1 inherits the GLOBAL interval, so the box
        // shows what the app is actually cycling at when the profile has not
        // pinned one -- the same thing a child's inherited value shows.
        float eff;
        if (isRenderHost)
            eff = pp.fTimeBetweenPresets >= 0.0f ? pp.fTimeBetweenPresets
                                                 : m_fTimeBetweenPresets;
        else
            eff = EffectiveChildInterval(cfg);
        if (hCycleOn)  { SetCheckbox(hCycleOn, hasPresets && eff > 0.0f);
                         EnableWindow(hCycleOn, hasPresets); }
        if (hCycleSec) {
            swprintf(buf, 32, L"%d", (int)(eff > 0.0f ? eff : 30.0f));
            SetWindowTextW(hCycleSec, hasPresets ? buf : L"");
            EnableWindow(hCycleSec, hasPresets && eff > 0.0f);
        }
        const bool seq = isRenderHost
            ? (pp.nSequentialOrder >= 0 ? pp.nSequentialOrder != 0
                                        : m_bSequentialPresetOrder)
            : cfg.bSequentialOrder;
        if (hOrdRnd) { SetCheckbox(hOrdRnd, hasPresets && !seq);
                       EnableWindow(hOrdRnd, hasPresets); }
        if (hOrdSeq) { SetCheckbox(hOrdSeq, hasPresets && seq);
                       EnableWindow(hOrdSeq, hasPresets); }

        // Preset lock: the RAW value picks the radio, because which of the
        // three is selected is exactly the thing the user set. Showing the
        // resolved value here would make Inherit unselectable the moment the
        // main window happened to agree with it.
        HWND hLkInh = GetDlgItem(hWnd, IDC_MW_DISP_LOCK_INHERIT);
        HWND hLkOff = GetDlgItem(hWnd, IDC_MW_DISP_LOCK_OFF);
        HWND hLkOn  = GetDlgItem(hWnd, IDC_MW_DISP_LOCK_ON);
        // The main render shows its OWN lock, not a pin, and cannot inherit --
        // it is what every other display inherits FROM.
        const int lockRaw = isRenderHost ? (m_bPresetLockedByUser ? 1 : 0)
                                         : cfg.nPresetLock;
        if (hLkInh) { SetCheckbox(hLkInh, hasPresets && lockRaw < 0);
                      EnableWindow(hLkInh, hasPresets && !isRenderHost); }
        if (hLkOff) { SetCheckbox(hLkOff, hasPresets && lockRaw == 0);
                      EnableWindow(hLkOff, hasPresets); }
        if (hLkOn)  { SetCheckbox(hLkOn,  hasPresets && lockRaw == 1);
                      EnableWindow(hLkOn,  hasPresets); }
    }
}

// ─── Display Profile Save / Load ─────────────────────────────────────────────

// ── Main-render settings a loaded profile is standing in front of ────────
//
// See PrimaryOverride in engine.h for why these exist. In short: a profile's
// main-render settings apply to the running session only, and the settings
// save has to keep writing the globals underneath them.

void Engine::ApplyPrimaryProfilePreset(const std::wstring& path) {
    if (path.empty()) return;
    // Queued, not called. LoadDisplayProfile runs on the UI thread and the
    // preset must change on the render thread -- the same route
    // DISPLAY_PRESET takes for the primary window.
    //
    // No existence check: this can be a UNC path, and stat-ing one from here
    // would stall the frame loop. A path that is not there fails in LoadPreset
    // exactly as a typed one does, and says so on screen.
    RenderCommand rc;
    rc.cmd = RenderCmd::LoadPresetPath;
    rc.iParam1 = -1;
    rc.fParam = m_fBlendTimeUser;
    rc.sParam = path;
    EnqueueRenderCmd(rc);
}

void Engine::StashPrimaryGlobal(int field) {
    PrimaryOverride& o = m_primaryOverride;
    switch (field) {
        case kPrimaryOverrideDir:
            if (o.hasDir) return;
            wcsncpy_s(o.presetDir, m_szPresetDir, _TRUNCATE);
            o.hasDir = true;
            break;
        case kPrimaryOverrideStartup:
            if (o.hasStartup) return;
            wcsncpy_s(o.presetStartup, m_szPresetStartup, _TRUNCATE);
            o.hasStartup = true;
            break;
        case kPrimaryOverrideTime:
            if (o.hasTime) return;
            o.timeBetweenPresets = m_fTimeBetweenPresets;
            o.hasTime = true;
            break;
        case kPrimaryOverrideOrder:
            if (o.hasOrder) return;
            o.sequentialOrder = m_bSequentialPresetOrder;
            o.hasOrder = true;
            break;
        case kPrimaryOverrideLock:
            if (o.hasLock) return;
            o.presetLocked = m_bPresetLockedByUser;
            o.hasLock = true;
            break;
        default:
            return;
    }
    o.has = true;
}

void Engine::ClearPrimaryOverride(int field) {
    if (!m_primaryOverride.has) return;
    switch (field) {
        case kPrimaryOverrideDir:     m_primaryOverride.hasDir = false; break;
        case kPrimaryOverrideStartup: m_primaryOverride.hasStartup = false; break;
        case kPrimaryOverrideTime:    m_primaryOverride.hasTime = false; break;
        case kPrimaryOverrideOrder:   m_primaryOverride.hasOrder = false; break;
        case kPrimaryOverrideLock:    m_primaryOverride.hasLock = false; break;
        default: return;
    }
    // Nothing overridden any more: drop the whole record, so the next profile
    // stashes a fresh -- and correct -- set of globals.
    if (!m_primaryOverride.hasDir && !m_primaryOverride.hasStartup &&
        !m_primaryOverride.hasTime && !m_primaryOverride.hasOrder &&
        !m_primaryOverride.hasLock)
        m_primaryOverride.has = false;
}

const wchar_t* Engine::GlobalPresetDir() const {
    return (m_primaryOverride.has && m_primaryOverride.hasDir)
        ? m_primaryOverride.presetDir : m_szPresetDir;
}

void Engine::SetMainRenderInterval(float secs) {
    if (secs < 0.0f) secs = 0.0f;
    m_fTimeBetweenPresets = secs;
    // -1 is this engine's "recompute on the next UpdateTime" flag, so a lowered
    // interval takes effect now rather than waiting out the old deadline. Same
    // reason SET_TIME_BETWEEN_PRESETS does it.
    m_fNextPresetTime = -1.0f;
    ClearPrimaryOverride(kPrimaryOverrideTime);
    m_primaryProfile.fTimeBetweenPresets = -1.0f;
    Config().SetFloat(L"Settings", L"fTimeBetweenPresets", m_fTimeBetweenPresets);
}

void Engine::SetMainRenderOrder(bool sequential) {
    m_bSequentialPresetOrder = sequential;
    ClearPrimaryOverride(kPrimaryOverrideOrder);
    m_primaryProfile.nSequentialOrder = -1;
    Config().SetInt(L"Settings", L"bSequentialPresetOrder", sequential ? 1 : 0);
}

void Engine::SetMainRenderLock(bool locked) {
    // The live lock, and only that. bPresetLockOnAtStartup is a PREFERENCE --
    // how the NEXT run begins -- and is written by the Settings window alone.
    SetUserPresetLock(locked);
    ClearPrimaryOverride(kPrimaryOverrideLock);
    m_primaryProfile.nPresetLock = -1;
}

// The "Enabled" checkbox and SET_DISPLAY_ENABLED share this, so there is one
// place that knows disabling an own-process display has to stop its child --
// not two copies that could disagree, the same shape as SetMainRenderLock
// above and the reason it exists.
//
// Reported live: "unchecking 'enabled' while on the display screen doesn't
// disable or stop a child." The checkbox handler only ever set bEnabled and
// never touched the child at all -- not on disable, and, by the same
// omission, not on re-enable either. Confirmed by reading the handler: no
// RequestChildKill/RequestChildSpawn anywhere in it, unlike every other place
// a display's own-process state changes (SET_DISPLAY_MODE's wantMirror and
// wantOff branches, the Displays tab's Mirror/Child radio buttons).
//
// bOwnProcess itself is left untouched, deliberately: "Enabled" is a power
// switch, not a mode selector. Unlike SET_DISPLAY_MODE=<N>,off (a three-way
// exclusive choice where "off" really does mean neither mirror nor child),
// this checkbox sits ABOVE the Mirror/Child radio pair in the UI, so turning
// a display off and back on should return it to whatever it was set to, not
// forget it.
void Engine::SetDisplayEnabled(DisplayOutput& out, bool enabled) {
    // Spawning or killing this display's child process stood here. The surface
    // that renders it is created and retired by SendToDisplayOutputs from the
    // config, which this has just written, so the next pass does both.
    out.config.bEnabled = enabled;
}

float Engine::GlobalTimeBetweenPresets() const {
    return (m_primaryOverride.has && m_primaryOverride.hasTime)
        ? m_primaryOverride.timeBetweenPresets : m_fTimeBetweenPresets;
}

bool Engine::GlobalSequentialOrder() const {
    return (m_primaryOverride.has && m_primaryOverride.hasOrder)
        ? m_primaryOverride.sequentialOrder : m_bSequentialPresetOrder;
}

bool Engine::GlobalPresetLocked() const {
    return (m_primaryOverride.has && m_primaryOverride.hasLock)
        ? m_primaryOverride.presetLocked : m_bPresetLockedByUser;
}

bool Engine::SaveDisplayProfile(const wchar_t* filePath,
                                bool bIncludePrimary, bool bIncludeMessaging,
                                bool bSnapshot)
{
    JsonWriter w;
    w.BeginObject();
    // v2 adds per-display child settings and the optional primary/messaging
    // sections. v1 files still load; see LoadDisplayProfile.
    w.Int(L"version", 2);
    w.Float(L"mainWindowOpacity", fOpacity);
    w.Bool(L"mirrorsActive", m_bMirrorsActive);
    w.Int(L"altSMode", m_nAltSMode);
    w.Bool(L"autostartAllDisplays", m_bProfileAutostartAll);
    // v2 readers only know the bool. Keep it truthful for them.
    w.Bool(L"mirrorModeForAltS", m_nAltSMode == ALTS_MIRROR);

    w.BeginArray(L"displays");
    for (auto& out : m_displayOutputs) {
        auto& cfg = out.config;
        w.BeginObject();
        w.String(L"type", cfg.type == DisplayOutputType::Monitor ? L"Monitor" : L"Spout");
        w.String(L"name", cfg.szName);
        w.Bool(L"enabled", cfg.bEnabled);

        if (cfg.type == DisplayOutputType::Monitor) {
            w.String(L"deviceName", cfg.szDeviceName);
            w.Bool(L"fullscreen", cfg.bFullscreen);
            w.Int(L"opacity", cfg.nOpacity);
            w.Bool(L"clickThrough", cfg.bClickThrough);
            w.Bool(L"independentRender", cfg.bIndependentRender);
            // Per-display child settings (forgejo#22)
            w.Bool(L"ownProcess", cfg.bOwnProcess);
            w.String(L"presetDir", cfg.szPresetDir);
            // A snapshot records what this display is PLAYING, so loading the
            // profile puts that preset back rather than whatever was pinned as
            // its startup preset -- usually nothing, in which case the display
            // would come back on a random one. Falls back to the pinned value
            // for a display whose surface has not adopted anything yet.
            {
                std::wstring startup = cfg.szStartupPreset;
                if (bSnapshot && cfg.bOwnProcess) {
                    const DisplayPresetInfo di = DisplayPresetStatus(cfg.szDeviceName);
                    if (!di.preset.empty()) startup = di.preset;
                }
                w.String(L"startupPreset", startup.c_str());
            }
            w.Float(L"timeBetweenPresets", cfg.fTimeBetweenPresets);
            w.Bool(L"sequentialOrder", cfg.bSequentialOrder);
            // -1 inherit / 0 unlocked / 1 locked, so a profile can carry "this
            // display never advances" independently of the main window.
            w.Int(L"presetLock", cfg.nPresetLock);
        } else {
            w.Bool(L"fixedSize", cfg.bFixedSize);
            w.Int(L"width", cfg.nWidth);
            w.Int(L"height", cfg.nHeight);
        }
        w.EndObject();
    }
    w.EndArray();

    // Optional: the primary window's own preset settings. Added so a profile
    // can capture a whole test setup rather than only the display topology.
    //
    // Normally NOT the running preset -- these are the settings that decide
    // what it plays, not an instruction to jump somewhere now. A SNAPSHOT is
    // the deliberate exception: its whole purpose is "put these exact presets
    // back on these exact screens", so there it records what is playing.
    if (bIncludePrimary) {
        w.BeginObject(L"primary");
        // The Main render group OVERRIDES what would otherwise be captured; it
        // does not replace the capture.
        //
        // Both halves matter. A profile saved from this window is a snapshot --
        // "put these presets back on these screens" -- so leaving a field at
        // "(default)" has to go on recording what the main window is actually
        // doing, exactly as it always did and as every display entry above
        // does. Writing only the pinned values would quietly turn every
        // existing profile's main-render section into "no opinion".
        //
        // What the group adds is the ability to say something ELSE: pin a
        // preset the main window is not playing, and that is what the profile
        // carries. Which is the whole request -- those settings were reachable
        // only from the Presets window, where changing one changes the global.
        const auto& pp = m_primaryProfile;

        w.String(L"presetDir",
                 pp.szPresetDir[0] ? pp.szPresetDir : m_szPresetDir);

        // Pinned first, then the running preset for a snapshot, then whatever
        // is merely pinned globally. A display entry resolves in the same
        // order for the same reason.
        const wchar_t* preset = pp.szStartupPreset;
        if (!preset[0])
            preset = (bSnapshot && m_szCurrentPresetFile[0])
                         ? m_szCurrentPresetFile : m_szPresetStartup;
        w.String(L"startupPreset", preset);

        w.Float(L"timeBetweenPresets",
                pp.fTimeBetweenPresets >= 0.0f ? pp.fTimeBetweenPresets
                                               : m_fTimeBetweenPresets);
        // -1 inherit / 0 random / 1 sequential. This was a plain JSON bool,
        // which has no room for inherit; asInt reads an old true/false back as
        // 1/0, so a v2 profile keeps meaning what it meant.
        w.Int(L"sequentialOrder",
              pp.nSequentialOrder >= 0 ? pp.nSequentialOrder
                                       : (m_bSequentialPresetOrder ? 1 : 0));
        w.Int(L"presetLock",
              pp.nPresetLock >= 0 ? pp.nPresetLock
                                  : (m_bPresetLockedByUser ? 1 : 0));
        w.EndObject();
    }

    // Optional: the messaging setup, message set INLINED.
    //
    // Not a path to messages.ini. The point of this section is that a captured
    // setup keeps meaning the same thing later, and a reference to a file that
    // gets edited next week silently stops doing that. Loading replaces the
    // in-memory message set and writes nothing -- the Messages window persists
    // exactly as it always has.
    if (bIncludeMessaging) {
        w.BeginObject(L"messaging");
        w.Bool(L"autoplay", m_bMsgAutoplay);
        w.Bool(L"sequential", m_bMsgSequential);
        w.Float(L"interval", m_fMsgAutoplayInterval);
        w.Float(L"jitter", m_fMsgAutoplayJitter);
        w.Int(L"maxOnScreen", m_nMsgMaxOnScreen);
        w.Bool(L"overrideRandomFont", m_bMsgOverrideRandomFont);
        w.Bool(L"overrideRandomColor", m_bMsgOverrideRandomColor);
        w.Bool(L"overrideRandomSize", m_bMsgOverrideRandomSize);
        w.Bool(L"overrideRandomEffects", m_bMsgOverrideRandomEffects);
        w.Bool(L"overrideRandomPos", m_bMsgOverrideRandomPos);
        w.Bool(L"overrideRandomGrowth", m_bMsgOverrideRandomGrowth);
        w.Bool(L"overrideSlideIn", m_bMsgOverrideSlideIn);
        w.Bool(L"overrideRandomDuration", m_bMsgOverrideRandomDuration);
        w.Bool(L"overrideShadow", m_bMsgOverrideShadow);
        w.Bool(L"overrideBox", m_bMsgOverrideBox);
        w.Bool(L"overrideApplyHueShift", m_bMsgOverrideApplyHueShift);
        w.Bool(L"overrideRandomHue", m_bMsgOverrideRandomHue);
        w.Bool(L"ignorePerMsgRandom", m_bMsgIgnorePerMsgRandom);

        w.BeginArray(L"messages");
        for (int i = 0; i < MAX_CUSTOM_MESSAGES; i++) {
            const td_custom_msg& m = m_CustomMessage[i];
            if (!m.szText[0]) continue;      // empty slot
            w.BeginObject();
            w.Int(L"slot", i);
            w.String(L"text", m.szText);
            w.Int(L"font", m.nFont);
            w.Float(L"size", m.fSize);
            w.Float(L"x", m.x);
            w.Float(L"y", m.y);
            w.Float(L"randx", m.randx);
            w.Float(L"randy", m.randy);
            w.Float(L"growth", m.growth);
            w.Float(L"time", m.fTime);
            w.Float(L"fade", m.fFade);
            w.Float(L"fadeOut", m.fFadeOut);
            w.Int(L"animProfile", m.nAnimProfile);
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    }

    w.EndObject();

    return w.SaveToFile(filePath);
}

// ─── Testing-mode snapshot / restore ─────────────────────────────────────────

void Engine::TakeDisplaySnapshot()
{
    m_displaySnapshot.clear();
    for (const auto& o : m_displayOutputs)
        m_displaySnapshot.push_back(o.config);
    m_snapAltSMode                 = m_nAltSMode;
    m_snapMirrorPromptDisabled     = m_bMirrorPromptDisabled;
    m_snapMirrorIndependentDefault = m_bMirrorIndependentDefault;
    m_snapDisableMirrorHud         = m_bDisableMirrorHud;
    m_snapMirrorMaxFps             = m_nMirrorMaxFps.load();
    m_bHaveDisplaySnapshot = true;
    DLOG_INFO("testing mode: snapshotted %zu display output(s) to restore on exit",
              m_displaySnapshot.size());
}

void Engine::RestoreDisplaysAfterTesting()
{
    if (!m_bHaveDisplaySnapshot) {
        mdrop::SetConfigWriteShield(false);
        return;
    }
    m_bHaveDisplaySnapshot = false;

    int changed = 0;
    for (auto& out : m_displayOutputs) {
        const DisplayOutputConfig* was = nullptr;
        for (const auto& snap : m_displaySnapshot) {
            if (snap.type != out.config.type) continue;
            // Monitors are identified by device name; a Spout output by its
            // sender name. Both are owned by the machine or the user, never by
            // the settings this restores.
            const wchar_t* a = (out.config.type == DisplayOutputType::Monitor)
                             ? out.config.szDeviceName : out.config.szName;
            const wchar_t* b = (snap.type == DisplayOutputType::Monitor)
                             ? snap.szDeviceName : snap.szName;
            if (wcscmp(a, b) == 0) { was = &snap; break; }
        }
        // A display that appeared DURING the run has no snapshot, so there is
        // nothing of the user's to put back and it is left exactly as it is.
        if (!was) continue;

        // Only the fields SaveDisplayOutputSettings republishes. Never the
        // whole struct: type, szDeviceName, szName and rcMonitor belong to
        // EnumerateDisplayOutputs, and assigning over them would turn a
        // monitor whose section is missing into a nameless Spout row.
        const bool ownWas = out.config.bOwnProcess;
        const bool enWas  = out.config.bEnabled;
        if (out.config.bEnabled            != was->bEnabled ||
            out.config.bFullscreen         != was->bFullscreen ||
            out.config.nOpacity            != was->nOpacity ||
            out.config.bClickThrough       != was->bClickThrough ||
            out.config.bIndependentRender  != was->bIndependentRender ||
            out.config.bOwnProcess         != was->bOwnProcess ||
            out.config.fTimeBetweenPresets != was->fTimeBetweenPresets ||
            out.config.bSequentialOrder    != was->bSequentialOrder ||
            out.config.nPresetLock         != was->nPresetLock ||
            wcscmp(out.config.szPresetDir,     was->szPresetDir) != 0 ||
            wcscmp(out.config.szStartupPreset, was->szStartupPreset) != 0 ||
            out.config.bFixedSize          != was->bFixedSize ||
            out.config.nWidth              != was->nWidth ||
            out.config.nHeight             != was->nHeight)
            changed++;

        out.config.bEnabled           = was->bEnabled;
        out.config.bFullscreen        = was->bFullscreen;
        out.config.nOpacity           = was->nOpacity;
        out.config.bClickThrough      = was->bClickThrough;
        out.config.bIndependentRender = was->bIndependentRender;
        out.config.bOwnProcess        = was->bOwnProcess;
        out.config.fTimeBetweenPresets = was->fTimeBetweenPresets;
        out.config.bSequentialOrder   = was->bSequentialOrder;
        out.config.nPresetLock        = was->nPresetLock;
        wcsncpy_s(out.config.szPresetDir,     was->szPresetDir,     _TRUNCATE);
        wcsncpy_s(out.config.szStartupPreset, was->szStartupPreset, _TRUNCATE);
        out.config.bFixedSize = was->bFixedSize;
        out.config.nWidth     = was->nWidth;
        out.config.nHeight    = was->nHeight;

        // Restoring the FIELD is not restoring the WORLD. A child process is
        // spawned and retired only by an explicit request -- nothing re-derives
        // it from config -- so a run that promoted a display would otherwise
        // leave a full second instance rendering with no record tying it to a
        // display, which is forgejo#35 by another door.
        // Spawning or retiring this display's child process stood here.
        // SendToDisplayOutputs creates and retires the surface from the config
        // on its next pass, which the restore above has just written.
        (void)ownWas; (void)enWas;
    }

    m_nAltSMode                 = m_snapAltSMode;
    m_bMirrorPromptDisabled     = m_snapMirrorPromptDisabled;
    m_bMirrorIndependentDefault = m_snapMirrorIndependentDefault;
    m_bDisableMirrorHud         = m_snapDisableMirrorHud;
    m_nMirrorMaxFps.store(m_snapMirrorMaxFps);

    // Opacity, click-through and fullscreen only reach a live window through
    // ApplyMirrorWindowStyles, which the render thread runs off these flags.
    if (changed) {
        m_bMirrorStylesDirty.store(true);
        m_bMirrorIndepSizeDirty.store(true);
        m_bRaiseMirrorsNextFrame.store(true);
    }

    // LAST. Until this line the file is still protected, so a process killed
    // before this command runs -- which is how most sweeps end -- leaves the
    // user's settings untouched rather than half-restored.
    mdrop::SetConfigWriteShield(false);

    DLOG_WARN("Testing mode OFF: %d display output(s) restored to their "
              "pre-test state, shield lowered", changed);
    if (changed) {
        wchar_t msg[128];
        FormatTo(msg, L"Testing mode off - %d display setting(s) restored", changed);
        AddNotification(msg);
    }
}

bool Engine::GetRenderMonitorDevice(wchar_t (&dev)[32]) const
{
    dev[0] = 0;
    if (m_bMirrorWatermarkActive && m_szWatermarkRenderDevice[0]) {
        CopyTo(dev, m_szWatermarkRenderDevice);
        return true;
    }
    if (!m_lpDX || !m_lpDX->GetHwnd()) return false;
    HMONITOR h = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi = { sizeof(mi) };
    if (!h || !GetMonitorInfoW(h, &mi)) return false;
    CopyTo(dev, mi.szDevice);
    return true;
}

void Engine::ResetDisplayOutputsToDefaults()
{
    // Soft-park rather than destroy, for the same reason LoadDisplayProfile
    // does: WaitForGpu + DestroyWindow from the UI thread races Present and
    // has TDR'd. The render thread hides them from the new flags.
    for (auto& out : m_displayOutputs) {
        if (out.monitorState) {
            out.monitorState->bSoftDisabled = true;
            if (out.monitorState->hWnd)
                ShowWindow(out.monitorState->hWnd, SW_HIDE);
        }
    }

    // Globals, back to the values LoadDisplayOutputSettings would pick when
    // the keys are absent.
    m_bMirrorsActive = false;
    m_nAltSMode = ALTS_STRETCH;
    m_bMirrorPromptDisabled = false;
    m_bMirrorIndependentDefault = false;
    m_bDisableMirrorHud = false;
    m_nMirrorMaxFps.store(0);

    // m_displayOutputsMutex guards the erase/insert below against
    // ChildConfigureNow's read on the child-worker thread. Scoped to just
    // this block, not the whole function, because EnumerateDisplayOutputs()
    // below takes the same (non-recursive) lock itself. #27.
    int oldCount;
    {
        std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);

        // Every Spout output is user-added, so they all go; a fresh install has
        // exactly one, disabled, named MDropDX12. Destroying a Spout sender from
        // this thread is what the Remove button already does.
        oldCount = (int)m_displayOutputs.size();
        for (auto it = m_displayOutputs.begin(); it != m_displayOutputs.end(); ) {
            if (it->config.type == DisplayOutputType::Spout) {
                DestroyDisplayOutput(*it);
                it = m_displayOutputs.erase(it);
            } else {
                ++it;
            }
        }

        // Monitors keep their identity -- they are the machine's, not a setting --
        // but every choice made about one goes back to the struct default. That
        // includes the per-display child fields, so no child is left configured to
        // spawn on the next start.
        for (auto& out : m_displayOutputs) {
            DisplayOutputConfig& c = out.config;
            const DisplayOutputConfig def;
            c.bEnabled = false;
            c.bFullscreen = def.bFullscreen;
            c.nOpacity = def.nOpacity;
            c.bClickThrough = def.bClickThrough;
            c.bIndependentRender = def.bIndependentRender;
            c.bOwnProcess = def.bOwnProcess;
            c.szPresetDir[0] = 0;
            c.szStartupPreset[0] = 0;
            c.fTimeBetweenPresets = def.fTimeBetweenPresets;
            c.bSequentialOrder = def.bSequentialOrder;
            c.nPresetLock = def.nPresetLock;
        }

        DisplayOutput spout;
        spout.config.type = DisplayOutputType::Spout;
        spout.config.bEnabled = false;
        CopyTo(spout.config.szName, L"MDropDX12");
        m_displayOutputs.insert(m_displayOutputs.begin(), std::move(spout));
    }

    bSpoutOut = false;
    bSpoutChanged = true;
    HWND hw = GetPluginWindow();
    if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0);

    // SaveDisplayOutputSettings writes Count and then sections 0..Count-1; it
    // never removes the ones above that. Shrinking the list without this leaves
    // DisplayOutput_N behind to be picked up if the count ever grows back.
    for (int i = (int)m_displayOutputs.size(); i < oldCount + 8; i++) {
        wchar_t section[64];
        swprintf(section, 64, L"DisplayOutput_%d", i);
        DisplayCfg().RemoveSection(section);
    }

    // Re-enumerate so the monitor list matches the machine as it is now, then
    // persist, so the reset survives a crash rather than only a clean exit.
    EnumerateDisplayOutputs();
    SaveDisplayOutputSettings();
    ConfigFlushAll();
}

bool Engine::LoadDisplayProfile(const wchar_t* filePath)
{
    JsonValue root = JsonLoadFile(filePath);
    if (!root.isObject()) return false;

    int version = root[L"version"].asInt(0);
    if (version < 1) return false;

    // Soft-park only — never Destroy from the UI thread (WaitForGpu + DestroyWindow
    // races with Present and TDRs). Render thread will show/hide from new flags.
    for (auto& out : m_displayOutputs) {
        if (out.monitorState) {
            out.monitorState->bSoftDisabled = true;
            if (out.monitorState->hWnd)
                ShowWindow(out.monitorState->hWnd, SW_HIDE);
        }
    }

    // Main window opacity
    if (root.has(L"mainWindowOpacity")) {
        fOpacity = root[L"mainWindowOpacity"].asFloat(1.0f);
        if (fOpacity < 0.0f) fOpacity = 0.0f;
        if (fOpacity > 1.0f) fOpacity = 1.0f;
        // Apply via message to render window (owns the HWND)
        HWND hw = GetPluginWindow();
        if (hw) PostMessage(hw, WM_MW_SET_OPACITY, 0, 0);
    }

    // Global flags
    m_bMirrorsActive = root[L"mirrorsActive"].asBool(false);
    // A v2 profile carries only the bool; a newer one carries the mode.
    m_nAltSMode = root[L"altSMode"].asInt(
        root[L"mirrorModeForAltS"].asBool(false) ? ALTS_MIRROR : ALTS_STRETCH);
    if (m_nAltSMode < ALTS_STRETCH || m_nAltSMode > ALTS_PROFILE)
        m_nAltSMode = ALTS_STRETCH;

    // A profile is a complete display state, not a patch.
    //
    // This used to only ever ADD Spout outputs and leave any monitor the
    // profile did not name exactly as the previous profile had set it, so
    // A -> B -> A did not round-trip and senders accumulated across every
    // profile ever loaded. Turn everything off first, drop Spout outputs the
    // profile does not name, and let the loop below turn back on only what it
    // asks for. Applies to v1 files too -- this is the bug fix, not a v2
    // feature.
    const auto& displays = root[L"displays"];

    // m_displayOutputsMutex guards this whole rewrite -- the erase just below
    // plus the per-display match/insert loop that follows -- against
    // ChildConfigureNow's read on the child-worker thread. A unique_lock,
    // unlocked explicitly once the loop finishes (rather than a lock_guard
    // scoped to a new brace block), so the diff against the existing
    // structure stays small. Not held across EnumerateDisplayOutputs() above,
    // which takes the same (non-recursive) lock itself. #27.
    std::unique_lock<std::recursive_mutex> lkDisp(m_displayOutputsMutex);

    // Which monitors this profile actually talks about, by device name.
    // "Autostart all displays" (below) must stay scoped to these -- a profile
    // is a complete state, not a patch (forgejo#22), so a monitor the profile
    // is silent about has to be left off by the reset above, not swept up by
    // a blanket enable meant for the displays the profile names.
    std::vector<std::wstring> namedMonitors;
    {
        std::vector<std::wstring> namedSpouts;
        for (size_t i = 0; i < displays.size(); i++) {
            const auto& d = displays.at(i);
            if (d[L"type"].asString(L"") == L"Spout")
                namedSpouts.push_back(d[L"name"].asString(L""));
        }
        for (auto& out : m_displayOutputs) {
            out.config.bEnabled = false;
            out.config.bOwnProcess = false;
        }
        m_displayOutputs.erase(
            std::remove_if(m_displayOutputs.begin(), m_displayOutputs.end(),
                [&](const DisplayOutput& o) {
                    return o.config.type == DisplayOutputType::Spout &&
                           std::find(namedSpouts.begin(), namedSpouts.end(),
                                     std::wstring(o.config.szName)) == namedSpouts.end();
                }),
            m_displayOutputs.end());
    }

    // Apply per-display settings
    for (size_t i = 0; i < displays.size(); i++) {
        const auto& d = displays.at(i);
        std::wstring type = d[L"type"].asString(L"");

        if (type == L"Monitor") {
            std::wstring devName = d[L"deviceName"].asString(L"");
            namedMonitors.push_back(devName);
            bool attached = false;
            // Match to currently enumerated monitor
            for (auto& out : m_displayOutputs) {
                if (out.config.type != DisplayOutputType::Monitor) continue;
                if (devName != out.config.szDeviceName) continue;
                out.config.bEnabled     = d[L"enabled"].asBool(false);
                out.config.bFullscreen  = d[L"fullscreen"].asBool(true);
                out.config.nOpacity     = d[L"opacity"].asInt(100);
                if (out.config.nOpacity < 1) out.config.nOpacity = 1;
                if (out.config.nOpacity > 100) out.config.nOpacity = 100;
                out.config.bClickThrough = d[L"clickThrough"].asBool(false);
                out.config.bIndependentRender = d[L"independentRender"].asBool(false);
                // Per-display child settings (forgejo#22). Absent in v1, and
                // the defaults below are what v1 always meant.
                //
                // Same degrade as the settings/displays.json loader just below
                // (bOwnProcess load, ~line 571) and for the same reason: a
                // profile is exactly the kind of file that gets loaded on every
                // startup via bLoadDisplayProfileOnStart, so an ownProcess=true
                // saved into one -- deliberately or, as measured, by saving
                // while a test had a real child running -- would bring up a
                // borderless-fullscreen instance every single launch with no
                // one having asked for that today. It degrades to an
                // independent mirror, never silently to nothing.
                out.config.bOwnProcess = d[L"ownProcess"].asBool(false);
                if (out.config.bOwnProcess) {
                    out.config.bOwnProcess = false;
                    out.config.bIndependentRender = true;
                }
                wcsncpy_s(out.config.szPresetDir,
                          d[L"presetDir"].asString(L"").c_str(), _TRUNCATE);
                wcsncpy_s(out.config.szStartupPreset,
                          d[L"startupPreset"].asString(L"").c_str(), _TRUNCATE);
                out.config.fTimeBetweenPresets =
                          d[L"timeBetweenPresets"].asFloat(-1.0f);
                out.config.bSequentialOrder = d[L"sequentialOrder"].asBool(false);
                out.config.nPresetLock = d[L"presetLock"].asInt(-1);
                if (out.config.nPresetLock < -1) out.config.nPresetLock = -1;
                if (out.config.nPresetLock > 1)  out.config.nPresetLock = 1;
                attached = true;
                break;
            }
            // Monitors come and go -- a laptop undocked, a TV switched off. A
            // profile naming one that is not here must not be all-or-nothing:
            // skip that entry, say so, and apply the rest.
            if (!attached)
                DLOG_WARN("display profile: %ls is not attached; skipped",
                          devName.c_str());
        } else if (type == L"Spout") {
            std::wstring name = d[L"name"].asString(L"");
            // Try to match existing Spout output by name
            bool matched = false;
            for (auto& out : m_displayOutputs) {
                if (out.config.type != DisplayOutputType::Spout) continue;
                if (name != out.config.szName) continue;
                out.config.bEnabled   = d[L"enabled"].asBool(false);
                out.config.bFixedSize = d[L"fixedSize"].asBool(false);
                out.config.nWidth     = d[L"width"].asInt(1920);
                out.config.nHeight    = d[L"height"].asInt(1080);
                matched = true;
                break;
            }
            // If no existing Spout output matched, add a new one
            if (!matched && !name.empty()) {
                DisplayOutput newOut;
                newOut.config.type      = DisplayOutputType::Spout;
                newOut.config.bEnabled  = d[L"enabled"].asBool(false);
                newOut.config.bFixedSize = d[L"fixedSize"].asBool(false);
                newOut.config.nWidth    = d[L"width"].asInt(1920);
                newOut.config.nHeight   = d[L"height"].asInt(1080);
                wcsncpy_s(newOut.config.szName, name.c_str(), _TRUNCATE);
                m_displayOutputs.insert(m_displayOutputs.begin(), std::move(newOut));
            }
        }
    }
    lkDisp.unlock();

    // Optional: the primary's own preset settings (v2). Absent from v1 files
    // and from any profile saved without the checkbox.
    if (root.has(L"primary")) {
        const auto& pr = root[L"primary"];

        // The globals underneath, captured before anything is replaced. These
        // settings apply to the RUNNING session only -- Shane's rule is "if
        // the profile doesn't load then the global settings still work", and
        // without this the shutdown save would promote them permanently.

        // Nothing below writes m_primaryProfile.
        //
        // That struct is what the NEXT saved profile should carry -- the pins
        // typed into the "Main render" group -- not a record of the profile
        // last loaded. Populating it here made every later save carry the
        // loaded profile's values instead of the live state, so a snapshot
        // taken after loading a profile and then changing the preset by hand
        // stored the preset that had been replaced.

        // Both fields can hold a MODE rather than a path -- <Random>,
        // <Current>, <Default> -- because the main render's combos offer the
        // same list every other display's do. They are resolved here through
        // the very same functions a child spawn uses, so there is one rule
        // for what a mode means and not a second one that can drift.
        //
        // They used to be written through verbatim. Angle brackets are illegal
        // in a Windows filename, so a profile saved with the combo left on
        // <Current> asked the render thread to load a file called literally
        // "<Current>", which of course failed -- the setting simply never took
        // (reported 2026-09-02: "I have saved profiles and the render preset
        // setting doesn't seem to take ... it's not loading them on profile
        // load").
        DisplayOutputConfig askedFor;   // a carrier for the resolvers, not a display
        wcsncpy_s(askedFor.szPresetDir,     pr[L"presetDir"].asString(L"").c_str(),     _TRUNCATE);
        wcsncpy_s(askedFor.szStartupPreset, pr[L"startupPreset"].asString(L"").c_str(), _TRUNCATE);

        // What is on screen, as a full path, worked out the same way the
        // <Current> resolver does. m_szCurrentPresetFile's comment claims it
        // carries no path and that is not reliably true, so it is tested
        // rather than trusted -- and this must be taken BEFORE m_szPresetDir
        // moves, or a bare name would be joined to the incoming directory
        // instead of the one it actually came from.
        std::wstring currentFull;
        if (m_szCurrentPresetFile[0]) {
            if (wcschr(m_szCurrentPresetFile, L'\\') || wcschr(m_szCurrentPresetFile, L'/')) {
                currentFull = m_szCurrentPresetFile;
            } else {
                currentFull = m_szPresetDir;
                if (!currentFull.empty() && currentFull.back() != L'\\' &&
                    currentFull.back() != L'/')
                    currentFull += L'\\';
                currentFull += m_szCurrentPresetFile;
            }
        }

        // Resolved BEFORE m_szPresetDir moves: <Current> and <Default> are
        // relative to what is playing now, which is the arrangement the
        // profile is replacing.
        const bool wantDir = askedFor.szPresetDir[0] != L'\0';
        const std::wstring dir = wantDir ? ResolveDisplayPresetDir(askedFor)
                                         : std::wstring();
        const bool wantPreset = askedFor.szStartupPreset[0] != L'\0';
        const std::wstring preset =
            wantPreset ? ResolveDisplayStartupPreset(askedFor,
                             wantDir ? dir.c_str() : m_szPresetDir)
                       : std::wstring();

        if (!dir.empty()) {
            StashPrimaryGlobal(kPrimaryOverrideDir);
            wcsncpy_s(m_szPresetDir, dir.c_str(), _TRUNCATE);
        }

        // An empty startupPreset means INHERIT, not "clear the pinned one".
        // It used to be written through unconditionally, so loading any
        // profile that carried no preset wiped szPresetStartup.
        if (!preset.empty()) {
            StashPrimaryGlobal(kPrimaryOverrideStartup);
            // The RESOLVED path is pinned, never the mode. m_szPresetStartup
            // is what the next launch opens, and a sentinel sitting in it
            // would fail there exactly as it failed here.
            wcsncpy_s(m_szPresetStartup, preset.c_str(), _TRUNCATE);
            // And PLAY it, rather than only pinning it for next launch.
            //
            // Every child display already has its preset put back on load;
            // the main window was the one screen that did not, so a profile
            // restored three quarters of a setup and left the biggest surface
            // on whatever happened to be running.
            //
            // Unless it is already on screen. <Current> resolves to the
            // running preset by definition, and re-loading that would restart
            // it -- a visible glitch, and a reset of the animation clock --
            // to arrive back where it started. Comparing paths rather than
            // special-casing the mode also spares the reload when a profile
            // pins the preset that happens to be playing.
            if (currentFull.empty() || _wcsicmp(preset.c_str(), currentFull.c_str()) != 0)
                ApplyPrimaryProfilePreset(preset);
        }

        const float secs = pr[L"timeBetweenPresets"].asFloat(-1.0f);
        if (secs >= 0.0f) {
            StashPrimaryGlobal(kPrimaryOverrideTime);
            m_fTimeBetweenPresets = secs;
        }

        // Read as an int so -1 can mean inherit. A v2 file wrote a JSON bool
        // here; asInt on true/false gives 1/0, which are the two real values,
        // so an old profile keeps meaning what it meant.
        const int order = pr[L"sequentialOrder"].asInt(-1);
        if (order >= 0) {
            StashPrimaryGlobal(kPrimaryOverrideOrder);
            m_bSequentialPresetOrder = (order != 0);
        }

        // "presetLocked" was the v2 name and a bool; "presetLock" is the
        // tri-state. Fall back so an older profile still locks what it locked.
        int lock = pr[L"presetLock"].asInt(-1);
        if (lock < 0 && pr.has(L"presetLocked"))
            lock = pr[L"presetLocked"].asBool(false) ? 1 : 0;
        if (lock >= 0) {
            StashPrimaryGlobal(kPrimaryOverrideLock);
            SetUserPresetLock(lock != 0);
        }

        m_fNextPresetTime = -1.0f;   // recompute; see SET_TIME_BETWEEN_PRESETS
    }

    // Optional: messaging (v2). Applies to the IN-MEMORY set only; nothing here
    // writes messages.ini.
    if (root.has(L"messaging")) {
        const auto& ms = root[L"messaging"];
        m_bMsgAutoplay         = ms[L"autoplay"].asBool(m_bMsgAutoplay);
        m_bMsgSequential       = ms[L"sequential"].asBool(m_bMsgSequential);
        m_fMsgAutoplayInterval = ms[L"interval"].asFloat(m_fMsgAutoplayInterval);
        m_fMsgAutoplayJitter   = ms[L"jitter"].asFloat(m_fMsgAutoplayJitter);
        m_nMsgMaxOnScreen      = ms[L"maxOnScreen"].asInt(m_nMsgMaxOnScreen);
        m_bMsgOverrideRandomFont     = ms[L"overrideRandomFont"].asBool(m_bMsgOverrideRandomFont);
        m_bMsgOverrideRandomColor    = ms[L"overrideRandomColor"].asBool(m_bMsgOverrideRandomColor);
        m_bMsgOverrideRandomSize     = ms[L"overrideRandomSize"].asBool(m_bMsgOverrideRandomSize);
        m_bMsgOverrideRandomEffects  = ms[L"overrideRandomEffects"].asBool(m_bMsgOverrideRandomEffects);
        m_bMsgOverrideRandomPos      = ms[L"overrideRandomPos"].asBool(m_bMsgOverrideRandomPos);
        m_bMsgOverrideRandomGrowth   = ms[L"overrideRandomGrowth"].asBool(m_bMsgOverrideRandomGrowth);
        m_bMsgOverrideSlideIn        = ms[L"overrideSlideIn"].asBool(m_bMsgOverrideSlideIn);
        m_bMsgOverrideRandomDuration = ms[L"overrideRandomDuration"].asBool(m_bMsgOverrideRandomDuration);
        m_bMsgOverrideShadow         = ms[L"overrideShadow"].asBool(m_bMsgOverrideShadow);
        m_bMsgOverrideBox            = ms[L"overrideBox"].asBool(m_bMsgOverrideBox);
        m_bMsgOverrideApplyHueShift  = ms[L"overrideApplyHueShift"].asBool(m_bMsgOverrideApplyHueShift);
        m_bMsgOverrideRandomHue      = ms[L"overrideRandomHue"].asBool(m_bMsgOverrideRandomHue);
        m_bMsgIgnorePerMsgRandom     = ms[L"ignorePerMsgRandom"].asBool(m_bMsgIgnorePerMsgRandom);

        // An empty array is indistinguishable from a truncated file, and a
        // profile must not be able to silently empty the user's message list.
        const auto& msgs = ms[L"messages"];
        if (msgs.size() > 0) {
            for (int i = 0; i < MAX_CUSTOM_MESSAGES; i++)
                m_CustomMessage[i].szText[0] = L'\0';
            for (size_t i = 0; i < msgs.size(); i++) {
                const auto& m = msgs.at(i);
                const int slot = m[L"slot"].asInt(-1);
                if (slot < 0 || slot >= MAX_CUSTOM_MESSAGES) continue;
                td_custom_msg& dst = m_CustomMessage[slot];
                wcsncpy_s(dst.szText, m[L"text"].asString(L"").c_str(), _TRUNCATE);
                dst.nFont    = m[L"font"].asInt(dst.nFont);
                dst.fSize    = m[L"size"].asFloat(dst.fSize);
                dst.x        = m[L"x"].asFloat(dst.x);
                dst.y        = m[L"y"].asFloat(dst.y);
                dst.randx    = m[L"randx"].asFloat(dst.randx);
                dst.randy    = m[L"randy"].asFloat(dst.randy);
                dst.growth   = m[L"growth"].asFloat(dst.growth);
                dst.fTime    = m[L"time"].asFloat(dst.fTime);
                dst.fFade    = m[L"fade"].asFloat(dst.fFade);
                dst.fFadeOut = m[L"fadeOut"].asFloat(dst.fFadeOut);
                dst.nAnimProfile = m[L"animProfile"].asInt(dst.nAnimProfile);
            }
        } else {
            DLOG_WARN("display profile: messaging section has no messages; "
                      "the existing message set was left alone");
        }
    }

    // "Autostart all displays": every display the profile names comes on,
    // not only the ones that were running when it was saved. Applied here,
    // before the pass below reads bEnabled to decide what to spawn and what to
    // kill, so one flag governs both.
    //
    // Monitors only. A Spout sender is not a screen and turning one on because
    // a profile mentioned it would start publishing a texture nobody asked for.
    //
    // Scoped to namedMonitors (forgejo#86): this used to loop over every
    // enumerated monitor, so a monitor the profile never mentioned -- one it
    // deliberately omits, meaning "leave it off" per the reset above -- got
    // swept up and force-enabled anyway the moment the checkbox was on for
    // ANY reason, including a stale true carried into this profile file from
    // an earlier save the user never meant to affect this one. "The profile
    // names it" is the actual contract the field's own comment describes;
    // the loop just wasn't enforcing it.
    m_bProfileAutostartAll = root[L"autostartAllDisplays"].asBool(false);
    if (m_bProfileAutostartAll) {
        int on = 0;
        for (auto& out : m_displayOutputs) {
            if (out.config.type != DisplayOutputType::Monitor) continue;
            if (out.config.bEnabled) continue;
            if (std::find(namedMonitors.begin(), namedMonitors.end(),
                          std::wstring(out.config.szDeviceName)) == namedMonitors.end())
                continue;
            out.config.bEnabled = true;
            on++;
        }
        if (on) DLOG_INFO("display profile: autostart turned on %d display(s)", on);
    }

    // Put the profile's preset onto each own-preset display.
    //
    // Not left to SendToDisplayOutputs, which resolves a display's preset ONCE
    // and then leaves the surface owning the answer -- correct for a display
    // that just started, wrong for a profile load. Loading a profile is an
    // explicit "make it this state", and putting the recorded preset back is
    // what makes a snapshot restore the set of presets it captured.
    //
    // Resolved, not posted raw: the field may hold <Random> or <Current>, and a
    // surface handed one of those would try to open a file called "<Random>".
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor) continue;
        MirrorSurface* ms = MirrorSurfaceFor(out.config.szDeviceName);
        if (!ms) continue;                  // no surface yet; it resolves at birth
        if (!(out.config.bOwnProcess && out.config.bEnabled)) {
            // Back to following the primary. An empty request is how a surface
            // is told that; assigning to the sim would race the worker.
            PostSurfacePreset(*ms, std::wstring());
            ms->nextPresetAt = 0.0;
            ms->seqIndex.store(-1);
            continue;
        }
        const std::wstring d = ResolveDisplayPresetDir(out.config);
        const std::wstring s = ResolveDisplayStartupPreset(out.config, d.c_str());
        if (!s.empty()) {
            PostSurfacePreset(*ms, s);
            const float interval = EffectiveChildInterval(out.config);
            ms->nextPresetAt = interval > 0.0f ? GetTime() + interval : -1.0;
        }
    }

    // Request render-thread mirror style refresh and save to INI
    m_bMirrorStylesDirty.store(true);
    SaveDisplayOutputSettings();
    RefreshDisplaysTab();
    // A loaded profile has to MATCH what the window shows, or the Displays
    // window becomes a liar: RefreshDisplaysTab rebuilds the list rows only,
    // so every control in the "Selected display" group -- Enabled, Render
    // mode, Preset dir, Startup, Cycle, Lock -- went on showing the values
    // from before the load. Re-applying the same selection repopulates them
    // from the config the profile just wrote.
    //
    // Here and not inside RefreshDisplaysTab, which also runs on a 1-second
    // child-status timer: repopulating edit and combo controls once a second
    // would fight the user typing a path into them.
    UpdateDisplaysTabSelection(m_nDisplaysTabSel);
    return true;
}

//======================================================================
// Spout output — sender lifecycle and control
//======================================================================

bool Engine::OpenSender(unsigned int width, unsigned int height) {
  DLOG_INFO("Spout: OpenSender(%d, %d)", width, height);

  // Close existing sender
  SpoutReleaseWraps();
  if (bInitialized) {
    spoutsender.Close();
    bInitialized = false;
  }

  if (!m_lpDX || !m_lpDX->m_device || !m_lpDX->m_commandQueue) {
    DebugLogA("Spout: OpenSender failed - no DX12 device/queue", LOG_ERROR);
    return false;
  }

  // Give the sender a name
  spoutsender.SetName(WinampSenderName);

  // Initialize SpoutDX12 with our DX12 device + command queue
  if (!spoutsender.Open(m_lpDX->m_device.Get(),
                        m_lpDX->m_commandQueue.Get())) {
    DebugLogA("Spout: Open failed", LOG_ERROR);
    return false;
  }

  // Wrap each swap chain backbuffer for DX11 access
  for (int n = 0; n < DXC_FRAME_COUNT; n++) {
    if (!spoutsender.WrapResource(
            m_lpDX->m_renderTargets[n].Get(),
            &m_pWrappedBackBuffers[n],
            D3D12_RESOURCE_STATE_RENDER_TARGET)) {
      DebugLogA("Spout: WrapResource failed for backbuffer", LOG_ERROR);
      SpoutReleaseWraps();
      spoutsender.Close();
      return false;
    }
  }

  g_Width = width;
  g_Height = height;
  bSpoutOut = true;
  bInitialized = true;
  m_bSpoutDX12Ready = true;

  DebugLogA("Spout: DX12 sender initialized successfully");

  return true;

} // end OpenSender

// Release wrapped DX12 backbuffers
void Engine::SpoutReleaseWraps() {
  for (auto& w : m_pWrappedBackBuffers) {
    if (w) { w->Release(); w = nullptr; }
  }
  m_bSpoutDX12Ready = false;
}

int Engine::ToggleSpout() {
  bSpoutChanged = true; // write config on exit
  bSpoutOut = !bSpoutOut;
  if (bSpoutOut) {
    AddNotification(L"Spout output enabled");
  }
  else {
    AddNotification(L"Spout output disabled");
  }

  // Sync first Spout output in m_displayOutputs
  for (auto& o : m_displayOutputs) {
    if (o.config.type == DisplayOutputType::Spout) {
      o.config.bEnabled = bSpoutOut;
      if (!bSpoutOut && o.spoutState) {
        DestroyDisplayOutput(o);
      }
      break;
    }
  }

  SetSpoutFixedSize(false, false);

  if (bInitialized || m_bSpoutDX12Ready) {
    SpoutReleaseWraps();
    spoutsender.Close();
    bInitialized = false;
  }

  ResetBufferAndFonts();
  SendSettingsInfoToMDropDX12Remote();
  return 0;
}

int Engine::SetSpoutFixedSize(bool toggleSwitch, bool showNotifications) {
  bSpoutChanged = true; // write config on exit
  if (toggleSwitch) {
    bSpoutFixedSize = !bSpoutFixedSize;
  }
  // Sync first Spout output in m_displayOutputs
  for (auto& o : m_displayOutputs) {
    if (o.config.type == DisplayOutputType::Spout) {
      o.config.bFixedSize = bSpoutFixedSize;
      o.config.nWidth = nSpoutFixedWidth;
      o.config.nHeight = nSpoutFixedHeight;
      break;
    }
  }
  if (IsSpoutActiveAndFixed()) {
    if (toggleSwitch && showNotifications) {
      std::wstring msg = L"Fixed Spout output size enabled ("
        + std::to_wstring(nSpoutFixedWidth) + L"x"
        + std::to_wstring(nSpoutFixedHeight) + L")";
      AddNotification(msg.data());
    }
    else if (showNotifications) {
      std::wstring msg = L"Spout output size set to "
        + std::to_wstring(nSpoutFixedWidth) + L"x"
        + std::to_wstring(nSpoutFixedHeight);
      AddNotification(msg.data());
    }
    // DX12 TODO: Fixed-size Spout requires a separate render target + copy/scale.
    // For now, Spout sends at window resolution regardless of fixed-size setting.
    ResetBufferAndFonts();
  }
  else {
    // bSpoutFixedSize OR bSpoutOut is false
    if (toggleSwitch && showNotifications && bSpoutOut) {
      AddNotification(L"Fixed Spout output size disabled");
    }
    ResetBufferAndFonts();
  }
  SendSettingsInfoToMDropDX12Remote();
  return 0;
}

// ─── The display-profile folder ────────────────────────────────────────────────

std::wstring Engine::DisplayProfileDir() const {
    return profiles::DirFor(m_szBaseDir, profiles::Kind::Display);
}

// resources/displayprofiles/ -> resources/profiles/display/, once.
//
// Four kinds of profile stored themselves four different ways; the layout is
// stated once in profile_paths.h now, and this is display's move onto it. The
// files were already one per profile with timestamped names, so this is a
// change of address and nothing else: no format conversion, no rewrite.
//
// MOVED, and only where the move is confirmed. MoveFileExW answers true or the
// original stays exactly where it was; anything that will not move is left
// behind and logged rather than quietly lost, and the old directory is removed
// only when it is EMPTY -- so whatever did not move keeps the folder alive to
// be noticed instead of vanishing with it.
void Engine::MigrateDisplayProfiles() {
    std::wstring oldDir = m_szBaseDir;
    if (!oldDir.empty() && oldDir.back() != L'\\' && oldDir.back() != L'/')
        oldDir += L'\\';
    oldDir += L"resources\\displayprofiles\\";

    const DWORD attr = GetFileAttributesW(oldDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return;                   // already moved, or there never was one

    const std::wstring newDir = DisplayProfileDir();
    if (!profiles::EnsureDirPath(newDir)) {
        DLOG_ERROR("display profiles: could not create %ls", newDir.c_str());
        return;
    }

    int moved = 0, kept = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((oldDir + L"*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring from = oldDir + fd.cFileName;
            const std::wstring to = newDir + fd.cFileName;
            // Never overwrite: a name already there was not put there by this
            // migration, and it wins.
            if (GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES) {
                DLOG_WARN("display profiles: %ls exists; leaving the old copy",
                          to.c_str());
                kept++;
                continue;
            }
            if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED)) {
                moved++;
            } else {
                kept++;
                DLOG_ERROR("display profiles: could not move %ls", from.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    if (kept == 0 && RemoveDirectoryW(oldDir.c_str()))
        DLOG_INFO("display profiles: moved %d into %ls, old folder removed",
                  moved, newDir.c_str());
    else if (moved || kept)
        DLOG_INFO("display profiles: moved %d, left %d in %ls",
                  moved, kept, oldDir.c_str());

    // A startup profile stored as an absolute path into the old folder would
    // now point at nothing. Re-point it at the same leaf in the new one.
    if (m_szStartupDisplayProfile[0] &&
        wcsstr(m_szStartupDisplayProfile, L"displayprofiles")) {
        const wchar_t* leaf = wcsrchr(m_szStartupDisplayProfile, L'\\');
        leaf = leaf ? leaf + 1 : m_szStartupDisplayProfile;
        const std::wstring repointed = newDir + leaf;
        wcsncpy_s(m_szStartupDisplayProfile, repointed.c_str(), _TRUNCATE);
        DLOG_INFO("display profiles: startup profile re-pointed to %ls",
                  m_szStartupDisplayProfile);
    }
}

std::wstring Engine::ResolveDisplayProfilePath(const wchar_t* nameOrPath) const {
    if (!nameOrPath || !*nameOrPath) return std::wstring();
    std::wstring s = nameOrPath;
    // Anything that looks like a path is one. Profiles saved before the folder
    // existed are absolute, and they must keep loading.
    if (s.find(L'\\') != std::wstring::npos || s.find(L'/') != std::wstring::npos ||
        (s.size() > 1 && s[1] == L':'))
        return s;
    if (s.size() < 5 || _wcsicmp(s.c_str() + s.size() - 5, L".json") != 0)
        s += L".json";
    return DisplayProfileDir() + s;
}

std::vector<std::wstring> Engine::ListDisplayProfiles() const {
    std::vector<std::wstring> out;
    const std::wstring dir = DisplayProfileDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    // By name, which is by age: SaveDisplayProfileSnapshot timestamps them
    // yyyy-MM-dd_HH-mm-ss precisely so the two orders are the same one.
    std::sort(out.begin(), out.end(),
              [](const std::wstring& a, const std::wstring& b) {
                  return _wcsicmp(a.c_str(), b.c_str()) < 0;
              });
    return out;
}

std::wstring Engine::SuggestedDisplayProfileName() const {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    FormatTo(name, L"%04d-%02d-%02d_%02d-%02d-%02d.json",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return name;
}

std::wstring Engine::SaveDisplayProfileSnapshot() {
    const std::wstring dir = DisplayProfileDir();
    // One level at a time: resources/ may exist while displayprofiles/ does not.
    {
        std::wstring res = m_szBaseDir;
        if (!res.empty() && res.back() != L'\\' && res.back() != L'/') res += L'\\';
        res += L"resources";
        CreateDirectoryW(res.c_str(), NULL);
        CreateDirectoryW(dir.c_str(), NULL);
    }

    const std::wstring path = dir + SuggestedDisplayProfileName();
    // primary and messaging both included: the point of a snapshot is to put
    // the whole thing back, and leaving the main window out of "all my screens"
    // would be a strange place to draw the line.
    if (!SaveDisplayProfile(path.c_str(), true, true, true))
        return std::wstring();
    wcsncpy_s(m_szStartupDisplayProfile, path.c_str(), _TRUNCATE);
    DLOG_INFO("display profile snapshot saved: %ls", path.c_str());
    return path;
}

bool Engine::LoadDefaultDisplayProfile() {
    if (!m_szStartupDisplayProfile[0]) {
        AddNotification((wchar_t*)L"No default display profile set");
        return false;
    }
    const std::wstring path = ResolveDisplayProfilePath(m_szStartupDisplayProfile);
    // Through the render command queue, for the same reason the IPC verb is:
    // loading mutates m_displayOutputs and spawns and kills children, and the
    // render thread walks that vector every frame.
    RenderCommand rc;
    rc.cmd = RenderCmd::LoadDisplayProfile;
    rc.sParam = path;
    EnqueueRenderCmd(rc);
    return true;
}

bool Engine::StepDisplayProfile(int delta) {
    std::vector<std::wstring> all = ListDisplayProfiles();
    if (all.empty()) {
        AddNotification((wchar_t*)L"No display profiles saved");
        return false;
    }

    // Where we are now, by file name. Unknown -- a profile loaded from outside
    // the folder, or none yet -- starts the walk at the first entry, so the
    // first press always lands somewhere rather than doing nothing.
    int idx = -1;
    if (m_szStartupDisplayProfile[0]) {
        const wchar_t* leaf = wcsrchr(m_szStartupDisplayProfile, L'\\');
        leaf = leaf ? leaf + 1 : m_szStartupDisplayProfile;
        for (size_t i = 0; i < all.size(); i++)
            if (_wcsicmp(all[i].c_str(), leaf) == 0) { idx = (int)i; break; }
    }
    const int n = (int)all.size();
    idx = (idx < 0) ? 0 : ((idx + delta) % n + n) % n;

    const std::wstring path = DisplayProfileDir() + all[idx];
    wcsncpy_s(m_szStartupDisplayProfile, path.c_str(), _TRUNCATE);
    RenderCommand rc;
    rc.cmd = RenderCmd::LoadDisplayProfile;
    rc.sParam = path;
    EnqueueRenderCmd(rc);
    AddNotification((wchar_t*)all[idx].c_str());
    return true;
}

bool Engine::DeleteDisplayProfile(const std::wstring& nameOrPath) {
    const std::wstring path = ResolveDisplayProfilePath(nameOrPath.c_str());
    if (path.empty()) return false;
    return DeleteFileW(path.c_str()) != FALSE;
}

// ── Named display-profile IPC, addressed like MIXER_PROFILE_*/VFX_PROFILE_* ──
//
// forgejo#95. The older SAVE_DISPLAY_PROFILE=/LOAD_DISPLAY_PROFILE= take a
// full path, which the harness and scripts already know how to build. A
// remote does not: it has no way to learn the profiles folder, so it cannot
// construct one, and nothing tells it what already exists.
bool Engine::HandleDisplayProfileIPC(const wchar_t* sMessage) {
    extern PipeServer g_pipeServer;

    if (MSG_IS(sMessage, L"DISPLAY_PROFILE_LIST")) {
        // resources/displayprofiles/ (DisplayProfileDir()) only -- the same
        // folder ListDisplayProfiles() already scans, nowhere else.
        std::wstring reply;
        for (const std::wstring& leaf : ListDisplayProfiles()) {
            if (!reply.empty()) reply += L"\n";
            // Display profiles carry no separate stored "name" the way mixer
            // profiles do (MixerProfileName reads a "name" field); the leaf
            // IS the name, timestamped, so "name" here is just the leaf with
            // its extension trimmed for display.
            std::wstring name = leaf;
            if (name.size() > 5 && _wcsicmp(name.c_str() + name.size() - 5, L".json") == 0)
                name.resize(name.size() - 5);
            wchar_t line[MAX_PATH * 2];
            FormatTo(line, L"DISPLAY_PROFILE|file=%s|name=%s", leaf.c_str(), name.c_str());
            reply += line;
        }
        if (reply.empty()) reply = L"DISPLAY_PROFILE_NONE";
        g_pipeServer.Send(reply.c_str());
        return true;
    }

    if (MSG_IS(sMessage, L"DISPLAY_PROFILE_SAVE=")) {
        // DISPLAY_PROFILE_SAVE=<name>[|primary=1][|messaging=1]
        // An empty name banks a timestamp, matching MIXER_PROFILE_SAVE=.
        //
        // Deliberately does NOT touch the startup profile. The older
        // SAVE_DISPLAY_PROFILE= still does -- see the fix just below it in
        // HandleDisplayModeIPC -- and doing the same here would repeat
        // exactly the surprise this issue asked to have fixed: a save must
        // not silently decide what loads at boot.
        std::wstring arg(sMessage + 21);
        const bool primary   = arg.find(L"|primary=1") != std::wstring::npos;
        const bool messaging = arg.find(L"|messaging=1") != std::wstring::npos;
        const size_t bar = arg.find(L'|');
        if (bar != std::wstring::npos) arg = arg.substr(0, bar);
        const std::wstring name = arg.empty() ? SuggestedDisplayProfileName() : arg;

        profiles::EnsureDirPath(DisplayProfileDir());
        const std::wstring path = ResolveDisplayProfilePath(name.c_str());
        if (path.empty() || !SaveDisplayProfile(path.c_str(), primary, messaging, false)) {
            g_pipeServer.Send(L"DISPLAY_PROFILE_ERR|reason=save_failed");
            return true;
        }
        g_pipeServer.Send((std::wstring(L"DISPLAY_PROFILE_SAVED|file=") +
                           FileNameOnly(path.c_str())).c_str());
        return true;
    }

    if (MSG_IS(sMessage, L"DISPLAY_PROFILE_LOAD=")) {
        const std::wstring name(sMessage + 21);
        if (name.empty()) {
            g_pipeServer.Send(L"DISPLAY_PROFILE_ERR|reason=malformed");
            return true;
        }
        const std::wstring path = ResolveDisplayProfilePath(name.c_str());
        if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            g_pipeServer.Send(L"DISPLAY_PROFILE_ERR|reason=unknown_profile");
            return true;
        }
        // Through the render command queue, same as LOAD_DISPLAY_PROFILE= and
        // for the same reason: loading mutates m_displayOutputs and spawns
        // and kills children, which must not happen off the render thread.
        // The eventual PROFILE_LOADED=<path> / PROFILE_LOADED=ERROR reply
        // comes from that same shared handler (engine_input.cpp), including
        // its existing "loading also becomes the new startup default"
        // behavior -- unchanged here. Only DISPLAY_PROFILE_SAVE='s side
        // effect was the one this issue asked to remove; decoupling LOAD
        // from the default too would change StepDisplayProfile's and
        // LoadDefaultDisplayProfile's shared plumbing well beyond what was
        // asked. DISPLAY_PROFILE_STARTUP= below is the deliberate way to
        // change the default without loading anything.
        RenderCommand rc;
        rc.cmd = RenderCmd::LoadDisplayProfile;
        rc.sParam = path;
        EnqueueRenderCmd(rc);
        return true;
    }

    if (MSG_IS(sMessage, L"DISPLAY_PROFILE_DELETE=")) {
        const std::wstring name(sMessage + 23);
        if (name.empty()) {
            g_pipeServer.Send(L"DISPLAY_PROFILE_ERR|reason=malformed");
            return true;
        }
        if (!DeleteDisplayProfile(name)) {
            g_pipeServer.Send(L"DISPLAY_PROFILE_ERR|reason=unknown_profile");
            return true;
        }
        g_pipeServer.Send((std::wstring(L"DISPLAY_PROFILE_DELETED|file=") + name).c_str());
        return true;
    }

    // DISPLAY_PROFILE_STARTUP=<name>  sets both the name and enables it.
    // DISPLAY_PROFILE_STARTUP=        (empty) disables it; the name is kept,
    //                                 so re-enabling later needs no name again.
    // DISPLAY_PROFILE_STARTUP         (no '=') queries both without changing
    //                                 either -- checked FIRST would be wrong,
    //                                 since MSG_IS is a prefix match and the
    //                                 bare form is a prefix of the '=' form.
    if (MSG_IS(sMessage, L"DISPLAY_PROFILE_STARTUP=")) {
        // One verb for both pieces of state (Shane's comment on forgejo#95):
        // WHICH profile and WHETHER one loads at all are only meaningful
        // together, and "set this to load at start" is one intention.
        // Neither loads nor saves anything -- a remote can point the default
        // at an existing profile without disturbing what is on screen now.
        const std::wstring name(sMessage + 24);
        if (name.empty()) {
            m_bLoadDisplayProfileOnStart = false;
        } else {
            const std::wstring path = ResolveDisplayProfilePath(name.c_str());
            if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                g_pipeServer.Send(L"DISPLAY_PROFILE_ERR|reason=unknown_profile");
                return true;
            }
            wcsncpy_s(m_szStartupDisplayProfile, path.c_str(), _TRUNCATE);
            m_bLoadDisplayProfileOnStart = true;
        }
        // Same section/keys and the same SetInt (not SetBool) the checkbox
        // itself uses (engine_displays_ui.cpp), so either surface reads back
        // what the other just wrote.
        Config().SetString(L"Settings", L"szStartupDisplayProfile", m_szStartupDisplayProfile);
        Config().SetInt(L"Settings", L"bLoadDisplayProfileOnStart",
                        m_bLoadDisplayProfileOnStart ? 1 : 0);
        wchar_t buf[MAX_PATH + 32];
        FormatTo(buf, L"DISPLAY_PROFILE_STARTUP|file=%s|enabled=%d",
                 FileNameOnly(m_szStartupDisplayProfile), m_bLoadDisplayProfileOnStart ? 1 : 0);
        g_pipeServer.Send(buf);
        return true;
    }

    if (MSG_IS(sMessage, L"DISPLAY_PROFILE_STARTUP")) {
        wchar_t buf[MAX_PATH + 32];
        FormatTo(buf, L"DISPLAY_PROFILE_STARTUP|file=%s|enabled=%d",
                 FileNameOnly(m_szStartupDisplayProfile), m_bLoadDisplayProfileOnStart ? 1 : 0);
        g_pipeServer.Send(buf);
        return true;
    }

    return false;
}

} // namespace mdrop

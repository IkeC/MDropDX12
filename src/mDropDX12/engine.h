/*
  LICENSE
  -------
Copyright 2005-2013 Nullsoft, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

  * Neither the name of Nullsoft nor the names of its contributors may be used to
    endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef MDROP_ENGINE_H
#define MDROP_ENGINE_H 1

// =========================================================
// SPOUT & DISPLAY OUTPUTS
#include "spout_bridge.h" // our own Spout interface; see forgejo#4
#include "display_output.h"
#include <queue>
#include "hotkeys.h"
#include <io.h> // for file existence check
// =========================================================

#include "engineshell.h"
#include "engine_helpers.h"  // SETTINGS_NUM_PAGES, control IDs
#include "md_defines.h"
#include "menu.h"
#include "support.h"
#include <mutex>
#include "guarded.h"  // Guarded<T> -- #11
#include "gpu_usage.h"  // per-process GPU utilisation
#include "texmgr.h"
#include "state.h"
#include "dx12helpers.h"  // DX12Texture
#include "canvas_metric.h"  // CanvasMetric, CanvasSample
#include "video_capture.h" // VideoCaptureSource (needed for unique_ptr complete type)
#include "midi_input.h"   // MidiInput, MidiRow (needed for MIDI members)
#include "tool_window.h"  // DisplaysWindow (needed for unique_ptr complete type)
#include "video_effect_params.h"  // VideoEffectParams, AudioLink
#include "vfx_profile_store.h"    // VFXProfileStore, MAX_VFX_PROFILE_NAME
#include <vector>
#include <array>
#include <map>
#include <string>
#include <unordered_map>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <set>
#include "audio_profile_store.h"
#include <string>
#include <regex>
#include "../ns-eel2/ns-eel.h"
#include "mdropdx12.h"

//#include <core/sdk/IPlaybackService.h>

extern "C" int (*warand)(void);

namespace mdrop {

struct ScriptState {
  std::vector<std::wstring> lines;  // parsed non-comment lines
  int currentLine = -1;             // -1 = not playing
  bool playing = false;
  bool loop = false;
  double bpm = 120.0;
  int beats = 4;                    // beats before next line
  double lastLineTime = 0.0;       // GetTime() when last line executed
  // Default message style
  std::wstring defaultFont = L"Arial";
  int defaultSize = 20;
  int defaultR = 255, defaultG = 255, defaultB = 255;
  std::wstring filePath;            // current script file path
};

struct WindowTitleProfile {
    wchar_t szName[64] = {};           // Profile name (e.g., "Spotify")
    wchar_t szWindowRegex[256] = {};   // Regex to match window title
    wchar_t szParseRegex[512] = {};    // Regex with named groups: (?<artist>...) (?<title>...) (?<album>...)
    int nPollIntervalSec = 2;          // Poll interval in seconds (1-10)
};

typedef enum { TEX_DISK, TEX_VS, TEX_FEEDBACK, TEX_IMAGE_FEEDBACK, TEX_AUDIO, TEX_BUFFER_B, TEX_BUFFER_C, TEX_BUFFER_D, TEX_BLUR0, TEX_BLUR1, TEX_BLUR2, TEX_BLUR3, TEX_BLUR4, TEX_BLUR5, TEX_BLUR6, TEX_BLUR_LAST } tex_code;
typedef enum { UI_REGULAR, UI_MENU, UI_LOAD, UI_LOAD_DEL, UI_LOAD_RENAME, UI_SAVEAS, UI_SAVE_OVERWRITE, UI_EDIT_MENU_STRING, UI_CHANGEDIR, UI_IMPORT_WAVE, UI_EXPORT_WAVE, UI_IMPORT_SHAPE, UI_EXPORT_SHAPE, UI_UPGRADE_PIXEL_SHADER, UI_MASHUP, UI_SETTINGS } ui_mode;
typedef struct { float rad; float ang; float a; float c; } td_vertinfo; // blending: mix = max(0,min(1,a*t + c));
typedef char* CHARPTR;
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

#define MY_FFT_SAMPLES 512     // for old [pre-vms] milkdrop sound analysis
struct td_mysounddata {
  float   imm[3];			// bass, mids, treble (absolute)
  float	  imm_rel[3];		// bass, mids, treble (relative to song; 1=avg, 0.9~below, 1.1~above)
  float	  avg[3];			// bass, mids, treble (absolute)
  float	  avg_rel[3];		// bass, mids, treble (relative to song; 1=avg, 0.9~below, 1.1~above)
  float	  long_avg[3];	// bass, mids, treble (absolute)
  float   fWave[2][576];
  float   fSpecLeft[MY_FFT_SAMPLES];
  float   fSpecRight[MY_FFT_SAMPLES];
  float   fShaderSpecLeft[MY_FFT_SAMPLES];   // clean FFT for shader texture (no equalization)
  float   fShaderSpecRight[MY_FFT_SAMPLES];
  static const int RECENT_BUF_MAX = 4096;
  float   recent_buf[3][RECENT_BUF_MAX];
  int     recent_pos[3];
  int     recent_len[3];
  float	  smooth[3];
  float	  smooth_rel[3];
};

// Per-simulation state for independent mirrors (internal fragment — needs
// td_mysounddata/td_vertinfo/MYVERTEX above; stays inside namespace mdrop).

typedef struct {
  int 	bActive;
  int 	bFilterBadChars;	// if true, it will filter out any characters that don't belong in a filename, plus the & symbol (because it doesn't display properly with DrawText)
  int 	bDisplayAsCode;		// if true, semicolons will be followed by a newline, for display
  int		nMaxLen;			// can't be more than 511
  int		nCursorPos;
  int		nSelAnchorPos;		// -1 if no selection made
  int 	bOvertypeMode;
  wchar_t	szText[48000];      // wide string editing (filenames, user text)
  char	szCode[96000];      // narrow code editing (shader/equation ASCII code)
  wchar_t	szPrompt[512];
  wchar_t	szToolTip[512];
  char	szClipboard[48000];
  wchar_t	szClipboardW[48000];
} td_waitstr;

typedef struct {
  int 	bBold;
  int 	bItal;
  wchar_t	szFace[128];
  int		nColorR;    // 0..255
  int		nColorG;    // 0..255
  int		nColorB;    // 0..255
}
td_custom_msg_font;

enum {
  MD2_PS_NONE = 0,
  MD2_PS_2_0 = 2,
  MD2_PS_2_X = 3,
  MD2_PS_3_0 = 4,
  MD2_PS_4_0 = 5, // not supported by milkdrop
  MD2_PS_5_0 = 6, // SM5.0 for Shadertoy (.milk3) presets
};

typedef struct {
  int		nFont;
  float	fSize;	// 0..100
  float	x;
  float	y;
  float	randx;
  float randy;
  float	growth;
  float	fTime;	// total time to display the message, in seconds
  float	fFade;	// % (0..1) of the time that is spent fading in
  float	fFadeOut;
  float	fBurnTime;

  // overrides
  int     bOverrideBold;
  int     bOverrideItal;
  int     bOverrideFace;
  int     bOverrideColorR;
  int     bOverrideColorG;
  int     bOverrideColorB;
  int	    nColorR;    // 0..255
  int	    nColorG;    // 0..255
  int	    nColorB;    // 0..255
  int     nRandR;
  int     nRandG;
  int  	  nRandB;
  int     bBold;
  int     bItal;
  wchar_t szFace[128];

  wchar_t	szText[256];

  // Per-message randomization flags (0=off, 1=on)
  int bRandPos;
  int bRandSize;
  int bRandFont;
  int bRandColor;
  int bRandEffects;
  int bRandGrowth;
  int bRandDuration;

  int nAnimProfile;  // -1 = use message's own settings, -2 = random profile, 0+ = named profile index
}
td_custom_msg;

#define MAX_ANIM_PROFILES 32

struct td_anim_profile {
  wchar_t szName[64] = {};        // profile name for UI (e.g. "Slide from Left")
  bool    bEnabled = true;        // included in randomization pool

  // Position
  float   fX = 0.5f, fY = 0.5f;  // target position (0..1)
  float   fRandX = 0.0f, fRandY = 0.0f;  // random offset ranges

  // Entry animation
  float   fStartX = -100.0f;     // start X (-100 = no slide)
  float   fStartY = -100.0f;     // start Y (-100 = no slide)
  float   fMoveTime = 0.0f;      // slide-in duration (seconds)
  int     nEaseMode = 2;         // 0=linear, 1=ease-in, 2=ease-out
  float   fEaseFactor = 2.0f;    // easing intensity

  // Appearance
  wchar_t szFontFace[128] = {};  // empty = use default
  float   fFontSize = 50.0f;     // 0..100
  int     bBold = 0, bItal = 0;
  int     nColorR = 255, nColorG = 255, nColorB = 255;
  int     nRandR = 0, nRandG = 0, nRandB = 0;

  // Timing
  float   fDuration = 5.0f;      // total display time
  float   fFadeIn = 0.2f;        // fade-in fraction (0..1)
  float   fFadeOut = 0.5f;       // fade-out time (seconds)
  float   fBurnTime = 0.0f;      // burn/flare effect

  // Effects
  float   fGrowth = 1.0f;        // text scale-over-time
  float   fShadowOffset = 0.0f;  // shadow distance
  float   fBoxAlpha = 0.0f;      // background box opacity (0=none)
  int     nBoxColR = 0, nBoxColG = 0, nBoxColB = 0;

  // Per-trigger randomization flags
  int     bRandPos = 0, bRandSize = 0, bRandColor = 0;
  int     bRandGrowth = 0, bRandDuration = 0;
};

typedef struct td_supertext {
  float	fStartTime = -1.0f; // off state
  int 	bRedrawSuperText;	// true if it needs redraw
  int 	bIsSongTitle;		// false for custom message, true for song title
  //char	szText[256];
  wchar_t	szTextW[512];
  wchar_t	nFontFace[128];
  int 	bBold;
  int 	bItal;
  float fMoveTime = -1;
  float	fStartX = - 100;
  float fStartY = - 100;
  float	fX;
  float fY;
  float	fFontSize;			// [0..100] for custom messages, [0..4] for song titles
  bool	bExplicitSize = false;	// true if size was explicitly set (skip autosize)
  float fGrowth;			// applies to custom messages only
  int		nFontSizeUsed;		// height IN PIXELS
  int		nTextWidthUsed = 0;	// width IN PIXELS of the rendered text in the title texture
  float	fDuration;
  float	fFadeInTime; // applies to custom messages only; song title fade times are handled specially
  float	fFadeOutTime; // applies to custom messages only; song title fade times are handled specially
  int  	nColorR;
  int   nColorG;
  int  	nColorB;
  int   nEaseMode = 2;	// 0 = linear, 1 = ease-in, 2 = ease-out (default)
  float fEaseFactor = 2.0f; // 1.0f = linear, 2.0f = ease-in/out, 3.0f = more pronounced ease-in/out
  float fShadowOffset = 2.0f;
  float fBurnTime; // seconds
  float fBoxAlpha = 0.0f; // 0 = transparent, 255 = opaque
  int fBoxColR = 0;
  int fBoxColG = 0;
  int fBoxColB = 0;
  float fBoxLeft = 1.0f;
  float fBoxRight = 1.0f;
  float fBoxTop = 1.0f;
  float fBoxBottom = 1.0f;
}
td_supertext;

typedef struct {
  wchar_t        texname[256];   // ~filename, but without path or extension!
  LPDIRECT3DBASETEXTURE9 texptr;
  int                w, h, d;
  //D3DXHANDLE         texsize_param;
  bool               bEvictable;
  int                 nAge;   // only valid if bEvictable is true
  int                 nSizeInBytes;    // only valid if bEvictable is true
  DX12Texture        dx12Tex;         // DX12 GPU resource + SRV
} TexInfo;

typedef struct {
  std::wstring    texname;  // just for ref
  D3DXHANDLE texsize_param;
  int        w, h;
} TexSizeParamInfo;

typedef struct SamplerInfo {
  LPDIRECT3DBASETEXTURE9 texptr;
  bool               bBilinear;
  bool               bWrap;
  UINT               dx12SrvIndex = UINT_MAX; // DX12 SRV heap index (UINT_MAX = none)
} SamplerInfo;

typedef struct {
  std::wstring   msg;
  bool      bBold;  // true == red bkg; false == black bkg
  float     birthTime;
  float     expireTime;
  int       category;
  bool      bSentToRemote;
  DWORD     color;  // 0 = use default font color
  uint64_t  id;     // stable across the render thread's snapshot; see forgejo#9
} ErrorMsg;

typedef std::vector<ErrorMsg> ErrorMsgList;

class CShaderParams {
public:
  // float4 handles:
  D3DXHANDLE rand_frame;
  // Scalars, from the active audio profile. See embedded_shaders.h: these
  // are uniforms rather than macros so a profile switch does not stale the
  // shader cache.
  D3DXHANDLE fft_params = NULL;
  D3DXHANDLE rand_preset;
  D3DXHANDLE const_handles[24];
  D3DXHANDLE q_const_handles[(NUM_Q_VAR + 3) / 4];
  D3DXHANDLE rot_mat[24];

  typedef std::vector<TexSizeParamInfo> TexSizeParamInfoList;
  TexSizeParamInfoList texsize_params;

  // sampler stages for various PS texture bindings:
  //int texbind_vs;
  //int texbind_disk[32];
  //int texbind_voronoi;
  //...
  SamplerInfo   m_texture_bindings[32];  // an entry for each texture slot (t-register).  These are ALIASES - DO NOT DELETE.
  tex_code      m_texcode[32];  // if ==TEX_VS, forget the pointer - texture bound @ that stage is the double-buffered VS.

  void Clear();
  void CacheParams(LPD3DXCONSTANTTABLE pCT, bool bHardErrors);
  CShaderParams();
  ~CShaderParams();

  // Copy stays available and is a DEEP copy: texsize_params is a std::vector
  // and copies itself properly, the D3DXHANDLEs are non-owning aliases into a
  // constant table, m_texture_bindings is documented "ALIASES - DO NOT DELETE",
  // and m_texcode is a plain enum array. Nothing here is owned twice by a copy.
  // It was never copying that was wrong -- it was `memcpy` over a type with a
  // vector in it (#5), which the vector's own copy constructor is precisely
  // what fixes.
  //
  // Move is declared because the user-declared destructor above suppresses the
  // implicit move operations, so without these a "move" of a CShaderParams
  // silently deep-copied the vector instead.
  CShaderParams(const CShaderParams&) = default;
  CShaderParams& operator=(const CShaderParams&) = default;
  CShaderParams(CShaderParams&&) noexcept = default;
  CShaderParams& operator=(CShaderParams&&) noexcept = default;
};


// VShaderInfo and PShaderInfo own COM references. Copying one raw duplicates
// the ownership without an AddRef, so both objects' destructors Release the
// same references and the second Release is on a dead object -- the defect
// class #5 exists to make unrepresentable.
//
// So implicit copy is DELETED and both types move instead. The two operations
// that actually happen in this codebase are named:
//
//   move       transfer ownership; the source is left holding nothing, so its
//              destructor has nothing to release. RotatePShaderSet is exactly
//              this and used to hand-write it as copy-then-null at each site.
//   CloneFrom  a genuine second owner, with the AddRefs done here rather than
//              remembered by the caller. Only the fallback shaders need it:
//              they are substituted into a live set while remaining valid for
//              the next preset that fails to compile.
//
// Deleting copy does not stop anyone writing memcpy over these types -- nothing
// can. What it stops is the quiet, plausible-looking `a = b` that compiles,
// runs, and double-releases later. The five sites that did that now say
// CloneFrom and cannot forget an AddRef.
class VShaderInfo {
public:
  IDirect3DVertexShader9* ptr;
  LPD3DXCONSTANTTABLE     CT;
  CShaderParams           params;
  VShaderInfo() { ptr = NULL; CT = NULL; params.Clear(); }
  ~VShaderInfo() { Clear(); }
  void Clear();

  VShaderInfo(const VShaderInfo&) = delete;
  VShaderInfo& operator=(const VShaderInfo&) = delete;

  VShaderInfo(VShaderInfo&& o) noexcept
      : ptr(o.ptr), CT(o.CT), params(std::move(o.params)) {
    o.ptr = NULL;
    o.CT = NULL;
    o.params.Clear();
  }
  VShaderInfo& operator=(VShaderInfo&& o) noexcept {
    if (this != &o) {
      Clear();
      ptr = o.ptr;
      CT = o.CT;
      params = std::move(o.params);
      o.ptr = NULL;
      o.CT = NULL;
      o.params.Clear();
    }
    return *this;
  }

  // Become a second, independently-owning reference to the same shader.
  void CloneFrom(const VShaderInfo& src) {
    if (this == &src) return;
    if (src.ptr) src.ptr->AddRef();
    if (src.CT) src.CT->AddRef();
    Clear();
    ptr = src.ptr;
    CT = src.CT;
    params = src.params;
  }
};

class PShaderInfo {
public:
  IDirect3DPixelShader9* ptr;
  LPD3DXCONSTANTTABLE     CT;
  CShaderParams           params;
  LPD3DXBUFFER            bytecodeBlob;  // DX12: compiled SM5.0 bytecode for PSO creation
  PShaderInfo() { ptr = NULL; CT = NULL; bytecodeBlob = NULL; params.Clear(); }
  ~PShaderInfo() { Clear(); }
  void Clear();

  PShaderInfo(const PShaderInfo&) = delete;
  PShaderInfo& operator=(const PShaderInfo&) = delete;

  PShaderInfo(PShaderInfo&& o) noexcept
      : ptr(o.ptr), CT(o.CT), params(std::move(o.params)),
        bytecodeBlob(o.bytecodeBlob) {
    o.ptr = NULL;
    o.CT = NULL;
    o.bytecodeBlob = NULL;
    o.params.Clear();
  }
  PShaderInfo& operator=(PShaderInfo&& o) noexcept {
    if (this != &o) {
      Clear();
      ptr = o.ptr;
      CT = o.CT;
      bytecodeBlob = o.bytecodeBlob;
      params = std::move(o.params);
      o.ptr = NULL;
      o.CT = NULL;
      o.bytecodeBlob = NULL;
      o.params.Clear();
    }
    return *this;
  }

  // Become a second, independently-owning reference to the same shader. All
  // three AddRefs live here, so a call site cannot cover two of them and miss
  // the third -- which is the shape the five fallback sites were written in.
  void CloneFrom(const PShaderInfo& src) {
    if (this == &src) return;
    if (src.ptr) src.ptr->AddRef();
    if (src.CT) src.CT->AddRef();
    if (src.bytecodeBlob) src.bytecodeBlob->AddRef();
    Clear();
    ptr = src.ptr;
    CT = src.CT;
    bytecodeBlob = src.bytecodeBlob;
    params = src.params;
  }
};

typedef struct {
  VShaderInfo vs;
  PShaderInfo ps;
} ShaderPairInfo;

typedef struct {
  PShaderInfo warp;
  PShaderInfo comp;
  PShaderInfo bufferA;  // Shadertoy Buffer A (pre-comp pass)
  PShaderInfo bufferB;  // Shadertoy Buffer B (second compute buffer)
  PShaderInfo bufferC;  // Shadertoy Buffer C (third compute buffer)
  PShaderInfo bufferD;  // Shadertoy Buffer D (fourth compute buffer)
} PShaderSet;

// Included HERE and not with the other headers at the top: RenderContext
// carries PShaderSet* and the preset pipeline objects (#184), so the
// fragment has to be able to see PShaderSet, which is defined just above.
// Nothing between the old include site and this point referred to
// RenderContext or MirrorSimContext, so moving it costs nothing.
#include "render_context.h"

// Transfer every member of `src` into `dst`: release whatever `dst` currently
// holds, take src's COM pointers and shader params, then null src out so its
// destructor has nothing left to release (forgejo#5). One place, so a
// seventh member cannot be added to PShaderSet without this seeing it --
// unlike the hand-written warp/comp-only version of this dance that used to
// live at each call site and silently stopped covering bufferA-D the moment
// Shadertoy added them: reachable live, by loading any Shadertoy preset and
// then either CLEARPRESET or a Preset Editor Apply, both a double-release.
// Safe to chain (RotatePShaderSet(a, b); RotatePShaderSet(b, c);) -- after
// the first call `b` holds nothing, so the second call's release of `b` is a
// no-op, not a second release of what `a` now owns.
void RotatePShaderSet(PShaderSet& dst, PShaderSet& src);

typedef struct {
  VShaderInfo warp;
  VShaderInfo comp;
} VShaderSet;

typedef struct {
  std::wstring  szFilename;    // without path
  float    fRatingThis;
  float    fRatingCum;
} PresetInfo;
typedef std::vector<PresetInfo> PresetList;

// Preset annotation flags (bitmask)
#define PFLAG_FAVORITE  0x01
#define PFLAG_ERROR     0x02
#define PFLAG_SKIP      0x04
#define PFLAG_BROKEN    0x08
// The preset misbehaves as the render canvas grows -- its own feedback maths
// destabilise above the size it was authored for. Recorded as a flag rather
// than left to a HUD notification, because a notification is transient and
// this is a property of the preset worth seeing in a list.
#define PFLAG_CANVAS    0x10

// What produced a stored errorText.  The distinction exists because the two
// kinds age differently: a Shader error is retracted the moment the preset
// compiles clean again, while a Runtime error records a frame that crashed
// while drawing, which a clean compile does not disprove.
enum class PresetErrorKind { Shader, Runtime };

// One rating, tied to the content version it was given against.
//
// Ratings are a list rather than a number so that editing a preset does not
// silently replace the opinion earned by the version before it: a preset rated
// 5, then changed, then rated 2 keeps both, with dates, and reports the average.
struct RatingObservation {
    std::wstring hash;      // content hash this rating was given against
    int          value = 0; // 0-5
    std::wstring when;      // ISO 8601 local time, may be empty on migrated data
};

// Aspect-preserving long-edge cap, 16-aligned. Defined in engine_displays.cpp;
// shared by the mirror swap-chain cap and the feedback-canvas limit.
void CapDimToLongEdge(int& w, int& h, int maxDim);

struct PresetAnnotation {
    std::wstring filename;      // fallback key — filename without path
    std::wstring hash;          // primary key — content identity (preset_hash.h)
    std::vector<std::wstring> paths;  // every location this preset was seen at
    std::vector<RatingObservation> ratings;  // one per content version
    std::wstring lastUsed;      // ISO 8601 local time of the last counted play
    int          useCount = 0;
    int          secondsShown = 0;
    int          rating = 0;    // 0-5, 0 = unrated
    uint32_t     flags = 0;     // PFLAG_ bitmask
    std::wstring notes;
    std::wstring errorText;     // auto-captured from shader compile
    std::wstring errorTime;     // ISO 8601 local time errorText was captured
    PresetErrorKind errorKind = PresetErrorKind::Shader;  // who wrote errorText
    std::vector<std::wstring> tags;  // user-defined tags (e.g., "ambient", "dark")

    // Per-preset override of whatever this preset's tags would have selected,
    // so a special case does not need a tag invented for it.  Resolved
    // independently for each slot: a preset can take its shader from a generic
    // tag rule while naming its own VFX profile.
    //
    // ABSENT AND EMPTY ARE DIFFERENT STATES.  has* false means "inherit from
    // the tags"; has* true with an empty string means "explicitly none", which
    // is how a preset carrying a tag opts out of that tag's rule.  The writer
    // must emit a member only when its flag is set, or every entry in the file
    // silently becomes "none".
    std::wstring shaderOverride;
    std::wstring vfxProfile;

    // Which AudioProfile feeds this preset. Same three states as the two
    // above: absent means inherit from the tags, present-and-empty means
    // explicitly the default profile, which suppresses whatever a tag rule
    // would have selected.
    std::wstring audioProfile;

    // Run THIS file instead of the preset itself. The per-preset half of the
    // MD31/MD32 replacement mechanism (preset_replacements.h): a full path,
    // designated from the Annotations window, and the highest-priority source
    // of a replacement. Absent means "no designation", which falls through to
    // the store's key map and then its search paths.
    std::wstring replacementPreset;

    // Long-edge cap for the feedback canvas on this preset, in px. Same
    // tri-state as the slots above: absent means inherit (no per-preset
    // limit). It can only REDUCE below the global ceiling -- never raise.
    int  canvasMax = 0;

    // Strength of the feedback damp mitigation, 0..1, 0 = off.
    //
    // The OTHER answer to a preset that destabilises as the canvas grows, and
    // the one to reach for first: rather than shrinking the canvas and losing
    // sharpness, bleed a little energy out of the feedback loop each frame so
    // the preset's own accumulator cannot run away. It is the absolute decay
    // that Flexi's author gave ret.y (`- 0.008`) and did not give ret.z.
    //
    // The applied multiplier is DERIVED FROM THE CANVAS, never fitted: it is
    // exactly 1.0 at the size the preset was authored for and only bites as the
    // canvas grows past it -- see EffectiveFeedbackDamp. A fixed constant would
    // corrupt a render that is already correct at 1080p.
    float feedbackDamp = 0.0f;

    bool hasShaderOverride = false;
    bool hasVfxProfile = false;
    bool hasAudioProfile = false;
    bool hasCanvasMax = false;
    bool hasFeedbackDamp = false;
    bool hasReplacementPreset = false;
};

class Engine : public EngineShell {
public:
  MDropDX12* mdropdx12;

  // Messages/Sprites mode helpers
  bool MessagesEnabled() const { return (m_nSpriteMessagesMode & 1) != 0; }
  bool SpritesEnabled()  const { return (m_nSpriteMessagesMode & 2) != 0; }
  // The only writer of m_nSpriteMessagesMode: persists immediately and tells
  // the open tool windows to resync. Toggling messages off from a hotkey used
  // to leave "Show Messages" ticked and the Settings combo stale, so the app
  // looked like it had simply stopped drawing messages.
  void SetSpriteMessagesMode(int mode);

  //====[ 1. members added to create this specific example plugin: ]================================================

// =========================================================
// SPOUT variables
  mdrop::SpoutSender spoutsender;  // Spout DX12 sender (D3D11On12 interop)

  char WinampSenderName[256]; // The sender name
  bool bInitialized; // did it work ?

  // Wrapped DX12 backbuffers for Spout DX11 send
  ID3D11Resource* m_pWrappedBackBuffers[DXC_FRAME_COUNT] = {};
  bool m_bSpoutDX12Ready = false; // SpoutDX12 initialized and wraps valid

  bool OpenSender(unsigned int width, unsigned int height);
  void SpoutReleaseWraps(); // Release wrapped backbuffers and mark not ready
  void OpenMDropDX12Remote();
  void SetAudioDeviceDisplayName(const wchar_t* displayName, bool isRenderDevice);
  void ExecuteRenderCommand(const RenderCommand& cmd) override;
  int  GetPresetCount() override;
  int  GetCurrentPresetIndex() override;
  const wchar_t* GetPresetName(int idx) override;

  void SaveShaderBytecodeToFile(ID3DXBuffer* pShaderByteCode, uint32_t checksum, char* prefix);
  ID3DXBuffer* LoadShaderBytecodeFromFile(uint32_t checksum, char* prefix);

  uint32_t crc32(const char* data, size_t length);

  bool CheckDX9DLL();
  bool CheckForDirectX9c();
  void ShowDirectXMissingMessage();

  bool bSpoutChanged; // set to write config on exit

  bool bEnablePresetStartup;
  bool bAutoLockPresetWhenNoMusic;
  //bool StartupPresetLoaded = false;
  unsigned int g_Width;
  unsigned int g_Height;
  HWND g_hwnd;
  HDC g_hdc;
  wchar_t	m_szSavedSongTitle[512]; // for saving song tile with Spout on or off
  // =========================================================

  // =========================================================
  // DISPLAY OUTPUTS (monitor mirrors + Spout senders)
  std::vector<DisplayOutput> m_displayOutputs;
  // Guards every touch of m_displayOutputs from a thread other than the
  // render thread: the Displays ToolWindow thread (which reads/writes a
  // selected entry's fields for its controls, and reads the whole vector to
  // rebuild the list) and the child-worker thread (which reads a display's
  // config to configure/place/poll a child). The render thread is the only
  // structural mutator (insert/erase/push_back -- the operations that can
  // reallocate the buffer or move elements), all of it now funneled through
  // RenderCmd handlers or functions the render thread alone calls
  // (#27), so this lock is never held by more than one of a small,
  // fixed set of functions rather than needing one at each of the ~115
  // individual field-level touch sites.
  //
  // Recursive, not std::mutex: RefreshDisplaysTab() and
  // UpdateDisplaysTabSelection() read m_displayOutputs and are called both
  // directly from a ToolWindow handler that has already taken this lock for
  // its own read/write, and from the render thread's RenderCmd handlers,
  // which have not. A plain mutex would deadlock the first case; retaking it
  // from the same thread is a no-op with recursive_mutex.
  //
  // Every lock scope here is narrow -- just the statements that touch
  // m_displayOutputs -- and never wraps a MessageBoxW/GetOpenFileNameW or
  // anything else that can block for a human-scale amount of time. Holding
  // this lock across one of those would stall the render thread's next
  // structural mutation for as long as the dialog stays open.
  //
  // mutable: TestChildRect is const (it only reads), but still needs to lock
  // out a concurrent render-thread reallocation while it does.
  mutable std::recursive_mutex m_displayOutputsMutex;

  // \\.\DISPLAY3 -> 3, and 0 for anything that is not a DISPLAYn device.
  static int DisplayNumberFromDevice(const wchar_t* deviceName);

  // szPresetDir / szStartupPreset each hold a literal path or a kDispMode*
  // sentinel. These turn one into the other; see child_instances.cpp. Call the
  // dir one FIRST -- the preset modes pick from the directory it returns.
  // Does this display hold a preset of its own? (#186 phase 4)
  //
  // bOwnProcess keeps its now-inaccurate INI name deliberately -- renaming the
  // key would silently un-configure every display that has it set, and the
  // cost of a misleading key is this comment while the cost of a rename is the
  // user's wall coming up wrong. The Displays window shows the honest name.
  bool DisplayHoldsOwnPreset(const DisplayOutputConfig& cfg) const {
    return cfg.type == DisplayOutputType::Monitor && cfg.bEnabled &&
           cfg.bOwnProcess;
  }

  std::wstring ResolveDisplayPresetDir(const DisplayOutputConfig& cfg) const;
  std::wstring ResolveDisplayStartupPreset(const DisplayOutputConfig& cfg,
                                           const wchar_t* dir) const;
  // fTimeBetweenPresets < 0 means "inherit the parent's". Resolving that was
  // written out in three places, and GET_CHILDREN printed the raw sentinel.
  float EffectiveChildInterval(const DisplayOutputConfig& cfg) const {
    return cfg.fTimeBetweenPresets < 0.0f ? m_fTimeBetweenPresets
                                          : cfg.fTimeBetweenPresets;
  }
  // nPresetLock < 0 means "inherit the main window's", exactly as above.
  bool EffectiveChildPresetLock(const DisplayOutputConfig& cfg) const {
    return cfg.nPresetLock < 0 ? m_bPresetLockedByUser : (cfg.nPresetLock != 0);
  }
  // THE way to change the user preset lock. Six unrelated things toggle it --
  // a hotkey, the scroll-lock key, a game controller, a script, the IPC verb
  // and a display profile -- and it is global, so every one of them has to
  // reach every display. That used to mean a pipe message to each child; a
  // display holds its preset in process now and simply reads the flag.
  //
  // Assign through here, never to m_bPresetLockedByUser directly.
  void SetUserPresetLock(bool bLocked);
  ComPtr<ID3D12CommandAllocator>     m_mirrorCmdAllocators[DXC_FRAME_COUNT];
  ComPtr<ID3D12GraphicsCommandList>  m_mirrorCmdList;
  // Opposite-orient milk2/classic: skippable lagged pass. Own list + fence.
  ComPtr<ID3D12Fence>                m_lagIndepFence;
  UINT64                             m_lagIndepSubmitted = 0;
  UINT64                             m_lagIndepSignal = 0;
  UINT                               m_lagIndepAuxFrame = UINT_MAX;
  ComPtr<ID3D12CommandAllocator>     m_lagIndepAlloc;
  ComPtr<ID3D12GraphicsCommandList>  m_lagIndepList;
  bool LagIndepFenceIdle() const;
  bool EnsureLagIndepObjects();
  void ReleaseLagIndepObjects();

  // Dedicated mirror thread + queue (same idea as a second process: own present
  // loop, shared audio/snapshot, not frame-locked to the primary).
  //
  // The QUEUE is shared and stays shared: ID3D12CommandQueue is free-threaded,
  // so N workers may ExecuteCommandLists and Signal on it concurrently. Every
  // other object that was here has moved onto MirrorSurface -- allocator, list
  // and fence in phase 5b, the wake event and the thread itself in 5c.
  ComPtr<ID3D12CommandQueue>         m_mirrorQueue;
  std::atomic<bool>                  m_bMirrorThreadQuit{false};
  // TIMED, and that is the whole point.
  //
  // A worker cannot block on this indefinitely: StopSurfaceWorker runs on the
  // render thread WHILE it holds this mutex, and its join times out and frees
  // the worker's objects underneath it (0xC0000409 on mirror disable,
  // 2026-08-21). So the worker used try_lock in a Sleep loop -- which never
  // ENQUEUES on the mutex, so the OS could not hand it over, and the render
  // thread re-acquiring after every frame starved it completely: one orient
  // frame per ~40 s, mirrors visibly frozen.
  //
  // The remedy at the time was a flag the worker raised and a Sleep(1) yield
  // on the render thread. That worked, but it is a 1-15 ms wait paid against a
  // 0.2 ms record, and #186 phase 5c made the cost visible: with a worker per
  // surface raising the flag twice as often, the uncapped primary fell from
  // 769 fps to 264 while total CPU DROPPED from 2.55 cores to 2.15 -- a thread
  // sleeping, not working. Shortening the yield to SwitchToThread swung it
  // straight back the other way: primary 757, mirrors 12.5.
  //
  // try_lock_for is what both sides actually wanted. The worker BLOCKS, so it
  // takes its place in the wait list and the OS hands the mutex over; the
  // timeout is only the quit check, not a pacing device. The render thread
  // then needs no yield at all, and there is no flag to keep in step with N
  // workers either.
  std::timed_mutex                   m_mirrorEngineMutex;
  // Independent-mirror worker rate cap: 0 = parity (free-run), else max fps.
  // Set from the Displays dropdown / SET_MIRROR_MAXFPS; read by the worker.
  std::atomic<int>                   m_nMirrorMaxFps{0};
  // Does any mirror target still want frames? Published once per frame by
  // SendToDisplayOutputs (render thread), read by the orient worker.
  //
  // An atomic rather than the worker walking m_displayOutputs itself: that
  // container has a locking contract, and the worker must never nest a second
  // lock against m_mirrorEngineMutex. The render thread has already computed
  // this value for its own use, so publishing it costs nothing.
  std::atomic<bool>                  m_bAnyMirrorTargetLive{false};
  // Audio analysis snapshot for mirror sims: published by the render thread
  // once per frame (after analysis), copied by sim threads under the mutex.
  AudioSnapshot                      m_audioSnap;
  std::mutex                         m_audioSnapMutex;
  void PublishAudioSnapshot();
  void CopyAudioSnapshot(AudioSnapshot& dst);

  // ── Independent mirror simulation (spec: 2026-08-21-independent-mirror-sims) ──
  // Preset bundle: published on the render thread at LoadPresetTick after a
  // successful classic (.milk/.milk2) apply. Mirror sim threads compare
  // versions at frame start and re-import into their own CStates. For .milk2
  // the SPLIT PRESET BODIES travel in memory (Shane, 2026-08-21) — adoption
  // writes them to context-private temp files only for the duration of
  // Import() (its GetFast parser reads a FILE*), then deletes them.
  struct MirrorPresetBundle {
    std::wstring path;          // original preset file (identity/logging)
    bool isMilk2 = false;
    std::string milk2Body1;     // split preset 1 (blend-from), empty if !isMilk2
    std::string milk2Body2;     // split preset 2 (blend-to)
    float milk2Progress = 0.5f; // frozen blend progress
    bool  milk2HasRandoms = false;
    float milk2Random[5] = {};  // MD3 locks shader rand_preset to these
    float blendTime = 0.f;      // primary's transition duration (transient blends)
    uint32_t version = 0;
  };
  MirrorPresetBundle                 m_presetBundle;
  std::mutex                         m_presetBundleMutex;
  std::atomic<uint32_t>              m_presetBundleVersion{0};
  // Serializes every CState::Import across threads: the GetFast line cache
  // keys on consecutively-allocated FILE*s (see engine_presets.cpp:2267), so
  // two concurrent Imports would poison each other. Held by the async preset
  // loaders and by mirror adoption. NEVER acquire m_mirrorEngineMutex while
  // holding this (LoadPresetTick joins a loader while holding the engine
  // mutex — nesting the other way would deadlock).
  std::mutex                         m_presetLoadMutex;
  std::string                        m_pendingMilk2Body1, m_pendingMilk2Body2;
  void PublishPresetBundle(bool isMilk2, float blendTime);
  // Adoption phase 1 (no engine mutex): import bundle into ctx states.
  // Returns true if a new preset was adopted (caller must regenerate the
  // ctx blend mesh under the engine mutex — see m_bMirrorSimPatternDirty).
  // #186 phase 1: there is deliberately NO resolver here any more. A mirror
  // follows the primary's published bundle, which is what makes it a mirror.
  // MirrorSimContext::ownPresetPath survives and stays empty; phase 4 sets it
  // from the display that owns the context, on the own-preset path.
  bool MirrorSimAdoptPreset(MirrorSimContext& c);
  void MirrorSimStepFrame(MirrorSimContext& c);       // per-frame EEL + mesh, ctx-pure
  void MirrorSimEnsureGrid(MirrorSimContext& c, int w, int h,
                           int aspectW = 0, int aspectH = 0);
  void MirrorSimApplyBlendPattern(MirrorSimContext& c); // engine mutex held (record path)
  void MirrorSimFree(MirrorSimContext& c);
  void ComputeGridAlphaValuesCtx(MirrorSimContext& c);
  void LoadPerFrameEvallibVarsCtx(MirrorSimContext& c, CState* pState);
  // Mirror throughput (observability: DIAG_MIRRORS + one log line per second).
  // Opposite-orient runs on its own thread and is deliberately not frame-locked,
  // so its rate is expected to differ from the primary and from each panel.
  std::atomic<unsigned>              m_nOrientFrameAccum{0};
  std::atomic<unsigned>              m_nMirrorPresentAccum{0};
  float                              m_fOrientFps = 0.0f;
  float                              m_fMirrorPresentFps = 0.0f;
  DWORD                              m_dwMirrorFpsTick = 0;
  // Jerkiness hunt (2026-08-23): rate alone says nothing about smoothness —
  // a steady 22 fps looks fine, 22 fps with a 60 ms outlier does not. Every
  // value is microseconds over the last 1 s window, published by the worker
  // (dt/lock/rec/sim) and by the render thread (primary frame interval).
  std::atomic<int> m_diagOrientDtMinUs{0};   // worker iteration interval
  std::atomic<int> m_diagOrientDtMaxUs{0};
  std::atomic<int> m_diagOrientDtAvgUs{0};
  std::atomic<int> m_diagOrientLockMaxUs{0}; // spin-wait for m_mirrorEngineMutex
  std::atomic<int> m_diagOrientLockAvgUs{0};
  std::atomic<int> m_diagOrientRecMaxUs{0};  // record under the engine lock
  std::atomic<int> m_diagOrientRecAvgUs{0};
  std::atomic<int> m_diagOrientSimMaxUs{0};  // MirrorSimStepFrame (lock-free)
  std::atomic<int> m_diagOrientSimAvgUs{0};
  std::atomic<int> m_diagOrientFenceWaits{0};// fence-gate misses (wasted wakes)
  std::atomic<int> m_diagPrimDtMinUs{0};     // primary frame interval
  std::atomic<int> m_diagPrimDtMaxUs{0};
  std::atomic<int> m_diagPrimDtAvgUs{0};
  void LockMirrorEngine() override;
  void UnlockMirrorEngine() override;
  // Start/stop the whole pool: the shared queue plus one worker per surface.
  // The names are unchanged because their call sites are "mirrors turned on"
  // and "mirrors turned off", which is still exactly what they mean.
  void StartMirrorThread();
  void StopMirrorThread();
  // StartSurfaceWorker / StopSurfaceWorker / MirrorThreadMain /
  // MirrorThreadDrawAndPresent are declared with MirrorSurface, below.
  //
  // Is the calling thread one of the mirror workers? A worker claims an upload
  // ring for its exclusive use at start and does not run without one, so the
  // ring registry IS the worker registry -- there is no second list to drift.
  //
  // Used where a code path is legal on the render thread and not on a worker:
  // EnsureOrientPipeline must never rebuild a pipe from inside a record.
  bool IsMirrorWorkerThread() const {
    return m_lpDX && m_lpDX->IsMirrorUploadThread();
  }
  // Panel output stays entirely on the RENDER thread (worker-side blits and
  // presents were tried 2026-08-21 and produced a driver TDR on the mirror
  // queue, then a D3D12Core queue-mutex deadlock on the main queue — never
  // submit or Present from the worker on the main queue). Instead the worker
  // double-buffers its display target and PUBLISHES the completed index once
  // its fence proves the frame done; the render thread always blits the
  // published image — the stretched-primary fallback (the two-image flicker)
  // is gone. -1 = nothing published yet (warmup: stretch fallback allowed).
  // Freshness token for the render thread's "did the worker publish a new
  // frame?" test. It MUST NOT be the disp[] slot: that index rotates mod 3, so
  // whenever the worker publishes at ~3x (or 6x) the render thread's sampling
  // rate, every sample lands on the same slot, the draw is skipped, and the
  // panels hold one image for a second at a time — the jerky mirrors of
  // 2026-08-23 (sim 240 fps, panels 0-2 fps). This counter only ever grows, so
  // equality means "nothing new" and nothing else. Never reset it: after an
  // orient-pipe recreate a stale panel value merely forces a redraw.
  // Primary frame counter, bumped once per render-thread frame. In parity mode
  // (m_nMirrorMaxFps == 0) the worker steps its sim exactly once per advance of
  // this, so the mirrors integrate their feedback at the SAME rate as the
  // window they mirror. Free-running the worker made them run visibly faster
  // (2026-08-23: 160 fps worker against a 40 fps capped primary).
  std::atomic<unsigned>              m_nPrimaryFrameSeq{0};
  UINT m_diagOrientAspectBad = 0;   // flicker hunt: ctx shape draws w/ wrong aspect
  UINT m_diagOrientAspectGood = 0;
  // Leader panel's RAW dims (pre 1920-cap, pre 16-align). The sim grid takes
  // its aspect from these: the rounded pipe (1920x1088 = 1.7647) vs the panel
  // (2560x1440 = 1.7778) reads as slightly-flat circles otherwise.
  bool m_bMirrorClassRegistered = false;
  bool m_bMirrorsActive = false;       // Displays tab button; always starts off
  // Render-thread countdown after EnsureFullScreen. Create swap chains only
  // once the primary SC is stable — same-turn Init+ResizeSwapChain TDRs.
  std::atomic<int> m_nDeferMirrorActivate{0};
  // One-shot: after mirror on/off, bring primary + mirrors to front (respects AOT for sticky topmost).
  std::atomic<bool> m_bRaiseMirrorsNextFrame{false};
  void RaiseMirrorSurfaces(HWND hPrimary);
  // The restore point for SET_ALL_FULLSCREEN=0 (forgejo#96): what each
  // display actually was, at the moment SET_ALL_FULLSCREEN=1 ran, so "put
  // them back" means back to what the user had -- not to some assumed
  // default. Deliberately does NOT touch bEnabled or m_bMirrorsActive: those
  // decide which displays are IN SCOPE (mirror activation has its own
  // deferred-swap-chain sequencing and is not this verb's to trigger), this
  // only remembers window state (minimized/fullscreen) for what was already
  // showing. bActive says a snapshot exists to restore; a second
  // SET_ALL_FULLSCREEN=1 while one is already active re-applies without
  // overwriting it, so nested calls cannot lose the original restore point.
  // SET_ALL_FULLSCREEN=2 (forgejo#98) bypasses this entirely -- it leaves
  // fullscreen unconditionally rather than restoring TO whatever this
  // remembers, and clears bActive afterward so a later =1 starts fresh.
  struct AllFullscreenSnapshot {
    bool bActive = false;
    bool primaryWasMinimized = false;
    bool primaryWasFullscreen = false;
  };
  AllFullscreenSnapshot m_allFullscreenSnapshot;
  bool m_bMirrorWatermarkActive = false; // True while in mirror watermark mode (App.cpp manages)
  wchar_t m_szWatermarkRenderDevice[32] = {}; // Device name of the display render moved to (for deterministic skip)
  bool m_bWatermarkActive = false;       // True while in single-window watermark mode (App.cpp manages)
  // What the ALT-S hotkey does. Was a bool "use mirrors instead of stretch";
  // a third answer -- load the default display profile -- does not fit a
  // checkbox, and the choice is now explicit rather than inferred.
  enum { ALTS_STRETCH = 0, ALTS_MIRROR = 1, ALTS_PROFILE = 2 };
  int  m_nAltSMode = ALTS_STRETCH;
  bool m_bMirrorPromptDisabled = false; // Skip "no mirrors enabled" prompt; auto-enable all
  bool m_bMirrorPromptActive = false;   // Guard: prompt already showing
  // Global default for new/all monitors: independent re-render (correct aspect).
  // Per-output config.bIndependentRender is the effective flag after load.
  bool m_bMirrorIndependentDefault = false;
  std::atomic<bool> m_bMirrorStylesDirty{false}; // UI thread sets; render thread applies
  // UI/hotkey request: tear down & recreate monitor mirrors on render thread only
  // (never WaitForGpu / DestroyDisplayOutput from the UI thread — that freezes the PC)
  std::atomic<bool> m_bMirrorForceReinit{false};
  // Soft-reset orient frame counters on next render pass (never free RTs on toggle).
  std::atomic<bool> m_bMirrorResetOrientNextFrame{false};
  std::atomic<bool> m_bMirrorIndepSizeDirty{false}; // resize SCs after independent toggle
  std::atomic<int> m_nDeferMirrorResize{0}; // skip ResizeBuffers after primary FS/idle
  // Reused 32-slot SRV block for classic letterbox (avoids heap growth / freeze)
  UINT m_mirrorLetterboxSrvBase = UINT_MAX;
  UINT m_mirrorLetterboxSrvEpoch = 0; // invalidate letterbox SRV after descriptor rewind

  // Last SendToDisplayOutputs frame summary (DIAG_MIRRORS only)
  int  m_mirrorDiagMainW = 0, m_mirrorDiagMainH = 0;
  int  m_mirrorDiagMainPortrait = 0;
  int  m_mirrorDiagNeedMainSrv = 0;
  int  m_mirrorDiagCanSampleMain = 0;
  int  m_mirrorDiagAnyOpposite = 0;
  int  m_mirrorDiagAnyIndepMilk3 = 0;
  int  m_mirrorDiagShadertoy = 0;
  int  m_mirrorDiagCompPso = 0;
  unsigned m_mirrorDiagAllocHr = 0;
  unsigned m_mirrorDiagListHr = 0;
  unsigned m_mirrorDiagSkipFrames = 0;
  unsigned m_mirrorDiagFrameCounter = 0;
  unsigned m_mirrorDiagAuxUsed = 0;
  int  m_mirrorDiagSlotCount = 0;

  enum MirrorActivateResult { MirrorActivated, MirrorFullscreenOnly, MirrorCancelled };
  MirrorActivateResult TryActivateMirrors(HWND hRenderWnd);

  void EnumerateDisplayOutputs();
  // Put every display back to what a first run would have produced: all
  // monitors off and inheriting, one disabled Spout sender, and the global
  // mirror flags cleared. Stale DisplayOutput_N sections are removed too.
  void ResetDisplayOutputsToDefaults();
  // Device name of the monitor hosting the render window, e.g. \.\DISPLAY2.
  // Watermark mode wins, because there the render window has been moved
  // deliberately and m_szWatermarkRenderDevice is the record of where to.
  // Compared by NAME, not HMONITOR: the child registry, m_displayOutputs and
  // MOVE_TO_DISPLAY are all keyed by device name, and a name survives a
  // display rearrange where an HMONITOR does not.
  bool GetRenderMonitorDevice(wchar_t (&dev)[32]) const;
  void LoadDisplayOutputSettings();
  void SaveDisplayOutputSettings();
  void InitDisplayOutput(DisplayOutput& out);
  void DestroyDisplayOutput(DisplayOutput& out);
  // Full GPU/window teardown of one mirror (frees its reserved RTV block).
  // Every path that drops a MonitorMirrorState must go through this — a plain
  // unique_ptr reset leaks the RTV reserve and the borderless popup HWND.
  void DestroyMonitorMirror(MonitorMirrorState& ms);
  // Mirrors detached by EnumerateDisplayOutputs (UI thread) wait here until the
  // render thread can destroy them (WaitForGpu/DestroyWindow are render-owned).
  std::mutex m_orphanMirrorMutex;
  std::vector<std::unique_ptr<MonitorMirrorState>> m_orphanMirrors;
  void DrainOrphanedMirrors();
  void DestroyAllDisplayOutputs();
  void ReleaseDisplayOutputWraps();
  void ResizeMirrorSwapChain(MonitorMirrorState& ms, int newW, int newH);
  // Draw + Present mirrors. Same-orient / copy-mode: cheap blit or CopyResource.
  // Opposite-orient independent: lagged fenced warp+comp (skip if still in flight).
  void SendToDisplayOutputs() override;
  // Milk3 image pass to mirror RT (native size). Shares same audio uniforms.
  // Fallback when orient pipeline is unavailable; uses primary feedback (shared history).
  void RenderMilk3ImageToMirror(ID3D12GraphicsCommandList* cmdList,
                                D3D12_CPU_DESCRIPTOR_HANDLE rtv, int monW, int monH);
  // Second orientation milk3 pipeline (portrait-sized feedback when primary is landscape).
  // Owns Buffer A–D + Image feedback at native opposite-orient size; full A→…→Image each frame.
  struct Milk3OrientPipeline {
    DX12Texture fbA[2], fbB[2], fbC[2], fbD[2], imgFb[2];
    // Dedicated display rotation, both modes: classic comp renders straight
    // into disp[dispWrite]; milk3 copies its final Image into it. THREE faces
    // so the published one is never rewritten while a late-recorded render-
    // thread blit may still sample it (two faces raced: write N returns at
    // N+2 while blits of the N-publish could still be in flight — the
    // "brief flashes" of 2026-08-21).
    DX12Texture disp[3];
    int  dispWrite = 0;
    // Classic independent: blur pyramid at orient size (GetBlur1/2/3). Without this,
    // presets like blue haze sample primary landscape blur → soft/wrong haze.
    DX12Texture blur[6];
    int  blurW[6] = {};
    int  blurH[6] = {};
    int  w = 0, h = 0;
    int  fbIdx = 0;
    int  frames = 0;
    UINT bindBase = UINT_MAX; // 5×32 SRV slots (A,B,C,D,comp) + 1 blit block
    UINT bindEpoch = 0;
    bool ready = false;
  };
  // ── One display's mirror surface (#186 phase 3) ─────────────────────
  //
  // Everything here used to be a single engine member, which is what made a
  // mirror a process-wide thing rather than a per-display one: one context,
  // one pipe, one published index. A second display of a DIFFERENT SHAPE then
  // forced EnsureOrientPipeline to tear the single pipe down and rebuild it at
  // the other size -- every frame, alternating -- because the pipe is keyed on
  // w/h and there was only ever one of it.
  //
  // Phase 3 makes it a collection. Phase 4 gives each entry a preset of its
  // own; until then every surface still follows the primary, so N surfaces
  // render the same preset and the picture is unchanged.
  struct MirrorSurface {
    std::wstring device;              // the display this serves; the key

    MirrorSimContext    sim;
    Milk3OrientPipeline pipe;

    // Published by the worker, read by the render thread. See the note on the
    // old m_orientPublishedIdx: the sequence MUST NOT be the disp[] slot,
    // which rotates mod 3 and aliases at publish rates near a multiple of the
    // sampling rate. This counter only grows; never reset it.
    std::atomic<int>      publishedIdx{-1};
    std::atomic<unsigned> publishedSeq{0};
    std::atomic<bool>     imageReady{false};

    std::atomic<int> needW{0};
    std::atomic<int> needH{0};
    // Panel's RAW dims (pre 1920-cap, pre 16-align). The sim grid takes its
    // aspect from these: the rounded pipe (1920x1088 = 1.7647) against the
    // panel (2560x1440 = 1.7778) reads as slightly-flat circles otherwise.
    std::atomic<int> panelW{0};
    std::atomic<int> panelH{0};

    // ── The display's own preset, requested here, adopted by the worker ──
    //
    // sim.ownPresetPath belongs to the WORKER: MirrorSimAdoptPreset reads it
    // every frame. The render thread (and the IPC thread, via
    // SET_DISPLAY_PRESET) must not assign to it -- concurrent read and write
    // of a std::wstring is exactly the kind of race that shows up as a
    // corrupted path months later. So a request is POSTED here and the worker
    // drains it at the top of its adopt.
    //
    // This replaces bd6bf4d7's process-wide m_mirrorRequestedPreset, which
    // existed only because one context served every panel and a per-display
    // request had nowhere per-display to live.
    std::mutex   requestMutex;
    std::wstring requestedPreset;
    bool         requestPending = false;
    // What this display is SHOWING, published by the worker after a successful
    // adopt and read by everything that reports on displays -- the Displays
    // panel, the list, GET_CHILDREN (#186 phase 6).
    //
    // A copy under requestMutex rather than a read of sim.loadedOwnPath, which
    // is the worker's own and is assigned while the UI and the IPC threads are
    // free to be looking at it. Publishing costs one string copy per preset
    // change; reading the live one costs a torn std::wstring eventually.
    std::wstring shownPreset;

    // Cycling, render-thread only. Kept here rather than read off the sim
    // because the sim's clock is advanced by the worker; the render thread
    // owning its own deadline needs no cross-thread read at all.
    double nextPresetAt = 0.0;        // GetTime() when this display advances; 0 = never
    // Sequential order's position in the directory. Atomic because DISPLAY_NEXT
    // arrives on the IPC thread while the cycle timer runs on the render one.
    std::atomic<int> seqIndex{-1};

    // ── This surface's own recording objects (#186 phase 5b) ──────────
    //
    // One allocator, list and fence per surface rather than one shared set.
    // With a single worker this changes nothing -- it records each surface into
    // that surface's own list and signals that surface's fence -- but it is
    // what lets 5c give each surface a thread: two threads cannot record into
    // one command list, and a shared fence cannot say which surface's frame is
    // done.
    ComPtr<ID3D12CommandAllocator>    workAlloc;
    ComPtr<ID3D12GraphicsCommandList> workList;
    ComPtr<ID3D12Fence>               workFence;
    UINT64                            workSubmitted = 0;
    HANDLE                            fenceEvt = nullptr;

    bool WorkIdle() const {
      return !workFence || workSubmitted == 0 ||
             workFence->GetCompletedValue() >= workSubmitted;
    }

    int lastWrite    = -1;            // worker-only
    // The face THIS wake recorded, held until the submit succeeds. With one
    // surface the worker could assign lastWrite straight after the submit;
    // with several, the submit is shared, so each surface has to remember what
    // it recorded before knowing whether the batch went through.
    int pendingWrite = -1;            // worker-only
    int canvasLimit  = 0;

    // ── This surface's own worker thread (#186 phase 5c) ──────────────
    //
    // The phase plan expected this slice to need a SHARED engine mutex, on the
    // reasoning that N records taking it exclusively would serialise and gain
    // nothing. Measured, that premise is wrong: free-running two surfaces, an
    // iteration is 3.8 ms of which the record under the lock is 0.4 and the
    // wait FOR the lock is 0.2. The serial cost is the SIMULATION, 1.6 ms,
    // and that already runs outside the lock.
    //
    // So each surface simulates on its own thread and the records stay
    // serialised on the existing exclusive mutex. That is not a lesser
    // version of the plan, it is the larger half of the win for none of the
    // risk: everything the record shares -- the shaders' constant tables
    // (one shadow cbuffer per shader, written by SetVector then copied out),
    // the per-pass blur descriptor slots, the milk2 blend scratch on the
    // engine, the video-input lazy init, the SRV/RTV bump allocators -- keeps
    // exactly the serialisation it has today. Making the record concurrent
    // means giving every one of those a per-thread copy, to chase 16%.
    HANDLE            hThread = nullptr;
    unsigned          threadId = 0;
    HANDLE            hWake = nullptr;
    std::atomic<bool> quit{false};
    // Pacing, worker-thread only. Both were engine members when one worker
    // paced every surface; a thread per surface has to pace itself.
    LONGLONG          lastOrientQpc = 0;
    unsigned          seenPrimaryFrame = 0;
    // Was a function-local static std::map keyed by device -- one map mutated
    // by every worker. One thread per surface owns this outright.
    std::wstring      lastShapeLogKey;
  };
  // shared_ptr, and a mutex, because TWO THREADS walk this list.
  //
  // The render thread grows it (a display starts mirroring) and shrinks it (one
  // stops), while the worker iterates it to step each sim -- and that sim work
  // is deliberately OUTSIDE the engine mutex, so the engine lock does not cover
  // it. A vector of unique_ptr is not enough: the pointed-to surfaces stay put,
  // but the vector's own array reallocates and shifts under the worker's
  // iterator.
  //
  // Found by INSPECTION while adding the collection, not from a crash: no
  // observed failure has been attributed to it. It is fixed because the race is
  // plain in the code.
  //
  // So: structural changes take m_mirrorSurfacesMutex, and the worker copies
  // the shared_ptrs out under it once per wake and then works from that
  // snapshot. shared_ptr rather than raw pointers so a surface retired
  // mid-wake stays alive until the worker drops its reference.
  //
  // mutable so the lazy materialisation below works from a const method. A
  // surface existing is infrastructure rather than engine state; the
  // alternative is a const overload that cannot create one, which just moves
  // the empty case to every caller.
  mutable std::vector<std::shared_ptr<MirrorSurface>> m_mirrorSurfaces;
  mutable std::mutex m_mirrorSurfacesMutex;

  // The worker's per-wake snapshot. Never iterate m_mirrorSurfaces directly
  // from the worker.
  std::vector<std::shared_ptr<MirrorSurface>> SnapshotMirrorSurfaces() const {
    std::lock_guard<std::mutex> lock(m_mirrorSurfacesMutex);
    return m_mirrorSurfaces;
  }

  // The single surface every caller used before there was a collection.
  //
  // Named to be temporary on purpose. Phase 3b routes the worker and the panel
  // blits per surface and phase 4 addresses them by display, at which point
  // every call to this becomes MirrorSurfaceFor(device) and this goes. A name
  // that reads as permanent would leave the collection looking finished.
  MirrorSurface& OnlyMirrorSurface() const {
    // Locked: the IPC thread reaches this through the diagnostics, so the lazy
    // create is a THIRD thread appending to the list.
    std::lock_guard<std::mutex> lock(m_mirrorSurfacesMutex);
    if (m_mirrorSurfaces.empty())
      m_mirrorSurfaces.emplace_back(std::make_shared<MirrorSurface>());
    return *m_mirrorSurfaces.front();
  }
  // Ask a surface to change preset. Safe from any thread -- the worker adopts
  // it at the top of its next frame. Empty path means "follow the primary".
  void PostSurfacePreset(MirrorSurface& surf, const std::wstring& path) {
    std::lock_guard<std::mutex> lock(surf.requestMutex);
    surf.requestedPreset = path;
    surf.requestPending = true;
  }
  // Worker side: move a pending request onto the sim. Returns true if one was
  // taken, so the caller can restart the display's cycle clock.
  bool DrainSurfacePreset(MirrorSurface& surf) {
    std::wstring taken;
    {
      std::lock_guard<std::mutex> lock(surf.requestMutex);
      if (!surf.requestPending)
        return false;
      taken.swap(surf.requestedPreset);
      surf.requestPending = false;
    }
    surf.sim.ownPresetPath = taken;
    if (taken.empty())
      surf.sim.loadedOwnPath.clear();   // back to following the primary
    return true;
  }
  // One preset from a display's directory: by index when the display is set to
  // sequential order, at random otherwise. Empty when the directory has none.
  std::wstring PickDisplayPreset(const std::wstring& dir, bool sequential,
                                 int& seqIndex) const;
  // Step one display to its next (or previous) preset, in process.
  //
  // Shared by the IPC verbs and the Displays window's own buttons. It used to
  // exist twice, because the window sent SIGNAL|NEXT_PRESET down a child's pipe
  // while the IPC verb did the work here -- two paths to one behaviour, and
  // only one of them survived the children (#186 phase 6).
  //
  // err is a stable token on failure: "no_surface" or "no_presets".
  bool AdvanceDisplayPreset(DisplayOutput& out, bool next,
                            const wchar_t*& err, std::wstring& detail);
  // Step EVERY own-preset display, for the global Next/Prev. A nudge, not an
  // assignment: each picks its own next-or-random preset from its own
  // directory, so a wall of displays does not fall into lockstep (forgejo#22).
  // Was a SIGNAL|NEXT_PRESET broadcast down every child's pipe.
  void StepOwnPresetDisplays(bool next);

  // The surface serving `device`, created on first use. RENDER THREAD ONLY:
  // the worker walks this list, so growing it from another thread would move
  // the vector under it.
  // Returns the shared_ptr, not a reference: the surface's worker thread holds
  // one for its whole life, which is what keeps a retired surface alive until
  // that thread has actually stopped touching it.
  std::shared_ptr<MirrorSurface> EnsureMirrorSurface(const wchar_t* device);
  // Drop every surface whose display is not in `keep`, releasing its pipe.
  // Render thread only, for the same reason.
  void RetireMirrorSurfacesExcept(const std::vector<std::wstring>& keep);
  // Allocator, list and fence for one surface. Render thread; idempotent.
  bool EnsureSurfaceWorkObjects(MirrorSurface& surf);
  void ReleaseSurfaceWorkObjects(MirrorSurface& surf);
  // One surface's worker thread. Idempotent; safe to call for a surface that
  // already has one. RENDER THREAD ONLY -- they run alongside the surface
  // lifecycle above, and stopping one has to happen before its pipe is freed.
  bool StartSurfaceWorker(const std::shared_ptr<MirrorSurface>& surf);
  void StopSurfaceWorker(MirrorSurface& surf);
  // The worker body: one thread, one surface (#186 phase 5c).
  void MirrorThreadMain(const std::shared_ptr<MirrorSurface>& surf);
  void MirrorThreadDrawAndPresent(MirrorSurface& surf);
  // True when no surface has GPU work outstanding. RENDER THREAD ONLY: it
  // walks the list unlocked, which is safe only for the thread that is also
  // the one adding to and removing from it. Workers ask surf.WorkIdle().
  bool AllSurfacesIdle() const {
    for (auto& sp : m_mirrorSurfaces)
      if (!sp->WorkIdle()) return false;
    return true;
  }

  // What a display is showing, for the UI and for the IPC replies that used to
  // describe a child process (#186 phase 6). haveSurface separates "this
  // display is not rendering" from "it is, and it is following the primary".
  struct DisplayPresetInfo {
    bool         haveSurface = false;
    bool         ready = false;     // it has produced an image
    std::wstring preset;            // its own preset; empty means it follows
  };
  DisplayPresetInfo DisplayPresetStatus(const wchar_t* device) const;

  // Phase 4's entry point; returns nullptr when that display has no surface.
  MirrorSurface* MirrorSurfaceFor(const wchar_t* device) const {
    if (!device || !*device) return nullptr;
    std::lock_guard<std::mutex> lock(m_mirrorSurfacesMutex);
    for (auto& s : m_mirrorSurfaces)
      if (_wcsicmp(s->device.c_str(), device) == 0) return s.get();
    return nullptr;
  }
  size_t MirrorSurfaceCount() const {
    std::lock_guard<std::mutex> lock(m_mirrorSurfacesMutex);
    return m_mirrorSurfaces.size();
  }
  bool EnsureOrientPipeline(MirrorSurface& surf, int w, int h);
  void ReleaseOrientPipeline(MirrorSurface& surf);
  // Every surface at once, for device teardown and shutdown.
  void ReleaseAllOrientPipelines();
  // Full milk3 Buffer A–D + Image into internal imgFb only (never a swap-chain face).
  // outW/outH select aspect / size; result is read via BlitOrientOutputToMirror.
  bool RenderMilk3OrientPipeline(MirrorSurface& surf,
                                 ID3D12GraphicsCommandList* cmdList, int outW, int outH);
  // Classic .milk independent: VS ping-pong in imgFb, comp to fbA[0] for display.
  // Same user-facing independent mode as milk3.
  bool RenderClassicOrientPipeline(MirrorSurface& surf,
                                   ID3D12GraphicsCommandList* cmdList, int outW, int outH);
  // When set, textured shapes sample this instead of m_dx12VS[0] (orient re-render).
  const DX12Texture* m_pShapeVsOverride = nullptr;

  // The frame number a mirror's record publishes to presets, or < 0 for "use
  // the engine's" (#111).
  //
  // An independent mirror simulates the preset for itself and keeps its own
  // frame counter, which is the point of the mode -- a user who wants an exact
  // copy of the primary chooses the stretch path instead. What was wrong was
  // not that it had its own count, but that only SOME of the preset saw it:
  // per-frame EEL was fed the context's counter (mirror_sim.cpp) while custom
  // shapes, custom waves, sprite scripts and the HLSL `frame` uniform all read
  // GetFrame() -- the primary's -- during the very same mirror frame. One
  // preset, two different values for `frame`.
  //
  // Written only from inside the mirror record, which holds m_mirrorEngineMutex,
  // in keeping with the rule that engine state the worker reads is only written
  // under that lock (.claude/memory/mirror_sim_architecture.md).

  // What ApplyShaderParams last gave the HLSL `frame` uniform. One NAMED
  // consumer, deliberately: a latch shared by every consumer reports whichever
  // ran last, so three sites still doing the right thing hide a fourth doing
  // the wrong one -- measured, when a reverted ApplyShaderParams was masked by
  // the shapes path. -1 until a shader has been bound.
  int m_nDiagShaderFrame = -1;

  // The frame UpdateAudioTexture last ADVANCED the analysis on, and how many
  // times it was called during that frame (#110).
  //
  // It is called from two places: RenderFrame (milkdropfs.cpp:1057) and the
  // Shadertoy path (milkdropfs.cpp:2431, via RenderFrameShadertoy). Both fire
  // for a .milk3 preset, because RenderFrame runs unconditionally and
  // DX12_RenderWarpAndComposite then branches into the Shadertoy pipeline. So
  // every attack, decay, peak HOLD and peak decay term advanced TWICE per
  // frame on Shadertoy presets -- they behaved as if at double the frame rate,
  // regardless of the actual rate.
  //
  // The count is kept so a test can assert one advance per frame rather than
  // inferring it from behaviour.
  int m_nAudioTexAdvancedFrame = -1;
  int m_nAudioTexCallsThisFrame = 0;

  // The peak-hold length and per-frame decay actually in force (#110). Equal to
  // the profile's values at or below 199fps and stretched above it, so the
  // floor can be read rather than reasoned about -- it cannot be exercised in a
  // test, because an unfocused window is throttled well below the threshold and
  // taking focus to raise it would corrupt the reading it was taken for.
  int   m_nDiagPeakHoldFrames = 0;
  float m_fDiagPeakDecay = 0.0f;

  // Classic orient: true when re-rendering opposite aspect (more work already in aux).
  // Same-orient re-render can spend more of the aux buffer on shapes.
  bool m_bOrientOppositeAspect = false;
  // Stretch-blit latest orient imgFb → mirror RT (full clear + full viewport).
  // Safe source: never samples a flip-model back buffer (that caused leader-only
  // ghost bands after running a while when peers blitted from leader.bb).
  bool BlitOrientOutputToMirror(MirrorSurface& surf, ID3D12GraphicsCommandList* cmdList,
                                D3D12_CPU_DESCRIPTOR_HANDLE mirrorRtv, int monW, int monH);
  // Lock m_nTexSize + aspect to primary VS (clears GetWidth override). Call at the
  // start of every primary frame so mirror SizeGuard cannot leak portrait dims.
  void RestorePrimaryTexSizeFromVS() override;
  // Intermediate copy of primary BB for stretch/letterbox — never SRV the flip
  // surface (sampling flip BB between Execute and Present ghosts mirrors).
  DX12Texture m_mirrorSrcTex;
  bool m_bMirrorSrcCopiedThisFrame = false;
  // Copy flip BB → m_mirrorSrcTex on the *primary* list while still RT,
  // before Execute. SendToDisplayOutputs must not touch the flip BB.
  void CopyPrimaryToMirrorSrc();
  bool ShouldHideMirrorHud() const override { return m_bDisableMirrorHud && m_bMirrorsActive; }
  bool AnyIndependentMirrorEnabled() const override;
  void OnAnimationTimeRebased(float delta) override;
  void DrawDeferredMessages() override;
  // Supertext messages + HUD (+ optional sprites) onto a mirror RT.
  void DrawOverlaysToMirror(ID3D12GraphicsCommandList* cmdList, int monW, int monH,
                            bool drawSprites = true);
  // Clear every mirror SC back buffer (PRESENT/COMMON → clear → restore). Stops
  // flip-chain ghosts when switching landscape stretch → portrait orient.
  void ClearMirrorSwapChainAllBuffers(ID3D12GraphicsCommandList* cmdList,
                                      MonitorMirrorState& ms, UINT keepAsRtvIndex);

  // Blit main BB → mirror RT. mainBB must be PIXEL_SHADER_RESOURCE.
  // scaleMode: 0 = stretch-fill, 1 = letterbox/fit (bars), 2 = cover/crop (fill, no bars).
  void BlitMainToMirror(ID3D12GraphicsCommandList* cmdList,
                        ID3D12Resource* mainBB, int mainW, int mainH,
                        D3D12_CPU_DESCRIPTOR_HANDLE mirrorRtv, int monW, int monH,
                        int scaleMode = 0);
  void ToggleMirrorIndependentRender();
  void SetMirrorIndependentRender(bool enable); // absolute set (IPC SET_MIRROR_INDEPENDENT=)
  void RefreshDisplaysTab();
  // What the Displays list would say about the own-preset displays right
  // now. Polled on a timer so the list can notice a change without being
  // rebuilt every second. Was ChildListSignature (#186 phase 6).
  std::wstring DisplayListSignature() const;
  void ApplyMirrorWindowStyles();   // apply click-through + opacity to all active mirrors (render thread only)
  // A profile describes the whole display state. The two optional sections are
  // for capturing a test setup: the primary's preset settings, and the
  // messaging setup with its message set inlined.
  // bSnapshot captures what every screen is PLAYING as its startup preset,
  // rather than the settings that decide what it plays. The two are different
  // on purpose -- see the comment in SaveDisplayProfile -- and a snapshot is
  // the one you want when the point is "put this exact set of presets back".
  bool SaveDisplayProfile(const wchar_t* filePath,
                          bool bIncludePrimary = false,
                          bool bIncludeMessaging = false,
                          bool bSnapshot = false);
  bool LoadDisplayProfile(const wchar_t* filePath);

  // ── The display-profile folder ────────────────────────────────────────────
  //
  // resources/displayprofiles/. Profiles used to be saved wherever the file
  // dialog was last pointed, which is fine for one and useless for a set: a
  // script that steps through them needs somewhere to look.
  std::wstring DisplayProfileDir() const;
  // Moves resources/displayprofiles/ onto the profiles/<type>/ layout,
  // once, at startup. Safe to call when there is nothing to move.
  void MigrateDisplayProfiles();
  // Moves [DisplayOutputs] and [DisplayOutput_*] into resources/displays.json,
  // once. Copies by key enumeration and verifies every value before removing
  // the sections. Safe to call when there is nothing to move.
  void MigrateDisplaySettings();

  // Saved into a display profile, and applied when one is loaded: every
  // display the profile names is enabled, rather than only those that happened
  // to be running when it was saved. A profile is then "this is my setup, put
  // it on screen" rather than a record of what was on at that moment.
  //
  // It does NOT decide child-vs-mirror. That is per display and deliberate;
  // this only says they should all be on.
  bool m_bProfileAutostartAll = false;
  // A bare name resolves inside that folder; anything with a separator or a
  // drive is taken as a path and used as-is, so existing profiles keep working.
  std::wstring ResolveDisplayProfilePath(const wchar_t* nameOrPath) const;
  // Every .json in the folder, sorted by name -- which is why saved names are
  // timestamped: sorted by name is then sorted by age.
  std::vector<std::wstring> ListDisplayProfiles() const;
  // Mirrors DeleteMixerProfile: bare name resolves inside the folder, a path
  // is used as-is. No display profile had a delete before forgejo#95 -- the
  // window's own Profiles UI has no delete button either.
  bool DeleteDisplayProfile(const std::wstring& nameOrPath);
  // "yyyy-MM-dd_HH-mm-ss.json" for right now: what a new profile should be
  // called unless the user says otherwise.
  //
  // Shared by the snapshot action and by the Save Profile dialog, which is the
  // point. The snapshot generated a timestamp and the dialog opened with an
  // empty name, so a profile saved through the dialog got whatever could be
  // typed quickly -- "0901.json" -- and that sorts nowhere near its neighbours.
  // ListDisplayProfiles orders by name BECAUSE the name is a timestamp, and
  // StepDisplayProfile walks that order, so a hand-typed name does not merely
  // look untidy: it steps out of sequence.
  std::wstring SuggestedDisplayProfileName() const;
  // Save a timestamped snapshot into the folder. Returns its full path, or an
  // empty string on failure.
  std::wstring SaveDisplayProfileSnapshot();
  // Load whatever szStartupDisplayProfile names. Used by ALT-S in profile mode
  // and by its own hotkey.
  bool LoadDefaultDisplayProfile();
  // Step to the next/previous profile in the folder, wrapping. Returns false
  // when the folder holds none.
  bool StepDisplayProfile(int delta);
  // Path of the profile last saved or loaded, and whether one is applied at
  // startup. A profile is a complete display state, so when it loads at start
  // it WINS over the per-display values in settings.ini -- getting that state
  // is the point of asking for it.
  wchar_t m_szStartupDisplayProfile[MAX_PATH] = {};
  bool    m_bLoadDisplayProfileOnStart = false;
  void UpdateDisplaysTabSelection(int sel);
  // Cheap "has anything the Displays list shows about children changed", so a
  // timer can refresh only when it would say something different.
  // Which view the Displays list is showing: 0 the monitor arrangement,
  // 1 the detail lines. Lives on the engine rather than the window so it
  // survives the window being closed and reopened.
  int m_nDisplayViewMode = 0;
  int  m_nDisplaysTabSel = -1;  // Selected index in Displays tab listbox

  // Configurable hotkeys (local + global)
  HotkeyBinding m_hotkeys[NUM_HOTKEYS];
  void ResetHotkeyDefaults();
  void LoadHotkeySettings();
  void SaveHotkeySettings();

  // Hotkeys live in resources/hotkeys.json, not settings.ini. Only bindings
  // that differ from their action's default are written, so an untouched
  // install has almost no file at all.
  std::wstring HotkeyStorePath() const;
  void SaveHotkeysJson();
  bool LoadHotkeysJson();
  // One-time move of a [Hotkeys] section out of settings.ini. Writes, reads
  // back, compares every binding, and only then removes the section.
  void MigrateHotkeysToJson();
  void RegisterGlobalHotkeys(HWND hwnd);
  void UnregisterGlobalHotkeys(HWND hwnd);

  // What Windows actually granted, as opposed to what m_hotkeys asks for.
  //
  // Written by the registration pass on the message thread and read by the
  // Hotkeys ToolWindow thread and the IPC thread, so it takes its own mutex
  // rather than borrowing a convention. It is a side table and not fields on
  // HotkeyBinding because that struct is the persisted shape of the config;
  // this describes one run of one process and is never written to disk.
  std::vector<GlobalHotkeyState> m_globalHotkeyState;
  mutable std::mutex             m_globalHotkeyStateMutex;
  // Snapshot, so a caller cannot hold a reference across a re-register.
  std::vector<GlobalHotkeyState> GlobalHotkeyStates() const;
  // Was this action's global binding granted? False for an id that carries no
  // global binding at all, so callers must check the binding first -- "not
  // held" and "not asked for" are different facts and only the first is a
  // problem worth marking in the UI.
  bool IsGlobalHotkeyHeld(int id) const;
  bool HasGlobalHotkeyFailure(int id, DWORD* pErr = nullptr) const;
  // Both facts about one binding, read under ONE hold of the mutex.
  //
  // The two calls above answer half the question each, and asking them in
  // turn takes the lock twice with a re-registration free to land in between
  // -- so the held flag and the error code could come from different passes
  // and disagree. Returns false when the id has no entry, which is a third
  // answer again: nothing global was ever asked for.
  bool GlobalHotkeyStatus(int id, bool* pHeld, DWORD* pErr) const;
  // What a registration pass may do here: keep, probe-and-release, or nothing.
  GlobalHotkeyMode GlobalHotkeyRegistrationMode() const;

  // Which global combinations are actually available, measured by trying.
  //
  // Safe to call from any thread, unlike RegisterGlobalHotkeys: the probes
  // pass hWnd = NULL, so they belong to the calling thread and never touch a
  // window somebody else owns. Nothing is left registered when it returns.
  std::vector<HotkeyScanRow> ScanGlobalHotkeyAvailability();
  // The label for a scan row's owner, or empty when it is not one of ours.
  std::wstring HotkeyOwnerName(int id) const;

  // The bindings that assigning <mod,vk> at <scope> would clear, as the
  // sentence the confirm dialog shows. Empty when nothing would be lost.
  // Built here rather than in the UI so the IPC diagnostic and the dialog
  // cannot disagree about what is at stake.
  std::wstring DescribeHotkeyConflicts(int excludeBuiltIn, int excludeUserId,
                                       UINT modifiers, UINT vk, HotkeyScope scope,
                                       int* pCount = nullptr) const;

  bool DispatchHotkeyAction(int actionId);
  bool LookupLocalHotkey(UINT vk, UINT modifiers);
  bool DispatchHotkeyByTag(const std::wstring& tag);
  std::wstring FormatHotkeyDisplay(UINT modifiers, UINT vk) const;

  // Dynamic F1 help text (generated from binding table, all pages in one buffer)
  wchar_t m_szHelpAll[16384] = {};
  int     m_nHelpLineCount = 0;   // total lines in m_szHelpAll
  void GenerateHelpText();

  // Help display category order (user-configurable)
  int  m_helpCatOrder[HKCAT_COUNT] = {};
  void ResetHelpCatOrder();
  void LoadHelpCatOrder();
  void SaveHelpCatOrder();

  // Dynamic user-added hotkeys (Script Commands and Launch Apps)
  std::vector<UserHotkey> m_userHotkeys;
  int m_nextUserHotkeyId = USER_HOTKEY_ID_BASE;
  int  AddUserHotkey(UserHotkeyType type);          // returns index in m_userHotkeys
  void RemoveUserHotkey(int index);
  void LaunchOrFocusApp(const std::wstring& path);

  // Idle timer (screensaver mode)
  bool m_bIdleTimerEnabled = false;
  int  m_nIdleTimeoutMinutes = 5;     // 1-60 minutes
  int  m_nIdleAction = 0;             // 0 = Fullscreen, 1 = Stretch/Mirror, 2 = Mirror all
  bool m_bIdleAutoRestore = true;     // True = restore on mouse/keyboard; false = manual hotkey only
  bool m_bIdleActivated = false;      // True when idle timer triggered activation
  void LoadIdleTimerSettings();
  void SaveIdleTimerSettings();
  // =========================================================

  /// CONFIG PANEL SETTINGS THAT WE'VE ADDED (TAB #2)
  bool		m_bFirstRun;
  bool    m_bSelfBootstrapped = false; // true when exe ran from empty directory (no resources found)
  float		m_fBlendTimeAuto;		// blend time when preset auto-switches
  float		m_fBlendTimeUser;		// blend time when user loads a new preset
  float		m_fTimeBetweenPresets;		// <- this is in addition to m_fBlendTimeAuto
  float		m_fTimeBetweenPresetsRand;	// <- this is in addition to m_fTimeBetweenPresets
  bool    m_bSequentialPresetOrder;
  bool		m_bHardCutsDisabled;
  float		m_fHardCutLoudnessThresh;
  int     m_nInjectEffectMode;   // 0=off 1=brighten 2=darken 3=solarize 4=invert (F11)
  float		m_fHardCutHalflife;
  // Initialised here as well as at frame 0: the hotkey and render-command
  // paths multiply it, and either can run before the first rendered frame.
  float		m_fHardCutThresh = 0.0f;
  //int			m_nWidth;
  //int			m_nHeight;
  //int			m_nDispBits;
  int     m_nCanvasStretch;   // 0=Auto, 100=None, 125 = 1.25X, 133, 150, 167, 200, 300, 400 (4X).
  int			m_nTexSizeX;			// -1 = exact match to screen; -2 = nearest power of 2.
  int			m_nTexSizeY;
  float   m_fAspectX;
  float   m_fAspectY;
  float   m_fInvAspectX;
  float   m_fInvAspectY;
  int     m_nTexBitsPerCh;
  int			m_nGridX;
  int			m_nGridY;

  // Parallel evaluation of the per-vertex (per_pixel) equations. The pool is
  // started lazily on first use and torn down with the engine; the calling
  // render thread participates as worker 0, so a count of 1 means "serial" and
  // no thread is ever created. See pv_workers.h for the measurements that
  // motivated this and the argument that it is safe.
  mdrop::PvThreadPool m_pvPool;
  int  GetPerVertexWorkerCount();   // includes the calling thread; 1 = serial
  void ShutdownPerVertexPool();
  float   m_fHudFontUserScale = 1.0f;  // 0.10..2.00 extra HUD size (Settings)
  bool    m_bDisableMirrorHud = false; // Settings: no HUD/overlays on mirrors
  int			m_nMixType = -1; // -1 = Random

  // bool		m_bShowPressF1ForHelp;
  //char		m_szMonitorName[256];
  bool		m_bShowMenuToolTips;
  int			m_n16BitGamma;
  bool		m_bAutoGamma;
  //int		m_nFpsLimit;
  //int			m_cLeftEye3DColor[3];
  //int			m_cRightEye3DColor[3];
  bool		m_bEnableRating;
  //bool        m_bInstaScan;
  bool		m_bSongTitleAnims;
  int     m_nSpriteMessagesMode = 3;  // 0=Off, 1=Messages, 2=Sprites, 3=Messages & Sprites
  float		m_fSongTitleAnimDuration;
  float		m_fTimeBetweenRandomSongTitles;
  float		m_fTimeBetweenRandomCustomMsgs;
  int			m_nSongTitlesSpawned;
  int			m_nCustMsgsSpawned;
  bool    m_bEnablePresetStartup = true;
  bool    m_bEnableAudioCapture = true;
  float   m_fAudioSensitivity = 1.0f;   // 1.0 = passthrough (default), >1 = manual gain boost
  bool    m_bEnablePresetStartupSavingOnClose = true;
  bool    m_bAutoLockPresetWhenNoMusic;
  bool    m_bScreenDependentRenderMode;
  int     m_nBassStart = 0;
  int     m_nBassEnd = 250;
  int     m_nMidStart = 250;
  int     m_nMidEnd = 4000;
  int     m_nTrebStart = 4000;
  int     m_nTrebEnd = 20000;
  float   m_MessageDefaultBurnTime = 0.1f;
  float   m_MessageDefaultFadeinTime = 0.2f;
  float   m_MessageDefaultFadeoutTime = 0.0f;
  
  bool m_WindowBorderless = false;
  float m_WindowWatermarkModeOpacity = 0.3f;
  int m_WindowX = 0;
  int m_WindowY = 0;
  int m_WindowWidth = 0;
  int m_WindowHeight = 0;
  int m_bStartFullscreen = 0;
  // Point on the monitor that was fullscreen when the app last exited.
  int m_FullscreenHintX = 0;
  int m_FullscreenHintY = 0;
  int m_WindowFixedWidth = 960;
  int m_WindowFixedHeight = 540;
  
  // Preset mouse interaction controls
  bool m_bEnableMouseInteraction = true;
  float m_mouseX = 0.5;
  float m_mouseY = 0.5;
  float m_lastMouseX;
  float m_lastMouseY;
  bool m_mouseDown;
  int m_mouseClicked;

  // Shadertoy iMouse state (pixel coordinates, bottom-left origin)
  float m_stMouseX = 0.f;      // drag position x (pixels), persists when released
  float m_stMouseY = 0.f;      // drag position y (pixels), persists when released
  float m_stClickX = 0.f;      // click-start position x (pixels)
  float m_stClickY = 0.f;      // click-start position y (pixels)
  bool  m_stMouseDown = false;  // left button currently held
  bool  m_stMouseJustClicked = false; // true for one frame on click

  float fOpacity = 1.0f; // 0.0f = 100% transparent, 1.0f = 100% opaque
  bool m_RemotePresetLink = false;
  bool m_bAlwaysOnTop = false;

  enum TrackInfoSource { TRACK_SOURCE_SMTC = 0, TRACK_SOURCE_IPC = 1, TRACK_SOURCE_WINDOW = 2 };
  int m_nTrackInfoSource = TRACK_SOURCE_SMTC;
  bool m_bSongInfoOverlay = true;           // show overlay text notifications on track change
  wchar_t m_szTrackWindowTitle[256] = {};   // window title to scrape (TRACK_SOURCE_WINDOW) — legacy, migrated to profiles

  std::vector<WindowTitleProfile> m_windowTitleProfiles;
  int m_nActiveWindowTitleProfile = 0;

  bool m_SongInfoPollingEnabled = true;
  int m_SongInfoDisplayCorner = 3;

  bool m_ChangePresetWithSong = true;
  float m_SongInfoDisplaySeconds = 5.0f;
  bool m_bSongInfoAlwaysShow = false;
  bool m_DisplayCover = true;
  bool m_DisplayCoverWhenPressingB = true;
  float m_MediaKeyNotifyTime = 1.0f;  // seconds to show media key notification
  bool m_HideNotificationsWhenRemoteActive = false;
  bool m_bShowNotifications = true;  // false = suppress all HUD notifications

  // Error Display Settings
  float   m_ErrorDuration       = 8.0f;     // seconds

  // FFT EQ Smoothing (Milkwave Remote)
  float   m_fFFTAttackGlobal    = 0.5f;     // attack rate (0..1), set via IPC or INI
  float   m_fFFTDecayGlobal     = 0.5f;     // decay rate (0..1), set via IPC or INI
  bool    m_bFFTSmoothingActive = false;     // true once Remote sends FFT params
  float   m_fFFTSmoothed[MY_FFT_SAMPLES];   // smoothed spectrum per bin
  // Largest bin this frame, computed unconditionally and published to shaders
  // as _fftParams.z. It is what lets a preset recovered from MD3 PRO -- whose
  // FFT texture is peak-normalised so texels always reach 1.0 -- divide back
  // into that domain, without switching the whole audio profile for it.
  float   m_fFFTFramePeak = 0.0f;
  float   m_fFFTPeak[MY_FFT_SAMPLES];       // peak hold per bin
  int     m_nFFTPeakHold[MY_FFT_SAMPLES];   // frames remaining in peak hold

  int m_MinPSVersionConfig = 4; // MD2_PS_3_0: DX12 requires ps_3_0 minimum (ps_2_a silently drops texture bindings)
  int m_MaxPSVersionConfig = 6;
  bool m_ShowUpArrowInDescriptionIfPSMinVersionForced = false;

  // GPU Protection Settings
  int  m_nMaxShapeInstances = 0;         // Cap per-shape instance count (0=unlimited, e.g. 512)
  bool m_bScaleInstancesByResolution = false; // Scale down num_inst at resolutions above base
  int  m_nInstanceScaleBaseWidth = 1920; // Reference width for instance scaling (instances scale down above this)
  bool m_bSkipHeavyPresets = false;      // Auto-skip presets exceeding GPU safety thresholds
  int  m_nHeavyPresetMaxInstances = 4096; // Total shape instances across all shapes that triggers skip
  // Exit when available local VRAM (budget-based) falls below this percent. 0 = disabled.
  int  m_nMinAvailableVramPercent = 5;
  // Last DXGI local VRAM sample (for debug overlay / exit message)
  UINT64 m_vramBudgetBytes = 0;
  UINT64 m_vramUsageBytes = 0;
  float  m_vramAvailablePercent = 100.0f;
  bool   m_bVramExitTriggered = false;

  //bool		m_bAlways3D;
  //float       m_fStereoSep;
  //bool		m_bAlwaysOnTop;
  //bool		m_bFixSlowText;
  //bool		m_bWarningsDisabled;		// messageboxes
  bool		    m_bWarningsDisabled2;		// warnings/errors in upper-right corner (m_szUserMessage)
  bool        m_bAnisotropicFiltering;
  bool        m_bPresetLockOnAtStartup;
  bool        m_bPreventScollLockHandling;
  int         m_nMaxPSVersion_ConfigPanel;  // -1 = auto, 0 = disable shaders, 2 = ps_2_0, 3 = ps_3_0
  int         m_nMaxPSVersion_DX9;          // 0 = no shader support, 2 = ps_2_0, 3 = ps_3_0
  int         m_nMaxPSVersion;              // this one will be the ~min of the other two.  0/2/3.
  int         m_nMaxImages;
  int         m_nMaxBytes;

  HFONT       m_gdi_title_font_doublesize;

  // PIXEL SHADERS
  DWORD                   m_dwShaderFlags;       // Shader compilation/linking flags
  //ID3DXFragmentLinker*    m_pFragmentLinker;     // Fragment linker interface
  //LPD3DXBUFFER            m_pCompiledFragments;  // Buffer containing compiled fragments
  LPD3DXBUFFER            m_pShaderCompileErrors;
  VShaderSet              m_fallbackShaders_vs;  // *these are the only vertex shaders used for the whole app.*
  PShaderSet              m_fallbackShaders_ps;  // these are just used when the preset's pixel shaders fail to compile.
  PShaderSet              m_shaders;     // includes shader pointers and constant tables for warp & comp shaders, for cur. preset
  PShaderSet              m_OldShaders;  // includes shader pointers and constant tables for warp & comp shaders, for prev. preset
  PShaderSet              m_NewShaders;  // includes shader pointers and constant tables for warp & comp shaders, for coming preset
  ShaderPairInfo          m_BlurShaders[2];
  bool                    m_bWarpShaderLock;
  bool                    m_bCompShaderLock;
  //bool LoadShaderFromFile( char* szFile, char* szFn, char* szProfile,
  //                         LPD3DXCONSTANTTABLE* ppConstTable, void** ppShader );
#define SHADER_WARP  0
#define SHADER_COMP  1
#define SHADER_BLUR  2
#define SHADER_OTHER 3
  // Which preset a shader compile error belongs to (#184).
  //
  // thread_local for the same reason g_eelCompileCtx is (state.h): each render
  // context compiles its own preset on its own thread, so the identity cannot
  // be a single Engine member. A wrong answer here is not a lost diagnostic --
  // AutoFlagPresetError writes an `error` flag into presets.json and the
  // selection policy then SKIPS that preset, so a misattribution silently drops
  // a working preset out of rotation.
  //
  // It also fixes a live mis-attribution that predates any of this. During a
  // load m_szCurrentPresetFile still names the OUTGOING preset --
  // LoadPresetTick promotes m_szLoadingPreset into it only once the load
  // commits -- so a shader failure compiling the INCOMING preset flagged
  // whatever was playing before it. The SEH crash diag already uses
  // m_szLoadingPreset for exactly this window; the shader path did not.
  //
  // Saves and restores rather than clearing, because compiles nest.
  static thread_local const wchar_t* t_shaderCompilePreset;
  struct ShaderCompileScope {
    const wchar_t* prev;
    explicit ShaderCompileScope(const wchar_t* preset)
        : prev(t_shaderCompilePreset) { t_shaderCompilePreset = preset; }
    ~ShaderCompileScope() { t_shaderCompilePreset = prev; }
  };
  // The file to flag: the preset being compiled on THIS thread, or the current
  // one when no compile scope is open (a Preset Editor Apply, say).
  const wchar_t* PresetFileForCompileError() const {
    return (t_shaderCompilePreset && t_shaderCompilePreset[0])
             ? t_shaderCompilePreset : m_szCurrentPresetFile;
  }

  bool LoadShaderFromMemory(const char* szShaderText, char* szFn, char* szProfile,
    LPD3DXCONSTANTTABLE* ppConstTable, void** ppShader, int shaderType, bool bHardErrors, bool compileOnly,
    LPD3DXBUFFER* ppBytecodeOut = nullptr, const char* szDiagName = nullptr);
  bool RecompileVShader(const char* szShadersText, VShaderInfo* si, int shaderType, bool bHardErrors, bool bCompileOnly);
  bool RecompilePShader(const char* szShadersText, PShaderInfo* si, int shaderType, bool bHardErrors, int PSVersion, bool bCompileOnly, const char* szDiagName = nullptr);

  // ── Compiled-shader line mapping (for the Preset Editor's error display) ──
  // LoadShaderFromMemory prepends include.fx and the per-type #defines, then
  // injects a few lines INSIDE the preset's shader_body (the "void PS(...)"
  // line, "float3 ret = 0;", and for comp a 4-line rad/ang block).  D3DCompile
  // reports line numbers in that assembled text, dozens of lines off from what
  // the user typed.  These record the shift so the editor can point at the
  // right line.  Written on the render thread during a shader compile.
  int              m_nShaderPreludeLines = 0;   // lines before the preset's own text
  std::vector<int> m_shaderInjectedLines;       // compiled-text lines the engine inserted
  std::wstring     m_wLastShaderError;          // raw D3DCompile text of the last failure
  // 1-based compiled line -> 1-based user line, or 0 if the line lies in
  // engine-generated prelude rather than anything the user wrote.
  int MapCompiledLineToUserLine(int nCompiledLine) const;

  // ── Preset Editor apply/save (RENDER THREAD ONLY) ──
  // Reached via RenderCmd::ApplyPresetCode / RenderCmd::SavePresetFile.  Never
  // call these from a ToolWindow thread: they touch m_pState, m_shaders and the
  // DX12 PSOs, all of which belong to the render thread.
  // nSide is a PresetSide: PSIDE_LIVE edits m_pState, PSIDE_BLENDFROM edits
  // m_pOldState (preset 1 of a frozen .milk2, which renders every frame too).
  bool ApplyPresetCodeSection(int section, int index, const char* code, int nSide = 0);
  // True when a .milk2 is on screen and both of its presets are being rendered,
  // so preset 1 is a real, editable, visible thing.
  bool HasEditableBlendFromPreset() const {
    return m_bMilk2FrozenBlend && m_pOldState != nullptr;
  }
  bool SavePresetToPath(const wchar_t* szPath);
  // Write both presets of a frozen .milk2 back out in the wrapper format.
  bool SaveMilk2ToPath(const wchar_t* szPath);
  // Replace the live preset from complete .milk text (the editor's whole-file
  // and raw views).  Leaves m_szCurrentPresetFile alone so Save still targets
  // the real file rather than the temp one this writes.
  bool ApplyPresetTextToState(const std::wstring& milkText, int nSide = 0);

  bool EvictSomeTexture();
  typedef std::vector<TexInfo> TexInfoList;
  TexInfoList     m_textures;
  bool m_bNeedRescanTexturesDir;

  // True once ANY preset -- real or the embedded built-in fallback -- has
  // ever been shown this session. Reused instead of the old bHasPreset check
  // (LoadRandomPreset compared m_pState->m_szDesc to INVALID_PRESET_DESC, a
  // literal nothing ever assigns since CState::Default() always fills
  // m_szDesc with a timestamp -- so that comparison was always false and the
  // branch it gated was dead code). Lets an empty-preset-directory rescan
  // apply the built-in fallback exactly once, then leave whatever is showing
  // (real or built-in) alone on every subsequent empty rescan.
  bool m_bAnyPresetEverShown = false;
  // vertex declarations:

  D3DXVECTOR4 m_rand_frame;  // 4 random floats (0..1); randomized once per frame; fed to pixel shaders.

  // RUNTIME SETTINGS THAT WE'VE ADDED
  float   m_prev_time;
  bool    m_bTexSizeWasAutoPow2;
  bool    m_bTexSizeWasAutoExact;
  bool    m_bPresetLockedByUser = false;

  // ── What a loaded display profile is standing in front of ────────────
  //
  // A display profile may carry main-render preset settings. Those apply to
  // the RUNNING session and must never become the saved globals: Shane's rule
  // is "this would only be the loaded profile, so if the profile doesn't load
  // then the global settings still work".
  //
  // Without this they would. The shutdown save writes fTimeBetweenPresets,
  // bPresetLockOnAtStartup and szPresetDir straight out of the live members
  // (engine.cpp, SaveSettings), so loading a profile and later quitting
  // silently promoted the profile's values into settings.ini -- and there was
  // then no way back to the globals short of editing the file.
  //
  // So the pre-profile value of every field a profile overrides is kept here,
  // and the settings save writes THIS instead of the live member. Per field,
  // because a profile may override the preset and leave the timing alone.
  //
  // A deliberate change clears the override for that field and becomes the new
  // global -- picking a preset directory in Settings, or a hotkey changing it,
  // is the user speaking about their configuration rather than a profile
  // borrowing it. See ClearPrimaryOverride.
  // ── The main render's settings AS THIS PROFILE CARRIES THEM ──────────
  //
  // What the Displays window's "Main render" group edits, and what a saved
  // profile writes out. Separate from the live m_sz/m_f members on purpose:
  // these are what the profile SAYS, and the live members are what the app is
  // currently doing. Editing the group therefore does not jump the main
  // window mid-edit -- picking a preset for a profile is not a request to
  // play it now.
  //
  // Every field has an inherit state, shown as "(default)" by the same mode
  // combos a child display uses: an empty string, or -1. A profile that
  // inherits everything carries no opinion about the main render at all, and
  // the globals stay in charge -- which is the point of the whole feature.
  struct PrimaryProfileCfg {
    wchar_t szPresetDir[MAX_PATH] = {};
    wchar_t szStartupPreset[MAX_PATH] = {};
    float   fTimeBetweenPresets = -1.0f;  // -1 inherit
    int     nSequentialOrder = -1;        // -1 inherit / 0 random / 1 sequential
    int     nPresetLock = -1;             // -1 inherit / 0 unlocked / 1 locked
  };
  PrimaryProfileCfg m_primaryProfile;

  // The main render's cycle interval, order and lock, set from the Displays
  // window's main-render row.
  //
  // These write the GLOBALS -- the same values the Presets window drives -- so
  // the two windows describe one setting rather than two. They used to write
  // m_primaryProfile instead, a staging area for the next saved profile, and
  // that one split produced three separate faults: unticking "Cycle every"
  // left the main window still cycling, the value did not survive a restart
  // because the pin was never persisted, and a saved profile fell back to
  // recording whatever the live global had drifted to. Measured across 13 real
  // profiles: the primary interval wandered 41 -> 0 -> 60 with no deliberate
  // change behind any of it.
  //
  // ClearPrimaryOverride, like every other deliberate change: a value the user
  // just typed is theirs, not a display profile's session-only loan, so it must
  // become what the shutdown save writes.
  void SetMainRenderInterval(float secs);
  void SetMainRenderOrder(bool sequential);
  void SetMainRenderLock(bool locked);
  // Shared by the Displays window's "Enabled" checkbox and
  // SET_DISPLAY_ENABLED= -- stops or starts an own-process display's child
  // along with the flag, since a mirror/spout display has nothing else to do
  // and a child display left running with bEnabled=false is exactly
  // "unchecking Enabled doesn't stop a child".
  void SetDisplayEnabled(DisplayOutput& out, bool enabled);

  // Put the main window on this preset now. Goes through the render command
  // queue rather than calling LoadPreset: a profile is loaded from the UI
  // thread and the preset has to change on the render thread.
  void ApplyPrimaryProfilePreset(const std::wstring& path);

  // Which field a deliberate edit is clearing. Not an enum class: these are
  // passed from plain C-style config code that has no using-declaration.
  enum {
    kPrimaryOverrideDir = 0,
    kPrimaryOverrideStartup,
    kPrimaryOverrideTime,
    kPrimaryOverrideOrder,
    kPrimaryOverrideLock,
  };
  struct PrimaryOverride {
    bool    has = false;
    wchar_t presetDir[MAX_PATH] = {};
    wchar_t presetStartup[MAX_PATH] = {};
    float   timeBetweenPresets = 0.0f;
    bool    sequentialOrder = false;
    bool    presetLocked = false;
    // Which fields the profile actually took over. Anything false here reads
    // and saves from the live member as normal.
    bool    hasDir = false, hasStartup = false, hasTime = false;
    bool    hasOrder = false, hasLock = false;
  };
  PrimaryOverride m_primaryOverride;

  // Remember the global before a profile replaces it, and mark that field
  // overridden. PER FIELD, and only the first time that field is taken over:
  //
  //   * per field, because a profile may override the preset and leave the
  //     timing alone, and a struct-wide "already stashed" flag then blocks the
  //     timing from ever recording its own global -- the next profile to take
  //     the timing would restore a value from two profiles ago;
  //   * only the first time, because a second profile loaded over the first
  //     must not record the FIRST profile's value as the global to restore.
  void StashPrimaryGlobal(int field);
  // A deliberate user edit: this field's global is now the live value again.
  void ClearPrimaryOverride(int field);   // see kPrimaryOverride* below
  // The value the settings save should write for each field.
  const wchar_t* GlobalPresetDir() const;
  float GlobalTimeBetweenPresets() const;
  bool  GlobalSequentialOrder() const;
  bool  GlobalPresetLocked() const;
  bool    m_bPresetLockedByCode;
  // True while the Preset Editor window is open.  AutoPresetChangesAllowed()
  // reads it to suppress auto-advance so an unsaved edit cannot be discarded by
  // a preset change.  Atomic: set from the editor's own thread, read on the
  // render thread every frame.
  std::atomic<bool> m_bPresetEditorOpen{false};

  // Testing mode: freeze everything that changes the frame without being asked
  // -- the timed advance, the audio hard cuts, the preset change on song
  // change, the idle timer -- and ignore the keyboard, so a stray keystroke
  // cannot alter a measurement in progress. Explicit IPC still works; the
  // point is to remove what the harness did not ask for.
  //
  // Deliberately NOT persisted. A testing flag that came back after a restart
  // would be worse than not having one, and it is the rule the VFX profile
  // store already settled on: nothing is written unless explicitly saved.
  bool    m_bTestingMode = false;

  // "Testing mode" and "nothing may change the preset on its own" used to be
  // the same flag, and child mode turns testing mode on permanently and
  // refuses to turn it off -- so a child could never advance its own preset
  // (forgejo#22). A child genuinely needs the rest of testing mode: the
  // keyboard ignored, settings.ini shielded, presets.json frozen, the idle
  // timer stopped. It does not need this one.
  //
  // Written only by SetTestingMode. Read only by
  // AutoPresetChangesAllowed(). Everything else about testing mode stays keyed
  // to m_bTestingMode and must stay that way.
  bool    m_bSuppressAutoPresetChanges = false;


  // ── Restraints ────────────────────────────────────────────────────────────
  //
  // Testing mode and child mode are two PROFILES over one set of named
  // behaviours, not one flag with exceptions bolted on. The second profile was
  // child mode, whose evidence was that SetTestingMode had to write
  // "&& !m_bChildMode" twice: once so a child could cycle its own presets, once
  // so a child was not pinned to 60fps. #186 phase 6 removed that profile; the
  // table stays because the next divergence should be a row, not a special case.
  //
  // Deliberately NARROW. Only the behaviours that actually differ are routed
  // through here; everything else about testing mode stays keyed to
  // m_bTestingMode, which a child still holds permanently. Moving all of it
  // at once would be a 37-site rewrite of a safety mechanism for no behaviour
  // change, and the review of that idea found real regressions hiding in it.
  struct restraint {
    enum Bits : unsigned {
      kNone       = 0,
      kAutoPreset = 1u << 0,   // timed advance, audio hard cuts, song change
      kFpsCap     = 1u << 1,   // pin the frame cap to kTestingModeFpsCap
      kMessages   = 1u << 2,   // the timers that spawn messages.ini text and
                               // random song titles (milkdropfs.cpp)
      kPrompts    = 1u << 3,   // dialogs that WAIT FOR A PERSON: the scoped
                               // VFX Keep/Discard window
                               // (engine_video_effects_ui.cpp)
    };
  };

  // Everything testing mode restrains, of the behaviours that vary.
  //
  // kPrompts is here because a sweep has nobody to answer one. The scoped VFX
  // prompt appears when a preset change retires a profile with a pending edit,
  // which a preset sweep does hundreds of times -- so an unattended run put a
  // window on Shane's screen and then sat behind it. A prompt raised by a test
  // is also asking the wrong person about the wrong thing: the edit it offers
  // to keep was made by the test, not by him.
  //
  // It restrains the WINDOW, not the decision. The scope still waits for an
  // answer, and VFX_SCOPED_KEEP / VFX_SCOPED_DISCARD still deliver one, so a
  // harness that means to exercise Keep can. Answering on the harness's behalf
  // is not the same thing as not asking, and only the second is wanted here.
  static constexpr unsigned kTestingRestraints =
      restraint::kAutoPreset | restraint::kFpsCap | restraint::kMessages |
      restraint::kPrompts;

  // What a child REFUSES even while in testing mode. A child renders a real
  // display for a real viewer: it has to advance its own presets (forgejo#22),
  // run at the rate its panel can do -- or a child-vs-mirror benchmark measures
  // the pin instead of the cost -- and show the messages the user configured.
  //
  // A SECOND profile stood beside the testing one: kChildVetoes, the set of
  // testing-mode restraints a -child instance lifted again, because a child
  // held testing mode permanently and still had to cycle its own presets, run
  // uncapped and spawn its own messages. #186 phase 6 deleted -child, so there
  // is one profile again and the table has one row.

  std::atomic<unsigned> m_restraints{restraint::kNone};

  // ── Leaving testing mode without leaving the test's state behind ──────────
  //
  // The write shield protected the FILE, never the memory. Every shielded
  // write had a paired assignment to a live member -- bEnabled,
  // bIndependentRender, bOwnProcess, nOpacity -- and lowering the shield threw
  // the overlay away while those survived. The next call to
  // SaveDisplayOutputSettings, a full-state serializer with 31 call sites,
  // then published the test's values as if the user had chosen them. That is
  // how a run left displays enabled and independent, and it happened twice in
  // one session before it was understood.
  //
  // Restoring by RE-READING the file was tried on paper and is wrong: it
  // cannot reconcile the live objects a config field controls (a running child
  // process, a mirror window), and an entry with no saved section would lose
  // the identity fields EnumerateDisplayOutputs owns. A snapshot of the live
  // state taken when the shield went up has neither problem -- it IS the
  // pre-test session state.
  std::vector<DisplayOutputConfig> m_displaySnapshot;
  bool     m_bHaveDisplaySnapshot = false;
  // Whether THIS session actually raised the shield. Not the same as
  // m_bTestingModeWritesSettings, which is a one-way latch that can be set
  // mid-session -- keying the teardown off that could strand the shield up.
  bool     m_bTestingShieldRaised = false;
  int      m_snapAltSMode = 0;
  bool     m_snapMirrorPromptDisabled = false;
  bool     m_snapMirrorIndependentDefault = false;
  bool     m_snapDisableMirrorHud = false;
  int      m_snapMirrorMaxFps = 0;

  void TakeDisplaySnapshot();
  // Runs on the RENDER thread, via RenderCmd::RestoreAfterTesting. It rewrites
  // m_displayOutputs and retires children, neither of which may happen on the
  // IPC or a ToolWindow thread while the render thread walks the same vector
  // (render_commands.h). It also lowers the write shield, LAST -- so a process
  // killed before this runs leaves the shield up and the file untouched, which
  // is the common way a sweep ends.
  void RestoreDisplaysAfterTesting();

  bool Restrained(unsigned bit) const {
    return (m_restraints.load(std::memory_order_relaxed) & bit) != 0;
  }
  void RecomputeRestraints() {
    unsigned r = m_bTestingMode ? kTestingRestraints : restraint::kNone;
    m_restraints.store(r, std::memory_order_relaxed);
    m_bSuppressAutoPresetChanges = (r & restraint::kAutoPreset) != 0;
  }

  // -child, and every field that configured one -- the display it covered,
  // its slot, its fade, its test rect, whether it hid its window or took
  // the foreground, its parent's pid, and the -nostartupchildren latch that
  // suppressed the spawns -- lived here. #186 phase 6 deleted the mode.

  // -test on the command line: come up as a test instance. See IsTestInstance.
  bool    m_bTestSwitch = false;

  // True when this process must behave as a harness instance rather than as
  // the user's visualizer.
  //
  // Two ways in, and both matter. The exe NAME carrying "_test" is how the
  // harness has always been recognised, and it has to keep working because the
  // decision is made long before any IPC could arrive to say so. `-test` is the
  // explicit form, which the renamed copy cannot express and which lets the
  // REAL exe be started as a test instance without being copied first.
  //
  // This used to be an inline StrStrIW(leaf, L"_test") written out at each
  // site -- the window title and the TCP server -- which is exactly the shape
  // that lets a third site be added and quietly not get the check.
  bool IsTestInstance() const;

  // The process was launched SW_SHOWNOACTIVATE: come up without taking the
  // foreground, and keep not taking it. On Engine rather than as an App.cpp
  // global because the activations are NOT all in App.cpp -- suppressing every
  // one of them there still stole focus, because RaiseMirrorSurfaces in
  // engine_input.cpp calls SetForegroundWindow too, and startup raises mirrors.
  bool    m_bQuietLaunch = false;
  // The frame cap in force before testing mode pinned it, so leaving testing
  // mode gives the user's cap back. -1 = nothing to restore.
  int     m_nFpsCapBeforeTesting = -1;
  // Time of the first ESC. Leaving testing mode takes two presses, because one
  // stray key must not end a run that has been going for minutes.
  float   m_fTestingEscapeArmedTime = -1.0f;
  // Latched on the first entry into testing mode and never cleared. Leaving
  // testing mode gives the keyboard and the auto-advance back, but it cannot
  // give back what the harness did to the settings held in memory -- the render
  // window has been moved and resized, the preset directory repointed. The
  // shutdown auto-save would then write all of that out as if the user had
  // arranged it, which is how settings.ini ended up holding a test rig's
  // 720x540 window at x=-1680. With this latched, that save is skipped; nothing
  // a person did is lost, because every settings control persists the moment it
  // is changed rather than waiting for shutdown.
  bool    m_bTestingModeUsedThisSession = false;
  // Opt out of the write shield, for the rare test that needs its settings
  // changes to land on disk. Set by [Milkwave] TestingModeWritesSettings=1 or
  // by sending TESTING_MODE=1,persist.
  bool    m_bTestingModeWritesSettings = false;
  // Preset loads compile asynchronously, so "freeze auto preset changes" has a
  // hole: an AUTO-initiated load already compiling when the freeze lands (or
  // when the user hits preset lock) still applies seconds later and the frame
  // changes anyway -- observed live during an A/B session. The timer path tags
  // its loads via m_bNextLoadIsAuto; LoadPreset() captures the tag into
  // m_bLoadingInitiatedByAuto; LoadPresetTick() DISCARDS a finished auto load
  // if AutoPresetChangesAllowed() has gone false since it started. Explicit
  // loads (IPC, hotkeys, browser) are never tagged and always apply.
  bool    m_bNextLoadIsAuto = false;
  bool    m_bLoadingInitiatedByAuto = false;
  // WHY the next preset load happens, for the log. Set immediately before a
  // LoadPreset/LoadRandomPreset/NextPreset call; LoadPreset captures and
  // clears it. A string literal with static lifetime, never an owned buffer --
  // the load outlives the caller's frame.
  //
  // Separate from m_bNextLoadIsAuto rather than replacing it: that flag drives
  // the discard-a-finished-auto-load rule above and means one specific thing,
  // while this is free-form and exists only to be read by a human.
  const char* m_pszNextLoadReason = nullptr;
  bool    m_ShaderCaching = true;
  bool    m_ShaderPrecompileOnStartup = true;
  bool    m_CheckDirectXOnStartup = true;
  int     m_LogLevel = 1; // 0=Off, 1=Error, 2=Warn, 3=Info, 4=Verbose
  int     m_LogOutput = 3; // LOG_OUTPUT_BOTH (FILE|ODS), see utility.h
  bool    m_ShowLockSymbol = true;
  float   m_fAnimTime;
  float   m_fStartTime;
  float   m_fPresetStartTime;
  bool    m_bPresetDiagLogged = false;
  int     m_nDiagDisplayMode = 0;  // 0=normal, 1=show VS[0] raw, 2=show VS[1] raw
  float   m_fNextPresetTime;
  float   m_fSnapPoint;
  CState* m_pState;				// points to current CState
  CState* m_pOldState;			// points to previous CState
  CState* m_pNewState;			// points to the coming CState - we're not yet blending to it b/c we're still compiling the shaders for it!
  int     m_nLoadingPreset;
  wchar_t m_szLoadingPreset[MAX_PATH];
  // The file actually being imported, which differs from m_szLoadingPreset only
  // when a replacement was resolved for it (preset_replacements.h). Everything
  // that reads BYTES uses this; everything that names the preset to the user, or
  // records it, keeps using m_szLoadingPreset -- that is the preset the user
  // asked for, and ~60 readers of m_szCurrentPresetFile already mean exactly
  // that.
  wchar_t m_szLoadingResolved[MAX_PATH];
  // Consecutive presets that would not load, so skipping past a run of bad
  // files terminates instead of recursing through the whole directory. Reset
  // by the first load that succeeds. See OnPresetLoadFailed.
  int     m_nConsecutiveLoadFailures = 0;
  float   m_fLoadingPresetBlendTime;
  std::thread        m_presetLoadThread;      // background thread for async shader compilation
  std::atomic<bool>  m_bPresetLoadReady{false}; // set by bg thread when Import + shaders are done
  std::atomic<uint64_t> m_nLoadGeneration{0}; // incremented each load; bg thread checks before signaling
  // Shader compile errors recorded since this load began.  Zero at apply time
  // is what licenses ClearPresetShaderError to retract a stale error flag.
  std::atomic<int> m_nShaderErrorsThisLoad{0};
  float   m_fLoadStartTime = 0;              // GetTime() when async load began (for timeout)
  float   m_fShaderCompileTimeout = 8.0f;    // seconds before auto-skipping a stuck compilation
  bool    m_bLoadingShadertoyMode = false;    // true when async load is for a .milk3 Shadertoy preset
  bool    m_bLoadingMilk2 = false;            // true when async load is a .milk2 double-preset
  int     m_nPresetsLoadedTotal; //important for texture eviction age-tracking...
  CState	m_state_DO_NOT_USE[4];	// do not use; use pState and pOldState instead.
  CState* m_pMilk2OldState;      // 4th CState for .milk2 preset 1 (old/blend-from state)
  PShaderSet m_Milk2OldShaders;   // preset 1's shaders during .milk2 async load
  int     m_nMilk2MixType = -1;   // blend pattern from .milk2 metadata
  // Which of MD3's three plasma branches to run: 0 plasma, 1 plasma2,
  // 2 plasma3. They share GenPlasma and the corner seeds; see mixtype 2.
  int     m_nMilk2PlasmaVariant = 0;
  char    m_szMilk2Pattern[32] = {};  // raw blending_pattern name from the .milk2 header
  bool    m_bMilk2DeterministicField = false;  // suppress RandomizeBlendPattern's
                                               // process-wide time accumulators
  bool    m_bMilk2FrozenBlend = false;  // true when .milk2 blend is permanently frozen
  float   m_fMilk2FrozenProgress = 0.5f; // frozen blend progress from .milk2 metadata
  float   m_fMilk2Random[5] = {};       // blending random_1..5 from the .milk2 header
  bool    m_bMilk2HasRandoms = false;   // true when the file provided random_1..5
  bool    m_bMilk2UseSavedRandoms = false; // RandomizeBlendPattern should consume saved values
  int     m_nMilk2Direction = 1;        // blending_direction from the .milk2 header
  wchar_t m_szMilk2Temp1[MAX_PATH] = {};  // temp file for preset 1 (deleted after load)
  wchar_t m_szMilk2Temp2[MAX_PATH] = {};  // temp file for preset 2 (deleted after load)
  // MilkDrop 3.28 [SPRITEn] blocks (file-level, last index wins).
  struct Milk2SpriteDesc {
    int          nIndex = 1;
    wchar_t      szName[MAX_PATH] = {};
    unsigned int nColorKey = 0;
    int          nLayer = 0;
    int          nBlend = 0;
    float        fAlpha = 1.0f;
    float        fBurn = 1.0f;
    float        fX = 0.0f;
    float        fY = 0.0f;
    // MD3 defaults SpriteSX/SY to -0.5f (0xbf000000). Not adopted yet:
    // the fitted scale curve in milkdropfs.cpp was calibrated with 1.0f
    // here, so changing both at once confounds them.
    float        fSX = 1.0f;
    float        fSY = 1.0f;
    float        fRot = 0.0f;
    float        fSpeed = 0.0f;
    float        fRepeatX = 1.0f;
    float        fRepeatY = 1.0f;
    std::string  szInit;
    std::string  szCode;
  };
  std::vector<Milk2SpriteDesc> m_milk2Sprites;
  bool    m_bMilk2SpritesNeedApply = false;
  static const int MILK2_SPRITE_USERDATA = 0x4D4B3200; // 'MK2\0'
  void        KillMilk2Sprites();
  void        ApplyMilk2Sprites();
  bool        LaunchMilk2Sprite(const Milk2SpriteDesc& spr);
  ui_mode	m_UI_mode;				// can be UI_REGULAR, UI_LOAD, UI_SAVEHOW, or UI_SAVEAS

#define MASH_SLOTS 5
#define MASH_APPLY_DELAY_FRAMES 1
  int         m_nMashSlot;    //0..MASH_SLOTS-1
  int         m_nMashPreset[MASH_SLOTS];
  int         m_nLastMashChangeFrame[MASH_SLOTS];

  bool		m_bUserPagedUp;
  bool		m_bUserPagedDown;
  float		m_fMotionVectorsTempDx;
  float		m_fMotionVectorsTempDy;

  td_waitstr  m_waitstring;
  void		WaitString_NukeSelection();
  void		WaitString_Cut();
  void		WaitString_Copy();
  void		WaitString_Paste();
  void		WaitString_SeekLeftWord();
  void		WaitString_SeekRightWord();
  int			WaitString_GetCursorColumn();
  int			WaitString_GetLineLength();
  void		WaitString_SeekUpOneLine();
  void		WaitString_SeekDownOneLine();

  int			m_nPresets;			// the # of entries in the file listing.  Includes directories and then files, sorted alphabetically.
  int			m_nDirs;			// the # of presets that are actually directories.  Always between 0 and m_nPresets.
  int			m_nPresetFilter = 0;	// 0=all, 1=.milk only, 2=.milk2 only, 3=.milk3 only
  int			m_nSubdirMode = 0;		// 0=never include subdirs, 1=ask, 2=always include
  bool		m_bRecursivePresets = false;	// true when current list was built recursively
  std::wstring m_szTagFilter;		// if non-empty, only show presets with this tag
  std::wstring m_szActivePresetList;	// if non-empty, currently loaded preset list name
  int			m_nPresetListCurPos;// Index of the currently-HIGHLIGHTED preset (the user must press Enter on it to select it).
  int			m_nCurrentPreset;	// Index of the currently-RUNNING preset.
  //   Note that this is NOT the same as the currently-highlighted preset! (that's m_nPresetListCurPos)
  //   Be careful - this can be -1 if the user changed dir. & a new preset hasn't been loaded yet.
  // The current preset, EITHER as a bare filename OR as a full path -- which
  // one depends on how it was loaded, and both are normal (#12).
  //
  // The old comment here said "w/o path. this is always valid", which was
  // false: LoadPresetTick copies m_szLoadingPreset into it whole, and
  // engine_presets.cpp:3261 sniffs for a separator precisely because it may be
  // either ("when the preset came from the browser it is a bare name, so the
  // preset dir is prepended here rather than losing the location").
  //
  // Ask CurrentPresetLeaf() or CurrentPresetPath() rather than resolving it by
  // hand. There is no stored discriminant and none is needed: a Windows
  // filename cannot contain '' or '/', so the string always answers the
  // question itself. What was missing was never information -- it was one place
  // that does the resolution, instead of 30.
  wchar_t		m_szCurrentPresetFile[512];
  // The file whose bytes are on screen, when that is NOT m_szCurrentPresetFile.
  // Empty the rest of the time, which is the common case -- so "is a replacement
  // in effect" is a single test rather than a string compare.
  wchar_t		m_szRenderedPresetFile[MAX_PATH] = {0};
  wchar_t		m_szPendingStartupSave[512] = {};  // preset path waiting to be persisted after 5s render time
  float		m_fPendingStartupSaveTime = 0;     // GetTime() when preset was loaded (0 = no pending save)
  // ── The preset list (#11) ────────────────────────────────────────────────
  //
  // The render thread REPLACES the whole vector when a directory scan lands
  // (`m_presets = std::move(m_pendingPresets)` in engine.cpp). g_csPresetPending
  // guards the pending buffer, not this one. Meanwhile the Presets and Settings
  // ToolWindow threads read names out of it, and they used to hold a
  // `.c_str()` into an element across an LB_ADDSTRING -- a SendMessage, so the
  // list-fill pumps. That is the same shape that was measured killing the
  // process in the annotation half of #11, one thread over.
  //
  // recursive_mutex, matching the annotation store: several render-thread
  // operations compose (a sort that calls a comparator that reads the list, a
  // delete that renumbers). Every entry point still takes the lock once.
  //
  // ASK FOR A NAME, DO NOT REACH FOR THE VECTOR. PresetNameAt returns BY
  // VALUE, which is what makes it safe -- but that means
  //
  //     const wchar_t* p = PresetNameAt(i).c_str();   // DANGLES
  //
  // is wrong: the temporary dies at the semicolon. Inline in a bigger
  // expression is fine (the temporary lives to the end of the full
  // expression); to keep a name, keep the std::wstring.
  mdrop::Guarded<PresetList, std::recursive_mutex> m_presets;

  // One row's name, by value. Empty for an out-of-range index.
  std::wstring PresetNameAt(int idx) const;
  size_t       PresetCount() const;
  // Is this row a directory entry? Directories are stored as "*name" ("*.."
  // for the parent); this is the test 14 sites were spelling by hand.
  bool         PresetIsDirAt(int idx) const;

  // Pending preset data — scan thread writes, render thread swaps in
  PresetList              m_pendingPresets;
  int                     m_nPendingPresets = 0;
  int                     m_nPendingDirs = 0;
  int                     m_nPendingCurPos = 0;
  bool                    m_bPendingListReady = false;
  std::atomic<bool>       m_bPendingPresetSwap{false};

  // Pending ratings (pass 2)
  std::vector<float>      m_pendingRatings;
  int                     m_nPendingRatingsCount = 0;
  std::atomic<bool>       m_bPendingRatingsSwap{false};

  // ── The annotation store (presets.json), #11 ────────────────────────────
  //
  // The two maps and the dirty flag are ONE unit behind ONE lock, not three
  // members that happen to be locked. The hash index maps into the key map, so
  // an insert has to update both or the index points at an entry that is not
  // there yet; two locks would need an ordering rule, and an ordering rule
  // nobody can see is how this class got here in the first place.
  //
  // Reached by four threads: the IPC thread (RELOAD_ANNOTATIONS clears and
  // repopulates the whole thing), the Annotations ToolWindow thread (iterates,
  // and erases on Remove), the Presets ToolWindow thread (one lookup per row
  // while filling its list), and the render thread (TickPresetUsage). Before
  // the guard, a rehash or a clear() on any one of them freed the entry
  // another was reading through -- measured, not theorised: the probe at
  // private/tools/milk2-probe/test_annotations_thread_safety.py killed the
  // process with 0xC0000409 at iteration 261.
  //
  // recursive_mutex, for the same reason m_displayOutputsMutex is: the public
  // setters are reached both from a caller that already holds the guard and
  // from one that does not. Every entry point is still written to take the
  // lock exactly once and then call the *Locked helpers -- the recursion is a
  // safety net, not a licence to nest.
  //
  // NEVER call SavePresetAnnotations, a MessageBoxW, or a UI SendMessage from
  // inside a with(). Snapshot and act outside, the way SavePresetAnnotations
  // now does.
  struct AnnotStore {
    std::unordered_map<std::wstring, PresetAnnotation> byKey;   // bare filename -> entry
    std::unordered_map<std::wstring, std::wstring>     byHash;  // content hash -> byKey key
    bool dirty = false;
  };
  mdrop::Guarded<AnnotStore, std::recursive_mutex> m_annot;

  using AnnotFn      = std::function<void(PresetAnnotation&)>;
  using AnnotConstFn = std::function<void(const PresetAnnotation&)>;

  // Run fn against one entry with the guard held. Returns false, and does not
  // call fn, when there is no such entry and create is false. The distinction
  // matters to callers that fall back from a hash lookup to a filename lookup:
  // "no entry" and "an entry with nothing set" are different answers.
  bool WithAnnotation(const wchar_t* filename, bool create, const AnnotFn& fn);
  bool WithAnnotationByHash(const std::wstring& hash, const AnnotConstFn& fn);
  bool WithResolvedAnnotation(const wchar_t* filename, const wchar_t* hash,
                              const wchar_t* fullPath, bool create,
                              const AnnotFn& fn);

  // Serialises SAVERS, and the snapshot is taken inside it (#121).
  //
  // Distinct from m_annot, which guards the store. This guards the FILE: the
  // save is a whole-file rewrite, and two threads each rewriting presets.json
  // is how a database gets truncated. The dirty flag alone was not enough --
  // it makes the common case single-writer, but two threads that each mutate
  // and then each save both claim it and both write.
  //
  // LOCK ORDER: m_annotSaveMutex OUTSIDE m_annot. SavePresetAnnotations is
  // never called from inside an m_annot.with() -- that is the rule stated on
  // the guard itself, because the save writes a file -- so the reverse order
  // cannot arise. Taking the snapshot inside this lock is also what makes the
  // LAST saver's data the last written; snapshotting outside it allows an
  // older snapshot to win the race to the disk.
  std::mutex m_annotSaveMutex;

  void LoadPresetAnnotations();
  void SavePresetAnnotations();
  void SetPresetFlag(const wchar_t* filename, uint32_t flag, bool set);
  void SetPresetNote(const wchar_t* filename, const std::wstring& note);
  void SetPresetTags(const wchar_t* filename, const std::vector<std::wstring>& tags);

  // The per-preset override slots. `present` false REMOVES the member, so the
  // preset inherits from its tags again; `present` true with an empty name is
  // "explicitly none". Two different states, deliberately not one argument.
  void SetPresetShaderOverride(const wchar_t* filename, const std::wstring& name,
                               bool present);
  void SetPresetVFXProfile(const wchar_t* filename, const std::wstring& name,
                           bool present);
  // Returns whether the assignment BOUND. False means no annotation could be
  // created for that preset -- a scratch preset (testing mode plus an
  // AnnotationIgnoreDirs folder) binds no per-preset override at all. It used
  // to return void, so every caller assumed success and an IPC client that
  // asked for a profile got silence whether or not anything happened.
  bool SetPresetAudioProfile(const wchar_t* filename, const std::wstring& name,
                             bool present);
  // The reply for AUDIO_PROFILE= and AUDIO_PROFILE_CLEAR. Both used to answer
  // nothing at all, which is how a command that silently did nothing went
  // unnoticed for months (#88).
  void AnswerAudioProfile(const wchar_t* name, bool ok, bool known);
  // The file that runs in place of this preset. Keyed by FILENAME, because the
  // resolver runs before the file is opened and has no hash -- see the setter.
  void SetPresetReplacement(const wchar_t* filename, const std::wstring& path,
                            bool present);
  std::wstring PresetReplacementFor(const wchar_t* filename);
  int  ImportMWRTags(const wchar_t* szTagsJsonPath);  // returns count of presets updated
  void CollectAllTags(std::vector<std::wstring>& allTags) const;  // unique sorted list of all tags

  // Content-hash identity (preset_hash.h).  Hash is the primary key; filename
  // is the fallback, and the two cover each other: the hash survives moving and
  // renaming, the filename survives the file being edited.
  // The index itself is AnnotStore::byHash, declared with the key map above --
  // the two are one unit behind one guard and cannot be locked separately.
  //
  // The hash index is rebuilt only from inside the guard now; there is no
  // public form. GetAnnotation, GetAnnotationByHash, ResolveAnnotation and
  // GetAnnotationForPreset are GONE (#11) -- they returned a pointer into the
  // store, which is the exact escape Guarded<T> exists to forbid. Use
  // WithAnnotation / WithAnnotationByHash / WithResolvedAnnotation.

  // Locked internals. Every one of these takes a reference the caller obtained
  // from m_annot.with(), so the pointers they return cannot outlive the lock.
  PresetAnnotation* FindLocked(AnnotStore& s, const wchar_t* filename, bool create);
  PresetAnnotation* FindByHashLocked(AnnotStore& s, const std::wstring& hash);
  PresetAnnotation* ResolveLocked(AnnotStore& s, const wchar_t* filename,
                                  const wchar_t* hash, const wchar_t* fullPath,
                                  bool create);
  PresetAnnotation* ForPresetLocked(AnnotStore& s, const wchar_t* filename,
                                    CState* pState, bool create);
  void RebuildHashIndexLocked(AnnotStore& s);
  // Merges "from" into "into" without discarding anything (see the design spec
  // section 3.4); notes/errorText are the only fields where one text wins.
  static void MergeAnnotations(PresetAnnotation& into, const PresetAnnotation& from);

  // ── Keeping test runs out of presets.json ──
  //
  // A harness run loads hundreds of throwaway presets, and every one of them
  // that stayed on screen past the usage threshold used to mint an entry.  The
  // database then describes the test rig rather than the user's library.
  //
  // Two independent gates, because they answer different questions:
  //   * testing mode  -- "is a measurement running right now"
  //   * ignored dirs  -- "is this file a scratch preset regardless of mode"
  // Both only ever block CREATION and writes.  An entry that already exists
  // still resolves and still reads, so a test run never hides real data.
  //
  // Folder names, matched against any single path segment, case-insensitively.
  // Semicolon-separated in the INI so the TEST/ convention is configurable
  // rather than a magic string compiled in.
  std::vector<std::wstring> m_annotIgnoreDirs;
  // The raw INI string, kept so it can be written back unchanged.
  wchar_t m_szAnnotIgnoreDirs[512] = L"TEST";
  void ParseAnnotIgnoreDirs(const wchar_t* semicolonList);
  bool IsAnnotationIgnoredPath(const wchar_t* fullPath) const;
  // Testing mode + an ignored directory = scratch: no identity, so no tags,
  // no rating and no per-preset overrides. Counterpart to
  // ShouldSkipAnnotationWrite, which gates writes.
  bool IsScratchPreset(const wchar_t* fullPath) const;
  // The one predicate both gates funnel through. fullPath may be NULL/empty,
  // in which case only the testing-mode gate can fire.
  bool ShouldSkipAnnotationWrite(const wchar_t* fullPath) const;

  // ── Duplicate detection ──
  //
  // One group per content hash that has more than one file on disk.  Built by
  // an explicit scan, never implicitly: it reads every preset file under a
  // root, which is far too expensive to do on a timer.
  struct DuplicateFile {
    std::wstring path;
    uint64_t     sizeBytes = 0;
    FILETIME     written{};      // last-write time, for "keep the newest"
  };
  struct DuplicateGroup {
    std::wstring hash;
    std::wstring displayName;    // basename of the first file found
    std::vector<DuplicateFile> files;
  };
  // Progress callback; return false to cancel.
  //
  // A scan has two phases and they need telling apart. While the tree is being
  // walked, `total` is 0 and `done` is the number of preset files found so far;
  // once hashing starts, `total` is the final count and `done` counts up to it.
  // Without the distinction the walk could not be cancelled at all -- and on a
  // deep or networked tree the walk is the slow half.
  using DupeScanProgressFn = std::function<bool(int done, int total, const wchar_t* current)>;
  // Returns every group with 2+ files, largest group first. `root` is walked
  // recursively. Groups are found by CONTENT, so a renamed copy still matches.
  //
  // const, and it touches no member: it is meant to run on a worker thread
  // while the UI thread keeps painting. Handing the result back through
  // AdoptDuplicateScan keeps every WRITE to the caches on one thread, so the
  // two never need a lock between them.
  std::vector<DuplicateGroup> ScanForDuplicatePresets(const wchar_t* root,
                                                      const DupeScanProgressFn& onProgress,
                                                      std::set<std::wstring>* outAllHashes) const;
  // Publish a finished scan into the session caches. Call on the thread that
  // reads them (the Annotations window), never from the worker.
  void AdoptDuplicateScan(const std::vector<DuplicateGroup>& groups,
                          std::set<std::wstring>&& allHashes);
  // Session cache of the last scan, so the list's Copies column and the details
  // dialog can both read it without rescanning. Cleared on demand, never saved:
  // it describes the disk at one moment, and a stale count is worse than none.
  std::unordered_map<std::wstring, std::vector<std::wstring>> m_dupeIndex;  // hash -> paths
  // Every hash the scan actually looked at, duplicated or not.  Without it a
  // hash missing from m_dupeIndex is ambiguous -- "scanned, exactly one file"
  // and "lives outside the folder that was scanned" would both read as one
  // copy, and the second is a claim the app has not earned.
  std::set<std::wstring> m_dupeScannedHashes;
  bool m_bDupeScanRun = false;   // distinguishes "no copies" from "never looked"
  // How many files on disk share this annotation's content.
  // 0 = unknown (no scan, or this preset was not under the scanned root).
  int  DuplicateCountFor(const PresetAnnotation& a) const;
  // Average of the rating observations, rounded; 0 when there are none.
  static int  AverageRating(const PresetAnnotation& a);
  // The rating to display: MDX12 average when observations exist, else the
  // preset file's own fRating, else 0.
  int  EffectiveRating(const wchar_t* filename, float fFileRating) const;
  void SetPresetRatingMDX(const wchar_t* filename, int value);
  void SetPresetRatingForFile(const wchar_t* filename, int value);
  static void AdoptHashIntoLegacyRatings(PresetAnnotation& a, const std::wstring& hash);
  void ResetUsageStats(const wchar_t* filenameOrNull);

  // ── Shader overrides (shader_overrides.h) ──
  //
  // The override's text is held HERE and never written into m_pState.
  // CState::Export writes m_szWarpShadersText / m_szCompShadersText straight
  // back into the .milk, so staging an override in state would silently bake
  // someone else's shader into the user's preset file the next time they saved
  // it.  Keeping state pristine makes that impossible rather than unlikely.
  // Where a resolved selection came from. Reported over IPC per slot, because
  // "the tag rule worked" and "the per-preset entry worked" are the two things
  // this feature is made of and the result alone cannot tell them apart.
  enum class OverrideSource { None, Rule, Preset };

  // ── Audio profile (audio_profile_store.h) ──
  // m_audioProfile is written ONLY by ApplyPendingAudioProfile on the render
  // thread and read by the three per-frame consumers on that same thread.
  // Resolution runs on the preset-load thread and communicates by name.
  AudioProfile        m_audioProfile;
  wchar_t             m_szPendingAudioProfile[128] = L"MDropDX12";
  std::atomic<bool>   m_bAudioProfilePending{ false };
  std::wstring        m_resolvedAudioProfile;
  OverrideSource      m_resolvedAudioSource = OverrideSource::None;
  wchar_t             m_szDefaultAudioProfile[128] = L"MDropDX12";

  struct ActiveShaderOverride {
    std::wstring name;
    std::string  warpText, compText;   // empty slot = keep the preset's own
    bool fromRule = false;             // false when applied by hand
    bool warpFailed = false, compFailed = false;
    std::wstring matchedTag;
    OverrideSource source = OverrideSource::None;
    bool IsActive() const { return !name.empty(); }
    void Clear() { *this = ActiveShaderOverride(); }
  };
  ActiveShaderOverride m_activeOverride;

  // The VFX profile this preset resolved to, and from where. Resolved
  // independently of the shader slot: a preset may take its shader from a
  // generic tag rule while naming its own profile.
  std::wstring   m_resolvedVFXProfile;
  OverrideSource m_resolvedVFXSource = OverrideSource::None;

  void ResolveShaderOverrideForPreset(CState* pState);
  void ResolveAudioProfileForPreset(CState* pState);
  void ApplyPendingAudioProfile();
  bool ApplyOverrideToCurrentPreset(const std::wstring& name);  // ad hoc, no rule
  void RevertOverrideOnCurrentPreset();
  void RequestShaderRecompile();       // posts the render-thread recompile
  // True only for the state being rendered or loaded. Shader precompilation
  // (CompilePresetShadersToFile) builds unrelated presets and must compile
  // exactly what those files contain.
  bool OverrideAppliesTo(const CState* pState) const {
    return m_activeOverride.IsActive() &&
           (pState == m_pState || pState == m_pNewState);
  }

  // Usage tracking.  A preset counts as played once it has been on screen for
  // kUsageCountThresholdSec, reusing the threshold the startup-preset save
  // already applies: cycling through forty presets looking for one should not
  // log forty plays.  Seconds accumulate from load, including those first five,
  // but only for a preset that crossed the threshold.
  static constexpr float kUsageCountThresholdSec = 5.0f;
  float   m_fPresetUsageStart = 0;      // GetTime() when the running preset loaded
  bool    m_bPresetUsageCounted = false;
  // Set when the usage tick decided this preset must not be recorded (testing
  // mode, or an ignored directory).  Separate from m_bPresetUsageCounted, which
  // the tick also sets in that case purely to stop re-testing every frame --
  // and which FlushPresetUsage reads as "this play was counted, bank its
  // seconds".  Without this second flag the skip would suppress the play count
  // and then add the time anyway.
  bool    m_bPresetUsageSuppressed = false;
  wchar_t m_szUsagePresetFile[512] = {}; // full path of the preset being timed
  void TickPresetUsage();                // once per frame
  // Drop recorded alias paths that hash to a different preset than the
  // entry holding them. Returns how many were removed.
  int  RepairAnnotationPaths(int* pChecked = nullptr, int* pUnjudged = nullptr);
  void FlushPresetUsage();               // on preset change and at shutdown
  void BeginPresetUsage(const wchar_t* fullPath);

  // Preset lists: save/load named subsets of presets
  bool SavePresetList(const wchar_t* listName);  // saves current preset list to file
  bool LoadPresetList(const wchar_t* listPath);   // loads a preset list from file
  void GetPresetListDir(wchar_t* szDir, int nMax) const;  // preset_lists/ dir
  void EnumPresetLists(std::vector<std::wstring>& names) const;  // list available .txt files
  void AutoFlagPresetError(const wchar_t* filename, const std::wstring& errorMsg,
                           PresetErrorKind kind = PresetErrorKind::Shader);

  // A preset could not be loaded at all -- tell the user, record it, move on.
  //
  // The failure paths used to clear their own flags and `return`, which reads
  // as tidy and is not: no error reached the user, no other preset was tried,
  // and a Next press over one corrupt file simply vanished. Shane drives Next
  // from a gamepad, so a dead press reads as the app ignoring him
  // (forgejo#103).
  //
  // This is the same answer App.cpp's SEH recovery already gives when a preset
  // CRASHES the renderer -- AddError, AutoFlagPresetError, advance, and stop
  // after too many -- reached by a different route.
  void OnPresetLoadFailed(const wchar_t* filename, const wchar_t* why);
  // Retract a shader error the preset has outgrown -- see the definition.
  void ClearPresetShaderError(const wchar_t* filename);
  // Import: parse annotations from an arbitrary presets.json file
  static std::unordered_map<std::wstring, PresetAnnotation> ParseAnnotationsFile(const wchar_t* path);
  // Scan loaded presets and build a map from fRatingThis (non-default ratings only)
  std::unordered_map<std::wstring, PresetAnnotation> ScanPresetsForRatings();

  void		UpdatePresetList(bool bBackground = false, bool bForce = false, bool bTryReselectCurrentPreset = true);
  wchar_t     m_szUpdatePresetMask[MAX_PATH];
  bool        m_bPresetListReady;
  //void		UpdatePresetRatings();
    //int         m_nRatingReadProgress;  // equals 'm_nPresets' if all ratings are read in & ready to go; -1 if uninitialized; otherwise, it's still reading them in, and range is: [0 .. m_nPresets-1]
  bool        m_bInitialPresetSelected;

  // PRESET HISTORY
#define PRESET_HIST_LEN (64+2)     // make this 2 more than the # you REALLY want to be able to go back.
  std::wstring m_presetHistory[PRESET_HIST_LEN];   //circular
  int m_presetHistoryPos;
  int m_presetHistoryBackFence;
  int m_presetHistoryFwdFence;
  void BuildPresetPath(int idx, wchar_t* szOut, int nMax) const;  // absolute path from m_presets[idx]
  void PrevPreset(float fBlendTime);
  void PrevPresetLocal(float fBlendTime);   // this instance only, no child broadcast
  void NextPreset(float fBlendTime);  // if not retracing our former steps, it will choose a random one.
  void OnFinishedLoadingPreset();
  // ── Audio mixer (audio_mixer.*, engine_mixer.cpp) ──
  // Off by default and inert in a child instance. The engine only ever talks
  // to the mixer through these; nothing else includes audio_mixer.h, which is
  // what keeps that module free of engine types.
  void MixerInit();
  void MixerShutdown();
  bool MixerIsEnabled() const;
  bool MixerSetEnabled(bool enabled);
  void MixerSetSubscribed(bool subscribed);
  // One record per line. Sent as separate IPC messages, because a single
  // blob of every channel, route and device overruns the readers.
  std::vector<std::wstring> MixerStateLines();
  bool MixerHandleCommand(const wchar_t* message, std::wstring& reply);
  // Chain-friendly wrapper: handles the verb and sends any reply itself,
  // so the dispatcher needs no local of its own.
  bool MixerHandleAndReply(const wchar_t* message);
  // Confirmation policy, per surface: 0 hotkey, 1 tool window, 2 remote.
  // Local surfaces default to off -- a bound key and a button in your own
  // window are both things you reached deliberately. The remote default is on
  // and cannot be relaxed until a PIN is configured.
  bool MixerConfirmRequired(int surface) const;
  // Opens a confirm window. Returns false when none is wanted, in which case
  // the caller acts immediately.
  bool MixerConfirmBegin(const std::wstring& what);
  // Answers a pending window. True means the action should proceed.
  bool MixerConfirmAnswer(bool yes);
  bool MixerConfirmPending() const;
  // Driven from the mixer worker's tick. Expires an unanswered window, and
  // fails CLOSED: the action is dropped, never performed late.
  void MixerConfirmTick();

  void MixerLoadFailoverConfig();

  // ── The headset battery on the HUD ──
  // The percentage the HUD draws, 0..100, or -1 when there is none to show.
  // A cached integer rather than a lookup: this is read once per frame on the
  // render thread, and the endpoint list behind it is neither cheap to walk
  // nor safe to walk from there.
  int  MixerBatteryPercent() const;
  // Re-pick the device and re-read its charge NOW. Called from the mixer
  // worker's tick and from the MIXER_BATTERY verb, never from the render
  // thread -- it reads Bluetooth device nodes.
  void MixerBatteryRecompute();
  // The slow poll behind the figure, run from the mixer worker's tick. Does
  // nothing unless the HUD line is switched on, and no more than once every
  // thirty seconds -- a charge does not move faster than that, and the mixer
  // window's own one-second tick supersedes it whenever that window is up.
  void MixerBatteryTick();
  // The one place the HUD toggle is written, whichever surface asked: the
  // hotkey, the Settings checkbox or the in-app settings list. Switching it on
  // refreshes the figure at once and says so when the mixer subsystem is off,
  // because a toggle that lights nothing up otherwise reads as a fault.
  void SetShowBattery(bool show);

  // Hotkey groups. One key moves every fader ticked into a group, which is
  // what the volume hotkeys were asked for and what the two "personal" and
  // "streaming" slots they replace never did.
  float MixerVolumeStep() const;
  int  MixerGroupCount() const;
  // The "<channel>|<fader>" keys in a group, empty when the index is out of
  // range or nothing is ticked.
  std::vector<std::wstring> MixerGroupMembers(int group) const;
  bool MixerSetGroupMembers(int group,
                            const std::vector<std::wstring>& members);
  // One line per group: its name, how many faders it holds, and how many of
  // those are present right now. The two counts differ exactly when a member
  // belongs to hardware that is switched off, which is worth being able to
  // see rather than inferring from a press that does less than expected.
  //
  // `extra` is appended as-is before the key list. It exists because the keys
  // MUST come last: a key is "<channel>|<fader>" and the pipe is also the
  // record separator, so anything after them would be swallowed by whatever
  // splits the line. MIXER_ORDER_SET puts its keys last for the same reason.
  std::wstring MixerGroupLine(int group,
                              const std::wstring& extra = std::wstring()) const;
  // Moves every present member by delta, clamped 0..1 INDIVIDUALLY so the
  // offsets between members survive a press at either rail. Returns how many
  // faders moved.
  int MixerNudgeGroup(int group, float delta);
  // One decision for the whole group rather than N independent toggles: any
  // member unmuted means mute them all, otherwise unmute them all. Returns
  // how many faders changed.
  int MixerToggleGroupMute(int group);
  // The hotkey entry point: acts, then puts the result on screen, because a
  // keystroke has no reply channel to fail into.
  void MixerGroupHotkey(int group, float delta, bool toggleMute);

  // The display order of the faders, as "channel|fader" keys. Shane asked to
  // "put controls I use most at top and those I don't use can be at bottom":
  // with two dozen endpoints and room for ten, the order decides what is
  // reachable. Held here rather than in the window so the window, the IPC
  // verbs and the phone all read one list.
  // Mixer profiles: resources/profiles/mixer/<name>.json, one file each.
  // A profile carries the levels AND the arrangement; the wider mixer
  // configuration goes in only when allSettings is asked for, so the common
  // file stays small. Load reports how many faders it applied and how many it
  // skipped for a device that is not here -- absent hardware is never an error.
  std::wstring MixerProfileDir() const;
  std::vector<std::wstring> ListMixerProfiles() const;
  std::wstring MixerProfileName(const std::wstring& leaf) const;
  std::wstring SaveMixerProfile(const std::wstring& displayName, bool allSettings);
  bool LoadMixerProfile(const std::wstring& nameOrPath,
                        int* pApplied = nullptr, int* pSkipped = nullptr);
  bool DeleteMixerProfile(const std::wstring& nameOrPath);

  std::vector<std::wstring> MixerFaderOrder();
  void MixerSetFaderOrder(const std::vector<std::wstring>& order);

  // ── The stored order, and the list that is actually drawn ──
  //
  // These are two different lists, and conflating them is forgejo#65.
  // MixerFaderOrder() is the STORED arrangement: what a move edits and what
  // mixer.json holds. On top of it the Mixer tab applies three view rules --
  // sortUnmutedFirst, pinFailoverDevices, and hiding provider-owned endpoints
  // -- so what a person is looking at is not what MIXER_ORDER reports.
  //
  // The rules live HERE rather than in the window, for two reasons. A rule
  // applied while drawing can only be asked about while the window is OPEN,
  // and a remote needs the answer either way; and a second copy of the rules
  // in the window is precisely how the two came to disagree in the first
  // place. engine_mixer_ui.cpp orders its rows by this, so the window and a
  // phone cannot hold different opinions about what the list looks like.
  struct MixerViewRow {
    std::wstring key;      // "<channel>|<fader>"
    int  order = -1;       // its index in MixerFaderOrder()/MIXER_ORDER space
    int  pos = -1;         // its drawn row, or -1 when hidden
    // Not drawn, for ANY reason -- the virtual-endpoint filter or the user's
    // own hidden flag. It has always meant "not drawn" and still does.
    bool hidden = false;
    // ...and this says the reason was the user's stored preference, which is a
    // different switch from showVirtualEndpoints and needs a different offer
    // to reveal it (forgejo#50).
    bool userHidden = false;
    // Lifted to the top by pinFailoverDevices. Reported per row because a
    // client cannot work it out for itself: it needs every route's full
    // allowlist, and MIXER_ROUTE carries only the route's current device.
    bool pinned = false;
  };
  // Only faders that EXIST are rows. The stored order keeps a place for a
  // device that is merely switched off (MergeAbsentFaderOrder) and the window
  // does not draw one, so those are absent here too -- but `order` still
  // points into the full stored list, which is what keeps a move addressable.
  std::vector<MixerViewRow> MixerViewRows();
  // A short token identifying the arrangement a client is looking at, so a
  // move can be refused when the list moved underneath it. Covers the
  // addressable order, the drawn sequence and the three view flags -- see
  // MixerOrderRev() in engine_mixer.cpp for why each is in there.
  std::wstring MixerOrderRev();
  // Tell subscribers the arrangement moved, if it did.
  //
  // The list reorders without anyone asking it to: a failover device
  // connecting is lifted to the top by the app itself, and a remote that only
  // refetches on demand goes on drawing the old order until someone pulls to
  // refresh. Cheap to call and idempotent -- it compares the token first and
  // says nothing when it has not changed.
  void MixerNotifyViewChanged();
  void OpenAudioMixerWindow();
  void CloseAudioMixerWindow();
  bool IsAudioMixerWindowOpen() const;

  // Checks a PIN submitted by a TCP client against [Network] PinHash, and
  // upgrades a legacy plaintext value to a salted hash on the first successful
  // check. True when no PIN is configured, so an unconfigured remote behaves
  // exactly as it did before verification existed. Both auth handlers call
  // this rather than repeating it -- see forgejo#44.
  bool VerifyRemotePin(const std::string& submittedUtf8);

  int SendMessageToMDropDX12Remote(const wchar_t* presetFile);
  int SendMessageToMDropDX12Remote(const wchar_t* presetFile, bool doForce);
  void PostMessageToMDropDX12Remote(UINT msg);

#define WM_USER_NEXT_PRESET WM_USER + 100
#define WM_USER_PREV_PRESET WM_USER + 101
#define WM_USER_COVER_CHANGED WM_USER + 102
#define WM_USER_SPRITE_MODE WM_USER + 103
#define WM_USER_MESSAGE_MODE WM_USER + 104

  FFT            myfft;
  FFT            m_fftShader;  // separate clean FFT for shader texture — no equalization, Hann³ window
  td_mysounddata mysound;

  // stuff for displaying text to user:
  bool		m_bShowFPS;
  bool		m_bShowBattery;   // headset charge, drawn under the FPS line
  bool		m_bShowRating;
  bool		m_bShowPresetInfo;
  bool		m_bShowDebugInfo;

  // Per-process GPU utilisation, for the HUD and DIAG_GPU. Sampled only while
  // something has asked for it -- the debug overlay being up, or a DIAG_GPU
  // within the last few seconds -- because each poll walks every GPU engine
  // instance on the machine. See gpu_usage.h.
  //
  // Was deliberately NOT gated on child mode, unlike the subsystems that own an
  // external interface: a child measuring its OWN pid was the entire point when
  // the question was what a parent plus N children cost. One process now.
  mdrop::GpuUsageSampler m_gpuUsage;
  ULONGLONG              m_gpuWantedUntilTick = 0;   // set by DIAG_GPU
  bool GpuUsageWanted() const {
    return m_bShowDebugInfo || GetTickCount64() < m_gpuWantedUntilTick;
  }
  bool		m_bShowSongTitle;
  bool		m_bShowSongTime;
  bool		m_bShowSongLen;
  float		m_fShowRatingUntilThisTime;

#define ERR_ALL    0
#define ERR_INIT   1  //specifically, loading a preset
#define ERR_PRESET 2  //specifically, loading a preset
#define ERR_MISC   3
#define ERR_NOTIFY 4  // a simple notification - not an error at all. ("shuffle is now ON." etc.)
  // NOTE: each NOTIFY msg clears all the old NOTIFY messages!
#define ERR_SCANNING_PRESETS 5
#define ERR_MSG_BOTTOM_EXTRA_1 6
#define ERR_MSG_BOTTOM_EXTRA_2 7
#define ERR_MSG_BOTTOM_EXTRA_3 8

  // Appended from 77 AddError/AddNotification call sites, on every ToolWindow
  // thread, and read by the render thread while it draws the overlay. EVERY
  // access goes through m_errorsMutex.
  //
  // The render thread must never hold a reference or a c_str() into an element
  // across anything that can append -- a push_back that reallocates frees the
  // buffer it is reading. It therefore snapshots under the lock and draws from
  // the copy, which also keeps the blocking remote-send and the text draw off
  // the lock. See forgejo#9.
  // Requested warp mesh size. RenderCmd::SetMeshSize carries no payload and
  // means "adopt whatever is here", which makes it idempotent and therefore
  // safe to coalesce: a slider drag queues ~23 of them and only the first one
  // drained does any work.
  //
  // Carrying the size IN the command instead made the render thread replay
  // every intermediate size, each a full ResetBufferAndFonts -- WaitForGpu,
  // CleanUpDX9Stuff, AllocateDX9Stuff, font atlas rebuild. Measured on a
  // 192 -> 16 drag: 11.9s to recover, against 1.6s for the same change made in
  // one step. That is the black screen in forgejo#8's follow-up.
  //
  // The code this replaced coalesced by accident, because the UI thread wrote
  // m_nGridX directly and every queued reset then saw the final value. That is
  // the race forgejo#8 fixed; the coalescing was worth keeping.
  std::atomic<int> m_reqGridX{ 0 };
  std::atomic<int> m_reqGridY{ 0 };

  ErrorMsgList m_errors;
  std::mutex   m_errorsMutex;
  uint64_t     m_nextErrorId = 1;   // guarded by m_errorsMutex
  void ClearErrorsLocked(int category);   // caller holds m_errorsMutex
  void SetFPSCap(int fps);

  // Script engine
  ScriptState m_script;
  void UpdateScript() override;
  void LoadScript(const wchar_t* path);
  void StartScript();
  void StopScript();
  void ExecuteScriptLine(int lineIndex);
  // bStrict: a token nothing recognises is reported to the caller instead of
  // being drawn on the visualizer as message text (issue 102).
  void ExecuteScriptLine(const wchar_t* text, bool bStrict = false); // pipe-split + execute
  void ExecuteScriptCommand(const std::wstring& cmd, bool bStrict = false);
  void SyncScriptUI();

  // True when nothing may change the preset on its own. Testing mode aside,
  // this is the condition that was copy-pasted at eight hard-cut sites.
  // The Preset Editor also holds it down: an auto-advance while someone is
  // typing into the live preset would throw their unsaved edits away.
  bool AutoPresetChangesAllowed() const {
    return !m_bPresetLockedByUser && !m_bPresetLockedByCode &&
           !m_bSuppressAutoPresetChanges &&
           !m_bPresetEditorOpen.load(std::memory_order_relaxed);
  }
  // Returns true if the key was swallowed by testing mode.
  bool TestingModeHandleKey(unsigned int vk);
  void SetTestingMode(bool bOn);

  void AddNotification(wchar_t* szMsg);
  void AddNotificationAudioDevice();
  void AddNotification(wchar_t* szMsg, float time);
  void AddNotificationColored(wchar_t* szMsg, float time, DWORD color);
  void AddError(wchar_t* szMsg, float fDuration, int category = ERR_ALL, bool bBold = true);
  void ClearErrors(int category = ERR_ALL);  // 0=all categories

  void GetSongTitle(wchar_t* szSongTitle, int nSize);

  //musik::core::sdk::IPlaybackService* playbackService;
  std::string emulatedWinampSongTitle;
  char		m_szDebugMessage[512];
  wchar_t		m_szSongTitle[512];
  wchar_t		m_szSongTitlePrev[512];

  // stuff for menu system:
  CMilkMenu* m_pCurMenu;	// should always be valid!
  CMilkMenu	 m_menuPreset;
  CMilkMenu	  m_menuWave;
  CMilkMenu	  m_menuAugment;
  CMilkMenu	  m_menuCustomWave;
  CMilkMenu	  m_menuCustomShape;
  CMilkMenu	  m_menuMotion;
  CMilkMenu	  m_menuPost;
  CMilkMenu    m_menuWavecode[MAX_CUSTOM_WAVES];
  CMilkMenu    m_menuShapecode[MAX_CUSTOM_SHAPES];
  bool         m_bShowShaderHelp;

  wchar_t		m_szMilkdrop2Path[MAX_PATH];		// ends in a backslash
  wchar_t		m_szMsgIniFile[MAX_PATH];
  wchar_t     m_szImgIniFile[MAX_PATH];
  wchar_t		m_szPresetDir[MAX_PATH];
  wchar_t     m_szPresetStartup[MAX_PATH];
  wchar_t     m_szAudioDevicePrevious[MAX_PATH];
  wchar_t     m_szAudioDevice[MAX_PATH];
  wchar_t     m_szAudioDeviceDisplayName[MAX_PATH];
  wchar_t     m_SongInfoFormat[MAX_PATH];
  wchar_t     m_szWindowTitle[256];         // configurable window title (empty = "MDropDX12 Visualizer")
  // The title CreateWindowAndRun actually resolved and gave the render window --
  // m_szWindowTitle or the default, the instance-numbering suffix, the [TEST]
  // tag -- so anything that later needs to restore the caption (forgejo#60:
  // OnFinishedLoadingPreset used to hardcode "MDropDX12" here) puts back what
  // the window really has rather than a literal that matches none of that.
  wchar_t     m_szActualWindowTitle[256] = {};

  wchar_t     m_szRemoteWindowTitle[256];   // configurable remote title (empty = "MDropDX12 Remote")
  wchar_t     m_szLastRemoteExePath[MAX_PATH] = {};  // last pipe-connected Remote exe path (for launch)
  int m_nSettingsCurSel = 0;       // currently highlighted setting in UI_SETTINGS
  bool m_bSettingsNeedAttention = false; // force settings open on bad config
  int m_nAudioLoopState = 0; // 0: Running, 1: Cancel running thread, 2: Must restart
  int m_nAudioDeviceRequestType = 0; // 0: Undefined, 1: Capture (in), 2: Render (out)
  int m_nAudioDeviceActiveType = 2;   // 0: Unknown, 1: Capture (in), 2: Render (out)
  int m_nAudioDevicePreviousType = 2;
  float		m_fRandStart[4];

  // DIRECTX 9 (legacy — kept for compilation; always nullptr at runtime):
#define NUM_BLUR_TEX 6
#if (NUM_BLUR_TEX>0)
  IDirect3DTexture9* m_lpBlur[NUM_BLUR_TEX]; // each is successively 1/2 size of prev.
  int               m_nBlurTexW[NUM_BLUR_TEX];
  int               m_nBlurTexH[NUM_BLUR_TEX];
#endif
  int m_nHighestBlurTexUsedThisFrame;

#define NUM_SUPERTEXTS 10
  td_supertext m_supertexts[NUM_SUPERTEXTS];

  // DX12 render targets (Phase 2)
  DX12Texture m_dx12VS[2];                    // double-buffered visualizer canvas
  DX12Texture m_dx12Blur[NUM_BLUR_TEX];       // blur pyramid (6 levels)
  DX12Texture m_dx12Title[NUM_SUPERTEXTS];    // title overlays
  ComPtr<ID3D12Resource> m_dx12TitleUploadBuf[NUM_SUPERTEXTS]; // per-slot upload buffers (avoids cross-slot corruption)
  HDC         m_titleDC = nullptr;             // GDI memory DC for title text rendering
  HBITMAP     m_titleDIB = nullptr;            // DIB section for title text
  BYTE*       m_titleDIBBits = nullptr;        // pixel data pointer

  // DX12 preset PSOs (Phase 5)
  ComPtr<ID3D12PipelineState> m_dx12WarpPSO;         // current preset warp
  ComPtr<ID3D12PipelineState> m_dx12CompPSO;         // current preset comp
  // Same comp/Image shader, built for a FLOAT32 render target. The .milk3 Image
  // pass draws into m_dx12ImageFeedback (FLOAT32) whenever the preset samples
  // its own previous Image ("self"), and a PSO whose RTVFormats[0] is the UNORM
  // backbuffer format may not be bound to that target — mismatched formats are
  // invalid D3D12 and, with no debug layer, corrupt or hang instead of erroring.
  ComPtr<ID3D12PipelineState> m_dx12CompFloatPSO;    // comp built for FLOAT32 RT
  ComPtr<ID3D12PipelineState> m_dx12FallbackWarpPSO; // default warp_ps.fx
  ComPtr<ID3D12PipelineState> m_dx12FallbackCompPSO; // default comp_ps.fx
  ComPtr<ID3D12PipelineState> m_dx12OldWarpPSO;      // previous preset warp (blend pass 0, no alpha)
  ComPtr<ID3D12PipelineState> m_dx12WarpBlendPSO;    // current preset warp (blend pass 1, alpha blend)
  ComPtr<ID3D12PipelineState> m_dx12OldCompPSO;      // previous preset comp (blend pass 0, no alpha)
  ComPtr<ID3D12PipelineState> m_dx12CompBlendPSO;    // current preset comp (blend pass 1, alpha blend)
  ComPtr<ID3D12PipelineState> m_dx12BlurPSO[2];      // [0] = horiz (blur1), [1] = vert (blur2)
  DX12Texture m_injectEffectTex;                     // back-buffer-sized copy for F11 inject post-process
  ComPtr<ID3D12PipelineState> m_pInjectEffectPSO;    // inject effect pixel shader PSO
  void RenderInjectEffect();                         // F11 inject effect post-process pass
  // Feedback-canvas ceiling (long edge, px; 0 = no limit). A per-preset
  // canvasMax may only reduce below this -- see EffectiveCanvasLimit.
  int m_nGlobalCanvasMax = 0;

  // min(global, per-preset). Never max(): a preset can only step the canvas
  // down, never raise it above the global ceiling.
  // Long-edge cap for the feedback canvas, resolved content-hash first.
  // Pass the incoming preset's hash when calling mid-apply, before the
  // state swap -- see the definition.
  int EffectiveCanvasLimit(const char* szHashOverride = nullptr) const;

  // The canvas long edge these presets were balanced at, and the pivot for the
  // damp mitigation. Below it the mitigation is inert, at it the multiplier is
  // exactly 1.0, above it the multiplier tracks how much per-frame transport
  // the bigger canvas has taken away. 1024 is where the measured sweep found
  // the runaway presets still stable (0.52 MP stable / 8.29 MP runaway).
  static const int kDampReferenceEdge = 1024;

  // What strength 1.0 on the dial means: remove at most this fraction of the
  // feedback per frame, in the limit of an infinitely large canvas. A range
  // for a user control, not a correction fitted to a preset -- the curve's
  // shape comes from the canvas, see EffectiveFeedbackDamp.
  //
  // 0.15 because it is the smallest value that puts BOTH multipliers ever
  // measured to work inside the dial, at opposite ends of it. At a 3840 px long
  // edge (lost = 0.7333):
  //
  //     0.97, which converged "Flexi - jellyfish jam"  -> strength 0.27, Gentle
  //     0.90, which collapsed "suksma - tetraxectsual"  -> strength 0.91, Maximum
  //
  // At 0.10 the second of those needed strength 1.36 and was simply
  // unreachable, which is the defect; the first sat at 0.41 and everything
  // useful crowded into the top of the range. Not higher than 0.15: at 0.20 the
  // top of the dial is a 15%/frame cut at that canvas, enough to visibly
  // shorten trails on a preset that was fine to begin with.
  //
  // Note what this is NOT evidence of. "Strength 1.0 does nothing to a runaway"
  // was never demonstrated -- the run that appeared to show it had a preset
  // whose loop never ignited (its strength-0 row read 0.056, already calm), so
  // 0.9267 was never tested against an ignited buffer. The case for 0.15 rests
  // on reachability of known-good values, not on a measured failure of 0.10.
  static constexpr float kDampMaxLoss = 0.15f;

  // The strengths offered in the UI, defined ONCE. The canvas-limit choices
  // are spelled out separately in three files and have to be kept in step by
  // hand; there is no reason to repeat that.
  static constexpr float kDampChoices[5] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
  // The canvas-limit choices, likewise defined ONCE (issue 19). The comment
  // above used to note that these were spelled out separately and kept in step
  // by hand; they now are not. 0 means "no limit".
  static constexpr int kCanvasChoices[6] = { 0, 1920, 1440, 1080, 768, 512 };
  static const wchar_t* DampChoiceLabel(int i);
  // Which entry of kDampChoices a stored strength corresponds to, so the
  // three UI surfaces that have to tick the right item do not each carry
  // their own float comparison. Anything unrecognised reads as Off.
  static int DampChoiceIndex(float strength);

  // A live, session-only feedback multiplier that beats the annotation and the
  // scratch-preset guard alike. Negative = no override.
  //
  // Exists so a mitigation can be tried on the running frame without editing a
  // preset, writing presets.json or relaunching -- which is what Shane asked
  // for, and is also the only way to measure the mechanism at all: the ruler
  // preset that makes the multiplier readable lives in a TEST folder, and a
  // TEST preset under testing mode deliberately has no identity to hang an
  // annotation on.
  float m_fDampOverride = -1.0f;

  // Per-frame feedback multiplier for the current preset, derived from the
  // canvas -- 1.0 means "do nothing", which is the answer for every preset
  // without a feedbackDamp annotation and for every canvas at or below the
  // reference edge. See the definition for why it is never a fitted constant.
  float EffectiveFeedbackDamp() const;

  // Same curve, for a render target that is NOT the primary canvas. The mirror
  // sims run at their own size, so the multiplier has to be derived from THEIR
  // long edge -- the whole point of the curve is that it tracks the canvas.
  float FeedbackDampForEdge(int longEdge) const;

  // The damp strength resolved for the preset currently on screen, 0 = off.
  //
  // Cached rather than looked up per frame, because the lookup reads
  // m_presetAnnotations and EffectiveFeedbackDamp runs on the RENDER thread
  // once every frame, while the IPC and ToolWindow threads insert into that
  // same map. EffectiveCanvasLimit gets away with the live lookup by being
  // called only on a preset change or a resize; this would have been a
  // std::map read racing a std::map insert sixty times a second.
  //
  // The STRENGTH is cached and the multiplier is derived per frame, so a
  // window resize takes effect immediately without anything having to notice
  // the preset did not change.
  float m_fCurrentDampStrength = 0.0f;

  // Re-resolve m_fCurrentDampStrength. Call on a preset change (passing the
  // INCOMING preset's hash, since m_pState is still the outgoing one at that
  // point) and whenever the annotation is written.
  void RefreshCurrentDampStrength(const char* szHashOverride = nullptr);

  // Does the running preset's composite shader invert or fold the feedback it
  // samples? If so the damp will BRIGHTEN it rather than calm it -- see
  // comp_inversion.h. A hint for the user, never acted on: nothing here
  // enables, disables or reverses a mitigation on its own.
  bool CompShaderInvertsFeedbackNow() const;

  // The running preset's bare filename. The `wcsrchr(..., '\') ? +1 : whole`
  // dance appears in a dozen places; the UI needs it to ask "is the row I am
  // drawing the preset that is actually on screen?".
  const wchar_t* CurrentPresetFilename() const;

  // Bleed energy out of the feedback buffer: a full-screen black quad at
  // alpha = 1 - damp, drawn into VS[1] right after the warp mesh so the
  // shapes and waves that follow are NOT attenuated -- they are this frame's
  // new signal, not the accumulated history that is running away.
  void ApplyFeedbackDamp(ID3D12GraphicsCommandList* cmdList, float damp);

  // Every mitigation that belongs INSIDE the render sequence, in one place, so
  // that adding one reaches the primary and every mirror at once (#15).
  //
  // Called from both render bodies at the same point: immediately after the
  // warp pass, while the write target holds only the warped history. That
  // position is load-bearing and the primary's comment says why -- "Here and
  // not later: VS1 currently holds ONLY the warped history, and that is the
  // only thing that should be attenuated."
  //
  // Add the third mitigation HERE. Do not add it to either caller.
  void ApplyRenderMitigations(const RenderContext& ctx);

  // The warp draw dispatch, for any surface (#15). The four branches that
  // decide what gets drawn during a blend used to exist twice, and had already
  // drifted: the primary's second pass was missing a PSO fallback the mirror
  // had, so a preset could vanish mid-blend. One body now, so they cannot
  // drift again.
  //
  // Callers still own everything AROUND the dispatch -- the resource
  // transitions, the render target, the viewport, and building the binding
  // slots -- because those legitimately differ. This is only the part where
  // the preset's own state decides what to draw.
  void RenderWarpPass(const RenderContext& ctx);

  // The comp draw dispatch, for any surface (#15). Same story as the warp
  // pass, including the same defect: the primary's second blend pass was
  // missing the PSO fallback the mirror had, so a preset could vanish
  // mid-blend here too.
  //
  // Callers own the render target, the viewport and the binding slots. The
  // primary's target is the backbuffer or the FLOAT feedback buffer depending
  // on the preset, at two different resolutions; a mirror's is its own display
  // texture. None of that is shared.
  void RenderCompPass(const RenderContext& ctx);

  // The blur pyramid, for any surface (#15). Unlike warp and comp this carries
  // no preset-state branching -- it is the same separable-blur loop either way
  // -- so the value here is that the weights, the progressive scale/bias maths
  // and the per-pass constant layout stop existing twice.
  int  ScanHighestBlurUsed(bool bBlending) const;
  void RenderBlurPasses(const RenderContext& ctx);
  // Long-edge clamp + 16-align for the MIRROR's own feedback canvas. The
  // mirror simulates into its own textures, so a canvasMax that only shrank
  // the primary left it running at 1920x1088 (2.09 MP) while the primary sat
  // at 432x768 (0.33 MP) — "Flexi - jellyfish jam.milk" still decayed to a
  // flat gradient on the panels with the window perfectly healthy (Shane,
  // 2026-08-23). Both sizing sites MUST call this: if they disagree the pipe
  // is recreated every frame, which TDRs.
  // Takes the surface, because the per-preset limit is PER SURFACE. It used
  // to read the first one's for every display -- harmless while there was
  // only ever one, wrong the moment #186 phase 3 made a collection.
  void ClampOrientCanvas(const MirrorSurface& surf, int& w, int& h) const;
  // Does the existing orient pipe have the textures the CURRENT preset mode
  // needs? classic wants imgFb[2]; milk3 additionally wants fbA[2]. Both the
  // render thread's "do I need to rebuild" test and EnsureOrientPipeline's
  // early-out must use this one function: when only the latter knew about the
  // mode, a classic -> .milk3 switch left the render thread thinking the pipe
  // was fine while the worker (barred from rebuilding it) skipped every frame,
  // and the mirrors froze on every .milk3 preset (2026-08-23).
  bool OrientPipeReadyForMode(const MirrorSurface& surf) const;
  // The canvas limit the orient pipe was BUILT with, latched at creation and
  // deliberately not tracked live. Resizing an existing orient pipe hangs the
  // GPU: ReleaseOrientPipeline drops the textures but DXContext::AllocateRtv is
  // a bump allocator with no free path, so each recreate burns ~10 RTV slots
  // below DXC_MIRROR_RTV_BASE until the allocator rewinds and the new RTVs
  // alias live ones. Deterministic TDR (0x887A0006) ~4 s after stepping off a
  // preset whose canvasMax differed, 2026-08-23; with the size held constant
  // the same transition is clean. Latching means a preset change never resizes
  // the pipe — the mirror adopts the current limit when the pipe is next built
  // (mirror enable, panel/orientation change, epoch recreate).

  // The limit the live canvas was actually allocated for. Compared against
  // EffectiveCanvasLimit() after a preset change, because the canvas is sized
  // in AllocateMyDX9Stuff and loading a preset does not otherwise resize it.
  // -1 = never allocated.
  int m_nCanvasLimitApplied = -1;

  // Write a per-preset canvas limit by filename (0 clears it). Shared by the
  // Presets context menu and the Annotations combo so the two surfaces cannot
  // drift. Rebuilds the canvas only when the affected preset is the one
  // playing -- setting a limit on a preset you are not watching must not
  // resize the live canvas.
  void SetPresetCanvasMaxByFile(const wchar_t* filenameOnly, int px);

  // Per-preset damp strength, 0..1 (0 clears). Companion to the above --
  // the two mitigations are independent and either, both, or neither may
  // be set on a preset.
  void SetPresetFeedbackDampByFile(const wchar_t* filenameOnly, float strength);

  // Live search predicate for the Annotations list. Pure -- no UI state -- so
  // it is testable over IPC and safe to call from the ToolWindow thread.
  // Substring by default; glob once the query contains * or ?.
  bool AnnotationMatches(const PresetAnnotation& a, const wchar_t* query) const;

  // Full path that exists on disk for this annotation, or empty if the preset
  // is gone. Consults the recorded `paths` as well as the current preset dir --
  // annotations outlive directory changes.
  std::wstring ResolveAnnotationPath(const PresetAnnotation& a) const;

  // Drop annotations whose preset can no longer be found. Returns the count.
  // True only when the entry NAMES locations and none of them exist. An entry
  // with no recorded path is not missing -- it is unlocated, which is not the
  // same thing and must never be deleted on that basis.
  bool IsAnnotationKnownMissing(const PresetAnnotation& a) const;

  int RemoveMissingAnnotations();

  // Canvas-limit / metric / annotation-query IPC. Split out of LaunchMessage:
  // that function is one long else-if chain and these tipped it past MSVC's
  // block nesting limit (C1061). Returns true when it handled the message.
  bool HandleCanvasIPC(const wchar_t* sMessage);
  // SET_PRESET_ORDER / SET_PRESET_LOCK / SET_TIME_BETWEEN_PRESETS /
  // GET_PRESET_CYCLE. A helper, not four more links in LaunchMessage's else-if
  // chain, which is at MSVC's block-nesting limit.
  bool HandlePresetCycleIPC(const wchar_t* sMessage);
  // SET_VSYNC / GET_VSYNC. VSync had no IPC command at all, so the only way to
  // change it was the Visual window's checkbox or hand-editing settings.ini --
  // which made every automated frame-rate measurement silently wrong, since it
  // defaults ON and pins each surface to the panel refresh. A helper, not two
  // more links in LaunchMessage's else-if chain, which is at MSVC's limit.
  bool HandleVSyncIPC(const wchar_t* sMessage);
  // SET_WATERMARK / SET_ALWAYS_ON_TOP / SET_MIRROR_INDEPENDENT: absolute forms
  // of window-mode toggles that only ever had WATERMARK/ALWAYS_ON_TOP/
  // MIRROR_INDEPENDENT (flip, direction unknown to the caller). A remote that
  // has to RENDER the state -- show a checkbox as checked or not -- cannot use
  // a toggle without first reading the state some other way; these let it just
  // assert what it wants. forgejo#85 comment thread. Same early-return reason
  // as HandlePresetCycleIPC.
  bool HandleWindowModeIPC(const wchar_t* sMessage);
  // GET_INPUT_MIX: the read-back the Spout/video input-mix SET_INPUTMIX_*
  // signals never had (forgejo#29). Those signals already work -- this adds
  // no new setter, only the reader a reconnecting remote needs to render
  // their true state instead of guessing. Same early-return reason as
  // HandlePresetCycleIPC.
  bool HandleInputMixIPC(const wchar_t* sMessage);
  // DISPLAY_PROFILE_LIST/_SAVE=/_LOAD=/_DELETE=/_STARTUP[=]: display profiles
  // addressed by name, the way MIXER_PROFILE_*/VFX_PROFILE_* already are
  // (forgejo#95). The older SAVE_DISPLAY_PROFILE=/LOAD_DISPLAY_PROFILE= take
  // a full path, which a remote has no way to construct. _STARTUP is the
  // piece that did not exist under any name: pointing the startup profile at
  // an existing one without loading or saving anything, so a remote can set
  // a default for a machine nobody is sitting at.
  bool HandleDisplayProfileIPC(const wchar_t* sMessage);
  // GET_COVER_STATUS: the read-back SIGNAL|SHOW_COVER never had (forgejo#29).
  // Reads live render state (a texmgr slot with nUserData==0, the identity
  // LaunchSprite gives cover art) rather than a separate tracked flag, so it
  // cannot drift from what the display is actually showing -- the cover
  // sprite's own EEL "done" var kills its slot when the fade-out finishes
  // (milkdropfs.cpp), and this reads that same slot. Same early-return
  // reason as HandlePresetCycleIPC.
  bool HandleCoverStatusIPC(const wchar_t* sMessage);
  // Shared by HandleWindowModeIPC's SET_WATERMARK= and
  // HandleDisplayPresetIPC's SET_DISPLAY_WATERMARK= primary branch, so there
  // is one place that knows "only post WM_MW_WATERMARK when the state is
  // actually changing" rather than two copies that could disagree.
  void SetWatermark(bool want);
  // Hotkey diagnostics: DIAG_HOTKEYS / _BIND= / _CONFLICTS= (forgejo#72).
  // Also an early-return helper -- the else-if chain is at MSVC's nesting limit.
  bool HandleHotkeyDiagIPC(const wchar_t* sMessage);
  // SET_DISPLAY_MODE / GET_CHILDREN. Same early-return reason as above.
  bool HandleDisplayModeIPC(const wchar_t* sMessage);
  // Browsing and annotating presets.json over IPC (forgejo#41). Lives in
  // engine_preset_annotations.cpp, beside the store it reads.
  bool HandlePresetBrowseIPC(const wchar_t* sMessage);

  // Lowercased bare filename -> every full path it was found at, under the
  // preset collection root. Built once, lazily, so an entry that recorded no
  // path can still be located: ResolveAnnotationPath only looks in the CURRENT
  // preset directory, and on a real install that is a subfolder holding a
  // fraction of the collection (forgejo#41).
  std::unordered_map<std::wstring, std::vector<std::wstring>> m_presetPathIndex;
  bool m_bPresetPathIndexBuilt = false;
  void BuildPresetPathIndex();
  std::wstring FindPresetByName(const std::wstring& bareFilename);
  bool HandleDisplayPresetIPC(const wchar_t* sMessage);


  DX12Texture m_canvasSrcTex;                        // back-buffer copy sampled by the canvas metric reduction
  DX12Texture m_canvasReduceTex;                     // 8x8 reduction target for the canvas metric
  ComPtr<ID3D12PipelineState> m_pCanvasReducePSO;    // 8x8 frame-reduction PSO
  CanvasMetric m_canvasMetric;                       // Phase 1 instrumentation: measures the presented frame
  DX12Texture m_dx12Feedback[2];                      // ping-pong feedback buffers for Buffer A (FLOAT32)
  DX12Texture m_dx12ImageFeedback[2];                 // ping-pong feedback buffers for Image pass (FLOAT32)
  int m_nFeedbackIdx = 0;                            // read index (write = 1 - read), shared by both pairs
  bool m_bCompUsesFeedback = false;                  // true when comp shader uses sampler_feedback
  bool m_bCompUsesImageFeedback = false;             // true when comp shader uses sampler_image

  // Audio FFT/waveform texture for Shadertoy shaders (512x2 R32_FLOAT)
  // Row 0 = FFT spectrum (512 bins, 0-11kHz), Row 1 = PCM waveform (512 samples)
  DX12Texture m_dx12AudioTex;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_audioUploadBuffer;
  void CreateAudioTexture();
  void UpdateAudioTexture();   // per-frame: upload latest FFT/waveform to GPU

  // Custom channel textures from Shader Import (user-selected texture files)
  DX12Texture m_dx12ChannelTex[4];           // loaded textures for sampler_chtex0..3
  std::wstring m_szChannelTexPath[4];        // file paths (set by Import UI)
  bool m_bHasBufferA = false;                        // true when preset has a Buffer A shader
  bool m_bHasBufferB = false;                        // true when preset has a Buffer B shader
  bool m_bHasBufferC = false;                        // true when preset has a Buffer C shader
  bool m_bHasBufferD = false;                        // true when preset has a Buffer D shader
  bool m_bShadertoyMode = false;                     // true when a .milk3 Shadertoy preset is active
  int  m_nShadertoyStartFrame = 0;                   // frame at which Shadertoy mode was activated (for iFrame=0)
  ComPtr<ID3D12PipelineState> m_dx12BufferAPSO;      // Buffer A pixel shader PSO
  ComPtr<ID3D12PipelineState> m_dx12BufferBPSO;      // Buffer B pixel shader PSO
  ComPtr<ID3D12PipelineState> m_dx12BufferCPSO;      // Buffer C pixel shader PSO
  ComPtr<ID3D12PipelineState> m_dx12BufferDPSO;      // Buffer D pixel shader PSO
  DX12Texture m_dx12FeedbackB[2];                    // ping-pong feedback buffers for Buffer B (FLOAT32)
  DX12Texture m_dx12FeedbackC[2];                    // ping-pong feedback buffers for Buffer C (FLOAT32)
  DX12Texture m_dx12FeedbackD[2];                    // ping-pong feedback buffers for Buffer D (FLOAT32)
  std::atomic<int> m_nRecompileResult{0};            // 0=idle, 1=pending, 2=done-ok, 3=done-fail
  void CopyBackbufferToFeedback();                   // capture comp output for next frame's feedback (single-pass)
  void RenderFrameShadertoy(ID3D12GraphicsCommandList* cmdList);  // Shadertoy pipeline (skip warp/blur/shapes)
  // See also RenderMilk3ImageToMirror / BlitMainToMirror (display outputs)
  UINT m_warpMainTexSlot = 0;                         // t-register for sampler_main in warp PS
  UINT m_compMainTexSlot = 0;                         // t-register for sampler_main in comp PS
  UINT m_oldWarpMainTexSlot = 0;                      // t-register for sampler_main in old warp PS
  UINT m_oldCompMainTexSlot = 0;                      // t-register for sampler_main in old comp PS
  bool m_bDX12PSOsDirty = false;                      // deferred PSO creation flag
  void CreateDX12PresetPSOs();                        // the engine's own, from m_shaders
  // #184: build a preset's pipeline objects from ANY shader set into ANY
  // destination, so a render context can have its own.
  void CreateDX12PresetPSOs(const PShaderSet& sh, const PShaderSet& oldSh,
                            PresetPsoOwned& out);

  // ── Video Input (Spout / Webcam / Video File) ──
  enum VideoInputSource {
      VID_SOURCE_NONE   = 0,
      VID_SOURCE_SPOUT  = 1,
      VID_SOURCE_WEBCAM = 2,
      VID_SOURCE_FILE   = 3
  };
  int     m_nVideoInputSource = VID_SOURCE_NONE; // active source type

  // Webcam / Video File capture (Media Foundation)
  std::unique_ptr<class VideoCaptureSource> m_videoCapture;
  wchar_t m_szWebcamDevice[256] = {};   // friendly name of selected webcam
  wchar_t m_szVideoFile[MAX_PATH] = {}; // path to video file
  bool    m_bVideoLoop = true;          // loop video file playback

  void    InitVideoCapture();
  void    DestroyVideoCapture();
  void    UpdateVideoCaptureTexture();   // per-frame GPU upload

  // Spout receiver (source type 1)
  struct SpoutInputState {
      mdrop::SpoutReceiver receiver;
      ComPtr<ID3D12Resource> pReceivedTexture;
      DX12Texture dx12InputTex;
      UINT nSenderWidth = 0, nSenderHeight = 0;
      bool bReceiverReady = false;
      bool bConnected = false;
  };
  std::unique_ptr<SpoutInputState> m_spoutInput;

  // Shared video input settings (apply to all sources)
  bool    m_bSpoutInputEnabled = false;  // kept for backward compat (maps to m_nVideoInputSource != 0)
  bool    m_bSpoutInputOnTop = false;       // false=background, true=overlay
  float   m_fSpoutInputOpacity = 1.0f;
  bool    m_bSpoutInputLumaKey = false;
  float   m_fSpoutInputLumaThreshold = 0.1f;
  float   m_fSpoutInputLumaSoftness = 0.1f;
  wchar_t m_szSpoutInputSender[256] = {};
  ComPtr<ID3D12PipelineState> m_pSpoutInputPSO;

  void InitSpoutInput();
  void DestroySpoutInput();
  void UpdateSpoutInputTexture();
  void CompositeSpoutInput(bool isBackground);
  void CompositeVideoInput(bool isBackground, DX12Texture& tex, UINT srcW, UINT srcH,
       const RenderContext* rc = nullptr);
  void CompileSpoutInputPSO();
  void EnumerateSpoutSenders(std::vector<std::string>& outNames);
  void SaveSpoutInputSettings();
  void LoadSpoutInputSettings();

  // ── Video Effects ──
  //
  // The parameter set itself is in video_effect_params.h. It was nested in
  // Engine, which made every consumer say Engine::VideoEffectParams and put a
  // ~1,900 line header in the way of touching a parameter set at all.
  VideoEffectParams m_videoFX;
  ComPtr<ID3D12PipelineState> m_pVideoFX_PSO_Alpha;
  ComPtr<ID3D12PipelineState> m_pVideoFX_PSO_Additive;
  ComPtr<ID3D12PipelineState> m_pVideoFX_PSO_Solid;   // for shader-based blend modes 2-5
  DX12Texture m_dx12VideoFXDest;                       // RT copy for shader-based blends
  void CompileVideoFXPSOs();
  void CompositeVideoInputFX(bool isBackground, DX12Texture& tex, UINT srcW, UINT srcH,
       const RenderContext* rc = nullptr);

  // Video Effects Window
  class VideoEffectsWindow* m_pVideoEffectsWindow = nullptr;
  void OpenVideoEffectsWindow();
  void CloseVideoEffectsWindow();

  // Video FX Profiles
  //
  // Every profile lives in ONE file, vfxprofiles.json, keyed by name -- not a
  // directory of small files, and not a live-state file that saves itself as
  // you drag a slider. Nothing about video effects is written unless a profile
  // is explicitly saved, and nothing is restored at startup unless "Load on
  // startup" names one. Parameters otherwise begin at their defaults.
  //
  // A rewrite re-emits members this build does not recognise, so the format
  // can gain sections later without an older build erasing them.
  //
  // m_videoFXSaved is the baseline the Save button's red state compares
  // against: the parameters as they were when the current profile was last
  // saved or loaded. Live != baseline means there is something to save.
  wchar_t m_szCurrentVFXProfile[MAX_VFX_PROFILE_NAME] = {};  // loaded profile NAME (empty = none)

  // ── Scoped VFX ──
  //
  // A profile applied because the running preset (or one of its tags) asked
  // for it. It is SCOPED: the VFX state from before the preset is snapshotted
  // and restored when the preset is left, so a preset cannot permanently move
  // settings the user did not change.
  //
  // One thing deliberately does NOT happen when one of these is applied:
  //   * m_szCurrentVFXProfile is not set -- SaveVideoFXOnExit writes the
  //     CURRENT profile back to disk, so a rule firing silently would make the
  //     app overwrite a profile the user never chose.
  //
  // If the state is edited during a scope the edit is kept and the scope is
  // marked dirty; the restore then waits for a Keep/Discard answer rather than
  // silently reverting a deliberate change.
  bool           m_bVFXScopeActive = false;
  bool           m_bVFXScopeDirty  = false;
  VFXProfileData m_vfxScopeBaseline;
  wchar_t        m_szVFXScopeProfile[MAX_VFX_PROFILE_NAME] = {};

  void PushScopedVFXProfile(const std::wstring& name);
  void PopScopedVFXProfile();
  // Answer a pending scoped edit. Keep writes the live fx into the scoped
  // profile; either way the baseline is then restored. This is the ONE path
  // that writes vfxprofiles.json for this feature, and both the prompt's
  // buttons and VFX_SCOPED_KEEP go through it so there is a single behaviour.
  void AnswerScopedVFX(bool bKeep);
  // Exit path: restores the baseline even when dirty. There is no opportunity
  // to ask at shutdown, and discarding the edit is far better than letting
  // Save-on-exit write it into whatever profile happens to be current.
  void ForceRestoreScopedVFX();
  bool    m_bEnableVFXStartup = false;
  wchar_t m_szVFXStartup[MAX_VFX_PROFILE_NAME] = {};
  // Off by default: saving a profile is an explicit act, and the Save Profile
  // button goes red while there is something to save. Turning this on says
  // "and also write it back when the program exits" -- program exit, not the
  // Video Effects window closing, which happens all the time.
  bool    m_bEnableVFXSaveOnExit = false;
  VideoEffectParams m_videoFXSaved;              // baseline for the dirty check
  // The store itself is VFXProfileStore (vfx_profile_store.h) -- the file, the
  // schema and the importers, with no Engine dependency. Call it directly for
  // anything that is purely about the file: m_vfxProfiles.Names(), .Delete(),
  // .Exists(), .Import().
  //
  // Only the two operations below stay here, because they are the ones that
  // bridge the store and the engine's live state.
  VFXProfileStore m_vfxProfiles;

  // Call once at startup, before anything touches the store.
  void InitVFXProfileStore();

  // Live parameters as a profile's worth of data, and back. Defined in
  // engine_video_effects_ui.cpp, beside the rest of the store bridge.
  VFXProfileData CaptureVFXProfile() const;
  void           ApplyVFXProfile(const VFXProfileData& d);

  bool    SaveVideoFXProfile(const wchar_t* name);
  // Also records the name in m_szCurrentVFXProfile. Sections the stored
  // profile does not carry leave the live values alone -- see
  // VFXProfileStore::Load.
  bool    LoadVideoFXProfile(const wchar_t* name);

  // Called once on the way out. Writes the loaded profile back if the user
  // asked for that with "Save on exit"; does nothing otherwise.
  void    SaveVideoFXOnExit();

  // Are there parameter changes not written to the current profile?
  // With no profile loaded there is nothing to be dirty against, so false.
  // Defined in engine_video_effects_ui.cpp, where the tunable table is in
  // scope -- it covers the render tunables as well as the effect parameters.
  bool    IsVideoFXDirty() const;

  // Snapshot live values as the clean baseline. Call after saving or loading a
  // profile; one function so the two halves cannot be updated separately.
  void    MarkVideoFXSaved();

  // Call after ANY change to m_videoFX: persists live state and repaints the
  // Save button so its red state tracks reality.
  void    OnVideoFXChanged();

  // VFX Profile Picker Window
  class CustomShadersWindow* m_pCustomShadersWindow = nullptr;
  void OpenCustomShadersWindow();
  void CloseCustomShadersWindow();

  class VFXProfileWindow* m_pVFXProfileWindow = nullptr;
  void OpenVFXProfileWindow();
  void CloseVFXProfileWindow();

  // Raised when a preset is left after its scoped VFX profile was edited.
  class VFXKeepPromptWindow* m_pVFXKeepPromptWindow = nullptr;
  void OpenVFXKeepPrompt(const std::wstring& profileName);
  void CloseVFXKeepPrompt();

  // ── Game Controller ──
  bool    m_bControllerEnabled = false;
  int     m_nControllerDeviceID = -1;    // winmm joy ID (0-15), -1 = none
  wchar_t m_szControllerName[256] = {};  // friendly name for INI persistence
  DWORD   m_dwLastControllerButtons = 0; // previous frame's button state
  std::map<int, std::string> m_controllerConfig; // button# → command
  std::string m_szControllerJSONText;    // raw JSON text for UI edit control

  void PollController();
  void ExecuteControllerCommand(const std::string& cmd);
  void EnumerateControllers(HWND hCombo);
  void LoadControllerJSON();
  void SaveControllerJSON(const std::string& jsonText);
  void LoadControllerSettings();
  void SaveControllerSettings();
  std::string GetDefaultControllerJSON();
  void ParseControllerJSON(const std::string& jsonText);
  void ShowControllerHelpPopup(HWND hParent);

  // ── MIDI ──
  bool    m_bMidiEnabled = false;
  int     m_nMidiDeviceID = -1;        // winmm MIDI input device ID
  wchar_t m_szMidiDeviceName[256] = {};
  int     m_nMidiBufferDelay = 30;     // CC debounce delay (ms)
  std::vector<MidiRow> m_midiRows;     // 50 mapping slots
  MidiInput m_midiInput;

  void LoadMidiJSON();
  void SaveMidiJSON();
  void LoadMidiSettings();
  void SaveMidiSettings();
  void ParseMidiJSON(const std::string& json);
  std::string SerializeMidiJSON() const;
  void ExecuteMidiButton(const MidiRow& row);
  void ExecuteMidiKnob(const MidiRow& row, int midiValue);
  void LoadMidiDefaultActions(std::vector<std::string>& out);
  void OpenMidiDevice();
  void CloseMidiDevice();

  int               m_nTitleTexSizeX, m_nTitleTexSizeY;
  UINT              m_adapterId;
  MYVERTEX* m_verts;
  MYVERTEX* m_verts_temp;
  // Warp-mesh indexed submission scratch (see DrawWarpMeshIndexed).
  // m_warpVertsStaged holds one cDecay-tinted copy of m_verts; m_warpIdx16All is the
  // full triangle list as 16-bit indices, built once; m_warpIdx16 is the culled subset.
  MYVERTEX* m_warpVertsStaged = nullptr;
  UINT16*   m_warpIdx16 = nullptr;
  UINT16*   m_warpIdx16All = nullptr;
  int       m_warpIdx16AllCount = 0;
  void DrawWarpMeshIndexed(const MYVERTEX* srcVerts, DWORD cDecay,
                           bool bCullTiles, bool bFlipCulling,
                           ID3D12GraphicsCommandList* cmdList);
  td_vertinfo* m_vertinfo;
  int* m_indices_strip;
  int* m_indices_list;

  // for final composite grid:
#define FCGSX 32 // final composite gridsize - # verts - should be EVEN.
#define FCGSY 24 // final composite gridsize - # verts - should be EVEN.
                 // # of grid *cells* is two less,
                 // since we have redundant verts along the center line in X and Y (...for clean 'ang' interp)
  MYVERTEX    m_comp_verts[FCGSX * FCGSY];
  int         m_comp_indices[(FCGSX - 2) * (FCGSY - 2) * 2 * 3];

  bool		m_bMMX;
  //bool		m_bSSE;
  bool        m_bHasFocus;
  bool        m_bHadFocus;
  bool		m_bOrigScrollLockState;
  //bool      m_bMilkdropScrollLockState;  // saved when focus is lost; restored when focus is regained

  int         m_nNumericInputMode;	// NUMERIC_INPUT_MODE_CUST_MSG, NUMERIC_INPUT_MODE_SPRITE
  int         m_nNumericInputNum;
  int			m_nNumericInputDigits;
  td_custom_msg_font   m_CustomMessageFont[MAX_CUSTOM_MESSAGE_FONTS];
  td_custom_msg        m_CustomMessage[MAX_CUSTOM_MESSAGES];

  // Animation profiles
  td_anim_profile      m_AnimProfiles[MAX_ANIM_PROFILES];
  int                  m_nAnimProfileCount = 0;
  int                  m_nSongTitleAnimProfile = -1;   // -1 = default hardcoded, -2 = random, 0+ = profile
  int                  m_nPresetNameAnimProfile = -3;  // -1 = disabled, -2 = random, -3 = simple, 0+ = profile

  // Simple preset name HUD display (non-animated, fixed font size)
  wchar_t              m_szPresetNameDisplay[512] = {};
  float                m_fPresetNameShowUntil = -1.0f;

  texmgr      m_texmgr;		// for user sprites
  
  bool m_blackmode = false;


  int         m_nFramesSinceResize;

  char        m_szShaderIncludeText[32768];     // note: this still has char 13's and 10's in it - it's never edited on screen or loaded/saved with a preset.
  int         m_nShaderIncludeTextLen;          //  # of chars, not including the final NULL.
  char        m_szDefaultWarpVShaderText[32768]; // THIS HAS CHAR 13/10 CONVERTED TO LINEFEED_CONTROL_CHAR
  char        m_szDefaultWarpPShaderText[32768]; // THIS HAS CHAR 13/10 CONVERTED TO LINEFEED_CONTROL_CHAR
  char        m_szDefaultCompVShaderText[32768]; // THIS HAS CHAR 13/10 CONVERTED TO LINEFEED_CONTROL_CHAR
  char        m_szDefaultCompPShaderText[32768]; // THIS HAS CHAR 13/10 CONVERTED TO LINEFEED_CONTROL_CHAR
  char        m_szBlurVS[32768];
  char        m_szBlurPSX[32768];
  char        m_szBlurPSY[32768];
  void        GenWarpPShaderText(char* szShaderText, bool bWrap);
  void        GenCompPShaderText(char* szShaderText, float hue_shader, bool bBrighten, bool bDarken, bool bSolarize, bool bInvert);

  //====[ 2. methods added: ]=====================================================================================

  void RenderFrame(int bRedraw);
  void DX12_RenderWarpAndComposite();
  // Overrides = mirror sim contexts drawing with their own states; null =
  // the primary's m_pState/m_pOldState (default, unchanged behavior).
  //
  // #186: these two took no override, so a mirror holding its OWN preset drew
  // the PRIMARY preset's wave, darken-center and borders into its feedback
  // face -- live, and warped forward every frame, so it read as a ghost of the
  // primary pulsing with the music. Harmless while both surfaces rendered the
  // same preset: the same coupling d9c410e4 fixed for the descriptor tables,
  // in the one stage that had no way to say which preset it was drawing.
  //
  // The sound override goes with the state one. DX12_DrawWave already took
  // this surface's wave SAMPLES while reading the engine's live imm/imm_rel
  // and spectrum, so half the wave reacted to one surface's audio and half to
  // the other. #111's rule -- "the defect was that only part of the preset saw
  // it" -- applies to the audio exactly as it did to `frame`.
  // pCtx carries THIS surface's canvas and aspect (#121). Null means the
  // engine's own, so the primary's call sites are unchanged. It is what
  // lets SizeGuard be deleted: these four hold 57 of the 61 engine
  // canvas reads in a mirror record.
  void DX12_DrawWave(float* fL, float* fR, CState* pStateOverride = nullptr,
                     const td_mysounddata* pSndOverride = nullptr,
                     const RenderContext* pCtx = nullptr);
  void DX12_DrawSprites(CState* pStateOverride = nullptr,
                        const RenderContext* pCtx = nullptr);
  void DX12_DrawCustomShapes(CState* pNewOverride = nullptr, CState* pOldOverride = nullptr,
                             const RenderContext* pCtx = nullptr);
  void DX12_DrawCustomWaves(CState* pNewOverride = nullptr, CState* pOldOverride = nullptr,
                            const RenderContext* pCtx = nullptr);
  void AlignWave(int nSamples);

  void        DrawTooltip(wchar_t* str, int xR, int yB);
  // The canvas a blend pattern is generated ONTO.
  //
  // These used to be read straight off the engine as m_vertinfo/m_fAspectX/
  // m_fAspectY, which meant a mirror generating a pattern for its own canvas
  // had to SWAP those engine members around the call and put them back --
  // one of the three mutation sites #186 phase 2 exists to remove, and the
  // only one that mutates a pointer rather than a scalar.
  //
  // Passing the target instead makes the generators pure with respect to the
  // engine: two canvases can be filled from two threads without either one
  // seeing the other's aspect ratio.
  struct BlendPatternTarget {
    td_vertinfo* vertinfo = nullptr;
    float        aspectX  = 1.0f;
    float        aspectY  = 1.0f;
  };
  // The primary's canvas, for the engine's own calls.
  BlendPatternTarget PrimaryBlendTarget() {
    return BlendPatternTarget{ m_vertinfo, m_fAspectX, m_fAspectY };
  }

  void        RandomizeBlendPattern(const BlendPatternTarget& tgt);
  void        RandomizeBlendPattern() { RandomizeBlendPattern(PrimaryBlendTarget()); }
  // Recomputes the frozen .milk2 wipe field; needed at load AND after a
  // device teardown, which reallocates m_vertinfo uninitialised.
  void        ApplyMilk2BlendPattern(const BlendPatternTarget& tgt);
  void        ApplyMilk2BlendPattern() { ApplyMilk2BlendPattern(PrimaryBlendTarget()); }
  // Fills m_vertinfo with the measured MD3 PRO wipe field for a named
  // pattern. Returns false if that pattern has no measured analytic form,
  // in which case the caller falls back to RandomizeBlendPattern().
  // Seven patterns whose generator is transcribed from the decompilation
  // rather than fitted -- they use a hardcoded band, so the random one the
  // fitted table assumes was never theirs (issue 128).
  bool        ComputeMilk2RecoveredField(const BlendPatternTarget& tgt,
                                         const char* pattern, int direction);

  bool        ComputeMilk2BlendField(const BlendPatternTarget& tgt,
                                     const char* pattern, float bandCoord,
                                     int direction);
  // Reports the field just built and warns when it has collapsed. Called at
  // EVERY exit of the builder above -- the baked path used to return without
  // it, which hid the three worst cases (issue 114).
  void        CheckMilk2BlendField(const BlendPatternTarget& tgt,
                                   const char* pattern, int direction, int nVert);
  void        GenPlasma(const BlendPatternTarget& tgt,
                        int x0, int x1, int y0, int y1, float dt);
  void        CompilePresetShadersToFile(wchar_t* m_szCurrentPresetFile);
  void        ClearPreset();
  void        RemoveAngleBrackets(wchar_t* str);
  // ── This surface's frame and canvas, or the engine's ───────────────
  //
  // The rule these encode is the one 43e807c7 had to fix in ApplyShaderParams:
  // test the VALUE, never merely that a context exists. The primary's warp and
  // comp contexts carry time, fps and audio but describe no canvas, so `rc ?`
  // alone fed 0 in on the primary path and bound texsize as (0,0,inf,inf).
  //
  // Kept in ONE place rather than repeated at each site, because the failure
  // mode is a ternary that looks right at ten sites and is wrong at the
  // eleventh -- which is how that defect survived three commits.
  int FrameOf(const RenderContext* rc) const {
    return (rc && rc->nFrame >= 0) ? rc->nFrame
                                   : const_cast<Engine*>(this)->GetFrame();
  }
  int TexSizeXOf(const RenderContext* rc) const {
    return (rc && rc->canvasW > 0) ? rc->canvasW : m_nTexSizeX;
  }
  int TexSizeYOf(const RenderContext* rc) const {
    return (rc && rc->canvasH > 0) ? rc->canvasH : m_nTexSizeY;
  }
  float InvAspectXOf(const RenderContext* rc) const {
    return (rc && rc->canvasW > 0) ? rc->fInvAspectX : m_fInvAspectX;
  }
  float InvAspectYOf(const RenderContext* rc) const {
    return (rc && rc->canvasH > 0) ? rc->fInvAspectY : m_fInvAspectY;
  }

  void        LoadPerFrameEvallibVars(CState* pState, const RenderContext* rc = nullptr);
  void        LoadCustomWavePerFrameEvallibVars(CState* pState, int i,
                                                const RenderContext* rc = nullptr);
  void        LoadCustomShapePerFrameEvallibVars(CState* pState, int i, int instance,
                                                 const RenderContext* rc = nullptr);
  void        WriteRealtimeConfig();	// called on Finish()
  void        dumpmsg(wchar_t* s, int level = LOG_INFO);
  void        Randomize();
  void        LoadRandomPreset(float fBlendTime);
  void        LoadPreset(const wchar_t* szPresetFilename, float fBlendTime);
  bool        ParseMilk2File(const wchar_t* szPath, wchar_t* outTemp1, wchar_t* outTemp2, int& outMixType, float& outProgress, int& outDirection);
  void        ParseEmbeddedSprites(const std::string& buf);
  bool        LoadEmbeddedSpritesFromFile(const wchar_t* szPath);
  void        LoadMilk3Preset(const wchar_t* szPresetFilename, float fBlendTime);
  void        LoadPresetTick();
  bool        WaitForPendingLoad(DWORD timeoutMs = 3000); // waits for bg thread, applies via LoadPresetTick
  void        FindValidPresetDir();
  wchar_t* GetMsgIniFile() { return m_szMsgIniFile; };
  wchar_t* GetPresetDir() { return m_szPresetDir; };
  void		SavePresetAs(wchar_t* szNewFile);		// overwrites the file if it was already there.
  void		DeletePresetFile(wchar_t* szDelFile);
  void		RenamePresetFile(wchar_t* szOldFile, wchar_t* szNewFile);
  void		SetCurrentPresetRating(float fNewRating);
  void		SeekToPreset(wchar_t cStartChar);
  bool		ReversePropagatePoint(float fx, float fy, float* fx2, float* fy2);
  int 		HandleRegularKey(WPARAM wParam);
  void    SaveCurrentPresetToQuicksave(bool altDir);
  void		LaunchCustomMessage(int nMsgNum);
  // bStrict: the connection asked for an unmatched keyword to come back as an
  // error instead of falling through to the script engine (issue 102). False
  // for every caller that did not ask, which is every existing one.
  void		LaunchMessage(wchar_t* sMessage, bool bStrict = false);
  void    SendPresetChangedInfoToMDropDX12Remote();
  void    SendPresetWaveInfoToMDropDX12Remote();
  void    SendSettingsInfoToMDropDX12Remote();
  // PRESET_CYCLE=order,lock,interval,rand — the read-back that makes
  // SET_PRESET_ORDER / SET_PRESET_LOCK / SET_TIME_BETWEEN_PRESETS usable by a
  // client that has to render their state rather than just flip them.
  void    SendPresetCycleInfo();
  void    SendTrackInfoToMDropDX12Remote();
  void    SetWaveParamsFromMessage(std::wstring& message);
  void		ReadCustomMessages();
  void		LaunchSongTitleAnim(int supertextIndex);
  void    PushSongTitleAsMessage();
  void    ApplyAnimProfileToSupertext(td_supertext& st, const td_anim_profile& prof);
  int     PickRandomAnimProfile();
  void    ReadAnimProfiles();
  void    WriteAnimProfiles();
  void    ExportAnimProfiles(wchar_t* szPath);
  void    ImportAnimProfiles(wchar_t* szPath);
  void    CreateDefaultAnimProfiles();
  void    CaptureScreenshot();
  bool    CaptureScreenshotWithFilename(wchar_t* outFilename, size_t outFilenameSize);

  bool		RenderStringToTitleTexture(int supertextIndex);
  // cmdList: optional (null = main frame list). Used to re-draw messages onto mirror RTs.
  void		ShowSongTitleAnim(/*IDirect3DTexture9* lpRenderTarget,*/ int w, int h, float fProgress, int supertextIndex,
                            ID3D12GraphicsCommandList* cmdList = nullptr);
  // DrawWave() removed -- the dead DX9 twin of DX12_DrawWave above.
  void        ComputeGridAlphaValues();
  //void        WarpedBlit();
               // note: 'bFlipAlpha' just flips the alpha blending in fixed-fn pipeline - not the values for culling tiles.
  void        GetSafeBlurMinMax(CState* pState, float* blur_min, float* blur_max);
  void		RunPerFrameEquations(int code);
  // targetLayer: -1=all, 0=classic behind-text, 1=front,
  //              10=milk2 in-back (layer 0), 12=milk2 merge (layers 2–4)
  void		DrawUserSprites(int targetLayer = -1, ID3D12GraphicsCommandList* cmdList = nullptr,
                        const RenderContext* rc = nullptr);
  // pState/warpVerts overrides: mirror sim ctx (blend alpha comes from ITS
  // wipe mesh at its aspect); nulls = primary members.
  void        UpdateCompMeshBlendColors(const DWORD cShade[4],
                                        CState* pState = nullptr,
                                        const MYVERTEX* warpVerts = nullptr);
  void        DrawCompMesh(bool bCullTiles, bool bFlipCulling, ID3D12GraphicsCommandList* cmdList = nullptr);
  void    DrawOnTopSprites() override { if (SpritesEnabled()) DrawUserSprites(1); }
  // SendToDisplayOutputs is declared with the display output members above
  void		MergeSortPresets(PresetList& v, int left, int right);
  void		BuildMenus();
  void        SetMenusForPresetVersion(int WarpPSVersion, int CompPSVersion);
  // Settings screen (overlay)
  void        GetSettingValueString(int id, wchar_t* buf, int bufLen);
  const wchar_t* GetSettingHint(int id);
  void        ToggleSetting(int id);
  void        AdjustSetting(int id, int direction);
  void        SaveSettingToINI(int id);
  void        OpenFolderPickerForPresetDir(HWND hOwnerOverride = NULL);
  // Registry of all live ToolWindows (for iteration — self-register in ctor, deregister in dtor)
  std::vector<ToolWindow*> m_toolWindows;

  // Template helpers for standard Open/Close pattern
  template<typename T>
  void OpenToolWindow(std::unique_ptr<T>& ptr) {
    if (!ptr) ptr = std::make_unique<T>(this);
    ptr->Open();
  }
  template<typename T>
  void CloseToolWindow(std::unique_ptr<T>& ptr) {
    if (ptr) ptr->Close();
  }

  // ToolWindow subclass instances (each runs on its own thread)
  std::unique_ptr<SettingsWindow>       m_settingsWindow;
  int         m_nSettingsFontSize = -20;     // Shared font size for all tool windows (negative = pixel height)
  std::unique_ptr<DisplaysWindow>       m_displaysWindow;
  std::unique_ptr<SongInfoWindow>       m_songInfoWindow;
  std::unique_ptr<HotkeysWindow>        m_hotkeysWindow;
  std::unique_ptr<MidiWindow>           m_midiWindow;
  std::unique_ptr<PresetsWindow>        m_presetsWindow;
  std::unique_ptr<AnnotationsWindow>    m_annotationsWindow;
  std::unique_ptr<SpritesWindow>        m_spritesWindow;
  std::unique_ptr<MessagesWindow>       m_messagesWindow;
  std::unique_ptr<MixerWindow>          m_mixerWindow;
  std::unique_ptr<ButtonBoardWindow>    m_boardWindow;
  std::unique_ptr<ShaderImportWindow>   m_shaderImportWindow;
  std::unique_ptr<ScriptWindow>         m_scriptWindow;
  std::unique_ptr<RemoteWindow>         m_remoteWindow;
  std::unique_ptr<VisualWindow>         m_visualWindow;
  std::unique_ptr<ColorsWindow>         m_colorsWindow;
  std::unique_ptr<ControllerWindow>     m_controllerWindow;
  std::unique_ptr<WelcomeWindow>        m_welcomeWindow;
  std::unique_ptr<TextAnimWindow>       m_textAnimWindow;
  std::unique_ptr<WorkspaceLayoutWindow> m_workspaceLayoutWindow;
  std::unique_ptr<PresetEditorWindow>   m_presetEditorWindow;

  // Open/Close — standard pattern uses template; non-standard kept as declarations
  void OpenSettingsWindow()      { OpenToolWindow(m_settingsWindow); }
  // Open it showing one particular page -- for another window offering a link
  // to a setting it does not own.
  void OpenSettingsWindowAt(int page);

  // Open a tool window by the short name its hotkey entry carries -- the same
  // vocabulary the Hotkeys window and settings.ini use, e.g. "OpenSettings" or
  // "OpenAudioMixer" -- optionally showing one page and focusing one control.
  // Returns false when no tool window answers to that name.
  bool ShowToolWindowUI(const std::wstring& name, int tab, int ctrlId);

  // The tool windows, by the same name, with whether each is open. So nothing
  // has to guess a name or hold a window handle.
  std::vector<std::wstring> ToolWindowNames();

  // Close one, press a control in one, or move one. `x`/`y` of INT_MIN centres
  // it on the display the visualiser is on -- the one placement a caller
  // cannot compute, since it knows neither the window's size nor where the
  // render window went.
  bool CloseToolWindowUI(const std::wstring& name);
  bool ClickToolWindowUI(const std::wstring& name, int ctrlId);
  // x/y are in-out: pass INT_MIN for "centre on the visualiser's display"
  // and they come back as the position it actually moved to, so the
  // caller can be told where the window went rather than what it asked.
  bool MoveToolWindowUI(const std::wstring& name, int& x, int& y);
  void CloseSettingsWindow()     { CloseToolWindow(m_settingsWindow); }
  void OpenDisplaysWindow()      { OpenToolWindow(m_displaysWindow); }
  void CloseDisplaysWindow()     { CloseToolWindow(m_displaysWindow); }
  void OpenSongInfoWindow()      { OpenToolWindow(m_songInfoWindow); }
  void CloseSongInfoWindow()     { CloseToolWindow(m_songInfoWindow); }
  void OpenHotkeysWindow()       { OpenToolWindow(m_hotkeysWindow); }
  void CloseHotkeysWindow()      { CloseToolWindow(m_hotkeysWindow); }
  void OpenMidiWindow()          { OpenToolWindow(m_midiWindow); }
  void CloseMidiWindow()         { CloseToolWindow(m_midiWindow); }
  void OpenPresetsWindow()       { OpenToolWindow(m_presetsWindow); }
  void ClosePresetsWindow()      { CloseToolWindow(m_presetsWindow); }
  void OpenAnnotationsWindow()   { OpenToolWindow(m_annotationsWindow); }
  void CloseAnnotationsWindow()  { CloseToolWindow(m_annotationsWindow); }
  void OpenPresetEditorWindow()  { OpenToolWindow(m_presetEditorWindow); }
  void ClosePresetEditorWindow() { CloseToolWindow(m_presetEditorWindow); }
  void OpenSpritesWindow()       { OpenToolWindow(m_spritesWindow); }
  void CloseSpritesWindow()      { CloseToolWindow(m_spritesWindow); }
  void OpenBoardWindow()         { OpenToolWindow(m_boardWindow); }
  void CloseBoardWindow()        { CloseToolWindow(m_boardWindow); }
  void OpenShaderImportWindow()  { OpenToolWindow(m_shaderImportWindow); }
  void CloseShaderImportWindow() { CloseToolWindow(m_shaderImportWindow); }
  void OpenScriptWindow()        { OpenToolWindow(m_scriptWindow); }
  void CloseScriptWindow()       { CloseToolWindow(m_scriptWindow); }
  // ── The current preset, resolved (#12) ──
  //
  // m_szCurrentPresetFile holds either a bare filename or a full path. These
  // are the only two things any caller wants from it, and having them here
  // rather than at 30 call sites is the point: those sites had drifted, and
  // most of them stripped only '\' while the field can legitimately hold a
  // '/' -- so a forward-slash path came back from them whole, as if the
  // directory were part of the name.
  const wchar_t* CurrentPresetLeaf() const;
  void CurrentPresetPath(wchar_t* out, size_t count) const;
  // The folder the current preset lives in -- the part before the last
  // separator, or m_szPresetDir when the field holds a bare name. That
  // distinction is load-bearing for <Current> vs <Default> in the child
  // preset modes, where the folder may be a SUBfolder of m_szPresetDir.
  std::wstring CurrentPresetDir() const;

  void OpenRemoteWindow()        { OpenToolWindow(m_remoteWindow); }

  // Put any self-placing tool window back on the render window's monitor.
  // Called once a second from the idle timer; a no-op outside testing mode.
  // See ToolWindow::EnsureOnRenderMonitor for why re-checking is needed at all.
  void EnsureTestToolWindowsOnRenderMonitor() {
    if (!m_bTestingMode) return;
    if (m_remoteWindow) m_remoteWindow->EnsureOnRenderMonitor();
  }

  // Testing mode opened the Remote window, so testing mode closes it again.
  // Without the flag a run would shut a window the user had opened himself.
  bool m_bTestingOpenedRemote = false;
  void CloseRemoteWindow()       { CloseToolWindow(m_remoteWindow); }
  void OpenVisualWindow()        { OpenToolWindow(m_visualWindow); }
  void CloseVisualWindow()       { CloseToolWindow(m_visualWindow); }
  void OpenColorsWindow()        { OpenToolWindow(m_colorsWindow); }
  void CloseColorsWindow()       { CloseToolWindow(m_colorsWindow); }
  void OpenControllerWindow()    { OpenToolWindow(m_controllerWindow); }
  void CloseControllerWindow()   { CloseToolWindow(m_controllerWindow); }
  void OpenWelcomeWindow()       { OpenToolWindow(m_welcomeWindow); }
  void CloseWelcomeWindow()      { CloseToolWindow(m_welcomeWindow); }
  void OpenTextAnimWindow()      { OpenToolWindow(m_textAnimWindow); }
  void CloseTextAnimWindow();    // non-standard: resets after close
  void OpenMessagesWindow()      { OpenToolWindow(m_messagesWindow); }
  void CloseMessagesWindow();    // non-standard: resets after close
  void OpenWorkspaceLayoutWindow() { OpenToolWindow(m_workspaceLayoutWindow); }
  void CloseWorkspaceLayoutWindow() { CloseToolWindow(m_workspaceLayoutWindow); }

  // Broadcast WM_MW_REBUILD_FONTS to all windows except the sender
  void BroadcastFontSync(HWND hSender);

  // Messages tab
  bool        ShowMsgOverridesDialog(HWND hParent);
  void        PopulateMsgListBox(HWND hList);
  void        BuildMsgPlaybackOrder();
  void        WriteCustomMessages();
  void        SaveMsgAutoplaySettings();
  void        LoadMsgAutoplaySettings();
  void        ScheduleNextAutoMessage();
  void        UpdateMsgPreview(HWND hSettingsWnd, int sel);
  bool        ShowMessageEditDialog(HWND hParent, int msgIndex, bool isNew);

  // Window Title Parser popup
  void        OpenWindowTitleParserPopup(HWND hParent);

  // Sprites tab (page 6)
  struct SpriteEntry {
    int          nIndex;           // [imgNN] number (0-99999)
    wchar_t      szImg[512];       // img= path
    unsigned int nColorkey;        // colorkey hex value
    std::string  szInitCode;       // init_N lines joined with \n
    std::string  szFrameCode;      // code_N lines joined with \n
  };
  std::vector<SpriteEntry> m_spriteEntries;
  int           m_nSpriteSelected = -1;
  void*         m_hSpriteImageList = NULL; // HIMAGELIST (commctrl.h not included here)
  HWND          m_hSpriteList = NULL;
  void          LoadSpritesFromINI();
  void          SaveSpritesToINI();
  void          PopulateSpriteListView();
  void          UpdateSpriteProperties(int sel);
  void          SaveCurrentSpriteProperties();
  HBITMAP       LoadThumbnailWIC(const wchar_t* szPath, int cx, int cy);

  // Pending sprite launches (queued from message handlers, flushed during render when command list is open)
  struct PendingSprite { int nSpriteNum; int nSlot; };
  std::vector<PendingSprite> m_pendingSpriteLoads;

  // Settings window theme
  enum ThemeMode { THEME_DARK = 0, THEME_LIGHT = 1, THEME_SYSTEM = 2 };
  ThemeMode   m_nThemeMode = THEME_DARK;
  bool        IsDarkTheme() const;  // resolves THEME_SYSTEM → actual dark/light
  COLORREF    m_colSettingsBg       = RGB(30, 30, 30);       // Main window background (matches MilkVision)
  COLORREF    m_colSettingsCtrlBg   = RGB(45, 45, 45);       // Edit/combo/list background
  COLORREF    m_colSettingsText     = RGB(0, 220, 0);        // Text color (green, matches MilkVision)
  COLORREF    m_colSettingsDisabled = RGB(128, 128, 128);    // Disabled text
  COLORREF    m_colSettingsBorder   = RGB(60, 60, 60);       // Border/button face
  COLORREF    m_colSettingsBtnFace     = RGB(60, 60, 60);   // Button face
  COLORREF    m_colSettingsBtnHi       = RGB(90, 90, 90);   // 3D highlight edge (top-left)
  COLORREF    m_colSettingsBtnShadow   = RGB(35, 35, 35);   // 3D shadow edge (bottom-right)
  COLORREF    m_colSettingsHighlightText = RGB(255, 255, 255); // Selected tab text
  HBRUSH      m_hBrSettingsBg      = NULL;
  HBRUSH      m_hBrSettingsCtrlBg  = NULL;
  void        LoadSettingsThemeFromINI();
  void        CleanupSettingsThemeBrushes();

  // User "safe" defaults (persisted to INI [UserDefaults] section)
  bool  m_bUserDefaultsSaved = false;
  float m_udOpacity = 1.0f;
  float m_udRenderQuality = 1.0f;
  float m_udTimeFactor = 1.0f;
  float m_udFrameFactor = 1.0f;
  float m_udFpsFactor = 1.0f;
  float m_udVisIntensity = 1.0f;
  float m_udVisShift = 0.0f;
  float m_udVisVersion = 1.0f;
  float m_udHue = 0.0f;
  float m_udSaturation = 0.0f;
  float m_udBrightness = 0.0f;
  float m_udGamma = 2.0f;
  void  SaveUserDefaults();
  void  LoadUserDefaults();
  void  SaveFallbackPaths();
  void  LoadFallbackPaths();

  // Fallback search paths (Files tab)
  // Guarded (#11). The Settings ToolWindow thread adds and removes entries
  // while the shader-compile path (engine_shaders.cpp) and the texture loader
  // (engine_textures.cpp) iterate them -- a vector realloc under an iteration
  // on another thread. Small and infrequent, and wrong all the same.
  //
  // Read it through FallbackPathsCopy(), never inside a with(). EVERY reader
  // here probes the disk per entry -- GetFileAttributesW, or a whole directory
  // scan -- and file I/O must not happen under this lock. The vector is capped
  // at 20 short strings, so the copy costs nothing beside the probes it feeds.
  mdrop::Guarded<std::vector<std::wstring>> m_fallbackPaths;
  std::vector<std::wstring> FallbackPathsCopy() const;
  void AddFallbackPath(const std::wstring& path);
  bool RemoveFallbackPathAt(int index);   // false when the index is stale
  wchar_t m_szRandomTexDir[MAX_PATH] = {};    // Dedicated random textures directory
  wchar_t m_szContentBasePath[MAX_PATH] = {};  // Base path for textures, sprites, etc.
  // (ResetToFactory, ResetToUserDefaults, UpdateVisualUI, UpdateColorsUI,
  //  RefreshIPCList, NavigatePresetDirUp/Into moved to SettingsWindow)

  // Message autoplay (Messages tab)
  bool    m_bMsgAutoplay = false;
  bool    m_bMsgSequential = false;           // true=sequential, false=random
  float   m_fMsgAutoplayInterval = 30.0f;     // base seconds between messages
  float   m_fMsgAutoplayJitter = 5.0f;        // +/- randomness (seconds)
  bool    m_bMessageAutoSize = true;          // global: auto-fit messages to screen width
  float   m_fNextAutoMsgTime = -1.0f;         // scheduled time for next auto message
  int     m_nNextSequentialMsg = 0;           // index into playback order
  int     m_nMsgAutoplayOrder[MAX_CUSTOM_MESSAGES]; // playback order array
  int     m_nMsgAutoplayCount = 0;            // active messages in order

  // Message overrides (Overrides modal)
  bool    m_bMsgOverrideRandomFont = false;
  bool    m_bMsgOverrideRandomColor = false;
  bool    m_bMsgOverrideRandomSize = false;
  bool    m_bMsgOverrideRandomEffects = false;  // randomize bold/italic
  float   m_fMsgOverrideSizeMin = 10.0f;        // min random size (floor: 0.01)
  float   m_fMsgOverrideSizeMax = 40.0f;        // max random size (ceiling: 100)
  int     m_nMsgMaxOnScreen = 1;                // max concurrent messages (1..NUM_SUPERTEXTS)
  // Animation overrides
  bool    m_bMsgOverrideRandomPos = false;
  bool    m_bMsgOverrideRandomGrowth = false;
  bool    m_bMsgOverrideSlideIn = false;
  bool    m_bMsgOverrideRandomDuration = false;
  bool    m_bMsgOverrideShadow = false;
  bool    m_bMsgOverrideBox = false;
  // Color shifting overrides
  bool    m_bMsgOverrideApplyHueShift = false;
  bool    m_bMsgOverrideRandomHue = false;
  bool    m_bMsgIgnorePerMsgRandom = false;

  // Resource Viewer
  HWND        m_hResourceWnd = NULL;
  HWND        m_hResourceList = NULL;
  static LRESULT CALLBACK ResourceViewerWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
  void        OpenResourceViewer();
  void        PopulateResourceViewer();
  void        LayoutResourceViewer();

  //void  ResetWindowSizeOnDisk();
  bool		LaunchSprite(int nSpriteNum, int nSlot);
  void		KillSprite(int iSlot);
  int         GetNextFreeSupertextIndex();
  void        DoCustomSoundAnalysis();
  void        DX12_DrawMotionVectors();

  bool        LoadShaders(PShaderSet* sh, CState* pState, bool bTick, bool bCompileOnly);
  void        UvToMathSpace(float u, float v, float* rad, float* ang);
  // Fill a RenderContext's animation inputs. Two overloads because the two
  // surfaces genuinely disagree about what time it is; keeping both formulas
  // here is what stops them drifting apart at nine call sites.
  // Point a context at the preset pipeline it should draw with. Both callers
  // pass the ENGINE's set today, so this changes nothing yet -- it is what
  // makes a context's own set expressible (#184).
  void        FillPresetPipeline(RenderContext& rc);
  // A mirror surface draws with ITS OWN compiled set once adoption has built
  // one, and with the engine's until then -- the first frames after startup,
  // before any bundle has been adopted.
  void        FillPresetPipeline(RenderContext& rc, const MirrorSimContext& c);
  void        FillAnimInputs(RenderContext& rc);
  void        FillAnimInputs(RenderContext& rc, const MirrorSimContext& c);
  // Canvas, aspect and frame for one surface. Split from FillAnimInputs
  // because the size is known at the CALL SITE (a pipe's w/h, the primary's
  // texsize) while the animation inputs come from a context.
  void        FillCanvasInputs(RenderContext& rc, int w, int h, int frame);
  // The mouse this surface should report, rescaled from the primary's
  // pixels to a canvas of w x h so mouse/texsize is preserved.
  void        FillMouseInputs(RenderContext& rc, int w, int h);
  void        ApplyShaderParams(CShaderParams* p, LPD3DXCONSTANTTABLE pCT, CState* pState,
                                const RenderContext* rc = nullptr);
  // bNoPrimaryFallback: when true, never fall back to primary m_dx12Feedback* (orient pipe).
  void        BuildBindingSlots(CShaderParams* params, const DX12Texture& vsTex, UINT outSlots[32], const DX12Texture* feedbackTex = nullptr, const DX12Texture* imageFeedbackTex = nullptr, const DX12Texture* bufferBTex = nullptr, const DX12Texture* bufferCTex = nullptr, const DX12Texture* bufferDTex = nullptr, bool bNoPrimaryFallback = false);
  // ── Binding-slot snapshots (DIAG_BINDINGS over the pipe) ─────────────────
  // The primary fills a per-frame descriptor block through
  // UpdatePerFrameBindings; the mirror rebuilds its own blocks every frame at
  // the surface pipe's bindBase from the SAME CShaderParams. When a disk texture
  // resolves on one path and lands on m_fallbackTexture on the other, nothing
  // in any existing log shows it — the fallback is bound silently. Record what
  // each path actually built so the two can be diffed side by side.
  //
  // Written under m_mirrorEngineMutex (both the primary frame and the mirror
  // record hold it), read unsynchronised by the IPC thread — diagnostic only,
  // same contract as DIAG_MIRRORS.
  struct BindSnapshot {
    UINT slots[32];       // resolved SRV heap index per t-register
    UINT bindingSrv[32];  // params->m_texture_bindings[i].dx12SrvIndex
    int  texcode[32];     // tex_code per t-register
    bool valid;
  };
  enum { BINDSNAP_WARP = 0, BINDSNAP_COMP, BINDSNAP_OLDWARP, BINDSNAP_OLDCOMP,
         BINDSNAP_COUNT };
  BindSnapshot m_bindSnapPrimary[BINDSNAP_COUNT] = {};
  BindSnapshot m_bindSnapMirror[BINDSNAP_COUNT] = {};
  static void CaptureBindSnapshot(BindSnapshot& dst, const CShaderParams* params,
                                  const UINT slots[32]);

  // Live fallback-texture swap requested over the pipe (SET_FALLBACK_TEX).
  // Building the texture uses the shared upload command list and the main
  // queue, so the IPC thread only parks the request — MyRenderFn applies it.
  std::atomic<bool> m_bFallbackTexRefreshPending{ false };
  int               m_nPendingFallbackStyle = 0;
  wchar_t           m_szPendingFallbackFile[MAX_PATH] = {};

  bool        AddNoiseTex(const wchar_t* szTexName, int size, int zoom_factor);
  bool        AddNoiseVol(const wchar_t* szTexName, int size, int zoom_factor);
  bool        AddNoiseTex_ST(const wchar_t* szTexName, int size);
  bool        AddNoiseVol_ST(const wchar_t* szTexName, int size);

  //====[ 3. virtual functions: ]===========================================================================

  virtual void OverrideDefaults();
  virtual void MyPreInitialize();
  virtual void MyReadConfig();
  virtual void MyWriteConfig();
  void SaveWindowSizeAndPosition(HWND hwnd);
  virtual int  AllocateMyNonDx9Stuff();
  virtual void  CleanUpMyNonDx9Stuff();
  virtual int  AllocateMyDX9Stuff();
  virtual void  CleanUpMyDX9Stuff(int final_cleanup);

  // Final, once-per-process release of every GPU object the engine owns.
  // Called from WinMain only -- NEVER from CleanUpMyDX9Stuff, PluginQuit or any
  // device-recovery path. See the comment on the definition in engine.cpp.
  void          ReleaseGpuResources();
  virtual void MyRenderFn(int redraw);
  virtual void MyRenderUI(int* upper_left_corner_y, int* upper_right_corner_y, int* lower_left_corner_y, int* lower_right_corner_y, int xL, int xR);
  void ToggleAlwaysOnTop(HWND hwnd);
  void SetOpacity(HWND hwnd);
  bool IsBorderlessFullscreen(HWND hWnd);
  virtual LRESULT MyWindowProc(HWND hWnd, unsigned uMsg, WPARAM wParam, LPARAM lParam);
  void KillAllSprites();
  void KillAllSupertexts();
  bool ChangePresetDir(wchar_t* newDir, wchar_t* oldDir);
  int ToggleSpout();
  int SetSpoutFixedSize(bool toggleSwitch, bool showNotifications);
  virtual void OnAltK();
};

} // namespace mdrop

#endif
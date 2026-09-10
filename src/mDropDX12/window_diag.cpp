#include "window_diag.h"

#include "format_to.h"
#include "pipe_server.h"

#include <wincodec.h>
#include <algorithm>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

// Global scope, not inside mdrop -- that is where it is defined, and declaring
// it in the namespace links against a different symbol that does not exist.
extern PipeServer g_pipeServer;

namespace mdrop {

namespace {

std::wstring Lower(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](wchar_t c) { return (wchar_t)towlower(c); });
  return s;
}

struct FindCtx {
  std::wstring want;      // already lowercased
  HWND exact = NULL;
  HWND prefix = NULL;
};

BOOL CALLBACK FindProc(HWND hWnd, LPARAM lp) {
  FindCtx* ctx = (FindCtx*)lp;
  DWORD pid = 0;
  GetWindowThreadProcessId(hWnd, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(hWnd)) return TRUE;

  wchar_t title[256] = {};
  GetWindowTextW(hWnd, title, 256);
  if (!title[0]) return TRUE;
  const std::wstring t = Lower(title);
  if (t == ctx->want) { ctx->exact = hWnd; return FALSE; }
  if (!ctx->prefix && t.rfind(ctx->want, 0) == 0) ctx->prefix = hWnd;
  return TRUE;
}

// Escapes the field separator so a control's text cannot forge a new field.
// A preset name really can contain '|', and a caller splitting on it would
// otherwise read one control as several.
//
// The pipe is escaped as \| and NOT as \p, which is what this used first and
// which is a trap: almost every string in this program is a Windows path, and
// "\presets" contains \p. A decoder that unescapes backslashes and then pipes
// -- the obvious two-pass way, and the way the first consumer of this really
// was written -- turned
//
//     C:\resources\presets\   into   C:\resources|resets\
//
// which looks exactly like the app truncating a buffer, in a dump whose whole
// purpose is finding truncated strings. \| cannot occur in a path, so even a
// naive decoder gets it right.
std::wstring Escape(const std::wstring& s) {
  std::wstring out;
  out.reserve(s.size());
  for (wchar_t c : s) {
    if (c == L'|') out += L"\\|";
    else if (c == L'\\') out += L"\\\\";
    else if (c == L'\r') out += L"\\r";
    else if (c == L'\n') out += L"\\n";
    else out += c;
  }
  return out;
}

// Windows strips a single '&' as a mnemonic prefix before drawing, so measuring
// the raw string measures a character that is never painted. '&&' is one
// literal ampersand and does count.
std::wstring StripMnemonic(const std::wstring& s) {
  std::wstring out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == L'&') {
      if (i + 1 < s.size() && s[i + 1] == L'&') { out += L'&'; i++; }
      continue;
    }
    out += s[i];
  }
  return out;
}

// How wide the control's own text is, and whether the control is wide enough
// for it (issue 159).
//
// The one thing a rectangle dump cannot infer. GetWindowTextW hands back the
// whole string whatever the control manages to paint, so a label cut in half
// reads here as perfectly correct -- which is how "Preset Name Profile:" drawn
// as "Preset Name" survived a pass whose entire purpose was finding truncated
// strings. It took a screenshot to see, and a screenshot is what this dump
// exists to make unnecessary.
//
// Measured against what THIS app's drawing code actually does, which is the
// reason it has to run in-process. Every button, check box and radio here is
// BS_OWNERDRAW -- the dark theme paints them itself -- so the style bits say
// "owner draw" and nothing more; the type is a window PROPERTY (IsCheckbox,
// IsRadio, IsPinBtn), and the text rectangle each one draws into is reproduced
// below from DrawOwnerCheckbox, DrawOwnerRadio and DrawOwnerButton.
struct TextMetric {
  // The width the CONTROL would need to hold this text on one line -- the text
  // itself plus whatever the control's own chrome takes before the text starts.
  //
  // Reported that way, and not as the bare text width, so the verdict can be
  // rechecked from the dump alone: textfits is 0 exactly when tw exceeds w.
  // A check box's glyph is 5 + max(h/2, 11) wide and lives inside the same
  // rectangle, so a bare text width made "Out" in a 55px radio look like it had
  // 19 pixels to spare while it was in fact being drawn as "Ou".
  int width = 0;
  int fits  = -1;  // 1 yes, 0 no, -1 not judged
};

TextMetric MeasureText(HWND hCtrl, const wchar_t* cls, const wchar_t* text) {
  TextMetric tm;
  if (!text || !text[0]) return tm;

  const LONG style = (LONG)GetWindowLongPtrW(hCtrl, GWL_STYLE);
  const bool isStatic = _wcsicmp(cls, L"Static") == 0;
  const bool isButton = _wcsicmp(cls, L"Button") == 0;

  std::wstring s = text;
  if (isButton || (isStatic && !(style & SS_NOPREFIX))) s = StripMnemonic(s);

  RECT rc = {};
  GetClientRect(hCtrl, &rc);
  const int ctrlW = rc.right, ctrlH = rc.bottom;

  HDC hdc = GetDC(hCtrl);
  if (!hdc) return tm;
  HFONT hf = (HFONT)SendMessageW(hCtrl, WM_GETFONT, 0, 0);
  HFONT hOld = hf ? (HFONT)SelectObject(hdc, hf) : NULL;

  SIZE sz = {};
  GetTextExtentPoint32W(hdc, s.c_str(), (int)s.size(), &sz);
  tm.width = sz.cx;

  TEXTMETRICW met = {};
  GetTextMetricsW(hdc, &met);

  // Judged only where the string is OURS and the box is ours to size. An edit
  // or a combo holds the user's data and scrolls it, so a value too long for
  // the field is not the same kind of fact and would drown this in noise; the
  // width above is reported for those anyway, so a reader can still judge.
  int reserve = -1;
  bool wraps = false;
  if (isStatic) {
    const LONG type = style & SS_TYPEMASK;
    // A static that declares an ellipsis style has SAID it will not fit
    // everything and has a graceful answer for that -- a device name, a
    // path. Judging it would report a design decision as a defect, and
    // those cases are exactly the ones no wording change can fix.
    const bool elides = (style & (SS_ENDELLIPSIS | SS_PATHELLIPSIS |
                                 SS_WORDELLIPSIS)) != 0;
    if (elides) {
      reserve = -1;
    } else if (type == SS_LEFT || type == SS_CENTER || type == SS_RIGHT) {
      reserve = 0;
      // An SS_LEFT static word-wraps, so one tall enough for a second line is
      // not truncated by a string wider than itself -- it is doing what it was
      // made that tall for. Only DrawText knows where the breaks fall.
      wraps = ctrlH >= met.tmHeight * 2;
    } else if (type == SS_SIMPLE || type == SS_LEFTNOWORDWRAP) {
      reserve = 0;
    }
  } else if (isButton && !GetPropW(hCtrl, L"IsPinBtn")) {
    // DrawOwnerCheckbox and DrawOwnerRadio both put the text at
    // rc.left + 1 + max(h/2, 11) + 4; DrawOwnerButton draws into the whole rect.
    reserve = (GetPropW(hCtrl, L"IsCheckbox") || GetPropW(hCtrl, L"IsRadio"))
                  ? 5 + max(ctrlH / 2, 11)
                  : 0;
  }

  // Two pixels of overhang is not a truncation -- the same slack, for the same
  // reason, that FitToContents allows on a control hanging past the client
  // edge. GetTextExtentPoint32W returns the sum of the glyph ADVANCES, which is
  // not exactly the ink DrawText lays down, and a centred button string one
  // pixel too wide loses half a pixel at each end. Reporting those as cut off
  // would bury the ones that really are -- and `tw` is exact either way, so
  // nothing is hidden by the tolerance.
  const int kSlack = 2;

  if (reserve >= 0) {
    tm.width += reserve;
    if (wraps) {
      RECT calc = { 0, 0, ctrlW, 0 };
      DrawTextW(hdc, s.c_str(), (int)s.size(), &calc,
                DT_CALCRECT | DT_WORDBREAK | DT_LEFT);
      // A wrapping static is judged on HEIGHT: tw stays the single-line width,
      // which is what it would need in order NOT to wrap.
      tm.fits = (calc.bottom <= ctrlH + kSlack) ? 1 : 0;
    } else {
      tm.fits = (tm.width <= ctrlW + kSlack) ? 1 : 0;
    }
  }

  if (hOld) SelectObject(hdc, hOld);
  ReleaseDC(hCtrl, hdc);
  return tm;
}

struct DumpCtx {
  HWND parent;
  int  index = 0;
};

BOOL CALLBACK DumpProc(HWND hCtrl, LPARAM lp) {
  DumpCtx* ctx = (DumpCtx*)lp;

  wchar_t cls[64] = {};
  GetClassNameW(hCtrl, cls, 64);

  // 512 rather than a name-sized buffer: an edit control on these windows can
  // hold a whole shader, and a truncated dump would read as a truncated
  // CONTROL, which is the bug this exists to find.
  wchar_t text[512] = {};
  GetWindowTextW(hCtrl, text, 512);

  RECT r = {};
  GetWindowRect(hCtrl, &r);
  POINT tl = { r.left, r.top };
  ScreenToClient(ctx->parent, &tl);

  RECT rcParent = {};
  GetClientRect(ctx->parent, &rcParent);
  const int w = r.right - r.left, h = r.bottom - r.top;
  const bool clipped = (tl.x + w > rcParent.right) || (tl.y + h > rcParent.bottom) ||
                       tl.x < 0 || tl.y < 0;

  // The codepoints, not just the text. A glyph button holding a mangled escape
  // still prints as SOMETHING; only the codepoints show that U+E8E5 arrived as
  // U+00C3 U+00A8 U+0045 U+0035.
  std::wstring codes;
  for (int i = 0; text[i] && i < 8; i++) {
    wchar_t buf[8];
    FormatTo(buf, L"%s%04X", i ? L" " : L"", (unsigned)text[i]);
    codes += buf;
  }

  const TextMetric tm = MeasureText(hCtrl, cls, text);

  wchar_t head[384];
  FormatTo(head,
           L"WINDOW_CTRL|i=%d|class=%s|id=%d|x=%d|y=%d|w=%d|h=%d"
           L"|vis=%d|enab=%d|clip=%d|tw=%d|textfits=%d|u=",
           ctx->index++, cls,
           (int)GetWindowLongPtrW(hCtrl, GWLP_ID),   // GetMenu is for menus
           tl.x, tl.y, w, h,
           IsWindowVisible(hCtrl) ? 1 : 0, IsWindowEnabled(hCtrl) ? 1 : 0,
           clipped ? 1 : 0, tm.width, tm.fits);
  std::wstring line = head;
  line += codes;
  line += L"|text=";
  line += Escape(text);
  g_pipeServer.Send(line.c_str());
  return TRUE;
}

}  // namespace

HWND FindToolWindowByTitle(const std::wstring& title) {
  FindCtx ctx;
  ctx.want = Lower(title);
  EnumWindows(FindProc, (LPARAM)&ctx);
  return ctx.exact ? ctx.exact : ctx.prefix;
}

void SendWindowControlDump(const std::wstring& title) {
  HWND hWnd = FindToolWindowByTitle(title);
  if (!hWnd) {
    g_pipeServer.Send(L"WINDOW_CTRLS=NOTFOUND");
    return;
  }
  wchar_t got[256] = {};
  GetWindowTextW(hWnd, got, 256);
  RECT rc = {};
  GetClientRect(hWnd, &rc);

  wchar_t head[512];
  FormatTo(head, L"WINDOW_CTRLS|title=%s|client=%dx%d", got, rc.right, rc.bottom);
  g_pipeServer.Send(head);

  DumpCtx ctx{ hWnd, 0 };
  EnumChildWindows(hWnd, DumpProc, (LPARAM)&ctx);
  g_pipeServer.Send(L"WINDOW_CTRLS_END");
}

void SendWindowCapture(const std::wstring& title, const std::wstring& baseDir) {
  HWND hWnd = FindToolWindowByTitle(title);
  if (!hWnd) {
    g_pipeServer.Send(L"WINDOW_CAPTURE=NOTFOUND");
    return;
  }

  RECT rc = {};
  GetClientRect(hWnd, &rc);
  const int w = rc.right, h = rc.bottom;
  if (w <= 0 || h <= 0) {
    g_pipeServer.Send(L"WINDOW_CAPTURE=ERROR|reason=empty-client");
    return;
  }

  HDC hdcWin = GetDC(hWnd);
  HDC hdcMem = CreateCompatibleDC(hdcWin);
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;              // top-down, so WIC needs no flip
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP hbm = CreateDIBSection(hdcWin, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  HGDIOBJ old = hbm ? SelectObject(hdcMem, hbm) : NULL;

  bool ok = false;
  if (hbm) {
    // PW_RENDERFULLCONTENT is the whole reason this runs in-process. Without
    // it, owner-drawn buttons -- the pin, the refresh, every glyph button --
    // are simply not painted, and the capture comes back clean and wrong.
    ok = PrintWindow(hWnd, hdcMem, PW_CLIENTONLY | PW_RENDERFULLCONTENT) != 0;
    if (!ok) ok = PrintWindow(hWnd, hdcMem, PW_CLIENTONLY) != 0;
  }

  std::wstring path;
  if (ok) {
    wchar_t dir[MAX_PATH];
    FormatTo(dir, L"%scapture\\", baseDir.c_str());
    CreateDirectoryW(dir, NULL);

    wchar_t safe[64] = {};
    {
      int j = 0;
      for (size_t i = 0; i < title.size() && j < 63; i++) {
        const wchar_t c = title[i];
        safe[j++] = (iswalnum(c) ? c : L'_');
      }
      safe[j] = 0;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t full[MAX_PATH];
    FormatTo(full, L"%s%04d%02d%02d-%02d%02d%02d-window-%s.png", dir,
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, safe);
    path = full;

    IWICImagingFactory* factory = nullptr;
    ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))) && factory;
    if (ok) {
      IWICStream* stream = nullptr;
      IWICBitmapEncoder* encoder = nullptr;
      IWICBitmapFrameEncode* frame = nullptr;
      ok = SUCCEEDED(factory->CreateStream(&stream)) &&
           SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
           SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
           SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(w, h));
      if (ok) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
             SUCCEEDED(frame->WritePixels(h, w * 4, w * 4 * h, (BYTE*)bits)) &&
             SUCCEEDED(frame->Commit()) &&
             SUCCEEDED(encoder->Commit());
      }
      if (frame) frame->Release();
      if (encoder) encoder->Release();
      if (stream) stream->Release();
      factory->Release();
    }
  }

  if (old) SelectObject(hdcMem, old);
  if (hbm) DeleteObject(hbm);
  DeleteDC(hdcMem);
  ReleaseDC(hWnd, hdcWin);

  if (ok) {
    std::wstring reply = L"WINDOW_CAPTURE_PATH=" + path;
    g_pipeServer.Send(reply.c_str());
  } else {
    g_pipeServer.Send(L"WINDOW_CAPTURE=ERROR|reason=capture-failed");
  }
}

bool HandleWindowDiagIPC(const wchar_t* sMessage, const std::wstring& baseDir) {
  if (!sMessage) return false;

  // Every control on one of our own tool windows: class, id, rect, visibility,
  // whether the client edge clips it, its text AND that text's codepoints.
  // The codepoints are the point -- a button whose glyph escape was mangled
  // into the system codepage still prints as something, and only the
  // codepoints show U+E8E5 arriving as U+00C3 U+00A8 U+0045 U+0035.
  if (wcsncmp(sMessage, L"DIAG_WINDOW=", 12) == 0) {
    SendWindowControlDump(sMessage + 12);
    return true;
  }
  if (wcsncmp(sMessage, L"CAPTURE_WINDOW=", 15) == 0) {
    SendWindowCapture(sMessage + 15, baseDir);
    return true;
  }
  return false;
}

}  // namespace mdrop

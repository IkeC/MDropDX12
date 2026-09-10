#pragma once

#include <windows.h>
#include <string>

// Looking at a tool window from outside the process (issue 154).
//
// Everything else about this app is drivable over the pipe, and its own
// windows were the blind spot. Two things kept going wrong and neither was
// visible to the harness:
//
//   - a control holding the WRONG TEXT. The Presets window's Browse buttons
//     came up reading "Ã¨E5" because a glyph escape had been mangled into the
//     system codepage. Nothing outside the process could see that.
//   - a control in the WRONG PLACE, or clipped. A button positioned past the
//     client edge looks identical to one that was never created.
//
// An external screenshot cannot answer either. PrintWindow from another
// process does not render owner-drawn buttons at all -- the pin and refresh
// glyphs in every tool window's title row come back as flat background -- so a
// capture taken from outside is blind to exactly the controls most likely to be
// wrong, and blind in a way that LOOKS like a clean result.
//
// Taken from inside, with PW_RENDERFULLCONTENT, it renders.

namespace mdrop {

// The top-level window of THIS process whose title matches, case-insensitively.
// Matches a whole title first, then a prefix, so "Preset" finds "Presets" but
// an exact "Presets" is never beaten by a longer name. NULL if nothing matches.
HWND FindToolWindowByTitle(const std::wstring& title);

// Sends one WINDOW_CTRL| line per child control, then WINDOW_CTRLS_END.
// Reports WINDOW_CTRLS=NOTFOUND if no window matches.
//
//   WINDOW_CTRL|i=|class=|id=|x=|y=|w=|h=|vis=|enab=|clip=|tw=|textfits=|u=|text=
//
// clip=1     the control's RECTANGLE extends past the client edge (issue 157)
// tw=        the width the CONTROL would need to hold its text on one line,
//            its own chrome included -- so textfits is 0 exactly when tw > w
// textfits=  0 when the text does not fit the control it is in (issue 159),
//            1 when it does, -1 where the question is not asked -- an edit or
//            a combo holds the user's data and scrolls it, which is not the
//            same kind of fact and would drown the signal
//
// Those two are different failures and neither implies the other: a button in
// the right place can still be drawn as "Ou" instead of "Out".
void SendWindowControlDump(const std::wstring& title);

// Captures the window to a PNG under capture\ and sends WINDOW_CAPTURE_PATH=.
// Reports WINDOW_CAPTURE=ERROR with a reason on failure.
void SendWindowCapture(const std::wstring& title, const std::wstring& baseDir);

// Handles DIAG_WINDOW= and CAPTURE_WINDOW=; true when it took the message.
//
// A dispatcher of its own, consulted beside HandleDisplayModeIPC and the rest,
// rather than two more arms on LaunchMessage's else-if chain. That is not a
// preference: adding them there produced
//
//     error C1061: compiler limit: blocks nested too deeply
//
// so that chain is AT the compiler's limit and cannot take another arm. Any
// new IPC command needs a small handler like this one.
bool HandleWindowDiagIPC(const wchar_t* sMessage, const std::wstring& baseDir);

}  // namespace mdrop

#pragma once
// display_panel.h — the Displays list, drawn as the monitors actually sit.
//
// A list box says "\\.\DISPLAY3 (CHILD) something.milk" and leaves you to work
// out which physical screen that is. The panel draws the desktop arrangement to
// scale, the way the Windows display settings page does, so the screen on your
// left is the tile on the left. State is a colour and a word rather than a
// parenthesis, and the selected tile is the one with the bright border.
//
// It does NOT own the selection. Clicking a tile sets the list box's selection
// and lets the existing LBN_SELCHANGE path run, so every control in the
// "Selected display" group below keeps working with no knowledge of this file.
// The two views are the same list, drawn twice.
//
// Spout outputs have no place on a desktop, so they are not given one: they are
// drawn on their own row underneath, each a rectangle in its configured aspect
// ratio, because for a Spout sender the interesting geometry is its dimensions
// and nothing else.

#ifndef DISPLAY_PANEL_H
#define DISPLAY_PANEL_H

#include <windows.h>
#include <vector>

namespace mdrop {

class Engine;

class DisplayPanel {
public:
    bool Create(HWND hParent, Engine* pEngine, int ctrlID,
                int x, int y, int w, int h);
    HWND GetHWND() const { return m_hWnd; }
    void Invalidate() { if (m_hWnd) InvalidateRect(m_hWnd, NULL, FALSE); }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void OnPaint(HDC hdc);
    int  HitTest(int x, int y) const;   // index into m_displayOutputs, or -1

    // Where each output ended up, recomputed on every paint so a monitor being
    // plugged in or a window resize needs no invalidation of its own.
    struct Tile { RECT rc; int outputIndex; };
    std::vector<Tile> m_tiles;

    Engine* m_pEngine = nullptr;
    HWND    m_hWnd = NULL;
    int     m_nCtrlID = 0;
    static bool s_bClassRegistered;
};

} // namespace mdrop

#endif // DISPLAY_PANEL_H

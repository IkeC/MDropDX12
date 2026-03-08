# MDropDX12 v1.5.0

## What's New

- **Named Pipe IPC** — Replaced WM_COPYDATA / hidden window IPC with Named Pipes (`\\.\pipe\Milkwave_<PID>`). PID-based discovery eliminates fragile window-title matching and removes the hidden 1x1 IPC window entirely.
- **Second-instance forwarding via pipe** — Double-clicking a .milk/.milk2 file forwards the preset path to the running instance over the named pipe instead of WM_COPYDATA.
- **Animated song title rendering** — DX12 warped text animation for song titles with selectable track info sources.

## IPC Changes

The IPC system has been completely rewritten. Milkwave Remote now discovers visualizers by enumerating `\\.\pipe\Milkwave_*` pipes and connecting by PID. All existing text message formats (`MSG|...`, `PRESET=...`, `WAVE|...`, `STATE`, etc.) are unchanged — only the transport layer has changed from WM_COPYDATA to Named Pipes.

Key benefits:

- No hidden IPC window (eliminates the 1x1 visible artifact)
- Deterministic PID-based discovery (no window title ambiguity)
- Duplex communication (visualizer can send messages back to Remote)
- Non-blocking writes (no SendMessage blocking the render pump)

**Requires Milkwave Remote 3.7+ for Named Pipe support.**

## Installation

Download `MDropDX12-1.5.0-Portable.zip`, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/v1.5/docs/Changes.md) for the complete list of changes.

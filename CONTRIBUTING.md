# Contributing to SYSCORE

Thanks for your interest! SYSCORE is a small, dependency-free native Windows app —
contributions that keep it lean are very welcome.

## Ground rules
- **No new dependencies.** SYSCORE links only against system DLLs that ship with
  Windows / MinGW (GDI+, PDH, WinHTTP, DXGI, etc.). No package managers, no vendored
  libraries. If a feature seems to need one, open an issue first to discuss alternatives.
- **MinGW-w64 g++** is the reference toolchain (no MSVC-only code). Target Windows 10/11 x64.
- Keep the binary small and the idle CPU/RAM low — it's a *system monitor*, it should be
  a good citizen.

## Building
```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1   # -> build\syscore.exe
# or
cmake -S . -B build -G "MinGW Makefiles" && cmake --build build
```

## Project layout
| File | Responsibility |
|------|----------------|
| `src/app.h` | data model, theme, config struct, shared declarations |
| `src/metrics.{h,cpp}` | all metric collection (CPU/MEM/GPU/disk/net/procs) |
| `src/renderer.{h,cpp}` | GDI+ HUD drawing (compact + expanded layouts) |
| `src/config.cpp` | JSON config load/save (`%APPDATA%\SYSCORE\config.json`) |
| `src/ai.{h,cpp}` | async "Ask your PC" over WinHTTP |
| `src/main.cpp` | window, message loop, input, menu, hotkeys |

See `CLAUDE.md` for the platform gotchas (localized PDH counters, MinGW wide-printf,
GDI+ lifetimes, layered-window premultiplied alpha, etc.) — please read it before
diving in.

## Pull requests
- One focused change per PR. Describe what and why.
- Verify the app builds and runs (a before/after screenshot for visual changes is gold).
- Match the surrounding code style.

## Good first issues
Themes, additional metrics, per-process detail views, accessibility, and packaging are
all great places to start. Check the open issues for `good first issue` labels.

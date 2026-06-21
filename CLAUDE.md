# CLAUDE.md — SYSCORE

Guidance for working in this repo. Read this before editing.

## ⛔ Hard constraints (read first)
- **Do NOT install anything.** No pip / npm / vcpkg / cargo / winget / choco, no new
  compilers, no third-party libraries. The user's disk is critically low. Build only
  with what's already present and link against system DLLs that ship with MinGW.
- **Toolchain is MinGW-w64 g++ only** (`C:\mingw64\bin\g++.exe`, tested 15.2.0). No MSVC
  is available. Target Windows 10/11 x64.
- No network access is required to build. If something *seems* to need a new dependency,
  find a Win32/stdlib alternative or ask — don't add a library.

## What this is
**SYSCORE** — a native C++ floating system-monitor HUD (Win32 + GDI+ + PDH).
Always-on-top, frameless, **per-pixel-alpha translucent** panel. Two modes: a small
**compact bar** that toggles to a full **expanded dashboard**. Modern minimal dark theme
(high-contrast white on slate, per-metric accent colors, ring gauge, smooth bars, live
sparklines). ~900 KB statically-linked exe, ~18 MB RAM, no runtime.

## Build & run
```powershell
# build  (-> build\syscore.exe)
powershell -ExecutionPolicy Bypass -File .\build.ps1
# run (builds if needed, then launches)
.\run.ps1
```
Raw g++ line (what build.ps1 runs):
```
g++ -std=c++17 -O2 -municode -mwindows -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 ^
    -static -static-libgcc -static-libstdc++ src\*.cpp -o build\syscore.exe ^
    -lgdiplus -lgdi32 -luser32 -lpdh -lpowrprof -ldxgi -lpsapi -lole32 -loleaut32 -ladvapi32 -lshell32 -lwinhttp
```
CMake alternative (also MinGW): `cmake -S . -B build -G "MinGW Makefiles" && cmake --build build`.

There is **no unit-test suite**; verify changes by running the app and screenshotting
(see *Verifying* below).

## Architecture
| File | Responsibility |
|---|---|
| `src/app.h` | Shared data model (`Metrics` + sub-structs), `History` ring buffer, `Theme` (colors + `load`/`value`/`lerp` helpers), `Config`. Also the NOMINMAX + Gdiplus min/max include fix. |
| `src/metrics.{h,cpp}` | `MetricsEngine` — collects every metric once per tick into a `Metrics` snapshot. |
| `src/renderer.{h,cpp}` | `Renderer` — GDI+ drawing into a premultiplied-ARGB DIB; `drawCompact` + `drawExpanded` layouts, widgets, and the AI result overlay. |
| `src/config.cpp` | hand-rolled JSON config load/save at `%APPDATA%\SYSCORE\config.json` (no JSON library). |
| `src/ai.{h,cpp}` | async "Ask your PC" — WinHTTP POST on a worker thread, hand-rolled JSON build/parse, Anthropic + OpenAI formats. |
| `src/main.cpp` | `wWinMain`, layered window, message loop, timers, drag/expand/menu/wheel/hotkeys, DPI, config load/save. |

**Data flow:** `TIMER_DATA` (1 Hz) → `engine.sample(g_metrics)`. `TIMER_RENDER`
(10/20/30/60 fps) → `present()` → `renderer.draw(g_metrics, cfg, t)` →
`UpdateLayeredWindow`. Single global instances: `g_engine`, `g_render`, `g_metrics`, `g_cfg`.

## Data sources
| Metric | API |
|---|---|
| CPU per-core usage | `NtQuerySystemInformation(SystemProcessorPerformanceInformation)` |
| CPU clock | `CallNtPowerInformation(ProcessorInformation)` |
| Memory | `GlobalMemoryStatusEx` + `GetPerformanceInfo` |
| GPU load | PDH `\GPU Engine(*)\Utilization Percentage` (summed per engine type) |
| VRAM | PDH `\GPU Adapter Memory(*)\Dedicated/Shared Usage` + DXGI (name/budget) |
| Disk | PDH `\PhysicalDisk(*)\…` + `GetDiskFreeSpaceEx` |
| Network | PDH `\Network Interface(*)\Bytes Received/Sent/sec` |
| Processes | `NtQuerySystemInformation(SystemProcessInformation)` |

## ⚠️ Platform gotchas (hard-won — keep these in mind when editing)
1. **Localized Windows.** This machine runs Korean Windows. PDH counter paths MUST be
   added with `PdhAddEnglishCounterA` (English names), never `PdhAddCounterA`, or they
   fail on non-English locales.
2. **MinGW wide `printf`.** `swprintf(L"%s", wchar_t*)` prints only the first character
   (MinGW ANSI stdio treats `%s` as `char*`). Always use `%ls` for `wchar_t*` in wide
   format strings.
3. **GDI+ + MinGW min/max.** `#define NOMINMAX`, then
   `namespace Gdiplus { using std::min; using std::max; }` *before* `<gdiplus.h>` — else
   libstdc++ headers break. (Already set up in `app.h`.)
4. **GDI+ object lifetime.** `Renderer` is a global, so its constructor runs *before*
   `GdiplusStartup` — it must NOT touch GDI+. GDI+ setup lives in `Renderer::init()`
   (called after `GdiplusStartup`); teardown in `Renderer::shutdown()` (called *before*
   `GdiplusShutdown`). Don't move GDI+ work into the ctor/dtor.
5. **Layered window.** Per-pixel alpha via `UpdateLayeredWindow` needs a top-down 32-bpp
   DIB with **premultiplied** alpha. We draw straight ARGB then `premultiply()` before
   present. Global opacity is `BLENDFUNCTION.SourceConstantAlpha`, not baked into pixels.
6. **Hand-declared kernel structs.** `SYSTEM_PROCESS_INFORMATION`,
   `SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION`, `PROCESSOR_POWER_INFORMATION`, and
   `MYUNICODE_STRING` are hand-declared with x64 layout in `metrics.cpp`. Do NOT include
   `<winternl.h>` (it clashes). If you touch these, preserve the exact field order/sizes.
7. **DXGI.** `IID_IDXGIFactory1` is defined locally so we need neither `__uuidof` nor
   `-ldxguid`.
8. **Temperatures.** AMD integrated GPU / this laptop expose no temp sensor without
   admin/vendor driver. `tempC < 0` means "unavailable" and the UI shows `--`. Never
   assume a temp exists.
9. **Window placement.** Frameless + `WS_EX_TOOLWINDOW` = no taskbar button, so an
   off-screen window is unrecoverable. `present()` calls `clampToWorkArea()` (monitor-aware)
   on every frame; `WM_DPICHANGED` honors the OS-suggested rect; `WM_CAPTURECHANGED` resets
   drag state. Keep these intact when changing window/drag logic.
10. **AI client.** `ai.cpp` uses `winhttp.dll` (link `-lwinhttp`) on a detached worker
    thread; results flow back through a mutex-guarded state read each frame via `aiSnapshot()`.
    JSON is **hand-rolled** (no library) — `jesc()` to build, `extractStr()` to parse the
    assistant text (Anthropic `content[].text`, OpenAI `choices[].message.content`). The API
    key is only in `%APPDATA%\SYSCORE\config.json`; never log it. Prompt/metrics strings are
    UTF-8 `std::string` (not wide) so the usual `%ls` rule doesn't apply inside `ai.cpp`.

## License
MIT (`LICENSE`). Keep new files dependency-free and compatible.

## Controls (for reference / testing)
Double-click or `E` = expand⇄collapse · drag = move · mouse-wheel = opacity ·
right-click = menu (always-on-top, top-processes, opacity, refresh rate, quit) · `Esc` = quit.

## Verifying changes (no test suite)
Build, run, and screenshot via PowerShell `System.Drawing` + `System.Windows.Forms`:
- Window class is `"SyscoreHUD"`. Find it by `EnumWindows` + `GetClassNameW` (match the
  class), then `GetWindowRect` + `CopyFromScreen`.
- Toggle expand for the dashboard shot: `PostMessageW(hwnd, WM_KEYDOWN /*0x100*/, 'E' /*0x45*/, 0)`.
- Test clean exit (exercises the GDI+ shutdown order): `PostMessageW(hwnd, WM_CLOSE /*0x10*/, 0, 0)`
  then confirm exit code 0.
- `Get-Process syscore` for the RAM footprint.

## Conventions
- C++17. Keep metrics in `metrics.cpp`, drawing in `renderer.cpp`, window/UX in `main.cpp`.
- Match surrounding style; favor stack-allocated GDI+ objects (`SolidBrush`, `Pen`,
  `GraphicsPath`) in draw helpers.
- **No new dependencies** (see Hard constraints).

## Hardware this was tuned on (code is generic, but useful context)
AMD Ryzen 5 3500U (4C/8T), Radeon Vega 8 iGPU (PDH LUID `luid_0x00000000_0x00013474`),
5.9 GB RAM, 3 disks (C/E/F), Realtek GbE + RTL8821CE Wi-Fi, Windows 10 Pro (Korean).

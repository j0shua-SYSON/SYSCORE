// ============================================================================
//  SYSCORE  ::  lightweight native system HUD  ::  shared model + theme
//  Target: Windows 10/11, MinGW-w64 g++  (Win32 + GDI+ + PDH)
// ============================================================================
#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   // Windows 10
#endif

// GDI+ wants min/max; libstdc++ breaks if they are macros. Canonical MinGW fix:
// suppress the windows.h macros, then re-inject the std versions into Gdiplus.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <deque>
#include <cstdint>
#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

// ---------------------------------------------------------------------------
//  Rolling history buffer for the live graphs (fixed capacity ring).
// ---------------------------------------------------------------------------
struct History {
    std::deque<float> v;
    size_t cap = 240;                 // ~4 minutes at 1Hz, plenty for sparklines
    void push(float x) {
        v.push_back(x);
        while (v.size() > cap) v.pop_front();
    }
    float last() const { return v.empty() ? 0.f : v.back(); }
    float maxv(float floor = 1.f) const {
        float m = floor;
        for (float f : v) if (f > m) m = f;
        return m;
    }
};

// ---------------------------------------------------------------------------
//  Per-subsystem snapshots
// ---------------------------------------------------------------------------
struct CoreInfo {
    float  usage = 0.f;               // 0..100
    double mhz   = 0.0;               // current clock
};

struct CpuInfo {
    std::string name = "CPU";
    int   physicalCores = 0;
    int   logicalCores  = 0;
    float usage = 0.f;                // overall 0..100
    double curMhz = 0.0;              // representative (max core) current clock
    double maxMhz = 0.0;              // nominal / max
    float tempC = -1.f;               // <0 == unavailable
    std::vector<CoreInfo> cores;
    History hist;                     // overall usage history
};

struct MemInfo {
    uint64_t totalPhys = 0, usedPhys = 0, availPhys = 0;
    uint64_t totalPage = 0, usedPage = 0;     // commit limit / commit current
    uint64_t cached = 0;
    float    percent = 0.f;          // physical used %
    History  hist;
};

struct GpuInfo {
    bool   available = false;
    std::string name = "GPU";
    float  usage = 0.f;              // overall 0..100 (max engine type)
    float  use3D = 0.f, useCopy = 0.f, useVideo = 0.f, useCompute = 0.f;
    uint64_t dedicatedUsed = 0, dedicatedTotal = 0;   // bytes
    uint64_t sharedUsed = 0;                          // bytes
    float  tempC = -1.f;
    History hist;
};

struct DiskInfo {
    std::string name;                // e.g. "C:"
    double readBps = 0, writeBps = 0;
    float  busyPct = 0.f;            // % disk time
    uint64_t freeBytes = 0, totalBytes = 0;
};

struct DiskAgg {
    double readBps = 0, writeBps = 0;
    float  busyPct = 0.f;
    std::vector<DiskInfo> disks;
    History histRead, histWrite;
};

struct NetIf {
    std::string name;
    double rxBps = 0, txBps = 0;
};

struct NetAgg {
    double rxBps = 0, txBps = 0;
    std::vector<NetIf> ifaces;
    History histRx, histTx;
};

struct ProcInfo {
    uint32_t pid = 0;
    std::string name;
    float  cpu = 0.f;                // % of total capacity
    uint64_t mem = 0;               // working set bytes
};

struct SysInfo {
    std::string host;
    std::string os;
    uint64_t bootTickMs = 0;        // GetTickCount64 at sample time
};

// ---------------------------------------------------------------------------
//  The whole metrics snapshot the renderer reads each frame.
// ---------------------------------------------------------------------------
struct Metrics {
    CpuInfo  cpu;
    MemInfo  mem;
    GpuInfo  gpu;
    DiskAgg  disk;
    NetAgg   net;
    SysInfo  sys;
    std::vector<ProcInfo> topCpu;   // top processes by cpu
    std::vector<ProcInfo> topMem;   // top processes by working set
    uint64_t uptimeSec = 0;
};

// ---------------------------------------------------------------------------
//  Modern minimal theme  (high-contrast on dark slate; per-metric accents)
// ---------------------------------------------------------------------------
namespace Theme {
    using Gdiplus::Color;
    // surfaces (alpha < 255 => the translucent floating panel)
    inline Color bg()        { return Color(240, 17, 19, 25); }   // app background
    inline Color bgInner()   { return Color(240, 21, 24, 31); }
    inline Color border()    { return Color(255, 46, 51, 62); }
    inline Color borderSoft(){ return Color(150, 46, 51, 62); }
    inline Color track()     { return Color(255, 37, 41, 51); }   // empty bar track
    inline Color divider()   { return Color(110, 60, 66, 80); }
    // text
    inline Color textHi()    { return Color(255, 234, 237, 244); }
    inline Color text()      { return Color(255, 190, 196, 208); }
    inline Color textDim()   { return Color(255, 140, 147, 162); }
    inline Color textFaint() { return Color(255, 102, 109, 124); }
    // per-metric accents
    inline Color cpu()       { return Color(255,  74, 144, 246); } // blue
    inline Color mem()       { return Color(255, 167, 139, 250); } // violet
    inline Color gpu()       { return Color(255,  45, 212, 191); } // teal
    inline Color disk()      { return Color(255, 240, 170,  76); } // amber
    inline Color netRx()     { return Color(255,  56, 189, 248); } // sky
    inline Color netTx()     { return Color(255, 251, 146, 120); } // coral
    // status
    inline Color ok()        { return Color(255,  52, 211, 153); }
    inline Color warn()      { return Color(255, 251, 191,  64); }
    inline Color crit()      { return Color(255, 248, 113, 113); }
    inline Color barEmpty()  { return track(); }

    inline Color lerp(Color a, Color b, float t){
        if (t < 0) t = 0; if (t > 1) t = 1;
        return Color(255,
            (BYTE)(a.GetRed()   + (b.GetRed()  - a.GetRed())  *t),
            (BYTE)(a.GetGreen() + (b.GetGreen()- a.GetGreen())*t),
            (BYTE)(a.GetBlue()  + (b.GetBlue() - a.GetBlue()) *t));
    }
    // keep the accent until ~70%, then warm toward amber/red on heavy load
    inline Color load(Color accent, float pct){
        if (pct <= 70.f) return accent;
        if (pct <= 88.f) return lerp(accent, warn(), (pct-70.f)/18.f);
        return lerp(warn(), crit(), (pct-88.f)/12.f);
    }
    // numeric readout color by threshold
    inline Color value(float pct){
        if (pct < 80.f) return textHi();
        if (pct < 92.f) return warn();
        return crit();
    }
    // legacy helper (a few call sites) -> blue-based load ramp
    inline Color ramp(float pct, BYTE a = 255){
        Color c = load(cpu(), pct);
        return Color(a, c.GetRed(), c.GetGreen(), c.GetBlue());
    }
}

// ---------------------------------------------------------------------------
//  Runtime configuration (mutated via the right-click menu)
// ---------------------------------------------------------------------------
struct Config {
    bool  expanded     = false;       // compact bar vs full dashboard
    bool  alwaysOnTop  = true;
    BYTE  opacity      = 242;         // global SourceConstantAlpha 60..255
    int   targetFps    = 30;          // render cadence
    bool  showProcs    = true;        // top-process panel in expanded mode
    std::string themeName = "midnight";

    // ---- AI "Ask your PC" (configurable endpoint; WinHTTP, no dependencies) ----
    bool        aiEnabled  = true;
    std::string aiFormat   = "anthropic";                          // "anthropic" | "openai"
    std::string aiEndpoint = "https://api.anthropic.com/v1/messages";
    std::string aiModel    = "claude-haiku-4-5-20251001";
    std::string aiKey      = "";                                   // bring-your-own
};

// Read-only snapshot of the async AI worker, handed to the renderer each frame.
struct AiView {
    int         phase = 0;            // 0 idle, 1 running, 2 done, 3 error
    std::string title;                // e.g. "Diagnose slowdown"
    std::string output;              // result text or error message
    uint64_t    startTick = 0;        // GetTickCount64 when the request started
};

// ---------------------------------------------------------------------------
//  config persistence  (defined in config.cpp)  ->  %APPDATA%\SYSCORE\config.json
// ---------------------------------------------------------------------------
std::wstring configPath();
bool loadConfig(Config&);              // false if file absent (defaults kept)
bool saveConfig(const Config&);
void openConfigInEditor();

// ---------------------------------------------------------------------------
//  small formatting helpers (defined in metrics.cpp)
// ---------------------------------------------------------------------------
std::string humanBytes(uint64_t b, int decimals = 1);
std::string humanRate(double bps);            // bytes/s -> "1.2M" style (per sec)
std::wstring widen(const std::string& s);

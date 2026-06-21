// ============================================================================
//  SYSCORE :: main  ::  frameless, layered, always-on-top HUD window
//  Build (MinGW):  see build.ps1 / CMakeLists.txt
// ============================================================================
#include "app.h"
#include "metrics.h"
#include "renderer.h"
#include "ai.h"
#include <gdiplus.h>

// ---- menu ids --------------------------------------------------------------
enum {
    ID_EXPAND = 100, ID_AOT, ID_SCAN, ID_PROCS, ID_QUIT = 199,
    ID_OP_50 = 110, ID_OP_65, ID_OP_80, ID_OP_92, ID_OP_100,
    ID_FPS_10 = 120, ID_FPS_20, ID_FPS_30, ID_FPS_60,
    ID_AI_DIAG = 130, ID_AI_HEALTH, ID_AI_ENABLE, ID_AI_OPENCFG, ID_AI_RELOAD
};
enum { TIMER_DATA = 1, TIMER_RENDER = 2 };
enum { HOTKEY_DIAG = 1, HOTKEY_HEALTH = 2 };

// ---- globals (single window app) ------------------------------------------
static MetricsEngine g_engine;
static Metrics       g_metrics;
static Renderer      g_render;
static Config        g_cfg;
static HWND          g_hwnd = nullptr;
static ULONG_PTR     g_gdipToken = 0;
static ULONGLONG     g_start = 0;
static int           g_winX = 200, g_winY = 80, g_curW = 0, g_curH = 0;
static bool          g_dragging = false, g_moved = false, g_downChevron = false, g_downAi = false;
static POINT         g_dragOff{};
static POINT         g_downPt{};

// ---- DPI helpers (resolve at runtime; MinGW headers may lack the constants)-
static float getScale(HWND h){
    typedef UINT (WINAPI *PFN_GDFW)(HWND);
    static PFN_GDFW p = (PFN_GDFW)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT dpi = 96;
    if (p) dpi = p(h);
    else { HDC dc = GetDC(h); dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(h, dc); }
    if (dpi < 72) dpi = 96;
    return (float)dpi / 96.f;
}
static void enableDpiAwareness(){
    typedef BOOL (WINAPI *PFN_SPDAC)(HANDLE);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    auto setCtx = (PFN_SPDAC)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (setCtx){ if (setCtx((HANDLE)-4)) return; }      // PER_MONITOR_AWARE_V2
    SetProcessDPIAware();
}

// ---------------------------------------------------------------------------
// keep the (frameless, taskbar-less) HUD on a visible monitor work area
static void clampToWorkArea(){
    if (g_curW <= 0 || g_curH <= 0) return;
    POINT center{ g_winX + g_curW/2, g_winY + g_curH/2 };
    RECT wa{};
    HMONITOR mon = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (mon && GetMonitorInfo(mon, &mi)) wa = mi.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int maxX = wa.right  - g_curW, maxY = wa.bottom - g_curH;
    if (g_winX > maxX) g_winX = maxX;
    if (g_winY > maxY) g_winY = maxY;
    if (g_winX < wa.left) g_winX = wa.left;   // min wins if window bigger than area
    if (g_winY < wa.top)  g_winY = wa.top;
}

static void present(){
    SIZE sz = g_render.preferredSize(g_cfg);
    if (sz.cx != g_curW || sz.cy != g_curH){
        int rightEdge = g_winX + g_curW;        // keep top-right corner anchored
        if (g_curW != 0) g_winX = rightEdge - sz.cx;
        g_curW = sz.cx; g_curH = sz.cy;
    }
    clampToWorkArea();
    g_render.ensureSize(g_curW, g_curH);
    double tsec = (double)(GetTickCount64() - g_start) / 1000.0;
    g_render.draw(g_metrics, g_cfg, tsec, aiSnapshot());

    POINT ptDst{ g_winX, g_winY };
    SIZE  size { g_curW, g_curH };
    POINT ptSrc{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, g_cfg.opacity, AC_SRC_ALPHA };
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(g_hwnd, screen, &ptDst, &size, g_render.hdc(), &ptSrc, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

static void setFps(int fps){
    g_cfg.targetFps = fps;
    KillTimer(g_hwnd, TIMER_RENDER);
    SetTimer(g_hwnd, TIMER_RENDER, 1000 / (fps>0?fps:30), nullptr);
}
static void applyTopmost(){
    SetWindowPos(g_hwnd, g_cfg.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}
static void persist(){ saveConfig(g_cfg); }

static void askAI(const char* title, const char* prompt){
    aiAsk(g_cfg, title, prompt, aiBuildContext(g_metrics));
    present();
}
static void askDiagnose(){
    askAI("Diagnose slowdown",
          "My PC feels slow. What is using the most resources right now, and what exactly "
          "should I do about it?");
}
static void askHealth(){
    askAI("Health report", "Give me a brief health report of this system and 2-3 optimization tips.");
}

// ---------------------------------------------------------------------------
static void showMenu(int sx, int sy){
    HMENU m = CreatePopupMenu();
    HMENU ai = CreatePopupMenu();
    AppendMenuW(ai, MF_STRING, ID_AI_DIAG,   L"Diagnose slowdown\tCtrl+Alt+A");
    AppendMenuW(ai, MF_STRING, ID_AI_HEALTH, L"Health report\tCtrl+Alt+H");
    AppendMenuW(ai, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(ai, MF_STRING | (g_cfg.aiEnabled?MF_CHECKED:0), ID_AI_ENABLE, L"AI enabled");
    AppendMenuW(ai, MF_STRING, ID_AI_OPENCFG, L"Open config file…");
    AppendMenuW(ai, MF_STRING, ID_AI_RELOAD,  L"Reload config");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)ai, L"\x2726  Ask AI");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_cfg.expanded?MF_CHECKED:0), ID_EXPAND, L"Expanded dashboard");
    AppendMenuW(m, MF_STRING | (g_cfg.alwaysOnTop?MF_CHECKED:0), ID_AOT, L"Always on top");
    AppendMenuW(m, MF_STRING | (g_cfg.showProcs?MF_CHECKED:0), ID_PROCS, L"Top processes");
    HMENU op = CreatePopupMenu();
    AppendMenuW(op, MF_STRING|(g_cfg.opacity==128?MF_CHECKED:0), ID_OP_50,  L"50%");
    AppendMenuW(op, MF_STRING|(g_cfg.opacity==166?MF_CHECKED:0), ID_OP_65,  L"65%");
    AppendMenuW(op, MF_STRING|(g_cfg.opacity==204?MF_CHECKED:0), ID_OP_80,  L"80%");
    AppendMenuW(op, MF_STRING|(g_cfg.opacity==235?MF_CHECKED:0), ID_OP_92,  L"92%");
    AppendMenuW(op, MF_STRING|(g_cfg.opacity==255?MF_CHECKED:0), ID_OP_100, L"100%");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)op, L"Opacity");
    HMENU fp = CreatePopupMenu();
    AppendMenuW(fp, MF_STRING|(g_cfg.targetFps==10?MF_CHECKED:0), ID_FPS_10, L"10 fps (idle)");
    AppendMenuW(fp, MF_STRING|(g_cfg.targetFps==20?MF_CHECKED:0), ID_FPS_20, L"20 fps");
    AppendMenuW(fp, MF_STRING|(g_cfg.targetFps==30?MF_CHECKED:0), ID_FPS_30, L"30 fps");
    AppendMenuW(fp, MF_STRING|(g_cfg.targetFps==60?MF_CHECKED:0), ID_FPS_60, L"60 fps");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)fp, L"Refresh rate");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit  (Esc)");

    SetForegroundWindow(g_hwnd);     // so the menu dismisses correctly
    int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON|TPM_RETURNCMD|TPM_NONOTIFY, sx, sy, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd){
        case ID_EXPAND: g_cfg.expanded = !g_cfg.expanded; persist(); break;
        case ID_AOT:    g_cfg.alwaysOnTop = !g_cfg.alwaysOnTop; applyTopmost(); persist(); break;
        case ID_PROCS:  g_cfg.showProcs = !g_cfg.showProcs; persist(); break;
        case ID_OP_50:  g_cfg.opacity=128; persist(); break;
        case ID_OP_65:  g_cfg.opacity=166; persist(); break;
        case ID_OP_80:  g_cfg.opacity=204; persist(); break;
        case ID_OP_92:  g_cfg.opacity=235; persist(); break;
        case ID_OP_100: g_cfg.opacity=255; persist(); break;
        case ID_FPS_10: setFps(10); persist(); break;
        case ID_FPS_20: setFps(20); persist(); break;
        case ID_FPS_30: setFps(30); persist(); break;
        case ID_FPS_60: setFps(60); persist(); break;
        case ID_AI_DIAG:    askDiagnose(); break;
        case ID_AI_HEALTH:  askHealth();   break;
        case ID_AI_ENABLE:  g_cfg.aiEnabled = !g_cfg.aiEnabled; persist(); break;
        case ID_AI_OPENCFG: openConfigInEditor(); break;
        case ID_AI_RELOAD:  loadConfig(g_cfg); setFps(g_cfg.targetFps); applyTopmost(); break;
        case ID_QUIT:   PostMessageW(g_hwnd, WM_CLOSE, 0, 0); break;
    }
    if (cmd) present();
}

// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp){
    switch (msg){
    case WM_TIMER:
        if (wp == TIMER_DATA)   g_engine.sample(g_metrics);
        if (wp == TIMER_RENDER) present();
        return 0;

    case WM_LBUTTONDOWN: {
        g_downPt = POINT{ (LONG)(short)LOWORD(lp), (LONG)(short)HIWORD(lp) };
        RECT cr = g_render.chevronRect();   g_downChevron = (PtInRect(&cr, g_downPt) != 0);
        RECT ar = g_render.aiButtonRect();  g_downAi      = (!g_downChevron && PtInRect(&ar, g_downPt) != 0);
        POINT cur; GetCursorPos(&cur);
        g_dragOff = POINT{ cur.x - g_winX, cur.y - g_winY };
        g_dragging = true; g_moved = false;
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_dragging){
            POINT cur; GetCursorPos(&cur);
            int nx = cur.x - g_dragOff.x, ny = cur.y - g_dragOff.y;
            if (abs((int)(short)LOWORD(lp) - g_downPt.x) > 3 || abs((int)(short)HIWORD(lp) - g_downPt.y) > 3)
                g_moved = true;
            g_winX = nx; g_winY = ny;
            present();
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_dragging){
            // snapshot BEFORE ReleaseCapture(): it synchronously sends WM_CAPTURECHANGED,
            // whose handler clears g_downChevron/g_downAi.
            bool moved = g_moved, onChevron = g_downChevron, onAi = g_downAi;
            g_dragging = false;
            ReleaseCapture();
            if (!moved){
                if (aiSnapshot().phase != 0){ aiDismiss(); present(); }        // dismiss result panel
                else if (onChevron){ g_cfg.expanded = !g_cfg.expanded; persist(); present(); }
                else if (onAi){ askDiagnose(); }                               // "✦ AI" button
            }
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (aiSnapshot().phase != 0) return 0;          // don't expand under the panel
        g_cfg.expanded = !g_cfg.expanded; persist(); present(); return 0;

    case WM_RBUTTONUP: {
        if (g_dragging){ g_dragging = false; ReleaseCapture(); }   // don't leave a drag stuck
        g_downChevron = false;
        POINT p; GetCursorPos(&p); showMenu(p.x, p.y); return 0;
    }
    case WM_CAPTURECHANGED:     // capture revoked (alt-tab, other window, system) -> reset
        g_dragging = false; g_downChevron = false;
        return 0;
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 12 : -12;
        int o = (int)g_cfg.opacity + d;
        g_cfg.opacity = (BYTE)(o < 60 ? 60 : (o > 255 ? 255 : o));
        present(); return 0;
    }
    case WM_HOTKEY:
        if (wp == HOTKEY_DIAG)   askDiagnose();
        if (wp == HOTKEY_HEALTH) askHealth();
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE){
            if (aiSnapshot().phase != 0){ aiDismiss(); present(); }
            else PostMessageW(h, WM_CLOSE, 0, 0);
        }
        if (wp == 'E'){ g_cfg.expanded = !g_cfg.expanded; persist(); present(); }
        return 0;

    case WM_DPICHANGED: {
        g_render.setScale(getScale(h));
        g_curW = g_curH = 0;          // force re-layout at new scale
        if (RECT* sug = (RECT*)lp){   // OS-suggested position on the target monitor
            g_winX = sug->left; g_winY = sug->top;
        }
        present();                    // present() clamps to the new monitor's work area
        return 0;
    }

    case WM_CLOSE:
        persist();
        UnregisterHotKey(h, HOTKEY_DIAG); UnregisterHotKey(h, HOTKEY_HEALTH);
        KillTimer(h, TIMER_DATA); KillTimer(h, TIMER_RENDER);
        DestroyWindow(h); return 0;
    case WM_DESTROY:
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int){
    enableDpiAwareness();
    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&g_gdipToken, &gsi, nullptr);
    g_render.init();                   // safe now that GDI+ is up
    loadConfig(g_cfg);                  // restore prefs / create template on first run

    g_engine.init();
    g_engine.sample(g_metrics);        // prime once so first frame has data

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SyscoreHUD";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"SYSCORE", WS_POPUP,
        g_winX, g_winY, 10, 10, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    g_render.setScale(getScale(g_hwnd));
    g_start = GetTickCount64();

    // initial top-right placement on the primary work area
    {
        RECT wa{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        SIZE sz = g_render.preferredSize(g_cfg);
        int margin = (int)(16 * g_render.scale());
        g_winX = wa.right - sz.cx - margin;
        g_winY = wa.top + margin;
    }

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    applyTopmost();
    present();

    // global hotkeys for AI (best-effort; ignore if already taken by another app)
    RegisterHotKey(g_hwnd, HOTKEY_DIAG,   MOD_CONTROL | MOD_ALT, 'A');
    RegisterHotKey(g_hwnd, HOTKEY_HEALTH, MOD_CONTROL | MOD_ALT, 'H');

    SetTimer(g_hwnd, TIMER_DATA, 1000, nullptr);
    setFps(g_cfg.targetFps);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)){
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_render.shutdown();              // free GDI+ objects while GDI+ is still up
    Gdiplus::GdiplusShutdown(g_gdipToken);
    return (int)msg.wParam;
}

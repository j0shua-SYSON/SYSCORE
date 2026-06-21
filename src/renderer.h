// ============================================================================
//  Renderer :: GDI+ HUD into a premultiplied-ARGB DIB for UpdateLayeredWindow
// ============================================================================
#pragma once
#include "app.h"

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    void  init();           // GDI+ setup; MUST be called after GdiplusStartup
    void  shutdown();       // free all GDI+ objects; call BEFORE GdiplusShutdown
    void  setScale(float s) { scale_ = s; }
    float scale() const { return scale_; }

    SIZE  preferredSize(const Config& cfg) const;   // device px for current mode
    void  ensureSize(int w, int h);                 // (re)alloc DIB if needed
    void  draw(const Metrics& m, const Config& cfg, double tsec, const AiView& ai);

    HDC   hdc()    const { return memDC_; }
    int   width()  const { return w_; }
    int   height() const { return h_; }

    // interactive zones (in client px), valid after draw()
    RECT  chevronRect()  const { return chevron_; }
    RECT  closeRect()    const { return close_; }
    RECT  headerRect()   const { return header_; }
    RECT  aiButtonRect() const { return aiBtn_; }

private:
    // ---- platform bitmap ----
    void  destroy();
    void  premultiply();
    int   w_ = 0, h_ = 0;
    void* bits_ = nullptr;
    HBITMAP dib_ = nullptr, old_ = nullptr;
    HDC   memDC_ = nullptr;
    Gdiplus::Bitmap*  bmp_ = nullptr;
    Gdiplus::Graphics* g_  = nullptr;
    Gdiplus::StringFormat* sf_ = nullptr;

    // ---- fonts ----
    Gdiplus::FontFamily* family_ = nullptr;     // regular UI family (Segoe UI)
    Gdiplus::FontFamily* semibold_ = nullptr;   // Segoe UI Semibold, if present
    std::vector<std::pair<int, Gdiplus::Font*>> fontCache_;
    Gdiplus::Font* font(float px, bool bold = false);

    // ---- scale + zones ----
    float scale_ = 1.f;
    RECT  chevron_{}, close_{}, header_{}, aiBtn_{};

    float px(float v) const { return v * scale_; }

    // ---- primitives ----
    float measure(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f);
    void  txt(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f,
              float x, float y, Gdiplus::Color c, int align = 0);
    void  txtGlow(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f,
                  float x, float y, Gdiplus::Color c, int align = 0);
    void  segBar(Gdiplus::Graphics& g, float x, float y, float w, float h,
                 float pct, int cells, bool ramp, Gdiplus::Color baseFill);
    void  spark(Gdiplus::Graphics& g, float x, float y, float w, float h,
                const History& hh, Gdiplus::Color c, float maxOv = -1.f, bool area = true);
    void  sparkPair(Gdiplus::Graphics& g, float x, float y, float w, float h,
                    const History& a, Gdiplus::Color ca, const History& b, Gdiplus::Color cb);
    void  ring(Gdiplus::Graphics& g, float cx, float cy, float rad, float thick,
               float pct, Gdiplus::Color c);
    void  panel(Gdiplus::Graphics& g, Gdiplus::RectF content);
    void  drawAiOverlay(Gdiplus::Graphics& g, Gdiplus::RectF content, const AiView& ai, double tsec);
    void  aiStar(Gdiplus::Graphics& g, float x, float y);   // header "ask AI" affordance
    void  statusPill(Gdiplus::Graphics& g, float x, float y, const Metrics& m);
    void  expandIcon(Gdiplus::Graphics& g, RECT r, bool expanded, Gdiplus::Color c);
    float sectionHeader(Gdiplus::Graphics& g, float x, float y, float w, const std::wstring& title);

    // ---- layouts ----
    void  drawCompact(Gdiplus::Graphics& g, Gdiplus::RectF c, const Metrics& m,
                      const Config& cfg, double tsec);
    void  drawExpanded(Gdiplus::Graphics& g, Gdiplus::RectF c, const Metrics& m,
                       const Config& cfg, double tsec);
    void  metricRow(Gdiplus::Graphics& g, float x, float y, float w,
                    const std::wstring& label, float pct, const std::wstring& valTxt,
                    Gdiplus::Color accent);
};

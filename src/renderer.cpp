// ============================================================================
//  Renderer implementation  ::  modern minimal monitoring HUD
// ============================================================================
#include "renderer.h"
#include <cstdio>
#include <cwchar>

using namespace Gdiplus;

// ---------------------------------------------------------------------------
//  small utils
// ---------------------------------------------------------------------------
static float clampf2(float v, float lo, float hi){ return v<lo?lo:(v>hi?hi:v); }
static Color withA(Color c, BYTE a){ return Color(a, c.GetRed(), c.GetGreen(), c.GetBlue()); }

static void addRoundRect(GraphicsPath& p, RectF r, float rad){
    if (r.Width <= 0 || r.Height <= 0) return;
    float d = rad*2.f;
    if (d > r.Width)  d = r.Width;
    if (d > r.Height) d = r.Height;
    p.AddArc(r.X,            r.Y,            d, d, 180, 90);
    p.AddArc(r.GetRight()-d, r.Y,            d, d, 270, 90);
    p.AddArc(r.GetRight()-d, r.GetBottom()-d,d, d,   0, 90);
    p.AddArc(r.X,            r.GetBottom()-d,d, d,  90, 90);
    p.CloseFigure();
}

static std::wstring fmtUptime(uint64_t sec){
    uint64_t d = sec/86400; sec%=86400;
    uint64_t h = sec/3600;  sec%=3600;
    uint64_t mi= sec/60;    uint64_t s = sec%60;
    wchar_t b[64];
    if (d) swprintf(b,64,L"%llud %02llu:%02llu:%02llu",(unsigned long long)d,(unsigned long long)h,(unsigned long long)mi,(unsigned long long)s);
    else   swprintf(b,64,L"%02llu:%02llu:%02llu",(unsigned long long)h,(unsigned long long)mi,(unsigned long long)s);
    return b;
}
static std::wstring wbytes(uint64_t b){ return widen(humanBytes(b)); }
static std::wstring wrate(double b){ return widen(humanRate(b)); }

// ---------------------------------------------------------------------------
//  ctor / dtor / bitmap
// ---------------------------------------------------------------------------
void Renderer::init(){
    if (sf_) return;                       // idempotent; GDI+ must be started first
    sf_ = StringFormat::GenericTypographic()->Clone();
    sf_->SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip | StringFormatFlagsMeasureTrailingSpaces);

    const wchar_t* prefs[] = { L"Segoe UI", L"Inter", L"Tahoma", L"Arial" };
    for (auto name : prefs){
        FontFamily* ff = new FontFamily(name);
        if (ff->IsAvailable()){ family_ = ff; break; }
        delete ff;
    }
    if (!family_) family_ = FontFamily::GenericSansSerif()->Clone();

    FontFamily* sb = new FontFamily(L"Segoe UI Semibold");
    if (sb->IsAvailable()) semibold_ = sb; else delete sb;
}

void Renderer::shutdown(){
    // idempotent: safe to call before GdiplusShutdown and again from the dtor
    for (auto& kv : fontCache_) delete kv.second;
    fontCache_.clear();
    delete semibold_; semibold_ = nullptr;
    delete family_;   family_   = nullptr;
    delete sf_;       sf_       = nullptr;
    destroy();        // frees Graphics/Bitmap + GDI handles, nulls them
}

Renderer::~Renderer(){
    shutdown();
}

void Renderer::destroy(){
    delete g_;   g_ = nullptr;
    delete bmp_; bmp_ = nullptr;
    if (memDC_ && old_) SelectObject(memDC_, old_);
    if (dib_)   { DeleteObject(dib_); dib_ = nullptr; }
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    old_ = nullptr; bits_ = nullptr; w_ = h_ = 0;
}

void Renderer::ensureSize(int w, int h){
    if (w == w_ && h == h_ && dib_) return;
    destroy();
    w_ = w; h_ = h;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;     // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    dib_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    ReleaseDC(nullptr, screen);
    memDC_ = CreateCompatibleDC(nullptr);
    old_ = (HBITMAP)SelectObject(memDC_, dib_);
    bmp_ = new Bitmap(w, h, w*4, PixelFormat32bppARGB, (BYTE*)bits_);
    g_ = new Graphics(bmp_);
    g_->SetSmoothingMode(SmoothingModeAntiAlias);
    g_->SetTextRenderingHint(TextRenderingHintAntiAlias);
    g_->SetPixelOffsetMode(PixelOffsetModeHalf);
    g_->SetInterpolationMode(InterpolationModeHighQuality);
}

void Renderer::premultiply(){
    uint32_t* px = (uint32_t*)bits_;
    int n = w_*h_;
    for (int i=0;i<n;i++){
        uint32_t c = px[i];
        uint32_t a = c >> 24;
        if (a == 0)   { px[i] = 0; continue; }
        if (a == 255) continue;
        uint32_t r = (c>>16)&0xff, g=(c>>8)&0xff, b=c&0xff;
        r = r*a/255; g = g*a/255; b = b*a/255;
        px[i] = (a<<24)|(r<<16)|(g<<8)|b;
    }
}

// ---------------------------------------------------------------------------
//  fonts
// ---------------------------------------------------------------------------
Font* Renderer::font(float pxSize, bool bold){
    int key = (int)(pxSize*4) * 2 + (bold?1:0);
    for (auto& kv : fontCache_) if (kv.first == key) return kv.second;
    FontFamily* fam = (bold && semibold_) ? semibold_ : family_;
    FontStyle   st  = (bold && !semibold_) ? FontStyleBold : FontStyleRegular;
    Font* f = new Font(fam, pxSize, st, UnitPixel);
    fontCache_.push_back({key, f});
    return f;
}

// ---------------------------------------------------------------------------
//  text primitives  (txtGlow == text with a soft 1px shadow for legibility)
// ---------------------------------------------------------------------------
float Renderer::measure(Graphics& g, const std::wstring& s, Font* f){
    RectF bb; g.MeasureString(s.c_str(), (INT)s.size(), f, PointF(0,0), sf_, &bb);
    return bb.Width;
}
void Renderer::txt(Graphics& g, const std::wstring& s, Font* f, float x, float y, Color c, int align){
    float dx = 0;
    if (align){ float w = measure(g,s,f); dx = align==1 ? -w : -w/2.f; }
    SolidBrush b(c);
    g.DrawString(s.c_str(), (INT)s.size(), f, PointF(x+dx, y), sf_, &b);
}
void Renderer::txtGlow(Graphics& g, const std::wstring& s, Font* f, float x, float y, Color c, int align){
    float dx = 0;
    if (align){ float w = measure(g,s,f); dx = align==1 ? -w : -w/2.f; }
    SolidBrush sh(Color(120,0,0,0));
    g.DrawString(s.c_str(), (INT)s.size(), f, PointF(x+dx, y+px(1)), sf_, &sh);
    SolidBrush b(c);
    g.DrawString(s.c_str(), (INT)s.size(), f, PointF(x+dx, y), sf_, &b);
}

// ---------------------------------------------------------------------------
//  smooth rounded progress bar
// ---------------------------------------------------------------------------
void Renderer::segBar(Graphics& g, float x, float y, float w, float h,
                      float pct, int /*cells*/, bool ramp, Color baseFill){
    float r = h*0.5f;
    GraphicsPath tp; addRoundRect(tp, RectF(x,y,w,h), r);
    SolidBrush tb(Theme::track()); g.FillPath(&tb, &tp);
    float p = clampf2(pct,0,100);
    if (p > 0.4f){
        float fw = w * p/100.f;
        if (fw < h) fw = h;                 // keep the rounded cap visible
        Color c = ramp ? Theme::load(baseFill, p) : baseFill;
        GraphicsPath fp; addRoundRect(fp, RectF(x,y,fw,h), r);
        LinearGradientBrush fb(PointF(x,y), PointF(x+fw,y), withA(c,235), c);
        g.FillPath(&fb, &fp);
    }
}

// ---------------------------------------------------------------------------
//  sparkline (subtle area + thin line + small end dot)
// ---------------------------------------------------------------------------
void Renderer::spark(Graphics& g, float x, float y, float w, float h,
                     const History& hh, Color c, float maxOv, bool area){
    size_t n = hh.v.size();
    if (n < 2) return;
    float mx = maxOv > 0 ? maxOv : hh.maxv(1.f);
    if (mx <= 0) mx = 1;
    std::vector<PointF> pts; pts.reserve(n);
    for (size_t i=0;i<n;i++){
        float fx = x + w * (float)i/(float)(n-1);
        float v  = clampf2(hh.v[i], 0, mx);
        float fy = y + h - (v/mx)*h*0.96f - h*0.02f;
        pts.push_back(PointF(fx, fy));
    }
    if (area){
        std::vector<PointF> poly = pts;
        poly.push_back(PointF(x+w, y+h));
        poly.push_back(PointF(x,   y+h));
        LinearGradientBrush lb(PointF(x,y), PointF(x,y+h+1), withA(c,55), withA(c,0));
        g.FillPolygon(&lb, poly.data(), (INT)poly.size());
    }
    Pen pen(c, px(1.5f)); pen.SetLineJoin(LineJoinRound);
    g.DrawLines(&pen, pts.data(), (INT)pts.size());
    PointF last = pts.back();
    SolidBrush dd(c); g.FillEllipse(&dd, last.X-px(2.0f), last.Y-px(2.0f), px(4.0f), px(4.0f));
}

void Renderer::sparkPair(Graphics& g, float x, float y, float w, float h,
                         const History& a, Color ca, const History& b, Color cb){
    float mx = 1.f;
    for (float v : a.v) if (v>mx) mx=v;
    for (float v : b.v) if (v>mx) mx=v;
    spark(g, x,y,w,h, a, ca, mx, true);
    spark(g, x,y,w,h, b, cb, mx, false);
}

// ---------------------------------------------------------------------------
//  thin ring gauge
// ---------------------------------------------------------------------------
void Renderer::ring(Graphics& g, float cx, float cy, float rad, float thick, float pct, Color c){
    Pen bg(Theme::track(), thick); bg.SetStartCap(LineCapRound); bg.SetEndCap(LineCapRound);
    g.DrawArc(&bg, cx-rad, cy-rad, rad*2, rad*2, 0, 360);
    float sweep = clampf2(pct,0,100)/100.f*360.f;
    Pen fg(c, thick); fg.SetStartCap(LineCapRound); fg.SetEndCap(LineCapRound);
    if (sweep > 1.0f) g.DrawArc(&fg, cx-rad, cy-rad, rad*2, rad*2, -90, sweep);
}

// ---------------------------------------------------------------------------
//  panel chrome: soft drop shadow + flat fill + hairline border
// ---------------------------------------------------------------------------
void Renderer::panel(Graphics& g, RectF content){
    for (int i=7;i>=1;--i){                 // soft shadow in the margin
        RectF rr = content; rr.Inflate(px(i*0.9f), px(i*0.9f));
        rr.Offset(0, px(i*0.35f));
        GraphicsPath sp; addRoundRect(sp, rr, px(15));
        SolidBrush sb(Color(9,0,0,0));
        g.FillPath(&sb, &sp);
    }
    GraphicsPath path; addRoundRect(path, content, px(13));
    LinearGradientBrush bg(PointF(content.X, content.Y),
                           PointF(content.X, content.GetBottom()+1),
                           Theme::bg(), Theme::bgInner());
    g.FillPath(&bg, &path);
    // faint top inner highlight
    Pen hi(Color(14,255,255,255), px(1.f));
    GraphicsPath hp; RectF hr = content; hr.Inflate(-px(0.5f),-px(0.5f));
    addRoundRect(hp, hr, px(12.5f));
    g.DrawPath(&hi, &hp);
    // hairline border
    Pen edge(Theme::border(), px(1.f));
    g.DrawPath(&edge, &path);
}

// expand / collapse glyph drawn from lines (no corny font glyphs)
void Renderer::expandIcon(Graphics& g, RECT r, bool expanded, Color c){
    Pen p(c, px(1.4f)); p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
    float x = (float)r.left, y = (float)r.top;
    float w = (float)(r.right-r.left), h = (float)(r.bottom-r.top);
    float a = px(3.2f);
    if (!expanded){ // arrows pointing outward (expand)
        // top-left
        g.DrawLine(&p, x, y+a, x, y);     g.DrawLine(&p, x, y, x+a, y);
        // bottom-right
        g.DrawLine(&p, x+w, y+h-a, x+w, y+h); g.DrawLine(&p, x+w, y+h, x+w-a, y+h);
    } else {        // arrows pointing inward (collapse)
        g.DrawLine(&p, x, y, x+a, y);     g.DrawLine(&p, x+a, y, x+a, y+a);
        g.DrawLine(&p, x+w, y+h, x+w-a, y+h); g.DrawLine(&p, x+w-a, y+h, x+w-a, y+h-a);
    }
}

// uppercase section label + thin divider
float Renderer::sectionHeader(Graphics& g, float x, float y, float w, const std::wstring& title){
    Font* f = font(px(9.0f), true);
    // letter-spaced uppercase
    std::wstring s; for (wchar_t ch : title){ s += ch; s += L' '; }
    txt(g, s, f, x, y, Theme::textDim());
    float tw = measure(g, s, f);
    Pen ln(Theme::divider(), px(1.0f));
    float ly = y + px(6);
    g.DrawLine(&ln, x+tw+px(6), ly, x+w, ly);
    return y + px(16);
}

// ---------------------------------------------------------------------------
//  status pill (dot + word), bottom-left
// ---------------------------------------------------------------------------
static const wchar_t* statusWord(float worst, Color& dot){
    if (worst < 60){ dot = Theme::ok();   return L"Nominal"; }
    if (worst < 85){ dot = Theme::warn(); return L"Elevated"; }
    dot = Theme::crit(); return L"High load";
}
void Renderer::statusPill(Graphics& g, float x, float y, const Metrics& m){
    float worst = m.cpu.usage;
    if (m.mem.percent > worst) worst = m.mem.percent;
    if (m.gpu.usage   > worst) worst = m.gpu.usage;
    Color dot; const wchar_t* word = statusWord(worst, dot);
    SolidBrush db(dot);
    g.FillEllipse(&db, x, y+px(3.5f), px(6.5f), px(6.5f));
    txt(g, word, font(px(9.5f)), x+px(12), y, Theme::textDim());
}

// ---------------------------------------------------------------------------
//  preferred window size
// ---------------------------------------------------------------------------
SIZE Renderer::preferredSize(const Config& cfg) const {
    SIZE s;
    if (cfg.expanded){ s.cx = (LONG)(600*scale_); s.cy = (LONG)(812*scale_); }
    else             { s.cx = (LONG)(308*scale_); s.cy = (LONG)(228*scale_); }
    return s;
}

// ---------------------------------------------------------------------------
//  draw entry
// ---------------------------------------------------------------------------
void Renderer::draw(const Metrics& m, const Config& cfg, double tsec, const AiView& ai){
    Graphics& g = *g_;
    g.Clear(Color(0,0,0,0));

    float M = px(8);
    RectF content(M, M, (float)w_-2*M, (float)h_-2*M);
    panel(g, content);

    GraphicsPath clip; addRoundRect(clip, content, px(13));
    g.SetClip(&clip);

    aiBtn_ = RECT{0,0,0,0};
    if (cfg.expanded) drawExpanded(g, content, m, cfg, tsec);
    else              drawCompact (g, content, m, cfg, tsec);

    if (ai.phase != 0) drawAiOverlay(g, content, ai, tsec);

    g.ResetClip();
    g.Flush(FlushIntentionSync);
    premultiply();
}

// "✦ AI" header affordance; records aiBtn_ hot-zone
void Renderer::aiStar(Graphics& g, float x, float y){
    Font* f = font(px(11), true);
    txt(g, L"\x2726 AI", f, x, y, Theme::cpu());
    float w = measure(g, L"\x2726 AI", f);
    aiBtn_ = RECT{ (LONG)(x-px(4)), (LONG)(y-px(2)), (LONG)(x+w+px(4)), (LONG)(y+px(14)) };
}

// full-panel AI result overlay
void Renderer::drawAiOverlay(Graphics& g, RectF c, const AiView& ai, double tsec){
    GraphicsPath p; addRoundRect(p, c, px(13));
    SolidBrush scrim(Color(244, 12, 14, 19));
    g.FillPath(&scrim, &p);

    float padL = c.X + px(16), right = c.GetRight() - px(16), y = c.Y + px(14);
    float w = right - padL;

    txtGlow(g, L"\x2726  " + widen(ai.title.empty() ? "Ask your PC" : ai.title),
            font(px(12.5f), true), padL, y, Theme::cpu());
    if (ai.phase == 1){
        double el = (double)(GetTickCount64() - ai.startTick) / 1000.0;
        wchar_t s[40]; swprintf(s, 40, L"analyzing  %.0fs", el);
        txt(g, s, font(px(9.5f)), right, y+px(2), Theme::textDim(), 1);
    } else {
        txt(g, L"Esc / click to dismiss", font(px(9.f)), right, y+px(2), Theme::textFaint(), 1);
    }
    y += px(22);
    Pen sep(Theme::divider(), px(1.f)); g.DrawLine(&sep, padL, y, right, y);
    y += px(12);

    if (ai.phase == 1){
        int dots = (int)(fmod(tsec, 1.6) / 0.4) + 1;
        std::wstring s = L"Reading your system";
        for (int i=0;i<dots && i<4;i++) s += L'.';
        txt(g, s, font(px(11)), padL, y, Theme::text());
    } else {
        Color col = (ai.phase == 3) ? Theme::crit() : Theme::textHi();
        RectF body(padL, y, w, c.GetBottom() - px(16) - y);
        StringFormat wf(StringFormat::GenericTypographic());
        wf.SetFormatFlags(StringFormatFlagsMeasureTrailingSpaces);
        wf.SetTrimming(StringTrimmingEllipsisWord);
        SolidBrush tb(col);
        std::wstring out = widen(ai.output);
        g.DrawString(out.c_str(), (INT)out.size(), font(px(10.5f)), body, &wf, &tb);
    }
}

// ---------------------------------------------------------------------------
//  shared header (title, uptime, expand affordance)
// ---------------------------------------------------------------------------
void Renderer::metricRow(Graphics& g, float x, float y, float w,
                         const std::wstring& label, float pct, const std::wstring& valTxt,
                         Color accent){
    txt(g, label, font(px(10.5f)), x, y+px(2), Theme::text());
    float bx = x + px(46);
    float bw = w - px(46) - px(58);
    segBar(g, bx, y+px(4), bw, px(8), pct, 0, true, accent);
    txtGlow(g, valTxt, font(px(12.5f), true), x+w, y, Theme::value(pct), 1);
}

// ---------------------------------------------------------------------------
//  COMPACT LAYOUT
// ---------------------------------------------------------------------------
void Renderer::drawCompact(Graphics& g, RectF c, const Metrics& m, const Config& cfg, double){
    float padL = c.X + px(15);
    float right = c.GetRight() - px(15);
    float w = right - padL;
    float y = c.Y + px(13);

    // header
    Color sdot; statusWord(
        (m.cpu.usage>m.mem.percent?(m.cpu.usage>m.gpu.usage?m.cpu.usage:m.gpu.usage)
                                  :(m.mem.percent>m.gpu.usage?m.mem.percent:m.gpu.usage)), sdot);
    SolidBrush db(sdot); g.FillEllipse(&db, padL, y+px(3), px(7), px(7));
    txt(g, L"SYSCORE", font(px(12), true), padL+px(13), y-px(1), Theme::textHi());
    RECT ic{ (LONG)(right-px(13)), (LONG)(y+px(1)), (LONG)(right-px(2)), (LONG)(y+px(11)) };
    expandIcon(g, ic, false, Theme::textDim());
    chevron_ = RECT{ (LONG)(right-px(26)), (LONG)(y-px(3)), (LONG)(right+px(6)), (LONG)(y+px(19)) };
    aiStar(g, right-px(66), y-px(1));
    txt(g, fmtUptime(m.uptimeSec), font(px(9.5f)), right-px(80), y, Theme::textFaint(), 1);
    header_  = RECT{ (LONG)c.X, (LONG)c.Y, (LONG)c.GetRight(), (LONG)(y+px(16)) };
    close_   = RECT{0,0,0,0};

    y += px(24);
    Pen sep(Theme::divider(), px(1.f)); g.DrawLine(&sep, padL, y, right, y);
    y += px(10);

    Font* fL = font(px(10.5f));
    Font* fV = font(px(13), true);
    Font* fS = font(px(9.f));

    struct Row { const wchar_t* lab; float pct; Color accent; std::wstring val; std::wstring sub; const History* hist; };
    wchar_t cpuv[16],memv[16],gpuv[16];
    swprintf(cpuv,16,L"%.0f%%",m.cpu.usage);
    swprintf(memv,16,L"%.0f%%",m.mem.percent);
    swprintf(gpuv,16,L"%.0f%%",m.gpu.usage);

    auto row = [&](const wchar_t* lab, float pct, Color accent, const std::wstring& val,
                   const std::wstring& sub, const History* hist){
        txt(g, lab, fL, padL, y+px(1), Theme::text());
        float bx = padL + px(40);
        float bw = w - px(40) - px(52);
        segBar(g, bx, y+px(3), bw, px(8), pct, 0, true, accent);
        txtGlow(g, val, fV, right, y-px(2), Theme::value(pct), 1);
        y += px(16);
        if (!sub.empty()) txt(g, sub, fS, padL, y, Theme::textDim());
        if (hist) spark(g, padL+px(120), y-px(2), w-px(120), px(13), *hist, accent, 100.f);
        y += px(15);
    };

    {   // CPU
        wchar_t sub[80];
        if (m.cpu.tempC >= 0) swprintf(sub,80,L"%.2f GHz   %.0f\x00B0""C", m.cpu.curMhz/1000.0, m.cpu.tempC);
        else                  swprintf(sub,80,L"%.2f GHz", m.cpu.curMhz/1000.0);
        row(L"CPU", m.cpu.usage, Theme::cpu(), cpuv, sub, &m.cpu.hist);
    }
    {   // MEM
        wchar_t sub[96];
        swprintf(sub,96,L"%ls / %ls", wbytes(m.mem.usedPhys).c_str(), wbytes(m.mem.totalPhys).c_str());
        row(L"MEM", m.mem.percent, Theme::mem(), memv, sub, nullptr);
    }
    {   // GPU
        wchar_t sub[96];
        swprintf(sub,96,L"VRAM %ls / %ls", wbytes(m.gpu.dedicatedUsed).c_str(), wbytes(m.gpu.dedicatedTotal).c_str());
        row(L"GPU", m.gpu.usage, Theme::gpu(), gpuv, sub, nullptr);
    }
    {   // NET
        txt(g, L"NET", fL, padL, y+px(1), Theme::text());
        wchar_t line[96];
        swprintf(line,96,L"\x2191 %ls/s   \x2193 %ls/s", wrate(m.net.txBps).c_str(), wrate(m.net.rxBps).c_str());
        txt(g, line, font(px(10)), padL+px(40), y+px(1), Theme::textHi());
        sparkPair(g, padL+px(170), y-px(2), w-px(170), px(13),
                  m.net.histRx, Theme::netRx(), m.net.histTx, Theme::netTx());
        y += px(17);
    }

    statusPill(g, padL, c.GetBottom()-px(20), m);
}

// ---------------------------------------------------------------------------
//  EXPANDED LAYOUT
// ---------------------------------------------------------------------------
void Renderer::drawExpanded(Graphics& g, RectF c, const Metrics& m, const Config& cfg, double){
    float padL = c.X + px(16);
    float right = c.GetRight() - px(16);
    float w = right - padL;
    float y = c.Y + px(14);

    Font* fSmall = font(px(9.5f));
    Font* fTiny  = font(px(9.f));
    Font* fMid   = font(px(10.5f));

    // ---- header band ----
    Color sdot; float worst = m.cpu.usage;
    if (m.mem.percent>worst) worst=m.mem.percent; if (m.gpu.usage>worst) worst=m.gpu.usage;
    statusWord(worst, sdot);
    SolidBrush db(sdot); g.FillEllipse(&db, padL, y+px(4), px(8), px(8));
    txt(g, L"SYSCORE", font(px(14), true), padL+px(16), y-px(1), Theme::textHi());
    RECT ic{ (LONG)(right-px(14)), (LONG)(y+px(2)), (LONG)(right-px(2)), (LONG)(y+px(13)) };
    expandIcon(g, ic, true, Theme::textDim());
    chevron_ = RECT{ (LONG)(right-px(26)), (LONG)(y-px(3)), (LONG)(right+px(6)), (LONG)(y+px(20)) };
    aiStar(g, right-px(66), y);
    txt(g, L"up " + fmtUptime(m.uptimeSec), font(px(10)), right-px(88), y+px(2), Theme::textFaint(), 1);
    header_  = RECT{ (LONG)c.X, (LONG)c.Y, (LONG)c.GetRight(), (LONG)(y+px(20)) };
    close_   = RECT{0,0,0,0};
    y += px(22);
    txt(g, widen(m.cpu.name), fSmall, padL, y, Theme::text());
    y += px(14);
    {
        wchar_t l3[160];
        swprintf(l3,160,L"%ls  \x00B7  %dC / %dT  \x00B7  %ls",
                 widen(m.sys.host).c_str(), m.cpu.physicalCores, m.cpu.logicalCores,
                 widen(m.gpu.name).c_str());
        txt(g, l3, fTiny, padL, y, Theme::textFaint());
        y += px(16);
    }

    // ---- CPU ----
    y = sectionHeader(g, padL, y, w, L"CPU");
    {
        float r = px(27), cx = padL + r, cy = y + r + px(2);
        ring(g, cx, cy, r, px(5.5f), m.cpu.usage, Theme::load(Theme::cpu(), m.cpu.usage));
        wchar_t pc[12]; swprintf(pc,12,L"%.0f", m.cpu.usage);
        txt(g, pc, font(px(16),true), cx, cy-px(12), Theme::textHi(), 2);
        txt(g, L"%", font(px(8.5f)), cx, cy+px(6), Theme::textDim(), 2);

        float bx = padL + r*2 + px(16);
        float bw = right - bx;
        wchar_t freq[64];
        if (m.cpu.tempC >= 0) swprintf(freq,64,L"%.2f GHz   \x00B7   %.0f\x00B0""C", m.cpu.curMhz/1000.0, m.cpu.tempC);
        else                  swprintf(freq,64,L"%.2f GHz", m.cpu.curMhz/1000.0);
        txt(g, freq, fMid, bx, y, Theme::text());
        wchar_t cv[12]; swprintf(cv,12,L"%.0f%%", m.cpu.usage);
        txtGlow(g, cv, font(px(12.5f),true), right, y-px(2), Theme::value(m.cpu.usage), 1);
        segBar(g, bx, y+px(16), bw, px(8), m.cpu.usage, 0, true, Theme::cpu());
        spark(g, bx, y+px(28), bw, px(20), m.cpu.hist, Theme::cpu(), 100.f);
        y = cy + r + px(8);

        int cores = (int)m.cpu.cores.size();
        int colN = 2, rows = (cores+colN-1)/colN;
        float colW = (w - px(14)) / colN;
        for (int i=0;i<cores;i++){
            int col = i / rows, row2 = i % rows;
            float x = padL + col*(colW+px(14));
            float yy = y + row2*px(15);
            wchar_t lab[8]; swprintf(lab,8,L"%d", i);
            txt(g, lab, fTiny, x, yy, Theme::textFaint());
            float bx2 = x + px(16), bw2 = colW - px(62);
            segBar(g, bx2, yy+px(3), bw2, px(6), m.cpu.cores[i].usage, 0, true, Theme::cpu());
            wchar_t cv2[24]; swprintf(cv2,24,L"%3.0f%%  %4.0f", m.cpu.cores[i].usage, m.cpu.cores[i].mhz);
            txt(g, cv2, fTiny, x+colW-px(2), yy, Theme::textDim(), 1);
        }
        y += rows*px(15) + px(10);
    }

    // ---- MEMORY ----
    y = sectionHeader(g, padL, y, w, L"MEMORY");
    {
        wchar_t v[16]; swprintf(v,16,L"%.0f%%", m.mem.percent);
        metricRow(g, padL, y, w, L"RAM", m.mem.percent, v, Theme::mem());
        y += px(16);
        wchar_t d[176];
        swprintf(d,176,L"%ls used / %ls   \x00B7   commit %ls / %ls   \x00B7   cache %ls",
                 wbytes(m.mem.usedPhys).c_str(), wbytes(m.mem.totalPhys).c_str(),
                 wbytes(m.mem.usedPage).c_str(),  wbytes(m.mem.totalPage).c_str(),
                 wbytes(m.mem.cached).c_str());
        txt(g, d, fTiny, padL, y, Theme::textDim());
        y += px(13);
        spark(g, padL, y, w, px(22), m.mem.hist, Theme::mem(), 100.f);
        y += px(28);
    }

    // ---- GPU ----
    y = sectionHeader(g, padL, y, w, L"GPU");
    {
        wchar_t v[16]; swprintf(v,16,L"%.0f%%", m.gpu.usage);
        metricRow(g, padL, y, w, L"Load", m.gpu.usage, v, Theme::gpu());
        y += px(16);
        struct E { const wchar_t* n; float v; } es[4] = {
            {L"3D", m.gpu.use3D}, {L"Copy", m.gpu.useCopy},
            {L"Video", m.gpu.useVideo}, {L"Compute", m.gpu.useCompute} };
        float ew = (w - px(24)) / 4;
        for (int i=0;i<4;i++){
            float x = padL + i*(ew+px(8));
            txt(g, es[i].n, fTiny, x, y, Theme::textFaint());
            segBar(g, x, y+px(13), ew, px(6), es[i].v, 0, true, Theme::gpu());
        }
        y += px(24);
        wchar_t d[176];
        swprintf(d,176,L"VRAM %ls / %ls   \x00B7   shared %ls",
                 wbytes(m.gpu.dedicatedUsed).c_str(), wbytes(m.gpu.dedicatedTotal).c_str(),
                 wbytes(m.gpu.sharedUsed).c_str());
        txt(g, d, fTiny, padL, y, Theme::textDim());
        y += px(13);
        float vrpct = m.gpu.dedicatedTotal ? (float)(100.0*m.gpu.dedicatedUsed/m.gpu.dedicatedTotal):0;
        segBar(g, padL, y, w, px(7), vrpct, 0, false, Theme::gpu());
        y += px(12);
        spark(g, padL, y, w, px(20), m.gpu.hist, Theme::gpu(), 100.f);
        y += px(26);
    }

    // ---- DISK ----
    y = sectionHeader(g, padL, y, w, L"DISK");
    {
        wchar_t agg[128];
        swprintf(agg,128,L"R %ls/s   \x00B7   W %ls/s   \x00B7   busy %.0f%%",
                 wrate(m.disk.readBps).c_str(), wrate(m.disk.writeBps).c_str(), m.disk.busyPct);
        txt(g, agg, fMid, padL, y, Theme::text());
        y += px(15);
        int shown = 0;
        for (auto& d : m.disk.disks){
            if (shown >= 3) break;
            float usedPct = d.totalBytes ? (float)(100.0*(d.totalBytes-d.freeBytes)/d.totalBytes) : 0;
            txt(g, widen(d.name), fTiny, padL, y, Theme::textDim());
            segBar(g, padL+px(48), y+px(3), px(120), px(6), usedPct, 0, true, Theme::disk());
            wchar_t info[128];
            swprintf(info,128,L"%ls free   \x00B7   R %ls/s  W %ls/s",
                     wbytes(d.freeBytes).c_str(), wrate(d.readBps).c_str(), wrate(d.writeBps).c_str());
            txt(g, info, fTiny, padL+px(180), y, Theme::textFaint());
            y += px(14); shown++;
        }
        y += px(2);
        sparkPair(g, padL, y, w, px(18), m.disk.histRead, Theme::netRx(),
                  m.disk.histWrite, Theme::disk());
        y += px(24);
    }

    // ---- NETWORK ----
    y = sectionHeader(g, padL, y, w, L"NETWORK");
    {
        for (auto& n : m.net.ifaces){
            std::wstring nm = widen(n.name);
            if (nm.size() > 34) nm = nm.substr(0,33) + L"\x2026";
            txt(g, nm, fTiny, padL, y, Theme::textDim());
            wchar_t info[80];
            swprintf(info,80,L"\x2191 %ls/s   \x2193 %ls/s", wrate(n.txBps).c_str(), wrate(n.rxBps).c_str());
            txt(g, info, fTiny, right, y, Theme::text(), 1);
            y += px(14);
        }
        sparkPair(g, padL, y, w, px(18), m.net.histRx, Theme::netRx(),
                  m.net.histTx, Theme::netTx());
        SolidBrush rb(Theme::netRx()); g.FillEllipse(&rb, padL, y+px(1), px(5), px(5));
        txt(g, L"rx", fTiny, padL+px(8), y-px(1), Theme::textFaint());
        SolidBrush tb(Theme::netTx()); g.FillEllipse(&tb, padL+px(34), y+px(1), px(5), px(5));
        txt(g, L"tx", fTiny, padL+px(42), y-px(1), Theme::textFaint());
        y += px(24);
    }

    // ---- TOP PROCESSES ----
    if (cfg.showProcs){
        y = sectionHeader(g, padL, y, w, L"TOP PROCESSES");
        float colW = (w - px(14)) / 2;
        float xL = padL, xR = padL + colW + px(14);
        txt(g, L"by cpu", fTiny, xL, y, Theme::textFaint());
        txt(g, L"by memory", fTiny, xR, y, Theme::textFaint());
        y += px(13);
        for (int i=0;i<6;i++){
            float yy = y + i*px(13.5f);
            if (i < (int)m.topCpu.size()){
                auto& p = m.topCpu[i];
                std::wstring nm = widen(p.name); if (nm.size()>18) nm=nm.substr(0,17)+L"\x2026";
                txt(g, nm, fTiny, xL, yy, Theme::text());
                wchar_t v[12]; swprintf(v,12,L"%.0f%%", p.cpu);
                txt(g, v, fTiny, xL+colW-px(2), yy, Theme::value(p.cpu), 1);
            }
            if (i < (int)m.topMem.size()){
                auto& p = m.topMem[i];
                std::wstring nm = widen(p.name); if (nm.size()>18) nm=nm.substr(0,17)+L"\x2026";
                txt(g, nm, fTiny, xR, yy, Theme::text());
                txt(g, wbytes(p.mem), fTiny, xR+colW-px(2), yy, Theme::textHi(), 1);
            }
        }
        y += 6*px(13.5f) + px(6);
    }

    statusPill(g, padL, c.GetBottom()-px(20), m);
}

// ============================================================================
//  ai.cpp :: async "Ask your PC" over WinHTTP (system lib; no dependencies)
// ============================================================================
#include "ai.h"
#include <winhttp.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cctype>

// ---------------------------------------------------------------------------
//  shared state
// ---------------------------------------------------------------------------
namespace {
    std::atomic<int> g_phase{0};        // 0 idle, 1 running, 2 done, 3 error
    std::mutex       g_mtx;
    std::string      g_title, g_output;
    uint64_t         g_startTick = 0;
}

bool aiBusy(){ return g_phase.load() == 1; }
AiView aiSnapshot(){
    AiView v; v.phase = g_phase.load();
    std::lock_guard<std::mutex> lk(g_mtx);
    v.title = g_title; v.output = g_output; v.startTick = g_startTick;
    return v;
}
void aiDismiss(){
    if (g_phase.load() == 1) return;    // don't clear a running request
    std::lock_guard<std::mutex> lk(g_mtx);
    g_output.clear(); g_title.clear();
    g_phase.store(0);
}

// ---------------------------------------------------------------------------
//  metrics -> compact context text
// ---------------------------------------------------------------------------
std::string aiBuildContext(const Metrics& m){
    std::string s; char b[512];
    auto line = [&](const char* fmt, ...){ va_list ap; va_start(ap,fmt);
        vsnprintf(b,sizeof b,fmt,ap); va_end(ap); s += b; s += '\n'; };

    line("Host: %s", m.sys.host.c_str());
    line("CPU: %s (%dC/%dT) usage %.0f%% @ %.2fGHz%s",
         m.cpu.name.c_str(), m.cpu.physicalCores, m.cpu.logicalCores,
         m.cpu.usage, m.cpu.curMhz/1000.0,
         m.cpu.tempC >= 0 ? "" : " (temp n/a)");
    line("RAM: %.0f%% used (%s of %s); commit %s/%s; cache %s",
         m.mem.percent, humanBytes(m.mem.usedPhys).c_str(), humanBytes(m.mem.totalPhys).c_str(),
         humanBytes(m.mem.usedPage).c_str(), humanBytes(m.mem.totalPage).c_str(),
         humanBytes(m.mem.cached).c_str());
    line("GPU: %s usage %.0f%% (3D %.0f%%, video %.0f%%); VRAM %s/%s, shared %s",
         m.gpu.name.c_str(), m.gpu.usage, m.gpu.use3D, m.gpu.useVideo,
         humanBytes(m.gpu.dedicatedUsed).c_str(), humanBytes(m.gpu.dedicatedTotal).c_str(),
         humanBytes(m.gpu.sharedUsed).c_str());
    line("Disk: read %s/s write %s/s busy %.0f%%",
         humanRate(m.disk.readBps).c_str(), humanRate(m.disk.writeBps).c_str(), m.disk.busyPct);
    for (auto& d : m.disk.disks)
        line("  %s: %s free of %s", d.name.c_str(),
             humanBytes(d.freeBytes).c_str(), humanBytes(d.totalBytes).c_str());
    line("Network: down %s/s up %s/s", humanRate(m.net.rxBps).c_str(), humanRate(m.net.txBps).c_str());
    s += "Top processes by CPU:\n";
    for (size_t i=0;i<m.topCpu.size() && i<6;i++)
        line("  %s (pid %u): %.0f%% cpu, %s ram",
             m.topCpu[i].name.c_str(), m.topCpu[i].pid, m.topCpu[i].cpu, humanBytes(m.topCpu[i].mem).c_str());
    s += "Top processes by memory:\n";
    for (size_t i=0;i<m.topMem.size() && i<6;i++)
        line("  %s (pid %u): %s ram, %.0f%% cpu",
             m.topMem[i].name.c_str(), m.topMem[i].pid, humanBytes(m.topMem[i].mem).c_str(), m.topMem[i].cpu);
    return s;
}

// ---------------------------------------------------------------------------
//  JSON helpers
// ---------------------------------------------------------------------------
static std::string jesc(const std::string& s){
    std::string o; o.reserve(s.size()+16);
    for (unsigned char c : s){
        switch (c){
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20){ char t[8]; snprintf(t,sizeof t,"\\u%04x",c); o += t; }
                else o += (char)c;
        }
    }
    return o;
}
static void appendUtf8(std::string& o, unsigned cp){
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800){ o += (char)(0xC0|(cp>>6)); o += (char)(0x80|(cp&0x3F)); }
    else if (cp < 0x10000){ o += (char)(0xE0|(cp>>12)); o += (char)(0x80|((cp>>6)&0x3F)); o += (char)(0x80|(cp&0x3F)); }
    else { o += (char)(0xF0|(cp>>18)); o += (char)(0x80|((cp>>12)&0x3F)); o += (char)(0x80|((cp>>6)&0x3F)); o += (char)(0x80|(cp&0x3F)); }
}
static unsigned hex4(const std::string& j, size_t p){
    unsigned v = 0;
    for (int i=0;i<4 && p+i<j.size();++i){
        char c = j[p+i]; v <<= 4;
        if (c>='0'&&c<='9') v|=c-'0';
        else if (c>='a'&&c<='f') v|=c-'a'+10;
        else if (c>='A'&&c<='F') v|=c-'A'+10;
    }
    return v;
}
// extract the first JSON string value for `key` occurring at/after `from`
static bool extractStr(const std::string& j, size_t from, const std::string& key, std::string& out){
    std::string k = "\"" + key + "\"";
    size_t p = j.find(k, from); if (p == std::string::npos) return false;
    p = j.find(':', p + k.size()); if (p == std::string::npos) return false;
    ++p; while (p < j.size() && isspace((unsigned char)j[p])) ++p;
    if (p >= j.size() || j[p] != '"') return false;
    ++p; out.clear();
    while (p < j.size() && j[p] != '"'){
        char c = j[p];
        if (c == '\\' && p+1 < j.size()){
            char e = j[p+1];
            if (e == 'u' && p+5 < j.size()){
                unsigned cp = hex4(j, p+2); p += 6;
                if (cp >= 0xD800 && cp <= 0xDBFF && p+5 < j.size() && j[p]=='\\' && j[p+1]=='u'){
                    unsigned lo = hex4(j, p+2); p += 6;
                    cp = 0x10000 + ((cp-0xD800)<<10) + (lo-0xDC00);
                }
                appendUtf8(out, cp);
            } else {
                p += 2;
                switch (e){ case 'n':out+='\n';break; case 't':out+='\t';break; case 'r':out+='\r';break;
                            case 'b':out+='\b';break; case 'f':out+='\f';break;
                            default: out+=e; }
            }
        } else { out += c; ++p; }
    }
    return true;
}

// ---------------------------------------------------------------------------
//  HTTPS POST via WinHTTP
// ---------------------------------------------------------------------------
static bool httpsPost(const std::wstring& url, const std::wstring& headers,
                      const std::string& body, DWORD& status, std::string& resp, std::string& err){
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof uc;
    wchar_t host[256]={0}, path[1024]={0};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 1023;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)){ err = "Bad endpoint URL"; return false; }
    bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    INTERNET_PORT port = uc.nPort ? uc.nPort : (secure ? 443 : 80);

    HINTERNET hS = WinHttpOpen(L"SYSCORE/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS){ err = "WinHttpOpen failed"; return false; }
    WinHttpSetTimeouts(hS, 10000, 15000, 30000, 60000);
    HINTERNET hC = WinHttpConnect(hS, host, port, 0);
    if (!hC){ err = "Connect failed"; WinHttpCloseHandle(hS); return false; }
    HINTERNET hR = WinHttpOpenRequest(hC, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hR){ err = "OpenRequest failed"; WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return false; }

    bool ok = WinHttpSendRequest(hR, headers.c_str(), (DWORD)-1L,
                                 (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0)
              && WinHttpReceiveResponse(hR, nullptr);
    if (ok){
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        status = code;
        DWORD avail = 0;
        do {
            avail = 0; WinHttpQueryDataAvailable(hR, &avail);
            if (!avail) break;
            std::vector<char> buf(avail);
            DWORD rd = 0;
            if (WinHttpReadData(hR, buf.data(), avail, &rd) && rd) resp.append(buf.data(), rd);
        } while (avail > 0);
    } else {
        err = "Request failed (network/TLS). code " + std::to_string((int)GetLastError());
    }
    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
    return ok;
}

// ---------------------------------------------------------------------------
//  worker
// ---------------------------------------------------------------------------
static void setResult(int phase, const std::string& text){
    std::lock_guard<std::mutex> lk(g_mtx);
    g_output = text; g_phase.store(phase);
}

static void worker(std::string fmt, std::string endpoint, std::string model, std::string key,
                   std::string userPrompt, std::string context){
    const std::string sys =
        "You are SYSCORE, a concise Windows performance assistant. You are given a live system "
        "snapshot. Answer the user's question using the data. Be specific: name exact processes and "
        "numbers. Reply as 2-5 short, actionable bullet points (use '- '). Plain text only, no markdown "
        "headings. Keep it under 150 words.";
    std::string user = userPrompt + "\n\n--- live snapshot ---\n" + context;

    std::string body; std::wstring headers;
    if (fmt == "openai"){
        body  = "{\"model\":\"" + jesc(model) + "\",\"max_tokens\":1024,\"messages\":[";
        body += "{\"role\":\"system\",\"content\":\"" + jesc(sys) + "\"},";
        body += "{\"role\":\"user\",\"content\":\"" + jesc(user) + "\"}]}";
        headers = L"authorization: Bearer " + widen(key) + L"\r\ncontent-type: application/json\r\n";
    } else { // anthropic
        body  = "{\"model\":\"" + jesc(model) + "\",\"max_tokens\":1024,";
        body += "\"system\":\"" + jesc(sys) + "\",";
        body += "\"messages\":[{\"role\":\"user\",\"content\":\"" + jesc(user) + "\"}]}";
        headers = L"x-api-key: " + widen(key) +
                  L"\r\nanthropic-version: 2023-06-01\r\ncontent-type: application/json\r\n";
    }

    DWORD status = 0; std::string resp, err;
    bool ok = httpsPost(widen(endpoint), headers, body, status, resp, err);
    if (!ok){ setResult(3, err + "\n\nCheck your internet connection and endpoint URL."); return; }

    std::string text;
    if (status == 200){
        bool got = (fmt == "openai")
            ? (extractStr(resp, resp.find("\"message\""), "content", text))
            : (extractStr(resp, resp.find("\"content\""), "text", text));
        if (got && !text.empty()){ setResult(2, text); return; }
        setResult(3, "Got a 200 but couldn't parse the reply.\n\n" + resp.substr(0, 600));
        return;
    }
    // non-200: surface the API error message if present
    std::string msg;
    if (extractStr(resp, 0, "message", msg))
        setResult(3, "API error " + std::to_string((int)status) + ": " + msg);
    else
        setResult(3, "HTTP " + std::to_string((int)status) + "\n\n" + resp.substr(0, 600));
}

// ---------------------------------------------------------------------------
//  public entry
// ---------------------------------------------------------------------------
void aiAsk(const Config& cfg, const std::string& title,
           const std::string& userPrompt, const std::string& context){
    if (g_phase.load() == 1) return;                 // one at a time
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_title = title; g_output.clear(); g_startTick = GetTickCount64();
    }
    if (!cfg.aiEnabled){ setResult(3, "AI is disabled. Enable it in the right-click menu."); return; }
    if (cfg.aiKey.empty()){
        setResult(3, "No API key set.\n\nRight-click -> Ask AI -> Open config, paste your key into "
                     "\"aiKey\", set \"aiFormat\" (anthropic or openai) and \"aiEndpoint\", then "
                     "Reload config.");
        return;
    }
    g_phase.store(1);
    std::thread(worker, cfg.aiFormat, cfg.aiEndpoint, cfg.aiModel, cfg.aiKey,
                userPrompt, context).detach();
}

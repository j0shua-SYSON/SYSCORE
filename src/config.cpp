// ============================================================================
//  config.cpp :: tiny hand-rolled JSON config (no dependencies)
//  Path: %APPDATA%\SYSCORE\config.json
// ============================================================================
#include "app.h"
#include <shellapi.h>
#include <string>
#include <cstdlib>

static std::wstring appDataDir(){
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".");
    return base + L"\\SYSCORE";
}
std::wstring configPath(){ return appDataDir() + L"\\config.json"; }

// ---- file IO via Win32 (handles non-ASCII user paths) ----------------------
static std::string readFileUtf8(const std::wstring& path){
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string out; char buf[4096]; DWORD rd = 0;
    while (ReadFile(h, buf, sizeof buf, &rd, nullptr) && rd) out.append(buf, rd);
    CloseHandle(h);
    return out;
}
static bool writeFileUtf8(const std::wstring& path, const std::string& data){
    CreateDirectoryW(appDataDir().c_str(), nullptr);   // ok if it already exists
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(h);
    return ok && wr == data.size();
}

// ---- minimal JSON helpers --------------------------------------------------
static std::string jesc(const std::string& s){
    std::string o;
    for (char c : s){
        switch (c){
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:   o += c;      break;
        }
    }
    return o;
}
static bool findKey(const std::string& j, const std::string& key, size_t& pos){
    std::string k = "\"" + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return false;
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return false;
    pos = p + 1;
    return true;
}
static std::string getStr(const std::string& j, const std::string& key, const std::string& def){
    size_t p; if (!findKey(j, key, p)) return def;
    while (p < j.size() && (j[p]==' '||j[p]=='\t'||j[p]=='\n'||j[p]=='\r')) ++p;
    if (p >= j.size() || j[p] != '"') return def;
    ++p; std::string out;
    while (p < j.size() && j[p] != '"'){
        if (j[p]=='\\' && p+1 < j.size()){
            char c = j[p+1]; p += 2;
            switch (c){ case 'n':out+='\n';break; case 't':out+='\t';break;
                        case 'r':out+='\r';break; default: out+=c; }
        } else out += j[p++];
    }
    return out;
}
static bool getBool(const std::string& j, const std::string& key, bool def){
    size_t p; if (!findKey(j, key, p)) return def;
    size_t q = j.find_first_not_of(" \t\n\r", p);
    if (q == std::string::npos) return def;
    if (j.compare(q,4,"true")==0)  return true;
    if (j.compare(q,5,"false")==0) return false;
    return def;
}
static int getInt(const std::string& j, const std::string& key, int def){
    size_t p; if (!findKey(j, key, p)) return def;
    return atoi(j.c_str() + p);
}

// ---- public API ------------------------------------------------------------
bool saveConfig(const Config& c){
    std::string j;
    j += "{\n";
    j += "  \"_comment\": \"SYSCORE config. Paste your API key in aiKey. aiFormat: anthropic or openai. aiEndpoint is the full URL. theme: midnight|nord|dracula|tokyo|light|matrix.\",\n";
    j += "  \"themeName\": \""  + jesc(c.themeName) + "\",\n";
    j += "  \"opacity\": "      + std::to_string((int)c.opacity) + ",\n";
    j += "  \"targetFps\": "    + std::to_string(c.targetFps) + ",\n";
    j += "  \"alwaysOnTop\": "  + std::string(c.alwaysOnTop ? "true":"false") + ",\n";
    j += "  \"showProcs\": "    + std::string(c.showProcs ? "true":"false") + ",\n";
    j += "  \"expanded\": "     + std::string(c.expanded ? "true":"false") + ",\n";
    j += "  \"aiEnabled\": "    + std::string(c.aiEnabled ? "true":"false") + ",\n";
    j += "  \"aiFormat\": \""   + jesc(c.aiFormat)   + "\",\n";
    j += "  \"aiEndpoint\": \"" + jesc(c.aiEndpoint) + "\",\n";
    j += "  \"aiModel\": \""    + jesc(c.aiModel)    + "\",\n";
    j += "  \"aiKey\": \""      + jesc(c.aiKey)      + "\"\n";
    j += "}\n";
    return writeFileUtf8(configPath(), j);
}

bool loadConfig(Config& c){
    std::string j = readFileUtf8(configPath());
    if (j.empty()){ saveConfig(c); return false; }   // create a template on first run
    c.themeName   = getStr (j, "themeName",  c.themeName);
    int op        = getInt (j, "opacity",    c.opacity);
    c.opacity     = (BYTE)(op < 60 ? 60 : (op > 255 ? 255 : op));
    int fps       = getInt (j, "targetFps",  c.targetFps);
    c.targetFps   = (fps==10||fps==20||fps==30||fps==60) ? fps : 30;
    c.alwaysOnTop = getBool(j, "alwaysOnTop",c.alwaysOnTop);
    c.showProcs   = getBool(j, "showProcs",  c.showProcs);
    c.expanded    = getBool(j, "expanded",   c.expanded);
    c.aiEnabled   = getBool(j, "aiEnabled",  c.aiEnabled);
    c.aiFormat    = getStr (j, "aiFormat",   c.aiFormat);
    c.aiEndpoint  = getStr (j, "aiEndpoint", c.aiEndpoint);
    c.aiModel     = getStr (j, "aiModel",    c.aiModel);
    c.aiKey       = getStr (j, "aiKey",      c.aiKey);
    return true;
}

void openConfigInEditor(){
    Config tmp; loadConfig(tmp);                       // ensure the file exists
    ShellExecuteW(nullptr, L"open", configPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

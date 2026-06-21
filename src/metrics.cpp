// ============================================================================
//  MetricsEngine implementation
//  CPU  : NtQuerySystemInformation (per-core) + CallNtPowerInformation (MHz)
//  MEM  : GlobalMemoryStatusEx + GetPerformanceInfo
//  GPU  : PDH "GPU Engine"/"GPU Adapter Memory" + DXGI (name/VRAM budget)
//  DISK : PDH PhysicalDisk + GetDiskFreeSpaceEx
//  NET  : PDH Network Interface
//  PROC : NtQuerySystemInformation (SystemProcessInformation)
//
//  NOTE: all PDH counters are added with the *English* API so the locale of
//  this machine (Korean) does not break the counter paths.
// ============================================================================
#include "metrics.h"

#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi.h>
#include <psapi.h>
#include <powrprof.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>

// IID_IDXGIFactory1 declared locally so we need neither __uuidof nor -ldxguid.
static const GUID kIID_IDXGIFactory1 =
    {0x770aae78,0xf26f,0x4dba,{0xa8,0x29,0x25,0x3c,0x83,0xd1,0xb3,0x87}};

// ----- ntdll bits we declare ourselves -------------------------------------
typedef LONG NTSTATUS_;
typedef LONG (WINAPI *PFN_NTQSI)(ULONG, PVOID, ULONG, PULONG);

#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((LONG)0xC0000004)
#endif

enum { SystemProcessorPerformanceInformation = 8, SystemProcessInformation = 5 };

// windows.h doesn't expose UNICODE_STRING here; define our own to avoid pulling
// in <winternl.h> (which would clash with the structs we hand-roll below).
typedef struct _MYUNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} MYUNICODE_STRING;

typedef struct _SPPI {
    LARGE_INTEGER IdleTime, KernelTime, UserTime, DpcTime, InterruptTime;
    ULONG InterruptCount;
} SPPI;

typedef struct _MYS_SPI {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    MYUNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
} MYS_SPI;

// PROCESSOR_POWER_INFORMATION (ProcessorInformation == POWER_INFORMATION_LEVEL 11)
typedef struct _PPI {
    ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
} PPI;

static PFN_NTQSI ntQSI() {
    static PFN_NTQSI p = (PFN_NTQSI)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                   "NtQuerySystemInformation");
    return p;
}

// ===========================================================================
//  formatting helpers
// ===========================================================================
std::string humanBytes(uint64_t b, int decimals) {
    const char* u[] = {"B","K","M","G","T","P"};
    double v = (double)b; int i = 0;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; ++i; }
    char buf[64];
    if (i == 0) snprintf(buf, sizeof buf, "%llu%s", (unsigned long long)b, u[0]);
    else        snprintf(buf, sizeof buf, "%.*f%s", decimals, v, u[i]);
    return buf;
}
std::string humanRate(double bps) {
    const char* u[] = {"B","K","M","G","T"};
    double v = bps < 0 ? 0 : bps; int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    char buf[48];
    snprintf(buf, sizeof buf, "%.1f%s", v, u[i]);
    return buf;
}
std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string narrow(const wchar_t* w, int len = -1) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], n, nullptr, nullptr);
    if (len == -1 && !s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static std::string lower(std::string s){ for(auto&c:s) c=(char)tolower((unsigned char)c); return s; }

// ===========================================================================
//  PDH helpers
// ===========================================================================
struct PdhItem { std::string instance; double value; };

static void* addEnglish(void* q, const char* path) {
    PDH_HCOUNTER h = nullptr;
    if (PdhAddEnglishCounterA((PDH_HQUERY)q, path, 0, &h) != ERROR_SUCCESS) return nullptr;
    return h;
}
static std::vector<PdhItem> readArray(void* counter) {
    std::vector<PdhItem> out;
    if (!counter) return out;
    DWORD bufSize = 0, itemCount = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayA((PDH_HCOUNTER)counter,
                        PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bufSize, &itemCount, nullptr);
    if (st != PDH_MORE_DATA || bufSize == 0) return out;
    std::vector<BYTE> buf(bufSize);
    auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_A*>(buf.data());
    st = PdhGetFormattedCounterArrayA((PDH_HCOUNTER)counter,
                        PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bufSize, &itemCount, items);
    if (st != ERROR_SUCCESS) return out;
    out.reserve(itemCount);
    for (DWORD i = 0; i < itemCount; ++i) {
        double v = (items[i].FmtValue.CStatus == ERROR_SUCCESS) ? items[i].FmtValue.doubleValue : 0.0;
        out.push_back({ items[i].szName ? items[i].szName : "", v });
    }
    return out;
}

// ===========================================================================
//  init
// ===========================================================================
bool MetricsEngine::init() {
    Metrics tmp;
    loadStaticInfo(tmp);
    loadDxgiAdapters();

    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryA(nullptr, 0, &q) == ERROR_SUCCESS) {
        query_ = q;
        cGpuUtil_  = addEnglish(q, "\\GPU Engine(*)\\Utilization Percentage");
        cGpuDed_   = addEnglish(q, "\\GPU Adapter Memory(*)\\Dedicated Usage");
        cGpuShr_   = addEnglish(q, "\\GPU Adapter Memory(*)\\Shared Usage");
        cDiskRd_   = addEnglish(q, "\\PhysicalDisk(*)\\Disk Read Bytes/sec");
        cDiskWr_   = addEnglish(q, "\\PhysicalDisk(*)\\Disk Write Bytes/sec");
        cDiskBusy_ = addEnglish(q, "\\PhysicalDisk(*)\\% Disk Time");
        cNetRx_    = addEnglish(q, "\\Network Interface(*)\\Bytes Received/sec");
        cNetTx_    = addEnglish(q, "\\Network Interface(*)\\Bytes Sent/sec");
        pdhReady_  = true;
        PdhCollectQueryData(q);          // prime (rate counters need a baseline)
    }
    return true;
}

// ===========================================================================
//  static info
// ===========================================================================
void MetricsEngine::loadStaticInfo(Metrics&) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    logCores_ = (int)si.dwNumberOfProcessors;

    // physical cores
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len) {
        std::vector<BYTE> buf(len);
        if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len)) {
            BYTE* p = buf.data(); BYTE* end = p + len; int n = 0;
            while (p < end) {
                auto* e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)p;
                if (e->Relationship == RelationProcessorCore) ++n;
                p += e->Size;
            }
            physCores_ = n;
        }
    }
    if (!physCores_) physCores_ = logCores_;

    // CPU brand string
    char name[256] = {0}; DWORD nlen = sizeof name;
    if (RegGetValueA(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            "ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &nlen) == ERROR_SUCCESS) {
        cpuName_ = name;
        // collapse runs of spaces
        std::string c; bool sp = false;
        for (char ch : cpuName_) { if (ch==' ') { if(!sp){c+=' '; sp=true;} } else { c+=ch; sp=false; } }
        while (!c.empty() && c.back()==' ') c.pop_back();
        while (!c.empty() && c.front()==' ') c.erase(c.begin());
        cpuName_ = c;
    } else cpuName_ = "CPU";

    prevIdle_.assign(logCores_, 0);
    prevKernel_.assign(logCores_, 0);
    prevUser_.assign(logCores_, 0);
}

void MetricsEngine::loadDxgiAdapters() {
    IDXGIFactory1* f = nullptr;
    if (CreateDXGIFactory1(kIID_IDXGIFactory1, (void**)&f) != S_OK || !f) return;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        if (a->GetDesc1(&d) == S_OK) {
            char key[64];
            snprintf(key, sizeof key, "luid_0x%08x_0x%08x",
                     (unsigned)d.AdapterLuid.HighPart, (unsigned)d.AdapterLuid.LowPart);
            Adapter ad;
            ad.name = narrow(d.Description);
            ad.dedTotal = d.DedicatedVideoMemory;
            ad.sharedTotal = d.SharedSystemMemory;
            ad.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            adapters_[lower(key)] = ad;
        }
        a->Release(); a = nullptr;
    }
    f->Release();
}

// ===========================================================================
//  sample (driver)
// ===========================================================================
void MetricsEngine::sample(Metrics& m) {
    m.cpu.name = cpuName_;
    m.cpu.physicalCores = physCores_;
    m.cpu.logicalCores  = logCores_;

    if (pdhReady_) PdhCollectQueryData((PDH_HQUERY)query_);

    sampleCpu(m);
    sampleFreq(m);
    sampleMem(m);
    sampleGpu(m);
    sampleDiskNet(m);
    sampleProcs(m);

    // sys / uptime
    char host[256] = {0}; DWORD hl = sizeof host;
    GetComputerNameA(host, &hl); m.sys.host = host;
    m.uptimeSec = GetTickCount64() / 1000;

    pdhPrimed_ = true;
}

// ===========================================================================
//  CPU
// ===========================================================================
void MetricsEngine::sampleCpu(Metrics& m) {
    auto qsi = ntQSI();
    if (!qsi || logCores_ <= 0) return;
    std::vector<SPPI> info(logCores_);
    ULONG need = 0;
    LONG st = qsi(SystemProcessorPerformanceInformation, info.data(),
                  (ULONG)(info.size()*sizeof(SPPI)), &need);
    if (st != 0) return;

    m.cpu.cores.resize(logCores_);
    double aggBusy = 0, aggTotal = 0;
    for (int i = 0; i < logCores_; ++i) {
        uint64_t idle   = (uint64_t)info[i].IdleTime.QuadPart;
        uint64_t kernel = (uint64_t)info[i].KernelTime.QuadPart;  // includes idle
        uint64_t user   = (uint64_t)info[i].UserTime.QuadPart;
        uint64_t dIdle   = idle   - prevIdle_[i];
        uint64_t dKernel = kernel - prevKernel_[i];
        uint64_t dUser   = user   - prevUser_[i];
        uint64_t total   = dKernel + dUser;
        uint64_t busy    = (total > dIdle) ? (total - dIdle) : 0;
        float u = total ? (float)(100.0 * busy / total) : 0.f;
        if (!cpuPrimed_) u = 0.f;
        m.cpu.cores[i].usage = clampf(u, 0, 100);
        aggBusy += busy; aggTotal += total;
        prevIdle_[i] = idle; prevKernel_[i] = kernel; prevUser_[i] = user;
    }
    m.cpu.usage = (cpuPrimed_ && aggTotal) ? clampf((float)(100.0*aggBusy/aggTotal),0,100) : 0.f;
    m.cpu.hist.push(m.cpu.usage);
    cpuPrimed_ = true;
}

void MetricsEngine::sampleFreq(Metrics& m) {
    std::vector<PPI> p(logCores_);
    if (CallNtPowerInformation((POWER_INFORMATION_LEVEL)11, nullptr, 0,
                               p.data(), (ULONG)(p.size()*sizeof(PPI))) == 0) {
        double cur = 0, mx = 0;
        for (int i = 0; i < logCores_ && i < (int)m.cpu.cores.size(); ++i) {
            m.cpu.cores[i].mhz = p[i].CurrentMhz;
            if (p[i].CurrentMhz > cur) cur = p[i].CurrentMhz;
            if (p[i].MaxMhz > mx) mx = p[i].MaxMhz;
        }
        m.cpu.curMhz = cur;
        m.cpu.maxMhz = mx;
        maxMhz_ = mx;
    }
}

// ===========================================================================
//  Memory
// ===========================================================================
void MetricsEngine::sampleMem(Metrics& m) {
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) {
        m.mem.totalPhys = ms.ullTotalPhys;
        m.mem.availPhys = ms.ullAvailPhys;
        m.mem.usedPhys  = ms.ullTotalPhys - ms.ullAvailPhys;
        m.mem.percent   = (float)ms.dwMemoryLoad;
    }
    PERFORMANCE_INFORMATION pi{}; pi.cb = sizeof pi;
    if (GetPerformanceInfo(&pi, sizeof pi)) {
        uint64_t pg = pi.PageSize;
        m.mem.usedPage  = (uint64_t)pi.CommitTotal * pg;
        m.mem.totalPage = (uint64_t)pi.CommitLimit * pg;
        m.mem.cached    = (uint64_t)pi.SystemCache * pg;
    }
    m.mem.hist.push(m.mem.percent);
}

// ===========================================================================
//  GPU  (engine util aggregated by type; VRAM from adapter-memory + DXGI)
// ===========================================================================
static std::string betweenTok(const std::string& s, const char* a, const char* b) {
    size_t i = s.find(a); if (i == std::string::npos) return "";
    i += strlen(a);
    size_t j = b ? s.find(b, i) : std::string::npos;
    return s.substr(i, j == std::string::npos ? std::string::npos : j - i);
}

void MetricsEngine::sampleGpu(Metrics& m) {
    if (!pdhReady_) return;

    // ---- engine utilization, summed per engine type ----
    double s3d=0, scopy=0, svideo=0, scompute=0, sother=0;
    for (auto& it : readArray(cGpuUtil_)) {
        // instance: pid_X_luid_..._eng_N_engtype_TYPE
        size_t k = it.instance.find("engtype_");
        std::string type = (k==std::string::npos) ? "" : it.instance.substr(k+8);
        double v = it.value;
        if      (type.find("3d")    != std::string::npos) s3d     += v;
        else if (type.find("copy")  != std::string::npos) scopy   += v;
        else if (type.find("video") != std::string::npos) svideo  += v;
        else if (type.find("compute")!=std::string::npos) scompute+= v;
        else sother += v;
    }
    m.gpu.use3D = clampf((float)s3d,0,100);
    m.gpu.useCopy = clampf((float)scopy,0,100);
    m.gpu.useVideo = clampf((float)svideo,0,100);
    m.gpu.useCompute = clampf((float)scompute,0,100);
    double overall = s3d; for (double x : {scopy,svideo,scompute}) if (x>overall) overall=x;
    m.gpu.usage = clampf((float)overall,0,100);

    // ---- dedicated/shared memory: pick the busiest luid ----
    auto ded = readArray(cGpuDed_);
    auto shr = readArray(cGpuShr_);
    std::string bestLuid; uint64_t bestDed = 0;
    for (auto& it : ded) {
        uint64_t b = (uint64_t)it.value;
        std::string luid = it.instance.substr(0, it.instance.find("_phys"));
        if (b >= bestDed) { bestDed = b; bestLuid = luid; }
    }
    m.gpu.dedicatedUsed = bestDed;
    for (auto& it : shr) {
        std::string luid = it.instance.substr(0, it.instance.find("_phys"));
        if (luid == bestLuid) { m.gpu.sharedUsed = (uint64_t)it.value; break; }
    }

    // ---- map luid -> DXGI for name + VRAM budget ----
    auto a = adapters_.find(lower(bestLuid));
    if (a != adapters_.end()) {
        m.gpu.name = a->second.name;
        m.gpu.dedicatedTotal = a->second.dedTotal;
    }
    if (m.gpu.name.empty() || m.gpu.name == "GPU") {
        // fall back to first non-software adapter
        for (auto& kv : adapters_) if (!kv.second.software) { m.gpu.name = kv.second.name; if(!m.gpu.dedicatedTotal) m.gpu.dedicatedTotal=kv.second.dedTotal; break; }
    }
    if (m.gpu.dedicatedTotal < m.gpu.dedicatedUsed)
        m.gpu.dedicatedTotal = m.gpu.dedicatedUsed;   // safety for UMA quirks
    m.gpu.available = !adapters_.empty();
    m.gpu.hist.push(m.gpu.usage);
}

// ===========================================================================
//  Disk + Network
// ===========================================================================
void MetricsEngine::sampleDiskNet(Metrics& m) {
    if (!pdhReady_) return;

    auto rd = readArray(cDiskRd_);
    auto wr = readArray(cDiskWr_);
    auto bz = readArray(cDiskBusy_);
    auto findv = [](std::vector<PdhItem>& v, const std::string& inst)->double{
        for (auto& it : v) if (it.instance == inst) return it.value; return 0.0; };

    m.disk.disks.clear();
    for (auto& it : rd) {
        std::string inst = it.instance;
        std::string li = lower(inst);
        if (li == "_total") {
            m.disk.readBps  = it.value;
            m.disk.writeBps = findv(wr, inst);
            m.disk.busyPct  = clampf((float)findv(bz, inst), 0, 100);
            continue;
        }
        DiskInfo d;
        d.name = inst;
        d.readBps  = it.value;
        d.writeBps = findv(wr, inst);
        d.busyPct  = clampf((float)findv(bz, inst), 0, 100);
        // parse first "X:" drive letter for free space
        for (size_t i=0;i+1<inst.size();++i){
            if (isalpha((unsigned char)inst[i]) && inst[i+1]==':'){
                char root[4] = { (char)toupper(inst[i]), ':', '\\', 0 };
                ULARGE_INTEGER freeAvail, total, totalFree;
                if (GetDiskFreeSpaceExA(root, &freeAvail, &total, &totalFree)) {
                    d.freeBytes = totalFree.QuadPart; d.totalBytes = total.QuadPart;
                }
                break;
            }
        }
        m.disk.disks.push_back(d);
    }
    m.disk.histRead.push((float)m.disk.readBps);
    m.disk.histWrite.push((float)m.disk.writeBps);

    // network
    auto nrx = readArray(cNetRx_);
    auto ntx = readArray(cNetTx_);
    m.net.ifaces.clear(); m.net.rxBps = 0; m.net.txBps = 0;
    for (auto& it : nrx) {
        std::string li = lower(it.instance);
        if (li.find("loopback")!=std::string::npos || li.find("isatap")!=std::string::npos ||
            li.find("teredo")!=std::string::npos) continue;
        double tx = findv(ntx, it.instance);
        NetIf n; n.name = it.instance; n.rxBps = it.value; n.txBps = tx;
        m.net.rxBps += it.value; m.net.txBps += tx;
        if (it.value > 1 || tx > 1 || m.net.ifaces.size() < 4)
            m.net.ifaces.push_back(n);
    }
    m.net.histRx.push((float)m.net.rxBps);
    m.net.histTx.push((float)m.net.txBps);
}

// ===========================================================================
//  Top processes
// ===========================================================================
void MetricsEngine::sampleProcs(Metrics& m) {
    auto qsi = ntQSI();
    if (!qsi) return;
    ULONG sz = 512 * 1024;
    std::vector<BYTE> buf(sz);
    for (;;) {
        ULONG need = 0;
        LONG st = qsi(SystemProcessInformation, buf.data(), (ULONG)buf.size(), &need);
        if (st == 0) break;
        if (st == STATUS_INFO_LENGTH_MISMATCH) { buf.resize(need ? need + 64*1024 : buf.size()*2); continue; }
        return;
    }

    uint64_t nowTick = GetTickCount64();
    double elapsedMs = lastProcTick_ ? (double)(nowTick - lastProcTick_) : 1000.0;
    if (elapsedMs < 1) elapsedMs = 1;
    // denominator: full machine capacity over the interval, in 100ns units
    double capacity = elapsedMs * 10000.0 * (double)logCores_;

    std::vector<ProcInfo> all;
    std::map<uint32_t, ProcPrev> cur;
    BYTE* p = buf.data();
    for (;;) {
        auto* e = reinterpret_cast<MYS_SPI*>(p);
        uint32_t pid = (uint32_t)(uintptr_t)e->UniqueProcessId;
        uint64_t total = (uint64_t)e->KernelTime.QuadPart + (uint64_t)e->UserTime.QuadPart;
        cur[pid].total = total;
        if (pid != 0) {
            ProcInfo pi;
            pi.pid = pid;
            pi.name = e->ImageName.Buffer
                        ? narrow(e->ImageName.Buffer, e->ImageName.Length/2)
                        : "System";
            pi.mem = (uint64_t)e->WorkingSetSize;
            auto it = procPrev_.find(pid);
            if (it != procPrev_.end() && lastProcTick_) {
                uint64_t d = total >= it->second.total ? total - it->second.total : 0;
                pi.cpu = clampf((float)(100.0 * (double)d / capacity), 0, 100);
            } else pi.cpu = 0.f;
            all.push_back(pi);
        }
        if (e->NextEntryOffset == 0) break;
        p += e->NextEntryOffset;
    }
    procPrev_.swap(cur);
    lastProcTick_ = nowTick;

    m.topCpu = all;
    std::sort(m.topCpu.begin(), m.topCpu.end(),
              [](const ProcInfo&a,const ProcInfo&b){ return a.cpu > b.cpu; });
    if (m.topCpu.size() > 8) m.topCpu.resize(8);

    m.topMem = all;
    std::sort(m.topMem.begin(), m.topMem.end(),
              [](const ProcInfo&a,const ProcInfo&b){ return a.mem > b.mem; });
    if (m.topMem.size() > 8) m.topMem.resize(8);
}

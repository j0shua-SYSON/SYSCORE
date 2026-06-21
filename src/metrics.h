// ============================================================================
//  MetricsEngine :: collects everything once per tick into a Metrics snapshot
// ============================================================================
#pragma once
#include "app.h"
#include <map>

class MetricsEngine {
public:
    bool init();                 // static info + PDH/DXGI setup; primes counters
    void sample(Metrics& m);     // refresh one snapshot (call ~1 Hz)

private:
    // --- static / one-time ---
    void loadStaticInfo(Metrics& m);
    void loadDxgiAdapters();

    // --- per-tick collectors ---
    void sampleCpu(Metrics& m);
    void sampleFreq(Metrics& m);
    void sampleMem(Metrics& m);
    void sampleGpu(Metrics& m);
    void sampleDiskNet(Metrics& m);
    void sampleProcs(Metrics& m);

    // CPU previous counters (per logical processor)
    std::vector<uint64_t> prevIdle_, prevKernel_, prevUser_;
    bool   cpuPrimed_ = false;

    // static CPU
    std::string cpuName_;
    int physCores_ = 0, logCores_ = 0;
    double maxMhz_ = 0;

    // process cpu deltas
    struct ProcPrev { uint64_t total = 0; };
    std::map<uint32_t, ProcPrev> procPrev_;
    uint64_t lastProcTick_ = 0;

    // DXGI adapter info, keyed by lowercase "luid_0x..._0x..." string
    struct Adapter { std::string name; uint64_t dedTotal = 0, sharedTotal = 0; bool software = false; };
    std::map<std::string, Adapter> adapters_;

    // PDH
    void*  query_ = nullptr;     // PDH_HQUERY
    void*  cGpuUtil_ = nullptr;  // \GPU Engine(*)\Utilization Percentage
    void*  cGpuDed_  = nullptr;  // \GPU Adapter Memory(*)\Dedicated Usage
    void*  cGpuShr_  = nullptr;  // \GPU Adapter Memory(*)\Shared Usage
    void*  cDiskRd_  = nullptr;  // \PhysicalDisk(*)\Disk Read Bytes/sec
    void*  cDiskWr_  = nullptr;  // \PhysicalDisk(*)\Disk Write Bytes/sec
    void*  cDiskBusy_= nullptr;  // \PhysicalDisk(*)\% Disk Time
    void*  cNetRx_   = nullptr;  // \Network Interface(*)\Bytes Received/sec
    void*  cNetTx_   = nullptr;  // \Network Interface(*)\Bytes Sent/sec
    bool   pdhReady_ = false;
    bool   pdhPrimed_ = false;
};

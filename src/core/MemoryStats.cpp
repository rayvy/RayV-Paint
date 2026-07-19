#include "MemoryStats.h"
#include "Logger.h"

#include <sstream>
#include <iomanip>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

ProcessMemoryInfo MemoryStats::QueryProcess() {
    ProcessMemoryInfo info;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc))) {
        info.workingSetBytes = static_cast<size_t>(pmc.WorkingSetSize);
        info.privateBytes = static_cast<size_t>(pmc.PrivateUsage);
        info.peakWorkingSetBytes = static_cast<size_t>(pmc.PeakWorkingSetSize);
    }

    MEMORYSTATUSEX mem = {};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        info.totalPhysBytes = static_cast<size_t>(mem.ullTotalPhys);
        info.availPhysBytes = static_cast<size_t>(mem.ullAvailPhys);
    }
#endif
    return info;
}

size_t MemoryStats::EstimateTileBytes(size_t tileCount, int bytesPerPixel, int tileSize) {
    return tileCount * static_cast<size_t>(tileSize) * static_cast<size_t>(tileSize) * static_cast<size_t>(bytesPerPixel);
}

size_t MemoryStats::EstimateImageBytes(int width, int height, int bytesPerPixel) {
    if (width <= 0 || height <= 0 || bytesPerPixel <= 0) return 0;
    return static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(bytesPerPixel);
}

std::string MemoryStats::FormatBytes(size_t bytes) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    const double b = static_cast<double>(bytes);
    if (bytes >= (1ull << 30)) {
        oss << (b / (1ull << 30)) << " GiB";
    } else if (bytes >= (1ull << 20)) {
        oss << (b / (1ull << 20)) << " MiB";
    } else if (bytes >= (1ull << 10)) {
        oss << (b / (1ull << 10)) << " KiB";
    } else {
        oss << bytes << " B";
    }
    return oss.str();
}

bool MemoryStats::ExceedsRamBudget(size_t estimatedBytes, double fractionOfTotal) {
    auto info = QueryProcess();
    if (info.totalPhysBytes == 0) return false;
    const size_t budget = static_cast<size_t>(static_cast<double>(info.totalPhysBytes) * fractionOfTotal);
    return estimatedBytes > budget;
}

void MemoryStats::LogSoftBudget(const std::string& context, size_t estimatedBytes,
                                double fractionOfTotal) {
    auto info = QueryProcess();
    const size_t budget = info.totalPhysBytes
        ? static_cast<size_t>(static_cast<double>(info.totalPhysBytes) * fractionOfTotal)
        : 0;
    Logger::Get().WarnTag("mem",
        "soft_budget " + context +
        " est=" + FormatBytes(estimatedBytes) +
        " soft=" + FormatBytes(budget) +
        " (" + std::to_string((int)(fractionOfTotal * 100.0)) + "% RAM)" +
        " avail=" + FormatBytes(info.availPhysBytes) +
        " — proceeding (soft limit, not refuse)");
}

bool MemoryStats::ExceedsHardRamCeiling(size_t estimatedBytes) {
    // Absolute absurdity guard (overflow / corrupt dims), not everyday soft budget.
    constexpr size_t kAbsMax = 64ull * 1024ull * 1024ull * 1024ull; // 64 GiB single estimate
    if (estimatedBytes > kAbsMax) return true;
    auto info = QueryProcess();
    if (info.totalPhysBytes == 0) return false;
    const size_t hard = static_cast<size_t>(static_cast<double>(info.totalPhysBytes) * 0.95);
    return estimatedBytes > hard;
}

void MemoryStats::LogSnapshot(const std::string& label) {
    auto info = QueryProcess();
    std::ostringstream oss;
    oss << "[mem] " << label
        << " | WS=" << FormatBytes(info.workingSetBytes)
        << " Private=" << FormatBytes(info.privateBytes)
        << " PeakWS=" << FormatBytes(info.peakWorkingSetBytes)
        << " SysRAM=" << FormatBytes(info.totalPhysBytes)
        << " Avail=" << FormatBytes(info.availPhysBytes);
    if (s_VramBudgetBytes > 0) {
        oss << " | simVRAM=" << FormatBytes(s_VramUsedBytes)
            << "/" << FormatBytes(s_VramBudgetBytes);
    }
    Logger::Get().Info(oss.str());
}

// Simulated VRAM (process-local estimate — not DXGI adapter query)
size_t MemoryStats::s_VramBudgetBytes = 0;
size_t MemoryStats::s_VramUsedBytes = 0;
size_t MemoryStats::s_VramPeakBytes = 0;
size_t MemoryStats::s_VramRefuseCount = 0;

void MemoryStats::SetSimulatedVramBudgetBytes(size_t bytes) {
    s_VramBudgetBytes = bytes;
    Logger::Get().InfoTag("mem",
        std::string("Simulated VRAM budget = ") +
        (bytes ? FormatBytes(bytes) : std::string("unlimited")));
}

size_t MemoryStats::GetSimulatedVramBudgetBytes() { return s_VramBudgetBytes; }
size_t MemoryStats::GetSimulatedVramUsedBytes() { return s_VramUsedBytes; }
size_t MemoryStats::GetSimulatedVramPeakBytes() { return s_VramPeakBytes; }
size_t MemoryStats::GetSimulatedVramRefuseCount() { return s_VramRefuseCount; }

void MemoryStats::ResetSimulatedVram() {
    s_VramUsedBytes = 0;
    s_VramPeakBytes = 0;
    s_VramRefuseCount = 0;
}

bool MemoryStats::TryReserveVram(size_t bytes, const char* context) {
    if (s_VramBudgetBytes == 0 || bytes == 0) {
        s_VramUsedBytes += bytes;
        if (s_VramUsedBytes > s_VramPeakBytes) s_VramPeakBytes = s_VramUsedBytes;
        return true;
    }
    if (s_VramUsedBytes + bytes > s_VramBudgetBytes) {
        ++s_VramRefuseCount;
        // Rate-limit: log 1st, then every 256th — flood was causing 6s "hangs" under stress.
        if (s_VramRefuseCount == 1 || (s_VramRefuseCount % 256ull) == 0) {
            Logger::Get().WarnTag("mem",
                std::string("sim_vram REFUSE ") + (context ? context : "alloc") +
                " need=" + FormatBytes(bytes) +
                " used=" + FormatBytes(s_VramUsedBytes) +
                " budget=" + FormatBytes(s_VramBudgetBytes) +
                " refuses=" + std::to_string(s_VramRefuseCount) +
                " — soft degrade (no crash)");
        }
        return false;
    }
    s_VramUsedBytes += bytes;
    if (s_VramUsedBytes > s_VramPeakBytes) s_VramPeakBytes = s_VramUsedBytes;
    return true;
}

void MemoryStats::ReleaseVram(size_t bytes) {
    if (bytes == 0) return;
    if (bytes >= s_VramUsedBytes) s_VramUsedBytes = 0;
    else s_VramUsedBytes -= bytes;
}

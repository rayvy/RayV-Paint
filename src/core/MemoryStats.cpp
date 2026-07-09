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

void MemoryStats::LogSnapshot(const std::string& label) {
    auto info = QueryProcess();
    std::ostringstream oss;
    oss << "[mem] " << label
        << " | WS=" << FormatBytes(info.workingSetBytes)
        << " Private=" << FormatBytes(info.privateBytes)
        << " PeakWS=" << FormatBytes(info.peakWorkingSetBytes)
        << " SysRAM=" << FormatBytes(info.totalPhysBytes)
        << " Avail=" << FormatBytes(info.availPhysBytes);
    Logger::Get().Info(oss.str());
}

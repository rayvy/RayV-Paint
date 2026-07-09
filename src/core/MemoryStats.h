#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Process + document memory diagnostics for large-texture work.
struct ProcessMemoryInfo {
    size_t workingSetBytes = 0;
    size_t privateBytes = 0;   // PagefileUsage on Windows
    size_t peakWorkingSetBytes = 0;
    size_t totalPhysBytes = 0;
    size_t availPhysBytes = 0;
};

class MemoryStats {
public:
    static ProcessMemoryInfo QueryProcess();
    static size_t EstimateTileBytes(size_t tileCount, int bytesPerPixel, int tileSize = 256);
    static size_t EstimateImageBytes(int width, int height, int bytesPerPixel);

    // Logs working set / private / system RAM with a label, e.g. "after_dds_load".
    static void LogSnapshot(const std::string& label);

    // True if estimatedBytes would exceed fraction of total physical RAM (default 45%).
    static bool ExceedsRamBudget(size_t estimatedBytes, double fractionOfTotal = 0.45);

    static std::string FormatBytes(size_t bytes);
};

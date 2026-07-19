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
    // SOFT policy (EMERGENCY_SAFE_PLAN): callers should WARN and still proceed for normal
    // document ops. Use only as a soft budget signal — not a hard refuse for paint/mask/undo.
    static bool ExceedsRamBudget(size_t estimatedBytes, double fractionOfTotal = 0.45);

    // Log a soft-budget advisory (does not refuse the operation).
    static void LogSoftBudget(const std::string& context, size_t estimatedBytes,
                              double fractionOfTotal = 0.45);

    // True only for absurd sizes (e.g. estimate > 95% of physical RAM or > 64 GiB).
    // Rare hard ceiling for mathematical / corrupt paths — not everyday soft budget.
    static bool ExceedsHardRamCeiling(size_t estimatedBytes);

    // --- Simulated VRAM budget (stress / low-end GPU rehearsal) ---
    // 0 = unlimited (default). Soft: TryReserveVram returns false when exceeded;
    // callers should degrade (skip atlas page / leave needsUpload) — never crash.
    static void SetSimulatedVramBudgetBytes(size_t bytes);
    static size_t GetSimulatedVramBudgetBytes();
    static size_t GetSimulatedVramUsedBytes();
    // Reserve estimated GPU alloc; false if over budget (no reserve).
    static bool TryReserveVram(size_t bytes, const char* context = nullptr);
    static void ReleaseVram(size_t bytes);
    static void ResetSimulatedVram();
    static size_t GetSimulatedVramPeakBytes();
    static size_t GetSimulatedVramRefuseCount();

    static std::string FormatBytes(size_t bytes);

private:
    static size_t s_VramBudgetBytes;
    static size_t s_VramUsedBytes;
    static size_t s_VramPeakBytes;
    static size_t s_VramRefuseCount;
};

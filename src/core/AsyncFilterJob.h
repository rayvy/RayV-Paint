#pragma once

#include "TileCache.h"
#include "../layer/LayerTypes.h"

#include <atomic>
#include <cstdint>
#include <future>
#include <mutex>
#include <vector>

// Async CPU filter bake: snapshot tiles on main thread, process on pool, apply on main.
namespace async_fx {

struct TileResult {
    int tx = 0, ty = 0;
    int x0 = 0, y0 = 0;
    int w = 0, h = 0;
    std::vector<float> rgba; // w*h*4
};

struct FilterJobResult {
    int layerIdx = -1;
    uint64_t generation = 0;
    bool ok = false;
    std::vector<TileResult> tiles;
};

// Build job input from live layer (main thread only).
struct FilterJobInput {
    int layerIdx = -1;
    uint64_t generation = 0;
    int docW = 0, docH = 0;
    CanvasPixelFormat format = CanvasPixelFormat::RGBA8;
    std::vector<LayerFilter> filters;
    // Seed tiles (dirty) + halo already expanded by caller as list of (tx,ty)
    // For each seed: full tile RGBA32F export of expanded region is done in worker
    // from packed snapshots:
    struct SnapTile {
        int tx = 0, ty = 0;
        int validW = 0, validH = 0;
        std::vector<uint8_t> bytes; // TILE_SIZE pitch
    };
    std::vector<SnapTile> contentTiles; // all tiles needed (seeds + neighbors)
    std::vector<std::pair<int, int>> seeds;
    int halo = 0;
};

FilterJobResult RunFilterJob(FilterJobInput input);

// Manager: at most one in-flight job per layer index.
class AsyncFilterQueue {
public:
    void Submit(FilterJobInput input); // cancels previous for same layer via generation
    // Poll completed jobs; returns results ready to apply (may be empty).
    std::vector<FilterJobResult> Poll();
    void CancelAll();
    bool IsBusy(int layerIdx) const;

private:
    struct Entry {
        int layerIdx = -1;
        uint64_t generation = 0;
        std::future<FilterJobResult> fut;
    };
    mutable std::mutex m_Mu;
    std::vector<Entry> m_InFlight;
    std::atomic<uint64_t> m_Gen{1};
};

} // namespace async_fx

#include "AsyncFilterJob.h"
#include "HalfFloat.h"
#include "ThreadPool.h"
#include "../layer/LayerStyles.h"

#include <algorithm>
#include <cstring>

namespace async_fx {
namespace {

void DecodeTileToFloat(const FilterJobInput::SnapTile& snap, CanvasPixelFormat fmt,
                       std::vector<float>& out) {
    out.assign((size_t)snap.validW * snap.validH * 4, 0.f);
    if (snap.bytes.empty() || snap.validW < 1 || snap.validH < 1) return;
    const uint8_t* raw = snap.bytes.data();
    for (int ly = 0; ly < snap.validH; ++ly) {
        for (int lx = 0; lx < snap.validW; ++lx) {
            size_t di = ((size_t)ly * snap.validW + lx) * 4;
            if (fmt == CanvasPixelFormat::RGBA8) {
                size_t si = ((size_t)ly * TILE_SIZE + lx) * 4;
                out[di + 0] = raw[si + 0] / 255.f;
                out[di + 1] = raw[si + 1] / 255.f;
                out[di + 2] = raw[si + 2] / 255.f;
                out[di + 3] = raw[si + 3] / 255.f;
            } else if (fmt == CanvasPixelFormat::RGBA16F) {
                float px[4];
                HalfFloat::LoadRGBA16F(raw + ((size_t)ly * TILE_SIZE + lx) * 8, px);
                out[di + 0] = px[0]; out[di + 1] = px[1];
                out[di + 2] = px[2]; out[di + 3] = px[3];
            } else {
                const float* fp = reinterpret_cast<const float*>(raw);
                size_t si = ((size_t)ly * TILE_SIZE + lx) * 4;
                out[di + 0] = fp[si + 0]; out[di + 1] = fp[si + 1];
                out[di + 2] = fp[si + 2]; out[di + 3] = fp[si + 3];
            }
        }
    }
}

const FilterJobInput::SnapTile* FindSnap(const std::vector<FilterJobInput::SnapTile>& tiles,
                                         int tx, int ty) {
    for (const auto& t : tiles)
        if (t.tx == tx && t.ty == ty) return &t;
    return nullptr;
}

} // namespace

FilterJobResult RunFilterJob(FilterJobInput input) {
    FilterJobResult out;
    out.layerIdx = input.layerIdx;
    out.generation = input.generation;
    if (input.seeds.empty() || input.filters.empty()) {
        out.ok = true;
        return out;
    }

    const int halo = std::max(0, input.halo);
    out.tiles.reserve(input.seeds.size());

    for (auto [stx, sty] : input.seeds) {
        const int x0 = stx * TILE_SIZE;
        const int y0 = sty * TILE_SIZE;
        const int tileW = std::min(TILE_SIZE, input.docW - x0);
        const int tileH = std::min(TILE_SIZE, input.docH - y0);
        if (tileW <= 0 || tileH <= 0) continue;

        if (halo == 0) {
            const FilterJobInput::SnapTile* snap = FindSnap(input.contentTiles, stx, sty);
            if (!snap) continue;
            TileResult tr;
            tr.tx = stx; tr.ty = sty;
            tr.x0 = x0; tr.y0 = y0;
            tr.w = tileW; tr.h = tileH;
            DecodeTileToFloat(*snap, input.format, tr.rgba);
            // Decode wrote validW×validH
            tr.w = snap->validW; tr.h = snap->validH;
            layer_fx::ApplyPixelFilters(tr.rgba, tr.w, tr.h, input.filters, x0, y0);
            out.tiles.push_back(std::move(tr));
            continue;
        }

        const int rx0 = std::max(0, x0 - halo);
        const int ry0 = std::max(0, y0 - halo);
        const int rx1 = std::min(input.docW, x0 + tileW + halo);
        const int ry1 = std::min(input.docH, y0 + tileH + halo);
        const int rw = rx1 - rx0;
        const int rh = ry1 - ry0;
        if (rw <= 0 || rh <= 0) continue;

        std::vector<float> region((size_t)rw * rh * 4, 0.f);
        // Gather from snapped tiles
        for (int y = 0; y < rh; ++y) {
            const int docY = ry0 + y;
            const int tty = docY / TILE_SIZE;
            const int sly = docY - tty * TILE_SIZE;
            for (int x = 0; x < rw; ) {
                const int docX = rx0 + x;
                const int ttx = docX / TILE_SIZE;
                const int slx0 = docX - ttx * TILE_SIZE;
                const int run = std::min(rw - x, TILE_SIZE - slx0);
                const FilterJobInput::SnapTile* snap = FindSnap(input.contentTiles, ttx, tty);
                if (snap && !snap->bytes.empty()) {
                    const int bpp = BytesPerPixel(input.format);
                    for (int k = 0; k < run; ++k) {
                        const int slx = slx0 + k;
                        if (slx >= snap->validW || sly >= snap->validH) continue;
                        size_t di = ((size_t)y * rw + x + k) * 4;
                        const uint8_t* p = snap->bytes.data() +
                            ((size_t)sly * TILE_SIZE + slx) * bpp;
                        if (input.format == CanvasPixelFormat::RGBA8) {
                            region[di + 0] = p[0] / 255.f; region[di + 1] = p[1] / 255.f;
                            region[di + 2] = p[2] / 255.f; region[di + 3] = p[3] / 255.f;
                        } else if (input.format == CanvasPixelFormat::RGBA16F) {
                            float px[4]; HalfFloat::LoadRGBA16F(p, px);
                            region[di + 0] = px[0]; region[di + 1] = px[1];
                            region[di + 2] = px[2]; region[di + 3] = px[3];
                        } else {
                            const float* fp = reinterpret_cast<const float*>(p);
                            region[di + 0] = fp[0]; region[di + 1] = fp[1];
                            region[di + 2] = fp[2]; region[di + 3] = fp[3];
                        }
                    }
                }
                x += run;
            }
        }

        layer_fx::ApplyPixelFilters(region, rw, rh, input.filters, rx0, ry0);

        const int ox = x0 - rx0;
        const int oy = y0 - ry0;
        TileResult tr;
        tr.tx = stx; tr.ty = sty;
        tr.x0 = x0; tr.y0 = y0;
        tr.w = tileW; tr.h = tileH;
        tr.rgba.resize((size_t)tileW * tileH * 4);
        for (int y = 0; y < tileH; ++y) {
            std::memcpy(tr.rgba.data() + (size_t)y * tileW * 4,
                        region.data() + ((size_t)(oy + y) * rw + ox) * 4,
                        (size_t)tileW * 4 * sizeof(float));
        }
        out.tiles.push_back(std::move(tr));
    }

    out.ok = true;
    return out;
}

void AsyncFilterQueue::Submit(FilterJobInput input) {
    const int layerIdx = input.layerIdx;
    const uint64_t gen = m_Gen.fetch_add(1);
    input.generation = gen;

    FilterJobInput moved = std::move(input);
    auto fut = ThreadPool::Get().Enqueue([job = std::move(moved)]() mutable {
        return RunFilterJob(std::move(job));
    });

    std::lock_guard<std::mutex> lock(m_Mu);
    // Drop older entry for same layer (future may still complete; generation discards)
    m_InFlight.erase(
        std::remove_if(m_InFlight.begin(), m_InFlight.end(),
                       [&](const Entry& e) { return e.layerIdx == layerIdx; }),
        m_InFlight.end());
    m_InFlight.push_back(Entry{ layerIdx, gen, std::move(fut) });
}

std::vector<FilterJobResult> AsyncFilterQueue::Poll() {
    std::vector<FilterJobResult> ready;
    std::lock_guard<std::mutex> lock(m_Mu);
    for (auto it = m_InFlight.begin(); it != m_InFlight.end(); ) {
        if (it->fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            FilterJobResult r = it->fut.get();
            // Only accept if generation still latest for this layer
            bool latest = true;
            for (const auto& e : m_InFlight) {
                if (e.layerIdx == r.layerIdx && e.generation > r.generation)
                    latest = false;
            }
            if (latest && r.ok)
                ready.push_back(std::move(r));
            it = m_InFlight.erase(it);
        } else {
            ++it;
        }
    }
    return ready;
}

void AsyncFilterQueue::CancelAll() {
    std::lock_guard<std::mutex> lock(m_Mu);
    m_Gen.fetch_add(1);
    m_InFlight.clear(); // abandon futures (they still run but results dropped)
}

bool AsyncFilterQueue::IsBusy(int layerIdx) const {
    std::lock_guard<std::mutex> lock(m_Mu);
    for (const auto& e : m_InFlight)
        if (e.layerIdx == layerIdx) return true;
    return false;
}

} // namespace async_fx

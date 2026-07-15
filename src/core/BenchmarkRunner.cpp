#include "BenchmarkRunner.h"
#include "Logger.h"
#include "MemoryStats.h"
#include "../Canvas.h"
#include "../layer/LayerTypes.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

BenchmarkRunner& BenchmarkRunner::Get() {
    static BenchmarkRunner inst;
    return inst;
}

float BenchmarkRunner::RandF(float a, float b) {
    std::uniform_real_distribution<float> d(a, b);
    return d(m_Rng);
}

int BenchmarkRunner::RandI(int a, int bInclusive) {
    std::uniform_int_distribution<int> d(a, bInclusive);
    return d(m_Rng);
}

void BenchmarkRunner::ScheduleWait(float minMs, float maxMs) {
    m_WaitLeft = RandF(minMs, maxMs) * 0.001f;
}

bool BenchmarkRunner::Waiting(float dtSec) {
    if (m_WaitLeft <= 0.f) return false;
    m_WaitLeft -= dtSec;
    if (m_WaitLeft < 0.f) m_WaitLeft = 0.f;
    return m_WaitLeft > 0.f;
}

void BenchmarkRunner::SnapshotMem(size_t& ws, size_t& priv) const {
    auto info = MemoryStats::QueryProcess();
    ws = info.workingSetBytes;
    priv = info.privateBytes;
}

size_t BenchmarkRunner::ActiveTiles(const Canvas& canvas) const {
    return canvas.GetActiveLayerTileCount();
}

void BenchmarkRunner::RecordFrame(float workMs) {
    ++m_TotalFrames;
    ++m_CurStats.frames;
    m_TotalWorkMs += workMs;
    m_CurStats.workMsSum += workMs;
    m_CurStats.workMsMin = std::min(m_CurStats.workMsMin, (double)workMs);
    m_CurStats.workMsMax = std::max(m_CurStats.workMsMax, (double)workMs);
    m_GlobalMinWork = std::min(m_GlobalMinWork, (double)workMs);
    m_GlobalMaxWork = std::max(m_GlobalMaxWork, (double)workMs);
    if (workMs > 33.f) ++m_FramesOver33ms;
    if (workMs > 50.f) ++m_FramesOver50ms;
    if (workMs > 100.f) ++m_FramesOver100ms;
    m_CurStats.workSamples.push_back(workMs);

    size_t ws = 0, priv = 0;
    SnapshotMem(ws, priv);
    m_MemPeakWS = std::max(m_MemPeakWS, ws);
}

void BenchmarkRunner::UpdateStatus() {
    char buf[256];
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_StartTime).count();
    std::snprintf(buf, sizeof(buf),
        "BENCHMARK | %s | %.1fs | frames %d | stroke %zu/%zu",
        m_PhaseName.c_str(), elapsed, m_TotalFrames, m_QueueIdx, m_Queue.size());
    m_StatusLine = buf;
}

void BenchmarkRunner::BeginPhase(Canvas& canvas, Phase phase) {
    if (m_CurStats.frames > 0 || !m_CurStats.name.empty()) {
        EndPhase(canvas);
    }
    m_Phase = phase;
    m_SubStep = 0;
    m_PhaseElapsed = 0.f;
    m_WaitLeft = 0.f;
    m_PhaseStart = std::chrono::steady_clock::now();
    m_CurStats = PhaseStats{};
    SnapshotMem(m_CurStats.memWorkingSetStart, m_CurStats.memPrivateStart);
    m_CurStats.tilesStart = ActiveTiles(canvas);

    switch (phase) {
    case Phase::Setup:               m_PhaseName = "Setup 8K"; break;
    case Phase::PaintCover:          m_PhaseName = "Paint cover"; break;
    case Phase::EraseCover:          m_PhaseName = "Erase cover"; break;
    case Phase::SelectMoveTransform: m_PhaseName = "Select/Move/Transform"; break;
    case Phase::ColorAndOperator:    m_PhaseName = "Color + operator"; break;
    case Phase::SecondLayerBlur:     m_PhaseName = "Layer2 + Blur"; break;
    case Phase::PaintOnLayer2:       m_PhaseName = "Paint layer2"; break;
    case Phase::UndoRedoStress:      m_PhaseName = "Undo/Redo stress"; break;
    case Phase::PaintAndMove:        m_PhaseName = "Paint + Move"; break;
    case Phase::DeleteLayerUndo:     m_PhaseName = "Delete layer + Undo"; break;
    case Phase::FinalPaint:          m_PhaseName = "Final paint"; break;
    case Phase::Report:              m_PhaseName = "Report"; break;
    case Phase::Done:                m_PhaseName = "Done"; break;
    }
    m_CurStats.name = m_PhaseName;
    Logger::Get().InfoTag("bench", "=== Phase: " + m_PhaseName + " ===");
    MemoryStats::LogSnapshot("bench_" + m_PhaseName + "_start");
    UpdateStatus();
}

void BenchmarkRunner::EndPhase(Canvas& canvas) {
    auto now = std::chrono::steady_clock::now();
    m_CurStats.durationMs = std::chrono::duration<double, std::milli>(now - m_PhaseStart).count();
    SnapshotMem(m_CurStats.memWorkingSetEnd, m_CurStats.memPrivateEnd);
    m_CurStats.tilesEnd = ActiveTiles(canvas);

    if (!m_CurStats.workSamples.empty()) {
        auto s = m_CurStats.workSamples;
        std::sort(s.begin(), s.end());
        size_t idx = (size_t)std::clamp((int)(s.size() * 0.95), 0, (int)s.size() - 1);
        m_CurStats.workMsP95 = s[idx];
    }

    const double avg = m_CurStats.frames > 0
        ? m_CurStats.workMsSum / m_CurStats.frames : 0.0;
    char line[512];
    std::snprintf(line, sizeof(line),
        "Phase '%s' done: %.0fms frames=%d work_avg=%.1fms min=%.1f max=%.1f p95=%.1f "
        "WS %s→%s tiles %zu→%zu",
        m_CurStats.name.c_str(),
        m_CurStats.durationMs,
        m_CurStats.frames,
        avg,
        m_CurStats.workMsMin >= 1e8 ? 0.0 : m_CurStats.workMsMin,
        m_CurStats.workMsMax,
        m_CurStats.workMsP95,
        MemoryStats::FormatBytes(m_CurStats.memWorkingSetStart).c_str(),
        MemoryStats::FormatBytes(m_CurStats.memWorkingSetEnd).c_str(),
        m_CurStats.tilesStart,
        m_CurStats.tilesEnd);
    Logger::Get().InfoTag("bench", line);
    MemoryStats::LogSnapshot("bench_" + m_CurStats.name + "_end");
    m_PhaseStats.push_back(std::move(m_CurStats));
    m_CurStats = PhaseStats{};
}

void BenchmarkRunner::SetupCanvas8K(Canvas& canvas, ID3D11Device* device) {
    Logger::Get().InfoTag("bench", "Resizing canvas to 8192x8192...");
    canvas.ResizeCanvas(device, kCanvasSize, kCanvasSize);
    // Ensure at least one paint layer
    if (canvas.GetLayers().empty()) {
        canvas.CreateNewLayer(device, "Background");
    }
    canvas.SetActiveLayerIndex(0);
    canvas.ResetView();
    // Fit roughly into a typical viewport (~900px tall content area)
    float fitZoom = 900.f / (float)kCanvasSize;
    if (fitZoom < 0.02f) fitZoom = 0.02f;
    canvas.SetZoom(fitZoom);
    canvas.SetPan(DirectX::XMFLOAT2(
        -kCanvasSize * 0.5f * fitZoom,
        -kCanvasSize * 0.5f * fitZoom));
    canvas.ClearSelection();
    canvas.ClearUndoHistory();
    canvas.MarkCompositeDirty();
    Logger::Get().InfoTag("bench",
        "Canvas ready " + std::to_string(canvas.GetWidth()) + "x" +
        std::to_string(canvas.GetHeight()));
}

// ---------- Path builders (human-ish, not robotic grid dots) ----------

BenchmarkRunner::PathStroke BenchmarkRunner::MakeSerpentine(
    int w, int h, float y0, float yStep, float amp,
    float radius, bool erase, const float color[4])
{
    PathStroke s;
    s.erase = erase;
    s.radius = radius;
    s.hardness = RandF(0.35f, 0.75f);
    s.opacity = RandF(0.75f, 1.0f);
    // Fast wide sweeps — on 8K, pure human 1.5k px/s would make one serpentine >1 min.
    s.speedPxS = RandF(3200.f, 5200.f);
    for (int i = 0; i < 4; ++i) s.color[i] = color[i];

    const float margin = radius * 1.1f;
    const float xL = margin;
    const float xR = (float)w - margin;
    bool leftToRight = true;
    float y = std::clamp(y0, margin, (float)h - margin);

    // Wide brush already spans multiple tiles; a handful of wavy rows still stress tiling.
    int passes = std::max(4, (int)((h - 2.f * margin) / std::max(80.f, yStep)));
    passes = std::min(passes, 7);
    for (int p = 0; p < passes; ++p) {
        float xStart = leftToRight ? xL : xR;
        float xEnd   = leftToRight ? xR : xL;
        int samples = RandI(36, 64);
        // Fixed wave freq per pass (not re-rolled every sample — avoids jittery robot noise)
        const float freq = RandF(1.6f, 2.6f);
        for (int i = 0; i <= samples; ++i) {
            float t = (float)i / (float)samples;
            // ease-in-out horizontal progress (human acceleration)
            float te = t * t * (3.f - 2.f * t);
            float x = xStart + (xEnd - xStart) * te;
            float wave = std::sin(t * 6.28318f * freq + y * 0.001f) * amp;
            wave += std::sin(t * 11.0f + (float)p) * amp * 0.22f;
            float yy = std::clamp(y + wave, margin, (float)h - margin);
            // micro jitter (hand tremor), small
            x += RandF(-3.5f, 3.5f);
            yy += RandF(-3.5f, 3.5f);
            s.pts.push_back({x, yy});
        }
        y += yStep + RandF(-yStep * 0.12f, yStep * 0.12f);
        if (y > (float)h - margin) break;
        leftToRight = !leftToRight;
    }
    return s;
}

BenchmarkRunner::PathStroke BenchmarkRunner::MakeDiagonal(
    int w, int h, float t0, float radius, bool erase, const float color[4])
{
    PathStroke s;
    s.erase = erase;
    s.radius = radius;
    s.hardness = RandF(0.4f, 0.8f);
    s.opacity = RandF(0.8f, 1.f);
    s.speedPxS = RandF(2800.f, 4800.f);
    for (int i = 0; i < 4; ++i) s.color[i] = color[i];

    const float margin = radius * 1.05f;
    int samples = RandI(40, 72);
    // Offset parallel diagonals via t0
    float ox = (t0 - 0.5f) * (float)w * 0.4f;
    for (int i = 0; i <= samples; ++i) {
        float t = (float)i / (float)samples;
        float te = t * t * (3.f - 2.f * t);
        float x = margin + ((float)w - 2.f * margin) * te + ox;
        float y = margin + ((float)h - 2.f * margin) * te;
        // bow the diagonal slightly
        float bow = std::sin(te * 3.14159f) * radius * 0.8f;
        x = std::clamp(x + bow * 0.35f + RandF(-3.f, 3.f), margin, (float)w - margin);
        y = std::clamp(y - bow * 0.2f + RandF(-3.f, 3.f), margin, (float)h - margin);
        s.pts.push_back({x, y});
    }
    return s;
}

BenchmarkRunner::PathStroke BenchmarkRunner::MakeFigureEight(
    int w, int h, float cx, float cy, float rx, float ry,
    float radius, bool erase, const float color[4])
{
    PathStroke s;
    s.erase = erase;
    s.radius = radius;
    s.hardness = RandF(0.3f, 0.7f);
    s.opacity = RandF(0.7f, 0.98f);
    s.speedPxS = RandF(2400.f, 4000.f);
    for (int i = 0; i < 4; ++i) s.color[i] = color[i];

    const float margin = radius * 1.05f;
    int samples = RandI(50, 90);
    for (int i = 0; i <= samples; ++i) {
        float t = (float)i / (float)samples * 6.2831853f * 2.f; // two loops
        // lemniscate-ish
        float x = cx + rx * std::sin(t);
        float y = cy + ry * std::sin(t) * std::cos(t);
        x += RandF(-4.f, 4.f);
        y += RandF(-4.f, 4.f);
        x = std::clamp(x, margin, (float)w - margin);
        y = std::clamp(y, margin, (float)h - margin);
        s.pts.push_back({x, y});
    }
    return s;
}

BenchmarkRunner::PathStroke BenchmarkRunner::MakeWavyFreehand(
    int w, int h, float x0, float y0, float x1, float y1,
    int samples, float radius, bool erase, const float color[4])
{
    PathStroke s;
    s.erase = erase;
    s.radius = radius;
    s.hardness = RandF(0.4f, 0.85f);
    s.opacity = RandF(0.75f, 1.f);
    s.speedPxS = RandF(2200.f, 4200.f);
    for (int i = 0; i < 4; ++i) s.color[i] = color[i];

    const float margin = radius * 1.05f;
    float phase = RandF(0.f, 6.28f);
    float amp = RandF(radius * 0.6f, radius * 1.8f);
    const float freq = RandF(1.2f, 2.4f);
    for (int i = 0; i <= samples; ++i) {
        float t = (float)i / (float)samples;
        float te = t * t * (3.f - 2.f * t);
        float x = x0 + (x1 - x0) * te;
        float y = y0 + (y1 - y0) * te;
        float nx = -(y1 - y0), ny = (x1 - x0);
        float nl = std::sqrt(nx * nx + ny * ny);
        if (nl > 1.f) { nx /= nl; ny /= nl; }
        float wave = std::sin(t * 6.28f * freq + phase) * amp;
        x += nx * wave + RandF(-3.f, 3.f);
        y += ny * wave + RandF(-3.f, 3.f);
        x = std::clamp(x, margin, (float)w - margin);
        y = std::clamp(y, margin, (float)h - margin);
        s.pts.push_back({x, y});
    }
    return s;
}

void BenchmarkRunner::BuildPaintCoverStrokes(int w, int h) {
    m_Queue.clear();
    m_QueueIdx = 0;
    m_PathIdx = 0;
    m_PathT = 0.f;
    m_StrokeLive = false;

    // Wide brushes — stress multi-tile stamps and COW backup.
    const float rBig = RandF(280.f, 450.f);
    const float rMed = RandF(200.f, 320.f);
    float c1[4] = {0.92f, 0.18f, 0.12f, 1.f};
    float c2[4] = {0.15f, 0.45f, 0.95f, 1.f};
    float c3[4] = {0.95f, 0.78f, 0.12f, 1.f};
    float c4[4] = {0.25f, 0.85f, 0.35f, 1.f};
    float c5[4] = {0.75f, 0.25f, 0.85f, 1.f};

    // Wide serpentine + diagonals — path length tuned for ~8–10s at 3–5k px/s
    m_Queue.push_back(MakeSerpentine(w, h, rBig * 1.2f, rBig * 2.1f, rBig * 0.55f, rBig, false, c1));
    m_Queue.push_back(MakeSerpentine(w, h, h * 0.5f, rMed * 2.2f, rMed * 0.65f, rMed, false, c2));

    // Diagonal + figure-eight for cross-tile paths
    m_Queue.push_back(MakeDiagonal(w, h, 0.35f, rBig * 0.95f, false, c3));
    m_Queue.push_back(MakeFigureEight(w, h, w * 0.5f, h * 0.5f, w * 0.28f, h * 0.22f, rBig * 0.9f, false, c5));

    // Freehand long sweeps (held stroke, wide radius)
    for (int i = 0; i < 2; ++i) {
        float x0 = RandF(rBig, w - rBig);
        float y0 = RandF(rBig, h - rBig);
        float x1 = RandF(rBig, w - rBig);
        float y1 = RandF(rBig, h - rBig);
        float col[4] = {RandF(0.1f, 1.f), RandF(0.1f, 1.f), RandF(0.1f, 1.f), 1.f};
        m_Queue.push_back(MakeWavyFreehand(w, h, x0, y0, x1, y1, RandI(40, 70),
                                           RandF(240.f, 400.f), false, col));
    }
    (void)c4;
}

void BenchmarkRunner::BuildEraseStrokes(int w, int h) {
    m_Queue.clear();
    m_QueueIdx = 0;
    m_PathIdx = 0;
    m_PathT = 0.f;
    m_StrokeLive = false;

    const float r = RandF(240.f, 380.f);
    float black[4] = {0, 0, 0, 1};
    m_Queue.push_back(MakeSerpentine(w, h, r * 1.8f, r * 2.4f, r * 0.5f, r, true, black));
    m_Queue.push_back(MakeDiagonal(w, h, 0.5f, r * 1.1f, true, black));
}

void BenchmarkRunner::BuildFreehandStrokes(int w, int h, int count, bool erase, float radiusMul) {
    m_Queue.clear();
    m_QueueIdx = 0;
    m_PathIdx = 0;
    m_PathT = 0.f;
    m_StrokeLive = false;

    for (int i = 0; i < count; ++i) {
        float r = RandF(160.f, 320.f) * radiusMul;
        float x0 = RandF(r, w - r);
        float y0 = RandF(r, h - r);
        float x1 = RandF(r, w - r);
        float y1 = RandF(r, h - r);
        float col[4] = {RandF(0.15f, 1.f), RandF(0.15f, 1.f), RandF(0.15f, 1.f), 1.f};
        m_Queue.push_back(MakeWavyFreehand(w, h, x0, y0, x1, y1, RandI(55, 100), r, erase, col));
    }
}

void BenchmarkRunner::EnsureStrokeStarted(Canvas& canvas) {
    if (m_StrokeLive || m_QueueIdx >= m_Queue.size()) return;
    const PathStroke& s = m_Queue[m_QueueIdx];
    if (s.pts.empty()) {
        ++m_QueueIdx;
        return;
    }
    m_StrokeBrush = BrushSettings{};
    m_StrokeBrush.erase = s.erase;
    m_StrokeBrush.radius = s.radius;
    m_StrokeBrush.hardness = s.hardness;
    m_StrokeBrush.opacity = s.opacity;
    m_StrokeBrush.spacing = 0.12f;
    m_StrokeBrush.stabilization = 2;
    for (int i = 0; i < 4; ++i) m_StrokeBrush.color[i] = s.color[i];
    if (s.erase) {
        m_StrokeBrush.color[0] = m_StrokeBrush.color[1] = m_StrokeBrush.color[2] = 0.f;
        m_StrokeBrush.color[3] = 1.f;
    }

    const auto& p0 = s.pts.front();
    canvas.PaintOnActiveLayer(p0.x, p0.y, StrokePhase::Begin, m_StrokeBrush);
    m_StrokeLive = true;
    m_PathIdx = 0;
    m_PathT = 0.f;
}

void BenchmarkRunner::FinishStrokeIfActive(Canvas& canvas) {
    if (!m_StrokeLive) return;
    canvas.PaintOnActiveLayer(0.f, 0.f, StrokePhase::End, m_StrokeBrush);
    m_StrokeLive = false;
    canvas.MarkCompositeDirty();
}

bool BenchmarkRunner::AdvanceStroke(Canvas& canvas, float dtSec) {
    if (m_QueueIdx >= m_Queue.size()) {
        FinishStrokeIfActive(canvas);
        return false; // queue empty
    }

    // Human pause between strokes
    if (!m_StrokeLive && Waiting(dtSec))
        return true;

    if (!m_StrokeLive) {
        // short random delay before pen-down
        if (m_WaitLeft <= 0.f && m_PathIdx == 0 && m_QueueIdx < m_Queue.size()) {
            // first entry to this stroke: maybe wait
            if (m_SubStep == 0) {
                ScheduleWait(40.f, 180.f);
                m_SubStep = 1;
                return true;
            }
        }
        EnsureStrokeStarted(canvas);
        m_SubStep = 0;
        if (!m_StrokeLive) {
            // empty stroke skipped
            ScheduleWait(30.f, 120.f);
            return m_QueueIdx < m_Queue.size();
        }
    }

    PathStroke& s = m_Queue[m_QueueIdx];
    if (s.pts.size() < 2) {
        FinishStrokeIfActive(canvas);
        ++m_QueueIdx;
        ScheduleWait(50.f, 200.f);
        return m_QueueIdx < m_Queue.size();
    }

    // Advance along path with human speed + jitter
    float speed = s.speedPxS * RandF(0.85f, 1.2f);
    // occasional micro-hesitation
    if (RandF(0.f, 1.f) < 0.04f)
        speed *= RandF(0.25f, 0.55f);

    float budget = speed * std::max(dtSec, 0.001f);
    // Cap work per frame so one frame doesn't stamp extreme dab counts on 8K
    // (still multi-tile segments — wide brush + long segment stresses tiling)
    budget = std::min(budget, s.radius * 10.f);

    while (budget > 0.f && m_PathIdx + 1 < s.pts.size()) {
        StrokePoint a = s.pts[m_PathIdx];
        StrokePoint b = s.pts[m_PathIdx + 1];
        float dx = b.x - a.x, dy = b.y - a.y;
        float segLen = std::sqrt(dx * dx + dy * dy);
        if (segLen < 1e-3f) {
            ++m_PathIdx;
            continue;
        }
        float remain = segLen - m_PathT;
        if (budget >= remain) {
            budget -= remain;
            m_PathT = 0.f;
            ++m_PathIdx;
            // paint at segment end
            canvas.PaintOnActiveLayer(b.x, b.y, StrokePhase::Update, m_StrokeBrush);
        } else {
            m_PathT += budget;
            float t = m_PathT / segLen;
            float x = a.x + dx * t;
            float y = a.y + dy * t;
            canvas.PaintOnActiveLayer(x, y, StrokePhase::Update, m_StrokeBrush);
            budget = 0.f;
        }
    }

    if (m_PathIdx + 1 >= s.pts.size()) {
        // ensure last point
        const auto& last = s.pts.back();
        canvas.PaintOnActiveLayer(last.x, last.y, StrokePhase::Update, m_StrokeBrush);
        FinishStrokeIfActive(canvas);
        ++m_QueueIdx;
        m_PathIdx = 0;
        m_PathT = 0.f;
        ScheduleWait(60.f, 280.f); // human pen lift / reposition
    }
    return m_QueueIdx < m_Queue.size() || m_StrokeLive;
}

void BenchmarkRunner::AddBlurFilter(Canvas& canvas, float radius, const char* undoName) {
    int ai = canvas.GetActiveLayerIndex();
    if (ai < 0 || ai >= (int)canvas.GetLayers().size()) return;
    auto& layer = canvas.GetLayers()[ai];
    auto before = Canvas::CaptureLayerProps(layer);
    LayerFilter nf;
    nf.type = FilterType::Blur;
    nf.enabled = true;
    nf.p[0] = radius;
    layer.filters.push_back(std::move(nf));
    layer.filtersDirty = true;
    layer.presentationDirty = true;
    canvas.CommitLayerPropsEdit(ai, before, undoName);
    // Enable live FX preview; cost is paid on subsequent composes (not a full synchronous bake).
    canvas.SetEffectsPreviewEnabled(true);
    canvas.MarkCompositeDirty();
}

void BenchmarkRunner::AddOutlineStyle(Canvas& canvas) {
    int ai = canvas.GetActiveLayerIndex();
    if (ai < 0) return;
    canvas.AddLayerStyle(ai, StyleType::Outline);
    canvas.SetEffectsPreviewEnabled(true);
    canvas.MarkCompositeDirty();
}

// ---------- Discrete phases ----------

bool BenchmarkRunner::TickSelectMoveTransform(Canvas& canvas, ID3D11Device* device, float dtSec) {
    if (Waiting(dtSec)) return true;
    const int w = canvas.GetWidth();
    const int h = canvas.GetHeight();

    switch (m_SubStep) {
    case 0: {
        // Mid-size rect — stresses float/transform without multi-second freezes on 8K.
        const int side = std::clamp(w / 8, 640, 1024);
        int cx = w / 2 + RandI(-w / 8, w / 8);
        int cy = h / 2 + RandI(-h / 8, h / 8);
        int x1 = std::clamp(cx - side / 2, 0, w - 2);
        int y1 = std::clamp(cy - side / 2, 0, h - 2);
        int x2 = std::clamp(x1 + side, 1, w - 1);
        int y2 = std::clamp(y1 + side, 1, h - 1);
        canvas.ApplyRectSelection(x1, y1, x2, y2, false, false);
        canvas.UpdateSelectionMaskTexture(device);
        Logger::Get().InfoTag("bench",
            "Rect selection applied " + std::to_string(x2 - x1) + "x" + std::to_string(y2 - y1));
        ScheduleWait(80.f, 220.f);
        m_SubStep = 1;
        return true;
    }
    case 1: {
        canvas.StartMovePixels(device);
        m_MoveDx = 0;
        m_MoveDy = 0;
        m_TransformT = 0.f;
        ScheduleWait(40.f, 120.f);
        m_SubStep = 2;
        return true;
    }
    case 2: {
        // Human-like drag of floating selection over ~0.8–1.4s
        if (m_TransformT <= 0.f && m_MoveDx == 0 && m_MoveDy == 0) {
            m_MoveDx = (int)RandF(150.f, 420.f) * (RandI(0, 1) ? 1 : -1);
            m_MoveDy = (int)RandF(120.f, 360.f) * (RandI(0, 1) ? 1 : -1);
            m_MoveDur = RandF(0.85f, 1.35f);
        }
        m_TransformT += dtSec;
        float t = std::clamp(m_TransformT / std::max(0.2f, m_MoveDur), 0.f, 1.f);
        float te = t * t * (3.f - 2.f * t);
        int dx = (int)std::lround(m_MoveDx * te);
        int dy = (int)std::lround(m_MoveDy * te);
        // micro wobble
        dx += (int)RandF(-2.f, 2.f);
        dy += (int)RandF(-2.f, 2.f);
        canvas.UpdateMovePixels(device, dx, dy);
        if (t >= 1.f) {
            ScheduleWait(50.f, 160.f);
            m_SubStep = 3;
            m_TransformT = 0.f;
            m_TfDur = RandF(0.7f, 1.2f);
        }
        return true;
    }
    case 3: {
        // Free transform: scale + slight rotate while floating
        m_TransformT += dtSec;
        float t = std::clamp(m_TransformT / std::max(0.2f, m_TfDur), 0.f, 1.f);
        float te = t * t * (3.f - 2.f * t);
        float sx = 1.f + 0.25f * te + RandF(-0.01f, 0.01f);
        float sy = 1.f + 0.18f * te + RandF(-0.01f, 0.01f);
        float rot = 0.35f * te; // ~20 degrees
        canvas.SetFloatingScaleX(sx);
        canvas.SetFloatingScaleY(sy);
        canvas.SetFloatingRotation(rot);
        canvas.MarkCompositeDirty();
        if (t >= 1.f) {
            ScheduleWait(60.f, 180.f);
            m_SubStep = 4;
        }
        return true;
    }
    case 4: {
        canvas.CommitMovePixels(device);
        Logger::Get().InfoTag("bench", "Transform committed");
        ScheduleWait(80.f, 200.f);
        m_SubStep = 5;
        return true;
    }
    default:
        return false;
    }
}

bool BenchmarkRunner::TickColorAndOperator(Canvas& canvas, ID3D11Device* device, float dtSec) {
    if (Waiting(dtSec)) return true;
    switch (m_SubStep) {
    case 0: {
        // "Color change" — paint a short stroke with new color (simulates palette switch)
        int w = canvas.GetWidth(), h = canvas.GetHeight();
        float col[4] = {0.1f, 0.95f, 0.85f, 1.f};
        m_Queue.clear();
        m_Queue.push_back(MakeWavyFreehand(w, h, w * 0.2f, h * 0.3f, w * 0.8f, h * 0.7f,
                                           80, RandF(200.f, 320.f), false, col));
        m_QueueIdx = 0;
        m_PathIdx = 0;
        m_StrokeLive = false;
        m_SubStep = 1;
        ScheduleWait(40.f, 120.f);
        return true;
    }
    case 1: {
        if (AdvanceStroke(canvas, dtSec))
            return true;
        ScheduleWait(50.f, 150.f);
        m_SubStep = 2;
        return true;
    }
    case 2: {
        // Operator: outline style + destructive-ish path via filter on active
        AddOutlineStyle(canvas);
        Logger::Get().InfoTag("bench", "Outline style operator applied");
        ScheduleWait(100.f, 250.f);
        m_SubStep = 3;
        return true;
    }
    case 3: {
        // Also a mild blur filter as "operator"
        AddBlurFilter(canvas, RandF(4.f, 10.f), "Bench Add Blur");
        Logger::Get().InfoTag("bench", "Blur filter operator applied");
        ScheduleWait(120.f, 280.f);
        m_SubStep = 4;
        return true;
    }
    default:
        return false;
    }
}

bool BenchmarkRunner::TickSecondLayerBlur(Canvas& canvas, ID3D11Device* device, float dtSec) {
    if (Waiting(dtSec)) return true;
    switch (m_SubStep) {
    case 0: {
        canvas.CreateNewLayer(device, "Bench Layer 2");
        // New layer is typically on top and active
        int n = (int)canvas.GetLayers().size();
        if (n > 0) canvas.SetActiveLayerIndex(n - 1);
        Logger::Get().InfoTag("bench", "Created second layer, layers=" + std::to_string(n));
        ScheduleWait(80.f, 200.f);
        m_SubStep = 1;
        return true;
    }
    case 1: {
        AddBlurFilter(canvas, RandF(8.f, 18.f), "Bench Layer2 Blur");
        Logger::Get().InfoTag("bench", "Blur operator on layer 2");
        ScheduleWait(100.f, 220.f);
        m_SubStep = 2;
        return true;
    }
    default:
        return false;
    }
}

bool BenchmarkRunner::TickUndoRedo(Canvas& canvas, float dtSec) {
    if (Waiting(dtSec)) return true;

    if (m_SubStep == 0) {
        SnapshotMem(m_MemBeforeUndoRedoWS, m_CurStats.memPrivateStart);
        // Count how many undos we can do; aim for a burst
        m_UndoLeft = 0;
        // We'll try up to N undos then redos
        m_UndoLeft = RandI(8, 16);
        m_RedoLeft = 0;
        m_UndoPass = true;
        m_SubStep = 1;
        Logger::Get().InfoTag("bench", "Undo/Redo stress starting, planned undos≈" +
            std::to_string(m_UndoLeft));
        return true;
    }

    if (m_UndoPass) {
        if (m_UndoLeft > 0 && canvas.CanUndo()) {
            canvas.Undo();
            --m_UndoLeft;
            ++m_RedoLeft;
            ScheduleWait(25.f, 90.f); // not instant spam — slight human delay
            return true;
        }
        m_UndoPass = false;
        ScheduleWait(80.f, 180.f);
        return true;
    }

    // Redo pass
    if (m_RedoLeft > 0 && canvas.CanRedo()) {
        canvas.Redo();
        --m_RedoLeft;
        ScheduleWait(25.f, 90.f);
        return true;
    }

    // Second wave: undo half again (memory churn)
    if (m_SubStep == 1) {
        m_SubStep = 2;
        m_UndoPass = true;
        m_UndoLeft = RandI(4, 10);
        m_RedoLeft = 0;
        ScheduleWait(60.f, 150.f);
        return true;
    }

    SnapshotMem(m_MemAfterUndoRedoWS, m_CurStats.memPrivateEnd);
    Logger::Get().InfoTag("bench",
        std::string("Undo/Redo done. WS before=") +
        MemoryStats::FormatBytes(m_MemBeforeUndoRedoWS) +
        " after=" + MemoryStats::FormatBytes(m_MemAfterUndoRedoWS));
    return false;
}

bool BenchmarkRunner::TickPaintAndMove(Canvas& canvas, ID3D11Device* device, float dtSec) {
    if (Waiting(dtSec)) return true;
    int w = canvas.GetWidth(), h = canvas.GetHeight();

    switch (m_SubStep) {
    case 0: {
        BuildFreehandStrokes(w, h, 3, false, 1.0f);
        m_SubStep = 1;
        return true;
    }
    case 1: {
        if (AdvanceStroke(canvas, dtSec))
            return true;
        ScheduleWait(60.f, 160.f);
        m_SubStep = 2;
        return true;
    }
    case 2: {
        // Select a mid region and move (keep float buffer manageable on 8K)
        const int side = std::clamp(w / 10, 512, 896);
        int x1 = w / 2 - side / 2, y1 = h / 2 - side / 2;
        int x2 = x1 + side, y2 = y1 + side;
        canvas.ApplyRectSelection(x1, y1, x2, y2, false, false);
        canvas.UpdateSelectionMaskTexture(device);
        canvas.StartMovePixels(device);
        m_TransformT = 0.f;
        m_MoveDx = RandI(80, 280) * (RandI(0, 1) ? 1 : -1);
        m_MoveDy = RandI(60, 220) * (RandI(0, 1) ? 1 : -1);
        m_SubStep = 3;
        return true;
    }
    case 3: {
        m_TransformT += dtSec;
        float dur = 0.7f;
        float t = std::clamp(m_TransformT / dur, 0.f, 1.f);
        float te = t * t * (3.f - 2.f * t);
        canvas.UpdateMovePixels(device,
            (int)std::lround(m_MoveDx * te),
            (int)std::lround(m_MoveDy * te));
        if (t >= 1.f) {
            canvas.CommitMovePixels(device);
            ScheduleWait(50.f, 140.f);
            m_SubStep = 4;
        }
        return true;
    }
    default:
        return false;
    }
}

bool BenchmarkRunner::TickDeleteLayerUndo(Canvas& canvas, ID3D11Device* device, float dtSec) {
    if (Waiting(dtSec)) return true;
    switch (m_SubStep) {
    case 0: {
        // Prefer deleting layer 2 if present
        int n = (int)canvas.GetLayers().size();
        if (n >= 2) {
            canvas.SetActiveLayerIndex(n - 1);
            canvas.DeleteLayer(n - 1);
            Logger::Get().InfoTag("bench", "Deleted top layer");
        } else if (n == 1) {
            // create then delete so undo path still runs
            canvas.CreateNewLayer(device, "Temp delete");
            int n2 = (int)canvas.GetLayers().size();
            canvas.DeleteLayer(n2 - 1);
            Logger::Get().InfoTag("bench", "Created+deleted temp layer");
        }
        ScheduleWait(100.f, 250.f);
        m_SubStep = 1;
        return true;
    }
    case 1: {
        if (canvas.CanUndo()) {
            canvas.Undo();
            Logger::Get().InfoTag("bench", "Undo restore layer");
        }
        ScheduleWait(80.f, 200.f);
        m_SubStep = 2;
        return true;
    }
    default:
        return false;
    }
}

void BenchmarkRunner::AdvancePhase(Canvas& canvas, ID3D11Device* device) {
    FinishStrokeIfActive(canvas);
    m_Queue.clear();
    m_QueueIdx = 0;
    m_StrokeLive = false;
    m_SubStep = 0;

    Phase next = Phase::Done;
    switch (m_Phase) {
    case Phase::Setup:               next = Phase::PaintCover; break;
    case Phase::PaintCover:          next = Phase::EraseCover; break;
    case Phase::EraseCover:          next = Phase::SelectMoveTransform; break;
    case Phase::SelectMoveTransform: next = Phase::ColorAndOperator; break;
    case Phase::ColorAndOperator:    next = Phase::SecondLayerBlur; break;
    case Phase::SecondLayerBlur:     next = Phase::PaintOnLayer2; break;
    case Phase::PaintOnLayer2:       next = Phase::UndoRedoStress; break;
    case Phase::UndoRedoStress:      next = Phase::PaintAndMove; break;
    case Phase::PaintAndMove:        next = Phase::DeleteLayerUndo; break;
    case Phase::DeleteLayerUndo:     next = Phase::FinalPaint; break;
    case Phase::FinalPaint:          next = Phase::Report; break;
    case Phase::Report:              next = Phase::Done; break;
    case Phase::Done:                next = Phase::Done; break;
    }

    if (next == Phase::PaintCover) {
        BuildPaintCoverStrokes(canvas.GetWidth(), canvas.GetHeight());
    } else if (next == Phase::EraseCover) {
        BuildEraseStrokes(canvas.GetWidth(), canvas.GetHeight());
    } else if (next == Phase::PaintOnLayer2 || next == Phase::FinalPaint) {
        BuildFreehandStrokes(canvas.GetWidth(), canvas.GetHeight(),
            next == Phase::FinalPaint ? 4 : 5, false, next == Phase::FinalPaint ? 1.1f : 1.0f);
    } else if (next == Phase::UndoRedoStress) {
        m_MemBeforeUndoRedoWS = 0;
        m_MemAfterUndoRedoWS = 0;
    }

    BeginPhase(canvas, next);
}

void BenchmarkRunner::EmitReport(Canvas& canvas) {
    auto now = std::chrono::steady_clock::now();
    double totalSec = std::chrono::duration<double>(now - m_StartTime).count();
    SnapshotMem(m_MemEndWS, m_CurStats.memPrivateEnd);

    const double avgWork = m_TotalFrames > 0 ? m_TotalWorkMs / m_TotalFrames : 0.0;
    // Approx FPS from work time (ignores vsync sleep): lower bound when work is heavy
    const double approxFps = avgWork > 0.01 ? 1000.0 / avgWork : 0.0;

    std::ostringstream oss;
    oss << "\n========== RayV Paint BENCHMARK REPORT ==========\n";
    oss << "Canvas: " << canvas.GetWidth() << "x" << canvas.GetHeight() << "\n";
    oss << "Duration: " << totalSec << " s\n";
    oss << "Frames: " << m_TotalFrames << "\n";
    oss << "Work time avg/min/max: " << avgWork << " / "
        << (m_GlobalMinWork >= 1e8 ? 0.0 : m_GlobalMinWork) << " / "
        << m_GlobalMaxWork << " ms\n";
    oss << "Approx FPS from work (uncapped): " << approxFps << "\n";
    oss << "Frames >33ms: " << m_FramesOver33ms
        << "  >50ms: " << m_FramesOver50ms
        << "  >100ms: " << m_FramesOver100ms << "\n";
    oss << "Working set start/peak/end: "
        << MemoryStats::FormatBytes(m_MemStartWS) << " / "
        << MemoryStats::FormatBytes(m_MemPeakWS) << " / "
        << MemoryStats::FormatBytes(m_MemEndWS) << "\n";
    if (m_MemBeforeUndoRedoWS > 0 && m_MemAfterUndoRedoWS > 0) {
        long long delta = (long long)m_MemAfterUndoRedoWS - (long long)m_MemBeforeUndoRedoWS;
        oss << "Undo/Redo WS delta: " << (delta >= 0 ? "+" : "")
            << MemoryStats::FormatBytes((size_t)std::llabs(delta))
            << " (before " << MemoryStats::FormatBytes(m_MemBeforeUndoRedoWS)
            << " → after " << MemoryStats::FormatBytes(m_MemAfterUndoRedoWS) << ")\n";
        // Soft fail hint: large permanent growth after undo/redo cycle
        if (delta > 64ll * 1024 * 1024)
            oss << "WARNING: WS grew >64 MiB across undo/redo — possible memory leak\n";
    }
    oss << "Active layer tiles: " << canvas.GetActiveLayerTileCount() << "\n";
    oss << "Layers: " << canvas.GetLayers().size() << "\n";
    oss << "---- Per phase ----\n";
    for (const auto& p : m_PhaseStats) {
        double a = p.frames > 0 ? p.workMsSum / p.frames : 0.0;
        oss << "  [" << p.name << "] " << p.durationMs << "ms  frames=" << p.frames
            << "  avg=" << a << "ms  p95=" << p.workMsP95 << "ms  max=" << p.workMsMax
            << "ms  tiles " << p.tilesStart << "→" << p.tilesEnd
            << "  WS " << MemoryStats::FormatBytes(p.memWorkingSetStart)
            << "→" << MemoryStats::FormatBytes(p.memWorkingSetEnd) << "\n";
    }
    oss << "=================================================\n";

    std::string report = oss.str();
    Logger::Get().InfoTag("bench", report);
    std::printf("%s", report.c_str());
    std::fflush(stdout);

    // Exit code: 0 = ok, 1 = severe frame spikes, 2 = suspected memory growth
    m_ExitCode = 0;
    if (m_FramesOver100ms > m_TotalFrames / 4 && m_TotalFrames > 30)
        m_ExitCode = 1;
    if (m_MemBeforeUndoRedoWS > 0 &&
        (long long)m_MemAfterUndoRedoWS - (long long)m_MemBeforeUndoRedoWS > 128ll * 1024 * 1024)
        m_ExitCode = 2;
}

void BenchmarkRunner::Start(Canvas& canvas, ID3D11Device* device) {
    if (!m_Enabled || m_Active) return;
    m_Active = true;
    m_Finished = false;
    m_ExitCode = 0;
    m_StartTime = std::chrono::steady_clock::now();
    m_PhaseStats.clear();
    m_TotalFrames = 0;
    m_TotalWorkMs = 0.0;
    m_GlobalMinWork = 1e9;
    m_GlobalMaxWork = 0.0;
    m_FramesOver33ms = m_FramesOver50ms = m_FramesOver100ms = 0;
    SnapshotMem(m_MemStartWS, m_CurStats.memPrivateStart);
    m_MemPeakWS = m_MemStartWS;

    Logger::Get().InfoTag("bench", "========== Benchmark starting ==========");
    MemoryStats::LogSnapshot("bench_start");

    BeginPhase(canvas, Phase::Setup);
    SetupCanvas8K(canvas, device);
    // one frame settle
    ScheduleWait(150.f, 250.f);
    m_SubStep = 1; // setup complete after wait
}

bool BenchmarkRunner::Tick(Canvas& canvas, ID3D11Device* device, float frameWorkMs) {
    if (!m_Active || m_Finished) return false;

    // Stroke advance uses clamped dt so a multi-second commit doesn't fast-forward the pen.
    float dtSec = frameWorkMs * 0.001f;
    if (dtSec < 0.001f) dtSec = 0.001f;
    if (dtSec > 0.05f) dtSec = 0.05f;

    RecordFrame(frameWorkMs);
    // Wall-clock phase elapsed — heavy frames (float transform) still count toward caps.
    m_PhaseElapsed = (float)std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_PhaseStart).count();
    UpdateStatus();

    // Hard timeout so we never hang forever (phase caps should finish earlier ~30s)
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_StartTime).count();
    if (elapsed > 48.0 && m_Phase != Phase::Report && m_Phase != Phase::Done) {
        Logger::Get().WarnTag("bench", "Hard timeout 48s — forcing report");
        FinishStrokeIfActive(canvas);
        if (canvas.IsMovingPixels())
            canvas.CommitMovePixels(device);
        BeginPhase(canvas, Phase::Report);
    }

    bool phaseBusy = true;

    switch (m_Phase) {
    case Phase::Setup:
        if (!Waiting(dtSec) && m_SubStep >= 1)
            phaseBusy = false;
        break;

    case Phase::PaintCover:
    case Phase::EraseCover:
    case Phase::PaintOnLayer2:
    case Phase::FinalPaint:
        phaseBusy = AdvanceStroke(canvas, dtSec);
        // Phase caps keep total run ~25–40s even if path queues are long
        if (m_PhaseElapsed > 9.f && (m_Phase == Phase::PaintCover)) {
            FinishStrokeIfActive(canvas);
            phaseBusy = false;
        } else if (m_PhaseElapsed > 4.5f && m_Phase == Phase::EraseCover) {
            FinishStrokeIfActive(canvas);
            phaseBusy = false;
        } else if (m_PhaseElapsed > 3.5f &&
                   (m_Phase == Phase::PaintOnLayer2 || m_Phase == Phase::FinalPaint)) {
            FinishStrokeIfActive(canvas);
            phaseBusy = false;
        }
        break;

    case Phase::SelectMoveTransform:
        phaseBusy = TickSelectMoveTransform(canvas, device, dtSec);
        if (m_PhaseElapsed > 6.f) {
            if (canvas.IsMovingPixels()) canvas.CommitMovePixels(device);
            phaseBusy = false;
        }
        break;

    case Phase::ColorAndOperator:
        phaseBusy = TickColorAndOperator(canvas, device, dtSec);
        if (m_PhaseElapsed > 4.f) {
            FinishStrokeIfActive(canvas);
            phaseBusy = false;
        }
        break;

    case Phase::SecondLayerBlur:
        phaseBusy = TickSecondLayerBlur(canvas, device, dtSec);
        if (m_PhaseElapsed > 2.5f) phaseBusy = false;
        break;

    case Phase::UndoRedoStress:
        phaseBusy = TickUndoRedo(canvas, dtSec);
        if (m_PhaseElapsed > 4.f) {
            if (m_MemAfterUndoRedoWS == 0)
                SnapshotMem(m_MemAfterUndoRedoWS, m_CurStats.memPrivateEnd);
            phaseBusy = false;
        }
        break;

    case Phase::PaintAndMove:
        phaseBusy = TickPaintAndMove(canvas, device, dtSec);
        if (m_PhaseElapsed > 4.f) {
            FinishStrokeIfActive(canvas);
            if (canvas.IsMovingPixels()) canvas.CommitMovePixels(device);
            phaseBusy = false;
        }
        break;

    case Phase::DeleteLayerUndo:
        phaseBusy = TickDeleteLayerUndo(canvas, device, dtSec);
        if (m_PhaseElapsed > 2.5f) phaseBusy = false;
        break;

    case Phase::Report:
        EndPhase(canvas);
        EmitReport(canvas);
        m_Phase = Phase::Done;
        m_Finished = true;
        m_Active = false;
        Logger::Get().InfoTag("bench", "Benchmark finished, exitCode=" + std::to_string(m_ExitCode));
        return false;

    case Phase::Done:
        m_Finished = true;
        m_Active = false;
        return false;
    }

    if (!phaseBusy) {
        AdvancePhase(canvas, device);
        if (m_Phase == Phase::Report) {
            // run report next tick
            return true;
        }
    }

    return true;
}

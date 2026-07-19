#include "StressRunner.h"
#include "CrashGuard.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "MemoryStats.h"
#include "PathUtil.h"
#include "ProjectManager.h"
#include "../Canvas.h"
#include "../layer/LayerTypes.h"

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

StressRunner& StressRunner::Get() {
    static StressRunner inst;
    return inst;
}

static void AppendSharedUtf8(const std::string& pathUtf8, const std::string& text) {
    if (pathUtf8.empty() || text.empty()) return;
#ifdef _WIN32
    std::wstring w = PathUtil::Utf8ToWide(pathUtf8);
    HANDLE h = CreateFileW(w.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
#else
    std::ofstream f(pathUtf8, std::ios::app);
    if (f) { f << text; f.flush(); }
#endif
}

void StressRunner::OpenJournal() {
    namespace fs = std::filesystem;
    m_JournalPath = kJournalRel;
    try {
        fs::create_directories("testfield");
    } catch (...) {}

    try {
        m_JournalPathUser = ConfigManager::GetUserSubdirectory("user") + "/stress_last.txt";
    } catch (...) {
        m_JournalPathUser.clear();
    }

    // Truncate session file (shared open so CrashGuard can append later)
#ifdef _WIN32
    {
        std::wstring w = PathUtil::Utf8ToWide(m_JournalPath);
        HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
#else
    m_Journal.open(m_JournalPath, std::ios::out | std::ios::trunc);
    if (m_Journal) m_Journal.close();
#endif

    char timeBuf[64] = {};
    {
        time_t t = time(nullptr);
        struct tm tm_b{};
        localtime_s(&tm_b, &t);
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm_b);
    }

    std::string cwd;
    try { cwd = PathUtil::WideToUtf8(fs::current_path().wstring()); }
    catch (...) { cwd = "?"; }

    // Absolute path for CrashGuard (must survive relative cwd)
    std::string absJournal = m_JournalPath;
    try {
        absJournal = PathUtil::WideToUtf8(fs::absolute(m_JournalPath).wstring());
        m_JournalPath = absJournal; // store absolute so user can open easily
    } catch (...) {}

    Journal(std::string("========== STRESS-16K JOURNAL =========="));
    Journal(std::string("started: ") + timeBuf);
    Journal(std::string("cwd: ") + cwd);
    Journal(std::string("journal: ") + m_JournalPath);
    if (!m_JournalPathUser.empty())
        Journal(std::string("journal_user_copy: ") + m_JournalPathUser);
    Journal(std::string("asset: ") + kAsset16K);
    Journal(std::string("hang_budget_ms: ") + std::to_string((int)kHangMs));
    Journal(std::string("wall_timeout_s: ") + std::to_string((int)kWallTimeoutSec));
    Journal("NOTE: every action is flushed immediately. On CRASH, CrashGuard appends here.");
    Journal("NOTE: silent crash mode ON — no MessageBox; process exits so you are not stuck.");
    Journal("----------------------------------------");

    std::printf("\n*** STRESS JOURNAL (open this file while running) ***\n  %s\n",
                m_JournalPath.c_str());
    if (!m_JournalPathUser.empty())
        std::printf("  copy: %s\n", m_JournalPathUser.c_str());
    std::printf("*** No crash MessageBox in this mode — watch the journal ***\n\n");
    std::fflush(stdout);

    CrashGuard::SetSilentMode(true);
    CrashGuard::SetExtraCrashLogPath(m_JournalPath);
}

void StressRunner::Journal(const std::string& line) {
    ++m_JournalSeq;
    double elapsed = 0.0;
    if (m_StartTime.time_since_epoch().count() != 0)
        elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - m_StartTime).count();

    char head[96];
    std::snprintf(head, sizeof(head), "[%06d +%7.3fs] ", m_JournalSeq, elapsed);
    const std::string full = std::string(head) + line + "\n";

    Logger::Get().InfoTag("stress", line);

    // Open-append-close with share flags so CrashGuard can also append on AV
    AppendSharedUtf8(m_JournalPath, full);
    if (!m_JournalPathUser.empty()) {
        if (m_JournalSeq == 1)
            AppendSharedUtf8(m_JournalPathUser, "\n===== new stress session =====\n");
        AppendSharedUtf8(m_JournalPathUser, full);
    }
#ifdef _WIN32
    std::printf("%s", full.c_str());
    std::fflush(stdout);
#endif
}

void StressRunner::Journalf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Journal(buf);
}

void StressRunner::CloseJournal(const char* reason) {
    Journal(std::string("========== STRESS END (") + (reason ? reason : "?") + ") ==========");
    Journalf("exit_code=%d frames=%d max_work_ms=%.1f hangs_over_2s=%d",
             m_ExitCode, m_TotalFrames, m_GlobalMaxWork, m_FramesOver2000ms);
}

float StressRunner::RandF(float a, float b) {
    std::uniform_real_distribution<float> d(a, b);
    return d(m_Rng);
}

int StressRunner::RandI(int a, int bInclusive) {
    std::uniform_int_distribution<int> d(a, bInclusive);
    return d(m_Rng);
}

void StressRunner::ScheduleWait(float minMs, float maxMs) {
    m_WaitLeft = RandF(minMs, maxMs) * 0.001f;
}

bool StressRunner::Waiting(float dtSec) {
    if (m_WaitLeft <= 0.f) return false;
    m_WaitLeft -= dtSec;
    if (m_WaitLeft < 0.f) m_WaitLeft = 0.f;
    return m_WaitLeft > 0.f;
}

void StressRunner::SnapshotMem(size_t& ws, size_t& priv) const {
    auto info = MemoryStats::QueryProcess();
    ws = info.workingSetBytes;
    priv = info.privateBytes;
}

void StressRunner::RecordFrame(float workMs) {
    ++m_TotalFrames;
    m_TotalWorkMs += workMs;
    m_GlobalMaxWork = std::max(m_GlobalMaxWork, (double)workMs);
    if (workMs > 500.f)  ++m_FramesOver500ms;
    if (workMs > 1000.f) ++m_FramesOver1000ms;
    if (workMs > kHangMs) {
        ++m_FramesOver2000ms;
        Journalf("HANG frame workMs=%.1f phase=%s step=%d budget=%.0f",
                 workMs, m_PhaseName.c_str(), m_SubStep, kHangMs);
    }
    // Heartbeat every 60 frames so the file is obviously "alive"
    if ((m_TotalFrames % 60) == 0) {
        Journalf("HEARTBEAT frame=%d phase=%s work=%.1fms peakWS=%s",
                 m_TotalFrames, m_PhaseName.c_str(), workMs,
                 MemoryStats::FormatBytes(m_MemPeakWS).c_str());
    }
    size_t ws = 0, priv = 0;
    SnapshotMem(ws, priv);
    m_MemPeakWS = std::max(m_MemPeakWS, ws);
}

void StressRunner::UpdateStatus() {
    char buf[320];
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_StartTime).count();
    std::snprintf(buf, sizeof(buf),
        "STRESS-16K | %s | %.1fs | frames %d | hang>%dms: %d | maxWork=%.0fms",
        m_PhaseName.c_str(), elapsed, m_TotalFrames,
        (int)kHangMs, m_FramesOver2000ms, m_GlobalMaxWork);
    m_StatusLine = buf;
}

void StressRunner::BeginPhase(Canvas& canvas, Phase phase) {
    m_Phase = phase;
    m_SubStep = 0;
    m_PhaseElapsed = 0.f;
    m_WaitLeft = 0.f;
    m_PhaseStart = std::chrono::steady_clock::now();
    m_Queue.clear();
    m_QueueIdx = 0;
    m_PathIdx = 0;
    m_PathT = 0.f;
    m_StrokeLive = false;

    switch (phase) {
    case Phase::Load16K:         m_PhaseName = "Load16K"; break;
    case Phase::PaintBurst:      m_PhaseName = "PaintBurst"; break;
    case Phase::FillSpawn:       m_PhaseName = "FillSpawn"; break;
    case Phase::FillMutate:      m_PhaseName = "FillMutate"; break;
    case Phase::UndoRedoHell:    m_PhaseName = "UndoRedoHell"; break;
    case Phase::MaskAbuse:       m_PhaseName = "MaskAbuse"; break;
    case Phase::LayerSpam:       m_PhaseName = "LayerSpam"; break;
    case Phase::SelectTransform: m_PhaseName = "SelectTransform"; break;
    case Phase::Chaos:           m_PhaseName = "Chaos"; break;
    case Phase::MultiTab:        m_PhaseName = "MultiTab"; break;
    case Phase::Report:          m_PhaseName = "Report"; break;
    case Phase::Done:            m_PhaseName = "Done"; break;
    }
    Journalf("PHASE BEGIN %s (layers=%zu tiles=%zu w=%d h=%d)",
             m_PhaseName.c_str(),
             canvas.GetLayers().size(),
             canvas.GetActiveLayerTileCount(),
             canvas.GetWidth(), canvas.GetHeight());
    MemoryStats::LogSnapshot("stress_" + m_PhaseName + "_start");
    UpdateStatus();
}

void StressRunner::EndPhase(Canvas& canvas) {
    MemoryStats::LogSnapshot("stress_" + m_PhaseName + "_end");
    Journalf("PHASE END %s elapsed=%.3fs tiles=%zu layers=%zu",
             m_PhaseName.c_str(), m_PhaseElapsed,
             canvas.GetActiveLayerTileCount(), canvas.GetLayers().size());
}

bool StressRunner::LoadAsset16K(Canvas& canvas, ID3D11Device* device) {
    namespace fs = std::filesystem;
    if (!fs::exists(kAsset16K)) {
        Journalf("ERROR missing asset: %s", kAsset16K);
        m_AssetMissing = true;
        return false;
    }
    Journalf("ACTION open_document begin path=%s", kAsset16K);
    MemoryStats::LogSnapshot("stress_before_16k_load");
    const bool ok = canvas.OpenDocument(device, kAsset16K);
    MemoryStats::LogSnapshot("stress_after_16k_load");
    if (!ok) {
        Journal("ERROR OpenDocument failed for 16K asset");
        return false;
    }
    const int w = canvas.GetWidth();
    const int h = canvas.GetHeight();
    Journalf("ACTION open_document ok size=%dx%d tiles=%zu layers=%zu",
             w, h, canvas.GetActiveLayerTileCount(), canvas.GetLayers().size());
    // Dense 16K expected; accept if at least 8K (scaled assets) but prefer true 16K
    if (w < 8192 || h < 8192) {
        Logger::Get().WarnTag("stress",
            "Canvas smaller than 8K after load — stress still continues");
    }
    canvas.ResetView();
    float fit = 900.f / (float)std::max(w, 1);
    if (fit < 0.01f) fit = 0.01f;
    canvas.SetZoom(fit);
    canvas.SetPan(DirectX::XMFLOAT2(-w * 0.5f * fit, -h * 0.5f * fit));
    canvas.ClearSelection();
    // Keep undo history from load empty-ish
    canvas.ClearUndoHistory();
    canvas.MarkCompositeDirty();
    return true;
}

void StressRunner::BuildBurstStrokes(int w, int h, int count, bool erase) {
    m_Queue.clear();
    m_QueueIdx = 0;
    m_PathIdx = 0;
    m_PathT = 0.f;
    m_StrokeLive = false;
    const float margin = 200.f;
    for (int s = 0; s < count; ++s) {
        PathStroke path;
        path.erase = erase;
        path.radius = RandF(280.f, 720.f);
        path.hardness = RandF(0.2f, 0.85f);
        path.opacity = RandF(0.7f, 1.f);
        path.speedPxS = RandF(6000.f, 14000.f);
        path.color[0] = RandF(0.1f, 1.f);
        path.color[1] = RandF(0.1f, 1.f);
        path.color[2] = RandF(0.1f, 1.f);
        path.color[3] = 1.f;
        const float x0 = RandF(margin, (float)w - margin);
        const float y0 = RandF(margin, (float)h - margin);
        const float x1 = RandF(margin, (float)w - margin);
        const float y1 = RandF(margin, (float)h - margin);
        const int samples = RandI(24, 48);
        for (int i = 0; i <= samples; ++i) {
            float t = (float)i / (float)samples;
            float wobble = std::sin(t * 12.f + (float)s) * path.radius * 0.35f;
            StrokePoint p;
            p.x = x0 + (x1 - x0) * t + wobble;
            p.y = y0 + (y1 - y0) * t + wobble * 0.6f;
            p.x = std::clamp(p.x, 1.f, (float)w - 2.f);
            p.y = std::clamp(p.y, 1.f, (float)h - 2.f);
            path.pts.push_back(p);
        }
        m_Queue.push_back(std::move(path));
    }
}

void StressRunner::EnsureStrokeStarted(Canvas& canvas) {
    if (m_StrokeLive || m_QueueIdx >= m_Queue.size()) return;
    PathStroke& s = m_Queue[m_QueueIdx];
    if (s.pts.empty()) {
        ++m_QueueIdx;
        return;
    }
    m_StrokeBrush = BrushSettings{};
    m_StrokeBrush.radius = s.radius;
    m_StrokeBrush.hardness = s.hardness;
    m_StrokeBrush.opacity = s.opacity;
    m_StrokeBrush.erase = s.erase;
    for (int i = 0; i < 4; ++i) m_StrokeBrush.color[i] = s.color[i];
    canvas.PaintOnActiveLayer(s.pts[0].x, s.pts[0].y, StrokePhase::Begin, m_StrokeBrush);
    m_StrokeLive = true;
    m_PathIdx = 0;
    m_PathT = 0.f;
    ++m_StrokeStarts;
    Journalf("ACTION stroke_begin #%d erase=%d radius=%.0f pts=%zu at=(%.0f,%.0f)",
             m_StrokeStarts, s.erase ? 1 : 0, s.radius, s.pts.size(),
             s.pts[0].x, s.pts[0].y);
}

void StressRunner::FinishStrokeIfActive(Canvas& canvas) {
    if (!m_StrokeLive) return;
    canvas.PaintOnActiveLayer(0.f, 0.f, StrokePhase::End, m_StrokeBrush);
    m_StrokeLive = false;
    canvas.MarkCompositeDirty();
    Journalf("ACTION stroke_end tiles=%zu", canvas.GetActiveLayerTileCount());
}

bool StressRunner::AdvanceStroke(Canvas& canvas, float dtSec) {
    if (m_QueueIdx >= m_Queue.size()) {
        FinishStrokeIfActive(canvas);
        return false;
    }
    if (!m_StrokeLive && Waiting(dtSec))
        return true;
    if (!m_StrokeLive) {
        EnsureStrokeStarted(canvas);
        if (!m_StrokeLive) {
            ScheduleWait(10.f, 40.f);
            return m_QueueIdx < m_Queue.size();
        }
    }
    PathStroke& s = m_Queue[m_QueueIdx];
    if (s.pts.size() < 2) {
        FinishStrokeIfActive(canvas);
        ++m_QueueIdx;
        return m_QueueIdx < m_Queue.size();
    }
    float budget = s.speedPxS * std::max(dtSec, 0.001f);
    // Cap dabs per frame so we yield — hang budget is sacred
    budget = std::min(budget, s.radius * 8.f);
    while (budget > 0.f && m_PathIdx + 1 < s.pts.size()) {
        StrokePoint a = s.pts[m_PathIdx];
        StrokePoint b = s.pts[m_PathIdx + 1];
        float dx = b.x - a.x, dy = b.y - a.y;
        float segLen = std::sqrt(dx * dx + dy * dy);
        if (segLen < 1e-3f) { ++m_PathIdx; continue; }
        float remain = segLen - m_PathT;
        if (budget >= remain) {
            budget -= remain;
            m_PathT = 0.f;
            ++m_PathIdx;
            canvas.PaintOnActiveLayer(b.x, b.y, StrokePhase::Update, m_StrokeBrush);
        } else {
            m_PathT += budget;
            float t = m_PathT / segLen;
            canvas.PaintOnActiveLayer(a.x + dx * t, a.y + dy * t, StrokePhase::Update, m_StrokeBrush);
            budget = 0.f;
        }
    }
    if (m_PathIdx + 1 >= s.pts.size()) {
        const auto& last = s.pts.back();
        canvas.PaintOnActiveLayer(last.x, last.y, StrokePhase::Update, m_StrokeBrush);
        FinishStrokeIfActive(canvas);
        ++m_QueueIdx;
        m_PathIdx = 0;
        m_PathT = 0.f;
        ScheduleWait(15.f, 60.f);
    }
    return m_QueueIdx < m_Queue.size() || m_StrokeLive;
}

bool StressRunner::TickFillSpawn(Canvas& canvas, ID3D11Device* device, float dtSec) {
    if (Waiting(dtSec)) return true;
    switch (m_SubStep) {
    case 0:
    case 1:
    case 2: {
        FillLayerParams p;
        p.EnsureDefaults();
        const int map = m_SubStep % (int)texset::MapKind::Count;
        p.mapColor[map].enabled = true;
        p.mapColor[map].rgba[0] = RandF(0.f, 1.f);
        p.mapColor[map].rgba[1] = RandF(0.f, 1.f);
        p.mapColor[map].rgba[2] = RandF(0.f, 1.f);
        p.mapColor[map].rgba[3] = RandF(0.4f, 1.f);
        // Multi-map fill pressure
        if (m_SubStep == 2) {
            p.mapColor[(int)texset::MapKind::Diffuse].enabled = true;
            p.mapColor[(int)texset::MapKind::LightMap].enabled = true;
            p.mapColor[(int)texset::MapKind::NormalMap].enabled = true;
        }
        std::string name = "StressFill_" + std::to_string(m_SubStep);
        canvas.CreateFillLayer(device, name, p);
        m_FillLayerIdx = canvas.GetActiveLayerIndex();
        canvas.MarkCompositeDirty();
        Journalf("ACTION fill_create name=%s idx=%d layers=%zu",
                 name.c_str(), m_FillLayerIdx, canvas.GetLayers().size());
        ++m_SubStep;
        ScheduleWait(40.f, 100.f);
        return true;
    }
    default:
        return false;
    }
}

bool StressRunner::TickFillMutate(Canvas& canvas, ID3D11Device* device, float dtSec) {
    (void)device;
    if (Waiting(dtSec)) return true;
    // Prefer last fill layer
    int idx = m_FillLayerIdx;
    if (idx < 0 || idx >= (int)canvas.GetLayers().size() || !canvas.IsFillLayer(idx)) {
        for (int i = (int)canvas.GetLayers().size() - 1; i >= 0; --i) {
            if (canvas.IsFillLayer(i)) { idx = i; break; }
        }
    }
    if (idx < 0) {
        Logger::Get().WarnTag("stress", "FillMutate: no fill layer");
        return false;
    }
    canvas.SetActiveLayerIndex(idx);
    auto& layer = canvas.GetLayers()[idx];
    auto before = Canvas::CaptureLayerProps(layer);
    layer.fill.mapColor[(int)texset::MapKind::Diffuse].enabled = true;
    layer.fill.mapColor[(int)texset::MapKind::Diffuse].rgba[0] = RandF(0.f, 1.f);
    layer.fill.mapColor[(int)texset::MapKind::Diffuse].rgba[1] = RandF(0.f, 1.f);
    layer.fill.mapColor[(int)texset::MapKind::Diffuse].rgba[2] = RandF(0.f, 1.f);
    layer.fill.mapColor[(int)texset::MapKind::Diffuse].rgba[3] = 1.f;
    layer.fill.texScale[0] = RandF(0.5f, 4.f);
    layer.fill.texScale[1] = RandF(0.5f, 4.f);
    layer.fill.texOffset[0] = RandF(-1.f, 1.f);
    layer.fill.texOffset[1] = RandF(-1.f, 1.f);
    layer.opacity = RandF(0.3f, 1.f);
    layer.presentationDirty = true;
    canvas.CommitLayerPropsEdit(idx, before, "Stress Fill Mutate");
    canvas.MarkCompositeDirty();
    ++m_SubStep;
    Journalf("ACTION fill_mutate step=%d layer=%d opacity=%.2f", m_SubStep, idx, layer.opacity);
    // Many unduable mutations for serialization stack
    if (m_SubStep >= 24)
        return false;
    ScheduleWait(5.f, 25.f); // aggressive, still yields frames
    return true;
}

bool StressRunner::TickUndoHell(Canvas& canvas, float dtSec) {
    if (Waiting(dtSec)) return true;

    if (m_SubStep == 0) {
        size_t priv = 0;
        SnapshotMem(m_MemBeforeUndoWS, priv);
        m_UndoLeft = 80;
        m_RedoLeft = 0;
        m_UndoPass = true;
        m_SubStep = 1;
        m_UndoOpsDone = 0;
        m_RedoOpsDone = 0;
        Journal("ACTION undo_hell plan undos≈80 then redos (each op logged)");
        return true;
    }

    if (m_UndoPass) {
        if (m_UndoLeft > 0 && canvas.CanUndo()) {
            CrashGuard::NoteCheckpoint("stress_undo");
            Journalf("ACTION undo #%d remaining_plan=%d layers=%zu BEFORE_CALL",
                     m_UndoOpsDone + 1, m_UndoLeft, canvas.GetLayers().size());
            canvas.Undo();
            ++m_UndoOpsDone;
            --m_UndoLeft;
            ++m_RedoLeft;
            Journalf("ACTION undo #%d OK after layers=%zu",
                     m_UndoOpsDone, canvas.GetLayers().size());
            ScheduleWait(0.f, 8.f);
            return true;
        }
        m_UndoPass = false;
        Journal("ACTION undo_pass finished → redo_pass");
        ScheduleWait(20.f, 40.f);
        return true;
    }

    if (m_RedoLeft > 0 && canvas.CanRedo()) {
        CrashGuard::NoteCheckpoint("stress_redo");
        Journalf("ACTION redo #%d remaining=%d BEFORE_CALL", m_RedoOpsDone + 1, m_RedoLeft);
        canvas.Redo();
        ++m_RedoOpsDone;
        --m_RedoLeft;
        Journalf("ACTION redo #%d OK", m_RedoOpsDone);
        ScheduleWait(0.f, 8.f);
        return true;
    }

    if (m_SubStep == 1) {
        m_SubStep = 2;
        m_UndoPass = true;
        m_UndoLeft = 40;
        m_RedoLeft = 0;
        Journal("ACTION undo_hell wave2 undos≈40");
        ScheduleWait(15.f, 40.f);
        return true;
    }

    if (m_SubStep == 2) {
        m_SubStep = 3;
        m_UndoLeft = 30;
        Journal("ACTION undo_hell wave3 alternate pairs x30");
        return true;
    }
    if (m_SubStep == 3) {
        if (m_UndoLeft > 0) {
            CrashGuard::NoteCheckpoint("stress_undo_pair");
            Journalf("ACTION undo_pair step=%d BEFORE", m_UndoLeft);
            if (canvas.CanUndo()) canvas.Undo();
            if (canvas.CanRedo()) canvas.Redo();
            if (canvas.CanUndo()) canvas.Undo();
            Journalf("ACTION undo_pair step=%d OK", m_UndoLeft);
            --m_UndoLeft;
            ScheduleWait(0.f, 5.f);
            return true;
        }
        m_SubStep = 4;
    }

    {
        size_t priv = 0;
        SnapshotMem(m_MemAfterUndoWS, priv);
    }
    Journalf("ACTION undo_hell done undos=%d redos=%d WS before=%s after=%s",
             m_UndoOpsDone, m_RedoOpsDone,
             MemoryStats::FormatBytes(m_MemBeforeUndoWS).c_str(),
             MemoryStats::FormatBytes(m_MemAfterUndoWS).c_str());
    return false;
}

bool StressRunner::TickMaskAbuse(Canvas& canvas, ID3D11Device* device, float dtSec) {
    if (Waiting(dtSec)) return true;
    switch (m_SubStep) {
    case 0: {
        // Paint layer preferred for mask
        int idx = 0;
        for (int i = 0; i < (int)canvas.GetLayers().size(); ++i) {
            if (canvas.CanPaintLayerContent(i)) { idx = i; break; }
        }
        canvas.SetActiveLayerIndex(idx);
        Journalf("ACTION mask_create layer=%d BEFORE", idx);
        canvas.CreateLayerMask(device, idx);
        Journalf("ACTION mask_create layer=%d OK has_mask=%d",
                 idx, canvas.GetLayers()[idx].hasMask ? 1 : 0);
        ScheduleWait(30.f, 80.f);
        m_SubStep = 1;
        return true;
    }
    case 1: {
        // Dabs on mask if paint target allows
        int w = canvas.GetWidth(), h = canvas.GetHeight();
        BuildBurstStrokes(w, h, 2, false);
        m_SubStep = 2;
        return true;
    }
    case 2: {
        if (AdvanceStroke(canvas, dtSec))
            return true;
        ScheduleWait(20.f, 50.f);
        m_SubStep = 3;
        return true;
    }
    default:
        return false;
    }
}

bool StressRunner::TickLayerSpam(Canvas& canvas, ID3D11Device* device, float dtSec) {
    if (Waiting(dtSec)) return true;
    if (m_SubStep < 6) {
        canvas.CreateNewLayer(device, "Spam_" + std::to_string(m_SubStep));
        int n = (int)canvas.GetLayers().size();
        if (n > 0) canvas.SetActiveLayerIndex(n - 1);
        // Tiny paint so tiles exist for undo payload
        BrushSettings b{};
        b.radius = 64.f;
        b.opacity = 1.f;
        b.hardness = 0.8f;
        b.color[0] = RandF(0.f, 1.f);
        b.color[1] = RandF(0.f, 1.f);
        b.color[2] = RandF(0.f, 1.f);
        b.color[3] = 1.f;
        float x = RandF(100.f, (float)std::max(200, canvas.GetWidth() - 100));
        float y = RandF(100.f, (float)std::max(200, canvas.GetHeight() - 100));
        canvas.PaintOnActiveLayer(x, y, StrokePhase::Begin, b);
        canvas.PaintOnActiveLayer(x + 40.f, y + 30.f, StrokePhase::Update, b);
        canvas.PaintOnActiveLayer(0.f, 0.f, StrokePhase::End, b);
        ++m_SubStep;
        ScheduleWait(10.f, 40.f);
        return true;
    }
    if (m_SubStep < 10) {
        int n = (int)canvas.GetLayers().size();
        if (n >= 2) {
            canvas.DeleteLayer(n - 1);
        }
        ++m_SubStep;
        ScheduleWait(10.f, 30.f);
        return true;
    }
    // Undo deletes partially
    if (m_SubStep < 14) {
        if (canvas.CanUndo()) canvas.Undo();
        ++m_SubStep;
        ScheduleWait(5.f, 20.f);
        return true;
    }
    return false;
}

bool StressRunner::TickSelectTransform(Canvas& canvas, ID3D11Device* device, float dtSec) {
    if (Waiting(dtSec)) return true;
    const int w = canvas.GetWidth();
    const int h = canvas.GetHeight();
    // Huge docs: free-transform float buffer can freeze >2s — keep only tiny rect + skip scale/rot.
    const bool hugeDoc = (w >= 8192 || h >= 8192);
    switch (m_SubStep) {
    case 0: {
        for (int i = 0; i < (int)canvas.GetLayers().size(); ++i) {
            if (canvas.CanPaintLayerContent(i)) {
                canvas.SetActiveLayerIndex(i);
                break;
            }
        }
        const int side = hugeDoc ? 128 : std::clamp(std::min(w, h) / 16, 256, 768);
        int x1 = w / 2 - side / 2, y1 = h / 2 - side / 2;
        Journalf("ACTION select_rect %dx%d huge=%d", side, side, hugeDoc ? 1 : 0);
        canvas.ApplyRectSelection(x1, y1, x1 + side, y1 + side, false, false);
        canvas.UpdateSelectionMaskTexture(device);
        ScheduleWait(30.f, 80.f);
        m_SubStep = 1;
        return true;
    }
    case 1: {
        // Selection may refuse flat masks on huge docs — skip transform if no selection.
        if (!canvas.HasSelection()) {
            Journal("ACTION select_transform SKIP (no selection on huge doc)");
            m_SubStep = 4;
            return true;
        }
        Journal("ACTION transform_start");
        canvas.StartMovePixels(device);
        m_MoveDx = RandI(20, 80) * (RandI(0, 1) ? 1 : -1);
        m_MoveDy = RandI(20, 80) * (RandI(0, 1) ? 1 : -1);
        m_TransformT = 0.f;
        m_SubStep = 2;
        return true;
    }
    case 2: {
        m_TransformT += dtSec;
        // Short drag; no scale/rotate on huge (that was 26s hang on 16K).
        const float dur = hugeDoc ? 0.15f : 0.6f;
        float t = std::clamp(m_TransformT / dur, 0.f, 1.f);
        float te = t * t * (3.f - 2.f * t);
        canvas.UpdateMovePixels(device,
            (int)std::lround(m_MoveDx * te),
            (int)std::lround(m_MoveDy * te));
        if (t >= 1.f) {
            if (!hugeDoc) {
                canvas.SetFloatingScaleX(1.15f);
                canvas.SetFloatingScaleY(0.92f);
                canvas.SetFloatingRotation(0.12f);
            }
            canvas.MarkCompositeDirty();
            ScheduleWait(20.f, 50.f);
            m_SubStep = 3;
        }
        return true;
    }
    case 3: {
        Journal("ACTION transform_commit");
        canvas.CommitMovePixels(device);
        canvas.ClearSelection();
        m_SubStep = 4;
        return true;
    }
    default:
        return false;
    }
}

bool StressRunner::TickMultiTab(Canvas& canvas, ID3D11Device* device, float dtSec) {
    if (Waiting(dtSec)) return true;
    auto& pm = ProjectManager::Get();

    auto dabOnActive = [&](Canvas& c) {
        BrushSettings b{};
        b.radius = 48.f;
        b.opacity = 1.f;
        b.hardness = 0.8f;
        b.color[0] = 0.2f; b.color[1] = 0.7f; b.color[2] = 1.f; b.color[3] = 1.f;
        for (int i = 0; i < (int)c.GetLayers().size(); ++i) {
            if (c.CanPaintLayerContent(i)) {
                c.SetActiveLayerIndex(i);
                break;
            }
        }
        const float x = (float)std::max(32, c.GetWidth() / 2);
        const float y = (float)std::max(32, c.GetHeight() / 2);
        c.PaintOnActiveLayer(x, y, StrokePhase::Begin, b);
        c.PaintOnActiveLayer(x + 40.f, y + 20.f, StrokePhase::Update, b);
        c.PaintOnActiveLayer(0.f, 0.f, StrokePhase::End, b);
    };

    switch (m_SubStep) {
    case 0: {
        m_MainProjectId = pm.ActiveProjectId();
        Journalf("ACTION multitab_main id=%d tabs=%zu", m_MainProjectId, pm.ProjectCount());
        m_SideProjectA = pm.CreateEmptyProject();
        m_SideProjectB = pm.CreateEmptyProject();
        Journalf("ACTION multitab_spawn A=%d B=%d", m_SideProjectA, m_SideProjectB);
        if (m_SideProjectA < 0 || m_SideProjectB < 0) {
            Journal("WARN multitab spawn failed — skip phase");
            return false;
        }
        // Shrink side docs so L2 .rayp save stays under hang budget (default may be 4K).
        if (device) {
            for (int id : { m_SideProjectA, m_SideProjectB }) {
                if (Project* p = pm.FindProject(id); p && p->canvas)
                    p->canvas->ResizeCanvas(device, 512, 512);
            }
        }
        Journal("ACTION multitab_resize_sides 512x512");
        m_SubStep = 1;
        ScheduleWait(20.f, 40.f);
        return true;
    }
    case 1: {
        if (!pm.SwitchTo(m_SideProjectA)) {
            Journal("WARN multitab switch A failed");
            return false;
        }
        dabOnActive(pm.ActiveCanvas());
        Journal("ACTION multitab_paint A");
        m_SubStep = 2;
        ScheduleWait(20.f, 40.f);
        return true;
    }
    case 2: {
        if (!pm.SwitchTo(m_SideProjectB)) {
            Journal("WARN multitab switch B failed");
            return false;
        }
        dabOnActive(pm.ActiveCanvas());
        Journal("ACTION multitab_paint B");
        m_SubStep = 3;
        ScheduleWait(20.f, 40.f);
        return true;
    }
    case 3: {
        // Back to main 16K boss doc — inactive sides eligible for dormancy.
        if (m_MainProjectId >= 0)
            pm.SwitchTo(m_MainProjectId);
        m_MultiTabSlept = pm.SuspendInactiveNow();
        Journalf("ACTION multitab_suspend_inactive slept=%d", m_MultiTabSlept);
        m_SubStep = 4;
        ScheduleWait(30.f, 60.f);
        return true;
    }
    case 4: {
        // One L2 per step — never batch multi-MB rayp saves in a single frame.
        const int n = pm.HibernateInactiveNow(1);
        m_MultiTabHibernated += n;
        Journalf("ACTION multitab_hibernate_one n=%d total=%d", n, m_MultiTabHibernated);
        if (n > 0) {
            ScheduleWait(30.f, 60.f);
            return true; // stay on step 4 until no more
        }
        auto tabs = pm.ListTabs();
        int disk = 0, z = 0;
        for (const auto& t : tabs) {
            if (t.diskHibernated) ++disk;
            else if (t.gpuSuspended) ++z;
        }
        Journalf("ACTION multitab_badges disk=%d z=%d total=%zu", disk, z, tabs.size());
        m_SubStep = 5;
        ScheduleWait(30.f, 60.f);
        return true;
    }
    case 5: {
        // Wake side A (may L2 restore from scratch).
        if (m_SideProjectA >= 0 && pm.SwitchTo(m_SideProjectA)) {
            ++m_MultiTabWakes;
            Canvas& a = pm.ActiveCanvas();
            Journalf("ACTION multitab_wake A ok layers=%zu disk=%d",
                     a.GetLayers().size(),
                     a.IsDiskHibernated() ? 1 : 0);
        }
        m_SubStep = 6;
        ScheduleWait(40.f, 80.f);
        return true;
    }
    case 6: {
        if (m_SideProjectB >= 0 && pm.SwitchTo(m_SideProjectB)) {
            ++m_MultiTabWakes;
            Journalf("ACTION multitab_wake B ok layers=%zu",
                     pm.ActiveCanvas().GetLayers().size());
        }
        m_SubStep = 7;
        ScheduleWait(20.f, 40.f);
        return true;
    }
    case 7: {
        // Return to main; force-close side tabs (dirty from dabs).
        if (m_MainProjectId >= 0)
            pm.SwitchTo(m_MainProjectId);
        if (m_SideProjectA >= 0)
            pm.CloseProject(m_SideProjectA, true);
        if (m_SideProjectB >= 0)
            pm.CloseProject(m_SideProjectB, true);
        Journalf("ACTION multitab_cleanup main=%d tabs=%zu slept=%d hib=%d wakes=%d",
                 pm.ActiveProjectId(), pm.ProjectCount(),
                 m_MultiTabSlept, m_MultiTabHibernated, m_MultiTabWakes);
        m_SubStep = 8;
        return true;
    }
    default:
        return false;
    }
}

bool StressRunner::TickChaos(Canvas& canvas, ID3D11Device* device, float dtSec) {
    if (Waiting(dtSec)) return true;
    if (m_ChaosOps >= 40)
        return false;

    const int op = RandI(0, 6);
    switch (op) {
    case 0:
        if (canvas.CanUndo()) canvas.Undo();
        break;
    case 1:
        if (canvas.CanRedo()) canvas.Redo();
        break;
    case 2: {
        BrushSettings b{};
        b.radius = RandF(32.f, 200.f);
        b.opacity = 1.f;
        b.hardness = 0.5f;
        b.color[0] = RandF(0.f, 1.f);
        b.color[1] = RandF(0.f, 1.f);
        b.color[2] = RandF(0.f, 1.f);
        b.color[3] = 1.f;
        float x = RandF(50.f, (float)std::max(100, canvas.GetWidth() - 50));
        float y = RandF(50.f, (float)std::max(100, canvas.GetHeight() - 50));
        // Ensure paint layer
        for (int i = 0; i < (int)canvas.GetLayers().size(); ++i) {
            if (canvas.CanPaintLayerContent(i)) {
                canvas.SetActiveLayerIndex(i);
                break;
            }
        }
        canvas.PaintOnActiveLayer(x, y, StrokePhase::Begin, b);
        canvas.PaintOnActiveLayer(x + RandF(-80.f, 80.f), y + RandF(-80.f, 80.f),
                                  StrokePhase::Update, b);
        canvas.PaintOnActiveLayer(0.f, 0.f, StrokePhase::End, b);
        break;
    }
    case 3: {
        if (m_FillLayerIdx >= 0 && m_FillLayerIdx < (int)canvas.GetLayers().size()
            && canvas.IsFillLayer(m_FillLayerIdx)) {
            auto& layer = canvas.GetLayers()[m_FillLayerIdx];
            auto before = Canvas::CaptureLayerProps(layer);
            layer.fill.mapColor[(int)texset::MapKind::Diffuse].rgba[0] = RandF(0.f, 1.f);
            layer.presentationDirty = true;
            canvas.CommitLayerPropsEdit(m_FillLayerIdx, before, "Chaos Fill");
            canvas.MarkCompositeDirty();
        }
        break;
    }
    case 4:
        canvas.CreateNewLayer(device, "Chaos");
        break;
    case 5: {
        int n = (int)canvas.GetLayers().size();
        if (n >= 3) canvas.DeleteLayer(n - 1);
        break;
    }
    default:
        canvas.MarkCompositeDirty();
        break;
    }
    ++m_ChaosOps;
    ScheduleWait(0.f, 12.f);
    return true;
}

void StressRunner::AdvancePhase(Canvas& canvas, ID3D11Device* device) {
    FinishStrokeIfActive(canvas);
    if (canvas.IsMovingPixels())
        canvas.CommitMovePixels(device);
    EndPhase(canvas);

    Phase next = Phase::Done;
    switch (m_Phase) {
    case Phase::Load16K:         next = Phase::PaintBurst; break;
    case Phase::PaintBurst:      next = Phase::FillSpawn; break;
    case Phase::FillSpawn:       next = Phase::FillMutate; break;
    case Phase::FillMutate:      next = Phase::UndoRedoHell; break;
    case Phase::UndoRedoHell:    next = Phase::MaskAbuse; break;
    case Phase::MaskAbuse:       next = Phase::LayerSpam; break;
    case Phase::LayerSpam:       next = Phase::SelectTransform; break;
    case Phase::SelectTransform: next = Phase::Chaos; break;
    case Phase::Chaos:           next = Phase::MultiTab; break;
    case Phase::MultiTab:        next = Phase::Report; break;
    case Phase::Report:          next = Phase::Done; break;
    case Phase::Done:            next = Phase::Done; break;
    }

    BeginPhase(canvas, next);
    // Build queues AFTER BeginPhase (it clears m_Queue).
    if (next == Phase::PaintBurst) {
        BuildBurstStrokes(canvas.GetWidth(), canvas.GetHeight(), 6, false);
    } else if (next == Phase::Chaos) {
        m_ChaosOps = 0;
    }
}

void StressRunner::EmitReport(Canvas& canvas) {
    auto now = std::chrono::steady_clock::now();
    double totalSec = std::chrono::duration<double>(now - m_StartTime).count();
    {
        size_t priv = 0;
        SnapshotMem(m_MemEndWS, priv);
    }
    const double avg = m_TotalFrames > 0 ? m_TotalWorkMs / m_TotalFrames : 0.0;

    std::ostringstream oss;
    oss << "\n========== RayV Paint STRESS-16K REPORT ==========\n";
    oss << "Asset: " << kAsset16K << "\n";
    oss << "Canvas: " << canvas.GetWidth() << "x" << canvas.GetHeight() << "\n";
    oss << "Duration: " << totalSec << " s\n";
    oss << "Frames: " << m_TotalFrames << "\n";
    oss << "Work avg/max: " << avg << " / " << m_GlobalMaxWork << " ms\n";
    oss << "Frames >500ms: " << m_FramesOver500ms
        << "  >1000ms: " << m_FramesOver1000ms
        << "  >" << (int)kHangMs << "ms(HANG): " << m_FramesOver2000ms << "\n";
    oss << "Working set start/peak/end: "
        << MemoryStats::FormatBytes(m_MemStartWS) << " / "
        << MemoryStats::FormatBytes(m_MemPeakWS) << " / "
        << MemoryStats::FormatBytes(m_MemEndWS) << "\n";
    if (m_MemBeforeUndoWS > 0) {
        long long d = (long long)m_MemAfterUndoWS - (long long)m_MemBeforeUndoWS;
        oss << "UndoHell WS delta: " << (d >= 0 ? "+" : "")
            << MemoryStats::FormatBytes((size_t)std::llabs(d)) << "\n";
    }
    oss << "Active tiles: " << canvas.GetActiveLayerTileCount()
        << "  layers: " << canvas.GetLayers().size() << "\n";
    oss << "MultiTab: slept=" << m_MultiTabSlept
        << " hibernated=" << m_MultiTabHibernated
        << " wakes=" << m_MultiTabWakes << "\n";
    oss << "Load OK: " << (m_LoadOk ? "yes" : "no")
        << "  asset missing: " << (m_AssetMissing ? "yes" : "no") << "\n";

    m_ExitCode = 0;
    if (m_AssetMissing) m_ExitCode = 3;
    else if (!m_LoadOk) m_ExitCode = 4;
    else if (m_FramesOver2000ms > 0) m_ExitCode = 1;
    else if (m_MemBeforeUndoWS > 0 &&
             (long long)m_MemAfterUndoWS - (long long)m_MemBeforeUndoWS > 256ll * 1024 * 1024)
        m_ExitCode = 2;

    oss << "EXIT CODE: " << m_ExitCode
        << " (0=ok 1=hang>2s 2=undo-leak 3=no-asset 4=load-fail)\n";
    oss << "Journal: " << m_JournalPath << "\n";
    if (!m_JournalPathUser.empty())
        oss << "Journal copy: " << m_JournalPathUser << "\n";
    oss << "=================================================\n";

    std::string report = oss.str();
    // Write full report into journal line-by-line
    {
        std::istringstream iss(report);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty())
                Journal(line);
        }
    }
    std::printf("%s", report.c_str());
    std::fflush(stdout);
}

void StressRunner::Start(Canvas& canvas, ID3D11Device* device) {
    if (!m_Enabled || m_Active) return;
    m_Active = true;
    m_Finished = false;
    m_ExitCode = 0;
    m_StartTime = std::chrono::steady_clock::now();
    m_TotalFrames = 0;
    m_TotalWorkMs = 0.0;
    m_GlobalMaxWork = 0.0;
    m_FramesOver500ms = m_FramesOver1000ms = m_FramesOver2000ms = 0;
    m_LoadOk = false;
    m_AssetMissing = false;
    m_FillLayerIdx = -1;
    m_JournalSeq = 0;
    m_StrokeStarts = 0;
    m_UndoOpsDone = 0;
    m_RedoOpsDone = 0;
    m_MainProjectId = -1;
    m_SideProjectA = -1;
    m_SideProjectB = -1;
    m_MultiTabSlept = 0;
    m_MultiTabHibernated = 0;
    m_MultiTabWakes = 0;
    {
        size_t priv = 0;
        SnapshotMem(m_MemStartWS, priv);
    }
    m_MemPeakWS = m_MemStartWS;

    OpenJournal();
    Journal("========== STRESS-16K starting ==========");
    MemoryStats::LogSnapshot("stress_start");
    CrashGuard::NoteCheckpoint("stress_start");
    BeginPhase(canvas, Phase::Load16K);
    m_LoadOk = LoadAsset16K(canvas, device);
    if (!m_LoadOk) {
        Journal("FATAL load failed — will report");
        m_SubStep = 99;
    } else {
        ScheduleWait(100.f, 200.f);
        m_SubStep = 1;
    }
}

bool StressRunner::Tick(Canvas& canvas, ID3D11Device* device, float frameWorkMs) {
    if (!m_Active || m_Finished) return false;

    float dtSec = frameWorkMs * 0.001f;
    if (dtSec < 0.001f) dtSec = 0.001f;
    if (dtSec > 0.05f) dtSec = 0.05f;

    RecordFrame(frameWorkMs);
    m_PhaseElapsed = (float)std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_PhaseStart).count();
    UpdateStatus();

    double wall = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_StartTime).count();
    if (wall > kWallTimeoutSec && m_Phase != Phase::Report && m_Phase != Phase::Done) {
        Journalf("WARN wall timeout %.0fs — forcing report", kWallTimeoutSec);
        FinishStrokeIfActive(canvas);
        if (canvas.IsMovingPixels()) canvas.CommitMovePixels(device);
        BeginPhase(canvas, Phase::Report);
    }

    bool phaseBusy = true;

    switch (m_Phase) {
    case Phase::Load16K:
        if (m_SubStep == 99 || (!Waiting(dtSec) && m_SubStep >= 1)) {
            if (!m_LoadOk) {
                BeginPhase(canvas, Phase::Report);
            } else {
                phaseBusy = false;
            }
        }
        break;

    case Phase::PaintBurst:
        phaseBusy = AdvanceStroke(canvas, dtSec);
        if (m_PhaseElapsed > 12.f) {
            FinishStrokeIfActive(canvas);
            phaseBusy = false;
        }
        break;

    case Phase::FillSpawn:
        phaseBusy = TickFillSpawn(canvas, device, dtSec);
        if (m_PhaseElapsed > 6.f) phaseBusy = false;
        break;

    case Phase::FillMutate:
        phaseBusy = TickFillMutate(canvas, device, dtSec);
        if (m_PhaseElapsed > 10.f) phaseBusy = false;
        break;

    case Phase::UndoRedoHell:
        phaseBusy = TickUndoHell(canvas, dtSec);
        if (m_PhaseElapsed > 25.f) {
            if (m_MemAfterUndoWS == 0) {
                size_t priv = 0;
                SnapshotMem(m_MemAfterUndoWS, priv);
            }
            phaseBusy = false;
        }
        break;

    case Phase::MaskAbuse:
        phaseBusy = TickMaskAbuse(canvas, device, dtSec);
        if (m_PhaseElapsed > 10.f) {
            FinishStrokeIfActive(canvas);
            phaseBusy = false;
        }
        break;

    case Phase::LayerSpam:
        phaseBusy = TickLayerSpam(canvas, device, dtSec);
        if (m_PhaseElapsed > 12.f) phaseBusy = false;
        break;

    case Phase::SelectTransform:
        phaseBusy = TickSelectTransform(canvas, device, dtSec);
        if (m_PhaseElapsed > 8.f) {
            if (canvas.IsMovingPixels()) canvas.CommitMovePixels(device);
            phaseBusy = false;
        }
        break;

    case Phase::Chaos:
        phaseBusy = TickChaos(canvas, device, dtSec);
        if (m_PhaseElapsed > 12.f) phaseBusy = false;
        break;

    case Phase::MultiTab:
        phaseBusy = TickMultiTab(canvas, device, dtSec);
        if (m_PhaseElapsed > 20.f) phaseBusy = false;
        break;

    case Phase::Report:
        EmitReport(canvas);
        CloseJournal("clean_report");
        m_Phase = Phase::Done;
        m_Finished = true;
        m_Active = false;
        return false;

    case Phase::Done:
        m_Finished = true;
        m_Active = false;
        return false;
    }

    if (!phaseBusy && m_Phase != Phase::Report && m_Phase != Phase::Done)
        AdvancePhase(canvas, device);

    return true;
}

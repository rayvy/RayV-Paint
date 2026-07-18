#pragma once

#include "PaintEngine.h"

#include <d3d11.h>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

class Canvas;

// Hellish reliability stress on real 16K assets.
// Goal: no crash, no single-frame hang > 2s, abusive fill + undo/redo.
// Launch: RayVPaint.exe --stress-16k
// Live journal: testfield/stress_journal.txt (+ user/stress_last.txt)
// See plans/EMERGENCY_SAFE_PLAN.MD
class StressRunner {
public:
    static StressRunner& Get();

    void Enable(bool on) { m_Enabled = on; }
    bool IsEnabled() const { return m_Enabled; }
    bool IsActive() const { return m_Active; }
    bool IsFinished() const { return m_Finished; }
    int  ExitCode() const { return m_ExitCode; }

    // Absolute or relative UTF-8 path to the session journal (set after Start).
    const std::string& JournalPath() const { return m_JournalPath; }

    void Start(Canvas& canvas, ID3D11Device* device);
    // Returns false when finished (caller should exit main loop).
    bool Tick(Canvas& canvas, ID3D11Device* device, float frameWorkMs);

    const char* StatusLine() const { return m_StatusLine.c_str(); }

private:
    StressRunner() = default;

    enum class Phase : int {
        Load16K = 0,
        PaintBurst,
        FillSpawn,
        FillMutate,
        UndoRedoHell,
        MaskAbuse,
        LayerSpam,
        SelectTransform,
        Chaos,
        Report,
        Done
    };

    struct StrokePoint { float x = 0.f, y = 0.f; };
    struct PathStroke {
        std::vector<StrokePoint> pts;
        bool erase = false;
        float radius = 400.f;
        float hardness = 0.5f;
        float opacity = 0.9f;
        float color[4] = {1.f, 0.2f, 0.1f, 1.f};
        float speedPxS = 8000.f;
    };

    void OpenJournal();
    void Journal(const std::string& line);
    void Journalf(const char* fmt, ...);
    void CloseJournal(const char* reason);

    void BeginPhase(Canvas& canvas, Phase phase);
    void EndPhase(Canvas& canvas);
    void AdvancePhase(Canvas& canvas, ID3D11Device* device);
    void EmitReport(Canvas& canvas);
    void UpdateStatus();
    void RecordFrame(float workMs);
    void SnapshotMem(size_t& ws, size_t& priv) const;
    void ScheduleWait(float minMs, float maxMs);
    bool Waiting(float dtSec);
    float RandF(float a, float b);
    int   RandI(int a, int bInclusive);

    bool LoadAsset16K(Canvas& canvas, ID3D11Device* device);
    void BuildBurstStrokes(int w, int h, int count, bool erase);
    void EnsureStrokeStarted(Canvas& canvas);
    bool AdvanceStroke(Canvas& canvas, float dtSec);
    void FinishStrokeIfActive(Canvas& canvas);

    bool TickFillSpawn(Canvas& canvas, ID3D11Device* device, float dtSec);
    bool TickFillMutate(Canvas& canvas, ID3D11Device* device, float dtSec);
    bool TickUndoHell(Canvas& canvas, float dtSec);
    bool TickMaskAbuse(Canvas& canvas, ID3D11Device* device, float dtSec);
    bool TickLayerSpam(Canvas& canvas, ID3D11Device* device, float dtSec);
    bool TickSelectTransform(Canvas& canvas, ID3D11Device* device, float dtSec);
    bool TickChaos(Canvas& canvas, ID3D11Device* device, float dtSec);

    bool m_Enabled = false;
    bool m_Active = false;
    bool m_Finished = false;
    int  m_ExitCode = 0;

    Phase m_Phase = Phase::Load16K;
    int   m_SubStep = 0;
    float m_WaitLeft = 0.f;
    float m_PhaseElapsed = 0.f;

    std::vector<PathStroke> m_Queue;
    size_t m_QueueIdx = 0;
    size_t m_PathIdx = 0;
    float  m_PathT = 0.f;
    bool   m_StrokeLive = false;
    BrushSettings m_StrokeBrush{};

    int m_UndoLeft = 0;
    int m_RedoLeft = 0;
    bool m_UndoPass = true;
    int m_ChaosOps = 0;
    int m_FillLayerIdx = -1;
    int m_MoveDx = 0, m_MoveDy = 0;
    float m_TransformT = 0.f;
    int m_UndoOpsDone = 0;
    int m_RedoOpsDone = 0;
    int m_StrokeStarts = 0;

    std::mt19937 m_Rng{std::random_device{}()};
    std::chrono::steady_clock::time_point m_StartTime{};
    std::chrono::steady_clock::time_point m_PhaseStart{};

    int m_TotalFrames = 0;
    double m_TotalWorkMs = 0.0;
    double m_GlobalMaxWork = 0.0;
    int m_FramesOver500ms = 0;
    int m_FramesOver1000ms = 0;
    int m_FramesOver2000ms = 0;
    size_t m_MemStartWS = 0;
    size_t m_MemPeakWS = 0;
    size_t m_MemEndWS = 0;
    size_t m_MemBeforeUndoWS = 0;
    size_t m_MemAfterUndoWS = 0;
    bool m_LoadOk = false;
    bool m_AssetMissing = false;

    std::string m_StatusLine = "Stress: idle";
    std::string m_PhaseName;
    std::string m_JournalPath;
    std::string m_JournalPathUser;
    int m_JournalSeq = 0;

    // Hard fail: any frame work above this = hang (EMERGENCY_SAFE_PLAN)
    static constexpr float kHangMs = 2000.f;
    // Whole-run ceiling so a stuck phase cannot lock CI forever
    static constexpr double kWallTimeoutSec = 180.0;
    static constexpr const char* kAsset16K = "testfield/16Ktest.dds";
    static constexpr const char* kJournalRel = "testfield/stress_journal.txt";
};

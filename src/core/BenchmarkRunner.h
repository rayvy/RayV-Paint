#pragma once

#include "PaintEngine.h"

#include <d3d11.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

class Canvas;

// Interactive stress-test: wide human-like strokes, erase, select/move/transform,
// layer ops, blur filter, undo/redo. Runs across real frames so FPS/memory matter.
//
// Launch: rayvpaint.exe --benchmark
// Typical duration: ~25–40 s. Auto-exits with a metrics report.
class BenchmarkRunner {
public:
    static BenchmarkRunner& Get();

    void Enable(bool on) { m_Enabled = on; }
    bool IsEnabled() const { return m_Enabled; }
    bool IsActive() const { return m_Active; }
    bool IsFinished() const { return m_Finished; }
    int  ExitCode() const { return m_ExitCode; }

    // Call once after ProjectManager + D3D are ready.
    void Start(Canvas& canvas, ID3D11Device* device);

    // Once per main-loop frame (after UI, before Present).
    // frameWorkMs: CPU/GPU work this frame excluding VSync wait if known; else full frame.
    // Returns false when benchmark is done (caller should exit).
    bool Tick(Canvas& canvas, ID3D11Device* device, float frameWorkMs);

    // Overlay label for viewport HUD.
    const char* StatusLine() const { return m_StatusLine.c_str(); }

private:
    BenchmarkRunner() = default;

    enum class Phase : int {
        Setup = 0,
        PaintCover,
        EraseCover,
        SelectMoveTransform,
        ColorAndOperator,
        SecondLayerBlur,
        PaintOnLayer2,
        UndoRedoStress,
        PaintAndMove,
        DeleteLayerUndo,
        FinalPaint,
        Report,
        Done
    };

    struct StrokePoint {
        float x = 0.f;
        float y = 0.f;
    };

    struct PathStroke {
        std::vector<StrokePoint> pts;
        bool erase = false;
        float radius = 280.f;
        float hardness = 0.55f;
        float opacity = 0.92f;
        float color[4] = {0.85f, 0.2f, 0.15f, 1.f};
        // Simulated pen speed (px/s) with mild per-stroke variance.
        float speedPxS = 1600.f;
    };

    // Metrics
    struct PhaseStats {
        std::string name;
        double durationMs = 0.0;
        int frames = 0;
        double workMsSum = 0.0;
        double workMsMin = 1e9;
        double workMsMax = 0.0;
        double workMsP95 = 0.0;
        size_t memWorkingSetStart = 0;
        size_t memWorkingSetEnd = 0;
        size_t memPrivateStart = 0;
        size_t memPrivateEnd = 0;
        size_t tilesStart = 0;
        size_t tilesEnd = 0;
        std::vector<float> workSamples; // for p95
    };

    void BeginPhase(Canvas& canvas, Phase phase);
    void EndPhase(Canvas& canvas);
    void AdvancePhase(Canvas& canvas, ID3D11Device* device);

    void SetupCanvas8K(Canvas& canvas, ID3D11Device* device);
    void BuildPaintCoverStrokes(int w, int h);
    void BuildEraseStrokes(int w, int h);
    void BuildFreehandStrokes(int w, int h, int count, bool erase, float radiusMul);

    // Active stroke playback (Begin → Updates → End across frames).
    void EnsureStrokeStarted(Canvas& canvas);
    bool AdvanceStroke(Canvas& canvas, float dtSec);
    void FinishStrokeIfActive(Canvas& canvas);

    // Discrete (non-stroke) phase steps with human-ish waits.
    bool TickSelectMoveTransform(Canvas& canvas, ID3D11Device* device, float dtSec);
    bool TickColorAndOperator(Canvas& canvas, ID3D11Device* device, float dtSec);
    bool TickSecondLayerBlur(Canvas& canvas, ID3D11Device* device, float dtSec);
    bool TickUndoRedo(Canvas& canvas, float dtSec);
    bool TickPaintAndMove(Canvas& canvas, ID3D11Device* device, float dtSec);
    bool TickDeleteLayerUndo(Canvas& canvas, ID3D11Device* device, float dtSec);

    PathStroke MakeSerpentine(int w, int h, float y0, float yStep, float amp,
                              float radius, bool erase, const float color[4]);
    PathStroke MakeDiagonal(int w, int h, float t0, float radius, bool erase,
                            const float color[4]);
    PathStroke MakeFigureEight(int w, int h, float cx, float cy, float rx, float ry,
                               float radius, bool erase, const float color[4]);
    PathStroke MakeWavyFreehand(int w, int h, float x0, float y0, float x1, float y1,
                                int samples, float radius, bool erase, const float color[4]);

    float RandF(float a, float b);
    int   RandI(int a, int bInclusive);
    void  ScheduleWait(float minMs, float maxMs);
    bool  Waiting(float dtSec);
    void  RecordFrame(float workMs);
    void  SnapshotMem(size_t& ws, size_t& priv) const;
    size_t ActiveTiles(const Canvas& canvas) const;
    void  UpdateStatus();
    void  EmitReport(Canvas& canvas);
    void  AddBlurFilter(Canvas& canvas, float radius, const char* undoName);
    void  AddOutlineStyle(Canvas& canvas);

    bool m_Enabled = false;
    bool m_Active = false;
    bool m_Finished = false;
    int  m_ExitCode = 0;

    Phase m_Phase = Phase::Setup;
    int   m_SubStep = 0;
    float m_WaitLeft = 0.f;
    float m_PhaseElapsed = 0.f;

    std::vector<PathStroke> m_Queue;
    size_t m_QueueIdx = 0;
    size_t m_PathIdx = 0;
    float  m_PathT = 0.f; // residual distance along segment
    bool   m_StrokeLive = false;
    BrushSettings m_StrokeBrush{};

    // Move / transform scratch
    int m_MoveDx = 0, m_MoveDy = 0;
    float m_TransformT = 0.f;
    float m_MoveDur = 1.1f;
    float m_TfDur = 1.0f;

    // Undo/redo
    int m_UndoLeft = 0;
    int m_RedoLeft = 0;
    bool m_UndoPass = true;

    std::mt19937 m_Rng{std::random_device{}()};

    std::chrono::steady_clock::time_point m_StartTime{};
    std::chrono::steady_clock::time_point m_PhaseStart{};
    std::vector<PhaseStats> m_PhaseStats;
    PhaseStats m_CurStats{};

    // Global metrics
    int m_TotalFrames = 0;
    double m_TotalWorkMs = 0.0;
    double m_GlobalMinWork = 1e9;
    double m_GlobalMaxWork = 0.0;
    int m_FramesOver33ms = 0;
    int m_FramesOver50ms = 0;
    int m_FramesOver100ms = 0;
    size_t m_MemStartWS = 0;
    size_t m_MemPeakWS = 0;
    size_t m_MemEndWS = 0;
    size_t m_MemAfterUndoRedoWS = 0;
    size_t m_MemBeforeUndoRedoWS = 0;

    std::string m_StatusLine = "Benchmark: idle";
    std::string m_PhaseName;

    static constexpr int kCanvasSize = 8192;
};

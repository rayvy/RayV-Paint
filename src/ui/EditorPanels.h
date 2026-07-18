#pragma once
#define IMGUI_DEFINE_MATH_OPERATORS
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <d3d11.h>
#include <imgui.h>
#include "../Canvas.h"
#include "../core/PaintEngine.h"
#include "FileExplorer.h"

enum class ActiveTool {
    Brush, Eraser, Pan,
    RectSelect, EllipseSelect, LassoSelect, PolygonalLasso,
    QuickSelect, MagicWand, SmartSelect,
    MovePixels, Pipette, BucketFill, Gradient, Smudge, BlurTool, Stamp,
    // Vector (Krita-like shape layer tools)
    VectorSelect, VectorEdit, VectorPen, VectorRect, VectorEllipse, VectorLine,
    VectorFreehand, VectorPolygon
};

#include <GLFW/glfw3.h>

namespace UI {

    // Shared style for vector creation tools (UI / main.cpp).
    struct VectorToolStyle {
        float fillRgba[4] = {0.25f, 0.55f, 0.95f, 1.f};
        float strokeRgba[4] = {0.08f, 0.08f, 0.1f, 1.f};
        float strokeWidth = 2.f;
        bool fillEnabled = true;
        bool strokeEnabled = true;
        bool freehandClosed = false; // freehand closes path on release
        bool scaleStyles = true;     // scale stroke when resizing selection
        float rectCornerRx = 0.f;    // default rounded-rect radii for new rects
        float rectCornerRy = 0.f;
        bool polygonClosed = true;   // polygon vs polyline for polygon tool
        // Advanced style defaults for new shapes / Apply style
        bool fillLinearGrad = false;
        float gradRgba1[4] = {0.95f, 0.35f, 0.55f, 1.f};
        float dashLen = 0.f;
        float gapLen = 0.f;
    };
    extern VectorToolStyle g_VectorToolStyle;

    inline bool IsVectorTool(ActiveTool t) {
        return t == ActiveTool::VectorSelect || t == ActiveTool::VectorEdit ||
               t == ActiveTool::VectorPen || t == ActiveTool::VectorRect ||
               t == ActiveTool::VectorEllipse || t == ActiveTool::VectorLine ||
               t == ActiveTool::VectorFreehand || t == ActiveTool::VectorPolygon;
    }

    struct DocumentLoadingState {
        std::atomic<bool> isLoading{false};
        std::atomic<float> progress{0.0f};
        std::mutex mutex;
        std::string stage;
        std::string filepath;
        bool success = false;
        std::atomic<bool> completed{false};
    };
    extern DocumentLoadingState g_LoadingState;

    void TriggerBackgroundOpenDocument(const std::string& filepath, ID3D11Device* device, Canvas& canvas);

    // Project tabs (header): close request when dirty — main/UI handles confirm.
    struct ProjectTabCloseRequest {
        int projectId = -1;
        bool pending = false;
    };
    extern ProjectTabCloseRequest g_ProjectTabCloseRequest;

    bool IsSelectTool(ActiveTool tool);
    bool IsLassoTool(ActiveTool tool);
    bool IsWandTool(ActiveTool tool);
    ActiveTool CycleSelectTool(ActiveTool current);
    ActiveTool CycleLassoTool(ActiveTool current);
    ActiveTool CycleWandTool(ActiveTool current);
    void SampleCanvasColor(Canvas& canvas, float canvasX, float canvasY, float outColor[4]);

    // Fill Layer color picker → sample next canvas click into armed map slot.
    bool IsFillPipetteArmed();
    void ArmFillPipette(int layerIdx, int mapIdx);
    // mapIdx < 0 → any map on that layer
    bool FillPipetteArmedFor(int layerIdx, int mapIdx);
    // If armed and LMB on canvas: write sample into fill.mapColor[map] and clear arm. Returns true if consumed.
    bool TryApplyFillPipette(Canvas& canvas, float canvasX, float canvasY);

    // Brush preset popup (RMB in viewport). Call every frame; open via openFlag/pos.
    void DrawBrushPickerPopup(bool& openFlag, ImVec2 popupPos, BrushSettings& brush);

    struct UIState {
        // Window visibility flags
        bool showConsole = true;
        bool showProperties = true;
        bool showViewportNav = true;   // Stage 2c — zoom/pan/flip/rot
        bool showLayerEffects = false; // Stage 2c — FX panel for active layer
        bool showLayers = true;
        bool showAssetBrowser = false;
        bool showChannels = true;
        bool showToolbar = true;
        bool showColors = true;
        bool showToolSettings = true;
        bool showContextDebug = false; // AppContext live dump (footer Context button)
        bool showRulers = true;
        bool showPreview3D = false;    // optional detachable 3D viewport (N-panel)
        bool showModSetup = false;     // INI/dump/semantics launcher (separate from Properties)
        bool preview3DNeedReload = false;
        // Channels panel: map import helpers
        int  importMapKind = 1; // default LightMap
        int  importMapSoloRole = 0; // 0=Full RGBA
        bool openAboutModal = false;
        bool openNewProjectWizard = false;
        bool openProjectSetup = false;   // one-shot trigger → opens showProjectSetup
        bool showProjectSetup = false;   // non-modal window (so File Explorer can open on top)
        int  projectSetupTab = 0; // 0 Maps 1 Labels 2 Export
        FileExplorerState fileExplorer;
        // Layer Effects modal selection: kind 0=style, 1=filter, -1=none
        int  layerEffectsFocusIdx = -1;
        int  layerEffectsSelKind = -1; // 0 style, 1 filter, -1 none
        int  layerEffectsSelIdx = -1;
        int  layerPreviewRefreshFrames = 0; // double-refresh after paint/edit

        // Modal visibility triggers
        bool openImportModal = false;
        bool openExportDdsModal = false;
        bool openExportStdModal = false;
        bool openExportAdvancedModal = false;
        bool openQuickExportTrigger = false;
        bool openSettingsModal = false;
        bool openSaveRaypModal = false;
        bool openLoadRaypModal = false;
        bool openCanvasSizeModal = false;
        bool openLoadConfigModal = false;
        bool openSaveConfigModal = false;
        bool showRecoveryModal = false;
        bool showRecentAutosaves = false; // cold start: recent autosaves picker

        // Move / Free Transform action signals
        bool commitTransform = false;
        bool cancelTransform = false;
        // Image → Free Transform… (Ctrl+T also); main loop enters Free Transform mode
        bool requestFreeTransform = false;
        bool requestPerspectiveWarp = false;
        bool requestMeshWarp = false;
        bool requestContentAwareFill = false;
        // Written by main: true while Ctrl+T Free Transform session is active
        bool freeTransformActive = false;

        // Rebinding helper state
        std::string rebindingAction = "";
        bool listeningForKey = false;
        
        // Settings cached values
        std::string activeTheme = "Dark";
        bool settingsInitialized = false;
        int defW = 1024;
        int defH = 1024;
        std::string backupDir = "";
        int autoSaveMins = 3;
        int autosaveMaxPerProject = 5;
        int maxUndo = 100;
        int maxUndoMem = 512;
        float maxBrushRadius = 250.f;

        // Per-tool settings
        float magicWandTolerance = 0.15f;
        bool  magicWandContiguous = true;
        float bucketFillTolerance = 0.15f;

        // Smudge / Blur tool settings (shared shape: radius, strength, spacing)
        SmudgeSettings smudge;
        SmudgeSettings blurTool{ 20.f, 0.5f, 0.15f };

        // Brush tip preset: 0=Soft, 1=Hard, 2=Pencil, 3=Airbrush, 4=Custom (or procedural null)
        int brushTipPreset = 0;
        bool hasCustomBrushTip = false;
        std::string customBrushTipName;

        // Layers multi-select (UI); paint target remains canvas.GetActiveLayerIndex()
        std::vector<int> selectedLayers;
        int layerSelectAnchor = -1;

        // Image adjustment modals
        bool showBlurModal    = false;
        bool showHSVModal     = false;
        bool showCurvesModal  = false;
        bool showNoiseModal   = false;
        float blurRadius      = 5.0f;
        float hsvH = 0.0f, hsvS = 0.0f, hsvV = 0.0f;
        float noiseStrength   = 0.1f;
        bool  noiseColor      = false;
        // Curves: control points for spline editor [{x,y} in [0,1]]
        std::vector<std::pair<float,float>> curvesPointsRGB;
        std::vector<float> curvesLUTRGB;
        std::vector<std::pair<float,float>> curvesPointsAlpha;
        std::vector<float> curvesLUTAlpha;
        int   curvesChannel   = 0; // 0 = RGB, 1 = Alpha

        // Recovery path
        std::string backupPath = "";

        // Frame timing trackers
        float frameTimeMs = 0.0f;
        float fps = 0.0f;
        double startupTimeMs = 0.0f;
    };

    void RenderAll(UIState& state, Canvas& canvas, BrushSettings& brush, ActiveTool& activeTool, ID3D11Device* device, ID3D11DeviceContext* context, GLFWwindow* window);
}

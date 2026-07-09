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

enum class ActiveTool { Brush, Eraser, Pan, RectSelect, EllipseSelect, LassoSelect, PolygonalLasso, QuickSelect, MagicWand, SmartSelect, MovePixels, Pipette, BucketFill, Gradient, Smudge };


#include <GLFW/glfw3.h>

namespace UI {

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

    bool IsSelectTool(ActiveTool tool);
    bool IsLassoTool(ActiveTool tool);
    bool IsWandTool(ActiveTool tool);
    ActiveTool CycleSelectTool(ActiveTool current);
    ActiveTool CycleLassoTool(ActiveTool current);
    ActiveTool CycleWandTool(ActiveTool current);
    void SampleCanvasColor(Canvas& canvas, float canvasX, float canvasY, float outColor[4]);

    // Brush preset popup (RMB in viewport). Call every frame; open via openFlag/pos.
    void DrawBrushPickerPopup(bool& openFlag, ImVec2 popupPos, BrushSettings& brush);

    struct UIState {
        // Window visibility flags
        bool showConsole = true;
        bool showProperties = true;
        bool showViewportNav = true;   // Stage 2c — zoom/pan/flip/rot
        bool showLayerEffects = false; // Stage 2c — FX panel for active layer
        bool showLayers = true;
        bool showChannels = true;
        bool showToolbar = true;
        bool showColors = true;
        bool showToolSettings = true;
        bool showRulers = true;
        bool openAboutModal = false;
        int  layerEffectsFocusIdx = -1; // selected FX in Layer Effects panel
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

        // Transform tool action signals
        bool commitTransform = false;
        bool cancelTransform = false;

        // Rebinding helper state
        std::string rebindingAction = "";
        bool listeningForKey = false;
        
        // Settings cached values
        std::string activeTheme = "Dark";
        bool settingsInitialized = false;
        int defW = 1024;
        int defH = 1024;
        std::string backupDir = "";
        int autoSaveMins = 5;
        int maxUndo = 100;
        int maxUndoMem = 512;
        float maxBrushRadius = 250.f;

        // Per-tool settings
        float magicWandTolerance = 0.15f;
        bool  magicWandContiguous = true;
        float bucketFillTolerance = 0.15f;

        // Smudge tool settings
        SmudgeSettings smudge;

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

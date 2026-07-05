#pragma once
#define IMGUI_DEFINE_MATH_OPERATORS
#include <string>
#include <vector>
#include <d3d11.h>
#include <imgui.h>
#include "../Canvas.h"
#include "../core/PaintEngine.h"

enum class ActiveTool { Brush, Eraser, Pan, Rotate };

#include <GLFW/glfw3.h>

namespace UI {

    struct UIState {
        // Window visibility flags
        bool showConsole = true;
        bool showProperties = true;
        bool showLayers = true;
        bool showToolbar = true;
        bool showColors = true;
        bool showBrushSettings = true;

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

        // Recovery path
        std::string backupPath = "";

        // Frame timing trackers
        float frameTimeMs = 0.0f;
        float fps = 0.0f;
        double startupTimeMs = 0.0f;
    };

    void RenderAll(UIState& state, Canvas& canvas, BrushSettings& brush, ActiveTool& activeTool, ID3D11Device* device, ID3D11DeviceContext* context, GLFWwindow* window);
}

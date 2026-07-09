#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <io.h>
#include <filesystem>

// GLFW
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

// Dear ImGui
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_dx11.h"

// Project Core
#include "Logger.h"
#include "ThreadPool.h"
#include "ConfigManager.h"
#include "ScriptingEngine.h"
#include "Canvas.h"
#include "core/MemoryStats.h"
#include "core/KeymapManager.h"
#include "core/ClipboardHelper.h"
#include "core/BrushLibrary.h"
#include "core/PathUtil.h"
#include "ui/EditorPanels.h"
#include "ui/style/UiTokens.h"

// Tablet Pointer API support
float g_PenPressure = 1.0f;
static bool g_IsPenActive = false;
static WNDPROC g_OriginalWndProc = nullptr;

LRESULT CALLBACK SubclassedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_LBUTTONDOWN:
        case WM_MOUSEMOVE: {
            // Only reset if this is a genuine mouse event, not a synthesized one from a pen/tablet
            if ((GetMessageExtraInfo() & 0xFFFFFF00) != 0xFF515700) {
                g_PenPressure = 1.0f;
                g_IsPenActive = false;
            }
            break;
        }
        case WM_POINTERDOWN:
        case WM_POINTERUPDATE: {
            UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
            POINTER_INPUT_TYPE pointerType = PT_POINTER;
            if (GetPointerType(pointerId, &pointerType)) {
                if (pointerType == PT_PEN) {
                    POINTER_PEN_INFO penInfo;
                    if (GetPointerPenInfo(pointerId, &penInfo)) {
                        g_PenPressure = (float)penInfo.pressure / 1024.0f;
                        g_IsPenActive = true;
                    }
                } else {
                    g_PenPressure = 1.0f;
                    g_IsPenActive = false;
                }
            }
            break;
        }
        case WM_POINTERUP: {
            g_PenPressure = 1.0f;
            g_IsPenActive = false;
            break;
        }
    }
    return CallWindowProc(g_OriginalWndProc, hWnd, uMsg, wParam, lParam);
}

// Chained GLFW Key Callback for Layout-Independent OEM Shortcuts
static GLFWkeyfun g_PrevKeyCallback = nullptr;
static void CustomKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    KeymapManager::Get().ProcessKeyEvent(key, scancode, action, mods);
    if (g_PrevKeyCallback) {
        g_PrevKeyCallback(window, key, scancode, action, mods);
    }
}

bool g_IsViewportHovered = false;
bool g_IsLayersHovered = false;



// DirectX 11 Global Objects
static ID3D11Device*           g_pd3dDevice = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*         g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// Canvas Render-to-Texture Objects
static ID3D11Texture2D*          g_canvasTexture = nullptr;
static ID3D11RenderTargetView*   g_canvasRTV = nullptr;
static ID3D11ShaderResourceView* g_canvasSRV = nullptr;
static float                     g_canvasRTWidth = 0.0f;
static float                     g_canvasRTHeight = 0.0f;

// Canvas Instance
static Canvas g_Canvas;
static ImVec2 g_LastDragDelta = ImVec2(0.0f, 0.0f);

// Global startup time
static double g_StartupTimeMs = 0.0;

// Painting state
static ActiveTool g_ActiveTool = ActiveTool::Brush;
static ActiveTool g_LastSelectTool = ActiveTool::RectSelect;
static ActiveTool g_LastLassoTool = ActiveTool::LassoSelect;
static ActiveTool g_LastWandTool = ActiveTool::MagicWand;
static BrushSettings g_Brush;
float g_SecondaryColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
// Color swap animation 0→1 (toolbar + Colors); bumped on X / chip click
float g_ColorSwapAnim = 0.f;
bool g_ColorSwapPending = false;
static bool g_IsPainting = false;
static ActiveTool g_PrevActiveTool = ActiveTool::Brush;

static int g_LayerPreviewRefreshFrames = 0;

static void EndBrushStrokeIfNeeded() {
    if (g_IsPainting) {
        g_Canvas.PaintOnActiveLayer(0, 0, StrokePhase::End, g_Brush);
        g_IsPainting = false;
        // Double-refresh layer thumbs (preview can lag one frame behind GPU upload)
        g_LayerPreviewRefreshFrames = 2;
        g_Canvas.MarkCompositeDirty();
    }
}

// Selection dragging state
static bool g_IsSelectionDragging = false;
static float g_SelectionDragStartX = 0.0f;
static float g_SelectionDragStartY = 0.0f;
static std::vector<std::pair<int, int>> g_LassoPoints;
static std::vector<std::pair<int, int>> g_PolygonalLassoPoints;

static bool g_IsMoveDragging = false;
static float g_MoveDragStartX = 0.0f;
static float g_MoveDragStartY = 0.0f;
static int g_MoveAccumulatedOffsetX = 0;
static int g_MoveAccumulatedOffsetY = 0;
static bool g_IsGradientDragging = false;

enum class TransformGizmoHandle {
    None,
    Move,
    Scale_TL, Scale_TR, Scale_BR, Scale_BL,
    Scale_T, Scale_R, Scale_B, Scale_L,
    Rotate
};

static TransformGizmoHandle g_ActiveGizmoHandle = TransformGizmoHandle::None;
static float g_GizmoDragStartScaleX = 1.0f;
static float g_GizmoDragStartScaleY = 1.0f;
static float g_GizmoDragStartRotation = 0.0f;
static float g_GizmoDragStartMouseAngle = 0.0f;
static float g_GizmoDragStartDist = 1.0f;

struct GizmoScreenGeometry {
    ImVec2 p1, p2, p3, p4;
    ImVec2 mT, mR, mB, mL;
    ImVec2 center;
};

static inline float distSq(ImVec2 a, ImVec2 b) {
    return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y);
}

static inline float crossProduct(ImVec2 a, ImVec2 b, ImVec2 p) {
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

static inline bool IsPointInsideQuad(ImVec2 p, ImVec2 p1, ImVec2 p2, ImVec2 p3, ImVec2 p4) {
    float d1 = crossProduct(p1, p2, p);
    float d2 = crossProduct(p2, p3, p);
    float d3 = crossProduct(p3, p4, p);
    float d4 = crossProduct(p4, p1, p);
    
    bool has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0) || (d4 < 0);
    bool has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0) || (d4 > 0);
    
    return !(has_neg && has_pos);
}

static bool GetGizmoGeometry(Canvas& canvas, const std::function<ImVec2(float, float)>& canvasToScreen, GizmoScreenGeometry& outGeo) {
    int minX = 0, minY = 0, minW = 0, minH = 0;
    if (!canvas.GetFloatingBBox(minX, minY, minW, minH)) {
        return false;
    }
    int maxX = minX + minW - 1;
    int maxY = minY + minH - 1;

    float cx = (minX + maxX) * 0.5f;
    float cy = (minY + maxY) * 0.5f;
    float cosA = std::cos(canvas.GetFloatingRotation());
    float sinA = std::sin(canvas.GetFloatingRotation());

    auto transformCorner = [&](float px, float py) -> ImVec2 {
        float rx = (px - cx) * canvas.GetFloatingScaleX();
        float ry = (py - cy) * canvas.GetFloatingScaleY();
        float tx = rx * cosA - ry * sinA + cx + (float)canvas.GetFloatingOffsetX();
        float ty = rx * sinA + ry * cosA + cy + (float)canvas.GetFloatingOffsetY();
        return canvasToScreen(tx, ty);
    };

    outGeo.p1 = transformCorner((float)minX, (float)minY);
    outGeo.p2 = transformCorner((float)maxX, (float)minY);
    outGeo.p3 = transformCorner((float)maxX, (float)maxY);
    outGeo.p4 = transformCorner((float)minX, (float)maxY);

    outGeo.mT = ImVec2((outGeo.p1.x + outGeo.p2.x) * 0.5f, (outGeo.p1.y + outGeo.p2.y) * 0.5f);
    outGeo.mR = ImVec2((outGeo.p2.x + outGeo.p3.x) * 0.5f, (outGeo.p2.y + outGeo.p3.y) * 0.5f);
    outGeo.mB = ImVec2((outGeo.p3.x + outGeo.p4.x) * 0.5f, (outGeo.p3.y + outGeo.p4.y) * 0.5f);
    outGeo.mL = ImVec2((outGeo.p4.x + outGeo.p1.x) * 0.5f, (outGeo.p4.y + outGeo.p1.y) * 0.5f);
    
    outGeo.center = transformCorner(cx, cy);
    return true;
}
static void CustomDropCallback(GLFWwindow* window, int count, const char** paths) {
    if (count <= 0) return;
    // GLFW may hand UTF-8 or legacy ACP on Windows — normalize before any I/O.
    std::string path = PathUtil::NormalizeToUtf8Path(paths[0] ? paths[0] : "");

    size_t dot = path.find_last_of('.');
    std::string ext;
    if (dot != std::string::npos) {
        ext = path.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    // .rayp is always a full project document — never import as a flat image layer.
    if (ext == "rayp") {
        UI::TriggerBackgroundOpenDocument(path, g_pd3dDevice, g_Canvas);
        return;
    }

    if (g_IsViewportHovered || g_IsLayersHovered) {
        UI::TriggerBackgroundOpenDocument(path, g_pd3dDevice, g_Canvas);
    } else {
        g_Canvas.ClearUndoHistory();
        g_Canvas.GetLayers().clear();
        g_Canvas.SetActiveLayerIndex(-1);
        g_Canvas.SetCurrentProjectFilePath("");

        UI::TriggerBackgroundOpenDocument(path, g_pd3dDevice, g_Canvas);
    }
}

// Forward declarations
bool CreateDeviceD3D(HWND hWnd, bool useNullDriver = false);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
void ResizeCanvasRenderTarget(int width, int height);
void CleanupCanvasRenderTarget();
void RenderCanvasToTexture(int width, int height);
void RedirectIOToConsole();

// Core API bindings exported to Scripting Engine
void TriggerCanvasResize(int w, int h) {
    g_Canvas.ResizeCanvas(g_pd3dDevice, w, h);
    Logger::Get().Info("Canvas resized to: " + std::to_string(w) + "x" + std::to_string(h));
}
float GetCanvasZoom() { return g_Canvas.GetZoom(); }
void SetCanvasZoom(float zoom) { g_Canvas.SetZoom(zoom); }
void SetCanvasPan(float x, float y) { g_Canvas.SetPan(DirectX::XMFLOAT2(x, y)); }
void ResetCanvasView() { g_Canvas.ResetView(); }
bool LoadCanvasImage(const std::string& filepath) {
    // Python / automation: open documents by type (.rayp or image).
    return g_Canvas.OpenDocument(g_pd3dDevice, filepath);
}
int GetCanvasWidth() { return g_Canvas.GetWidth(); }
int GetCanvasHeight() { return g_Canvas.GetHeight(); }
size_t GetActiveLayerTileCount() { return g_Canvas.GetActiveLayerTileCount(); }
double GetProcessWorkingSetMiB() {
    auto info = MemoryStats::QueryProcess();
    return static_cast<double>(info.workingSetBytes) / (1024.0 * 1024.0);
}
bool SaveCanvasDDS(const std::string& filepath, int formatChoice) {
    DdsFormat fmt = DdsFormat::RGBA8_UNORM;
    if (formatChoice == 1) fmt = DdsFormat::RGBA32_FLOAT;
    else if (formatChoice == 2) fmt = DdsFormat::RGBA16_UNORM;
    else if (formatChoice == 3) fmt = DdsFormat::RGBA16_FLOAT;
    else if (formatChoice == 4) fmt = DdsFormat::R8_UNORM;
    else if (formatChoice == 5) fmt = DdsFormat::R16_FLOAT;
    else if (formatChoice == 6) fmt = DdsFormat::R32_FLOAT;
    return g_Canvas.SaveCanvas(filepath, fmt);
}
bool SaveCanvasStandard(const std::string& filepath, const std::string& iccProfilePath) {
    return g_Canvas.SaveCanvasStandard(filepath, iccProfilePath);
}


// Entry point helper to dynamically spawn or attach console
void SetupConsole(bool forceConsole) {
    if (forceConsole) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
            AllocConsole();
        }
        RedirectIOToConsole();
    }
}

void RedirectIOToConsole() {
    // Only rebind stdio to the console when a console exists and handles are
    // not already redirected (e.g. `exe > log.txt`). Rebinding over a pipe
    // breaks Python's sys.stdout init ("can't initialize sys standard streams").
    auto isConsoleHandle = [](DWORD id) -> bool {
        HANDLE h = GetStdHandle(id);
        if (!h || h == INVALID_HANDLE_VALUE) return false;
        DWORD mode = 0;
        return GetConsoleMode(h, &mode) != 0;
    };
    const bool haveConsoleOut = isConsoleHandle(STD_OUTPUT_HANDLE) || isConsoleHandle(STD_ERROR_HANDLE);

    FILE* fDummy = nullptr;
    if (haveConsoleOut || GetConsoleWindow() != nullptr) {
        if (!isConsoleHandle(STD_OUTPUT_HANDLE) && GetConsoleWindow()) {
            freopen_s(&fDummy, "CONOUT$", "w", stdout);
        }
        if (!isConsoleHandle(STD_ERROR_HANDLE) && GetConsoleWindow()) {
            freopen_s(&fDummy, "CONOUT$", "w", stderr);
        }
        if (!isConsoleHandle(STD_INPUT_HANDLE) && GetConsoleWindow()) {
            freopen_s(&fDummy, "CONIN$", "r", stdin);
        }
    }
    std::cout.clear();
    std::cerr.clear();
    std::cin.clear();
}

void ApplyTheme(const std::string& themeName) {
    ImGuiStyle& style = ImGui::GetStyle();

    // Stage 1 UI kit tokens (icon tint, elevated panels, motion-related radii)
    Ui::Tokens().ApplyFromThemeName(themeName);
    Ui::Tokens().ApplyToImGuiStyle(style);

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.ScrollbarSize = 13.0f;
    style.GrabMinSize = 10.0f;
    // Tooltips: only after ~1s stationary hover (pair with DelayNormal flags where used)
    style.HoverDelayNormal = Ui::Tokens().tooltipDelaySec;
    style.HoverDelayShort = Ui::Tokens().tooltipDelaySec;
    style.HoverStationaryDelay = 0.15f;

    if (themeName == "Classic") {
        ImGui::StyleColorsClassic();
        style.FrameRounding = 0.0f;
        style.WindowRounding = 0.0f;
        Ui::Tokens().ApplyDark(); // classic → light icons on dark chrome still
    } else if (themeName == "Light") {
        ImGui::StyleColorsLight();
        ImVec4* colors = style.Colors;
        colors[ImGuiCol_WindowBg]           = ImVec4(0.95f, 0.95f, 0.96f, 1.00f);
        colors[ImGuiCol_ChildBg]            = ImVec4(0.98f, 0.98f, 0.98f, 1.00f);
        colors[ImGuiCol_PopupBg]            = ImVec4(1.00f, 1.00f, 1.00f, 0.96f);
        colors[ImGuiCol_Border]             = ImVec4(0.80f, 0.80f, 0.85f, 1.00f);
        colors[ImGuiCol_FrameBg]            = ImVec4(0.88f, 0.88f, 0.90f, 1.00f);
        colors[ImGuiCol_FrameBgHovered]     = ImVec4(0.82f, 0.82f, 0.86f, 1.00f);
        colors[ImGuiCol_FrameBgActive]      = ImVec4(0.78f, 0.78f, 0.82f, 1.00f);
        colors[ImGuiCol_TitleBg]            = ImVec4(0.90f, 0.90f, 0.92f, 1.00f);
        colors[ImGuiCol_TitleBgActive]      = ImVec4(0.85f, 0.85f, 0.88f, 1.00f);
        colors[ImGuiCol_Button]             = ImVec4(0.80f, 0.80f, 0.85f, 1.00f);
        colors[ImGuiCol_ButtonHovered]      = ImVec4(0.26f, 0.38f, 0.70f, 0.80f);
        colors[ImGuiCol_ButtonActive]       = ImVec4(0.26f, 0.38f, 0.70f, 1.00f);
        colors[ImGuiCol_Header]             = ImVec4(0.85f, 0.85f, 0.90f, 1.00f);
        colors[ImGuiCol_HeaderHovered]      = ImVec4(0.26f, 0.38f, 0.70f, 0.70f);
        colors[ImGuiCol_HeaderActive]       = ImVec4(0.26f, 0.38f, 0.70f, 1.00f);
        colors[ImGuiCol_TabActive]          = ImVec4(0.88f, 0.88f, 0.92f, 1.00f);
    } else { // "Dark" - elevated Apple-like charcoal
        ImGui::StyleColorsDark();
        ImVec4* colors = style.Colors;
        const auto& T = Ui::Tokens();
        
        // Window & Child — slight transparency on popups for glass feel
        colors[ImGuiCol_WindowBg]               = T.bgWindow;
        colors[ImGuiCol_ChildBg]                = ImVec4(0.12f, 0.12f, 0.14f, 0.92f);
        colors[ImGuiCol_PopupBg]                = ImVec4(T.bgElevated.x, T.bgElevated.y, T.bgElevated.z, 0.94f);
        colors[ImGuiCol_Border]                 = T.strokeHairline;
        
        // Input Fields & Frames
        colors[ImGuiCol_FrameBg]                = ImVec4(0.15f, 0.15f, 0.17f, 1.00f);
        colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
        colors[ImGuiCol_FrameBgActive]          = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);
        
        // Title Bars
        colors[ImGuiCol_TitleBg]                = ImVec4(0.07f, 0.07f, 0.08f, 1.00f);
        colors[ImGuiCol_TitleBgActive]          = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.07f, 0.07f, 0.08f, 0.75f);
        
        // Headers & Trees
        colors[ImGuiCol_Header]                 = ImVec4(0.15f, 0.15f, 0.17f, 1.00f);
        colors[ImGuiCol_HeaderHovered]          = ImVec4(0.24f, 0.32f, 0.52f, 1.00f);
        colors[ImGuiCol_HeaderActive]           = ImVec4(0.26f, 0.38f, 0.70f, 1.00f);
        
        // Buttons
        colors[ImGuiCol_Button]                 = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
        colors[ImGuiCol_ButtonHovered]          = ImVec4(0.24f, 0.32f, 0.52f, 1.00f);
        colors[ImGuiCol_ButtonActive]           = ImVec4(0.26f, 0.38f, 0.70f, 1.00f);
        
        // Tabs
        colors[ImGuiCol_Tab]                    = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
        colors[ImGuiCol_TabHovered]             = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
        colors[ImGuiCol_TabActive]              = ImVec4(0.12f, 0.12f, 0.13f, 1.00f);
        colors[ImGuiCol_TabUnfocused]           = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
        colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.12f, 0.12f, 0.13f, 1.00f);
        
        // Scrollbar & Sliders
        colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.09f, 0.09f, 0.10f, 0.50f);
        colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.25f, 0.25f, 0.28f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.32f, 0.32f, 0.36f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.40f, 0.40f, 0.45f, 1.00f);
        colors[ImGuiCol_SliderGrab]             = ImVec4(0.26f, 0.38f, 0.70f, 0.90f);
        colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.26f, 0.38f, 0.70f, 1.00f);
        
        // Docking & Viewports
        colors[ImGuiCol_DockingPreview]         = ImVec4(0.26f, 0.38f, 0.70f, 0.70f);
        colors[ImGuiCol_DockingEmptyBg]         = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
    }
}




int main(int argc, char* argv[]) {
    auto startupStart = std::chrono::high_resolution_clock::now();

    // 1. CLI Arguments parsing
    bool testMode = false;
    bool headlessMode = false;
    bool testBrushes = false;
    bool forceConsole = false;
    bool test16kMode = false;
    std::string scriptPath = "";
    std::string configPath = "";
    std::string startupImagePath = "";
    int processExitCode = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--test") {
            testMode = true;
        } else if (arg == "--test-brushes") {
            testBrushes = true;
            forceConsole = true;
        } else if (arg == "--test-16k") {
            // Heavy large-texture suite (optional path after smoke).
            test16kMode = true;
            headlessMode = true;
            testMode = true;
            if (scriptPath.empty()) scriptPath = "test_16k.py";
        } else if (arg == "--headless") {
            headlessMode = true;
            testMode = true; // Headless implies auto-testing behavior
        } else if (arg == "--console") {
            forceConsole = true;
        } else if (arg == "--version") {
            SetupConsole(true);
            std::cout << "RayV Paint - Version 0.2.0" << std::endl;
            return 0;
        } else if (arg == "--script" && i + 1 < argc) {
            scriptPath = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg[0] != '-') {
            startupImagePath = arg;
        }
    }

    // Force console if in test mode or headless mode
    if (testMode || headlessMode) {
        forceConsole = true;
    }

    SetupConsole(forceConsole);

    // If configPath was not overridden by CLI, resolve to the user directory config
    if (configPath.empty()) {
        configPath = ConfigManager::GetUserSubdirectory("user") + "/config.json";
    }

    // 2. Initialize Core Logging & Configuration Systems
    std::string logPath = ConfigManager::GetUserSubdirectory("user") + "/rayv_paint.log";
    Logger::Get().Init(logPath);
    Logger::Get().Info("===================================================");
    Logger::Get().Info("Starting RayVPaint tech-art editor...");

    auto time_prev = startupStart;
    auto log_step = [&](const std::string& stepName) {
        auto now = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - time_prev).count();
        Logger::Get().Info("[Profiler] Step '" + stepName + "' took: " + std::to_string(ms) + " ms");
        time_prev = now;
    };
    log_step("CLI & Console Setup");

    ConfigManager::Get().Load(configPath);
    
    // Set logger level from config
    std::string cfgLevel = ConfigManager::Get().GetLogLevel();
    if (cfgLevel == "debug") Logger::Get().SetMinLevel(LogLevel::LogLevel_Debug);
    else if (cfgLevel == "warn") Logger::Get().SetMinLevel(LogLevel::LogLevel_Warning);
    else if (cfgLevel == "error") Logger::Get().SetMinLevel(LogLevel::LogLevel_Error);
    else Logger::Get().SetMinLevel(LogLevel::LogLevel_Info);

    // Brush presets: global user library (AppData/RayVPaint/brushes)
    BrushLibrary::Get().LoadAll();
    if (testBrushes) {
        bool ok = BrushLibrary::RunSmokeTest();
        Logger::Get().Info(ok ? "BrushLibrary smoke PASS" : "BrushLibrary smoke FAIL");
        return ok ? 0 : 2;
    }
    log_step("Logger & Config Systems");

    // 3. Initialize Concurrency (ThreadPool)
    unsigned int numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 2;
    ThreadPool::Get().Init(numThreads);
    log_step("Concurrency (ThreadPool)");

    // 4. Initialize Scripting Engine
    if (!scriptPath.empty() || headlessMode || testMode) {
        ScriptingEngine::Get().Initialize();
    } else {
        std::thread([]() {
            ScriptingEngine::Get().Initialize();
        }).detach();
    }
    log_step("Scripting Engine Init Link/Start");

    // Canvas dimensions and GPU resources are created lazily when the user opens or creates a document.

    // 5. Initialize GLFW (if not in true headless mode)
    if (!glfwInit()) {
        Logger::Get().Error("Failed to initialize GLFW");
        return 1;
    }

    // Configure window visibility (hidden for headless / test mode)
    if (testMode) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Using DirectX 11
    GLFWwindow* window = glfwCreateWindow(1280, 720, "RayV Paint", nullptr, nullptr);
    if (!window) {
        Logger::Get().Error("Failed to create GLFW window");
        glfwTerminate();
        return 1;
    }
    log_step("GLFW Window Creation");

    HWND hWnd = glfwGetWin32Window(window);

    // Load and set the application icon from resource ID 1 (IDI_ICON1)
    HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));
    if (hIcon) {
        SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    }

    // Subclass window procedure for high-precision pointer/tablet messages
    g_OriginalWndProc = (WNDPROC)SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)SubclassedWndProc);

    // Initialize KeymapManager (requires GLFW initialized and window created for scancode resolution)
    KeymapManager::Get().Initialize();
    KeymapManager::Get().Load();
    log_step("Keymap Manager Init");

    // 6. Initialize DirectX 11
    if (!CreateDeviceD3D(hWnd, headlessMode)) {
        Logger::Get().Error("Failed to initialize DirectX 11");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    CreateRenderTarget();
    log_step("DirectX 11 Device Creation");

    // 7. Initialize Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // Set custom ini path in documents folder
    static std::string imguiIniPath = ConfigManager::GetUserSubdirectory("user") + "/imgui.ini";
    io.IniFilename = imguiIniPath.c_str();

    // Load Segoe UI from system fonts for premium typography
    std::string fontPath = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (std::filesystem::exists(fontPath)) {
        io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 17.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    } else {
        io.Fonts->AddFontDefault();
    }

    // Apply configured theme
    ApplyTheme(ConfigManager::Get().GetTheme());

    ImGui_ImplGlfw_InitForOther(window, true);
    g_PrevKeyCallback = glfwSetKeyCallback(window, CustomKeyCallback);
    glfwSetDropCallback(window, CustomDropCallback);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    log_step("ImGui Context & Backend Init");

    // 8. Initialize Canvas Renderer
    if (!g_Canvas.Initialize(g_pd3dDevice)) {
        Logger::Get().Error("Failed to initialize Canvas renderer");
        CleanupDeviceD3D();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    log_step("Canvas Renderer D3D Init (including Shader Loading/Compiling)");

    // Check for crash recovery / autosave restore
    std::string backupDir = ConfigManager::Get().GetBackupDir();
    std::string backupPath = backupDir + "/autosave_backup.rayp";
    bool showRecoveryModal = std::filesystem::exists(backupPath);

    // Load startup document if specified on CLI (image or .rayp project)
    if (!startupImagePath.empty()) {
        if (!scriptPath.empty()) {
            if (!g_Canvas.OpenDocument(g_pd3dDevice, startupImagePath)) {
                Logger::Get().Error("Failed to open startup document: " + startupImagePath);
            }
        } else {
            UI::TriggerBackgroundOpenDocument(startupImagePath, g_pd3dDevice, g_Canvas);
        }
    }

    // Measure startup time
    auto startupEnd = std::chrono::high_resolution_clock::now();
    g_StartupTimeMs = std::chrono::duration<double, std::milli>(startupEnd - startupStart).count();
    Logger::Get().Info("Startup completed in: " + std::to_string(g_StartupTimeMs) + " ms");

    // Execute script from CLI if provided
    if (!scriptPath.empty()) {
        Logger::Get().InfoTag("test", "Running script: " + scriptPath);
        MemoryStats::LogSnapshot("before_script");
        if (!ScriptingEngine::Get().RunScript(scriptPath)) {
            Logger::Get().ErrorTag("test", "Script failed: " + scriptPath);
            processExitCode = 2;
        } else {
            Logger::Get().InfoTag("test", "Script finished OK: " + scriptPath);
        }
        MemoryStats::LogSnapshot("after_script");
        if (test16kMode) {
            Logger::Get().InfoTag("test",
                "16k post-check canvas=" + std::to_string(GetCanvasWidth()) + "x" +
                std::to_string(GetCanvasHeight()) +
                " tiles=" + std::to_string(GetActiveLayerTileCount()) +
                " WS_MiB=" + std::to_string(GetProcessWorkingSetMiB()));
        }
    }

    // Trackers
    int currentWindowWidth = 1280;
    int currentWindowHeight = 720;
    glfwGetFramebufferSize(window, &currentWindowWidth, &currentWindowHeight);

    UI::UIState uiState;
    uiState.startupTimeMs = g_StartupTimeMs;
    uiState.backupDir = backupDir;
    uiState.backupPath = backupPath;
    uiState.showRecoveryModal = showRecoveryModal;

    auto lastAutoSaveTime = std::chrono::steady_clock::now();

    // 9. Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Handle completed background loading
        if (UI::g_LoadingState.isLoading && UI::g_LoadingState.completed) {
            UI::g_LoadingState.isLoading = false;
            g_Canvas.MarkCompositeDirty();
            if (UI::g_LoadingState.success) {
                Logger::Get().Info("Background document load successful: " + UI::g_LoadingState.filepath);
            } else {
                Logger::Get().Error("Background document load failed: " + UI::g_LoadingState.filepath);
            }
        }

        // Handle window resizing
        int winW, winH;
        glfwGetFramebufferSize(window, &winW, &winH);
        if (winW > 0 && winH > 0 && (winW != currentWindowWidth || winH != currentWindowHeight)) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, winW, winH, DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
            currentWindowWidth = winW;
            currentWindowHeight = winH;
            Logger::Get().Debug("Swapchain backbuffers resized to " + std::to_string(winW) + "x" + std::to_string(winH));
        }

        auto loopStart = std::chrono::high_resolution_clock::now();

        // Start Dear ImGui Frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        g_IsViewportHovered = false;
        g_IsLayersHovered = false;

        // Layer preview double-refresh (paint/edit may leave thumbs one frame stale)
        if (g_LayerPreviewRefreshFrames > 0) {
            g_Canvas.MarkCompositeDirty();
            --g_LayerPreviewRefreshFrames;
        }

        // Render all UI Panels and Modals (Toolbar, Properties, Layers, Brush settings, Console logs, Colors)
        UI::RenderAll(uiState, g_Canvas, g_Brush, g_ActiveTool, g_pd3dDevice, g_pd3dDeviceContext, window);

        // Keyboard Shortcuts Handler (Layout-Independent via KeymapManager)
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput) {
            if (KeymapManager::Get().ConsumeActionTrigger("Undo")) {
                g_Canvas.Undo();
            }
            if (KeymapManager::Get().ConsumeActionTrigger("Redo")) {
                g_Canvas.Redo();
            }
            if (KeymapManager::Get().ConsumeActionTrigger("SaveProject")) {
                uiState.openSaveRaypModal = true;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("OpenProject")) {
                uiState.openLoadRaypModal = true;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("BrushTool")) {
                g_ActiveTool = ActiveTool::Brush;
                g_Brush.erase = false;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("EraserTool")) {
                g_ActiveTool = ActiveTool::Eraser;
                g_Brush.erase = true;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("BucketFillTool")) {
                g_ActiveTool = ActiveTool::BucketFill;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("GradientTool")) {
                g_ActiveTool = ActiveTool::Gradient;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("PipetteTool")) {
                g_ActiveTool = ActiveTool::Pipette;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("SelectToolGroup")) {
                if (UI::IsSelectTool(g_ActiveTool)) {
                    g_ActiveTool = UI::CycleSelectTool(g_ActiveTool);
                } else {
                    g_ActiveTool = g_LastSelectTool;
                }
                g_LastSelectTool = g_ActiveTool;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("LassoToolGroup")) {
                if (UI::IsLassoTool(g_ActiveTool)) {
                    g_ActiveTool = UI::CycleLassoTool(g_ActiveTool);
                } else {
                    g_ActiveTool = g_LastLassoTool;
                }
                g_LastLassoTool = g_ActiveTool;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("WandToolGroup")) {
                if (UI::IsWandTool(g_ActiveTool)) {
                    g_ActiveTool = UI::CycleWandTool(g_ActiveTool);
                } else {
                    g_ActiveTool = g_LastWandTool;
                }
                g_LastWandTool = g_ActiveTool;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("PanTool")) {
                g_ActiveTool = ActiveTool::Pan;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("RotateTool")) {
                g_ActiveTool = ActiveTool::Pan;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("TransformTool")) {
                g_ActiveTool = ActiveTool::MovePixels;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("SmudgeTool")) {
                g_ActiveTool = ActiveTool::Smudge;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("SelectAll")) {
                g_Canvas.SelectAll();
            }
            if (KeymapManager::Get().ConsumeActionTrigger("DuplicateLayer")) {
                if (!uiState.selectedLayers.empty()) {
                    g_Canvas.DuplicateLayers(g_pd3dDevice, uiState.selectedLayers);
                    // Refresh selection to new clones is best-effort: leave active as set by core
                    uiState.selectedLayers.clear();
                    if (g_Canvas.GetActiveLayerIndex() >= 0)
                        uiState.selectedLayers.push_back(g_Canvas.GetActiveLayerIndex());
                } else if (g_Canvas.GetActiveLayerIndex() >= 0) {
                    int neu = g_Canvas.DuplicateLayer(g_pd3dDevice, g_Canvas.GetActiveLayerIndex());
                    uiState.selectedLayers.clear();
                    if (neu >= 0) uiState.selectedLayers.push_back(neu);
                }
            }
            if (KeymapManager::Get().ConsumeActionTrigger("CropToSelection")) {
                if (g_Canvas.HasSelection()) {
                    g_Canvas.CropCanvasToSelection(g_pd3dDevice);
                }
            }
            if (KeymapManager::Get().ConsumeActionTrigger("InvertSelection")) {
                g_Canvas.InvertSelection();
            }
            if (KeymapManager::Get().ConsumeActionTrigger("InvertColors")) {
                g_Canvas.InvertColors();
            }
            if (KeymapManager::Get().ConsumeActionTrigger("InvertAlpha")) {
                g_Canvas.InvertAlpha();
            }
            if (KeymapManager::Get().ConsumeActionTrigger("AdjustHSV")) {
                uiState.showHSVModal = true;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("QuickExport") || uiState.openQuickExportTrigger) {
                uiState.openQuickExportTrigger = false;
                std::string path = g_Canvas.GetExportPath();
                if (path.empty()) {
                    std::string proj = g_Canvas.GetCurrentProjectFilePath();
                    if (!proj.empty()) {
                        size_t dot = proj.find_last_of('.');
                        if (dot != std::string::npos) {
                            path = proj.substr(0, dot) + ".png";
                        } else {
                            path = proj + ".png";
                        }
                    } else {
                        path = "export.png";
                    }
                    g_Canvas.SetExportPath(path);
                }
                
                size_t dot = path.find_last_of('.');
                std::string ext = "";
                if (dot != std::string::npos) {
                    ext = path.substr(dot + 1);
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                }
                
                if (ext == "dds") {
                    DdsFormat fmt = DdsFormat::RGBA8_UNORM;
                    if (g_Canvas.SaveCanvas(path, fmt)) {
                        Logger::Get().Info("Quick exported DDS successfully to: " + path);
                    } else {
                        Logger::Get().Error("Quick export DDS failed for path: " + path);
                    }
                } else {
                    if (g_Canvas.SaveCanvasStandard(path, g_Canvas.GetExportIccPreset())) {
                        Logger::Get().Info("Quick exported image successfully to: " + path);
                    } else {
                        Logger::Get().Error("Quick export image failed for path: " + path);
                    }
                }
            }
            if (KeymapManager::Get().ConsumeActionTrigger("AdvancedExport")) {
                uiState.openExportAdvancedModal = true;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("Copy")) {
                std::vector<float> composite = g_Canvas.GetCompositePixels();
                ClipboardHelper::CopyImageToClipboard(composite, g_Canvas.GetWidth(), g_Canvas.GetHeight());
            }
            if (KeymapManager::Get().ConsumeActionTrigger("Paste")) {
                std::vector<float> pastedPixels;
                int pastedW = 0, pastedH = 0;
                if (ClipboardHelper::PasteImageFromClipboard(pastedPixels, pastedW, pastedH)) {
                    g_Canvas.CreateLayerFromPixels(g_pd3dDevice, "Pasted Layer", pastedPixels, pastedW, pastedH);
                }
            }
        }

        // Background Auto-Save trigger
        static bool s_IsAutoSaving = false;
        int autoSaveInterval = ConfigManager::Get().GetAutoSaveIntervalMinutes();
        if (autoSaveInterval > 0 && g_Canvas.IsDocumentModified() && !s_IsAutoSaving) {
            auto currentTime = std::chrono::steady_clock::now();
            auto elapsedMinutes = std::chrono::duration_cast<std::chrono::minutes>(currentTime - lastAutoSaveTime).count();
            if (elapsedMinutes >= autoSaveInterval) {
                s_IsAutoSaving = true;
                std::filesystem::create_directories(uiState.backupDir);
                Logger::Get().Info("Triggering background auto-save to " + uiState.backupPath);
                g_Canvas.SaveCanvasRaypAsync(uiState.backupPath, [](bool success) {
                    s_IsAutoSaving = false;
                    if (success) {
                        Logger::Get().Info("Background auto-save completed successfully.");
                    } else {
                        Logger::Get().Error("Background auto-save failed.");
                    }
                });
                lastAutoSaveTime = currentTime;
            }
        }

        // 9.5 Draw Canvas Viewport
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("Canvas Viewport", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        g_IsViewportHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        ImGui::PopStyleVar();

        ImVec2 viewportPanelSize = ImGui::GetContentRegionAvail();
        int viewportWidth = static_cast<int>(viewportPanelSize.x);
        int viewportHeight = static_cast<int>(viewportPanelSize.y);

        if (viewportWidth > 0 && viewportHeight > 0) {
            ResizeCanvasRenderTarget(viewportWidth, viewportHeight);

            // Render first to have the texture ready for ImGui::Image
            RenderCanvasToTexture(viewportWidth, viewportHeight);

            // Draw the viewport image using exact integer dimensions to prevent pixel interpolation blurring
            ImGui::Image((void*)g_canvasSRV, ImVec2(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight)));

            // Calculate precise mouse coordinates using the actual image bounding box
            ImVec2 imageMin = ImGui::GetItemRectMin();
            ImVec2 mousePos = ImGui::GetMousePos();
            float localMouseX = mousePos.x - imageMin.x;
            float localMouseY = mousePos.y - imageMin.y;

            // Precise hover check: only true if mouse is directly over the canvas viewport image
            bool isHovered = ImGui::IsItemHovered() && !UI::g_LoadingState.isLoading;

            // Map mouse coordinates to Canvas pixel coordinates using floored origin matching the vertex shader
            float screenOriginX = std::floor(g_Canvas.GetPan().x + static_cast<float>(viewportWidth) * 0.5f);
            float screenOriginY = std::floor(g_Canvas.GetPan().y + static_cast<float>(viewportHeight) * 0.5f);

            float rotatedX = (localMouseX - screenOriginX) / g_Canvas.GetZoom();
            float rotatedY = (localMouseY - screenOriginY) / g_Canvas.GetZoom();

            float angle = g_Canvas.GetRotationAngle();
            float cosA = std::cos(angle);
            float sinA = std::sin(angle);
            
            float centerX = g_Canvas.GetWidth() * 0.5f;
            float centerY = g_Canvas.GetHeight() * 0.5f;
            
            float relX = rotatedX - centerX;
            float relY = rotatedY - centerY;
            
            float canvasX = relX * cosA + relY * sinA + centerX;
            float canvasY = -relX * sinA + relY * cosA + centerY;

            if (g_Canvas.GetViewportFlipH()) {
                canvasX = (float)g_Canvas.GetWidth() - canvasX;
            }
            if (g_Canvas.GetViewportFlipV()) {
                canvasY = (float)g_Canvas.GetHeight() - canvasY;
            }

            // Check if cursor is within active canvas boundary
            bool isInsideCanvas = (canvasX >= 0.0f && canvasX < (float)g_Canvas.GetWidth() &&
                                   canvasY >= 0.0f && canvasY < (float)g_Canvas.GetHeight());

            // Smudge is NOT brush-like: must not paint with brush.color / accent color.
            bool isBrushLikeTool = (g_ActiveTool == ActiveTool::Brush || g_ActiveTool == ActiveTool::Eraser);
            bool isSmudgeTool = (g_ActiveTool == ActiveTool::Smudge);
            bool isPipetteTool = (g_ActiveTool == ActiveTool::Pipette);
            // Alt-sample blocked when Ctrl held (future: Ctrl+Alt+LMB = brush rotation)
            bool isEyedropperMode = ((isBrushLikeTool || isSmudgeTool) && ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeyCtrl)
                                    || isPipetteTool;

            if (g_ActiveTool != g_PrevActiveTool) {
                EndBrushStrokeIfNeeded();
                // Contract: cancel in-progress quick-select stroke on tool switch (no undo/selection change)
                if (g_PrevActiveTool == ActiveTool::QuickSelect && g_Canvas.IsQuickSelectStrokeActive()) {
                    g_Canvas.CancelQuickSelectStroke();
                    g_IsSelectionDragging = false;
                    g_LassoPoints.clear();
                }
                g_PrevActiveTool = g_ActiveTool;
            }

            // Panning: Middle mouse button drag OR left mouse button drag when Pan tool is selected
            bool isPanning = false;
            bool isRotating = false;
            float dragDx = 0.0f;
            float dragDy = 0.0f;
            if (isHovered) {
                bool wantRotate = false;
                bool wantPan = false;

                if (g_ActiveTool == ActiveTool::Pan) {
                    if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
                        wantRotate = true;
                    } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                        if (ImGui::GetIO().KeyShift) {
                            wantRotate = true;
                        } else {
                            wantPan = true;
                        }
                    }
                }
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                    wantPan = true;
                }

                static bool s_IsDraggingRotation = false;
                static float s_StartRotationAngle = 0.0f;

                if (wantPan) {
                    ImGuiMouseButton panButton = ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ? ImGuiMouseButton_Middle : ImGuiMouseButton_Left;
                    ImVec2 drag = ImGui::GetMouseDragDelta(panButton);
                    dragDx = drag.x - g_LastDragDelta.x;
                    dragDy = drag.y - g_LastDragDelta.y;
                    g_LastDragDelta = drag;
                    isPanning = true;
                    s_IsDraggingRotation = false;
                } else if (wantRotate) {
                    if (!s_IsDraggingRotation) {
                        s_IsDraggingRotation = true;
                        s_StartRotationAngle = g_Canvas.GetRotationAngle();
                    }
                    ImGuiMouseButton rotateButton = ImGui::IsMouseDragging(ImGuiMouseButton_Right) ? ImGuiMouseButton_Right : ImGuiMouseButton_Left;
                    ImVec2 drag = ImGui::GetMouseDragDelta(rotateButton);
                    dragDx = drag.x - g_LastDragDelta.x;
                    g_LastDragDelta = drag;
                    
                    float targetAngle = s_StartRotationAngle + drag.x * 0.005f;
                    if (ImGui::GetIO().KeyCtrl) {
                        float snapStep = 45.0f * (3.14159265f / 180.0f);
                        targetAngle = std::round(targetAngle / snapStep) * snapStep;
                    }
                    g_Canvas.SetRotationAngle(targetAngle);
                    isRotating = true;
                } else {
                    s_IsDraggingRotation = false;
                    g_LastDragDelta = ImVec2(0.0f, 0.0f);
                }
            } else {
                g_LastDragDelta = ImVec2(0.0f, 0.0f);
            }

            float wheelDelta = isHovered ? ImGui::GetIO().MouseWheel : 0.0f;

            // Photoshop-like Brush Resize/Hardness (Ctrl+Alt+RMB)
            // CRITICAL: use ABSOLUTE offset from start (not accumulated MouseDelta).
            // Freezing MousePos made MouseDelta = full travel each frame → explosive size/hardness.
            // WYSIWYG size: screen circle grows 1:1 with horizontal mouse travel
            //   r_screen = radius * zoom  ⇒  radius = startR + dx_screen / zoom
            // Hardness: full 0..1 over a drag equal to the max brush circle radius on screen
            //   (same visual zone as the ghost max ring).
            static bool g_IsCtrlAltRmbDragging = false;
            static ImVec2 g_CtrlAltRmbScreenPos(0, 0);
            static float g_CtrlAltRmbStartRadius = 10.f;
            static float g_CtrlAltRmbStartHardness = 0.5f;
            static double g_CtrlAltRmbGlfwX = 0.0, g_CtrlAltRmbGlfwY = 0.0;

            const bool brushOrEraser = isBrushLikeTool;
            const float maxBrushR = std::max(10.f, ConfigManager::Get().GetMaxBrushRadius());
            ImGuiIO& ioRmb = ImGui::GetIO();

            if (!g_IsCtrlAltRmbDragging && brushOrEraser && ioRmb.KeyCtrl && ioRmb.KeyAlt
                && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
                && (isHovered || ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))) {
                g_IsCtrlAltRmbDragging = true;
                g_CtrlAltRmbScreenPos = ImGui::GetMousePos();
                g_CtrlAltRmbStartRadius = g_Brush.radius;
                g_CtrlAltRmbStartHardness = g_Brush.hardness;
                GLFWwindow* win = glfwGetCurrentContext();
                if (win) {
                    glfwGetCursorPos(win, &g_CtrlAltRmbGlfwX, &g_CtrlAltRmbGlfwY);
                    glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
                }
            }

            if (g_IsCtrlAltRmbDragging) {
                GLFWwindow* win = glfwGetCurrentContext();
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) || !brushOrEraser) {
                    g_IsCtrlAltRmbDragging = false;
                    if (win) {
                        glfwSetCursorPos(win, g_CtrlAltRmbGlfwX, g_CtrlAltRmbGlfwY);
                        glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                    }
                    ioRmb.MouseDelta = ImVec2(0, 0);
                } else {
                    // Absolute screen offset from invoke point (GLFW client coords)
                    double cx = g_CtrlAltRmbGlfwX, cy = g_CtrlAltRmbGlfwY;
                    if (win) glfwGetCursorPos(win, &cx, &cy);
                    float dxScreen = (float)(cx - g_CtrlAltRmbGlfwX);
                    float dyScreen = (float)(cy - g_CtrlAltRmbGlfwY);

                    const float zoom = std::max(0.001f, g_Canvas.GetZoom());
                    // Size: 1 screen px of drag ↔ 1 screen px of circle radius change
                    g_Brush.radius = std::clamp(
                        g_CtrlAltRmbStartRadius + dxScreen / zoom, 1.0f, maxBrushR);

                    // Hardness: drag across the max-radius ghost ring (screen px) = 0..1 full range
                    // Matches visual: travel from center to ghost edge = full hardness swing
                    float hardSpanScreen = std::max(32.f, maxBrushR * zoom);
                    g_Brush.hardness = std::clamp(
                        g_CtrlAltRmbStartHardness - dyScreen / hardSpanScreen, 0.0f, 1.0f);

                    // Kill ImGui input so pan/paint don't move; do NOT feed fake MousePos
                    // (that recreated huge deltas next frame).
                    ioRmb.MouseDelta = ImVec2(0, 0);
                    ioRmb.WantCaptureMouse = true;

                    ImGui::SetTooltip(
                        "Size: %.1f / %.0f px  (screen circle = size × zoom)\n"
                        "Hardness: %.0f%%  (full range = drag max-ring radius)",
                        g_Brush.radius, maxBrushR, g_Brush.hardness * 100.f);

                    ImDrawList* drawList = ImGui::GetForegroundDrawList();
                    const ImVec2 fixed = g_CtrlAltRmbScreenPos;
                    float rScreen = g_Brush.radius * zoom;
                    float hScreen = rScreen * g_Brush.hardness;
                    float maxScreen = maxBrushR * zoom;
                    // Ghost max = hardness/size reference zone
                    drawList->AddCircle(fixed, maxScreen, IM_COL32(255, 255, 255, 50), 64, 1.0f);
                    // Soft falloff annulus + hard core
                    if (g_Brush.hardness < 0.999f && rScreen > hScreen + 0.5f)
                        drawList->AddCircleFilled(fixed, rScreen, IM_COL32(235, 64, 52, 35), 64);
                    if (hScreen > 0.5f)
                        drawList->AddCircleFilled(fixed, hScreen, IM_COL32(235, 64, 52, 110), 64);
                    drawList->AddCircle(fixed, rScreen, IM_COL32(235, 64, 52, 235), 64, 2.0f);
                    if (hScreen > 1.f && hScreen < rScreen - 0.5f)
                        drawList->AddCircle(fixed, hScreen, IM_COL32(255, 200, 120, 200), 64, 1.25f);
                }
                isPanning = false;
                isRotating = false;
                wheelDelta = 0.f;
                dragDx = dragDy = 0.f;
            }

            // RMB single-click (Brush/Eraser, no mod) → brush preset popup
            static bool s_BrushRmbArmed = false;
            static ImVec2 s_BrushRmbStart(0, 0);
            static ImVec2 s_BrushPopupPos(0, 0);
            static bool s_WantBrushPopup = false;
            if (isHovered && brushOrEraser && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt
                && !g_IsCtrlAltRmbDragging && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                s_BrushRmbArmed = true;
                s_BrushRmbStart = ImGui::GetMousePos();
            }
            if (s_BrushRmbArmed) {
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                    ImVec2 p = ImGui::GetMousePos();
                    float ddx = p.x - s_BrushRmbStart.x, ddy = p.y - s_BrushRmbStart.y;
                    if (ddx * ddx + ddy * ddy < 36.f) { // <6px = click, not drag
                        s_WantBrushPopup = true;
                        s_BrushPopupPos = s_BrushRmbStart;
                    }
                    s_BrushRmbArmed = false;
                } else {
                    ImVec2 p = ImGui::GetMousePos();
                    float ddx = p.x - s_BrushRmbStart.x, ddy = p.y - s_BrushRmbStart.y;
                    if (ddx * ddx + ddy * ddy > 100.f) // moved too far → cancel
                        s_BrushRmbArmed = false;
                }
            }

            // Handle custom brush / smudge visualizer and cursor hiding when inside canvas bounds
            if (isHovered && isInsideCanvas && !g_IsCtrlAltRmbDragging && (isBrushLikeTool || isSmudgeTool) && !isEyedropperMode) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);

                // Draw custom outline circle at mouse position
                ImDrawList* drawList = ImGui::GetForegroundDrawList();
                float cursorRadius = isSmudgeTool ? uiState.smudge.radius : g_Brush.radius;
                float screenRadius = cursorRadius * g_Canvas.GetZoom();
                drawList->AddCircle(mousePos, screenRadius, IM_COL32(0, 0, 0, 255), 32, 1.5f);
                drawList->AddCircle(mousePos, screenRadius, IM_COL32(255, 255, 255, 255), 32, 1.0f);
            }

            float zoom = g_Canvas.GetZoom();
            float cw = (float)g_Canvas.GetWidth();
            float ch = (float)g_Canvas.GetHeight();

            auto canvasToScreen = [&](float cx, float cy) -> ImVec2 {
                if (g_Canvas.GetViewportFlipH()) cx = cw - cx;
                if (g_Canvas.GetViewportFlipV()) cy = ch - cy;
                float rx = cx - cw * 0.5f;
                float ry = cy - ch * 0.5f;
                float rotX = rx * cosA - ry * sinA;
                float rotY = rx * sinA + ry * cosA;
                return ImVec2(
                    imageMin.x + screenOriginX + (rotX + cw * 0.5f) * zoom,
                    imageMin.y + screenOriginY + (rotY + ch * 0.5f) * zoom
                );
            };

            // Rulers (canvas pixel coords) — View → Rulers
            if (uiState.showRulers) {
                const float R = 16.f;
                ImDrawList* rdl = ImGui::GetWindowDrawList();
                ImU32 bg = IM_COL32(28, 28, 30, 230);
                ImU32 tick = IM_COL32(160, 160, 165, 200);
                ImU32 textC = IM_COL32(200, 200, 205, 220);
                // Background strips
                rdl->AddRectFilled(imageMin, ImVec2(imageMin.x + viewportWidth, imageMin.y + R), bg);
                rdl->AddRectFilled(imageMin, ImVec2(imageMin.x + R, imageMin.y + viewportHeight), bg);
                rdl->AddRectFilled(imageMin, ImVec2(imageMin.x + R, imageMin.y + R), IM_COL32(22, 22, 24, 255));

                // Choose step so labels stay readable
                float step = 50.f;
                if (zoom >= 2.f) step = 25.f;
                if (zoom >= 4.f) step = 10.f;
                if (zoom < 0.5f) step = 100.f;
                if (zoom < 0.25f) step = 200.f;

                // Horizontal ticks (top)
                for (float cx = 0.f; cx <= cw; cx += step) {
                    ImVec2 sp = canvasToScreen(cx, 0.f);
                    if (sp.x < imageMin.x + R || sp.x > imageMin.x + viewportWidth) continue;
                    float len = (std::fmod(cx, step * 2.f) < 0.01f) ? R - 2.f : R * 0.55f;
                    rdl->AddLine(ImVec2(sp.x, imageMin.y + R), ImVec2(sp.x, imageMin.y + R - len), tick, 1.f);
                    if (std::fmod(cx, step * 2.f) < 0.01f || step >= 50.f) {
                        char lb[16]; std::snprintf(lb, sizeof(lb), "%.0f", cx);
                        rdl->AddText(ImVec2(sp.x + 2.f, imageMin.y + 1.f), textC, lb);
                    }
                }
                // Vertical ticks (left)
                for (float cy = 0.f; cy <= ch; cy += step) {
                    ImVec2 sp = canvasToScreen(0.f, cy);
                    if (sp.y < imageMin.y + R || sp.y > imageMin.y + viewportHeight) continue;
                    float len = (std::fmod(cy, step * 2.f) < 0.01f) ? R - 2.f : R * 0.55f;
                    rdl->AddLine(ImVec2(imageMin.x + R, sp.y), ImVec2(imageMin.x + R - len, sp.y), tick, 1.f);
                    if (std::fmod(cy, step * 2.f) < 0.01f || step >= 50.f) {
                        char lb[16]; std::snprintf(lb, sizeof(lb), "%.0f", cy);
                        rdl->AddText(ImVec2(imageMin.x + 1.f, sp.y + 1.f), textC, lb);
                    }
                }
            }

            // Draw Symmetrical Guidelines
            if (g_Canvas.GetMirrorHorizontal() || g_Canvas.GetMirrorVertical()) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                auto drawDashedLine = [](ImDrawList* drawList, ImVec2 p1, ImVec2 p2, ImU32 col, float thickness, float dashLength) {
                    ImVec2 d = ImVec2(p2.x - p1.x, p2.y - p1.y);
                    float len = std::sqrt(d.x * d.x + d.y * d.y);
                    if (len == 0.0f) return;
                    ImVec2 dir = ImVec2(d.x / len, d.y / len);
                    float traveled = 0.0f;
                    bool draw = true;
                    while (traveled < len) {
                        float step = dashLength;
                        if (traveled + step > len) step = len - traveled;
                        if (draw) {
                            drawList->AddLine(
                                ImVec2(p1.x + dir.x * traveled, p1.y + dir.y * traveled),
                                ImVec2(p1.x + dir.x * (traveled + step), p1.y + dir.y * (traveled + step)),
                                col, thickness
                            );
                        }
                        traveled += step;
                        draw = !draw;
                    }
                };

                // Push clip rect to ensure guides don't draw outside viewport boundaries
                dl->PushClipRect(imageMin, ImVec2(imageMin.x + viewportWidth, imageMin.y + viewportHeight), true);

                if (g_Canvas.GetMirrorHorizontal()) {
                    ImVec2 h1 = canvasToScreen(cw * 0.5f, -ch * 5.0f);
                    ImVec2 h2 = canvasToScreen(cw * 0.5f, cw * 0.5f); // Wait! Let's check the original line: h2 was canvasToScreen(cw * 0.5f, ch * 6.0f)!
                    // Let's make sure it's ch * 6.0f!
                    h2 = canvasToScreen(cw * 0.5f, ch * 6.0f);
                    drawDashedLine(dl, h1, h2, IM_COL32(235, 64, 52, 180), 1.5f, 6.0f);
                }
                if (g_Canvas.GetMirrorVertical()) {
                    ImVec2 v1 = canvasToScreen(-cw * 5.0f, ch * 0.5f);
                    ImVec2 v2 = canvasToScreen(cw * 6.0f, ch * 0.5f);
                    drawDashedLine(dl, v1, v2, IM_COL32(235, 64, 52, 180), 1.5f, 6.0f);
                }

                dl->PopClipRect();
            }

            // Draw interaction logic
            static float g_LastStrokeEndX = 0.0f;
            static float g_LastStrokeEndY = 0.0f;
            static bool g_HasLastStrokeEnd = false;

            static float g_StrokeStartX = 0.0f;
            static float g_StrokeStartY = 0.0f;
            static bool g_LockAxisSelected = false;
            enum class LockAxis { None, Horizontal, Vertical };
            static LockAxis g_LockAxis = LockAxis::None;

            bool isShiftHeld = ImGui::GetIO().KeyShift;

            // Eyedropper: live preview sample + commit on LMB
            static float s_PipettePreview[4] = {0, 0, 0, 1};
            static bool s_PipetteHasPreview = false;
            if (isHovered && isInsideCanvas && isEyedropperMode && !isPanning && !g_IsCtrlAltRmbDragging) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);
                UI::SampleCanvasColor(g_Canvas, canvasX, canvasY, s_PipettePreview);
                s_PipetteHasPreview = true;
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    g_Brush.color[0] = s_PipettePreview[0];
                    g_Brush.color[1] = s_PipettePreview[1];
                    g_Brush.color[2] = s_PipettePreview[2];
                    g_Brush.color[3] = s_PipettePreview[3];
                }
            } else if (!isEyedropperMode) {
                s_PipetteHasPreview = false;
            }

            if (isHovered && !isPanning && !g_IsCtrlAltRmbDragging && isBrushLikeTool && !isEyedropperMode) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    g_IsPainting = true;
                    if (isShiftHeld && g_HasLastStrokeEnd) {
                        // Draw line from last stroke end to current mouse at an arbitrary angle (NO axis lock)
                        g_Canvas.PaintOnActiveLayer(g_LastStrokeEndX, g_LastStrokeEndY, StrokePhase::Begin, g_Brush);
                        g_Canvas.PaintOnActiveLayer(canvasX, canvasY, StrokePhase::Update, g_Brush);
                        
                        g_StrokeStartX = canvasX;
                        g_StrokeStartY = canvasY;
                        g_LastStrokeEndX = canvasX;
                        g_LastStrokeEndY = canvasY;
                        g_LockAxisSelected = false;
                        g_LockAxis = LockAxis::None;
                    } else {
                        // Normal start
                        g_Canvas.PaintOnActiveLayer(canvasX, canvasY, StrokePhase::Begin, g_Brush);
                        g_StrokeStartX = canvasX;
                        g_StrokeStartY = canvasY;
                        g_LockAxisSelected = false;
                        g_LockAxis = LockAxis::None;
                        g_LastStrokeEndX = canvasX;
                        g_LastStrokeEndY = canvasY;
                    }
                } 
                else if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && g_IsPainting) {
                    float targetX = canvasX;
                    float targetY = canvasY;
                    
                    if (isShiftHeld) {
                        if (!g_LockAxisSelected) {
                            float dx = canvasX - g_StrokeStartX;
                            float dy = canvasY - g_StrokeStartY;
                            float dist = std::sqrt(dx * dx + dy * dy);
                            if (dist > 8.0f) {
                                g_LockAxisSelected = true;
                                if (std::abs(dx) > std::abs(dy)) {
                                    g_LockAxis = LockAxis::Horizontal;
                                } else {
                                    g_LockAxis = LockAxis::Vertical;
                                }
                            }
                        }
                        
                        if (g_LockAxisSelected) {
                            if (g_LockAxis == LockAxis::Horizontal) {
                                targetY = g_StrokeStartY;
                            } else if (g_LockAxis == LockAxis::Vertical) {
                                targetX = g_StrokeStartX;
                            }
                        }
                    } else {
                        g_LockAxisSelected = false;
                        g_LockAxis = LockAxis::None;
                    }
                    
                    g_Canvas.PaintOnActiveLayer(targetX, targetY, StrokePhase::Update, g_Brush);
                    g_LastStrokeEndX = targetX;
                    g_LastStrokeEndY = targetY;
                    g_HasLastStrokeEnd = true;
                }
            }

            if (g_IsPainting && (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
                g_Canvas.PaintOnActiveLayer(0, 0, StrokePhase::End, g_Brush);
                g_IsPainting = false;
            }

            // Smudge End
            if (g_ActiveTool == ActiveTool::Smudge && (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
                g_Canvas.SmudgeOnActiveLayer(0, 0, StrokePhase::End, uiState.smudge);
            }

            // Commit / Cancel Move Pixels (keyboard or Tool Settings panel buttons)
            if (g_Canvas.IsMovingPixels()) {
                bool doCommit = uiState.commitTransform ||
                    (!ImGui::GetIO().WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)));
                bool doCancel = uiState.cancelTransform ||
                    (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape));
                uiState.commitTransform = false;
                uiState.cancelTransform = false;

                if (doCommit) {
                    g_Canvas.CommitMovePixels(g_pd3dDevice);
                    g_MoveAccumulatedOffsetX = 0;
                    g_MoveAccumulatedOffsetY = 0;
                }
                else if (doCancel) {
                    g_Canvas.CancelMovePixels(g_pd3dDevice);
                    g_MoveAccumulatedOffsetX = 0;
                    g_MoveAccumulatedOffsetY = 0;
                }
            }

            // Auto-commit Move Pixels if tool switched
            if (g_ActiveTool != ActiveTool::MovePixels && g_Canvas.IsMovingPixels()) {
                g_Canvas.CommitMovePixels(g_pd3dDevice);
                g_MoveAccumulatedOffsetX = 0;
                g_MoveAccumulatedOffsetY = 0;
            }

            // Keyboard shortcuts (like Ctrl+D)
            if ((ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)) && !ImGui::GetIO().WantTextInput) {
                if (ImGui::IsKeyPressed(ImGuiKey_D)) {
                    g_Canvas.ClearSelection();
                    g_Canvas.UpdateSelectionMaskTexture(g_pd3dDevice);
                }
            }

            // Keyboard shortcut X for swapping primary/secondary colors (not Ctrl+X = crop)
            if (ImGui::IsKeyPressed(ImGuiKey_X) && !ImGui::GetIO().WantTextInput &&
                !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt) {
                std::swap(g_Brush.color[0], g_SecondaryColor[0]);
                std::swap(g_Brush.color[1], g_SecondaryColor[1]);
                std::swap(g_Brush.color[2], g_SecondaryColor[2]);
                std::swap(g_Brush.color[3], g_SecondaryColor[3]);
                g_ColorSwapPending = true; // toolbar/Colors play cross-fade
            }

            // Selection modifiers: Ctrl = add, Alt = subtract (Photoshop-like)
            auto selAdd = []() { return ImGui::GetIO().KeyCtrl; };
            auto selSub = []() { return ImGui::GetIO().KeyAlt; };
            // Shift constrains Rect/Ellipse to 1:1
            auto constrainSquare = [](float x0, float y0, float& x1, float& y1) {
                float dx = x1 - x0, dy = y1 - y0;
                float s = std::max(std::fabs(dx), std::fabs(dy));
                if (s < 1e-4f) return;
                x1 = x0 + (dx >= 0.f ? s : -s);
                y1 = y0 + (dy >= 0.f ? s : -s);
            };

            // Drag-based selection tools only (Magic Wand is click-once, not drag).
            bool isDragSelectionTool = (g_ActiveTool == ActiveTool::RectSelect ||
                                        g_ActiveTool == ActiveTool::EllipseSelect ||
                                        g_ActiveTool == ActiveTool::LassoSelect ||
                                        g_ActiveTool == ActiveTool::SmartSelect ||
                                        g_ActiveTool == ActiveTool::QuickSelect);

            if (isHovered && !isPanning && !g_IsCtrlAltRmbDragging) {
                // Magic Wand (click sets sticky seed + selection; not a drag tool)
                if (g_ActiveTool == ActiveTool::MagicWand) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && isInsideCanvas) {
                        bool add = selAdd();
                        bool subtract = selSub();
                        int sx = std::clamp((int)std::floor(canvasX), 0, g_Canvas.GetWidth() - 1);
                        int sy = std::clamp((int)std::floor(canvasY), 0, g_Canvas.GetHeight() - 1);
                        g_Canvas.ApplyMagicWandSelection(g_pd3dDevice, sx, sy, uiState.magicWandTolerance, add, subtract, uiState.magicWandContiguous);
                    }
                }
                // Bucket Fill
                else if (g_ActiveTool == ActiveTool::BucketFill) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        g_Canvas.ApplyBucketFill((int)canvasX, (int)canvasY, uiState.bucketFillTolerance, g_Brush.color, true);
                    }
                }
                // Gradient (drag to define vector)
                else if (g_ActiveTool == ActiveTool::Gradient) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        g_IsGradientDragging = true; 
                        g_SelectionDragStartX = canvasX;
                        g_SelectionDragStartY = canvasY;
                    }
                }
                // Smudge Tool
                else if (g_ActiveTool == ActiveTool::Smudge) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        g_Canvas.SmudgeOnActiveLayer(canvasX, canvasY, StrokePhase::Begin, uiState.smudge);
                    } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        g_Canvas.SmudgeOnActiveLayer(canvasX, canvasY, StrokePhase::Update, uiState.smudge);
                    }
                }
                else if (g_ActiveTool == ActiveTool::MovePixels) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        if (!g_Canvas.IsMovingPixels()) {
                            g_Canvas.StartMovePixels(g_pd3dDevice);
                            g_ActiveGizmoHandle = TransformGizmoHandle::Move;
                            g_IsMoveDragging = true;
                            g_MoveDragStartX = canvasX;
                            g_MoveDragStartY = canvasY;
                        } else {
                            GizmoScreenGeometry geo;
                            if (GetGizmoGeometry(g_Canvas, canvasToScreen, geo)) {
                                float threshSq = 64.0f; // 8px radius
                                ImVec2 mousePos = ImGui::GetMousePos();
                                if (distSq(mousePos, geo.p1) <= threshSq) g_ActiveGizmoHandle = TransformGizmoHandle::Scale_TL;
                                else if (distSq(mousePos, geo.p2) <= threshSq) g_ActiveGizmoHandle = TransformGizmoHandle::Scale_TR;
                                else if (distSq(mousePos, geo.p3) <= threshSq) g_ActiveGizmoHandle = TransformGizmoHandle::Scale_BR;
                                else if (distSq(mousePos, geo.p4) <= threshSq) g_ActiveGizmoHandle = TransformGizmoHandle::Scale_BL;
                                else if (distSq(mousePos, geo.mT) <= threshSq) g_ActiveGizmoHandle = TransformGizmoHandle::Scale_T;
                                else if (distSq(mousePos, geo.mR) <= threshSq) g_ActiveGizmoHandle = TransformGizmoHandle::Scale_R;
                                else if (distSq(mousePos, geo.mB) <= threshSq) g_ActiveGizmoHandle = TransformGizmoHandle::Scale_B;
                                else if (distSq(mousePos, geo.mL) <= threshSq) g_ActiveGizmoHandle = TransformGizmoHandle::Scale_L;
                                else if (IsPointInsideQuad(mousePos, geo.p1, geo.p2, geo.p3, geo.p4)) {
                                    g_ActiveGizmoHandle = TransformGizmoHandle::Move;
                                } else {
                                    float diag = std::sqrt(distSq(geo.p1, geo.p3));
                                    float distToCenter = std::sqrt(distSq(mousePos, geo.center));
                                    if (distToCenter < diag + 80.0f) {
                                        g_ActiveGizmoHandle = TransformGizmoHandle::Rotate;
                                        g_GizmoDragStartRotation = g_Canvas.GetFloatingRotation();
                                        g_GizmoDragStartMouseAngle = std::atan2(mousePos.y - geo.center.y, mousePos.x - geo.center.x);
                                    } else {
                                        g_ActiveGizmoHandle = TransformGizmoHandle::None;
                                    }
                                }

                                if (g_ActiveGizmoHandle != TransformGizmoHandle::None) {
                                    g_IsMoveDragging = true;
                                    g_MoveDragStartX = canvasX;
                                    g_MoveDragStartY = canvasY;
                                    g_GizmoDragStartScaleX = g_Canvas.GetFloatingScaleX();
                                    g_GizmoDragStartScaleY = g_Canvas.GetFloatingScaleY();
                                    g_GizmoDragStartDist = std::sqrt(distSq(mousePos, geo.center));
                                    if (g_GizmoDragStartDist < 5.0f) g_GizmoDragStartDist = 5.0f;
                                }
                            } else {
                                g_ActiveGizmoHandle = TransformGizmoHandle::Move;
                                g_IsMoveDragging = true;
                                g_MoveDragStartX = canvasX;
                                g_MoveDragStartY = canvasY;
                            }
                        }
                    }
                }
                // Drag-based selections
                else if (isDragSelectionTool) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        g_IsSelectionDragging = true;
                        g_SelectionDragStartX = canvasX;
                        g_SelectionDragStartY = canvasY;
                        g_LassoPoints.clear();
                        g_LassoPoints.push_back({ (int)canvasX, (int)canvasY });
                        if (g_ActiveTool == ActiveTool::QuickSelect) {
                            g_Canvas.BeginQuickSelectStroke();
                        }
                    }
                }
                // Polygonal Lasso
                else if (g_ActiveTool == ActiveTool::PolygonalLasso) {
                    bool add = selAdd();
                    bool subtract = selSub();
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (!g_PolygonalLassoPoints.empty()) {
                            g_PolygonalLassoPoints.pop_back(); // double-click also emits a single click vertex
                        }
                        if (g_PolygonalLassoPoints.size() >= 3) {
                            g_Canvas.ApplyPolygonalLassoSelection(g_PolygonalLassoPoints, add, subtract);
                            g_Canvas.UpdateSelectionMaskTexture(g_pd3dDevice);
                        }
                        g_PolygonalLassoPoints.clear();
                    }
                    else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        g_PolygonalLassoPoints.push_back({ (int)canvasX, (int)canvasY });
                    }
                }
            }

            // Keyboard close/cancel for Polygonal Lasso
            if (g_ActiveTool == ActiveTool::PolygonalLasso && !ImGui::GetIO().WantTextInput) {
                bool add = selAdd();
                bool subtract = selSub();
                if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                    if (g_PolygonalLassoPoints.size() >= 3) {
                        g_Canvas.ApplyPolygonalLassoSelection(g_PolygonalLassoPoints, add, subtract);
                        g_Canvas.UpdateSelectionMaskTexture(g_pd3dDevice);
                    }
                    g_PolygonalLassoPoints.clear();
                }
                else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    g_PolygonalLassoPoints.clear();
                }
            }

            // Quick Select: Esc mid-stroke → CancelQuickSelectStroke (no selection/undo change)
            if (g_ActiveTool == ActiveTool::QuickSelect && !ImGui::GetIO().WantTextInput &&
                ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                if (g_Canvas.IsQuickSelectStrokeActive()) {
                    g_Canvas.CancelQuickSelectStroke();
                }
                g_IsSelectionDragging = false;
                g_LassoPoints.clear();
            }

            // Accumulate points if lasso or smart select or quick select is active
            if (g_IsSelectionDragging && (g_ActiveTool == ActiveTool::LassoSelect || g_ActiveTool == ActiveTool::SmartSelect || g_ActiveTool == ActiveTool::QuickSelect)) {
                int cx = (int)canvasX;
                int cy = (int)canvasY;
                if (g_LassoPoints.empty() || g_LassoPoints.back() != std::make_pair(cx, cy)) {
                    g_LassoPoints.push_back({ cx, cy });
                    if (g_ActiveTool == ActiveTool::QuickSelect) {
                        g_Canvas.StrokeQuickSelect(g_LassoPoints, g_Brush.radius, ImGui::GetIO().KeyAlt);
                    }
                }
            }

            // End drag handling
            if (g_IsSelectionDragging && (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
                g_IsSelectionDragging = false;
                bool add = selAdd() || (g_ActiveTool == ActiveTool::QuickSelect && !selSub());
                bool subtract = selSub();

                float fx2 = canvasX, fy2 = canvasY;
                if ((g_ActiveTool == ActiveTool::RectSelect || g_ActiveTool == ActiveTool::EllipseSelect)
                    && ImGui::GetIO().KeyShift) {
                    constrainSquare(g_SelectionDragStartX, g_SelectionDragStartY, fx2, fy2);
                }
                int x1 = (int)g_SelectionDragStartX;
                int y1 = (int)g_SelectionDragStartY;
                int x2 = (int)fx2;
                int y2 = (int)fy2;

                if (g_ActiveTool == ActiveTool::RectSelect) {
                    g_Canvas.ApplyRectSelection(x1, y1, x2, y2, add, subtract);
                    g_Canvas.UpdateSelectionMaskTexture(g_pd3dDevice);
                }
                else if (g_ActiveTool == ActiveTool::EllipseSelect) {
                    g_Canvas.ApplyEllipseSelection(x1, y1, x2, y2, add, subtract);
                    g_Canvas.UpdateSelectionMaskTexture(g_pd3dDevice);
                }
                else if (g_ActiveTool == ActiveTool::LassoSelect) {
                    g_Canvas.ApplyLassoSelection(g_LassoPoints, add, subtract);
                    g_Canvas.UpdateSelectionMaskTexture(g_pd3dDevice);
                }
                else if (g_ActiveTool == ActiveTool::SmartSelect) {
                    g_Canvas.ApplySmartSelectSelection(g_pd3dDevice, g_LassoPoints, add, subtract);
                }
                else if (g_ActiveTool == ActiveTool::QuickSelect) {
                    // Skip if already cancelled via Esc
                    if (g_Canvas.IsQuickSelectStrokeActive())
                        g_Canvas.EndQuickSelectStroke(g_pd3dDevice, add, subtract);
                }
                g_LassoPoints.clear();
            }

            // Move/Transform drag update
            if (g_IsMoveDragging) {
                ImVec2 mousePos = ImGui::GetMousePos();
                if (g_ActiveGizmoHandle == TransformGizmoHandle::Move) {
                    int dx = (int)floor(canvasX - g_MoveDragStartX);
                    int dy = (int)floor(canvasY - g_MoveDragStartY);
                    g_Canvas.UpdateMovePixels(g_pd3dDevice, g_MoveAccumulatedOffsetX + dx, g_MoveAccumulatedOffsetY + dy);
                }
                else if (g_ActiveGizmoHandle == TransformGizmoHandle::Rotate) {
                    GizmoScreenGeometry geo;
                    if (GetGizmoGeometry(g_Canvas, canvasToScreen, geo)) {
                        float currentAngle = std::atan2(mousePos.y - geo.center.y, mousePos.x - geo.center.x);
                        float deltaAngle = currentAngle - g_GizmoDragStartMouseAngle;
                        g_Canvas.SetFloatingRotation(g_GizmoDragStartRotation + deltaAngle);
                        g_Canvas.MarkCompositeDirty();
                    }
                }
                else if (g_ActiveGizmoHandle == TransformGizmoHandle::Scale_TL ||
                         g_ActiveGizmoHandle == TransformGizmoHandle::Scale_TR ||
                         g_ActiveGizmoHandle == TransformGizmoHandle::Scale_BR ||
                         g_ActiveGizmoHandle == TransformGizmoHandle::Scale_BL) {
                    GizmoScreenGeometry geo;
                    if (GetGizmoGeometry(g_Canvas, canvasToScreen, geo)) {
                        float currentDist = std::sqrt(distSq(mousePos, geo.center));
                        float factor = currentDist / g_GizmoDragStartDist;
                        g_Canvas.SetFloatingScaleX(g_GizmoDragStartScaleX * factor);
                        g_Canvas.SetFloatingScaleY(g_GizmoDragStartScaleY * factor);
                        g_Canvas.MarkCompositeDirty();
                    }
                }
                else if (g_ActiveGizmoHandle == TransformGizmoHandle::Scale_T ||
                         g_ActiveGizmoHandle == TransformGizmoHandle::Scale_B) {
                    GizmoScreenGeometry geo;
                    if (GetGizmoGeometry(g_Canvas, canvasToScreen, geo)) {
                        float currentDist = std::sqrt(distSq(mousePos, geo.center));
                        float factor = currentDist / g_GizmoDragStartDist;
                        g_Canvas.SetFloatingScaleY(g_GizmoDragStartScaleY * factor);
                        g_Canvas.MarkCompositeDirty();
                    }
                }
                else if (g_ActiveGizmoHandle == TransformGizmoHandle::Scale_L ||
                         g_ActiveGizmoHandle == TransformGizmoHandle::Scale_R) {
                    GizmoScreenGeometry geo;
                    if (GetGizmoGeometry(g_Canvas, canvasToScreen, geo)) {
                        float currentDist = std::sqrt(distSq(mousePos, geo.center));
                        float factor = currentDist / g_GizmoDragStartDist;
                        g_Canvas.SetFloatingScaleX(g_GizmoDragStartScaleX * factor);
                        g_Canvas.MarkCompositeDirty();
                    }
                }
            }

            // Move drag release
            if (g_IsMoveDragging && (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
                g_IsMoveDragging = false;
                if (g_ActiveGizmoHandle == TransformGizmoHandle::Move) {
                    g_MoveAccumulatedOffsetX += (int)floor(canvasX - g_MoveDragStartX);
                    g_MoveAccumulatedOffsetY += (int)floor(canvasY - g_MoveDragStartY);
                }
                g_ActiveGizmoHandle = TransformGizmoHandle::None;
            }

            // Gradient drag release
            if (g_ActiveTool == ActiveTool::Gradient && g_IsGradientDragging && (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
                g_IsGradientDragging = false;
                g_Canvas.ApplyGradient((int)g_SelectionDragStartX, (int)g_SelectionDragStartY, (int)canvasX, (int)canvasY, g_Brush.color, g_SecondaryColor);
            }

            // Draw interactive shape outline during drag/selection
            if (g_IsSelectionDragging || (g_ActiveTool == ActiveTool::Gradient && g_IsGradientDragging) || 
                (g_ActiveTool == ActiveTool::PolygonalLasso && !g_PolygonalLassoPoints.empty())) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(imageMin, ImVec2(imageMin.x + viewportWidth, imageMin.y + viewportHeight), true);

                ImU32 outlineCol = IM_COL32(255, 255, 0, 255);
                float thickness = 2.0f;

                float drawX = canvasX, drawY = canvasY;
                if ((g_ActiveTool == ActiveTool::RectSelect || g_ActiveTool == ActiveTool::EllipseSelect)
                    && ImGui::GetIO().KeyShift) {
                    constrainSquare(g_SelectionDragStartX, g_SelectionDragStartY, drawX, drawY);
                }
                if (g_ActiveTool == ActiveTool::RectSelect) {
                    ImVec2 p1 = canvasToScreen(g_SelectionDragStartX, g_SelectionDragStartY);
                    ImVec2 p2 = canvasToScreen(drawX, g_SelectionDragStartY);
                    ImVec2 p3 = canvasToScreen(drawX, drawY);
                    ImVec2 p4 = canvasToScreen(g_SelectionDragStartX, drawY);
                    dl->AddLine(p1, p2, outlineCol, thickness);
                    dl->AddLine(p2, p3, outlineCol, thickness);
                    dl->AddLine(p3, p4, outlineCol, thickness);
                    dl->AddLine(p4, p1, outlineCol, thickness);
                }
                else if (g_ActiveTool == ActiveTool::EllipseSelect) {
                    float cx = (g_SelectionDragStartX + drawX) * 0.5f;
                    float cy = (g_SelectionDragStartY + drawY) * 0.5f;
                    float rx = std::abs(drawX - g_SelectionDragStartX) * 0.5f;
                    float ry = std::abs(drawY - g_SelectionDragStartY) * 0.5f;
                    const int numSegments = 36;
                    std::vector<ImVec2> pts(numSegments);
                    for (int i = 0; i < numSegments; ++i) {
                        float theta = i * 2.0f * 3.14159265f / numSegments;
                        float px = cx + rx * std::cos(theta);
                        float py = cy + ry * std::sin(theta);
                        pts[i] = canvasToScreen(px, py);
                    }
                    for (int i = 0; i < numSegments; ++i) {
                        dl->AddLine(pts[i], pts[(i + 1) % numSegments], outlineCol, thickness);
                    }
                }
                else if (g_ActiveTool == ActiveTool::LassoSelect || g_ActiveTool == ActiveTool::SmartSelect || g_ActiveTool == ActiveTool::QuickSelect) {
                    if (g_LassoPoints.size() >= 2) {
                        for (size_t i = 0; i < g_LassoPoints.size() - 1; ++i) {
                            dl->AddLine(canvasToScreen((float)g_LassoPoints[i].first, (float)g_LassoPoints[i].second),
                                        canvasToScreen((float)g_LassoPoints[i+1].first, (float)g_LassoPoints[i+1].second),
                                        outlineCol, thickness);
                        }
                        dl->AddLine(canvasToScreen((float)g_LassoPoints.back().first, (float)g_LassoPoints.back().second),
                                    canvasToScreen(canvasX, canvasY),
                                    outlineCol, thickness);
                    }
                }
                else if (g_ActiveTool == ActiveTool::PolygonalLasso) {
                    if (!g_PolygonalLassoPoints.empty()) {
                        for (size_t i = 0; i < g_PolygonalLassoPoints.size() - 1; ++i) {
                            ImVec2 pStart = canvasToScreen((float)g_PolygonalLassoPoints[i].first, (float)g_PolygonalLassoPoints[i].second);
                            ImVec2 pEnd = canvasToScreen((float)g_PolygonalLassoPoints[i+1].first, (float)g_PolygonalLassoPoints[i+1].second);
                            dl->AddLine(pStart, pEnd, outlineCol, thickness);
                            dl->AddCircleFilled(pStart, 3.0f, IM_COL32(255, 255, 255, 255));
                            dl->AddCircle(pStart, 3.0f, IM_COL32(0, 0, 0, 255));
                        }
                        ImVec2 pLast = canvasToScreen((float)g_PolygonalLassoPoints.back().first, (float)g_PolygonalLassoPoints.back().second);
                        dl->AddCircleFilled(pLast, 3.0f, IM_COL32(255, 255, 255, 255));
                        dl->AddCircle(pLast, 3.0f, IM_COL32(0, 0, 0, 255));

                        // Line from last point to current mouse position
                        dl->AddLine(pLast, canvasToScreen(canvasX, canvasY), outlineCol, thickness);
                    }
                }
                else if (g_ActiveTool == ActiveTool::Gradient) {
                    ImVec2 pStart = canvasToScreen(g_SelectionDragStartX, g_SelectionDragStartY);
                    ImVec2 pEnd = canvasToScreen(canvasX, canvasY);
                    dl->AddLine(pStart, pEnd, outlineCol, thickness);
                    dl->AddCircleFilled(pStart, 4.0f, outlineCol);
                    dl->AddCircleFilled(pEnd, 4.0f, outlineCol);
                }

                dl->PopClipRect();
            }

            // Draw Move Pixels Gizmo
            if (g_Canvas.IsMovingPixels()) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(imageMin, ImVec2(imageMin.x + viewportWidth, imageMin.y + viewportHeight), true);
                g_Canvas.DrawMoveGizmo(dl, canvasToScreen);
                dl->PopClipRect();
            }

            // Magic Wand seed crosshair (sticky sample point)
            if (g_ActiveTool == ActiveTool::MagicWand && g_Canvas.HasWandSeed()) {
                int sx = 0, sy = 0;
                g_Canvas.GetWandSeed(sx, sy);
                ImVec2 sp = canvasToScreen((float)sx + 0.5f, (float)sy + 0.5f);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(imageMin, ImVec2(imageMin.x + viewportWidth, imageMin.y + viewportHeight), true);
                const float arm = 8.0f;
                ImU32 colOuter = IM_COL32(0, 0, 0, 200);
                ImU32 colInner = IM_COL32(255, 220, 40, 255);
                dl->AddLine(ImVec2(sp.x - arm, sp.y), ImVec2(sp.x + arm, sp.y), colOuter, 3.0f);
                dl->AddLine(ImVec2(sp.x, sp.y - arm), ImVec2(sp.x, sp.y + arm), colOuter, 3.0f);
                dl->AddLine(ImVec2(sp.x - arm, sp.y), ImVec2(sp.x + arm, sp.y), colInner, 1.5f);
                dl->AddLine(ImVec2(sp.x, sp.y - arm), ImVec2(sp.x, sp.y + arm), colInner, 1.5f);
                dl->AddCircle(sp, 4.0f, colInner, 12, 1.5f);
                dl->PopClipRect();
            }

            // Pipette / eyedropper HUD: sample ring + HEX/RGB chip near cursor
            if (isEyedropperMode && isHovered && isInsideCanvas && s_PipetteHasPreview) {
                ImDrawList* dl = ImGui::GetForegroundDrawList();
                ImVec2 mp = ImGui::GetMousePos();
                const float r = 18.0f;
                ImVec2 ringC(mp.x + 28.f, mp.y + 28.f);
                // Avoid going off viewport edges
                if (ringC.x + 90.f > imageMin.x + viewportWidth) ringC.x = mp.x - 28.f;
                if (ringC.y + 70.f > imageMin.y + viewportHeight) ringC.y = mp.y - 28.f;

                auto& tok = Ui::Tokens();
                float pr = s_PipettePreview[0], pg = s_PipettePreview[1], pb = s_PipettePreview[2], pa = s_PipettePreview[3];
                int Ri = (int)std::lround(std::clamp(pr, 0.f, 1.f) * 255.f);
                int Gi = (int)std::lround(std::clamp(pg, 0.f, 1.f) * 255.f);
                int Bi = (int)std::lround(std::clamp(pb, 0.f, 1.f) * 255.f);
                ImU32 sampleCol = IM_COL32(Ri, Gi, Bi, 255);

                // Checker under circle for alpha context
                const float cell = 5.f;
                for (int cy = -3; cy <= 3; ++cy) {
                    for (int cx = -3; cx <= 3; ++cx) {
                        float dx = (cx + 0.5f) * cell, dy = (cy + 0.5f) * cell;
                        if (dx * dx + dy * dy > (r - 1.f) * (r - 1.f)) continue;
                        bool dark = ((cx + cy) & 1) != 0;
                        dl->AddRectFilled(
                            ImVec2(ringC.x + cx * cell - cell * 0.5f, ringC.y + cy * cell - cell * 0.5f),
                            ImVec2(ringC.x + cx * cell + cell * 0.5f, ringC.y + cy * cell + cell * 0.5f),
                            dark ? IM_COL32(90, 90, 90, 255) : IM_COL32(180, 180, 180, 255));
                    }
                }
                dl->AddCircleFilled(ringC, r, IM_COL32(Ri, Gi, Bi, (int)std::lround(std::clamp(pa, 0.f, 1.f) * 255.f)), 32);
                dl->AddCircle(ringC, r, IM_COL32(0, 0, 0, 200), 32, 2.5f);
                dl->AddCircle(ringC, r, tok.ColU32(tok.strokeActive), 32, 1.25f);

                // Crosshair at sample point (cursor)
                const float arm = 6.f;
                dl->AddLine(ImVec2(mp.x - arm, mp.y), ImVec2(mp.x + arm, mp.y), IM_COL32(0, 0, 0, 180), 2.5f);
                dl->AddLine(ImVec2(mp.x, mp.y - arm), ImVec2(mp.x, mp.y + arm), IM_COL32(0, 0, 0, 180), 2.5f);
                dl->AddLine(ImVec2(mp.x - arm, mp.y), ImVec2(mp.x + arm, mp.y), IM_COL32(255, 255, 255, 230), 1.0f);
                dl->AddLine(ImVec2(mp.x, mp.y - arm), ImVec2(mp.x, mp.y + arm), IM_COL32(255, 255, 255, 230), 1.0f);

                // Info chip: HEX + RGB
                char hex[16], rgbLine[48];
                std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", Ri, Gi, Bi);
                std::snprintf(rgbLine, sizeof(rgbLine), "RGB %d %d %d", Ri, Gi, Bi);
                ImVec2 chipPos(ringC.x + r + 8.f, ringC.y - 14.f);
                ImVec2 hexSz = ImGui::CalcTextSize(hex);
                ImVec2 rgbSz = ImGui::CalcTextSize(rgbLine);
                float chipW = std::max(hexSz.x, rgbSz.x) + 16.f;
                float chipH = hexSz.y + rgbSz.y + 14.f;
                ImVec2 c0 = chipPos;
                ImVec2 c1(c0.x + chipW, c0.y + chipH);
                dl->AddRectFilled(c0, c1, tok.ColU32(ImVec4(tok.bgElevated.x, tok.bgElevated.y, tok.bgElevated.z, 0.92f)), tok.rSm);
                dl->AddRect(c0, c1, tok.ColU32(tok.strokeHairline), tok.rSm, 0, 1.0f);
                // Color swatch strip
                dl->AddRectFilled(ImVec2(c0.x + 4.f, c0.y + 4.f), ImVec2(c0.x + 12.f, c1.y - 4.f), sampleCol, 2.f);
                dl->AddText(ImVec2(c0.x + 16.f, c0.y + 4.f), tok.ColU32(tok.textPrimary), hex);
                dl->AddText(ImVec2(c0.x + 16.f, c0.y + 4.f + hexSz.y + 2.f), tok.ColU32(tok.textSecondary), rgbLine);
            }

            // Draw Smart Select background process progress & cancel option UI
            if (g_Canvas.IsSmartSelectInProgress()) {
                ImGui::SetCursorScreenPos(ImVec2(imageMin.x + 20.0f, imageMin.y + 20.0f));
                ImGui::BeginChild("SmartSelectProgress", ImVec2(320.0f, 90.0f), true, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
                ImGui::Text("Smart Select (GrabCut) is running...");
                float t = (float)fmod(ImGui::GetTime() * 2.0, 100.0) / 100.0f;
                char buf[32];
                sprintf(buf, "Processing...");
                ImGui::ProgressBar(t, ImVec2(-1.0f, 0.0f), buf);
                if (ImGui::Button("Cancel")) {
                    g_Canvas.CancelSmartSelect();
                }
                ImGui::EndChild();
            }

            g_Canvas.Update(viewportWidth, viewportHeight, isHovered, localMouseX, localMouseY, isPanning, dragDx, dragDy, wheelDelta);

            // Brush preset picker (RMB click on Brush/Eraser)
            UI::DrawBrushPickerPopup(s_WantBrushPopup, s_BrushPopupPos, g_Brush);
        }

        ImGui::End();



        // Handle Test Mode Execution: Perform 1 Frame and Exit
        if (testMode) {
            Logger::Get().Info("[TEST] Render completed. Saving config and exiting successfully.");
            ImGui::Render();
            g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
            float clearColor[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
            g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            
            if (!headlessMode) {
                g_pSwapChain->Present(1, 0);
            }
            break;
        }

        // Standard Render Presentation
        ImGui::Render();
        
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        float clearColor[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }

        g_pSwapChain->Present(1, 0); // VSync enabled

        // End frame timing
        auto loopEnd = std::chrono::high_resolution_clock::now();
        uiState.frameTimeMs = std::chrono::duration<float, std::milli>(loopEnd - loopStart).count();
        
        static float frameTimeAccumulator = 0.0f;
        static int frameCount = 0;
        frameTimeAccumulator += uiState.frameTimeMs;
        frameCount++;
        if (frameTimeAccumulator >= 500.0f) { // Update FPS every 500ms
            uiState.fps = 1000.0f / (frameTimeAccumulator / frameCount);
            frameTimeAccumulator = 0.0f;
            frameCount = 0;
        }
    }

    // Delete autosave backup on clean exit
    try {
        std::string bPath = ConfigManager::Get().GetBackupDir() + "/autosave_backup.rayp";
        if (std::filesystem::exists(bPath)) {
            std::filesystem::remove(bPath);
        }
    } catch (...) {}

    // Cleanup Subsystems in reverse order
    g_Canvas.Shutdown();
    CleanupCanvasRenderTarget();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    CleanupRenderTarget();
    CleanupDeviceD3D();
    if (g_OriginalWndProc) {
        SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)g_OriginalWndProc);
        g_OriginalWndProc = nullptr;
    }
    glfwDestroyWindow(window);
    glfwTerminate();

    ScriptingEngine::Get().Shutdown();
    ThreadPool::Get().Shutdown();
    if (processExitCode != 0) {
        Logger::Get().Error("RayVPaint exiting with code " + std::to_string(processExitCode));
    } else {
        Logger::Get().Info("RayVPaint shut down cleanly.");
    }
    Logger::Get().Shutdown();

    return processExitCode;
}

// WinMain entry point for Native WIN32 GUI
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    return main(__argc, __argv);
}

// DirectX 11 Helper Functions

bool CreateDeviceD3D(HWND hWnd, bool useNullDriver) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_DRIVER_TYPE driverType = useNullDriver ? D3D_DRIVER_TYPE_NULL : D3D_DRIVER_TYPE_HARDWARE;
    
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, driverType, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext
    );

    if (FAILED(hr)) return false;
    return true;
}

void CleanupDeviceD3D() {
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    if (!g_pSwapChain) return;
    ID3D11Texture2D* pBackBuffer = nullptr;
    HRESULT hr = g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (SUCCEEDED(hr) && pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

void ResizeCanvasRenderTarget(int width, int height) {
    if (g_canvasTexture && static_cast<int>(g_canvasRTWidth) == width && static_cast<int>(g_canvasRTHeight) == height) {
        return;
    }

    CleanupCanvasRenderTarget();

    g_canvasRTWidth = static_cast<float>(width);
    g_canvasRTHeight = static_cast<float>(height);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = g_pd3dDevice->CreateTexture2D(&desc, nullptr, &g_canvasTexture);
    if (SUCCEEDED(hr)) {
        g_pd3dDevice->CreateRenderTargetView(g_canvasTexture, nullptr, &g_canvasRTV);
        g_pd3dDevice->CreateShaderResourceView(g_canvasTexture, nullptr, &g_canvasSRV);
    }
}

void CleanupCanvasRenderTarget() {
    if (g_canvasTexture) { g_canvasTexture->Release(); g_canvasTexture = nullptr; }
    if (g_canvasRTV) { g_canvasRTV->Release(); g_canvasRTV = nullptr; }
    if (g_canvasSRV) { g_canvasSRV->Release(); g_canvasSRV = nullptr; }
}

void RenderCanvasToTexture(int width, int height) {
    if (!g_canvasRTV) return;

    // Skip canvas rendering if background thread is loading resources
    if (UI::g_LoadingState.isLoading) {
        float clearColor[4] = { 0.12f, 0.12f, 0.14f, 1.0f }; // Slate background matches ImGui Child window
        g_pd3dDeviceContext->ClearRenderTargetView(g_canvasRTV, clearColor);
        return;
    }

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    g_pd3dDeviceContext->RSSetViewports(1, &vp);

    float clearColor[4] = { 0.12f, 0.12f, 0.14f, 1.0f }; // Slate background matches ImGui Child window
    g_pd3dDeviceContext->ClearRenderTargetView(g_canvasRTV, clearColor);

    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_canvasRTV, nullptr);

    g_Canvas.Render(g_pd3dDeviceContext, static_cast<float>(width), static_cast<float>(height));

    ID3D11RenderTargetView* nullRTV = nullptr;
    g_pd3dDeviceContext->OMSetRenderTargets(1, &nullRTV, nullptr);
}

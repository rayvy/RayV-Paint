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
#include <algorithm>
#include <cctype>

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
#include "assets/AssetManager.h"
#include "ConfigManager.h"
#include "ScriptingEngine.h"
#include "Canvas.h"
#include "core/MemoryStats.h"
#include "core/KeymapManager.h"
#include "core/ClipboardHelper.h"
#include "core/BrushLibrary.h"
#include "core/PathUtil.h"
#include "core/ProjectManager.h"
#include "core/SingleInstance.h"
#include "core/BenchmarkRunner.h"
#include "shell/DdsThumbRegister.h"
#include "modio/VertexLayout.h"
#include "preview3d/MeshGpu.h"
#include "ui/EditorPanels.h"
#include "ui/style/UiTokens.h"

// App version (CLI --version, about, logs)
static constexpr const char* kRayVPaintVersion = "0.3.0";

// Tablet / pen support (Pointer API + disable OS press-and-hold dead zone)
float g_PenPressure = 1.0f;
static bool g_IsPenActive = false;
// Barrel / "right click" button on pen (driver-mapped RMB often fails under Windows Ink)
static bool g_PenBarrelDown = false;
static bool g_PenEraserDown = false;
static WNDPROC g_OriginalWndProc = nullptr;

// Tablet PC gesture flags (tpcshrd.h) — disable press-and-hold "ring" that freezes
// pen movement in a small *screen-space* radius until you leave the circle.
#ifndef WM_TABLET_DEFBASE
#define WM_TABLET_DEFBASE                    0x02C0
#endif
#ifndef WM_TABLET_QUERYSYSTEMGESTURESTATUS
#define WM_TABLET_QUERYSYSTEMGESTURESTATUS   (WM_TABLET_DEFBASE + 12)
#endif
#ifndef TABLET_DISABLE_PRESSANDHOLD
#define TABLET_DISABLE_PRESSANDHOLD          0x00000001
#define TABLET_DISABLE_PENTAPFEEDBACK        0x00000008
#define TABLET_DISABLE_PENBARRELFEEDBACK     0x00000010
#define TABLET_DISABLE_FLICKS                0x00010000
#define TABLET_DISABLE_SMOOTHSCROLLING       0x00080000
#define TABLET_DISABLE_FLICKFALLBACKKEYS     0x00100000
#endif

// Window property used by the Tablet PC input service (same flags as gesture query).
#ifndef MICROSOFT_TABLETPENSERVICE_PROPERTY
#define MICROSOFT_TABLETPENSERVICE_PROPERTY  L"MicrosoftTabletPenServiceProperty"
#endif

static DWORD TabletGestureDisableFlags() {
    return TABLET_DISABLE_PRESSANDHOLD
         | TABLET_DISABLE_PENTAPFEEDBACK
         | TABLET_DISABLE_PENBARRELFEEDBACK
         | TABLET_DISABLE_FLICKS
         | TABLET_DISABLE_SMOOTHSCROLLING
         | TABLET_DISABLE_FLICKFALLBACKKEYS;
}

// Call once after HWND exists. Marks this window as a drawing surface so Windows
// does not treat pen as a "press-and-hold → right-click" gesture app.
static void ConfigureWindowForPenDrawing(HWND hWnd) {
    if (!hWnd) return;
    // Prefer SetProp (works even when WM_TABLET_QUERYSYSTEMGESTURESTATUS is not sent).
    SetPropW(hWnd, MICROSOFT_TABLETPENSERVICE_PROPERTY,
             reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(TabletGestureDisableFlags())));

    // Enable all pointer messages (pen/touch) so we get pressure + barrel reliably.
    // EnableMouseInPointer: when TRUE, pen also generates mouse; we still need gestures off.
    // Keep mouse-in-pointer so existing ImGui/GLFW mouse path works.
    EnableMouseInPointer(TRUE);

    // Register as touch window with want-palm so OS palm rejection is less aggressive;
    // does not enable press-and-hold if tablet props above are set.
    RegisterTouchWindow(hWnd, TWF_WANTPALM);

    Logger::Get().InfoTag("input",
        "Pen drawing mode: press-and-hold/flicks disabled for main window");
}

static void UpdatePenStateFromPointer(UINT32 pointerId, bool isUp) {
    if (isUp) {
        g_PenPressure = 1.0f;
        g_IsPenActive = false;
        g_PenBarrelDown = false;
        g_PenEraserDown = false;
        return;
    }
    POINTER_INPUT_TYPE pointerType = PT_POINTER;
    if (!GetPointerType(pointerId, &pointerType) || pointerType != PT_PEN) {
        g_PenPressure = 1.0f;
        g_IsPenActive = false;
        g_PenBarrelDown = false;
        g_PenEraserDown = false;
        return;
    }
    POINTER_PEN_INFO penInfo = {};
    if (!GetPointerPenInfo(pointerId, &penInfo)) return;

    g_IsPenActive = true;
    // pressure is 0..1024 for pen
    g_PenPressure = std::clamp((float)penInfo.pressure / 1024.0f, 0.f, 1.f);

    const UINT32 flags = penInfo.pointerInfo.pointerFlags;
    const UINT32 penFlags = penInfo.penFlags;
    // Barrel button and/or "second button" (right) — driver-mapped RMB often never
    // reaches GLFW; we inject ImGui right-button after NewFrame.
    g_PenBarrelDown =
        ((penFlags & PEN_FLAG_BARREL) != 0) ||
        ((flags & POINTER_FLAG_SECONDBUTTON) != 0);
    g_PenEraserDown = ((penFlags & PEN_FLAG_ERASER) != 0);
}

LRESULT CALLBACK SubclassedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_COPYDATA: {
            // Second instance → open path in this process (new project tab).
            auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lParam);
            std::string path = SingleInstance::ParseCopyData(cds);
            if (!path.empty()) {
                ProjectManager::Get().EnqueueOpenPath(path);
                SingleInstance::FocusWindow(hWnd);
                return TRUE;
            }
            return FALSE;
        }

        // Critical: tell Tablet PC service this is a drawing app — no press-and-hold
        // dead zone (the small screen-space circle that blocks paint until you leave it).
        case WM_TABLET_QUERYSYSTEMGESTURESTATUS:
            return static_cast<LRESULT>(TabletGestureDisableFlags());

        // Swallow OS gesture messages that can eat pen input
        case WM_GESTURE:
        case WM_GESTURENOTIFY:
            return 0;

        case WM_LBUTTONDOWN:
        case WM_MOUSEMOVE: {
            // Only reset if this is a genuine mouse event, not a synthesized one from a pen/tablet
            // (pen-synthesized mouse uses signature 0xFF515700 in GetMessageExtraInfo)
            if ((GetMessageExtraInfo() & 0xFFFFFF00) != 0xFF515700) {
                g_PenPressure = 1.0f;
                g_IsPenActive = false;
                g_PenBarrelDown = false;
                g_PenEraserDown = false;
            }
            break;
        }
        case WM_POINTERDOWN:
        case WM_POINTERUPDATE: {
            UpdatePenStateFromPointer(GET_POINTERID_WPARAM(wParam), /*isUp=*/false);
            break;
        }
        case WM_POINTERUP:
        case WM_POINTERCAPTURECHANGED: {
            UpdatePenStateFromPointer(GET_POINTERID_WPARAM(wParam), /*isUp=*/true);
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

// Active project canvas (ProjectManager owns N Canvas instances — one D3D device).
static Canvas& ActiveCanvas() {
    return ProjectManager::Get().ActiveCanvas();
}
static HANDLE g_SingleInstanceMutex = nullptr;
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
        ActiveCanvas().PaintOnActiveLayer(0, 0, StrokePhase::End, g_Brush);
        g_IsPainting = false;
        // Double-refresh layer thumbs (preview can lag one frame behind GPU upload)
        g_LayerPreviewRefreshFrames = 2;
        ActiveCanvas().MarkCompositeDirty();
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
// Free Transform operator (Ctrl+T): full gizmo + Enter confirm; restores previous tool.
// Move tool (V): translation only; commits on defocus / tool switch (no Enter required).
static bool g_FreeTransformMode = false;
static ActiveTool g_ToolBeforeFreeTransform = ActiveTool::Brush;
// Warp operators (perspective / mesh) — control drag state
static int g_WarpDragIndex = -1;
static ActiveTool g_ToolBeforeWarp = ActiveTool::Brush;
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
static float g_GizmoDragStartDistX = 5.0f;
static float g_GizmoDragStartDistY = 5.0f;

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
static void OpenPathAsNewProject(const std::string& path) {
    if (path.empty() || !g_pd3dDevice) return;
    const int id = ProjectManager::Get().ActivateOrPrepareOpen(path);
    if (id < 0) return;
    Project* proj = ProjectManager::Get().FindProject(id);
    if (!proj || !proj->canvas) return;
    // Already open & loaded → just switched.
    if (!proj->IsBlank() && !proj->canvas->GetCurrentProjectFilePath().empty()) {
        std::string a = PathUtil::NormalizeToUtf8Path(proj->canvas->GetCurrentProjectFilePath());
        std::string b = PathUtil::NormalizeToUtf8Path(path);
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            return s;
        };
        if (lower(a) == lower(b))
            return;
    }
    UI::TriggerBackgroundOpenDocument(path, g_pd3dDevice, *proj->canvas);
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

    // .rayp is always a full project document — new tab (or activate existing).
    if (ext == "rayp") {
        OpenPathAsNewProject(path);
        return;
    }

    // Drop onto viewport/layers → import into *current* project as image layer.
    if (g_IsViewportHovered || g_IsLayersHovered) {
        UI::TriggerBackgroundOpenDocument(path, g_pd3dDevice, ActiveCanvas());
        return;
    }

    // Drop elsewhere → new project tab (Photoshop-like).
    OpenPathAsNewProject(path);
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
    ActiveCanvas().ResizeCanvas(g_pd3dDevice, w, h);
    Logger::Get().Info("Canvas resized to: " + std::to_string(w) + "x" + std::to_string(h));
}
float GetCanvasZoom() { return ActiveCanvas().GetZoom(); }
void SetCanvasZoom(float zoom) { ActiveCanvas().SetZoom(zoom); }
void SetCanvasPan(float x, float y) { ActiveCanvas().SetPan(DirectX::XMFLOAT2(x, y)); }
void ResetCanvasView() { ActiveCanvas().ResetView(); }
bool LoadCanvasImage(const std::string& filepath) {
    // Python / automation: open documents by type (.rayp or image).
    return ActiveCanvas().OpenDocument(g_pd3dDevice, filepath);
}
int GetCanvasWidth() { return ActiveCanvas().GetWidth(); }
int GetCanvasHeight() { return ActiveCanvas().GetHeight(); }
size_t GetActiveLayerTileCount() { return ActiveCanvas().GetActiveLayerTileCount(); }
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
    return ActiveCanvas().SaveCanvas(filepath, fmt);
}
bool SaveCanvasStandard(const std::string& filepath, const std::string& iccProfilePath) {
    return ActiveCanvas().SaveCanvasStandard(filepath, iccProfilePath);
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
    bool benchmarkMode = false;
    bool perfMode = false; // uncapped Present(0) — prefer for FPS testing / heavy docs
    bool allowMultiInstance = false;
    std::string scriptPath = "";
    std::string configPath = "";
    std::string startupImagePath = "";
    std::string parseModIniPath = "";
    std::string testAdvancedImportFolder = "";
    std::string testAdvancedBase = "";
    std::string testAdvancedOut = "";
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
        } else if (arg == "--benchmark") {
            // Interactive 8K stress suite (FPS / tiling / undo memory). Visible window.
            benchmarkMode = true;
            forceConsole = true;
            allowMultiInstance = true;
            BenchmarkRunner::Get().Enable(true);
        } else if (arg == "--perf") {
            // Uncapped swap chain present (no VSync). Also used when window unfocused.
            perfMode = true;
        } else if (arg == "--headless") {
            headlessMode = true;
            testMode = true; // Headless implies auto-testing behavior
        } else if (arg == "--allow-multi-instance") {
            allowMultiInstance = true;
        } else if (arg == "--console") {
            forceConsole = true;
        } else if (arg == "--parse-mod-ini" && i + 1 < argc) {
            parseModIniPath = argv[++i];
            headlessMode = true;
            testMode = true;
            forceConsole = true;
            allowMultiInstance = true;
        } else if (arg == "--test-advanced-import" && i + 1 < argc) {
            // Create Advanced multi-map project from folder of BelleHairA* textures, export, exit.
            testAdvancedImportFolder = argv[++i];
            headlessMode = true;
            testMode = true;
            forceConsole = true;
            allowMultiInstance = true;
        } else if (arg == "--test-advanced-base" && i + 1 < argc) {
            testAdvancedBase = argv[++i];
        } else if (arg == "--test-advanced-out" && i + 1 < argc) {
            testAdvancedOut = argv[++i];
        } else if (arg == "--version") {
            SetupConsole(true);
            std::cout << "RayV Paint - Version " << kRayVPaintVersion << std::endl;
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
    if (testMode || headlessMode || benchmarkMode) {
        forceConsole = true;
        // Tests/headless may run parallel processes — allow multi unless user wants single.
        allowMultiInstance = true;
    }

    SetupConsole(forceConsole);

    // Photoshop-style single instance (one D3D11 device). Second launch forwards paths & exits.
    if (!SingleInstance::GuardStartup(argc, argv, allowMultiInstance, g_SingleInstanceMutex)) {
        return 0;
    }

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

    // Brush presets: builtins instantly; custom *.rvbrush scan on background thread
    BrushLibrary::Get().LoadBuiltins();
    BrushLibrary::Get().StartAsyncDiskLoad();
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

    assets::AssetManager::Get().Startup();
    log_step("AssetManager");

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
    GLFWwindow* window = glfwCreateWindow(1280, 720,
        benchmarkMode ? "RayV Paint — BENCHMARK" : "RayV Paint", nullptr, nullptr);
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

    // Subclass window procedure for high-precision pointer/tablet messages + single-instance IPC
    g_OriginalWndProc = (WNDPROC)SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)SubclassedWndProc);
    // Disable Windows press-and-hold / flicks for this HWND (Photoshop-like pen drawing).
    // Without this, pen tip freezes inside a small *screen-space* circle until moved out.
    ConfigureWindowForPenDrawing(hWnd);
    SingleInstance::RegisterMainWindow(hWnd);

    // Explorer DDS previews: register COM thumbnail handler under HKCU (no admin).
    // Replaces the default-app icon with real image thumbs (Paint.NET used to own this).
    if (!headlessMode && !testMode) {
        if (DdsThumbRegister::EnsureRegistered())
            Logger::Get().Info("DDS Explorer thumbnails: registered/OK");
        else
            Logger::Get().Warn("DDS Explorer thumbnails: handler DLL missing or reg failed "
                               "(build RayVPaint_DdsThumb and restart)");
    }

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

    // 8. ProjectManager — first empty project + Canvas GPU init (one device for all tabs)
    if (!ProjectManager::Get().Initialize(g_pd3dDevice)) {
        Logger::Get().Error("Failed to initialize ProjectManager / Canvas renderer");
        CleanupDeviceD3D();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    log_step("ProjectManager + Canvas Renderer D3D Init");

    // Check for crash recovery / autosave restore
    std::string backupDir = ConfigManager::Get().GetBackupDir();
    std::string backupPath = backupDir + "/autosave_backup.rayp";
    bool showRecoveryModal = std::filesystem::exists(backupPath);

    // Load startup document if specified on CLI (image or .rayp project) → new/reuse project tab
    if (!startupImagePath.empty()) {
        if (!scriptPath.empty()) {
            // Headless/automation: synchronous open into prepared project tab
            ProjectManager::Get().ActivateOrPrepareOpen(startupImagePath);
            if (!ActiveCanvas().OpenDocument(g_pd3dDevice, startupImagePath)) {
                Logger::Get().Error("Failed to open startup document: " + startupImagePath);
            }
        } else {
            OpenPathAsNewProject(startupImagePath);
        }
    }

    // ---- CLI proof: Advanced multi-map import + batch export ----
    if (!testAdvancedImportFolder.empty() || !testAdvancedBase.empty()) {
        SetupConsole(true);
        std::string base = testAdvancedBase;
        if (base.empty()) {
            // Prefer *Diffuse.dds then *_Diffuse.png in folder
            namespace fs = std::filesystem;
            try {
                fs::path dir = PathUtil::FromUtf8(testAdvancedImportFolder);
                std::string bestDds, bestPng;
                for (auto& ent : fs::directory_iterator(dir)) {
                    if (!ent.is_regular_file()) continue;
                    std::string fn = PathUtil::WideToUtf8(ent.path().filename().wstring());
                    std::string low = fn;
                    std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                    std::string full = PathUtil::WideToUtf8(ent.path().wstring());
                    if (low.find("diffuse") == std::string::npos) continue;
                    if (low.size() >= 4 && low.substr(low.size()-4) == ".dds") bestDds = full;
                    if (low.size() >= 4 && low.substr(low.size()-4) == ".png") bestPng = full;
                }
                base = !bestDds.empty() ? bestDds : bestPng;
            } catch (const std::exception& e) {
                Logger::Get().Error(std::string("test-advanced-import scan failed: ") + e.what());
            }
        }
        if (base.empty()) {
            Logger::Get().Error("--test-advanced-import: no Diffuse texture found");
            std::cerr << "FAIL: no Diffuse base\n";
            processExitCode = 2;
        } else {
            Project* proj = ProjectManager::Get().ActiveProject();
            if (!proj) {
                processExitCode = 3;
            } else {
                Logger::Get().Info("TEST Advanced import base=" + base);
                int n = proj->SetupAdvancedFromBaseTexture(g_pd3dDevice, base, "ZZZ", "CLI_Test");
                auto ptype = ActiveCanvas().GetProjectType();
                int w = ActiveCanvas().GetWidth();
                int h = ActiveCanvas().GetHeight();
                size_t layers = ActiveCanvas().GetLayers().size();
                int enabledMaps = 0;
                if (texset::TextureSet* set = proj->textureSets.Active()) {
                    for (const auto& m : set->maps)
                        if (m.enabled) ++enabledMaps;
                }
                // Content checks: tile presence + LightMap composite variance (Diffuse can be pure white intentionally)
                bool hasContent = false;
                float minL = 1.f, maxL = 0.f;
                size_t diffuseTiles = 0;
                if (layers > 0) {
                    const auto& L = ActiveCanvas().GetLayers()[0];
                    if (L.tileCache) {
                        diffuseTiles = L.tileCache->GetTileCount();
                        // scan first few tiles for any non-uniform bytes
                        for (int ty = 0; ty < L.tileCache->GetTilesY() && ty < 4; ++ty) {
                            for (int tx = 0; tx < L.tileCache->GetTilesX() && tx < 4; ++tx) {
                                const uint8_t* td = L.tileCache->GetTileData(tx, ty);
                                if (!td) continue;
                                uint8_t lo = 255, hi = 0;
                                for (int i = 0; i < 256 * 4; i += 4) {
                                    lo = std::min(lo, td[i]);
                                    hi = std::max(hi, td[i]);
                                }
                                if (hi > lo) { hasContent = true; break; }
                            }
                            if (hasContent) break;
                        }
                        // all-white diffuse is still valid content if tiles exist
                        if (diffuseTiles > 0) hasContent = true;
                    }
                }
                // LightMap content = layers bound to LightMap
                for (const auto& L : ActiveCanvas().GetLayers()) {
                    if (!L.workSpace.AffectsMap(texset::MapKind::LightMap) || !L.tileCache) continue;
                    for (int y = 0; y < ActiveCanvas().GetHeight(); y += 128) {
                        for (int x = 0; x < ActiveCanvas().GetWidth(); x += 128) {
                            float c[4];
                            L.tileCache->GetPixelF(x, y, c);
                            float lum = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
                            minL = std::min(minL, lum);
                            maxL = std::max(maxL, lum);
                        }
                    }
                }
                Logger::Get().Info("Diffuse tiles=" + std::to_string(diffuseTiles) +
                    " LightMap lum=[" + std::to_string(minL) + ".." + std::to_string(maxL) + "]");

                std::string outDir = testAdvancedOut;
                if (outDir.empty())
                    outDir = testAdvancedImportFolder.empty()
                        ? (std::filesystem::temp_directory_path() / "rayv_adv_export").string()
                        : (testAdvancedImportFolder + "/_rayv_export_test");
                try {
                    std::filesystem::create_directories(PathUtil::FromUtf8(outDir));
                } catch (...) {}
                int written = proj->QuickExportAllMaps(outDir);

                std::cout << "=== test-advanced-import ===\n";
                std::cout << "base: " << base << "\n";
                std::cout << "maps_loaded: " << n << "\n";
                std::cout << "project_type: " << (int)ptype << " (1=Advanced)\n";
                std::cout << "canvas: " << w << "x" << h << " layers=" << layers << "\n";
                std::cout << "enabled_maps: " << enabledMaps << "\n";
                std::cout << "diffuse_tiles: " << diffuseTiles << "\n";
                std::cout << "lightmap_lum: [" << minL << ".." << maxL << "]\n";
                std::cout << "has_content: " << (hasContent ? "yes" : "NO") << "\n";
                std::cout << "export_written: " << written << " dir=" << outDir << "\n";

                bool lmVar = (maxL - minL) > 0.01f;
                std::cout << "layers: " << layers << "\n";
                bool ok = (n >= 4) &&
                          (ptype == Canvas::ProjectType::Advanced) &&
                          (w > 0 && h > 0 && layers >= 4) && // each map = a real layer
                          (diffuseTiles > 0) &&
                          hasContent &&
                          lmVar &&
                          (written >= 4) &&
                          (enabledMaps >= 4);
                if (ok) {
                    std::cout << "RESULT: PASS\n";
                    processExitCode = 0;
                } else {
                    std::cout << "RESULT: FAIL\n";
                    processExitCode = 1;
                }
            }
        }
        // Exit after test (no UI loop)
        ProjectManager::Get().Shutdown();
        CleanupDeviceD3D();
        glfwDestroyWindow(window);
        glfwTerminate();
        return processExitCode;
    }

    // Smoke: parse XXMI/3DMigoto character ini (+ optional mesh decode)
    if (!parseModIniPath.empty()) {
        ActiveCanvas().SetProjectType(Canvas::ProjectType::AdvancedModMode);
        ActiveCanvas().SetModIniPath(parseModIniPath);
        // Optional: --parse-mod-ini path -- also try sibling DUMP via env-less default
        bool ok = ActiveCanvas().ApplyModIniParse();
        const std::string summary = ActiveCanvas().GetModParseSummary();
        Logger::Get().Info("---- ModIni parse summary ----\n" + summary);
        std::cout << summary << std::endl;
        if (!ok) {
            Logger::Get().Error("ModIni parse returned failure");
            processExitCode = 3;
        } else {
            const auto& sc = ActiveCanvas().GetModScene();
            if (sc.components.empty() && sc.PartCount() == 0) {
                Logger::Get().Error("ModIni parse OK but empty scene");
                processExitCode = 4;
            } else {
                // Layout smoke: texcoord must have roles; UV_Outline should exist for ZZZ presets
                if (!sc.components.empty()) {
                    const auto& tl = sc.components[0].texcoordLayout;
                    std::cout << modio::FormatLayoutSummary(tl);
                    Logger::Get().Info(modio::FormatLayoutSummary(tl));
                    bool hasUV0 = tl.HasRole(modio::AttrRole::UV0);
                    if (!hasUV0)
                        Logger::Get().Warn("Layout has no UV0 role — check dump/manual mapping");
                }
                // Mesh decode smoke (CPU) for first geo part
                for (const auto& c : sc.components) {
                    for (const auto& p : c.parts) {
                        if (!p.hasGeometry) continue;
                        std::vector<preview3d::PreviewVertex> verts;
                        std::string err;
                        if (preview3d::DecodeComponentVertices(c, verts, err)) {
                            Logger::Get().Info("Mesh decode " + c.name + ": " +
                                std::to_string(verts.size()) + " verts OK");
                            std::cout << "Mesh decode " << c.name << ": " << verts.size() << " verts\n";
                        } else {
                            Logger::Get().Warn("Mesh decode " + c.name + ": " + err);
                        }
                        goto mesh_done;
                    }
                }
            mesh_done:
                Logger::Get().Info("ModIni parse smoke PASS");
            }
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
    // Recovery modal blocks input; skip in automated benchmark.
    if (benchmarkMode)
        uiState.showRecoveryModal = false;

    auto lastAutoSaveTime = std::chrono::steady_clock::now();

    if (benchmarkMode) {
        Logger::Get().InfoTag("bench", "Starting interactive 8K benchmark mode");
        BenchmarkRunner::Get().Start(ActiveCanvas(), g_pd3dDevice);
    }

    // 9. Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Merge async brush disk scan (non-blocking)
        BrushLibrary::Get().PollAsyncDiskLoad();
        assets::AssetManager::Get().Poll(g_pd3dDevice, 4.0);

        // Paths from second instance (WM_COPYDATA) → new project tabs
        ProjectManager::Get().DrainPendingOpens(
            [](const std::string& path, Canvas& canvas) {
                if (g_pd3dDevice)
                    UI::TriggerBackgroundOpenDocument(path, g_pd3dDevice, canvas);
            });

        // Handle completed background loading
        if (UI::g_LoadingState.isLoading && UI::g_LoadingState.completed) {
            UI::g_LoadingState.isLoading = false;
            ActiveCanvas().MarkCompositeDirty();
            if (UI::g_LoadingState.success) {
                Logger::Get().Info("Background document load successful: " + UI::g_LoadingState.filepath);
                // Plan 0: restore Texture Set library from .rayp meta / sync Diffuse size
                if (Project* proj = ProjectManager::Get().ActiveProject())
                    proj->ApplyTextureSetsFromCanvas();
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
        // Pen barrel / second button → ImGui RMB. Driver-mapped "right click" often never
        // reaches GLFW under Windows Ink; inject edge-triggered button events after GLFW backend.
        {
            static bool s_PrevPenBarrel = false;
            ImGuiIO& ioPen = ImGui::GetIO();
            const bool wantRmb = g_IsPenActive && g_PenBarrelDown;
            if (wantRmb != s_PrevPenBarrel) {
                ioPen.AddMouseButtonEvent(ImGuiMouseButton_Right, wantRmb);
                s_PrevPenBarrel = wantRmb;
            }
        }
        ImGui::NewFrame();
        g_IsViewportHovered = false;
        g_IsLayersHovered = false;

        // Layer preview double-refresh (paint/edit may leave thumbs one frame stale)
        if (g_LayerPreviewRefreshFrames > 0) {
            ActiveCanvas().MarkCompositeDirty();
            --g_LayerPreviewRefreshFrames;
        }

        // Render all UI Panels and Modals (Toolbar, Properties, Layers, Brush settings, Console logs, Colors)
        UI::RenderAll(uiState, ActiveCanvas(), g_Brush, g_ActiveTool, g_pd3dDevice, g_pd3dDeviceContext, window);

        // Keyboard Shortcuts Handler (Layout-Independent via KeymapManager)
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput) {
            if (KeymapManager::Get().ConsumeActionTrigger("Undo")) {
                ActiveCanvas().Undo();
            }
            if (KeymapManager::Get().ConsumeActionTrigger("Redo")) {
                ActiveCanvas().Redo();
            }
            if (KeymapManager::Get().ConsumeActionTrigger("SaveProject")) {
                UI::FileExplorerOpen(uiState.fileExplorer, UI::FileExplorerMode::SaveProject);
            }
            if (KeymapManager::Get().ConsumeActionTrigger("OpenProject")) {
                UI::FileExplorerOpen(uiState.fileExplorer, UI::FileExplorerMode::OpenProject);
            }
            if (KeymapManager::Get().ConsumeActionTrigger("NewProject")) {
                uiState.openNewProjectWizard = true;
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
                // V = Move tool (pixels only)
                g_FreeTransformMode = false;
                g_ActiveTool = ActiveTool::MovePixels;
            }
            auto enterFreeTransform = [&]() {
                if (!g_FreeTransformMode) {
                    g_ToolBeforeFreeTransform = g_ActiveTool;
                    if (g_ToolBeforeFreeTransform == ActiveTool::MovePixels)
                        g_ToolBeforeFreeTransform = ActiveTool::Brush;
                }
                g_FreeTransformMode = true;
                g_ActiveTool = ActiveTool::MovePixels;
                if (!ActiveCanvas().IsMovingPixels())
                    ActiveCanvas().StartMovePixels(g_pd3dDevice);
            };
            if (KeymapManager::Get().ConsumeActionTrigger("FreeTransform") || uiState.requestFreeTransform) {
                uiState.requestFreeTransform = false;
                enterFreeTransform();
            }
            uiState.freeTransformActive = g_FreeTransformMode;
            if (KeymapManager::Get().ConsumeActionTrigger("SmudgeTool")) {
                g_ActiveTool = ActiveTool::Smudge;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("BlurTool")) {
                g_ActiveTool = ActiveTool::BlurTool;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("StampTool")) {
                g_ActiveTool = ActiveTool::Stamp;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("ContentAwareFill") || uiState.requestContentAwareFill) {
                uiState.requestContentAwareFill = false;
                ActiveCanvas().ApplyContentAwareFill(g_pd3dDevice);
            }
            if (KeymapManager::Get().ConsumeActionTrigger("RefreshCanvas")) {
                ActiveCanvas().RefreshCanvas(g_pd3dDevice);
            }
            if (KeymapManager::Get().ConsumeActionTrigger("PerspectiveWarp") || uiState.requestPerspectiveWarp) {
                uiState.requestPerspectiveWarp = false;
                g_ToolBeforeWarp = g_ActiveTool;
                ActiveCanvas().StartWarpOperator(g_pd3dDevice, Canvas::WarpOperatorMode::Perspective);
                g_WarpDragIndex = -1;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("MeshWarp") || uiState.requestMeshWarp) {
                uiState.requestMeshWarp = false;
                g_ToolBeforeWarp = g_ActiveTool;
                ActiveCanvas().StartWarpOperator(g_pd3dDevice, Canvas::WarpOperatorMode::Mesh);
                g_WarpDragIndex = -1;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("SelectAll")) {
                ActiveCanvas().SelectAll();
            }
            if (KeymapManager::Get().ConsumeActionTrigger("DuplicateLayer")) {
                if (!uiState.selectedLayers.empty()) {
                    ActiveCanvas().DuplicateLayers(g_pd3dDevice, uiState.selectedLayers);
                    // Refresh selection to new clones is best-effort: leave active as set by core
                    uiState.selectedLayers.clear();
                    if (ActiveCanvas().GetActiveLayerIndex() >= 0)
                        uiState.selectedLayers.push_back(ActiveCanvas().GetActiveLayerIndex());
                } else if (ActiveCanvas().GetActiveLayerIndex() >= 0) {
                    int neu = ActiveCanvas().DuplicateLayer(g_pd3dDevice, ActiveCanvas().GetActiveLayerIndex());
                    uiState.selectedLayers.clear();
                    if (neu >= 0) uiState.selectedLayers.push_back(neu);
                }
            }
            if (KeymapManager::Get().ConsumeActionTrigger("CropToSelection")) {
                if (ActiveCanvas().HasSelection()) {
                    ActiveCanvas().CropCanvasToSelection(g_pd3dDevice);
                }
            }
            if (KeymapManager::Get().ConsumeActionTrigger("InvertSelection")) {
                ActiveCanvas().InvertSelection();
            }
            if (KeymapManager::Get().ConsumeActionTrigger("InvertColors")) {
                ActiveCanvas().InvertColors();
            }
            if (KeymapManager::Get().ConsumeActionTrigger("InvertAlpha")) {
                ActiveCanvas().InvertAlpha();
            }
            if (KeymapManager::Get().ConsumeActionTrigger("AdjustHSV")) {
                uiState.showHSVModal = true;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("AdjustCurves")) {
                uiState.showCurvesModal = true;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("AdjustBlur")) {
                uiState.showBlurModal = true;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("AdjustNoise")) {
                uiState.showNoiseModal = true;
            }
            // Ctrl+Shift+E: Advanced → Batch Export folder FE; Simple → Advanced Export FE
            if (KeymapManager::Get().ConsumeActionTrigger("AdvancedExport")) {
                const bool advanced =
                    ActiveCanvas().GetProjectType() != Canvas::ProjectType::Simple;
                if (advanced)
                    UI::FileExplorerOpen(uiState.fileExplorer, UI::FileExplorerMode::ExportTemplate);
                else
                    UI::FileExplorerOpen(uiState.fileExplorer, UI::FileExplorerMode::AdvancedExport);
            }

            if (KeymapManager::Get().ConsumeActionTrigger("QuickExport") || uiState.openQuickExportTrigger) {
                uiState.openQuickExportTrigger = false;
                const bool advanced =
                    ActiveCanvas().GetProjectType() != Canvas::ProjectType::Simple;

                // Advanced+: batch export all enabled maps with channel packing
                if (advanced) {
                    if (Project* proj = ProjectManager::Get().ActiveProject()) {
                        int n = proj->QuickExportAllMaps();
                        if (n > 0)
                            Logger::Get().Info("Batch export: " + std::to_string(n) + " map(s) written");
                        else
                            Logger::Get().Error("Batch export: no maps written");
                    }
                } else {
                    // Simple: single file export via hard container (DDS/PNG)
                    std::string used;
                    if (ActiveCanvas().ExportWithProjectSettings(&used))
                        Logger::Get().Info("Quick exported: " + used);
                    else
                        Logger::Get().Error("Quick export failed: " +
                            (used.empty() ? ActiveCanvas().GetExportPath() : used));
                }
            }
            if (KeymapManager::Get().ConsumeActionTrigger("AdvancedExport")) {
                uiState.openExportAdvancedModal = true;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("FillSecondary")) {
                if (ActiveCanvas().HasSelection() || ActiveCanvas().GetActiveLayerIndex() >= 0)
                    ActiveCanvas().FillSelection(g_SecondaryColor);
            }
            if (KeymapManager::Get().ConsumeActionTrigger("DeleteContent")) {
                ActiveCanvas().DeleteSelectionContent();
            }
            if (KeymapManager::Get().ConsumeActionTrigger("CopyLayers")) {
                std::vector<int> idxs = uiState.selectedLayers;
                if (idxs.empty() && ActiveCanvas().GetActiveLayerIndex() >= 0)
                    idxs.push_back(ActiveCanvas().GetActiveLayerIndex());
                ActiveCanvas().CopyLayersToClipboard(idxs);
            }
            if (KeymapManager::Get().ConsumeActionTrigger("Copy")) {
                // Selection or active layer content → system + internal content clipboard
                if (!ActiveCanvas().CopyContentToClipboard()) {
                    // Fallback: merged composite (legacy)
                    std::vector<float> composite = ActiveCanvas().GetCompositePixels();
                    ClipboardHelper::CopyImageToClipboard(composite, ActiveCanvas().GetWidth(), ActiveCanvas().GetHeight());
                }
            }
            if (KeymapManager::Get().ConsumeActionTrigger("PasteAsNewLayer")) {
                if (!ActiveCanvas().PasteContentAsNewLayer(g_pd3dDevice, "Pasted Layer"))
                    Logger::Get().Warn("PasteAsNewLayer: no image on clipboard");
            }
            if (KeymapManager::Get().ConsumeActionTrigger("Paste")) {
                // External image (Chrome/Blender/PS PNG) takes priority over internal
                // layer/content clipboard when the system clipboard was overwritten.
                const bool externalImage =
                    ClipboardHelper::HasClipboardImage() &&
                    ClipboardHelper::IsSystemClipboardNewerThanLastCopy();

                if (externalImage) {
                    // Mask paint target → stamp into mask (UV layout → mask workflow).
                    // Otherwise always new layer so transparency is preserved cleanly.
                    if (ActiveCanvas().IsEditingLayerMask()) {
                        if (!ActiveCanvas().PasteContentIntoActive(g_pd3dDevice))
                            Logger::Get().Warn("Paste: failed to paste into mask");
                    } else if (!ActiveCanvas().PasteContentAsNewLayer(g_pd3dDevice, "Pasted Layer")) {
                        Logger::Get().Warn("Paste: failed to paste system image as layer");
                    }
                } else if (ActiveCanvas().HasLayerClipboard()) {
                    ActiveCanvas().PasteLayersFromClipboard(g_pd3dDevice);
                } else if (!ActiveCanvas().PasteContentIntoActive(g_pd3dDevice)) {
                    // Fallback: system image as new layer (first paste / no internal content)
                    if (!ActiveCanvas().PasteContentAsNewLayer(g_pd3dDevice, "Pasted Layer"))
                        Logger::Get().Warn("Paste: clipboard has no pasteable image");
                }
            }
        }

        // Background Auto-Save trigger (disabled during benchmark — I/O noise)
        static bool s_IsAutoSaving = false;
        int autoSaveInterval = benchmarkMode ? 0 : ConfigManager::Get().GetAutoSaveIntervalMinutes();
        if (autoSaveInterval > 0 && ActiveCanvas().IsDocumentModified() && !s_IsAutoSaving) {
            auto currentTime = std::chrono::steady_clock::now();
            auto elapsedMinutes = std::chrono::duration_cast<std::chrono::minutes>(currentTime - lastAutoSaveTime).count();
            if (elapsedMinutes >= autoSaveInterval) {
                s_IsAutoSaving = true;
                std::filesystem::create_directories(uiState.backupDir);
                Logger::Get().Info("Triggering background auto-save to " + uiState.backupPath);
                if (Project* proj = ProjectManager::Get().ActiveProject())
                    proj->InjectTextureSetsIntoCanvas();
                ActiveCanvas().SaveCanvasRaypAsync(uiState.backupPath, [](bool success) {
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

        // Benchmark steps before canvas compose so stroke/tile work is in this frame's cost.
        // metric uses previous frame's work time (human-ish dt for path advance).
        static float s_benchPrevWorkMs = 16.0f;
        if (benchmarkMode && BenchmarkRunner::Get().IsActive()) {
            const float metricMs = s_benchPrevWorkMs > 0.5f ? s_benchPrevWorkMs : 16.0f;
            if (!BenchmarkRunner::Get().Tick(ActiveCanvas(), g_pd3dDevice, metricMs)) {
                processExitCode = BenchmarkRunner::Get().ExitCode();
                Logger::Get().InfoTag("bench",
                    "Benchmark finished mid-frame, will exit after present. code=" +
                    std::to_string(processExitCode));
            }
        }

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
            float screenOriginX = std::floor(ActiveCanvas().GetPan().x + static_cast<float>(viewportWidth) * 0.5f);
            float screenOriginY = std::floor(ActiveCanvas().GetPan().y + static_cast<float>(viewportHeight) * 0.5f);

            float rotatedX = (localMouseX - screenOriginX) / ActiveCanvas().GetZoom();
            float rotatedY = (localMouseY - screenOriginY) / ActiveCanvas().GetZoom();

            float angle = ActiveCanvas().GetRotationAngle();
            float cosA = std::cos(angle);
            float sinA = std::sin(angle);
            
            float centerX = ActiveCanvas().GetWidth() * 0.5f;
            float centerY = ActiveCanvas().GetHeight() * 0.5f;
            
            float relX = rotatedX - centerX;
            float relY = rotatedY - centerY;
            
            float canvasX = relX * cosA + relY * sinA + centerX;
            float canvasY = -relX * sinA + relY * cosA + centerY;

            if (ActiveCanvas().GetViewportFlipH()) {
                canvasX = (float)ActiveCanvas().GetWidth() - canvasX;
            }
            if (ActiveCanvas().GetViewportFlipV()) {
                canvasY = (float)ActiveCanvas().GetHeight() - canvasY;
            }

            // Check if cursor is within active canvas boundary
            bool isInsideCanvas = (canvasX >= 0.0f && canvasX < (float)ActiveCanvas().GetWidth() &&
                                   canvasY >= 0.0f && canvasY < (float)ActiveCanvas().GetHeight());

            // Smudge is NOT brush-like: must not paint with brush.color / accent color.
            // Stamp is brush-like for size/hardness, but Alt = set clone source (not pipette).
            bool isStampTool = (g_ActiveTool == ActiveTool::Stamp);
            bool isBrushLikeTool = (g_ActiveTool == ActiveTool::Brush || g_ActiveTool == ActiveTool::Eraser || isStampTool);
            bool isSmudgeTool = (g_ActiveTool == ActiveTool::Smudge || g_ActiveTool == ActiveTool::BlurTool);
            bool isPipetteTool = (g_ActiveTool == ActiveTool::Pipette);
            // Alt-sample blocked when Ctrl held (future: Ctrl+Alt+LMB = brush rotation)
            // Stamp: Alt does NOT open pipette — sets clone source instead.
            bool isEyedropperMode = (((isBrushLikeTool && !isStampTool) || isSmudgeTool) && ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeyCtrl)
                                    || isPipetteTool;

            if (g_ActiveTool != g_PrevActiveTool) {
                EndBrushStrokeIfNeeded();
                // Contract: cancel in-progress quick-select stroke on tool switch (no undo/selection change)
                if (g_PrevActiveTool == ActiveTool::QuickSelect && ActiveCanvas().IsQuickSelectStrokeActive()) {
                    ActiveCanvas().CancelQuickSelectStroke();
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
                        s_StartRotationAngle = ActiveCanvas().GetRotationAngle();
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
                    ActiveCanvas().SetRotationAngle(targetAngle);
                    isRotating = true;
                } else {
                    s_IsDraggingRotation = false;
                    g_LastDragDelta = ImVec2(0.0f, 0.0f);
                }
            } else {
                g_LastDragDelta = ImVec2(0.0f, 0.0f);
            }

            float wheelDelta = isHovered ? ImGui::GetIO().MouseWheel : 0.0f;

            // Photoshop-like Brush Resize/Hardness (Ctrl+Alt+RMB) and Rotation (Ctrl+Alt+LMB)
            // Use ImGui mouse coords (DPI-consistent with drawn rings), absolute offset from start.
            // Size WYSIWYG: screen radius changes 1:1 with horizontal drag.
            // Hardness: fixed ~180 screen-px drag = full 0..1 (not maxBrush*zoom — that felt dead).
            // Rotation (LMB): angular drag around anchor, or fallback horizontal if near center.
            static bool g_IsCtrlAltRmbDragging = false;
            static bool g_IsCtrlAltLmbDragging = false;
            static ImVec2 g_CtrlAltAnchorScreen(0, 0);
            static float g_CtrlAltStartRadius = 10.f;
            static float g_CtrlAltStartHardness = 0.5f;
            static float g_CtrlAltStartRotation = 0.f;
            static float g_CtrlAltStartAngleRad = 0.f; // for LMB angular

            const bool brushOrEraser = isBrushLikeTool;
            const float maxBrushR = std::max(10.f, ConfigManager::Get().GetMaxBrushRadius());
            ImGuiIO& ioRmb = ImGui::GetIO();
            const float brushZoom = std::max(0.001f, ActiveCanvas().GetZoom());
            const bool modsResize = brushOrEraser && ioRmb.KeyCtrl && ioRmb.KeyAlt
                && (isHovered || ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem));

            // --- Begin RMB: size + hardness ---
            if (!g_IsCtrlAltRmbDragging && !g_IsCtrlAltLmbDragging && modsResize
                && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                g_IsCtrlAltRmbDragging = true;
                g_CtrlAltAnchorScreen = ImGui::GetMousePos();
                g_CtrlAltStartRadius = g_Brush.radius;
                g_CtrlAltStartHardness = g_Brush.hardness;
            }
            // --- Begin LMB: brush tip rotation ---
            if (!g_IsCtrlAltRmbDragging && !g_IsCtrlAltLmbDragging && modsResize
                && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                g_IsCtrlAltLmbDragging = true;
                g_CtrlAltAnchorScreen = ImGui::GetMousePos();
                g_CtrlAltStartRotation = g_Brush.rotationDeg;
                ImVec2 mp = ImGui::GetMousePos();
                g_CtrlAltStartAngleRad = std::atan2(mp.y - g_CtrlAltAnchorScreen.y,
                                                    mp.x - g_CtrlAltAnchorScreen.x);
            }

            auto drawBrushAdjustOverlay = [&](bool showHardness, bool showRotation) {
                ImDrawList* drawList = ImGui::GetForegroundDrawList();
                const ImVec2 fixed = g_CtrlAltAnchorScreen;
                float rScreen = g_Brush.radius * brushZoom;
                float hScreen = rScreen * g_Brush.hardness;
                // Soft / hard rings
                if (g_Brush.hardness < 0.999f && rScreen > hScreen + 0.5f)
                    drawList->AddCircleFilled(fixed, rScreen, IM_COL32(235, 64, 52, 35), 64);
                if (hScreen > 0.5f)
                    drawList->AddCircleFilled(fixed, hScreen, IM_COL32(235, 64, 52, 110), 64);
                drawList->AddCircle(fixed, rScreen, IM_COL32(235, 64, 52, 235), 64, 2.0f);
                if (showHardness && hScreen > 1.f && hScreen < rScreen - 0.5f)
                    drawList->AddCircle(fixed, hScreen, IM_COL32(255, 200, 120, 200), 64, 1.25f);
                // Rotation direction: diameter + arrow head
                if (showRotation || std::fabs(g_Brush.rotationDeg) > 0.01f) {
                    float rad = g_Brush.rotationDeg * (3.14159265f / 180.f);
                    float c = std::cos(rad), s = std::sin(rad);
                    float len = std::max(12.f, rScreen);
                    ImVec2 a(fixed.x - c * len, fixed.y - s * len);
                    ImVec2 b(fixed.x + c * len, fixed.y + s * len);
                    drawList->AddLine(a, b, IM_COL32(80, 180, 255, 240), 2.0f);
                    // arrow at +direction end
                    ImVec2 t1(b.x - c * 10.f + s * 5.f, b.y - s * 10.f - c * 5.f);
                    ImVec2 t2(b.x - c * 10.f - s * 5.f, b.y - s * 10.f + c * 5.f);
                    drawList->AddTriangleFilled(b, t1, t2, IM_COL32(80, 180, 255, 240));
                    // cross tick for tip "top"
                    float px = -s, py = c;
                    drawList->AddLine(ImVec2(fixed.x - px * 6.f, fixed.y - py * 6.f),
                                      ImVec2(fixed.x + px * 6.f, fixed.y + py * 6.f),
                                      IM_COL32(80, 180, 255, 180), 1.5f);
                }
            };

            if (g_IsCtrlAltRmbDragging) {
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) || !brushOrEraser || !ioRmb.KeyCtrl || !ioRmb.KeyAlt) {
                    g_IsCtrlAltRmbDragging = false;
                    ioRmb.MouseDelta = ImVec2(0, 0);
                } else {
                    ImVec2 cur = ImGui::GetMousePos();
                    float dxScreen = cur.x - g_CtrlAltAnchorScreen.x;
                    float dyScreen = cur.y - g_CtrlAltAnchorScreen.y;

                    // Screen-space 1:1: new_screen_r = start_screen_r + dx
                    float startRScreen = g_CtrlAltStartRadius * brushZoom;
                    float newRScreen = std::max(1.f, startRScreen + dxScreen);
                    g_Brush.radius = std::clamp(newRScreen / brushZoom, 1.0f, maxBrushR);

                    // Hardness: comfortable ~180 px vertical drag = full range
                    constexpr float kHardSpanPx = 180.f;
                    g_Brush.hardness = std::clamp(
                        g_CtrlAltStartHardness - dyScreen / kHardSpanPx, 0.0f, 1.0f);

                    ioRmb.MouseDelta = ImVec2(0, 0);
                    ioRmb.WantCaptureMouse = true;

                    ImGui::SetTooltip(
                        "Size: %.1f px  (drag right/left)\n"
                        "Hardness: %.0f%%  (drag up/down, ~180px = full range)",
                        g_Brush.radius, g_Brush.hardness * 100.f);
                    drawBrushAdjustOverlay(true, true);
                }
                isPanning = false;
                isRotating = false;
                wheelDelta = 0.f;
                dragDx = dragDy = 0.f;
            }

            if (g_IsCtrlAltLmbDragging) {
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || !brushOrEraser || !ioRmb.KeyCtrl || !ioRmb.KeyAlt) {
                    g_IsCtrlAltLmbDragging = false;
                    ioRmb.MouseDelta = ImVec2(0, 0);
                } else {
                    ImVec2 cur = ImGui::GetMousePos();
                    float dx = cur.x - g_CtrlAltAnchorScreen.x;
                    float dy = cur.y - g_CtrlAltAnchorScreen.y;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    if (dist > 8.f) {
                        // Angular: rotate around anchor
                        float ang = std::atan2(dy, dx);
                        float deltaDeg = (ang - g_CtrlAltStartAngleRad) * (180.f / 3.14159265f);
                        g_Brush.rotationDeg = g_CtrlAltStartRotation + deltaDeg;
                    } else {
                        // Near center: horizontal fine-tune (0.4° per px)
                        g_Brush.rotationDeg = g_CtrlAltStartRotation + dx * 0.4f;
                    }
                    // Normalize to [0, 360)
                    while (g_Brush.rotationDeg < 0.f) g_Brush.rotationDeg += 360.f;
                    while (g_Brush.rotationDeg >= 360.f) g_Brush.rotationDeg -= 360.f;

                    ioRmb.MouseDelta = ImVec2(0, 0);
                    ioRmb.WantCaptureMouse = true;
                    ImGui::SetTooltip("Brush rotation: %.1f°\n(orbit around center, or drag near center horizontally)",
                        g_Brush.rotationDeg);
                    drawBrushAdjustOverlay(false, true);
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
            if (isHovered && isInsideCanvas && !g_IsCtrlAltRmbDragging && !g_IsCtrlAltLmbDragging
                && (isBrushLikeTool || isSmudgeTool) && !isEyedropperMode) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);

                // Draw custom outline circle + rotation direction at mouse position
                ImDrawList* drawList = ImGui::GetForegroundDrawList();
                float cursorRadius = (g_ActiveTool == ActiveTool::Smudge) ? uiState.smudge.radius
                    : (g_ActiveTool == ActiveTool::BlurTool) ? uiState.blurTool.radius
                    : g_Brush.radius;
                float screenRadius = cursorRadius * ActiveCanvas().GetZoom();
                float hScreen = screenRadius * g_Brush.hardness;
                drawList->AddCircle(mousePos, screenRadius, IM_COL32(0, 0, 0, 255), 32, 1.5f);
                drawList->AddCircle(mousePos, screenRadius, IM_COL32(255, 255, 255, 255), 32, 1.0f);
                if (!isSmudgeTool && hScreen > 1.f && hScreen < screenRadius - 0.5f)
                    drawList->AddCircle(mousePos, hScreen, IM_COL32(255, 200, 120, 160), 32, 1.0f);
                // Rotation direction (where the tip "faces") so user sees stroke orientation
                if (!isSmudgeTool) {
                    float rad = g_Brush.rotationDeg * (3.14159265f / 180.f);
                    float c = std::cos(rad), s = std::sin(rad);
                    float len = std::max(10.f, screenRadius);
                    ImVec2 a(mousePos.x - c * len, mousePos.y - s * len);
                    ImVec2 b(mousePos.x + c * len, mousePos.y + s * len);
                    drawList->AddLine(a, b, IM_COL32(0, 0, 0, 200), 2.5f);
                    drawList->AddLine(a, b, IM_COL32(100, 200, 255, 255), 1.25f);
                    ImVec2 t1(b.x - c * 8.f + s * 4.f, b.y - s * 8.f - c * 4.f);
                    ImVec2 t2(b.x - c * 8.f - s * 4.f, b.y - s * 8.f + c * 4.f);
                    drawList->AddTriangleFilled(b, t1, t2, IM_COL32(100, 200, 255, 230));
                }
            }

            float zoom = ActiveCanvas().GetZoom();
            float cw = (float)ActiveCanvas().GetWidth();
            float ch = (float)ActiveCanvas().GetHeight();

            auto canvasToScreen = [&](float cx, float cy) -> ImVec2 {
                if (ActiveCanvas().GetViewportFlipH()) cx = cw - cx;
                if (ActiveCanvas().GetViewportFlipV()) cy = ch - cy;
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
            if (ActiveCanvas().GetMirrorHorizontal() || ActiveCanvas().GetMirrorVertical()) {
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

                if (ActiveCanvas().GetMirrorHorizontal()) {
                    ImVec2 h1 = canvasToScreen(cw * 0.5f, -ch * 5.0f);
                    ImVec2 h2 = canvasToScreen(cw * 0.5f, cw * 0.5f); // Wait! Let's check the original line: h2 was canvasToScreen(cw * 0.5f, ch * 6.0f)!
                    // Let's make sure it's ch * 6.0f!
                    h2 = canvasToScreen(cw * 0.5f, ch * 6.0f);
                    drawDashedLine(dl, h1, h2, IM_COL32(235, 64, 52, 180), 1.5f, 6.0f);
                }
                if (ActiveCanvas().GetMirrorVertical()) {
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
            // Fill Layer pipette (one-shot): do not require isHovered — popup may still
            // hold ImGui focus; use canvas bounds + armed flag only.
            if (UI::IsFillPipetteArmed() && isInsideCanvas && !isPanning
                && !g_IsCtrlAltRmbDragging && !g_IsCtrlAltLmbDragging) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);
                UI::SampleCanvasColor(ActiveCanvas(), canvasX, canvasY, s_PipettePreview);
                s_PipetteHasPreview = true;
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    UI::TryApplyFillPipette(ActiveCanvas(), canvasX, canvasY);
                }
            } else if (isHovered && isInsideCanvas && isEyedropperMode && !isPanning && !g_IsCtrlAltRmbDragging && !g_IsCtrlAltLmbDragging) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);
                UI::SampleCanvasColor(ActiveCanvas(), canvasX, canvasY, s_PipettePreview);
                s_PipetteHasPreview = true;
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    g_Brush.color[0] = s_PipettePreview[0];
                    g_Brush.color[1] = s_PipettePreview[1];
                    g_Brush.color[2] = s_PipettePreview[2];
                    g_Brush.color[3] = s_PipettePreview[3];
                }
            } else if (!isEyedropperMode && !UI::IsFillPipetteArmed()) {
                s_PipetteHasPreview = false;
            }

            // Stamp: Alt+LMB sets clone source (no paint)
            if (isStampTool && isHovered && isInsideCanvas && ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeyCtrl
                && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !isPanning) {
                ActiveCanvas().StampSetSource(canvasX, canvasY);
            }

            // Build brush for paint (Stamp injects clone offsets)
            auto makePaintBrush = [&]() -> BrushSettings {
                BrushSettings b = g_Brush;
                b.cloneStamp = false;
                if (isStampTool) {
                    b.erase = false;
                    b.cloneStamp = true;
                    float ox = 0, oy = 0;
                    ActiveCanvas().StampGetOffset(ox, oy);
                    b.cloneOffsetX = ox;
                    b.cloneOffsetY = oy;
                }
                return b;
            };

            if (isHovered && !isPanning && !g_IsCtrlAltRmbDragging && !g_IsCtrlAltLmbDragging
                && isBrushLikeTool && !isEyedropperMode && !UI::IsFillPipetteArmed()
                && !(ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyAlt)
                && !(isStampTool && ImGui::GetIO().KeyAlt)) {
                // Stamp without source: no paint
                const bool stampBlocked = isStampTool && !ActiveCanvas().StampHasSource();
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !stampBlocked) {
                    g_IsPainting = true;
                    if (isStampTool)
                        ActiveCanvas().StampLockOffsetFromDab(canvasX, canvasY);
                    BrushSettings pb = makePaintBrush();
                    if (isShiftHeld && g_HasLastStrokeEnd) {
                        if (isStampTool)
                            ActiveCanvas().StampLockOffsetFromDab(g_LastStrokeEndX, g_LastStrokeEndY);
                        pb = makePaintBrush();
                        ActiveCanvas().PaintOnActiveLayer(g_LastStrokeEndX, g_LastStrokeEndY, StrokePhase::Begin, pb);
                        ActiveCanvas().PaintOnActiveLayer(canvasX, canvasY, StrokePhase::Update, pb);
                        
                        g_StrokeStartX = canvasX;
                        g_StrokeStartY = canvasY;
                        g_LastStrokeEndX = canvasX;
                        g_LastStrokeEndY = canvasY;
                        g_LockAxisSelected = false;
                        g_LockAxis = LockAxis::None;
                    } else {
                        ActiveCanvas().PaintOnActiveLayer(canvasX, canvasY, StrokePhase::Begin, pb);
                        g_StrokeStartX = canvasX;
                        g_StrokeStartY = canvasY;
                        g_LockAxisSelected = false;
                        g_LockAxis = LockAxis::None;
                        g_LastStrokeEndX = canvasX;
                        g_LastStrokeEndY = canvasY;
                    }
                } 
                else if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && g_IsPainting && !stampBlocked) {
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
                    
                    ActiveCanvas().PaintOnActiveLayer(targetX, targetY, StrokePhase::Update, makePaintBrush());
                    g_LastStrokeEndX = targetX;
                    g_LastStrokeEndY = targetY;
                    g_HasLastStrokeEnd = true;
                }
            }

            if (g_IsPainting && (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
                BrushSettings endB = g_Brush;
                if (isStampTool) {
                    endB.cloneStamp = true;
                    float ox = 0, oy = 0;
                    ActiveCanvas().StampGetOffset(ox, oy);
                    endB.cloneOffsetX = ox;
                    endB.cloneOffsetY = oy;
                }
                ActiveCanvas().PaintOnActiveLayer(0, 0, StrokePhase::End, endB);
                g_IsPainting = false;
            }

            // Smudge / Blur Tool End
            if ((g_ActiveTool == ActiveTool::Smudge || g_ActiveTool == ActiveTool::BlurTool) &&
                (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
                if (g_ActiveTool == ActiveTool::Smudge)
                    ActiveCanvas().SmudgeOnActiveLayer(0, 0, StrokePhase::End, uiState.smudge);
                else
                    ActiveCanvas().BlurToolOnActiveLayer(0, 0, StrokePhase::End, uiState.blurTool);
            }

            // Warp operator commit / cancel
            if (ActiveCanvas().IsWarpOperatorActive()) {
                bool doCommit = uiState.commitTransform ||
                    (!ImGui::GetIO().WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)));
                bool doCancel = uiState.cancelTransform ||
                    (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape));
                uiState.commitTransform = false;
                uiState.cancelTransform = false;
                if (doCommit) {
                    ActiveCanvas().CommitWarpOperator(g_pd3dDevice);
                    g_ActiveTool = g_ToolBeforeWarp;
                    g_WarpDragIndex = -1;
                } else if (doCancel) {
                    ActiveCanvas().CancelWarpOperator(g_pd3dDevice);
                    g_ActiveTool = g_ToolBeforeWarp;
                    g_WarpDragIndex = -1;
                }
            }

            // Warp operator: drag control points (any tool / even outside strict hover after grab)
            if (ActiveCanvas().IsWarpOperatorActive()) {
                float hitR = 12.f / std::max(0.01f, ActiveCanvas().GetZoom());
                if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    g_WarpDragIndex = ActiveCanvas().HitTestWarpControl(canvasX, canvasY, hitR);
                if (g_WarpDragIndex >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    ActiveCanvas().SetWarpControlPoint(g_WarpDragIndex, canvasX, canvasY);
                    ActiveCanvas().PreviewWarpOperator(g_pd3dDevice);
                }
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                    g_WarpDragIndex = -1;
            }

            // Commit / Cancel Move / Free Transform
            if (ActiveCanvas().IsMovingPixels() && !ActiveCanvas().IsWarpOperatorActive()) {
                const bool enterPressed = !ImGui::GetIO().WantTextInput &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
                // Free Transform (Ctrl+T): Enter required. Move tool: Enter optional (also defocus).
                bool doCommit = uiState.commitTransform ||
                    (g_FreeTransformMode ? enterPressed : (enterPressed || false));
                bool doCancel = uiState.cancelTransform ||
                    (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape));
                uiState.commitTransform = false;
                uiState.cancelTransform = false;

                // Move tool: defocus / click outside canvas confirms (PS-like, no Enter required)
                if (!g_FreeTransformMode && !g_IsMoveDragging && !doCommit && !doCancel) {
                    if (!isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                        !ImGui::IsAnyItemHovered() && !ImGui::GetIO().WantCaptureMouse) {
                        doCommit = true;
                    }
                }

                auto finishTransformSession = [&](bool committed) {
                    if (committed)
                        ActiveCanvas().CommitMovePixels(g_pd3dDevice);
                    else
                        ActiveCanvas().CancelMovePixels(g_pd3dDevice);
                    g_MoveAccumulatedOffsetX = 0;
                    g_MoveAccumulatedOffsetY = 0;
                    g_IsMoveDragging = false;
                    g_ActiveGizmoHandle = TransformGizmoHandle::None;
                    if (g_FreeTransformMode) {
                        g_FreeTransformMode = false;
                        g_ActiveTool = g_ToolBeforeFreeTransform;
                    }
                };

                if (doCommit) finishTransformSession(true);
                else if (doCancel) finishTransformSession(false);
            }

            // Auto-commit Move if tool switched (not Free Transform restore path)
            if (g_ActiveTool != ActiveTool::MovePixels && ActiveCanvas().IsMovingPixels()) {
                ActiveCanvas().CommitMovePixels(g_pd3dDevice);
                g_MoveAccumulatedOffsetX = 0;
                g_MoveAccumulatedOffsetY = 0;
                g_IsMoveDragging = false;
                g_FreeTransformMode = false;
            }

            // Keyboard: Ctrl+D = Deselect (safe with empty selection — no null UpdateSubresource)
            if ((ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)) &&
                !ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeyShift) {
                if (ImGui::IsKeyPressed(ImGuiKey_D)) {
                    ActiveCanvas().ClearSelection();
                    ActiveCanvas().UpdateSelectionMaskTexture(g_pd3dDevice);
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

            // Drag-based selection tools (Quick Select handled as live brush, not marquee).
            bool isDragSelectionTool = (g_ActiveTool == ActiveTool::RectSelect ||
                                        g_ActiveTool == ActiveTool::EllipseSelect ||
                                        g_ActiveTool == ActiveTool::LassoSelect ||
                                        g_ActiveTool == ActiveTool::SmartSelect);

            if (isHovered && !isPanning && !g_IsCtrlAltRmbDragging) {
                // Magic Wand (click sets sticky seed + selection; not a drag tool)
                if (g_ActiveTool == ActiveTool::MagicWand) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && isInsideCanvas) {
                        bool add = selAdd();
                        bool subtract = selSub();
                        int sx = std::clamp((int)std::floor(canvasX), 0, ActiveCanvas().GetWidth() - 1);
                        int sy = std::clamp((int)std::floor(canvasY), 0, ActiveCanvas().GetHeight() - 1);
                        ActiveCanvas().ApplyMagicWandSelection(g_pd3dDevice, sx, sy, uiState.magicWandTolerance, add, subtract, uiState.magicWandContiguous);
                    }
                }
                // Bucket Fill
                else if (g_ActiveTool == ActiveTool::BucketFill) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        ActiveCanvas().ApplyBucketFill((int)canvasX, (int)canvasY, uiState.bucketFillTolerance, g_Brush.color, true);
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
                        ActiveCanvas().SmudgeOnActiveLayer(canvasX, canvasY, StrokePhase::Begin, uiState.smudge);
                    } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        ActiveCanvas().SmudgeOnActiveLayer(canvasX, canvasY, StrokePhase::Update, uiState.smudge);
                    }
                }
                // Blur Tool
                else if (g_ActiveTool == ActiveTool::BlurTool) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        ActiveCanvas().BlurToolOnActiveLayer(canvasX, canvasY, StrokePhase::Begin, uiState.blurTool);
                    } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        ActiveCanvas().BlurToolOnActiveLayer(canvasX, canvasY, StrokePhase::Update, uiState.blurTool);
                    }
                }
                else if (g_ActiveTool == ActiveTool::MovePixels && !ActiveCanvas().IsWarpOperatorActive()) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        if (!ActiveCanvas().IsMovingPixels()) {
                            ActiveCanvas().StartMovePixels(g_pd3dDevice);
                            g_ActiveGizmoHandle = TransformGizmoHandle::Move;
                            g_IsMoveDragging = true;
                            g_MoveDragStartX = canvasX;
                            g_MoveDragStartY = canvasY;
                            g_MoveAccumulatedOffsetX = 0;
                            g_MoveAccumulatedOffsetY = 0;
                        } else if (!g_FreeTransformMode) {
                            // Move tool: translation only
                            g_ActiveGizmoHandle = TransformGizmoHandle::Move;
                            g_IsMoveDragging = true;
                            g_MoveDragStartX = canvasX;
                            g_MoveDragStartY = canvasY;
                        } else {
                            // Free Transform operator: full gizmo
                            GizmoScreenGeometry geo;
                            if (GetGizmoGeometry(ActiveCanvas(), canvasToScreen, geo)) {
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
                                        g_GizmoDragStartRotation = ActiveCanvas().GetFloatingRotation();
                                        g_GizmoDragStartMouseAngle = std::atan2(mousePos.y - geo.center.y, mousePos.x - geo.center.x);
                                    } else {
                                        g_ActiveGizmoHandle = TransformGizmoHandle::None;
                                    }
                                }

                                if (g_ActiveGizmoHandle != TransformGizmoHandle::None) {
                                    g_IsMoveDragging = true;
                                    g_MoveDragStartX = canvasX;
                                    g_MoveDragStartY = canvasY;
                                    g_GizmoDragStartScaleX = ActiveCanvas().GetFloatingScaleX();
                                    g_GizmoDragStartScaleY = ActiveCanvas().GetFloatingScaleY();
                                    g_GizmoDragStartDist = std::sqrt(distSq(mousePos, geo.center));
                                    if (g_GizmoDragStartDist < 5.0f) g_GizmoDragStartDist = 5.0f;
                                    g_GizmoDragStartDistX = std::max(5.f, std::fabs(mousePos.x - geo.center.x));
                                    g_GizmoDragStartDistY = std::max(5.f, std::fabs(mousePos.y - geo.center.y));
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
                // Quick Select: PS-like progressive brush (live ants, Alt=subtract)
                else if (g_ActiveTool == ActiveTool::QuickSelect) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && isInsideCanvas) {
                        g_IsSelectionDragging = true;
                        g_LassoPoints.clear();
                        g_LassoPoints.push_back({ (int)canvasX, (int)canvasY });
                        ActiveCanvas().BeginQuickSelectStroke();
                        ActiveCanvas().StrokeQuickSelect(g_pd3dDevice, g_LassoPoints, g_Brush.radius,
                                                        ImGui::GetIO().KeyAlt);
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
                    }
                }
                // Polygonal Lasso
                else if (g_ActiveTool == ActiveTool::PolygonalLasso) {
                    bool add = selAdd();
                    bool subtract = selSub();
                    auto closePoly = [&]() {
                        if (g_PolygonalLassoPoints.size() >= 3) {
                            ActiveCanvas().ApplyPolygonalLassoSelection(g_PolygonalLassoPoints, add, subtract);
                            ActiveCanvas().UpdateSelectionMaskTexture(g_pd3dDevice);
                        }
                        g_PolygonalLassoPoints.clear();
                    };
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (!g_PolygonalLassoPoints.empty()) {
                            g_PolygonalLassoPoints.pop_back(); // double-click also emits a single click vertex
                        }
                        closePoly();
                    }
                    else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        // Auto-close when click is within Ø3px (radius 1.5) of start
                        if (g_PolygonalLassoPoints.size() >= 3) {
                            float dx = canvasX - (float)g_PolygonalLassoPoints[0].first;
                            float dy = canvasY - (float)g_PolygonalLassoPoints[0].second;
                            if (dx * dx + dy * dy <= 1.5f * 1.5f) {
                                closePoly();
                            } else {
                                g_PolygonalLassoPoints.push_back({ (int)canvasX, (int)canvasY });
                            }
                        } else {
                            g_PolygonalLassoPoints.push_back({ (int)canvasX, (int)canvasY });
                        }
                    }
                }
            }

            // Keyboard close/cancel for Polygonal Lasso
            if (g_ActiveTool == ActiveTool::PolygonalLasso && !ImGui::GetIO().WantTextInput) {
                bool add = selAdd();
                bool subtract = selSub();
                if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                    if (g_PolygonalLassoPoints.size() >= 3) {
                        ActiveCanvas().ApplyPolygonalLassoSelection(g_PolygonalLassoPoints, add, subtract);
                        ActiveCanvas().UpdateSelectionMaskTexture(g_pd3dDevice);
                    }
                    g_PolygonalLassoPoints.clear();
                }
                else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    g_PolygonalLassoPoints.clear();
                }
            }

            // Quick Select: Esc mid-stroke → restore selection as of stroke start
            if (g_ActiveTool == ActiveTool::QuickSelect && !ImGui::GetIO().WantTextInput &&
                ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                if (ActiveCanvas().IsQuickSelectStrokeActive()) {
                    ActiveCanvas().CancelQuickSelectStroke();
                    ActiveCanvas().UpdateSelectionMaskTexture(g_pd3dDevice);
                }
                g_IsSelectionDragging = false;
                g_LassoPoints.clear();
            }

            // Live Quick Select while dragging (every mouse sample → progressive grow)
            if (g_IsSelectionDragging && g_ActiveTool == ActiveTool::QuickSelect &&
                ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                int cx = (int)canvasX;
                int cy = (int)canvasY;
                if (g_LassoPoints.empty() || g_LassoPoints.back() != std::make_pair(cx, cy)) {
                    g_LassoPoints.push_back({ cx, cy });
                    // Keep path short for grow seeds — only need recent tip; stroke uses last point
                    if (g_LassoPoints.size() > 8)
                        g_LassoPoints.erase(g_LassoPoints.begin(), g_LassoPoints.end() - 4);
                }
                ActiveCanvas().StrokeQuickSelect(g_pd3dDevice, g_LassoPoints, g_Brush.radius,
                                                ImGui::GetIO().KeyAlt);
            }

            // Lasso / Smart Select path accumulation
            if (g_IsSelectionDragging && (g_ActiveTool == ActiveTool::LassoSelect || g_ActiveTool == ActiveTool::SmartSelect)) {
                int cx = (int)canvasX;
                int cy = (int)canvasY;
                if (g_LassoPoints.empty() || g_LassoPoints.back() != std::make_pair(cx, cy)) {
                    g_LassoPoints.push_back({ cx, cy });
                }
            }

            // End drag handling
            if (g_IsSelectionDragging && (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
                g_IsSelectionDragging = false;
                bool add = selAdd();
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
                    ActiveCanvas().ApplyRectSelection(x1, y1, x2, y2, add, subtract);
                    ActiveCanvas().UpdateSelectionMaskTexture(g_pd3dDevice);
                }
                else if (g_ActiveTool == ActiveTool::EllipseSelect) {
                    ActiveCanvas().ApplyEllipseSelection(x1, y1, x2, y2, add, subtract);
                    ActiveCanvas().UpdateSelectionMaskTexture(g_pd3dDevice);
                }
                else if (g_ActiveTool == ActiveTool::LassoSelect) {
                    ActiveCanvas().ApplyLassoSelection(g_LassoPoints, add, subtract);
                    ActiveCanvas().UpdateSelectionMaskTexture(g_pd3dDevice);
                }
                else if (g_ActiveTool == ActiveTool::SmartSelect) {
                    ActiveCanvas().ApplySmartSelectSelection(g_pd3dDevice, g_LassoPoints, add, subtract);
                }
                else if (g_ActiveTool == ActiveTool::QuickSelect) {
                    if (ActiveCanvas().IsQuickSelectStrokeActive())
                        ActiveCanvas().EndQuickSelectStroke(g_pd3dDevice, ImGui::GetIO().KeyAlt);
                }
                g_LassoPoints.clear();
            }

            // Move/Transform drag update
            if (g_IsMoveDragging) {
                ImVec2 mousePos = ImGui::GetMousePos();
                if (g_ActiveGizmoHandle == TransformGizmoHandle::Move) {
                    int dx = (int)floor(canvasX - g_MoveDragStartX);
                    int dy = (int)floor(canvasY - g_MoveDragStartY);
                    ActiveCanvas().UpdateMovePixels(g_pd3dDevice, g_MoveAccumulatedOffsetX + dx, g_MoveAccumulatedOffsetY + dy);
                }
                else if (g_ActiveGizmoHandle == TransformGizmoHandle::Rotate) {
                    GizmoScreenGeometry geo;
                    if (GetGizmoGeometry(ActiveCanvas(), canvasToScreen, geo)) {
                        float currentAngle = std::atan2(mousePos.y - geo.center.y, mousePos.x - geo.center.x);
                        float deltaAngle = currentAngle - g_GizmoDragStartMouseAngle;
                        ActiveCanvas().SetFloatingRotation(g_GizmoDragStartRotation + deltaAngle);
                        ActiveCanvas().MarkCompositeDirty();
                    }
                }
                else if (g_ActiveGizmoHandle == TransformGizmoHandle::Scale_TL ||
                         g_ActiveGizmoHandle == TransformGizmoHandle::Scale_TR ||
                         g_ActiveGizmoHandle == TransformGizmoHandle::Scale_BR ||
                         g_ActiveGizmoHandle == TransformGizmoHandle::Scale_BL) {
                    GizmoScreenGeometry geo;
                    if (GetGizmoGeometry(ActiveCanvas(), canvasToScreen, geo)) {
                        // Shift: uniform scale (preserve pre-transform aspect ratio)
                        // without Shift: free scale per axis
                        if (ImGui::GetIO().KeyShift) {
                            float currentDist = std::sqrt(distSq(mousePos, geo.center));
                            float factor = currentDist / g_GizmoDragStartDist;
                            ActiveCanvas().SetFloatingScaleX(g_GizmoDragStartScaleX * factor);
                            ActiveCanvas().SetFloatingScaleY(g_GizmoDragStartScaleY * factor);
                        } else {
                            float fx = std::fabs(mousePos.x - geo.center.x) / g_GizmoDragStartDistX;
                            float fy = std::fabs(mousePos.y - geo.center.y) / g_GizmoDragStartDistY;
                            fx = std::max(0.01f, fx);
                            fy = std::max(0.01f, fy);
                            ActiveCanvas().SetFloatingScaleX(g_GizmoDragStartScaleX * fx);
                            ActiveCanvas().SetFloatingScaleY(g_GizmoDragStartScaleY * fy);
                        }
                        ActiveCanvas().MarkCompositeDirty();
                    }
                }
                else if (g_ActiveGizmoHandle == TransformGizmoHandle::Scale_T ||
                         g_ActiveGizmoHandle == TransformGizmoHandle::Scale_B) {
                    GizmoScreenGeometry geo;
                    if (GetGizmoGeometry(ActiveCanvas(), canvasToScreen, geo)) {
                        float currentDist = std::sqrt(distSq(mousePos, geo.center));
                        float factor = currentDist / g_GizmoDragStartDist;
                        if (ImGui::GetIO().KeyShift) {
                            // Lock aspect: scale both axes uniformly
                            ActiveCanvas().SetFloatingScaleX(g_GizmoDragStartScaleX * factor);
                            ActiveCanvas().SetFloatingScaleY(g_GizmoDragStartScaleY * factor);
                        } else {
                            ActiveCanvas().SetFloatingScaleY(g_GizmoDragStartScaleY * factor);
                        }
                        ActiveCanvas().MarkCompositeDirty();
                    }
                }
                else if (g_ActiveGizmoHandle == TransformGizmoHandle::Scale_L ||
                         g_ActiveGizmoHandle == TransformGizmoHandle::Scale_R) {
                    GizmoScreenGeometry geo;
                    if (GetGizmoGeometry(ActiveCanvas(), canvasToScreen, geo)) {
                        float currentDist = std::sqrt(distSq(mousePos, geo.center));
                        float factor = currentDist / g_GizmoDragStartDist;
                        if (ImGui::GetIO().KeyShift) {
                            ActiveCanvas().SetFloatingScaleX(g_GizmoDragStartScaleX * factor);
                            ActiveCanvas().SetFloatingScaleY(g_GizmoDragStartScaleY * factor);
                        } else {
                            ActiveCanvas().SetFloatingScaleX(g_GizmoDragStartScaleX * factor);
                        }
                        ActiveCanvas().MarkCompositeDirty();
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
                ActiveCanvas().ApplyGradient((int)g_SelectionDragStartX, (int)g_SelectionDragStartY, (int)canvasX, (int)canvasY, g_Brush.color, g_SecondaryColor);
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

            // Draw Move / Free Transform / Warp gizmo
            if (ActiveCanvas().IsMovingPixels() || ActiveCanvas().IsWarpOperatorActive()) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(imageMin, ImVec2(imageMin.x + viewportWidth, imageMin.y + viewportHeight), true);
                if (ActiveCanvas().IsWarpOperatorActive())
                    ActiveCanvas().DrawWarpGizmo(dl, canvasToScreen);
                else
                    ActiveCanvas().DrawMoveGizmo(dl, canvasToScreen, /*showHandles=*/g_FreeTransformMode);
                dl->PopClipRect();
            }

            // Stamp clone-source crosshair
            if (g_ActiveTool == ActiveTool::Stamp && ActiveCanvas().StampHasSource()) {
                float sx = 0, sy = 0;
                ActiveCanvas().StampGetSource(sx, sy);
                ImVec2 sp = canvasToScreen(sx + 0.5f, sy + 0.5f);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(imageMin, ImVec2(imageMin.x + viewportWidth, imageMin.y + viewportHeight), true);
                const float arm = 10.0f;
                ImU32 col = IM_COL32(80, 220, 255, 255);
                dl->AddLine(ImVec2(sp.x - arm, sp.y), ImVec2(sp.x + arm, sp.y), col, 2.0f);
                dl->AddLine(ImVec2(sp.x, sp.y - arm), ImVec2(sp.x, sp.y + arm), col, 2.0f);
                dl->AddCircle(sp, 6.f, col, 0, 1.5f);
                // Live source under cursor when painting with offset
                if (ActiveCanvas().StampHasOffset() && g_IsPainting) {
                    float ox = 0, oy = 0;
                    ActiveCanvas().StampGetOffset(ox, oy);
                    ImVec2 live = canvasToScreen(canvasX - ox + 0.5f, canvasY - oy + 0.5f);
                    dl->AddCircle(live, 5.f, IM_COL32(255, 180, 40, 220), 0, 1.5f);
                }
                dl->PopClipRect();
            }

            // Magic Wand seed crosshair (sticky sample point)
            if (g_ActiveTool == ActiveTool::MagicWand && ActiveCanvas().HasWandSeed()) {
                int sx = 0, sy = 0;
                ActiveCanvas().GetWandSeed(sx, sy);
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

                // Info chip: float docs → linear float; U8 → HEX + 0..255
                const bool floatDoc = (ActiveCanvas().GetDocumentBitDepth() != Canvas::DocumentBitDepth::U8);
                char line0[64], line1[72], line2[48];
                if (floatDoc) {
                    std::snprintf(line0, sizeof(line0), "R %.5f", pr);
                    std::snprintf(line1, sizeof(line1), "G %.5f  B %.5f", pg, pb);
                    std::snprintf(line2, sizeof(line2), "A %.5f", pa);
                } else {
                    std::snprintf(line0, sizeof(line0), "#%02X%02X%02X", Ri, Gi, Bi);
                    std::snprintf(line1, sizeof(line1), "RGB %d %d %d", Ri, Gi, Bi);
                    std::snprintf(line2, sizeof(line2), "A %d", (int)std::lround(std::clamp(pa, 0.f, 1.f) * 255.f));
                }
                ImVec2 chipPos(ringC.x + r + 8.f, ringC.y - 18.f);
                ImVec2 s0 = ImGui::CalcTextSize(line0);
                ImVec2 s1 = ImGui::CalcTextSize(line1);
                ImVec2 s2 = ImGui::CalcTextSize(line2);
                float chipW = std::max(s0.x, std::max(s1.x, s2.x)) + 20.f;
                float lineH = s0.y + 1.f;
                float chipH = lineH * 3.f + 12.f;
                ImVec2 c0 = chipPos;
                ImVec2 c1(c0.x + chipW, c0.y + chipH);
                dl->AddRectFilled(c0, c1, tok.ColU32(ImVec4(tok.bgElevated.x, tok.bgElevated.y, tok.bgElevated.z, 0.92f)), tok.rSm);
                dl->AddRect(c0, c1, tok.ColU32(tok.strokeHairline), tok.rSm, 0, 1.0f);
                dl->AddRectFilled(ImVec2(c0.x + 4.f, c0.y + 4.f), ImVec2(c0.x + 12.f, c1.y - 4.f), sampleCol, 2.f);
                ImU32 tPri = tok.ColU32(tok.textPrimary);
                ImU32 tSec = tok.ColU32(tok.textSecondary);
                dl->AddText(ImVec2(c0.x + 16.f, c0.y + 4.f), tPri, line0);
                dl->AddText(ImVec2(c0.x + 16.f, c0.y + 4.f + lineH), tSec, line1);
                dl->AddText(ImVec2(c0.x + 16.f, c0.y + 4.f + lineH * 2.f), tSec, line2);
            }

            // Draw Smart Select background process progress & cancel option UI
            if (ActiveCanvas().IsSmartSelectInProgress()) {
                ImGui::SetCursorScreenPos(ImVec2(imageMin.x + 20.0f, imageMin.y + 20.0f));
                ImGui::BeginChild("SmartSelectProgress", ImVec2(320.0f, 90.0f), true, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
                ImGui::Text("Smart Select (GrabCut) is running...");
                float t = (float)fmod(ImGui::GetTime() * 2.0, 100.0) / 100.0f;
                char buf[32];
                sprintf(buf, "Processing...");
                ImGui::ProgressBar(t, ImVec2(-1.0f, 0.0f), buf);
                if (ImGui::Button("Cancel")) {
                    ActiveCanvas().CancelSmartSelect();
                }
                ImGui::EndChild();
            }

            ActiveCanvas().Update(viewportWidth, viewportHeight, isHovered, localMouseX, localMouseY, isPanning, dragDx, dragDy, wheelDelta);

            // Brush preset picker (RMB click on Brush/Eraser)
            UI::DrawBrushPickerPopup(s_WantBrushPopup, s_BrushPopupPos, g_Brush);
        }

        ImGui::End();



        // Benchmark HUD (before ImGui::Render so it shows in the frame)
        if (benchmarkMode && BenchmarkRunner::Get().IsActive()) {
            ImGui::SetNextWindowPos(ImVec2(14.f, 14.f), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.78f);
            ImGuiWindowFlags hudFlags =
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav |
                ImGuiWindowFlags_NoFocusOnAppearing;
            if (ImGui::Begin("##bench_hud", nullptr, hudFlags)) {
                ImGui::TextUnformatted(BenchmarkRunner::Get().StatusLine());
                ImGui::Text("UI FPS: %.1f   last frame: %.1f ms", uiState.fps, uiState.frameTimeMs);
                ImGui::TextDisabled("8K tiling / stroke / undo stress — auto-exits when done");
            }
            ImGui::End();
        }

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

        // Work timing (excludes VSync wait when Present blocks)
        auto workEnd = std::chrono::high_resolution_clock::now();
        const float frameWorkMs =
            std::chrono::duration<float, std::milli>(workEnd - loopStart).count();

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

        // Present: VSync when focused; uncapped when unfocused or --perf (Phase A).
        {
            const bool focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE;
            const UINT syncInterval = (perfMode || benchmarkMode || !focused) ? 0u : 1u;
            g_pSwapChain->Present(syncInterval, 0);
        }

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

        if (benchmarkMode) {
            s_benchPrevWorkMs = frameWorkMs > 0.5f ? frameWorkMs : uiState.frameTimeMs;
            if (BenchmarkRunner::Get().IsFinished()) {
                processExitCode = BenchmarkRunner::Get().ExitCode();
                Logger::Get().InfoTag("bench",
                    "Exiting after benchmark, code=" + std::to_string(processExitCode));
                break;
            }
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
    ProjectManager::Get().Shutdown();
    CleanupCanvasRenderTarget();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    CleanupRenderTarget();
    CleanupDeviceD3D();
    SingleInstance::UnregisterMainWindow(hWnd);
    if (g_OriginalWndProc) {
        SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)g_OriginalWndProc);
        g_OriginalWndProc = nullptr;
    }
    if (g_SingleInstanceMutex) {
        ReleaseMutex(g_SingleInstanceMutex);
        CloseHandle(g_SingleInstanceMutex);
        g_SingleInstanceMutex = nullptr;
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    ScriptingEngine::Get().Shutdown();
    assets::AssetManager::Get().Shutdown();
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

    ActiveCanvas().Render(g_pd3dDeviceContext, static_cast<float>(width), static_cast<float>(height));

    ID3D11RenderTargetView* nullRTV = nullptr;
    g_pd3dDeviceContext->OMSetRenderTargets(1, &nullRTV, nullptr);
}

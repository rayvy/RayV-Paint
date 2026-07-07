#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
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
#include "imgui_impl_dx12.h"

// Project Core
#include "Logger.h"
#include "ThreadPool.h"
#include "ConfigManager.h"
#include "Canvas.h"
#include "core/KeymapManager.h"
#include "core/ClipboardHelper.h"
#include "ui/EditorPanels.h"
#include "CanvasRendererDX12.h"
#include "render/Dx12Device.h"


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



// DirectX 12 renderer objects
static constexpr UINT kFrameCount = 2;
static Dx12Device g_DX12;

static ID3D12DescriptorHeap*        g_canvasRtvHeap = nullptr;
static D3D12_CPU_DESCRIPTOR_HANDLE  g_canvasRtvHandle = {};

// Canvas Render-to-Texture Objects
static ID3D12Resource*              g_canvasTexture12 = nullptr;
static D3D12_CPU_DESCRIPTOR_HANDLE  g_canvasTextureSrvCpuHandle = {};
static D3D12_GPU_DESCRIPTOR_HANDLE  g_canvasTextureSrvGpuHandle = {};
static float                        g_canvasRTWidth = 0.0f;
static float                        g_canvasRTHeight = 0.0f;

// Canvas Instance
static Canvas g_Canvas;
static ImVec2 g_LastDragDelta = ImVec2(0.0f, 0.0f);
static CanvasRendererDX12 g_CanvasRenderer;
static bool g_CanvasRendererReady = false;

// Global startup time
static double g_StartupTimeMs = 0.0;

// Painting state
static ActiveTool g_ActiveTool = ActiveTool::Brush;
static ActiveTool g_LastSelectTool = ActiveTool::RectSelect;
static ActiveTool g_LastWandTool = ActiveTool::MagicWand;
static BrushSettings g_Brush;
static float g_SecondaryColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
static bool g_IsPainting = false;
static ActiveTool g_PrevActiveTool = ActiveTool::Brush;

static void EndBrushStrokeIfNeeded() {
    if (g_IsPainting) {
        g_Canvas.PaintOnActiveLayer(0, 0, StrokePhase::End, g_Brush);
        g_IsPainting = false;
    }
}

// Selection dragging state
static bool g_IsSelectionDragging = false;
static float g_SelectionDragStartX = 0.0f;
static float g_SelectionDragStartY = 0.0f;
static std::vector<std::pair<int, int>> g_LassoPoints;

static bool g_IsMoveDragging = false;
static float g_MoveDragStartX = 0.0f;
static float g_MoveDragStartY = 0.0f;
static int g_MoveAccumulatedOffsetX = 0;
static int g_MoveAccumulatedOffsetY = 0;
static bool g_IsGradientDragging = false;
static void CustomDropCallback(GLFWwindow* window, int count, const char** paths) {
    if (count <= 0) return;
    std::string path = paths[0];
    
    if (g_IsViewportHovered || g_IsLayersHovered) {
        if (g_Canvas.LoadImageToLayer(path)) {
            Logger::Get().Info("Dropped file imported as layer: " + path);
        } else {
            Logger::Get().Error("Failed to import dropped file as layer: " + path);
        }
    } else {
        size_t dot = path.find_last_of('.');
        std::string ext = "";
        if (dot != std::string::npos) {
            ext = path.substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        }
        
        if (ext == "rayp") {
            if (g_Canvas.LoadCanvasRayp(path)) {
                Logger::Get().Info("Successfully loaded project from drop: " + path);
            } else {
                Logger::Get().Error("Failed to load project from drop: " + path);
            }
        } else {
            g_Canvas.ClearUndoHistory();
            g_Canvas.GetLayers().clear();
            g_Canvas.SetActiveLayerIndex(-1);
            g_Canvas.SetCurrentProjectFilePath("");
            
            if (g_Canvas.LoadImageToLayer(path)) {
                Logger::Get().Info("Dropped file opened as new project: " + path);
            } else {
                Logger::Get().Error("Failed to open dropped file as new project: " + path);
                g_Canvas.CreateNewLayer("Background");
            }
        }
    }
}

// Forward declarations
bool CreateDeviceD3D(HWND hWnd, bool useNullDriver = false);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
bool BeginMainRenderTarget();
void EndMainRenderTarget();
void WaitForGpu();
void ResizeCanvasRenderTarget(int width, int height);
void CleanupCanvasRenderTarget();
void RenderCanvasToTexture(int width, int height);
void RedirectIOToConsole();

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
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONOUT$", "w", stderr);
    freopen_s(&fDummy, "CONIN$", "r", stdin);
    std::cout.clear();
    std::cerr.clear();
    std::cin.clear();
}

void ApplyTheme(const std::string& themeName) {
    ImGuiStyle& style = ImGui::GetStyle();
    
    // Core structure / spacings common to modern premium interfaces
    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 6.0f);
    style.ScrollbarSize = 13.0f;
    style.GrabMinSize = 10.0f;

    if (themeName == "Classic") {
        ImGui::StyleColorsClassic();
        style.FrameRounding = 0.0f;
        style.WindowRounding = 0.0f;
    } else if (themeName == "Light") {
        ImGui::StyleColorsLight();
        ImVec4* colors = style.Colors;
        colors[ImGuiCol_WindowBg]           = ImVec4(0.95f, 0.95f, 0.96f, 1.00f);
        colors[ImGuiCol_ChildBg]            = ImVec4(0.98f, 0.98f, 0.98f, 1.00f);
        colors[ImGuiCol_PopupBg]            = ImVec4(1.00f, 1.00f, 1.00f, 0.98f);
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
    } else { // "Dark" - Charcoal Figma/Adobe Premium Palette
        ImGui::StyleColorsDark();
        ImVec4* colors = style.Colors;
        
        // Window & Child Panels
        colors[ImGuiCol_WindowBg]               = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
        colors[ImGuiCol_ChildBg]                = ImVec4(0.12f, 0.12f, 0.13f, 1.00f);
        colors[ImGuiCol_PopupBg]                = ImVec4(0.07f, 0.07f, 0.08f, 0.98f);
        colors[ImGuiCol_Border]                 = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);
        
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
    bool forceConsole = false;
    std::string configPath = "";
    std::string startupImagePath = "";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--test") {
            testMode = true;
        } else if (arg == "--headless") {
            headlessMode = true;
            testMode = true; // Headless implies auto-testing behavior
        } else if (arg == "--console") {
            forceConsole = true;
        } else if (arg == "--version") {
            SetupConsole(true);
            std::cout << "RayVPaint - Tech Art Editor - Version 0.2.0" << std::endl;
            return 0;
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
    log_step("Logger & Config Systems");

    // 3. Initialize Concurrency (ThreadPool)
    unsigned int numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 2;
    ThreadPool::Get().Init(numThreads);
    log_step("Concurrency (ThreadPool)");

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

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Native window, renderer is created separately
    GLFWwindow* window = glfwCreateWindow(1280, 720, "RayVPaint - Tech Art Editor", nullptr, nullptr);
    if (!window) {
        Logger::Get().Error("Failed to create GLFW window");
        glfwTerminate();
        return 1;
    }
    log_step("GLFW Window Creation");

    HWND hWnd = glfwGetWin32Window(window);

    // Subclass window procedure for high-precision pointer/tablet messages
    g_OriginalWndProc = (WNDPROC)SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)SubclassedWndProc);

    // Initialize KeymapManager (requires GLFW initialized and window created for scancode resolution)
    KeymapManager::Get().Initialize();
    KeymapManager::Get().Load();
    log_step("Keymap Manager Init");

    // 6. Initialize DX12-backed renderer (native swapchain)
    if (!g_DX12.Initialize(hWnd, headlessMode)) {
        Logger::Get().Error("Failed to initialize DX12-backed renderer");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    log_step("DX12-backed Device Creation");

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

    ImGui_ImplDX12_InitInfo dx12InitInfo = {};
    dx12InitInfo.Device = g_DX12.GetDevice();
    dx12InitInfo.CommandQueue = g_DX12.GetCommandQueue();
    dx12InitInfo.NumFramesInFlight = kFrameCount;
    dx12InitInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    dx12InitInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    dx12InitInfo.SrvDescriptorHeap = g_DX12.GetSrvHeap();
    dx12InitInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_desc_handle) {
        out_cpu_desc_handle->ptr = 0;
        out_gpu_desc_handle->ptr = 0;
        if (!g_DX12.AllocateSrv(out_cpu_desc_handle, out_gpu_desc_handle)) {
            Logger::Get().Error("DX12 SRV descriptor heap exhausted");
        }
    };
    dx12InitInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_desc_handle) {
        g_DX12.FreeSrv(cpu_desc_handle, gpu_desc_handle);
    };
    if (!ImGui_ImplDX12_Init(&dx12InitInfo)) {
        Logger::Get().Error("Failed to initialize ImGui DX12 backend");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        g_DX12.Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    log_step("ImGui Context & Backend Init");

    // 8. Initialize Canvas Renderer
    if (!g_Canvas.Initialize()) {
        Logger::Get().Error("Failed to initialize Canvas renderer");
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        g_DX12.Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
        // Initialize tiled DX12 Canvas Renderer
    if (!g_CanvasRenderer.Initialize(
        g_DX12.GetDevice(),
        g_DX12.GetCommandQueue(),
        [](D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) { return g_DX12.AllocateSrv(cpu, gpu); },
        [](D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu) { g_DX12.FreeSrv(cpu, gpu); }
    )) {
        Logger::Get().Error("Failed to initialize CanvasRendererDX12");
        // Non-fatal: falls back to grey clear
    } else {
        g_CanvasRendererReady = true;
        Logger::Get().Info("CanvasRendererDX12 initialized successfully.");
    }

    log_step("Canvas Renderer D3D Init (including Shader Loading/Compiling)");

    // Check for crash recovery / autosave restore
    std::string backupDir = ConfigManager::Get().GetBackupDir();
    std::string backupPath = backupDir + "/autosave_backup.rayp";
    bool showRecoveryModal = std::filesystem::exists(backupPath);

    // Load startup image if specified on CLI
    if (!startupImagePath.empty()) {
        g_Canvas.LoadImageToLayer(startupImagePath);
    }

    // Measure startup time
    auto startupEnd = std::chrono::high_resolution_clock::now();
    g_StartupTimeMs = std::chrono::duration<double, std::milli>(startupEnd - startupStart).count();
    Logger::Get().Info("Startup completed in: " + std::to_string(g_StartupTimeMs) + " ms");

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

        // Handle window resizing
        int winW, winH;
        glfwGetFramebufferSize(window, &winW, &winH);
        if (winW > 0 && winH > 0 && (winW != currentWindowWidth || winH != currentWindowHeight)) {
            g_DX12.ResizeSwapchain(static_cast<uint32_t>(winW), static_cast<uint32_t>(winH));
            currentWindowWidth = winW;
            currentWindowHeight = winH;
            Logger::Get().Debug("Swapchain backbuffers resized to " + std::to_string(winW) + "x" + std::to_string(winH));
        }

        auto loopStart = std::chrono::high_resolution_clock::now();

        // Start Dear ImGui Frame
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        g_IsViewportHovered = false;
        g_IsLayersHovered = false;

        // Render all UI Panels and Modals (Toolbar, Properties, Layers, Brush settings, Console logs, Colors)
        UI::RenderAll(uiState, g_Canvas, g_Brush, g_ActiveTool, window);

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
            if (KeymapManager::Get().ConsumeActionTrigger("InvertSelection")) {
                g_Canvas.InvertSelection();
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
                    std::string icc = g_Canvas.GetExportPngColorSpace();
                    if (g_Canvas.SaveCanvasStandard(path, icc == "sRGB" ? "" : icc)) {
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
                    g_Canvas.CreateLayerFromPixels("Pasted Layer", pastedPixels, pastedW, pastedH);
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
            if (g_canvasTextureSrvGpuHandle.ptr != 0) {
                ImGui::Image((ImTextureID)(uintptr_t)g_canvasTextureSrvGpuHandle.ptr, ImVec2(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight)));
            }

            // Calculate precise mouse coordinates using the actual image bounding box
            ImVec2 imageMin = ImGui::GetItemRectMin();
            ImVec2 mousePos = ImGui::GetMousePos();
            float localMouseX = mousePos.x - imageMin.x;
            float localMouseY = mousePos.y - imageMin.y;

            // Precise hover check: only true if mouse is directly over the canvas viewport image
            bool isHovered = ImGui::IsItemHovered();

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

            bool isBrushLikeTool = (g_ActiveTool == ActiveTool::Brush || g_ActiveTool == ActiveTool::Eraser || g_ActiveTool == ActiveTool::Smudge);
            bool isPipetteTool = (g_ActiveTool == ActiveTool::Pipette);
            bool isEyedropperMode = (isBrushLikeTool && ImGui::GetIO().KeyAlt) || isPipetteTool;

            if (g_ActiveTool != g_PrevActiveTool) {
                EndBrushStrokeIfNeeded();
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

            // Photoshop-like Brush Resize/Hardness drag controls (Ctrl+Alt+RMB)
            static bool g_IsCtrlAltRmbDragging = false;
            static double g_CtrlAltRmbStartX = 0.0;
            static double g_CtrlAltRmbStartY = 0.0;

            if (isHovered && ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                g_IsCtrlAltRmbDragging = true;
                GLFWwindow* win = glfwGetCurrentContext();
                if (win) {
                    glfwGetCursorPos(win, &g_CtrlAltRmbStartX, &g_CtrlAltRmbStartY);
                    glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
                }
            }
            if (g_IsCtrlAltRmbDragging) {
                GLFWwindow* win = glfwGetCurrentContext();
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                    g_IsCtrlAltRmbDragging = false;
                    if (win) {
                        glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                    }
                } else {
                    ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;
                    g_Brush.radius = std::clamp(g_Brush.radius + mouseDelta.x * 0.5f, 1.0f, 250.0f);
                    g_Brush.hardness = std::clamp(g_Brush.hardness - mouseDelta.y * 0.01f, 0.0f, 1.0f);

                    if (win) {
                        glfwSetCursorPos(win, g_CtrlAltRmbStartX, g_CtrlAltRmbStartY);
                    }

                    // Show visual feedback tooltip
                    ImGui::BeginTooltip();
                    ImGui::Text("Brush Size: %.1f px", g_Brush.radius);
                    ImGui::Text("Hardness: %.2f", g_Brush.hardness);
                    ImGui::EndTooltip();

                    // Draw Photoshop-like preview
                    ImDrawList* drawList = ImGui::GetForegroundDrawList();
                    float rScreen = g_Brush.radius * g_Canvas.GetZoom();
                    float hScreen = rScreen * g_Brush.hardness;
                    
                    ImU32 outerColor = IM_COL32(235, 64, 52, 200);      // Ring
                    ImU32 solidColor = IM_COL32(235, 64, 52, 90);       // Solid core
                    ImU32 falloffColor = IM_COL32(235, 64, 52, 35);     // Falloff region
                    
                    if (g_Brush.hardness < 1.0f) {
                        drawList->AddCircleFilled(mousePos, rScreen, falloffColor, 64);
                    }
                    if (hScreen > 0.0f) {
                        drawList->AddCircleFilled(mousePos, hScreen, solidColor, 64);
                    }
                    drawList->AddCircle(mousePos, rScreen, outerColor, 64, 2.0f);
                }
            }

            // Handle custom brush visualizer and cursor hiding when inside canvas bounds
            if (isHovered && isInsideCanvas && !g_IsCtrlAltRmbDragging && isBrushLikeTool && !isEyedropperMode) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);

                // Draw custom outline circle at mouse position
                ImDrawList* drawList = ImGui::GetForegroundDrawList();
                float cursorRadius = (g_ActiveTool == ActiveTool::Smudge) ? uiState.smudge.radius : g_Brush.radius;
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

            if (isHovered && isInsideCanvas && isEyedropperMode && !isPanning && !g_IsCtrlAltRmbDragging) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    UI::SampleCanvasColor(g_Canvas, canvasX, canvasY, g_Brush.color);
                }
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
                    g_Canvas.CommitMovePixels();
                    g_MoveAccumulatedOffsetX = 0;
                    g_MoveAccumulatedOffsetY = 0;
                }
                else if (doCancel) {
                    g_Canvas.CancelMovePixels();
                    g_MoveAccumulatedOffsetX = 0;
                    g_MoveAccumulatedOffsetY = 0;
                }
            }

            // Auto-commit Move Pixels if tool switched
            if (g_ActiveTool != ActiveTool::MovePixels && g_Canvas.IsMovingPixels()) {
                g_Canvas.CommitMovePixels();
                g_MoveAccumulatedOffsetX = 0;
                g_MoveAccumulatedOffsetY = 0;
            }

            // Keyboard shortcuts (like Ctrl+D)
            if ((ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)) && !ImGui::GetIO().WantTextInput) {
                if (ImGui::IsKeyPressed(ImGuiKey_D)) {
                    g_Canvas.ClearSelection();
                    g_Canvas.MarkSelectionMaskDirty();
                }
            }

            // Keyboard shortcut X for swapping primary/secondary colors
            if (ImGui::IsKeyPressed(ImGuiKey_X) && !ImGui::GetIO().WantTextInput) {
                std::swap(g_Brush.color[0], g_SecondaryColor[0]);
                std::swap(g_Brush.color[1], g_SecondaryColor[1]);
                std::swap(g_Brush.color[2], g_SecondaryColor[2]);
                std::swap(g_Brush.color[3], g_SecondaryColor[3]);
            }

            // Selection tools, pipette, bucket fill, gradient interaction
            bool isSelectionTool = (g_ActiveTool == ActiveTool::RectSelect || 
                                    g_ActiveTool == ActiveTool::EllipseSelect || 
                                    g_ActiveTool == ActiveTool::LassoSelect || 
                                    g_ActiveTool == ActiveTool::MagicWand ||
                                    g_ActiveTool == ActiveTool::SmartSelect);

            if (isHovered && !isPanning && !g_IsCtrlAltRmbDragging) {
                // Magic Wand
                if (g_ActiveTool == ActiveTool::MagicWand) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        bool add = ImGui::GetIO().KeyShift;
                        bool subtract = ImGui::GetIO().KeyAlt;
                        g_Canvas.ApplyMagicWandSelection((int)canvasX, (int)canvasY, uiState.magicWandTolerance, add, subtract, uiState.magicWandContiguous);
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
                            g_Canvas.StartMovePixels();
                        }
                        g_IsMoveDragging = true;
                        g_MoveDragStartX = canvasX;
                        g_MoveDragStartY = canvasY;
                    }
                }
                // Drag-based selections
                else if (isSelectionTool) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        g_IsSelectionDragging = true;
                        g_SelectionDragStartX = canvasX;
                        g_SelectionDragStartY = canvasY;
                        g_LassoPoints.clear();
                        g_LassoPoints.push_back({ (int)canvasX, (int)canvasY });
                    }
                }
            }

            // Accumulate points if lasso or smart select is active
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
                bool add = ImGui::GetIO().KeyShift;
                bool subtract = ImGui::GetIO().KeyAlt;

                int x1 = (int)g_SelectionDragStartX;
                int y1 = (int)g_SelectionDragStartY;
                int x2 = (int)canvasX;
                int y2 = (int)canvasY;

                if (g_ActiveTool == ActiveTool::RectSelect) {
                    g_Canvas.ApplyRectSelection(x1, y1, x2, y2, add, subtract);
                    g_Canvas.MarkSelectionMaskDirty();
                }
                else if (g_ActiveTool == ActiveTool::EllipseSelect) {
                    g_Canvas.ApplyEllipseSelection(x1, y1, x2, y2, add, subtract);
                    g_Canvas.MarkSelectionMaskDirty();
                }
                else if (g_ActiveTool == ActiveTool::LassoSelect) {
                    g_Canvas.ApplyLassoSelection(g_LassoPoints, add, subtract);
                    g_Canvas.MarkSelectionMaskDirty();
                }
                else if (g_ActiveTool == ActiveTool::SmartSelect) {
                    g_Canvas.ApplySmartSelectSelection(g_LassoPoints, add, subtract);
                }
                g_LassoPoints.clear();
            }

            // Move drag update
            if (g_IsMoveDragging) {
                int dx = (int)floor(canvasX - g_MoveDragStartX);
                int dy = (int)floor(canvasY - g_MoveDragStartY);
                g_Canvas.UpdateMovePixels(g_MoveAccumulatedOffsetX + dx, g_MoveAccumulatedOffsetY + dy);
            }

            // Move drag release
            if (g_IsMoveDragging && (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
                g_IsMoveDragging = false;
                g_MoveAccumulatedOffsetX += (int)floor(canvasX - g_MoveDragStartX);
                g_MoveAccumulatedOffsetY += (int)floor(canvasY - g_MoveDragStartY);
            }

            // Gradient drag release
            if (g_ActiveTool == ActiveTool::Gradient && g_IsGradientDragging && (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
                g_IsGradientDragging = false;
                g_Canvas.ApplyGradient((int)g_SelectionDragStartX, (int)g_SelectionDragStartY, (int)canvasX, (int)canvasY, g_Brush.color, g_SecondaryColor);
            }

            // Draw interactive shape outline during drag/selection
            if (g_IsSelectionDragging || (g_ActiveTool == ActiveTool::Gradient && g_IsGradientDragging)) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(imageMin, ImVec2(imageMin.x + viewportWidth, imageMin.y + viewportHeight), true);

                ImU32 outlineCol = IM_COL32(255, 255, 0, 255);
                float thickness = 2.0f;

                if (g_ActiveTool == ActiveTool::RectSelect) {
                    ImVec2 p1 = canvasToScreen(g_SelectionDragStartX, g_SelectionDragStartY);
                    ImVec2 p2 = canvasToScreen(canvasX, g_SelectionDragStartY);
                    ImVec2 p3 = canvasToScreen(canvasX, canvasY);
                    ImVec2 p4 = canvasToScreen(g_SelectionDragStartX, canvasY);
                    dl->AddLine(p1, p2, outlineCol, thickness);
                    dl->AddLine(p2, p3, outlineCol, thickness);
                    dl->AddLine(p3, p4, outlineCol, thickness);
                    dl->AddLine(p4, p1, outlineCol, thickness);
                }
                else if (g_ActiveTool == ActiveTool::EllipseSelect) {
                    float cx = (g_SelectionDragStartX + canvasX) * 0.5f;
                    float cy = (g_SelectionDragStartY + canvasY) * 0.5f;
                    float rx = std::abs(canvasX - g_SelectionDragStartX) * 0.5f;
                    float ry = std::abs(canvasY - g_SelectionDragStartY) * 0.5f;
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
                else if (g_ActiveTool == ActiveTool::LassoSelect || g_ActiveTool == ActiveTool::SmartSelect) {
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
        }

        ImGui::End();



        // Handle Test Mode Execution: Perform 1 Frame and Exit
        if (testMode) {
            Logger::Get().Info("[TEST] Render completed. Saving config and exiting successfully.");
            ImGui::Render();
            if (g_DX12.BeginFrame()) {
                ID3D12DescriptorHeap* heaps[] = { g_DX12.GetSrvHeap() };
                g_DX12.GetCommandList()->SetDescriptorHeaps(1, heaps);
                D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_DX12.GetCurrentRtv();
                g_DX12.GetCommandList()->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                float clearColor[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
                g_DX12.GetCommandList()->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
                ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_DX12.GetCommandList());
                if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                    ImGui::UpdatePlatformWindows();
                    ImGui::RenderPlatformWindowsDefault();
                }
                g_DX12.EndFrame();
            }
            break;
        }

        // Standard Render Presentation
        ImGui::Render();
        if (g_DX12.BeginFrame()) {
            ID3D12DescriptorHeap* heaps[] = { g_DX12.GetSrvHeap() };
            g_DX12.GetCommandList()->SetDescriptorHeaps(1, heaps);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_DX12.GetCurrentRtv();
            g_DX12.GetCommandList()->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            float clearColor[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
            g_DX12.GetCommandList()->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
            
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_DX12.GetCommandList());

            if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
            }

            g_DX12.EndFrame();
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
    }

    // Delete autosave backup on clean exit
    try {
        std::string bPath = ConfigManager::Get().GetBackupDir() + "/autosave_backup.rayp";
        if (std::filesystem::exists(bPath)) {
            std::filesystem::remove(bPath);
        }
    } catch (...) {}

    // Cleanup Subsystems in reverse order
    g_CanvasRenderer.Shutdown();
        g_CanvasRendererReady = false;
    g_Canvas.Shutdown();
    CleanupCanvasRenderTarget();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    g_DX12.Shutdown();
    if (g_OriginalWndProc) {
        SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)g_OriginalWndProc);
        g_OriginalWndProc = nullptr;
    }
    glfwDestroyWindow(window);
    glfwTerminate();

    ThreadPool::Get().Shutdown();
    Logger::Get().Info("RayVPaint shut down cleanly.");
    Logger::Get().Shutdown();

    return 0;
}

// WinMain entry point for Native WIN32 GUI
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    return main(__argc, __argv);
}

// DX12-backed renderer helpers.
void ResizeCanvasRenderTarget(int width, int height) {
    if (g_canvasTexture12 && static_cast<int>(g_canvasRTWidth) == width && static_cast<int>(g_canvasRTHeight) == height) {
        return;
    }

    CleanupCanvasRenderTarget();

    g_canvasRTWidth = static_cast<float>(width);
    g_canvasRTHeight = static_cast<float>(height);

    if (!g_DX12.GetDevice()) {
        return;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = static_cast<UINT64>(width);
    desc.Height = static_cast<UINT>(height);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clearValue.Color[0] = 0.12f;
    clearValue.Color[1] = 0.12f;
    clearValue.Color[2] = 0.14f;
    clearValue.Color[3] = 1.0f;

    HRESULT hr = g_DX12.GetDevice()->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &clearValue,
        IID_PPV_ARGS(&g_canvasTexture12)
    );
    if (FAILED(hr) || !g_canvasTexture12) {
        CleanupCanvasRenderTarget();
        return;
    }

    D3D12_DESCRIPTOR_HEAP_DESC canvasRtvHeapDesc = {};
    canvasRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    canvasRtvHeapDesc.NumDescriptors = 1;
    canvasRtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = g_DX12.GetDevice()->CreateDescriptorHeap(&canvasRtvHeapDesc, IID_PPV_ARGS(&g_canvasRtvHeap));
    if (FAILED(hr) || !g_canvasRtvHeap) {
        CleanupCanvasRenderTarget();
        return;
    }

    g_canvasRtvHandle = g_canvasRtvHeap->GetCPUDescriptorHandleForHeapStart();
    g_DX12.GetDevice()->CreateRenderTargetView(g_canvasTexture12, nullptr, g_canvasRtvHandle);

    if (!g_DX12.AllocateSrv(&g_canvasTextureSrvCpuHandle, &g_canvasTextureSrvGpuHandle)) {
        CleanupCanvasRenderTarget();
        return;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.PlaneSlice = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    g_DX12.GetDevice()->CreateShaderResourceView(g_canvasTexture12, &srvDesc, g_canvasTextureSrvCpuHandle);
}

void CleanupCanvasRenderTarget() {
    if (g_canvasTextureSrvCpuHandle.ptr != 0 && g_canvasTextureSrvGpuHandle.ptr != 0) {
        g_DX12.FreeSrv(g_canvasTextureSrvCpuHandle, g_canvasTextureSrvGpuHandle);
        g_canvasTextureSrvCpuHandle.ptr = 0;
        g_canvasTextureSrvGpuHandle.ptr = 0;
    }

    if (g_canvasRtvHeap) { g_canvasRtvHeap->Release(); g_canvasRtvHeap = nullptr; }
    g_canvasRtvHandle.ptr = 0;
    if (g_canvasTexture12) { g_canvasTexture12->Release(); g_canvasTexture12 = nullptr; }
    g_canvasRTWidth = 0.0f;
    g_canvasRTHeight = 0.0f;
}

void RenderCanvasToTexture(int width, int height) {
    if (!g_canvasTexture12 || !g_canvasRtvHeap ||
        !g_DX12.GetCommandAllocator() || !g_DX12.GetCommandList() || !g_DX12.GetCommandQueue()) {
        return;
    }

    if (FAILED(g_DX12.GetCommandAllocator()->Reset()) ||
        FAILED(g_DX12.GetCommandList()->Reset(g_DX12.GetCommandAllocator(), nullptr))) {
        return;
    }

    ID3D12DescriptorHeap* heaps[] = { g_DX12.GetSrvHeap() };
    g_DX12.GetCommandList()->SetDescriptorHeaps(1, heaps);

    if (g_CanvasRendererReady) {
        bool renderOk = g_CanvasRenderer.Render(
            g_DX12.GetCommandList(),
            g_Canvas,
            g_canvasTexture12,
            g_canvasRtvHandle,
            width, height
        );
        if (!renderOk) {
            Logger::Get().Error("CanvasRendererDX12::Render failed this frame.");
        }
    } else {
        // Fallback: clear to grey
        D3D12_RESOURCE_BARRIER toRT = {};
        toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRT.Transition.pResource = g_canvasTexture12;
        toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toRT.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_DX12.GetCommandList()->ResourceBarrier(1, &toRT);
        float clearColor[4] = { 0.12f, 0.12f, 0.14f, 1.0f };
        g_DX12.GetCommandList()->ClearRenderTargetView(g_canvasRtvHandle, clearColor, 0, nullptr);
        D3D12_RESOURCE_BARRIER toSRV = toRT;
        toSRV.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toSRV.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_DX12.GetCommandList()->ResourceBarrier(1, &toSRV);
    }

    g_DX12.GetCommandList()->Close();
    ID3D12CommandList* commandLists[] = { g_DX12.GetCommandList() };
    g_DX12.GetCommandQueue()->ExecuteCommandLists(1, commandLists);
    g_DX12.WaitForGpu();
}

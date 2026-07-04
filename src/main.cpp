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
#include "core/KeymapManager.h"

// Chained GLFW Key Callback for Layout-Independent OEM Shortcuts
static GLFWkeyfun g_PrevKeyCallback = nullptr;
static void CustomKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    KeymapManager::Get().ProcessKeyEvent(key, scancode, action, mods);
    if (g_PrevKeyCallback) {
        g_PrevKeyCallback(window, key, scancode, action, mods);
    }
}

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
enum class ActiveTool { Brush, Eraser, Pan };
static ActiveTool g_ActiveTool = ActiveTool::Brush;
static BrushSettings g_Brush;
static bool g_IsPainting = false;
static float g_PrevCanvasMouseX = 0.0f;
static float g_PrevCanvasMouseY = 0.0f;

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
    return g_Canvas.LoadImageToLayer(g_pd3dDevice, filepath);
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
    std::string scriptPath = "";
    std::string configPath = "config.json";
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

    // 2. Initialize Core Logging & Configuration Systems
    Logger::Get().Init("rayv_paint.log");
    Logger::Get().Info("===================================================");
    Logger::Get().Info("Starting RayVPaint tech-art editor...");

    ConfigManager::Get().Load(configPath);
    
    // Set logger level from config
    std::string cfgLevel = ConfigManager::Get().GetLogLevel();
    if (cfgLevel == "debug") Logger::Get().SetMinLevel(LogLevel::LogLevel_Debug);
    else if (cfgLevel == "warn") Logger::Get().SetMinLevel(LogLevel::LogLevel_Warning);
    else if (cfgLevel == "error") Logger::Get().SetMinLevel(LogLevel::LogLevel_Error);
    else Logger::Get().SetMinLevel(LogLevel::LogLevel_Info);

    // 3. Initialize Concurrency (ThreadPool)
    unsigned int numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 2;
    ThreadPool::Get().Init(numThreads);

    // 4. Initialize Scripting Engine
    ScriptingEngine::Get().Initialize();

    // Resize canvas default dimensions according to config
    g_Canvas.ResizeCanvas(nullptr, ConfigManager::Get().GetDefaultWidth(), ConfigManager::Get().GetDefaultHeight());

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
    GLFWwindow* window = glfwCreateWindow(1280, 720, "RayVPaint - Tech Art Editor", nullptr, nullptr);
    if (!window) {
        Logger::Get().Error("Failed to create GLFW window");
        glfwTerminate();
        return 1;
    }

    HWND hWnd = glfwGetWin32Window(window);

    // Initialize KeymapManager (requires GLFW initialized and window created for scancode resolution)
    KeymapManager::Get().Initialize();
    KeymapManager::Get().Load();

    // 6. Initialize DirectX 11
    if (!CreateDeviceD3D(hWnd, headlessMode)) {
        Logger::Get().Error("Failed to initialize DirectX 11");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    CreateRenderTarget();

    // 7. Initialize Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // Load Segoe UI from system fonts for premium typography
    std::string fontPath = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (std::filesystem::exists(fontPath)) {
        io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 17.0f);
    } else {
        io.Fonts->AddFontDefault();
    }

    // Apply configured theme
    ApplyTheme(ConfigManager::Get().GetTheme());

    ImGui_ImplGlfw_InitForOther(window, true);
    g_PrevKeyCallback = glfwSetKeyCallback(window, CustomKeyCallback);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // 8. Initialize Canvas Renderer
    if (!g_Canvas.Initialize(g_pd3dDevice)) {
        Logger::Get().Error("Failed to initialize Canvas renderer");
        CleanupDeviceD3D();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // Check for crash recovery / autosave restore
    std::string backupDir = ConfigManager::Get().GetBackupDir();
    std::string backupPath = backupDir + "/autosave_backup.rayp";
    bool showRecoveryModal = std::filesystem::exists(backupPath);

    // Load startup image if specified on CLI
    if (!startupImagePath.empty()) {
        g_Canvas.LoadImageToLayer(g_pd3dDevice, startupImagePath);
    }

    // Measure startup time
    auto startupEnd = std::chrono::high_resolution_clock::now();
    g_StartupTimeMs = std::chrono::duration<double, std::milli>(startupEnd - startupStart).count();
    Logger::Get().Info("Startup completed in: " + std::to_string(g_StartupTimeMs) + " ms");

    // Execute script from CLI if provided
    if (!scriptPath.empty()) {
        ScriptingEngine::Get().RunScript(scriptPath);
    }

    // Trackers
    int currentWindowWidth = 1280;
    int currentWindowHeight = 720;
    glfwGetFramebufferSize(window, &currentWindowWidth, &currentWindowHeight);

    float s_FrameTimeMs = 0.0f;
    float s_FPS = 0.0f;

    // UI state flags
    bool showConsole = true;
    bool showProperties = true;
    bool showLayers = true;
    bool showToolbar = true;
    bool showColors = true;

    // Modals
    bool openImportModal = false;
    bool openExportDdsModal = false;
    bool openExportStdModal = false;
    bool openSettingsModal = false;
    bool openSaveRaypModal = false;
    bool openLoadRaypModal = false;
    bool openCanvasSizeModal = false;

    auto lastAutoSaveTime = std::chrono::steady_clock::now();
    bool isAutoSaving = false;

    // 9. Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

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

        ImGuiViewport* mainViewport = ImGui::GetMainViewport();

        // 9.1 Persistent Header (Main Menu Bar)
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open Project (.rayp)", KeymapManager::Get().GetActionShortcutString("OpenProject").c_str())) {
                    openLoadRaypModal = true;
                }
                if (ImGui::MenuItem("Save Project (.rayp)", KeymapManager::Get().GetActionShortcutString("SaveProject").c_str())) {
                    openSaveRaypModal = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Import Image...", "Ctrl+I")) {
                    openImportModal = true;
                }
                if (ImGui::BeginMenu("Export...")) {
                    if (ImGui::MenuItem("Natively to DDS...")) {
                        openExportDdsModal = true;
                    }
                    if (ImGui::MenuItem("Standard formats (PNG/JPG)...")) {
                        openExportStdModal = true;
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Settings / Preferences...")) {
                    openSettingsModal = true;
                }
                if (ImGui::MenuItem("Save Settings")) {
                    ConfigManager::Get().Save();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Alt+F4")) {
                    glfwSetWindowShouldClose(window, true);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                std::string undoLabel = "Undo";
                if (g_Canvas.CanUndo()) {
                    undoLabel += " (" + g_Canvas.GetUndoName() + ")";
                }
                if (ImGui::MenuItem(undoLabel.c_str(), KeymapManager::Get().GetActionShortcutString("Undo").c_str(), false, g_Canvas.CanUndo())) {
                    g_Canvas.Undo();
                }

                std::string redoLabel = "Redo";
                if (g_Canvas.CanRedo()) {
                    redoLabel += " (" + g_Canvas.GetRedoName() + ")";
                }
                if (ImGui::MenuItem(redoLabel.c_str(), KeymapManager::Get().GetActionShortcutString("Redo").c_str(), false, g_Canvas.CanRedo())) {
                    g_Canvas.Redo();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Canvas")) {
                if (ImGui::MenuItem("Canvas Size...")) {
                    openCanvasSizeModal = true;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Toolbar", nullptr, &showToolbar);
                ImGui::MenuItem("Properties", nullptr, &showProperties);
                ImGui::MenuItem("Layers", nullptr, &showLayers);
                ImGui::MenuItem("Colors Window", nullptr, &showColors);
                ImGui::MenuItem("Console logs", nullptr, &showConsole);
                ImGui::Separator();
                if (ImGui::MenuItem("Reset View")) {
                    g_Canvas.ResetView();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Scripting")) {
                if (ImGui::MenuItem("Run test command")) {
                    ScriptingEngine::Get().RunString("import rayv; rayv.log_warn('Executing scripting check.')");
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // 9.2 Persistent Footer (Status Bar)
        ImGui::BeginViewportSideBar("##StatusBar", mainViewport, ImGuiDir_Down, 22.0f, 
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
        
        ImGui::Text("Startup: %.1f ms | Frame: %.2f ms | FPS: %.1f | Canvas: %d x %d | Zoom: %.0f%% | Threads: %d | Tool: %s", 
            g_StartupTimeMs, s_FrameTimeMs, s_FPS, g_Canvas.GetWidth(), g_Canvas.GetHeight(), g_Canvas.GetZoom() * 100.0f, numThreads,
            (g_ActiveTool == ActiveTool::Brush ? "Brush" : (g_ActiveTool == ActiveTool::Eraser ? "Eraser" : "Pan")));
        
        ImGui::End();

        // 9.3 DockSpace Configuration
        ImGui::DockSpaceOverViewport(0, mainViewport);

        // Popups/Modals Integration
        if (openImportModal) {
            ImGui::OpenPopup("Import Image");
            openImportModal = false;
        }
        if (openExportDdsModal) {
            ImGui::OpenPopup("Export DDS");
            openExportDdsModal = false;
        }
        if (openExportStdModal) {
            ImGui::OpenPopup("Export Standard Image");
            openExportStdModal = false;
        }
        if (openSettingsModal) {
            ImGui::OpenPopup("Settings");
            openSettingsModal = false;
        }
        if (openCanvasSizeModal) {
            ImGui::OpenPopup("Canvas Size");
            openCanvasSizeModal = false;
        }
        if (openSaveRaypModal) {
            ImGui::OpenPopup("Save Project");
            openSaveRaypModal = false;
        }
        if (openLoadRaypModal) {
            ImGui::OpenPopup("Load Project");
            openLoadRaypModal = false;
        }
        if (showRecoveryModal) {
            ImGui::OpenPopup("Restore Auto-Saved Session?");
            showRecoveryModal = false;
        }

        // Import Popup Modal
        if (ImGui::BeginPopupModal("Import Image", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char importPath[512] = "";
            ImGui::Text("Enter absolute path to image:");
            ImGui::InputText("##importpath", importPath, IM_ARRAYSIZE(importPath));
            ImGui::Separator();
            if (ImGui::Button("Import", ImVec2(120, 0))) {
                if (g_Canvas.LoadImageToLayer(g_pd3dDevice, importPath)) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Export DDS Popup Modal
        if (ImGui::BeginPopupModal("Export DDS", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char exportPath[512] = "export.dds";
            static int formatChoice = 0; 
            ImGui::Text("Enter export path:");
            ImGui::InputText("##exportpath", exportPath, IM_ARRAYSIZE(exportPath));
            ImGui::Text("DDS Format:");
            static const char* formatNames[] = {
                "8-bit SDR (RGBA8)",
                "16-bit SDR (RGBA16)",
                "16-bit float HDR (RGBA16F)",
                "32-bit float HDR (RGBA32F)",
                "8-bit Grayscale (R8)",
                "16-bit float Grayscale (R16F)",
                "32-bit float Grayscale (R32F)"
            };
            ImGui::Combo("##ddsformat", &formatChoice, formatNames, IM_ARRAYSIZE(formatNames));
            ImGui::Separator();
            if (ImGui::Button("Export", ImVec2(120, 0))) {
                DdsFormat fmt = DdsFormat::RGBA8_UNORM;
                if (formatChoice == 1) fmt = DdsFormat::RGBA16_UNORM;
                else if (formatChoice == 2) fmt = DdsFormat::RGBA16_FLOAT;
                else if (formatChoice == 3) fmt = DdsFormat::RGBA32_FLOAT;
                else if (formatChoice == 4) fmt = DdsFormat::R8_UNORM;
                else if (formatChoice == 5) fmt = DdsFormat::R16_FLOAT;
                else if (formatChoice == 6) fmt = DdsFormat::R32_FLOAT;
                
                if (g_Canvas.SaveCanvas(exportPath, fmt)) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Export Standard Image Popup Modal
        if (ImGui::BeginPopupModal("Export Standard Image", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char exportPath[512] = "export.png";
            static char iccPath[512] = "";
            ImGui::Text("Enter export path (PNG, JPG, BMP, TGA):");
            ImGui::InputText("##exportpathstd", exportPath, IM_ARRAYSIZE(exportPath));
            ImGui::Spacing();
            ImGui::Text("Optional ICC Profile (PNG only):");
            ImGui::InputText("##iccpath", iccPath, IM_ARRAYSIZE(iccPath));
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                iccPath[0] = '\0';
            }
            ImGui::TextDisabled("Leave empty for default sRGB colorspace");
            ImGui::Separator();
            if (ImGui::Button("Export", ImVec2(120, 0))) {
                if (g_Canvas.SaveCanvasStandard(exportPath, iccPath)) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        // Settings / Preferences Popup Modal
        if (ImGui::BeginPopupModal("Settings", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char activeTheme[64] = "";
            static int defW = ConfigManager::Get().GetDefaultWidth();
            static int defH = ConfigManager::Get().GetDefaultHeight();
            static char backupDir[256] = "";
            static int autoSaveMins = ConfigManager::Get().GetAutoSaveIntervalMinutes();
            static int maxUndo = ConfigManager::Get().GetMaxUndoSteps();
            static int maxUndoMem = ConfigManager::Get().GetMaxUndoMemoryMB();
            static bool settingsInitialized = false;

            if (!settingsInitialized) {
                std::strncpy(activeTheme, ConfigManager::Get().GetTheme().c_str(), sizeof(activeTheme));
                std::strncpy(backupDir, ConfigManager::Get().GetBackupDir().c_str(), sizeof(backupDir));
                settingsInitialized = true;
            }

            if (ImGui::BeginTabBar("SettingsTabs")) {
                if (ImGui::BeginTabItem("General")) {
                    ImGui::Spacing();
                    ImGui::Text("Interface Settings");
                    ImGui::Separator();
                    
                    const char* themes[] = { "Dark", "Light", "Classic" };
                    int currentThemeIdx = 0;
                    if (std::strcmp(activeTheme, "Light") == 0) currentThemeIdx = 1;
                    else if (std::strcmp(activeTheme, "Classic") == 0) currentThemeIdx = 2;

                    if (ImGui::Combo("Theme", &currentThemeIdx, themes, IM_ARRAYSIZE(themes))) {
                        std::strncpy(activeTheme, themes[currentThemeIdx], sizeof(activeTheme));
                        ApplyTheme(activeTheme); // Instantly apply styling for visual feedback!
                    }

                    ImGui::Spacing();
                    ImGui::Text("Canvas Defaults");
                    ImGui::Separator();
                    ImGui::InputInt("Default Width", &defW, 128, 256);
                    ImGui::InputInt("Default Height", &defH, 128, 256);

                    ImGui::Spacing();
                    ImGui::Text("Autosave & Backup System");
                    ImGui::Separator();
                    ImGui::InputText("Backups Directory", backupDir, IM_ARRAYSIZE(backupDir));
                    ImGui::SliderInt("Autosave (minutes)", &autoSaveMins, 0, 60, "%d min");
                    ImGui::TextDisabled("Set to 0 to disable periodic auto-saves");

                    ImGui::Spacing();
                    ImGui::Text("Undo / Redo Cache Limits");
                    ImGui::Separator();
                    ImGui::SliderInt("Max History Steps", &maxUndo, 5, 200, "%d steps");
                    ImGui::SliderInt("Max RAM Cache Size", &maxUndoMem, 64, 2048, "%d MB");
                    
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Keybindings")) {
                    ImGui::Spacing();
                    ImGui::Text("Click 'Rebind' next to an action to assign a new physical hotkey.");
                    ImGui::Separator();
                    ImGui::Spacing();

                    static std::string rebindingAction = "";
                    static bool listeningForKey = false;

                    // Get a local copy of bindings
                    auto bindings = KeymapManager::Get().GetBindings();
                    for (const auto& pair : bindings) {
                        ImGui::PushID(pair.first.c_str());
                        ImGui::Text("%s:", pair.first.c_str());
                        ImGui::SameLine(180);

                        if (listeningForKey && rebindingAction == pair.first) {
                            ImGui::TextColored(ImVec4(0.2f, 0.7f, 1.0f, 1.0f), "[Press any key + Ctrl/Shift/Alt...]");
                            
                            ImGuiIO& io = ImGui::GetIO();
                            for (int k = 0; k < ImGuiKey_NamedKey_END; ++k) {
                                ImGuiKey imguiKey = (ImGuiKey)k;
                                if (ImGui::IsKeyPressed(imguiKey)) {
                                    int glfwKey = 0;
                                    if (imguiKey >= ImGuiKey_A && imguiKey <= ImGuiKey_Z) glfwKey = GLFW_KEY_A + (imguiKey - ImGuiKey_A);
                                    else if (imguiKey >= ImGuiKey_0 && imguiKey <= ImGuiKey_9) glfwKey = GLFW_KEY_0 + (imguiKey - ImGuiKey_0);
                                    else if (imguiKey >= ImGuiKey_F1 && imguiKey <= ImGuiKey_F12) glfwKey = GLFW_KEY_F1 + (imguiKey - ImGuiKey_F1);
                                    else if (imguiKey == ImGuiKey_Space) glfwKey = GLFW_KEY_SPACE;
                                    else if (imguiKey == ImGuiKey_Enter || imguiKey == ImGuiKey_KeypadEnter) glfwKey = GLFW_KEY_ENTER;
                                    else if (imguiKey == ImGuiKey_Escape) glfwKey = GLFW_KEY_ESCAPE;
                                    else if (imguiKey == ImGuiKey_Tab) glfwKey = GLFW_KEY_TAB;
                                    else if (imguiKey == ImGuiKey_Backspace) glfwKey = GLFW_KEY_BACKSPACE;
                                    else if (imguiKey == ImGuiKey_Insert) glfwKey = GLFW_KEY_INSERT;
                                    else if (imguiKey == ImGuiKey_Delete) glfwKey = GLFW_KEY_DELETE;
                                    else if (imguiKey == ImGuiKey_RightArrow) glfwKey = GLFW_KEY_RIGHT;
                                    else if (imguiKey == ImGuiKey_LeftArrow) glfwKey = GLFW_KEY_LEFT;
                                    else if (imguiKey == ImGuiKey_DownArrow) glfwKey = GLFW_KEY_DOWN;
                                    else if (imguiKey == ImGuiKey_UpArrow) glfwKey = GLFW_KEY_UP;
                                    else if (imguiKey == ImGuiKey_Comma) glfwKey = GLFW_KEY_COMMA;
                                    else if (imguiKey == ImGuiKey_Period) glfwKey = GLFW_KEY_PERIOD;
                                    else if (imguiKey == ImGuiKey_Slash) glfwKey = GLFW_KEY_SLASH;
                                    else if (imguiKey == ImGuiKey_Semicolon) glfwKey = GLFW_KEY_SEMICOLON;
                                    else if (imguiKey == ImGuiKey_Equal) glfwKey = GLFW_KEY_EQUAL;
                                    else if (imguiKey == ImGuiKey_Minus) glfwKey = GLFW_KEY_MINUS;
                                    else if (imguiKey == ImGuiKey_LeftBracket) glfwKey = GLFW_KEY_LEFT_BRACKET;
                                    else if (imguiKey == ImGuiKey_RightBracket) glfwKey = GLFW_KEY_RIGHT_BRACKET;
                                    else if (imguiKey == ImGuiKey_Backslash) glfwKey = GLFW_KEY_BACKSLASH;
                                    else if (imguiKey == ImGuiKey_GraveAccent) glfwKey = GLFW_KEY_GRAVE_ACCENT;

                                    if (imguiKey != ImGuiKey_LeftCtrl && imguiKey != ImGuiKey_RightCtrl &&
                                        imguiKey != ImGuiKey_LeftShift && imguiKey != ImGuiKey_RightShift &&
                                        imguiKey != ImGuiKey_LeftAlt && imguiKey != ImGuiKey_RightAlt) {
                                        
                                        if (glfwKey != 0) {
                                            KeyCombination pendingCombo;
                                            pendingCombo.key = glfwKey;
                                            pendingCombo.ctrl = io.KeyCtrl;
                                            pendingCombo.shift = io.KeyShift;
                                            pendingCombo.alt = io.KeyAlt;
                                            
                                            KeymapManager::Get().BindAction(rebindingAction, pendingCombo);
                                            listeningForKey = false;
                                            rebindingAction = "";
                                            break;
                                        }
                                    }
                                }
                            }
                        } else {
                            ImGui::Text("%s", pair.second.ToString().c_str());
                            ImGui::SameLine(320);
                            if (ImGui::Button("Rebind")) {
                                rebindingAction = pair.first;
                                listeningForKey = true;
                            }
                        }
                        ImGui::PopID();
                    }

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("Save & Close", ImVec2(120, 0))) {
                ConfigManager::Get().SetTheme(activeTheme);
                ConfigManager::Get().SetDefaultWidth(defW);
                ConfigManager::Get().SetDefaultHeight(defH);
                ConfigManager::Get().SetBackupDir(backupDir);
                ConfigManager::Get().SetAutoSaveIntervalMinutes(autoSaveMins);
                ConfigManager::Get().SetMaxUndoSteps(maxUndo);
                ConfigManager::Get().SetMaxUndoMemoryMB(maxUndoMem);
                ConfigManager::Get().Save();
                
                // Persist keyboard map configuration
                KeymapManager::Get().Save();
                
                settingsInitialized = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
             if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                // Re-apply original theme and reload keymaps on cancel
                ApplyTheme(ConfigManager::Get().GetTheme());
                KeymapManager::Get().Load();
                settingsInitialized = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Canvas Size Popup Modal
        if (ImGui::BeginPopupModal("Canvas Size", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static int targetW = 0;
            static int targetH = 0;
            static bool initSize = false;
            if (!initSize) {
                targetW = g_Canvas.GetWidth();
                targetH = g_Canvas.GetHeight();
                initSize = true;
            }

            ImGui::Text("Resize Canvas Dimensions:");
            ImGui::Separator();
            ImGui::InputInt("Width", &targetW, 128, 256);
            ImGui::InputInt("Height", &targetH, 128, 256);

            // Clamp positive dimensions
            if (targetW < 1) targetW = 1;
            if (targetH < 1) targetH = 1;

            ImGui::Separator();
            if (ImGui::Button("Resize", ImVec2(120, 0))) {
                g_Canvas.ResizeCanvas(g_pd3dDevice, targetW, targetH);
                initSize = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                initSize = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Save Project (.rayp) Modal
        if (ImGui::BeginPopupModal("Save Project", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char savePath[512] = "project.rayp";
            ImGui::Text("Enter project file path (.rayp):");
            ImGui::InputText("##savepathrayp", savePath, IM_ARRAYSIZE(savePath));
            ImGui::Separator();
            if (ImGui::Button("Save", ImVec2(120, 0))) {
                if (g_Canvas.SaveCanvasRayp(savePath)) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Load Project (.rayp) Modal
        if (ImGui::BeginPopupModal("Load Project", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char loadPath[512] = "project.rayp";
            ImGui::Text("Enter project file path (.rayp):");
            ImGui::InputText("##loadpathrayp", loadPath, IM_ARRAYSIZE(loadPath));
            ImGui::Separator();
            if (ImGui::Button("Load", ImVec2(120, 0))) {
                if (g_Canvas.LoadCanvasRayp(loadPath, g_pd3dDevice)) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Restore Backup Modal
        if (ImGui::BeginPopupModal("Restore Auto-Saved Session?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("It looks like the application closed unexpectedly.");
            ImGui::Text("Would you like to restore your auto-saved session?");
            ImGui::Separator();
            if (ImGui::Button("Restore Session", ImVec2(140, 0))) {
                if (g_Canvas.LoadCanvasRayp(backupPath, g_pd3dDevice)) {
                    Logger::Get().Info("Restored auto-saved session successfully.");
                } else {
                    Logger::Get().Error("Failed to restore auto-saved session.");
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard", ImVec2(140, 0))) {
                try {
                    std::filesystem::remove(backupPath);
                } catch (...) {}
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Keyboard Shortcuts Handler (Layout-Independent via KeymapManager)
        if (!io.WantTextInput) {
            if (KeymapManager::Get().ConsumeActionTrigger("Undo")) {
                g_Canvas.Undo();
            }
            if (KeymapManager::Get().ConsumeActionTrigger("Redo")) {
                g_Canvas.Redo();
            }
            if (KeymapManager::Get().ConsumeActionTrigger("SaveProject")) {
                openSaveRaypModal = true;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("OpenProject")) {
                openLoadRaypModal = true;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("BrushTool")) {
                g_ActiveTool = ActiveTool::Brush;
                g_Brush.erase = false;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("EraserTool")) {
                g_ActiveTool = ActiveTool::Eraser;
                g_Brush.erase = true;
            }
            if (KeymapManager::Get().ConsumeActionTrigger("PanTool")) {
                g_ActiveTool = ActiveTool::Pan;
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
                std::filesystem::create_directories(backupDir);
                Logger::Get().Info("Triggering background auto-save to " + backupPath);
                g_Canvas.SaveCanvasRaypAsync(backupPath, [](bool success) {
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

        // 9.4 Draw Toolbar Panel
        if (showToolbar) {
            ImGui::Begin("Toolbar", &showToolbar, ImGuiWindowFlags_NoCollapse);
            ImGui::Text("Tools");
            ImGui::Separator();
            
            // Brush selector button
            bool isBrush = (g_ActiveTool == ActiveTool::Brush);
            if (isBrush) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            if (ImGui::Button("Brush", ImVec2(-1, 40))) { 
                g_ActiveTool = ActiveTool::Brush; 
                g_Brush.erase = false;
            }
            if (isBrush) ImGui::PopStyleColor();

            // Eraser selector button
            bool isEraser = (g_ActiveTool == ActiveTool::Eraser);
            if (isEraser) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            if (ImGui::Button("Eraser", ImVec2(-1, 40))) { 
                g_ActiveTool = ActiveTool::Eraser; 
                g_Brush.erase = true;
            }
            if (isEraser) ImGui::PopStyleColor();

            // Pan selector button
            bool isPan = (g_ActiveTool == ActiveTool::Pan);
            if (isPan) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            if (ImGui::Button("Pan (LDrag)", ImVec2(-1, 40))) { 
                g_ActiveTool = ActiveTool::Pan; 
            }
            if (isPan) ImGui::PopStyleColor();

            ImGui::Separator();
            if (ImGui::Button("Reset View", ImVec2(-1, 30))) {
                g_Canvas.ResetView();
            }
            ImGui::End();
        }

        // 9.5 Draw Canvas Viewport
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("Canvas Viewport", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
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
            bool isHovered = ImGui::IsItemHovered();

            // Map mouse coordinates to Canvas pixel coordinates using floored origin matching the vertex shader
            float screenOriginX = std::floor(g_Canvas.GetPan().x + static_cast<float>(viewportWidth) * 0.5f);
            float screenOriginY = std::floor(g_Canvas.GetPan().y + static_cast<float>(viewportHeight) * 0.5f);

            float canvasX = (localMouseX - screenOriginX) / g_Canvas.GetZoom();
            float canvasY = (localMouseY - screenOriginY) / g_Canvas.GetZoom();

            // Check if cursor is within active canvas boundary
            bool isInsideCanvas = (canvasX >= 0.0f && canvasX < (float)g_Canvas.GetWidth() &&
                                   canvasY >= 0.0f && canvasY < (float)g_Canvas.GetHeight());

            // Panning: Middle mouse button drag OR left mouse button drag when Pan tool is selected
            bool isPanning = false;
            float dragDx = 0.0f;
            float dragDy = 0.0f;
            if (isHovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || (g_ActiveTool == ActiveTool::Pan && ImGui::IsMouseDragging(ImGuiMouseButton_Left)))) {
                ImGuiMouseButton panButton = ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ? ImGuiMouseButton_Middle : ImGuiMouseButton_Left;
                ImVec2 drag = ImGui::GetMouseDragDelta(panButton);
                dragDx = drag.x - g_LastDragDelta.x;
                dragDy = drag.y - g_LastDragDelta.y;
                g_LastDragDelta = drag;
                isPanning = true;
            } else {
                g_LastDragDelta = ImVec2(0.0f, 0.0f);
            }

            float wheelDelta = isHovered ? ImGui::GetIO().MouseWheel : 0.0f;

            // Handle custom brush visualizer and cursor hiding when inside canvas bounds
            if (isHovered && isInsideCanvas && (g_ActiveTool == ActiveTool::Brush || g_ActiveTool == ActiveTool::Eraser)) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);

                // Draw custom outline circle at mouse position
                ImDrawList* drawList = ImGui::GetForegroundDrawList(); // Use foreground draw list so it renders on top of everything
                float screenRadius = g_Brush.radius * g_Canvas.GetZoom();
                drawList->AddCircle(mousePos, screenRadius, IM_COL32(0, 0, 0, 255), 32, 1.5f);
                drawList->AddCircle(mousePos, screenRadius, IM_COL32(255, 255, 255, 255), 32, 1.0f);
            }

            // Draw interaction logic
            if (isHovered && !isPanning && (g_ActiveTool == ActiveTool::Brush || g_ActiveTool == ActiveTool::Eraser)) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    g_IsPainting = true;
                    g_Canvas.PaintOnActiveLayer(canvasX, canvasY, StrokePhase::Begin, g_Brush);
                } 
                else if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && g_IsPainting) {
                    g_Canvas.PaintOnActiveLayer(canvasX, canvasY, StrokePhase::Update, g_Brush);
                }
            }

            if (g_IsPainting && (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
                g_Canvas.PaintOnActiveLayer(0, 0, StrokePhase::End, g_Brush);
                g_IsPainting = false;
            }

            g_Canvas.Update(viewportWidth, viewportHeight, isHovered, localMouseX, localMouseY, isPanning, dragDx, dragDy, wheelDelta);
        }

        ImGui::End();

        // 9.6 Draw Properties Panel
        if (showProperties) {
            ImGui::Begin("Properties", &showProperties, ImGuiWindowFlags_NoCollapse);
            
            ImGui::Text("Zoom: %.0f%%", g_Canvas.GetZoom() * 100.0f);
            ImGui::Text("Pan: (%.1f, %.1f)", g_Canvas.GetPan().x, g_Canvas.GetPan().y);
            
            ImGui::Separator();
            ImGui::Text("Brush Settings:");
            ImGui::SliderFloat("Radius", &g_Brush.radius, 1.0f, 250.0f, "%.0f px");
            ImGui::SliderFloat("Hardness", &g_Brush.hardness, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Opacity##brush", &g_Brush.opacity, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Spacing##brush", &g_Brush.spacing, 0.01f, 5.0f, "%.2f");
            ImGui::SliderInt("Stabilization##brush", &g_Brush.stabilization, 1, 50, "%d");
            ImGui::ColorEdit4("Color##brush", g_Brush.color, ImGuiColorEditFlags_NoInputs);

            ImGui::Separator();
            ImGui::Text("Channel Filter (Vis Mode):");
            int mode = g_Canvas.GetVisualizationMode();
            ImGui::RadioButton("RGBA (Normal)", &mode, 0);
            ImGui::RadioButton("RGB (No Alpha)", &mode, 1);
            ImGui::RadioButton("Alpha channel", &mode, 2);
            ImGui::RadioButton("Alpha mask", &mode, 3);
            g_Canvas.SetVisualizationMode(mode);

            if (mode == 3) {
                ImGui::ColorEdit3("Mask Color", g_Canvas.GetAlphaMaskColor());
            }

            ImGui::End();
        }

        // 9.6b Draw Layers Panel (Standalone docked window)
        if (showLayers) {
            ImGui::Begin("Layers", &showLayers, ImGuiWindowFlags_NoCollapse);
            
            // Channels Write Mask at the top of Layers panel
            ImGui::Text("Active Layer Write Channels:");
            ImGui::Checkbox("R", &g_Brush.writeR); ImGui::SameLine();
            ImGui::Checkbox("G", &g_Brush.writeG); ImGui::SameLine();
            ImGui::Checkbox("B", &g_Brush.writeB); ImGui::SameLine();
            ImGui::Checkbox("A", &g_Brush.writeA);

            ImGui::Separator();
            
            if (ImGui::Button("Add Layer", ImVec2(-1, 25))) {
                std::string lName = "Layer " + std::to_string(g_Canvas.GetLayers().size() + 1);
                g_Canvas.CreateNewLayer(g_pd3dDevice, lName);
            }

            ImGui::BeginChild("LayersList", ImVec2(0, 0), true);
            auto& layers = g_Canvas.GetLayers();
            for (int i = static_cast<int>(layers.size()) - 1; i >= 0; --i) {
                ImGui::PushID(i);
                
                // Visible toggle (Alt+Click to Isolate Layer)
                bool isIsolated = g_Canvas.IsLayerIsolated(i);
                if (isIsolated) {
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.45f, 0.15f, 1.0f));
                }
                
                bool vis = layers[i].visible;
                if (ImGui::Checkbox("##visible", &vis)) {
                    if (ImGui::GetIO().KeyAlt) {
                        g_Canvas.ToggleLayerIsolation(i);
                    } else {
                        layers[i].visible = vis;
                    }
                }
                if (isIsolated) {
                    ImGui::PopStyleColor();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Alt+Click to Isolate this layer");
                }
                
                ImGui::SameLine();

                // Selectable layer name
                bool isSelected = (g_Canvas.GetActiveLayerIndex() == i);
                if (ImGui::Selectable(layers[i].name.c_str(), isSelected, ImGuiSelectableFlags_None, ImVec2(ImGui::GetContentRegionAvail().x - 70, 0))) {
                    g_Canvas.SetActiveLayerIndex(i);
                }
                
                ImGui::SameLine();
                // Delete button
                if (layers.size() > 1) {
                    if (ImGui::Button("Del")) {
                        g_Canvas.DeleteLayer(i);
                    }
                }

                // Opacity slider for the layer
                ImGui::PushItemWidth(100);
                ImGui::SliderFloat("Opacity", &layers[i].opacity, 0.0f, 1.0f, "%.2f");
                ImGui::PopItemWidth();

                ImGui::Separator();
                ImGui::PopID();
            }
            ImGui::EndChild();

            ImGui::End();
        }

        // 9.7 Draw Logging Console Panel
        if (showConsole) {
            ImGui::Begin("Console Logs", &showConsole);
            if (ImGui::Button("Clear")) {
                Logger::Get().ClearRecentLogs();
            }
            ImGui::Separator();
            ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
            auto logs = Logger::Get().GetRecentLogs();
            for (const auto& log : logs) {
                if (log.find("[ERROR]") != std::string::npos) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), log.c_str());
                } else if (log.find("[WARN ]") != std::string::npos) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), log.c_str());
                } else if (log.find("[DEBUG]") != std::string::npos) {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.8f, 1.0f), log.c_str());
                } else {
                    ImGui::TextUnformatted(log.c_str());
                }
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
            ImGui::End();
        }

        // 9.8 Draw Colors Panel
        if (showColors) {
            ImGui::Begin("Colors", &showColors);
            
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_AlphaPreview;
            ImGui::ColorPicker4("##color_picker", g_Brush.color, flags);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Quick Palette:");
            static const ImVec4 paletteColors[] = {
                ImVec4(0, 0, 0, 1), ImVec4(1, 1, 1, 1), ImVec4(0.5f, 0.5f, 0.5f, 1), ImVec4(0.75f, 0.75f, 0.75f, 1),
                ImVec4(1, 0, 0, 1), ImVec4(1, 1, 0, 1), ImVec4(0, 1, 0, 1), ImVec4(0, 1, 1, 1),
                ImVec4(0, 0, 1, 1), ImVec4(1, 0, 1, 1), ImVec4(0.5f, 0, 0, 1), ImVec4(0.5f, 0.5f, 0, 1),
                ImVec4(0, 0.5f, 0, 1), ImVec4(0, 0.5f, 0.5f, 1), ImVec4(0, 0, 0.5f, 1), ImVec4(0.5f, 0, 0.5f, 1)
            };
            for (int i = 0; i < IM_ARRAYSIZE(paletteColors); ++i) {
                ImGui::PushID(i);
                if (i > 0 && i % 8 != 0) ImGui::SameLine();
                if (ImGui::ColorButton("##palette_color", paletteColors[i], ImGuiColorEditFlags_NoTooltip, ImVec2(22, 22))) {
                    g_Brush.color[0] = paletteColors[i].x;
                    g_Brush.color[1] = paletteColors[i].y;
                    g_Brush.color[2] = paletteColors[i].z;
                    g_Brush.color[3] = paletteColors[i].w;
                }
                ImGui::PopID();
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
        s_FrameTimeMs = std::chrono::duration<float, std::milli>(loopEnd - loopStart).count();
        
        static float frameTimeAccumulator = 0.0f;
        static int frameCount = 0;
        frameTimeAccumulator += s_FrameTimeMs;
        frameCount++;
        if (frameTimeAccumulator >= 500.0f) { // Update FPS every 500ms
            s_FPS = 1000.0f / (frameTimeAccumulator / frameCount);
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
    glfwDestroyWindow(window);
    glfwTerminate();

    ScriptingEngine::Get().Shutdown();
    ThreadPool::Get().Shutdown();
    Logger::Get().Info("RayVPaint shut down cleanly.");
    Logger::Get().Shutdown();

    return 0;
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

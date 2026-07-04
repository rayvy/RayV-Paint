#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

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

// Forward declarations
bool CreateDeviceD3D(HWND hWnd, bool useNullDriver = false);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
void ResizeCanvasRenderTarget(float width, float height);
void CleanupCanvasRenderTarget();
void RenderCanvasToTexture(float width, float height);

// Core API bindings exported to Scripting Engine
void TriggerCanvasResize(int w, int h) {
    g_Canvas.ResizeCanvas(w, h);
    Logger::Get().Info("Canvas resized to: " + std::to_string(w) + "x" + std::to_string(h));
}
float GetCanvasZoom() { return g_Canvas.GetZoom(); }
void SetCanvasZoom(float zoom) { g_Canvas.SetZoom(zoom); }
void SetCanvasPan(float x, float y) { g_Canvas.SetPan(DirectX::XMFLOAT2(x, y)); }
void ResetCanvasView() { g_Canvas.ResetView(); }

int main(int argc, char* argv[]) {
    auto startupStart = std::chrono::high_resolution_clock::now();

    // 1. CLI Arguments parsing
    bool testMode = false;
    bool headlessMode = false;
    std::string scriptPath = "";
    std::string configPath = "config.json";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--test") {
            testMode = true;
        } else if (arg == "--headless") {
            headlessMode = true;
            testMode = true; // Headless implies auto-testing behavior
        } else if (arg == "--version") {
            std::cout << "RayVPaint - Tech Art Editor - Version 0.1.0 (Alpha)" << std::endl;
            return 0;
        } else if (arg == "--script" && i + 1 < argc) {
            scriptPath = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        }
    }

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
    g_Canvas.ResizeCanvas(ConfigManager::Get().GetDefaultWidth(), ConfigManager::Get().GetDefaultHeight());

    // 5. Initialize GLFW (if not in true headless mode)
    if (!glfwInit()) {
        Logger::Get().Error("Failed to initialize GLFW");
        return 1;
    }

    // Configure window visibility (hidden for headless / test mode)
    if (testMode) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Using DirectX 11, not OpenGL
    GLFWwindow* window = glfwCreateWindow(1280, 720, "RayVPaint - Tech Art Editor", nullptr, nullptr);
    if (!window) {
        Logger::Get().Error("Failed to create GLFW window");
        glfwTerminate();
        return 1;
    }

    HWND hWnd = glfwGetWin32Window(window);

    // 6. Initialize DirectX 11 (uses Null Driver in Headless/Test mode if windowing/GPU is missing, else normal hardware)
    bool useNullDriver = headlessMode;
    if (!CreateDeviceD3D(hWnd, useNullDriver)) {
        Logger::Get().Error("Failed to initialize DirectX 11 device and swap chain");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    CreateRenderTarget();

    // 7. Initialize Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;           // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;         // Enable Multi-Viewport / Platform Windows

    ImGui::StyleColorsDark();
    
    // Customize styles for a premium modern look
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                   = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_Border]                 = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBg]                = ImVec4(0.15f, 0.15f, 0.17f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.20f, 0.20f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.25f, 0.25f, 0.30f, 1.00f);
    colors[ImGuiCol_TitleBg]                = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_Header]                 = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.25f, 0.25f, 0.35f, 1.00f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.30f, 0.30f, 0.45f, 1.00f);
    colors[ImGuiCol_Button]                 = ImVec4(0.20f, 0.20f, 0.25f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.30f, 0.30f, 0.40f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.40f, 0.40f, 0.55f, 1.00f);
    colors[ImGuiCol_Tab]                    = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.25f, 0.25f, 0.35f, 1.00f);
    colors[ImGuiCol_TabActive]              = ImVec4(0.20f, 0.20f, 0.28f, 1.00f);
    colors[ImGuiCol_DockingPreview]         = ImVec4(0.35f, 0.45f, 0.65f, 0.70f);

    ImGui_ImplGlfw_InitForOther(window, true);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // 8. Initialize Canvas Renderer
    if (!g_Canvas.Initialize(g_pd3dDevice)) {
        Logger::Get().Error("Failed to initialize Canvas renderer");
        CleanupDeviceD3D();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // Measure startup time
    auto startupEnd = std::chrono::high_resolution_clock::now();
    g_StartupTimeMs = std::chrono::duration<double, std::milli>(startupEnd - startupStart).count();
    Logger::Get().Info("Startup completed in: " + std::to_string(g_StartupTimeMs) + " ms");

    // Execute script from CLI if provided
    if (!scriptPath.empty()) {
        ScriptingEngine::Get().RunScript(scriptPath);
    }

    // Performance trackers
    auto frameStart = std::chrono::high_resolution_clock::now();
    float s_FrameTimeMs = 0.0f;
    float s_FPS = 0.0f;

    // UI state flags
    bool showConsole = true;
    bool showProperties = true;
    bool showToolbar = true;

    // 9. Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Start frame timing
        auto loopStart = std::chrono::high_resolution_clock::now();

        // Start Dear ImGui Frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* mainViewport = ImGui::GetMainViewport();

        // 9.1 Persistent Header (Main Menu Bar)
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Save Config", "Ctrl+S")) {
                    ConfigManager::Get().Save();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Alt+F4")) {
                    glfwSetWindowShouldClose(window, true);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Toolbar", nullptr, &showToolbar);
                ImGui::MenuItem("Properties", nullptr, &showProperties);
                ImGui::MenuItem("Console logs", nullptr, &showConsole);
                ImGui::Separator();
                if (ImGui::MenuItem("Reset View")) {
                    g_Canvas.ResetView();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Scripting")) {
                if (ImGui::MenuItem("Run test command")) {
                    ScriptingEngine::Get().RunString("import rayv; rayv.log_warn('Running test command from UI!')");
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // 9.2 Persistent Footer (Status Bar)
        ImGui::BeginViewportSideBar("##StatusBar", mainViewport, ImGuiDir_Down, 22.0f, 
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
        
        ImGui::Text("Startup: %.1f ms | Frame: %.2f ms | FPS: %.1f | Canvas: %d x %d | Zoom: %.0f%% | Threads Active: %d", 
            g_StartupTimeMs, s_FrameTimeMs, s_FPS, g_Canvas.GetWidth(), g_Canvas.GetHeight(), g_Canvas.GetZoom() * 100.0f, numThreads);
        
        ImGui::End();

        // 9.3 DockSpace Configuration
        ImGui::DockSpaceOverViewport(0, mainViewport);

        // 9.4 Draw Toolbar Panel
        if (showToolbar) {
            ImGui::Begin("Toolbar", &showToolbar, ImGuiWindowFlags_NoCollapse);
            ImGui::Text("Tools");
            ImGui::Separator();
            if (ImGui::Button("Brush", ImVec2(-1, 40))) { /* Brush selection */ }
            if (ImGui::Button("Eraser", ImVec2(-1, 40))) { /* Eraser selection */ }
            if (ImGui::Button("Pan (MDrag)", ImVec2(-1, 40))) { /* Pan selection */ }
            ImGui::Separator();
            if (ImGui::Button("Reset View", ImVec2(-1, 30))) {
                g_Canvas.ResetView();
            }
            ImGui::End();
        }

        // 9.5 Draw Canvas Viewport
        ImGui::Begin("Canvas Viewport", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        
        ImVec2 viewportPanelSize = ImGui::GetContentRegionAvail();
        float viewportWidth = viewportPanelSize.x;
        float viewportHeight = viewportPanelSize.y;

        if (viewportWidth > 0 && viewportHeight > 0) {
            ResizeCanvasRenderTarget(viewportWidth, viewportHeight);

            ImVec2 canvasWindowPos = ImGui::GetCursorScreenPos();
            ImVec2 mousePos = ImGui::GetMousePos();
            float localMouseX = mousePos.x - canvasWindowPos.x;
            float localMouseY = mousePos.y - canvasWindowPos.y;

            bool isHovered = ImGui::IsWindowHovered();

            bool isPanning = false;
            float dragDx = 0.0f;
            float dragDy = 0.0f;
            if (isHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
                dragDx = drag.x - g_LastDragDelta.x;
                dragDy = drag.y - g_LastDragDelta.y;
                g_LastDragDelta = drag;
                isPanning = true;
            } else {
                g_LastDragDelta = ImVec2(0.0f, 0.0f);
            }

            float wheelDelta = isHovered ? ImGui::GetIO().MouseWheel : 0.0f;

            g_Canvas.Update(viewportWidth, viewportHeight, isHovered, localMouseX, localMouseY, isPanning, dragDx, dragDy, wheelDelta);

            RenderCanvasToTexture(viewportWidth, viewportHeight);

            ImGui::Image((void*)g_canvasSRV, ImVec2(viewportWidth, viewportHeight));
        }

        ImGui::End();

        // 9.6 Draw Properties Panel
        if (showProperties) {
            ImGui::Begin("Properties", &showProperties, ImGuiWindowFlags_NoCollapse);
            
            // Allow manual resizing of the Canvas
            int curW = g_Canvas.GetWidth();
            int curH = g_Canvas.GetHeight();
            
            ImGui::Text("Canvas Size:");
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputInt("Width", &curW, 128, 256)) {
                g_Canvas.ResizeCanvas(curW, curH);
            }
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputInt("Height", &curH, 128, 256)) {
                g_Canvas.ResizeCanvas(curW, curH);
            }
            
            ImGui::Separator();
            ImGui::Text("Zoom: %.0f%%", g_Canvas.GetZoom() * 100.0f);
            ImGui::Text("Pan: (%.1f, %.1f)", g_Canvas.GetPan().x, g_Canvas.GetPan().y);
            ImGui::Separator();
            ImGui::Text("Layers");
            if (ImGui::BeginListBox("##layers", ImVec2(-1, 150))) {
                ImGui::Selectable("Base Layer (diffuse)", true);
                ImGui::EndListBox();
            }
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
    
    // In headless test mode, we do not need a real swapchain/window output, but CreateDeviceAndSwapChain expects a HWND.
    // If null driver is used, we can still call it or create device without swapchain.
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
    // Swap chain won't be fully initialized or active in headless Null Driver mode, check for safety
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

void ResizeCanvasRenderTarget(float width, float height) {
    // If the size hasn't changed, do nothing
    if (g_canvasTexture && g_canvasRTWidth == width && g_canvasRTHeight == height) {
        return;
    }

    CleanupCanvasRenderTarget();

    g_canvasRTWidth = width;
    g_canvasRTHeight = height;

    // Create texture description
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;

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

void RenderCanvasToTexture(float width, float height) {
    if (!g_canvasRTV) return;

    // Set viewport to target texture size
    D3D11_VIEWPORT vp = {};
    vp.Width = width;
    vp.Height = height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    g_pd3dDeviceContext->RSSetViewports(1, &vp);

    // Clear texture render target
    float clearColor[4] = { 0.12f, 0.12f, 0.14f, 1.0f }; // Slate background matches ImGui Child window
    g_pd3dDeviceContext->ClearRenderTargetView(g_canvasRTV, clearColor);

    // Bind texture RTV
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_canvasRTV, nullptr);

    // Render the canvas
    g_Canvas.Render(g_pd3dDeviceContext, width, height);

    // Unbind render targets
    ID3D11RenderTargetView* nullRTV = nullptr;
    g_pd3dDeviceContext->OMSetRenderTargets(1, &nullRTV, nullptr);
}

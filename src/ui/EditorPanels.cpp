#include "EditorPanels.h"
#include "../core/ConfigManager.h"
#include "../core/Logger.h"
#include "../core/KeymapManager.h"
#include "../core/ImageManager.h"
#include "../scripting/ScriptingEngine.h"
#include "../core/ThreadPool.h"
#include <thread>
#include <imgui_internal.h>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <cmath>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

extern void ApplyTheme(const std::string& themeName);
extern bool g_IsLayersHovered;
extern bool g_IsViewportHovered;
extern std::vector<float> Canvas_BuildSplineLUT(const std::vector<std::pair<float,float>>& pts);

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

static std::wstring UTF8ToWString(const std::string& str) {
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

static std::string WStringToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

static std::wstring ConvertFilterToWString(const char* filter) {
    if (!filter) return L"";
    std::vector<char> filterBuffer;
    const char* p = filter;
    while (true) {
        if (*p == '\0' && *(p + 1) == '\0') {
            filterBuffer.push_back('\0');
            filterBuffer.push_back('\0');
            break;
        }
        filterBuffer.push_back(*p);
        p++;
    }
    int size = static_cast<int>(filterBuffer.size());
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, filterBuffer.data(), size, NULL, 0);
    std::wstring wfilter(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, filterBuffer.data(), size, &wfilter[0], size_needed);
    return wfilter;
}

static bool ShowOpenFileWin32(char* outPath, size_t maxLen, const char* filter = "All Files (*.*)\0*.*\0") {
    OPENFILENAMEW ofn;
    wchar_t szFile[512] = { 0 };
    if (outPath && strlen(outPath) > 0) {
        std::wstring wpath = UTF8ToWString(outPath);
        std::wcsncpy(szFile, wpath.c_str(), sizeof(szFile)/sizeof(wchar_t) - 1);
    }
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile)/sizeof(wchar_t);
    std::wstring wfilter = ConvertFilterToWString(filter);
    ofn.lpstrFilter = wfilter.c_str();
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn) == TRUE) {
        std::string utf8Path = WStringToUTF8(ofn.lpstrFile);
        std::strncpy(outPath, utf8Path.c_str(), maxLen - 1);
        outPath[maxLen - 1] = '\0';
        return true;
    }
    return false;
}

static bool ShowSaveFileWin32(char* outPath, size_t maxLen, const char* filter = "All Files (*.*)\0*.*\0") {
    OPENFILENAMEW ofn;
    wchar_t szFile[512] = { 0 };
    if (outPath && strlen(outPath) > 0) {
        std::wstring wpath = UTF8ToWString(outPath);
        std::wcsncpy(szFile, wpath.c_str(), sizeof(szFile)/sizeof(wchar_t) - 1);
    }
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile)/sizeof(wchar_t);
    std::wstring wfilter = ConvertFilterToWString(filter);
    ofn.lpstrFilter = wfilter.c_str();
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

    if (GetSaveFileNameW(&ofn) == TRUE) {
        std::string utf8Path = WStringToUTF8(ofn.lpstrFile);
        std::strncpy(outPath, utf8Path.c_str(), maxLen - 1);
        outPath[maxLen - 1] = '\0';
        return true;
    }
    return false;
}
#else
static bool ShowOpenFileWin32(char* outPath, size_t maxLen, const char* filter = "All Files (*.*)\0*.*\0") { return false; }
static bool ShowSaveFileWin32(char* outPath, size_t maxLen, const char* filter = "All Files (*.*)\0*.*\0") { return false; }
#endif

namespace UI {

    DocumentLoadingState g_LoadingState;

    void TriggerBackgroundOpenDocument(const std::string& filepath, ID3D11Device* device, Canvas& canvas) {
        if (g_LoadingState.isLoading) return;
        
        g_LoadingState.isLoading = true;
        g_LoadingState.progress = 0.0f;
        g_LoadingState.filepath = filepath;
        g_LoadingState.completed = false;
        g_LoadingState.success = false;
        {
            std::lock_guard<std::mutex> lock(g_LoadingState.mutex);
            g_LoadingState.stage = "Initializing";
        }

        std::thread([filepath, device, &canvas]() {
            Logger::Get().Info("Starting background load of: " + filepath);
            bool ok = canvas.OpenDocument(device, filepath, [](float progress, const char* stage) {
                g_LoadingState.progress = progress;
                if (stage) {
                    std::lock_guard<std::mutex> lock(g_LoadingState.mutex);
                    g_LoadingState.stage = stage;
                }
            });
            g_LoadingState.success = ok;
            g_LoadingState.completed = true;
        }).detach();
    }

    // True if `maybeAncestor` is parent/ancestor of `layerIdx` (cycle guard).
    static bool IsGroupAncestorOf(const std::vector<Layer>& layers, int maybeAncestor, int layerIdx) {
        int p = layers[layerIdx].parentGroupId;
        int guard = 0;
        while (p >= 0 && p < (int)layers.size() && guard++ < 64) {
            if (p == maybeAncestor) return true;
            p = layers[p].parentGroupId;
        }
        return false;
    }

    static void DrawLayerDropHighlight(const ImVec2& rmin, const ImVec2& rmax, bool intoGroup) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (intoGroup) {
            dl->AddRectFilled(rmin, rmax, IM_COL32(60, 140, 255, 55));
            dl->AddRect(rmin, rmax, IM_COL32(80, 160, 255, 220), 0.0f, 0, 2.0f);
        } else {
            float y = rmax.y;
            dl->AddLine(ImVec2(rmin.x, y), ImVec2(rmax.x, y), IM_COL32(80, 160, 255, 255), 2.5f);
        }
    }

    // ICC preset combo (presets only — no free-text path). Returns true if changed.
    static bool DrawIccPresetCombo(Canvas& canvas, const char* label = "ICC Profile") {
        static const Canvas::IccPreset kPresets[] = {
            Canvas::IccPreset::None,
            Canvas::IccPreset::sRGB,
            Canvas::IccPreset::DisplayP3,
            Canvas::IccPreset::AdobeRGB
        };
        int cur = 1; // sRGB default
        Canvas::IccPreset p = canvas.GetExportIccPreset();
        for (int i = 0; i < IM_ARRAYSIZE(kPresets); ++i) {
            if (kPresets[i] == p) { cur = i; break; }
        }
        const char* names[] = {
            Canvas::IccPresetName(Canvas::IccPreset::None),
            Canvas::IccPresetName(Canvas::IccPreset::sRGB),
            Canvas::IccPresetName(Canvas::IccPreset::DisplayP3),
            Canvas::IccPresetName(Canvas::IccPreset::AdobeRGB)
        };
        if (ImGui::Combo(label, &cur, names, IM_ARRAYSIZE(names))) {
            canvas.SetExportIccPreset(kPresets[cur]);
            return true;
        }
        return false;
    }

    // ---------------------------------------------------------------------------
    // Themed monochrome SVG icons (ignore native fill colors; tint via ImGui text color)
    // ---------------------------------------------------------------------------
    struct SvgIconTex {
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;
    };
    static std::unordered_map<std::string, SvgIconTex> s_SvgIcons;
    static ID3D11Device* s_SvgIconDevice = nullptr;

    static void ReleaseSvgIcons() {
        for (auto& kv : s_SvgIcons) {
            if (kv.second.srv) kv.second.srv->Release();
        }
        s_SvgIcons.clear();
        s_SvgIconDevice = nullptr;
    }

    static float SvgParseNumber(const char*& p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) ++p;
        char* end = nullptr;
        float v = strtof(p, &end);
        p = end ? end : p;
        return v;
    }

    static void SvgSampleCubic(std::vector<ImVec2>& out, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, int segs = 12) {
        for (int i = 1; i <= segs; ++i) {
            float t = (float)i / (float)segs;
            float u = 1.f - t;
            float x = u*u*u*p0.x + 3*u*u*t*p1.x + 3*u*t*t*p2.x + t*t*t*p3.x;
            float y = u*u*u*p0.y + 3*u*u*t*p1.y + 3*u*t*t*p2.y + t*t*t*p3.y;
            out.push_back(ImVec2(x, y));
        }
    }

    static bool SvgParsePath(const std::string& d, std::vector<std::vector<ImVec2>>& contours) {
        const char* p = d.c_str();
        std::vector<ImVec2> cur;
        ImVec2 pos(0, 0), start(0, 0), lastC(0, 0);
        char cmd = 0;
        auto flush = [&]() {
            if (cur.size() >= 3) contours.push_back(cur);
            cur.clear();
        };
        while (*p) {
            while (*p && (isspace((unsigned char)*p) || *p == ',')) ++p;
            if (!*p) break;
            if (isalpha((unsigned char)*p)) { cmd = *p++; }
            if (!cmd) break;
            bool rel = islower((unsigned char)cmd);
            char c = (char)tolower((unsigned char)cmd);
            if (c == 'm') {
                float x = SvgParseNumber(p), y = SvgParseNumber(p);
                if (rel) { x += pos.x; y += pos.y; }
                flush();
                pos = start = ImVec2(x, y);
                cur.push_back(pos);
                cmd = rel ? 'l' : 'L'; // subsequent pairs are lineto
            } else if (c == 'l') {
                float x = SvgParseNumber(p), y = SvgParseNumber(p);
                if (rel) { x += pos.x; y += pos.y; }
                pos = ImVec2(x, y);
                cur.push_back(pos);
            } else if (c == 'h') {
                float x = SvgParseNumber(p);
                if (rel) x += pos.x;
                pos.x = x;
                cur.push_back(pos);
            } else if (c == 'v') {
                float y = SvgParseNumber(p);
                if (rel) y += pos.y;
                pos.y = y;
                cur.push_back(pos);
            } else if (c == 'c') {
                float x1 = SvgParseNumber(p), y1 = SvgParseNumber(p);
                float x2 = SvgParseNumber(p), y2 = SvgParseNumber(p);
                float x  = SvgParseNumber(p), y  = SvgParseNumber(p);
                if (rel) { x1 += pos.x; y1 += pos.y; x2 += pos.x; y2 += pos.y; x += pos.x; y += pos.y; }
                ImVec2 p1(x1, y1), p2(x2, y2), p3(x, y);
                SvgSampleCubic(cur, pos, p1, p2, p3);
                lastC = p2;
                pos = p3;
            } else if (c == 'z') {
                if (!cur.empty()) cur.push_back(start);
                pos = start;
                flush();
                cmd = 0;
            } else {
                // unsupported command — skip one number pair if possible
                SvgParseNumber(p); SvgParseNumber(p);
            }
        }
        flush();
        return !contours.empty();
    }

    static void SvgRasterizeEvenOdd(const std::vector<std::vector<ImVec2>>& contours,
                                    int outW, int outH, float vbX, float vbY, float vbW, float vbH,
                                    std::vector<uint8_t>& rgba) {
        rgba.assign((size_t)outW * outH * 4, 0);
        if (vbW <= 0.f || vbH <= 0.f) return;
        auto mapX = [&](float x) { return (x - vbX) / vbW * (outW - 1); };
        auto mapY = [&](float y) { return (y - vbY) / vbH * (outH - 1); };

        for (int y = 0; y < outH; ++y) {
            float py = (float)y + 0.5f;
            std::vector<float> xs;
            for (const auto& c : contours) {
                for (size_t i = 0, n = c.size(); i + 1 < n; ++i) {
                    float y0 = mapY(c[i].y), y1 = mapY(c[i + 1].y);
                    float x0 = mapX(c[i].x), x1 = mapX(c[i + 1].x);
                    if ((y0 <= py && y1 > py) || (y1 <= py && y0 > py)) {
                        float t = (py - y0) / (y1 - y0);
                        xs.push_back(x0 + t * (x1 - x0));
                    }
                }
            }
            std::sort(xs.begin(), xs.end());
            for (size_t i = 0; i + 1 < xs.size(); i += 2) {
                int xStart = std::clamp((int)std::floor(xs[i]), 0, outW - 1);
                int xEnd   = std::clamp((int)std::ceil(xs[i + 1]), 0, outW);
                for (int x = xStart; x < xEnd; ++x) {
                    size_t idx = ((size_t)y * outW + x) * 4;
                    rgba[idx + 0] = 255;
                    rgba[idx + 1] = 255;
                    rgba[idx + 2] = 255;
                    rgba[idx + 3] = 255;
                }
            }
        }
    }

    static bool LoadSvgIconFile(ID3D11Device* device, const std::string& path, SvgIconTex& out) {
        std::string text;
        {
#ifdef _WIN32
            std::ifstream in(UTF8ToWString(path), std::ios::binary);
#else
            std::ifstream in(path, std::ios::binary);
#endif
            if (!in) return false;
            std::ostringstream ss; ss << in.rdbuf();
            text = ss.str();
        }
        float vbX = 0, vbY = 0, vbW = 32, vbH = 32;
        size_t vbPos = text.find("viewBox");
        if (vbPos != std::string::npos) {
            size_t q = text.find_first_of("\"'", vbPos);
            if (q != std::string::npos) {
                char quote = text[q];
                size_t q2 = text.find(quote, q + 1);
                if (q2 != std::string::npos) {
                    std::string vb = text.substr(q + 1, q2 - q - 1);
                    std::replace(vb.begin(), vb.end(), ',', ' ');
                    std::istringstream iss(vb);
                    iss >> vbX >> vbY >> vbW >> vbH;
                }
            }
        }
        std::vector<std::vector<ImVec2>> contours;
        size_t pos = 0;
        while ((pos = text.find("<path", pos)) != std::string::npos) {
            size_t dpos = text.find(" d=", pos);
            size_t endTag = text.find('>', pos);
            if (dpos != std::string::npos && endTag != std::string::npos && dpos < endTag) {
                size_t q = text.find_first_of("\"'", dpos + 3);
                if (q != std::string::npos) {
                    char quote = text[q];
                    size_t q2 = text.find(quote, q + 1);
                    if (q2 != std::string::npos) {
                        SvgParsePath(text.substr(q + 1, q2 - q - 1), contours);
                    }
                }
            }
            pos = endTag != std::string::npos ? endTag + 1 : pos + 5;
        }
        if (contours.empty()) return false;

        const int iconSize = 64;
        std::vector<uint8_t> rgba;
        SvgRasterizeEvenOdd(contours, iconSize, iconSize, vbX, vbY, vbW, vbH, rgba);

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = iconSize; desc.Height = iconSize;
        desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA srd = {};
        srd.pSysMem = rgba.data();
        srd.SysMemPitch = iconSize * 4;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(device->CreateTexture2D(&desc, &srd, &tex))) return false;
        ID3D11ShaderResourceView* srv = nullptr;
        HRESULT hr = device->CreateShaderResourceView(tex, nullptr, &srv);
        tex->Release();
        if (FAILED(hr)) return false;
        out.srv = srv; out.w = iconSize; out.h = iconSize;
        return true;
    }

    static const char* SvgFileForAction(const char* actionName) {
        if (strcmp(actionName, "BrushTool") == 0) return "brush.svg";
        if (strcmp(actionName, "EraserTool") == 0) return "eraser.svg";
        if (strcmp(actionName, "BucketFillTool") == 0) return "fill_bucket.svg";
        return nullptr;
    }

    static SvgIconTex* GetSvgIcon(ID3D11Device* device, const char* actionName) {
        if (!device || !actionName) return nullptr;
        if (device != s_SvgIconDevice) {
            ReleaseSvgIcons();
            s_SvgIconDevice = device;
        }
        auto it = s_SvgIcons.find(actionName);
        if (it != s_SvgIcons.end()) return it->second.srv ? &it->second : nullptr;

        const char* file = SvgFileForAction(actionName);
        SvgIconTex tex;
        if (file) {
            // Search common locations relative to cwd / exe
            const char* roots[] = { "testfield/svg/", "svg/", "assets/icons/", "../testfield/svg/" };
            for (const char* root : roots) {
                std::string path = std::string(root) + file;
                if (LoadSvgIconFile(device, path, tex)) break;
            }
        }
        s_SvgIcons[actionName] = tex;
        return tex.srv ? &s_SvgIcons[actionName] : nullptr;
    }

    // Theme-aware icon color: dark UI → white, light UI → black (ignore SVG native colors)
    static ImU32 GetThemedIconColor(bool active) {
        ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        float lum = 0.2126f * bg.x + 0.7152f * bg.y + 0.0722f * bg.z;
        float v = (lum < 0.5f) ? 1.0f : 0.0f;
        float a = active ? 1.0f : 0.45f;
        return IM_COL32((int)(v * 255), (int)(v * 255), (int)(v * 255), (int)(a * 255));
    }

    static bool DrawThemedSvgIcon(ID3D11Device* device, const char* actionName, ImVec2 min, ImVec2 max, bool active) {
        SvgIconTex* icon = GetSvgIcon(device, actionName);
        if (!icon || !icon->srv) return false;
        // Preserve aspect: fit inside button with padding
        float bw = max.x - min.x, bh = max.y - min.y;
        float pad = std::min(bw, bh) * 0.12f;
        float side = std::min(bw, bh) - pad * 2.0f;
        if (side < 4.0f) return false;
        ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
        ImVec2 p0(c.x - side * 0.5f, c.y - side * 0.5f);
        ImVec2 p1(c.x + side * 0.5f, c.y + side * 0.5f);
        ImGui::GetWindowDrawList()->AddImage((ImTextureID)icon->srv, p0, p1, ImVec2(0,0), ImVec2(1,1), GetThemedIconColor(active));
        return true;
    }

    bool IsSelectTool(ActiveTool tool) {
        return tool == ActiveTool::RectSelect || tool == ActiveTool::EllipseSelect;
    }

    bool IsLassoTool(ActiveTool tool) {
        return tool == ActiveTool::LassoSelect || tool == ActiveTool::PolygonalLasso;
    }

    bool IsWandTool(ActiveTool tool) {
        return tool == ActiveTool::MagicWand || tool == ActiveTool::SmartSelect || tool == ActiveTool::QuickSelect;
    }

    ActiveTool CycleSelectTool(ActiveTool current) {
        if (current == ActiveTool::RectSelect) return ActiveTool::EllipseSelect;
        return ActiveTool::RectSelect;
    }

    ActiveTool CycleLassoTool(ActiveTool current) {
        if (current == ActiveTool::LassoSelect) return ActiveTool::PolygonalLasso;
        return ActiveTool::LassoSelect;
    }

    ActiveTool CycleWandTool(ActiveTool current) {
        if (current == ActiveTool::MagicWand) return ActiveTool::SmartSelect;
        if (current == ActiveTool::SmartSelect) return ActiveTool::QuickSelect;
        return ActiveTool::MagicWand;
    }

    void SampleCanvasColor(Canvas& canvas, float canvasX, float canvasY, float outColor[4]) {
        int cx = std::clamp((int)canvasX, 0, canvas.GetWidth() - 1);
        int cy = std::clamp((int)canvasY, 0, canvas.GetHeight() - 1);
        canvas.SampleCompositePixel(cx, cy, outColor);
    }

    struct ToolVariant {
        const char* actionName;
        const char* displayName;
        ActiveTool tool;
    };

    static void DrawToolIcon(const char* actionName, ImVec2 min, ImVec2 max, ImU32 color, bool active = true) {
        // Prefer themed SVG assets (ignore native SVG fill colors)
        if (s_SvgIconDevice && DrawThemedSvgIcon(s_SvgIconDevice, actionName, min, max, active)) {
            return;
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float w = max.x - min.x;
        float h = max.y - min.y;
        // Preserve square aspect for procedural glyphs
        float side = std::min(w, h);
        float ox = (w - side) * 0.5f;
        float oy = (h - side) * 0.5f;
        min = ImVec2(min.x + ox, min.y + oy);
        max = ImVec2(min.x + side, min.y + side);
        w = side; h = side;
        float cx = min.x + w * 0.5f;
        float cy = min.y + h * 0.5f;
        float pad = w * 0.25f;

        if (strcmp(actionName, "BrushTool") == 0) {
            ImVec2 tip = ImVec2(min.x + pad + 2.0f, max.y - pad - 2.0f);
            ImVec2 end = ImVec2(max.x - pad - 2.0f, min.y + pad + 2.0f);
            drawList->AddLine(end, tip, color, 4.0f);
            drawList->AddCircleFilled(tip, 5.0f, color);
        }
        else if (strcmp(actionName, "EraserTool") == 0) {
            ImVec2 p1 = ImVec2(min.x + pad, cy + pad * 0.5f);
            ImVec2 p2 = ImVec2(cx - pad * 0.5f, min.y + pad);
            ImVec2 p3 = ImVec2(max.x - pad, cy - pad * 0.5f);
            ImVec2 p4 = ImVec2(cx + pad * 0.5f, max.y - pad);
            ImVec2 pts[4] = { p1, p2, p3, p4 };
            drawList->AddConvexPolyFilled(pts, 4, color);
        }
        else if (strcmp(actionName, "BucketFillTool") == 0) {
            // Наклонное ведерко
            ImVec2 p1 = ImVec2(cx - w * 0.15f, cy - h * 0.1f);
            ImVec2 p2 = ImVec2(cx + w * 0.15f, cy - h * 0.25f);
            ImVec2 p3 = ImVec2(cx + w * 0.25f, cy + h * 0.1f);
            ImVec2 p4 = ImVec2(cx - w * 0.05f, cy + h * 0.25f);
            drawList->AddLine(p1, p2, color, 1.5f);
            drawList->AddLine(p2, p3, color, 1.5f);
            drawList->AddLine(p3, p4, color, 1.5f);
            drawList->AddLine(p4, p1, color, 1.5f);
            // Изливаемая капля
            drawList->AddTriangleFilled(ImVec2(cx - w * 0.1f, cy + h * 0.25f), ImVec2(cx - w * 0.2f, cy + h * 0.4f), ImVec2(cx, cy + h * 0.4f), color);
        }
        else if (strcmp(actionName, "GradientTool") == 0) {
            // Линия перехода градиента с узлами на концах
            ImVec2 pStart = ImVec2(min.x + pad, max.y - pad);
            ImVec2 pEnd = ImVec2(max.x - pad, min.y + pad);
            drawList->AddLine(pStart, pEnd, color, 2.0f);
            drawList->AddCircle(pStart, 3.5f, color, 12, 1.5f);
            drawList->AddCircleFilled(pEnd, 4.0f, color);
        }
        else if (strcmp(actionName, "PipetteTool") == 0) {
            // Пипетка под наклоном
            ImVec2 tip = ImVec2(min.x + pad + 1.0f, max.y - pad - 1.0f);
            ImVec2 end = ImVec2(max.x - pad - 1.0f, min.y + pad + 1.0f);
            drawList->AddLine(tip, end, color, 3.0f);
            drawList->AddCircleFilled(end, 4.0f, color);
            drawList->AddLine(tip, ImVec2(tip.x - 2.0f, tip.y + 2.0f), color, 1.5f);
        }
        else if (strcmp(actionName, "RectSelectTool") == 0) {
            // Прямоугольная рамка выделения (штрихи по углам)
            float rLeft = min.x + pad;
            float rRight = max.x - pad;
            float rTop = min.y + pad;
            float rBottom = max.y - pad;
            drawList->AddLine(ImVec2(rLeft, rTop), ImVec2(rLeft + 4.0f, rTop), color, 1.0f);
            drawList->AddLine(ImVec2(rLeft, rTop), ImVec2(rLeft, rTop + 4.0f), color, 1.0f);
            drawList->AddLine(ImVec2(rRight, rTop), ImVec2(rRight - 4.0f, rTop), color, 1.0f);
            drawList->AddLine(ImVec2(rRight, rTop), ImVec2(rRight, rTop + 4.0f), color, 1.0f);
            drawList->AddLine(ImVec2(rLeft, rBottom), ImVec2(rLeft + 4.0f, rBottom), color, 1.0f);
            drawList->AddLine(ImVec2(rLeft, rBottom), ImVec2(rLeft, rBottom - 4.0f), color, 1.0f);
            drawList->AddLine(ImVec2(rRight, rBottom), ImVec2(rRight - 4.0f, rBottom), color, 1.0f);
            drawList->AddLine(ImVec2(rRight, rBottom), ImVec2(rRight, rBottom - 4.0f), color, 1.0f);
        }
        else if (strcmp(actionName, "EllipseSelectTool") == 0) {
            // Штриховой овал/круг
            float radius = (w - pad * 2.0f) * 0.5f;
            const int segments = 16;
            for (int i = 0; i < segments; i += 2) {
                float a1 = (i) * 2.0f * 3.14159f / segments;
                float a2 = (i + 1) * 2.0f * 3.14159f / segments;
                drawList->PathArcTo(ImVec2(cx, cy), radius, a1, a2, 4);
                drawList->PathStroke(color, false, 1.0f);
            }
        }
        else if (strcmp(actionName, "LassoSelectTool") == 0) {
            // Схематичное лассо произвольной формы штрихами
            ImVec2 pts[] = {
                ImVec2(cx - 5.0f, cy - 5.0f),
                ImVec2(cx + 5.0f, cy - 7.0f),
                ImVec2(cx + 7.0f, cy),
                ImVec2(cx + 3.0f, cy + 6.0f),
                ImVec2(cx - 6.0f, cy + 4.0f),
                ImVec2(cx - 7.0f, cy - 1.0f)
            };
            for (int i = 0; i < 6; ++i) {
                if (i % 2 == 0) {
                    drawList->AddLine(pts[i], pts[(i + 1) % 6], color, 1.0f);
                }
            }
        }
        else if (strcmp(actionName, "MagicWandTool") == 0) {
            // Волшебная палочка (палочка и звезды-точки на конце)
            ImVec2 wStart(min.x + pad, max.y - pad);
            ImVec2 wEnd(cx + 2.0f, cy - 2.0f);
            drawList->AddLine(wStart, wEnd, color, 2.0f);
            drawList->AddLine(ImVec2(cx - 1.0f, cy - 5.0f), ImVec2(cx + 5.0f, cy - 5.0f), color, 1.0f);
            drawList->AddLine(ImVec2(cx + 2.0f, cy - 8.0f), ImVec2(cx + 2.0f, cy - 2.0f), color, 1.0f);
        }
        else if (strcmp(actionName, "SmartSelectTool") == 0) {
            // Умное выделение (уголки ограничительной рамки и круглый маркер по центру)
            float rL = min.x + pad;
            float rR = max.x - pad;
            float rT = min.y + pad;
            float rB = max.y - pad;
            drawList->AddLine(ImVec2(rL, rT), ImVec2(rL + 4.0f, rT), color, 1.0f);
            drawList->AddLine(ImVec2(rL, rT), ImVec2(rL, rT + 4.0f), color, 1.0f);
            drawList->AddLine(ImVec2(rR, rB), ImVec2(rR - 4.0f, rB), color, 1.0f);
            drawList->AddLine(ImVec2(rR, rB), ImVec2(rR, rB - 4.0f), color, 1.0f);
            drawList->AddCircleFilled(ImVec2(cx, cy), 2.5f, color);
            drawList->AddLine(ImVec2(cx, cy - 4.0f), ImVec2(cx, cy + 4.0f), color, 1.0f);
            drawList->AddLine(ImVec2(cx - 4.0f, cy), ImVec2(cx + 4.0f, cy), color, 1.0f);
        }
        else if (strcmp(actionName, "PanTool") == 0) {
            float r = w * 0.25f;
            drawList->AddLine(ImVec2(cx - r, cy), ImVec2(cx + r, cy), color, 2.0f);
            drawList->AddLine(ImVec2(cx, cy - r), ImVec2(cx, cy + r), color, 2.0f);
            drawList->AddTriangleFilled(ImVec2(cx - r, cy - 3.0f), ImVec2(cx - r, cy + 3.0f), ImVec2(cx - r - 4.0f, cy), color);
            drawList->AddTriangleFilled(ImVec2(cx + r, cy - 3.0f), ImVec2(cx + r, cy + 3.0f), ImVec2(cx + r + 4.0f, cy), color);
            drawList->AddTriangleFilled(ImVec2(cx - 3.0f, cy - r), ImVec2(cx + 3.0f, cy - r), ImVec2(cx, cy - r - 4.0f), color);
            drawList->AddTriangleFilled(ImVec2(cx - 3.0f, cy + r), ImVec2(cx + 3.0f, cy + r), ImVec2(cx, cy + r + 4.0f), color);
        }
        else if (strcmp(actionName, "RotateTool") == 0) {
            float r = w * 0.22f;
            drawList->AddCircle(ImVec2(cx, cy), r, color, 16, 2.0f);
            drawList->AddTriangleFilled(ImVec2(cx + r - 3.0f, cy - 3.0f), ImVec2(cx + r + 3.0f, cy - 3.0f), ImVec2(cx + r, cy + 2.0f), color);
        }
        else if (strcmp(actionName, "PolygonalLassoTool") == 0) {
            ImVec2 pts[] = {
                ImVec2(cx - 6.0f, cy - 6.0f),
                ImVec2(cx + 6.0f, cy - 6.0f),
                ImVec2(cx + 6.0f, cy + 3.0f),
                ImVec2(cx - 1.0f, cy + 7.0f),
                ImVec2(cx - 7.0f, cy + 2.0f)
            };
            for (int i = 0; i < 5; ++i) {
                drawList->AddLine(pts[i], pts[(i + 1) % 5], color, 1.5f);
                drawList->AddCircleFilled(pts[i], 1.5f, color);
            }
        }
        else if (strcmp(actionName, "QuickSelectTool") == 0) {
            drawList->AddCircle(ImVec2(cx - 3.0f, cy + 3.0f), 4.0f, color, 12, 1.5f);
            drawList->AddLine(ImVec2(cx + 1.0f, cy - 1.0f), ImVec2(cx + 7.0f, cy - 7.0f), color, 2.0f);
            drawList->AddCircleFilled(ImVec2(cx - 8.0f, cy - 8.0f), 1.0f, color);
            drawList->AddCircleFilled(ImVec2(cx + 8.0f, cy + 8.0f), 1.0f, color);
            drawList->AddCircleFilled(ImVec2(cx + 8.0f, cy - 8.0f), 1.0f, color);
            drawList->AddCircleFilled(ImVec2(cx - 8.0f, cy + 8.0f), 1.0f, color);
        }
        else if (strcmp(actionName, "TransformTool") == 0) {
            float rL = min.x + pad;
            float rR = max.x - pad;
            float rT = min.y + pad;
            float rB = max.y - pad;
            drawList->AddRect(ImVec2(rL, rT), ImVec2(rR, rB), color, 1.0f);
            float hs = 2.0f;
            auto drawBoxHandle = [&](ImVec2 p) {
                drawList->AddRectFilled(ImVec2(p.x - hs, p.y - hs), ImVec2(p.x + hs, p.y + hs), color);
            };
            drawBoxHandle(ImVec2(rL, rT));
            drawBoxHandle(ImVec2(rR, rT));
            drawBoxHandle(ImVec2(rR, rB));
            drawBoxHandle(ImVec2(rL, rB));
        }
        else {
            drawList->AddRect(min, max, color, 1.0f);
        }
    }

    static void DrawKeybindBadge(ImVec2 btnMax, const std::string& keybindString, float btnSize) {
        if (keybindString.empty()) return;

        std::string badgeText;
        size_t plusPos = keybindString.find_last_of('+');
        if (plusPos != std::string::npos) {
            badgeText = keybindString.substr(plusPos + 1);
        } else {
            badgeText = keybindString;
        }
        if (badgeText.empty()) return;

        std::string singleChar = badgeText.substr(0, 1);
        float badgeSize = std::clamp(btnSize * 0.32f, 10.0f, 18.0f);
        ImVec2 badgeMin = ImVec2(btnMax.x - badgeSize, btnMax.y - badgeSize);
        ImVec2 badgeMax = btnMax;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(badgeMin, badgeMax, ImGui::GetColorU32(ImGuiCol_FrameBgActive), 2.0f);
        drawList->AddRect(badgeMin, badgeMax, ImGui::GetColorU32(ImGuiCol_Border), 2.0f);

        ImVec2 textSize = ImGui::CalcTextSize(singleChar.c_str());
        float fontScale = std::clamp(btnSize / 44.0f, 0.55f, 1.0f);
        ImVec2 textPos = ImVec2(
            badgeMin.x + (badgeSize - textSize.x * fontScale) * 0.5f,
            badgeMin.y + (badgeSize - textSize.y * fontScale) * 0.5f - 1.0f);
        drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * fontScale, textPos,
            ImGui::GetColorU32(ImGuiCol_Text), singleChar.c_str());
    }

    static float ComputeAdaptiveToolButtonSize(ImVec2 avail, bool isVertical, int buttonCount, bool hasSeparator) {
        constexpr float kMin = 16.0f;
        constexpr float kMax = 64.0f;
        if (buttonCount <= 0) return 44.0f;

        ImGuiStyle& style = ImGui::GetStyle();
        float gap = isVertical ? style.ItemSpacing.y : style.ItemSpacing.x;
        float separatorExtra = 0.0f;
        if (hasSeparator) {
            separatorExtra = gap * 2.0f + 1.0f;
        }

        float usableMain = isVertical ? avail.y : avail.x;
        usableMain -= separatorExtra + gap * (float)(buttonCount - 1);
        float sizeFromMain = usableMain / (float)buttonCount;
        float sizeFromCross = isVertical ? avail.x : avail.y;

        return std::clamp(std::min(sizeFromMain, sizeFromCross), kMin, kMax);
    }

    static void ToolbarAdvance(bool isVertical, float gap) {
        if (isVertical) {
            ImGui::Dummy(ImVec2(0.0f, gap));
        } else {
            ImGui::SameLine(0.0f, gap);
        }
    }

    static void ToolbarBeginLayout(ImVec2 avail, bool isVertical, int buttonCount, float btnSize, float gap, bool hasSeparator) {
        // Anchor to top-left, no dynamic alignment padding
    }

    static void RenderGroupedToolButton(
        const char* groupId,
        const char* rebindActionName,
        const ToolVariant* variants,
        int variantCount,
        const char* groupTooltip,
        const std::string& keybindString,
        float size,
        std::string& rebindAction,
        ActiveTool& activeTool)
    {
        static std::unordered_map<std::string, int> s_LastVariantIndex;
        static std::unordered_map<ImGuiID, double> s_PressStart;
        static std::unordered_map<ImGuiID, bool> s_LongPressOpened;

        int activeIdx = -1;
        for (int i = 0; i < variantCount; ++i) {
            if (activeTool == variants[i].tool) {
                activeIdx = i;
                break;
            }
        }

        int displayIdx = (activeIdx >= 0) ? activeIdx : s_LastVariantIndex[groupId];
        if (displayIdx < 0 || displayIdx >= variantCount) displayIdx = 0;

        const ToolVariant& display = variants[displayIdx];
        bool isActive = (activeIdx >= 0);

        ImGui::PushID(groupId);
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Button]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
        }

        ImGui::BeginGroup();
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        float winWidth = win->Size.x;
        float winHeight = win->Size.y;
        bool isVertical = (winHeight > winWidth);
        if (isVertical) {
            float posX = (winWidth - size) * 0.5f;
            if (posX > 0.0f) ImGui::SetCursorPosX(posX);
        } else {
            float posY = (winHeight - size) * 0.5f;
            if (posY > 0.0f) ImGui::SetCursorPosY(posY);
        }
        ImGui::Button("##groupBtn", ImVec2(size, size));
        ImGuiID itemId = ImGui::GetItemID();
        double now = ImGui::GetTime();

        if (ImGui::IsItemActive()) {
            if (s_PressStart.find(itemId) == s_PressStart.end()) {
                s_PressStart[itemId] = now;
                s_LongPressOpened[itemId] = false;
            }
            if (!s_LongPressOpened[itemId] && (now - s_PressStart[itemId]) > 0.15) {
                ImGui::OpenPopup("##variantPopup");
                s_LongPressOpened[itemId] = true;
            }
        }

        if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (!s_LongPressOpened[itemId]) {
                activeTool = display.tool;
                s_LastVariantIndex[groupId] = displayIdx;
            }
            s_PressStart.erase(itemId);
            s_LongPressOpened.erase(itemId);
        }

        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            rebindAction = rebindActionName;
            ImGui::OpenPopup("RebindToolPopup");
        }

        if (ImGui::BeginPopup("##variantPopup")) {
            for (int i = 0; i < variantCount; ++i) {
                if (ImGui::Selectable(variants[i].displayName, displayIdx == i)) {
                    activeTool = variants[i].tool;
                    s_LastVariantIndex[groupId] = i;
                }
            }
            ImGui::EndPopup();
        }

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s (%s)\nClick: activate  |  Hold: pick variant\nRight-click: rebind",
                groupTooltip, keybindString.c_str());
        }

        ImVec2 btnMin = ImGui::GetItemRectMin();
        ImVec2 btnMax = ImGui::GetItemRectMax();
        DrawToolIcon(display.actionName, btnMin, btnMax,
            GetThemedIconColor(isActive), isActive);
        DrawKeybindBadge(btnMax, keybindString, size);

        ImGui::EndGroup();
        ImGui::PopStyleColor(2);
        ImGui::PopID();
    }

    static void RenderToolButton(const char* actionName, const char* displayName, ActiveTool targetTool, bool isEraseTool, std::string keybindString, float size, std::string& rebindAction, ActiveTool& activeTool, BrushSettings& brush, Canvas& canvas) {
        bool isActive = (activeTool == targetTool && (targetTool != ActiveTool::Brush || isEraseTool == brush.erase));
        if (strcmp(actionName, "Reset") == 0) isActive = false;

        ImGui::PushID(actionName);
        
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Button]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
        }
        
        ImGui::BeginGroup();
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        float winWidth = win->Size.x;
        float winHeight = win->Size.y;
        bool isVertical = (winHeight > winWidth);
        if (isVertical) {
            float posX = (winWidth - size) * 0.5f;
            if (posX > 0.0f) ImGui::SetCursorPosX(posX);
        } else {
            float posY = (winHeight - size) * 0.5f;
            if (posY > 0.0f) ImGui::SetCursorPosY(posY);
        }
        ImGui::Button("##toolBtn", ImVec2(size, size));
        
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (strcmp(actionName, "Reset") == 0) {
                canvas.ResetView();
            } else {
                activeTool = targetTool;
                if (targetTool == ActiveTool::Brush) {
                    brush.erase = isEraseTool;
                }
            }
        }
        if (strcmp(actionName, "Reset") != 0 && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            rebindAction = actionName;
            ImGui::OpenPopup("RebindToolPopup");
        }
        if (ImGui::IsItemHovered()) {
            if (strcmp(actionName, "Reset") == 0) {
                ImGui::SetTooltip("Reset View");
            } else {
                ImGui::SetTooltip("%s (%s)\nRight-click to rebind", displayName, keybindString.c_str());
            }
        }
        
        ImVec2 btnMin = ImGui::GetItemRectMin();
        ImVec2 btnMax = ImGui::GetItemRectMax();
        DrawToolIcon(actionName, btnMin, btnMax, GetThemedIconColor(isActive), isActive);

        if (strcmp(actionName, "Reset") != 0) {
            DrawKeybindBadge(btnMax, keybindString, size);
        }

        ImGui::EndGroup();
        ImGui::PopStyleColor(2);
        ImGui::PopID();
    }

    void RenderAll(UIState& state, Canvas& canvas, BrushSettings& brush, ActiveTool& activeTool, ID3D11Device* device, ID3D11DeviceContext* context, GLFWwindow* window) {
        if (device && device != s_SvgIconDevice) {
            ReleaseSvgIcons();
            s_SvgIconDevice = device;
        } else if (device) {
            s_SvgIconDevice = device;
        }
        ImGuiViewport* mainViewport = ImGui::GetMainViewport();

        // 1. Persistent Header (Main Menu Bar)
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open Project (.rayp)", KeymapManager::Get().GetActionShortcutString("OpenProject").c_str())) {
                    state.openLoadRaypModal = true;
                }
                if (ImGui::MenuItem("Save Project (.rayp)", KeymapManager::Get().GetActionShortcutString("SaveProject").c_str())) {
                    state.openSaveRaypModal = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Import Image...", "Ctrl+I")) {
                    state.openImportModal = true;
                }
                if (ImGui::MenuItem("Quick Export", KeymapManager::Get().GetActionShortcutString("QuickExport").c_str())) {
                    state.openQuickExportTrigger = true;
                }
                if (ImGui::MenuItem("Advanced Export...", KeymapManager::Get().GetActionShortcutString("AdvancedExport").c_str())) {
                    state.openExportAdvancedModal = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Load Config...")) {
                    state.openLoadConfigModal = true;
                }
                if (ImGui::MenuItem("Save Config...")) {
                    state.openSaveConfigModal = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Settings / Preferences...")) {
                    state.openSettingsModal = true;
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
                if (canvas.CanUndo()) {
                    undoLabel += " (" + canvas.GetUndoName() + ")";
                }
                if (ImGui::MenuItem(undoLabel.c_str(), KeymapManager::Get().GetActionShortcutString("Undo").c_str(), false, canvas.CanUndo())) {
                    canvas.Undo();
                }

                std::string redoLabel = "Redo";
                if (canvas.CanRedo()) {
                    redoLabel += " (" + canvas.GetRedoName() + ")";
                }
                if (ImGui::MenuItem(redoLabel.c_str(), KeymapManager::Get().GetActionShortcutString("Redo").c_str(), false, canvas.CanRedo())) {
                    canvas.Redo();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Canvas")) {
                if (ImGui::MenuItem("Canvas Edit...")) {
                    state.openCanvasSizeModal = true;
                }
                if (ImGui::MenuItem("Crop to Selection", KeymapManager::Get().GetActionShortcutString("CropToSelection").c_str(), false, canvas.HasSelection())) {
                    canvas.CropCanvasToSelection(device);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Rotate Canvas 90 CW")) {
                    canvas.RotateCanvas90(device, true);
                }
                if (ImGui::MenuItem("Rotate Canvas 90 CCW")) {
                    canvas.RotateCanvas90(device, false);
                }
                if (ImGui::MenuItem("Flip Canvas Horizontally")) {
                    canvas.FlipCanvasHorizontal(device);
                }
                if (ImGui::MenuItem("Flip Canvas Vertically")) {
                    canvas.FlipCanvasVertical(device);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Flip Active Layer Horizontally", nullptr, false, canvas.GetActiveLayerIndex() != -1)) {
                    canvas.FlipActiveLayerHorizontal(device);
                }
                if (ImGui::MenuItem("Flip Active Layer Vertically", nullptr, false, canvas.GetActiveLayerIndex() != -1)) {
                    canvas.FlipActiveLayerVertical(device);
                }
                ImGui::EndMenu();
            }
            // ---- Image Menu ----
            if (ImGui::BeginMenu("Image")) {
                bool hasLayer = canvas.GetActiveLayerIndex() != -1;
                if (ImGui::MenuItem("Invert Colors", KeymapManager::Get().GetActionShortcutString("InvertColors").c_str(), false, hasLayer))
                    canvas.InvertColors();
                if (ImGui::MenuItem("Invert Alpha", KeymapManager::Get().GetActionShortcutString("InvertAlpha").c_str(), false, hasLayer))
                    canvas.InvertAlpha();
                ImGui::Separator();
                if (ImGui::MenuItem("Blur...", nullptr, false, hasLayer))
                    state.showBlurModal = true;
                if (ImGui::MenuItem("HSV Adjust...", "Ctrl+U", false, hasLayer))
                    state.showHSVModal = true;
                if (ImGui::MenuItem("Curves...", nullptr, false, hasLayer)) {
                    state.showCurvesModal = true;
                }
                if (ImGui::MenuItem("Add Noise...", nullptr, false, hasLayer))
                    state.showNoiseModal = true;
                ImGui::EndMenu();
            }
            // ---- Select Menu ----
            if (ImGui::BeginMenu("Select")) {
                if (ImGui::MenuItem("Select All", "Ctrl+A")) canvas.SelectAll();
                if (ImGui::MenuItem("Deselect",   "Ctrl+D")) canvas.ClearSelection();
                if (ImGui::MenuItem("Invert Selection", "Ctrl+Shift+I")) canvas.InvertSelection();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Toolbar", nullptr, &state.showToolbar);
                ImGui::MenuItem("Properties", nullptr, &state.showProperties);
                ImGui::MenuItem("Layers", nullptr, &state.showLayers);
                ImGui::MenuItem("Channels", nullptr, &state.showChannels);
                ImGui::MenuItem("Colors Window", nullptr, &state.showColors);
                ImGui::MenuItem("Tool Settings", nullptr, &state.showToolSettings);
                ImGui::MenuItem("Console logs", nullptr, &state.showConsole);
                ImGui::Separator();
                if (ImGui::MenuItem("Reset View")) {
                    canvas.ResetView();
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

        // ---- Image Adjustment Modals ----

        // Blur Modal
        if (state.showBlurModal) ImGui::OpenPopup("Blur##modal");
        if (ImGui::BeginPopupModal("Blur##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Gaussian Blur (3-pass box)");
            ImGui::SliderFloat("Radius", &state.blurRadius, 0.5f, 80.0f, "%.1f px");
            ImGui::Spacing();
            if (ImGui::Button("Apply", ImVec2(100,0))) {
                canvas.ApplyBlur(state.blurRadius);
                state.showBlurModal = false; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100,0))) { state.showBlurModal = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        // HSV Modal
        if (state.showHSVModal) ImGui::OpenPopup("HSV Adjust##modal");
        if (ImGui::BeginPopupModal("HSV Adjust##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Hue / Saturation / Value");
            ImGui::SliderFloat("Hue",        &state.hsvH, -0.5f, 0.5f, "%.3f");
            ImGui::SliderFloat("Saturation", &state.hsvS, -1.0f, 1.0f, "%.3f");
            ImGui::SliderFloat("Value",      &state.hsvV, -1.0f, 1.0f, "%.3f");
            ImGui::Spacing();
            if (ImGui::Button("Apply", ImVec2(100,0))) {
                canvas.ApplyHSV(state.hsvH, state.hsvS, state.hsvV);
                state.hsvH=state.hsvS=state.hsvV=0.f;
                state.showHSVModal=false; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel",ImVec2(100,0))){ state.hsvH=state.hsvS=state.hsvV=0.f; state.showHSVModal=false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        // Noise Modal
        if (state.showNoiseModal) ImGui::OpenPopup("Add Noise##modal");
        if (ImGui::BeginPopupModal("Add Noise##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SliderFloat("Strength", &state.noiseStrength, 0.0f, 1.0f, "%.3f");
            ImGui::Checkbox("Color Noise", &state.noiseColor);
            ImGui::Spacing();
            if (ImGui::Button("Apply",ImVec2(100,0))){ canvas.ApplyNoise(state.noiseStrength, state.noiseColor); state.showNoiseModal=false; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel",ImVec2(100,0))){ state.showNoiseModal=false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        // Curves Modal — interactive spline editor (mouse on graph moves points, not window)
        if (state.showCurvesModal) ImGui::OpenPopup("Curves##modal");
        if (ImGui::BeginPopupModal("Curves##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (state.curvesPointsRGB.empty()) {
                state.curvesPointsRGB = {{0.f, 0.f}, {1.f, 1.f}};
                state.curvesLUTRGB = Canvas_BuildSplineLUT(state.curvesPointsRGB);
            }
            if (state.curvesPointsAlpha.empty()) {
                state.curvesPointsAlpha = {{0.f, 0.f}, {1.f, 1.f}};
                state.curvesLUTAlpha = Canvas_BuildSplineLUT(state.curvesPointsAlpha);
            }

            static const char* chanNames[] = {"RGB","Alpha"};
            ImGui::Combo("Channel", &state.curvesChannel, chanNames, 2);
            ImGui::SameLine(); ImGui::TextDisabled("(right-click = remove point)");

            std::vector<std::pair<float,float>>& activePoints = (state.curvesChannel == 0) ? state.curvesPointsRGB : state.curvesPointsAlpha;
            std::vector<float>& activeLUT = (state.curvesChannel == 0) ? state.curvesLUTRGB : state.curvesLUTAlpha;

            const float graphSz = 256.f;
            const float pad = 8.f;

            // Child captures mouse so parent modal is not dragged from the graph area
            ImGui::BeginChild("##curves_graph_child", ImVec2(graphSz + pad * 2.f, graphSz + pad * 2.f),
                ImGuiChildFlags_Borders, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

            ImVec2 graphPos = ImGui::GetCursorScreenPos();
            graphPos.x += pad; graphPos.y += pad;
            ImGui::SetCursorScreenPos(graphPos);
            ImGui::InvisibleButton("##graph_bounds", ImVec2(graphSz, graphSz));
            const bool graphActive = ImGui::IsItemActive();
            const bool graphHovered = ImGui::IsItemHovered();

            ImDrawList* dl = ImGui::GetWindowDrawList();

            dl->AddRectFilled(graphPos, ImVec2(graphPos.x+graphSz,graphPos.y+graphSz), IM_COL32(30,30,30,255));
            for (int gi=1;gi<4;++gi) {
                float gx=graphPos.x+graphSz*gi/4.f, gy=graphPos.y+graphSz*gi/4.f;
                dl->AddLine(ImVec2(gx,graphPos.y),ImVec2(gx,graphPos.y+graphSz),IM_COL32(60,60,60,255));
                dl->AddLine(ImVec2(graphPos.x,gy),ImVec2(graphPos.x+graphSz,gy),IM_COL32(60,60,60,255));
            }
            dl->AddLine(graphPos,ImVec2(graphPos.x+graphSz,graphPos.y+graphSz),IM_COL32(80,80,80,255));
            dl->AddRect(graphPos,ImVec2(graphPos.x+graphSz,graphPos.y+graphSz),IM_COL32(120,120,120,255));

            if (!activePoints.empty())
                activeLUT = Canvas_BuildSplineLUT(activePoints);
            for (int xi=0;xi<255;++xi) {
                float x0=graphPos.x+xi, y0=graphPos.y+graphSz*(1.f-activeLUT[xi]);
                float x1=graphPos.x+xi+1, y1=graphPos.y+graphSz*(1.f-activeLUT[xi+1]);
                dl->AddLine(ImVec2(x0,y0),ImVec2(x1,y1),IM_COL32(220,220,220,255),1.5f);
            }

            static int draggingPt = -1;
            ImVec2 mpos = ImGui::GetIO().MousePos;
            bool inGraph = graphHovered || graphActive ||
                (mpos.x>=graphPos.x && mpos.x<=graphPos.x+graphSz && mpos.y>=graphPos.y && mpos.y<=graphPos.y+graphSz);

            for (int pi=0;pi<(int)activePoints.size();++pi) {
                float cx=graphPos.x+activePoints[pi].first*graphSz;
                float cy=graphPos.y+(1.f-activePoints[pi].second)*graphSz;
                bool hovered = fabsf(mpos.x-cx)<7.f && fabsf(mpos.y-cy)<7.f;
                dl->AddCircleFilled(ImVec2(cx,cy),5.f,hovered?IM_COL32(255,200,100,255):IM_COL32(200,200,200,255));
                dl->AddCircle(ImVec2(cx,cy),5.f,IM_COL32(60,60,60,255));

                if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) draggingPt=pi;
                if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && pi!=0 && pi!=(int)activePoints.size()-1) {
                    activePoints.erase(activePoints.begin()+pi);
                    activeLUT = Canvas_BuildSplineLUT(activePoints);
                    break;
                }
            }
            if (draggingPt>=0) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    float nx=std::clamp((mpos.x-graphPos.x)/graphSz,0.f,1.f);
                    float ny=std::clamp(1.f-(mpos.y-graphPos.y)/graphSz,0.f,1.f);
                    if (draggingPt==0) nx=0.f;
                    if (draggingPt==(int)activePoints.size()-1) nx=1.f;
                    activePoints[draggingPt]={nx,ny};
                    std::sort(activePoints.begin(),activePoints.end(),[](auto&a,auto&b){return a.first<b.first;});
                    // re-find dragged index after sort (endpoints fixed at 0/1)
                    if (draggingPt > 0 && draggingPt < (int)activePoints.size()-1) {
                        // keep draggingPt as sorted index of moved point — approximate via nearest
                    }
                } else draggingPt=-1;
            }
            // Left-click empty graph = add point (InvisibleButton is active, so don't require !IsAnyItemActive)
            if (inGraph && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && draggingPt < 0) {
                bool onPoint = false;
                for (auto& pt : activePoints) {
                    float cx=graphPos.x+pt.first*graphSz;
                    float cy=graphPos.y+(1.f-pt.second)*graphSz;
                    if (fabsf(mpos.x-cx)<7.f && fabsf(mpos.y-cy)<7.f) { onPoint = true; break; }
                }
                if (!onPoint) {
                    float nx=std::clamp((mpos.x-graphPos.x)/graphSz,0.f,1.f);
                    float ny=std::clamp(1.f-(mpos.y-graphPos.y)/graphSz,0.f,1.f);
                    activePoints.push_back({nx,ny});
                    std::sort(activePoints.begin(),activePoints.end(),[](auto&a,auto&b){return a.first<b.first;});
                }
            }

            ImGui::EndChild();

            float posX=(mpos.x-graphPos.x)/graphSz*255.f, posY=(1.f-(mpos.y-graphPos.y)/graphSz)*255.f;
            if (inGraph) ImGui::Text("(%.0f, %.0f)", posX, posY);
            else ImGui::TextDisabled("Move mouse over graph");
            ImGui::Spacing();
            if (ImGui::Button("Reset",ImVec2(80,0))){
                activePoints={{0.f,0.f},{1.f,1.f}};
                activeLUT = Canvas_BuildSplineLUT(activePoints);
            }
            ImGui::SameLine();
            if (ImGui::Button("Apply",ImVec2(80,0))){
                canvas.ApplyCurves(state.curvesLUTRGB, state.curvesLUTAlpha);
                state.showCurvesModal=false; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel",ImVec2(80,0))){ state.showCurvesModal=false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        // 2. Persistent Footer (Status Bar)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
        ImGui::BeginViewportSideBar("##StatusBar", mainViewport, ImGuiDir_Down, 28.0f, 
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
        
        const char* toolLabel = "Hand";
        switch (activeTool) {
            case ActiveTool::Brush: toolLabel = "Brush"; break;
            case ActiveTool::Eraser: toolLabel = "Eraser"; break;
            case ActiveTool::Pan: toolLabel = "Hand"; break;
            case ActiveTool::RectSelect: toolLabel = "Rect Select"; break;
            case ActiveTool::EllipseSelect: toolLabel = "Ellipse Select"; break;
            case ActiveTool::LassoSelect: toolLabel = "Lasso Select"; break;
            case ActiveTool::PolygonalLasso: toolLabel = "Polygonal Lasso"; break;
            case ActiveTool::QuickSelect: toolLabel = "Quick Select"; break;
            case ActiveTool::MagicWand: toolLabel = "Magic Wand"; break;
            case ActiveTool::SmartSelect: toolLabel = "Smart Select"; break;
            case ActiveTool::MovePixels: toolLabel = "Transform"; break;
            case ActiveTool::Pipette: toolLabel = "Pipette"; break;
            case ActiveTool::BucketFill: toolLabel = "Bucket Fill"; break;
            case ActiveTool::Gradient: toolLabel = "Gradient"; break;
            case ActiveTool::Smudge: toolLabel = "Smudge"; break;
        }
        ImGui::Text("Startup: %.1f ms | Frame: %.2f ms | FPS: %.1f | Canvas: %d x %d | Zoom: %.0f%% | Threads: %d | Tool: %s",
            state.startupTimeMs, state.frameTimeMs, state.fps, canvas.GetWidth(), canvas.GetHeight(), canvas.GetZoom() * 100.0f,
            ThreadPool::Get().GetThreadCount(), toolLabel);
        
        ImGui::End();
        ImGui::PopStyleVar();

        // 3. DockSpace Default Layout Setup
        ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(0, mainViewport);
        static bool firstTimeDock = !std::filesystem::exists(ConfigManager::GetUserSubdirectory("user") + "/imgui.ini");
        if (firstTimeDock) {
            firstTimeDock = false;
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, mainViewport->Size);

            ImGuiID dock_main_id = dockspace_id;
            ImGuiID dock_left_id = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Left, 0.08f, NULL, &dock_main_id);
            ImGuiID dock_right_id = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Right, 0.25f, NULL, &dock_main_id);
            
            ImGuiID dock_right_top_id = ImGui::DockBuilderSplitNode(dock_right_id, ImGuiDir_Up, 0.35f, NULL, &dock_right_id);
            ImGuiID dock_right_middle_id = ImGui::DockBuilderSplitNode(dock_right_id, ImGuiDir_Up, 0.50f, NULL, &dock_right_id);
            ImGuiID dock_right_bottom_id = dock_right_id;

            ImGui::DockBuilderDockWindow("Toolbar", dock_left_id);
            ImGui::DockBuilderDockWindow("Canvas Viewport", dock_main_id);
            ImGui::DockBuilderDockWindow("Properties", dock_right_top_id);
            ImGui::DockBuilderDockWindow("Layers", dock_right_middle_id);
            ImGui::DockBuilderDockWindow("Channels", dock_right_middle_id);
            ImGui::DockBuilderDockWindow("Tool Settings", dock_right_bottom_id);

            ImGui::DockBuilderFinish(dockspace_id);
        }

        // 4. Modals Triggers
        if (state.openImportModal) { ImGui::OpenPopup("Import Image"); state.openImportModal = false; }
        if (state.openExportDdsModal) { ImGui::OpenPopup("Export DDS"); state.openExportDdsModal = false; }
        if (state.openExportStdModal) { ImGui::OpenPopup("Export Standard Image"); state.openExportStdModal = false; }
        if (state.openExportAdvancedModal) { ImGui::OpenPopup("Advanced Export Settings"); state.openExportAdvancedModal = false; }
        if (state.openSettingsModal) { ImGui::OpenPopup("Settings"); state.openSettingsModal = false; }
        if (state.openCanvasSizeModal) { ImGui::OpenPopup("Canvas Edit"); state.openCanvasSizeModal = false; }
        if (state.openSaveRaypModal) { ImGui::OpenPopup("Save Project"); state.openSaveRaypModal = false; }
        if (state.openLoadRaypModal) { ImGui::OpenPopup("Load Project"); state.openLoadRaypModal = false; }
        if (state.openLoadConfigModal) { ImGui::OpenPopup("Load Config"); state.openLoadConfigModal = false; }
        if (state.openSaveConfigModal) { ImGui::OpenPopup("Save Config"); state.openSaveConfigModal = false; }
        if (state.showRecoveryModal) { ImGui::OpenPopup("Restore Auto-Saved Session?"); state.showRecoveryModal = false; }

        // Load Config Modal
        if (ImGui::BeginPopupModal("Load Config", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char loadConfigPath[512] = "config.json";
            ImGui::Text("Enter config file path (.json):");
            ImGui::InputText("##loadconfigpath", loadConfigPath, IM_ARRAYSIZE(loadConfigPath));
            ImGui::Separator();
            if (ImGui::Button("Load", ImVec2(120, 0))) {
                if (ConfigManager::Get().Load(loadConfigPath)) {
                    ApplyTheme(ConfigManager::Get().GetTheme().c_str());
                    Logger::Get().Info("Config loaded successfully: " + std::string(loadConfigPath));
                    ImGui::CloseCurrentPopup();
                } else {
                    Logger::Get().Error("Failed to load config from: " + std::string(loadConfigPath));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Save Config Modal
        if (ImGui::BeginPopupModal("Save Config", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char saveConfigPath[512] = "config.json";
            ImGui::Text("Enter config file path (.json):");
            ImGui::InputText("##saveconfigpath", saveConfigPath, IM_ARRAYSIZE(saveConfigPath));
            ImGui::Separator();
            if (ImGui::Button("Save", ImVec2(120, 0))) {
                if (ConfigManager::Get().Save(saveConfigPath)) {
                    Logger::Get().Info("Config saved successfully to: " + std::string(saveConfigPath));
                    ImGui::CloseCurrentPopup();
                } else {
                    Logger::Get().Error("Failed to save config to: " + std::string(saveConfigPath));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Import Popup Modal
        if (ImGui::BeginPopupModal("Import Image", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char importPath[512] = "";
            ImGui::Text("Enter absolute path to image:");
            ImGui::InputText("##importpath", importPath, IM_ARRAYSIZE(importPath));
            ImGui::SameLine();
            if (ImGui::Button("Browse...##import")) {
                ShowOpenFileWin32(importPath, IM_ARRAYSIZE(importPath), "Image Files (*.dds;*.png;*.jpg;*.jpeg;*.tga;*.bmp)\0*.dds;*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All Files (*.*)\0*.*\0");
            }
            ImGui::Separator();
            if (ImGui::Button("Import", ImVec2(120, 0))) {
                TriggerBackgroundOpenDocument(importPath, device, canvas);
                ImGui::CloseCurrentPopup();
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
            ImGui::SameLine();
            if (ImGui::Button("Browse...##exportdds")) {
                ShowSaveFileWin32(exportPath, IM_ARRAYSIZE(exportPath), "DDS Files (*.dds)\0*.dds\0All Files (*.*)\0*.*\0");
            }
            ImGui::Text("DDS Format:");
            static const char* formatNames[] = {
                "BC7 (sRGB)",
                "BC7 (Linear)",
                "BC1 (sRGB)",
                "BC1 (Linear)",
                "BC2 (sRGB)",
                "BC2 (Linear)",
                "BC3 (sRGB)",
                "BC3 (Linear, DXT5)",
                "BC3 (Linear, RXGB)",
                "BC4 (Linear, Unsigned)",
                "BC5 (Linear, Unsigned)",
                "BC5 (Linear, Signed)",
                "BC6H (Linear, Unsigned)",
                "BC6H (Linear, Signed)",
                "B8G8R8A8 (Linear)",
                "B8G8R8A8 (sRGB)",
                "B8G8R8X8 (Linear)",
                "B8G8R8X8 (sRGB)",
                "R8G8B8A8 (Linear)",
                "R8G8B8A8 (sRGB)",
                "R8 (Linear, Unsigned)",
                "R8G8 (Linear, Unsigned)",
                "R8G8 (Linear, Signed)",
                "R32 (Linear, Float)"
            };
            ImGui::Combo("##ddsformat", &formatChoice, formatNames, IM_ARRAYSIZE(formatNames));
            ImGui::Separator();
            if (ImGui::Button("Export", ImVec2(120, 0))) {
                std::string chosenFormat = formatNames[formatChoice];
                if (canvas.SaveCanvasCompressed(exportPath, chosenFormat, true, "Bicubic", "Medium")) {
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
            ImGui::Text("Enter export path (PNG, JPG, BMP, TGA):");
            ImGui::InputText("##exportpathstd", exportPath, IM_ARRAYSIZE(exportPath));
            ImGui::SameLine();
            if (ImGui::Button("Browse...##exportstd")) {
                ShowSaveFileWin32(exportPath, IM_ARRAYSIZE(exportPath), "Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files (*.*)\0*.*\0");
            }
            ImGui::Spacing();
            DrawIccPresetCombo(canvas, "ICC Profile (PNG)");
            ImGui::TextDisabled("Presets only — no free-text ICC path");
            ImGui::Separator();
            if (ImGui::Button("Export", ImVec2(120, 0))) {
                if (canvas.SaveCanvasStandard(exportPath, canvas.GetExportIccPreset())) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Advanced Export Settings Modal
        if (ImGui::BeginPopupModal("Advanced Export Settings", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char exportPath[512] = "";
            static bool inited = false;
            if (!inited) {
                std::strncpy(exportPath, canvas.GetExportPath().c_str(), sizeof(exportPath));
                if (strlen(exportPath) == 0) {
                    std::strncpy(exportPath, "export.png", sizeof(exportPath));
                }
                inited = true;
            }
            
            ImGui::Text("Export File Path:");
            ImGui::InputText("##advpath", exportPath, IM_ARRAYSIZE(exportPath));
            ImGui::SameLine();
            if (ImGui::Button("Browse...##adv")) {
                ShowSaveFileWin32(exportPath, IM_ARRAYSIZE(exportPath), "DDS Files (*.dds)\0*.dds\0PNG Files (*.png)\0*.png\0All Files (*.*)\0*.*\0");
            }
            
            std::string pathStr = exportPath;
            size_t dot = pathStr.find_last_of('.');
            std::string ext = "";
            if (dot != std::string::npos) {
                ext = pathStr.substr(dot + 1);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            }
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            
            if (ext == "dds") {
                ImGui::TextColored(ImVec4(0.2f, 0.7f, 1.0f, 1.0f), "DDS Export Settings (Subprocess Compressed)");
                
                const char* formats[] = { "BC7_UNORM_SRGB", "BC7_UNORM", "BC3_UNORM", "BC1_UNORM", "RGBA8_UNORM", "RGBA16_FLOAT" };
                static int currentFormatIdx = 0;
                std::string currentFmt = canvas.GetExportFormat();
                for (int i = 0; i < IM_ARRAYSIZE(formats); ++i) {
                    if (currentFmt == formats[i]) currentFormatIdx = i;
                }
                if (ImGui::Combo("DDS Format / Preset", &currentFormatIdx, formats, IM_ARRAYSIZE(formats))) {
                    canvas.SetExportFormat(formats[currentFormatIdx]);
                }
                
                bool mips = canvas.GetExportGenerateMipMaps();
                if (ImGui::Checkbox("Generate Mipmaps", &mips)) {
                    canvas.SetExportGenerateMipMaps(mips);
                }
                
                if (mips) {
                    const char* filters[] = { "Point", "Box", "Linear", "Cubic" };
                    static int currentFilterIdx = 3;
                    std::string currentFilter = canvas.GetExportMipFilter();
                    for (int i = 0; i < IM_ARRAYSIZE(filters); ++i) {
                        if (currentFilter == filters[i]) currentFilterIdx = i;
                    }
                    if (ImGui::Combo("Mip Filter", &currentFilterIdx, filters, IM_ARRAYSIZE(filters))) {
                        canvas.SetExportMipFilter(filters[currentFilterIdx]);
                    }
                }
                
                const char* speeds[] = { "Fast", "Medium", "Slow", "Best" };
                static int currentSpeedIdx = 1;
                std::string currentSpeed = canvas.GetExportCompressionSpeed();
                for (int i = 0; i < IM_ARRAYSIZE(speeds); ++i) {
                    if (currentSpeed == speeds[i]) currentSpeedIdx = i;
                }
                if (ImGui::Combo("Compression Quality", &currentSpeedIdx, speeds, IM_ARRAYSIZE(speeds))) {
                    canvas.SetExportCompressionSpeed(speeds[currentSpeedIdx]);
                }
            } else if (ext == "png") {
                ImGui::TextColored(ImVec4(0.2f, 0.7f, 1.0f, 1.0f), "PNG Export Settings");
                DrawIccPresetCombo(canvas, "ICC Profile");
                ImGui::TextDisabled("Presets only — no free-text ICC path");
            } else {
                ImGui::TextColored(ImVec4(0.2f, 0.7f, 1.0f, 1.0f), "Standard Format Export");
                ImGui::Text("Format: %s", ext.empty() ? "None" : ext.c_str());
            }
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            
            if (ImGui::Button("Export Now", ImVec2(120, 0))) {
                canvas.SetExportPath(exportPath);
                
                bool success = false;
                if (ext == "dds") {
                    success = canvas.SaveCanvasCompressed(
                        exportPath,
                        canvas.GetExportFormat(),
                        canvas.GetExportGenerateMipMaps(),
                        canvas.GetExportMipFilter(),
                        canvas.GetExportCompressionSpeed()
                    );
                } else {
                    success = canvas.SaveCanvasStandard(exportPath, canvas.GetExportIccPreset());
                }
                
                if (success) {
                    inited = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                inited = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Settings / Preferences Popup Modal
        if (ImGui::BeginPopupModal("Settings", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (!state.settingsInitialized) {
                state.activeTheme = ConfigManager::Get().GetTheme();
                state.backupDir = ConfigManager::Get().GetBackupDir();
                state.defW = ConfigManager::Get().GetDefaultWidth();
                state.defH = ConfigManager::Get().GetDefaultHeight();
                state.autoSaveMins = ConfigManager::Get().GetAutoSaveIntervalMinutes();
                state.maxUndo = ConfigManager::Get().GetMaxUndoSteps();
                state.maxUndoMem = ConfigManager::Get().GetMaxUndoMemoryMB();
                state.settingsInitialized = true;
            }

            if (ImGui::BeginTabBar("SettingsTabs")) {
                if (ImGui::BeginTabItem("General")) {
                    ImGui::Spacing();
                    ImGui::Text("Interface Settings");
                    ImGui::Separator();
                    
                    const char* themes[] = { "Dark", "Light", "Classic" };
                    int currentThemeIdx = 0;
                    if (state.activeTheme == "Light") currentThemeIdx = 1;
                    else if (state.activeTheme == "Classic") currentThemeIdx = 2;

                    if (ImGui::Combo("Theme", &currentThemeIdx, themes, IM_ARRAYSIZE(themes))) {
                        state.activeTheme = themes[currentThemeIdx];
                        ApplyTheme(state.activeTheme);
                    }

                    ImGui::Spacing();
                    ImGui::Text("Canvas Defaults");
                    ImGui::Separator();
                    ImGui::InputInt("Default Width", &state.defW, 128, 256);
                    ImGui::InputInt("Default Height", &state.defH, 128, 256);

                    ImGui::Spacing();
                    ImGui::Text("Autosave & Backup System");
                    ImGui::Separator();
                    
                    char tempBackupDir[256] = "";
                    std::strncpy(tempBackupDir, state.backupDir.c_str(), sizeof(tempBackupDir));
                    if (ImGui::InputText("Backups Directory", tempBackupDir, IM_ARRAYSIZE(tempBackupDir))) {
                        state.backupDir = tempBackupDir;
                    }
                    ImGui::SliderInt("Autosave (minutes)", &state.autoSaveMins, 0, 60, "%d min");
                    ImGui::TextDisabled("Set to 0 to disable periodic auto-saves");

                    ImGui::Spacing();
                    ImGui::Text("Undo / Redo Cache Limits");
                    ImGui::Separator();
                    ImGui::SliderInt("Max History Steps", &state.maxUndo, 5, 200, "%d steps");
                    ImGui::SliderInt("Max RAM Cache Size", &state.maxUndoMem, 64, 2048, "%d MB");
                    
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Keybindings")) {
                    ImGui::Spacing();
                    ImGui::Text("Click 'Rebind' next to an action to assign a new physical hotkey.");
                    ImGui::Separator();
                    ImGui::Spacing();

                    auto bindings = KeymapManager::Get().GetBindings();
                    for (const auto& pair : bindings) {
                        ImGui::PushID(pair.first.c_str());
                        ImGui::Text("%s:", pair.first.c_str());
                        ImGui::SameLine(180);

                        if (state.listeningForKey && state.rebindingAction == pair.first) {
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
                                            
                                            KeymapManager::Get().BindAction(state.rebindingAction, pendingCombo);
                                            state.listeningForKey = false;
                                            state.rebindingAction = "";
                                            break;
                                        }
                                    }
                                }
                            }
                        } else {
                            ImGui::Text("%s", pair.second.ToString().c_str());
                            ImGui::SameLine(320);
                            if (ImGui::Button("Rebind")) {
                                state.rebindingAction = pair.first;
                                state.listeningForKey = true;
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
                ConfigManager::Get().SetTheme(state.activeTheme);
                ConfigManager::Get().SetDefaultWidth(state.defW);
                ConfigManager::Get().SetDefaultHeight(state.defH);
                ConfigManager::Get().SetBackupDir(state.backupDir);
                ConfigManager::Get().SetAutoSaveIntervalMinutes(state.autoSaveMins);
                ConfigManager::Get().SetMaxUndoSteps(state.maxUndo);
                ConfigManager::Get().SetMaxUndoMemoryMB(state.maxUndoMem);
                ConfigManager::Get().Save();
                
                KeymapManager::Get().Save();
                
                state.settingsInitialized = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ApplyTheme(ConfigManager::Get().GetTheme());
                KeymapManager::Get().Load();
                state.settingsInitialized = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Canvas Edit Popup Modal (Extend | Resize + resample algorithm)
        if (ImGui::BeginPopupModal("Canvas Edit", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static int targetW = 0;
            static int targetH = 0;
            static int editMode = 0; // 0=Extend, 1=Resize
            static int resample = 1; // 0=Nearest, 1=Bilinear, 2=Lanczos
            static bool initSize = false;
            if (!initSize) {
                targetW = canvas.GetWidth();
                targetH = canvas.GetHeight();
                initSize = true;
            }

            ImGui::Text("Canvas: %d x %d", canvas.GetWidth(), canvas.GetHeight());
            ImGui::Separator();
            ImGui::RadioButton("Extend (pad/crop, no scale)", &editMode, 0);
            ImGui::RadioButton("Resize (scale content)", &editMode, 1);
            ImGui::Separator();
            ImGui::InputInt("Width", &targetW, 128, 256);
            ImGui::InputInt("Height", &targetH, 128, 256);
            if (targetW < 1) targetW = 1;
            if (targetH < 1) targetH = 1;

            if (editMode == 1) {
                const char* filters[] = { "Nearest", "Bilinear", "Lanczos" };
                ImGui::Combo("Algorithm", &resample, filters, IM_ARRAYSIZE(filters));
            } else {
                ImGui::TextDisabled("Extend keeps pixels 1:1; content is centered.");
            }

            ImGui::Separator();
            if (ImGui::Button("Apply", ImVec2(120, 0))) {
                auto mode = (editMode == 0) ? Canvas::CanvasEditMode::Extend : Canvas::CanvasEditMode::Resize;
                auto filter = Canvas::ResampleFilter::Bilinear;
                if (resample == 0) filter = Canvas::ResampleFilter::Nearest;
                else if (resample == 2) filter = Canvas::ResampleFilter::Lanczos;
                canvas.EditCanvas(device, mode, targetW, targetH, filter, 0.5f, 0.5f);
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
            ImGui::SameLine();
            if (ImGui::Button("Browse...##saveproject")) {
                ShowSaveFileWin32(savePath, IM_ARRAYSIZE(savePath), "RayP Projects (*.rayp)\0*.rayp\0All Files (*.*)\0*.*\0");
            }
            ImGui::Separator();
            if (ImGui::Button("Save", ImVec2(120, 0))) {
                if (canvas.SaveCanvasRayp(savePath)) {
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
            ImGui::SameLine();
            if (ImGui::Button("Browse...##loadproject")) {
                ShowOpenFileWin32(loadPath, IM_ARRAYSIZE(loadPath), "RayP Projects (*.rayp)\0*.rayp\0All Files (*.*)\0*.*\0");
            }
            ImGui::Separator();
            if (ImGui::Button("Load", ImVec2(120, 0))) {
                TriggerBackgroundOpenDocument(loadPath, device, canvas);
                ImGui::CloseCurrentPopup();
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
                TriggerBackgroundOpenDocument(state.backupPath, device, canvas);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard", ImVec2(140, 0))) {
                try {
                    std::filesystem::remove(state.backupPath);
                } catch (...) {}
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // 5. Draw Toolbar Panel
        if (state.showToolbar) {
            ImGuiWindow* window = ImGui::FindWindowByName("Toolbar");
            bool isVertical = true;
            if (window) {
                isVertical = (window->Size.y > window->Size.x);
            }
            // Size constraints apply when floating; when docked, also clamp dock node cross-axis
            if (isVertical) {
                ImGui::SetNextWindowSizeConstraints(ImVec2(16.0f, 100.0f), ImVec2(64.0f, 16384.0f));
            } else {
                ImGui::SetNextWindowSizeConstraints(ImVec2(100.0f, 16.0f), ImVec2(16384.0f, 64.0f));
            }
            ImGui::Begin("Toolbar", &state.showToolbar, ImGuiWindowFlags_NoCollapse);

            // Docked toolbar: Dear ImGui ignores SetNextWindowSizeConstraints on docked
            // windows — size is owned by the DockNode. Clamp SizeRef every frame so the
            // strip stays thin while docked (floating still uses constraints above).
            if (ImGuiWindow* tw = ImGui::GetCurrentWindow()) {
                if (tw->DockNode && !tw->DockNode->IsFloatingNode() && tw->DockNode->HostWindow) {
                    ImGuiDockNode* node = tw->DockNode;
                    // Only constrain leaf nodes that host this toolbar (not the whole dockspace)
                    if (!node->IsSplitNode()) {
                        if (isVertical) {
                            float w = std::clamp(node->SizeRef.x > 1.f ? node->SizeRef.x : node->Size.x, 36.0f, 64.0f);
                            node->SizeRef.x = w;
                            if (std::fabs(node->Size.x - w) > 1.0f)
                                ImGui::DockBuilderSetNodeSize(node->ID, ImVec2(w, node->Size.y));
                        } else {
                            float h = std::clamp(node->SizeRef.y > 1.f ? node->SizeRef.y : node->Size.y, 36.0f, 64.0f);
                            node->SizeRef.y = h;
                            if (std::fabs(node->Size.y - h) > 1.0f)
                                ImGui::DockBuilderSetNodeSize(node->ID, ImVec2(node->Size.x, h));
                        }
                    }
                }
            }

            ImVec2 avail = ImGui::GetContentRegionAvail();

            std::string brushBind = KeymapManager::Get().GetActionShortcutString("BrushTool");
            std::string eraserBind = KeymapManager::Get().GetActionShortcutString("EraserTool");
            std::string panBind = KeymapManager::Get().GetActionShortcutString("PanTool");
            std::string rotateBind = KeymapManager::Get().GetActionShortcutString("RotateTool");
            std::string fillBind = KeymapManager::Get().GetActionShortcutString("BucketFillTool");
            std::string gradientBind = KeymapManager::Get().GetActionShortcutString("GradientTool");
            std::string pipetteBind = KeymapManager::Get().GetActionShortcutString("PipetteTool");
            std::string smudgeBind  = KeymapManager::Get().GetActionShortcutString("SmudgeTool");
            std::string selectBind = KeymapManager::Get().GetActionShortcutString("SelectToolGroup");
            std::string lassoBind = KeymapManager::Get().GetActionShortcutString("LassoToolGroup");
            std::string wandBind = KeymapManager::Get().GetActionShortcutString("WandToolGroup");
            std::string transformBind = KeymapManager::Get().GetActionShortcutString("TransformTool");

            static const ToolVariant s_SelectVariants[] = {
                { "RectSelectTool", "Rectangular Selection", ActiveTool::RectSelect },
                { "EllipseSelectTool", "Ellipse Selection", ActiveTool::EllipseSelect },
            };
            static const ToolVariant s_LassoVariants[] = {
                { "LassoSelectTool", "Lasso Selection", ActiveTool::LassoSelect },
                { "PolygonalLassoTool", "Polygonal Lasso", ActiveTool::PolygonalLasso },
            };
            static const ToolVariant s_WandVariants[] = {
                { "MagicWandTool", "Magic Wand", ActiveTool::MagicWand },
                { "QuickSelectTool", "Quick Selection", ActiveTool::QuickSelect },
                { "SmartSelectTool", "Smart Select", ActiveTool::SmartSelect },
            };

            static std::string s_RebindAction = "";
            constexpr int kToolbarButtonCount = 11;
            const bool hasSeparator = true;
            // Adaptive icon size from dock/window content region (works docked + floating)
            float btnSize = ComputeAdaptiveToolButtonSize(avail, isVertical, kToolbarButtonCount + 1 /*+Reset*/, hasSeparator);
            float gap = isVertical ? ImGui::GetStyle().ItemSpacing.y : ImGui::GetStyle().ItemSpacing.x;

            ToolbarBeginLayout(avail, isVertical, kToolbarButtonCount, btnSize, gap, hasSeparator);

            RenderToolButton("BrushTool", "Brush", ActiveTool::Brush, false, brushBind, btnSize, s_RebindAction, activeTool, brush, canvas);
            ToolbarAdvance(isVertical, gap);
            RenderToolButton("EraserTool", "Eraser", ActiveTool::Eraser, true, eraserBind, btnSize, s_RebindAction, activeTool, brush, canvas);
            ToolbarAdvance(isVertical, gap);
            RenderToolButton("BucketFillTool", "Fill", ActiveTool::BucketFill, false, fillBind, btnSize, s_RebindAction, activeTool, brush, canvas);
            ToolbarAdvance(isVertical, gap);
            RenderToolButton("GradientTool", "Gradient", ActiveTool::Gradient, false, gradientBind, btnSize, s_RebindAction, activeTool, brush, canvas);
            ToolbarAdvance(isVertical, gap);
            RenderToolButton("SmudgeTool", "Smudge", ActiveTool::Smudge, false, smudgeBind, btnSize, s_RebindAction, activeTool, brush, canvas);
            ToolbarAdvance(isVertical, gap);
            RenderToolButton("PipetteTool", "Pipette", ActiveTool::Pipette, false, pipetteBind, btnSize, s_RebindAction, activeTool, brush, canvas);
            ToolbarAdvance(isVertical, gap);
            RenderGroupedToolButton("SelectGroup", "SelectToolGroup", s_SelectVariants, IM_ARRAYSIZE(s_SelectVariants),
                "Selection Tools", selectBind, btnSize, s_RebindAction, activeTool);
            ToolbarAdvance(isVertical, gap);
            RenderGroupedToolButton("LassoGroup", "LassoToolGroup", s_LassoVariants, IM_ARRAYSIZE(s_LassoVariants),
                "Lasso Tools", lassoBind, btnSize, s_RebindAction, activeTool);
            ToolbarAdvance(isVertical, gap);
            RenderGroupedToolButton("WandGroup", "WandToolGroup", s_WandVariants, IM_ARRAYSIZE(s_WandVariants),
                "Wand / Selection Tools", wandBind, btnSize, s_RebindAction, activeTool);
            ToolbarAdvance(isVertical, gap);
            RenderToolButton("TransformTool", "Transform", ActiveTool::MovePixels, false, transformBind, btnSize, s_RebindAction, activeTool, brush, canvas);
            ToolbarAdvance(isVertical, gap);
            RenderToolButton("PanTool", "Hand", ActiveTool::Pan, false, panBind + " / " + rotateBind, btnSize, s_RebindAction, activeTool, brush, canvas);
            if (isVertical) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
            } else {
                ImGui::SameLine(0.0f, gap * 2.0f);
            }
            RenderToolButton("Reset", "Reset View", activeTool, false, std::string(""), btnSize, s_RebindAction, activeTool, brush, canvas);

            if (ImGui::BeginPopup("RebindToolPopup")) {
                ImGui::Text("Rebind Action: %s", s_RebindAction.c_str());
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.2f, 0.7f, 1.0f, 1.0f), "[Press any key to rebind]");
                
                ImGuiIO& io = ImGui::GetIO();
                bool bound = false;
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
                                
                                KeymapManager::Get().BindAction(s_RebindAction, pendingCombo);
                                KeymapManager::Get().Save();
                                bound = true;
                                break;
                            }
                        }
                    }
                }
                if (bound) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::End();
        }

        // 6. Draw Properties Panel
        if (state.showProperties) {
            ImGui::Begin("Properties", &state.showProperties, ImGuiWindowFlags_NoCollapse);
            
            ImGui::Text("Zoom: %.0f%%", canvas.GetZoom() * 100.0f);
            ImGui::Text("Pan: (%.1f, %.1f)", canvas.GetPan().x, canvas.GetPan().y);
            
            ImGui::Spacing();
            ImGui::Text("Viewport Transformations:");
            bool flipH = canvas.GetViewportFlipH();
            if (ImGui::Checkbox("Flip Horizontal", &flipH)) {
                canvas.SetViewportFlipH(flipH);
            }
            ImGui::SameLine();
            bool flipV = canvas.GetViewportFlipV();
            if (ImGui::Checkbox("Flip Vertical", &flipV)) {
                canvas.SetViewportFlipV(flipV);
            }

            float rotAngle = canvas.GetRotationAngle() * (180.0f / 3.14159265f);
            if (ImGui::SliderFloat("Rotation", &rotAngle, -180.0f, 180.0f, "%.1f deg")) {
                canvas.SetRotationAngle(rotAngle * (3.14159265f / 180.0f));
            }

            if (ImGui::Button("Reset Viewport")) {
                canvas.ResetView();
            }
            
            ImGui::Separator();
            ImGui::Text("Project Properties:");
            int pType = (canvas.GetProjectType() == Canvas::ProjectType::Simple) ? 0 : 1;
            const char* pTypeNames[] = { "Simple Project", "Advanced Project (.rayp)" };
            if (ImGui::Combo("Project Type", &pType, pTypeNames, IM_ARRAYSIZE(pTypeNames))) {
                canvas.SetProjectType((pType == 0) ? Canvas::ProjectType::Simple : Canvas::ProjectType::Advanced);
            }

            char propProjPath[512] = "";
            std::strncpy(propProjPath, canvas.GetCurrentProjectFilePath().c_str(), sizeof(propProjPath));
            ImGui::InputText("Project Path", propProjPath, IM_ARRAYSIZE(propProjPath));
            ImGui::SameLine();
            if (ImGui::Button("...##propProjPath")) {
                if (ShowOpenFileWin32(propProjPath, IM_ARRAYSIZE(propProjPath), "RayP Projects (*.rayp)\0*.rayp\0All Files (*.*)\0*.*\0")) {
                    canvas.SetCurrentProjectFilePath(propProjPath);
                }
            }

            ImGui::NewLine();
            ImGui::Separator();
            ImGui::Text("Project Output Format (DDS ↔ PNG):");
            ImGui::TextDisabled("Sets default Quick Export target for this project.");

            char propExportPath[512] = "";
            std::strncpy(propExportPath, canvas.GetExportPath().c_str(), sizeof(propExportPath));

            // Derive current format family from path extension (default PNG)
            std::string pathStr = propExportPath;
            size_t dot = pathStr.find_last_of('.');
            std::string ext = "";
            if (dot != std::string::npos) {
                ext = pathStr.substr(dot + 1);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            }
            int outFmt = (ext == "dds") ? 1 : 0; // 0=PNG, 1=DDS
            const char* outFmtNames[] = { "PNG / Standard image", "DDS (compressed)" };
            if (ImGui::Combo("Output Type", &outFmt, outFmtNames, IM_ARRAYSIZE(outFmtNames))) {
                // Switch extension on the export path
                std::string base = propExportPath;
                size_t d = base.find_last_of('.');
                if (d != std::string::npos) base = base.substr(0, d);
                if (base.empty()) base = "export";
                base += (outFmt == 1) ? ".dds" : ".png";
                std::strncpy(propExportPath, base.c_str(), sizeof(propExportPath) - 1);
                propExportPath[sizeof(propExportPath) - 1] = '\0';
                canvas.SetExportPath(propExportPath);
                ext = (outFmt == 1) ? "dds" : "png";
            }

            ImGui::InputText("Export Path", propExportPath, IM_ARRAYSIZE(propExportPath));
            ImGui::SameLine();
            if (ImGui::Button("...##propExportPath")) {
                if (ShowSaveFileWin32(propExportPath, IM_ARRAYSIZE(propExportPath),
                    "PNG (*.png)\0*.png\0DDS (*.dds)\0*.dds\0All Files (*.*)\0*.*\0")) {
                    canvas.SetExportPath(propExportPath);
                    pathStr = propExportPath;
                    dot = pathStr.find_last_of('.');
                    ext.clear();
                    if (dot != std::string::npos) {
                        ext = pathStr.substr(dot + 1);
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    }
                }
            } else if (std::string(propExportPath) != canvas.GetExportPath()) {
                canvas.SetExportPath(propExportPath);
            }

            // Re-read ext after edits
            pathStr = canvas.GetExportPath();
            dot = pathStr.find_last_of('.');
            ext.clear();
            if (dot != std::string::npos) {
                ext = pathStr.substr(dot + 1);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            }

            if (ext == "dds") {
                const char* formats[] = { "BC7_UNORM_SRGB", "BC7_UNORM", "BC3_UNORM", "BC1_UNORM", "RGBA8_UNORM", "RGBA16_FLOAT" };
                int currentFormatIdx = 0;
                std::string currentFmt = canvas.GetExportFormat();
                for (int i = 0; i < IM_ARRAYSIZE(formats); ++i) {
                    if (currentFmt == formats[i]) currentFormatIdx = i;
                }
                if (ImGui::Combo("DDS Preset", &currentFormatIdx, formats, IM_ARRAYSIZE(formats))) {
                    canvas.SetExportFormat(formats[currentFormatIdx]);
                }
                bool mips = canvas.GetExportGenerateMipMaps();
                if (ImGui::Checkbox("Mipmaps", &mips)) canvas.SetExportGenerateMipMaps(mips);
                if (mips) {
                    const char* filters[] = { "Point", "Box", "Linear", "Cubic" };
                    int currentFilterIdx = 3;
                    std::string currentFilter = canvas.GetExportMipFilter();
                    for (int i = 0; i < IM_ARRAYSIZE(filters); ++i) {
                        if (currentFilter == filters[i]) currentFilterIdx = i;
                    }
                    if (ImGui::Combo("Mip Filter", &currentFilterIdx, filters, IM_ARRAYSIZE(filters))) {
                        canvas.SetExportMipFilter(filters[currentFilterIdx]);
                    }
                }
            } else {
                DrawIccPresetCombo(canvas, "ICC Profile");
            }

            if (ImGui::Button("Quick Export (project format)", ImVec2(-1, 0))) {
                state.openQuickExportTrigger = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Export using the path/format above (same as Ctrl+E)");

            ImGui::End();
        }

        if (state.showLayers) {
            ImGui::Begin("Layers", &state.showLayers, ImGuiWindowFlags_NoCollapse);
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
                g_IsLayersHovered = true;
            }

            // Compact header actions
            float hw = ImGui::GetContentRegionAvail().x;
            if (ImGui::Button("+L", ImVec2(hw * 0.5f - 4.f, 0))) {
                canvas.CreateNewLayer(device, "Layer " + std::to_string(canvas.GetLayers().size() + 1));
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add Layer");
            ImGui::SameLine();
            if (ImGui::Button("+G", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                canvas.CreateLayerGroup(device, "Group " + std::to_string(canvas.GetLayers().size() + 1));
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add Group");

            ImGui::BeginChild("LayersList", ImVec2(0, 0), true);
            auto& layers = canvas.GetLayers();
            const float thumb = 28.0f;

            for (int i = static_cast<int>(layers.size()) - 1; i >= 0; --i) {
                auto& layer = layers[i];

                bool parentCollapsed = false;
                int currParentId = layer.parentGroupId;
                while (currParentId >= 0 && currParentId < (int)layers.size()) {
                    if (!layers[currParentId].groupExpanded) { parentCollapsed = true; break; }
                    currParentId = layers[currParentId].parentGroupId;
                }
                if (parentCollapsed) continue;

                ImGui::PushID(i);

                int depth = 0;
                for (int pId = layer.parentGroupId; pId >= 0 && pId < (int)layers.size(); pId = layers[pId].parentGroupId)
                    depth++;
                if (depth > 0) ImGui::Indent(12.0f * depth);

                // Visibility
                bool isIsolated = canvas.IsLayerIsolated(i);
                if (isIsolated) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.45f, 0.15f, 1.0f));
                bool vis = layer.visible;
                if (ImGui::Checkbox("##vis", &vis)) {
                    if (ImGui::GetIO().KeyAlt) canvas.ToggleLayerIsolation(i);
                    else { layer.visible = vis; canvas.MarkCompositeDirty(); }
                }
                if (isIsolated) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Visibility\nAlt+Click: Isolate");
                ImGui::SameLine(0, 4);

                if (layer.isGroup) {
                    if (ImGui::ArrowButton("##exp", layer.groupExpanded ? ImGuiDir_Down : ImGuiDir_Right))
                        layer.groupExpanded = !layer.groupExpanded;
                    ImGui::SameLine(0, 2);
                }

                if (!layer.isGroup && layer.srv) {
                    bool isActiveContent = (canvas.GetActiveLayerIndex() == i && canvas.GetPaintTarget() == PaintTarget::LayerContent);
                    if (isActiveContent) {
                        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.6f, 1.0f, 1.0f));
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    }
                    if (ImGui::ImageButton("##thumb", (ImTextureID)layer.srv, ImVec2(thumb, thumb), ImVec2(0,0), ImVec2(1,1))) {
                        if (ImGui::GetIO().KeyCtrl) {
                            canvas.SelectOpaquePixels(i);
                            canvas.UpdateSelectionMaskTexture(device);
                        } else {
                            canvas.SetActiveLayerIndex(i);
                            canvas.SetPaintTarget(PaintTarget::LayerContent);
                        }
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Content\nCtrl+Click: select opaque");
                    if (layer.type == Layer::Type::SmartObject || layer.type == Layer::Type::VectorSvg) {
                        ImVec2 tmin = ImGui::GetItemRectMin();
                        ImVec2 tmax = ImGui::GetItemRectMax();
                        const char* badge = (layer.type == Layer::Type::VectorSvg) ? "V" : "S";
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            ImVec2(tmax.x - 10, tmin.y), ImVec2(tmax.x, tmin.y + 10), IM_COL32(40,120,220,230), 2.f);
                        ImGui::GetWindowDrawList()->AddText(ImVec2(tmax.x - 8, tmin.y - 1), IM_COL32(255,255,255,255), badge);
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0, 2);
                } else if (layer.isGroup) {
                    ImGui::Button("G##g", ImVec2(thumb, thumb));
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                                "LAYER_INDEX",
                                ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                            int draggedIdx = *(const int*)payload->Data;
                            bool ok = draggedIdx >= 0 && draggedIdx < (int)layers.size() &&
                                      draggedIdx != i && !layers[draggedIdx].isGroup &&
                                      !IsGroupAncestorOf(layers, draggedIdx, i);
                            if (ok) {
                                DrawLayerDropHighlight(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true);
                                if (payload->IsDelivery()) {
                                    canvas.SetActiveLayerIndex(canvas.MoveLayerIntoGroup(draggedIdx, i));
                                    canvas.MarkCompositeDirty();
                                }
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::SameLine(0, 2);
                }

                // Mask: smart +M (selection -> from selection, else blank)
                if (!layer.isGroup) {
                    if (layer.hasMask && layer.maskSRV) {
                        bool isActiveMask = (canvas.GetActiveLayerIndex() == i && canvas.GetPaintTarget() == PaintTarget::LayerMask);
                        if (isActiveMask) {
                            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.6f, 1.0f, 1.0f));
                            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
                        } else {
                            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
                            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                        }
                        if (ImGui::ImageButton("##mask", (ImTextureID)layer.maskSRV, ImVec2(thumb, thumb), ImVec2(0,0), ImVec2(1,1))) {
                            canvas.SetActiveLayerIndex(i);
                            canvas.SetPaintTarget(PaintTarget::LayerMask);
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Layer Mask\nRight-click: Apply / Delete");
                        if (ImGui::BeginPopupContextItem("##maskctx")) {
                            if (ImGui::MenuItem("Apply Mask")) canvas.ApplyLayerMask(i);
                            if (ImGui::MenuItem("Delete Mask")) canvas.DeleteLayerMask(i);
                            ImGui::EndPopup();
                        }
                        ImGui::PopStyleVar();
                        ImGui::PopStyleColor();
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.55f));
                        if (ImGui::Button("+M##addm", ImVec2(thumb, thumb))) {
                            if (canvas.HasSelection())
                                canvas.CreateLayerMaskFromSelection(device, i);
                            else
                                canvas.CreateLayerMask(device, i);
                            canvas.SetPaintTarget(PaintTarget::LayerMask);
                        }
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(canvas.HasSelection()
                                ? "Add Layer Mask from Selection"
                                : "Add Layer Mask");
                        }
                    }
                    ImGui::SameLine(0, 4);
                }

                bool isSelected = (canvas.GetActiveLayerIndex() == i);
                char label[256];
                if (layer.isGroup) std::snprintf(label, sizeof(label), "[G] %s", layer.name.c_str());
                else if (layer.type == Layer::Type::VectorSvg) std::snprintf(label, sizeof(label), "[SVG] %s", layer.name.c_str());
                else if (layer.type == Layer::Type::SmartObject) std::snprintf(label, sizeof(label), "[SO] %s", layer.name.c_str());
                else std::snprintf(label, sizeof(label), "%s", layer.name.c_str());

                float rightReserve = 150.0f;
                float nameW = ImGui::GetContentRegionAvail().x - rightReserve;
                if (nameW < 40.f) nameW = 40.f;
                if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_None, ImVec2(nameW, thumb))) {
                    if (ImGui::GetIO().KeyCtrl && !layer.isGroup) {
                        canvas.SelectOpaquePixels(i);
                        canvas.UpdateSelectionMaskTexture(device);
                    } else {
                        canvas.SetActiveLayerIndex(i);
                    }
                }
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
                    ImGui::SetDragDropPayload("LAYER_INDEX", &i, sizeof(int));
                    ImGui::Text("%s", layer.name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                            "LAYER_INDEX",
                            ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                        int draggedIdx = *(const int*)payload->Data;
                        if (draggedIdx >= 0 && draggedIdx < (int)layers.size() && draggedIdx != i) {
                            const bool intoGroup = layer.isGroup && !layers[draggedIdx].isGroup &&
                                                   !IsGroupAncestorOf(layers, draggedIdx, i);
                            DrawLayerDropHighlight(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), intoGroup);
                            if (payload->IsDelivery()) {
                                if (intoGroup) {
                                    canvas.SetActiveLayerIndex(canvas.MoveLayerIntoGroup(draggedIdx, i));
                                } else {
                                    int targetParentOrig = layer.parentGroupId;
                                    int newIdx = canvas.ReorderLayer(draggedIdx, i);
                                    auto mapIdx = [](int j, int fromIdx, int toIdx) {
                                        if (fromIdx == toIdx) return j;
                                        if (fromIdx < toIdx) {
                                            if (j == fromIdx) return toIdx;
                                            if (j > fromIdx && j <= toIdx) return j - 1;
                                            return j;
                                        }
                                        if (j == fromIdx) return toIdx;
                                        if (j >= toIdx && j < fromIdx) return j + 1;
                                        return j;
                                    };
                                    int newParent = mapIdx(targetParentOrig, draggedIdx, i);
                                    auto& L = canvas.GetLayers();
                                    if (newParent == newIdx || newParent < 0 || newParent >= (int)L.size()) newParent = -1;
                                    if (newParent >= 0 && !L[newParent].isGroup) newParent = -1;
                                    L[newIdx].parentGroupId = newParent;
                                    canvas.SetActiveLayerIndex(newIdx);
                                }
                                canvas.MarkCompositeDirty();
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                if (ImGui::BeginPopupContextItem("##lyrctx")) {
                    if (ImGui::MenuItem("Remove from Group", nullptr, false, layer.parentGroupId != -1))
                        canvas.RemoveLayerFromGroup(i);
                    if (ImGui::BeginMenu("Add to Group")) {
                        for (int g = 0; g < (int)layers.size(); ++g) {
                            if (layers[g].isGroup && g != i && ImGui::MenuItem(layers[g].name.c_str()))
                                canvas.AddLayerToGroup(i, g);
                        }
                        ImGui::EndMenu();
                    }
                    if (!layer.isGroup && (layer.type == Layer::Type::SmartObject || layer.type == Layer::Type::VectorSvg)) {
                        ImGui::Separator();
                        if (ImGui::MenuItem("Rasterize Layer")) canvas.RasterizeLayer(device, i);
                    }
                    if (!layer.isGroup) {
                        ImGui::Separator();
                        if (!layer.hasMask) {
                            if (ImGui::MenuItem(canvas.HasSelection() ? "Add Mask from Selection" : "Add Mask")) {
                                if (canvas.HasSelection()) canvas.CreateLayerMaskFromSelection(device, i);
                                else canvas.CreateLayerMask(device, i);
                            }
                        } else {
                            if (ImGui::MenuItem("Apply Mask")) canvas.ApplyLayerMask(i);
                            if (ImGui::MenuItem("Delete Mask")) canvas.DeleteLayerMask(i);
                        }
                    }
                    ImGui::Separator();
                    if (layers.size() > 1 && ImGui::MenuItem("Delete Layer")) canvas.DeleteLayer(i);
                    ImGui::EndPopup();
                }

                ImGui::SameLine(0, 4);
                ImGui::SetNextItemWidth(48.f);
                if (ImGui::SliderFloat("##op", &layer.opacity, 0.f, 1.f, "%.2f"))
                    canvas.MarkCompositeDirty();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opacity");

                ImGui::SameLine(0, 2);
                static const char* blendNames[] = {
                    "N","M","S","O","A","Sub","Dk","Lt","HL","SL"
                };
                static const char* blendTips[] = {
                    "Normal","Multiply","Screen","Overlay","Add","Subtract","Darken","Lighten","Hard Light","Soft Light"
                };
                int blendIdx = (int)layer.blendMode;
                ImGui::SetNextItemWidth(42.f);
                if (ImGui::Combo("##bl", &blendIdx, blendNames, IM_ARRAYSIZE(blendNames))) {
                    layer.blendMode = (BlendMode)blendIdx;
                    canvas.MarkCompositeDirty();
                }
                if (ImGui::IsItemHovered() && blendIdx >= 0 && blendIdx < IM_ARRAYSIZE(blendTips))
                    ImGui::SetTooltip("%s", blendTips[blendIdx]);

                ImGui::SameLine(0, 2);
                bool hasFx = !layer.filters.empty();
                if (hasFx) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.9f, 0.7f));
                if (ImGui::SmallButton("Fx")) ImGui::OpenPopup("##filterPopup");
                if (hasFx) ImGui::PopStyleColor();
                if (ImGui::BeginPopup("##filterPopup")) {
                    ImGui::Text("Filters: %s", layer.name.c_str());
                    ImGui::Separator();
                    static const char* filterTypeNames[] = {"Blur","HSV","Curves","Alpha Invert","Noise"};
                    for (int fi = 0; fi < (int)layer.filters.size(); ++fi) {
                        ImGui::PushID(fi);
                        LayerFilter& flt = layer.filters[fi];
                        bool wasEnabled = flt.enabled;
                        if (ImGui::Checkbox("##fen", &flt.enabled) && flt.enabled != wasEnabled)
                            layer.filtersDirty = true;
                        ImGui::SameLine();
                        ImGui::TextUnformatted(filterTypeNames[(int)flt.type]);
                        ImGui::SameLine();
                        if (ImGui::SmallButton("X")) { layer.filters.erase(layer.filters.begin()+fi); layer.filtersDirty=true; ImGui::PopID(); break; }
                        ImGui::PopID();
                    }
                    if (ImGui::BeginMenu("Add Filter")) {
                        static const FilterType ftypes[] = {FilterType::Blur,FilterType::HSV,FilterType::Curves,FilterType::AlphaInvert,FilterType::Noise};
                        for (int ti=0;ti<5;++ti) {
                            if (ImGui::MenuItem(filterTypeNames[ti])) {
                                LayerFilter nf; nf.type=ftypes[ti]; nf.enabled=true;
                                if (ftypes[ti]==FilterType::Blur) nf.p[0]=5.f;
                                if (ftypes[ti]==FilterType::Curves){ nf.lut.resize(256); for(int li=0;li<256;++li) nf.lut[li]=(float)li/255.f; }
                                layer.filters.push_back(nf);
                                layer.filtersDirty=true;
                            }
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::EndPopup();
                }

                if (depth > 0) ImGui::Unindent(12.0f * depth);
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::End();
        }

        if (state.showChannels) {
            ImGui::Begin("Channels", &state.showChannels, ImGuiWindowFlags_NoCollapse);

            bool r = canvas.GetChannelR();
            bool g = canvas.GetChannelG();
            bool b = canvas.GetChannelB();
            bool a = canvas.GetChannelA();

            ImVec2 avail = ImGui::GetContentRegionAvail();
            // More vertical space than horizontal -> list with names; else horizontal previews only
            bool listMode = (avail.y >= avail.x);

            struct Ch { const char* name; bool* flag; ImVec4 tint; bool alphaLike; };
            Ch chans[] = {
                { "Red",   &r, ImVec4(1, 0, 0, 1), false },
                { "Green", &g, ImVec4(0, 1, 0, 1), false },
                { "Blue",  &b, ImVec4(0, 0, 1, 1), false },
                { "Alpha", &a, ImVec4(1, 1, 1, 1), true  },
            };

            float thumb = listMode ? 36.f : std::clamp(std::min(avail.x / 4.5f, avail.y - 8.f), 28.f, 64.f);

            for (int i = 0; i < 4; ++i) {
                ImGui::PushID(i);
                if (!listMode && i > 0) ImGui::SameLine(0, 6);

                ImGui::BeginGroup();
                ImGui::Checkbox("##en", chans[i].flag);
                if (listMode) ImGui::SameLine();

                // Alpha: grayscale preview (multiply RGB by white, show as luminance-style)
                ImVec4 tint = chans[i].alphaLike ? ImVec4(1, 1, 1, 1) : chans[i].tint;
                if (canvas.GetCompositeSRV()) {
                    if (chans[i].alphaLike) {
                        // Draw desaturated / B&W-style: use white tint on composite (shows luminance-ish);
                        // force monochrome by drawing as grayscale via identical RGB channels in tint
                        // and a dark background so alpha hole reads as black.
                        ImGui::ImageWithBg((ImTextureID)canvas.GetCompositeSRV(), ImVec2(thumb, thumb),
                            ImVec2(0,0), ImVec2(1,1), ImVec4(0,0,0,1), ImVec4(1,1,1,1));
                        // Overlay label that it's alpha B/W approximation
                    } else {
                        ImGui::ImageWithBg((ImTextureID)canvas.GetCompositeSRV(), ImVec2(thumb, thumb),
                            ImVec2(0,0), ImVec2(1,1), ImVec4(0,0,0,0), tint);
                    }
                } else {
                    ImGui::Dummy(ImVec2(thumb, thumb));
                }

                if (listMode) {
                    ImGui::SameLine();
                    ImGui::TextUnformatted(chans[i].name);
                } else if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", chans[i].name);
                }
                ImGui::EndGroup();
                ImGui::PopID();
            }

            canvas.SetChannelR(r);
            canvas.SetChannelG(g);
            canvas.SetChannelB(b);
            canvas.SetChannelA(a);

            ImGui::End();
        }

        // 8. Draw Standalone Tool Settings Panel (horizontal-first, icon toggles)
        if (state.showToolSettings) {
            ImGui::Begin("Tool Settings", &state.showToolSettings, ImGuiWindowFlags_NoCollapse);

            ImVec2 tsAvail = ImGui::GetContentRegionAvail();
            bool tsHorizontal = (tsAvail.x >= tsAvail.y * 0.85f);

            auto IconToggle = [](const char* id, const char* glyph, bool* v, const char* tip) {
                ImVec4 col = *v ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                               : ImGui::GetStyleColorVec4(ImGuiCol_Button);
                ImGui::PushStyleColor(ImGuiCol_Button, col);
                if (ImGui::Button(glyph, ImVec2(28, 28))) *v = !*v;
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                return *v;
            };

            auto MiniSlider = [&](const char* id, float* v, float mn, float mx, const char* tip, float width = 110.f) {
                ImGui::SetNextItemWidth(width);
                ImGui::SliderFloat(id, v, mn, mx, "%.2f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            };

            bool isBrushLike = (activeTool == ActiveTool::Brush || activeTool == ActiveTool::Eraser);

            if (isBrushLike) {
                // Brush tip presets (core BrushPresets)
                static BrushTip s_CustomTip;
                static bool s_CustomLoaded = false;
                const char* tipNames[] = { "Soft", "Hard", "Pencil", "Air", "Custom" };
                int tipIdx = state.brushTipPreset;
                ImGui::SetNextItemWidth(72.f);
                if (ImGui::Combo("##tip", &tipIdx, tipNames, IM_ARRAYSIZE(tipNames))) {
                    state.brushTipPreset = tipIdx;
                    switch (tipIdx) {
                    case 0: brush.tip = &BrushPresets::SoftRound(); break;
                    case 1: brush.tip = &BrushPresets::HardRound(); break;
                    case 2: brush.tip = &BrushPresets::Pencil(); break;
                    case 3: brush.tip = &BrushPresets::Airbrush(); break;
                    case 4: brush.tip = (s_CustomLoaded ? &s_CustomTip : nullptr); break;
                    default: brush.tip = nullptr; break;
                    }
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Brush tip preset");
                ImGui::SameLine();
                if (ImGui::SmallButton("Load Tip...")) {
                    char path[512] = "";
                    if (ShowOpenFileWin32(path, sizeof(path), "Images (*.png;*.jpg;*.bmp;*.tga)\0*.png;*.jpg;*.bmp;*.tga\0All\0*.*\0")) {
                        std::vector<uint8_t> px; int tw=0, th=0;
                        if (ImageManager::LoadImageFromFile(path, px, tw, th) && tw > 0 && th > 0) {
                            // Build grayscale tip (max size 128)
                            int side = std::min(tw, th);
                            side = std::min(side, 128);
                            s_CustomTip.size = side;
                            s_CustomTip.pixels.assign((size_t)side * side, 0);
                            s_CustomTip.name = "Custom";
                            s_CustomTip.spacingMul = 1.0f;
                            for (int y = 0; y < side; ++y) {
                                for (int x = 0; x < side; ++x) {
                                    int sx = x * tw / side;
                                    int sy = y * th / side;
                                    size_t si = ((size_t)sy * tw + sx) * 4;
                                    uint8_t r8 = px[si], g8 = px[si+1], b8 = px[si+2], a8 = px[si+3];
                                    // luminance * alpha
                                    float lum = (0.2126f*r8 + 0.7152f*g8 + 0.0722f*b8) * (a8 / 255.f);
                                    s_CustomTip.pixels[(size_t)y * side + x] = (uint8_t)std::clamp(lum, 0.f, 255.f);
                                }
                            }
                            s_CustomLoaded = true;
                            state.hasCustomBrushTip = true;
                            state.customBrushTipName = path;
                            state.brushTipPreset = 4;
                            brush.tip = &s_CustomTip;
                        }
                    }
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Load grayscale stamp texture for custom tip");
                ImGui::SameLine();

                MiniSlider("##rad", &brush.radius, 1.f, 250.f, "Radius (px)", 100.f);
                ImGui::SameLine();
                IconToggle("##pr", "P·R", &brush.pressureRadius, "Pressure → Radius");
                ImGui::SameLine();
                MiniSlider("##hrd", &brush.hardness, 0.f, 1.f, "Hardness", 80.f);
                ImGui::SameLine();
                IconToggle("##ph", "P·H", &brush.pressureHardness, "Pressure → Hardness");
                ImGui::SameLine();
                MiniSlider("##opc", &brush.opacity, 0.f, 1.f, "Opacity", 80.f);
                ImGui::SameLine();
                IconToggle("##po", "P·O", &brush.pressureOpacity, "Pressure → Opacity");
                ImGui::SameLine();
                MiniSlider("##spc", &brush.spacing, 0.01f, 5.f, "Spacing", 70.f);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.f);
                ImGui::SliderInt("##stb", &brush.stabilization, 1, 50);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stabilization");

                if (activeTool == ActiveTool::Brush) {
                    ImGui::SameLine();
                    bool mirrorH = canvas.GetMirrorHorizontal();
                    bool mirrorV = canvas.GetMirrorVertical();
                    if (ImGui::Button(mirrorH ? "[H]" : " H ", ImVec2(28, 28))) {
                        canvas.SetMirrorHorizontal(!mirrorH);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mirror Horizontal");
                    ImGui::SameLine();
                    if (ImGui::Button(mirrorV ? "[V]" : " V ", ImVec2(28, 28))) {
                        canvas.SetMirrorVertical(!mirrorV);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mirror Vertical");
                }
            }
            else if (activeTool == ActiveTool::MagicWand || activeTool == ActiveTool::SmartSelect || activeTool == ActiveTool::QuickSelect) {
                if (activeTool == ActiveTool::MagicWand) {
                    bool changed = false;
                    MiniSlider("##tol", &state.magicWandTolerance, 0.f, 1.f, "Tolerance", 140.f);
                    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
                    // also live while dragging:
                    if (ImGui::IsItemActive()) changed = true;
                    ImGui::SameLine();
                    if (ImGui::Checkbox("##cont", &state.magicWandContiguous)) changed = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contiguous");
                    if (changed && canvas.HasWandSeed()) {
                        bool add = ImGui::GetIO().KeyShift;
                        bool subtract = ImGui::GetIO().KeyAlt;
                        canvas.PreviewWandFromSeed(device, state.magicWandTolerance, add, subtract, state.magicWandContiguous);
                    }
                } else if (activeTool == ActiveTool::QuickSelect) {
                    MiniSlider("##qsr", &brush.radius, 1.f, 200.f, "Quick Select brush size", 140.f);
                } else {
                    ImGui::TextDisabled("Smart Select: draw contour");
                }
            }
            else if (activeTool == ActiveTool::BucketFill) {
                MiniSlider("##bft", &state.bucketFillTolerance, 0.f, 1.f, "Fill Tolerance", 140.f);
            }
            else if (IsSelectTool(activeTool) || IsLassoTool(activeTool)) {
                if (activeTool == ActiveTool::PolygonalLasso)
                    ImGui::TextDisabled("Click vertices · Enter/Dbl close · Esc cancel");
                else
                    ImGui::TextDisabled("Shift: add  ·  Alt: subtract");
            }
            else if (activeTool == ActiveTool::Gradient) {
                ImGui::TextDisabled("Drag: Primary → Secondary");
            }
            else if (activeTool == ActiveTool::Pipette) {
                ImGui::TextDisabled("Click canvas to sample");
            }
            else if (activeTool == ActiveTool::Smudge) {
                // No color controls — smudge only radius / strength / spacing
                MiniSlider("##smr", &state.smudge.radius, 1.f, 150.f, "Smudge Radius", 110.f);
                ImGui::SameLine();
                MiniSlider("##sms", &state.smudge.strength, 0.f, 1.f, "Strength", 100.f);
                ImGui::SameLine();
                MiniSlider("##smp", &state.smudge.spacing, 0.01f, 1.f, "Spacing", 90.f);
            }
            else if (activeTool == ActiveTool::MovePixels) {
                float sx = canvas.GetFloatingScaleX();
                float sy = canvas.GetFloatingScaleY();
                float rotDeg = canvas.GetFloatingRotation() * (180.0f / 3.14159265f);
                ImGui::SetNextItemWidth(80.f);
                if (ImGui::SliderFloat("##sx", &sx, 0.05f, 5.f, "X:%.2f")) canvas.SetFloatingScaleX(sx);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.f);
                if (ImGui::SliderFloat("##sy", &sy, 0.05f, 5.f, "Y:%.2f")) canvas.SetFloatingScaleY(sy);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90.f);
                if (ImGui::SliderFloat("##rot", &rotDeg, -180.f, 180.f, "%.0f°"))
                    canvas.SetFloatingRotation(rotDeg * (3.14159265f / 180.0f));
                ImGui::SameLine();
                if (ImGui::Button("⇄")) canvas.SetFloatingScaleX(-canvas.GetFloatingScaleX());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Flip H");
                ImGui::SameLine();
                if (ImGui::Button("⇅")) canvas.SetFloatingScaleY(-canvas.GetFloatingScaleY());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Flip V");
                ImGui::SameLine();
                if (ImGui::Button("Reset")) {
                    canvas.SetFloatingScaleX(1.f); canvas.SetFloatingScaleY(1.f); canvas.SetFloatingRotation(0.f);
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.18f, 1.0f));
                if (ImGui::Button("OK")) state.commitTransform = true;
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
                if (ImGui::Button("✕")) state.cancelTransform = true;
                ImGui::PopStyleColor();
            }
            else {
                ImGui::TextDisabled("Hand: pan · RMB/Shift: rotate");
            }

            ImGui::End();
        }

        // 9. Draw Logging Console Panel
        if (state.showConsole) {
            ImGui::Begin("Console Logs", &state.showConsole);
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

        // 10. Draw Colors Panel — SV square + Hue / Alpha sliders (adaptive)
        if (state.showColors) {
            ImGui::Begin("Colors", &state.showColors);

            ImVec2 avail = ImGui::GetContentRegionAvail();
            bool wide = avail.x > avail.y * 1.1f;

            float h, s, v;
            ImGui::ColorConvertRGBtoHSV(brush.color[0], brush.color[1], brush.color[2], h, s, v);

            // SV square size
            float sq = wide ? std::min(avail.y - 8.f, avail.x * 0.55f) : std::min(avail.x - 8.f, 180.f);
            sq = std::clamp(sq, 80.f, 220.f);

            ImVec2 sqPos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##svsq", ImVec2(sq, sq));
            bool svActive = ImGui::IsItemActive();
            bool svHovered = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Draw SV square for current Hue
            const int steps = 32;
            for (int yi = 0; yi < steps; ++yi) {
                for (int xi = 0; xi < steps; ++xi) {
                    float ss = (xi + 0.5f) / steps;
                    float vv = 1.f - (yi + 0.5f) / steps;
                    float rr, gg, bb;
                    ImGui::ColorConvertHSVtoRGB(h, ss, vv, rr, gg, bb);
                    ImU32 col = IM_COL32((int)(rr*255),(int)(gg*255),(int)(bb*255),255);
                    float x0 = sqPos.x + xi * (sq / steps);
                    float y0 = sqPos.y + yi * (sq / steps);
                    float x1 = sqPos.x + (xi + 1) * (sq / steps);
                    float y1 = sqPos.y + (yi + 1) * (sq / steps);
                    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
                }
            }
            dl->AddRect(sqPos, ImVec2(sqPos.x + sq, sqPos.y + sq), IM_COL32(120,120,120,255));
            // Cursor
            ImVec2 cur(sqPos.x + s * sq, sqPos.y + (1.f - v) * sq);
            dl->AddCircle(cur, 5.f, IM_COL32(0,0,0,200), 16, 2.f);
            dl->AddCircle(cur, 5.f, IM_COL32(255,255,255,220), 16, 1.f);

            if (svActive || (svHovered && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
                ImVec2 mp = ImGui::GetIO().MousePos;
                s = std::clamp((mp.x - sqPos.x) / sq, 0.f, 1.f);
                v = std::clamp(1.f - (mp.y - sqPos.y) / sq, 0.f, 1.f);
                ImGui::ColorConvertHSVtoRGB(h, s, v, brush.color[0], brush.color[1], brush.color[2]);
            }

            if (wide) {
                ImGui::SameLine();
                ImGui::BeginGroup();
            }

            // Hue slider 0..1
            ImGui::SetNextItemWidth(wide ? std::max(80.f, avail.x - sq - 24.f) : sq);
            if (ImGui::SliderFloat("##hue", &h, 0.f, 1.f, "H %.2f")) {
                ImGui::ColorConvertHSVtoRGB(h, s, v, brush.color[0], brush.color[1], brush.color[2]);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hue");

            ImGui::SetNextItemWidth(wide ? std::max(80.f, avail.x - sq - 24.f) : sq);
            ImGui::SliderFloat("##alpha", &brush.color[3], 0.f, 1.f, "A %.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Alpha");

            // Primary / Secondary swatches
            ImGui::ColorButton("##pri", ImVec4(brush.color[0], brush.color[1], brush.color[2], brush.color[3]),
                ImGuiColorEditFlags_AlphaPreview, ImVec2(28, 28));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Primary");

            if (wide) ImGui::EndGroup();

            ImGui::Spacing();
            // Compact palette
            static const ImVec4 paletteColors[] = {
                ImVec4(0,0,0,1), ImVec4(1,1,1,1), ImVec4(0.5f,0.5f,0.5f,1), ImVec4(0.75f,0.75f,0.75f,1),
                ImVec4(1,0,0,1), ImVec4(1,1,0,1), ImVec4(0,1,0,1), ImVec4(0,1,1,1),
                ImVec4(0,0,1,1), ImVec4(1,0,1,1), ImVec4(0.5f,0,0,1), ImVec4(0.5f,0.5f,0,1),
                ImVec4(0,0.5f,0,1), ImVec4(0,0.5f,0.5f,1), ImVec4(0,0,0.5f,1), ImVec4(0.5f,0,0.5f,1)
            };
            float cell = 18.f;
            int perRow = std::max(4, (int)(ImGui::GetContentRegionAvail().x / (cell + 4.f)));
            for (int i = 0; i < IM_ARRAYSIZE(paletteColors); ++i) {
                ImGui::PushID(i);
                if (i > 0 && (i % perRow) != 0) ImGui::SameLine(0, 3);
                if (ImGui::ColorButton("##pal", paletteColors[i], ImGuiColorEditFlags_NoTooltip, ImVec2(cell, cell))) {
                    brush.color[0] = paletteColors[i].x;
                    brush.color[1] = paletteColors[i].y;
                    brush.color[2] = paletteColors[i].z;
                    brush.color[3] = paletteColors[i].w;
                }
                ImGui::PopID();
            }

            ImGui::End();
        }

        // Draw loading progress modal
        if (g_LoadingState.isLoading) {
            ImGui::OpenPopup("Loading Document...");
            ImGuiViewport* mainViewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(mainViewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            
            if (ImGui::BeginPopupModal("Loading Document...", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
                ImGui::Text("Loading: %s", g_LoadingState.filepath.substr(g_LoadingState.filepath.find_last_of("\\/") + 1).c_str());
                
                float progress = g_LoadingState.progress;
                std::string stage;
                {
                    std::lock_guard<std::mutex> lock(g_LoadingState.mutex);
                    stage = g_LoadingState.stage;
                }
                
                ImGui::ProgressBar(progress, ImVec2(300, 20), stage.c_str());
                ImGui::EndPopup();
            }
        }
    }
}

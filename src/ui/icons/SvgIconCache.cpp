#include "SvgIconCache.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), w.data(), n);
    return w;
}
#endif

namespace Ui {

SvgIconCache& SvgIconCache::Get() {
    static SvgIconCache inst;
    return inst;
}

void SvgIconCache::SetDevice(ID3D11Device* device) {
    if (device != m_Device) {
        Clear();
        m_Device = device;
    }
}

void SvgIconCache::Clear() {
    for (auto& kv : m_Cache) {
        if (kv.second.srv) kv.second.srv->Release();
    }
    m_Cache.clear();
}

static float ParseNum(const char*& p) {
    while (*p && (isspace((unsigned char)*p) || *p == ',')) ++p;
    char* end = nullptr;
    float v = strtof(p, &end);
    p = end ? end : p;
    return v;
}

static void SampleCubic(std::vector<ImVec2>& out, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, int segs = 12) {
    for (int i = 1; i <= segs; ++i) {
        float t = (float)i / (float)segs;
        float u = 1.f - t;
        out.push_back(ImVec2(
            u*u*u*p0.x + 3*u*u*t*p1.x + 3*u*t*t*p2.x + t*t*t*p3.x,
            u*u*u*p0.y + 3*u*u*t*p1.y + 3*u*t*t*p2.y + t*t*t*p3.y));
    }
}

static bool ParsePath(const std::string& d, std::vector<std::vector<ImVec2>>& contours) {
    const char* p = d.c_str();
    std::vector<ImVec2> cur;
    ImVec2 pos(0, 0), start(0, 0);
    char cmd = 0;
    auto flush = [&]() {
        if (cur.size() >= 3) contours.push_back(cur);
        cur.clear();
    };
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) ++p;
        if (!*p) break;
        if (isalpha((unsigned char)*p)) cmd = *p++;
        if (!cmd) break;
        bool rel = islower((unsigned char)cmd);
        char c = (char)tolower((unsigned char)cmd);
        if (c == 'm') {
            float x = ParseNum(p), y = ParseNum(p);
            if (rel) { x += pos.x; y += pos.y; }
            flush();
            pos = start = ImVec2(x, y);
            cur.push_back(pos);
            cmd = rel ? 'l' : 'L';
        } else if (c == 'l') {
            float x = ParseNum(p), y = ParseNum(p);
            if (rel) { x += pos.x; y += pos.y; }
            pos = ImVec2(x, y);
            cur.push_back(pos);
        } else if (c == 'h') {
            float x = ParseNum(p);
            if (rel) x += pos.x;
            pos.x = x;
            cur.push_back(pos);
        } else if (c == 'v') {
            float y = ParseNum(p);
            if (rel) y += pos.y;
            pos.y = y;
            cur.push_back(pos);
        } else if (c == 'c') {
            float x1 = ParseNum(p), y1 = ParseNum(p);
            float x2 = ParseNum(p), y2 = ParseNum(p);
            float x = ParseNum(p), y = ParseNum(p);
            if (rel) { x1 += pos.x; y1 += pos.y; x2 += pos.x; y2 += pos.y; x += pos.x; y += pos.y; }
            ImVec2 p1(x1, y1), p2(x2, y2), p3(x, y);
            SampleCubic(cur, pos, p1, p2, p3);
            pos = p3;
        } else if (c == 'z') {
            if (!cur.empty()) cur.push_back(start);
            pos = start;
            flush();
            cmd = 0;
        } else {
            ParseNum(p); ParseNum(p);
        }
    }
    flush();
    return !contours.empty();
}

static void Rasterize(const std::vector<std::vector<ImVec2>>& contours,
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
                    float t = (py - y0) / (y1 - y0 + 1e-12f);
                    xs.push_back(x0 + t * (x1 - x0));
                }
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2) {
            int xStart = std::clamp((int)std::floor(xs[i]), 0, outW - 1);
            int xEnd = std::clamp((int)std::ceil(xs[i + 1]), 0, outW);
            for (int x = xStart; x < xEnd; ++x) {
                size_t idx = ((size_t)y * outW + x) * 4;
                rgba[idx] = rgba[idx + 1] = rgba[idx + 2] = rgba[idx + 3] = 255;
            }
        }
    }
}

bool SvgIconCache::LoadFile(const std::string& path, int pixelSize, SvgIcon& out) {
    if (!m_Device || pixelSize < 4) return false;
    std::string text;
    {
#ifdef _WIN32
        std::ifstream in(Utf8ToWide(path), std::ios::binary);
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
                if (q2 != std::string::npos)
                    ParsePath(text.substr(q + 1, q2 - q - 1), contours);
            }
        }
        pos = endTag != std::string::npos ? endTag + 1 : pos + 5;
    }
    if (contours.empty()) return false;

    std::vector<uint8_t> rgba;
    Rasterize(contours, pixelSize, pixelSize, vbX, vbY, vbW, vbH, rgba);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = pixelSize; desc.Height = pixelSize;
    desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srd = {};
    srd.pSysMem = rgba.data();
    srd.SysMemPitch = pixelSize * 4;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(m_Device->CreateTexture2D(&desc, &srd, &tex))) return false;
    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = m_Device->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    if (FAILED(hr)) return false;
    out.srv = srv; out.w = pixelSize; out.h = pixelSize;
    return true;
}

static std::string GetExecutableDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::filesystem::path p(buf);
    // narrow path for exists() — icons live next to ascii path on our builds
    return p.parent_path().string();
#else
    return {};
#endif
}

std::string SvgIconCache::ResolvePath(const char* logicalName) const {
    if (!logicalName || !*logicalName) return {};
    std::string name = logicalName;
    if (name.size() > 4 && name.substr(name.size() - 4) == ".svg")
        name = name.substr(0, name.size() - 4);

    std::string exeDir = GetExecutableDir();

    // Prefer next-to-exe (POST_BUILD copies here), then source tree / cwd
    std::vector<std::string> roots;
    if (!exeDir.empty()) {
        roots.push_back(exeDir + "/icons/");
        roots.push_back(exeDir + "/../icons/");
        // launcher may sit in Release/, core in Release/bin/
        roots.push_back(exeDir + "/bin/icons/");
        roots.push_back(exeDir + "/../bin/icons/");
    }
    roots.push_back("icons/");
    roots.push_back("src/resources/icons/");
    roots.push_back("resources/icons/");
    roots.push_back("testfield/svg/");
    roots.push_back("../testfield/svg/");
    roots.push_back("../../testfield/svg/");
    // repo-relative from bin
    if (!exeDir.empty()) {
        roots.push_back(exeDir + "/../../../src/resources/icons/");
        roots.push_back(exeDir + "/../../../testfield/svg/");
        roots.push_back(exeDir + "/../../../../src/resources/icons/");
        roots.push_back(exeDir + "/../../../../testfield/svg/");
    }

    // Logical name → on-disk file (aliases for older call sites)
    std::string candidates[] = {
        name + ".svg",
        (name == "tool_brush" ? "brush.svg" : ""),
        (name == "tool_eraser" ? "eraser.svg" : ""),
        (name == "tool_fill" || name == "fill" ? "fill_bucket.svg" : ""),
        (name == "fill_bucket" ? "fill_bucket.svg" : ""),
        (name == "layer_fill" || name == "add_fill" ? "layer_fill.svg" : ""),
        (name == "layer_vector" || name == "add_vector" ? "layer_vector.svg" : ""),
        (name == "layer_merge" || name == "merge" ? "layer_merge.svg" : ""),
        (name == "layer_fx" || name == "fx" || name == "effects" ? "layer_fx.svg" : ""),
        (name == "tool_blur" || name == "blur" ? "tool_blur.svg" : ""),
        (name == "tool_stamp" || name == "stamp" ? "tool_stamp.svg" : ""),
        (name == "tool_vector_pen" || name == "vector_pen" ? "tool_vector_pen.svg" : ""),
        (name == "tool_vector_line" || name == "vector_line" ? "tool_vector_line.svg" : ""),
        (name == "tool_pipette" || name == "pipette" ? "tool_pipette.svg" : ""),
        (name == "brush" ? "brush.svg" : ""),
        (name == "eraser" ? "eraser.svg" : ""),
        (name == "search" || name == "loupe" || name == "magnifier" ? "search.svg" : ""),
        "placeholder.svg",
    };

    for (const auto& root : roots) {
        for (const auto& file : candidates) {
            if (file.empty()) continue;
            std::string path = root + file;
            std::error_code ec;
            if (std::filesystem::exists(std::filesystem::u8path(path), ec))
                return path;
        }
    }
    return {};
}

const SvgIcon* SvgIconCache::Get(const char* logicalName, int pixelSize) {
    if (!m_Device || !logicalName) return nullptr;
    std::string key = std::string(logicalName) + "#" + std::to_string(pixelSize);
    auto it = m_Cache.find(key);
    if (it != m_Cache.end()) return it->second.valid() ? &it->second : nullptr;

    SvgIcon icon;
    std::string path = ResolvePath(logicalName);
    if (!path.empty())
        LoadFile(path, pixelSize, icon);
    if (!icon.valid()) {
        // force placeholder
        path = ResolvePath("placeholder");
        if (!path.empty()) LoadFile(path, pixelSize, icon);
    }
    m_Cache[key] = icon;
    return icon.valid() ? &m_Cache[key] : nullptr;
}

void SvgIconCache::Draw(const SvgIcon* icon, ImVec2 min, ImVec2 max, ImU32 tint) const {
    if (!icon || !icon->srv) return;
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)icon->srv, min, max, ImVec2(0, 0), ImVec2(1, 1), tint);
}

void SvgIconCache::DrawCentered(const SvgIcon* icon, ImVec2 center, float side, ImU32 tint) const {
    ImVec2 half(side * 0.5f, side * 0.5f);
    Draw(icon, ImVec2(center.x - half.x, center.y - half.y), ImVec2(center.x + half.x, center.y + half.y), tint);
}

} // namespace Ui

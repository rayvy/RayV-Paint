#include "AtlasUi.h"
#include "../atlas/PngIo.h"
#include "../convert/FormatCatalog.h"
#include "../convert/TexconvRunner.h"
#include "../common/SettingsStore.h"
#include "../common/Utf8.h"

#include "imgui.h"

#include <algorithm>
#include <filesystem>

namespace helpers {
namespace {

const int kMaxSizes[] = { 1024, 2048, 4096, 8192 };
const char* kMaxSizeLabels[] = { "1024", "2048", "4096", "8192" };
const char* kSpeeds[] = { "Fast", "Medium", "Slow" };

void RememberDir(const std::vector<std::string>& files) {
    if (files.empty()) return;
    Settings s = LoadSettings();
    std::filesystem::path p(Utf8ToWide(files.front()));
    s.lastPngDir = WideToUtf8(p.parent_path().wstring());
    SaveSettings(s);
}

void PersistAtlasPrefs(const AtlasUiState& st) {
    Settings s = LoadSettings();
    s.atlasPadding = st.padding;
    s.atlasMaxSize = kMaxSizes[st.maxSizeIndex < 0 || st.maxSizeIndex > 3 ? 2 : st.maxSizeIndex];
    s.atlasPowerOfTwo = st.powerOfTwo;
    s.atlasExportDds = st.alsoExportDds;
    if (st.formatIndex >= 0 && st.formatIndex < kDdsFormatCount)
        s.lastDdsFormat = kDdsFormats[st.formatIndex].texconv;
    s.generateMips = st.generateMips;
    SaveSettings(s);
}

void Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool RunAtlasExport(AtlasUiState& st, Dx11ImGuiApp& app) {
    if (st.files.empty() || st.busy) return false;
    st.busy = true;
    st.status = "Loading sprites…";
    Pump();

    std::vector<AtlasSprite> sprites;
    std::string err;
    if (!LoadSprites(st.files, sprites, &err)) {
        st.status = err;
        st.busy = false;
        return false;
    }

    AtlasOptions opt;
    opt.padding = st.padding;
    opt.maxSize = kMaxSizes[st.maxSizeIndex < 0 || st.maxSizeIndex > 3 ? 2 : st.maxSizeIndex];
    opt.powerOfTwo = st.powerOfTwo;

    st.status = "Packing…";
    Pump();
    AtlasResult pack = PackAtlas(std::move(sprites), opt);
    if (!pack.ok) {
        st.status = pack.message;
        st.busy = false;
        return false;
    }
    if (!RasterizeAtlas(pack)) {
        st.status = "Rasterize failed";
        st.busy = false;
        return false;
    }

    ReleaseAtlasPreview(st);
    st.previewSrv = app.CreateRgbaSrv(pack.image.rgba.data(), pack.image.width, pack.image.height);
    st.previewW = pack.image.width;
    st.previewH = pack.image.height;
    st.lastPack = std::move(pack);

    RememberDir(st.files);
    PersistAtlasPrefs(st);

    Settings s = LoadSettings();
    std::string startDir = ResolveBrowseStartDir(s);
    std::string outPng = BrowseSavePngFile(startDir, "atlas.png");
    if (outPng.empty()) {
        st.status = st.lastPack.message + " (save cancelled)";
        st.busy = false;
        return true;
    }

    if (!SavePng(outPng, st.lastPack.image, &err)) {
        st.status = "PNG save failed: " + err;
        st.busy = false;
        return false;
    }

    std::filesystem::path jsonPath = std::filesystem::path(Utf8ToWide(outPng)).replace_extension(L".json");
    SaveAtlasJson(WideToUtf8(jsonPath.wstring()), st.lastPack, &err);

    // Remember folder of output for next time
    {
        Settings s2 = LoadSettings();
        s2.lastPngDir = WideToUtf8(std::filesystem::path(Utf8ToWide(outPng)).parent_path().wstring());
        SaveSettings(s2);
    }

    if (st.alsoExportDds) {
        st.status = "Converting atlas to DDS…";
        Pump();
        ConvertOptions copt;
        copt.format = kDdsFormats[st.formatIndex < 0 || st.formatIndex >= kDdsFormatCount
                                      ? 0 : st.formatIndex].texconv;
        copt.generateMips = st.generateMips;
        copt.compressionSpeed = kSpeeds[st.speedIndex < 0 || st.speedIndex > 2 ? 1 : st.speedIndex];
        auto cr = ConvertOne(outPng, copt);
        if (!cr.ok) {
            st.status = "Atlas PNG OK; DDS failed: " + cr.message;
            st.busy = false;
            return false;
        }
        st.status = "Saved " + outPng + " + " + cr.dstPath + " + JSON";
    } else {
        st.status = "Saved " + outPng + " + " + WideToUtf8(jsonPath.wstring());
    }

    st.busy = false;
    return true;
}

} // namespace

void ReleaseAtlasPreview(AtlasUiState& st) {
    if (st.previewSrv) {
        st.previewSrv->Release();
        st.previewSrv = nullptr;
    }
    st.previewW = st.previewH = 0;
}

void DrawAtlasUi(AtlasUiState& st, Dx11ImGuiApp& app) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("Texture Atlas Creator", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextUnformatted("Pack multiple images into one atlas PNG (+ JSON rects).");
    ImGui::TextDisabled("Optional: also convert the atlas to DDS (converter has no atlas options).");
    ImGui::Separator();

    if (ImGui::Button("Add images…") && !st.busy) {
        Settings s = LoadSettings();
        auto added = BrowseOpenPngFiles(ResolveBrowseStartDir(s));
        for (auto& a : added) {
            bool exists = false;
            for (auto& f : st.files) if (f == a) { exists = true; break; }
            if (!exists) st.files.push_back(std::move(a));
        }
        if (!added.empty()) RememberDir(added);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear") && !st.busy) {
        st.files.clear();
        ReleaseAtlasPreview(st);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d image(s)", (int)st.files.size());

    ImGui::BeginChild("##alist", ImVec2(0, 100), true);
    int removeIdx = -1;
    for (int i = 0; i < (int)st.files.size(); ++i) {
        ImGui::PushID(i);
        if (ImGui::SmallButton("x") && !st.busy) removeIdx = i;
        ImGui::SameLine();
        ImGui::TextUnformatted(st.files[i].c_str());
        ImGui::PopID();
    }
    if (removeIdx >= 0) st.files.erase(st.files.begin() + removeIdx);
    ImGui::EndChild();

    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("Padding", &st.padding);
    if (st.padding < 0) st.padding = 0;
    if (st.padding > 64) st.padding = 64;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::Combo("Max size", &st.maxSizeIndex, kMaxSizeLabels, 4);
    ImGui::SameLine();
    ImGui::Checkbox("Power of two", &st.powerOfTwo);

    ImGui::Separator();
    ImGui::Checkbox("Also convert atlas to DDS", &st.alsoExportDds);
    if (st.alsoExportDds) {
        const char* labels[64];
        int n = kDdsFormatCount < 64 ? kDdsFormatCount : 64;
        for (int i = 0; i < n; ++i) labels[i] = kDdsFormats[i].uiLabel;
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("DDS format", &st.formatIndex, labels, n);
        ImGui::Checkbox("Mipmaps##atlas", &st.generateMips);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::Combo("Quality##atlas", &st.speedIndex, kSpeeds, 3);
    }

    ImGui::Separator();
    const bool canGo = !st.files.empty() && !st.busy;
    if (!canGo) ImGui::BeginDisabled();
    if (ImGui::Button("Pack & Save…", ImVec2(160, 0)))
        RunAtlasExport(st, app);
    if (!canGo) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(100, 0)))
        app.RequestClose();

    if (!st.status.empty())
        ImGui::TextWrapped("%s", st.status.c_str());

    if (st.previewSrv) {
        ImGui::Separator();
        ImGui::Text("Preview %dx%d", st.previewW, st.previewH);
        float maxW = ImGui::GetContentRegionAvail().x;
        float maxH = ImGui::GetContentRegionAvail().y - 8.f;
        float scale = 1.f;
        if (st.previewW > 0 && st.previewH > 0) {
            scale = std::min(maxW / (float)st.previewW, maxH / (float)st.previewH);
            if (scale > 1.f) scale = 1.f;
        }
        ImGui::Image((ImTextureID)st.previewSrv,
                     ImVec2(st.previewW * scale, st.previewH * scale));
    }

    ImGui::End();
}

} // namespace helpers

#include "ConverterUi.h"
#include "Dx11ImGuiApp.h"
#include "../convert/FormatCatalog.h"
#include "../convert/TexconvRunner.h"
#include "../atlas/PngIo.h"
#include "../common/SettingsStore.h"
#include "../common/Utf8.h"

#include "imgui.h"

#include <filesystem>

namespace helpers {
namespace {

const char* kSpeeds[] = { "Fast", "Medium", "Slow" };

void RememberDirFromFiles(const std::vector<std::string>& files) {
    if (files.empty()) return;
    Settings s = LoadSettings();
    std::filesystem::path p(Utf8ToWide(files.front()));
    s.lastPngDir = WideToUtf8(p.parent_path().wstring());
    SaveSettings(s);
}

void PushLog(ConverterUiState& st, const std::string& line) {
    st.logLines.push_back(line);
    if (st.logLines.size() > 200)
        st.logLines.erase(st.logLines.begin(), st.logLines.begin() + 50);
}

} // namespace

void RunConvert(ConverterUiState& st) {
    if (st.files.empty() || st.busy) return;
    st.busy = true;
    st.progress = 0.f;
    st.status = "Converting…";
    PushLog(st, "Starting batch (" + std::to_string(st.files.size()) + " files)");

    ConvertOptions opt;
    if (st.formatIndex < 0 || st.formatIndex >= kDdsFormatCount) st.formatIndex = 0;
    opt.format = kDdsFormats[st.formatIndex].texconv;
    opt.generateMips = st.generateMips;
    opt.compressionSpeed = kSpeeds[st.speedIndex < 0 || st.speedIndex > 2 ? 1 : st.speedIndex];
    opt.mipFilter = "CUBIC";

    // Persist prefs
    {
        Settings s = LoadSettings();
        s.lastDdsFormat = opt.format;
        s.generateMips = opt.generateMips;
        s.compressionSpeed = opt.compressionSpeed;
        SaveSettings(s);
    }
    RememberDirFromFiles(st.files);

    int okN = 0, failN = 0;
    auto results = ConvertMany(st.files, opt, [&](int i, int total, const std::string& path) {
        st.progress = total > 0 ? (float)i / (float)total : 1.f;
        if (!path.empty())
            st.status = path;
        // Pump messages so window stays responsive during long BC7
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    });

    for (const auto& r : results) {
        if (r.ok) {
            ++okN;
            PushLog(st, "OK  " + r.dstPath);
        } else {
            ++failN;
            PushLog(st, "FAIL " + r.srcPath + " — " + r.message);
        }
    }

    st.progress = 1.f;
    st.busy = false;
    st.status = "Done: " + std::to_string(okN) + " ok, " + std::to_string(failN) + " failed";
    PushLog(st, st.status);
}

void DrawConverterUi(ConverterUiState& st, Dx11ImGuiApp& app) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("PNG → DDS Converter", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextUnformatted("Convert PNG images to DDS next to the source files.");
    ImGui::Separator();

    if (ImGui::Button("Add PNGs…") && !st.busy) {
        Settings s = LoadSettings();
        auto added = BrowseOpenPngFiles(ResolveBrowseStartDir(s));
        for (auto& a : added) {
            bool exists = false;
            for (auto& f : st.files) if (f == a) { exists = true; break; }
            if (!exists) st.files.push_back(std::move(a));
        }
        if (!added.empty()) RememberDirFromFiles(added);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear list") && !st.busy)
        st.files.clear();
    ImGui::SameLine();
    ImGui::TextDisabled("%d file(s)", (int)st.files.size());

    ImGui::BeginChild("##filelist", ImVec2(0, 140), true);
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

    ImGui::Spacing();
    ImGui::TextUnformatted("DDS format");
    // Build label list once per frame (tiny)
    const char* labels[64];
    int n = kDdsFormatCount < 64 ? kDdsFormatCount : 64;
    for (int i = 0; i < n; ++i) labels[i] = kDdsFormats[i].uiLabel;
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##fmt", &st.formatIndex, labels, n);

    ImGui::Checkbox("Generate mipmaps", &st.generateMips);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::Combo("Quality", &st.speedIndex, kSpeeds, 3);

    ImGui::Separator();
    const bool canGo = !st.files.empty() && !st.busy;
    if (!canGo) ImGui::BeginDisabled();
    if (ImGui::Button("Convert", ImVec2(140, 0)))
        RunConvert(st);
    if (!canGo) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(100, 0)))
        app.RequestClose();

    if (st.busy || st.progress > 0.f)
        ImGui::ProgressBar(st.progress, ImVec2(-1, 0), st.busy ? "Working…" : "");
    if (!st.status.empty())
        ImGui::TextWrapped("%s", st.status.c_str());

    ImGui::Separator();
    ImGui::TextUnformatted("Log");
    ImGui::BeginChild("##log", ImVec2(0, 0), true);
    for (const auto& line : st.logLines)
        ImGui::TextUnformatted(line.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.f)
        ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();

    ImGui::End();
}

} // namespace helpers

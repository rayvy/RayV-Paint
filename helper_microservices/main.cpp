// RayVHelpers — isolated PNG→DDS converter + atlas creator.
// No link to RayVPaint paint core. Registration API shared with main app only.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

#include "common/Utf8.h"
#include "common/SettingsStore.h"
#include "common/BatchCollector.h"
#include "shell/HelperShellRegister.h"
#include "convert/FormatCatalog.h"
#include "ui/Dx11ImGuiApp.h"
#include "ui/ConverterUi.h"
#include "ui/AtlasUi.h"
#include "ui/HubUi.h"

#pragma comment(lib, "shell32.lib")

namespace {

enum class CliMode { Hub, Convert, Atlas, Register, Unregister };

bool Eq(const std::wstring& a, const wchar_t* b) {
    return _wcsicmp(a.c_str(), b) == 0;
}

bool IsFlag(const std::wstring& a) {
    return !a.empty() && a[0] == L'-';
}

std::string ExtLower(const std::string& path) {
    auto pos = path.find_last_of('.');
    if (pos == std::string::npos) return {};
    std::string e = path.substr(pos);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

bool LooksLikeImage(const std::string& path) {
    auto e = ExtLower(path);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" ||
           e == ".tga" || e == ".tif" || e == ".tiff" || e == ".dds";
}

void ApplySettingsToConverter(helpers::ConverterUiState& st) {
    helpers::Settings s = helpers::LoadSettings();
    st.formatIndex = helpers::FindFormatIndex(s.lastDdsFormat.c_str());
    st.generateMips = s.generateMips;
    if (s.compressionSpeed == "Fast") st.speedIndex = 0;
    else if (s.compressionSpeed == "Slow") st.speedIndex = 2;
    else st.speedIndex = 1;
}

void ApplySettingsToAtlas(helpers::AtlasUiState& st) {
    helpers::Settings s = helpers::LoadSettings();
    st.padding = s.atlasPadding;
    st.powerOfTwo = s.atlasPowerOfTwo;
    st.alsoExportDds = s.atlasExportDds;
    st.formatIndex = helpers::FindFormatIndex(s.lastDdsFormat.c_str());
    st.generateMips = s.generateMips;
    if (s.compressionSpeed == "Fast") st.speedIndex = 0;
    else if (s.compressionSpeed == "Slow") st.speedIndex = 2;
    else st.speedIndex = 1;
    const int sizes[] = { 1024, 2048, 4096, 8192 };
    st.maxSizeIndex = 2;
    for (int i = 0; i < 4; ++i)
        if (sizes[i] == s.atlasMaxSize) st.maxSizeIndex = i;
}

int RunUi(CliMode mode, std::vector<std::string> files) {
    HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));

    const wchar_t* title = L"RayV Helpers";
    int w = 520, h = 420;
    if (mode == CliMode::Convert) {
        title = L"RayV — PNG to DDS";
        w = 560; h = 520;
    } else if (mode == CliMode::Atlas) {
        title = L"RayV — Atlas Creator";
        w = 640; h = 620;
    }

    helpers::Dx11ImGuiApp app;
    if (!app.Create(title, w, h, icon))
        return 1;

    helpers::AppMode uiMode =
        mode == CliMode::Convert ? helpers::AppMode::Convert :
        mode == CliMode::Atlas   ? helpers::AppMode::Atlas :
                                   helpers::AppMode::Hub;

    helpers::ConverterUiState conv;
    helpers::AtlasUiState atlas;
    ApplySettingsToConverter(conv);
    ApplySettingsToAtlas(atlas);
    conv.files = files;
    atlas.files = files;

    int code = app.Run([&]() {
        if (uiMode == helpers::AppMode::Hub) {
            auto next = helpers::DrawHubUi(app);
            if (next == helpers::AppMode::Convert) {
                uiMode = helpers::AppMode::Convert;
                SetWindowTextW(app.Hwnd(), L"RayV — PNG to DDS");
            } else if (next == helpers::AppMode::Atlas) {
                uiMode = helpers::AppMode::Atlas;
                SetWindowTextW(app.Hwnd(), L"RayV — Atlas Creator");
            }
        } else if (uiMode == helpers::AppMode::Convert) {
            helpers::DrawConverterUi(conv, app);
        } else if (uiMode == helpers::AppMode::Atlas) {
            helpers::DrawAtlasUi(atlas, app);
        }
    });

    helpers::ReleaseAtlasPreview(atlas);
    app.Destroy();
    return code;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // COM for WIC / file dialogs
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        CoUninitialize();
        return 1;
    }

    CliMode mode = CliMode::Hub;
    std::vector<std::string> files;
    bool afterDashDash = false;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (!afterDashDash && Eq(a, L"--")) {
            afterDashDash = true;
            continue;
        }
        if (!afterDashDash && (Eq(a, L"--mode") || Eq(a, L"-m")) && i + 1 < argc) {
            std::wstring m = argv[++i];
            if (Eq(m, L"convert") || Eq(m, L"dds") || Eq(m, L"png2dds"))
                mode = CliMode::Convert;
            else if (Eq(m, L"atlas") || Eq(m, L"pack"))
                mode = CliMode::Atlas;
            else if (Eq(m, L"hub") || Eq(m, L"home"))
                mode = CliMode::Hub;
            continue;
        }
        if (!afterDashDash && (Eq(a, L"--register") || Eq(a, L"/register"))) {
            mode = CliMode::Register;
            continue;
        }
        if (!afterDashDash && (Eq(a, L"--unregister") || Eq(a, L"/unregister"))) {
            mode = CliMode::Unregister;
            continue;
        }
        if (!afterDashDash && (Eq(a, L"--convert"))) {
            mode = CliMode::Convert;
            continue;
        }
        if (!afterDashDash && (Eq(a, L"--atlas"))) {
            mode = CliMode::Atlas;
            continue;
        }
        if (!afterDashDash && IsFlag(a))
            continue;

        // Path argument
        std::string u = helpers::WideToUtf8(a);
        // Strip surrounding quotes if any leaked
        if (u.size() >= 2 && u.front() == '"' && u.back() == '"')
            u = u.substr(1, u.size() - 2);
        if (!u.empty())
            files.push_back(std::move(u));
    }
    LocalFree(argv);

    // If files given without mode, default to convert (context-menu friendly).
    if (mode == CliMode::Hub && !files.empty())
        mode = CliMode::Convert;

    if (mode == CliMode::Register) {
        bool ok = HelperShellRegister::EnsureRegistered();
        CoUninitialize();
        return ok ? 0 : 1;
    }
    if (mode == CliMode::Unregister) {
        bool ok = HelperShellRegister::Unregister();
        CoUninitialize();
        return ok ? 0 : 1;
    }

    // Filter to images when present
    if (!files.empty()) {
        std::vector<std::string> img;
        for (auto& f : files)
            if (LooksLikeImage(f)) img.push_back(std::move(f));
        files = std::move(img);
    }

    // Merge multi-process Explorer selection into one window
    if ((mode == CliMode::Convert || mode == CliMode::Atlas) && !files.empty()) {
        std::vector<std::string> merged;
        const char* tag = mode == CliMode::Convert ? "convert" : "atlas";
        if (!helpers::CollectBatchFiles(tag, files, merged)) {
            CoUninitialize();
            return 0; // follower — quiet exit
        }
        files = std::move(merged);
    }

    int rc = RunUi(mode, std::move(files));
    CoUninitialize();
    return rc;
}

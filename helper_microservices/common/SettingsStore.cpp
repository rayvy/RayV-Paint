#include "SettingsStore.h"
#include "Utf8.h"

#include <fstream>
#include <filesystem>
#include <shlobj.h>

namespace helpers {
namespace {

std::filesystem::path SettingsPath() {
    wchar_t appdata[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appdata)))
        return {};
    auto dir = std::filesystem::path(appdata) / L"RayVPaint";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / L"helpers_settings.ini";
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

bool ParseBool(const std::string& v, bool def) {
    if (v == "1" || v == "true" || v == "True" || v == "yes") return true;
    if (v == "0" || v == "false" || v == "False" || v == "no") return false;
    return def;
}

} // namespace

Settings LoadSettings() {
    Settings s;
    auto path = SettingsPath();
    if (path.empty()) return s;
    std::ifstream in(path);
    if (!in) return s;
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == '[') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        if (key == "last_png_dir") s.lastPngDir = val;
        else if (key == "last_dds_format") s.lastDdsFormat = val;
        else if (key == "generate_mips") s.generateMips = ParseBool(val, s.generateMips);
        else if (key == "compression_speed") s.compressionSpeed = val;
        else if (key == "mip_filter") s.mipFilter = val;
        else if (key == "atlas_padding") {
            try { s.atlasPadding = std::stoi(val); } catch (...) {}
        } else if (key == "atlas_max_size") {
            try { s.atlasMaxSize = std::stoi(val); } catch (...) {}
        } else if (key == "atlas_power_of_two") s.atlasPowerOfTwo = ParseBool(val, s.atlasPowerOfTwo);
        else if (key == "atlas_export_dds") s.atlasExportDds = ParseBool(val, s.atlasExportDds);
    }
    return s;
}

void SaveSettings(const Settings& s) {
    auto path = SettingsPath();
    if (path.empty()) return;
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    out << "# RayV Helpers settings\n";
    out << "last_png_dir=" << s.lastPngDir << "\n";
    out << "last_dds_format=" << s.lastDdsFormat << "\n";
    out << "generate_mips=" << (s.generateMips ? "1" : "0") << "\n";
    out << "compression_speed=" << s.compressionSpeed << "\n";
    out << "mip_filter=" << s.mipFilter << "\n";
    out << "atlas_padding=" << s.atlasPadding << "\n";
    out << "atlas_max_size=" << s.atlasMaxSize << "\n";
    out << "atlas_power_of_two=" << (s.atlasPowerOfTwo ? "1" : "0") << "\n";
    out << "atlas_export_dds=" << (s.atlasExportDds ? "1" : "0") << "\n";
}

std::string ResolveBrowseStartDir(const Settings& s) {
    std::error_code ec;
    if (!s.lastPngDir.empty()) {
        auto p = std::filesystem::path(Utf8ToWide(s.lastPngDir));
        if (std::filesystem::is_directory(p, ec))
            return s.lastPngDir;
    }
    wchar_t docs[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYDOCUMENTS, nullptr, 0, docs)))
        return WideToUtf8(docs);
    return {};
}

} // namespace helpers

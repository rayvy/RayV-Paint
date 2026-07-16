#include "PackageIO.h"
#include "../core/Logger.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace rvp {
namespace {

bool WriteU16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back((uint8_t)(v & 0xFF));
    buf.push_back((uint8_t)((v >> 8) & 0xFF));
    return true;
}
bool WriteU32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back((uint8_t)(v & 0xFF));
    buf.push_back((uint8_t)((v >> 8) & 0xFF));
    buf.push_back((uint8_t)((v >> 16) & 0xFF));
    buf.push_back((uint8_t)((v >> 24) & 0xFF));
    return true;
}
bool ReadU16(const uint8_t*& p, const uint8_t* end, uint16_t& v) {
    if (p + 2 > end) return false;
    v = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    p += 2;
    return true;
}
bool ReadU32(const uint8_t*& p, const uint8_t* end, uint32_t& v) {
    if (p + 4 > end) return false;
    v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;
    return true;
}

std::string NormalizePath(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    while (!s.empty() && s[0] == '/') s.erase(s.begin());
    return s;
}

} // namespace

bool WritePackageToMemory(const Package& pkg, std::vector<uint8_t>& out, std::string* err) {
    out.clear();
    if (pkg.manifestJson.empty()) {
        if (err) *err = "empty manifest";
        return false;
    }
    WriteU32(out, kPackageMagic);
    WriteU32(out, kPackageVersion);
    if (pkg.manifestJson.size() > 0xFFFFFFFFu) {
        if (err) *err = "manifest too large";
        return false;
    }
    WriteU32(out, (uint32_t)pkg.manifestJson.size());
    out.insert(out.end(), pkg.manifestJson.begin(), pkg.manifestJson.end());
    if (pkg.resources.size() > 0xFFFFFFFFu) {
        if (err) *err = "too many resources";
        return false;
    }
    WriteU32(out, (uint32_t)pkg.resources.size());
    for (const auto& kv : pkg.resources) {
        std::string path = NormalizePath(kv.first);
        if (path.size() > 0xFFFFu) {
            if (err) *err = "resource path too long: " + path;
            return false;
        }
        WriteU16(out, (uint16_t)path.size());
        out.insert(out.end(), path.begin(), path.end());
        if (kv.second.size() > 0xFFFFFFFFu) {
            if (err) *err = "resource too large: " + path;
            return false;
        }
        WriteU32(out, (uint32_t)kv.second.size());
        out.insert(out.end(), kv.second.begin(), kv.second.end());
    }
    return true;
}

bool ReadPackageFromMemory(const uint8_t* data, size_t size, Package& outPkg, std::string* err) {
    outPkg = Package{};
    if (!data || size < 16) {
        if (err) *err = "buffer too small";
        return false;
    }
    const uint8_t* p = data;
    const uint8_t* end = data + size;
    uint32_t magic = 0, ver = 0, manSize = 0;
    if (!ReadU32(p, end, magic) || magic != kPackageMagic) {
        if (err) *err = "bad magic (expected RVPK)";
        return false;
    }
    if (!ReadU32(p, end, ver) || ver != kPackageVersion) {
        if (err) *err = "unsupported package version";
        return false;
    }
    if (!ReadU32(p, end, manSize) || p + manSize > end) {
        if (err) *err = "truncated manifest";
        return false;
    }
    outPkg.manifestJson.assign((const char*)p, (const char*)p + manSize);
    p += manSize;

    PackageFormat fmt = PackageFormat::Unknown;
    ParseManifestFormat(outPkg.manifestJson, fmt);
    outPkg.format = fmt;

    uint32_t count = 0;
    if (!ReadU32(p, end, count)) {
        if (err) *err = "truncated resource count";
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t pathLen = 0;
        if (!ReadU16(p, end, pathLen) || p + pathLen > end) {
            if (err) *err = "truncated resource path";
            return false;
        }
        std::string path((const char*)p, (const char*)p + pathLen);
        p += pathLen;
        path = NormalizePath(path);
        uint32_t dataLen = 0;
        if (!ReadU32(p, end, dataLen) || p + dataLen > end) {
            if (err) *err = "truncated resource data: " + path;
            return false;
        }
        std::vector<uint8_t> blob(p, p + dataLen);
        p += dataLen;
        outPkg.resources[path] = std::move(blob);
    }
    return true;
}

bool WritePackage(const std::string& filepath, const Package& pkg, std::string* err) {
    std::vector<uint8_t> buf;
    if (!WritePackageToMemory(pkg, buf, err)) return false;
    try {
        fs::path p = fs::u8path(filepath);
        if (p.has_parent_path())
            fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        if (!out) {
            if (err) *err = "cannot open for write: " + filepath;
            return false;
        }
        out.write(reinterpret_cast<const char*>(buf.data()), (std::streamsize)buf.size());
        return (bool)out;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

bool ReadPackage(const std::string& filepath, Package& out, std::string* err) {
    try {
        std::ifstream in(fs::u8path(filepath), std::ios::binary);
        if (!in) {
            if (err) *err = "cannot open: " + filepath;
            return false;
        }
        std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
        return ReadPackageFromMemory(buf.data(), buf.size(), out, err);
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

bool ParseManifestFormat(const std::string& manifestJson, PackageFormat& outFmt) {
    outFmt = PackageFormat::Unknown;
    try {
        json j = json::parse(manifestJson);
        if (j.contains("format") && j["format"].is_string())
            outFmt = PackageFormatFromName(j["format"].get<std::string>());
        return outFmt != PackageFormat::Unknown;
    } catch (...) {
        return false;
    }
}

std::string MakeManifest(PackageFormat fmt, const std::string& id, const std::string& displayName,
                         const std::string& kind, int schemaVersion) {
    json j;
    j["format"] = PackageFormatName(fmt);
    j["version"] = schemaVersion;
    j["id"] = id;
    j["displayName"] = displayName;
    j["kind"] = kind;
    return j.dump(2);
}

bool BuildTexturePackage(Package& out, const std::string& id, const std::string& displayName,
                         const std::vector<uint8_t>& pngBytes,
                         const std::vector<uint8_t>& thumb32,
                         const std::vector<uint8_t>& thumb128,
                         int w, int h) {
    out = Package{};
    out.format = PackageFormat::RVPAF;
    json j = json::parse(MakeManifest(PackageFormat::RVPAF, id, displayName, "texture"));
    if (w > 0) j["width"] = w;
    if (h > 0) j["height"] = h;
    j["image"] = paths::kImagePng;
    if (!thumb32.empty()) j["thumbnail"] = paths::kThumbPng;
    if (!thumb128.empty()) j["thumbnail_h"] = paths::kThumbHPng;
    out.manifestJson = j.dump(2);
    if (!pngBytes.empty())
        out.Set(paths::kImagePng, pngBytes);
    if (!thumb32.empty())
        out.Set(paths::kThumbPng, thumb32);
    if (!thumb128.empty())
        out.Set(paths::kThumbHPng, thumb128);
    return !pngBytes.empty();
}

bool BuildBrushPackage(Package& out, const std::string& id, const std::string& displayName,
                       const std::string& configJson,
                       const std::vector<uint8_t>& tipRaw8, int tipSize) {
    out = Package{};
    out.format = PackageFormat::RVPBF;
    json j = json::parse(MakeManifest(PackageFormat::RVPBF, id, displayName, "brush"));
    j["config"] = paths::kConfigJson;
    if (tipSize > 0 && !tipRaw8.empty()) {
        j["tip"] = paths::kTipRaw8;
        j["tipSize"] = tipSize;
        j["tipEncoding"] = "raw8";
    }
    out.manifestJson = j.dump(2);
    out.SetText(paths::kConfigJson, configJson);
    if (tipSize > 0 && !tipRaw8.empty())
        out.Set(paths::kTipRaw8, tipRaw8);
    return true;
}

bool BuildConfigPackage(Package& out, ConfigKind kind, const std::string& id,
                        const std::string& displayName, const std::string& configJson,
                        const std::unordered_map<std::string, std::vector<uint8_t>>& extra) {
    out = Package{};
    out.format = PackageFormat::RVPCF;
    const char* kindStr = "unknown";
    if (kind == ConfigKind::ExportTemplate) kindStr = "export_template";
    else if (kind == ConfigKind::ShaderPreset) kindStr = "shader_preset";
    json j = json::parse(MakeManifest(PackageFormat::RVPCF, id, displayName, kindStr));
    j["config"] = paths::kConfigJson;
    out.manifestJson = j.dump(2);
    out.SetText(paths::kConfigJson, configJson);
    for (const auto& kv : extra)
        out.Set(NormalizePath(kv.first), kv.second);
    return true;
}

PackageFormat FormatFromPath(const std::string& path) {
    std::string ext;
    try {
        ext = fs::path(path).extension().string();
    } catch (...) {
        return PackageFormat::Unknown;
    }
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext == ".rvpcf") return PackageFormat::RVPCF;
    if (ext == ".rvpaf") return PackageFormat::RVPAF;
    if (ext == ".rvpbf") return PackageFormat::RVPBF;
    return PackageFormat::Unknown;
}

} // namespace rvp

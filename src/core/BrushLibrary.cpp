#include "BrushLibrary.h"
#include "Logger.h"
#include "ConfigManager.h"
#include "../package/PackageIO.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <random>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#endif

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// BrushPresetParams
// ---------------------------------------------------------------------------

static std::string DetectBuiltinTipId(const BrushTip* tip) {
    if (!tip) return {};
    if (tip == &BrushPresets::SoftRound()) return "soft_round";
    if (tip == &BrushPresets::HardRound()) return "hard_round";
    if (tip == &BrushPresets::Pencil())    return "pencil";
    if (tip == &BrushPresets::Airbrush())  return "airbrush";
    // Compare by name for safety if pointer identity failed
    if (tip->name) {
        std::string n = tip->name;
        if (n == "Soft Round") return "soft_round";
        if (n == "Hard Round") return "hard_round";
        if (n == "Pencil") return "pencil";
        if (n == "Airbrush") return "airbrush";
    }
    return {};
}

const BrushTip* BuiltinTipById(const std::string& id) {
    if (id == "soft_round") return &BrushPresets::SoftRound();
    if (id == "hard_round") return &BrushPresets::HardRound();
    if (id == "pencil")     return &BrushPresets::Pencil();
    if (id == "airbrush")   return &BrushPresets::Airbrush();
    return nullptr;
}

BrushPresetParams BrushPresetParams::FromSettings(const BrushSettings& s, const BrushTip* tip) {
    BrushPresetParams p;
    p.radius = s.radius;
    p.hardness = s.hardness;
    p.opacity = s.opacity;
    p.spacing = s.spacing;
    p.stabilization = s.stabilization;
    p.erase = s.erase;
    p.writeR = s.writeR; p.writeG = s.writeG; p.writeB = s.writeB; p.writeA = s.writeA;
    p.pressureRadius = s.pressureRadius;
    p.pressureHardness = s.pressureHardness;
    p.pressureOpacity = s.pressureOpacity;
    p.rotationDeg = s.rotationDeg;
    p.pressureRotation = s.pressureRotation;
    p.scatter = s.scatter;
    p.angleJitter = s.angleJitter;
    p.tipSourcePath = s.tipSourcePath;

    const BrushTip* t = tip ? tip : s.tip;
    if (!t || t->size <= 0 || t->pixels.empty()) {
        p.tipType = TipType::None;
        return p;
    }
    std::string bid = DetectBuiltinTipId(t);
    if (!bid.empty()) {
        p.tipType = TipType::Builtin;
        p.tipBuiltinId = bid;
        p.tipSpacingMul = t->spacingMul;
        return p;
    }
    p.tipType = TipType::Embedded;
    p.tipSize = t->size;
    p.tipPixels = t->pixels;
    p.tipSpacingMul = t->spacingMul;
    return p;
}

void BrushPresetParams::ApplyToSettings(BrushSettings& s, const BrushTip* ownedTipPtr) const {
    s.radius = radius;
    s.hardness = hardness;
    s.opacity = opacity;
    s.spacing = spacing;
    s.stabilization = stabilization;
    // Do not force erase — UI tool mode owns brush vs eraser. Still restore stored flag.
    s.erase = erase;
    s.writeR = writeR; s.writeG = writeG; s.writeB = writeB; s.writeA = writeA;
    s.pressureRadius = pressureRadius;
    s.pressureHardness = pressureHardness;
    s.pressureOpacity = pressureOpacity;
    s.rotationDeg = rotationDeg;
    s.pressureRotation = pressureRotation;
    s.scatter = scatter;
    s.angleJitter = angleJitter;
    s.tipSourcePath = tipSourcePath;

    if (tipType == TipType::None) {
        s.tip = nullptr;
    } else if (tipType == TipType::Builtin) {
        s.tip = BuiltinTipById(tipBuiltinId);
    } else {
        s.tip = ownedTipPtr;
    }
}

// ---------------------------------------------------------------------------
// Base64
// ---------------------------------------------------------------------------

static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string BrushLibrary::Base64Encode(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kB64[(n >> 18) & 63]);
        out.push_back(kB64[(n >> 12) & 63]);
        out.push_back(kB64[(n >> 6) & 63]);
        out.push_back(kB64[n & 63]);
        i += 3;
    }
    if (i < data.size()) {
        uint32_t n = data[i] << 16;
        out.push_back(kB64[(n >> 18) & 63]);
        if (i + 1 < data.size()) {
            n |= data[i + 1] << 8;
            out.push_back(kB64[(n >> 12) & 63]);
            out.push_back(kB64[(n >> 6) & 63]);
            out.push_back('=');
        } else {
            out.push_back(kB64[(n >> 12) & 63]);
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

bool BrushLibrary::Base64Decode(const std::string& in, std::vector<uint8_t>& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = val(c);
        if (v < 0) return false;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((buf >> bits) & 0xFF));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Paths / singleton
// ---------------------------------------------------------------------------

BrushLibrary& BrushLibrary::Get() {
    static BrushLibrary inst;
    return inst;
}

std::filesystem::path BrushLibrary::DefaultRootDir() {
#ifdef _WIN32
    PWSTR appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData) {
        std::filesystem::path p = std::filesystem::path(appData) / "RayVPaint" / "brushes";
        CoTaskMemFree(appData);
        return p;
    }
#endif
    // Fallback: next to Documents user dir used by ConfigManager
    return std::filesystem::path(ConfigManager::GetUserDirectory()) / "brushes";
}

void BrushLibrary::SetRootDir(const std::filesystem::path& root) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Root = root;
}

std::filesystem::path BrushLibrary::GetRootDir() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Root;
}

void BrushLibrary::EnsureRootExists() {
    // Caller should hold m_Mutex or call from single-threaded init.
    if (m_Root.empty())
        m_Root = DefaultRootDir();
    std::error_code ec;
    std::filesystem::create_directories(m_Root, ec);
    Logger::Get().Info("BrushLibrary root: " + m_Root.string() +
                       (ec ? (" (create err: " + ec.message() + ")") : " (ok)"));
}

std::string BrushLibrary::NewUuid() {
    // Simple non-crypto uuid-ish: time + random
    static std::mt19937_64 rng{
        (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count()
        ^ (uint64_t)std::random_device{}()
    };
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t a = dist(rng), b = dist(rng);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
        (unsigned)(a >> 32),
        (unsigned)((a >> 16) & 0xFFFF),
        (unsigned)(a & 0xFFFF),
        (unsigned)(b >> 48),
        (unsigned long long)(b & 0xFFFFFFFFFFFFULL));
    return buf;
}

// ---------------------------------------------------------------------------
// Entry helpers
// ---------------------------------------------------------------------------

void BrushLibrary::Entry::RebuildOwnedTip() {
    ownedTip = BrushTip{};
    if (params.tipType == BrushPresetParams::TipType::Embedded &&
        params.tipSize > 0 &&
        (int)params.tipPixels.size() >= params.tipSize * params.tipSize) {
        ownedTip.size = params.tipSize;
        ownedTip.pixels = params.tipPixels;
        ownedTip.spacingMul = params.tipSpacingMul;
        ownedTip.name = "Custom";
    }
}

BrushLibrary::Entry* BrushLibrary::Find(const std::string& id) {
    for (auto& e : m_Entries)
        if (e.meta.id == id) return &e;
    return nullptr;
}

const BrushLibrary::Entry* BrushLibrary::Find(const std::string& id) const {
    for (const auto& e : m_Entries)
        if (e.meta.id == id) return &e;
    return nullptr;
}

void BrushLibrary::RebuildMetaList() {
    m_MetaList.clear();
    m_MetaList.reserve(m_Entries.size());
    // Builtins first, then customs, staging last among customs
    std::vector<const Entry*> order;
    order.reserve(m_Entries.size());
    for (const auto& e : m_Entries)
        if (e.meta.isBuiltin) order.push_back(&e);
    for (const auto& e : m_Entries)
        if (!e.meta.isBuiltin) order.push_back(&e);
    for (const Entry* e : order)
        m_MetaList.push_back(e->meta);
}

// ---------------------------------------------------------------------------
// Builtins
// ---------------------------------------------------------------------------

void BrushLibrary::RegisterBuiltins() {
    auto add = [&](const char* id, const char* name, float radius, float hard, float opac,
                   float spacing, int stab, const char* tipId) {
        Entry e;
        e.meta.id = id;
        e.meta.displayName = name;
        e.meta.isBuiltin = true;
        e.meta.isDirty = false;
        e.params.radius = radius;
        e.params.hardness = hard;
        e.params.opacity = opac;
        e.params.spacing = spacing;
        e.params.stabilization = stab;
        e.params.tipType = BrushPresetParams::TipType::Builtin;
        e.params.tipBuiltinId = tipId;
        if (const BrushTip* t = BuiltinTipById(tipId))
            e.params.tipSpacingMul = t->spacingMul;
        e.RebuildOwnedTip();
        m_Entries.push_back(std::move(e));
    };

    // Clear previous builtins only if reloading full library
    m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
        [](const Entry& e) { return e.meta.isBuiltin; }), m_Entries.end());

    add("builtin.soft_round", "Soft Round", 18.f, 0.35f, 1.f, 0.12f, 1, "soft_round");
    add("builtin.hard_round", "Hard Round", 12.f, 0.95f, 1.f, 0.10f, 1, "hard_round");
    add("builtin.pencil",     "Pencil",     4.f,  0.80f, 0.9f, 0.08f, 2, "pencil");
    add("builtin.airbrush",   "Airbrush",   40.f, 0.05f, 0.35f, 0.15f, 1, "airbrush");
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

static json ParamsToJson(const BrushPresetParams& p) {
    json j;
    j["radius"] = p.radius;
    j["hardness"] = p.hardness;
    j["opacity"] = p.opacity;
    j["spacing"] = p.spacing;
    j["stabilization"] = p.stabilization;
    j["erase"] = p.erase;
    j["writeR"] = p.writeR; j["writeG"] = p.writeG; j["writeB"] = p.writeB; j["writeA"] = p.writeA;
    j["pressureRadius"] = p.pressureRadius;
    j["pressureHardness"] = p.pressureHardness;
    j["pressureOpacity"] = p.pressureOpacity;
    j["rotationDeg"] = p.rotationDeg;
    j["pressureRotation"] = p.pressureRotation;
    j["scatter"] = p.scatter;
    j["angleJitter"] = p.angleJitter;
    if (!p.tipSourcePath.empty()) j["tipSourcePath"] = p.tipSourcePath;
    return j;
}

static void ParamsFromJson(BrushPresetParams& p, const json& j) {
    if (j.contains("radius")) p.radius = j["radius"].get<float>();
    if (j.contains("hardness")) p.hardness = j["hardness"].get<float>();
    if (j.contains("opacity")) p.opacity = j["opacity"].get<float>();
    if (j.contains("spacing")) p.spacing = j["spacing"].get<float>();
    if (j.contains("stabilization")) p.stabilization = j["stabilization"].get<int>();
    if (j.contains("erase")) p.erase = j["erase"].get<bool>();
    if (j.contains("writeR")) p.writeR = j["writeR"].get<bool>();
    if (j.contains("writeG")) p.writeG = j["writeG"].get<bool>();
    if (j.contains("writeB")) p.writeB = j["writeB"].get<bool>();
    if (j.contains("writeA")) p.writeA = j["writeA"].get<bool>();
    if (j.contains("pressureRadius")) p.pressureRadius = j["pressureRadius"].get<bool>();
    if (j.contains("pressureHardness")) p.pressureHardness = j["pressureHardness"].get<bool>();
    if (j.contains("pressureOpacity")) p.pressureOpacity = j["pressureOpacity"].get<bool>();
    if (j.contains("rotationDeg")) p.rotationDeg = j["rotationDeg"].get<float>();
    if (j.contains("pressureRotation")) p.pressureRotation = j["pressureRotation"].get<bool>();
    if (j.contains("scatter")) p.scatter = j["scatter"].get<float>();
    if (j.contains("angleJitter")) p.angleJitter = j["angleJitter"].get<float>();
    if (j.contains("tipSourcePath")) p.tipSourcePath = j["tipSourcePath"].get<std::string>();
}

bool BrushLibrary::WriteFile(const Entry& e) const {
    if (e.meta.isBuiltin) return false;
    if (m_Root.empty()) return false;

    json cfg;
    cfg["id"] = e.meta.id;
    cfg["name"] = e.meta.displayName;
    cfg["params"] = ParamsToJson(e.params);
    json tip;
    std::vector<uint8_t> tipRaw;
    int tipSize = 0;
    if (e.params.tipType == BrushPresetParams::TipType::None) {
        tip["type"] = "none";
    } else if (e.params.tipType == BrushPresetParams::TipType::Builtin) {
        tip["type"] = "builtin";
        tip["builtin_id"] = e.params.tipBuiltinId;
        tip["spacing_mul"] = e.params.tipSpacingMul;
    } else {
        tip["type"] = "embedded";
        tip["size"] = e.params.tipSize;
        tip["encoding"] = "raw8";
        tip["spacing_mul"] = e.params.tipSpacingMul;
        tipRaw = e.params.tipPixels;
        tipSize = e.params.tipSize;
    }
    cfg["tip"] = tip;

    rvp::Package pkg;
    if (!rvp::BuildBrushPackage(pkg, e.meta.id, e.meta.displayName, cfg.dump(2), tipRaw, tipSize)) {
        Logger::Get().Error("BrushLibrary: BuildBrushPackage failed for " + e.meta.id);
        return false;
    }
    std::filesystem::path path = m_Root / (e.meta.id + ".rvpbf");
    std::string err;
    if (!rvp::WritePackage(path.string(), pkg, &err)) {
        Logger::Get().Error("BrushLibrary: cannot write " + path.string() + " (" + err + ")");
        return false;
    }
    Logger::Get().Info("BrushLibrary: saved " + path.string());
    return true;
}

bool BrushLibrary::LoadFileUnlocked(const std::filesystem::path& path) {
    rvp::Package pkg;
    std::string err;
    if (!rvp::ReadPackage(path.string(), pkg, &err) || pkg.format != rvp::PackageFormat::RVPBF) {
        Logger::Get().Warn("BrushLibrary: not a valid RVPBF: " + path.string() + " (" + err + ")");
        return false;
    }
    std::string cfg = pkg.GetText(rvp::paths::kConfigJson);
    if (cfg.empty()) {
        Logger::Get().Warn("BrushLibrary: RVPBF missing config.json " + path.string());
        return false;
    }
    json root;
    try {
        root = json::parse(cfg);
    } catch (...) {
        Logger::Get().Warn("BrushLibrary: invalid config.json " + path.string());
        return false;
    }

    Entry e;
    e.meta.id = root.value("id", path.stem().string());
    e.meta.displayName = root.value("name", e.meta.id);
    e.meta.isBuiltin = false;
    e.meta.isDirty = false;
    e.meta.sourcePath = path.string();

    if (e.meta.id.rfind("builtin.", 0) == 0) {
        Logger::Get().Warn("BrushLibrary: skip custom with builtin id " + e.meta.id);
        return false;
    }

    if (root.contains("params"))
        ParamsFromJson(e.params, root["params"]);

    if (root.contains("tip")) {
        const json& tip = root["tip"];
        std::string type = tip.value("type", "none");
        if (type == "builtin") {
            e.params.tipType = BrushPresetParams::TipType::Builtin;
            e.params.tipBuiltinId = tip.value("builtin_id", "soft_round");
            e.params.tipSpacingMul = tip.value("spacing_mul", 1.f);
        } else if (type == "embedded") {
            e.params.tipType = BrushPresetParams::TipType::Embedded;
            e.params.tipSize = tip.value("size", 0);
            e.params.tipSpacingMul = tip.value("spacing_mul", 1.f);
            if (const auto* tipBytes = pkg.Get(rvp::paths::kTipRaw8)) {
                e.params.tipPixels = *tipBytes;
                if (e.params.tipSize <= 0) {
                    // Derive size from raw8 square
                    int n = (int)tipBytes->size();
                    int s = (int)std::lround(std::sqrt((double)n));
                    if (s * s == n) e.params.tipSize = s;
                }
            } else {
                e.params.tipType = BrushPresetParams::TipType::None;
            }
        } else {
            e.params.tipType = BrushPresetParams::TipType::None;
        }
    }
    e.RebuildOwnedTip();

    if (Entry* old = Find(e.meta.id)) {
        *old = std::move(e);
    } else {
        m_Entries.push_back(std::move(e));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void BrushLibrary::LoadBuiltins() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Root.empty())
        m_Root = DefaultRootDir();
    // Do not scan disk — only builtins for instant startup
    std::vector<Entry> staging;
    for (auto& e : m_Entries) {
        if (e.meta.isDirty && !e.meta.isBuiltin)
            staging.push_back(std::move(e));
    }
    m_Entries.clear();
    RegisterBuiltins();
    for (auto& s : staging)
        m_Entries.push_back(std::move(s));
    RebuildMetaList();
    m_Loaded = true;
    Logger::Get().Info("BrushLibrary: builtins ready (" + std::to_string(m_MetaList.size()) +
                       "), disk scan deferred");
}

void BrushLibrary::StartAsyncDiskLoad() {
    if (m_DiskLoadRunning.exchange(true)) return;

    std::filesystem::path root;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Root.empty())
            m_Root = DefaultRootDir();
        EnsureRootExists();
        root = m_Root;
    }

    std::thread([this, root]() {
        std::vector<Entry> loaded;
        std::error_code ec;
        if (std::filesystem::exists(root, ec)) {
            for (auto& ent : std::filesystem::directory_iterator(root, ec)) {
                if (!ent.is_regular_file()) continue;
                auto ext = ent.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext != ".rvpbf") continue;

                rvp::Package pkg;
                if (!rvp::ReadPackage(ent.path().string(), pkg, nullptr) ||
                    pkg.format != rvp::PackageFormat::RVPBF)
                    continue;
                std::string cfg = pkg.GetText(rvp::paths::kConfigJson);
                if (cfg.empty()) continue;
                json rootJ;
                try { rootJ = json::parse(cfg); } catch (...) { continue; }

                Entry e;
                e.meta.id = rootJ.value("id", ent.path().stem().string());
                e.meta.displayName = rootJ.value("name", e.meta.id);
                e.meta.isBuiltin = false;
                e.meta.isDirty = false;
                e.meta.sourcePath = ent.path().string();
                if (e.meta.id.rfind("builtin.", 0) == 0) continue;
                auto& p = e.params;
                if (rootJ.contains("params")) {
                    const json& j = rootJ["params"];
                    if (j.contains("radius")) p.radius = j["radius"].get<float>();
                    if (j.contains("hardness")) p.hardness = j["hardness"].get<float>();
                    if (j.contains("opacity")) p.opacity = j["opacity"].get<float>();
                    if (j.contains("spacing")) p.spacing = j["spacing"].get<float>();
                    if (j.contains("stabilization")) p.stabilization = j["stabilization"].get<int>();
                    if (j.contains("erase")) p.erase = j["erase"].get<bool>();
                    if (j.contains("writeR")) p.writeR = j["writeR"].get<bool>();
                    if (j.contains("writeG")) p.writeG = j["writeG"].get<bool>();
                    if (j.contains("writeB")) p.writeB = j["writeB"].get<bool>();
                    if (j.contains("writeA")) p.writeA = j["writeA"].get<bool>();
                    if (j.contains("pressureRadius")) p.pressureRadius = j["pressureRadius"].get<bool>();
                    if (j.contains("pressureHardness")) p.pressureHardness = j["pressureHardness"].get<bool>();
                    if (j.contains("pressureOpacity")) p.pressureOpacity = j["pressureOpacity"].get<bool>();
                    if (j.contains("rotationDeg")) p.rotationDeg = j["rotationDeg"].get<float>();
                    if (j.contains("pressureRotation")) p.pressureRotation = j["pressureRotation"].get<bool>();
                    if (j.contains("scatter")) p.scatter = j["scatter"].get<float>();
                    if (j.contains("angleJitter")) p.angleJitter = j["angleJitter"].get<float>();
                    if (j.contains("tipSourcePath")) p.tipSourcePath = j["tipSourcePath"].get<std::string>();
                }
                if (rootJ.contains("tip")) {
                    const json& tip = rootJ["tip"];
                    std::string type = tip.value("type", "none");
                    if (type == "builtin") {
                        p.tipType = BrushPresetParams::TipType::Builtin;
                        p.tipBuiltinId = tip.value("builtin_id", "soft_round");
                        p.tipSpacingMul = tip.value("spacing_mul", 1.f);
                    } else if (type == "embedded") {
                        p.tipType = BrushPresetParams::TipType::Embedded;
                        p.tipSize = tip.value("size", 0);
                        p.tipSpacingMul = tip.value("spacing_mul", 1.f);
                        if (const auto* tipBytes = pkg.Get(rvp::paths::kTipRaw8)) {
                            p.tipPixels = *tipBytes;
                            if (p.tipSize <= 0) {
                                int n = (int)tipBytes->size();
                                int s = (int)std::lround(std::sqrt((double)n));
                                if (s * s == n) p.tipSize = s;
                            }
                        } else {
                            p.tipType = BrushPresetParams::TipType::None;
                        }
                    } else {
                        p.tipType = BrushPresetParams::TipType::None;
                    }
                }
                e.RebuildOwnedTip();
                loaded.push_back(std::move(e));
            }
        }
        {
            std::lock_guard<std::mutex> plock(m_PendingMutex);
            m_PendingDiskEntries = std::move(loaded);
            m_PendingReady.store(true);
        }
        m_DiskLoadRunning.store(false);
        Logger::Get().Info("BrushLibrary: async disk scan finished");
    }).detach();
}

void BrushLibrary::PollAsyncDiskLoad() {
    if (!m_PendingReady.load()) return;
    std::vector<Entry> pending;
    {
        std::lock_guard<std::mutex> plock(m_PendingMutex);
        if (!m_PendingReady.load()) return;
        pending = std::move(m_PendingDiskEntries);
        m_PendingReady.store(false);
    }
    std::lock_guard<std::mutex> lock(m_Mutex);
    int added = 0;
    for (auto& e : pending) {
        if (Entry* old = Find(e.meta.id)) {
            // Don't overwrite dirty staging
            if (!old->meta.isDirty) {
                *old = std::move(e);
                ++added;
            }
        } else {
            m_Entries.push_back(std::move(e));
            ++added;
        }
    }
    RebuildMetaList();
    Logger::Get().Info("BrushLibrary: merged " + std::to_string(added) +
                       " disk presets, total=" + std::to_string(m_MetaList.size()));
}

void BrushLibrary::LoadAll() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    EnsureRootExists();
    std::vector<Entry> staging;
    for (auto& e : m_Entries) {
        if (e.meta.isDirty && !e.meta.isBuiltin)
            staging.push_back(std::move(e));
    }
    m_Entries.clear();
    RegisterBuiltins();

    std::error_code ec;
    if (std::filesystem::exists(m_Root, ec)) {
        for (auto& ent : std::filesystem::directory_iterator(m_Root, ec)) {
            if (!ent.is_regular_file()) continue;
            auto ext = ent.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".rvpbf")
                LoadFileUnlocked(ent.path());
        }
    }

    for (auto& s : staging) {
        if (!Find(s.meta.id))
            m_Entries.push_back(std::move(s));
    }

    RebuildMetaList();
    m_Loaded = true;
    Logger::Get().Info("BrushLibrary: loaded " + std::to_string(m_MetaList.size()) + " presets (sync)");
}

std::vector<BrushPresetMeta> BrushLibrary::List() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_MetaList;
}

bool BrushLibrary::Get(const std::string& id, BrushPresetParams& outParams) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    const Entry* e = Find(id);
    if (!e) return false;
    outParams = e->params;
    return true;
}

bool BrushLibrary::GetMeta(const std::string& id, BrushPresetMeta& out) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    const Entry* e = Find(id);
    if (!e) return false;
    out = e->meta;
    return true;
}

bool BrushLibrary::ApplyTo(const std::string& id, BrushSettings& brush) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    Entry* e = Find(id);
    if (!e) return false;
    e->RebuildOwnedTip();
    const BrushTip* tipPtr = nullptr;
    if (e->params.tipType == BrushPresetParams::TipType::Embedded)
        tipPtr = &e->ownedTip;
    e->params.ApplyToSettings(brush, tipPtr);
    m_ActiveId = id;
    return true;
}

void BrushLibrary::SetActiveId(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ActiveId = id;
}

std::string BrushLibrary::GetActiveId() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_ActiveId;
}

std::string BrushLibrary::CreateFromCurrent(const BrushSettings& brush, const std::string& name) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Loaded) {
        // builtins only — don't block on disk
        RegisterBuiltins();
        m_Loaded = true;
    }
    Entry e;
    e.meta.id = NewUuid();
    e.meta.displayName = name.empty() ? "New Brush" : name;
    e.meta.isBuiltin = false;
    e.meta.isDirty = true;
    e.params = BrushPresetParams::FromSettings(brush, brush.tip);
    e.RebuildOwnedTip();
    std::string id = e.meta.id;
    m_Entries.push_back(std::move(e));
    RebuildMetaList();
    m_ActiveId = id;
    Logger::Get().Info("BrushLibrary: staged brush '" + name + "' id=" + id);
    return id;
}

bool BrushLibrary::UpdateStaging(const std::string& id, const BrushSettings& brush) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    Entry* e = Find(id);
    if (!e || e->meta.isBuiltin) return false;
    e->params = BrushPresetParams::FromSettings(brush, brush.tip);
    e->RebuildOwnedTip();
    e->meta.isDirty = true;
    RebuildMetaList();
    return true;
}

bool BrushLibrary::Rename(const std::string& id, const std::string& newDisplayName) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    Entry* e = Find(id);
    if (!e || e->meta.isBuiltin) return false;
    e->meta.displayName = newDisplayName;
    e->meta.isDirty = true;
    RebuildMetaList();
    return true;
}

bool BrushLibrary::SaveToDisk(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    Entry* e = Find(id);
    if (!e || e->meta.isBuiltin) return false;
    EnsureRootExists();
    if (!WriteFile(*e)) return false;
    e->meta.isDirty = false;
    e->meta.sourcePath = (m_Root / (e->meta.id + ".rvpbf")).string();
    RebuildMetaList();
    return true;
}

bool BrushLibrary::DiscardStaging(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    Entry* e = Find(id);
    if (!e || e->meta.isBuiltin) return false;
    if (!e->meta.isDirty && !e->meta.sourcePath.empty()) {
        return LoadFileUnlocked(e->meta.sourcePath);
    }
    if (e->meta.sourcePath.empty()) {
        m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
            [&](const Entry& x) { return x.meta.id == id; }), m_Entries.end());
        if (m_ActiveId == id) m_ActiveId.clear();
        RebuildMetaList();
        return true;
    }
    std::string path = e->meta.sourcePath;
    m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
        [&](const Entry& x) { return x.meta.id == id; }), m_Entries.end());
    bool ok = LoadFileUnlocked(path);
    RebuildMetaList();
    return ok;
}

bool BrushLibrary::DeleteCustom(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    Entry* e = Find(id);
    if (!e) return false;
    if (e->meta.isBuiltin) {
        Logger::Get().Warn("BrushLibrary: refuse DeleteCustom on builtin " + id);
        return false;
    }
    if (!e->meta.sourcePath.empty()) {
        std::error_code ec;
        std::filesystem::remove(e->meta.sourcePath, ec);
    } else {
        std::error_code ec;
        std::filesystem::remove(m_Root / (id + ".rvpbf"), ec);
    }
    m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
        [&](const Entry& x) { return x.meta.id == id; }), m_Entries.end());
    if (m_ActiveId == id) m_ActiveId.clear();
    RebuildMetaList();
    Logger::Get().Info("BrushLibrary: deleted custom " + id);
    return true;
}

// ---------------------------------------------------------------------------
// Smoke test
// ---------------------------------------------------------------------------

bool BrushLibrary::RunSmokeTest() {
    Logger::Get().Info("BrushLibrary::RunSmokeTest begin");
    auto& lib = Get();
    // Use a temp subfolder under default root so we don't pollute user brushes permanently
    auto testRoot = DefaultRootDir() / "_smoke_test";
    std::error_code ec;
    std::filesystem::remove_all(testRoot, ec);
    lib.SetRootDir(testRoot);
    lib.LoadAll();

    size_t n0 = lib.List().size();
    size_t builtins = 0;
    for (const auto& m : lib.List())
        if (m.isBuiltin) ++builtins;
    if (builtins < 4) {
        Logger::Get().Error("Smoke: expected >=4 builtins");
        return false;
    }

    BrushSettings b;
    b.radius = 33.f;
    b.hardness = 0.42f;
    b.opacity = 0.77f;
    b.spacing = 0.2f;
    b.stabilization = 3;
    b.pressureRadius = true;
    b.tip = &BrushPresets::Pencil();

    std::string id = lib.CreateFromCurrent(b, "Smoke Brush");
    if (id.empty()) return false;
    BrushPresetMeta meta;
    if (!lib.GetMeta(id, meta) || !meta.isDirty || meta.isBuiltin) {
        Logger::Get().Error("Smoke: staging meta wrong");
        return false;
    }
    if (!lib.SaveToDisk(id)) {
        Logger::Get().Error("Smoke: SaveToDisk failed");
        return false;
    }
    if (!lib.GetMeta(id, meta) || meta.isDirty) {
        Logger::Get().Error("Smoke: isDirty not cleared");
        return false;
    }

    // Builtin delete must fail
    if (lib.DeleteCustom("builtin.soft_round")) {
        Logger::Get().Error("Smoke: DeleteCustom builtin should fail");
        return false;
    }

    // Reload as fresh library state
    BrushSettings applied;
    lib.LoadAll();
    if (!lib.ApplyTo(id, applied)) {
        Logger::Get().Error("Smoke: ApplyTo after reload failed");
        return false;
    }
    if (std::abs(applied.radius - 33.f) > 0.01f ||
        std::abs(applied.hardness - 0.42f) > 0.01f ||
        std::abs(applied.opacity - 0.77f) > 0.01f ||
        applied.stabilization != 3 ||
        !applied.pressureRadius) {
        Logger::Get().Error("Smoke: params mismatch after reload");
        return false;
    }
    if (applied.tip != &BrushPresets::Pencil()) {
        Logger::Get().Error("Smoke: tip not pencil after reload");
        return false;
    }

    if (!lib.DeleteCustom(id)) {
        Logger::Get().Error("Smoke: DeleteCustom failed");
        return false;
    }
    lib.LoadAll();
    BrushPresetMeta gone;
    if (lib.GetMeta(id, gone)) {
        Logger::Get().Error("Smoke: deleted id still present");
        return false;
    }

    std::filesystem::remove_all(testRoot, ec);
    // Restore normal root for app
    lib.SetRootDir(DefaultRootDir());
    lib.LoadAll();

    Logger::Get().Info("BrushLibrary::RunSmokeTest OK (builtins=" + std::to_string(builtins) +
                       " list0=" + std::to_string(n0) + ")");
    return true;
}

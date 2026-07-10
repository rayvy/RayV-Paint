#include "ModIniParser.h"
#include "../core/PathUtil.h"
#include "../core/Logger.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace modio {
namespace {

std::string Trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Strip comments but keep full line comment content if line is only comment
bool IsCommentLine(const std::string& line) {
    std::string t = Trim(line);
    return !t.empty() && t[0] == ';';
}

std::string StripInlineComment(const std::string& line) {
    // 3DMigoto: ; starts comment. Don't strip if inside quotes (rare).
    bool inQuote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') inQuote = !inQuote;
        if (!inQuote && c == ';')
            return Trim(line.substr(0, i));
    }
    return Trim(line);
}

std::string CommentText(const std::string& line) {
    std::string t = Trim(line);
    if (t.empty() || t[0] != ';') return {};
    t = t.substr(1);
    return Trim(t);
}

struct RawSection {
    std::string name;
    std::vector<std::pair<std::string, std::string>> keys; // order preserved
    std::vector<std::string> rawLines; // original lines for draw/comment association
};

std::vector<RawSection> SplitSections(const std::string& text) {
    std::vector<RawSection> sections;
    RawSection* cur = nullptr;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = Trim(line);
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            sections.push_back({});
            cur = &sections.back();
            cur->name = trimmed.substr(1, trimmed.size() - 2);
            continue;
        }
        if (!cur) continue;
        cur->rawLines.push_back(line);
        if (IsCommentLine(line) || trimmed.empty())
            continue;
        std::string body = StripInlineComment(line);
        if (body.empty()) continue;
        size_t eq = body.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(body.substr(0, eq));
        std::string val = Trim(body.substr(eq + 1));
        cur->keys.emplace_back(std::move(key), std::move(val));
    }
    return sections;
}

std::string JoinDir(const std::string& dir, const std::string& rel) {
    if (rel.empty()) return {};
    // Absolute path already?
#ifdef _WIN32
    if (rel.size() >= 2 && std::isalpha((unsigned char)rel[0]) && rel[1] == ':')
        return PathUtil::NormalizeToUtf8Path(rel);
    if (rel.size() >= 2 && (rel[0] == '\\' || rel[0] == '/'))
        return PathUtil::NormalizeToUtf8Path(rel);
#else
    if (!rel.empty() && rel[0] == '/') return rel;
#endif
    std::string d = dir;
    if (!d.empty() && d.back() != '\\' && d.back() != '/')
        d.push_back('/');
    return PathUtil::NormalizeToUtf8Path(d + rel);
}

bool FileExistsUtf8(const std::string& path) {
    if (path.empty()) return false;
    return PathUtil::Exists(path);
}

// Extract component base from section names like TextureOverrideBelleBodyA → BelleBody
// TextureOverrideBelleHairBlend → BelleHair
std::string ComponentKeyFromOverride(const std::string& section) {
    std::string s = section;
    const std::string prefix = "TextureOverride";
    if (s.size() > prefix.size() && ToLower(s.substr(0, prefix.size())) == ToLower(prefix))
        s = s.substr(prefix.size());

    // Strip known suffixes
    static const char* kSuffixes[] = {
        "VertexLimitRaise", "Blend", "Texcoord", "Position", "IB",
        "NormalMap", "Diffuse", "LightMap", "MaterialMap"
    };
    std::string lower = ToLower(s);
    for (const char* suf : kSuffixes) {
        std::string sl = ToLower(suf);
        if (lower.size() > sl.size() && lower.compare(lower.size() - sl.size(), sl.size(), sl) == 0) {
            s = s.substr(0, s.size() - sl.size());
            lower = ToLower(s);
            break;
        }
    }
    // Trailing single letter part marker: BodyA → Body, LegsA → Legs, HairA → Hair
    // But keep if whole name is one letter. Also BodyB, FaceA, etc.
    if (s.size() >= 2) {
        char last = s.back();
        if ((last >= 'A' && last <= 'Z') || (last >= 'a' && last <= 'z')) {
            // Only strip if preceding char is letter (BodyA) not digit only
            char prev = s[s.size() - 2];
            if (std::isalpha((unsigned char)prev)) {
                // Don't strip if name ends with common words ending in letter... 
                // XXMI parts almost always single trailing A/B/C
                if (last == 'A' || last == 'B' || last == 'C' || last == 'D' ||
                    last == 'a' || last == 'b' || last == 'c' || last == 'd') {
                    s.pop_back();
                }
            }
        }
    }
    return s;
}

std::string PartNameFromSection(const std::string& section, const std::string& componentKey) {
    std::string s = section;
    const std::string prefix = "TextureOverride";
    if (s.size() > prefix.size() && ToLower(s.substr(0, prefix.size())) == ToLower(prefix))
        s = s.substr(prefix.size());
    // If starts with component key, remainder is part name
    if (!componentKey.empty() && s.size() >= componentKey.size() &&
        ToLower(s.substr(0, componentKey.size())) == ToLower(componentKey)) {
        std::string rem = s.substr(componentKey.size());
        if (rem.empty()) return "Main";
        return rem;
    }
    return s.empty() ? "Part" : s;
}

// Parse "ref ResourceX" or "ResourceX"
std::string ParseResourceRef(const std::string& val) {
    std::string v = Trim(val);
    std::string low = ToLower(v);
    if (low.rfind("ref ", 0) == 0)
        v = Trim(v.substr(4));
    return v;
}

// drawindexed = count, start, base
bool ParseDrawIndexed(const std::string& val, DrawIndexed& out) {
    // split by comma
    std::vector<int> nums;
    std::string cur;
    for (char c : val) {
        if (c == ',') {
            std::string t = Trim(cur);
            if (!t.empty()) nums.push_back(std::atoi(t.c_str()));
            cur.clear();
        } else cur.push_back(c);
    }
    std::string t = Trim(cur);
    if (!t.empty()) nums.push_back(std::atoi(t.c_str()));
    if (nums.empty()) return false;
    out.indexCount = nums[0];
    out.indexStart = nums.size() > 1 ? nums[1] : 0;
    out.baseVertex = nums.size() > 2 ? nums[2] : 0;
    return out.indexCount > 0;
}

// draw = vertexCount, 0  (used on blend/position root sections)
bool ParseDrawVertexCount(const std::string& val, int& outCount) {
    size_t comma = val.find(',');
    std::string first = Trim(comma == std::string::npos ? val : val.substr(0, comma));
    outCount = std::atoi(first.c_str());
    return outCount > 0;
}

// ps-t12 → 12
int ParsePsSlotKey(const std::string& key) {
    std::string k = ToLower(key);
    // allow "ps-t0" or "post ps-t0" handled by caller skipping post
    if (k.rfind("ps-t", 0) != 0) return -1;
    std::string num = k.substr(4);
    if (num.empty() || !std::isdigit((unsigned char)num[0])) return -1;
    return std::atoi(num.c_str());
}

// Resource\ZZMI\Diffuse or Resource\GI\NormalMap
bool ParseApiAssignKey(const std::string& key, std::string& outToken) {
    std::string k = key;
    // Normalize slashes
    for (char& c : k) if (c == '/') c = '\\';
    std::string low = ToLower(k);
    if (low.rfind("resource\\", 0) != 0) return false;
    // Resource\ZZMI\Diffuse → take last segment
    size_t last = k.find_last_of('\\');
    if (last == std::string::npos || last + 1 >= k.size()) return false;
    outToken = k.substr(last + 1);
    // Ignore non-texture API helpers
    std::string tl = ToLower(outToken);
    if (tl == "settextures" || tl.find("command") != std::string::npos)
        return false;
    return true;
}

TextureBind MakeBindFromApi(const std::string& apiToken, const std::string& resourceName,
                            const std::unordered_map<std::string, ModResource>& resMap) {
    TextureBind b;
    b.source = BindSource::ApiNamed;
    b.apiName = apiToken;
    b.slot = MaterialSlotFromApiToken(apiToken);
    if (b.slot == MaterialSlot::Unknown)
        b.slot = MaterialSlotFromResourceName(resourceName);
    b.resourceName = resourceName;
    auto it = resMap.find(ToLower(resourceName));
    if (it != resMap.end()) {
        b.absolutePath = it->second.absolutePath;
        b.exists = it->second.exists;
        if (b.slot == MaterialSlot::Unknown)
            b.slot = MaterialSlotFromResourceName(it->second.filename.empty()
                                                     ? resourceName
                                                     : it->second.filename);
    }
    return b;
}

TextureBind MakeBindFromPs(int slot, const std::string& resourceName,
                           const std::unordered_map<std::string, ModResource>& resMap) {
    TextureBind b;
    b.source = BindSource::PsSlot;
    b.psSlot = slot;
    b.resourceName = resourceName;
    if (const SpecialSlotHint* sp = FindSpecialSlot(slot)) {
        b.slot = sp->role;
    } else {
        // GI convention: t0 Normal, t1 Diffuse, t2 LightMap, t3 MaterialMap (common)
        if (slot == 0) b.slot = MaterialSlot::NormalMap;
        else if (slot == 1) b.slot = MaterialSlot::Diffuse;
        else if (slot == 2) b.slot = MaterialSlot::LightMap;
        else if (slot == 3) b.slot = MaterialSlot::MaterialMap;
        else b.slot = MaterialSlotFromResourceName(resourceName);
    }
    // Resource name wins over slot default when clear
    MaterialSlot fromName = MaterialSlotFromResourceName(resourceName);
    if (fromName != MaterialSlot::Unknown)
        b.slot = fromName;

    auto it = resMap.find(ToLower(resourceName));
    if (it != resMap.end()) {
        b.absolutePath = it->second.absolutePath;
        b.exists = it->second.exists;
    }
    return b;
}

// Slot-keyed texture state (last bind wins per slot)
using TexState = std::unordered_map<int, TextureBind>; // key = (int)MaterialSlot or 1000+ps

static std::vector<TextureBind> TexStateToList(const TexState& st) {
    std::vector<TextureBind> out;
    out.reserve(st.size());
    for (const auto& kv : st) out.push_back(kv.second);
    return out;
}

static int BindKey(const TextureBind& b) {
    if (b.slot != MaterialSlot::Unknown && b.slot != MaterialSlot::Custom)
        return (int)b.slot;
    if (b.psSlot >= 0) return 1000 + b.psSlot;
    return 2000 + (int)std::hash<std::string>{}(b.resourceName) % 500;
}

// Apply one bind line into state
static void ApplyBindLine(const std::string& key, const std::string& val,
                          const std::unordered_map<std::string, ModResource>& resMap,
                          TexState& state) {
    std::string keyLow = ToLower(key);
    if (keyLow.rfind("post ", 0) == 0) return;

    std::string apiToken;
    if (ParseApiAssignKey(key, apiToken)) {
        std::string res = ParseResourceRef(val);
        if (res.empty() || ToLower(res) == "null") return;
        TextureBind b = MakeBindFromApi(apiToken, res, resMap);
        state[BindKey(b)] = b;
        return;
    }
    int ps = ParsePsSlotKey(keyLow);
    if (ps >= 0) {
        std::string res = ParseResourceRef(val);
        if (res.empty() || ToLower(res) == "null") return;
        TextureBind b = MakeBindFromPs(ps, res, resMap);
        state[BindKey(b)] = b;
    }
}

// Sequential multi-texture parse: texture binds + drawindexed + run=CommandList*
// Dedup drawindexed within the part by (count, start).
struct BatchParseCtx {
    const std::unordered_map<std::string, const RawSection*>* secByName = nullptr;
    const std::unordered_map<std::string, ModResource>* resMap = nullptr;
    ModPart* part = nullptr;
    TexState state;
    bool stateDirty = true;
    std::string pendingComment;
    std::unordered_set<uint64_t> seenDraws; // dedup key
    int recursion = 0;
};

static uint64_t DrawKey(int count, int start) {
    return (uint64_t)(uint32_t)count << 32 | (uint32_t)start;
}

static void FlushBatchIfNeeded(BatchParseCtx& ctx) {
    if (!ctx.stateDirty && !ctx.part->batches.empty()) return;
    DrawBatch batch;
    batch.textures = TexStateToList(ctx.state);
    batch.name = ctx.pendingComment.empty()
        ? ("batch" + std::to_string(ctx.part->batches.size()))
        : ctx.pendingComment;
    ctx.part->batches.push_back(std::move(batch));
    ctx.stateDirty = false;
}

static void ParseSectionLinesSequential(const RawSection& sec, BatchParseCtx& ctx);

static void HandleRunCommand(const std::string& val, BatchParseCtx& ctx) {
    // run = CommandListBody  or  CommandList\ZZMI\SetTextures
    std::string name = Trim(val);
    if (name.empty()) return;
    // Engine helpers without local section — ignore quietly
    if (!ctx.secByName) return;
    auto it = ctx.secByName->find(ToLower(name));
    if (it == ctx.secByName->end()) {
        // try without CommandList prefix variants
        return;
    }
    if (ctx.recursion > 8) return;
    ++ctx.recursion;
    ParseSectionLinesSequential(*it->second, ctx);
    --ctx.recursion;
}

static void ParseSectionLinesSequential(const RawSection& sec, BatchParseCtx& ctx) {
    for (const std::string& raw : sec.rawLines) {
        if (IsCommentLine(raw)) {
            std::string c = CommentText(raw);
            if (!c.empty()) ctx.pendingComment = c;
            continue;
        }
        std::string body = StripInlineComment(raw);
        if (body.empty()) continue;
        size_t eq = body.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(body.substr(0, eq));
        std::string val = Trim(body.substr(eq + 1));
        std::string keyLow = ToLower(key);

        if (keyLow == "drawindexed") {
            DrawIndexed d;
            if (!ParseDrawIndexed(val, d)) continue;
            // Dedup within part
            uint64_t dk = DrawKey(d.indexCount, d.indexStart);
            if (ctx.seenDraws.count(dk)) continue;
            ctx.seenDraws.insert(dk);

            d.commentLabel = ctx.pendingComment;
            FlushBatchIfNeeded(ctx);
            if (ctx.part->batches.empty()) {
                DrawBatch b;
                b.textures = TexStateToList(ctx.state);
                b.name = ctx.pendingComment.empty() ? "batch0" : ctx.pendingComment;
                ctx.part->batches.push_back(std::move(b));
                ctx.stateDirty = false;
            }
            // If comment names the mesh, rename current batch when it has no draws yet
            if (!ctx.pendingComment.empty() && ctx.part->batches.back().draws.empty())
                ctx.part->batches.back().name = ctx.pendingComment;
            ctx.part->batches.back().draws.push_back(d);
            ctx.pendingComment.clear();
            continue;
        }

        if (keyLow == "run") {
            // Only expand CommandList / CustomShader that may contain draws or binds
            std::string vlow = ToLower(val);
            if (vlow.find("commandlist") != std::string::npos ||
                vlow.find("customshader") != std::string::npos) {
                HandleRunCommand(val, ctx);
            }
            continue;
        }

        // Texture / API binds change material state → next draw starts new batch
        std::string apiTok;
        bool isBind = ParseApiAssignKey(key, apiTok) || ParsePsSlotKey(keyLow) >= 0;
        if (isBind) {
            ApplyBindLine(key, val, *ctx.resMap, ctx.state);
            ctx.stateDirty = true;
        }
    }
}

// Build batches for a TextureOverride part section (+ nested CommandLists)
static void ParsePartBatches(const RawSection& sec,
                             const std::unordered_map<std::string, const RawSection*>& secByName,
                             const std::unordered_map<std::string, ModResource>& resMap,
                             ModPart& part) {
    BatchParseCtx ctx;
    ctx.secByName = &secByName;
    ctx.resMap = &resMap;
    ctx.part = &part;
    ParseSectionLinesSequential(sec, ctx);
    // Drop empty batches
    part.batches.erase(
        std::remove_if(part.batches.begin(), part.batches.end(),
                       [](const DrawBatch& b) { return b.draws.empty(); }),
        part.batches.end());
}

} // namespace

ModScene ModIniParser::ParseFile(const std::string& iniPath) {
    const std::string path = PathUtil::NormalizeToUtf8Path(iniPath);
    std::string text;
    {
#ifdef _WIN32
        std::ifstream in(PathUtil::FromUtf8(path), std::ios::binary);
#else
        std::ifstream in(path, std::ios::binary);
#endif
        if (!in) {
            ModScene s;
            s.iniPath = path;
            s.ok = false;
            s.error = "Cannot open ini: " + path;
            return s;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }
    std::string dir;
    try {
        auto p = PathUtil::FromUtf8(path);
        dir = PathUtil::WideToUtf8(p.parent_path().wstring());
    } catch (...) {
        size_t slash = path.find_last_of("/\\");
        dir = (slash == std::string::npos) ? std::string() : path.substr(0, slash);
    }
    return ParseText(text, path, dir);
}

ModScene ModIniParser::ParseText(const std::string& iniText, const std::string& iniPathForMeta,
                                 const std::string& iniDirectory) {
    ModScene scene;
    scene.iniPath = iniPathForMeta;
    scene.iniDirectory = iniDirectory;

    auto sections = SplitSections(iniText);
    if (sections.empty()) {
        scene.ok = false;
        scene.error = "No sections found in ini";
        return scene;
    }

    std::unordered_map<std::string, ModResource> resMap; // lower(section) → resource
    std::unordered_map<std::string, const RawSection*> secByName;

    // Pass 1: Resources
    for (const auto& sec : sections) {
        secByName[ToLower(sec.name)] = &sec;
        if (ToLower(sec.name).rfind("resource", 0) != 0)
            continue;
        // Skip nested Resource\ paths that aren't real sections — sections are [ResourceName]
        if (sec.name.find('\\') != std::string::npos || sec.name.find('/') != std::string::npos)
            continue;

        ModResource r;
        r.sectionName = sec.name;
        for (const auto& kv : sec.keys) {
            std::string k = ToLower(kv.first);
            if (k == "filename") r.filename = kv.second;
            else if (k == "stride") r.stride = std::atoi(kv.second.c_str());
            else if (k == "format") r.format = kv.second;
            else if (k == "type") { /* Buffer / Texture2D — ignore */ }
        }
        if (r.filename.empty()) {
            // data= text buffers etc.
            continue;
        }
        r.absolutePath = JoinDir(iniDirectory, r.filename);
        r.exists = FileExistsUtf8(r.absolutePath);
        r.kind = GuessBufferKind(r.sectionName, r.stride, r.format);
        // Filename extension hint
        std::string fl = ToLower(r.filename);
        if (fl.size() >= 3 && fl.compare(fl.size() - 3, 3, ".ib") == 0)
            r.kind = BufferKind::Index;
        else if (fl.size() >= 4 && fl.compare(fl.size() - 4, 4, ".dds") == 0)
            r.kind = BufferKind::Texture;
        else if (fl.size() >= 4 && fl.compare(fl.size() - 4, 4, ".buf") == 0 &&
                 r.kind == BufferKind::Unknown) {
            // leave unknown
        }

        if (!r.exists) {
            scene.warnings.push_back({ "Missing file for " + r.sectionName + ": " + r.filename });
        }
        resMap[ToLower(r.sectionName)] = r;
        scene.resources.push_back(r);
    }

    // Pass 2: identify component roots from TextureOverride *Blend / *Position / *Texcoord
    struct ComponentBuild {
        ModComponent comp;
        bool hasRoot = false;
    };
    std::unordered_map<std::string, ComponentBuild> comps; // key lower(componentKey)

    auto getComp = [&](const std::string& key) -> ComponentBuild& {
        std::string lk = ToLower(key);
        auto it = comps.find(lk);
        if (it == comps.end()) {
            ComponentBuild b;
            b.comp.name = key;
            auto [ins, _] = comps.emplace(lk, std::move(b));
            return ins->second;
        }
        return it->second;
    };

    for (const auto& sec : sections) {
        if (ToLower(sec.name).rfind("textureoverride", 0) != 0)
            continue;

        std::string vb0, vb1, vb2;
        int vertexCount = 0;
        for (const auto& kv : sec.keys) {
            std::string k = ToLower(kv.first);
            std::string v = ParseResourceRef(kv.second);
            if (k == "vb0") vb0 = v;
            else if (k == "vb1") vb1 = v;
            else if (k == "vb2") vb2 = v;
            else if (k == "draw") ParseDrawVertexCount(kv.second, vertexCount);
            else if (k == "override_vertex_count") vertexCount = std::atoi(kv.second.c_str());
        }

        auto resolveKind = [&](const std::string& resName) -> BufferKind {
            auto it = resMap.find(ToLower(resName));
            if (it != resMap.end()) return it->second.kind;
            return GuessBufferKind(resName, 0, {});
        };

        // Component root: assigns position buffer
        bool hasPos = false;
        std::string posRes, texRes, blendRes;
        if (!vb0.empty() && resolveKind(vb0) == BufferKind::Position) {
            hasPos = true; posRes = vb0;
        }
        // Sometimes only blend section has vb0=Position under if DRAW_TYPE
        if (!vb0.empty() && ToLower(vb0).find("position") != std::string::npos) {
            hasPos = true; posRes = vb0;
        }
        if (!vb1.empty() && (resolveKind(vb1) == BufferKind::Texcoord ||
                             ToLower(vb1).find("texcoord") != std::string::npos))
            texRes = vb1;
        if (!vb2.empty() && (resolveKind(vb2) == BufferKind::Blend ||
                             ToLower(vb2).find("blend") != std::string::npos))
            blendRes = vb2;

        // Texcoord-only section
        bool isTexSection = ToLower(sec.name).find("texcoord") != std::string::npos;
        bool isBlendSection = ToLower(sec.name).find("blend") != std::string::npos;
        bool isPosSection = ToLower(sec.name).find("position") != std::string::npos;

        if (hasPos || isTexSection || isBlendSection || isPosSection) {
            std::string ckey = ComponentKeyFromOverride(sec.name);
            if (ckey.empty()) ckey = sec.name;
            auto& b = getComp(ckey);
            b.hasRoot = true;
            if (!posRes.empty()) {
                b.comp.positionResource = posRes;
                auto it = resMap.find(ToLower(posRes));
                if (it != resMap.end()) {
                    b.comp.positionPath = it->second.absolutePath;
                    b.comp.positionStride = it->second.stride > 0 ? it->second.stride : 40;
                }
            }
            if (!texRes.empty()) {
                b.comp.texcoordResource = texRes;
                auto it = resMap.find(ToLower(texRes));
                if (it != resMap.end()) {
                    b.comp.texcoordPath = it->second.absolutePath;
                    b.comp.texcoordStride = it->second.stride > 0 ? it->second.stride : 20;
                }
            }
            if (!blendRes.empty())
                b.comp.blendResource = blendRes;
            if (vertexCount > 0)
                b.comp.vertexCountHint = vertexCount;

            // Pure texcoord section: vb1 only
            if (isTexSection && texRes.empty()) {
                for (const auto& kv : sec.keys) {
                    if (ToLower(kv.first) == "vb1") {
                        texRes = ParseResourceRef(kv.second);
                        b.comp.texcoordResource = texRes;
                        auto it = resMap.find(ToLower(texRes));
                        if (it != resMap.end()) {
                            b.comp.texcoordPath = it->second.absolutePath;
                            b.comp.texcoordStride = it->second.stride > 0 ? it->second.stride : 20;
                        }
                    }
                }
            }
        }
    }

    // Pass 3: parts — TextureOverride with drawindexed OR run=CommandList that draws
    for (const auto& sec : sections) {
        if (ToLower(sec.name).rfind("textureoverride", 0) != 0)
            continue;

        bool hasDraw = false;
        bool hasRunCL = false;
        for (const auto& kv : sec.keys) {
            std::string k = ToLower(kv.first);
            if (k == "drawindexed") hasDraw = true;
            if (k == "run") {
                std::string v = ToLower(kv.second);
                if (v.find("commandlist") != std::string::npos) hasRunCL = true;
            }
        }
        // Also scan raw lines (keys map may collapse duplicates)
        for (const auto& raw : sec.rawLines) {
            std::string body = StripInlineComment(raw);
            std::string low = ToLower(body);
            if (low.rfind("drawindexed", 0) == 0) hasDraw = true;
            if (low.rfind("run", 0) == 0 && low.find("commandlist") != std::string::npos)
                hasRunCL = true;
        }
        if (!hasDraw && !hasRunCL) continue;

        ModPart part;
        part.sectionName = sec.name;
        for (const auto& kv : sec.keys) {
            std::string k = ToLower(kv.first);
            if (k == "ib") part.ibResource = ParseResourceRef(kv.second);
            else if (k == "hash") part.hash = kv.second;
            else if (k == "match_first_index") part.matchFirstIndex = std::atoi(kv.second.c_str());
        }
        if (!part.ibResource.empty()) {
            auto it = resMap.find(ToLower(part.ibResource));
            if (it != resMap.end()) {
                part.ibAbsolutePath = it->second.absolutePath;
                part.hasGeometry = it->second.exists;
                if (!it->second.exists)
                    scene.warnings.push_back({ "Missing IB: " + part.ibResource });
            } else {
                part.hasGeometry = false;
                scene.warnings.push_back({ "Unknown IB resource: " + part.ibResource });
            }
        } else {
            part.hasGeometry = false;
        }

        ParsePartBatches(sec, secByName, resMap, part);
        if (part.batches.empty()) continue;

        std::string ckey = ComponentKeyFromOverride(sec.name);
        part.name = PartNameFromSection(sec.name, ckey);

        auto it = comps.find(ToLower(ckey));
        if (it != comps.end()) {
            it->second.comp.parts.push_back(std::move(part));
        } else {
            bool attached = false;
            if (!part.ibResource.empty()) {
                for (auto& [lk, b] : comps) {
                    if (ToLower(part.ibResource).find(lk) != std::string::npos) {
                        part.name = PartNameFromSection(sec.name, b.comp.name);
                        b.comp.parts.push_back(std::move(part));
                        attached = true;
                        break;
                    }
                }
            }
            if (!attached) {
                if (!ckey.empty()) {
                    auto& b = getComp(ckey);
                    b.comp.parts.push_back(std::move(part));
                } else {
                    scene.orphanParts.push_back(std::move(part));
                }
            }
        }
    }

    // Emit components (stable-ish order by name)
    std::vector<std::string> keys;
    keys.reserve(comps.size());
    for (const auto& [k, _] : comps) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    for (const auto& k : keys) {
        auto& b = comps[k];
        // Skip empty shells with no parts and no position (noise)
        if (b.comp.parts.empty() && b.comp.positionResource.empty())
            continue;
        // If position missing but parts exist — still keep (texture-only components)
        if (b.comp.positionResource.empty() && !b.comp.parts.empty()) {
            bool anyGeo = false;
            for (const auto& p : b.comp.parts)
                if (p.hasGeometry && p.TotalDraws() > 0) anyGeo = true;
            if (!anyGeo) {
                scene.warnings.push_back({
                    "Component '" + b.comp.name + "' has parts but no Position buffer (texture-only?)"
                });
            } else {
                scene.warnings.push_back({
                    "Component '" + b.comp.name + "' has geometry parts but Position buffer not resolved"
                });
            }
        }
        scene.components.push_back(std::move(b.comp));
    }

    if (scene.components.empty() && scene.orphanParts.empty()) {
        scene.ok = false;
        scene.error = "No components or drawindexed parts found";
        // Still useful if resources exist
        if (!scene.resources.empty()) {
            scene.ok = true;
            scene.error.clear();
            scene.warnings.push_back({ "Parsed resources only — no drawable parts" });
        }
    } else {
        scene.ok = true;
    }

    // Default layouts from stride presets (roles are defaults — user/dump can override)
    AssignLayouts(scene, scene.gameHint);

    Logger::Get().Info("ModIniParser: " + scene.iniPath +
                       " components=" + std::to_string(scene.components.size()) +
                       " parts=" + std::to_string(scene.PartCount()) +
                       " draws=" + std::to_string(scene.DrawCount()) +
                       " binds=" + std::to_string(scene.TextureBindCount()) +
                       " warnings=" + std::to_string(scene.warnings.size()) +
                       (scene.ok ? " OK" : " FAIL: " + scene.error));

    return scene;
}

static std::string LowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

void ModIniParser::AssignLayouts(ModScene& scene, GameLayoutHint game) {
    scene.gameHint = game;
    scene.defaultPositionLayout = PresetForPositionStride(40, game);
    // Most common texcoord; per-component may differ
    scene.defaultTexcoordLayout = PresetForTexcoordStride(20, game);

    for (auto& c : scene.components) {
        // Preserve manual role locks if layouts already present
        BufferLayout prevPos = c.positionLayout;
        BufferLayout prevTex = c.texcoordLayout;

        c.positionLayout = PresetForPositionStride(
            c.positionStride > 0 ? c.positionStride : 40, game);
        c.texcoordLayout = PresetForTexcoordStride(
            c.texcoordStride > 0 ? c.texcoordStride : 20, game);

        // Re-apply manual roles by semantic match
        auto mergeManual = [](BufferLayout& dst, const BufferLayout& prev) {
            if (prev.elements.empty()) return;
            for (auto& e : dst.elements) {
                for (const auto& p : prev.elements) {
                    if (p.roleManual &&
                        LowerCopy(p.dumpSemanticName) == LowerCopy(e.dumpSemanticName) &&
                        p.dumpSemanticIndex == e.dumpSemanticIndex) {
                        e.role = p.role;
                        e.roleManual = true;
                    }
                }
            }
        };
        mergeManual(c.positionLayout, prevPos);
        mergeManual(c.texcoordLayout, prevTex);

        if (!c.positionLayout.valid) {
            scene.warnings.push_back({
                "Component '" + c.name + "': " + c.positionLayout.error
            });
        }
        if (!c.texcoordLayout.valid) {
            scene.warnings.push_back({
                "Component '" + c.name + "': " + c.texcoordLayout.error +
                " — set dump path or edit semantics manually"
            });
        }
    }
}

bool ModIniParser::ApplyDumpPath(ModScene& scene, const std::string& dumpPath,
                                 GameLayoutHint game) {
    scene.dumpPath = PathUtil::NormalizeToUtf8Path(dumpPath);
    scene.gameHint = game;

    if (scene.dumpPath.empty()) {
        scene.warnings.push_back({ "Empty dump path" });
        return false;
    }
    if (!PathUtil::Exists(scene.dumpPath)) {
        scene.warnings.push_back({ "Dump path does not exist: " + scene.dumpPath });
        return false;
    }

    // Find first usable vb0 layout text
    std::string bestDump;
    try {
        namespace fs = std::filesystem;
        auto root = PathUtil::FromUtf8(scene.dumpPath);
        if (fs::is_regular_file(root)) {
            bestDump = scene.dumpPath;
        } else if (fs::is_directory(root)) {
            for (auto it = fs::recursive_directory_iterator(root);
                 it != fs::recursive_directory_iterator(); ++it) {
                if (!it->is_regular_file()) continue;
                auto name = PathUtil::WideToUtf8(it->path().filename().wstring());
                std::string low = name;
                std::transform(low.begin(), low.end(), low.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                // Prefer *-vb0=*.txt or *vb0*.txt
                if (low.find("vb0") != std::string::npos &&
                    (low.size() >= 4 && low.compare(low.size() - 4, 4, ".txt") == 0)) {
                    bestDump = PathUtil::WideToUtf8(it->path().wstring());
                    // Prefer larger / character dumps over tiny ones — keep first with "stride:"
                    // Try open quickly
                    break;
                }
            }
        }
    } catch (const std::exception& e) {
        scene.warnings.push_back({ std::string("Dump scan failed: ") + e.what() });
        return false;
    }

    if (bestDump.empty()) {
        scene.warnings.push_back({
            "No vb0 layout .txt found under dump path (need 3DMigoto frame analysis header)"
        });
        return false;
    }

    scene.dumpFullLayout = ParseDumpVbLayoutHeader(bestDump);
    if (!scene.dumpFullLayout.valid) {
        scene.warnings.push_back({ "Dump parse failed: " + scene.dumpFullLayout.error });
        return false;
    }

    AssignSuggestedRoles(scene.dumpFullLayout, game);
    SplitDumpLayoutByInputSlot(scene.dumpFullLayout,
                               scene.dumpPositionLayout,
                               scene.dumpTexcoordLayout);
    AssignSuggestedRoles(scene.dumpPositionLayout, game);
    AssignSuggestedRoles(scene.dumpTexcoordLayout, game);

    scene.defaultPositionLayout = scene.dumpPositionLayout;
    scene.defaultTexcoordLayout = scene.dumpTexcoordLayout;

    // Apply to components (keep manual locks)
    for (auto& c : scene.components) {
        BufferLayout prevPos = c.positionLayout;
        BufferLayout prevTex = c.texcoordLayout;

        // Prefer dump texcoord if stride matches component, else keep preset for that stride
        if (scene.dumpPositionLayout.valid) {
            c.positionLayout = scene.dumpPositionLayout;
            if (c.positionStride > 0)
                c.positionLayout.stride = c.positionStride;
        }
        if (scene.dumpTexcoordLayout.valid) {
            if (c.texcoordStride <= 0 ||
                c.texcoordStride == scene.dumpTexcoordLayout.stride ||
                std::abs(c.texcoordStride - scene.dumpTexcoordLayout.stride) <= 4) {
                c.texcoordLayout = scene.dumpTexcoordLayout;
                if (c.texcoordStride > 0)
                    c.texcoordLayout.stride = c.texcoordStride;
            } else {
                // stride mismatch — keep preset for component stride, but copy roles from dump by semantic
                c.texcoordLayout = PresetForTexcoordStride(c.texcoordStride, game);
                for (auto& e : c.texcoordLayout.elements) {
                    for (const auto& d : scene.dumpTexcoordLayout.elements) {
                        if (LowerCopy(d.dumpSemanticName) == LowerCopy(e.dumpSemanticName) &&
                            d.dumpSemanticIndex == e.dumpSemanticIndex) {
                            e.role = d.role;
                        }
                    }
                }
            }
        }

        auto mergeManual = [](BufferLayout& dst, const BufferLayout& prev) {
            for (auto& e : dst.elements) {
                for (const auto& p : prev.elements) {
                    if (p.roleManual &&
                        LowerCopy(p.dumpSemanticName) == LowerCopy(e.dumpSemanticName) &&
                        p.dumpSemanticIndex == e.dumpSemanticIndex) {
                        e.role = p.role;
                        e.roleManual = true;
                    }
                }
            }
        };
        mergeManual(c.positionLayout, prevPos);
        mergeManual(c.texcoordLayout, prevTex);
        c.positionLayout.valid = !c.positionLayout.elements.empty();
        c.texcoordLayout.valid = !c.texcoordLayout.elements.empty();
    }

    Logger::Get().Info("Dump layouts applied from " + bestDump + "\n" +
                       FormatLayoutSummary(scene.dumpPositionLayout) +
                       FormatLayoutSummary(scene.dumpTexcoordLayout));
    return true;
}

void ModIniParser::RefreshExistence(ModScene& scene) {
    for (auto& r : scene.resources) {
        if (!r.absolutePath.empty())
            r.exists = FileExistsUtf8(r.absolutePath);
    }
    auto fixPart = [&](ModPart& p) {
        if (!p.ibAbsolutePath.empty())
            p.hasGeometry = FileExistsUtf8(p.ibAbsolutePath);
        for (auto& b : p.batches) {
            for (auto& t : b.textures) {
                if (!t.absolutePath.empty())
                    t.exists = FileExistsUtf8(t.absolutePath);
            }
        }
    };
    for (auto& c : scene.components) {
        if (!c.positionPath.empty() && !FileExistsUtf8(c.positionPath))
            c.positionPath = c.positionPath; // existence checked via resources
        for (auto& p : c.parts) fixPart(p);
    }
    for (auto& p : scene.orphanParts) fixPart(p);
}

std::string FormatSceneSummary(const ModScene& scene) {
    std::ostringstream o;
    o << "INI: " << scene.iniPath << "\n";
    if (!scene.ok) o << "ERROR: " << scene.error << "\n";
    o << "Components: " << scene.components.size()
      << "  Parts: " << scene.PartCount()
      << "  Draws: " << scene.DrawCount()
      << "  TextureBinds: " << scene.TextureBindCount()
      << "  Resources: " << scene.resources.size() << "\n";
    for (const auto& c : scene.components) {
        o << "  [" << c.name << "] pos=" << c.positionResource
          << " (stride " << c.positionStride << ")"
          << " tex=" << c.texcoordResource
          << " (stride " << c.texcoordStride << ")"
          << " verts~" << c.vertexCountHint
          << " parts=" << c.parts.size() << "\n";
        for (const auto& p : c.parts) {
            o << "    - " << p.name << " ib=" << p.ibResource
              << " batches=" << p.batches.size()
              << " draws=" << p.TotalDraws()
              << (p.hasGeometry ? "" : " [no geo]") << "\n";
            for (const auto& bat : p.batches) {
                o << "      batch \"" << bat.name << "\" draws=" << bat.draws.size()
                  << " tex=" << bat.textures.size()
                  << (bat.visible ? "" : " [hidden]") << "\n";
                for (const auto& t : bat.textures) {
                    o << "        " << MaterialSlotName(t.slot);
                    if (t.source == BindSource::ApiNamed) o << " (API:" << t.apiName << ")";
                    o << " → " << t.resourceName << (t.exists ? "" : " MISSING") << "\n";
                }
                for (const auto& d : bat.draws) {
                    o << "        drawindexed " << d.indexCount << ", " << d.indexStart;
                    if (!d.commentLabel.empty()) o << "  ; " << d.commentLabel;
                    if (!d.visible) o << " [hidden]";
                    o << "\n";
                }
            }
        }
    }
    if (!scene.orphanParts.empty()) {
        o << "  Orphan parts: " << scene.orphanParts.size() << "\n";
    }
    for (const auto& w : scene.warnings)
        o << "  ! " << w.message << "\n";
    return o.str();
}

} // namespace modio

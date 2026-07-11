#pragma once

#include "TextureSetTypes.h"
#include "../core/TileCache.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace texset {

// Texture Set = maps + shared layer stack + logical UV space.
// Phase P0.2: Diffuse still lives primarily on Canvas; TextureSet holds map meta
// and optional extra map composites. Full multi-cache layers land in P0.4.

class TextureSet {
public:
    int id = 0;
    std::string name;
    std::string templateId = "Default";

    TextureSet() = default;
    TextureSet(TextureSet&&) noexcept = default;
    TextureSet& operator=(TextureSet&&) noexcept = default;
    // mapComposites owns TileCaches — not copyable
    TextureSet(const TextureSet&) = delete;
    TextureSet& operator=(const TextureSet&) = delete;

    // Map slots (Diffuse always enabled)
    std::vector<MapSlot> maps;

    // Logical UV space reference size (usually Diffuse size)
    int LogicalWidth() const;
    int LogicalHeight() const;

    // Active map shown in 2D viewport (Simple always Diffuse)
    MapKind activeMap = MapKind::Diffuse;

    // ---- Construction ----
    static TextureSet CreateDefault(const std::string& name = "Texture Set");
    static TextureSet CreateFromTemplate(const std::string& name, const SetTemplate& t);

    void ApplySetTemplate(const SetTemplate& t);

    MapSlot* GetMap(MapKind k) { return FindMap(maps, k); }
    const MapSlot* GetMap(MapKind k) const { return FindMap(maps, k); }

    // Enable map and set size (e.g. after import). Does not allocate layers.
    bool EnableMap(MapKind k, int w, int h, const std::string& sourcePath = {});
    void DisableMap(MapKind k); // Diffuse cannot disable

    // Extra map pixel stores (non-Diffuse) — sparse TileCache composites for view/export
    // Key = (int)MapKind. Owned by set; invalidated when dirty.
    std::unordered_map<int, std::unique_ptr<TileCache>> mapComposites;
    std::unordered_map<int, bool> mapCompositeDirty;

    TileCache* EnsureComposite(MapKind k, CanvasPixelFormat fmt = CanvasPixelFormat::RGBA8);
    void MarkCompositeDirty(MapKind k);
    void MarkAllCompositesDirty();

    // Serialize meta (no pixels) for .rayp
    // Implemented via nlohmann in .cpp
    std::string MetaToJson() const;
    static bool MetaFromJson(const std::string& json, TextureSet& out);
};

// Project-level bag of texture sets (Advanced / Advanced Mod)
struct TextureSetLibrary {
    std::vector<TextureSet> sets;
    int activeSetId = -1;
    int nextId = 1;

    TextureSetLibrary() = default;
    TextureSetLibrary(TextureSetLibrary&&) noexcept = default;
    TextureSetLibrary& operator=(TextureSetLibrary&&) noexcept = default;
    TextureSetLibrary(const TextureSetLibrary&) = delete;
    TextureSetLibrary& operator=(const TextureSetLibrary&) = delete;

    TextureSet* Active();
    const TextureSet* Active() const;
    TextureSet* Find(int id);
    const TextureSet* Find(int id) const;

    int AddSet(TextureSet set); // assigns id, returns id
    bool RemoveSet(int id);     // refuses if last set
    bool SetActive(int id);

    // Simple mode helper: ensure exactly one Diffuse-only set
    void EnsureSimpleDefault();

    std::string MetaToJson() const;
    static bool MetaFromJson(const std::string& json, TextureSetLibrary& out);
};

} // namespace texset

#pragma once

#include "TextureSetTypes.h"

#include <string>
#include <vector>

namespace texset {

// Texture Set = map slots (meta) + document layers (Canvas) own the pixels.
// No hidden mapComposites store — each map is a layer with workSpace.

class TextureSet {
public:
    int id = 0;
    std::string name;
    std::string templateId = "Default";

    TextureSet() = default;
    TextureSet(TextureSet&&) noexcept = default;
    TextureSet& operator=(TextureSet&&) noexcept = default;
    TextureSet(const TextureSet&) = delete;
    TextureSet& operator=(const TextureSet&) = delete;

    std::vector<MapSlot> maps;

    int LogicalWidth() const;
    int LogicalHeight() const;

    MapKind activeMap = MapKind::Diffuse;

    static TextureSet CreateDefault(const std::string& name = "Texture Set");
    static TextureSet CreateFromTemplate(const std::string& name, const SetTemplate& t);

    void ApplySetTemplate(const SetTemplate& t);

    MapSlot* GetMap(MapKind k) { return FindMap(maps, k); }
    const MapSlot* GetMap(MapKind k) const { return FindMap(maps, k); }

    bool EnableMap(MapKind k, int w, int h, const std::string& sourcePath = {});
    void DisableMap(MapKind k); // Diffuse can disable too (user request)

    std::string MetaToJson() const;
    static bool MetaFromJson(const std::string& json, TextureSet& out);
};

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

    int AddSet(TextureSet set);
    bool RemoveSet(int id);
    bool SetActive(int id);

    void EnsureSimpleDefault();

    std::string MetaToJson() const;
    static bool MetaFromJson(const std::string& json, TextureSetLibrary& out);
};

} // namespace texset

#pragma once
#include "PaintEngine.h"
#include <string>
#include <vector>
#include <filesystem>
#include <optional>

// ---------------------------------------------------------------------------
// Brush preset system (data + disk + RAM staging).
// UI owns popup chrome; core owns identity, I/O, and tip lifetime.
// See plans/BRUSH_PRESETS.md
// ---------------------------------------------------------------------------

struct BrushPresetMeta {
    std::string id;           // stable uuid or "builtin.<name>"
    std::string displayName;
    bool isBuiltin = false;   // blue, non-deletable
    bool isDirty   = false;   // RAM-only / unsaved edits
    std::string sourcePath;   // empty for builtins/staging; absolute .rvbrush path for disk customs
};

// Serializable paint snapshot (FG color intentionally NOT stored — like Photoshop brushes).
// Includes tablet toggles, spacing, rotation/scatter placeholders for future engine work.
struct BrushPresetParams {
    float radius = 10.0f;
    float hardness = 0.5f;
    float opacity = 1.0f;
    float spacing = 0.1f;
    int   stabilization = 1;
    bool  erase = false;
    bool  writeR = true, writeG = true, writeB = true, writeA = false;
    bool  pressureRadius = false, pressureHardness = false, pressureOpacity = false;

    // Placeholders (saved in .rvbrush; engine may ignore until implemented)
    float rotationDeg = 0.f;
    bool  pressureRotation = false;
    float scatter = 0.f;
    float angleJitter = 0.f;
    std::string tipSourcePath;

    // Tip: one of none / builtin_id / embedded grayscale
    enum class TipType : uint8_t { None = 0, Builtin = 1, Embedded = 2 };
    TipType tipType = TipType::None;
    std::string tipBuiltinId; // "soft_round" | "hard_round" | "pencil" | "airbrush"
    int tipSize = 0;
    std::vector<uint8_t> tipPixels; // size*size, only if Embedded
    float tipSpacingMul = 1.0f;

    static BrushPresetParams FromSettings(const BrushSettings& s, const BrushTip* tip);
    void ApplyToSettings(BrushSettings& s, const BrushTip* ownedTipPtr) const;
};

class BrushLibrary {
public:
    static BrushLibrary& Get();

    // Paths: default = %AppData%/Roaming/RayVPaint/brushes (Windows).
    // Fallback = <exeDir>/RayVPaint/brushes. Never requires admin.
    void SetRootDir(const std::filesystem::path& root);
    std::filesystem::path GetRootDir() const { return m_Root; }
    void EnsureRootExists();
    void LoadAll(); // builtins + scan *.rvbrush

    const std::vector<BrushPresetMeta>& List() const { return m_MetaList; }

    // Resolve by id → params + owned tip (pointer valid until next mutating API that drops entry).
    bool Get(const std::string& id, BrushPresetParams& outParams) const;
    bool GetMeta(const std::string& id, BrushPresetMeta& out) const;

    // Apply preset into live BrushSettings. tip pointer points into library storage.
    bool ApplyTo(const std::string& id, BrushSettings& brush);

    // RAM staging (unsaved working brush). Returns new id (uuid). isDirty=true.
    std::string CreateFromCurrent(const BrushSettings& brush, const std::string& name = "New Brush");
    bool UpdateStaging(const std::string& id, const BrushSettings& brush);
    bool Rename(const std::string& id, const std::string& newDisplayName);

    // Persist staging/custom to disk as .rvbrush; clears isDirty. Fails for builtins.
    bool SaveToDisk(const std::string& id);
    // Drop unsaved staging (or reload disk custom from file). Builtins: no-op false.
    bool DiscardStaging(const std::string& id);
    // Delete custom file + entry. Refuses builtins.
    bool DeleteCustom(const std::string& id);

    // Active selection for UI (optional convenience; not auto-persisted to .rayp).
    void SetActiveId(const std::string& id) { m_ActiveId = id; }
    const std::string& GetActiveId() const { return m_ActiveId; }

    // Smoke: returns true if create→save→reload→apply matches. Logs details.
    static bool RunSmokeTest();

private:
    BrushLibrary() = default;

    struct Entry {
        BrushPresetMeta meta;
        BrushPresetParams params;
        BrushTip ownedTip; // library-owned; BrushSettings::tip may point here
        void RebuildOwnedTip();
    };

    void RegisterBuiltins();
    void RebuildMetaList();
    Entry* Find(const std::string& id);
    const Entry* Find(const std::string& id) const;
    bool LoadFile(const std::filesystem::path& path);
    bool WriteFile(const Entry& e) const;
    static std::string NewUuid();
    static std::filesystem::path DefaultRootDir();
    static std::string Base64Encode(const std::vector<uint8_t>& data);
    static bool Base64Decode(const std::string& in, std::vector<uint8_t>& out);

    std::filesystem::path m_Root;
    std::vector<Entry> m_Entries;
    std::vector<BrushPresetMeta> m_MetaList;
    std::string m_ActiveId;
    bool m_Loaded = false;
};

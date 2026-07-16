#pragma once
// Periodic + quit autosaves with rotation, previews, and cold-start recent picker.
//
// File naming:
//   {BASE}_{YYYYMMDD_HHMMSS}_{projectType}[_quit].rayp
//   BASE = stem of project path, or UNTITLED / UNTITLED-N
//   projectType = simple | advanced | advanced_mod
// Sibling preview: same stem + .preview.png

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Canvas;
struct Project;

namespace core {

struct AutosaveEntry {
    std::string raypPath;
    std::string previewPath; // may be empty / missing on disk
    std::string baseName;    // UNTITLED / project stem
    std::string projectType; // simple / advanced / advanced_mod
    std::string displayName; // for UI
    bool isQuit = false;
    int64_t mtime = 0;       // unix seconds
    int width = 0;
    int height = 0;
};

class AutoSaveManager {
public:
    static AutoSaveManager& Get();

    // Root folder (ConfigManager backup dir).
    std::string RootDir() const;

    // BASE for rotation key (UNTITLED, UNTITLED-2, MyDoc, …).
    static std::string ProjectBaseName(const Project* project);

    // simple | advanced | advanced_mod
    static std::string ProjectTypeToken(const Canvas& canvas);

    // Build path for a new autosave (does not write).
    std::string MakeSavePath(const std::string& baseName,
                             const std::string& typeToken,
                             bool quitSave) const;

    // Periodic autosave of active (or given) project. No-op if not modified / busy / disabled.
    // Returns true if a job was started.
    bool TryPeriodicSave(Project* project, Canvas& canvas);

    // Always attempts (if canvas has content). Used on app quit.
    bool SaveOnQuit(Project* project, Canvas& canvas);

    // Force save now (settings test / menu).
    bool SaveNow(Project* project, Canvas& canvas, bool quitSave);

    bool IsBusy() const { return m_Busy.load(); }

    // Scan autosave root; newest first. limit=0 → all.
    std::vector<AutosaveEntry> ListRecent(int limit = 40) const;

    // Keep at most maxPerProject files per BASE (by mtime). Deletes older .rayp + .preview.png.
    void PruneProject(const std::string& baseName, int maxPerProject) const;

    // After load: refresh index (optional; ListRecent scans disk).
    void NoteSaved(const AutosaveEntry& e);

private:
    AutoSaveManager() = default;

    std::atomic<bool> m_Busy{false};
    double m_LastSaveSteadySec = 0.0; // rate-limit periodic

    static double SteadySec();
    static std::string TimeStampLocal();
    static std::string SanitizeBase(const std::string& name);
    bool StartSaveJob(Project* project, Canvas& canvas, bool quitSave);
};

} // namespace core

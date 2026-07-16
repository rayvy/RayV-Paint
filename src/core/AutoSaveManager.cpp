#include "AutoSaveManager.h"
#include "ConfigManager.h"
#include "JobManager.h"
#include "Logger.h"
#include "Notifications.h"
#include "PathUtil.h"
#include "ProjectManager.h"
#include "../Canvas.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#include <cwctype>
#endif

namespace fs = std::filesystem;

namespace core {

AutoSaveManager& AutoSaveManager::Get() {
    static AutoSaveManager s;
    return s;
}

double AutoSaveManager::SteadySec() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

std::string AutoSaveManager::TimeStampLocal() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return os.str();
}

std::string AutoSaveManager::SanitizeBase(const std::string& name) {
    std::string s = name;
    if (s.empty()) s = "UNTITLED";
    for (char& c : s) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
            c == '|' || c == '?' || c == '*' || c == ' ')
            c = '_';
    }
    // Uppercase UNTITLED variants stay readable
    if (s.size() >= 8) {
        std::string up = s;
        for (char& c : up) c = (char)std::toupper((unsigned char)c);
        if (up.rfind("UNTITLED", 0) == 0)
            s = up;
    }
    return s;
}

std::string AutoSaveManager::RootDir() const {
    return ConfigManager::Get().GetBackupDir();
}

std::string AutoSaveManager::ProjectBaseName(const Project* project) {
    if (!project || !project->canvas)
        return "UNTITLED";
    const std::string& path = project->canvas->GetCurrentProjectFilePath();
    if (!path.empty()) {
        try {
            auto stem = fs::path(PathUtil::Utf8ToWide(path)).stem().wstring();
            std::string u8 = PathUtil::WideToUtf8(stem);
            if (!u8.empty()) return SanitizeBase(u8);
        } catch (...) {}
    }
    // Untitled projects → UNTITLED or UNTITLED-N
    if (project->untitledIndex > 0)
        return "UNTITLED-" + std::to_string(project->untitledIndex);
    return "UNTITLED";
}

std::string AutoSaveManager::ProjectTypeToken(const Canvas& canvas) {
    switch (canvas.GetProjectType()) {
    case Canvas::ProjectType::Simple: return "simple";
    case Canvas::ProjectType::AdvancedModMode: return "advanced_mod";
    default: return "advanced";
    }
}

std::string AutoSaveManager::MakeSavePath(const std::string& baseName,
                                         const std::string& typeToken,
                                         bool quitSave) const {
    const std::string base = SanitizeBase(baseName);
    const std::string stamp = TimeStampLocal();
    std::string file = base + "_" + stamp + "_" + typeToken;
    if (quitSave) file += "_quit";
    file += ".rayp";

    fs::path dir = fs::u8path(RootDir()) / fs::u8path(base);
    std::error_code ec;
    fs::create_directories(dir, ec);
    return PathUtil::WideToUtf8((dir / fs::u8path(file)).wstring());
}

bool AutoSaveManager::TryPeriodicSave(Project* project, Canvas& canvas) {
    const int mins = ConfigManager::Get().GetAutoSaveIntervalMinutes();
    if (mins <= 0) return false;
    if (m_Busy.load()) return false;
    if (!canvas.IsDocumentModified()) return false;
    // Blank empty starter with no edits — skip
    if (project && project->IsBlank()) return false;

    const double now = SteadySec();
    const double intervalSec = (double)mins * 60.0;
    if (m_LastSaveSteadySec > 0.0 && (now - m_LastSaveSteadySec) < intervalSec)
        return false;

    return StartSaveJob(project, canvas, /*quitSave=*/false);
}

bool AutoSaveManager::SaveOnQuit(Project* project, Canvas& canvas) {
    // Quit save even if not marked modified? Prefer if has content / dirty.
    if (project && project->IsBlank() && !canvas.IsDocumentModified())
        return false;
    // If nothing to save
    if (canvas.GetWidth() <= 0 || canvas.GetHeight() <= 0)
        return false;
    return StartSaveJob(project, canvas, /*quitSave=*/true);
}

bool AutoSaveManager::SaveNow(Project* project, Canvas& canvas, bool quitSave) {
    if (m_Busy.load()) return false;
    return StartSaveJob(project, canvas, quitSave);
}

bool AutoSaveManager::StartSaveJob(Project* project, Canvas& canvas, bool quitSave) {
    if (m_Busy.exchange(true)) return false;

    const std::string base = ProjectBaseName(project);
    const std::string type = ProjectTypeToken(canvas);
    const std::string rayp = MakeSavePath(base, type, quitSave);
    std::string preview = rayp;
    // foo.rayp → foo.preview.png
    if (preview.size() > 5 && preview.substr(preview.size() - 5) == ".rayp")
        preview = preview.substr(0, preview.size() - 5) + ".preview.png";
    else
        preview += ".preview.png";

    if (project)
        project->InjectTextureSetsIntoCanvas();

    m_LastSaveSteadySec = SteadySec();

    const uint64_t jobId = JobManager::Get().Begin(
        quitSave ? "Quit autosave" : "Autosave",
        /*locksDocument=*/true,
        /*cancellable=*/false);
    JobManager::Get().SetProgress(jobId, -1.f, rayp);

    Logger::Get().InfoTag("autosave",
        std::string(quitSave ? "Quit" : "Periodic") + " → " + rayp);

    const int maxKeep = ConfigManager::Get().GetAutosaveMaxPerProject();
    const std::string baseCopy = base;

    canvas.SaveCanvasRaypAsync(rayp, [this, jobId, rayp, preview, quitSave, maxKeep, baseCopy](bool ok) {
        m_Busy.store(false);
        if (ok) {
            Logger::Get().InfoTag("autosave", "OK " + rayp);
            Notifications::Get().Push(
                quitSave ? "Quit save written" : "Autosaved",
                NotifyLevel::Info);
            try {
                PruneProject(baseCopy, maxKeep);
            } catch (...) {}
        } else {
            Logger::Get().ErrorTag("autosave", "FAILED " + rayp);
        }
        JobManager::Get().Complete(jobId, ok,
            ok ? (quitSave ? "Quit save OK" : "Autosave OK") : "Autosave failed",
            /*notify=*/false);
        (void)preview; // preview written inside SaveCanvasRaypAsync when path set
    }, preview);

    return true;
}

void AutoSaveManager::PruneProject(const std::string& baseName, int maxPerProject) const {
    if (maxPerProject <= 0) return;
    const std::string base = SanitizeBase(baseName);
    fs::path dir = fs::u8path(RootDir()) / fs::u8path(base);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;

    struct Item {
        fs::path path;
        fs::file_time_type ft;
    };
    std::vector<Item> items;
    for (auto& ent : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) { ec.clear(); continue; }
        if (!ent.is_regular_file(ec)) continue;
        auto ext = ent.path().extension().wstring();
        for (auto& c : ext) c = (wchar_t)towlower(c);
        if (ext != L".rayp") continue;
        items.push_back({ ent.path(), ent.last_write_time(ec) });
    }
    if ((int)items.size() <= maxPerProject) return;

    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.ft > b.ft; // newest first
    });
    for (size_t i = (size_t)maxPerProject; i < items.size(); ++i) {
        std::error_code e2;
        fs::remove(items[i].path, e2);
        // sibling preview
        fs::path prev = items[i].path;
        prev.replace_extension();
        auto stem = prev.wstring() + L".preview.png";
        fs::remove(stem, e2);
        // also try path without .rayp + .preview.png
        fs::path p2 = items[i].path;
        std::wstring w = p2.wstring();
        if (w.size() > 5) {
            w = w.substr(0, w.size() - 5) + L".preview.png";
            fs::remove(w, e2);
        }
        Logger::Get().InfoTag("autosave",
            "Pruned old " + PathUtil::WideToUtf8(items[i].path.wstring()));
    }
}

std::vector<AutosaveEntry> AutoSaveManager::ListRecent(int limit) const {
    std::vector<AutosaveEntry> out;
    fs::path root = fs::u8path(RootDir());
    std::error_code ec;
    if (!fs::exists(root, ec)) return out;

    auto considerFile = [&](const fs::path& p) {
        if (!p.has_extension()) return;
        auto ext = p.extension().wstring();
        for (auto& c : ext) c = (wchar_t)towlower(c);
        if (ext != L".rayp") return;

        AutosaveEntry e;
        e.raypPath = PathUtil::WideToUtf8(p.wstring());
        std::wstring w = p.wstring();
        if (w.size() > 5)
            e.previewPath = PathUtil::WideToUtf8(w.substr(0, w.size() - 5) + L".preview.png");

        std::string name = PathUtil::WideToUtf8(p.filename().wstring());
        e.displayName = name;
        e.isQuit = (name.find("_quit.rayp") != std::string::npos) ||
                   (name.find("_quit.") != std::string::npos);

        // Parse BASE from parent folder or filename prefix
        try {
            e.baseName = PathUtil::WideToUtf8(p.parent_path().filename().wstring());
            if (e.baseName.empty() || e.baseName == "autosaves") {
                // flat legacy: first token before _
                auto pos = name.find('_');
                e.baseName = pos != std::string::npos ? name.substr(0, pos) : name;
            }
        } catch (...) {
            e.baseName = "UNTITLED";
        }

        // type token from name
        if (name.find("_advanced_mod") != std::string::npos) e.projectType = "advanced_mod";
        else if (name.find("_advanced") != std::string::npos) e.projectType = "advanced";
        else if (name.find("_simple") != std::string::npos) e.projectType = "simple";
        else e.projectType = "?";

        try {
            auto ft = fs::last_write_time(p, ec);
            if (!ec) {
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                e.mtime = (int64_t)std::chrono::system_clock::to_time_t(sctp);
            }
        } catch (...) {}

        // legacy single file
        if (name == "autosave_backup.rayp") {
            e.baseName = "LEGACY";
            e.displayName = "Previous session (legacy)";
            e.projectType = "unknown";
        }

        out.push_back(std::move(e));
    };

    // Nested: autosaves/BASE/*.rayp
    for (auto& ent : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) { ec.clear(); continue; }
        if (ent.is_directory(ec)) {
            for (auto& f : fs::directory_iterator(ent.path(), fs::directory_options::skip_permission_denied, ec)) {
                if (ec) { ec.clear(); continue; }
                if (f.is_regular_file(ec)) considerFile(f.path());
            }
        } else if (ent.is_regular_file(ec)) {
            considerFile(ent.path()); // flat / legacy
        }
    }

    std::sort(out.begin(), out.end(), [](const AutosaveEntry& a, const AutosaveEntry& b) {
        return a.mtime > b.mtime;
    });
    if (limit > 0 && (int)out.size() > limit)
        out.resize((size_t)limit);
    return out;
}

void AutoSaveManager::NoteSaved(const AutosaveEntry&) {}

} // namespace core

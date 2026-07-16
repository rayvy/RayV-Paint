#include "ScriptPluginHost.h"
#include "ScriptingEngine.h"
#include "../core/ConfigManager.h"
#include "../core/Logger.h"
#include "../core/PathUtil.h"
#include <filesystem>
#include <sstream>
#include <cctype>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace script {

ScriptPluginHost& ScriptPluginHost::Get() {
    static ScriptPluginHost s;
    return s;
}

std::string ScriptPluginHost::BuiltinScriptsDir() {
#ifdef _WIN32
    wchar_t exeBuf[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH)) {
        fs::path p = fs::path(exeBuf).parent_path() / "scripts";
        return PathUtil::WideToUtf8(p.wstring());
    }
#endif
    return "scripts";
}

std::string ScriptPluginHost::UserScriptsDir() {
    return ConfigManager::GetUserSubdirectory("scripts");
}

static void CollectPyFiles(const fs::path& dir, const char* source,
                           std::vector<PluginInfo>& out) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    for (auto& ent : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!ent.is_regular_file()) continue;
        auto ext = ent.path().extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext != ".py") continue;
        std::string stem = PathUtil::WideToUtf8(ent.path().stem().wstring());
        if (stem.empty() || stem[0] == '_') continue; // skip _helpers.py
        PluginInfo info;
        info.id = stem;
        info.path = PathUtil::WideToUtf8(ent.path().wstring());
        info.title = stem;
        info.source = source;
        out.push_back(std::move(info));
    }
}

bool ScriptPluginHost::Reload(std::string* outSummary) {
    m_Plugins.clear();
    m_Loaded = false;
    m_UiDisabled = false;

    if (!ScriptingEngine::Get().IsInitialized()) {
        if (!ScriptingEngine::Get().Initialize()) {
            if (outSummary) *outSummary = "Python not initialized";
            return false;
        }
    }

    CollectPyFiles(fs::u8path(BuiltinScriptsDir()), "builtin", m_Plugins);
    CollectPyFiles(fs::u8path(UserScriptsDir()), "user", m_Plugins);

    // User plugins override builtin same id: load user second in import order.
    // Import each module into sys.modules under rayv_plugins.<id>
    std::ostringstream code;
    code <<
        "import importlib, importlib.util, sys, types\n"
        "if 'rayv_plugins' not in sys.modules:\n"
        "    rayv_plugins = types.ModuleType('rayv_plugins')\n"
        "    sys.modules['rayv_plugins'] = rayv_plugins\n"
        "else:\n"
        "    rayv_plugins = sys.modules['rayv_plugins']\n"
        "if not hasattr(rayv_plugins, '_registry'):\n"
        "    rayv_plugins._registry = {}\n"
        "if not hasattr(rayv_plugins, '_open_requests'):\n"
        "    rayv_plugins._open_requests = set()\n"
        "rayv_plugins._registry.clear()\n";

    for (auto& p : m_Plugins) {
        // Escape path for Python string
        std::string path = p.path;
        std::string esc;
        for (char c : path) {
            if (c == '\\') esc += "\\\\";
            else if (c == '\'') esc += "\\'";
            else esc += c;
        }
        code << "try:\n"
             << "    _spec = importlib.util.spec_from_file_location('rayv_plugins." << p.id
             << "', r'''" << esc << "''')\n"
             << "    _mod = importlib.util.module_from_spec(_spec)\n"
             << "    sys.modules['rayv_plugins." << p.id << "'] = _mod\n"
             << "    _spec.loader.exec_module(_mod)\n"
             << "    _meta = getattr(_mod, 'PLUGIN', {}) or {}\n"
             << "    _title = _meta.get('name', '" << p.id << "')\n"
             << "    rayv_plugins._registry['" << p.id << "'] = {\n"
             << "        'module': _mod,\n"
             << "        'title': _title,\n"
             << "        'source': '" << p.source << "',\n"
             << "        'path': r'''" << esc << "''',\n"
             << "        'menu': _meta.get('menu', _title),\n"
             << "    }\n"
             << "    if hasattr(_mod, 'on_load'):\n"
             << "        try: _mod.on_load()\n"
             << "        except Exception as e:\n"
             << "            import rayv; rayv.log_error(f'plugin {_title} on_load: {e}')\n"
             << "except Exception as e:\n"
             << "    import rayv; rayv.log_error(f'Failed to load plugin " << p.id << ": {e}')\n";
    }

    bool ok = ScriptingEngine::Get().RunString(code.str());

    // Refresh titles / flags from Python registry
    if (ok) {
        ScriptingEngine::Get().RunString(
            "import rayv_plugins as _rp\n"
            "import rayv\n"
            "_titles = {k: v.get('title', k) for k,v in _rp._registry.items()}\n"
            "rayv.log_info('Plugins loaded: ' + ', '.join(sorted(_titles.keys())) if _titles else 'Plugins loaded: (none)')\n");
    }

    // Update local titles via a simple second pass reading PLUGIN isn't easy without return values.
    for (auto& p : m_Plugins) {
        p.hasUi = true;
        p.hasMenu = true;
    }

    m_Loaded = ok;
    std::ostringstream sum;
    sum << "builtin=" << BuiltinScriptsDir() << " user=" << UserScriptsDir()
        << " plugins=" << m_Plugins.size();
    if (outSummary) *outSummary = sum.str();
    Logger::Get().InfoTag("script", "ScriptPluginHost reload: " + sum.str());
    return ok;
}

void ScriptPluginHost::DrawAllUi() {
    if (!m_Loaded || m_UiDisabled || !ScriptingEngine::Get().IsInitialized()) return;
    // Fail soft: never let a plugin take down the host process uncaught.
    ScriptingEngine::Get().RunString(
        "import rayv_plugins as _rp\n"
        "try:\n"
        "    _reqs = list(getattr(_rp, '_open_requests', set()))\n"
        "    _rp._open_requests = set()\n"
        "    for _id in _reqs:\n"
        "        _e = _rp._registry.get(_id)\n"
        "        if not _e: continue\n"
        "        _m = _e['module']\n"
        "        if hasattr(_m, 'on_open'):\n"
        "            try: _m.on_open()\n"
        "            except Exception as e:\n"
        "                import rayv; rayv.log_error(f'plugin {_id} on_open: {e}')\n"
        "    for _id, _e in list(_rp._registry.items()):\n"
        "        _m = _e['module']\n"
        "        if hasattr(_m, 'on_ui'):\n"
        "            try: _m.on_ui()\n"
        "            except Exception as e:\n"
        "                import rayv; rayv.log_error(f'plugin {_id} on_ui: {e}')\n"
        "except Exception as e:\n"
        "    import rayv; rayv.log_error(f'DrawAllUi fatal: {e}')\n");
}

void ScriptPluginHost::RequestOpen(const std::string& pluginId) {
    if (pluginId.empty()) return;
    std::string esc;
    for (char c : pluginId) {
        if (c == '\\' || c == '\'') continue;
        esc += c;
    }
    ScriptingEngine::Get().RunString(
        "import rayv_plugins as _rp\n"
        "_rp._open_requests = getattr(_rp, '_open_requests', set())\n"
        "_rp._open_requests.add('" + esc + "')\n");
}

} // namespace script

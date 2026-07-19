#include "ScriptingEngine.h"
#include "ScriptDocApi.h"
#include "ScriptPluginHost.h"
#include "ScriptDockRegistry.h"
#include "ScriptMainThread.h"
#include "ScriptImage.h"
#include "ScriptUiPreview.h"
#include "ScriptViewApi.h"
#include "../core/Logger.h"
#include "../core/ConfigManager.h"
#include "../core/KeymapManager.h"
#include "../core/ops/OperatorRegistry.h"
#include "../core/ops/ActionCatalog.h"
#include "../core/ops/AppContext.h"
#include "../core/BenchmarkRunner.h"
#include "../core/CrashGuard.h"
#include <imgui.h>
#include <pybind11/embed.h>
#include <pybind11/stl.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>

namespace py = pybind11;

// Forward declarations for App/Canvas interactions (main.cpp)
extern void TriggerCanvasResize(int w, int h);
extern float GetCanvasZoom();
extern void SetCanvasZoom(float zoom);
extern void SetCanvasPan(float x, float y);
extern void ResetCanvasView();
extern bool LoadCanvasImage(const std::string& filepath);
extern bool SaveCanvasDDS(const std::string& filepath, int formatChoice);
extern bool SaveCanvasStandard(const std::string& filepath, const std::string& iccProfilePath);
extern int GetCanvasWidth();
extern int GetCanvasHeight();
extern size_t GetActiveLayerTileCount();
extern double GetProcessWorkingSetMiB();

// Define embedded module
PYBIND11_EMBEDDED_MODULE(rayv, m) {
    m.doc() = "RayVPaint Python Scripting API\n"
              "Use rayv.ops.list() / rayv.ops.invoke(id) for editor operators "
              "(same poll rules as hotkeys unless force=True).";

    // Logging Bindings
    m.def("log_debug", [](const std::string& msg) { Logger::Get().Debug("[Python] " + msg); });
    m.def("log_info",  [](const std::string& msg) { Logger::Get().Info("[Python] " + msg); });
    m.def("log_warn",  [](const std::string& msg) { Logger::Get().Warn("[Python] " + msg); });
    m.def("log_error", [](const std::string& msg) { Logger::Get().Error("[Python] " + msg); });

    // Config Manager Bindings
    m.def("get_default_width",  []() { return ConfigManager::Get().GetDefaultWidth(); });
    m.def("get_default_height", []() { return ConfigManager::Get().GetDefaultHeight(); });
    m.def("set_default_width",  [](int w) { ConfigManager::Get().SetDefaultWidth(w); });
    m.def("set_default_height", [](int h) { ConfigManager::Get().SetDefaultHeight(h); });
    m.def("save_config",        []() { return ConfigManager::Get().Save(); });

    // Canvas / Viewer Bindings
    m.def("resize_canvas",      [](int w, int h) { TriggerCanvasResize(w, h); });
    m.def("get_zoom",           []() { return GetCanvasZoom(); });
    m.def("set_zoom",           [](float z) { SetCanvasZoom(z); });
    m.def("set_pan",            [](float x, float y) { SetCanvasPan(x, y); });
    m.def("reset_view",         []() { ResetCanvasView(); });
    m.def("load_image",         [](const std::string& path) { return LoadCanvasImage(path); });
    m.def("save_dds",           [](const std::string& path, int fmt) { return SaveCanvasDDS(path, fmt); });
    m.def("save_image",         [](const std::string& path, const std::string& iccPath) {
        return SaveCanvasStandard(path, iccPath);
    }, py::arg("path"), py::arg("icc_path") = "");
    m.def("get_canvas_width",   []() { return GetCanvasWidth(); });
    m.def("get_canvas_height",  []() { return GetCanvasHeight(); });
    m.def("get_tile_count",     []() { return static_cast<uint64_t>(GetActiveLayerTileCount()); });
    m.def("get_memory_mb",      []() { return GetProcessWorkingSetMiB(); });

    // ------------------------------------------------------------------
    // rayv.ops — operator surface (Phase O4)
    // ------------------------------------------------------------------
    py::module_ ops = m.def_submodule("ops",
        "Editor operators (ActionCatalog + OperatorRegistry).\n"
        "Examples:\n"
        "  rayv.ops.list()\n"
        "  rayv.ops.invoke('SelectAll')\n"
        "  rayv.ops.invoke('FillSecondary', force=True)  # automation");

    ops.def("list", []() {
        py::list out;
        for (const auto& def : core::ops::ActionCatalog::All()) {
            py::dict d;
            d["id"] = def.id ? def.id : "";
            d["label"] = def.label ? def.label : "";
            d["category"] = core::ops::ActionCatalog::CategoryLabel(def.category);
            d["has_execute"] = core::ops::OperatorRegistry::Get().HasExecute(def.id ? def.id : "");
            d["shortcut"] = KeymapManager::Get().GetActionShortcutString(def.id ? def.id : "");
            d["note"] = def.note ? def.note : "";
            bool can = false;
            if (def.id) {
                const auto* ad = core::ops::ActionCatalog::Find(def.id);
                core::ops::ActionScope scope = ad ? ad->scope : core::ops::ActionScope::Document;
                can = core::ops::AppContext::CGet().Allows(scope)
                      && core::ops::OperatorRegistry::Get().HasExecute(def.id);
            }
            d["can_invoke"] = can;
            out.append(d);
        }
        return out;
    }, "List catalog actions with labels, shortcuts, and can_invoke.");

    ops.def("can_invoke", [](const std::string& id) {
        const auto* def = core::ops::ActionCatalog::Find(id);
        if (!def) return false;
        if (!core::ops::OperatorRegistry::Get().HasExecute(id)) return false;
        return core::ops::AppContext::CGet().Allows(def->scope);
    }, py::arg("id"), "True if poll passes and execute is registered.");

    ops.def("invoke", [](const std::string& id, bool force) {
        auto r = core::ops::OperatorRegistry::Get().InvokeForScript(id, force);
        return std::string(core::ops::OperatorRegistry::ResultName(r));
    }, py::arg("id"), py::arg("force") = false,
       "Run operator. Returns 'finished'|'blocked'|'cancelled'|'pass_through'.\n"
       "force=True skips AppContext poll (use for headless tests only).");

    ops.def("has_execute", [](const std::string& id) {
        return core::ops::OperatorRegistry::Get().HasExecute(id);
    }, py::arg("id"));

    // ------------------------------------------------------------------
    // rayv.doc — document / layers / pixels (converter & automation surface)
    // ------------------------------------------------------------------
    py::module_ doc = m.def_submodule("doc",
        "Active document API.\n"
        "Examples:\n"
        "  rayv.doc.width(), rayv.doc.layers()\n"
        "  rayv.doc.get_pixels(0, 0, 0, 64, 64)\n"
        "  rayv.doc.set_pixel(0, 10, 10, 1,0,0,1)\n"
        "  rayv.doc.save_rayp('out.rayp')");

    doc.def("width", &script::Width);
    doc.def("height", &script::Height);
    doc.def("size", []() {
        return py::make_tuple(script::Width(), script::Height());
    });
    doc.def("bit_depth", &script::BitDepth);
    doc.def("path", &script::ProjectPath);
    doc.def("is_modified", &script::IsModified);
    doc.def("tile_size", &script::TileSize);
    doc.def("active_tile_count", &script::ActiveLayerTileCount);

    doc.def("open", &script::Open, py::arg("path"),
            "Open image or .rayp into the active project tab.");
    doc.def("save_rayp", &script::SaveRayp, py::arg("path"));
    doc.def("save_image", &script::SaveImage, py::arg("path"),
            "Save composite as standard image (png/…).");
    doc.def("new_blank", &script::NewBlank, py::arg("w"), py::arg("h"),
            "Resize canvas (extend) to w×h.");

    doc.def("layer_count", &script::LayerCount);
    doc.def("active_layer", &script::ActiveLayerIndex);
    doc.def("set_active_layer", &script::SetActiveLayer, py::arg("index"));
    doc.def("layers", []() {
        py::list out;
        int n = script::LayerCount();
        for (int i = 0; i < n; ++i) {
            py::dict d;
            d["index"] = i;
            d["name"] = script::LayerName(i);
            d["visible"] = script::LayerVisible(i);
            d["opacity"] = script::LayerOpacity(i);
            d["is_group"] = script::LayerIsGroup(i);
            d["has_mask"] = script::LayerHasMask(i);
            d["can_paint"] = script::LayerCanPaintContent(i);
            d["active"] = (i == script::ActiveLayerIndex());
            out.append(d);
        }
        return out;
    }, "List layers as dicts.");

    doc.def("layer_name", &script::LayerName, py::arg("index"));
    doc.def("set_layer_name", &script::SetLayerName, py::arg("index"), py::arg("name"));
    doc.def("set_layer_visible", &script::SetLayerVisible, py::arg("index"), py::arg("visible"));
    doc.def("set_layer_opacity", &script::SetLayerOpacity, py::arg("index"), py::arg("opacity"));
    doc.def("create_layer", &script::CreateLayer, py::arg("name") = "Layer",
            "Create raster layer; returns new index or -1.");
    doc.def("delete_layer", &script::DeleteLayer, py::arg("index"));

    doc.def("get_pixel", [](int layer, int x, int y) {
        float rgba[4] = {0, 0, 0, 0};
        if (!script::GetPixel(layer, x, y, rgba))
            return py::object(py::none());
        return py::object(py::make_tuple(rgba[0], rgba[1], rgba[2], rgba[3]));
    }, py::arg("layer"), py::arg("x"), py::arg("y"),
       "Float RGBA tuple or None.");

    doc.def("set_pixel", &script::SetPixel,
            py::arg("layer"), py::arg("x"), py::arg("y"),
            py::arg("r"), py::arg("g"), py::arg("b"), py::arg("a") = 1.f);

    doc.def("get_pixels", [](int layer, int x, int y, int w, int h) {
        return script::GetPixels(layer, x, y, w, h);
    }, py::arg("layer"), py::arg("x"), py::arg("y"), py::arg("w"), py::arg("h"),
       "Float list length w*h*4 (RGBA). Empty on error.");

    doc.def("set_pixels", [](int layer, int x, int y, int w, int h, const std::vector<float>& rgba) {
        return script::SetPixels(layer, x, y, w, h, rgba);
    }, py::arg("layer"), py::arg("x"), py::arg("y"), py::arg("w"), py::arg("h"), py::arg("rgba"));

    doc.def("get_layer_rgba8", [](int layer) {
        auto v = script::GetLayerRgba8(layer);
        if (v.empty()) return py::bytes();
        return py::bytes(reinterpret_cast<const char*>(v.data()), v.size());
    }, py::arg("layer"), "Full layer as packed RGBA8 bytes (w*h*4).");

    doc.def("set_layer_rgba8", [](int layer, int src_w, int src_h, py::bytes data, int dst_x, int dst_y) {
        std::string s = data;
        std::vector<uint8_t> buf(s.begin(), s.end());
        return script::SetLayerRgba8(layer, src_w, src_h, buf, dst_x, dst_y);
    }, py::arg("layer"), py::arg("src_w"), py::arg("src_h"), py::arg("data"),
       py::arg("dst_x") = 0, py::arg("dst_y") = 0);

    doc.def("has_mask", &script::LayerHasMask, py::arg("layer"));
    doc.def("create_mask", &script::CreateMask, py::arg("layer"));
    doc.def("get_mask", [](int layer) {
        auto v = script::GetMask(layer);
        if (v.empty()) return py::bytes();
        return py::bytes(reinterpret_cast<const char*>(v.data()), v.size());
    }, py::arg("layer"), "Grayscale mask bytes w*h, or empty.");
    doc.def("set_mask", [](int layer, py::bytes data) {
        std::string s = data;
        std::vector<uint8_t> buf(s.begin(), s.end());
        return script::SetMask(layer, buf);
    }, py::arg("layer"), py::arg("data"));

    doc.def("has_selection", &script::HasSelection);
    doc.def("get_selection", []() {
        auto v = script::GetSelection();
        if (v.empty()) return py::bytes();
        return py::bytes(reinterpret_cast<const char*>(v.data()), v.size());
    }, "Selection mask bytes w*h, or empty if none.");
    doc.def("set_selection", [](py::bytes data) {
        std::string s = data;
        std::vector<uint8_t> buf(s.begin(), s.end());
        return script::SetSelection(buf);
    }, py::arg("data"));

    doc.def("notify_changed", &script::NotifyPixelsChanged, py::arg("layer"),
            "Force GPU dirty after external edits.");

    doc.def("export_tiles", [](const std::string& out_dir, const std::string& name_pattern,
                               int tiles_x, int tiles_y, const std::string& mode) {
        std::string err;
        int n = script::ExportCompositeTiles(out_dir, name_pattern, tiles_x, tiles_y, mode, &err);
        py::dict d;
        d["ok"] = (n > 0);
        d["count"] = n;
        d["error"] = err;
        return d;
    }, py::arg("out_dir"), py::arg("name_pattern"), py::arg("tiles_x"), py::arg("tiles_y"),
       py::arg("mode") = "count",
       "Split composite into tiles. mode='count'|'size'. Invalid params → ok=False + log.");

    doc.def("fill_rect", &script::FillRect,
            py::arg("layer"), py::arg("x"), py::arg("y"), py::arg("w"), py::arg("h"),
            py::arg("r"), py::arg("g"), py::arg("b"), py::arg("a") = 1.f,
            "Solid fill on layer; refuses OOB / non-raster.");

    doc.def("begin_edit", &script::BeginEdit, py::arg("layer"),
            "Start undoable pixel session on layer (main thread).");
    doc.def("end_edit", &script::EndEdit, py::arg("action_name") = "Script Edit",
            "Commit pixel session as one Undo step.");
    doc.def("cancel_edit", &script::CancelEdit, "Discard pixel session.");
    doc.def("is_edit_active", &script::IsEditActive);

    doc.def("selection_bounds", []() -> py::object {
        int x, y, w, h;
        if (!script::SelectionBounds(x, y, w, h)) return py::none();
        return py::make_tuple(x, y, w, h);
    }, "Selection AABB (x,y,w,h) or None.");

    doc.def("get_region_for_generate", [](int layer) -> py::object {
        std::vector<float> rgba;
        int x, y, w, h;
        if (!script::GetPixelsSelectionOrFull(layer, rgba, x, y, w, h))
            return py::none();
        py::dict d;
        d["x"] = x; d["y"] = y; d["w"] = w; d["h"] = h;
        d["rgba"] = rgba; // float list
        return d;
    }, py::arg("layer"),
       "Float RGBA region: selection bbox if any, else full layer. For init-image export.");

    // ------------------------------------------------------------------
    // rayv.image — decode/encode without pip packages
    // ------------------------------------------------------------------
    py::module_ image = m.def_submodule("image",
        "Decode/encode images in-process (STB). No PIL/numpy required.");

    image.def("decode", [](py::bytes data) -> py::object {
        std::string s = data;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        std::string err;
        if (!script::ImageDecodeMemory(
                reinterpret_cast<const uint8_t*>(s.data()), s.size(), rgba, w, h, &err)) {
            py::dict d;
            d["ok"] = false;
            d["error"] = err;
            return d;
        }
        py::dict d;
        d["ok"] = true;
        d["width"] = w;
        d["height"] = h;
        d["rgba8"] = py::bytes(reinterpret_cast<const char*>(rgba.data()), rgba.size());
        d["error"] = "";
        return d;
    }, py::arg("data"),
       "Decode PNG/JPG/… bytes → {ok,width,height,rgba8,error}.");

    image.def("encode_png", [](py::bytes rgba8, int w, int h) -> py::object {
        std::string s = rgba8;
        if ((size_t)w * h * 4 != s.size()) {
            py::dict d;
            d["ok"] = false;
            d["error"] = "rgba8 size != w*h*4";
            d["png"] = py::bytes();
            return d;
        }
        std::vector<uint8_t> png;
        std::string err;
        if (!script::ImageEncodePng(
                reinterpret_cast<const uint8_t*>(s.data()), w, h, w * 4, png, &err)) {
            py::dict d;
            d["ok"] = false;
            d["error"] = err;
            d["png"] = py::bytes();
            return d;
        }
        py::dict d;
        d["ok"] = true;
        d["error"] = "";
        d["png"] = py::bytes(reinterpret_cast<const char*>(png.data()), png.size());
        return d;
    }, py::arg("rgba8"), py::arg("w"), py::arg("h"),
       "Encode RGBA8 → {ok,png,error}.");

    image.def("load_file", [](const std::string& path) -> py::object {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        std::string err;
        if (!script::ImageLoadFile(path, rgba, w, h, &err)) {
            py::dict d; d["ok"] = false; d["error"] = err; return d;
        }
        py::dict d;
        d["ok"] = true; d["width"] = w; d["height"] = h;
        d["rgba8"] = py::bytes(reinterpret_cast<const char*>(rgba.data()), rgba.size());
        d["error"] = "";
        return d;
    }, py::arg("path"));

    image.def("save_file", [](const std::string& path, py::bytes rgba8, int w, int h) {
        std::string s = rgba8;
        std::string err;
        bool ok = (size_t)w * h * 4 == s.size() &&
                  script::ImageSaveFile(path, reinterpret_cast<const uint8_t*>(s.data()), w, h, &err);
        py::dict d; d["ok"] = ok; d["error"] = err; return d;
    }, py::arg("path"), py::arg("rgba8"), py::arg("w"), py::arg("h"));

    // ------------------------------------------------------------------
    // rayv.host — threading contract
    // ------------------------------------------------------------------
    py::module_ host = m.def_submodule("host",
        "Host threading helpers. Document mutations only on main thread.");

    host.def("is_main_thread", &script::IsMainThread,
             "True if this is the RayV UI/main thread.");
    host.def("pending_jobs", []() { return (int)script::MainThreadJobCount(); });
    host.def("call_on_main", [](py::function fn) {
        // Keep Python callable alive until main runs it.
        auto hold = std::make_shared<py::object>(std::move(fn));
        return script::PostToMainThread([hold]() {
            try {
                py::gil_scoped_acquire gil;
                (*hold)();
            } catch (const py::error_already_set& e) {
                Logger::Get().ErrorTag("script", std::string("call_on_main Python error: ") + e.what());
            } catch (const std::exception& e) {
                Logger::Get().ErrorTag("script", std::string("call_on_main: ") + e.what());
            }
        });
    }, py::arg("fn"),
       "Queue callable to run on main thread (next frame drain). Thread-safe.\n"
       "Use from worker threads after HTTP/Comfy completes.");

    // ------------------------------------------------------------------
    // rayv.ui — thin Dear ImGui wrappers for Python plugin windows
    // ------------------------------------------------------------------
    py::module_ ui = m.def_submodule("ui",
        "Dear ImGui helpers for plugin dialogs. Call only from plugin on_ui().");

    ui.def("begin", [](const std::string& title, bool is_open) {
        bool o = is_open;
        bool show = ImGui::Begin(title.c_str(), &o);
        return py::make_tuple(show, o);
    }, py::arg("title"), py::arg("is_open") = true,
       "Returns (visible, is_open). Always call end() if visible. Windows are dockable.");

    ui.def("end", []() { ImGui::End(); });

    // Absolute placement (call before begin / begin_overlay)
    ui.def("set_next_window_pos", [](float x, float y, float pivot_x, float pivot_y) {
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always, ImVec2(pivot_x, pivot_y));
    }, py::arg("x"), py::arg("y"), py::arg("pivot_x") = 0.f, py::arg("pivot_y") = 0.f,
       "Screen coords. pivot 0..1 (0.5,1 = bottom-center of window at x,y).");

    ui.def("set_next_window_size", [](float w, float h) {
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    }, py::arg("w"), py::arg("h"));

    ui.def("set_next_window_bg_alpha", [](float alpha) {
        ImGui::SetNextWindowBgAlpha(std::clamp(alpha, 0.f, 1.f));
    }, py::arg("alpha"));

    // Floating HUD over canvas (no dock). Pair with set_next_window_pos + view.selection_screen_rect.
    ui.def("begin_overlay", [](const std::string& id, bool is_open) {
        bool o = is_open;
        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
        // Stable id: use ### so label can be empty chrome
        std::string name = std::string("###overlay_") + id;
        bool show = ImGui::Begin(name.c_str(), is_open ? &o : nullptr, flags);
        return py::make_tuple(show, o);
    }, py::arg("id"), py::arg("is_open") = true,
       "Non-docked floating chrome. Returns (visible, is_open). Call end_overlay() when visible.");

    ui.def("end_overlay", []() { ImGui::End(); });

    // ---- Script docks (registered panels in dockspace + View menu) ----
    ui.def("register_dock", [](const std::string& dock_id, const std::string& title, bool default_open) {
        return script::ScriptDockRegistry::Get().Register(dock_id, title, default_open);
    }, py::arg("dock_id"), py::arg("title") = "", py::arg("default_open") = true,
       "Register a dockable panel. Call once from on_load(). Draw with begin_dock/end_dock.");

    ui.def("unregister_dock", [](const std::string& dock_id) {
        script::ScriptDockRegistry::Get().Unregister(dock_id);
    }, py::arg("dock_id"));

    ui.def("begin_dock", [](const std::string& dock_id) {
        bool vis = false;
        bool began = script::ScriptDockRegistry::Get().Begin(dock_id, &vis);
        // Returns (visible, is_open) — is_open false if user closed dock
        bool open = script::ScriptDockRegistry::Get().IsOpen(dock_id);
        if (!began)
            return py::make_tuple(false, open);
        return py::make_tuple(true, open);
    }, py::arg("dock_id"),
       "Begin registered dock panel. Returns (visible, is_open). Call end_dock() when visible.");

    ui.def("end_dock", []() {
        script::ScriptDockRegistry::Get().End();
    });

    ui.def("set_dock_open", [](const std::string& dock_id, bool open) {
        return script::ScriptDockRegistry::Get().SetOpen(dock_id, open);
    }, py::arg("dock_id"), py::arg("open"));

    ui.def("list_docks", []() {
        py::list out;
        for (const auto& d : script::ScriptDockRegistry::Get().List()) {
            py::dict x;
            x["id"] = d.id;
            x["title"] = d.title;
            x["open"] = d.open;
            out.append(x);
        }
        return out;
    });

    ui.def("open_popup", [](const std::string& id) {
        ImGui::OpenPopup(id.c_str());
    }, py::arg("id"));

    ui.def("begin_popup_modal", [](const std::string& title, bool is_open) {
        bool o = is_open;
        bool show = false;
        if (o)
            show = ImGui::BeginPopupModal(title.c_str(), &o, ImGuiWindowFlags_AlwaysAutoResize);
        return py::make_tuple(show, o);
    }, py::arg("title"), py::arg("is_open") = true,
       "Returns (visible, is_open). Call end_popup() when visible.");

    ui.def("end_popup", []() { ImGui::EndPopup(); });

    ui.def("text", [](const std::string& s) { ImGui::TextUnformatted(s.c_str()); });
    ui.def("text_wrapped", [](const std::string& s) { ImGui::TextWrapped("%s", s.c_str()); });
    ui.def("text_disabled", [](const std::string& s) { ImGui::TextDisabled("%s", s.c_str()); });
    ui.def("separator", []() { ImGui::Separator(); });
    ui.def("spacing", []() { ImGui::Spacing(); });
    ui.def("same_line", []() { ImGui::SameLine(); });

    ui.def("button", [](const std::string& label) {
        return ImGui::Button(label.c_str());
    }, py::arg("label"));

    ui.def("checkbox", [](const std::string& label, bool v) {
        bool x = v;
        bool ch = ImGui::Checkbox(label.c_str(), &x);
        return py::make_tuple(ch, x);
    }, py::arg("label"), py::arg("value"));

    ui.def("input_int", [](const std::string& label, int v) {
        int x = v;
        bool ch = ImGui::InputInt(label.c_str(), &x);
        return py::make_tuple(ch, x);
    }, py::arg("label"), py::arg("value"));

    ui.def("input_text", [](const std::string& label, const std::string& v, int buf_size) {
        std::vector<char> buf((size_t)std::max(32, buf_size), 0);
        size_t n = std::min(v.size(), buf.size() - 1);
        if (n) std::memcpy(buf.data(), v.data(), n);
        bool ch = ImGui::InputText(label.c_str(), buf.data(), buf.size());
        return py::make_tuple(ch, std::string(buf.data()));
    }, py::arg("label"), py::arg("value"), py::arg("buf_size") = 512);

    // Multi-line text field (prompts, notes). Returns (changed, value).
    ui.def("input_text_multiline", [](const std::string& label, const std::string& v,
                                      int buf_size, float height) {
        std::vector<char> buf((size_t)std::max(64, buf_size), 0);
        size_t n = std::min(v.size(), buf.size() - 1);
        if (n) std::memcpy(buf.data(), v.data(), n);
        ImVec2 size(-1.f, height > 1.f ? height : 80.f);
        bool ch = ImGui::InputTextMultiline(label.c_str(), buf.data(), buf.size(), size);
        return py::make_tuple(ch, std::string(buf.data()));
    }, py::arg("label"), py::arg("value"), py::arg("buf_size") = 4096, py::arg("height") = 80.f,
       "Multi-line text. Call from on_ui only. Returns (changed, value).");

    ui.def("combo", [](const std::string& label, int current, const std::vector<std::string>& items) {
        int cur = current;
        std::vector<const char*> ptrs;
        ptrs.reserve(items.size());
        for (const auto& s : items) ptrs.push_back(s.c_str());
        bool ch = false;
        if (!ptrs.empty())
            ch = ImGui::Combo(label.c_str(), &cur, ptrs.data(), (int)ptrs.size());
        return py::make_tuple(ch, cur);
    }, py::arg("label"), py::arg("current"), py::arg("items"));

    ui.def("progress_bar", [](float fraction, const std::string& overlay) {
        float f = std::clamp(fraction, 0.f, 1.f);
        ImGui::ProgressBar(f, ImVec2(-1.f, 0.f), overlay.empty() ? nullptr : overlay.c_str());
    }, py::arg("fraction"), py::arg("overlay") = "",
       "fraction in 0..1. Call from on_ui only.");

    ui.def("set_image_rgba8", [](const std::string& key, py::bytes rgba8, int w, int h) {
        std::string s = rgba8;
        if ((size_t)w * h * 4 != s.size()) {
            Logger::Get().ErrorTag("script.ui", "set_image_rgba8: size mismatch");
            return false;
        }
        return script::UiPreviewSetRgba8(key, reinterpret_cast<const uint8_t*>(s.data()), w, h);
    }, py::arg("key"), py::arg("rgba8"), py::arg("w"), py::arg("h"),
       "Upload/update GPU preview texture (main thread / on_ui).");

    ui.def("image", [](const std::string& key, float display_w, float display_h) {
        return script::UiPreviewDraw(key, display_w, display_h);
    }, py::arg("key"), py::arg("display_w") = 0.f, py::arg("display_h") = 0.f,
       "Draw preview previously set via set_image_rgba8.");

    ui.def("destroy_image", [](const std::string& key) {
        script::UiPreviewDestroy(key);
    }, py::arg("key"));

    // ------------------------------------------------------------------
    // rayv.view — canvas viewport transform (doc ↔ screen)
    // ------------------------------------------------------------------
    py::module_ view = m.def_submodule("view",
        "Canvas viewport transform. Valid after the host lays out the canvas each frame.\n"
        "Use with ui.set_next_window_pos + ui.begin_overlay for selection-context HUD.");

    view.def("valid", &script::IsViewValid,
        "False on first frames / headless / empty viewport.");

    view.def("zoom", &script::ViewZoom);
    view.def("pan", []() {
        return py::make_tuple(script::ViewPanX(), script::ViewPanY());
    });
    view.def("rotation_rad", &script::ViewRotationRad);

    view.def("viewport_rect", []() -> py::object {
        float x, y, w, h;
        if (!script::ViewportScreenRect(x, y, w, h))
            return py::none();
        return py::make_tuple(x, y, w, h);
    }, "Canvas image rect in screen space: (x, y, w, h) or None.");

    view.def("doc_to_screen", [](float doc_x, float doc_y) -> py::object {
        float sx, sy;
        if (!script::DocToScreen(doc_x, doc_y, sx, sy))
            return py::none();
        return py::make_tuple(sx, sy);
    }, py::arg("doc_x"), py::arg("doc_y"),
       "Document pixel → absolute screen coords (sx, sy) or None.");

    view.def("screen_to_doc", [](float sx, float sy) -> py::object {
        float dx, dy;
        if (!script::ScreenToDoc(sx, sy, dx, dy))
            return py::none();
        return py::make_tuple(dx, dy);
    }, py::arg("sx"), py::arg("sy"),
       "Screen → document pixel float (x, y) or None.");

    view.def("selection_screen_rect", []() -> py::object {
        float x, y, w, h;
        if (!script::SelectionScreenRect(x, y, w, h))
            return py::none();
        return py::make_tuple(x, y, w, h);
    }, "Screen AABB of selection (maps 4 doc corners). None if no selection / invalid view.");

    // ------------------------------------------------------------------
    // rayv.plugins — reload / open / list
    // ------------------------------------------------------------------
    py::module_ plugins = m.def_submodule("plugins", "Built-in + user Python plugins");
    plugins.def("reload", []() {
        std::string sum;
        bool ok = script::ScriptPluginHost::Get().Reload(&sum);
        return py::make_tuple(ok, sum);
    });
    plugins.def("open", [](const std::string& id) {
        script::ScriptPluginHost::Get().RequestOpen(id);
    }, py::arg("id"));
    plugins.def("builtin_dir", &script::ScriptPluginHost::BuiltinScriptsDir);
    plugins.def("user_dir", &script::ScriptPluginHost::UserScriptsDir);
    plugins.def("list", []() {
        py::list out;
        for (const auto& p : script::ScriptPluginHost::Get().List()) {
            py::dict d;
            d["id"] = p.id;
            d["title"] = p.title;
            d["path"] = p.path;
            d["source"] = p.source;
            out.append(d);
        }
        return out;
    });
}

bool ScriptingEngine::ReloadPlugins(std::string* outSummary) {
    return script::ScriptPluginHost::Get().Reload(outSummary);
}

static void DrawPluginsUiFn(void*) {
    script::ScriptPluginHost::Get().DrawAllUi();
}

void ScriptingEngine::DrawPluginsUi() {
    // Must run on main thread during ImGui frame (after NewFrame).
    if (!m_Initialized) return;
    if (!script::ScriptPluginHost::Get().Loaded()) return;
    if (script::ScriptPluginHost::Get().UiDisabled()) return;
    if (!ImGui::GetCurrentContext()) return;
    // Benchmark stress must not fight Python/ImGui plugin frames.
    if (BenchmarkRunner::Get().IsActive()) return;
    try {
        if (!CrashGuard::RunUnderSeh(&DrawPluginsUiFn, nullptr, "DrawPluginsUi")) {
            Logger::Get().ErrorTag("script", "DrawPluginsUi SEH fault — plugin UI disabled for this session");
            Logger::Get().Flush();
            script::ScriptPluginHost::Get().DisableUi();
        }
    } catch (const std::exception& e) {
        Logger::Get().ErrorTag("script", std::string("DrawPluginsUi C++ exception: ") + e.what());
        Logger::Get().Flush();
        script::ScriptPluginHost::Get().DisableUi();
    } catch (...) {
        Logger::Get().ErrorTag("script", "DrawPluginsUi unknown C++ exception");
        Logger::Get().Flush();
        script::ScriptPluginHost::Get().DisableUi();
    }
}

ScriptingEngine& ScriptingEngine::Get() {
    static ScriptingEngine instance;
    return instance;
}

ScriptingEngine::~ScriptingEngine() {
    Shutdown();
}

bool ScriptingEngine::Initialize() {
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (m_Initialized) return true;

    try {
        Logger::Get().Info("Initializing embedded Python interpreter...");
#ifdef _WIN32
        _putenv_s("PYTHONUNBUFFERED", "1");
#endif
        py::initialize_interpreter();

        py::exec("import sys; import rayv; rayv.log_info(f'Python Interpreter Initialized. Version: {sys.version}')");
        m_Initialized = true;
        return true;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Failed to initialize Python scripting engine: " + std::string(e.what()));
        try {
            Logger::Get().Warn("Retrying Python interpreter initialization...");
            py::initialize_interpreter();
            py::exec("import sys; import rayv; rayv.log_info(f'Python Interpreter Initialized (retry). Version: {sys.version}')");
            m_Initialized = true;
            return true;
        } catch (const std::exception& e2) {
            Logger::Get().Error("Python init retry failed: " + std::string(e2.what()));
            return false;
        }
    }
}

void ScriptingEngine::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (!m_Initialized) return;

    try {
        Logger::Get().Info("Shutting down embedded Python interpreter...");
        py::finalize_interpreter();
        m_Initialized = false;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Error shutting down Python scripting engine: " + std::string(e.what()));
    }
}

bool ScriptingEngine::RunString(const std::string& code) {
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (!m_Initialized) {
        Logger::Get().Error("Scripting engine not initialized.");
        return false;
    }

    try {
        py::exec(code);
        return true;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Python execution error: " + std::string(e.what()));
        return false;
    }
}

bool ScriptingEngine::RunScript(const std::string& filepath) {
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (!m_Initialized) {
        Logger::Get().Error("Scripting engine not initialized.");
        return false;
    }

    std::ifstream file(filepath);
    if (!file.is_open()) {
        Logger::Get().Error("Failed to open Python script file: " + filepath);
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    try {
        Logger::Get().Info("Executing Python script: " + filepath);
        py::exec(buffer.str());
        return true;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Python execution error in script " + filepath + ": " + std::string(e.what()));
        return false;
    }
}

#pragma once
#include "UiTokens.h"
#include <imgui.h>
#include <cmath>
#include <algorithm>
#include <unordered_map>

namespace Ui {

inline float Clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

// Pure easing curves. Prefer EaseOut* for open/release; EaseIn* for close.
float Ease(EaseKind kind, float t01);

struct AnimFloat {
    float value = 0.f;
    float from = 0.f;
    float to = 0.f;
    float duration = 0.2f;
    float elapsed = 0.f;
    EaseKind ease = EaseKind::EaseOutCubic;
    bool active = false;

    void Snap(float v) {
        value = from = to = v;
        elapsed = duration;
        active = false;
    }

    void SetTarget(float target, float dur, EaseKind e) {
        if (std::fabs(target - to) < 1e-5f && active) return;
        from = value;
        to = target;
        duration = std::max(0.001f, dur);
        elapsed = 0.f;
        ease = e;
        active = true;
    }

    void Update(float dt) {
        if (!active) return;
        elapsed += dt;
        float p = Clampf(elapsed / duration, 0.f, 1.f);
        value = from + (to - from) * Ease(ease, p);
        if (p >= 1.f) {
            value = to;
            active = false;
        }
    }
};

// 0 closed → 1 open
struct AnimBool {
    AnimFloat progress;
    bool targetOpen = false;

    float Value() const { return progress.value; }
    bool IsOpen() const { return targetOpen || progress.value > 0.001f; }
    bool IsFullyOpen() const { return targetOpen && progress.value > 0.999f; }

    void SetOpen(bool open, float durOpen, float durClose) {
        if (open == targetOpen && !progress.active) return;
        targetOpen = open;
        if (open)
            progress.SetTarget(1.f, durOpen, EaseKind::EaseOutCubic);
        else
            progress.SetTarget(0.f, durClose, EaseKind::EaseInCubic);
    }

    void Snap(bool open) {
        targetOpen = open;
        progress.Snap(open ? 1.f : 0.f);
    }

    void Update(float dt) { progress.Update(dt); }
};

inline float DeltaTime() {
    float dt = ImGui::GetIO().DeltaTime;
    return Clampf(dt, 0.f, 0.05f);
}

// Per-widget anim state keyed by ImGuiID
template<typename T>
T& AnimState(ImGuiID id) {
    static std::unordered_map<ImGuiID, T> map;
    return map[id];
}

} // namespace Ui

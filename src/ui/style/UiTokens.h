#pragma once
#include <imgui.h>
#include <cstdint>
#include <string>

// Central design tokens — edit look & feel here (Stage 1 kit).
namespace Ui {

enum class EaseKind : uint8_t {
    Linear = 0, // only for internal math; avoid as final UI motion
    EaseInCubic,
    EaseOutCubic,
    EaseInOutCubic,
    EaseOutQuint,
    EaseOutBack, // bounce release
};

struct UiTokens {
    // Colors (RGBA 0..1)
    ImVec4 bgWindow      = {0.09f, 0.09f, 0.10f, 1.00f};
    ImVec4 bgElevated    = {0.14f, 0.14f, 0.16f, 0.92f};
    ImVec4 bgElevatedSoft= {0.16f, 0.16f, 0.18f, 0.78f};
    ImVec4 strokeHairline= {1.00f, 1.00f, 1.00f, 0.10f};
    ImVec4 strokeActive  = {0.35f, 0.55f, 1.00f, 0.95f};
    ImVec4 accent        = {0.30f, 0.50f, 0.95f, 1.00f};
    ImVec4 textPrimary   = {0.95f, 0.95f, 0.97f, 1.00f};
    ImVec4 textSecondary = {0.70f, 0.70f, 0.74f, 1.00f};
    ImVec4 textDisabled  = {0.45f, 0.45f, 0.48f, 1.00f};
    ImVec4 danger        = {0.90f, 0.30f, 0.28f, 1.00f};
    ImVec4 iconTint      = {1.00f, 1.00f, 1.00f, 1.00f}; // dark theme → white
    ImVec4 iconTintMuted = {1.00f, 1.00f, 1.00f, 0.40f};
    ImVec4 scrim         = {0.00f, 0.00f, 0.00f, 0.35f};

    // Radii (px)
    float rSm = 6.0f;
    float rMd = 10.0f;
    float rLg = 14.0f;

    // Spacing
    float s1 = 4.0f, s2 = 8.0f, s3 = 12.0f, s4 = 16.0f, s5 = 20.0f;

    // Icons
    float iconSm = 16.0f;
    float iconMd = 22.0f;
    float iconLg = 28.0f;
    float iconDock = 32.0f;

    // Motion durations (seconds)
    float durFast = 0.12f;
    float durMed  = 0.20f;
    float durSlow = 0.32f;

    // Press / bounce
    float pressScale = 0.90f;
    float bounceOvershoot = 1.06f;

    // Dropdown (hold-to-select threshold)
    float holdThresholdSec = 0.18f;
    float dropdownPanelAlpha = 0.94f;

    bool isDark = true;

    void ApplyDark();
    void ApplyLight();
    void ApplyFromThemeName(const std::string& themeName);

    // Apply soft ImGui style rounding/padding from tokens
    void ApplyToImGuiStyle(ImGuiStyle& style) const;

    ImU32 ColU32(const ImVec4& c) const {
        return ImGui::ColorConvertFloat4ToU32(c);
    }
};

// Global tokens singleton
UiTokens& Tokens();

} // namespace Ui

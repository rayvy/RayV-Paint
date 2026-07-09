#include "UiTokens.h"

namespace Ui {

UiTokens& Tokens() {
    static UiTokens t;
    return t;
}

void UiTokens::ApplyDark() {
    isDark = true;
    bgWindow       = {0.09f, 0.09f, 0.10f, 1.00f};
    bgElevated     = {0.16f, 0.16f, 0.18f, 0.94f};
    bgElevatedSoft = {0.18f, 0.18f, 0.20f, 0.80f};
    strokeHairline = {1.00f, 1.00f, 1.00f, 0.12f};
    strokeActive   = {0.40f, 0.60f, 1.00f, 1.00f};
    accent         = {0.35f, 0.55f, 0.98f, 1.00f};
    textPrimary    = {0.96f, 0.96f, 0.98f, 1.00f};
    textSecondary  = {0.68f, 0.68f, 0.72f, 1.00f};
    textDisabled   = {0.42f, 0.42f, 0.46f, 1.00f};
    iconTint       = {1.00f, 1.00f, 1.00f, 1.00f};
    iconTintMuted  = {1.00f, 1.00f, 1.00f, 0.38f};
    scrim          = {0.00f, 0.00f, 0.00f, 0.40f};
}

void UiTokens::ApplyLight() {
    isDark = false;
    bgWindow       = {0.95f, 0.95f, 0.96f, 1.00f};
    bgElevated     = {1.00f, 1.00f, 1.00f, 0.96f};
    bgElevatedSoft = {0.98f, 0.98f, 0.99f, 0.88f};
    strokeHairline = {0.00f, 0.00f, 0.00f, 0.12f};
    strokeActive   = {0.20f, 0.40f, 0.85f, 1.00f};
    accent         = {0.26f, 0.42f, 0.85f, 1.00f};
    textPrimary    = {0.10f, 0.10f, 0.12f, 1.00f};
    textSecondary  = {0.40f, 0.40f, 0.44f, 1.00f};
    textDisabled   = {0.60f, 0.60f, 0.64f, 1.00f};
    iconTint       = {0.00f, 0.00f, 0.00f, 1.00f};
    iconTintMuted  = {0.00f, 0.00f, 0.00f, 0.35f};
    scrim          = {0.00f, 0.00f, 0.00f, 0.25f};
}

void UiTokens::ApplyFromThemeName(const std::string& themeName) {
    if (themeName == "Light") ApplyLight();
    else ApplyDark(); // Dark + Classic share dark icon tint
}

void UiTokens::ApplyToImGuiStyle(ImGuiStyle& style) const {
    style.WindowRounding = rMd;
    style.ChildRounding = rSm;
    style.FrameRounding = rSm;
    style.PopupRounding = rMd;
    style.ScrollbarRounding = rLg;
    style.GrabRounding = rSm;
    style.TabRounding = rSm;
    style.WindowPadding = ImVec2(s3, s3);
    style.FramePadding = ImVec2(s2, s2 * 0.75f);
    style.ItemSpacing = ImVec2(s2, s2 * 0.75f);
    style.ItemInnerSpacing = ImVec2(s2 * 0.75f, s2 * 0.75f);
}

} // namespace Ui

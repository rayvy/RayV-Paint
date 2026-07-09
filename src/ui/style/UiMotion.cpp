#include "UiMotion.h"

namespace Ui {

float Ease(EaseKind kind, float t01) {
    float t = Clampf(t01, 0.f, 1.f);
    switch (kind) {
    case EaseKind::Linear:
        return t;
    case EaseKind::EaseInCubic:
        return t * t * t;
    case EaseKind::EaseOutCubic: {
        float u = 1.f - t;
        return 1.f - u * u * u;
    }
    case EaseKind::EaseInOutCubic:
        return t < 0.5f
            ? 4.f * t * t * t
            : 1.f - std::pow(-2.f * t + 2.f, 3.f) * 0.5f;
    case EaseKind::EaseOutQuint: {
        float u = 1.f - t;
        return 1.f - u * u * u * u * u;
    }
    case EaseKind::EaseOutBack: {
        // Soft overshoot (Apple-ish release bounce)
        const float c1 = 1.70158f;
        const float c3 = c1 + 1.f;
        float u = t - 1.f;
        return 1.f + c3 * u * u * u + c1 * u * u;
    }
    }
    return t;
}

} // namespace Ui

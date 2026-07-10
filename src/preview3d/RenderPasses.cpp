#include "RenderPasses.h"

namespace preview3d {

const char* RenderPassName(RenderPassId id) {
    switch (id) {
    case RenderPassId::Main: return "Main";
    case RenderPassId::Glow: return "Glow";
    case RenderPassId::OutlineZZZ: return "Outline (ZZZ)";
    case RenderPassId::OutlineGI: return "Outline (GI)";
    case RenderPassId::Bloom: return "Bloom";
    default: return "?";
    }
}

} // namespace preview3d

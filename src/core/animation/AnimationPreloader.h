#pragma once

namespace cm {
namespace animation {

struct AnimationPlayerComponent;

// Preloads animation clips and controllers referenced by the component so that
// runtime evaluation does not incur first-frame stalls. Safe to call multiple times.
void PreloadAnimatorComponent(AnimationPlayerComponent& component);

} // namespace animation
} // namespace cm



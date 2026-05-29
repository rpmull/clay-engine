#pragma once

#include <memory>
#include <string>

namespace cm {
namespace animation {

struct AnimatorControllerOverrideAsset;

std::shared_ptr<AnimatorControllerOverrideAsset> LoadAnimatorControllerOverrideFromFile(const std::string& path);
bool SaveAnimatorControllerOverride(const AnimatorControllerOverrideAsset& asset, const std::string& path);

} // namespace animation
} // namespace cm

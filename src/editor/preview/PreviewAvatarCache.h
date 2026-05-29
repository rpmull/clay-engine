// PreviewAvatarCache.h
#pragma once

#include <optional>
#include <string>
#include <tuple>

#include "editor/import/ModelLoader.h"           // Model
#include "core/ecs/AnimationComponents.h"         // SkeletonComponent
#include "core/animation/AvatarDefinition.h"      // AvatarDefinition
#include "core/animation/AnimationTypes.h"        // AnimationClip

class PreviewAvatarCache {
public:
    // Returns (Model, Skeleton, HumanoidMap?)
    std::tuple<Model, SkeletonComponent, const cm::animation::AvatarDefinition*>
    ResolveForClip(const cm::animation::AnimationClip& clip);
};



#pragma once
#include <ecs/Scene.h>
#include "core/rendering/SkinnedPBRMaterial.h"

class SkinningSystem {
public:
    static void Update(Scene& scene, float dt = 1.0f / 60.0f);
};

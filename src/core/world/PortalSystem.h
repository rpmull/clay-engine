#pragma once
#include "core/ecs/Scene.h"

namespace cm::world {

class PortalSystem
{
public:
    static PortalSystem& Get()
    {
        static PortalSystem s_Instance;
        return s_Instance;
    }

    void Update(Scene& scene, float dt);
    static void RebuildRuntimePortals(Scene& scene);

private:
    PortalSystem() = default;
};

} // namespace cm::world

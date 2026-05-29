#pragma once

#include <vector>

namespace cm::interop {

struct EntityInteropLayout {
    std::vector<void*> args;
    int entityCoreCount = 0;
    int tweenCount = 0;
    int componentCount = 0;
    int moduleCount = 0;
    int sceneCount = 0;
};

EntityInteropLayout BuildEntityInteropLayout();
int GetExpectedEntityInteropTotal(const EntityInteropLayout& layout);

} // namespace cm::interop

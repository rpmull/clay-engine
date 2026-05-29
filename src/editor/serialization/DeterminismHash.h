#pragma once
#include <cstdint>
#include "core/ecs/Scene.h"

namespace cm { namespace editor {

struct HashOptions { bool includeChildOrder = true; };
uint64_t HashSubtree(const Entity& e, const HashOptions& opts);
uint64_t HashScene(const Scene& s, const HashOptions& opts);

}} // namespace cm::editor



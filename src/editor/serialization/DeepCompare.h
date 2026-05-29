#pragma once
#include <vector>
#include "DiffTypes.h"
#include "core/ecs/Scene.h"

namespace cm { namespace editor {

std::vector<Diff> DeepCompare(const Scene& a, const Scene& b, float floatEps);

}} // namespace cm::editor



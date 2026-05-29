#pragma once
#include <cstdint>
#include "core/ecs/Scene.h"

namespace cm { namespace editor {

struct FuzzOptions { int passes = 10; uint64_t seed = 0; };
int FuzzScene(Scene& s, const FuzzOptions& opts);

}} // namespace cm::editor



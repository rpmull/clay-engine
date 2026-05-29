#pragma once

#include <nlohmann/json.hpp>
#include "core/rendering/MaterialSource.h"

namespace material_serialization
{
    nlohmann::json ToJson(const MaterialSource& source);
    MaterialSource FromJson(const nlohmann::json& node);
}



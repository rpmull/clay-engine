#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "ScriptableObject.h"

namespace scriptableio {

// Load from a .asset JSON file
ScriptableObject* LoadFromFile(const std::string& path);

// Save to a .asset JSON file
bool SaveToFile(const ScriptableObject& obj, const std::string& path, uint32_t version);

}



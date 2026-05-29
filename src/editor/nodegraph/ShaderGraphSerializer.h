#pragma once

#include "ShaderGraph.h"
#include <string>
#include <nlohmann/json.hpp>

namespace shadergraph {

class ShaderGraphSerializer {
public:
    // Save a shader graph to a JSON file (.shgraph)
    static bool SaveToFile(const ShaderGraph& graph, const std::string& path);
    
    // Load a shader graph from a JSON file
    static bool LoadFromFile(const std::string& path, ShaderGraph& outGraph);
    
    // Convert to/from JSON
    static void ToJson(const ShaderGraph& graph, nlohmann::json& j);
    static void FromJson(const nlohmann::json& j, ShaderGraph& graph);
    
private:
    static const int kCurrentVersion;
};

} // namespace shadergraph


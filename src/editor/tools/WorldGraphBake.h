#pragma once
#include <string>

namespace cm::editor::worldgraph {

// Returns true on success. Writes .bin/world/worldgraph.json
bool BakeWorldGraph();
// Returns true on success. Re-bakes the seed scene and connected scenes only.
bool BakeWorldGraphFromScene(const std::string& seedScenePath);

} // namespace cm::editor::worldgraph

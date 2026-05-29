#pragma once

#include "GraphCore.h"
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

namespace nodegraph
{
   struct EditorMeta
   {
      std::unordered_map<NodeId, std::pair<float, float>> nodePositions;
      float panX{0.0f};
      float panY{0.0f};
      float zoom{1.0f};
   };

   struct GraphAsset
   {
      Graph graph;
      EditorMeta editor;
      // Arbitrary node payloads, keyed by NodeId
      std::unordered_map<NodeId, nlohmann::json> nodePayloads;
   };

   class GraphSerializer
   {
   public:
      static bool SaveToFile(const GraphAsset& asset, const std::string& path);
      static bool LoadFromFile(const std::string& path, GraphAsset& outAsset);

      static void ToJson(const GraphAsset& asset, json& j);
      static void FromJson(const json& j, GraphAsset& out);
   };
}



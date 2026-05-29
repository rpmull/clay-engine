#include "GraphSerializer.h"
#include <fstream>
#include <unordered_map>

namespace nodegraph
{
   static const int kCurrentVersion = 1;

   static const char* ToString(PinValueType t)
   {
      switch (t)
      {
      case PinValueType::Bool: return "Bool";
      case PinValueType::Int: return "Int";
      case PinValueType::Float: return "Float";
      case PinValueType::String: return "String";
      case PinValueType::Entity: return "Entity";
      case PinValueType::Event: return "Event";
      case PinValueType::Any: default: return "Any";
      }
   }

   static PinValueType ParsePinValueType(const std::string& s)
   {
      if (s == "Bool") return PinValueType::Bool;
      if (s == "Int") return PinValueType::Int;
      if (s == "Float") return PinValueType::Float;
      if (s == "String") return PinValueType::String;
      if (s == "Entity") return PinValueType::Entity;
      if (s == "Event") return PinValueType::Event;
      return PinValueType::Any;
   }

   void GraphSerializer::ToJson(const GraphAsset& asset, json& j)
   {
      const Graph& g = asset.graph;
      j["version"] = kCurrentVersion;
      j["name"] = g.name;

      // Nodes
      json jnodes = json::array();
      for (const auto& n : g.nodes)
      {
         json jn;
         jn["id"] = n.id;
         jn["typeId"] = n.typeId;
         jn["name"] = n.name;
         jn["inputs"] = n.inputs;
         jn["outputs"] = n.outputs;
         jnodes.push_back(std::move(jn));
      }
      j["nodes"] = std::move(jnodes);

      // Pins
      json jpins = json::array();
      for (const auto& p : g.pins)
      {
         json jp;
         jp["id"] = p.id;
         jp["nodeId"] = p.nodeId;
         jp["kind"] = (p.kind == PinKind::Input ? "Input" : "Output");
         jp["PinValueType"] = ToString(p.type);
         jp["name"] = p.name;
         jpins.push_back(std::move(jp));
      }
      j["pins"] = std::move(jpins);

      // Links
      json jlinks = json::array();
      for (const auto& l : g.links)
      {
         json jl;
         jl["id"] = l.id;
         jl["fromPin"] = l.from;
         jl["toPin"] = l.to;
         jlinks.push_back(std::move(jl));
      }
      j["links"] = std::move(jlinks);

      // Counters
      j["nextIds"] = { {"node", g.nextNodeId}, {"pin", g.nextPinId}, {"link", g.nextLinkId} };

      // Payloads
      if (!asset.nodePayloads.empty())
      {
         json jpayloads = json::object();
         for (const auto& kv : asset.nodePayloads)
         {
            jpayloads[std::to_string(kv.first)] = kv.second;
         }
         j["payloads"] = std::move(jpayloads);
      }

      // Editor meta
      json jmeta;
      jmeta["zoom"] = asset.editor.zoom;
      jmeta["panning"] = { asset.editor.panX, asset.editor.panY };
      json jpos = json::array();
      for (const auto& kv : asset.editor.nodePositions)
      {
         json e; e["id"] = kv.first; e["x"] = kv.second.first; e["y"] = kv.second.second; jpos.push_back(std::move(e));
      }
      jmeta["nodePositions"] = std::move(jpos);
      j["editor"] = std::move(jmeta);
   }

   void GraphSerializer::FromJson(const json& j, GraphAsset& out)
   {
      Graph& g = out.graph; g = Graph();
      if (j.contains("name")) g.name = j.value("name", std::string());

      // Nodes
      if (j.contains("nodes"))
      {
         for (const auto& jn : j["nodes"]) {
            Node n; n.id = jn.value("id", 0); n.typeId = jn.value("typeId", 0); n.name = jn.value("name", std::string());
            if (jn.contains("inputs")) jn["inputs"].get_to(n.inputs);
            if (jn.contains("outputs")) jn["outputs"].get_to(n.outputs);
            g.nodes.push_back(std::move(n));
         }
      }

      // Pins
      if (j.contains("pins"))
      {
         for (const auto& jp : j["pins"]) {
            Pin p; p.id = jp.value("id", 0); p.nodeId = jp.value("nodeId", 0);
            std::string k = jp.value("kind", std::string("Input"));
            p.kind = (k == "Input" ? PinKind::Input : PinKind::Output);
            p.type = ParsePinValueType(jp.value("PinValueType", std::string("Any")));
            p.name = jp.value("name", std::string());
            g.pins.push_back(std::move(p));
         }
      }

      // Links
      if (j.contains("links"))
      {
         for (const auto& jl : j["links"]) {
            Link l; l.id = jl.value("id", 0); l.from = jl.value("fromPin", 0); l.to = jl.value("toPin", 0);
            g.links.push_back(std::move(l));
         }
      }

      // Counters
      if (j.contains("nextIds")) {
         const auto& ni = j["nextIds"]; g.nextNodeId = ni.value("node", 1); g.nextPinId = ni.value("pin", 1); g.nextLinkId = ni.value("link", 1);
      }

      // Payloads
      out.nodePayloads.clear();
      if (j.contains("payloads") && j["payloads"].is_object())
      {
         for (auto it = j["payloads"].begin(); it != j["payloads"].end(); ++it)
         {
            NodeId nid = std::stoi(it.key());
            out.nodePayloads[nid] = it.value();
         }
      }

      // Editor
      out.editor = EditorMeta();
      if (j.contains("editor"))
      {
         const auto& jm = j["editor"];
         out.editor.zoom = jm.value("zoom", 1.0f);
         if (jm.contains("panning") && jm["panning"].is_array() && jm["panning"].size() >= 2) {
            out.editor.panX = jm["panning"][0].get<float>();
            out.editor.panY = jm["panning"][1].get<float>();
         }
         if (jm.contains("nodePositions"))
         {
            for (const auto& e : jm["nodePositions"]) {
               NodeId id = e.value("id", 0); float x = e.value("x", 0.0f); float y = e.value("y", 0.0f);
               out.editor.nodePositions[id] = { x, y };
            }
         }
      }
   }

   bool GraphSerializer::SaveToFile(const GraphAsset& asset, const std::string& path)
   {
      json j; ToJson(asset, j);
      std::ofstream out(path);
      if (!out) return false;
      out << j.dump(2);
      return true;
   }

   bool GraphSerializer::LoadFromFile(const std::string& path, GraphAsset& outAsset)
   {
      std::ifstream in(path);
      if (!in) return false;
      json j; in >> j;
      FromJson(j, outAsset);
      return true;
   }
}



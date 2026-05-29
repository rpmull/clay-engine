#pragma once

#include "GraphCore.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <unordered_map>

struct EvalContext {
   // Game bridges: blackboard, services, time, etc.
   void* userContext = nullptr;
   };

struct PinValue {
   PinValueType type{ PinValueType::Any };

   bool b{};
   int i{};
   float f{};
   std::string s;

   };

struct IEvaluatorIO {
   virtual bool GetInput(PinId pinId, PinValue& outValue) = 0;
   virtual bool SetOutput(PinId pinId, const PinValue& value) = 0;
   virtual ~IEvaluatorIO() = default;
   };

// Each node type defines how to create, serialize, draw, and evaluate nodes of that type.
struct NodeType {
   NodeTypeId typeId{};
   const char* name;

   std::function<void(Graph&, Node&)> Create;
   std::function<void(Graph&, Node&)> DrawInspector;

   // Serialize/Deserialize node payload (NOT pins/links)
   std::function<void(const Graph&, const Node&, nlohmann::json& out)> Serialize;
   std::function<void(Graph&, Node&, const nlohmann::json& in)>        Deserialize;
   // Evaluate at runtime (tick or on-demand)
   std::function<void(const Graph&, const Node&, IEvaluatorIO&, EvalContext&)> Evaluate;

   };

// Registry for node types
struct NodeTypeRegistry {
   std::unordered_map<NodeTypeId, NodeType> byId;
   std::unordered_map<std::string, NodeTypeId> byName;

   void Register(const NodeType& t) { byId[t.typeId] = t; byName[t.name] = t.typeId; }
   const NodeType* Get(NodeTypeId id) const {
      auto it = byId.find(id); return it == byId.end() ? nullptr : &it->second;
      }
   const NodeType* Get(const std::string& name) const {
      auto it = byName.find(name); return it == byName.end() ? nullptr : Get(byName.at(name));
      }
   };
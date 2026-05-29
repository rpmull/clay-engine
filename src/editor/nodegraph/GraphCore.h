#pragma once
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <cstdint>


// IDs
using NodeId = int32_t;
using PinId = int32_t;
using LinkId = int32_t;
using NodeTypeId = int32_t;

enum class PinKind {
   Input,
   Output
   };

enum class PinValueType {
   Bool,
   Int,
   Float,
   String,
   Entity,
   Event,
   Any
   };
 
struct Pin {
   PinId id{};
   NodeId nodeId{};
   PinKind kind{};
   PinValueType type{ PinValueType::Any };
   std::string name;
   };

struct Node {
   NodeId id{};
   NodeTypeId typeId{};
   std::string name;
   std::vector<PinId> inputs;
   std::vector<PinId> outputs;
   };

struct Link {
   LinkId id{};
   PinId from{}; // output pin id
   PinId to{};   // input pin id
   };

struct Graph {
   std::string name;
   std::vector<Node> nodes;
   std::vector<Pin> pins;
   std::vector<Link> links;

   int nextNodeId{ 1 };
   int nextPinId{ 1 };
   int nextLinkId{ 1 };

   // Editor-only metadata (optional persistence)
   // Map of node id to editor-space position (x,y)
   std::unordered_map<NodeId, std::pair<float, float>> editorNodePositions;

   NodeId NewNodeId() { return nextNodeId++; }
   PinId NewPinId() { return nextPinId++; }
   LinkId NewLinkId() { return nextLinkId++; }
   };
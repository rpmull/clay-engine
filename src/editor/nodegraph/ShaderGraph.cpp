#include "ShaderGraph.h"
#include <algorithm>
#include <unordered_set>
#include <queue>

namespace shadergraph {

ShaderNode* ShaderGraph::FindNode(NodeId id) {
    for (auto& node : nodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

const ShaderNode* ShaderGraph::FindNode(NodeId id) const {
    for (const auto& node : nodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

ShaderPin* ShaderGraph::FindPin(PinId id) {
    for (auto& pin : pins) {
        if (pin.id == id) return &pin;
    }
    return nullptr;
}

const ShaderPin* ShaderGraph::FindPin(PinId id) const {
    for (const auto& pin : pins) {
        if (pin.id == id) return &pin;
    }
    return nullptr;
}

ShaderLink* ShaderGraph::FindLink(LinkId id) {
    for (auto& link : links) {
        if (link.id == id) return &link;
    }
    return nullptr;
}

const ShaderLink* ShaderGraph::FindLink(LinkId id) const {
    for (const auto& link : links) {
        if (link.id == id) return &link;
    }
    return nullptr;
}

ShaderLink* ShaderGraph::FindLinkToPin(PinId inputPinId) {
    for (auto& link : links) {
        if (link.toPin == inputPinId) return &link;
    }
    return nullptr;
}

const ShaderLink* ShaderGraph::FindLinkToPin(PinId inputPinId) const {
    for (const auto& link : links) {
        if (link.toPin == inputPinId) return &link;
    }
    return nullptr;
}

std::vector<ShaderLink*> ShaderGraph::FindLinksFromPin(PinId outputPinId) {
    std::vector<ShaderLink*> result;
    for (auto& link : links) {
        if (link.fromPin == outputPinId) result.push_back(&link);
    }
    return result;
}

ShaderNode* ShaderGraph::GetPinOwner(PinId pinId) {
    const ShaderPin* pin = FindPin(pinId);
    if (!pin) return nullptr;
    return FindNode(pin->nodeId);
}

const ShaderNode* ShaderGraph::GetPinOwner(PinId pinId) const {
    const ShaderPin* pin = FindPin(pinId);
    if (!pin) return nullptr;
    return FindNode(pin->nodeId);
}

NodeId ShaderGraph::AddNode(const NodeTypeId& typeId, const glm::vec2& pos) {
    ShaderNode node;
    node.id = NewNodeId();
    node.typeId = typeId;
    node.displayName = typeId.name;
    node.editorPos = pos;
    nodes.push_back(node);
    return node.id;
}

PinId ShaderGraph::AddPin(NodeId nodeId, PinKind kind, ShaderValueType type, const std::string& name) {
    ShaderNode* node = FindNode(nodeId);
    if (!node) return 0;
    
    ShaderPin pin;
    pin.id = NewPinId();
    pin.nodeId = nodeId;
    pin.kind = kind;
    pin.type = type;
    pin.name = name;
    
    // Set reasonable defaults based on type
    if (type == ShaderValueType::Float || type == ShaderValueType::Float2 || 
        type == ShaderValueType::Float3 || type == ShaderValueType::Float4) {
        pin.defaultValue = glm::vec4(0.0f);
    } else if (type == ShaderValueType::Color3 || type == ShaderValueType::Color4) {
        pin.defaultValue = glm::vec4(1.0f);  // Default white
    }
    
    pins.push_back(pin);
    
    if (kind == PinKind::Input) {
        node->inputPins.push_back(pin.id);
    } else {
        node->outputPins.push_back(pin.id);
    }
    
    return pin.id;
}

LinkId ShaderGraph::AddLink(PinId fromPin, PinId toPin) {
    // Validate connection
    if (!CanConnect(fromPin, toPin)) return 0;
    
    // Remove existing link to this input pin
    links.erase(
        std::remove_if(links.begin(), links.end(),
            [toPin](const ShaderLink& l) { return l.toPin == toPin; }),
        links.end());
    
    ShaderLink link;
    link.id = NewLinkId();
    link.fromPin = fromPin;
    link.toPin = toPin;
    links.push_back(link);
    return link.id;
}

void ShaderGraph::RemoveNode(NodeId id) {
    ShaderNode* node = FindNode(id);
    if (!node) return;
    
    // Remove all links connected to this node's pins
    std::unordered_set<PinId> nodePins;
    for (PinId p : node->inputPins) nodePins.insert(p);
    for (PinId p : node->outputPins) nodePins.insert(p);
    
    links.erase(
        std::remove_if(links.begin(), links.end(),
            [&nodePins](const ShaderLink& l) {
                return nodePins.count(l.fromPin) || nodePins.count(l.toPin);
            }),
        links.end());
    
    // Remove the node's pins
    pins.erase(
        std::remove_if(pins.begin(), pins.end(),
            [id](const ShaderPin& p) { return p.nodeId == id; }),
        pins.end());
    
    // Remove the node
    nodes.erase(
        std::remove_if(nodes.begin(), nodes.end(),
            [id](const ShaderNode& n) { return n.id == id; }),
        nodes.end());
}

void ShaderGraph::RemoveLink(LinkId id) {
    links.erase(
        std::remove_if(links.begin(), links.end(),
            [id](const ShaderLink& l) { return l.id == id; }),
        links.end());
}

bool ShaderGraph::CanConnect(PinId fromPinId, PinId toPinId) const {
    const ShaderPin* fromPin = FindPin(fromPinId);
    const ShaderPin* toPin = FindPin(toPinId);
    
    if (!fromPin || !toPin) return false;
    
    // Must be output -> input
    if (fromPin->kind != PinKind::Output || toPin->kind != PinKind::Input) return false;
    
    // Can't connect to self
    if (fromPin->nodeId == toPin->nodeId) return false;
    
    // Check type compatibility
    return AreTypesCompatible(fromPin->type, toPin->type);
}

bool ShaderGraph::HasCycle() const {
    // Build adjacency list based on node connections
    std::unordered_map<NodeId, std::vector<NodeId>> adj;
    std::unordered_map<NodeId, int> inDegree;
    
    for (const auto& node : nodes) {
        adj[node.id] = {};
        inDegree[node.id] = 0;
    }
    
    for (const auto& link : links) {
        const ShaderPin* fromPin = FindPin(link.fromPin);
        const ShaderPin* toPin = FindPin(link.toPin);
        if (!fromPin || !toPin) continue;
        
        NodeId fromNode = fromPin->nodeId;
        NodeId toNode = toPin->nodeId;
        
        adj[fromNode].push_back(toNode);
        inDegree[toNode]++;
    }
    
    // Kahn's algorithm for cycle detection
    std::queue<NodeId> queue;
    for (const auto& [id, degree] : inDegree) {
        if (degree == 0) queue.push(id);
    }
    
    int count = 0;
    while (!queue.empty()) {
        NodeId curr = queue.front();
        queue.pop();
        count++;
        
        for (NodeId neighbor : adj[curr]) {
            inDegree[neighbor]--;
            if (inDegree[neighbor] == 0) {
                queue.push(neighbor);
            }
        }
    }
    
    return count != static_cast<int>(nodes.size());
}

std::vector<const ShaderPin*> ShaderGraph::GetExposedParameters() const {
    std::vector<const ShaderPin*> result;
    for (const auto& pin : pins) {
        if (pin.exposed && pin.kind == PinKind::Input) {
            result.push_back(&pin);
        }
    }
    return result;
}

ShaderNode* ShaderGraph::FindMasterNode() {
    for (auto& node : nodes) {
        if (node.typeId.category == "Output") {
            return &node;
        }
    }
    return nullptr;
}

const ShaderNode* ShaderGraph::FindMasterNode() const {
    for (const auto& node : nodes) {
        if (node.typeId.category == "Output") {
            return &node;
        }
    }
    return nullptr;
}

void ShaderGraph::Clear() {
    name = "New Shader Graph";
    guid.clear();
    surfaceType = SurfaceType::Lit;
    nodes.clear();
    pins.clear();
    links.clear();
    nextNodeId = 1;
    nextPinId = 1;
    nextLinkId = 1;
    editorPan = glm::vec2(0.0f);
    editorZoom = 1.0f;
}

// --- Utility functions ---

const char* ValueTypeToGLSL(ShaderValueType type) {
    switch (type) {
        case ShaderValueType::Float: return "float";
        case ShaderValueType::Float2: return "vec2";
        case ShaderValueType::Float3: return "vec3";
        case ShaderValueType::Float4: return "vec4";
        case ShaderValueType::Int: return "int";
        case ShaderValueType::Bool: return "bool";
        case ShaderValueType::Color3: return "vec3";
        case ShaderValueType::Color4: return "vec4";
        case ShaderValueType::Texture2D: return "sampler2D";
        case ShaderValueType::Sampler2D: return "sampler2D";
        case ShaderValueType::Matrix3: return "mat3";
        case ShaderValueType::Matrix4: return "mat4";
        case ShaderValueType::Any: return "vec4";
        default: return "float";
    }
}

int ValueTypeComponentCount(ShaderValueType type) {
    switch (type) {
        case ShaderValueType::Float: return 1;
        case ShaderValueType::Float2: return 2;
        case ShaderValueType::Float3: return 3;
        case ShaderValueType::Float4: return 4;
        case ShaderValueType::Int: return 1;
        case ShaderValueType::Bool: return 1;
        case ShaderValueType::Color3: return 3;
        case ShaderValueType::Color4: return 4;
        default: return 4;
    }
}

bool AreTypesCompatible(ShaderValueType from, ShaderValueType to) {
    // Any accepts everything
    if (to == ShaderValueType::Any) return true;
    if (from == ShaderValueType::Any) return true;
    
    // Same types always compatible
    if (from == to) return true;
    
    // Color types are compatible with their float equivalents
    if (from == ShaderValueType::Color3 && to == ShaderValueType::Float3) return true;
    if (from == ShaderValueType::Float3 && to == ShaderValueType::Color3) return true;
    if (from == ShaderValueType::Color4 && to == ShaderValueType::Float4) return true;
    if (from == ShaderValueType::Float4 && to == ShaderValueType::Color4) return true;
    
    // Float can be promoted to larger types
    if (from == ShaderValueType::Float) {
        return to == ShaderValueType::Float2 || to == ShaderValueType::Float3 || 
               to == ShaderValueType::Float4 || to == ShaderValueType::Color3 ||
               to == ShaderValueType::Color4;
    }
    
    // Larger types can be used where smaller types expected (will use .xyz, .xy, .x)
    int fromCount = ValueTypeComponentCount(from);
    int toCount = ValueTypeComponentCount(to);
    
    // Allow truncation (vec4 -> vec3 -> vec2 -> float)
    if (fromCount >= toCount && 
        (to == ShaderValueType::Float || to == ShaderValueType::Float2 || 
         to == ShaderValueType::Float3 || to == ShaderValueType::Float4 ||
         to == ShaderValueType::Color3 || to == ShaderValueType::Color4)) {
        return true;
    }
    
    return false;
}

std::string GetTypeConversion(ShaderValueType from, ShaderValueType to, const std::string& expr) {
    if (from == to) return expr;
    
    // Any type accepts everything without conversion
    if (to == ShaderValueType::Any || from == ShaderValueType::Any) return expr;
    
    // Handle color <-> float conversions
    if ((from == ShaderValueType::Color3 && to == ShaderValueType::Float3) ||
        (from == ShaderValueType::Float3 && to == ShaderValueType::Color3)) {
        return expr;  // No conversion needed, same underlying type
    }
    if ((from == ShaderValueType::Color4 && to == ShaderValueType::Float4) ||
        (from == ShaderValueType::Float4 && to == ShaderValueType::Color4)) {
        return expr;
    }
    
    int fromCount = ValueTypeComponentCount(from);
    int toCount = ValueTypeComponentCount(to);
    
    // Float scalar to larger type - EXPLICIT SPLAT (no scalar constructor!)
    if (from == ShaderValueType::Float && toCount > 1) {
        switch (toCount) {
            case 2: return "vec2(" + expr + ", " + expr + ")";
            case 3: return "vec3(" + expr + ", " + expr + ", " + expr + ")";
            case 4: return "vec4(" + expr + ", " + expr + ", " + expr + ", " + expr + ")";
        }
    }
    
    // Truncation (use swizzle)
    if (fromCount > toCount) {
        std::string swizzle;
        switch (toCount) {
            case 1: swizzle = ".x"; break;
            case 2: swizzle = ".xy"; break;
            case 3: swizzle = ".xyz"; break;
            default: return expr;
        }
        return "(" + expr + ")" + swizzle;
    }
    
    // Expansion with zeros
    if (fromCount < toCount) {
        switch (fromCount) {
            case 1:
                // Float to larger - already handled above
                return expr;
            case 2:
                if (toCount == 3) return "vec3(" + expr + ", 0.0)";
                if (toCount == 4) return "vec4(" + expr + ", 0.0, 0.0)";
                break;
            case 3:
                if (toCount == 4) return "vec4(" + expr + ", 1.0)";  // Alpha = 1.0 for colors
                break;
        }
    }
    
    return expr;
}

} // namespace shadergraph


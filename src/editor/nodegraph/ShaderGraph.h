#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <glm/glm.hpp>
#include <cstdint>

namespace shadergraph {

// Forward declarations
struct ShaderNode;
struct ShaderPin;
struct ShaderLink;

// IDs
using NodeId = int32_t;
using PinId = int32_t;
using LinkId = int32_t;

// Pin direction
enum class PinKind {
    Input,
    Output
};

// Value types for shader graph pins
enum class ShaderValueType {
    Float,
    Float2,
    Float3,
    Float4,
    Int,
    Bool,
    Color3,      // Like Float3 but treated as color in UI
    Color4,      // Like Float4 but treated as color in UI
    Texture2D,
    Sampler2D,
    Matrix3,
    Matrix4,
    Any          // For flexible connections
};

// Node type categories
enum class NodeCategory {
    Input,       // Constants, parameters, vertex data
    Math,        // Add, Multiply, etc.
    Vector,      // Swizzle, Combine, etc.
    Texture,     // Sample, etc.
    Utility,     // Lerp, Clamp, etc.
    Output       // Master/Surface nodes
};

// Surface type for master nodes
enum class SurfaceType {
    Unlit,
    Lit,         // Standard PBR
    Custom
};

// A pin on a node
struct ShaderPin {
    PinId id = 0;
    NodeId nodeId = 0;
    PinKind kind = PinKind::Input;
    ShaderValueType type = ShaderValueType::Float;
    std::string name;
    
    // Default value when not connected (for input pins)
    glm::vec4 defaultValue = glm::vec4(0.0f);
    
    // For texture pins: default texture asset path
    std::string defaultTexturePath;
    
    // Whether this pin is exposed as a material parameter
    bool exposed = false;
    
    // The exposed parameter name (if different from pin name)
    std::string exposedName;
};

// A link connecting two pins
struct ShaderLink {
    LinkId id = 0;
    PinId fromPin = 0;  // Output pin
    PinId toPin = 0;    // Input pin
};

// Node type identifier
struct NodeTypeId {
    std::string category;  // e.g., "Math", "Input", "Output"
    std::string name;      // e.g., "Add", "Float", "PBRMaster"
    
    std::string Full() const { return category + "/" + name; }
    
    bool operator==(const NodeTypeId& other) const {
        return category == other.category && name == other.name;
    }
};

// A node in the shader graph
struct ShaderNode {
    NodeId id = 0;
    NodeTypeId typeId;
    std::string displayName;  // User-editable display name
    
    // Editor position
    glm::vec2 editorPos = glm::vec2(0.0f);
    
    // Pin IDs owned by this node
    std::vector<PinId> inputPins;
    std::vector<PinId> outputPins;
    
    // Node-specific properties stored as JSON-like map
    // Used for things like: selected swizzle mask, operation type, etc.
    std::unordered_map<std::string, std::string> properties;
    
    // Helper to get property with default
    std::string GetProperty(const std::string& key, const std::string& defaultVal = "") const {
        auto it = properties.find(key);
        return it != properties.end() ? it->second : defaultVal;
    }
    
    void SetProperty(const std::string& key, const std::string& value) {
        properties[key] = value;
    }
};

// The complete shader graph
struct ShaderGraph {
    std::string name = "New Shader Graph";
    std::string guid;  // Unique identifier for asset system
    
    // Graph type
    SurfaceType surfaceType = SurfaceType::Lit;
    
    // Environment settings (fog and ambient light)
    bool applyFog = true;       // Apply distance fog
    bool applyAmbient = true;   // Apply ambient lighting
    
    // All nodes, pins, and links
    std::vector<ShaderNode> nodes;
    std::vector<ShaderPin> pins;
    std::vector<ShaderLink> links;
    
    // ID counters
    NodeId nextNodeId = 1;
    PinId nextPinId = 1;
    LinkId nextLinkId = 1;
    
    // Compiled shader info (populated after successful compilation)
    std::string compiledVSName;      // e.g., "vs_shgraph_ColorShader"
    std::string compiledFSName;      // e.g., "fs_shgraph_ColorShader"
    std::string compiledVSPath;      // Path to compiled VS binary (relative to project)
    std::string compiledFSPath;      // Path to compiled FS binary (relative to project)
    bool isCompiled = false;
    
    // Editor metadata
    glm::vec2 editorPan = glm::vec2(0.0f);
    float editorZoom = 1.0f;
    
    // ID generation
    NodeId NewNodeId() { return nextNodeId++; }
    PinId NewPinId() { return nextPinId++; }
    LinkId NewLinkId() { return nextLinkId++; }
    
    // Find helpers
    ShaderNode* FindNode(NodeId id);
    const ShaderNode* FindNode(NodeId id) const;
    ShaderPin* FindPin(PinId id);
    const ShaderPin* FindPin(PinId id) const;
    ShaderLink* FindLink(LinkId id);
    const ShaderLink* FindLink(LinkId id) const;
    
    // Find the link connected to an input pin (if any)
    ShaderLink* FindLinkToPin(PinId inputPinId);
    const ShaderLink* FindLinkToPin(PinId inputPinId) const;
    
    // Find all links from an output pin
    std::vector<ShaderLink*> FindLinksFromPin(PinId outputPinId);
    
    // Get the node that owns a pin
    ShaderNode* GetPinOwner(PinId pinId);
    const ShaderNode* GetPinOwner(PinId pinId) const;
    
    // Create a new node and add it to the graph
    NodeId AddNode(const NodeTypeId& typeId, const glm::vec2& pos = glm::vec2(0.0f));
    
    // Create a pin and add it to the graph (and to the owning node)
    PinId AddPin(NodeId nodeId, PinKind kind, ShaderValueType type, const std::string& name);
    
    // Create a link between two pins
    LinkId AddLink(PinId fromPin, PinId toPin);
    
    // Remove operations
    void RemoveNode(NodeId id);
    void RemoveLink(LinkId id);
    
    // Validation
    bool CanConnect(PinId fromPin, PinId toPin) const;
    bool HasCycle() const;
    
    // Get all exposed parameters (for material binding)
    std::vector<const ShaderPin*> GetExposedParameters() const;
    
    // Find the master/output node
    ShaderNode* FindMasterNode();
    const ShaderNode* FindMasterNode() const;
    
    // Clear the graph
    void Clear();
};

// Helper to convert value type to GLSL type string
const char* ValueTypeToGLSL(ShaderValueType type);

// Helper to get component count for a type
int ValueTypeComponentCount(ShaderValueType type);

// Check if types are compatible for connection
bool AreTypesCompatible(ShaderValueType from, ShaderValueType to);

// Get the implicit conversion code (if any) from one type to another
std::string GetTypeConversion(ShaderValueType from, ShaderValueType to, const std::string& expr);

} // namespace shadergraph


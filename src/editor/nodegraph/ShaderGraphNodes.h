#pragma once

#include "ShaderGraph.h"
#include <functional>
#include <vector>
#include <memory>

namespace shadergraph {

// Forward declaration
class ShaderGraphCodeGen;

// Node definition describes how to create a node type
struct NodeDefinition {
    NodeTypeId typeId;
    std::string displayName;
    NodeCategory category;
    std::string description;
    
    // Pin definitions
    struct PinDef {
        std::string name;
        ShaderValueType type;
        glm::vec4 defaultValue = glm::vec4(0.0f);
        bool canBeExposed = false;  // Can this pin become a material parameter
    };
    std::vector<PinDef> inputs;
    std::vector<PinDef> outputs;
    
    // Optional: custom property definitions (for dropdowns, etc.)
    struct PropertyDef {
        std::string key;
        std::string displayName;
        std::string defaultValue;
        std::vector<std::string> options;  // For dropdown selection
    };
    std::vector<PropertyDef> properties;
    
    // Code generation function signature
    // Takes: codegen context, node, output expressions map
    // Returns: success
    using CodeGenFunc = std::function<bool(ShaderGraphCodeGen&, const ShaderNode&)>;
    CodeGenFunc codeGen;
};

// Registry of all available node types
class NodeRegistry {
public:
    static NodeRegistry& Instance();
    
    // Register a node type
    void Register(const NodeDefinition& def);
    
    // Get all registered node types
    const std::vector<NodeDefinition>& GetAll() const { return m_Definitions; }
    
    // Get nodes by category
    std::vector<const NodeDefinition*> GetByCategory(NodeCategory category) const;
    
    // Find a specific node definition
    const NodeDefinition* Find(const NodeTypeId& typeId) const;
    const NodeDefinition* Find(const std::string& fullId) const;
    
    // Create a node with all its pins in a graph
    NodeId CreateNode(ShaderGraph& graph, const NodeTypeId& typeId, const glm::vec2& pos);
    
private:
    NodeRegistry();
    void RegisterBuiltins();
    
    std::vector<NodeDefinition> m_Definitions;
};

// ============================================================================
// Built-in Node Type Definitions
// ============================================================================

namespace nodes {

// --- Input Nodes ---
void RegisterFloatNode(NodeRegistry& reg);
void RegisterFloat2Node(NodeRegistry& reg);
void RegisterFloat3Node(NodeRegistry& reg);
void RegisterFloat4Node(NodeRegistry& reg);
void RegisterColorNode(NodeRegistry& reg);
void RegisterTimeNode(NodeRegistry& reg);
void RegisterUVNode(NodeRegistry& reg);
void RegisterTilingOffsetNode(NodeRegistry& reg);
void RegisterRotateUVNode(NodeRegistry& reg);
void RegisterPolarCoordinatesNode(NodeRegistry& reg);
void RegisterWorldPositionNode(NodeRegistry& reg);
void RegisterWorldNormalNode(NodeRegistry& reg);
void RegisterViewDirectionNode(NodeRegistry& reg);
void RegisterCameraPositionNode(NodeRegistry& reg);
void RegisterTexture2DNode(NodeRegistry& reg);

// --- Math Nodes ---
void RegisterAddNode(NodeRegistry& reg);
void RegisterSubtractNode(NodeRegistry& reg);
void RegisterMultiplyNode(NodeRegistry& reg);
void RegisterDivideNode(NodeRegistry& reg);
void RegisterPowerNode(NodeRegistry& reg);
void RegisterSqrtNode(NodeRegistry& reg);
void RegisterAbsNode(NodeRegistry& reg);
void RegisterNegateNode(NodeRegistry& reg);
void RegisterSinNode(NodeRegistry& reg);
void RegisterCosNode(NodeRegistry& reg);
void RegisterFloorNode(NodeRegistry& reg);
void RegisterCeilNode(NodeRegistry& reg);
void RegisterFracNode(NodeRegistry& reg);
void RegisterMinNode(NodeRegistry& reg);
void RegisterMaxNode(NodeRegistry& reg);
void RegisterClampNode(NodeRegistry& reg);
void RegisterSaturateNode(NodeRegistry& reg);
void RegisterLerpNode(NodeRegistry& reg);
void RegisterStepNode(NodeRegistry& reg);
void RegisterSmoothstepNode(NodeRegistry& reg);
void RegisterOneMinus(NodeRegistry& reg);

// --- Vector Nodes ---
void RegisterDotNode(NodeRegistry& reg);
void RegisterCrossNode(NodeRegistry& reg);
void RegisterNormalizeNode(NodeRegistry& reg);
void RegisterLengthNode(NodeRegistry& reg);
void RegisterDistanceNode(NodeRegistry& reg);
void RegisterReflectNode(NodeRegistry& reg);
void RegisterCombineNode(NodeRegistry& reg);
void RegisterSplitNode(NodeRegistry& reg);
void RegisterSwizzleNode(NodeRegistry& reg);

// --- Texture Nodes ---
void RegisterSampleTexture2DNode(NodeRegistry& reg);

// --- Utility / Color Nodes ---
void RegisterFresnelNode(NodeRegistry& reg);
void RegisterColorRampNode(NodeRegistry& reg);
void RegisterHueSaturationValueNode(NodeRegistry& reg);

// --- Output/Master Nodes ---
void RegisterUnlitMasterNode(NodeRegistry& reg);
void RegisterPBRMasterNode(NodeRegistry& reg);

// Register all built-in nodes
void RegisterAllBuiltins(NodeRegistry& reg);

} // namespace nodes

} // namespace shadergraph


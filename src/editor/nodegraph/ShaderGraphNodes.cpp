#include "ShaderGraphNodes.h"
#include "ShaderGraphCodeGen.h"
#include <sstream>

namespace shadergraph {

NodeRegistry& NodeRegistry::Instance() {
    static NodeRegistry instance;
    return instance;
}

NodeRegistry::NodeRegistry() {
    RegisterBuiltins();
}

void NodeRegistry::Register(const NodeDefinition& def) {
    m_Definitions.push_back(def);
}

std::vector<const NodeDefinition*> NodeRegistry::GetByCategory(NodeCategory category) const {
    std::vector<const NodeDefinition*> result;
    for (const auto& def : m_Definitions) {
        if (def.category == category) {
            result.push_back(&def);
        }
    }
    return result;
}

const NodeDefinition* NodeRegistry::Find(const NodeTypeId& typeId) const {
    for (const auto& def : m_Definitions) {
        if (def.typeId == typeId) {
            return &def;
        }
    }
    return nullptr;
}

const NodeDefinition* NodeRegistry::Find(const std::string& fullId) const {
    for (const auto& def : m_Definitions) {
        if (def.typeId.Full() == fullId) {
            return &def;
        }
    }
    return nullptr;
}

NodeId NodeRegistry::CreateNode(ShaderGraph& graph, const NodeTypeId& typeId, const glm::vec2& pos) {
    const NodeDefinition* def = Find(typeId);
    if (!def) return 0;
    
    NodeId nodeId = graph.AddNode(typeId, pos);
    ShaderNode* node = graph.FindNode(nodeId);
    if (!node) return 0;
    
    node->displayName = def->displayName;
    
    // Create input pins
    for (const auto& pinDef : def->inputs) {
        PinId pinId = graph.AddPin(nodeId, PinKind::Input, pinDef.type, pinDef.name);
        ShaderPin* pin = graph.FindPin(pinId);
        if (pin) {
            pin->defaultValue = pinDef.defaultValue;
        }
    }
    
    // Create output pins
    for (const auto& pinDef : def->outputs) {
        graph.AddPin(nodeId, PinKind::Output, pinDef.type, pinDef.name);
    }
    
    // Set default properties
    for (const auto& propDef : def->properties) {
        node->SetProperty(propDef.key, propDef.defaultValue);
    }
    
    return nodeId;
}

void NodeRegistry::RegisterBuiltins() {
    nodes::RegisterAllBuiltins(*this);
}

// ============================================================================
// Built-in Node Implementations
// ============================================================================

namespace nodes {

// Helper macro for simple unary math operations
#define REGISTER_UNARY_MATH(Name, OpName, GlslFunc) \
void Register##Name##Node(NodeRegistry& reg) { \
    NodeDefinition def; \
    def.typeId = {"Math", #Name}; \
    def.displayName = #Name; \
    def.category = NodeCategory::Math; \
    def.description = #Name " operation"; \
    def.inputs = {{"In", ShaderValueType::Any, glm::vec4(0.0f)}}; \
    def.outputs = {{"Out", ShaderValueType::Any}}; \
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool { \
        std::string inExpr = gen.GetInputExpression(node, "In"); \
        std::string outVar = gen.MakeVarName(node.id, "Out"); \
        ShaderValueType outType = gen.DeduceOutputType(node, "Out"); \
        gen.EmitLine(std::string(ValueTypeToGLSL(outType)) + " " + outVar + " = " GlslFunc "(" + inExpr + ");"); \
        gen.SetOutputExpression(node, "Out", outVar); \
        return true; \
    }; \
    reg.Register(def); \
}

// Helper for binary math operations
#define REGISTER_BINARY_MATH(Name, OpSymbol) \
void Register##Name##Node(NodeRegistry& reg) { \
    NodeDefinition def; \
    def.typeId = {"Math", #Name}; \
    def.displayName = #Name; \
    def.category = NodeCategory::Math; \
    def.description = #Name " two values"; \
    def.inputs = { \
        {"A", ShaderValueType::Any, glm::vec4(0.0f)}, \
        {"B", ShaderValueType::Any, glm::vec4(0.0f)} \
    }; \
    def.outputs = {{"Out", ShaderValueType::Any}}; \
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool { \
        std::string aExpr = gen.GetInputExpression(node, "A"); \
        std::string bExpr = gen.GetInputExpression(node, "B"); \
        std::string outVar = gen.MakeVarName(node.id, "Out"); \
        ShaderValueType outType = gen.DeduceOutputType(node, "Out"); \
        gen.EmitLine(std::string(ValueTypeToGLSL(outType)) + " " + outVar + " = " + aExpr + " " OpSymbol " " + bExpr + ";"); \
        gen.SetOutputExpression(node, "Out", outVar); \
        return true; \
    }; \
    reg.Register(def); \
}

// --- Input Nodes ---

void RegisterFloatNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Input", "Float"};
    def.displayName = "Float";
    def.category = NodeCategory::Input;
    def.description = "A constant float value";
    def.inputs = {{"Value", ShaderValueType::Float, glm::vec4(0.0f), true}};
    def.outputs = {{"Out", ShaderValueType::Float}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string expr = gen.GetInputExpression(node, "Value");
        gen.SetOutputExpression(node, "Out", expr);
        return true;
    };
    reg.Register(def);
}

void RegisterFloat2Node(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Input", "Float2"};
    def.displayName = "Vector2";
    def.category = NodeCategory::Input;
    def.description = "A constant vec2 value";
    def.inputs = {
        {"X", ShaderValueType::Float, glm::vec4(0.0f), true},
        {"Y", ShaderValueType::Float, glm::vec4(0.0f), true}
    };
    def.outputs = {{"Out", ShaderValueType::Float2}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string x = gen.GetInputExpression(node, "X");
        std::string y = gen.GetInputExpression(node, "Y");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        gen.EmitLine("vec2 " + outVar + " = vec2(" + x + ", " + y + ");");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

void RegisterFloat3Node(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Input", "Float3"};
    def.displayName = "Vector3";
    def.category = NodeCategory::Input;
    def.description = "A constant vec3 value";
    def.inputs = {
        {"X", ShaderValueType::Float, glm::vec4(0.0f), true},
        {"Y", ShaderValueType::Float, glm::vec4(0.0f), true},
        {"Z", ShaderValueType::Float, glm::vec4(0.0f), true}
    };
    def.outputs = {{"Out", ShaderValueType::Float3}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string x = gen.GetInputExpression(node, "X");
        std::string y = gen.GetInputExpression(node, "Y");
        std::string z = gen.GetInputExpression(node, "Z");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        gen.EmitLine("vec3 " + outVar + " = vec3(" + x + ", " + y + ", " + z + ");");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

void RegisterFloat4Node(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Input", "Float4"};
    def.displayName = "Vector4";
    def.category = NodeCategory::Input;
    def.description = "A constant vec4 value";
    def.inputs = {
        {"X", ShaderValueType::Float, glm::vec4(0.0f), true},
        {"Y", ShaderValueType::Float, glm::vec4(0.0f), true},
        {"Z", ShaderValueType::Float, glm::vec4(0.0f), true},
        {"W", ShaderValueType::Float, glm::vec4(0.0f), true}
    };
    def.outputs = {{"Out", ShaderValueType::Float4}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string x = gen.GetInputExpression(node, "X");
        std::string y = gen.GetInputExpression(node, "Y");
        std::string z = gen.GetInputExpression(node, "Z");
        std::string w = gen.GetInputExpression(node, "W");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        gen.EmitLine("vec4 " + outVar + " = vec4(" + x + ", " + y + ", " + z + ", " + w + ");");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

void RegisterColorNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Input", "Color"};
    def.displayName = "Color";
    def.category = NodeCategory::Input;
    def.description = "An RGBA color value";
    def.inputs = {{"Color", ShaderValueType::Color4, glm::vec4(1.0f), true}};
    def.outputs = {
        {"RGBA", ShaderValueType::Color4},
        {"RGB", ShaderValueType::Color3},
        {"R", ShaderValueType::Float},
        {"G", ShaderValueType::Float},
        {"B", ShaderValueType::Float},
        {"A", ShaderValueType::Float}
    };
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string colorExpr = gen.GetInputExpression(node, "Color");
        std::string baseVar = gen.MakeVarName(node.id, "color");
        gen.EmitLine("vec4 " + baseVar + " = " + colorExpr + ";");
        gen.SetOutputExpression(node, "RGBA", baseVar);
        gen.SetOutputExpression(node, "RGB", baseVar + ".rgb");
        gen.SetOutputExpression(node, "R", baseVar + ".r");
        gen.SetOutputExpression(node, "G", baseVar + ".g");
        gen.SetOutputExpression(node, "B", baseVar + ".b");
        gen.SetOutputExpression(node, "A", baseVar + ".a");
        return true;
    };
    reg.Register(def);
}

void RegisterTimeNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Input", "Time"};
    def.displayName = "Time";
    def.category = NodeCategory::Input;
    def.description = "Time since start (uses u_time built-in from bgfx)";
    def.outputs = {
        {"Time", ShaderValueType::Float},
        {"SinTime", ShaderValueType::Float},
        {"CosTime", ShaderValueType::Float}
    };
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        // bgfx provides u_time as vec4(time, time*2, time*3, time*4) but we'll use our own
        gen.RequireUniform("u_Time", "vec4");  // x = time, y = sin(time), z = cos(time)
        gen.SetOutputExpression(node, "Time", "u_Time.x");
        gen.SetOutputExpression(node, "SinTime", "sin(u_Time.x)");
        gen.SetOutputExpression(node, "CosTime", "cos(u_Time.x)");
        return true;
    };
    reg.Register(def);
}

void RegisterUVNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Input", "UV"};
    def.displayName = "UV";
    def.category = NodeCategory::Input;
    def.description = "Texture coordinates from vertex input";
    def.outputs = {
        {"UV", ShaderValueType::Float2},
        {"U", ShaderValueType::Float},
        {"V", ShaderValueType::Float}
    };
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        gen.RequireVarying("v_texcoord0");
        gen.SetOutputExpression(node, "UV", "v_texcoord0.xy");
        gen.SetOutputExpression(node, "U", "v_texcoord0.x");
        gen.SetOutputExpression(node, "V", "v_texcoord0.y");
        return true;
    };
    reg.Register(def);
}

void RegisterTilingOffsetNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Input", "TilingOffset"};
    def.displayName = "Tiling and Offset";
    def.category = NodeCategory::Input;
    def.description = "Apply tiling (scale) and offset to UV coordinates";
    def.inputs = {
        {"UV", ShaderValueType::Float2, glm::vec4(0.0f)},
        {"Tiling", ShaderValueType::Float2, glm::vec4(1.0f, 1.0f, 0.0f, 0.0f), true},
        {"Offset", ShaderValueType::Float2, glm::vec4(0.0f), true}
    };
    def.outputs = {{"Out", ShaderValueType::Float2}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string uv = gen.GetInputExpression(node, "UV");
        std::string tiling = gen.GetInputExpression(node, "Tiling");
        std::string offset = gen.GetInputExpression(node, "Offset");
        
        // If UV not connected, use default texcoord
        if (uv.empty() || uv == "vec2(0.0, 0.0)") {
            gen.RequireVarying("v_texcoord0");
            uv = "v_texcoord0.xy";
        }
        
        std::string var = gen.MakeVarName(node.id, "tiledUV");
        gen.EmitLine("vec2 " + var + " = " + uv + " * " + tiling + " + " + offset + ";");
        gen.SetOutputExpression(node, "Out", var);
        return true;
    };
    reg.Register(def);
}

void RegisterRotateUVNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Input", "RotateUV"};
    def.displayName = "Rotate UV";
    def.category = NodeCategory::Input;
    def.description = "Rotate UV coordinates around a center point";
    def.inputs = {
        {"UV", ShaderValueType::Float2, glm::vec4(0.0f)},
        {"Center", ShaderValueType::Float2, glm::vec4(0.5f, 0.5f, 0.0f, 0.0f), true},
        {"Rotation", ShaderValueType::Float, glm::vec4(0.0f), true}  // In radians
    };
    def.outputs = {{"Out", ShaderValueType::Float2}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string uv = gen.GetInputExpression(node, "UV");
        std::string center = gen.GetInputExpression(node, "Center");
        std::string rotation = gen.GetInputExpression(node, "Rotation");
        
        if (uv.empty() || uv == "vec2(0.0, 0.0)") {
            gen.RequireVarying("v_texcoord0");
            uv = "v_texcoord0.xy";
        }
        
        std::string var = gen.MakeVarName(node.id, "rotUV");
        std::string sinVar = gen.MakeVarName(node.id, "sinR");
        std::string cosVar = gen.MakeVarName(node.id, "cosR");
        std::string centered = gen.MakeVarName(node.id, "centered");
        
        gen.EmitLine("float " + sinVar + " = sin(" + rotation + ");");
        gen.EmitLine("float " + cosVar + " = cos(" + rotation + ");");
        gen.EmitLine("vec2 " + centered + " = " + uv + " - " + center + ";");
        gen.EmitLine("vec2 " + var + " = vec2(" + 
                     centered + ".x * " + cosVar + " - " + centered + ".y * " + sinVar + ", " +
                     centered + ".x * " + sinVar + " + " + centered + ".y * " + cosVar + ") + " + center + ";");
        gen.SetOutputExpression(node, "Out", var);
        return true;
    };
    reg.Register(def);
}

void RegisterPolarCoordinatesNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Input", "PolarCoordinates"};
    def.displayName = "Polar Coordinates";
    def.category = NodeCategory::Input;
    def.description = "Convert UV to polar coordinates (radial/angular)";
    def.inputs = {
        {"UV", ShaderValueType::Float2, glm::vec4(0.0f)},
        {"Center", ShaderValueType::Float2, glm::vec4(0.5f, 0.5f, 0.0f, 0.0f), true},
        {"RadialScale", ShaderValueType::Float, glm::vec4(1.0f), true},
        {"LengthScale", ShaderValueType::Float, glm::vec4(1.0f), true}
    };
    def.outputs = {{"Out", ShaderValueType::Float2}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string uv = gen.GetInputExpression(node, "UV");
        std::string center = gen.GetInputExpression(node, "Center");
        std::string radialScale = gen.GetInputExpression(node, "RadialScale");
        std::string lengthScale = gen.GetInputExpression(node, "LengthScale");
        
        if (uv.empty() || uv == "vec2(0.0, 0.0)") {
            gen.RequireVarying("v_texcoord0");
            uv = "v_texcoord0.xy";
        }
        
        std::string delta = gen.MakeVarName(node.id, "delta");
        std::string var = gen.MakeVarName(node.id, "polar");
        
        gen.EmitLine("vec2 " + delta + " = " + uv + " - " + center + ";");
        gen.EmitLine("vec2 " + var + " = vec2(length(" + delta + ") * 2.0 * " + lengthScale + ", " +
                     "atan2(" + delta + ".y, " + delta + ".x) * " + radialScale + " / 3.14159265);");
        gen.SetOutputExpression(node, "Out", var);
        return true;
    };
    reg.Register(def);
}

void RegisterWorldPositionNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Input", "WorldPosition"};
    def.displayName = "World Position";
    def.category = NodeCategory::Input;
    def.description = "Fragment world position";
    def.outputs = {{"Position", ShaderValueType::Float3}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        gen.RequireVarying("v_worldPos");
        gen.SetOutputExpression(node, "Position", "v_worldPos");
        return true;
    };
    reg.Register(def);
}

void RegisterWorldNormalNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Input", "WorldNormal"};
    def.displayName = "World Normal";
    def.category = NodeCategory::Input;
    def.description = "Fragment world normal (normalized)";
    def.outputs = {{"Normal", ShaderValueType::Float3}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        gen.RequireVarying("v_normal");
        std::string outVar = gen.MakeVarName(node.id, "worldNormal");
        gen.EmitLine("vec3 " + outVar + " = normalize(v_normal);");
        gen.SetOutputExpression(node, "Normal", outVar);
        return true;
    };
    reg.Register(def);
}

void RegisterViewDirectionNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Input", "ViewDirection"};
    def.displayName = "View Direction";
    def.category = NodeCategory::Input;
    def.description = "Direction from fragment to camera";
    def.outputs = {{"Direction", ShaderValueType::Float3}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        gen.RequireVarying("v_viewDir");
        std::string outVar = gen.MakeVarName(node.id, "viewDir");
        gen.EmitLine("vec3 " + outVar + " = normalize(v_viewDir);");
        gen.SetOutputExpression(node, "Direction", outVar);
        return true;
    };
    reg.Register(def);
}

void RegisterCameraPositionNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Input", "CameraPosition"};
    def.displayName = "Camera Position";
    def.category = NodeCategory::Input;
    def.description = "World space camera position";
    def.outputs = {{"Position", ShaderValueType::Float3}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        gen.RequireUniform("u_cameraPos", "vec4");
        gen.SetOutputExpression(node, "Position", "u_cameraPos.xyz");
        return true;
    };
    reg.Register(def);
}

void RegisterTexture2DNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Input", "Texture2D"};
    def.displayName = "Texture 2D";
    def.category = NodeCategory::Input;
    def.description = "A 2D texture parameter";
    def.inputs = {{"Texture", ShaderValueType::Texture2D, glm::vec4(0.0f), true}};
    def.outputs = {{"Texture", ShaderValueType::Texture2D}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        // Find the input pin to get sampler name
        std::string samplerName = gen.GetTextureSamplerName(node, "Texture");
        gen.SetOutputExpression(node, "Texture", samplerName);
        return true;
    };
    reg.Register(def);
}

// --- Math Nodes ---

REGISTER_BINARY_MATH(Add, "+")
REGISTER_BINARY_MATH(Subtract, "-")
REGISTER_BINARY_MATH(Multiply, "*")
REGISTER_BINARY_MATH(Divide, "/")

void RegisterPowerNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Math", "Power"};
    def.displayName = "Power";
    def.category = NodeCategory::Math;
    def.description = "Raise A to the power of B";
    def.inputs = {
        {"Base", ShaderValueType::Any, glm::vec4(0.0f)},
        {"Exponent", ShaderValueType::Float, glm::vec4(1.0f)}
    };
    def.outputs = {{"Out", ShaderValueType::Any}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string base = gen.GetInputExpression(node, "Base");
        std::string exp = gen.GetInputExpression(node, "Exponent");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        ShaderValueType outType = gen.DeduceOutputType(node, "Out");
        gen.EmitLine(std::string(ValueTypeToGLSL(outType)) + " " + outVar + " = pow(" + base + ", " + exp + ");");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

REGISTER_UNARY_MATH(Sqrt, "Sqrt", "sqrt")
REGISTER_UNARY_MATH(Abs, "Abs", "abs")

void RegisterNegateNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Math", "Negate"};
    def.displayName = "Negate";
    def.category = NodeCategory::Math;
    def.description = "Negate (multiply by -1)";
    def.inputs = {{"In", ShaderValueType::Any, glm::vec4(0.0f)}};
    def.outputs = {{"Out", ShaderValueType::Any}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string inExpr = gen.GetInputExpression(node, "In");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        ShaderValueType outType = gen.DeduceOutputType(node, "Out");
        gen.EmitLine(std::string(ValueTypeToGLSL(outType)) + " " + outVar + " = -(" + inExpr + ");");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

REGISTER_UNARY_MATH(Sin, "Sin", "sin")
REGISTER_UNARY_MATH(Cos, "Cos", "cos")
REGISTER_UNARY_MATH(Floor, "Floor", "floor")
REGISTER_UNARY_MATH(Ceil, "Ceil", "ceil")
REGISTER_UNARY_MATH(Frac, "Frac", "fract")

void RegisterMinNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Math", "Min"};
    def.displayName = "Min";
    def.category = NodeCategory::Math;
    def.description = "Return minimum of A and B";
    def.inputs = {
        {"A", ShaderValueType::Any, glm::vec4(0.0f)},
        {"B", ShaderValueType::Any, glm::vec4(0.0f)}
    };
    def.outputs = {{"Out", ShaderValueType::Any}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string a = gen.GetInputExpression(node, "A");
        std::string b = gen.GetInputExpression(node, "B");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        ShaderValueType outType = gen.DeduceOutputType(node, "Out");
        gen.EmitLine(std::string(ValueTypeToGLSL(outType)) + " " + outVar + " = min(" + a + ", " + b + ");");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

void RegisterMaxNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Math", "Max"};
    def.displayName = "Max";
    def.category = NodeCategory::Math;
    def.description = "Return maximum of A and B";
    def.inputs = {
        {"A", ShaderValueType::Any, glm::vec4(0.0f)},
        {"B", ShaderValueType::Any, glm::vec4(0.0f)}
    };
    def.outputs = {{"Out", ShaderValueType::Any}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string a = gen.GetInputExpression(node, "A");
        std::string b = gen.GetInputExpression(node, "B");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        ShaderValueType outType = gen.DeduceOutputType(node, "Out");
        gen.EmitLine(std::string(ValueTypeToGLSL(outType)) + " " + outVar + " = max(" + a + ", " + b + ");");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

void RegisterClampNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Math", "Clamp"};
    def.displayName = "Clamp";
    def.category = NodeCategory::Math;
    def.description = "Clamp value between min and max";
    def.inputs = {
        {"In", ShaderValueType::Any, glm::vec4(0.0f)},
        {"Min", ShaderValueType::Float, glm::vec4(0.0f)},
        {"Max", ShaderValueType::Float, glm::vec4(1.0f)}
    };
    def.outputs = {{"Out", ShaderValueType::Any}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string in = gen.GetInputExpression(node, "In");
        std::string minVal = gen.GetInputExpression(node, "Min");
        std::string maxVal = gen.GetInputExpression(node, "Max");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        ShaderValueType outType = gen.DeduceOutputType(node, "Out");
        gen.EmitLine(std::string(ValueTypeToGLSL(outType)) + " " + outVar + " = clamp(" + in + ", " + minVal + ", " + maxVal + ");");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

REGISTER_UNARY_MATH(Saturate, "Saturate", "saturate")

void RegisterLerpNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Math", "Lerp"};
    def.displayName = "Lerp";
    def.category = NodeCategory::Math;
    def.description = "Linear interpolation between A and B";
    def.inputs = {
        {"A", ShaderValueType::Any, glm::vec4(0.0f)},
        {"B", ShaderValueType::Any, glm::vec4(1.0f)},
        {"T", ShaderValueType::Float, glm::vec4(0.5f)}
    };
    def.outputs = {{"Out", ShaderValueType::Any}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string a = gen.GetInputExpression(node, "A");
        std::string b = gen.GetInputExpression(node, "B");
        std::string t = gen.GetInputExpression(node, "T");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        ShaderValueType outType = gen.DeduceOutputType(node, "Out");
        gen.EmitLine(std::string(ValueTypeToGLSL(outType)) + " " + outVar + " = mix(" + a + ", " + b + ", " + t + ");");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

void RegisterStepNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Math", "Step"};
    def.displayName = "Step";
    def.category = NodeCategory::Math;
    def.description = "Returns 0 if In < Edge, 1 otherwise";
    def.inputs = {
        {"Edge", ShaderValueType::Any, glm::vec4(0.5f)},
        {"In", ShaderValueType::Any, glm::vec4(0.0f)}
    };
    def.outputs = {{"Out", ShaderValueType::Any}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string edge = gen.GetInputExpression(node, "Edge");
        std::string in = gen.GetInputExpression(node, "In");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        ShaderValueType outType = gen.DeduceOutputType(node, "Out");
        gen.EmitLine(std::string(ValueTypeToGLSL(outType)) + " " + outVar + " = step(" + edge + ", " + in + ");");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

void RegisterSmoothstepNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Math", "Smoothstep"};
    def.displayName = "Smoothstep";
    def.category = NodeCategory::Math;
    def.description = "Hermite smoothstep between edge0 and edge1";
    def.inputs = {
        {"Edge0", ShaderValueType::Any, glm::vec4(0.0f)},
        {"Edge1", ShaderValueType::Any, glm::vec4(1.0f)},
        {"In", ShaderValueType::Any, glm::vec4(0.5f)}
    };
    def.outputs = {{"Out", ShaderValueType::Any}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string e0 = gen.GetInputExpression(node, "Edge0");
        std::string e1 = gen.GetInputExpression(node, "Edge1");
        std::string in = gen.GetInputExpression(node, "In");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        ShaderValueType outType = gen.DeduceOutputType(node, "Out");
        gen.EmitLine(std::string(ValueTypeToGLSL(outType)) + " " + outVar + " = smoothstep(" + e0 + ", " + e1 + ", " + in + ");");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

void RegisterOneMinus(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Math", "OneMinus"};
    def.displayName = "One Minus";
    def.category = NodeCategory::Math;
    def.description = "Returns 1.0 - In";
    def.inputs = {{"In", ShaderValueType::Any, glm::vec4(0.0f)}};
    def.outputs = {{"Out", ShaderValueType::Any}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string in = gen.GetInputExpression(node, "In");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        ShaderValueType outType = gen.DeduceOutputType(node, "Out");
        int components = ValueTypeComponentCount(outType);
        std::string one;
        switch (components) {
            case 1: one = "1.0"; break;
            case 2: one = "vec2(1.0, 1.0)"; break;
            case 3: one = "vec3(1.0, 1.0, 1.0)"; break;
            case 4: one = "vec4(1.0, 1.0, 1.0, 1.0)"; break;
            default: one = "1.0"; break;
        }
        gen.EmitLine(std::string(ValueTypeToGLSL(outType)) + " " + outVar + " = " + one + " - " + in + ";");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

// --- Vector Nodes ---

void RegisterDotNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Vector", "Dot"};
    def.displayName = "Dot Product";
    def.category = NodeCategory::Vector;
    def.description = "Dot product of two vectors";
    def.inputs = {
        {"A", ShaderValueType::Float3, glm::vec4(0.0f)},
        {"B", ShaderValueType::Float3, glm::vec4(0.0f)}
    };
    def.outputs = {{"Out", ShaderValueType::Float}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string a = gen.GetInputExpression(node, "A");
        std::string b = gen.GetInputExpression(node, "B");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        gen.EmitLine("float " + outVar + " = dot(" + a + ", " + b + ");");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

void RegisterCrossNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Vector", "Cross"};
    def.displayName = "Cross Product";
    def.category = NodeCategory::Vector;
    def.description = "Cross product of two vec3 vectors";
    def.inputs = {
        {"A", ShaderValueType::Float3, glm::vec4(0.0f)},
        {"B", ShaderValueType::Float3, glm::vec4(0.0f)}
    };
    def.outputs = {{"Out", ShaderValueType::Float3}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string a = gen.GetInputExpression(node, "A");
        std::string b = gen.GetInputExpression(node, "B");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        gen.EmitLine("vec3 " + outVar + " = cross(" + a + ", " + b + ");");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

void RegisterNormalizeNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Vector", "Normalize"};
    def.displayName = "Normalize";
    def.category = NodeCategory::Vector;
    def.description = "Normalize a vector";
    def.inputs = {{"In", ShaderValueType::Float3, glm::vec4(0.0f)}};
    def.outputs = {{"Out", ShaderValueType::Float3}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string in = gen.GetInputExpression(node, "In");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        gen.EmitLine("vec3 " + outVar + " = normalize(" + in + ");");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

void RegisterLengthNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Vector", "Length"};
    def.displayName = "Length";
    def.category = NodeCategory::Vector;
    def.description = "Length of a vector";
    def.inputs = {{"In", ShaderValueType::Float3, glm::vec4(0.0f)}};
    def.outputs = {{"Out", ShaderValueType::Float}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string in = gen.GetInputExpression(node, "In");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        gen.EmitLine("float " + outVar + " = length(" + in + ");");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

void RegisterDistanceNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Vector", "Distance"};
    def.displayName = "Distance";
    def.category = NodeCategory::Vector;
    def.description = "Distance between two points";
    def.inputs = {
        {"A", ShaderValueType::Float3, glm::vec4(0.0f)},
        {"B", ShaderValueType::Float3, glm::vec4(0.0f)}
    };
    def.outputs = {{"Out", ShaderValueType::Float}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string a = gen.GetInputExpression(node, "A");
        std::string b = gen.GetInputExpression(node, "B");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        gen.EmitLine("float " + outVar + " = distance(" + a + ", " + b + ");");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

void RegisterReflectNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Vector", "Reflect"};
    def.displayName = "Reflect";
    def.category = NodeCategory::Vector;
    def.description = "Reflect vector around normal";
    def.inputs = {
        {"In", ShaderValueType::Float3, glm::vec4(0.0f)},
        {"Normal", ShaderValueType::Float3, glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)}
    };
    def.outputs = {{"Out", ShaderValueType::Float3}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string in = gen.GetInputExpression(node, "In");
        std::string normal = gen.GetInputExpression(node, "Normal");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        gen.EmitLine("vec3 " + outVar + " = reflect(" + in + ", " + normal + ");");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

void RegisterCombineNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Vector", "Combine"};
    def.displayName = "Combine";
    def.category = NodeCategory::Vector;
    def.description = "Combine scalars into a vector";
    def.inputs = {
        {"R", ShaderValueType::Float, glm::vec4(0.0f)},
        {"G", ShaderValueType::Float, glm::vec4(0.0f)},
        {"B", ShaderValueType::Float, glm::vec4(0.0f)},
        {"A", ShaderValueType::Float, glm::vec4(1.0f)}
    };
    def.outputs = {
        {"RGBA", ShaderValueType::Float4},
        {"RGB", ShaderValueType::Float3},
        {"RG", ShaderValueType::Float2}
    };
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string r = gen.GetInputExpression(node, "R");
        std::string g = gen.GetInputExpression(node, "G");
        std::string b = gen.GetInputExpression(node, "B");
        std::string a = gen.GetInputExpression(node, "A");
        std::string baseVar = gen.MakeVarName(node.id, "combined");
        gen.EmitLine("vec4 " + baseVar + " = vec4(" + r + ", " + g + ", " + b + ", " + a + ");");
        gen.SetOutputExpression(node, "RGBA", baseVar);
        gen.SetOutputExpression(node, "RGB", baseVar + ".rgb");
        gen.SetOutputExpression(node, "RG", baseVar + ".rg");
        return true;
    };
    reg.Register(def);
}

void RegisterSplitNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Vector", "Split"};
    def.displayName = "Split";
    def.category = NodeCategory::Vector;
    def.description = "Split a vector into components";
    def.inputs = {{"In", ShaderValueType::Float4, glm::vec4(0.0f)}};
    def.outputs = {
        {"R", ShaderValueType::Float},
        {"G", ShaderValueType::Float},
        {"B", ShaderValueType::Float},
        {"A", ShaderValueType::Float}
    };
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string in = gen.GetInputExpression(node, "In");
        std::string baseVar = gen.MakeVarName(node.id, "split");
        gen.EmitLine("vec4 " + baseVar + " = " + in + ";");
        gen.SetOutputExpression(node, "R", baseVar + ".r");
        gen.SetOutputExpression(node, "G", baseVar + ".g");
        gen.SetOutputExpression(node, "B", baseVar + ".b");
        gen.SetOutputExpression(node, "A", baseVar + ".a");
        return true;
    };
    reg.Register(def);
}

void RegisterSwizzleNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Vector", "Swizzle"};
    def.displayName = "Swizzle";
    def.category = NodeCategory::Vector;
    def.description = "Swizzle vector components";
    def.inputs = {{"In", ShaderValueType::Float4, glm::vec4(0.0f)}};
    def.outputs = {{"Out", ShaderValueType::Any}};
    def.properties = {{"mask", "Mask", "xyzw", {"x", "y", "z", "w", "xy", "xz", "xw", "yz", "yw", "zw", "xyz", "xyw", "xzw", "yzw", "xyzw", "xxxx", "yyyy", "zzzz", "wwww"}}};
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string in = gen.GetInputExpression(node, "In");
        std::string mask = node.GetProperty("mask", "xyzw");
        std::string outVar = gen.MakeVarName(node.id, "Out");
        int outSize = static_cast<int>(mask.size());
        const char* outType = outSize == 1 ? "float" : (outSize == 2 ? "vec2" : (outSize == 3 ? "vec3" : "vec4"));
        gen.EmitLine(std::string(outType) + " " + outVar + " = (" + in + ")." + mask + ";");
        gen.SetOutputExpression(node, "Out", outVar);
        return true;
    };
    reg.Register(def);
}

// --- Texture Nodes ---

void RegisterSampleTexture2DNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Texture", "Sample2D"};
    def.displayName = "Sample Texture 2D";
    def.category = NodeCategory::Texture;
    def.description = "Sample a 2D texture";
    def.inputs = {
        {"Texture", ShaderValueType::Texture2D, glm::vec4(0.0f), true},
        {"UV", ShaderValueType::Float2, glm::vec4(0.0f)}
    };
    def.outputs = {
        {"RGBA", ShaderValueType::Color4},
        {"RGB", ShaderValueType::Color3},
        {"R", ShaderValueType::Float},
        {"G", ShaderValueType::Float},
        {"B", ShaderValueType::Float},
        {"A", ShaderValueType::Float}
    };
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string sampler = gen.GetTextureSamplerName(node, "Texture");
        std::string uv = gen.GetInputExpression(node, "UV");
        
        // If UV not connected, use default texcoord
        if (uv.empty() || uv == "vec2(0.0, 0.0)") {
            gen.RequireVarying("v_texcoord0");
            uv = "v_texcoord0.xy";
        }
        
        std::string baseVar = gen.MakeVarName(node.id, "texSample");
        gen.EmitLine("vec4 " + baseVar + " = texture2D(" + sampler + ", " + uv + ");");
        gen.SetOutputExpression(node, "RGBA", baseVar);
        gen.SetOutputExpression(node, "RGB", baseVar + ".rgb");
        gen.SetOutputExpression(node, "R", baseVar + ".r");
        gen.SetOutputExpression(node, "G", baseVar + ".g");
        gen.SetOutputExpression(node, "B", baseVar + ".b");
        gen.SetOutputExpression(node, "A", baseVar + ".a");
        return true;
    };
    reg.Register(def);
}

// --- Output/Master Nodes ---

void RegisterUnlitMasterNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Output", "UnlitMaster"};
    def.displayName = "Unlit Master";
    def.category = NodeCategory::Output;
    def.description = "Unlit surface output (no lighting)";
    def.inputs = {
        {"Color", ShaderValueType::Color3, glm::vec4(1.0f)},
        {"Alpha", ShaderValueType::Float, glm::vec4(1.0f)}
    };
    // Master nodes have no outputs - they ARE the output
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        std::string color = gen.GetInputExpression(node, "Color");
        std::string alpha = gen.GetInputExpression(node, "Alpha");
        
        bool applyFog = gen.ShouldApplyFog();
        
        if (applyFog) {
            gen.RequireUniform("u_cameraPos", "vec4");
            gen.RequireUniform("u_ambientFog", "vec4");
            gen.RequireUniform("u_fogParams", "vec4");
            gen.RequireVarying("v_worldPos");
            
            gen.EmitLine("vec3 unlitColor = " + color + ";");
            gen.EmitLine("");
            gen.EmitLine("// Exponential fog");
            gen.EmitLine("if (u_ambientFog.w > 0.5) {");
            gen.EmitLine("    float fogDist = length(v_worldPos - u_cameraPos.xyz);");
            gen.EmitLine("    float fogFactor = 1.0 - clamp(exp(-u_fogParams.x * fogDist), 0.0, 1.0);");
            gen.EmitLine("    vec3 fogColor = u_fogParams.yzw;");
            gen.EmitLine("    unlitColor = mix(unlitColor, fogColor, fogFactor);");
            gen.EmitLine("}");
            gen.EmitLine("gl_FragColor = vec4(unlitColor, " + alpha + ");");
        } else {
            gen.EmitLine("gl_FragColor = vec4(" + color + ", " + alpha + ");");
        }
        return true;
    };
    reg.Register(def);
}

void RegisterPBRMasterNode(NodeRegistry& reg) {
    NodeDefinition def;
    def.typeId = {"Output", "PBRMaster"};
    def.displayName = "PBR Master";
    def.category = NodeCategory::Output;
    def.description = "PBR lit surface output";
    def.inputs = {
        {"BaseColor", ShaderValueType::Color3, glm::vec4(1.0f)},
        {"Metallic", ShaderValueType::Float, glm::vec4(0.0f)},
        {"Roughness", ShaderValueType::Float, glm::vec4(0.5f)},
        {"Normal", ShaderValueType::Float3, glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)},
        {"Emission", ShaderValueType::Color3, glm::vec4(0.0f)},
        {"AO", ShaderValueType::Float, glm::vec4(1.0f)},
        {"Alpha", ShaderValueType::Float, glm::vec4(1.0f)}
    };
    def.codeGen = [](ShaderGraphCodeGen& gen, const ShaderNode& node) -> bool {
        // Get all inputs
        std::string baseColor = gen.GetInputExpression(node, "BaseColor");
        std::string metallic = gen.GetInputExpression(node, "Metallic");
        std::string roughness = gen.GetInputExpression(node, "Roughness");
        std::string normal = gen.GetInputExpression(node, "Normal");
        std::string emission = gen.GetInputExpression(node, "Emission");
        std::string ao = gen.GetInputExpression(node, "AO");
        std::string alpha = gen.GetInputExpression(node, "Alpha");
        
        bool applyAmbient = gen.ShouldApplyAmbient();
        bool applyFog = gen.ShouldApplyFog();
        
        // Require PBR lighting uniforms and lib
        gen.RequireInclude("lib_pbr_common.sh");
        gen.RequireUniform("u_lightColors", "vec4[4]");
        gen.RequireUniform("u_lightPositions", "vec4[4]");
        gen.RequireUniform("u_lightParams", "vec4[4]");
        gen.RequireUniform("u_cameraPos", "vec4");
        gen.RequireUniform("u_ambientFog", "vec4");
        if (applyFog) {
            gen.RequireUniform("u_fogParams", "vec4");
        }
        gen.RequireVarying("v_worldPos");
        gen.RequireVarying("v_normal");
        gen.RequireVarying("v_viewDir");
        
        // Emit PBR lighting code
        gen.EmitLine("");
        gen.EmitLine("// PBR Lighting");
        gen.EmitLine("vec3 N = normalize(" + normal + ");");
        gen.EmitLine("vec3 V = normalize(v_viewDir);");
        gen.EmitLine("vec3 albedo = " + baseColor + ";");
        gen.EmitLine("float metal = " + metallic + ";");
        gen.EmitLine("float rough = clamp(" + roughness + ", 0.04, 1.0);");
        gen.EmitLine("float occlusion = " + ao + ";");
        gen.EmitLine("");
        
        // Ambient lighting (conditional)
        if (applyAmbient) {
            gen.EmitLine("vec3 ambientIrradiance = u_ambientFog.xyz;");
            gen.EmitLine("vec3 diffuseColor = albedo * (1.0 - metal);");
            gen.EmitLine("vec3 ambientColor = diffuseColor * ambientIrradiance * occlusion;");
            gen.EmitLine("vec3 finalColor = ambientColor;");
        } else {
            gen.EmitLine("vec3 finalColor = vec3(0.0, 0.0, 0.0);");
        }
        gen.EmitLine("");
        gen.EmitLine("// Process lights");
        gen.EmitLine("for (int i = 0; i < 4; i++) {");
        gen.EmitLine("    float lightType = u_lightPositions[i].w;");
        gen.EmitLine("    vec3 lightColor = u_lightColors[i].rgb;");
        gen.EmitLine("    float lightIntensity = u_lightColors[i].a;");
        gen.EmitLine("    vec3 L;");
        gen.EmitLine("    float attenuation = 1.0;");
        gen.EmitLine("    if (lightType < 0.5) {");
        gen.EmitLine("        L = normalize(-u_lightPositions[i].xyz);");
        gen.EmitLine("    } else {");
        gen.EmitLine("        vec3 lightPos = u_lightPositions[i].xyz;");
        gen.EmitLine("        vec3 lightDir = lightPos - v_worldPos;");
        gen.EmitLine("        float dist = length(lightDir);");
        gen.EmitLine("        L = normalize(lightDir);");
        gen.EmitLine("        float range = u_lightParams[i].x;");
        gen.EmitLine("        if (range > 0.0 && dist > range) continue;");
        gen.EmitLine("        float constant = u_lightParams[i].y;");
        gen.EmitLine("        float linearTerm = u_lightParams[i].z;");
        gen.EmitLine("        float quadratic = u_lightParams[i].w;");
        gen.EmitLine("        attenuation = 1.0 / (constant + linearTerm * dist + quadratic * dist * dist);");
        gen.EmitLine("    }");
        gen.EmitLine("    finalColor += CalculatePBRLighting(N, V, L, albedo, metal, rough, lightColor, lightIntensity) * attenuation * occlusion;");
        gen.EmitLine("}");
        gen.EmitLine("");
        gen.EmitLine("finalColor += " + emission + ";");
        
        // Fog (conditional)
        if (applyFog) {
            gen.EmitLine("");
            gen.EmitLine("// Exponential fog");
            gen.EmitLine("if (u_ambientFog.w > 0.5) {");
            gen.EmitLine("    float fogDist = length(v_worldPos - u_cameraPos.xyz);");
            gen.EmitLine("    float fogFactor = 1.0 - clamp(exp(-u_fogParams.x * fogDist), 0.0, 1.0);");
            gen.EmitLine("    vec3 fogColor = u_fogParams.yzw;");
            gen.EmitLine("    finalColor = mix(finalColor, fogColor, fogFactor);");
            gen.EmitLine("}");
        }
        
        gen.EmitLine("gl_FragColor = vec4(finalColor, " + alpha + ");");
        
        return true;
    };
    reg.Register(def);
}

void RegisterAllBuiltins(NodeRegistry& reg) {
    // Input nodes
    RegisterFloatNode(reg);
    RegisterFloat2Node(reg);
    RegisterFloat3Node(reg);
    RegisterFloat4Node(reg);
    RegisterColorNode(reg);
    RegisterTimeNode(reg);
    RegisterUVNode(reg);
    RegisterTilingOffsetNode(reg);
    RegisterRotateUVNode(reg);
    RegisterPolarCoordinatesNode(reg);
    RegisterWorldPositionNode(reg);
    RegisterWorldNormalNode(reg);
    RegisterViewDirectionNode(reg);
    RegisterCameraPositionNode(reg);
    RegisterTexture2DNode(reg);
    
    // Math nodes
    RegisterAddNode(reg);
    RegisterSubtractNode(reg);
    RegisterMultiplyNode(reg);
    RegisterDivideNode(reg);
    RegisterPowerNode(reg);
    RegisterSqrtNode(reg);
    RegisterAbsNode(reg);
    RegisterNegateNode(reg);
    RegisterSinNode(reg);
    RegisterCosNode(reg);
    RegisterFloorNode(reg);
    RegisterCeilNode(reg);
    RegisterFracNode(reg);
    RegisterMinNode(reg);
    RegisterMaxNode(reg);
    RegisterClampNode(reg);
    RegisterSaturateNode(reg);
    RegisterLerpNode(reg);
    RegisterStepNode(reg);
    RegisterSmoothstepNode(reg);
    RegisterOneMinus(reg);
    
    // Vector nodes
    RegisterDotNode(reg);
    RegisterCrossNode(reg);
    RegisterNormalizeNode(reg);
    RegisterLengthNode(reg);
    RegisterDistanceNode(reg);
    RegisterReflectNode(reg);
    RegisterCombineNode(reg);
    RegisterSplitNode(reg);
    RegisterSwizzleNode(reg);
    
    // Texture nodes
    RegisterSampleTexture2DNode(reg);
    
    // Output nodes
    RegisterUnlitMasterNode(reg);
    RegisterPBRMasterNode(reg);
}

} // namespace nodes
} // namespace shadergraph


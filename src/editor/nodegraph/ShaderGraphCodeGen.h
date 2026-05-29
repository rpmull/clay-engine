#pragma once

#include "ShaderGraph.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <sstream>

namespace shadergraph {

// Result of shader compilation
struct ShaderCompileResult {
    bool success = false;
    std::string vertexShaderSource;
    std::string fragmentShaderSource;
    std::string vertexShaderPath;    // Path where VS was written
    std::string fragmentShaderPath;  // Path where FS was written
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    
    // Parameters for material binding
    struct Parameter {
        std::string name;          // Uniform name (e.g., "u_BaseColor")
        std::string displayName;   // UI display name (e.g., "Base Color")
        ShaderValueType type;
        glm::vec4 defaultValue;
        std::string texturePath;   // For texture params
        int textureSlot = -1;      // Assigned texture slot for samplers
    };
    std::vector<Parameter> parameters;
};

// Code generator for shader graphs
class ShaderGraphCodeGen {
public:
    ShaderGraphCodeGen(const ShaderGraph& graph);
    
    // Compile the graph to bgfx shader sources
    ShaderCompileResult Compile();
    
    // --- Methods called by node codegen functions ---
    
    // Get the expression for an input pin (evaluates upstream if needed)
    std::string GetInputExpression(const ShaderNode& node, const std::string& pinName);
    
    // Get the expression for a specific pin ID
    std::string GetInputExpression(PinId pinId);
    
    // Set the expression for an output pin
    void SetOutputExpression(const ShaderNode& node, const std::string& pinName, const std::string& expr);
    
    // Make a unique variable name for a node
    std::string MakeVarName(NodeId nodeId, const std::string& suffix);
    
    // Emit a line of shader code
    void EmitLine(const std::string& line);
    
    // Deduce the output type for an 'Any' typed output based on inputs
    ShaderValueType DeduceOutputType(const ShaderNode& node, const std::string& outputPinName);
    
    // Request that a uniform be declared
    void RequireUniform(const std::string& name, const std::string& type);
    
    // Request that an include be added
    void RequireInclude(const std::string& path);
    
    // Request that a varying be used in fragment shader
    void RequireVarying(const std::string& name);
    
    // Get the sampler name for a texture input
    std::string GetTextureSamplerName(const ShaderNode& node, const std::string& pinName);
    
    // Environment settings from graph
    bool ShouldApplyFog() const { return m_Graph.applyFog; }
    bool ShouldApplyAmbient() const { return m_Graph.applyAmbient; }
    
private:
    const ShaderGraph& m_Graph;
    ShaderCompileResult m_Result;
    
    // Topologically sorted nodes
    std::vector<const ShaderNode*> m_SortedNodes;
    
    // Output expressions for each output pin
    std::unordered_map<PinId, std::string> m_OutputExpressions;
    
    // Set of nodes that have been evaluated
    std::unordered_set<NodeId> m_EvaluatedNodes;
    
    // Current shader code being built
    std::stringstream m_FragmentCode;
    std::string m_CurrentIndent;
    
    // Required declarations
    std::unordered_map<std::string, std::string> m_RequiredUniforms;  // name -> type
    std::unordered_set<std::string> m_RequiredIncludes;
    std::unordered_set<std::string> m_RequiredVaryings;
    
    // Texture samplers: pin ID -> sampler info
    struct SamplerInfo {
        std::string name;
        int slot;
        std::string texturePath;  // Default texture path if any
    };
    std::unordered_map<PinId, SamplerInfo> m_Samplers;
    int m_NextTextureSlot = 0;
    
    // Cache for deduced output types (for 'Any' typed pins that were evaluated)
    std::unordered_map<PinId, ShaderValueType> m_DeducedTypes;
    
    // Validation
    bool Validate();
    bool ValidateNoScalarSplats(const std::string& code);
    
    // Topological sort
    bool TopologicalSort();
    
    // Evaluate a node (generate its code)
    bool EvaluateNode(const ShaderNode& node);
    
    // Generate the vertex shader
    std::string GenerateVertexShader();
    
    // Generate the fragment shader
    std::string GenerateFragmentShader();
    
    // Generate uniform declarations
    std::string GenerateUniformDeclarations();
    
    // Generate sampler declarations
    std::string GenerateSamplerDeclarations();
    
    // Get default value as GLSL literal
    std::string DefaultValueToGLSL(const ShaderPin& pin);
    
    // Extract parameters from exposed pins
    void ExtractParameters();
    
    // Reserved uniform names (bgfx built-ins we must not redeclare)
    static const std::unordered_set<std::string> s_ReservedUniforms;
};

// Result of writing and compiling shaders
struct ShaderCompileOutput {
    bool success = false;
    std::string error;
    std::string vsName;       // e.g., "vs_shgraph_ColorShader"
    std::string fsName;       // e.g., "fs_shgraph_ColorShader"
    std::string vsBinPath;    // Path to compiled VS binary (relative to project)
    std::string fsBinPath;    // Path to compiled FS binary (relative to project)
};

// Write shader sources to files and compile via shaderc
// projectRoot: path to the Clay project root (where .bin will be created)
// baseName: base name for the shader (e.g., "shgraph_ColorShader")
ShaderCompileOutput WriteAndCompileShaders(const ShaderCompileResult& result, 
                                           const std::string& projectRoot,
                                           const std::string& baseName);

} // namespace shadergraph


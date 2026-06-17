#include "ShaderGraphCodeGen.h"
#include "ShaderGraphNodes.h"
#include <algorithm>
#include <queue>
#include <fstream>
#include <filesystem>
#include <regex>
#include <cctype>

namespace shadergraph {

// Sanitize a name to be a valid shader identifier (replace spaces and special chars with underscores)
static std::string SanitizeIdentifier(const std::string& name) {
    std::string result = name;
    for (char& c : result) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            c = '_';
        }
    }
    return result;
}

// Reserved bgfx built-in uniforms that must not be redeclared
const std::unordered_set<std::string> ShaderGraphCodeGen::s_ReservedUniforms = {
    "u_viewRect", "u_viewTexel", "u_view", "u_invView", "u_proj", "u_invProj",
    "u_viewProj", "u_invViewProj", "u_model", "u_modelView", "u_modelViewProj",
    "u_alphaRef4"
};

ShaderGraphCodeGen::ShaderGraphCodeGen(const ShaderGraph& graph)
    : m_Graph(graph)
{
}

ShaderCompileResult ShaderGraphCodeGen::Compile() {
    m_Result = ShaderCompileResult();
    m_OutputExpressions.clear();
    m_EvaluatedNodes.clear();
    m_FragmentCode.str("");
    m_FragmentCode.clear();
    m_CurrentIndent = "    ";  // Start with base indentation
    m_RequiredUniforms.clear();
    m_RequiredIncludes.clear();
    m_RequiredVaryings.clear();
    m_Samplers.clear();
    m_NextTextureSlot = 0;
    m_DeducedTypes.clear();
    
    // Validate the graph
    if (!Validate()) {
        m_Result.success = false;
        return m_Result;
    }
    
    // Topologically sort nodes
    if (!TopologicalSort()) {
        m_Result.errors.push_back("Graph contains cycles");
        m_Result.success = false;
        return m_Result;
    }
    
    // Extract texture samplers from exposed texture pins
    for (const auto& pin : m_Graph.pins) {
        if (pin.exposed && pin.type == ShaderValueType::Texture2D) {
            SamplerInfo info;
            std::string uniformName = pin.exposedName.empty() ? pin.name : pin.exposedName;
            uniformName = SanitizeIdentifier(uniformName);  // Remove spaces and special chars
            // Ensure uniform name starts with s_ for samplers
            if (uniformName.substr(0, 2) != "s_") {
                uniformName = "s_" + uniformName;
            }
            info.name = uniformName;
            info.slot = m_NextTextureSlot++;
            info.texturePath = pin.defaultTexturePath;
            m_Samplers[pin.id] = info;
        }
    }
    
    // Evaluate all nodes in topological order
    for (const ShaderNode* node : m_SortedNodes) {
        if (!EvaluateNode(*node)) {
            m_Result.errors.push_back("Failed to evaluate node: " + node->displayName);
            m_Result.success = false;
            return m_Result;
        }
    }
    
    // bgfx requires every fragment-shader input varying to also be written by
    // the vertex shader, otherwise createProgram() fails to link. Graphs that
    // need no interpolated data (e.g. a constant Color -> Unlit Master) leave
    // m_RequiredVaryings empty, yet GenerateFragmentShader() still emits a
    // fallback "$input v_texcoord0" so the stage has at least one varying. If
    // the vertex stage doesn't also emit v_texcoord0 the program silently fails
    // to load and the material falls back to the default PBR shader (black
    // preview). Guarantee both stages agree on the fallback varying.
    if (m_RequiredVaryings.empty()) {
        RequireVarying("v_texcoord0");
    }

    // Generate shader sources
    m_Result.vertexShaderSource = GenerateVertexShader();
    m_Result.fragmentShaderSource = GenerateFragmentShader();
    
    // Validate no scalar splats
    if (!ValidateNoScalarSplats(m_Result.vertexShaderSource)) {
        m_Result.errors.push_back("Vertex shader contains invalid scalar splat constructor (vec3(x) or vec4(x))");
    }
    if (!ValidateNoScalarSplats(m_Result.fragmentShaderSource)) {
        m_Result.errors.push_back("Fragment shader contains invalid scalar splat constructor (vec3(x) or vec4(x))");
    }
    
    // Extract parameters for material binding
    ExtractParameters();
    
    m_Result.success = m_Result.errors.empty();
    return m_Result;
}

bool ShaderGraphCodeGen::Validate() {
    // Must have exactly one master/output node
    const ShaderNode* master = m_Graph.FindMasterNode();
    if (!master) {
        m_Result.errors.push_back("Graph must have exactly one Output node (UnlitMaster or PBRMaster)");
        return false;
    }
    
    // Check for cycles
    if (m_Graph.HasCycle()) {
        m_Result.errors.push_back("Graph contains cycles");
        return false;
    }
    
    // Validate all links have compatible types
    for (const auto& link : m_Graph.links) {
        const ShaderPin* fromPin = m_Graph.FindPin(link.fromPin);
        const ShaderPin* toPin = m_Graph.FindPin(link.toPin);
        if (!fromPin || !toPin) {
            m_Result.errors.push_back("Invalid link: pins not found");
            return false;
        }
        if (!AreTypesCompatible(fromPin->type, toPin->type)) {
            m_Result.errors.push_back("Type mismatch: cannot connect " + 
                std::string(ValueTypeToGLSL(fromPin->type)) + " to " + 
                std::string(ValueTypeToGLSL(toPin->type)));
            return false;
        }
    }
    
    return true;
}

bool ShaderGraphCodeGen::ValidateNoScalarSplats(const std::string& code) {
    // Check for patterns like vec3(x) or vec4(x) where x is a single identifier/number
    // This is a simplified check - real validation would be more sophisticated
    std::regex scalarSplat(R"(vec[234]\s*\(\s*[a-zA-Z_][a-zA-Z0-9_]*\s*\))");
    std::regex scalarSplatNum(R"(vec[234]\s*\(\s*[\d.]+\s*\))");
    
    // Check for vec3(x) patterns (single argument)
    // We need to allow vec3(x, y, z) but not vec3(x)
    std::regex singleArgVec(R"(vec([234])\s*\(\s*([^,()]+)\s*\))");
    std::smatch match;
    std::string::const_iterator searchStart(code.cbegin());
    
    while (std::regex_search(searchStart, code.cend(), match, singleArgVec)) {
        std::string vecSize = match[1];
        std::string arg = match[2];
        
        // Check if the single argument is not already a vec or function call
        // Simple heuristic: if it doesn't contain ( it might be a scalar splat
        if (arg.find('(') == std::string::npos && arg.find(',') == std::string::npos) {
            // This looks like vec3(x) with a single scalar - that's bad!
            // But we need to be careful: vec4(v.xyz, 1.0) is fine
            // For now just warn about obvious cases
            m_Result.warnings.push_back("Potential scalar splat detected: vec" + vecSize + "(" + arg + ")");
        }
        searchStart = match.suffix().first;
    }
    
    return true;  // Don't fail, just warn
}

bool ShaderGraphCodeGen::TopologicalSort() {
    m_SortedNodes.clear();
    
    // Build dependency graph
    std::unordered_map<NodeId, std::vector<NodeId>> deps;  // node -> nodes it depends on
    std::unordered_map<NodeId, int> inDegree;
    
    for (const auto& node : m_Graph.nodes) {
        deps[node.id] = {};
        inDegree[node.id] = 0;
    }
    
    // For each link, the destination node depends on the source node
    for (const auto& link : m_Graph.links) {
        const ShaderPin* fromPin = m_Graph.FindPin(link.fromPin);
        const ShaderPin* toPin = m_Graph.FindPin(link.toPin);
        if (!fromPin || !toPin) continue;
        
        NodeId srcNode = fromPin->nodeId;
        NodeId dstNode = toPin->nodeId;
        
        deps[dstNode].push_back(srcNode);
        inDegree[srcNode]++;  // srcNode is depended on
    }
    
    // Kahn's algorithm - but we want reverse order (sources first)
    std::queue<NodeId> queue;
    for (const auto& [id, degree] : inDegree) {
        bool hasDeps = false;
        for (const auto& link : m_Graph.links) {
            const ShaderPin* toPin = m_Graph.FindPin(link.toPin);
            if (toPin && toPin->nodeId == id) {
                hasDeps = true;
                break;
            }
        }
        if (!hasDeps) {
            queue.push(id);
        }
    }
    
    // Actually we need proper topo sort where dependencies come first
    // Let's redo this correctly
    m_SortedNodes.clear();
    std::unordered_map<NodeId, int> incomingCount;
    
    for (const auto& node : m_Graph.nodes) {
        incomingCount[node.id] = 0;
    }
    
    for (const auto& link : m_Graph.links) {
        const ShaderPin* toPin = m_Graph.FindPin(link.toPin);
        if (toPin) {
            incomingCount[toPin->nodeId]++;
        }
    }
    
    std::queue<NodeId> q;
    for (const auto& [id, count] : incomingCount) {
        if (count == 0) {
            q.push(id);
        }
    }
    
    while (!q.empty()) {
        NodeId curr = q.front();
        q.pop();
        
        const ShaderNode* node = m_Graph.FindNode(curr);
        if (node) {
            m_SortedNodes.push_back(node);
        }
        
        // Find all nodes that this node outputs to
        for (const auto& link : m_Graph.links) {
            const ShaderPin* fromPin = m_Graph.FindPin(link.fromPin);
            const ShaderPin* toPin = m_Graph.FindPin(link.toPin);
            if (!fromPin || !toPin) continue;
            
            if (fromPin->nodeId == curr) {
                NodeId dstNode = toPin->nodeId;
                incomingCount[dstNode]--;
                if (incomingCount[dstNode] == 0) {
                    q.push(dstNode);
                }
            }
        }
    }
    
    return m_SortedNodes.size() == m_Graph.nodes.size();
}

bool ShaderGraphCodeGen::EvaluateNode(const ShaderNode& node) {
    if (m_EvaluatedNodes.count(node.id)) {
        return true;  // Already evaluated
    }
    
    // Find the node definition
    const NodeDefinition* def = NodeRegistry::Instance().Find(node.typeId);
    if (!def) {
        m_Result.errors.push_back("Unknown node type: " + node.typeId.Full());
        return false;
    }
    
    // Call the code generation function
    if (!def->codeGen) {
        m_Result.errors.push_back("Node type has no code generator: " + node.typeId.Full());
        return false;
    }
    
    bool success = def->codeGen(*this, node);
    if (success) {
        m_EvaluatedNodes.insert(node.id);
    }
    return success;
}

std::string ShaderGraphCodeGen::GetInputExpression(const ShaderNode& node, const std::string& pinName) {
    // Find the input pin
    for (PinId pinId : node.inputPins) {
        const ShaderPin* pin = m_Graph.FindPin(pinId);
        if (pin && pin->name == pinName) {
            return GetInputExpression(pinId);
        }
    }
    
    // Pin not found - return default
    m_Result.warnings.push_back("Input pin not found: " + pinName + " on node " + node.displayName);
    return "0.0";
}

std::string ShaderGraphCodeGen::GetInputExpression(PinId pinId) {
    const ShaderPin* pin = m_Graph.FindPin(pinId);
    if (!pin) return "0.0";
    
    // Check if there's a link to this pin
    const ShaderLink* link = m_Graph.FindLinkToPin(pinId);
    if (link) {
        // There's a connection - get the output expression from the source
        auto it = m_OutputExpressions.find(link->fromPin);
        if (it != m_OutputExpressions.end()) {
            // Apply type conversion if needed
            const ShaderPin* fromPin = m_Graph.FindPin(link->fromPin);
            if (fromPin && fromPin->type != pin->type) {
                return GetTypeConversion(fromPin->type, pin->type, it->second);
            }
            return it->second;
        }
        m_Result.warnings.push_back("Source output not evaluated for pin");
        return "0.0";
    }
    
    // No connection - check if this is an exposed parameter
    if (pin->exposed) {
        std::string uniformName = pin->exposedName.empty() ? pin->name : pin->exposedName;
        uniformName = SanitizeIdentifier(uniformName);  // Remove spaces and special chars
        
        // Handle texture samplers specially
        if (pin->type == ShaderValueType::Texture2D) {
            auto it = m_Samplers.find(pinId);
            if (it != m_Samplers.end()) {
                return it->second.name;
            }
        }
        
        // Ensure uniform name is valid
        if (uniformName.substr(0, 2) != "u_") {
            uniformName = "u_" + uniformName;
        }
        
        // Add to required uniforms (if not reserved)
        if (s_ReservedUniforms.find(uniformName) == s_ReservedUniforms.end()) {
            m_RequiredUniforms[uniformName] = ValueTypeToGLSL(pin->type);
        }
        
        // bgfx uniforms are always vec4, so add swizzle for smaller types
        switch (pin->type) {
            case ShaderValueType::Float:
            case ShaderValueType::Int:
            case ShaderValueType::Bool:
                return uniformName + ".x";
            case ShaderValueType::Float2:
                return uniformName + ".xy";
            case ShaderValueType::Float3:
            case ShaderValueType::Color3:
                return uniformName + ".xyz";
            default:
                return uniformName;
        }
    }
    
    // Return default value as literal
    return DefaultValueToGLSL(*pin);
}

void ShaderGraphCodeGen::SetOutputExpression(const ShaderNode& node, const std::string& pinName, const std::string& expr) {
    for (PinId pinId : node.outputPins) {
        const ShaderPin* pin = m_Graph.FindPin(pinId);
        if (pin && pin->name == pinName) {
            m_OutputExpressions[pinId] = expr;
            return;
        }
    }
}

std::string ShaderGraphCodeGen::MakeVarName(NodeId nodeId, const std::string& suffix) {
    return "n" + std::to_string(nodeId) + "_" + suffix;
}

void ShaderGraphCodeGen::EmitLine(const std::string& line) {
    m_FragmentCode << m_CurrentIndent << line << "\n";
}

ShaderValueType ShaderGraphCodeGen::DeduceOutputType(const ShaderNode& node, const std::string& outputPinName) {
    // Find the output pin to cache the result
    PinId outputPinId = 0;
    for (PinId pinId : node.outputPins) {
        const ShaderPin* pin = m_Graph.FindPin(pinId);
        if (pin && pin->name == outputPinName) {
            outputPinId = pinId;
            // Check if already cached
            auto cached = m_DeducedTypes.find(pinId);
            if (cached != m_DeducedTypes.end()) {
                return cached->second;
            }
            break;
        }
    }
    
    // For 'Any' typed outputs, deduce from inputs
    // Find the largest input type
    ShaderValueType maxType = ShaderValueType::Float;
    int maxComponents = 1;
    
    for (PinId pinId : node.inputPins) {
        const ShaderPin* pin = m_Graph.FindPin(pinId);
        if (!pin) continue;
        
        // Get the actual type (from connected pin or default)
        ShaderValueType actualType = pin->type;
        const ShaderLink* link = m_Graph.FindLinkToPin(pinId);
        if (link) {
            const ShaderPin* srcPin = m_Graph.FindPin(link->fromPin);
            if (srcPin) {
                actualType = srcPin->type;
                // If source is 'Any', check our cache for its deduced type
                if (actualType == ShaderValueType::Any) {
                    auto cachedSrc = m_DeducedTypes.find(link->fromPin);
                    if (cachedSrc != m_DeducedTypes.end()) {
                        actualType = cachedSrc->second;
                    }
                }
            }
        }
        
        // Skip 'Any' types - they don't give us useful size information
        if (actualType == ShaderValueType::Any) continue;
        
        int components = ValueTypeComponentCount(actualType);
        if (components > maxComponents) {
            maxComponents = components;
            maxType = actualType;
        }
    }
    
    // Cache the deduced type for this output pin
    if (outputPinId != 0) {
        m_DeducedTypes[outputPinId] = maxType;
    }
    
    return maxType;
}

void ShaderGraphCodeGen::RequireUniform(const std::string& name, const std::string& type) {
    if (s_ReservedUniforms.find(name) == s_ReservedUniforms.end()) {
        m_RequiredUniforms[name] = type;
    }
}

void ShaderGraphCodeGen::RequireInclude(const std::string& path) {
    m_RequiredIncludes.insert(path);
}

void ShaderGraphCodeGen::RequireVarying(const std::string& name) {
    m_RequiredVaryings.insert(name);
}

std::string ShaderGraphCodeGen::GetTextureSamplerName(const ShaderNode& node, const std::string& pinName) {
    // Find the input pin for the texture
    for (PinId pinId : node.inputPins) {
        const ShaderPin* pin = m_Graph.FindPin(pinId);
        if (pin && pin->name == pinName && pin->type == ShaderValueType::Texture2D) {
            // Check if there's a sampler assigned
            auto it = m_Samplers.find(pinId);
            if (it != m_Samplers.end()) {
                return it->second.name;
            }
            
            // Check if connected to a texture node
            const ShaderLink* link = m_Graph.FindLinkToPin(pinId);
            if (link) {
                auto srcIt = m_Samplers.find(link->fromPin);
                if (srcIt != m_Samplers.end()) {
                    return srcIt->second.name;
                }
                // Check if source output expression is a sampler name
                auto exprIt = m_OutputExpressions.find(link->fromPin);
                if (exprIt != m_OutputExpressions.end()) {
                    return exprIt->second;
                }
            }
            
            // Create a new sampler for this pin
            SamplerInfo info;
            std::string baseName = pin->exposedName.empty() ? pin->name : pin->exposedName;
            baseName = SanitizeIdentifier(baseName);  // Remove spaces and special chars
            if (baseName.substr(0, 2) != "s_") {
                baseName = "s_" + baseName;
            }
            info.name = baseName;
            info.slot = m_NextTextureSlot++;
            info.texturePath = pin->defaultTexturePath;
            m_Samplers[pinId] = info;
            return info.name;
        }
    }
    
    // Fallback
    return "s_texture" + std::to_string(m_NextTextureSlot++);
}

std::string ShaderGraphCodeGen::DefaultValueToGLSL(const ShaderPin& pin) {
    const glm::vec4& v = pin.defaultValue;
    
    switch (pin.type) {
        case ShaderValueType::Float:
            return std::to_string(v.x);
        case ShaderValueType::Float2:
            return "vec2(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ")";
        case ShaderValueType::Float3:
        case ShaderValueType::Color3:
            return "vec3(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ")";
        case ShaderValueType::Float4:
        case ShaderValueType::Color4:
            return "vec4(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ", " + std::to_string(v.w) + ")";
        case ShaderValueType::Int:
            return std::to_string(static_cast<int>(v.x));
        case ShaderValueType::Bool:
            return v.x > 0.5f ? "true" : "false";
        default:
            return "0.0";
    }
}

void ShaderGraphCodeGen::ExtractParameters() {
    for (const auto& pin : m_Graph.pins) {
        if (!pin.exposed || pin.kind != PinKind::Input) continue;
        
        ShaderCompileResult::Parameter param;
        param.displayName = pin.exposedName.empty() ? pin.name : pin.exposedName;
        param.type = pin.type;
        param.defaultValue = pin.defaultValue;
        
        if (pin.type == ShaderValueType::Texture2D) {
            auto it = m_Samplers.find(pin.id);
            if (it != m_Samplers.end()) {
                param.name = it->second.name;
                param.textureSlot = it->second.slot;
                param.texturePath = it->second.texturePath;
            }
        } else {
            param.name = SanitizeIdentifier(param.displayName);
            if (param.name.substr(0, 2) != "u_") {
                param.name = "u_" + param.name;
            }
        }
        
        m_Result.parameters.push_back(param);
    }
}

std::string ShaderGraphCodeGen::GenerateVertexShader() {
    std::stringstream ss;
    
    // Determine which attributes and outputs are needed
    bool needsTexcoord = m_RequiredVaryings.count("v_texcoord0") > 0;
    bool needsWorldPos = m_RequiredVaryings.count("v_worldPos") > 0;
    bool needsNormal = m_RequiredVaryings.count("v_normal") > 0;
    bool needsViewDir = m_RequiredVaryings.count("v_viewDir") > 0;
    
    // Input/output declarations
    ss << "$input a_position";
    if (needsNormal || needsViewDir) ss << ", a_normal";
    if (needsTexcoord) ss << ", a_texcoord0";
    ss << "\n";
    
    ss << "$output";
    bool first = true;
    if (needsWorldPos) { ss << (first ? " " : ", ") << "v_worldPos"; first = false; }
    if (needsNormal) { ss << (first ? " " : ", ") << "v_normal"; first = false; }
    if (needsTexcoord) { ss << (first ? " " : ", ") << "v_texcoord0"; first = false; }
    if (needsViewDir) { ss << (first ? " " : ", ") << "v_viewDir"; first = false; }
    ss << "\n\n";
    
    // Includes
    ss << "#include <bgfx_shader.sh>\n";
    
    // Uniforms
    if (needsViewDir) {
        ss << "\nuniform vec4 u_cameraPos;\n";
    }
    ss << "uniform vec4 u_UVTransform;\n";
    
    // Main function
    ss << "\nvoid main()\n";
    ss << "{\n";
    ss << "    vec4 worldPos = mul(u_model[0], vec4(a_position, 1.0));\n";
    
    if (needsWorldPos) {
        ss << "    v_worldPos = worldPos.xyz;\n";
    }
    
    if (needsNormal) {
        ss << "    v_normal = normalize(mul((mat3)u_model[0], a_normal));\n";
    }
    
    if (needsTexcoord) {
        ss << "    vec2 scaledUV = a_texcoord0.xy * u_UVTransform.xy + u_UVTransform.zw;\n";
        ss << "    v_texcoord0.xy = scaledUV;\n";
        ss << "    v_texcoord0.zw = a_texcoord0.zw;\n";
    }
    
    if (needsViewDir) {
        ss << "    v_viewDir = u_cameraPos.xyz - worldPos.xyz;\n";
    }
    
    ss << "    gl_Position = mul(u_viewProj, worldPos);\n";
    ss << "}\n";
    
    return ss.str();
}

std::string ShaderGraphCodeGen::GenerateFragmentShader() {
    std::stringstream ss;
    
    // Input declarations
    ss << "$input";
    bool first = true;
    for (const auto& varying : m_RequiredVaryings) {
        ss << (first ? " " : ", ") << varying;
        first = false;
    }
    if (first) ss << " v_texcoord0";  // Need at least one varying
    ss << "\n\n";
    
    // Includes
    ss << "#include <bgfx_shader.sh>\n";
    for (const auto& inc : m_RequiredIncludes) {
        ss << "#include \"" << inc << "\"\n";
    }
    ss << "\n";
    
    // Sampler declarations
    ss << GenerateSamplerDeclarations();
    
    // Uniform declarations
    ss << GenerateUniformDeclarations();
    ss << "\n";
    
    // Main function
    ss << "void main()\n";
    ss << "{\n";
    
    // Node-generated code
    ss << m_FragmentCode.str();
    
    ss << "}\n";
    
    return ss.str();
}

std::string ShaderGraphCodeGen::GenerateUniformDeclarations() {
    std::stringstream ss;
    
    for (const auto& [name, type] : m_RequiredUniforms) {
        // Skip samplers (handled separately)
        if (type.find("sampler") != std::string::npos) continue;
        
        // Skip reserved uniforms
        if (s_ReservedUniforms.find(name) != s_ReservedUniforms.end()) continue;
        
        // Handle array types: "vec4[4]" -> "uniform vec4 name[4];"
        size_t bracketPos = type.find('[');
        if (bracketPos != std::string::npos) {
            std::string baseType = type.substr(0, bracketPos);
            std::string arrayPart = type.substr(bracketPos);
            ss << "uniform " << baseType << " " << name << arrayPart << ";\n";
        } else {
            // bgfx requires all non-array uniforms to be vec4
            // Use swizzles when accessing (handled in GetInputExpression)
            ss << "uniform vec4 " << name << ";\n";
        }
    }
    
    return ss.str();
}

std::string ShaderGraphCodeGen::GenerateSamplerDeclarations() {
    std::stringstream ss;
    
    for (const auto& [pinId, info] : m_Samplers) {
        ss << "SAMPLER2D(" << info.name << ", " << info.slot << ");\n";
    }
    
    return ss.str();
}

// --- Write and compile shaders ---

ShaderCompileOutput WriteAndCompileShaders(const ShaderCompileResult& result, 
                                           const std::string& projectRoot,
                                           const std::string& baseName) {
    namespace fs = std::filesystem;
    
    ShaderCompileOutput output;
    output.vsName = "vs_" + baseName;
    output.fsName = "fs_" + baseName;
    
    // Determine paths based on project root
    fs::path projPath = projectRoot.empty() ? fs::current_path() : fs::path(projectRoot);
    
    // Shader sources go in <project>/shaders/ for compatibility with existing shaders
    fs::path shadersDir = projPath / "shaders";
    fs::create_directories(shadersDir);
    
    // Compiled binaries go in <project>/.bin/shaders/
    fs::path binDir = projPath / ".bin" / "shaders";
    fs::create_directories(binDir);
    
    fs::path vsSrcPath = shadersDir / (output.vsName + ".sc");
    fs::path fsSrcPath = shadersDir / (output.fsName + ".sc");
    fs::path vsBinPath = binDir / (output.vsName + ".bin");
    fs::path fsBinPath = binDir / (output.fsName + ".bin");
    
    // Store relative paths
    output.vsBinPath = ".bin/shaders/" + output.vsName + ".bin";
    output.fsBinPath = ".bin/shaders/" + output.fsName + ".bin";
    
    // Write vertex shader source
    {
        std::ofstream out(vsSrcPath);
        if (!out) {
            output.error = "Failed to write vertex shader: " + vsSrcPath.string();
            return output;
        }
        out << result.vertexShaderSource;
    }
    
    // Write fragment shader source
    {
        std::ofstream out(fsSrcPath);
        if (!out) {
            output.error = "Failed to write fragment shader: " + fsSrcPath.string();
            return output;
        }
        out << result.fragmentShaderSource;
    }
    
    // Find shaderc and varying.def.sc
    fs::path toolsDir = fs::current_path() / "tools";
    fs::path shaderc = toolsDir / "shaderc.exe";
    fs::path varying = shadersDir / "varying.def.sc";
    
    // Fallback: try engine's shaders dir for varying.def.sc
    if (!fs::exists(varying)) {
        varying = fs::current_path() / "shaders" / "varying.def.sc";
    }
    
    // Find bgfx include path
    fs::path bgfxInc = fs::current_path();
    for (int i = 0; i < 12 && !fs::exists(bgfxInc / "external/bgfx/src/bgfx_shader.sh"); ++i) {
        bgfxInc = bgfxInc.parent_path();
    }
    bgfxInc /= "external/bgfx/src";
    
    auto compileShader = [&](const fs::path& srcPath, const fs::path& outPath, 
                             const std::string& type) -> bool {
        std::string cmd = "\"" + shaderc.string() + "\"";
        cmd += " -f " + srcPath.string();
        cmd += " -o " + outPath.string();
        cmd += " --type " + type;
        cmd += " --platform windows";
        cmd += " --profile s_5_0";
        cmd += " --varyingdef " + varying.string();
        cmd += " -i " + shadersDir.string();
        cmd += " -i " + (fs::current_path() / "shaders").string();  // Engine shaders
        cmd += " -i " + bgfxInc.string();
        
        int ret = system(cmd.c_str());
        if (ret != 0) {
            output.error = "Shader compilation failed for " + srcPath.filename().string();
            return false;
        }
        return true;
    };
    
    if (!compileShader(vsSrcPath, vsBinPath, "vertex")) return output;
    if (!compileShader(fsSrcPath, fsBinPath, "fragment")) return output;
    
    output.success = true;
    return output;
}

} // namespace shadergraph


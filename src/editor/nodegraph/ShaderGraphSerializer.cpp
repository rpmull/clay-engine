#include "ShaderGraphSerializer.h"
#include <fstream>

namespace shadergraph {

const int ShaderGraphSerializer::kCurrentVersion = 1;

static const char* SurfaceTypeToString(SurfaceType type) {
    switch (type) {
        case SurfaceType::Unlit: return "Unlit";
        case SurfaceType::Lit: return "Lit";
        case SurfaceType::Custom: return "Custom";
        default: return "Lit";
    }
}

static SurfaceType StringToSurfaceType(const std::string& s) {
    if (s == "Unlit") return SurfaceType::Unlit;
    if (s == "Custom") return SurfaceType::Custom;
    return SurfaceType::Lit;
}

static const char* ValueTypeToString(ShaderValueType type) {
    switch (type) {
        case ShaderValueType::Float: return "Float";
        case ShaderValueType::Float2: return "Float2";
        case ShaderValueType::Float3: return "Float3";
        case ShaderValueType::Float4: return "Float4";
        case ShaderValueType::Int: return "Int";
        case ShaderValueType::Bool: return "Bool";
        case ShaderValueType::Color3: return "Color3";
        case ShaderValueType::Color4: return "Color4";
        case ShaderValueType::Texture2D: return "Texture2D";
        case ShaderValueType::Sampler2D: return "Sampler2D";
        case ShaderValueType::Matrix3: return "Matrix3";
        case ShaderValueType::Matrix4: return "Matrix4";
        case ShaderValueType::Any: return "Any";
        default: return "Float";
    }
}

static ShaderValueType StringToValueType(const std::string& s) {
    if (s == "Float") return ShaderValueType::Float;
    if (s == "Float2") return ShaderValueType::Float2;
    if (s == "Float3") return ShaderValueType::Float3;
    if (s == "Float4") return ShaderValueType::Float4;
    if (s == "Int") return ShaderValueType::Int;
    if (s == "Bool") return ShaderValueType::Bool;
    if (s == "Color3") return ShaderValueType::Color3;
    if (s == "Color4") return ShaderValueType::Color4;
    if (s == "Texture2D") return ShaderValueType::Texture2D;
    if (s == "Sampler2D") return ShaderValueType::Sampler2D;
    if (s == "Matrix3") return ShaderValueType::Matrix3;
    if (s == "Matrix4") return ShaderValueType::Matrix4;
    if (s == "Any") return ShaderValueType::Any;
    return ShaderValueType::Float;
}

void ShaderGraphSerializer::ToJson(const ShaderGraph& graph, nlohmann::json& j) {
    j["version"] = kCurrentVersion;
    j["name"] = graph.name;
    j["guid"] = graph.guid;
    j["surfaceType"] = SurfaceTypeToString(graph.surfaceType);
    j["applyFog"] = graph.applyFog;
    j["applyAmbient"] = graph.applyAmbient;
    
    // Nodes
    nlohmann::json jnodes = nlohmann::json::array();
    for (const auto& node : graph.nodes) {
        nlohmann::json jn;
        jn["id"] = node.id;
        jn["typeCategory"] = node.typeId.category;
        jn["typeName"] = node.typeId.name;
        jn["displayName"] = node.displayName;
        jn["editorPos"] = {node.editorPos.x, node.editorPos.y};
        jn["inputPins"] = node.inputPins;
        jn["outputPins"] = node.outputPins;
        
        if (!node.properties.empty()) {
            nlohmann::json jprops = nlohmann::json::object();
            for (const auto& [k, v] : node.properties) {
                jprops[k] = v;
            }
            jn["properties"] = jprops;
        }
        
        jnodes.push_back(jn);
    }
    j["nodes"] = jnodes;
    
    // Pins
    nlohmann::json jpins = nlohmann::json::array();
    for (const auto& pin : graph.pins) {
        nlohmann::json jp;
        jp["id"] = pin.id;
        jp["nodeId"] = pin.nodeId;
        jp["kind"] = (pin.kind == PinKind::Input ? "Input" : "Output");
        jp["type"] = ValueTypeToString(pin.type);
        jp["name"] = pin.name;
        jp["defaultValue"] = {pin.defaultValue.x, pin.defaultValue.y, pin.defaultValue.z, pin.defaultValue.w};
        jp["exposed"] = pin.exposed;
        if (!pin.exposedName.empty()) {
            jp["exposedName"] = pin.exposedName;
        }
        if (!pin.defaultTexturePath.empty()) {
            jp["defaultTexturePath"] = pin.defaultTexturePath;
        }
        jpins.push_back(jp);
    }
    j["pins"] = jpins;
    
    // Links
    nlohmann::json jlinks = nlohmann::json::array();
    for (const auto& link : graph.links) {
        nlohmann::json jl;
        jl["id"] = link.id;
        jl["fromPin"] = link.fromPin;
        jl["toPin"] = link.toPin;
        jlinks.push_back(jl);
    }
    j["links"] = jlinks;
    
    // ID counters
    j["nextIds"] = {
        {"node", graph.nextNodeId},
        {"pin", graph.nextPinId},
        {"link", graph.nextLinkId}
    };
    
    // Editor metadata
    j["editor"] = {
        {"pan", {graph.editorPan.x, graph.editorPan.y}},
        {"zoom", graph.editorZoom}
    };
    
    // Compiled shader info
    if (graph.isCompiled) {
        j["compiled"] = {
            {"vsName", graph.compiledVSName},
            {"fsName", graph.compiledFSName},
            {"vsPath", graph.compiledVSPath},
            {"fsPath", graph.compiledFSPath}
        };
    }
}

void ShaderGraphSerializer::FromJson(const nlohmann::json& j, ShaderGraph& graph) {
    graph.Clear();
    
    if (j.contains("name")) graph.name = j["name"].get<std::string>();
    if (j.contains("guid")) graph.guid = j["guid"].get<std::string>();
    if (j.contains("surfaceType")) graph.surfaceType = StringToSurfaceType(j["surfaceType"].get<std::string>());
    graph.applyFog = j.value("applyFog", true);       // Default true for backwards compatibility
    graph.applyAmbient = j.value("applyAmbient", true);
    
    // Load nodes
    if (j.contains("nodes")) {
        for (const auto& jn : j["nodes"]) {
            ShaderNode node;
            node.id = jn.value("id", 0);
            node.typeId.category = jn.value("typeCategory", "");
            node.typeId.name = jn.value("typeName", "");
            node.displayName = jn.value("displayName", "");
            
            if (jn.contains("editorPos") && jn["editorPos"].is_array() && jn["editorPos"].size() >= 2) {
                node.editorPos.x = jn["editorPos"][0].get<float>();
                node.editorPos.y = jn["editorPos"][1].get<float>();
            }
            
            if (jn.contains("inputPins")) {
                jn["inputPins"].get_to(node.inputPins);
            }
            if (jn.contains("outputPins")) {
                jn["outputPins"].get_to(node.outputPins);
            }
            
            if (jn.contains("properties") && jn["properties"].is_object()) {
                for (auto it = jn["properties"].begin(); it != jn["properties"].end(); ++it) {
                    node.properties[it.key()] = it.value().get<std::string>();
                }
            }
            
            graph.nodes.push_back(node);
        }
    }
    
    // Load pins
    if (j.contains("pins")) {
        for (const auto& jp : j["pins"]) {
            ShaderPin pin;
            pin.id = jp.value("id", 0);
            pin.nodeId = jp.value("nodeId", 0);
            pin.kind = (jp.value("kind", "Input") == "Input") ? PinKind::Input : PinKind::Output;
            pin.type = StringToValueType(jp.value("type", "Float"));
            pin.name = jp.value("name", "");
            pin.exposed = jp.value("exposed", false);
            
            if (jp.contains("defaultValue") && jp["defaultValue"].is_array() && jp["defaultValue"].size() >= 4) {
                pin.defaultValue.x = jp["defaultValue"][0].get<float>();
                pin.defaultValue.y = jp["defaultValue"][1].get<float>();
                pin.defaultValue.z = jp["defaultValue"][2].get<float>();
                pin.defaultValue.w = jp["defaultValue"][3].get<float>();
            }
            
            if (jp.contains("exposedName")) pin.exposedName = jp["exposedName"].get<std::string>();
            if (jp.contains("defaultTexturePath")) pin.defaultTexturePath = jp["defaultTexturePath"].get<std::string>();
            
            graph.pins.push_back(pin);
        }
    }
    
    // Load links
    if (j.contains("links")) {
        for (const auto& jl : j["links"]) {
            ShaderLink link;
            link.id = jl.value("id", 0);
            link.fromPin = jl.value("fromPin", 0);
            link.toPin = jl.value("toPin", 0);
            graph.links.push_back(link);
        }
    }
    
    // Load ID counters
    if (j.contains("nextIds")) {
        const auto& ni = j["nextIds"];
        graph.nextNodeId = ni.value("node", 1);
        graph.nextPinId = ni.value("pin", 1);
        graph.nextLinkId = ni.value("link", 1);
    }
    
    // Load editor metadata
    if (j.contains("editor")) {
        const auto& je = j["editor"];
        if (je.contains("pan") && je["pan"].is_array() && je["pan"].size() >= 2) {
            graph.editorPan.x = je["pan"][0].get<float>();
            graph.editorPan.y = je["pan"][1].get<float>();
        }
        graph.editorZoom = je.value("zoom", 1.0f);
    }
    
    // Load compiled shader info
    if (j.contains("compiled")) {
        const auto& jc = j["compiled"];
        graph.compiledVSName = jc.value("vsName", "");
        graph.compiledFSName = jc.value("fsName", "");
        graph.compiledVSPath = jc.value("vsPath", "");
        graph.compiledFSPath = jc.value("fsPath", "");
        graph.isCompiled = !graph.compiledVSPath.empty() && !graph.compiledFSPath.empty();
    }
}

bool ShaderGraphSerializer::SaveToFile(const ShaderGraph& graph, const std::string& path) {
    nlohmann::json j;
    ToJson(graph, j);
    
    std::ofstream out(path);
    if (!out) return false;
    
    out << j.dump(2);
    return true;
}

bool ShaderGraphSerializer::LoadFromFile(const std::string& path, ShaderGraph& outGraph) {
    std::ifstream in(path);
    if (!in) return false;
    
    try {
        nlohmann::json j;
        in >> j;
        FromJson(j, outGraph);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace shadergraph


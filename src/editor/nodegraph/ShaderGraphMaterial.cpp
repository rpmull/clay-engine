#include "ShaderGraphMaterial.h"
#include "ShaderGraphSerializer.h"
#include "ShaderGraphCodeGen.h"
#include "core/rendering/MaterialCache.h"
#include "core/rendering/ShaderManager.h"
#include "core/rendering/TextureSamplerFlags.h"
#include "core/rendering/TextureLoader.h"
#include "core/ecs/Scene.h"
#include "core/vfs/FileSystem.h"
#include "ui/Logger.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <cmath>
#include <cctype>
#include <bx/timer.h>

namespace shadergraph {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
bgfx::TextureHandle GetFallbackTextureForParameter(const std::string& parameterName) {
    TextureSpecifier spec;
    std::string lowerName = parameterName;
    for (char& ch : lowerName) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    if (lowerName.find("normal") != std::string::npos) {
        spec.Path = "assets/debug/normal.png";
    } else if (lowerName.find("metal") != std::string::npos ||
               lowerName.find("rough") != std::string::npos ||
               lowerName.find("orm") != std::string::npos) {
        spec.Path = "assets/debug/metallic_roughness.png";
    } else {
        spec.Path = "assets/debug/white.png";
    }

    return AcquireTextureHandle(spec, TextureColorSpace::Linear);
}
}

// Convert an absolute or arbitrary path to a VFS-relative path for portable storage
static std::string ToVirtualPath(const std::string& path) {
    if (path.empty()) return {};
    std::string normalized = path;
    for (char& c : normalized) if (c == '\\') c = '/';
    
    // If already a VFS path starting with assets/, keep it
    if (normalized.find("assets/") == 0) {
        return normalized;
    }
    
    // Look for "/assets/" in the path and extract from there
    size_t assetsPos = normalized.find("/assets/");
    if (assetsPos != std::string::npos) {
        return normalized.substr(assetsPos + 1); // Skip the leading '/'
    }
    
    // Try to make relative to project root
    try {
        fs::path p(normalized);
        if (p.is_absolute()) {
            const fs::path& proj = FileSystem::Instance().GetProjectRoot();
            if (!proj.empty()) {
                std::error_code ec;
                auto rel = fs::relative(p, proj, ec);
                if (!ec) {
                    std::string relStr = rel.string();
                    for (char& c : relStr) if (c == '\\') c = '/';
                    // Only use if it doesn't go up directories
                    if (relStr.find("../") == std::string::npos) {
                        return relStr;
                    }
                }
            }
        }
    } catch (...) {}
    
    // Fallback: just the filename
    auto pos = normalized.find_last_of('/');
    if (pos != std::string::npos) {
        return normalized.substr(pos + 1);
    }
    return normalized;
}

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

// --- Serialization ---

static const char* ValueTypeToString(ShaderValueType type) {
    switch (type) {
        case ShaderValueType::Float: return "Float";
        case ShaderValueType::Float2: return "Float2";
        case ShaderValueType::Float3: return "Float3";
        case ShaderValueType::Float4: return "Float4";
        case ShaderValueType::Color3: return "Color3";
        case ShaderValueType::Color4: return "Color4";
        case ShaderValueType::Int: return "Int";
        case ShaderValueType::Bool: return "Bool";
        case ShaderValueType::Texture2D: return "Texture2D";
        default: return "Float4";
    }
}

static ShaderValueType StringToValueType(const std::string& s) {
    if (s == "Float") return ShaderValueType::Float;
    if (s == "Float2") return ShaderValueType::Float2;
    if (s == "Float3") return ShaderValueType::Float3;
    if (s == "Float4") return ShaderValueType::Float4;
    if (s == "Color3") return ShaderValueType::Color3;
    if (s == "Color4") return ShaderValueType::Color4;
    if (s == "Int") return ShaderValueType::Int;
    if (s == "Bool") return ShaderValueType::Bool;
    if (s == "Texture2D") return ShaderValueType::Texture2D;
    return ShaderValueType::Float4;
}

bool LoadShaderGraphMaterial(const std::string& path, ShaderGraphMaterialDesc& out) {
    std::ifstream in(path);
    if (!in) return false;
    
    fs::path sgmatPath = fs::absolute(path);
    fs::path sgmatDir = sgmatPath.parent_path();
    
    try {
        json j;
        in >> j;
        
        out.name = j.value("name", "");
        std::string rawShaderGraphPath = j.value("shaderGraph", "");
        out.vertexShaderName = j.value("vsName", "");
        out.fragmentShaderName = j.value("fsName", "");
        
        // Resolve shaderGraphPath using VFS - handles editor, play mode, and runtime
        if (!rawShaderGraphPath.empty()) {
            // Normalize the path (convert backslashes to forward slashes)
            std::string normalized = IVirtualFS::NormalizePath(rawShaderGraphPath);
            
            // Helper to resolve a VFS path to disk path
            auto resolveVfsPath = [&](const std::string& vfsPath) -> fs::path {
                fs::path p(vfsPath);
                if (p.is_absolute() && fs::exists(p)) {
                    return p;
                }
                // Try relative to project root
                const fs::path& projectRoot = FileSystem::Instance().GetProjectRoot();
                if (!projectRoot.empty()) {
                    fs::path resolved = projectRoot / vfsPath;
                    if (fs::exists(resolved)) {
                        return resolved;
                    }
                }
                return {};
            };
            
            // First try: use VFS to resolve the path (works for assets/... paths)
            fs::path resolved = resolveVfsPath(normalized);
            if (!resolved.empty()) {
                out.shaderGraphPath = resolved.string();
            }
            // Second try: resolve relative to .sgmat file directory (common case: same folder)
            else {
                fs::path graphPath(normalized);
                fs::path justFilename = graphPath.filename();
                
                if (fs::exists(sgmatDir / justFilename)) {
                    out.shaderGraphPath = (sgmatDir / justFilename).string();
                }
                // Third try: relative path from .sgmat location
                else if (graphPath.is_relative() && fs::exists(sgmatDir / graphPath)) {
                    out.shaderGraphPath = (sgmatDir / graphPath).string();
                }
                // Fourth try: VFS path with assets/ prefix
                else if (normalized.find("assets/") != 0 && normalized.find('/') == std::string::npos) {
                    // Might be just a filename - try prepending the .sgmat's VFS directory
                    std::string sgmatVfsPath = ToVirtualPath(path);
                    auto lastSlash = sgmatVfsPath.find_last_of('/');
                    if (lastSlash != std::string::npos) {
                        std::string vfsDir = sgmatVfsPath.substr(0, lastSlash + 1);
                        fs::path altResolved = resolveVfsPath(vfsDir + normalized);
                        if (!altResolved.empty()) {
                            out.shaderGraphPath = altResolved.string();
                        } else {
                            out.shaderGraphPath = normalized; // Last resort
                        }
                    } else {
                        out.shaderGraphPath = normalized;
                    }
                }
                else {
                    out.shaderGraphPath = normalized; // Last resort
                }
            }
        }
        
        if (j.contains("uvScale") && j["uvScale"].is_array() && j["uvScale"].size() >= 2) {
            out.uvScale = glm::vec2(j["uvScale"][0], j["uvScale"][1]);
        }
        if (j.contains("uvOffset") && j["uvOffset"].is_array() && j["uvOffset"].size() >= 2) {
            out.uvOffset = glm::vec2(j["uvOffset"][0], j["uvOffset"][1]);
        }
        
        out.twoSided = j.value("twoSided", false);
        out.alphaClip = j.value("alphaClip", false);
        out.alphaClipThreshold = j.value("alphaClipThreshold", 0.5f);
        
        out.parameters.clear();
        if (j.contains("parameters") && j["parameters"].is_array()) {
            for (const auto& jp : j["parameters"]) {
                MaterialParameter param;
                param.name = SanitizeIdentifier(jp.value("name", ""));
                param.displayName = jp.value("displayName", param.name);
                param.type = StringToValueType(jp.value("type", "Float4"));
                
                if (jp.contains("value") && jp["value"].is_array() && jp["value"].size() >= 4) {
                    param.value = glm::vec4(jp["value"][0], jp["value"][1], jp["value"][2], jp["value"][3]);
                }
                
                param.texturePath = jp.value("texturePath", "");
                param.textureSlot = jp.value("textureSlot", -1);
                
                out.parameters.push_back(param);
            }
        }
        
        return true;
    } catch (...) {
        return false;
    }
}

bool SaveShaderGraphMaterial(const std::string& path, const ShaderGraphMaterialDesc& in) {
    json j;
    j["name"] = in.name;
    
    // Store shaderGraphPath as a VFS-relative path for portability across machines
    j["shaderGraph"] = ToVirtualPath(in.shaderGraphPath);
    
    j["vsName"] = in.vertexShaderName;
    j["fsName"] = in.fragmentShaderName;
    j["uvScale"] = { in.uvScale.x, in.uvScale.y };
    j["uvOffset"] = { in.uvOffset.x, in.uvOffset.y };
    j["twoSided"] = in.twoSided;
    j["alphaClip"] = in.alphaClip;
    j["alphaClipThreshold"] = in.alphaClipThreshold;
    
    json jparams = json::array();
    for (const auto& param : in.parameters) {
        json jp;
        jp["name"] = param.name;
        jp["displayName"] = param.displayName;
        jp["type"] = ValueTypeToString(param.type);
        jp["value"] = { param.value.x, param.value.y, param.value.z, param.value.w };
        jp["texturePath"] = param.texturePath;
        jp["textureSlot"] = param.textureSlot;
        jparams.push_back(jp);
    }
    j["parameters"] = jparams;
    
    std::ofstream out(path);
    if (!out) return false;
    out << j.dump(4);
    return true;
}

// --- ShaderGraphMaterial ---

ShaderGraphMaterial::ShaderGraphMaterial(const std::string& name, bgfx::ProgramHandle program)
    : Material(name, program,
        BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
        BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA | BGFX_STATE_CULL_CW)
{
    m_UVTransformUniform = bgfx::createUniform("u_UVTransform", bgfx::UniformType::Vec4);
    m_TimeUniform = bgfx::createUniform("u_Time", bgfx::UniformType::Vec4);
}

ShaderGraphMaterial::~ShaderGraphMaterial() {
    cm::rendering::SafeDestroyHandle(m_UVTransformUniform);
    cm::rendering::SafeDestroyHandle(m_TimeUniform);
    
    for (auto& param : m_Parameters) {
        cm::rendering::SafeDestroyHandle(param.uniformHandle);
    }
}

std::shared_ptr<Material> ShaderGraphMaterial::Clone() const {
    // Create a new ShaderGraphMaterial with the same program but fresh handles
    auto clone = std::make_shared<ShaderGraphMaterial>(GetName() + "_Clone", GetProgram());
    
    // Copy shader graph state
    clone->m_ShaderGraphPath = m_ShaderGraphPath;
    clone->m_UVTransform = m_UVTransform;
    clone->m_StateFlags = m_StateFlags;
    
    // Deep copy parameters (handles will be recreated fresh)
    clone->m_Parameters.clear();
    for (const auto& param : m_Parameters) {
        MaterialParameter newParam;
        newParam.name = param.name;
        newParam.displayName = param.displayName;
        newParam.type = param.type;
        newParam.value = param.value;
        newParam.texturePath = param.texturePath;
        newParam.textureSlot = param.textureSlot;
        // textureHandle is shared (textures are cached)
        newParam.textureHandle = param.textureHandle;
        // uniformHandle will be created fresh
        newParam.uniformHandle = BGFX_INVALID_HANDLE;
        clone->m_Parameters.push_back(newParam);
    }
    
    // Create fresh uniform handles
    clone->EnsureUniformHandles();
    
    return clone;
}

ShaderGraphMaterialDesc ShaderGraphMaterial::GetDesc() const {
    ShaderGraphMaterialDesc desc;
    desc.name = GetName();
    desc.shaderGraphPath = m_ShaderGraphPath;
    desc.uvScale = glm::vec2(m_UVTransform.x, m_UVTransform.y);
    desc.uvOffset = glm::vec2(m_UVTransform.z, m_UVTransform.w);
    desc.stateFlags = m_StateFlags;
    desc.twoSided = (m_StateFlags & BGFX_STATE_CULL_MASK) == 0;
    
    // Copy parameters
    for (const auto& param : m_Parameters) {
        MaterialParameter p;
        p.name = param.name;
        p.displayName = param.displayName;
        p.type = param.type;
        p.value = param.value;
        p.texturePath = param.texturePath;
        p.textureSlot = param.textureSlot;
        desc.parameters.push_back(p);
    }
    
    return desc;
}

std::shared_ptr<ShaderGraphMaterial> ShaderGraphMaterial::CreateFromDesc(const ShaderGraphMaterialDesc& desc) {
    // Load the shader program
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    
    Logger::Log("[ShaderGraphMaterial] Creating from desc, shaderGraphPath: " + desc.shaderGraphPath);
    
    // First, try to load from compiled shader paths in the graph
    if (!desc.shaderGraphPath.empty()) {
        ShaderGraph graph;
        if (ShaderGraphSerializer::LoadFromFile(desc.shaderGraphPath, graph)) {
            Logger::Log("[ShaderGraphMaterial] Loaded graph: " + graph.name + 
                      ", isCompiled: " + std::to_string(graph.isCompiled) + 
                      ", vsPath: " + graph.compiledVSPath + 
                      ", fsPath: " + graph.compiledFSPath);
            
            // If the graph has compiled shader info, try to load from those paths
            if (graph.isCompiled && !graph.compiledVSPath.empty() && !graph.compiledFSPath.empty()) {
                // Resolve paths relative to the shader graph location
                std::filesystem::path graphDir = std::filesystem::path(desc.shaderGraphPath).parent_path();
                // Walk up to find project root (directory containing assets folder or .library)
                std::filesystem::path projectRoot = graphDir;
                for (auto p = graphDir; !p.empty() && p.has_parent_path(); p = p.parent_path()) {
                    if (std::filesystem::exists(p / "assets") || 
                        std::filesystem::exists(p / ".library") ||
                        p.filename() == "assets") {
                        projectRoot = (p.filename() == "assets") ? p.parent_path() : p;
                        break;
                    }
                }
                
                std::filesystem::path vsBinPath = projectRoot / graph.compiledVSPath;
                std::filesystem::path fsBinPath = projectRoot / graph.compiledFSPath;
                
                Logger::Log("[ShaderGraphMaterial] Looking for binaries at:");
                Logger::Log("  VS: " + vsBinPath.string() + " (exists: " + std::to_string(std::filesystem::exists(vsBinPath)) + ")");
                Logger::Log("  FS: " + fsBinPath.string() + " (exists: " + std::to_string(std::filesystem::exists(fsBinPath)) + ")");
                
                if (std::filesystem::exists(vsBinPath) && std::filesystem::exists(fsBinPath)) {
                    program = ShaderManager::Instance().LoadProgramFromPaths(
                        vsBinPath.string(), fsBinPath.string());
                    Logger::Log("[ShaderGraphMaterial] Loaded program from paths, valid: " + std::to_string(bgfx::isValid(program)));
                }
            } else {
                Logger::LogWarning("[ShaderGraphMaterial] Graph not compiled or missing paths");
            }
            
            // If still no program, try to compile the shader graph on-demand
            if (!bgfx::isValid(program)) {
                Logger::Log("[ShaderGraphMaterial] Attempting on-demand compilation...");
                ShaderGraphCodeGen codegen(graph);
                ShaderCompileResult result = codegen.Compile();
                Logger::Log("[ShaderGraphMaterial] Compilation result: " + std::string(result.success ? "SUCCESS" : "FAILED"));
                for (const auto& err : result.errors) {
                    Logger::LogError("[ShaderGraphMaterial]   Error: " + err);
                }
                
                if (result.success) {
                    std::string baseName = std::filesystem::path(desc.shaderGraphPath).stem().string();
                    std::replace(baseName.begin(), baseName.end(), ' ', '_');
                    baseName = "shgraph_" + baseName;
                    
                    // Find project root
                    std::filesystem::path graphDir = std::filesystem::path(desc.shaderGraphPath).parent_path();
                    std::string projectRoot;
                    for (auto p = graphDir; !p.empty() && p.has_parent_path(); p = p.parent_path()) {
                        if (std::filesystem::exists(p / "assets") || 
                            std::filesystem::exists(p / ".library") ||
                            p.filename() == "assets") {
                            projectRoot = (p.filename() == "assets") ? p.parent_path().string() : p.string();
                            break;
                        }
                    }
                    
                    ShaderCompileOutput compileOutput = WriteAndCompileShaders(result, projectRoot, baseName);
                    if (compileOutput.success) {
                        std::filesystem::path vsBinPath = std::filesystem::path(projectRoot) / compileOutput.vsBinPath;
                        std::filesystem::path fsBinPath = std::filesystem::path(projectRoot) / compileOutput.fsBinPath;
                        program = ShaderManager::Instance().LoadProgramFromPaths(
                            vsBinPath.string(), fsBinPath.string());
                    }
                }
            }
        }
    }
    
    // Fallback to explicit shader names if provided
    if (!bgfx::isValid(program) && !desc.vertexShaderName.empty() && !desc.fragmentShaderName.empty()) {
        program = ShaderManager::Instance().LoadProgram(desc.vertexShaderName, desc.fragmentShaderName);
    }
    
    if (!bgfx::isValid(program)) {
        // Fallback to a default shader
        Logger::LogWarning("[ShaderGraphMaterial] Falling back to default PBR shader!");
        program = ShaderManager::Instance().LoadProgram("vs_pbr", "fs_pbr");
    } else {
        Logger::Log("[ShaderGraphMaterial] Successfully loaded shader graph program");
    }
    
    auto mat = std::make_shared<ShaderGraphMaterial>(desc.name, program);
    mat->m_ShaderGraphPath = desc.shaderGraphPath;
    mat->m_UVTransform = glm::vec4(desc.uvScale, desc.uvOffset);
    
    // If parameters are provided in desc, use them; otherwise sync from shader graph
    if (!desc.parameters.empty()) {
        mat->m_Parameters = desc.parameters;
    } else if (!desc.shaderGraphPath.empty()) {
        // Sync with shader graph to get exposed parameters
        mat->SyncWithShaderGraph(desc.shaderGraphPath);
    }
    
    // Apply render state
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                     BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA;
    if (!desc.twoSided) {
        state |= BGFX_STATE_CULL_CW;
    }
    mat->m_StateFlags = state;
    
    // Ensure uniform handles
    mat->EnsureUniformHandles();
    
    // Load textures
    for (auto& param : mat->m_Parameters) {
        if (param.type == ShaderValueType::Texture2D && !param.texturePath.empty()) {
            TextureSpecifier spec;
            spec.Path = param.texturePath;
            param.textureHandle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
        }
    }
    
    return mat;
}

void ShaderGraphMaterial::SetFloat(const std::string& name, float value) {
    MaterialParameter* param = FindParameter(name);
    if (param) {
        param->value.x = value;
    }
}

void ShaderGraphMaterial::SetFloat2(const std::string& name, const glm::vec2& value) {
    MaterialParameter* param = FindParameter(name);
    if (param) {
        param->value.x = value.x;
        param->value.y = value.y;
    }
}

void ShaderGraphMaterial::SetFloat3(const std::string& name, const glm::vec3& value) {
    MaterialParameter* param = FindParameter(name);
    if (param) {
        param->value.x = value.x;
        param->value.y = value.y;
        param->value.z = value.z;
    }
}

void ShaderGraphMaterial::SetFloat4(const std::string& name, const glm::vec4& value) {
    MaterialParameter* param = FindParameter(name);
    if (param) {
        param->value = value;
    }
}

void ShaderGraphMaterial::SetColor(const std::string& name, const glm::vec4& color) {
    SetFloat4(name, color);
}

void ShaderGraphMaterial::SetTexture(const std::string& name, bgfx::TextureHandle texture) {
    MaterialParameter* param = FindParameter(name);
    if (param) {
        param->textureHandle = texture;
        param->texturePath.clear();
    }
}

void ShaderGraphMaterial::SetTextureFromPath(const std::string& name, const std::string& path) {
    MaterialParameter* param = FindParameter(name);
    if (param) {
        param->texturePath = path;
        if (!path.empty()) {
            TextureSpecifier spec;
            spec.Path = path;
            param->textureHandle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
        } else {
            param->textureHandle = BGFX_INVALID_HANDLE;
        }
    }
}

bool ShaderGraphMaterial::GetFloat(const std::string& name, float& out) const {
    const MaterialParameter* param = FindParameter(name);
    if (!param) return false;
    out = param->value.x;
    return true;
}

bool ShaderGraphMaterial::GetFloat2(const std::string& name, glm::vec2& out) const {
    const MaterialParameter* param = FindParameter(name);
    if (!param) return false;
    out = glm::vec2(param->value.x, param->value.y);
    return true;
}

bool ShaderGraphMaterial::GetFloat3(const std::string& name, glm::vec3& out) const {
    const MaterialParameter* param = FindParameter(name);
    if (!param) return false;
    out = glm::vec3(param->value.x, param->value.y, param->value.z);
    return true;
}

bool ShaderGraphMaterial::GetFloat4(const std::string& name, glm::vec4& out) const {
    const MaterialParameter* param = FindParameter(name);
    if (!param) return false;
    out = param->value;
    return true;
}

void ShaderGraphMaterial::SetUVScale(const glm::vec2& scale) {
    m_UVTransform.x = scale.x;
    m_UVTransform.y = scale.y;
}

void ShaderGraphMaterial::SetUVOffset(const glm::vec2& offset) {
    m_UVTransform.z = offset.x;
    m_UVTransform.w = offset.y;
}

void ShaderGraphMaterial::BindUniforms() const {
    RefreshTextureParametersFromPaths();
    // Bind base uniforms
    Material::BindUniforms();
    
    // Bind UV transform
    if (bgfx::isValid(m_UVTransformUniform)) {
        bgfx::setUniform(m_UVTransformUniform, &m_UVTransform);
    }
    
    // Bind time uniform (x = time, y = sin(time), z = cos(time), w = frac(time))
    if (bgfx::isValid(m_TimeUniform)) {
        // Compute time since application start
        static uint64_t s_startCounter = bx::getHPCounter();
        const uint64_t now = bx::getHPCounter();
        const double freq = double(bx::getHPFrequency());
        float timeSec = static_cast<float>((double(now - s_startCounter) / freq));
        
        glm::vec4 timeVec(timeSec, std::sin(timeSec), std::cos(timeSec), timeSec - std::floor(timeSec));
        bgfx::setUniform(m_TimeUniform, &timeVec);
    }
    
    // Bind all parameters
    int textureUnit = 0;
    const uint32_t samplerFlags =
        cm::rendering::GetTextureSamplerFlags(Scene::Get().GetEnvironment());
    for (const auto& param : m_Parameters) {
        if (param.type == ShaderValueType::Texture2D) {
            if (!bgfx::isValid(param.uniformHandle)) {
                continue;
            }

            const int slot = param.textureSlot >= 0 ? param.textureSlot : textureUnit++;
            bgfx::TextureHandle texture = bgfx::isValid(param.textureHandle)
                ? param.textureHandle
                : GetFallbackTextureForParameter(param.name);
            if (bgfx::isValid(texture)) {
                bgfx::setTexture(slot, param.uniformHandle, texture, samplerFlags);
            }
        } else {
            // Uniform binding
            if (bgfx::isValid(param.uniformHandle)) {
                bgfx::setUniform(param.uniformHandle, &param.value);
            }
        }
    }
}

void ShaderGraphMaterial::RefreshTextureParametersFromPaths() const {
    auto* self = const_cast<ShaderGraphMaterial*>(this);
    for (auto& param : self->m_Parameters) {
        if (param.type != ShaderValueType::Texture2D) {
            continue;
        }
        if (param.texturePath.empty()) {
            param.textureHandle = BGFX_INVALID_HANDLE;
            continue;
        }

        TextureSpecifier spec;
        spec.Path = param.texturePath;
        param.textureHandle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
    }
}

void ShaderGraphMaterial::SyncWithShaderGraph(const std::string& shaderGraphPath) {
    m_ShaderGraphPath = shaderGraphPath;
    
    ShaderGraph graph;
    if (!ShaderGraphSerializer::LoadFromFile(shaderGraphPath, graph)) {
        return;
    }
    
    // Compile the graph to get parameter info
    ShaderGraphCodeGen codegen(graph);
    ShaderCompileResult result = codegen.Compile();
    
    if (!result.success) {
        return;
    }
    
    // Preserve existing parameter values where possible
    std::unordered_map<std::string, MaterialParameter> oldParams;
    for (const auto& param : m_Parameters) {
        oldParams[param.name] = param;
    }
    
    // Update parameters from compile result
    m_Parameters.clear();
    for (const auto& resultParam : result.parameters) {
        MaterialParameter param;
        param.name = resultParam.name;
        param.displayName = resultParam.displayName;
        param.type = resultParam.type;
        param.textureSlot = resultParam.textureSlot;
        
        // Try to preserve old value
        auto it = oldParams.find(resultParam.name);
        if (it != oldParams.end()) {
            param.value = it->second.value;
            param.texturePath = it->second.texturePath;
            param.textureHandle = it->second.textureHandle;
        } else {
            param.value = resultParam.defaultValue;
            param.texturePath = resultParam.texturePath;
        }
        
        m_Parameters.push_back(param);
    }
    
    EnsureUniformHandles();
}

MaterialParameter* ShaderGraphMaterial::FindParameter(const std::string& name) {
    for (auto& param : m_Parameters) {
        if (param.name == name) return &param;
    }
    return nullptr;
}

const MaterialParameter* ShaderGraphMaterial::FindParameter(const std::string& name) const {
    for (const auto& param : m_Parameters) {
        if (param.name == name) return &param;
    }
    return nullptr;
}

void ShaderGraphMaterial::EnsureUniformHandles() {
    for (auto& param : m_Parameters) {
        if (!bgfx::isValid(param.uniformHandle)) {
            bgfx::UniformType::Enum uniformType = bgfx::UniformType::Vec4;
            if (param.type == ShaderValueType::Texture2D) {
                uniformType = bgfx::UniformType::Sampler;
            }
            param.uniformHandle = bgfx::createUniform(param.name.c_str(), uniformType);
        }
    }
}

} // namespace shadergraph


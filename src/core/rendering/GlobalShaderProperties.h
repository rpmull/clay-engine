#pragma once

#include <unordered_map>
#include <string>
#include <glm/glm.hpp>
#include <bgfx/bgfx.h>

// Singleton to hold project-wide/global shader properties.
// These are applied for every draw call before per-instance overrides.
class GlobalShaderProperties {
public:
    static GlobalShaderProperties& Instance() {
        static GlobalShaderProperties inst; return inst;
    }

    // Set and query global vec4 uniforms by name
    void SetVec4(const std::string& name, const glm::vec4& value) { m_Vec4[name] = value; }
    bool TryGetVec4(const std::string& name, glm::vec4& out) const {
        auto it = m_Vec4.find(name); if (it == m_Vec4.end()) return false; out = it->second; return true;
    }

    // Remove a property
    void ClearVec4(const std::string& name) { m_Vec4.erase(name); }
    void ClearAll() { m_Vec4.clear(); }

    // Apply all globals to current draw call
    void Apply() {
        for (auto& kv : m_Vec4) {
            bgfx::UniformHandle h = GetOrCreateVec4Handle(kv.first);
            if (bgfx::isValid(h)) {
                const glm::vec4& v = kv.second;
                bgfx::setUniform(h, &v);
            }
        }
    }

    // Serialize to JSON-like object (name -> [x,y,z,w])
    template<typename Json>
    Json ToJson() const {
        Json j = Json::object();
        if (!m_Vec4.empty()) {
            Json v4 = Json::object();
            for (const auto& kv : m_Vec4) {
                v4[kv.first] = Json{ kv.second.x, kv.second.y, kv.second.z, kv.second.w };
            }
            j["vec4"] = v4;
        }
        return j;
    }

    // Deserialize from JSON-like object
    template<typename Json>
    void FromJson(const Json& j) {
        m_Vec4.clear();
        if (j.contains("vec4") && j["vec4"].is_object()) {
            for (auto it = j["vec4"].begin(); it != j["vec4"].end(); ++it) {
                const auto& arr = it.value();
                if (arr.is_array() && arr.size() == 4) {
                    m_Vec4[it.key()] = glm::vec4(arr[0], arr[1], arr[2], arr[3]);
                }
            }
        }
    }

private:
    GlobalShaderProperties() = default;
    bgfx::UniformHandle GetOrCreateVec4Handle(const std::string& name) {
        auto it = m_Vec4Handles.find(name);
        if (it != m_Vec4Handles.end()) return it->second;
        bgfx::UniformHandle h = bgfx::createUniform(name.c_str(), bgfx::UniformType::Vec4);
        if (bgfx::isValid(h)) m_Vec4Handles.emplace(name, h);
        return h;
    }

    std::unordered_map<std::string, glm::vec4> m_Vec4;
    std::unordered_map<std::string, bgfx::UniformHandle> m_Vec4Handles;
};



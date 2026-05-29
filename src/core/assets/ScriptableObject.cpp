#include "ScriptableObject.h"
#include "ScriptableRegistry.h"
#include <algorithm>

using json = nlohmann::json;

static bool WriteVariantToJson(const Variant& v, const FieldDesc* fd, json& out);
static bool ReadVariantFromJson(const json& in, const FieldDesc* fd, Variant& out);

bool ScriptableObject::GetField(const char* name, Variant& out) const {
    if (!name) return false;
    std::lock_guard<std::mutex> lk(m_FieldMutex);
    auto it = m_Fields.find(name);
    if (it == m_Fields.end()) return false;
    out = it->second;
    return true;
}

bool ScriptableObject::SetField(const char* name, const Variant& in) {
    if (!name) return false;
    {
        std::lock_guard<std::mutex> lk(m_FieldMutex);
        m_Fields[name] = in;
    }
    NotifyChanged();
    return true;
}

void ScriptableObject::AddOnChanged(ChangeCallback cb) { m_OnChanged = std::move(cb); }
void ScriptableObject::RemoveOnChanged() { m_OnChanged = nullptr; }
void ScriptableObject::NotifyChanged() { if (m_OnChanged) m_OnChanged(*this); }

bool ScriptableObject::Serialize(json& j) const {
    // Caller fills type/version/guid; we emit fields only here
    const ScriptableTypeDesc* desc = ScriptableTypeRegistry::Get().FindByName(m_TypeName);
    json f = json::object();

    if (desc) {
        // Serialize only registered fields in descriptor order
        std::lock_guard<std::mutex> lk(m_FieldMutex);
        for (const auto& fd : desc->fields) {
            auto it = m_Fields.find(fd.name);
            Variant v = (it != m_Fields.end()) ? it->second : Variant{};
            json val;
            if (!WriteVariantToJson(v, &fd, val)) continue;
            f[fd.name] = std::move(val);
        }
    } else {
        // Fallback: serialize all fields best-effort
        std::vector<std::pair<std::string, Variant>> sorted;
        {
            std::lock_guard<std::mutex> lk(m_FieldMutex);
            sorted.reserve(m_Fields.size());
            for (const auto& kv : m_Fields) sorted.emplace_back(kv.first, kv.second);
        }
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
        for (const auto& kv : sorted) {
            json val;
            if (!WriteVariantToJson(kv.second, nullptr, val)) continue;
            f[kv.first] = std::move(val);
        }
    }

    j["fields"] = std::move(f);
    return true;
}

bool ScriptableObject::Deserialize(const json& j, uint32_t /*version*/) {
    if (!j.contains("fields") || !j["fields"].is_object()) return true; // empty ok
    json f = j["fields"];
    std::lock_guard<std::mutex> lk(m_FieldMutex);
    m_Fields.clear();

    const ScriptableTypeDesc* desc = ScriptableTypeRegistry::Get().FindByName(m_TypeName);

    if (desc) {
        for (const auto& fd : desc->fields) {
            if (!f.contains(fd.name)) continue;
            const json& val = f[fd.name];
            Variant v;
            if (ReadVariantFromJson(val, &fd, v)) {
                m_Fields[fd.name] = std::move(v);
            }
        }
    } else {
        for (auto it = f.begin(); it != f.end(); ++it) {
            Variant v;
            if (ReadVariantFromJson(it.value(), nullptr, v)) {
                m_Fields[it.key()] = std::move(v);
            }
        }
    }
    return true;
}

static bool WriteVariantToJson(const Variant& v, const FieldDesc* fd, json& out) {
    if (std::holds_alternative<std::monostate>(v.value)) { out = nullptr; return true; }
    if (auto p = std::get_if<bool>(&v.value)) { out = *p; return true; }
    if (auto p = std::get_if<int32_t>(&v.value)) { out = *p; return true; }
    if (auto p = std::get_if<int64_t>(&v.value)) { out = *p; return true; }
    if (auto p = std::get_if<float>(&v.value)) { out = *p; return true; }
    if (auto p = std::get_if<double>(&v.value)) { out = *p; return true; }
    if (auto p = std::get_if<std::string>(&v.value)) { out = *p; return true; }
    if (auto p = std::get_if<glm::vec2>(&v.value)) { out = { (*p).x, (*p).y }; return true; }
    if (auto p = std::get_if<glm::vec3>(&v.value)) { out = { (*p).x, (*p).y, (*p).z }; return true; }
    if (auto p = std::get_if<glm::vec4>(&v.value)) { out = { (*p).x, (*p).y, (*p).z, (*p).w }; return true; }
    if (auto p = std::get_if<glm::quat>(&v.value)) { out = { (*p).w, (*p).x, (*p).y, (*p).z }; return true; }
    if (auto p = std::get_if<ClaymoreGUID>(&v.value)) { out = (*p).ToString(); return true; }
    if (auto p = std::get_if<Variant::Array>(&v.value)) {
        out = json::array();
        const FieldDesc* elementDesc = fd;
        FieldDesc temp;
        if (fd && fd->arrayRank > 0) {
            temp = *fd;
            temp.arrayRank = 0;
            temp.type = (fd->listElementType == ValueType::None) ? fd->type : fd->listElementType;
            elementDesc = &temp;
        }
        for (const auto& e : *p) {
            json je;
            if (!WriteVariantToJson(e, elementDesc, je)) return false;
            out.push_back(std::move(je));
        }
        return true;
    }
    if (auto p = std::get_if<Variant::Struct>(&v.value)) {
        out = json::object();
        // If struct metadata exists, order fields accordingly
        if (fd && !fd->structFields.empty()) {
            for (const auto& sub : fd->structFields) {
                auto it = p->find(sub.name);
                if (it == p->end()) continue;
                json sv;
                if (WriteVariantToJson(it->second, &sub, sv)) {
                    out[sub.name] = std::move(sv);
                }
            }
        } else {
            for (const auto& kv : *p) {
                json sv;
                if (WriteVariantToJson(kv.second, nullptr, sv)) {
                    out[kv.first] = std::move(sv);
                }
            }
        }
        return true;
    }
    return false;
}

static bool ReadVariantFromJson(const json& in, const FieldDesc* fd, Variant& out) {
    if (!fd) {
        // Fallback best-effort
        if (in.is_boolean()) { out.value = in.get<bool>(); return true; }
        if (in.is_number_integer()) { out.value = (int64_t)in.get<int64_t>(); return true; }
        if (in.is_number_float()) { out.value = in.get<double>(); return true; }
        if (in.is_string()) {
            std::string s = in.get<std::string>();
            ClaymoreGUID g = ClaymoreGUID::FromString(s);
            if (g.high != 0 || g.low != 0) out.value = g; else out.value = s;
            return true;
        }
        if (in.is_array()) {
            Variant::Array arr;
            for (const auto& e : in) {
                Variant ev;
                if (ReadVariantFromJson(e, nullptr, ev)) arr.push_back(std::move(ev));
            }
            out.value = std::move(arr);
            return true;
        }
        if (in.is_object()) {
            Variant::Struct st;
            for (auto it = in.begin(); it != in.end(); ++it) {
                Variant ev;
                if (ReadVariantFromJson(it.value(), nullptr, ev)) st[it.key()] = std::move(ev);
            }
            out.value = std::move(st);
            return true;
        }
        return false;
    }

    // Type-directed read
    switch (fd->type) {
        case ValueType::Bool:
            if (in.is_boolean()) { out.value = in.get<bool>(); return true; }
            break;
        case ValueType::Int32:
            if (in.is_number_integer()) { out.value = (int32_t)in.get<int64_t>(); return true; }
            break;
        case ValueType::Int64:
            if (in.is_number_integer()) { out.value = (int64_t)in.get<int64_t>(); return true; }
            break;
        case ValueType::Float:
            if (in.is_number()) { out.value = in.get<float>(); return true; }
            break;
        case ValueType::Double:
            if (in.is_number()) { out.value = in.get<double>(); return true; }
            break;
        case ValueType::String:
            if (in.is_string()) { out.value = in.get<std::string>(); return true; }
            break;
        case ValueType::Enum:
            if (in.is_number_integer()) { out.value = (int32_t)in.get<int64_t>(); return true; }
            break;
        case ValueType::Mesh:
        case ValueType::Prefab:
        case ValueType::ClayObject:
        case ValueType::DialogueLibrary:
        case ValueType::Texture:
        case ValueType::Audio:
        case ValueType::Guid:
            if (in.is_string()) {
                ClaymoreGUID g = ClaymoreGUID::FromString(in.get<std::string>());
                out.value = g;
                return true;
            }
            break;
        case ValueType::AnimationController:
        case ValueType::AnimationControllerOverride:
            if (in.is_string()) {
                out.value = in.get<std::string>();
                return true;
            }
            break;
        case ValueType::Vec2:
            if (in.is_array() && in.size() >= 2) {
                out.value = glm::vec2(in[0].get<float>(), in[1].get<float>()); return true;
            }
            break;
        case ValueType::Vec3:
            if (in.is_array() && in.size() >= 3) {
                out.value = glm::vec3(in[0].get<float>(), in[1].get<float>(), in[2].get<float>()); return true;
            }
            break;
        case ValueType::Vec4:
        case ValueType::Color:
            if (in.is_array() && in.size() >= 4) {
                out.value = glm::vec4(in[0].get<float>(), in[1].get<float>(), in[2].get<float>(), in[3].get<float>()); return true;
            }
            break;
        case ValueType::Quat:
            if (in.is_array() && in.size() >= 4) {
                out.value = glm::quat(in[0].get<float>(), in[1].get<float>(), in[2].get<float>(), in[3].get<float>()); return true;
            }
            break;
        case ValueType::Struct:
            if (in.is_object()) {
                Variant::Struct st;
                for (const auto& sub : fd->structFields) {
                    if (!in.contains(sub.name)) continue;
                    Variant sv;
                    if (ReadVariantFromJson(in[sub.name], &sub, sv)) {
                        st[sub.name] = std::move(sv);
                    }
                }
                out.value = std::move(st);
                return true;
            }
            break;
        default:
            break;
    }

    // List support
    if (fd->arrayRank > 0 && in.is_array()) {
        Variant::Array arr;
        FieldDesc elementDesc = *fd;
        elementDesc.arrayRank = 0;
        elementDesc.type = fd->listElementType == ValueType::None ? fd->type : fd->listElementType;
        for (const auto& e : in) {
            Variant ev;
            if (ReadVariantFromJson(e, &elementDesc, ev)) arr.push_back(std::move(ev));
        }
        out.value = std::move(arr);
        return true;
    }

    return false;
}

#include "ScriptReflection.h"
#include <glm/glm.hpp>
#include <sstream>

// Static member definition
std::unordered_map<std::string, std::vector<PropertyInfo>> ScriptReflection::s_ScriptProperties;

void ScriptReflection::RegisterScriptProperty(const std::string& scriptClass, const PropertyInfo& property) {
    auto& vec = s_ScriptProperties[scriptClass];
    // Update existing entry if present; otherwise append
    auto it = std::find_if(vec.begin(), vec.end(), [&](const PropertyInfo& p){ return p.name == property.name; });
    if (it == vec.end()) {
        vec.push_back(property);
    } else {
        it->type = property.type;
        it->defaultValue = property.defaultValue;
        it->currentValue = property.currentValue;
        // Preserve existing getter/setter hooks if already wired
        if (!property.getter) { /* keep existing */ } else it->getter = property.getter;
        if (!property.setter) { /* keep existing */ } else it->setter = property.setter;
    }
}

std::vector<PropertyInfo>& ScriptReflection::GetScriptProperties(const std::string& scriptClass) {
    return s_ScriptProperties[scriptClass]; // creates if not present
}

bool ScriptReflection::HasProperties(const std::string& scriptClass) {
    auto it = s_ScriptProperties.find(scriptClass);
    return it != s_ScriptProperties.end() && !it->second.empty();
}

void ScriptReflection::ClearAllScriptProperties() {
    s_ScriptProperties.clear();
}

void ScriptReflection::ClearScriptPropertiesForClass(const std::string& scriptClass) {
    auto it = s_ScriptProperties.find(scriptClass);
    if (it != s_ScriptProperties.end()) {
        it->second.clear();
    }
}

std::string ScriptReflection::PropertyTypeToString(PropertyType type) {
    switch (type) {
        case PropertyType::Int: return "int";
        case PropertyType::Float: return "float";
        case PropertyType::Bool: return "bool";
        case PropertyType::String: return "string";
        case PropertyType::Vector3: return "Vector3";
        case PropertyType::Entity: return "Entity";
        case PropertyType::ComponentRef: return "ComponentRef";
        case PropertyType::ScriptRef: return "ScriptRef";
        case PropertyType::Prefab: return "Prefab";
        case PropertyType::Enum: return "Enum";
        case PropertyType::List: return "List";
        case PropertyType::Struct: return "Struct";
        case PropertyType::ClayObject: return "ClayObject";
        case PropertyType::Mesh: return "Mesh";
        case PropertyType::DialogueLibrary: return "DialogueLibraryRef";
        case PropertyType::AnimatorController: return "AnimationController";
        case PropertyType::AnimatorControllerOverride: return "AnimationControllerOverride";
        case PropertyType::Texture: return "Texture";
        case PropertyType::Audio: return "AudioClip";
        default: return "unknown";
    }
}

PropertyType ScriptReflection::StringToPropertyType(const std::string& typeStr) {
    if (typeStr == "int") return PropertyType::Int;
    if (typeStr == "float") return PropertyType::Float;
    if (typeStr == "bool") return PropertyType::Bool;
    if (typeStr == "string") return PropertyType::String;
    if (typeStr == "Vector3") return PropertyType::Vector3;
    if (typeStr == "Entity") return PropertyType::Entity;
    if (typeStr == "ComponentRef") return PropertyType::ComponentRef;
    if (typeStr == "ScriptRef") return PropertyType::ScriptRef;
    if (typeStr == "Prefab") return PropertyType::Prefab;
    if (typeStr == "Enum") return PropertyType::Enum;
    if (typeStr == "List") return PropertyType::List;
    if (typeStr == "Struct") return PropertyType::Struct;
    if (typeStr == "ClayObject") return PropertyType::ClayObject;
    if (typeStr == "Mesh") return PropertyType::Mesh;
    if (typeStr == "DialogueLibraryRef") return PropertyType::DialogueLibrary;
    if (typeStr == "AnimationController") return PropertyType::AnimatorController;
    if (typeStr == "AnimationControllerOverride") return PropertyType::AnimatorControllerOverride;
    if (typeStr == "Texture") return PropertyType::Texture;
    if (typeStr == "AudioClip") return PropertyType::Audio;
    return PropertyType::Int; // Default fallback
}

// ---------------- Boxing helpers ----------------
PropertyValue ScriptReflection::BoxToValue(void* boxed, PropertyType type)
{
    // If no boxed value is provided from managed side, fall back to a
    // sensible typed default so variant alternatives always match the
    // declared PropertyType.
    if (!boxed)
    {
        switch (type)
        {
            case PropertyType::Int:      return 0;
            case PropertyType::Float:    return 0.0f;
            case PropertyType::Bool:     return false;
            case PropertyType::String:   return std::string();
            case PropertyType::Vector3:  return glm::vec3(0.0f);
            case PropertyType::Entity:   return -1; // "None" entity by convention
            case PropertyType::ComponentRef: return -1; // entity id holder
            case PropertyType::ScriptRef:    return -1; // entity id holder
            case PropertyType::Prefab:   return std::string(); // empty GUID string
            case PropertyType::Enum:     return 0; // default enum value
            case PropertyType::ClayObject: return std::string(); // empty GUID string
            case PropertyType::Mesh:     return std::string(); // empty "GUID:fileID" string
            case PropertyType::DialogueLibrary: return std::string(); // empty GUID string
            case PropertyType::AnimatorController: return std::string(); // empty path string
            case PropertyType::AnimatorControllerOverride: return std::string(); // empty path string
            case PropertyType::Texture:  return std::string(); // empty "GUID:fileID" string
            case PropertyType::Audio:    return std::string(); // empty GUID string
            case PropertyType::List:     return std::make_shared<ListPropertyValue>();
            case PropertyType::Struct:   return std::make_shared<StructPropertyValue>();
        }
    }

    switch (type)
    {
        case PropertyType::Int:
        case PropertyType::Entity:
        case PropertyType::ComponentRef:
        case PropertyType::ScriptRef:
        case PropertyType::Enum:  // Enum stored as int
            return *(int*)boxed;
        case PropertyType::Float:
            return *(float*)boxed;
        case PropertyType::Bool:
            return *(bool*)boxed;
        case PropertyType::String:
        case PropertyType::Prefab:  // Prefab stored as GUID string
        case PropertyType::ClayObject:  // ClayObject stored as GUID string
        case PropertyType::Mesh:  // Mesh stored as "GUID:fileID" string
        case PropertyType::DialogueLibrary:  // DialogueLibrary stored as GUID string
        case PropertyType::AnimatorController: // AnimatorController stored as VFS path string
        case PropertyType::AnimatorControllerOverride: // AnimatorControllerOverride stored as VFS path string
        case PropertyType::Texture:  // Texture stored as "GUID:fileID" string
        case PropertyType::Audio:  // Audio stored as GUID string
            return std::string((const char*)boxed);
        case PropertyType::Vector3:
            return *(glm::vec3*)boxed;
        case PropertyType::List:
            return std::make_shared<ListPropertyValue>();
        case PropertyType::Struct:
            return std::make_shared<StructPropertyValue>();
    }
    // Fallback to int 0 if an unknown type ever slips through
    return 0;
}

void* ScriptReflection::ValueToBox(const PropertyValue& v)
{
    // Thread-local buffer for string types to avoid memory leaks
    // The pointer is valid until the next call to ValueToBox with a string
    static thread_local std::string s_stringBuffer;
    
    return std::visit([](auto&& val)->void*
    {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::string>) {
            // For strings, return a pointer to C-string data (char*)
            // Managed code expects char* for Marshal.PtrToStringAnsi
            s_stringBuffer = val;
            return (void*)s_stringBuffer.c_str();
        } else {
            // For other types, allocate on heap
            T* p = new T(val);
            return (void*)p;
        }
    }, v);
}

std::string ScriptReflection::PropertyValueToString(const PropertyValue& value) {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, int>) {
            return std::to_string(v);
        } else if constexpr (std::is_same_v<T, float>) {
            return std::to_string(v);
        } else if constexpr (std::is_same_v<T, bool>) {
            return v ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::string>) {
            return v;
        } else if constexpr (std::is_same_v<T, glm::vec3>) {
            std::ostringstream oss;
            oss << v.x << "," << v.y << "," << v.z;
            return oss.str();
        } else if constexpr (std::is_same_v<T, std::shared_ptr<ListPropertyValue>>) {
            // Serialize list as pipe-separated elements, each element as string
            if (!v || v->elements.empty()) return "";
            std::string result;
            for (size_t i = 0; i < v->elements.size(); ++i) {
                if (i > 0) result += "|";
                result += PropertyValueToString(v->elements[i]);
            }
            return result;
        } else if constexpr (std::is_same_v<T, std::shared_ptr<StructPropertyValue>>) {
            // Struct serialization - not fully implemented
            return "";
        }
        return "";
    }, value);
}

PropertyValue ScriptReflection::StringToPropertyValue(const std::string& str, PropertyType type) {
    switch (type) {
        case PropertyType::Int:
            try { return std::stoi(str); }
            catch (...) { return 0; }
            
        case PropertyType::Float:
            try { return std::stof(str); }
            catch (...) { return 0.0f; }
            
        case PropertyType::Bool:
            return str == "true" || str == "1";
            
        case PropertyType::String:
        case PropertyType::Prefab:  // Prefab stored as GUID string
        case PropertyType::ClayObject:  // ClayObject stored as GUID string
        case PropertyType::Mesh:  // Mesh stored as "GUID:fileID" string
        case PropertyType::DialogueLibrary:  // DialogueLibrary stored as GUID string
        case PropertyType::AnimatorController:  // AnimatorController stored as VFS path string
        case PropertyType::AnimatorControllerOverride:  // AnimatorControllerOverride stored as VFS path string
        case PropertyType::Texture:  // Texture stored as "GUID:fileID" string
        case PropertyType::Audio:  // Audio stored as GUID string
            return str;
            
        case PropertyType::Enum:
            // Enum stored as int value
            try { return std::stoi(str); }
            catch (...) { return 0; }
            
        case PropertyType::Vector3: {
            std::istringstream iss(str);
            std::string token;
            glm::vec3 vec(0.0f);
            int component = 0;
            
            while (std::getline(iss, token, ',') && component < 3) {
                try {
                    vec[component] = std::stof(token);
                } catch (...) {
                    vec[component] = 0.0f;
                }
                component++;
            }
            return vec;
        }
        
        case PropertyType::List: {
            // Deserialize pipe-separated list - element type needs to come from property metadata
            // For now, store as string list; actual element parsing happens during sync
            auto list = std::make_shared<ListPropertyValue>();
            if (!str.empty()) {
                std::istringstream iss(str);
                std::string elem;
                while (std::getline(iss, elem, '|')) {
                    // Store elements as strings; managed side will interpret by element type
                    list->elements.push_back(elem);
                }
            }
            return list;
        }
        
        default:
            return 0;
    }
}


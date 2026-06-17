#include "ScriptReflection.h"
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <sstream>

namespace {

using json = nlohmann::json;

static bool IsStructuredList(const std::shared_ptr<ListPropertyValue>& list)
{
    if (!list) {
        return false;
    }

    if (list->elementType == PropertyType::Struct ||
        list->elementType == PropertyType::List ||
        list->elementType == PropertyType::Dictionary) {
        return true;
    }

    for (const PropertyValue& element : list->elements) {
        if (std::holds_alternative<std::shared_ptr<StructPropertyValue>>(element) ||
            std::holds_alternative<std::shared_ptr<ListPropertyValue>>(element) ||
            std::holds_alternative<std::shared_ptr<DictionaryPropertyValue>>(element)) {
            return true;
        }
    }

    return false;
}

static json PropertyValueToJson(const PropertyValue& value);

static json ListPropertyValueToJson(const std::shared_ptr<ListPropertyValue>& list)
{
    json array = json::array();
    if (!list) {
        return array;
    }

    for (const PropertyValue& element : list->elements) {
        array.push_back(PropertyValueToJson(element));
    }

    return array;
}

static json StructPropertyValueToJson(const std::shared_ptr<StructPropertyValue>& value)
{
    json object = json::object();
    if (!value) {
        return object;
    }

    for (const auto& field : value->fields) {
        object[field.first] = PropertyValueToJson(field.second);
    }

    return object;
}

static json DictionaryPropertyValueToJson(const std::shared_ptr<DictionaryPropertyValue>& value)
{
    json object = json::object();
    if (!value) {
        return object;
    }

    json entries = json::array();
    for (const auto& entry : value->entries) {
        entries.push_back({
            { "key", PropertyValueToJson(entry.first) },
            { "value", PropertyValueToJson(entry.second) }
        });
    }

    object["entries"] = std::move(entries);
    return object;
}

static json PropertyValueToJson(const PropertyValue& value)
{
    return std::visit([](const auto& typedValue) -> json {
        using T = std::decay_t<decltype(typedValue)>;
        if constexpr (std::is_same_v<T, int>) {
            return typedValue;
        } else if constexpr (std::is_same_v<T, float>) {
            return typedValue;
        } else if constexpr (std::is_same_v<T, bool>) {
            return typedValue;
        } else if constexpr (std::is_same_v<T, std::string>) {
            return typedValue;
        } else if constexpr (std::is_same_v<T, glm::vec3>) {
            return json::array({ typedValue.x, typedValue.y, typedValue.z });
        } else if constexpr (std::is_same_v<T, std::shared_ptr<ListPropertyValue>>) {
            return ListPropertyValueToJson(typedValue);
        } else if constexpr (std::is_same_v<T, std::shared_ptr<StructPropertyValue>>) {
            return StructPropertyValueToJson(typedValue);
        } else if constexpr (std::is_same_v<T, std::shared_ptr<DictionaryPropertyValue>>) {
            return DictionaryPropertyValueToJson(typedValue);
        } else {
            return json();
        }
    }, value);
}

static PropertyValue JsonToPropertyValue(const json& value)
{
    if (value.is_boolean()) {
        return value.get<bool>();
    }

    if (value.is_number_integer()) {
        return value.get<int>();
    }

    if (value.is_number()) {
        return value.get<float>();
    }

    if (value.is_string()) {
        return value.get<std::string>();
    }

    if (value.is_array()) {
        bool looksLikeVec3 = value.size() == 3;
        if (looksLikeVec3) {
            for (const auto& item : value) {
                if (!item.is_number()) {
                    looksLikeVec3 = false;
                    break;
                }
            }
        }

        if (looksLikeVec3) {
            return glm::vec3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
        }

        auto list = std::make_shared<ListPropertyValue>();
        for (const auto& item : value) {
            list->elements.push_back(JsonToPropertyValue(item));
        }
        if (!list->elements.empty()) {
            const PropertyValue& first = list->elements.front();
            if (std::holds_alternative<std::shared_ptr<StructPropertyValue>>(first)) {
                list->elementType = PropertyType::Struct;
            } else if (std::holds_alternative<std::shared_ptr<ListPropertyValue>>(first)) {
                list->elementType = PropertyType::List;
            } else if (std::holds_alternative<std::shared_ptr<DictionaryPropertyValue>>(first)) {
                list->elementType = PropertyType::Dictionary;
            }
        }
        return list;
    }

    if (value.is_object()) {
        if (value.size() == 1 && value.contains("entries") && value["entries"].is_array()) {
            auto dict = std::make_shared<DictionaryPropertyValue>();
            for (const auto& entry : value["entries"]) {
                dict->entries.emplace_back(
                    JsonToPropertyValue(entry.value("key", json())),
                    JsonToPropertyValue(entry.value("value", json())));
            }
            return dict;
        }

        auto structValue = std::make_shared<StructPropertyValue>();
        for (auto it = value.begin(); it != value.end(); ++it) {
            structValue->fields.emplace_back(it.key(), JsonToPropertyValue(it.value()));
        }
        return structValue;
    }

    return std::string();
}

} // namespace

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
        it->auxTypeName = property.auxTypeName;
        it->enumMeta = property.enumMeta;
        it->listElementType = property.listElementType;
        it->listElementTypeName = property.listElementTypeName;
        it->structFields = property.structFields;
        it->dictKeyType = property.dictKeyType;
        it->dictValueType = property.dictValueType;
        it->dictKeyTypeName = property.dictKeyTypeName;
        it->dictValueTypeName = property.dictValueTypeName;
        it->dictKeyEnumMeta = property.dictKeyEnumMeta;
        it->dictValueEnumMeta = property.dictValueEnumMeta;
        it->populateFromResources = property.populateFromResources;
        it->selectFromResources = property.selectFromResources;
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
        case PropertyType::Dictionary: return "Dictionary";
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
    if (typeStr == "Dictionary") return PropertyType::Dictionary;
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
            case PropertyType::Dictionary: return std::make_shared<DictionaryPropertyValue>();
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
        case PropertyType::Dictionary:
            return std::make_shared<DictionaryPropertyValue>();
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
            if (!v || v->elements.empty()) return "";

            if (IsStructuredList(v)) {
                return ListPropertyValueToJson(v).dump();
            }

            std::string result;
            for (size_t i = 0; i < v->elements.size(); ++i) {
                if (i > 0) result += "|";
                result += PropertyValueToString(v->elements[i]);
            }
            return result;
        } else if constexpr (std::is_same_v<T, std::shared_ptr<StructPropertyValue>>) {
            return StructPropertyValueToJson(v).dump();
        } else if constexpr (std::is_same_v<T, std::shared_ptr<DictionaryPropertyValue>>) {
            return DictionaryPropertyValueToJson(v).dump();
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
            auto list = std::make_shared<ListPropertyValue>();
            if (str.empty()) {
                return list;
            }

            try {
                size_t firstNonWhitespace = str.find_first_not_of(" \t\r\n");
                if (firstNonWhitespace != std::string::npos &&
                    (str[firstNonWhitespace] == '[' || str[firstNonWhitespace] == '{')) {
                    json parsed = json::parse(str);
                    if (parsed.is_array()) {
                        for (const auto& element : parsed) {
                            list->elements.push_back(JsonToPropertyValue(element));
                        }

                        if (!list->elements.empty()) {
                            const PropertyValue& first = list->elements.front();
                            if (std::holds_alternative<std::shared_ptr<StructPropertyValue>>(first)) {
                                list->elementType = PropertyType::Struct;
                            } else if (std::holds_alternative<std::shared_ptr<ListPropertyValue>>(first)) {
                                list->elementType = PropertyType::List;
                            } else if (std::holds_alternative<std::shared_ptr<DictionaryPropertyValue>>(first)) {
                                list->elementType = PropertyType::Dictionary;
                            }
                        }
                        return list;
                    }
                }
            } catch (...) {
            }

            std::istringstream iss(str);
            std::string elem;
            while (std::getline(iss, elem, '|')) {
                list->elements.push_back(elem);
            }
            return list;
        }

        case PropertyType::Struct: {
            if (str.empty()) {
                return std::make_shared<StructPropertyValue>();
            }

            try {
                json parsed = json::parse(str);
                if (parsed.is_object()) {
                    PropertyValue value = JsonToPropertyValue(parsed);
                    if (std::holds_alternative<std::shared_ptr<StructPropertyValue>>(value)) {
                        return value;
                    }
                }
            } catch (...) {
            }

            return std::make_shared<StructPropertyValue>();
        }

        case PropertyType::Dictionary: {
            if (str.empty()) {
                return std::make_shared<DictionaryPropertyValue>();
            }

            try {
                json parsed = json::parse(str);
                PropertyValue value = JsonToPropertyValue(parsed);
                if (std::holds_alternative<std::shared_ptr<DictionaryPropertyValue>>(value)) {
                    return value;
                }
            } catch (...) {
            }

            return std::make_shared<DictionaryPropertyValue>();
        }
        
        default:
            return 0;
    }
}


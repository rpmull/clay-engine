#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <functional>
#include <memory>
#include <glm/glm.hpp>
#include "core/assets/AssetReference.h"

// Forward declarations for complex property types
struct ListPropertyValue;
struct StructPropertyValue;
struct DictionaryPropertyValue;

// Supported property types for reflection
using PropertyValue = std::variant<int, float, bool, std::string, glm::vec3, 
                                   std::shared_ptr<ListPropertyValue>,
                                   std::shared_ptr<StructPropertyValue>,
                                   std::shared_ptr<DictionaryPropertyValue>>;

enum class PropertyType {
    Int,
    Float,
    Bool,
    String,
    Vector3,
    Entity,
    ComponentRef,
    ScriptRef,
    Prefab,          // Asset reference to .prefab files (stored as GUID string)
    Enum,            // Enum type (stores int value, displays dropdown)
    List,            // List<T> (expandable list UI)
    Struct,          // Serializable struct (expandable fields)
    ClayObject,      // Reference to ClayScriptableObject asset
    Mesh,            // Asset reference to mesh files (stored as "GUID:fileID" string)
    Dictionary,      // Dictionary<TKey, TValue> (table UI)
    DialogueLibrary,  // Reference to dialogue library asset (.dlglib files)
    AnimatorController, // Reference to animator controller asset (.animctrl path)
    AnimatorControllerOverride, // Reference to animator controller override asset (.animoverride path)
    Texture,         // Asset reference to texture files (stored as "GUID:fileID" string)
    Audio            // Asset reference to audio files (stored as GUID string)
};

// Enum metadata for dropdown display (must be before DictionaryPropertyValue)
struct EnumMetadata {
    std::vector<std::string> names;
    std::vector<int> values;
};

struct ScriptEntityRefMetadata {
    int32_t entityId = -1;
    ClaymoreGUID guid{};
    ClaymoreGUID modelGuid{};
    ClaymoreGUID modelRootGuid{};
    std::string modelNodePath;
    bool unresolved = false;
};

// Structure for list property values
struct ListPropertyValue {
    PropertyType elementType = PropertyType::Int;
    std::string elementTypeName;
    std::vector<PropertyValue> elements;
    // Optional metadata for entity-like lists to preserve GUID/path information
    std::vector<ScriptEntityRefMetadata> entityRefs;
};

// Structure for struct property values  
struct StructPropertyValue {
    std::string typeName;
    std::vector<std::pair<std::string, PropertyValue>> fields;
};

// Structure for dictionary property values
struct DictionaryPropertyValue {
    PropertyType keyType = PropertyType::String;
    PropertyType valueType = PropertyType::Int;
    std::string keyTypeName;    // For enum keys
    std::string valueTypeName;  // For enum/complex values
    EnumMetadata keyEnumMeta;   // If key is enum, dropdown options
    EnumMetadata valueEnumMeta; // If value is enum, dropdown options
    std::vector<std::pair<PropertyValue, PropertyValue>> entries;
};

struct PropertyInfo {
    std::string name;
    PropertyType type = PropertyType::Int;
    PropertyValue defaultValue;
    PropertyValue currentValue;
    // For ComponentRef/ScriptRef/ClayObject, holds the managed full type name to validate drops
    std::string auxTypeName;
    
    // Extended metadata for complex types
    EnumMetadata enumMeta;           // For Enum type: dropdown options
    PropertyType listElementType = PropertyType::Int;    // For List type: element type
    std::string listElementTypeName; // For List type: element type name (for enums/structs)
    std::vector<PropertyInfo> structFields; // For Struct type: nested fields
    
    // Dictionary metadata
    PropertyType dictKeyType = PropertyType::String;    // For Dictionary type: key type
    PropertyType dictValueType = PropertyType::Int;     // For Dictionary type: value type
    std::string dictKeyTypeName;    // For Dictionary type: key type name (for enums)
    std::string dictValueTypeName;  // For Dictionary type: value type name (for enums)
    EnumMetadata dictKeyEnumMeta;   // For Dictionary type with enum keys
    EnumMetadata dictValueEnumMeta; // For Dictionary type with enum values
    
    // Callbacks for getting/setting values from managed scripts
    std::function<PropertyValue()> getter;
    std::function<void(const PropertyValue&)> setter;
    
    // Flag indicating this field is auto-populated from resources (read-only in inspector)
    bool populateFromResources = false;
    
    // Flag indicating this ClayObject field should show as a dropdown of available resources
    bool selectFromResources = false;
};

class ScriptReflection {
public:
    // Static registry for script properties
    static void RegisterScriptProperty(const std::string& scriptClass, const PropertyInfo& property);
    static std::vector<PropertyInfo>& GetScriptProperties(const std::string& scriptClass);
    static bool HasProperties(const std::string& scriptClass);
    static void ClearAllScriptProperties();
    // Clear all previously registered properties for a script class (used on script reload)
    static void ClearScriptPropertiesForClass(const std::string& scriptClass);
    
    // Property type utilities
    static std::string PropertyTypeToString(PropertyType type);
    static PropertyType StringToPropertyType(const std::string& typeStr);

    // Boxing helpers for interop
    static PropertyValue BoxToValue(void* boxed, PropertyType type);
    static void*         ValueToBox(const PropertyValue& v);
    
    // Property value utilities
    static std::string PropertyValueToString(const PropertyValue& value);
    static PropertyValue StringToPropertyValue(const std::string& str, PropertyType type);

private:
    static std::unordered_map<std::string, std::vector<PropertyInfo>> s_ScriptProperties;
};

// Macro for easy property registration (would be used in managed code interop)
#define REGISTER_SCRIPT_PROPERTY(scriptClass, propertyName, propertyType, defaultVal) \
    ScriptReflection::RegisterScriptProperty(scriptClass, {propertyName, propertyType, defaultVal, defaultVal, std::string(), nullptr, nullptr});


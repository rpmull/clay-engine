#pragma once

#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include <functional>
#include <mutex>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>
#include "AssetReference.h"

// Scriptable assets are data-only assets described by managed (C#) schemas and
// registered at runtime via delegate-driven interop. They live as loose .asset files.

struct TypeId {
    uint64_t high = 0;
    uint64_t low = 0;
    bool operator==(const TypeId& o) const { return high == o.high && low == o.low; }
    bool operator!=(const TypeId& o) const { return !(*this == o); }
    bool IsValid() const { return high != 0 || low != 0; }
};

namespace std {
    template<> struct hash<TypeId> {
        size_t operator()(const TypeId& t) const noexcept {
            return std::hash<uint64_t>{}(t.high) ^ (std::hash<uint64_t>{}(t.low) << 1);
        }
    };
}

enum class ValueType : uint32_t {
    None = 0,
    Bool,
    Int32,
    Int64,
    Float,
    Double,
    String,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    Color,
    Guid,
    Enum,
    Mesh,             // Asset reference to mesh files (stored as "GUID:fileID" string)
    Prefab,           // Asset reference to prefab files (stored as GUID string)
    Struct,           // Serializable struct (nested fields)
    ClayObject,       // Reference to ClayScriptableObject asset (GUID)
    DialogueLibrary,  // Reference to dialogue library asset (GUID)
    Texture,          // Asset reference to texture files (stored as "GUID:fileID" string)
    AnimationController,         // Asset reference to animator controller files (stored as VFS path)
    AnimationControllerOverride, // Asset reference to animator controller override files (stored as VFS path)
    Audio,            // Asset reference to audio files (stored as GUID string)
};

struct Variant {
    using Array = std::vector<Variant>;
    using Struct = std::unordered_map<std::string, Variant>;
    using Storage = std::variant<std::monostate, bool, int32_t, int64_t, float, double, std::string,
                                 glm::vec2, glm::vec3, glm::vec4, glm::quat, ClaymoreGUID,
                                 Array, Struct>;
    Storage value;

    Variant() = default;
    template<typename T>
    Variant(T v) : value(std::move(v)) {}

    bool IsArray() const { return std::holds_alternative<Array>(value); }
};

struct FieldFlags {
    enum : uint32_t {
        Serialized = 1u << 0,
        ReadOnly   = 1u << 1,
        Hidden     = 1u << 2,
        PopulateFromResources = 1u << 3, // Auto-populated, read-only
        SelectFromResources   = 1u << 4, // Show dropdown of resources
    };
};

struct FieldDesc {
    std::string name;
    ValueType type = ValueType::None;
    uint32_t flags = FieldFlags::Serialized;
    int arrayRank = 0; // 0=scalar, 1=list
    std::string enumType;
    std::vector<std::string> enumNames;  // Enum value names for dropdown display
    std::vector<int> enumValues;         // Corresponding integer values
    // Extended metadata
    ValueType listElementType = ValueType::None;   // For list fields, the element type
    std::string listElementTypeName;               // Fully-qualified element type name (for enums/structs/clay)
    std::string auxType;                           // For ClayObject/Struct, managed full type name
    std::string structFieldsJson;                  // Serialized struct field info (for reconstruction)
    std::vector<FieldDesc> structFields;           // Parsed struct fields (filled from structFieldsJson)
    // Conditional visibility (empty showIfField = no condition)
    std::string showIfField;
    std::string showIfValue;
    uint32_t showIfMode = 0;                       // 0 = show when equal, 1 = hide when equal
};

struct ScriptableTypeDesc {
    TypeId id{};
    std::string fullName;
    std::string menuPath;
    std::string defaultFile;
    int order = 0;
    uint32_t version = 1;
    std::vector<FieldDesc> fields;
    // Creation hook (native allocation only). Managed default values are applied via interop.
    void* (*CreateNative)() = nullptr;
};

class ScriptableObject {
public:
    ScriptableObject() = default;
    virtual ~ScriptableObject() = default;

    const TypeId& GetTypeId() const { return m_TypeId; }
    const std::string& GetTypeName() const { return m_TypeName; }
    const ClaymoreGUID& GetGuid() const { return m_Guid; }

    bool GetField(const char* name, Variant& out) const;
    bool SetField(const char* name, const Variant& in);

    // JSON serialization entry points
    virtual bool Serialize(nlohmann::json& j) const;
    virtual bool Deserialize(const nlohmann::json& j, uint32_t version);

    // Change notifications
    using ChangeCallback = std::function<void(ScriptableObject&)>;
    void AddOnChanged(ChangeCallback cb);
    void RemoveOnChanged();
    void NotifyChanged();

    // Internal setters used by loader/registry
    void __SetType(const TypeId& id, const std::string& fullName) { m_TypeId = id; m_TypeName = fullName; }
    void __SetGuid(const ClaymoreGUID& g) { m_Guid = g; }

    // Field table access (thread-safe readers)
    const std::unordered_map<std::string, Variant>& __Fields() const { return m_Fields; }

protected:
    mutable std::mutex m_FieldMutex;
    std::unordered_map<std::string, Variant> m_Fields;
    TypeId m_TypeId{};
    std::string m_TypeName;
    ClaymoreGUID m_Guid{};
    ChangeCallback m_OnChanged;
};



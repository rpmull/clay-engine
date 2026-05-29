#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <optional>
#include <variant>
#include <cstdint>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/gtc/quaternion.hpp>

#include <nlohmann/json.hpp>

#include "core/utils/TypeId.h"

namespace cm {

   // Distinct wrapper so std::variant doesn't contain glm::vec4 twice.
   struct ColorRGBA {
      float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
      explicit ColorRGBA(const glm::vec4& v) : r(v.x), g(v.y), b(v.z), a(v.w) {}
      ColorRGBA() = default;
      glm::vec4 ToVec4() const { return glm::vec4{ r, g, b, a }; }
      };

   // Distinct wrapper so std::variant doesn't contain int32_t twice.
   struct EnumValue {
      int32_t value = 0;
      };

   // Value kinds for dynamic fields (align with editor widgets)
   enum class ValueType : uint8_t {
      Bool, Int, Int64, Float, Double,
      String,
      Vec2, Vec3, Vec4, Quat, Color, // Color maps to ColorRGBA wrapper
      Guid,                           // stored as hex string
      Enum                            // backed by int
      };

   // NOTE: Guid is always std::string here for portability;
   // if/when you integrate AssetReference, swap this alias + (de)serialize helpers.
   using VariantValue = std::variant<
      bool, int32_t, int64_t, float, double, std::string,
      glm::vec2, glm::vec3, glm::vec4, glm::quat, ColorRGBA,
      EnumValue           // Enum int wrapped
   >;

   struct Variant {
      ValueType   type{};
      VariantValue value{};

      Variant() = default;
      Variant(ValueType t, VariantValue v)
         : type(t), value(std::move(v)) {
         }

      template <class T>
      static Variant Make(ValueType t, T&& v) {
         return Variant{ t, VariantValue{ std::forward<T>(v) } };
         }
      };

   // Field declaration (schema info)
   struct FieldDesc {
      std::string name;
      ValueType   type;
      uint32_t    flags = 0;     // e.g., read-only, hidden
      int         arrayRank = 0; // 0 = scalar
      std::string enumType;      // name for enums (optional)
      };

   // A dynamic component attached to an entity
   class ModuleComponent {
   public:
      struct Field {
         std::string name;
         Variant     data;
         };

      ModuleComponent() = default;
      explicit ModuleComponent(TypeId id, uint32_t ver = 1) : m_Type(id), m_Version(ver) {}

      // Identity
      TypeId   GetTypeId() const noexcept { return m_Type; }
      uint32_t GetVersion() const noexcept { return m_Version; }
      void     SetVersion(uint32_t v) noexcept { m_Version = v; }

      // Schema bootstrap then values
      void DefineFields(const std::vector<FieldDesc>& defs);
      bool HasField(std::string_view name) const;

      // Typed setters/getters
      bool Set(std::string_view name, const Variant& v);
      std::optional<Variant> Get(std::string_view name) const;

      bool SetBool(std::string_view n, bool v);
      bool SetInt(std::string_view n, int32_t v);
      bool SetInt64(std::string_view n, int64_t v);
      bool SetFloat(std::string_view n, float v);
      bool SetDouble(std::string_view n, double v);
      bool SetString(std::string_view n, const std::string& v);
      bool SetVec2(std::string_view n, const glm::vec2& v);
      bool SetVec3(std::string_view n, const glm::vec3& v);
      bool SetVec4(std::string_view n, const glm::vec4& v);
      bool SetQuat(std::string_view n, const glm::quat& v);
      bool SetColor(std::string_view n, const ColorRGBA& v);
      bool SetGuid(std::string_view n, const std::string& guidHex);
      bool SetEnum(std::string_view n, int32_t v);

      // Iteration (ordered)
      const std::vector<Field>& Fields() const { return m_Fields; }

      // JSON (de)serialization of *values*:
      // Emits: { "typeId": "...", "version": N, "fields": { name: value, ... } }
      nlohmann::json SerializeJson() const;
      bool DeserializeJson(const nlohmann::json& j);

      // Reset values to defaults for its schema type (zeros/empty)
      void ResetToDefaults();

   private:
      TypeId m_Type{};
      uint32_t m_Version = 1;
      std::vector<Field> m_Fields; // keep order for UI
      std::unordered_map<std::string, size_t> m_Index; // quick lookup

      size_t index_of(std::string_view name) const;
      void   rebuild_index();

      static bool write_value_json(const Variant& v, nlohmann::json& out);
      static bool read_value_json(ValueType t, const nlohmann::json& in, Variant& out);
      };

   } // namespace cm

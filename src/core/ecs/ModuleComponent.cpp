#include "ModuleComponent.h"
#include <algorithm>

namespace cm {

   // ---------- small helpers (keep in cm namespace) ----------
   static inline nlohmann::json vec_to_json(const glm::vec2& v) {
      return nlohmann::json::array({ v.x, v.y });
      }
   static inline nlohmann::json vec_to_json(const glm::vec3& v) {
      return nlohmann::json::array({ v.x, v.y, v.z });
      }
   static inline nlohmann::json vec_to_json(const glm::vec4& v) {
      return nlohmann::json::array({ v.x, v.y, v.z, v.w });
      }
   static inline nlohmann::json quat_to_json(const glm::quat& q) {
      // (w,x,y,z) � change to (x,y,z,w) if that matches your engine
      return nlohmann::json::array({ q.w, q.x, q.y, q.z });
      }

   static inline bool json_to_vec2(const nlohmann::json& j, glm::vec2& v) {
      if (!j.is_array() || j.size() != 2) return false;
      v.x = j[0].get<float>(); v.y = j[1].get<float>(); return true;
      }
   static inline bool json_to_vec3(const nlohmann::json& j, glm::vec3& v) {
      if (!j.is_array() || j.size() != 3) return false;
      v.x = j[0].get<float>(); v.y = j[1].get<float>(); v.z = j[2].get<float>(); return true;
      }
   static inline bool json_to_vec4(const nlohmann::json& j, glm::vec4& v) {
      if (!j.is_array() || j.size() != 4) return false;
      v.x = j[0].get<float>(); v.y = j[1].get<float>(); v.z = j[2].get<float>(); v.w = j[3].get<float>(); return true;
      }
   static inline bool json_to_quat(const nlohmann::json& j, glm::quat& q) {
      if (!j.is_array() || j.size() != 4) return false;
      q.w = j[0].get<float>(); q.x = j[1].get<float>(); q.y = j[2].get<float>(); q.z = j[3].get<float>(); return true;
      }

   // ---------- ModuleComponent impl ----------

   void ModuleComponent::DefineFields(const std::vector<FieldDesc>& defs) {
      m_Fields.clear();
      m_Index.clear();
      m_Fields.reserve(defs.size());
      for (const auto& d : defs) {
         VariantValue vv{};
         switch (d.type) {
               case ValueType::Bool:   vv = false; break;
               case ValueType::Int:    vv = int32_t{ 0 }; break;
               case ValueType::Int64:  vv = int64_t{ 0 }; break;
               case ValueType::Float:  vv = 0.0f; break;
               case ValueType::Double: vv = 0.0; break;
               case ValueType::String: vv = std::string{}; break;
               case ValueType::Vec2:   vv = glm::vec2{ 0.0f }; break;
               case ValueType::Vec3:   vv = glm::vec3{ 0.0f }; break;
               case ValueType::Vec4:   vv = glm::vec4{ 0.0f }; break;
               case ValueType::Quat:   vv = glm::quat{ 1,0,0,0 }; break;
               case ValueType::Color:  vv = ColorRGBA{}; break;
               case ValueType::Guid:   vv = std::string{}; break;
               case ValueType::Enum:   vv = EnumValue{ 0 }; break;
            }
         m_Fields.push_back(Field{ d.name, Variant{ d.type, std::move(vv) } });
         }
      rebuild_index();
      }

   bool ModuleComponent::HasField(std::string_view name) const {
      return m_Index.find(std::string(name)) != m_Index.end();
      }

   size_t ModuleComponent::index_of(std::string_view name) const {
      auto it = m_Index.find(std::string(name));
      return it == m_Index.end() ? static_cast<size_t>(-1) : it->second;
      }

   void ModuleComponent::rebuild_index() {
      m_Index.clear();
      for (size_t i = 0; i < m_Fields.size(); ++i)
         m_Index.emplace(m_Fields[i].name, i);
      }

   bool ModuleComponent::Set(std::string_view name, const Variant& v) {
      const size_t i = index_of(name);
      if (i == static_cast<size_t>(-1)) return false;
      if (m_Fields[i].data.type != v.type) return false;
      m_Fields[i].data.value = v.value;
      return true;
      }

   std::optional<Variant> ModuleComponent::Get(std::string_view name) const {
      const size_t i = index_of(name);
      if (i == static_cast<size_t>(-1)) return std::nullopt;
      return m_Fields[i].data;
      }

   bool ModuleComponent::SetBool(std::string_view n, bool v) { return Set(n, Variant::Make(ValueType::Bool, v)); }
   bool ModuleComponent::SetInt(std::string_view n, int32_t v) { return Set(n, Variant::Make(ValueType::Int, v)); }
   bool ModuleComponent::SetInt64(std::string_view n, int64_t v) { return Set(n, Variant::Make(ValueType::Int64, v)); }
   bool ModuleComponent::SetFloat(std::string_view n, float v) { return Set(n, Variant::Make(ValueType::Float, v)); }
   bool ModuleComponent::SetDouble(std::string_view n, double v) { return Set(n, Variant::Make(ValueType::Double, v)); }
   bool ModuleComponent::SetString(std::string_view n, const std::string& v) { return Set(n, Variant::Make(ValueType::String, v)); }
   bool ModuleComponent::SetVec2(std::string_view n, const glm::vec2& v) { return Set(n, Variant::Make(ValueType::Vec2, v)); }
   bool ModuleComponent::SetVec3(std::string_view n, const glm::vec3& v) { return Set(n, Variant::Make(ValueType::Vec3, v)); }
   bool ModuleComponent::SetVec4(std::string_view n, const glm::vec4& v) { return Set(n, Variant::Make(ValueType::Vec4, v)); }
   bool ModuleComponent::SetQuat(std::string_view n, const glm::quat& v) { return Set(n, Variant::Make(ValueType::Quat, v)); }
   bool ModuleComponent::SetColor(std::string_view n, const ColorRGBA& v) { return Set(n, Variant::Make(ValueType::Color, v)); }
   bool ModuleComponent::SetGuid(std::string_view n, const std::string& v) { return Set(n, Variant::Make(ValueType::Guid, v)); }
   bool ModuleComponent::SetEnum(std::string_view n, int32_t v) { return Set(n, Variant::Make(ValueType::Enum, EnumValue{ v })); }

   // -------- JSON I/O --------

   nlohmann::json ModuleComponent::SerializeJson() const {
      nlohmann::json j;
      j["typeId"] = m_Type.ToHex();
      j["version"] = m_Version;
      auto& jf = j["fields"] = nlohmann::json::object();
      for (const auto& f : m_Fields) {
         nlohmann::json v;
         if (write_value_json(f.data, v)) {
            jf[f.name] = std::move(v);
            }
         }
      return j;
      }

   bool ModuleComponent::DeserializeJson(const nlohmann::json& j) {
      if (!j.contains("fields") || !j["fields"].is_object()) return false;
      const auto& jf = j["fields"];
      for (auto it = jf.begin(); it != jf.end(); ++it) {
         const std::string& name = it.key();
         const size_t idx = index_of(name);
         if (idx == static_cast<size_t>(-1)) {
            // Unknown field for this schema: ignore (or stash elsewhere)
            continue;
            }
         Variant out;
         if (!read_value_json(m_Fields[idx].data.type, it.value(), out))
            continue; // type mismatch; ignore
         // enforce type stickiness
         out.type = m_Fields[idx].data.type;
         m_Fields[idx].data = std::move(out);
         }
      return true;
      }

   void ModuleComponent::ResetToDefaults() {
      for (auto& f : m_Fields) {
         switch (f.data.type) {
               case ValueType::Bool:   f.data.value = false; break;
               case ValueType::Int:    f.data.value = int32_t{ 0 }; break;
               case ValueType::Int64:  f.data.value = int64_t{ 0 }; break;
               case ValueType::Float:  f.data.value = 0.0f; break;
               case ValueType::Double: f.data.value = 0.0; break;
               case ValueType::String: f.data.value = std::string{}; break;
               case ValueType::Vec2:   f.data.value = glm::vec2{ 0.0f }; break;
               case ValueType::Vec3:   f.data.value = glm::vec3{ 0.0f }; break;
               case ValueType::Vec4:   f.data.value = glm::vec4{ 0.0f }; break;
               case ValueType::Quat:   f.data.value = glm::quat{ 1,0,0,0 }; break;
               case ValueType::Color:  f.data.value = ColorRGBA{}; break;
               case ValueType::Guid:   f.data.value = std::string{}; break;
               case ValueType::Enum:   f.data.value = EnumValue{ 0 }; break;
            }
         }
      }

   // ---- helpers: JSON encode/decode for Variant ----

   bool ModuleComponent::write_value_json(const Variant& v, nlohmann::json& out) {
      switch (v.type) {
            case ValueType::Bool:   out = std::get<bool>(v.value); return true;
            case ValueType::Int:    out = std::get<int32_t>(v.value); return true;
            case ValueType::Int64:  out = std::get<int64_t>(v.value); return true;
            case ValueType::Float:  out = std::get<float>(v.value); return true;
            case ValueType::Double: out = std::get<double>(v.value); return true;
            case ValueType::String: out = std::get<std::string>(v.value); return true;
            case ValueType::Vec2:   out = vec_to_json(std::get<glm::vec2>(v.value)); return true;
            case ValueType::Vec3:   out = vec_to_json(std::get<glm::vec3>(v.value)); return true;
            case ValueType::Vec4:   out = vec_to_json(std::get<glm::vec4>(v.value)); return true;
            case ValueType::Quat:   out = quat_to_json(std::get<glm::quat>(v.value)); return true;
            case ValueType::Color: {
            const auto& c = std::get<ColorRGBA>(v.value);
            out = nlohmann::json::array({ c.r, c.g, c.b, c.a });
            return true;
            }
            case ValueType::Guid:   out = std::get<std::string>(v.value); return true;
            case ValueType::Enum:   out = std::get<EnumValue>(v.value).value; return true;
         }
      return false;
      }

   bool ModuleComponent::read_value_json(ValueType t, const nlohmann::json& in, Variant& out) {
      switch (t) {
            case ValueType::Bool:
               if (in.is_boolean()) { out = Variant::Make(t, in.get<bool>()); return true; } return false;
            case ValueType::Int:
               if (in.is_number_integer()) { out = Variant::Make(t, in.get<int32_t>()); return true; } return false;
            case ValueType::Int64:
               if (in.is_number_integer()) { out = Variant::Make(t, in.get<int64_t>()); return true; } return false;
            case ValueType::Float:
               if (in.is_number()) { out = Variant::Make(t, in.get<float>()); return true; } return false;
            case ValueType::Double:
               if (in.is_number()) { out = Variant::Make(t, in.get<double>()); return true; } return false;
            case ValueType::String:
               if (in.is_string()) { out = Variant::Make(t, in.get<std::string>()); return true; } return false;
                
            case ValueType::Vec2: {
            glm::vec2 v{}; if (!json_to_vec2(in, v)) return false;
            out = Variant::Make(t, v); return true;
            }
            case ValueType::Vec3: {
            glm::vec3 v{}; if (!json_to_vec3(in, v)) return false;
            out = Variant::Make(t, v); return true;
            }
            case ValueType::Vec4: {
            glm::vec4 v{}; if (!json_to_vec4(in, v)) return false;
            out = Variant::Make(t, v); return true;
            }
            case ValueType::Quat: {
            glm::quat q{}; if (!json_to_quat(in, q)) return false;
            out = Variant::Make(t, q); return true;
            }
            case ValueType::Color: {
            if (!in.is_array() || in.size() != 4) return false;
            ColorRGBA c{};
            c.r = in[0].get<float>();
            c.g = in[1].get<float>();
            c.b = in[2].get<float>();
            c.a = in[3].get<float>();
            out = Variant::Make(t, c);
            return true;
            }
            case ValueType::Guid: {
            if (!in.is_string()) return false;
            out = Variant::Make(t, in.get<std::string>());
            return true;
            }
            case ValueType::Enum: {
            if (!in.is_number_integer()) return false;
            EnumValue ev{ static_cast<int32_t>(in.get<int64_t>()) };
            out = Variant::Make(t, ev);
            return true;
            }
        }
      return false;
      }

   } // namespace cm

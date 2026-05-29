#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <optional>
#include "core/utils/TypeId.h"
#include "core/ecs/ModuleComponent.h"

namespace cm {

   // Forward declaration
   struct FieldDesc;


   //----------------------------------------------------------------------
   // Field Descriptor: describes a single field in a component type
   //----------------------------------------------------------------------
   struct ComponentDesc {
      cm::TypeId typeId{};
      std::string fullName;
      std::string menuPath;
      uint32_t version = 1;
      std::vector<FieldDesc> fields;
      int order = 0;
      // Optional managed hooks (resolved via interop layer)
      void* Upgrade = nullptr;         // bool (*)(TypeId,uint32_t, uint64_t entityId)
      void* CustomInspector = nullptr; // bool (*)(TypeId, void* nativeEntityOrComponent, void* drawerApi)
      };

   //----------------------------------------------------------------------
   // Component Registry: global registry of component types
   //----------------------------------------------------------------------
   class ComponentRegistry {
   public:
      static ComponentRegistry& Instance();

      bool Register(const ComponentDesc& desc);
      bool UnregisterByModulePrefix(const std::string& modulePrefix);

      const ComponentDesc* Find(const cm::TypeId& id) const;
      const ComponentDesc* FindByName(std::string_view fullName) const;
      const std::vector<const ComponentDesc*>& All() const { return m_AllOrdered; }

   private:
      std::unordered_map<cm::TypeId, ComponentDesc, cm::TypeIdHasher> m_ById;
      std::unordered_map<std::string, cm::TypeId> m_ByName;
      std::vector<const ComponentDesc*> m_AllOrdered;
      };

   }



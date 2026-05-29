#include "ComponentRegistry.h"
#include <iostream>

namespace cm {

   static ComponentRegistry* s_Instance = nullptr;

   ///----------------------------------------------------------------------
   /// Instance: Singleton instance accessor
   ///----------------------------------------------------------------------
   ComponentRegistry& ComponentRegistry::Instance() {
      if (!s_Instance)
         {
         s_Instance = new ComponentRegistry();
         }

      return *s_Instance;
      }


   ///----------------------------------------------------------------------
   /// Register: Registers a new component type. Returns true
   /// on success, false if the typeId or fullName is already registered.
   ///----------------------------------------------------------------------
   bool ComponentRegistry::Register(const ComponentDesc& desc) {
      if (desc.fullName.empty())
         {
         std::cerr << "[ComponentRegistry] Reject: empty fullName\n";
         return false;
         }

      if (desc.typeId.hi == 0 && desc.typeId.lo == 0)
         {
         std::cerr << "[ComponentRegistry] Reject: zero typeId for " << desc.fullName << "\n";
         return false;
         }

      if (m_ById.find(desc.typeId) != m_ById.end())
         {
         std::cerr << "[ComponentRegistry] Duplicate typeId: " << desc.fullName << " (" << desc.typeId << ")\n";
         return false;
         }

      if (m_ByName.find(desc.fullName) != m_ByName.end())
         {
         std::cerr << "[ComponentRegistry] Duplicate fullName: " << desc.fullName << "\n"; return false;
         }

      m_ById.emplace(desc.typeId, desc);
      m_ByName.emplace(desc.fullName, desc.typeId);
      m_AllOrdered.push_back(&m_ById.find(desc.typeId)->second);

      return true;
      }


   ///----------------------------------------------------------------------
   /// UnregisterByModulePrefix: Unregisters all component types whose
   /// fullName starts with the given modulePrefix.
   ///----------------------------------------------------------------------
   bool ComponentRegistry::UnregisterByModulePrefix(const std::string& modulePrefix)
      {

      // Remove any descriptors whose name starts with 
      // prefix + '.' or exact match prefix
      std::vector<cm::TypeId> toRemove;
      for (auto& kv : m_ById) {
         const std::string& n = kv.second.fullName;
         if (n.rfind(modulePrefix, 0) == 0)
            {
            toRemove.push_back(kv.first);
            }
         }

      if (toRemove.empty())
         {
         return false;
         }

      for (const auto& id : toRemove) {
         auto it = m_ById.find(id);
         if (it == m_ById.end())
            {
            continue;
            }

         m_ByName.erase(it->second.fullName);
         m_ById.erase(it);
         }

      // Rebuild ordered view
      m_AllOrdered.clear();
      m_AllOrdered.reserve(m_ById.size());

      for (auto& kv : m_ById)
         {
         m_AllOrdered.push_back(&kv.second);
         }

      return true;
      }


   ///----------------------------------------------------------------------
   /// Find: Finds a component descriptor by its typeId. Returns nullptr
   ///----------------------------------------------------------------------
   const ComponentDesc* ComponentRegistry::Find(const cm::TypeId& id) const {
      auto it = m_ById.find(id);
      return it == m_ById.end() ? nullptr : &it->second;
      }


   ///----------------------------------------------------------------------
   /// FindByName: Finds a component descriptor by its fullName. 
   /// Returns nullptr if not found.
   ///----------------------------------------------------------------------
   const ComponentDesc* ComponentRegistry::FindByName(std::string_view fullName) const {
      auto itn = m_ByName.find(std::string(fullName));
      
      if (itn == m_ByName.end()) 
         { 
         return nullptr; 
         }
      
      auto iti = m_ById.find(itn->second);
      return iti == m_ById.end() ? nullptr : &iti->second;
      }

   }
#pragma once
#include "ScriptComponent.h"
#include "ManagedScriptComponent.h"
#include <functional>
#include <cstdint>
#include <unordered_map>
#include <string>

class ScriptSystem {
public:
   enum ScriptTypeFlags : uint32_t {
      ScriptType_None = 0,
      ScriptType_DontDestroyOnLoad = 1 << 0,
      ScriptType_BakeInWorldGraph = 1 << 1,
      ScriptType_PreAnimationUpdate = 1 << 2
   };
   using ScriptFactory = std::function<std::shared_ptr<ScriptComponent>()>;

   static ScriptSystem& Instance() {
      static ScriptSystem instance;
      return instance;
      }

   void Register(const std::string& className, ScriptFactory factory) {
      m_Factories[className] = factory;
      }

   void RegisterManaged(const std::string& className) {
      m_Factories[className] = [className]() {
         return std::make_shared<ManagedScriptComponent>(className);
         };
      }

   void SetScriptPriority(const std::string& className, int priority) {
      m_ScriptPriority[className] = priority;
      }

   void SetScriptFlags(const std::string& className, uint32_t flags) {
      m_ScriptFlags[className] = flags;
      }

   uint32_t GetScriptFlags(const std::string& className) const {
      auto it = m_ScriptFlags.find(className);
      return it != m_ScriptFlags.end() ? it->second : ScriptType_None;
      }

   bool HasScriptFlag(const std::string& className, ScriptTypeFlags flag) const {
      return (GetScriptFlags(className) & flag) != 0;
      }

   bool IsScriptDontDestroyOnLoad(const std::string& className) const {
      return HasScriptFlag(className, ScriptType_DontDestroyOnLoad);
      }

   int GetScriptPriority(const std::string& className) const {
      auto it = m_ScriptPriority.find(className);
      return it != m_ScriptPriority.end() ? it->second : 0;
      }

   const std::unordered_map<std::string, ScriptFactory>& GetRegistry() const {
      return m_Factories;
      }


   std::shared_ptr<ScriptComponent> Create(const std::string& className) {
      auto it = m_Factories.find(className);
      if (it != m_Factories.end())
         return it->second();

      // If not native, assume managed
      return std::make_shared<ManagedScriptComponent>(className);
      }


private:
   std::unordered_map<std::string, ScriptFactory> m_Factories;
   std::unordered_map<std::string, int> m_ScriptPriority;
   std::unordered_map<std::string, uint32_t> m_ScriptFlags;
   };


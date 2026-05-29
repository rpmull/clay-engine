#pragma once

#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>
#include "ScriptableObject.h"

class ScriptableTypeRegistry {
public:
    static ScriptableTypeRegistry& Get() { static ScriptableTypeRegistry g; return g; }

    bool Register(const ScriptableTypeDesc& desc);
    const ScriptableTypeDesc* Find(const TypeId& id) const;
    const ScriptableTypeDesc* FindByName(const std::string& fullName) const;
    const std::vector<const ScriptableTypeDesc*>& All() const { return m_AllOrder; }

private:
    mutable std::mutex m_Mutex;
    std::unordered_map<TypeId, ScriptableTypeDesc> m_ById;
    std::unordered_map<std::string, TypeId> m_ByName;
    std::vector<const ScriptableTypeDesc*> m_AllOrder;
};

// Utility to compute a stable 128-bit TypeId from string name
TypeId ComputeTypeIdFromName(const std::string& fullName);



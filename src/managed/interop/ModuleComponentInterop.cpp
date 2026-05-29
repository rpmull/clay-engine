#include "ModuleComponentInterop.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/ModuleComponent.h"
#include "core/ecs/ComponentRegistry.h"
#include "core/utils/TypeId.h"
#include <cstring>
#include <iostream>

using namespace cm;

namespace {
constexpr size_t kMaxTypeNameLength = 512;

bool TryCopyTypeName(const char* src, std::string& out) {
    if (!src) return false;
#if defined(_MSC_VER)
    __try {
        size_t len = strnlen_s(src, kMaxTypeNameLength);
        if (len == 0 || len >= kMaxTypeNameLength) return false;
        out.assign(src, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    size_t len = strnlen(src, kMaxTypeNameLength);
    if (len == 0 || len >= kMaxTypeNameLength) return false;
    out.assign(src, len);
    return true;
#endif
}
} // namespace

static bool IsValidTypeId(const TypeId& id) {
    return id.hi != 0 || id.lo != 0;
}

static TypeId GetTypeIdFromName(const char* typeName) {
    std::string safeName;
    if (!TryCopyTypeName(typeName, safeName)) {
        std::cerr << "[ModuleInterop] Invalid module type name pointer" << std::endl;
        return {};
    }
    return TypeId::FromName(std::string_view(safeName));
}

// Helper function to get ModuleComponent from entity
static ModuleComponent* GetModuleComponent(int entityId, const char* typeName) {
    auto* scene = &Scene::Get();
    auto* entityData = scene->GetEntityData(entityId);
    if (!entityData) return nullptr;
    
    TypeId typeId = GetTypeIdFromName(typeName);
    if (!IsValidTypeId(typeId)) return nullptr;
    return scene->GetDynamicComponent(entityId, typeId);
}

// Module component lifecycle functions 
extern "C" bool HasModuleComponent_Native(int entityId, const char* typeName) {
    auto* scene = &Scene::Get();
    auto* entityData = scene->GetEntityData(entityId);
    if (!entityData) return false;
    
    TypeId typeId = GetTypeIdFromName(typeName);
    if (!IsValidTypeId(typeId)) return false;
    auto it = entityData->Dynamic.find(typeId);
    auto rtn = it != entityData->Dynamic.end();
    return rtn;
}

extern "C" void AddModuleComponent_Native(int entityId, const char* typeName) {
    auto* scene = &Scene::Get();
    TypeId typeId = GetTypeIdFromName(typeName);
    if (!IsValidTypeId(typeId)) return;
    scene->AddDynamicComponent(entityId, typeId);
}

extern "C" void RemoveModuleComponent_Native(int entityId, const char* typeName) {
    auto* scene = &Scene::Get();
    TypeId typeId = GetTypeIdFromName(typeName);
    if (!IsValidTypeId(typeId)) return;
    scene->RemoveDynamicComponent(entityId, typeId);
}

extern "C" void* GetModuleComponent_Native(int entityId, const char* typeName) {
    auto* comp = GetModuleComponent(entityId, typeName);
    return static_cast<void*>(comp);
}

extern "C" void* GetModuleComponentByFullName_Native(int entityId, const char* fullName) {
    auto* scene = &Scene::Get();
    auto* entityData = scene->GetEntityData(entityId);
    if (!entityData) return nullptr;
    std::string safeName;
    if (!TryCopyTypeName(fullName, safeName)) return nullptr;
    
    // Find component by full name in the registry
    if (const auto* desc = cm::ComponentRegistry::Instance().FindByName(safeName)) {
        auto it = entityData->Dynamic.find(desc->typeId);
        if (it != entityData->Dynamic.end()) {
            return static_cast<void*>(&it->second);
        }
    }
    
    return nullptr;
}

// Field getters
extern "C" bool GetModuleFieldBool_Native(int entityId, const char* typeName, const char* fieldName) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp) return false;
    
    auto value = comp->Get(fieldName);
    if (!value.has_value() || value->type != ValueType::Bool) return false;
    
    return std::get<bool>(value->value);
}

extern "C" int GetModuleFieldInt_Native(int entityId, const char* typeName, const char* fieldName) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp) return 0;
    
    auto value = comp->Get(fieldName);
    if (!value.has_value() || value->type != ValueType::Int) return 0;
    
    return std::get<int32_t>(value->value);
}

extern "C" long long GetModuleFieldInt64_Native(int entityId, const char* typeName, const char* fieldName) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp) return 0;
    
    auto value = comp->Get(fieldName);
    if (!value.has_value() || value->type != ValueType::Int64) return 0;
    
    return (long long)std::get<int64_t>(value->value);
}

extern "C" float GetModuleFieldFloat_Native(int entityId, const char* typeName, const char* fieldName) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp) return 0.0f;
    
    auto value = comp->Get(fieldName);
    if (!value.has_value() || value->type != ValueType::Float) return 0.0f;
    
    return std::get<float>(value->value);
}

extern "C" double GetModuleFieldDouble_Native(int entityId, const char* typeName, const char* fieldName) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp) return 0.0;
    
    auto value = comp->Get(fieldName);
    if (!value.has_value() || value->type != ValueType::Double) return 0.0;
    
    return std::get<double>(value->value);
}

extern "C" const char* GetModuleFieldString_Native(int entityId, const char* typeName, const char* fieldName) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp) return "";
    
    auto value = comp->Get(fieldName);
    if (!value.has_value() || value->type != ValueType::String) return "";
    
    const auto& str = std::get<std::string>(value->value);
    return str.c_str();
}

extern "C" void GetModuleFieldVec2_Native(int entityId, const char* typeName, const char* fieldName, float* x, float* y) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp || !x || !y) return;
    
    auto value = comp->Get(fieldName);
    if (!value.has_value() || value->type != ValueType::Vec2) {
        *x = *y = 0.0f;
        return;
    }
    
    const auto& vec = std::get<glm::vec2>(value->value);
    *x = vec.x;
    *y = vec.y;
}

extern "C" void GetModuleFieldVec3_Native(int entityId, const char* typeName, const char* fieldName, float* x, float* y, float* z) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp || !x || !y || !z) return;
    
    auto value = comp->Get(fieldName);
    if (!value.has_value() || value->type != ValueType::Vec3) {
        *x = *y = *z = 0.0f;
        return;
    }
    
    const auto& vec = std::get<glm::vec3>(value->value);
    *x = vec.x;
    *y = vec.y;
    *z = vec.z;
}

extern "C" void GetModuleFieldVec4_Native(int entityId, const char* typeName, const char* fieldName, float* x, float* y, float* z, float* w) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp || !x || !y || !z || !w) return;
    
    auto value = comp->Get(fieldName);
    if (!value.has_value() || value->type != ValueType::Vec4) {
        *x = *y = *z = *w = 0.0f;
        return;
    }
    
    const auto& vec = std::get<glm::vec4>(value->value);
    *x = vec.x;
    *y = vec.y;
    *z = vec.z;
    *w = vec.w;
}

extern "C" void GetModuleFieldQuat_Native(int entityId, const char* typeName, const char* fieldName, float* x, float* y, float* z, float* w) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp || !x || !y || !z || !w) return;
    
    auto value = comp->Get(fieldName);
    if (!value.has_value() || value->type != ValueType::Quat) {
        *x = *y = *z = 0.0f;
        *w = 1.0f;
        return;
    }
    
    const auto& quat = std::get<glm::quat>(value->value);
    *x = quat.x;
    *y = quat.y;
    *z = quat.z;
    *w = quat.w;
}

// Field setters
extern "C" void SetModuleFieldBool_Native(int entityId, const char* typeName, const char* fieldName, bool value) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp) return;
    
    comp->SetBool(fieldName, value);
}

extern "C" void SetModuleFieldInt_Native(int entityId, const char* typeName, const char* fieldName, int value) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp) return;
    
    comp->SetInt(fieldName, value);
}

extern "C" void SetModuleFieldInt64_Native(int entityId, const char* typeName, const char* fieldName, long long value) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp) return;
    
    comp->SetInt64(fieldName, value);
}

extern "C" void SetModuleFieldFloat_Native(int entityId, const char* typeName, const char* fieldName, float value) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp) return;
    
    comp->SetFloat(fieldName, value);
}

extern "C" void SetModuleFieldDouble_Native(int entityId, const char* typeName, const char* fieldName, double value) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp) return;
    
    comp->SetDouble(fieldName, value);
}

extern "C" void SetModuleFieldString_Native(int entityId, const char* typeName, const char* fieldName, const char* value) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp || !value) return;
    
    comp->SetString(fieldName, std::string(value));
}

extern "C" void SetModuleFieldVec2_Native(int entityId, const char* typeName, const char* fieldName, float x, float y) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp) return;
    
    comp->SetVec2(fieldName, glm::vec2(x, y));
}

extern "C" void SetModuleFieldVec3_Native(int entityId, const char* typeName, const char* fieldName, float x, float y, float z) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp) return;
    
    comp->SetVec3(fieldName, glm::vec3(x, y, z));
}

extern "C" void SetModuleFieldVec4_Native(int entityId, const char* typeName, const char* fieldName, float x, float y, float z, float w) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp) return;
    
    comp->SetVec4(fieldName, glm::vec4(x, y, z, w));
}

extern "C" void SetModuleFieldQuat_Native(int entityId, const char* typeName, const char* fieldName, float x, float y, float z, float w) {
    auto* comp = GetModuleComponent(entityId, typeName);
    if (!comp) return;
    
    comp->SetQuat(fieldName, glm::quat(w, x, y, z)); // Note: glm::quat constructor is (w, x, y, z)
}

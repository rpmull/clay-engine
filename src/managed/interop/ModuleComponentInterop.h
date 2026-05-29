#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

    // --- Module Component Lifecycle ---
    __declspec(dllexport) bool HasModuleComponent_Native(int entityId, const char* typeName);
    __declspec(dllexport) void AddModuleComponent_Native(int entityId, const char* typeName);
    __declspec(dllexport) void RemoveModuleComponent_Native(int entityId, const char* typeName);
    __declspec(dllexport) void* GetModuleComponent_Native(int entityId, const char* typeName);
    __declspec(dllexport) void* GetModuleComponentByFullName_Native(int entityId, const char* fullName);

    // --- Module Field Getters ---
    __declspec(dllexport) bool GetModuleFieldBool_Native(int entityId, const char* typeName, const char* fieldName);
    __declspec(dllexport) int GetModuleFieldInt_Native(int entityId, const char* typeName, const char* fieldName);
    __declspec(dllexport) long long GetModuleFieldInt64_Native(int entityId, const char* typeName, const char* fieldName);
    __declspec(dllexport) float GetModuleFieldFloat_Native(int entityId, const char* typeName, const char* fieldName);
    __declspec(dllexport) double GetModuleFieldDouble_Native(int entityId, const char* typeName, const char* fieldName);
    __declspec(dllexport) const char* GetModuleFieldString_Native(int entityId, const char* typeName, const char* fieldName);
    __declspec(dllexport) void GetModuleFieldVec2_Native(int entityId, const char* typeName, const char* fieldName, float* x, float* y);
    __declspec(dllexport) void GetModuleFieldVec3_Native(int entityId, const char* typeName, const char* fieldName, float* x, float* y, float* z);
    __declspec(dllexport) void GetModuleFieldVec4_Native(int entityId, const char* typeName, const char* fieldName, float* x, float* y, float* z, float* w);
    __declspec(dllexport) void GetModuleFieldQuat_Native(int entityId, const char* typeName, const char* fieldName, float* x, float* y, float* z, float* w);

    // --- Module Field Setters ---
    __declspec(dllexport) void SetModuleFieldBool_Native(int entityId, const char* typeName, const char* fieldName, bool value);
    __declspec(dllexport) void SetModuleFieldInt_Native(int entityId, const char* typeName, const char* fieldName, int value);
    __declspec(dllexport) void SetModuleFieldInt64_Native(int entityId, const char* typeName, const char* fieldName, long long value);
    __declspec(dllexport) void SetModuleFieldFloat_Native(int entityId, const char* typeName, const char* fieldName, float value);
    __declspec(dllexport) void SetModuleFieldDouble_Native(int entityId, const char* typeName, const char* fieldName, double value);
    __declspec(dllexport) void SetModuleFieldString_Native(int entityId, const char* typeName, const char* fieldName, const char* value);
    __declspec(dllexport) void SetModuleFieldVec2_Native(int entityId, const char* typeName, const char* fieldName, float x, float y);
    __declspec(dllexport) void SetModuleFieldVec3_Native(int entityId, const char* typeName, const char* fieldName, float x, float y, float z);
    __declspec(dllexport) void SetModuleFieldVec4_Native(int entityId, const char* typeName, const char* fieldName, float x, float y, float z, float w);
    __declspec(dllexport) void SetModuleFieldQuat_Native(int entityId, const char* typeName, const char* fieldName, float x, float y, float z, float w);

#ifdef __cplusplus
}
#endif

#pragma once
#include "ScriptReflection.h"
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Managed -> Native reflection registration
// ---------------------------------------------------------------------------
// propType is integer value of PropertyType enum
__declspec(dllexport) void RegisterScriptPropertyNative(const char* className,
                                                        const char* fieldName,
                                                        int propType,
                                                        void* boxedDefault,
                                                        const char* auxTypeFullName);

// Extended property registration with metadata for enum/list/struct types
// enumNames/enumValues: pipe-separated for enums (e.g., "None|Option1|Option2" and "0|1|2")
// listElementType: property type of list elements
// structFieldsJson: JSON array describing struct fields
// populateFromResources: if true, this field is auto-populated from Resources and read-only in inspector
// selectFromResources: if true, this ClayObject field shows as a dropdown of available resources
__declspec(dllexport) void RegisterScriptPropertyExtended(const char* className,
                                                          const char* fieldName,
                                                          int propType,
                                                          void* boxedDefault,
                                                          const char* auxTypeFullName,
                                                          const char* enumNames,
                                                          const char* enumValues,
                                                          int listElementType,
                                                          const char* listElementTypeName,
                                                          const char* structFieldsJson,
                                                          bool populateFromResources,
                                                          bool selectFromResources);

// Clear all registered properties for a given script class (used before re-registering after a reload)
__declspec(dllexport) void ClearScriptPropertiesNative(const char* className);

// Native -> Managed script type registration (priority used for OnCreate ordering)
__declspec(dllexport) void NativeRegisterScriptType(const char* className, int priority);
__declspec(dllexport) void NativeRegisterScriptFlags(const char* className, uint32_t flags);

// Native keeps a pointer to managed setter so Inspector can push values back
// SetManagedFieldPtr is now extern in core/managed/ScriptInterop.h

#ifdef __cplusplus
}
// Include ScriptInterop.h outside of extern "C" block
#include "core/managed/ScriptInterop.h"
#endif
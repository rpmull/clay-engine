#pragma once

#include <cstdint>
#include <string>
#include "assets/ScriptableObject.h"
#include "managed/interop/DotNetHost.h"

// Managed value used for interop marshaling (blittable). Arrays marshaled element-by-element.
struct ManagedValue {
    uint32_t type;     // ValueType
    uint32_t isArray;  // 0/1
    void*    data;     // pointer to value or array buffer
    uint32_t count;    // array length when isArray
};

// Blittable field descriptor for interop (matches managed FieldDesc)
struct FieldDescInterop {
    const char* name;       // UTF8 cstr
    uint32_t type;          // ValueType
    uint32_t flags;
    int32_t arrayRank;
    const char* enumType;   // UTF8 cstr
    const char* enumNames;  // UTF8 pipe-separated names (e.g., "None|Option1|Option2")
    const char* enumValues; // UTF8 pipe-separated values (e.g., "0|1|2")
    // Extended metadata for lists/structs/resource attributes
    uint32_t listElementType;      // ValueType for list elements
    const char* listElementTypeName; // UTF8 cstr for list element type (full name)
    const char* auxType;             // UTF8 cstr for ClayObject/Struct managed full name
    const char* structFieldsJson;    // UTF8 JSON describing nested struct fields
    uint32_t populateFromResources;  // 0/1 flag
    uint32_t selectFromResources;    // 0/1 flag
    // Conditional visibility (null/empty showIfField = no condition)
    const char* showIfField;
    const char* showIfValue;
    uint32_t showIfMode;             // 0 = show when equal, 1 = hide when equal
};

// Blittable type descriptor for interop (matches managed ScriptableTypeDesc)
struct ScriptableTypeDescInterop {
    TypeId id;
    const char* fullName;   // UTF8
    const char* menuPath;   // UTF8
    const char* defaultFile;// UTF8
    int32_t order;
    uint32_t version;
    FieldDescInterop* fields;
    int32_t fieldCount;
    void* CreateNative;     // fn ptr
};

struct InspectorDrawerAPI { /* reserved for future custom inspectors */ };

// Field order must match C# struct NativeScriptableAPI exactly (Sequential layout)
// Function pointers are marshaled as IntPtr when passed to managed code
struct NativeScriptableAPI {
    void* user;                                                              // Offset 0
    bool (*RegisterType)(const ScriptableTypeDescInterop&);                  // Offset 8
    bool (*SetField)(void* nativeInstance, const char* field, const ManagedValue&);  // Offset 16
    bool (*GetField)(void* nativeInstance, const char* field, ManagedValue* out);      // Offset 24
    bool (*MarkDirty)(void* nativeInstance);                                  // Offset 32
    const char* (*GetPathForGUID)(const char* guidStr);                      // Offset 40
    bool (*IsTypeAssignable)(const char* assetTypeName, const char* expectedTypeName);  // Offset 48
    const char* (*ReadFileContents)(const char* path);                       // Offset 56
    void (*InvalidateCache)(const char* guidStr);                            // Offset 64
};

struct ManagedScriptableAPI {
    void (*EnumerateTypes)(void* user);
    bool (*CreateDefault)(TypeId type, void* nativeInstance);
    bool (*CustomInspector)(TypeId type, void* nativeInstance, InspectorDrawerAPI*);
    bool (*Upgrade)(TypeId type, uint32_t fromVersion, void* nativeInstance);
};

// Entry point signature to fetch managed API from managed DLL
typedef void (*GetManagedScriptableAPI)(const NativeScriptableAPI* nativeApi, ManagedScriptableAPI* outManagedApi);

// Shared storage and helpers
const ManagedScriptableAPI* GetManagedScriptable();
void Scriptable_SetManagedAPI(const ManagedScriptableAPI& api);

// Native wrapper callbacks (exposed so DotNetHost can fill the table)
bool Scriptable_NativeRegisterType(const ScriptableTypeDescInterop& d);
bool Scriptable_NativeSetField(void* nativeInstance, const char* field, const ManagedValue& mv);
bool Scriptable_NativeGetField(void* nativeInstance, const char* field, ManagedValue* out);
bool Scriptable_NativeMarkDirty(void* nativeInstance);
const char* Scriptable_GetPathForGUID(const char* guidStr);
bool Scriptable_IsTypeAssignable(const char* assetTypeName, const char* expectedTypeName);
const char* Scriptable_ReadFileContents(const char* path);
void Scriptable_InvalidateCache(const char* guidStr);

// Function pointer type for clay object cache invalidation (extern so DotNetHost can set it)
using InvalidateCacheFn = void (*)(const char* guidStr);
extern InvalidateCacheFn g_InvalidateCache;



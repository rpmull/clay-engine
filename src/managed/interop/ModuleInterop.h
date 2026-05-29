#pragma once
#include <cstdint>
#include <string>
#include "core/utils/TypeId.h"

// Interop contract for module-defined components registration

struct InteropFieldDesc {
    const char* name;
    uint32_t type;    // cm::ValueType
    uint32_t flags;
    int32_t  arrayRank;
    const char* enumType; // optional
};

struct InteropComponentDesc {
    cm::TypeId typeId;
    const char* fullName;   // UTF8
    const char* menuPath;   // UTF8
    uint32_t version;
    InteropFieldDesc* fields;
    int32_t fieldCount;
    int32_t order;
    void* Upgrade;          // optional function pointers (opaque)
    void* CustomInspector;  // optional function pointers (opaque)
};

struct NativeAPIs {
    // Called by managed to register one component descriptor
    bool (*RegisterComponent)(const InteropComponentDesc&);
};

struct ManagedAPIs {
    // Managed will implement this and call back RegisterComponent repeatedly
    void (*EnumerateComponents)(void* user);
    // Optional hooks can be appended later
};

// Fill native API table
void FillModuleNativeAPIs(NativeAPIs& out);



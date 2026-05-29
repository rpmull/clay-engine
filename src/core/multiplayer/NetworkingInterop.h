#pragma once

#include "managed/interop/DotNetHost.h"

extern "C" {
__declspec(dllexport) const char* Networking_GetEntityGuid(int entityID);
__declspec(dllexport) int Networking_FindEntityByGuid(const char* guid);
}

using Networking_GetEntityGuid_fn = const char*(*)(int);
using Networking_FindEntityByGuid_fn = int(*)(const char*);

extern Networking_GetEntityGuid_fn Networking_GetEntityGuidPtr;
extern Networking_FindEntityByGuid_fn Networking_FindEntityByGuidPtr;

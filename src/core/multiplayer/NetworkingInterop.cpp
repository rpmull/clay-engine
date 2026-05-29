#include "NetworkingInterop.h"

#include "core/ecs/Scene.h"

Networking_GetEntityGuid_fn Networking_GetEntityGuidPtr = &Networking_GetEntityGuid;
Networking_FindEntityByGuid_fn Networking_FindEntityByGuidPtr = &Networking_FindEntityByGuid;

extern "C" {

__declspec(dllexport) const char* Networking_GetEntityGuid(int entityID) {
    static thread_local std::string buffer;
    buffer.clear();

    if (EntityData* data = Scene::Get().GetEntityData(entityID)) {
        buffer = data->EntityGuid.ToString();
    }

    return buffer.c_str();
}

__declspec(dllexport) int Networking_FindEntityByGuid(const char* guid) {
    if (!guid || guid[0] == '\0') {
        return INVALID_ENTITY_ID;
    }

    const ClaymoreGUID parsed = ClaymoreGUID::FromString(guid);
    if (parsed == ClaymoreGUID()) {
        return INVALID_ENTITY_ID;
    }

    return Scene::Get().FindEntityByGUID(parsed);
}

} // extern "C"

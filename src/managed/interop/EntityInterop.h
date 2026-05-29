#pragma once

#ifdef __cplusplus
extern "C" {
#endif

	// World position (extracted from WorldMatrix - use for reading actual world-space position)
	__declspec(dllexport) void GetEntityWorldPosition(int entityID, float* outX, float* outY, float* outZ);
	// Set world position (converts to local using parent's inverse WorldMatrix)
	__declspec(dllexport) void SetEntityWorldPosition(int entityID, float x, float y, float z);
	// Local position (direct read/write of Transform.Position)
	__declspec(dllexport) void GetEntityLocalPosition(int entityID, float* outX, float* outY, float* outZ);
	__declspec(dllexport) void SetEntityLocalPosition(int entityID, float x, float y, float z);
	__declspec(dllexport) int  FindEntityByName(const char* name);
	__declspec(dllexport) const char* GetEntityName(int entityID);
    
    // Entity management
    __declspec(dllexport) int  CreateEntity(const char* name);
    __declspec(dllexport) void DestroyEntity(int entityID);
    __declspec(dllexport) int GetEntityByID(int entityID);
    __declspec(dllexport) int* GetEntities();
    __declspec(dllexport) int GetEntityCount();

    // Rotation & Scale
    __declspec(dllexport) void GetEntityRotation(int entityID, float* outX, float* outY, float* outZ);
    __declspec(dllexport) void SetEntityRotation(int entityID, float x, float y, float z);
    // Quaternion rotation (x, y, z, w)
    __declspec(dllexport) void GetEntityRotationQuat(int entityID, float* outX, float* outY, float* outZ, float* outW);
    __declspec(dllexport) void SetEntityRotationQuat(int entityID, float x, float y, float z, float w);
    __declspec(dllexport) void GetEntityScale(int entityID, float* outX, float* outY, float* outZ);
    __declspec(dllexport) void SetEntityScale(int entityID, float x, float y, float z);

    // Physics
    __declspec(dllexport) void SetLinearVelocity(int entityID, float x, float y, float z);
    __declspec(dllexport) void SetAngularVelocity(int entityID, float x, float y, float z);

    // Visibility / Active
    __declspec(dllexport) void SetEntityVisible(int entityID, bool visible);
    __declspec(dllexport) bool GetEntityVisible(int entityID);
    __declspec(dllexport) void SetEntityPresentationHidden(int entityID, bool hidden);
    __declspec(dllexport) bool GetEntityPresentationHidden(int entityID);
    __declspec(dllexport) void SetEntityActive(int entityID, bool active);
    __declspec(dllexport) bool GetEntityActive(int entityID);

    // Entity duplication
    __declspec(dllexport) int DuplicateEntity(int entityID);

    // UI Mouse position (screen space coordinates)
    __declspec(dllexport) bool GetUIMousePosition(float* outX, float* outY);

    // Scene interop
    __declspec(dllexport) bool Scene_LoadScene(const char* path);
    __declspec(dllexport) bool Scene_LoadSceneEx(const char* path, bool async);
    __declspec(dllexport) float Scene_GetLoadProgress();
    __declspec(dllexport) bool Scene_IsSceneLoading();
    __declspec(dllexport) bool Scene_IsSceneLoaded();
    __declspec(dllexport) const char* Scene_GetCurrentScenePath();
    __declspec(dllexport) void Scene_UnloadScene();


#ifdef __cplusplus
}
#endif

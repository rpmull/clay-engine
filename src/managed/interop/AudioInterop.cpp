#include "core/audio/AudioComponents.h"
#include "core/assets/AssetReference.h"
#include "core/assets/IAssetResolver.h"
#include "core/ecs/Scene.h"

#include <algorithm>
#include <filesystem>
#include <string>

#ifndef CLAYMORE_RUNTIME
#include "editor/pipeline/AssetLibrary.h"
#endif

namespace {

constexpr int kAudioAssetType = 100;

std::string NormalizeAssetPath(const char* path)
{
    if (!path) return {};
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return normalized;
}

ClaymoreGUID ResolveGuidFromPath(const std::string& path)
{
    if (path.empty()) return {};

    const std::string normalized = NormalizeAssetPath(path.c_str());
    ClaymoreGUID guid{};

    if (IAssetResolver* resolver = Assets::GetResolver()) {
        guid = resolver->GetGUID(normalized);
    }

#ifndef CLAYMORE_RUNTIME
    if (guid == ClaymoreGUID()) {
        guid = AssetLibrary::Instance().GetGUIDForPath(normalized);
    }
    if (guid == ClaymoreGUID()) {
        guid = AssetLibrary::Instance().GetGUIDForPath(path);
    }
#endif

    return guid;
}

std::string ResolvePathFromGuid(const ClaymoreGUID& guid)
{
    if (guid == ClaymoreGUID()) return {};

    if (IAssetResolver* resolver = Assets::GetResolver()) {
        std::string path = resolver->GetPathForGUID(guid);
        if (!path.empty()) {
            return path;
        }
    }

#ifndef CLAYMORE_RUNTIME
    std::string editorPath = AssetLibrary::Instance().GetAudioPath(guid);
    if (!editorPath.empty()) {
        return editorPath;
    }
#endif

    return {};
}

AudioSourceComponent* GetAudioSource(int entityID)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->AudioSource) return nullptr;
    return data->AudioSource.get();
}

const char* Audio_GetAssetNameByGuid(unsigned long long hi, unsigned long long lo)
{
    static thread_local std::string s_nameBuffer;
    s_nameBuffer.clear();

    ClaymoreGUID guid{ hi, lo };
    if (guid == ClaymoreGUID()) {
        return "";
    }

#ifndef CLAYMORE_RUNTIME
    if (auto* asset = AssetLibrary::Instance().GetAsset(guid)) {
        s_nameBuffer = asset->name;
        if (!s_nameBuffer.empty()) {
            return s_nameBuffer.c_str();
        }
    }
#endif

    std::string path = ResolvePathFromGuid(guid);
    if (!path.empty()) {
        s_nameBuffer = std::filesystem::path(path).stem().string();
    }

    return s_nameBuffer.c_str();
}

const char* Audio_GetGuidFromPath(const char* path)
{
    static thread_local std::string s_guidBuffer;
    s_guidBuffer.clear();

    ClaymoreGUID guid = ResolveGuidFromPath(path ? path : "");
    if (guid == ClaymoreGUID()) {
        return "";
    }

    s_guidBuffer = guid.ToString();
    return s_guidBuffer.c_str();
}

void AudioSource_GetClipReference(int entityID, unsigned long long* outHi, unsigned long long* outLo)
{
    if (outHi) *outHi = 0;
    if (outLo) *outLo = 0;

    if (auto* source = GetAudioSource(entityID)) {
        if (!source->AudioClip.IsValid() && !source->AudioPath.empty()) {
            ClaymoreGUID resolved = ResolveGuidFromPath(source->AudioPath);
            if (resolved != ClaymoreGUID()) {
                source->AudioClip = AssetReference(resolved, 0, kAudioAssetType);
            }
        }
        if (outHi) *outHi = source->AudioClip.guid.high;
        if (outLo) *outLo = source->AudioClip.guid.low;
    }
}

void AudioSource_SetClipReference(int entityID, unsigned long long hi, unsigned long long lo)
{
    if (auto* source = GetAudioSource(entityID)) {
        ClaymoreGUID guid{ hi, lo };
        if (guid == ClaymoreGUID()) {
            source->AudioClip = AssetReference();
            source->AudioPath.clear();
            return;
        }

        source->AudioClip = AssetReference(guid, 0, kAudioAssetType);
        source->AudioPath = ResolvePathFromGuid(guid);
    }
}

const char* AudioSource_GetAudioPath(int entityID)
{
    static thread_local std::string s_pathBuffer;
    s_pathBuffer.clear();

    if (auto* source = GetAudioSource(entityID)) {
        s_pathBuffer = source->AudioPath;
    }

    return s_pathBuffer.c_str();
}

void AudioSource_SetAudioPath(int entityID, const char* path)
{
    if (auto* source = GetAudioSource(entityID)) {
        source->AudioPath = NormalizeAssetPath(path);
        const ClaymoreGUID guid = ResolveGuidFromPath(source->AudioPath);
        if (guid == ClaymoreGUID()) {
            source->AudioClip = AssetReference();
        } else {
            source->AudioClip = AssetReference(guid, 0, kAudioAssetType);
        }
    }
}

float AudioSource_GetVolume(int entityID) { if (auto* source = GetAudioSource(entityID)) return source->Volume; return 1.0f; }
void AudioSource_SetVolume(int entityID, float value) { if (auto* source = GetAudioSource(entityID)) source->Volume = value; }

float AudioSource_GetPitch(int entityID) { if (auto* source = GetAudioSource(entityID)) return source->Pitch; return 1.0f; }
void AudioSource_SetPitch(int entityID, float value) { if (auto* source = GetAudioSource(entityID)) source->Pitch = value; }

bool AudioSource_GetLoop(int entityID) { if (auto* source = GetAudioSource(entityID)) return source->Loop; return false; }
void AudioSource_SetLoop(int entityID, bool value) { if (auto* source = GetAudioSource(entityID)) source->Loop = value; }

bool AudioSource_GetPlayOnAwake(int entityID) { if (auto* source = GetAudioSource(entityID)) return source->PlayOnAwake; return true; }
void AudioSource_SetPlayOnAwake(int entityID, bool value) { if (auto* source = GetAudioSource(entityID)) source->PlayOnAwake = value; }

bool AudioSource_GetMute(int entityID) { if (auto* source = GetAudioSource(entityID)) return source->Mute; return false; }
void AudioSource_SetMute(int entityID, bool value) { if (auto* source = GetAudioSource(entityID)) source->Mute = value; }

bool AudioSource_GetSpatial(int entityID) { if (auto* source = GetAudioSource(entityID)) return source->Spatial; return true; }
void AudioSource_SetSpatial(int entityID, bool value) { if (auto* source = GetAudioSource(entityID)) source->Spatial = value; }

float AudioSource_GetMinDistance(int entityID) { if (auto* source = GetAudioSource(entityID)) return source->MinDistance; return 1.0f; }
void AudioSource_SetMinDistance(int entityID, float value) { if (auto* source = GetAudioSource(entityID)) source->MinDistance = value; }

float AudioSource_GetMaxDistance(int entityID) { if (auto* source = GetAudioSource(entityID)) return source->MaxDistance; return 50.0f; }
void AudioSource_SetMaxDistance(int entityID, float value) { if (auto* source = GetAudioSource(entityID)) source->MaxDistance = value; }

float AudioSource_GetDopplerFactor(int entityID) { if (auto* source = GetAudioSource(entityID)) return source->DopplerFactor; return 1.0f; }
void AudioSource_SetDopplerFactor(int entityID, float value) { if (auto* source = GetAudioSource(entityID)) source->DopplerFactor = value; }

float AudioSource_GetRolloff(int entityID) { if (auto* source = GetAudioSource(entityID)) return source->Rolloff; return 1.0f; }
void AudioSource_SetRolloff(int entityID, float value) { if (auto* source = GetAudioSource(entityID)) source->Rolloff = value; }

bool AudioSource_GetIsPlaying(int entityID) { if (auto* source = GetAudioSource(entityID)) return source->IsPlaying; return false; }
bool AudioSource_GetIsPaused(int entityID) { if (auto* source = GetAudioSource(entityID)) return source->IsPaused; return false; }

void AudioSource_Play(int entityID) { if (auto* source = GetAudioSource(entityID)) source->Play(); }
void AudioSource_Stop(int entityID) { if (auto* source = GetAudioSource(entityID)) source->Stop(); }
void AudioSource_Pause(int entityID) { if (auto* source = GetAudioSource(entityID)) source->Pause(); }
void AudioSource_Resume(int entityID) { if (auto* source = GetAudioSource(entityID)) source->Resume(); }

} // namespace

extern "C" {
    __declspec(dllexport) void* Get_Audio_GetAssetNameByGuid_Ptr() { return (void*)&Audio_GetAssetNameByGuid; }
    __declspec(dllexport) void* Get_Audio_GetGuidFromPath_Ptr() { return (void*)&Audio_GetGuidFromPath; }
    __declspec(dllexport) void* Get_AudioSource_GetClipReference_Ptr() { return (void*)&AudioSource_GetClipReference; }
    __declspec(dllexport) void* Get_AudioSource_SetClipReference_Ptr() { return (void*)&AudioSource_SetClipReference; }
    __declspec(dllexport) void* Get_AudioSource_GetAudioPath_Ptr() { return (void*)&AudioSource_GetAudioPath; }
    __declspec(dllexport) void* Get_AudioSource_SetAudioPath_Ptr() { return (void*)&AudioSource_SetAudioPath; }
    __declspec(dllexport) void* Get_AudioSource_GetVolume_Ptr() { return (void*)&AudioSource_GetVolume; }
    __declspec(dllexport) void* Get_AudioSource_SetVolume_Ptr() { return (void*)&AudioSource_SetVolume; }
    __declspec(dllexport) void* Get_AudioSource_GetPitch_Ptr() { return (void*)&AudioSource_GetPitch; }
    __declspec(dllexport) void* Get_AudioSource_SetPitch_Ptr() { return (void*)&AudioSource_SetPitch; }
    __declspec(dllexport) void* Get_AudioSource_GetLoop_Ptr() { return (void*)&AudioSource_GetLoop; }
    __declspec(dllexport) void* Get_AudioSource_SetLoop_Ptr() { return (void*)&AudioSource_SetLoop; }
    __declspec(dllexport) void* Get_AudioSource_GetPlayOnAwake_Ptr() { return (void*)&AudioSource_GetPlayOnAwake; }
    __declspec(dllexport) void* Get_AudioSource_SetPlayOnAwake_Ptr() { return (void*)&AudioSource_SetPlayOnAwake; }
    __declspec(dllexport) void* Get_AudioSource_GetMute_Ptr() { return (void*)&AudioSource_GetMute; }
    __declspec(dllexport) void* Get_AudioSource_SetMute_Ptr() { return (void*)&AudioSource_SetMute; }
    __declspec(dllexport) void* Get_AudioSource_GetSpatial_Ptr() { return (void*)&AudioSource_GetSpatial; }
    __declspec(dllexport) void* Get_AudioSource_SetSpatial_Ptr() { return (void*)&AudioSource_SetSpatial; }
    __declspec(dllexport) void* Get_AudioSource_GetMinDistance_Ptr() { return (void*)&AudioSource_GetMinDistance; }
    __declspec(dllexport) void* Get_AudioSource_SetMinDistance_Ptr() { return (void*)&AudioSource_SetMinDistance; }
    __declspec(dllexport) void* Get_AudioSource_GetMaxDistance_Ptr() { return (void*)&AudioSource_GetMaxDistance; }
    __declspec(dllexport) void* Get_AudioSource_SetMaxDistance_Ptr() { return (void*)&AudioSource_SetMaxDistance; }
    __declspec(dllexport) void* Get_AudioSource_GetDopplerFactor_Ptr() { return (void*)&AudioSource_GetDopplerFactor; }
    __declspec(dllexport) void* Get_AudioSource_SetDopplerFactor_Ptr() { return (void*)&AudioSource_SetDopplerFactor; }
    __declspec(dllexport) void* Get_AudioSource_GetRolloff_Ptr() { return (void*)&AudioSource_GetRolloff; }
    __declspec(dllexport) void* Get_AudioSource_SetRolloff_Ptr() { return (void*)&AudioSource_SetRolloff; }
    __declspec(dllexport) void* Get_AudioSource_GetIsPlaying_Ptr() { return (void*)&AudioSource_GetIsPlaying; }
    __declspec(dllexport) void* Get_AudioSource_GetIsPaused_Ptr() { return (void*)&AudioSource_GetIsPaused; }
    __declspec(dllexport) void* Get_AudioSource_Play_Ptr() { return (void*)&AudioSource_Play; }
    __declspec(dllexport) void* Get_AudioSource_Stop_Ptr() { return (void*)&AudioSource_Stop; }
    __declspec(dllexport) void* Get_AudioSource_Pause_Ptr() { return (void*)&AudioSource_Pause; }
    __declspec(dllexport) void* Get_AudioSource_Resume_Ptr() { return (void*)&AudioSource_Resume; }
}

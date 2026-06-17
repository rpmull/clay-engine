#include "AssetPipeline.h"
#include "AssetRegistry.h"
#include "core/assets/AssetMetadata.h"
#include "AssetLibrary.h"
#include "BinaryAssetCache.h"
#include "AssetEventBus.h"
#include "ModelNodeIdentity.h"
#include "editor/import/ModelLoader.h"
#include "core/rendering/TextureLoader.h"
#include "core/rendering/MaterialCache.h"
#include "core/rendering/MaterialAssetCache.h"
#include "core/vfs/FileSystem.h"
#include "core/rendering/ShaderManager.h"
#include "editor/import/AnimationImporter.h"
#include "editor/pipeline/AnimationImportSettings.h"
#include "core/animation/AnimationSerializer.h"
#include "core/animation/AnimationAssetCache.h"
#include "core/animation/AnimatorControllerIO.h"
#include "ModelImportCache.h"
#include "TextureCleanup.h"
#include "ShaderImporter.h"
#include "core/rendering/ShaderBundle.h"
#include "core/ecs/Components.h"
#include "core/ecs/Scene.h"
#include "core/model/ModelDelta.h"
#include "core/model/ModelDeltaExtractor.h"
#include "core/model/ModelDeltaApplicator.h"
#include "core/utils/Profiler.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <functional>
#include <cctype>
#include <limits>
#include <unordered_set>
#include <mutex>
#include <openssl/md5.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include "editor/ui/Logger.h"
#include <stb_image.h>
#include "managed/interop/DotNetHost.h"
#include "editor/Project.h"
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include "core/animation/AvatarSerializer.h"

#include "core/jobs/Jobs.h"
#include "core/jobs/JobSystem.h"
#include "core/jobs/ParallelFor.h"
#include <vector>
#include "core/navigation/NavSerialization.h"
#include "core/prefab/PrefabAPI.h"
#include "core/prefab/RuntimePrefabInstantiator.h"
#include "editor/prefab/PrefabEditorAPI.h"

// Needed for glm::decompose used when building humanoid bind-relative animation deltas.
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

static bool IsFbxAnimationSource(const std::string& path)
{
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".fbx";
}

static bool ShouldSkipProjectBuildDirectory(const fs::path& path)
{
    std::string name = path.filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return name == "bin" || name == "obj" || name == ".bin" || name == ".library" || name == ".git" || name == ".vs";
}

template <typename Fn>
void ForEachProjectEntry(const fs::path& rootPath, Fn&& fn)
{
    std::error_code ec;
    fs::recursive_directory_iterator it(
        rootPath,
        fs::directory_options::skip_permission_denied,
        ec);
    fs::recursive_directory_iterator end;

    while (!ec && it != end) {
        const auto entry = *it;
        if (entry.is_directory(ec) && ShouldSkipProjectBuildDirectory(entry.path())) {
            it.disable_recursion_pending();
            ++it;
            continue;
        }

        fn(entry);
        ++it;
    }
}

template <typename Fn>
void ForEachProjectScriptFile(const fs::path& rootPath, Fn&& fn)
{
    ForEachProjectEntry(rootPath, [&](const fs::directory_entry& entry) {
        std::error_code ec;
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".cs") {
            return;
        }

        fn(entry);
    });
}

// Serialize Roslyn/GameScripts.dll builds. ProcessMainThreadTasks runs ImportAsset in parallel;
// a bulk touch (e.g. normalizing every .cs) enqueues one import per file, each calling ImportScript.
// Without this lock that meant concurrent ScriptCompiler.exe + concurrent writes to the same DLL/PDB.
static std::mutex g_ScriptCompileMutex;

static void ComputeNewestProjectScriptWriteTime(const fs::path& rootPath,
                                                fs::file_time_type& outNewest,
                                                bool& outHasAny)
{
    outHasAny = false;
    ForEachProjectScriptFile(rootPath, [&](const fs::directory_entry& entry) {
        std::error_code ec;
        if (!entry.is_regular_file(ec)) {
            return;
        }
        auto wt = fs::last_write_time(entry.path(), ec);
        if (ec) {
            return;
        }
        if (!outHasAny || outNewest < wt) {
            outNewest = wt;
            outHasAny = true;
        }
    });
}

static bool IsGameScriptsDllUpToDate(const fs::path& projectRoot, const fs::path& scriptsDllPath)
{
    std::error_code ec;
    if (!fs::exists(scriptsDllPath, ec) || ec) {
        return false;
    }
    fs::file_time_type newestSourceTime{};
    bool hasScripts = false;
    ComputeNewestProjectScriptWriteTime(projectRoot, newestSourceTime, hasScripts);
    if (!hasScripts) {
        return true;
    }
    auto dllTime = fs::last_write_time(scriptsDllPath, ec);
    if (ec) {
        return false;
    }
    return !(dllTime < newestSourceTime);
}

static std::string MakeProjectRelativeVPath(const std::string& diskPath)
{
    std::error_code ec;
    fs::path rel = fs::relative(diskPath, Project::GetProjectDirectory(), ec);
    std::string vpath = (ec ? diskPath : rel.string());
    std::replace(vpath.begin(), vpath.end(), '\\', '/');
    return vpath;
}

// Sidecar .meta is parsed from parallel ImportAsset; must not throw (parallel_for rethrows worker exceptions).
// Strips UTF-8 BOM so files written with utf-8-sig (e.g. tooling) still parse.
static bool TryLoadSidecarMeta(const fs::path& metaPath, AssetMetadata& outMeta)
{
    try {
        std::ifstream in(metaPath, std::ios::binary);
        if (!in) {
            return false;
        }
        std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (raw.size() >= 3 &&
            static_cast<unsigned char>(raw[0]) == 0xEF &&
            static_cast<unsigned char>(raw[1]) == 0xBB &&
            static_cast<unsigned char>(raw[2]) == 0xBF) {
            raw.erase(0, 3);
        }
        json j = json::parse(raw);
        outMeta = j.get<AssetMetadata>();
        return true;
    } catch (const json::exception& e) {
        std::cerr << "[AssetPipeline] Invalid .meta JSON \"" << metaPath.string() << "\": " << e.what() << std::endl;
        return false;
    } catch (const std::exception& e) {
        std::cerr << "[AssetPipeline] Failed to read .meta \"" << metaPath.string() << "\": " << e.what() << std::endl;
        return false;
    }
}

static bool IsValidGuid(const ClaymoreGUID& guid)
{
    return guid.high != 0 || guid.low != 0;
}

static std::string LowerExtension(const fs::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

static bool IsPrefabAuthoringJson(const std::string& path)
{
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return normalized.find("assets/prefabs/") != std::string::npos;
}

static AssetType InferAssetTypeForMetadata(const std::string& path, const AssetMetadata& meta)
{
    const std::string ext = LowerExtension(fs::path(path));
    if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") return AssetType::Mesh;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") return AssetType::Texture;
    if (ext == ".sc" || ext == ".glsl" || ext == ".shader") return AssetType::Shader;
    if (ext == ".mat") return AssetType::Material;
    if (ext == ".prefab" || (ext == ".json" && IsPrefabAuthoringJson(path))) return AssetType::Prefab;
    if (ext == ".anim") return AssetType::Animation;
    if (ext == ".animctrl" || ext == ".controller") return AssetType::AnimatorController;
    if (ext == ".ngraph") return AssetType::NodeGraph;
    if (ext == ".navbin") return AssetType::NavMesh;
    if (ext == ".ttf" || ext == ".otf") return AssetType::Font;
    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac") return AssetType::Audio;
    if (ext == ".asset" || ext == ".clayobj" || ext == ".dlglib") return AssetType::Scriptable;
    if (ext == ".cs") return AssetType::Script;

    if (meta.type == "model") return AssetType::Mesh;
    if (meta.type == "texture") return AssetType::Texture;
    if (meta.type == "shader") return AssetType::Shader;
    if (meta.type == "material") return AssetType::Material;
    if (meta.type == "prefab") return AssetType::Prefab;
    if (meta.type == "animation") return AssetType::Animation;
    if (meta.type == "animatorcontroller") return AssetType::AnimatorController;
    if (meta.type == "nodegraph") return AssetType::NodeGraph;
    if (meta.type == "navmesh") return AssetType::NavMesh;
    if (meta.type == "font") return AssetType::Font;
    if (meta.type == "audio") return AssetType::Audio;
    if (meta.type == "scriptable" || meta.type == "dialogue") return AssetType::Scriptable;
    if (meta.type == "script") return AssetType::Script;
    return AssetType::Unknown;
}

static void RegisterMetadataForLookup(const std::string& path, const AssetMetadata& meta)
{
    AssetRegistry::Instance().SetMetadata(path, meta);

    AssetReference ref = meta.reference;
    if (!ref.IsValid() && IsValidGuid(meta.guid)) {
        ref.guid = meta.guid;
    }
    if (!ref.IsValid()) {
        return;
    }

    const AssetType type = InferAssetTypeForMetadata(path, meta);
    if (type == AssetType::Unknown) {
        return;
    }
    if (ref.type == 0) {
        ref.type = static_cast<int32_t>(type);
    }

    const std::string name = fs::path(path).stem().string();
    AssetLibrary::Instance().RegisterAsset(ref, type, path, name);

    const std::string projectRelative = MakeProjectRelativeVPath(path);
    if (!projectRelative.empty() && projectRelative != path) {
        AssetLibrary::Instance().RegisterPathAlias(ref.guid, projectRelative);
    }

    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    const size_t assetsPos = normalized.find("assets/");
    if (assetsPos != std::string::npos) {
        AssetLibrary::Instance().RegisterPathAlias(ref.guid, normalized.substr(assetsPos));
    }
}

static void EnsurePreparedBinaryIfNeeded(const std::string& path)
{
    if (BinaryAssetCache::GetAssetType(path) == BinaryAssetCache::AssetType::Unknown) {
        return;
    }
    if (!BinaryAssetCache::Instance().IsCurrent(path)) {
        (void)BinaryAssetCache::Instance().EnsureBinary(path);
    }
}

static glm::mat4 AssimpMat4ToGlm(const aiMatrix4x4& m)
{
    glm::mat4 out(1.0f);
    out[0][0] = m.a1; out[1][0] = m.a2; out[2][0] = m.a3; out[3][0] = m.a4;
    out[0][1] = m.b1; out[1][1] = m.b2; out[2][1] = m.b3; out[3][1] = m.b4;
    out[0][2] = m.c1; out[1][2] = m.c2; out[2][2] = m.c3; out[3][2] = m.c4;
    out[0][3] = m.d1; out[1][3] = m.d2; out[2][3] = m.d3; out[3][3] = m.d4;
    return out;
}

static glm::quat ComputeAnimationAxisFix(const aiScene* scene, bool isFbx)
{
    if (isFbx) {
        return glm::angleAxis(-glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
    }

    int upAxis = 1;
    int upSign = 1;
    if (scene && scene->mMetaData) {
        (void)scene->mMetaData->Get("UpAxis", upAxis);
        (void)scene->mMetaData->Get("UpAxisSign", upSign);
    }

    if (upAxis == 2) {
        float ang = (upSign >= 0 ? -glm::half_pi<float>() : glm::half_pi<float>());
        return glm::angleAxis(ang, glm::vec3(1.0f, 0.0f, 0.0f));
    }
    if (upAxis == 0) {
        float ang = (upSign >= 0 ? glm::half_pi<float>() : -glm::half_pi<float>());
        return glm::angleAxis(ang, glm::vec3(0.0f, 0.0f, 1.0f));
    }
    if (upAxis == 1 && upSign < 0) {
        return glm::angleAxis(glm::pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
    }
    return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

static glm::mat4 ApplyAxisFixToRootLocal(const glm::mat4& local,
                                         const glm::quat& axisFixQ,
                                         bool enabled,
                                         bool isFbx)
{
    if (!enabled || axisFixQ == glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) {
        return local;
    }

    glm::vec3 t(0.0f), s(1.0f), skew(0.0f);
    glm::vec4 persp(0.0f);
    glm::quat r(1.0f, 0.0f, 0.0f, 0.0f);
    glm::decompose(local, s, r, t, skew, persp);
    r = glm::normalize(axisFixQ * glm::normalize(r));
    if (isFbx) {
        // Match AnimationImporter::ImportFromModel: FBX root positions are converted
        // from Blender Z-up to engine Y-up, while non-FBX root translations remain as-is.
        t = glm::vec3(t.x, t.z, -t.y);
    }
    return glm::translate(glm::mat4(1.0f), t)
         * glm::mat4_cast(r)
         * glm::scale(glm::mat4(1.0f), s);
}

static bool TryDecomposeAnimationLocal(const glm::mat4& local,
                                       glm::vec3& outT,
                                       glm::quat& outR,
                                       glm::vec3& outS)
{
    glm::vec3 skew(0.0f);
    glm::vec4 perspective(0.0f);
    if (!glm::decompose(local, outS, outR, outT, skew, perspective)) {
        outT = glm::vec3(0.0f);
        outR = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        outS = glm::vec3(1.0f);
        return false;
    }

    outR = glm::normalize(outR);
    return std::isfinite(outT.x) && std::isfinite(outT.y) && std::isfinite(outT.z) &&
           std::isfinite(outS.x) && std::isfinite(outS.y) && std::isfinite(outS.z) &&
           std::isfinite(outR.x) && std::isfinite(outR.y) && std::isfinite(outR.z) &&
           std::isfinite(outR.w);
}

static void ResolveHumanoidAuthoringBindBasis(const cm::animation::AnimationClip& clip,
                                              const std::string& boneName,
                                              const cm::animation::AvatarDefinition& srcAvatar,
                                              int mappedId,
                                              glm::vec3& outTranslationBind,
                                              glm::vec3& outTranslationDeltaScale,
                                              glm::quat& outRotationBind,
                                              glm::vec3& outScaleBind)
{
    outTranslationBind = glm::vec3(0.0f);
    outTranslationDeltaScale = glm::vec3(1.0f);
    outRotationBind = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    outScaleBind = glm::vec3(1.0f);

    bool hasSourceRestLocal = false;
    glm::vec3 restT(0.0f), restS(1.0f);
    glm::quat restR(1.0f, 0.0f, 0.0f, 0.0f);
    if (auto itRest = clip.SourceRestLocalTransforms.find(boneName);
        itRest != clip.SourceRestLocalTransforms.end()) {
        hasSourceRestLocal = TryDecomposeAnimationLocal(itRest->second, restT, restR, restS);
    }

    bool hasAvatarBind = false;
    glm::vec3 avatarBindT(0.0f), avatarBindS(1.0f);
    glm::quat avatarBindR(1.0f, 0.0f, 0.0f, 0.0f);
    if (mappedId >= 0 && mappedId < static_cast<int>(cm::animation::HumanoidBoneCount)) {
        hasAvatarBind = srcAvatar.GetAnimationBindTRS(static_cast<cm::animation::HumanoidBone>(mappedId),
                                                      avatarBindT,
                                                      avatarBindR,
                                                      avatarBindS);
    }

    if (hasSourceRestLocal) {
        outTranslationBind = restT;
        outRotationBind = restR;
        outScaleBind = restS;
    }

    if (hasAvatarBind) {
        // Rotation retargeting should stay anchored to the avatar bind pose because source node
        // rest rotations can include FBX pre/post-rotation bake differences that do not exist
        // on the runtime skeleton. Translation and scale stay in authored node-local space.
        outRotationBind = avatarBindR;
        if (hasSourceRestLocal) {
            outTranslationDeltaScale = glm::vec3(
                (std::abs(restS.x) > 1e-6f) ? (avatarBindS.x / restS.x) : 1.0f,
                (std::abs(restS.y) > 1e-6f) ? (avatarBindS.y / restS.y) : 1.0f,
                (std::abs(restS.z) > 1e-6f) ? (avatarBindS.z / restS.z) : 1.0f
            );
        }
        if (!hasSourceRestLocal) {
            outTranslationBind = avatarBindT;
            outScaleBind = avatarBindS;
        }
    }
}

// Collision-resistant GUID packing using FNV-1a hash
static uint64_t PackGuidHash(const ClaymoreGUID& g) { 
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME = 1099511628211ULL;
    uint64_t hash = FNV_OFFSET;
    for (int i = 0; i < 8; ++i) { hash ^= (g.high >> (i * 8)) & 0xFF; hash *= FNV_PRIME; }
    for (int i = 0; i < 8; ++i) { hash ^= (g.low >> (i * 8)) & 0xFF; hash *= FNV_PRIME; }
    return hash;
}

// ---------------------------------------
// SCAN PROJECT (background safe)
// ---------------------------------------
void AssetPipeline::ScanProject(const std::string& rootPath) {
    m_LastScanList.clear();
    ForEachProjectEntry(rootPath, [&](const fs::directory_entry& entry) {
        std::error_code ec;
        if (!entry.is_regular_file(ec)) return;
        std::string ext = LowerExtension(entry.path());
        if (!IsSupportedAsset(ext)) return;

        std::string filePath = entry.path().string();
        m_LastScanList.push_back(filePath);

        AssetMetadata sidecarMeta;
        const bool hasSidecarMeta = TryLoadSidecarMeta(filePath + ".meta", sidecarMeta);
        const std::string hash = ComputeFileHash(filePath);
        if (hasSidecarMeta && sidecarMeta.hash == hash) {
            RegisterMetadataForLookup(filePath, sidecarMeta);
        }

        const bool needsPreparedBinary =
            BinaryAssetCache::GetAssetType(filePath) != BinaryAssetCache::AssetType::Unknown &&
            !BinaryAssetCache::Instance().IsCurrent(filePath);

        if (!hasSidecarMeta || sidecarMeta.hash != hash || needsPreparedBinary) {
            EnqueueAssetImport(filePath);
        }
    });
    std::cout << "[AssetPipeline] Scan complete. Assets found: " << m_LastScanList.size() << std::endl;
}

// ---------------------------------------
// QUEUE FOR CPU IMPORT
// ---------------------------------------
void AssetPipeline::EnqueueAssetImport(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_QueueMutex);
    m_ImportQueue.push(path);
}

// ---------------------------------------
// PROCESS IMPORTS + GPU TASKS
// ---------------------------------------
void AssetPipeline::ProcessMainThreadTasks() {
    ScopedTimer t("AssetPipeline/MainThread");
    m_LastBudgetStats = {};
    const auto frameStart = std::chrono::high_resolution_clock::now();
    const MainThreadBudgetConfig cfg = m_MainThreadBudgetConfig;

    // 1. Import queue (CPU-bound work can run in parallel)
    std::vector<std::string> batch;
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        const size_t importCap = cfg.Enabled ? std::max<size_t>(1, cfg.MaxQueuedTasksPerFrame) : m_ImportQueue.size();
        while (!m_ImportQueue.empty() && batch.size() < importCap) {
            batch.push_back(std::move(m_ImportQueue.front()));
            m_ImportQueue.pop();
        }
        if (!m_ImportQueue.empty()) {
            m_LastBudgetStats.DeferredMainThreadTasks += static_cast<uint64_t>(m_ImportQueue.size());
        }
    }
    if (!batch.empty()) {
        auto& js = Jobs();
        const size_t n = batch.size();
        const size_t chunk = 4; // balance enqueue overhead vs. I/O contention
        parallel_for(js, size_t(0), n, chunk, [&](size_t s, size_t c){
            for (size_t i = 0; i < c; ++i) {
                const std::string& path = batch[s + i];
                try {
                    // ImportAsset may enqueue main-thread and GPU tasks; those are thread-safe
                    ImportAsset(path);
                } catch (const std::exception& e) {
                    std::cerr << "[AssetPipeline] Import failed for \"" << path << "\": " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "[AssetPipeline] Import failed for \"" << path << "\" (unknown exception)" << std::endl;
                }
            }
        });
        m_LastBudgetStats.ImportedAssets = static_cast<uint64_t>(batch.size());
    }

    // 2. Execute scheduled lambdas (e.g., GPU safe)
    size_t executedMainThreadTasks = 0;
    const size_t taskCap = cfg.Enabled ? std::max<size_t>(1, cfg.MaxQueuedTasksPerFrame) : std::numeric_limits<size_t>::max();
    while (executedMainThreadTasks < taskCap) {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lock(m_MainThreadQueueMutex);
            if (m_MainThreadTasks.empty()) break;
            task = std::move(m_MainThreadTasks.front());
            m_MainThreadTasks.pop();
        }
        task();
        ++executedMainThreadTasks;
        if (cfg.Enabled && cfg.MaxMainThreadMs > 0.0) {
            const auto now = std::chrono::high_resolution_clock::now();
            const double elapsedMs = std::chrono::duration<double, std::milli>(now - frameStart).count();
            if (elapsedMs >= cfg.MaxMainThreadMs) {
                break;
            }
        }
    }
    m_LastBudgetStats.ExecutedMainThreadTasks = static_cast<uint64_t>(executedMainThreadTasks);
    {
        std::lock_guard<std::mutex> lock(m_MainThreadQueueMutex);
        m_LastBudgetStats.DeferredMainThreadTasks += static_cast<uint64_t>(m_MainThreadTasks.size());
    }

    // 2b. (removed) model-specific queue no longer used; callbacks go through m_MainThreadTasks

    // 3. Process GPU upload jobs
    ProcessGPUUploads();
    const auto frameEnd = std::chrono::high_resolution_clock::now();
    m_LastBudgetStats.ElapsedMs = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();

    Profiler::Get().SetCounter("AssetPipeline/ImportedAssets", m_LastBudgetStats.ImportedAssets);
    Profiler::Get().SetCounter("AssetPipeline/MainThreadTasks", m_LastBudgetStats.ExecutedMainThreadTasks);
    Profiler::Get().SetCounter("AssetPipeline/GpuUploads", m_LastBudgetStats.ExecutedGpuUploads);
    Profiler::Get().SetCounter("AssetPipeline/DeferredMainThreadTasks", m_LastBudgetStats.DeferredMainThreadTasks);
    Profiler::Get().SetCounter("AssetPipeline/DeferredGpuUploads", m_LastBudgetStats.DeferredGpuUploads);
}

void AssetPipeline::ProcessGPUUploads() {
    const MainThreadBudgetConfig cfg = m_MainThreadBudgetConfig;
    const size_t uploadCap = cfg.Enabled ? std::max<size_t>(1, cfg.MaxGpuUploadsPerFrame) : std::numeric_limits<size_t>::max();
    size_t executedUploads = 0;
    while (executedUploads < uploadCap) {
        PendingGPUUpload uploadTask;
        {
            std::lock_guard<std::mutex> lock(m_GPUQueueMutex);
            if (m_GPUUploadQueue.empty()) break;
            uploadTask = std::move(m_GPUUploadQueue.front());
            m_GPUUploadQueue.pop();
        }
        uploadTask.Upload();
        ++executedUploads;
    }
    m_LastBudgetStats.ExecutedGpuUploads = static_cast<uint64_t>(executedUploads);
    {
        std::lock_guard<std::mutex> lock(m_GPUQueueMutex);
        m_LastBudgetStats.DeferredGpuUploads = static_cast<uint64_t>(m_GPUUploadQueue.size());
    }
}

// Block until current import queue and tasks are processed (called after menu-triggered reimport)
void AssetPipeline::ProcessAllBlocking() {
    // Pump until queues are empty for this frame budget
    int safety = 10000;
    while (safety-- > 0) {
        size_t q1, q2, q3;
        {
            std::lock_guard<std::mutex> l1(m_QueueMutex);
            q1 = m_ImportQueue.size();
        }
        {
            std::lock_guard<std::mutex> l2(m_MainThreadQueueMutex);
            q2 = m_MainThreadTasks.size();
        }
        {
            std::lock_guard<std::mutex> l3(m_GPUQueueMutex);
            q3 = m_GPUUploadQueue.size();
        }
        if (q1 == 0 && q2 == 0 && q3 == 0) break;
        ProcessMainThreadTasks();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void AssetPipeline::EnqueueMainThreadTask(std::function<void()> task) {
    std::lock_guard<std::mutex> lock(m_MainThreadQueueMutex);
    m_MainThreadTasks.push(std::move(task));
}

void AssetPipeline::EnqueueGPUUpload(PendingGPUUpload&& task) {
    std::lock_guard<std::mutex> lock(m_GPUQueueMutex);
    m_GPUUploadQueue.push(std::move(task));
}

// ---------------------------------------
// IMPORT ASSET
// ---------------------------------------
void AssetPipeline::ImportAsset(const std::string& path, bool force) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string normalizedImportPath = path;
    std::replace(normalizedImportPath.begin(), normalizedImportPath.end(), '\\', '/');
    std::string metaPath = path + ".meta";

    std::string hash = ComputeFileHash(path);

    AssetMetadata meta;
    bool hasMeta = false;

    if (fs::exists(metaPath)) {
        hasMeta = TryLoadSidecarMeta(metaPath, meta);
    }

    if (!force && hasMeta && meta.hash == hash && (IsValidGuid(meta.guid) || meta.reference.IsValid())) {
        RegisterMetadataForLookup(path, meta);
        EnsurePreparedBinaryIfNeeded(path);
        return;
    }

    // Dispatch
    if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") {
        // Ensure on-disk model cache (meshbin/skelbin/meta) is up-to-date as part of import,
        // so later fast instantiation never has to build it.
        BuiltModelPaths built{};
        (void)EnsureModelCache(path, built);

        ImportModel(path);
        meta.type = "model";
    }
    else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
        // Texture source remains authoritative; GPU handles are invalidated and
        // reacquired lazily by users of the texture cache.
        meta.type = "texture";
        std::string name = fs::path(path).stem().string();
        meta.sourcePath = path;
        meta.processedPath = path;
        meta.hash = hash;
        if (meta.guid.high == 0 && meta.guid.low == 0) meta.guid = ClaymoreGUID::Generate();
        meta.reference = AssetReference(meta.guid, 0, static_cast<int32_t>(AssetType::Texture));
        AssetRegistry::Instance().SetMetadata(path, meta);
        AssetLibrary::Instance().RegisterAsset(meta.reference, AssetType::Texture, path, name);
        std::ofstream out(metaPath); if (out) { json j = meta; out << j.dump(2); }
        ImportTextureCPU(path);
        AssetEventBus::Instance().Emit(AssetEvent::Reimported, AssetType::Texture, path, meta.guid);
        return;
    }
    else if (ext == ".sc" || ext == ".glsl" || ext == ".shader") {
        ImportShader(path);
        meta.type = "shader";
        std::string name = fs::path(path).stem().string();
        meta.sourcePath = path;
        meta.processedPath = path;
        meta.hash = hash;
        if (meta.guid.high == 0 && meta.guid.low == 0) meta.guid = ClaymoreGUID::Generate();
        meta.reference = AssetReference(meta.guid, 0, static_cast<int32_t>(AssetType::Shader));
        AssetRegistry::Instance().SetMetadata(path, meta);
        AssetLibrary::Instance().RegisterAsset(meta.reference, AssetType::Shader, path, name);
        std::ofstream out(metaPath); if (out) { json j = meta; out << j.dump(2); }
        AssetEventBus::Instance().Emit(AssetEvent::Reimported, AssetType::Shader, path, meta.guid);
        return;
    }
    else if (ext == ".prefab" ||
             (ext == ".json" && normalizedImportPath.find("assets/prefabs/") != std::string::npos)) {
        // Prefab save/update: ensure metadata GUID matches author prefab GUID, register, and hot-swap
        meta.type = "prefab";
        std::string name = fs::path(path).stem().string();
        meta.sourcePath = path;
        meta.processedPath = path;
        meta.hash = hash;
        // Align meta GUID with prefab authoring GUID for stable instance tracking
        try {
            PrefabAsset author; if (PrefabIO::LoadPrefabSource(path, author)) { meta.guid = author.Guid; }
        } catch(...) {}
        if (meta.guid.high == 0 && meta.guid.low == 0) meta.guid = ClaymoreGUID::Generate();
        meta.reference = AssetReference(meta.guid, 0, static_cast<int32_t>(AssetType::Prefab));
        AssetRegistry::Instance().SetMetadata(path, meta);
        AssetLibrary::Instance().RegisterAsset(meta.reference, AssetType::Prefab, path, name);
        // Save sidecar meta for portability
        std::ofstream out(metaPath); if (out) { json j = meta; out << j.dump(2); }
        BinaryAssetCache::Instance().EnsureBinary(path);
        // Live hot-swap any instances in the active scene
        HotSwapPrefabInScene(path);
        // Emit event so other systems (like PrefabEditor) can react
        AssetEventBus::Instance().Emit(AssetEvent::Reimported, AssetType::Prefab, path, meta.guid);
        return;
    }
    else if (ext == ".ngraph") {
        // NodeGraph authoring file: register mapping and write meta; no processing required now
        meta.type = "nodegraph";
        std::string name = fs::path(path).stem().string();
        meta.sourcePath = path;
        meta.processedPath = path;
        meta.hash = hash;
        if (meta.guid.high == 0 && meta.guid.low == 0) meta.guid = ClaymoreGUID::Generate();
        meta.reference = AssetReference(meta.guid, 0, static_cast<int32_t>(AssetType::NodeGraph));
        AssetRegistry::Instance().SetMetadata(path, meta);
        AssetLibrary::Instance().RegisterAsset(meta.reference, AssetType::NodeGraph, path, name);
        std::ofstream out(metaPath); if (out) { json jj = meta; out << jj.dump(2); }
        return;
    }
    else if (ext == ".navbin") {
        // Register navbin as asset; no CPU import, but ensure library entry exists
        meta.type = "navmesh";
        std::string name = fs::path(path).stem().string();
        meta.sourcePath = path;
        meta.processedPath = path;
        meta.hash = hash;
        if (meta.guid.high == 0 && meta.guid.low == 0) meta.guid = ClaymoreGUID::Generate();
        meta.reference = AssetReference(meta.guid, 0, static_cast<int32_t>(AssetType::NavMesh));
        AssetRegistry::Instance().SetMetadata(path, meta);
        AssetLibrary::Instance().RegisterAsset(meta.reference, AssetType::NavMesh, path, name);
        // Save meta and early out
        std::ofstream out(metaPath);
        if (out) { json j = meta; out << j.dump(2); }
        return;
    }
    else if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac") {
        // Audio file: register in library, store metadata
        // Audio files are loaded on-demand by the Audio system; no preprocessing required
        meta.type = "audio";
        std::string name = fs::path(path).stem().string();
        meta.sourcePath = path;
        meta.processedPath = path;  // Audio uses source directly (miniaudio handles decoding)
        meta.hash = hash;
        if (meta.guid.high == 0 && meta.guid.low == 0) meta.guid = ClaymoreGUID::Generate();
        meta.reference = AssetReference(meta.guid, 0, static_cast<int32_t>(AssetType::Audio));
        AssetRegistry::Instance().SetMetadata(path, meta);
        AssetLibrary::Instance().RegisterAsset(meta.reference, AssetType::Audio, path, name);
        // Save meta
        std::ofstream out(metaPath);
        if (out) { json j = meta; out << j.dump(2); }
        // Emit event for asset browser refresh
        AssetEventBus::Instance().Emit(AssetEvent::Reimported, AssetType::Audio, path, meta.guid);
        return;
    }
    else if (ext == ".mat") {
        ImportMaterial(path);
        meta.type = "material";
        std::string name = fs::path(path).stem().string();
        meta.sourcePath = path;
        meta.processedPath = path;
        meta.hash = hash;
        if (meta.guid.high == 0 && meta.guid.low == 0) meta.guid = ClaymoreGUID::Generate();
        meta.reference = AssetReference(meta.guid, 0, static_cast<int32_t>(AssetType::Material));
        AssetRegistry::Instance().SetMetadata(path, meta);
        AssetLibrary::Instance().RegisterAsset(meta.reference, AssetType::Material, path, name);
        BinaryAssetCache::Instance().EnsureBinary(path);
        MaterialAssetCache::Invalidate(path);
        std::ofstream out(metaPath); if (out) { json j = meta; out << j.dump(2); }
        AssetEventBus::Instance().Emit(AssetEvent::Reimported, AssetType::Material, path, meta.guid);
        return;
    }
    else if (ext == ".anim") {
        meta.type = "animation";
        std::string name = fs::path(path).stem().string();
        meta.sourcePath = path;
        meta.processedPath = path;
        meta.hash = hash;
        if (meta.guid.high == 0 && meta.guid.low == 0) meta.guid = ClaymoreGUID::Generate();
        meta.reference = AssetReference(meta.guid, 0, static_cast<int32_t>(AssetType::Animation));
        AssetRegistry::Instance().SetMetadata(path, meta);
        AssetLibrary::Instance().RegisterAsset(meta.reference, AssetType::Animation, path, name);
        std::ofstream out(metaPath); if (out) { json j = meta; out << j.dump(2); }
        cm::animation::InvalidateAnimationAssetCache(path);
        Scene::Get().InvalidateAllAnimatorAssetCaches();
        BinaryAssetCache::Instance().EnsureBinary(path);
        AssetEventBus::Instance().Emit(AssetEvent::Reimported, AssetType::Animation, path, meta.guid);
        return;
    }
    else if (ext == ".animctrl" || ext == ".controller") {
        meta.type = "animatorcontroller";
        std::string name = fs::path(path).stem().string();
        meta.sourcePath = path;
        meta.processedPath = path;
        meta.hash = hash;
        if (meta.guid.high == 0 && meta.guid.low == 0) meta.guid = ClaymoreGUID::Generate();
        meta.reference = AssetReference(meta.guid, 0, static_cast<int32_t>(AssetType::AnimatorController));
        AssetRegistry::Instance().SetMetadata(path, meta);
        AssetLibrary::Instance().RegisterAsset(meta.reference, AssetType::AnimatorController, path, name);
        std::ofstream out(metaPath); if (out) { json j = meta; out << j.dump(2); }
        cm::animation::InvalidateAnimatorControllerCache(path);
        Scene::Get().InvalidateAllAnimatorAssetCaches();
        BinaryAssetCache::Instance().EnsureBinary(path);
        AssetEventBus::Instance().Emit(AssetEvent::Reimported, AssetType::AnimatorController, path, meta.guid);
        return;
    }
    else if (ext == ".asset") {
        // ScriptableObject data file: just register GUID and file as-is
        meta.type = "scriptable";
        std::string name = fs::path(path).stem().string();
        meta.sourcePath = path;
        meta.processedPath = path;
        meta.hash = hash;
        if (meta.guid.high == 0 && meta.guid.low == 0) meta.guid = ClaymoreGUID::Generate();
        meta.reference = AssetReference(meta.guid, 0, static_cast<int32_t>(AssetType::Scriptable));
        AssetRegistry::Instance().SetMetadata(path, meta);
        AssetLibrary::Instance().RegisterAsset(meta.reference, AssetType::Scriptable, path, name);
        std::ofstream out(metaPath); if (out) { json jj = meta; out << jj.dump(2); }
        return;
    }
    else if (ext == ".clayobj") {
        // ClayScriptableObject data file: register GUID from the JSON file
        meta.type = "scriptable";
        std::string name = fs::path(path).stem().string();
        meta.sourcePath = path;
        meta.processedPath = path;
        meta.hash = hash;
        
        // Read GUID from inside the .clayobj JSON file
        try {
            std::ifstream clayFile(path);
            if (clayFile.is_open()) {
                json clayJson;
                clayFile >> clayJson;
                if (clayJson.contains("guid") && clayJson["guid"].is_string()) {
                    meta.guid = ClaymoreGUID::FromString(clayJson["guid"].get<std::string>());
                }
            }
        } catch (...) {
            std::cerr << "[AssetPipeline] Failed to read GUID from .clayobj: " << path << std::endl;
        }
        
        // Generate GUID if not present in file
        if (meta.guid.high == 0 && meta.guid.low == 0) meta.guid = ClaymoreGUID::Generate();
        
        meta.reference = AssetReference(meta.guid, 0, static_cast<int32_t>(AssetType::Scriptable));
        AssetRegistry::Instance().SetMetadata(path, meta);
        AssetLibrary::Instance().RegisterAsset(meta.reference, AssetType::Scriptable, path, name);
        
        // Also register both absolute and relative path variants for lookup
        std::string vpath = path;
        std::replace(vpath.begin(), vpath.end(), '\\', '/');
        size_t pos = vpath.find("assets/");
        if (pos != std::string::npos) {
            std::string relPath = vpath.substr(pos);
            AssetLibrary::Instance().RegisterPathAlias(meta.guid, relPath);
        }
        
        std::ofstream out(metaPath); if (out) { json jj = meta; out << jj.dump(2); }
        std::cout << "[AssetPipeline] Registered .clayobj: " << path << " (GUID: " << meta.guid.ToString() << ")" << std::endl;
        return;
    }
    else if (ext == ".cs") {
        ImportScript(path);
        meta.type = "script";
        std::string name = fs::path(path).stem().string();
        meta.sourcePath = path;
        meta.processedPath = path;
        meta.hash = hash;
        if (meta.guid.high == 0 && meta.guid.low == 0) {
            meta.guid = ClaymoreGUID::Generate();
        }
        meta.reference = AssetReference(meta.guid, 0, static_cast<int32_t>(AssetType::Script));
        AssetRegistry::Instance().SetMetadata(path, meta);
        const std::string libPath = MakeProjectRelativeVPath(path);
        AssetLibrary::Instance().RegisterAsset(meta.reference, AssetType::Script, libPath, name);
        std::string absNorm = path;
        std::replace(absNorm.begin(), absNorm.end(), '\\', '/');
        AssetLibrary::Instance().RegisterPathAlias(meta.guid, absNorm);
        std::ofstream outCs(metaPath);
        if (outCs) {
            json j = meta;
            outCs << j.dump(2);
        }
        std::cout << "[AssetPipeline] Imported: " << path << " (GUID: " << meta.guid.ToString() << ")" << std::endl;
        return;
    }
    else if (ext == ".ttf" || ext == ".otf") {
        // Fonts: just register asset and write meta; no GPU work here
        meta.type = "font";
        std::string name = fs::path(path).stem().string();
        meta.sourcePath = path;
        meta.processedPath = path;
        meta.hash = hash;
        if (meta.guid.high == 0 && meta.guid.low == 0) meta.guid = ClaymoreGUID::Generate();
        meta.reference = AssetReference(meta.guid, 0, static_cast<int32_t>(AssetType::Font));
        AssetRegistry::Instance().SetMetadata(path, meta);
        AssetLibrary::Instance().RegisterAsset(meta.reference, AssetType::Font, path, name);
        // Save meta and early out
        std::ofstream out(metaPath);
        if (out) { json j = meta; out << j.dump(2); }
        return;
    }
    else if (ext == ".dlglib") {
        // Dialogue library files: read GUID from JSON and register
        meta.type = "dialogue";
        std::string name = fs::path(path).stem().string();
        meta.sourcePath = path;
        meta.processedPath = path;
        meta.hash = hash;
        bool hasGuidInFile = false;
        nlohmann::json dlgJson;

        // Read GUID from inside the .dlglib JSON file
        try {
            std::ifstream dlgFile(path);
            if (dlgFile.is_open()) {
                dlgFile >> dlgJson;
                if (dlgJson.contains("guid") && dlgJson["guid"].is_string()) {
                    meta.guid = ClaymoreGUID::FromString(dlgJson["guid"].get<std::string>());
                    hasGuidInFile = true;
                }
            }
        } catch (...) {
            std::cerr << "[AssetPipeline] Failed to read GUID from .dlglib: " << path << std::endl;
        }
        
        // Generate GUID if not present in file
        if (meta.guid.high == 0 && meta.guid.low == 0) meta.guid = ClaymoreGUID::Generate();

        // If the file lacked a GUID, persist the generated one into the file to keep it stable.
        if (!hasGuidInFile) {
            try {
                dlgJson["guid"] = meta.guid.ToString();
                std::ofstream outDlg(path, std::ios::trunc);
                if (outDlg.is_open()) {
                    outDlg << dlgJson.dump(2);
                }
            } catch (...) {
                std::cerr << "[AssetPipeline] Failed to write GUID into .dlglib: " << path << std::endl;
            }
        }
        
        meta.reference = AssetReference(meta.guid, 0, static_cast<int32_t>(AssetType::Scriptable)); // Use Scriptable type for now
        AssetRegistry::Instance().SetMetadata(path, meta);
        AssetLibrary::Instance().RegisterAsset(meta.reference, AssetType::Scriptable, path, name);
        
        // Also register both absolute and relative path variants for lookup
        std::string vpath = path;
        std::replace(vpath.begin(), vpath.end(), '\\', '/');
        size_t pos = vpath.find("assets/");
        if (pos != std::string::npos) {
            std::string relPath = vpath.substr(pos);
            AssetLibrary::Instance().RegisterPathAlias(meta.guid, relPath);
        }
        
        std::ofstream out(metaPath); if (out) { json jj = meta; out << jj.dump(2); }
        std::cout << "[AssetPipeline] Registered .dlglib: " << path << " (GUID: " << meta.guid.ToString() << ")" << std::endl;
        return;
    }
    else {
        return;
    }

    meta.sourcePath = path;
    meta.processedPath = "cache/" + fs::path(path).filename().string();
    meta.hash = hash;
    meta.lastImported = GetCurrentTimestamp();
    
            // Generate GUID and asset reference if not already present
        if (meta.guid.high == 0 && meta.guid.low == 0) {
            meta.guid = ClaymoreGUID::Generate();
            meta.reference = AssetReference(meta.guid, 0, static_cast<int32_t>(AssetType::Mesh));
        }

    json j = meta;
    std::ofstream out(metaPath);
    out << j.dump(4);

    AssetRegistry::Instance().SetMetadata(path, meta);
    
    // Register asset in AssetLibrary
    AssetLibrary::Instance().RegisterAsset(meta.reference,
        static_cast<AssetType>(meta.reference.type),
        path,
        fs::path(path).filename().string());

    std::cout << "[AssetPipeline] Imported: " << path << " (GUID: " << meta.guid.ToString() << ")" << std::endl;
}
void AssetPipeline::HotSwapPrefabInScene(const std::string& prefabPath) {
    // Prefer GUID-based linking for prefab instances; fallback to PrefabSource
    Scene& scene = Scene::Get();

    // Determine prefab GUID from metadata or authoring JSON
    ClaymoreGUID prefabGuid{};
    try {
        const AssetMetadata* meta = AssetRegistry::Instance().GetMetadata(prefabPath);
        if (meta) prefabGuid = meta->guid;
    } catch(...) {}
    if (prefabGuid.high == 0 && prefabGuid.low == 0) {
        // Try parse authoring prefab JSON
        try {
            PrefabAsset author; if (PrefabIO::LoadPrefabSource(prefabPath, author)) prefabGuid = author.Guid;
        } catch(...) {}
    }

    // Collect prefab root entities by matching PrefabGuid (robust) or PrefabSource (fallback)
    std::vector<EntityID> roots;
    // Normalize path and treat .json/.prefab as interchangeable authoring references for PrefabSource matching
    std::string v = prefabPath; for (char& c : v) if (c=='\\') c='/'; size_t pos = v.find("assets/"); if (pos != std::string::npos) v = v.substr(pos);
    auto normalizeAuthoringPath = [](std::string s) {
        for (char& c : s) if (c=='\\') c='/';
        // If extension is .json under assets/prefabs, also consider .prefab equivalent and vice versa
        std::string alt = s;
        std::string ext = fs::path(s).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".json") { alt = fs::path(s).replace_extension(".prefab").string(); }
        else if (ext == ".prefab") { alt = fs::path(s).replace_extension(".json").string(); }
        for (char& c : alt) if (c=='\\') c='/';
        return std::pair<std::string,std::string>{s, alt};
    };
    auto vpair = normalizeAuthoringPath(v);
    for (const auto& e : scene.GetEntities()) {
        auto* d = scene.GetEntityData(e.GetID()); if (!d) continue;
        bool match = false;
        if (!(prefabGuid.high == 0 && prefabGuid.low == 0) && !(d->PrefabGuid.high == 0 && d->PrefabGuid.low == 0)) {
            match = (d->PrefabGuid.high == prefabGuid.high && d->PrefabGuid.low == prefabGuid.low);
        }
        if (!match && !d->PrefabSource.empty()) {
            std::string src = d->PrefabSource; for (char& c : src) if (c=='\\') c='/';
            // Match against either form (.prefab or .json) under assets/prefabs
            match = (src == vpair.first || src == vpair.second);
        }
        if (match) roots.push_back(e.GetID());
    }
    if (roots.empty()) return;

    for (EntityID root : roots) {
        auto* rd = scene.GetEntityData(root); if (!rd) continue;
        // Preserve placement and parent
        TransformComponent savedXf = rd->Transform;
        EntityID parent = rd->Parent;
        std::string name = rd->Name;

        // Collect GUID-anchored overrides by diffing live subtree against base prefab authoring JSON
        PrefabAsset base; 
        std::vector<prefab::PropertyOverride> ov;
        bool haveBase = false;
        try { if (PrefabIO::LoadPrefabSource(prefabPath, base)) haveBase = true; } catch(...) {}
        // Build set of GUIDs present in the live instance prior to swap
        std::unordered_set<uint64_t> liveGuids;
        auto pack = [](const ClaymoreGUID& g)->uint64_t { return PackGuidHash(g); };
        {
            std::function<void(EntityID)> dfs = [&](EntityID id){ auto* d = scene.GetEntityData(id); if (!d) return; liveGuids.insert(pack(d->EntityGuid)); for (EntityID c : d->Children) dfs(c); };
            dfs(root);
        }
        if (haveBase) {
            ov = prefab_editor::ComputeOverrides(base, scene, root);
            // With unified PropertyOverride system, overrides are already GUID-based
            // No filtering needed as we don't use path-based "removeEntity" ops anymore
        }

        // Remove old instance
        scene.RemoveEntity(root);

        // Re-instantiate from prefab path (support both .prefab and authoring .json)
        EntityID nid = (EntityID)-1;
        std::string ext = fs::path(prefabPath).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (BinaryAssetCache::GetAssetType(prefabPath) == BinaryAssetCache::AssetType::Prefab &&
            BinaryAssetCache::Instance().EnsureBinary(prefabPath)) {
            const std::string binaryPath = BinaryAssetCache::Instance().GetBinaryPath(prefabPath);
            if (!binaryPath.empty()) {
                runtime::RuntimePrefabInstantiator::Preload(binaryPath);
                nid = runtime::RuntimePrefabInstantiator::InstantiateBlocking(binaryPath, scene);
            }
        }
        if ((nid == (EntityID)-1 || nid == (EntityID)0) && (ext == ".prefab" || ext == ".json")) {
            nid = InstantiatePrefabFromPath(prefabPath, scene);
        }
        if (nid == (EntityID)-1 || nid == (EntityID)0) continue;
        if (auto* nd = scene.GetEntityData(nid)) {
            nd->Name = name;
            nd->Transform = savedXf;
            nd->Transform.TransformDirty = true;
            try { std::string v2 = v; for (char& c : v2) if (c=='\\') c='/'; nd->PrefabSource = v2; } catch(...) {}
        }
        if (parent != (EntityID)-1) scene.SetParent(nid, parent);
        scene.MarkTransformDirty(nid);

        // Apply property overrides with unified system
        if (!ov.empty()) {
            std::cout << "[AssetPipeline] Applying " << ov.size() << " overrides to re-instantiated prefab\n";
            ApplyPrefabOverrides(nid, scene, ov);
        }
        scene.UpdateTransforms();
    }
}

void AssetPipeline::ImportMaterial(const std::string& path) {
    EnqueueMainThreadTask([path]() {
        std::cout << "[AssetPipeline] Material reloaded: " << path << std::endl;
        // Future: notify inspector/renderer of material changes
    });
}



void AssetPipeline::ImportScript(const std::string& path, bool forceRebuild)
{
   std::lock_guard<std::mutex> compileLock(g_ScriptCompileMutex);

   const fs::path projectRoot = Project::GetProjectDirectory();
   if (!projectRoot.empty() && fs::exists(projectRoot)) {
       const fs::path scriptsDllPath = projectRoot / ".library" / "GameScripts.dll";
       if (!forceRebuild && IsGameScriptsDllUpToDate(projectRoot, scriptsDllPath)) {
           SetScriptsCompiled(true);
           return;
       }
   }

   std::wstring compilerExe = (std::filesystem::current_path() / "tools" / "ScriptCompiler.exe").wstring();

   // Get executable directory (for managed engine dll)
   wchar_t exePath[MAX_PATH];
   GetModuleFileNameW(NULL, exePath, MAX_PATH);
   std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();

   // Build output DLL path under the active project: <Project>/.library/GameScripts.dll
   std::filesystem::path projDir = Project::GetProjectDirectory();
   std::filesystem::path projLibDir = projDir / ".library";
   std::error_code ecMk; std::filesystem::create_directories(projLibDir, ecMk);
   std::filesystem::path gameScriptsDll = projLibDir / "GameScripts.dll";
   std::filesystem::path engineDll = exeDir / "ClaymoreEngine.dll";

   // Use Project path
   std::wstring projectPath = std::filesystem::path(Project::GetProjectDirectory()).wstring();

   // Find module DLLs in the project
   std::vector<std::filesystem::path> moduleDlls;
   std::filesystem::path modulesDir = projDir / "modules";
   if (std::filesystem::exists(modulesDir)) {
       for (const auto& entry : std::filesystem::recursive_directory_iterator(modulesDir)) {
           if (entry.is_regular_file() && entry.path().extension() == ".dll") {
               moduleDlls.push_back(entry.path());
               std::cout << "[Roslyn] Found module DLL: " << entry.path().string() << std::endl;
           }
       }
   }
   
   if (moduleDlls.empty()) {
       std::cout << "[Roslyn] No module DLLs found in " << modulesDir.string() << std::endl;
   } else {
       std::cout << "[Roslyn] Found " << moduleDlls.size() << " module DLL(s) for script compilation" << std::endl;
   }

   // Quote all paths and build the args string
   std::wstring args = L"\"" + projectPath + L"\" "
       L"\"" + gameScriptsDll.wstring() + L"\" "
       L"\"" + engineDll.wstring() + L"\"";

   if (Project::GetManagedScriptDebuggingEnabled()) {
       args += L" --managed-debugging";
       std::cout << "[Roslyn] Managed script debugging is enabled; emitting symbols and debug-friendly script assemblies.\n";
   }

   // Add module DLLs as references
   for (const auto& moduleDll : moduleDlls) {
       args += L" \"" + moduleDll.wstring() + L"\"";
   }

   std::wstring commandLine = L"\"" + compilerExe + L"\" " + args;

   STARTUPINFOW si = { sizeof(si) };
   PROCESS_INFORMATION pi;

   BOOL success = CreateProcessW(
      NULL,
      &commandLine[0],  // mutable buffer
      NULL, NULL,
      FALSE,
      0, NULL, NULL,
      &si, &pi
   );

   if (success)
      {
      WaitForSingleObject(pi.hProcess, INFINITE);
      DWORD exitCode;
      GetExitCodeProcess(pi.hProcess, &exitCode);
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);

      if (exitCode == 0) {
         std::cout << "[Roslyn] Successfully compiled C# scripts.\n";
         SetScriptsCompiled(true);
         // Only attempt reload if the .NET runtime is ready; otherwise the engine init will load scripts
         if (IsDotnetRuntimeReady()) {
            // Reload scripts and re-apply OnValidate on the main thread to avoid
            // running script lifecycle callbacks from the asset worker thread.
            EnqueueMainThreadTask([]() {
               ReloadScripts();
               CallOnValidateForAllScripts();
            });
         }
         }
      else {
         std::cerr << "[Roslyn] Compilation failed.\n";
         Logger::LogError("[Roslyn] Script compilation failed. Check errors above.");
         SetScriptsCompiled(false);
         }
      }
   else {
      std::cerr << "[Roslyn] Failed to launch ScriptCompiler.exe\n";
      Logger::LogError("[Roslyn] Failed to launch ScriptCompiler.exe");
      SetScriptsCompiled(false);
      }

   }



// ---------------------------------------
// MODEL IMPORT (GPU-safe queued)
// ---------------------------------------
void AssetPipeline::ImportModel(const std::string& path) {
    EnqueueMainThreadTask([path]() {
        // Invalidate any cached mesh data for this model before reimporting
        AssetLibrary::Instance().InvalidateMesh(path);
        
        auto model = ModelLoader::LoadModel(path);
        std::cout << "[AssetPipeline] Model uploaded to GPU: " << path << std::endl;

        // --------- Auto-generate humanoid avatar (heuristic) ---------
        // Use the runtime scene loader path to build skeleton bind data, then export an .avatar file next to the model.
        if (!model.BoneNames.empty() && !model.InverseBindPoses.empty()) {
            try {
                // Build a transient skeleton with parents using Assimp hierarchy for accurate bind locals
                Assimp::Importer aimport;
                const aiScene* aScene = aimport.ReadFile(path,
                    aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace |
                    aiProcess_FlipWindingOrder | aiProcess_FlipUVs | aiProcess_JoinIdenticalVertices |
                    aiProcess_ImproveCacheLocality | aiProcess_LimitBoneWeights);
                SkeletonComponent tempSkel;
                tempSkel.InverseBindPoses = model.InverseBindPoses;
                tempSkel.BoneParents.resize(model.BoneNames.size(), -1);
                for (int i = 0; i < (int)model.BoneNames.size(); ++i) tempSkel.BoneNameToIndex[model.BoneNames[i]] = i;

                if (aScene && aScene->mRootNode) {
                    std::unordered_map<std::string, aiNode*> nodeByName;
                    std::function<void(aiNode*)> gather = [&](aiNode* n){ nodeByName[n->mName.C_Str()] = n; for (unsigned c=0;c<n->mNumChildren;++c) gather(n->mChildren[c]); };
                    gather(aScene->mRootNode);
                    // Build name->index map
                    std::unordered_map<std::string, int> boneIndexMap;
                    for (int i = 0; i < (int)model.BoneNames.size(); ++i) boneIndexMap[model.BoneNames[i]] = i;
                    for (size_t i = 0; i < model.BoneNames.size(); ++i) {
                        auto itNode = nodeByName.find(model.BoneNames[i]);
                        if (itNode != nodeByName.end()) {
                            aiNode* p = itNode->second->mParent;
                            while (p) {
                                auto itBI = boneIndexMap.find(p->mName.C_Str());
                                if (itBI != boneIndexMap.end()) { tempSkel.BoneParents[i] = itBI->second; break; }
                                p = p->mParent;
                            }
                        }
                    }
                }


                cm::animation::AvatarDefinition avatar;
                avatar.RigName = std::filesystem::path(path).stem().string();
                cm::animation::avatar_builders::BuildFromSkeleton(tempSkel, avatar, true);
                // Save next to the model
                std::filesystem::path p(path);
                std::string avatarPath = (p.parent_path() / (p.stem().string() + ".avatar")).string();
                cm::animation::SaveAvatar(avatar, avatarPath);
                std::cout << "[AssetPipeline] Wrote avatar: " << avatarPath << std::endl;
            } catch(...) {
                // Non-fatal
            }
        }

        // --------- Capture textures (including embedded) into assets/textures/<model>/ and register ---------
        try {
            fs::path src(path);
            std::string modelName = src.stem().string();
            fs::path proj = Project::GetProjectDirectory();
            fs::path texRoot = proj / "assets" / "textures" / modelName;
            std::error_code ec; fs::create_directories(texRoot, ec);

            Assimp::Importer aimport2;
            const aiScene* s2 = aimport2.ReadFile(path, aiProcess_Triangulate | aiProcess_GenNormals);
            if (s2 && s2->HasMaterials()) {
                auto isSafeTextureExtension = [](std::string ext) {
                    if (ext.size() < 2 || ext[0] != '.') return false;
                    std::transform(ext.begin() + 1, ext.end(), ext.begin() + 1,
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    for (size_t i = 1; i < ext.size(); ++i) {
                        if (!std::isalnum(static_cast<unsigned char>(ext[i]))) return false;
                    }
                    return true;
                };
                auto chooseEmbeddedLeaf = [&](const aiTexture* at,
                                              const std::string& defaultBaseName,
                                              const std::string& fallbackExtension) {
                    fs::path outputLeaf = fs::path(defaultBaseName + fallbackExtension);
                    if (at) {
                        fs::path hintedLeaf = fs::path(at->mFilename.C_Str()).filename();
                        if (!hintedLeaf.empty()) {
                            outputLeaf = hintedLeaf;
                        }
                    }

                    std::string leafExt = outputLeaf.extension().string();
                    std::transform(leafExt.begin(), leafExt.end(), leafExt.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (!isSafeTextureExtension(leafExt) || leafExt != fallbackExtension) {
                        outputLeaf.replace_extension(fallbackExtension);
                    }
                    return outputLeaf;
                };
                for (unsigned mi = 0; mi < s2->mNumMaterials; ++mi) {
                    const aiMaterial* aim = s2->mMaterials[mi];

                    // Gather all texture paths across many stacks and indices (robust for various exporters)
                    std::vector<std::string> texPaths;
                    auto maybeAdd = [&](const std::string& p){ if (!p.empty() && std::find(texPaths.begin(), texPaths.end(), p) == texPaths.end()) texPaths.push_back(p); };
                    auto gatherType = [&](aiTextureType t){
                        const unsigned count = aim->GetTextureCount(t);
                        for (unsigned ti = 0; ti < count; ++ti) { aiString s; if (aim->GetTexture(t, ti, &s) == AI_SUCCESS) maybeAdd(std::string(s.C_Str())); }
                    };

                    // Common and extended stacks
                    gatherType(aiTextureType_BASE_COLOR);
                    gatherType(aiTextureType_DIFFUSE);
                    gatherType(aiTextureType_SPECULAR);
                    gatherType(aiTextureType_AMBIENT);
                    gatherType(aiTextureType_EMISSIVE);
                    gatherType(aiTextureType_NORMAL_CAMERA);
                    gatherType(aiTextureType_NORMALS);
                    gatherType(aiTextureType_HEIGHT);
                    gatherType(aiTextureType_SHININESS);
                    gatherType(aiTextureType_OPACITY);
                    gatherType(aiTextureType_DISPLACEMENT);
                    gatherType(aiTextureType_LIGHTMAP);
                    gatherType(aiTextureType_REFLECTION);
                    gatherType(aiTextureType_METALNESS);
                    gatherType(aiTextureType_DIFFUSE_ROUGHNESS);
                    gatherType(aiTextureType_AMBIENT_OCCLUSION);
                    gatherType(aiTextureType_CLEARCOAT);
                    gatherType(aiTextureType_SHEEN);
                    gatherType(aiTextureType_TRANSMISSION);
                    gatherType(aiTextureType_UNKNOWN);

                    for (const auto& tpath : texPaths) {
                        if (tpath.empty()) continue;
                        if (tpath[0] == '*') {
                            // Embedded: extract from aiTexture array
                            int idx = 0; try { idx = std::stoi(tpath.substr(1)); } catch(...) { continue; }
                            if ((unsigned)idx >= s2->mNumTextures) continue;
                            const aiTexture* at = s2->mTextures[idx]; if (!at) continue;
                            // Choose filename based on hint
                            std::string baseName = std::string("emb_") + std::to_string(mi) + "_" + std::to_string(idx);
                            std::string fallbackExt = ".png";
                            if (at->mHeight == 0 && at->achFormatHint[0] != '\0') {
                                // Use format hint (e.g., jpg, png)
                                fallbackExt = "." + std::string(at->achFormatHint);
                                std::transform(fallbackExt.begin(), fallbackExt.end(), fallbackExt.begin(),
                                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            }
                            fs::path pdst = texRoot / chooseEmbeddedLeaf(at, baseName, fallbackExt);
                            if (at->mHeight == 0) {
                                // Compressed blob of size mWidth
                                std::ofstream of(pdst, std::ios::binary | std::ios::trunc);
                                if (of.is_open()) {
                                    of.write(reinterpret_cast<const char*>(at->pcData), (std::streamsize)at->mWidth);
                                }
                            } else {
                                // Raw ARGB8888 → write simple TGA
                                const int w = (int)at->mWidth, h = (int)at->mHeight;
                                std::vector<uint8_t> out((size_t)w * (size_t)h * 4u);
                                for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
                                    const aiTexel& t = at->pcData[y*w + x]; size_t o = ((size_t)y*(size_t)w + (size_t)x)*4u;
                                    out[o+0]=t.r; out[o+1]=t.g; out[o+2]=t.b; out[o+3]=t.a;
                                }
                                // Write uncompressed TGA header + data
                                fs::path tga = texRoot / chooseEmbeddedLeaf(at, baseName, ".tga");
                                std::ofstream of(tga, std::ios::binary | std::ios::trunc);
                                if (of.is_open()) {
                                    uint8_t header[18]{}; header[2]=2; // uncompressed true-color
                                    header[12] = (uint8_t)(w & 0xFF); header[13] = (uint8_t)((w>>8)&0xFF);
                                    header[14] = (uint8_t)(h & 0xFF); header[15] = (uint8_t)((h>>8)&0xFF);
                                    header[16] = 32; // 32 bpp
                                    of.write((char*)header, 18);
                                    // TGA expects BGRA; convert
                                    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
                                        size_t o = ((size_t)y*(size_t)w + (size_t)x)*4u;
                                        uint8_t bgra[4] = { out[o+2], out[o+1], out[o+0], out[o+3] };
                                        of.write((char*)bgra, 4);
                                    }
                                }
                                pdst = tga;
                            }
                            std::string vpath = pdst.string(); std::replace(vpath.begin(), vpath.end(), '\\', '/');
                            size_t pos = vpath.find("assets/"); if (pos != std::string::npos) vpath = vpath.substr(pos);
                            AssetMetadata tmeta; tmeta.guid = ClaymoreGUID::Generate(); tmeta.type = "texture"; tmeta.sourcePath = vpath; tmeta.processedPath = vpath;
                            nlohmann::json tj = tmeta; std::ofstream outm((pdst.string()+".meta").c_str()); if (outm) outm << tj.dump(4);
                            AssetLibrary::Instance().RegisterAsset(AssetReference(tmeta.guid, 0, (int)AssetType::Texture), AssetType::Texture, vpath, pdst.filename().string());
                            std::cout << "[Import] Extracted embedded texture to: " << pdst << std::endl;
                            continue;
                        }

                        fs::path psrc = tpath;
                        if (!fs::exists(psrc)) psrc = src.parent_path() / tpath;
                        if (!fs::exists(psrc)) continue;
                        fs::path pdst = texRoot / psrc.filename();
                        fs::copy_file(psrc, pdst, fs::copy_options::overwrite_existing, ec);
                        std::string vpath = pdst.string(); std::replace(vpath.begin(), vpath.end(), '\\', '/');
                        size_t pos = vpath.find("assets/"); if (pos != std::string::npos) vpath = vpath.substr(pos);
                        AssetMetadata tmeta; tmeta.guid = ClaymoreGUID::Generate(); tmeta.type = "texture"; tmeta.sourcePath = vpath; tmeta.processedPath = vpath;
                        nlohmann::json tj = tmeta; std::ofstream outm((pdst.string()+".meta").c_str()); if (outm) outm << tj.dump(4);
                        AssetLibrary::Instance().RegisterAsset(AssetReference(tmeta.guid, 0, (int)AssetType::Texture), AssetType::Texture, vpath, pdst.filename().string());
                        std::cout << "[Import] Copied texture to: " << pdst << std::endl;
                    }
                }
            }
        } catch(...) { std::cerr << "[FBXImport] Texture capture step failed: " << path << std::endl; }

        // --------- Failsafe: grep PNG file paths referenced in FBX and copy any missing ---------
        try {
            fs::path src(path);
            std::string modelName = src.stem().string();
            fs::path proj = Project::GetProjectDirectory();
            fs::path texRoot = proj / "assets" / "textures" / modelName;
            std::error_code ec; fs::create_directories(texRoot, ec);

            std::ifstream fin(path, std::ios::binary);
            if (fin) {
                std::vector<char> buf((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());

                auto isAllowed = [](char c) -> bool {
                    unsigned char uc = static_cast<unsigned char>(c);
                    return std::isalnum(uc) || c=='_' || c=='-' || c=='.' || c=='/' || c=='\\';
                };

                std::vector<std::string> candidates;
                auto addUnique = [&](const std::string& s){ if (!s.empty() && std::find(candidates.begin(), candidates.end(), s)==candidates.end()) candidates.push_back(s); };

                for (size_t i = 0; i + 4 <= buf.size(); ++i) {
                    char c0 = buf[i+0];
                    char c1 = (i+1<buf.size()?buf[i+1]:0);
                    char c2 = (i+2<buf.size()?buf[i+2]:0);
                    char c3 = (i+3<buf.size()?buf[i+3]:0);
                    if (c0=='.' && (c1=='p'||c1=='P') && (c2=='n'||c2=='N') && (c3=='g'||c3=='G')) {
                        // Expand backwards to include path/filename
                        size_t start = i;
                        while (start>0 && isAllowed(buf[start-1])) --start;
                        // Expand forwards to include any trailing path parts (unlikely after extension, but safe)
                        size_t end = i+4;
                        while (end<buf.size() && isAllowed(buf[end])) ++end;
                        if (end>start) {
                            std::string token(buf.data()+start, buf.data()+end);
                            // Normalize separators to the local FS style
                            for (char& ch : token) if (ch=='\\') ch = '\\';
                            addUnique(token);
                        }
                    }
                }

                for (const auto& tpath : candidates) {
                    // Skip embedded markers (e.g., "*0")
                    if (!tpath.empty() && tpath[0]=='*') continue;

                    fs::path psrc = tpath;
                    if (!fs::exists(psrc)) psrc = src.parent_path() / tpath;
                    if (!fs::exists(psrc)) psrc = src.parent_path() / fs::path(tpath).filename();
                    if (!fs::exists(psrc)) continue;

                    fs::path pdst = texRoot / psrc.filename();
                    if (!fs::exists(pdst)) {
                        fs::copy_file(psrc, pdst, fs::copy_options::overwrite_existing, ec);
                        std::string vpath = pdst.string(); std::replace(vpath.begin(), vpath.end(), '\\', '/');
                        size_t pos = vpath.find("assets/"); if (pos != std::string::npos) vpath = vpath.substr(pos);
                        AssetMetadata tmeta; tmeta.guid = ClaymoreGUID::Generate(); tmeta.type = "texture"; tmeta.sourcePath = vpath; tmeta.processedPath = vpath;
                        nlohmann::json tj = tmeta; std::ofstream outm((pdst.string()+".meta").c_str()); if (outm) outm << tj.dump(4);
                        AssetLibrary::Instance().RegisterAsset(AssetReference(tmeta.guid, 0, (int)AssetType::Texture), AssetType::Texture, vpath, pdst.filename().string());
                        std::cout << "[FBXImport] Grep-copied PNG to: " << pdst << std::endl;
                    }
                }
            }
        } catch(...) { std::cerr << "[FBXImport] PNG grep step failed: " << path << std::endl; }

        // --------- Deduplicate extracted textures into shared pool ---------
        try {
            texture_cleanup::ImportDedupReport dedup;
            if (texture_cleanup::DeduplicateImportedModelTextures(path, &dedup)) {
                if (dedup.texturesShared > 0 || dedup.texturesRemoved > 0) {
                    std::cout << "[Import] Texture dedup: shared " << dedup.texturesShared
                              << ", removed " << dedup.texturesRemoved << std::endl;
                }
            }
        } catch(...) {
            std::cerr << "[Import] Texture dedup step failed: " << path << std::endl;
        }

        // --------- Extract animations (unified .anim as primary) ---------
        using namespace cm::animation;
        auto clips = AnimationImporter::ImportFromModel(path);
        std::cout << "[AssetPipeline] ImportFromModel found " << clips.size() << " animation(s)." << std::endl;
        if (!clips.empty()) {
            bool anyAnimationClipChanged = false;
            std::filesystem::path p(path);
            std::string dir = p.parent_path().string();
            SkeletonComponent importSkeleton;
            if (!model.BoneNames.empty() && !model.InverseBindPoses.empty()) {
                importSkeleton.InverseBindPoses = model.InverseBindPoses;
                importSkeleton.BoneParents = model.BoneParents;
                importSkeleton.BoneNames = model.BoneNames;
                if (importSkeleton.BoneParents.size() < importSkeleton.BoneNames.size()) {
                    importSkeleton.BoneParents.resize(importSkeleton.BoneNames.size(), -1);
                }
                for (int i = 0; i < (int)importSkeleton.BoneNames.size(); ++i) {
                    importSkeleton.BoneNameToIndex[importSkeleton.BoneNames[i]] = i;
                }
            }
            std::unordered_map<std::string, glm::mat4> sourceRestLocalTransforms;
            {
                Assimp::Importer animImport;
                const aiScene* animScene = animImport.ReadFile(path,
                    aiProcess_Triangulate |
                    aiProcess_GenNormals |
                    aiProcess_LimitBoneWeights |
                    aiProcess_JoinIdenticalVertices |
                    aiProcess_ImproveCacheLocality |
                    aiProcess_FlipUVs |
                    aiProcess_GlobalScale);
                if (animScene && animScene->mRootNode) {
                    std::unordered_map<std::string, aiNode*> nodeByName;
                    std::function<void(aiNode*)> gatherNodes = [&](aiNode* node) {
                        if (!node) return;
                        nodeByName[node->mName.C_Str()] = node;
                        for (unsigned int i = 0; i < node->mNumChildren; ++i) {
                            gatherNodes(node->mChildren[i]);
                        }
                    };
                    gatherNodes(animScene->mRootNode);

                    const bool isFbx = IsFbxAnimationSource(path);
                    const glm::quat axisFixQ = ComputeAnimationAxisFix(animScene, isFbx);
                    std::unordered_set<std::string> rootBoneNames;
                    if (axisFixQ != glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) {
                        std::function<void(aiNode*, bool)> gatherRoots = [&](aiNode* node, bool parentIsRoot) {
                            if (!node) return;
                            const std::string nodeName = node->mName.C_Str();
                            if (parentIsRoot || node->mParent == animScene->mRootNode) {
                                bool animated = false;
                                for (unsigned int a = 0; a < animScene->mNumAnimations && !animated; ++a) {
                                    const aiAnimation* anim = animScene->mAnimations[a];
                                    for (unsigned int c = 0; c < anim->mNumChannels; ++c) {
                                        if (nodeName == anim->mChannels[c]->mNodeName.C_Str()) {
                                            animated = true;
                                            break;
                                        }
                                    }
                                }
                                if (animated) {
                                    rootBoneNames.insert(nodeName);
                                }
                            }
                            for (unsigned int i = 0; i < node->mNumChildren; ++i) {
                                bool isArmature = (nodeName.find("Armature") != std::string::npos ||
                                                   nodeName.find("armature") != std::string::npos ||
                                                   nodeName == "Root" || nodeName == "root");
                                gatherRoots(node->mChildren[i], isArmature);
                            }
                        };
                        gatherRoots(animScene->mRootNode, false);
                    }

                    for (const auto& [name, node] : nodeByName) {
                        const bool isRootBone = rootBoneNames.count(name) > 0;
                        sourceRestLocalTransforms[name] = ApplyAxisFixToRootLocal(
                            AssimpMat4ToGlm(node->mTransformation),
                            axisFixQ,
                            isRootBone,
                            isFbx);
                    }
                }
            }
            // Try to load/create a source avatar for this rig
            AvatarDefinition srcAvatar;
            std::string avatarPath = (p.parent_path() / (p.stem().string() + ".avatar")).string();
            bool hasAvatar = cm::animation::LoadAvatar(srcAvatar, avatarPath);
            if (hasAvatar && !importSkeleton.InverseBindPoses.empty()) {
                cm::animation::avatar_builders::PopulateBindDataFromSkeleton(importSkeleton, srcAvatar);
            }
            for (auto& clip : clips) {
                clip.SourceRestLocalTransforms = sourceRestLocalTransforms;
                // Build a unified AnimationAsset and convert skeletal to Avatar tracks if humanoid
                AnimationAsset asset; asset.meta.version = 1; asset.meta.fps = (clip.TicksPerSecond > 0.0f ? clip.TicksPerSecond : 30.0f); asset.meta.length = clip.Duration;
                bool isHumanoid = false;
                if (hasAvatar) {
                    // Convert using avatar mapping when bone names match, otherwise fall back to skeletal.
                    // For humanoid tracks we store transforms as deltas relative to the avatar bind pose so
                    // that local = Delta * BindLocal at playback, enabling consistent orientation across rigs.
                    
                    // Compute reference hips height from source avatar for position normalization.
                    // This allows animations to be properly scaled when retargeting to differently-sized models.
                    float referenceHipsHeight = 0.0f;
                    if (srcAvatar.IsBonePresent(HumanoidBone::Hips)) {
                        uint16_t hipsIdx = static_cast<uint16_t>(HumanoidBone::Hips);
                        if (hipsIdx < srcAvatar.BindModel.size()) {
                            // Use model-space Y position of hips as reference height
                            referenceHipsHeight = srcAvatar.BindModel[hipsIdx][3].y;
                        }
                    }
                    // Fallback: if hips height is too small (scaled model), try to estimate from bone positions
                    if (referenceHipsHeight < 0.01f) {
                        // Look for any reasonable height from position keys in the first frame
                        for (const auto& [boneName, bt] : clip.BoneTracks) {
                            if (boneName.find("Hips") != std::string::npos || boneName.find("hips") != std::string::npos) {
                                if (!bt.PositionKeys.empty()) {
                                    referenceHipsHeight = std::max(referenceHipsHeight, std::abs(bt.PositionKeys[0].Value.y));
                                }
                            }
                        }
                    }
                    asset.meta.referenceHipsHeight = referenceHipsHeight;
                    
                    for (const auto& [boneName, bt] : clip.BoneTracks) {
                        // Find mapped humanoid id
                        int mappedId = -1;
                        for (uint16_t i = 0; i < cm::animation::HumanoidBoneCount; ++i) {
                            if (!srcAvatar.Present[i]) continue;
                            const auto& e = srcAvatar.Map[i];
                            if (!e.BoneName.empty() && e.BoneName == boneName) { mappedId = (int)i; break; }
                        }
                        if (mappedId >= 0) {
                            isHumanoid = true;
                            asset.meta.humanoidTrackMode = HumanoidTrackMode::BindRelative;

                            glm::vec3 bindT(0.0f), bindS(1.0f), translationDeltaScale(1.0f);
                            glm::quat bindR(1.0f, 0.0f, 0.0f, 0.0f);
                            ResolveHumanoidAuthoringBindBasis(clip, boneName, srcAvatar, mappedId, bindT, translationDeltaScale, bindR, bindS);

                            auto t = std::make_unique<AssetAvatarTrack>();
                            t->humanBoneId = mappedId;
                            t->name = std::string("Humanoid:") + ToString(static_cast<cm::animation::HumanoidBone>(mappedId));

                            // Position/scale use the authored node-local rest basis so rigs with
                            // scale compensation on hips/root still reconstruct the expected relative motion.
                            // Rotation stays avatar-bind relative for stable retargeting across rigs.
                            for (const auto& k : bt.PositionKeys) {
                                cm::animation::KeyVec3 kk;
                                kk.id = 0ull;
                                kk.t = k.Time;
                                kk.v = (k.Value - bindT) * translationDeltaScale;
                                t->t.keys.push_back(kk);
                            }
                            // Rotation: Delta relative to bind rotation
                            for (const auto& k : bt.RotationKeys) {
                                cm::animation::KeyQuat kk;
                                kk.id = 0ull;
                                kk.t = k.Time;
                                glm::quat q = k.Value;
                                // Delta rotation: qDelta = q * inverse(bindR)
                                kk.v = glm::normalize(q * glm::conjugate(bindR));
                                t->r.keys.push_back(kk);
                            }
                            // Scale: Store as ratio to bind scale (multiplicative delta)
                            for (const auto& k : bt.ScaleKeys) {
                                cm::animation::KeyVec3 kk;
                                kk.id = 0ull;
                                kk.t = k.Time;
                                // Multiplicative delta: animScale / bindScale (element-wise)
                                kk.v = glm::vec3(
                                    (std::abs(bindS.x) > 1e-6f) ? k.Value.x / bindS.x : k.Value.x,
                                    (std::abs(bindS.y) > 1e-6f) ? k.Value.y / bindS.y : k.Value.y,
                                    (std::abs(bindS.z) > 1e-6f) ? k.Value.z / bindS.z : k.Value.z
                                );
                                t->s.keys.push_back(kk);
                            }

                            asset.tracks.push_back(std::move(t));
                        }
                    }
                }
                // If not humanoid or no avatar mapping, keep skeletal bone tracks
                if (!isHumanoid) {
                    for (const auto& [boneName, bt] : clip.BoneTracks) {
                        auto t = std::make_unique<AssetBoneTrack>();
                        t->name = boneName;
                        for (const auto& k : bt.PositionKeys) t->t.keys.push_back({0ull, k.Time, k.Value});
                        for (const auto& k : bt.RotationKeys) t->r.keys.push_back({0ull, k.Time, k.Value});
                        for (const auto& k : bt.ScaleKeys)    t->s.keys.push_back({0ull, k.Time, k.Value});
                        asset.tracks.push_back(std::move(t));
                    }
                }

                // Save as unified .anim
                // Normalize clip names to avoid Blender/FBX duplicates (Armature prefix, .001 suffix).
                auto normalizeClipName = [](const std::string& name) -> std::string
                {
                    if (name.empty()) return name;
                    std::string out = name;
                    auto startsWithNoCase = [](const std::string& s, const std::string& prefix) -> bool
                    {
                        if (s.size() < prefix.size()) return false;
                        for (size_t i = 0; i < prefix.size(); ++i)
                        {
                            if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
                                return false;
                        }
                        return true;
                    };
                    auto stripPrefixNoCase = [&](const std::string& prefix) -> bool
                    {
                        if (!startsWithNoCase(out, prefix)) return false;
                        out = out.substr(prefix.size());
                        return true;
                    };
                    bool stripped = true;
                    while (stripped)
                    {
                        stripped = false;
                        stripped = stripPrefixNoCase("Armature|") || stripped;
                        stripped = stripPrefixNoCase("Armature:") || stripped;
                        stripped = stripPrefixNoCase("Armature.") || stripped;
                        stripped = stripPrefixNoCase("Armature_") || stripped;
                    }
                    while (!out.empty())
                    {
                        char c = out.front();
                        if (c == '|' || c == ':' || c == '.' || c == '_' || std::isspace(static_cast<unsigned char>(c)))
                            out.erase(out.begin());
                        else
                            break;
                    }
                    const std::string duplicateSuffix = ".001";
                    if (out.size() > duplicateSuffix.size() &&
                        out.rfind(duplicateSuffix) == out.size() - duplicateSuffix.size())
                    {
                        out = out.substr(0, out.size() - duplicateSuffix.size());
                    }
                    return out.empty() ? name : out;
                };

                // Sanitize clip name for filesystem safety (avoid characters illegal on Windows paths).
                auto sanitizeForFilename = [](const std::string& name) -> std::string
                {
                    if (name.empty()) return std::string("Anim");
                    std::string out; out.reserve(name.size());
                    for (char c : name)
                    {
                        // Disallow Windows reserved characters:  < > : \" / \ | ? *
                        if (c == '<' || c == '>' || c == ':' || c == '\"'
                            || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
                        {
                            out.push_back('_');
                        }
                        else
                        {
                            out.push_back(c);
                        }
                    }
                    // Avoid trailing dots/spaces which Windows also dislikes for filenames.
                    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
                        out.pop_back();
                    if (out.empty()) out = "Anim";
                    return out;
                };

                std::string normalizedName = normalizeClipName(clip.Name);
                std::string sanitizedName = sanitizeForFilename(normalizedName);
                std::string sanitizedOriginalName = sanitizeForFilename(clip.Name);
                asset.name = normalizedName.empty() ? clip.Name : normalizedName;

                std::string outPath = dir + "/" + p.stem().string() + "_" + sanitizedName + ".anim";
                std::string legacyOutPath;
                if (sanitizedOriginalName != sanitizedName)
                {
                    legacyOutPath = dir + "/" + p.stem().string() + "_" + sanitizedOriginalName + ".anim";
                }

                // Preserve script events from existing animation files on reimport.
                auto tryLoadAssetFromSource = [](const std::string& sourcePath, AnimationAsset& outAsset) -> bool
                {
                    std::ifstream f(sourcePath);
                    if (!f.is_open()) return false;
                    nlohmann::json j;
                    try
                    {
                        f >> j;
                    }
                    catch (...)
                    {
                        return false;
                    }
                    outAsset = DeserializeAnimationAsset(j);
                    return true;
                };
                auto hasScriptEvents = [](const AnimationAsset& src) -> bool
                {
                    for (const auto& track : src.tracks)
                    {
                        if (!track || track->type != TrackType::ScriptEvent) continue;
                        const auto* st = static_cast<const AssetScriptEventTrack*>(track.get());
                        if (st && !st->events.empty()) return true;
                    }
                    return false;
                };
                auto mergeScriptEvents = [](AnimationAsset& dst, const AnimationAsset& src)
                {
                    bool hasEvents = false;
                    for (const auto& track : src.tracks)
                    {
                        if (!track || track->type != TrackType::ScriptEvent) continue;
                        const auto* st = static_cast<const AssetScriptEventTrack*>(track.get());
                        if (st && !st->events.empty()) { hasEvents = true; break; }
                    }
                    if (!hasEvents) return;
                    dst.tracks.erase(
                        std::remove_if(dst.tracks.begin(), dst.tracks.end(),
                            [](const std::unique_ptr<ITrack>& t)
                            {
                                return t && t->type == TrackType::ScriptEvent;
                            }),
                        dst.tracks.end());
                    for (const auto& track : src.tracks)
                    {
                        if (!track || track->type != TrackType::ScriptEvent) continue;
                        const auto* st = static_cast<const AssetScriptEventTrack*>(track.get());
                        if (!st || st->events.empty()) continue;
                        auto clone = std::make_unique<AssetScriptEventTrack>();
                        clone->type = TrackType::ScriptEvent;
                        clone->id = st->id;
                        clone->name = st->name;
                        clone->muted = st->muted;
                        clone->events = st->events;
                        dst.tracks.push_back(std::move(clone));
                    }
                };

                AnimationAsset existingAsset;
                bool mergedEvents = false;
                if (fs::exists(outPath) && tryLoadAssetFromSource(outPath, existingAsset) && hasScriptEvents(existingAsset))
                {
                    mergeScriptEvents(asset, existingAsset);
                    mergedEvents = true;
                }
                else if (!legacyOutPath.empty() && fs::exists(legacyOutPath) &&
                         tryLoadAssetFromSource(legacyOutPath, existingAsset) && hasScriptEvents(existingAsset))
                {
                    mergeScriptEvents(asset, existingAsset);
                    mergedEvents = true;
                }

                std::cout << "[AssetPipeline] Attempting to save animation clip '" << clip.Name
                          << "' (normalized: '" << normalizedName << "', sanitized: '" << sanitizedName
                          << "') to '" << outPath << "'\n";
                if (mergedEvents)
                {
                    std::cout << "[AssetPipeline] Preserved script events for '" << outPath << "'\n";
                }

                bool saveOk = SaveAnimationAsset(asset, outPath);
                if (saveOk) {
                    anyAnimationClipChanged = true;
                    std::cout << "[AssetPipeline] Saved animation asset: " << outPath << std::endl;
                    
                    // Create/update import settings with source file info for reimport support
                    AnimationImportSettings importSettings;
                    AnimationImportSettings::LoadFromMeta(outPath, importSettings); // Load existing if any
                    importSettings.SourceFilePath = path;
                    importSettings.SourceAnimationIndex = static_cast<int>(&clip - &clips[0]);
                    AnimationImportSettings::SaveToMeta(outPath, importSettings);
                } else {
                    std::cerr << "[AssetPipeline] ERROR: Failed to save animation asset to '" << outPath
                              << "'. Check directory permissions and path validity.\n";
                }
            }
            if (anyAnimationClipChanged) {
                // Force fresh animation loads after extraction/reimport.
                cm::animation::ClearAnimationAssetCache();
                Scene::Get().InvalidateAllAnimatorAssetCaches();
            }
        }
        // After import: hot-swap in active scene and emit event
        AssetPipeline::Instance().HotSwapModelInScene(path);
        
        // Emit reimport event so other systems (like PrefabEditor) can react
        ClaymoreGUID modelGuid = AssetLibrary::Instance().GetGUIDForPath(path);
        AssetEventBus::Instance().Emit(AssetEvent::Reimported, AssetType::Mesh, path, modelGuid);
        });
}

void AssetPipeline::RebuildAllPrefabBinaries() {
    // Hard guarantee: this command forces a complete binary cache rebuild so that
    // the on-disk binaries used by runtime are always regenerated from current sources.
    // We intentionally do not rely on AssetLibrary registration here, since that can
    // miss authoring files or point at processed paths. Instead we:
    //  1) Drop the entire binary cache (.bin/*).
    //  2) Rebuild all current binaries from source via EnsureAllCurrent().
    //
    // This makes it structurally impossible for a stale .prefabb in the cache to
    // survive a "Rebuild Prefab Binaries" operation.
    std::cout << "[PrefabRebuild] Invalidating all binary assets..." << std::endl;
    BinaryAssetCache::Instance().InvalidateAll();

    std::cout << "[PrefabRebuild] Rebuilding binary cache for all assets..." << std::endl;
    BinaryAssetCache::Instance().EnsureAllCurrent();

    std::cout << "[PrefabRebuild] Completed full binary cache rebuild." << std::endl;
}

/// @brief Rebuild texture handles in a property block from stored paths
/// Uses the global texture cache to ensure handles are properly managed
static void RebuildPropertyBlockTexturesFromPathsLocal(
    MaterialPropertyBlock& block,
    const std::unordered_map<std::string, std::string>& texturePaths)
{
    // Clear existing texture references (don't destroy - they're cached)
    block.Textures.clear();
    block.TexturesByID.clear();
    
    // Reload from paths using the texture cache
    for (const auto& kv : texturePaths) {
        if (kv.second.empty()) continue;
        std::string path = kv.second;
        try { path = IVirtualFS::NormalizePath(path); } catch(...) {}
        
        // Determine color space based on sampler name
        // Color textures (albedo, emission) use sRGB; data textures (normal, MR, AO) use Linear
        // Engine does not use sRGB gamma correction - all textures loaded as Linear
        TextureColorSpace colorSpace = TextureColorSpace::Linear;
        
        // Use the global texture cache to get/create handles
        TextureSpecifier spec;
        spec.Path = path;
        bgfx::TextureHandle handle = AcquireTextureHandle(spec, colorSpace);
        if (!bgfx::isValid(handle)) continue;
        block.SetTexture(kv.first, handle);
    }
}

/// @brief Clear texture handles from a property block without destroying them
/// Used before RemoveEntity to avoid the cached copy having stale handles
static void ClearPropertyBlockTextureHandles(MaterialPropertyBlock& block) {
    block.Textures.clear();
    block.TexturesByID.clear();
}

/// @brief Check if a vec3 contains finite values
static bool IsFiniteVec3(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/// @brief Check if a mat4 contains finite values
static bool IsFiniteMatrix(const glm::mat4& m) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (!std::isfinite(m[c][r])) return false;
        }
    }
    return true;
}

/// @brief Validate and sanitize a transform, resetting invalid values to safe defaults
static void SanitizeTransform(TransformComponent& xf) {
    if (!IsFiniteVec3(xf.Position)) {
        std::cerr << "[HotSwap] WARNING: Invalid position detected, resetting to origin" << std::endl;
        xf.Position = glm::vec3(0.0f);
    }
    if (!IsFiniteVec3(xf.Rotation)) {
        std::cerr << "[HotSwap] WARNING: Invalid rotation detected, resetting to zero" << std::endl;
        xf.Rotation = glm::vec3(0.0f);
    }
    if (!IsFiniteVec3(xf.Scale) || xf.Scale == glm::vec3(0.0f)) {
        std::cerr << "[HotSwap] WARNING: Invalid scale detected, resetting to unit" << std::endl;
        xf.Scale = glm::vec3(1.0f);
    }
    if (!IsFiniteMatrix(xf.LocalMatrix)) {
        std::cerr << "[HotSwap] WARNING: Invalid LocalMatrix detected, resetting to identity" << std::endl;
        xf.LocalMatrix = glm::mat4(1.0f);
    }
    if (!IsFiniteMatrix(xf.WorldMatrix)) {
        std::cerr << "[HotSwap] WARNING: Invalid WorldMatrix detected, resetting to identity" << std::endl;
        xf.WorldMatrix = glm::mat4(1.0f);
    }
}

/// @brief Complete override storage for a model node during hot reload
/// This captures ALL user-authored state that should survive a model re-export
struct HotSwapNodeOverride {
    // Identification (multiple fallbacks for matching)
    std::string nodePath;           ///< Full path from root (e.g., "Armature/Hips/Spine")
    std::string normalizedPath;     ///< Path with _### suffixes stripped
    std::string nodeName;           ///< Just the node name
    std::string normalizedName;     ///< Name with _### suffix stripped
    int meshFileId = -1;            ///< Mesh file ID for fallback matching
    uint64_t contentHash = 0;       ///< Content hash for renamed node matching
    ClaymoreGUID derivedGuid;       ///< Stable derived GUID from identity system
    
    // Is this the root node? Root node's transform is always preserved.
    bool isRoot = false;
    
    // Parent path tracking for reparented nodes (e.g., sword moved from root to a bone)
    // If empty, the node hasn't been reparented and should stay at its model-defined location
    std::string parentPath;
    std::string parentNormalizedPath;
    bool wasReparented = false;  // True if user reparented this node within the model
    
    // Transform - preserved for ALL nodes (root and children) to maintain user positioning
    TransformComponent transform;
    // Track if user modified the transform from the model's default (for children)
    bool transformModified = false;
    
    // Mesh component overrides
    bool hasMesh = false;
    std::shared_ptr<Material> material;
    std::vector<std::shared_ptr<Material>> materials;
    MaterialPropertyBlock propertyBlock;
    std::unordered_map<std::string, std::string> propertyBlockTexturePaths;
    std::vector<MaterialPropertyBlock> slotPropertyBlocks;
    std::vector<std::unordered_map<std::string, std::string>> slotPropertyBlockTexturePaths;
    std::vector<std::string> materialAssetPaths;
    bool uniqueMaterial = false;
    bool showBackfaces = false;
    bool renderOnTop = false;
    int renderOrder = 0;
    float boundsPadding = 1.0f;
    
    // RenderOverrides component (CRITICAL: was missing before)
    bool hasRenderOverrides = false;
    bool alphaBlendEnabled = false;
    bool useAlphaCutout = false;
    float alphaCutoutThreshold = 0.5f;
    bool castShadows = true;
    bool receiveShadows = true;
    bool renderOverridesVisible = true;  // RenderOverrides::Visible
    int sortingOrder = 0;
    
    // Entity-level visibility (EntityData::Visible, separate from RenderOverrides)
    bool entityVisible = true;
    
    // TintMaskController (tint colors for child meshes)
    bool hasTintController = false;
    std::unique_ptr<TintMaskController> tintController;
    
    // UnifiedMorph (aggregated blend shapes at model root)
    bool hasUnifiedMorph = false;
    std::unique_ptr<UnifiedMorphComponent> unifiedMorph;
    
    // BlendShapes (per-mesh blend shape weights)
    bool hasBlendShapes = false;
    std::unordered_map<std::string, float> blendShapeWeights;
    
    // AnimationPlayer (animation state)
    bool hasAnimationPlayer = false;
    std::unique_ptr<cm::animation::AnimationPlayerComponent> animationPlayer;
    
    // User-added components (preserved by reference, not deep-copied)
    bool hasLight = false;
    std::unique_ptr<LightComponent> light;
    bool hasCollider = false;
    std::unique_ptr<ColliderComponent> collider;
    bool hasRigidBody = false;
    std::unique_ptr<RigidBodyComponent> rigidBody;
    bool hasStaticBody = false;
    std::unique_ptr<StaticBodyComponent> staticBody;
    bool hasCamera = false;
    std::unique_ptr<CameraComponent> camera;
    bool hasEmitter = false;
    std::unique_ptr<ParticleEmitterComponent> emitter;
    std::vector<ScriptInstance> scripts;
    
    // BoneAttachment (for attaching non-skinned meshes to skeleton bones)
    bool hasBoneAttachment = false;
    std::unique_ptr<BoneAttachmentComponent> boneAttachment;
    
    // LookAt constraints (rotation-only constraints for look/aim behavior)
    std::vector<cm::animation::lookat::LookAtConstraintComponent> lookAtConstraints;
    
    // IK constraints (positional IK chains)
    std::vector<cm::animation::ik::IKComponent> iks;
    
    // Entity metadata
    std::string name;
    int layer = 0;
    std::string tag;
    nlohmann::json extra;
};

/// @brief Tracks a user-added child entity that should be preserved across model reimport
/// User-added children are entities that aren't part of the original model (e.g., attached weapons,
/// custom child objects, etc.) and should be re-parented after the model is reinstantiated.
struct UserAddedChild {
    EntityID entityId;              ///< The entity ID of the user-added child
    std::string parentPath;         ///< Path of the parent node in the model (for re-parenting)
    std::string parentNormalizedPath; ///< Normalized version for fallback matching
    std::string parentName;         ///< Just the parent name for fallback matching
};

void AssetPipeline::HotSwapModelInScene(const std::string& modelPath) {
    // Resolve GUID for this path if known
    ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(modelPath);
    Scene& scene = Scene::Get();

    // Try to load the new model's identity map for improved matching
    ModelIdentityMap newIdentityMap;
    bool hasNewIdentityMap = false;
    {
        BuiltModelPaths built{};
        if (HasModelCache(modelPath, built)) {
            try {
                std::ifstream in(built.metaPath);
                if (in.is_open()) {
                    nlohmann::json meta; in >> meta; in.close();
                    if (meta.contains("nodeIdentities") && meta["nodeIdentities"].is_object()) {
                        newIdentityMap = ModelIdentityMap::FromJson(meta["nodeIdentities"]);
                        hasNewIdentityMap = true;
                        std::cout << "[HotSwap] Loaded identity map with " << newIdentityMap.Nodes.size() << " nodes" << std::endl;
                    }
                }
            } catch (...) {}
        }
    }

    // Helper: does this entity (or any descendant) contain a mesh from the target GUID?
    std::function<bool(EntityID)> subtreeHasGuid = [&](EntityID id) -> bool {
        auto* d = scene.GetEntityData(id); if (!d) return false;
        if (d->Mesh && d->Mesh->meshReference.guid == guid && (guid.high != 0 || guid.low != 0)) return true;
        for (EntityID c : d->Children) if (subtreeHasGuid(c)) return true;
        return false;
    };

    // Collect candidate roots to replace (top-most nodes whose subtree references the guid)
    std::vector<EntityID> roots;
    for (const auto& e : scene.GetEntities()) {
        EntityID id = e.GetID();
        auto* d = scene.GetEntityData(id); if (!d) continue;
        if (!subtreeHasGuid(id)) continue;
        // If parent also has target in its subtree, skip (we want top-most)
        if (d->Parent != (EntityID)-1) {
            if (subtreeHasGuid(d->Parent)) continue;
        }
        roots.push_back(id);
    }
    if (roots.empty()) return;

    std::cout << "[HotSwap] Found " << roots.size() << " model instance(s) to hot-swap for: " << modelPath << std::endl;

    // Utility: compute relative path using names; normalize by stripping trailing _digits
    auto normalizeName = [](const std::string& name) -> std::string {
        return ModelNodeIdentity::NormalizeName(name);
    };
    
    auto relPathOf = [&](EntityID root, EntityID node) -> std::string {
        std::vector<std::string> parts;
        EntityID cur = node;
        while (cur != (EntityID)-1) {
            auto* d = scene.GetEntityData(cur); if (!d) break;
            parts.push_back(d->Name);
            if (cur == root) break;
            cur = d->Parent;
        }
        if (parts.empty()) return std::string();
        std::reverse(parts.begin(), parts.end());
        if (!parts.empty()) parts.erase(parts.begin()); // make relative to root
        std::string s; for (size_t i = 0; i < parts.size(); ++i) { s += parts[i]; if (i + 1 < parts.size()) s += "/"; }
        return s;
    };

    for (EntityID root : roots) {
        auto* rd = scene.GetEntityData(root); if (!rd) continue;
        
        // Save root placement and hierarchy linkage
        TransformComponent savedRootXf = rd->Transform;
        EntityID savedParent = rd->Parent;
        std::string savedName = rd->Name;
        int savedLayer = rd->Layer;
        std::string savedTag = rd->Tag;
        
        // Save deleted model nodes list (user-intentional deletions that should persist)
        std::vector<std::string> savedDeletedNodes = rd->DeletedModelNodes;

        // Build OLD entity ID to path mapping for TintController target remapping
        std::unordered_map<EntityID, std::string> oldEntityIdToPath;
        std::function<void(EntityID)> buildOldIdMap = [&](EntityID id) {
            auto* d = scene.GetEntityData(id); if (!d) return;
            oldEntityIdToPath[id] = relPathOf(root, id);
            for (EntityID c : d->Children) buildOldIdMap(c);
        };
        buildOldIdMap(root);
        // Root maps to empty path
        oldEntityIdToPath[root] = "";
        
        // Identify user-added children: entities that aren't part of the original model
        // These are children that don't have a mesh with the model's GUID, indicating
        // they were manually added by the user (e.g., attached weapons, custom objects, particle emitters)
        // 
        // KEY FIX: We use the NEW model's identity map to check if a node path exists in the model.
        // This correctly identifies bones (which exist in the new model) vs user-added children
        // (which don't exist in the new model's structure).
        std::vector<UserAddedChild> userAddedChildren;
        std::function<void(EntityID, const std::string&)> findUserAddedChildren = [&](EntityID id, const std::string& parentPath) {
            auto* d = scene.GetEntityData(id); if (!d) return;
            
            std::string currentPath = parentPath.empty() ? d->Name : (parentPath + "/" + d->Name);
            
            for (EntityID childId : d->Children) {
                auto* childData = scene.GetEntityData(childId);
                if (!childData) continue;
                
                // Build the child's path relative to model root (excludes root name)
                std::string childRelPath = currentPath.empty() ? childData->Name : (currentPath + "/" + childData->Name);
                std::string childNormPath = ModelNodeIdentity::NormalizePath(childRelPath);
                
                // Check if this child is part of the model (has mesh with matching GUID)
                bool isModelNode = false;
                if (childData->Mesh) {
                    // If it has a mesh from this model, it's a model node
                    if (childData->Mesh->meshReference.guid == guid && (guid.high != 0 || guid.low != 0)) {
                        isModelNode = true;
                    }
                }
                
                // CRITICAL: Check if this child is a NESTED MODEL (has its own ModelAssetGuid)
                // Nested models (e.g., armor pieces parented to SkeletonRoot) should be preserved
                // and serialized in their own right - they are NOT part of the parent model
                // We check this EARLY so we don't accidentally flag nested models as model nodes
                bool isNestedModelRoot = (childData->ModelAssetGuid.high != 0 || childData->ModelAssetGuid.low != 0);
                
                // Use the NEW model's identity map to check if this path exists in the model
                // This is the authoritative way to distinguish bones from user-added children
                // IMPORTANT: Skip this for nested models - they have their own identity
                if (!isModelNode && !isNestedModelRoot && hasNewIdentityMap) {
                    // Check if the node path (or normalized path) exists in the new model
                    bool existsInNewModel = false;
                    for (const auto& nodeId : newIdentityMap.Nodes) {
                        if (nodeId.NodePath == childRelPath || nodeId.NormalizedPath == childNormPath ||
                            ModelNodeIdentity::NormalizePath(nodeId.NodePath) == childNormPath) {
                            existsInNewModel = true;
                            break;
                        }
                    }
                    if (existsInNewModel) {
                        // This is a model node (bone/structure) - will be replaced by new model
                        isModelNode = true;
                    }
                }
                
                // Fallback: Check if node looks like a skeleton structure (SkeletonRoot or parented to it)
                // This handles cases where identity map isn't available
                // IMPORTANT: Only use this fallback when identity map is NOT available!
                // If identity map IS available and the node isn't in it, it's truly user-added.
                // IMPORTANT: Skip this check for nested models - they should be treated as user-added
                if (!isModelNode && !childData->Mesh && !isNestedModelRoot && !hasNewIdentityMap) {
                    // Check if this is a SkeletonRoot or has Skeleton component
                    if (childData->Skeleton || childData->Name == "SkeletonRoot") {
                        isModelNode = true;
                    }
                    // Check if any ancestor has a Skeleton component (indicating this is a bone)
                    else {
                        EntityID parent = childData->Parent;
                        while (parent != (EntityID)-1 && parent != root) {
                            auto* parentData = scene.GetEntityData(parent);
                            if (!parentData) break;
                            if (parentData->Skeleton) {
                                // This child is under a skeleton - it's a bone, not user-added
                                isModelNode = true;
                                break;
                            }
                            parent = parentData->Parent;
                        }
                    }
                }
                
                // If still not identified as model node and has no mesh, check if it's user-added
                // This includes: nested models, particle emitters, lights, empty transforms
                if (!isModelNode && !childData->Mesh) {
                    // Entity has no mesh and isn't part of model structure - it's user-added
                    UserAddedChild uac;
                    uac.entityId = childId;
                    uac.parentPath = currentPath;
                    uac.parentNormalizedPath = ModelNodeIdentity::NormalizePath(currentPath);
                    uac.parentName = d->Name;
                    userAddedChildren.push_back(uac);
                    if (isNestedModelRoot) {
                        std::cout << "[HotSwap] Found nested model: " << childData->Name 
                                  << " (parent: " << currentPath << ")" << std::endl;
                    } else {
                        std::cout << "[HotSwap] Found user-added child: " << childData->Name 
                                  << " (parent: " << currentPath << ")" << std::endl;
                    }
                    continue; // Don't recurse into user-added subtrees
                }
                
                // Also detect user-added children that DO have meshes (from other models/sources)
                if (!isModelNode && childData->Mesh) {
                    // Has a mesh but it's from a different model - this is user-added
                    UserAddedChild uac;
                    uac.entityId = childId;
                    uac.parentPath = currentPath;
                    uac.parentNormalizedPath = ModelNodeIdentity::NormalizePath(currentPath);
                    uac.parentName = d->Name;
                    userAddedChildren.push_back(uac);
                    std::cout << "[HotSwap] Found user-added child with external mesh: " << childData->Name 
                              << " (parent: " << currentPath << ")" << std::endl;
                    continue; // Don't recurse into user-added subtrees
                }
                
                // Recurse into model nodes
                findUserAddedChildren(childId, currentPath);
            }
        };
        // Start from root's children
        findUserAddedChildren(root, "");

        // Walk old subtree and cache COMPLETE overrides for all nodes
        std::vector<HotSwapNodeOverride> overrides;
        std::function<void(EntityID, bool)> dfsCache = [&](EntityID id, bool isRootNode) {
            auto* d = scene.GetEntityData(id); if (!d) return;
            
            HotSwapNodeOverride ov;
            ov.nodePath = relPathOf(root, id);
            ov.normalizedPath = ModelNodeIdentity::NormalizePath(ov.nodePath);
            ov.nodeName = d->Name;
            ov.normalizedName = normalizeName(d->Name);
            ov.isRoot = isRootNode;
            ov.transform = d->Transform;
            ov.transformModified = true;  // Always preserve transforms (user may have repositioned any node)
            ov.name = d->Name;
            ov.layer = d->Layer;
            ov.tag = d->Tag;
            ov.extra = d->Extra;
            ov.entityVisible = d->Visible;  // Preserve entity-level visibility
            
            // Track parent path for reparenting detection (e.g., sword moved from root to bone)
            if (!isRootNode && d->Parent != (EntityID)-1 && d->Parent != root) {
                ov.parentPath = relPathOf(root, d->Parent);
                ov.parentNormalizedPath = ModelNodeIdentity::NormalizePath(ov.parentPath);
            }
            
            // Mesh component - capture EVERYTHING
            if (d->Mesh) {
                ov.hasMesh = true;
                ov.meshFileId = d->Mesh->meshReference.fileID;
                ov.material = d->Mesh->material;
                ov.materials = d->Mesh->materials;
                
                // Copy property blocks but clear texture handles - they'll become stale after RemoveEntity
                // We store the paths and will rebuild fresh texture handles after reinstantiating
                ov.propertyBlock = d->Mesh->PropertyBlock;
                ov.propertyBlockTexturePaths = d->Mesh->PropertyBlockTexturePaths;
                ClearPropertyBlockTextureHandles(ov.propertyBlock);
                
                ov.slotPropertyBlocks = d->Mesh->SlotPropertyBlocks;
                ov.slotPropertyBlockTexturePaths = d->Mesh->SlotPropertyBlockTexturePaths;
                for (auto& pb : ov.slotPropertyBlocks) {
                    ClearPropertyBlockTextureHandles(pb);
                }
                
                ov.materialAssetPaths = d->Mesh->MaterialAssetPaths;
                ov.uniqueMaterial = d->Mesh->UniqueMaterial;
                ov.showBackfaces = d->Mesh->ShowBackfaces;
                ov.renderOnTop = d->Mesh->RenderOnTop;
                ov.renderOrder = d->Mesh->RenderOrder;
                ov.boundsPadding = d->Mesh->BoundsPadding;
                
                // Compute content hash for fallback matching
                std::vector<int> meshIndices = { d->Mesh->meshReference.fileID };
                const float* xfData = &d->Transform.LocalMatrix[0][0];
                int vertHint = d->Mesh->mesh ? static_cast<int>(d->Mesh->mesh->Vertices.size()) : 0;
                ov.contentHash = ModelNodeIdentity::ComputeContentHash(meshIndices, xfData, vertHint);
            }
            
            // RenderOverrides component (CRITICAL: was missing before!)
            if (d->RenderOverrides) {
                ov.hasRenderOverrides = true;
                ov.alphaBlendEnabled = d->RenderOverrides->AlphaBlendEnabled;
                ov.useAlphaCutout = d->RenderOverrides->UseAlphaCutout;
                ov.alphaCutoutThreshold = d->RenderOverrides->AlphaCutoutThreshold;
                ov.castShadows = d->RenderOverrides->CastShadows;
                ov.receiveShadows = d->RenderOverrides->ReceiveShadows;
                ov.renderOverridesVisible = d->RenderOverrides->Visible;
                ov.sortingOrder = d->RenderOverrides->SortingOrder;
            }
            
            // BlendShapes (per-mesh blend shape weights)
            if (d->BlendShapes && !d->BlendShapes->Shapes.empty()) {
                ov.hasBlendShapes = true;
                for (const auto& shape : d->BlendShapes->Shapes) {
                    ov.blendShapeWeights[shape.Name] = shape.Weight;
                }
            }
            
            // User-added components - deep copy via clone or move
            if (d->Light) {
                ov.hasLight = true;
                ov.light = std::make_unique<LightComponent>(*d->Light);
            }
            if (d->Collider) {
                ov.hasCollider = true;
                ov.collider = std::make_unique<ColliderComponent>(*d->Collider);
            }
            if (d->RigidBody) {
                ov.hasRigidBody = true;
                ov.rigidBody = std::make_unique<RigidBodyComponent>(*d->RigidBody);
            }
            if (d->StaticBody) {
                ov.hasStaticBody = true;
                ov.staticBody = std::make_unique<StaticBodyComponent>(*d->StaticBody);
            }
            if (d->Camera) {
                ov.hasCamera = true;
                ov.camera = std::make_unique<CameraComponent>(*d->Camera);
            }
            if (d->Emitter) {
                ov.hasEmitter = true;
                ov.emitter = std::make_unique<ParticleEmitterComponent>(*d->Emitter);
            }
            ov.scripts = d->Scripts;
            
            // TintMaskController (preserve tint colors for child meshes)
            if (d->TintController) {
                ov.hasTintController = true;
                ov.tintController = std::make_unique<TintMaskController>(*d->TintController);
            }
            
            // UnifiedMorph (preserve aggregated blend shape weights)
            if (d->UnifiedMorph) {
                ov.hasUnifiedMorph = true;
                ov.unifiedMorph = std::make_unique<UnifiedMorphComponent>(*d->UnifiedMorph);
            }
            
            // AnimationPlayer (preserve animation state)
            if (d->AnimationPlayer) {
                ov.hasAnimationPlayer = true;
                ov.animationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>(*d->AnimationPlayer);
            }
            
            // BoneAttachment (for attaching non-skinned meshes to skeleton bones)
            if (d->BoneAttachment) {
                ov.hasBoneAttachment = true;
                ov.boneAttachment = std::make_unique<BoneAttachmentComponent>(*d->BoneAttachment);
            }
            
            // LookAt constraints (rotation-only constraints for look/aim behavior)
            if (!d->LookAtConstraints.empty()) {
                ov.lookAtConstraints = d->LookAtConstraints;
            }
            
            // IK constraints (positional IK chains)
            if (!d->IKs.empty()) {
                ov.iks = d->IKs;
            }
            
            // Track if node has significant user data (for logging purposes)
            bool hasUserData = ov.hasMesh || ov.hasRenderOverrides || ov.hasLight || 
                               ov.hasCollider || ov.hasRigidBody || ov.hasStaticBody || ov.hasBoneAttachment ||
                               ov.hasCamera || ov.hasEmitter || !ov.scripts.empty() ||
                               !ov.extra.empty() || ov.hasTintController || ov.hasUnifiedMorph ||
                               ov.hasAnimationPlayer || ov.hasBlendShapes || !ov.entityVisible ||
                               !ov.lookAtConstraints.empty() || !ov.iks.empty();
            (void)hasUserData;  // Suppress unused variable warning
            // Always store for transform preservation (all nodes may have user positioning)
            overrides.push_back(std::move(ov));
            
            for (EntityID c : d->Children) dfsCache(c, false);  // Child nodes are not root
        };
        dfsCache(root, true);  // Root node

        std::cout << "[HotSwap] Captured " << overrides.size() << " node override(s)" << std::endl;
        
        // Detach user-added children BEFORE removing the old subtree
        // This prevents them from being deleted along with the model
        for (const auto& uac : userAddedChildren) {
            scene.SetParent(uac.entityId, (EntityID)-1); // Unparent to scene root
            std::cout << "[HotSwap] Detached user-added child: " << uac.entityId << std::endl;
        }

        // Remove old subtree (user-added children are now safe at scene root)
        scene.RemoveEntity(root);

        // Instantiate fresh model at the same place
        EntityID newRoot = scene.InstantiateModel(modelPath, glm::vec3(0.0f));
        if (newRoot == (EntityID)-1 || newRoot == (EntityID)0) {
            std::cerr << "[HotSwap] Failed to reinstantiate model: " << modelPath << std::endl;
            continue;
        }
        
        if (auto* nd = scene.GetEntityData(newRoot)) {
            nd->Name = savedName;
            nd->Layer = savedLayer;
            nd->Tag = savedTag;
            nd->Transform = savedRootXf;
            SanitizeTransform(nd->Transform);  // Prevent -INF or NaN from corrupting the scene
            nd->Transform.TransformDirty = true;
            
            // Restore deleted model nodes list
            nd->DeletedModelNodes = savedDeletedNodes;
            
            // Apply deletions to the new model instance (paths are RELATIVE to root, not including root name)
            if (!savedDeletedNodes.empty()) {
                // Build path-to-EntityID map for deletion
                std::unordered_map<std::string, EntityID> delPathToEntity;
                std::function<void(EntityID, const std::string&)> buildDelPathMap = [&](EntityID id, const std::string& parentPath) {
                    auto* data = scene.GetEntityData(id);
                    if (!data) return;
                    std::string nodePath = parentPath.empty() ? data->Name : (parentPath + "/" + data->Name);
                    delPathToEntity[nodePath] = id;
                    for (EntityID c : data->Children) buildDelPathMap(c, nodePath);
                };
                // Start from children of root (paths exclude root name)
                for (EntityID rootChild : nd->Children) {
                    buildDelPathMap(rootChild, "");
                }
                // Delete nodes in the deletion list
                for (const std::string& delPath : savedDeletedNodes) {
                    auto it = delPathToEntity.find(delPath);
                    if (it != delPathToEntity.end()) {
                        scene.RemoveEntity(it->second);
                        std::cout << "[HotSwap] Removed deleted model node: " << delPath << std::endl;
                    } else {
                        std::cout << "[HotSwap] WARNING: Could not find deleted node path: " << delPath << std::endl;
                    }
                }
            }
        }
        if (savedParent != (EntityID)-1) scene.SetParent(newRoot, savedParent);

        // Build lookup structures for new model nodes
        std::unordered_map<std::string, EntityID> newNodesByPath;
        std::unordered_map<std::string, EntityID> newNodesByNormalizedPath;
        std::unordered_map<int, EntityID> newNodesByFileId;
        std::unordered_map<std::string, EntityID> newNodesByNormalizedName;
        
        std::function<void(EntityID, const std::string&)> buildNewLookups = [&](EntityID id, const std::string& parentPath) {
            auto* d = scene.GetEntityData(id); if (!d) return;
            std::string path = parentPath.empty() ? d->Name : (parentPath + "/" + d->Name);
            std::string normPath = ModelNodeIdentity::NormalizePath(path);
            
            newNodesByPath[path] = id;
            newNodesByNormalizedPath[normPath] = id;
            newNodesByNormalizedName[normalizeName(d->Name)] = id;
            
            if (d->Mesh && d->Mesh->meshReference.fileID >= 0) {
                newNodesByFileId[d->Mesh->meshReference.fileID] = id;
            }
            
            for (EntityID c : d->Children) {
                auto* cd = scene.GetEntityData(c);
                if (cd) buildNewLookups(c, path);
            }
        };
        auto* newRootData = scene.GetEntityData(newRoot);
        if (newRootData) {
            buildNewLookups(newRoot, "");
        }

        // Apply cached overrides with multi-fallback matching
        int appliedCount = 0;
        for (const auto& ov : overrides) {
            EntityID target = (EntityID)-1;
            bool matchedByPath = false;  // Track if we matched by exact path
            
            // 1. Try exact path match
            {
                auto it = newNodesByPath.find(ov.nodePath);
                if (it != newNodesByPath.end()) {
                    target = it->second;
                    matchedByPath = true;
                }
            }
            
            // 2. Try normalized path match
            if (target == (EntityID)-1 && !ov.normalizedPath.empty()) {
                auto it = newNodesByNormalizedPath.find(ov.normalizedPath);
                if (it != newNodesByNormalizedPath.end()) {
                    target = it->second;
                }
            }
            
            // 3. Try mesh file ID match
            if (target == (EntityID)-1 && ov.meshFileId >= 0) {
                auto it = newNodesByFileId.find(ov.meshFileId);
                if (it != newNodesByFileId.end()) {
                    target = it->second;
                }
            }
            
            // 4. Try identity map matching (uses content hash for renamed nodes)
            if (target == (EntityID)-1 && hasNewIdentityMap && ov.contentHash != 0) {
                auto candidates = newIdentityMap.FindByHash(ov.contentHash);
                for (const auto* candidate : candidates) {
                    // Find entity by the candidate's path
                    auto it = newNodesByNormalizedPath.find(candidate->NormalizedPath);
                    if (it != newNodesByNormalizedPath.end()) {
                        target = it->second;
                        std::cout << "[HotSwap] Matched by content hash: " << ov.nodePath << " -> " << candidate->NodePath << std::endl;
                        break;
                    }
                }
            }
            
            // 5. Last resort: fuzzy name match
            if (target == (EntityID)-1 && !ov.normalizedName.empty()) {
                auto it = newNodesByNormalizedName.find(ov.normalizedName);
                if (it != newNodesByNormalizedName.end()) {
                    target = it->second;
                    std::cout << "[HotSwap] Matched by fuzzy name: " << ov.nodePath << " -> " << ov.normalizedName << std::endl;
                }
            }
            
            if (target == (EntityID)-1) {
                std::cout << "[HotSwap] WARNING: Could not match node: '" << ov.nodePath << "'"
                          << " (normalized: '" << ov.normalizedPath << "')"
                          << " (hasMesh: " << ov.hasMesh << ", meshFileId: " << ov.meshFileId << ")"
                          << std::endl;
                continue;
            }
            
            auto* td = scene.GetEntityData(target);
            if (!td) continue;
            
            appliedCount++;
            
            // Handle reparenting: if we matched by fallback (not by path) and wasReparented flag is set,
            // or if the node path implies a different parent than current, reparent the node.
            // This restores user hierarchy changes after model reimport.
            if (!matchedByPath && !ov.isRoot) {
                // Parse intended parent from nodePath
                std::string intendedParentPath;
                auto slashPos = ov.nodePath.find_last_of('/');
                if (slashPos != std::string::npos) {
                    intendedParentPath = ov.nodePath.substr(0, slashPos);
                }
                // intendedParentPath is empty if node should be direct child of model root
                
                EntityID intendedParent = (EntityID)-1;
                if (intendedParentPath.empty()) {
                    intendedParent = newRoot;
                } else {
                    // Try exact path match first
                    auto itPath = newNodesByPath.find(intendedParentPath);
                    if (itPath != newNodesByPath.end()) {
                        intendedParent = itPath->second;
                    } else {
                        // Fallback to normalized path match
                        auto itNorm = newNodesByNormalizedPath.find(ModelNodeIdentity::NormalizePath(intendedParentPath));
                        if (itNorm != newNodesByNormalizedPath.end()) {
                            intendedParent = itNorm->second;
                        }
                    }
                }
                
                if (intendedParent != (EntityID)-1 && td->Parent != intendedParent) {
                    // Node is not under its intended parent - reparent it
                    scene.SetParent(target, intendedParent);
                    std::cout << "[HotSwap] Reparented '" << td->Name << "' to intended parent path='" << intendedParentPath << "'" << std::endl;
                }
            }
            
            // Apply transforms for ALL nodes to preserve user positioning
            // Both root placement and child node repositioning are preserved
            if (ov.transformModified) {
                td->Transform = ov.transform;
                SanitizeTransform(td->Transform);  // Prevent -INF or NaN from corrupting the scene
                td->Transform.TransformDirty = true;
            }
            
            // Restore entity-level visibility
            td->Visible = ov.entityVisible;
            
            // Apply mesh overrides - charitable to authored model changes while preserving local modifications
            // Principle: The new mesh defines the authoritative slot count; user overrides are applied
            // only to slots that still exist; new slots keep their fresh values from instantiation.
            if (ov.hasMesh && td->Mesh) {
                td->Mesh->material = ov.material;
                
                // The new mesh's slot count is authoritative (may have added/removed slots in DCC)
                const size_t newSlotCount = td->Mesh->materials.size();
                const size_t overrideCount = ov.materials.size();
                const size_t copyCount = std::min(newSlotCount, overrideCount);
                
                // Debug: log slot count changes
                if (newSlotCount != overrideCount) {
                    std::cout << "[HotSwap] Material slot count changed for '" << td->Name << "': "
                              << overrideCount << " -> " << newSlotCount << std::endl;
                }
                
                // Apply user overrides to slots that still exist in both old and new
                for (size_t i = 0; i < copyCount; ++i) {
                    td->Mesh->materials[i] = ov.materials[i];
                }
                // Slots >= copyCount keep their fresh instantiation values (either new slots, or
                // the new mesh already has the right count from instantiation)
                
                // Per-mesh property block (not per-slot)
                td->Mesh->PropertyBlock = ov.propertyBlock;
                td->Mesh->PropertyBlockTexturePaths = ov.propertyBlockTexturePaths;
                
                // Per-slot property blocks: copy overrides for existing slots, expand for new
                td->Mesh->SlotPropertyBlocks.resize(newSlotCount);
                td->Mesh->SlotPropertyBlockTexturePaths.resize(newSlotCount);
                for (size_t i = 0; i < std::min(newSlotCount, ov.slotPropertyBlocks.size()); ++i) {
                    td->Mesh->SlotPropertyBlocks[i] = ov.slotPropertyBlocks[i];
                }
                for (size_t i = 0; i < std::min(newSlotCount, ov.slotPropertyBlockTexturePaths.size()); ++i) {
                    td->Mesh->SlotPropertyBlockTexturePaths[i] = ov.slotPropertyBlockTexturePaths[i];
                }
                // New slots (beyond override count) keep default-constructed empty property blocks
                
                // Rebuild fresh texture handles from stored paths
                RebuildPropertyBlockTexturesFromPathsLocal(td->Mesh->PropertyBlock, td->Mesh->PropertyBlockTexturePaths);
                for (size_t i = 0; i < td->Mesh->SlotPropertyBlocks.size() && i < td->Mesh->SlotPropertyBlockTexturePaths.size(); ++i) {
                    RebuildPropertyBlockTexturesFromPathsLocal(td->Mesh->SlotPropertyBlocks[i], td->Mesh->SlotPropertyBlockTexturePaths[i]);
                }
                
                // Material asset paths: apply overrides for existing slots
                td->Mesh->MaterialAssetPaths.resize(newSlotCount);
                for (size_t i = 0; i < std::min(newSlotCount, ov.materialAssetPaths.size()); ++i) {
                    td->Mesh->MaterialAssetPaths[i] = ov.materialAssetPaths[i];
                }
                // New slots get empty path (default)
                
                td->Mesh->UniqueMaterial = ov.uniqueMaterial;
                td->Mesh->ShowBackfaces = ov.showBackfaces;
                td->Mesh->RenderOnTop = ov.renderOnTop;
                td->Mesh->RenderOrder = ov.renderOrder;
                td->Mesh->BoundsPadding = ov.boundsPadding;
            }
            
            // Apply RenderOverrides (CRITICAL: was missing before!)
            if (ov.hasRenderOverrides) {
                if (!td->RenderOverrides) {
                    td->RenderOverrides = std::make_unique<RenderOverridesComponent>();
                }
                td->RenderOverrides->AlphaBlendEnabled = ov.alphaBlendEnabled;
                td->RenderOverrides->UseAlphaCutout = ov.useAlphaCutout;
                td->RenderOverrides->AlphaCutoutThreshold = ov.alphaCutoutThreshold;
                td->RenderOverrides->CastShadows = ov.castShadows;
                td->RenderOverrides->ReceiveShadows = ov.receiveShadows;
                td->RenderOverrides->Visible = ov.renderOverridesVisible;
                td->RenderOverrides->SortingOrder = ov.sortingOrder;
            }
            
            // Apply blend shape weights (per-mesh blend shapes)
            if (ov.hasBlendShapes && td->BlendShapes) {
                for (auto& shape : td->BlendShapes->Shapes) {
                    auto it = ov.blendShapeWeights.find(shape.Name);
                    if (it != ov.blendShapeWeights.end()) {
                        shape.Weight = it->second;
                    }
                }
            } else if (ov.hasBlendShapes && !td->BlendShapes) {
                // Store pending weights if BlendShapes component isn't populated yet
                td->PendingBlendShapeWeights = ov.blendShapeWeights;
            }
            
            // Apply user-added components
            if (ov.hasLight && ov.light) {
                td->Light = std::make_unique<LightComponent>(*ov.light);
            }
            if (ov.hasCollider && ov.collider) {
                td->Collider = std::make_unique<ColliderComponent>(*ov.collider);
                // Rebuild collider shape if it's a mesh collider
                if (td->Collider->ShapeType == ColliderShape::Mesh && td->Mesh && td->Mesh->mesh) {
                    td->Collider->BuildShape(td->Mesh->mesh.get());
                }
            }
            if (ov.hasRigidBody && ov.rigidBody) {
                td->RigidBody = std::make_unique<RigidBodyComponent>(*ov.rigidBody);
            }
            if (ov.hasStaticBody && ov.staticBody) {
                td->StaticBody = std::make_unique<StaticBodyComponent>(*ov.staticBody);
            }
            if (ov.hasCamera && ov.camera) {
                td->Camera = std::make_unique<CameraComponent>(*ov.camera);
            }
            if (ov.hasEmitter && ov.emitter) {
                td->Emitter = std::make_unique<ParticleEmitterComponent>(*ov.emitter);
                // Invalidate handles - they were destroyed when the old entity was removed.
                // ParticleEmitterSystem will recreate them lazily, and reload sprite from SpritePath.
                td->Emitter->Handle = { uint16_t{UINT16_MAX} };
                td->Emitter->SpriteHandle = { uint16_t{UINT16_MAX} };
                td->Emitter->Uniforms.m_handle = { uint16_t{UINT16_MAX} };
            }
            if (!ov.scripts.empty()) {
                td->Scripts = ov.scripts;
            }
            
            // Apply TintMaskController (tint colors for child meshes)
            if (ov.hasTintController && ov.tintController) {
                td->TintController = std::make_unique<TintMaskController>(*ov.tintController);
                // Remap TintTarget.TargetEntity from old EntityIDs to new EntityIDs via paths
                for (auto& tintTarget : td->TintController->Targets) {
                    if (tintTarget.TargetEntity != (EntityID)-1) {
                        // Find the path for the old EntityID
                        auto oldPathIt = oldEntityIdToPath.find(tintTarget.TargetEntity);
                        if (oldPathIt != oldEntityIdToPath.end()) {
                            // Find the new EntityID for this path
                            const std::string& oldPath = oldPathIt->second;
                            auto newIdIt = newNodesByPath.find(oldPath);
                            if (newIdIt != newNodesByPath.end()) {
                                tintTarget.TargetEntity = newIdIt->second;
                            } else {
                                // Try normalized path
                                std::string normPath = ModelNodeIdentity::NormalizePath(oldPath);
                                auto normIt = newNodesByNormalizedPath.find(normPath);
                                if (normIt != newNodesByNormalizedPath.end()) {
                                    tintTarget.TargetEntity = normIt->second;
                                } else {
                                    std::cout << "[HotSwap] WARNING: Could not remap TintTarget entity for path: " << oldPath << std::endl;
                                    tintTarget.TargetEntity = (EntityID)-1;
                                }
                            }
                        } else {
                            std::cout << "[HotSwap] WARNING: TintTarget entity ID not found in old mapping" << std::endl;
                            tintTarget.TargetEntity = (EntityID)-1;
                        }
                    }
                }
                // Also remap MatchingMeshes (legacy pattern matching)
                for (auto& meshId : td->TintController->MatchingMeshes) {
                    auto oldPathIt = oldEntityIdToPath.find(meshId);
                    if (oldPathIt != oldEntityIdToPath.end()) {
                        auto newIdIt = newNodesByPath.find(oldPathIt->second);
                        if (newIdIt != newNodesByPath.end()) {
                            meshId = newIdIt->second;
                        } else {
                            std::string normPath = ModelNodeIdentity::NormalizePath(oldPathIt->second);
                            auto normIt = newNodesByNormalizedPath.find(normPath);
                            if (normIt != newNodesByNormalizedPath.end()) {
                                meshId = normIt->second;
                            }
                        }
                    }
                }
                td->TintController->NeedsRefresh = true;
            }
            
            // Apply UnifiedMorph (aggregated blend shape weights)
            if (ov.hasUnifiedMorph && ov.unifiedMorph) {
                td->UnifiedMorph = std::make_unique<UnifiedMorphComponent>(*ov.unifiedMorph);
                // Remap MemberMeshes EntityIDs via paths
                for (auto& meshId : td->UnifiedMorph->MemberMeshes) {
                    auto oldPathIt = oldEntityIdToPath.find(meshId);
                    if (oldPathIt != oldEntityIdToPath.end()) {
                        auto newIdIt = newNodesByPath.find(oldPathIt->second);
                        if (newIdIt != newNodesByPath.end()) {
                            meshId = newIdIt->second;
                        } else {
                            std::string normPath = ModelNodeIdentity::NormalizePath(oldPathIt->second);
                            auto normIt = newNodesByNormalizedPath.find(normPath);
                            if (normIt != newNodesByNormalizedPath.end()) {
                                meshId = normIt->second;
                            }
                        }
                    }
                }
            }
            
            // Apply AnimationPlayer (animation state)
            if (ov.hasAnimationPlayer && ov.animationPlayer) {
                td->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>(*ov.animationPlayer);
            }
            
            // Apply BoneAttachment (for non-skinned meshes attached to skeleton bones)
            if (ov.hasBoneAttachment && ov.boneAttachment) {
                td->BoneAttachment = std::make_unique<BoneAttachmentComponent>(*ov.boneAttachment);
                // Remap SkeletonEntity if it was part of this model
                if (td->BoneAttachment->SkeletonEntity != INVALID_ENTITY_ID) {
                    auto oldPathIt = oldEntityIdToPath.find(td->BoneAttachment->SkeletonEntity);
                    if (oldPathIt != oldEntityIdToPath.end()) {
                        auto newIdIt = newNodesByPath.find(oldPathIt->second);
                        if (newIdIt != newNodesByPath.end()) {
                            td->BoneAttachment->SkeletonEntity = newIdIt->second;
                        } else {
                            std::string normPath = ModelNodeIdentity::NormalizePath(oldPathIt->second);
                            auto normIt = newNodesByNormalizedPath.find(normPath);
                            if (normIt != newNodesByNormalizedPath.end()) {
                                td->BoneAttachment->SkeletonEntity = normIt->second;
                            }
                        }
                    }
                }
            }
            
            // Apply LookAt constraints (rotation-only constraints for look/aim behavior)
            if (!ov.lookAtConstraints.empty()) {
                td->LookAtConstraints = ov.lookAtConstraints;
                // Remap TargetEntity references from old EntityIDs to new EntityIDs
                for (auto& lac : td->LookAtConstraints) {
                    if (lac.TargetEntity != 0 && lac.TargetEntity != (EntityID)-1) {
                        auto oldPathIt = oldEntityIdToPath.find(lac.TargetEntity);
                        if (oldPathIt != oldEntityIdToPath.end()) {
                            auto newIdIt = newNodesByPath.find(oldPathIt->second);
                            if (newIdIt != newNodesByPath.end()) {
                                lac.TargetEntity = newIdIt->second;
                            } else {
                                std::string normPath = ModelNodeIdentity::NormalizePath(oldPathIt->second);
                                auto normIt = newNodesByNormalizedPath.find(normPath);
                                if (normIt != newNodesByNormalizedPath.end()) {
                                    lac.TargetEntity = normIt->second;
                                }
                                // If target was outside model (e.g., camera), keep original ID
                            }
                        }
                        // If not in oldEntityIdToPath, the target is likely outside this model subtree
                        // (e.g., a camera entity), so keep the original EntityID
                    }
                }
            }
            
            // Apply IK constraints (positional IK chains)
            if (!ov.iks.empty()) {
                td->IKs = ov.iks;
                // Remap TargetEntity and PoleEntity references from old EntityIDs to new EntityIDs
                for (auto& ik : td->IKs) {
                    if (ik.TargetEntity != 0 && ik.TargetEntity != (EntityID)-1) {
                        auto oldPathIt = oldEntityIdToPath.find(ik.TargetEntity);
                        if (oldPathIt != oldEntityIdToPath.end()) {
                            auto newIdIt = newNodesByPath.find(oldPathIt->second);
                            if (newIdIt != newNodesByPath.end()) {
                                ik.TargetEntity = newIdIt->second;
                            } else {
                                std::string normPath = ModelNodeIdentity::NormalizePath(oldPathIt->second);
                                auto normIt = newNodesByNormalizedPath.find(normPath);
                                if (normIt != newNodesByNormalizedPath.end()) {
                                    ik.TargetEntity = normIt->second;
                                }
                            }
                        }
                    }
                    if (ik.PoleEntity != 0 && ik.PoleEntity != (EntityID)-1) {
                        auto oldPathIt = oldEntityIdToPath.find(ik.PoleEntity);
                        if (oldPathIt != oldEntityIdToPath.end()) {
                            auto newIdIt = newNodesByPath.find(oldPathIt->second);
                            if (newIdIt != newNodesByPath.end()) {
                                ik.PoleEntity = newIdIt->second;
                            } else {
                                std::string normPath = ModelNodeIdentity::NormalizePath(oldPathIt->second);
                                auto normIt = newNodesByNormalizedPath.find(normPath);
                                if (normIt != newNodesByNormalizedPath.end()) {
                                    ik.PoleEntity = normIt->second;
                                }
                            }
                        }
                    }
                }
            }
            
            // Apply metadata
            td->Layer = ov.layer;
            td->Tag = ov.tag;
            if (!ov.extra.empty()) {
                td->Extra = ov.extra;
            }
        }

        std::cout << "[HotSwap] Applied " << appliedCount << "/" << overrides.size() << " override(s)" << std::endl;
        
        // Reparent model nodes that were manually reparented by the user (e.g., sword moved to hand bone)
        // This is different from user-added children - these are original model nodes that were rearranged
        for (const auto& ov : overrides) {
            if (ov.parentPath.empty()) continue; // Not reparented
            
            // Find the target node (the node that was reparented)
            EntityID target = (EntityID)-1;
            {
                auto it = newNodesByPath.find(ov.nodePath);
                if (it != newNodesByPath.end()) target = it->second;
            }
            if (target == (EntityID)-1) {
                auto it = newNodesByNormalizedPath.find(ov.normalizedPath);
                if (it != newNodesByNormalizedPath.end()) target = it->second;
            }
            if (target == (EntityID)-1) continue; // Node not found in new model
            
            // Find the desired parent (where user reparented to)
            EntityID desiredParent = (EntityID)-1;
            {
                auto it = newNodesByPath.find(ov.parentPath);
                if (it != newNodesByPath.end()) desiredParent = it->second;
            }
            if (desiredParent == (EntityID)-1) {
                auto it = newNodesByNormalizedPath.find(ov.parentNormalizedPath);
                if (it != newNodesByNormalizedPath.end()) desiredParent = it->second;
            }
            if (desiredParent == (EntityID)-1) {
                std::cout << "[HotSwap] WARNING: Could not find parent '" << ov.parentPath 
                          << "' for reparented node '" << ov.nodePath << "'" << std::endl;
                continue;
            }
            
            // Check if the node's current parent differs from the desired parent
            auto* td = scene.GetEntityData(target);
            if (td && td->Parent != desiredParent) {
                scene.SetParent(target, desiredParent);
                std::cout << "[HotSwap] Reparented model node '" << ov.nodeName 
                          << "' to '" << ov.parentPath << "'" << std::endl;
            }
        }
        
        // Re-parent user-added children back to their original parent nodes in the new model
        std::cout << "[HotSwap] Re-parenting " << userAddedChildren.size() << " user-added children" << std::endl;
        
        // Debug: print all available paths in new model
        std::cout << "[HotSwap] Available paths in new model:" << std::endl;
        for (const auto& [path, id] : newNodesByPath) {
            std::cout << "  Path: '" << path << "' -> EntityID " << id << std::endl;
        }
        std::cout << "[HotSwap] Available normalized paths in new model:" << std::endl;
        for (const auto& [path, id] : newNodesByNormalizedPath) {
            std::cout << "  NormPath: '" << path << "' -> EntityID " << id << std::endl;
        }
        
        for (const auto& uac : userAddedChildren) {
            EntityID newParent = (EntityID)-1;
            std::string matchMethod = "none";
            
            auto* uacData = scene.GetEntityData(uac.entityId);
            std::cout << "[HotSwap] Processing user-added child: " << (uacData ? uacData->Name : "?")
                      << " (EntityID " << uac.entityId << ")" << std::endl;
            std::cout << "  parentPath: '" << uac.parentPath << "'" << std::endl;
            std::cout << "  parentNormalizedPath: '" << uac.parentNormalizedPath << "'" << std::endl;
            std::cout << "  parentName: '" << uac.parentName << "'" << std::endl;
            
            // Try to find the parent node by path in the new model
            // 1. Try exact path match
            auto pathIt = newNodesByPath.find(uac.parentPath);
            if (pathIt != newNodesByPath.end()) {
                newParent = pathIt->second;
                matchMethod = "exact path";
            }
            // 2. Try normalized path match
            if (newParent == (EntityID)-1) {
                auto normIt = newNodesByNormalizedPath.find(uac.parentNormalizedPath);
                if (normIt != newNodesByNormalizedPath.end()) {
                    newParent = normIt->second;
                    matchMethod = "normalized path";
                }
            }
            // 3. Try name match
            if (newParent == (EntityID)-1) {
                std::string normalizedParentName = normalizeName(uac.parentName);
                auto nameIt = newNodesByNormalizedName.find(normalizedParentName);
                if (nameIt != newNodesByNormalizedName.end()) {
                    newParent = nameIt->second;
                    matchMethod = "name match (" + normalizedParentName + ")";
                }
            }
            // 4. Fallback to new root if parent node was removed/renamed
            if (newParent == (EntityID)-1) {
                newParent = newRoot;
                matchMethod = "FALLBACK to root";
                std::cout << "[HotSwap] WARNING: Could not find parent for user-added child, attaching to root: " 
                          << uac.parentPath << std::endl;
            }
            
            // Re-parent the user-added child
            scene.SetParent(uac.entityId, newParent);
            auto* newParentData = scene.GetEntityData(newParent);
            std::cout << "[HotSwap] Re-parented '" << (uacData ? uacData->Name : "?") 
                      << "' to EntityID " << newParent << " ('" << (newParentData ? newParentData->Name : "?") 
                      << "') via " << matchMethod << std::endl;
        }

        // Force transform update for the whole subtree once
        scene.MarkTransformDirty(newRoot);
        scene.UpdateTransforms();
    }
}

// ---------------------------------------
// Enqueue Model Import (BG job → main-thread callback)
// ---------------------------------------
void AssetPipeline::EnqueueModelImport(const ImportRequest& req) {
    // Run CPU-heavy build in a background job; then marshal to main thread
    Jobs().Enqueue([this, req]{
        try {
            BuiltModelPaths built{};
            if (!EnsureModelCache(req.sourcePath, built)) {
                EnqueueMainThreadTask([cb = req.onReady]{ if (cb) { BuiltModelPaths empty{}; cb(empty); } });
                return;
            }
            EnqueueMainThreadTask([built, cb = req.onReady]{ if (cb) cb(built); });
        } catch (const std::exception& e) {
            EnqueueMainThreadTask([cb = req.onReady, msg = std::string(e.what())]{
                std::cerr << "[AssetPipeline] Import job threw: " << msg << std::endl;
                if (cb) { BuiltModelPaths empty{}; cb(empty); }
            });
        } catch (...) {
            EnqueueMainThreadTask([cb = req.onReady]{
                std::cerr << "[AssetPipeline] Import job threw unknown exception" << std::endl;
                if (cb) { BuiltModelPaths empty{}; cb(empty); }
            });
        }
    });
}

// ---------------------------------------
// TEXTURE IMPORT (cache invalidation only)
// ---------------------------------------
void AssetPipeline::ImportTextureCPU(const std::string& path) {
    // Texture GPU resources are loaded on demand through AcquireTextureHandle.
    // Import only needs to invalidate the live caches, and that work must stay
    // on the main thread because render/editor code may still be touching them.
    EnqueueMainThreadTask([path]() {
        AssetLibrary::Instance().InvalidateTexture(path);
        std::cout << "[AssetPipeline] Invalidated texture for reload: " << path << std::endl;
    });
}

// ---------------------------------------
// SHADER IMPORT (CPU compile -> GPU upload)
// ---------------------------------------
void AssetPipeline::ImportShader(const std::string& path) {
    // Dispatch based on extension: unified .shader vs legacy .sc/.glsl
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".shader") {
        // Unified path: run ShaderImporter to compile both stages and emit meta
        EnqueueMainThreadTask([path]() {
            cm::ShaderImporterContext ctx;
            ctx.projectRoot = std::filesystem::current_path().string();
            ctx.toolsDir = (std::filesystem::current_path() / "tools").string();
            ctx.shadersOutRoot = (std::filesystem::current_path() / "shaders").string();
            ctx.platform = "windows";
            cm::ShaderMeta meta; std::string err;
            if (!cm::ShaderImporter::ImportShader(path, ctx, meta, err)) {
                std::cerr << "[AssetPipeline] Shader import failed: " << err << std::endl;
                return;
            }
            // Invalidate cached program for this base name
            ShaderBundle::Instance().Invalidate(meta.baseName);
            std::cout << "[AssetPipeline] Shader imported: " << path << std::endl;
        });
    } else {
        ShaderType type = ShaderType::Fragment;
        if (path.find("vs_") != std::string::npos) type = ShaderType::Vertex;
        else if (path.find("fs_") != std::string::npos) type = ShaderType::Fragment;
        EnqueueMainThreadTask([path, type]() {
            ShaderManager::Instance().CompileAndCache(path, type);
            std::cout << "[AssetPipeline] Shader compiled and loaded: " << path << std::endl;
        });
    }
}

// ---------------------------------------
// HASH UTILITIES
// ---------------------------------------
std::string AssetPipeline::ComputeFileHash(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    std::string data = contents.str();

    unsigned char result[MD5_DIGEST_LENGTH];
    MD5(reinterpret_cast<const unsigned char*>(data.c_str()), data.size(), result);

    std::ostringstream hex;
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
        hex << std::hex << std::setw(2) << std::setfill('0') << (int)result[i];
    return hex.str();
}

std::string AssetPipeline::ComputeHash(const std::string& path) const {
    auto time = fs::last_write_time(path).time_since_epoch().count();
    return std::to_string(time);
}

// ---------------------------------------
// UTILITIES
// ---------------------------------------
bool AssetPipeline::IsSupportedAsset(const std::string& ext) const {
    static const std::unordered_set<std::string> supported = {
        ".fbx", ".obj", ".gltf", ".glb", // Models
        ".png", ".jpg", ".jpeg", ".tga", // Textures
        ".sc", ".shader", ".glsl",         // Shaders
        ".mat",                                // Materials
        ".cs",
        ".navbin",
        ".ttf", ".otf",                       // Fonts
        ".wav", ".mp3", ".ogg", ".flac",   // Audio
        ".anim", ".animctrl", ".controller", // Animation assets/controllers
        ".asset",                              // Scriptable objects
        ".clayobj",                            // ClayScriptableObject files
        ".ngraph",                            // Node graphs
        ".prefab",                             // Prefabs
        ".json",                               // Authoring prefabs (and other json)
        ".dlglib"                              // Dialogue library files
    };
    std::string normalizedExt = ext;
    std::transform(normalizedExt.begin(), normalizedExt.end(), normalizedExt.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return supported.find(normalizedExt) != supported.end();
}

std::string AssetPipeline::DetermineType(const std::string& ext) {
    std::string normalizedExt = ext;
    std::transform(normalizedExt.begin(), normalizedExt.end(), normalizedExt.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (normalizedExt == ".obj" || normalizedExt == ".fbx" || normalizedExt == ".gltf" || normalizedExt == ".glb") return "model";
    if (normalizedExt == ".png" || normalizedExt == ".jpg" || normalizedExt == ".jpeg" || normalizedExt == ".tga") return "texture";
    if (normalizedExt == ".sc" || normalizedExt == ".shader" || normalizedExt == ".glsl") return "shader";
    if (normalizedExt == ".mat") return "material";
    if (normalizedExt == ".cs") return "script";
    if (normalizedExt == ".navbin") return "navmesh";
    if (normalizedExt == ".ttf" || normalizedExt == ".otf") return "font";
    if (normalizedExt == ".wav" || normalizedExt == ".mp3" || normalizedExt == ".ogg" || normalizedExt == ".flac") return "audio";
    if (normalizedExt == ".anim") return "animation";
    if (normalizedExt == ".animctrl" || normalizedExt == ".controller") return "animatorcontroller";
    if (normalizedExt == ".asset" || normalizedExt == ".clayobj" || normalizedExt == ".dlglib") return "scriptable";
    if (normalizedExt == ".ngraph") return "nodegraph";
    if (normalizedExt == ".prefab") return "prefab";

    return "unknown";
}

std::string AssetPipeline::GetCurrentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void AssetPipeline::CheckAndCompileScriptsAtStartup()
{
    namespace fs = std::filesystem;
    fs::path projectRoot = Project::GetProjectDirectory();
    if (projectRoot.empty() || !fs::exists(projectRoot)) {
        std::cerr << "[Startup] Project root is invalid; cannot resolve scripts.\n";
        return;
    }

    fs::path libDir = projectRoot / ".library";
    fs::create_directories(libDir);
    fs::path scriptsDllPath = libDir / "GameScripts.dll";

    fs::file_time_type newestSourceTime{};
    bool hasScripts = false;
    ComputeNewestProjectScriptWriteTime(projectRoot, newestSourceTime, hasScripts);

    // Expose whether any scripts exist for editor logic (e.g., allowing play without scripts)
    m_HasScripts = hasScripts;
    if (!hasScripts) {
        std::cerr << "[Startup] No .cs scripts found in project. Skipping GameScripts.dll build.\n";
        SetScriptsCompiled(true); // Treat as OK so play mode isn't blocked
        return;
    }

    bool needRebuild = true;
    std::error_code ec;
    if (fs::exists(scriptsDllPath, ec) && !ec) {
        auto dllTime = fs::last_write_time(scriptsDllPath, ec);
        needRebuild = ec || dllTime < newestSourceTime;
    }

    if (!needRebuild) {
        std::cout << "[Startup] Using existing scripts: " << scriptsDllPath << "\n";
        SetScriptsCompiled(true);
        return;
    }

    // Find any script file to trigger the compile
    std::string firstScriptPath;
    ForEachProjectScriptFile(projectRoot, [&](const fs::directory_entry& entry) {
        if (firstScriptPath.empty()) {
            firstScriptPath = entry.path().string();
        }
    });
    if (!firstScriptPath.empty()) {
        std::cout << "[Startup] Compiling scripts to: " << scriptsDllPath << "\n";
        ImportScript(firstScriptPath);
    }
}

bool AssetPipeline::ForceRebuildScripts()
{
    namespace fs = std::filesystem;
    fs::path projectRoot = Project::GetProjectDirectory();
    if (projectRoot.empty() || !fs::exists(projectRoot)) {
        std::cerr << "[Scripts] Project root invalid; cannot rebuild GameScripts.dll\n";
        return false;
    }
    fs::path libDir = projectRoot / ".library";
    std::error_code ec; fs::create_directories(libDir, ec);
    fs::path scriptsDllPath = libDir / "GameScripts.dll";

    std::cout << "[Scripts] Forcing scripts rebuild to: " << scriptsDllPath << "\n";

    // Find any script file to trigger the compile
    std::string anyCs;
    ForEachProjectScriptFile(projectRoot, [&](const fs::directory_entry& entry) {
        if (anyCs.empty()) {
            anyCs = entry.path().string();
        }
    });
    if (anyCs.empty()) {
        std::cerr << "[Scripts] No .cs files found to compile.\n";
        SetScriptsCompiled(false);
        return false;
    }

    ImportScript(anyCs, true);
    return AreScriptsCompiled();
}

// -----------------------------------------------------
// FIXUP GUID REFERENCES in scenes/prefabs after reimport
// -----------------------------------------------------
void AssetPipeline::FixupAssetReferencesByName(const std::string& projectRoot) {
    namespace fs = std::filesystem;
    // Build lookups:
    // - filename (without dir) -> {GUID, virtual path}
    // - filename -> virtual path (for non-GUID assets like textures)
    std::unordered_map<std::string, std::pair<ClaymoreGUID, std::string>> nameToGuidPath;
    std::unordered_map<std::string, std::string> nameToVPath;
    // Walk assets dir
    fs::path assetsDir = fs::path(projectRoot) / "assets";
    if (fs::exists(assetsDir)) {
        for (auto& e : fs::recursive_directory_iterator(assetsDir)) {
            if (!e.is_regular_file()) continue;
            std::string p = e.path().string();
            const AssetMetadata* meta = AssetRegistry::Instance().GetMetadata(p);
            std::string v = p; std::replace(v.begin(), v.end(), '\\', '/');
            auto pos = v.find("assets/"); if (pos != std::string::npos) v = v.substr(pos);
            const std::string fname = e.path().filename().string();
            nameToVPath[fname] = v;
            if (meta && !(meta->guid.high == 0 && meta->guid.low == 0)) {
                nameToGuidPath[fname] = { meta->guid, v };
            }
        }
    }

    auto tryFixFile = [&](const fs::path& path) {
        try {
            std::ifstream in(path.string());
            if (!in.is_open()) return;
            nlohmann::json j; in >> j; in.close();
            bool changed = false;
            // Walk entities -> mesh and material/anim paths
            if (j.contains("entities") && j["entities"].is_array()) {
                for (auto& ent : j["entities"]) {
                    if (ent.contains("mesh") && ent["mesh"].is_object()) {
                        auto& m = ent["mesh"];
                        // If meshReference guid is zero, or guid not known, try resolve by filename from meshPath
                        bool need = false;
                        ClaymoreGUID g{};
                        if (m.contains("meshReference") && m["meshReference"].contains("guid")) {
                            std::string gs = m["meshReference"]["guid"].get<std::string>();
                            g = ClaymoreGUID::FromString(gs);
                            if (g.high == 0 && g.low == 0) need = true;
                        } else need = true;
                        std::string filename;
                        if (m.contains("meshPath")) filename = fs::path(m["meshPath"].get<std::string>()).filename().string();
                        else if (m.contains("meshName")) filename = m["meshName"].get<std::string>();
                        if (need && !filename.empty()) {
                            auto it = nameToGuidPath.find(filename);
                            if (it != nameToGuidPath.end()) {
                                m["meshReference"]["guid"] = it->second.first.ToString();
                                m["meshReference"]["type"] = (int)AssetType::Mesh;
                                if (!m.contains("meshPath")) m["meshPath"] = it->second.second;
                                changed = true;
                            }
                        }

                        // Normalize material texture paths to virtual assets path if missing or invalid
                        auto fixTex = [&](const char* key){
                            if (!m.contains(key)) return;
                            std::string val = m[key].get<std::string>();
                            std::string fname = fs::path(val).filename().string();
                            // If not under assets/ or file not present, map by filename
                            bool needsMap = val.empty() || val.find("assets/") == std::string::npos;
                            if (!needsMap) {
                                // keep
                            } else if (!fname.empty()) {
                                auto it2 = nameToVPath.find(fname);
                                if (it2 != nameToVPath.end()) { m[key] = it2->second; changed = true; }
                            }
                        };
                        fixTex("mat_albedoPath");
                        fixTex("mat_mrPath");
                        fixTex("mat_normalPath");

                        // PropertyBlock texture overrides
                        if (m.contains("propertyBlockTextures") && m["propertyBlockTextures"].is_object()) {
                            for (auto it = m["propertyBlockTextures"].begin(); it != m["propertyBlockTextures"].end(); ++it) {
                                std::string val = it.value().get<std::string>();
                                std::string fname = fs::path(val).filename().string();
                                if (!fname.empty()) {
                                    auto it2 = nameToVPath.find(fname);
                                    if (it2 != nameToVPath.end()) { it.value() = it2->second; changed = true; }
                                }
                            }
                        }
                    }

                    // Animator controller/clip paths normalization
                    if (ent.contains("animator") && ent["animator"].is_object()) {
                        auto& a = ent["animator"];
                        auto normAsset = [&](const char* key){
                            if (!a.contains(key)) return;
                            std::string val = a[key].get<std::string>();
                            std::string fname = fs::path(val).filename().string();
                            if (!fname.empty()) {
                                auto it2 = nameToVPath.find(fname);
                                if (it2 != nameToVPath.end()) { a[key] = it2->second; changed = true; }
                            }
                        };
                        normAsset("controllerPath");
                        normAsset("controllerOverridePath");
                        normAsset("singleClipPath");
                    }
                }
            }
            if (changed) {
                std::ofstream out(path.string());
                out << j.dump(4);
            }
        } catch (...) { }
    };

    // Helper: robust directory walk (skip permission errors)
    auto walkDir = [&](const fs::path& dir, const std::string& ext){
        std::error_code ec;
        if (!fs::exists(dir, ec)) return;
        fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
        while (!ec && it != end) {
            if (it->is_regular_file(ec) && it->path().extension() == ext) tryFixFile(it->path());
            it.increment(ec);
        }
    };

    // Fix scenes under <projectRoot>/scenes
    walkDir(fs::path(projectRoot) / "scenes", ".scene");
    // Fix prefabs under <projectRoot>/assets
    walkDir(fs::path(projectRoot) / "assets", ".prefab");
}

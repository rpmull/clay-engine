#include "UILayer.h"
#include <imgui_internal.h>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <unordered_map>
#include "core/rendering/Renderer.h"
#include "editor/import/ModelLoader.h"
#include <bx/math.h>
#include "core/rendering/ShaderManager.h"
#include "core/rendering/TextureLoader.h"
#include "core/rendering/TerrainGrass.h"
#include "panels/InspectorPanel.h"
#include "Logger.h"
#include "editor/panels/AnimationInspector.h"
#include "editor/animation/AnimationTimelinePanel.h"
#include "core/rendering/MaterialManager.h"
#include "core/rendering/MaterialInstance.h"
#include "core/rendering/PBRMaterial.h"
#include "core/rendering/StandardMeshManager.h"
#include "core/world/RuntimeWorld.h"
#include "utility/ComponentDrawerRegistry.h"
#include "managed/interop/ScriptReflectionSetup.h"
#include "utility/ComponentDrawerSetup.h"
#include "core/utils/TerrainPainter.h"
#include "editor/tools/NavLinkPainter.h"
#include <ecs/debug/TestScript.h>
#include <glm/glm.hpp>
#include <editor/application.h>
#include "core/ecs/EntityData.h"
#include "core/ecs/Components.h"
#include <memory>
#include "imnodes.h"
#include "core/input/Input.h"
#include "core/serialization/Serializer.h"
#include "core/serialization/EntityBinaryLoader.h"
#include "core/assets/BinaryFormats.h"
#include "core/assets/RuntimeAssetResolver.h"
#include "core/assets/ModelRegistry.h"
#include "core/vfs/FileSystem.h"
#include "core/vfs/VirtualFS.h"
#include "core/resources/ResourceManifest.h"
#include "core/rendering/MaterialAsset.h"
#include "core/rendering/MaterialAssetCache.h"
#include "editor/pipeline/EntityBinaryWriter.h"
#include "editor/pipeline/BuildExporter.h"
#include "editor/undo/EditorSceneUndoStack.h"
#include "editor/tools/WorldGraphBake.h"
#include "editor/pipeline/BinaryAssetCache.h"
#include "editor/Project.h"
#include "editor/nodegraph/ShaderGraphMaterial.h"
#include "editor/nodegraph/ShaderGraphSerializer.h"
#include "editor/nodegraph/ShaderGraphCodeGen.h"
#include <nlohmann/json.hpp>
#include <ImGuizmo.h>
#include <navigation/NavDebugDraw.h>
#include "panels/PrefabEditorPanel.h"
#include "panels/CodeEditorPanel.h"
#include "panels/NodeGraphPanel.h"
#include "editor/ui/command/CommandRegistry.h"
#include <imgui_claymore_style.h>
#include "utility/EditorThemeUtils.h"

#include "core/physics/Physics.h"
#include "core/utils/Profiler.h"
#include "core/prefab/PrefabAPI.h"
#include "core/prefab/RuntimePrefabInstantiator.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Friend opener used from MenuBarPanel
extern "C" void OpenSerializerSanityWindow(UILayer* ui) { if (ui) ui->OpenSerializerSanity(); }

// Forward-declare file dialog helpers from MenuBarPanel.cpp
extern std::string ShowOpenFileDialog();
extern std::string ShowSaveFileDialog(const std::string& defaultName);
namespace fs = std::filesystem;
std::vector<std::string> g_RegisteredScriptNames;

namespace {
uint64_t HashCombine64(uint64_t h, uint64_t k)
{
    h ^= k + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

uint64_t HashString64(const std::string& value)
{
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t ComputePlayModeSceneHash(Scene& scene)
{
    try {
        json sceneJson = Serializer::SerializeScene(scene);
        return HashString64(sceneJson.dump());
    } catch (const std::exception& e) {
        std::cerr << "[PlayMode] Failed to hash scene for temporary binary reuse: "
                  << e.what() << "\n";
    } catch (...) {
        std::cerr << "[PlayMode] Failed to hash scene for temporary binary reuse.\n";
    }
    return 0;
}

bool ReadPlayModeSceneStamp(const fs::path& stampPath, uint64_t& outHash)
{
    outHash = 0;
    std::ifstream in(stampPath);
    if (!in.is_open()) {
        return false;
    }

    try {
        json stamp;
        in >> stamp;
        if (stamp.value("sceneVersion", 0u) != binary::SCENE_VERSION) {
            return false;
        }
        outHash = stamp.value("sceneHash", 0ull);
        return outHash != 0;
    } catch (...) {
        return false;
    }
}

void WritePlayModeSceneStamp(const fs::path& stampPath, uint64_t sceneHash)
{
    std::error_code ec;
    fs::create_directories(stampPath.parent_path(), ec);

    std::ofstream out(stampPath, std::ios::trunc);
    if (!out.is_open()) {
        return;
    }

    json stamp;
    stamp["sceneVersion"] = binary::SCENE_VERSION;
    stamp["sceneHash"] = sceneHash;
    out << stamp.dump(2);
}

uint32_t FloatBits(float v)
{
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "float and uint32 size mismatch");
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

struct MaterialEquivalenceKeyInfo
{
    uint64_t Key = 0;
    bool EquivalentSafe = false;
};

MaterialEquivalenceKeyInfo GetMaterialEquivalenceKey(const Material* material)
{
    MaterialEquivalenceKeyInfo info{};
    if (!material) {
        return info;
    }

    info.Key = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(material));
    if (dynamic_cast<const MaterialInstance*>(material) != nullptr) {
        return info;
    }

    if (const auto* pbr = dynamic_cast<const PBRMaterial*>(material)) {
        const PropertyID colorTintId = PropertyID::Get("u_ColorTint");
        const PropertyID tintParamsId = PropertyID::Get("u_TintParams");

        uint64_t h = 1469598103934665603ULL;
        h = HashCombine64(h, static_cast<uint64_t>(material->GetProgram().idx));
        h = HashCombine64(h, static_cast<uint64_t>(material->GetStateFlags()));
        h = HashCombine64(h, static_cast<uint64_t>(pbr->m_AlbedoTex.idx));
        h = HashCombine64(h, static_cast<uint64_t>(pbr->m_MetallicRoughnessTex.idx));
        h = HashCombine64(h, static_cast<uint64_t>(pbr->m_NormalTex.idx));
        h = HashCombine64(h, static_cast<uint64_t>(pbr->m_AOTex.idx));
        h = HashCombine64(h, static_cast<uint64_t>(pbr->m_EmissionTex.idx));
        h = HashCombine64(h, static_cast<uint64_t>(pbr->m_DisplacementTex.idx));
        h = HashCombine64(h, FloatBits(pbr->GetMetallic()));
        h = HashCombine64(h, FloatBits(pbr->GetRoughness()));
        h = HashCombine64(h, FloatBits(pbr->GetNormalScale()));
        h = HashCombine64(h, FloatBits(pbr->GetAmbientOcclusion()));
        h = HashCombine64(h, FloatBits(pbr->GetEmissionStrength()));
        h = HashCombine64(h, FloatBits(pbr->GetDisplacementScale()));

        const glm::vec3 emissionColor = pbr->GetEmissionColor();
        h = HashCombine64(h, FloatBits(emissionColor.x));
        h = HashCombine64(h, FloatBits(emissionColor.y));
        h = HashCombine64(h, FloatBits(emissionColor.z));

        const glm::vec2 uvScale = pbr->GetUVScale();
        const glm::vec2 uvOffset = pbr->GetUVOffset();
        h = HashCombine64(h, FloatBits(uvScale.x));
        h = HashCombine64(h, FloatBits(uvScale.y));
        h = HashCombine64(h, FloatBits(uvOffset.x));
        h = HashCombine64(h, FloatBits(uvOffset.y));

        glm::vec4 tint(1.0f);
        glm::vec4 tintParams(0.0f);
        material->TryGetUniform(colorTintId, tint);
        material->TryGetUniform(tintParamsId, tintParams);
        h = HashCombine64(h, FloatBits(tint.x));
        h = HashCombine64(h, FloatBits(tint.y));
        h = HashCombine64(h, FloatBits(tint.z));
        h = HashCombine64(h, FloatBits(tint.w));
        h = HashCombine64(h, FloatBits(tintParams.x));
        h = HashCombine64(h, FloatBits(tintParams.y));
        h = HashCombine64(h, FloatBits(tintParams.z));
        h = HashCombine64(h, FloatBits(tintParams.w));

        info.Key = h;
        info.EquivalentSafe = true;
    }

    return info;
}

ImVec4 LerpImVec4(const ImVec4& a, const ImVec4& b, float t)
{
    return ImVec4(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t
    );
}

ImVec4 WithAlpha(const ImVec4& color, float alpha)
{
    return ImVec4(color.x, color.y, color.z, alpha);
}

struct ProcessWindowSearch {
    DWORD processId = 0;
    HWND window = nullptr;
};

BOOL CALLBACK FindTopLevelProcessWindow(HWND hwnd, LPARAM lParam) {
    auto* search = reinterpret_cast<ProcessWindowSearch*>(lParam);
    if (!search || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return TRUE;
    }

    DWORD windowProcessId = 0;
    GetWindowThreadProcessId(hwnd, &windowProcessId);
    if (windowProcessId != search->processId) {
        return TRUE;
    }

    if (GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }

    search->window = hwnd;
    return FALSE;
}

size_t CountPendingPlayPrefabLoads(Scene& scene, size_t* outFailed = nullptr)
{
    size_t pending = 0;
    size_t failed = 0;
    for (const auto& entity : scene.GetEntities()) {
        auto* data = scene.GetEntityData(entity.GetID());
        if (!data) {
            continue;
        }
        if (data->PrefabAsyncPending) {
            ++pending;
        }
        if (data->PrefabAsyncFailed) {
            ++failed;
        }
    }
    if (outFailed) {
        *outFailed = failed;
    }
    return pending;
}

bool FinalizePlayClonePrefabLoads(Scene& scene)
{
    constexpr auto kMaxWait = std::chrono::seconds(5);
    constexpr int kMaxIterations = 4096;

    size_t failed = 0;
    size_t pending = CountPendingPlayPrefabLoads(scene, &failed);
    if (pending == 0) {
        return failed == 0;
    }

    std::cout << "[PlayMode] Waiting for " << pending
              << " pending prefab load(s) before cloning play scene..." << std::endl;

    const auto start = std::chrono::steady_clock::now();
    size_t lastPending = pending;

    for (int iteration = 0; iteration < kMaxIterations && pending > 0; ++iteration) {
        runtime::RuntimePrefabInstantiator::UpdateAsync(4.0);
        scene.ProcessPendingCreations();
        scene.ProcessPendingRemovals();

        pending = CountPendingPlayPrefabLoads(scene, &failed);
        if (pending == 0) {
            break;
        }

        if (pending != lastPending) {
            std::cout << "[PlayMode] Pending prefab loads remaining: " << pending << std::endl;
            lastPending = pending;
        }

        if (std::chrono::steady_clock::now() - start > kMaxWait) {
            std::cerr << "[PlayMode] WARNING: Timed out waiting for pending prefab loads; "
                      << pending << " root(s) still pending." << std::endl;
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    pending = CountPendingPlayPrefabLoads(scene, &failed);
    if (failed > 0) {
        std::cerr << "[PlayMode] WARNING: " << failed
                  << " prefab root(s) failed while preparing play scene." << std::endl;
    }

    return pending == 0 && failed == 0;
}

bool PrepareSceneForPlayClone(Scene& scene, const char* sourceLabel)
{
    scene.ProcessPendingCreations();
    scene.ProcessPendingRemovals();

    const bool prefabsReady = FinalizePlayClonePrefabLoads(scene);

    scene.ProcessPendingCreations();
    scene.ProcessPendingRemovals();

    // Match runtime/export startup so play mode clones from fully resolved authoring state.
    scene.UpdateTransforms();
    scene.ResolveScriptEntityReferencesFromMetadata();

    if (!prefabsReady) {
        std::cerr << "[PlayMode] WARNING: " << sourceLabel
                  << " scene entered clone prep with unresolved prefab work." << std::endl;
    }
    return prefabsReady;
}

HWND FindRuntimePreviewWindow(DWORD processId) {
    if (processId == 0) {
        return nullptr;
    }

    ProcessWindowSearch search{};
    search.processId = processId;
    EnumWindows(&FindTopLevelProcessWindow, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

std::string GetProjectNameOrFallback() {
    std::string projName = Project::GetProjectName();
    if (!projName.empty()) {
        return projName;
    }

    const fs::path projDir = Project::GetProjectDirectory();
    if (!projDir.empty()) {
        projName = projDir.filename().string();
    }

    return projName.empty() ? "Game" : projName;
}

std::string NormalizeProjectPath(const std::string& path)
{
    if (path.empty()) {
        return {};
    }

    fs::path normalizedPath = fs::path(IVirtualFS::NormalizePath(path));
    if (normalizedPath.is_relative()) {
        const fs::path projectRoot = Project::GetProjectDirectory();
        if (!projectRoot.empty()) {
            normalizedPath = projectRoot / normalizedPath;
        }
    }

    std::error_code ec;
    normalizedPath = normalizedPath.lexically_normal();
    if (fs::exists(normalizedPath, ec)) {
        fs::path weak = fs::weakly_canonical(normalizedPath, ec);
        if (!ec) {
            normalizedPath = weak;
        }
    }

    return normalizedPath.generic_string();
}

struct ShaderGraphDependencySnapshot
{
    std::string normalizedGraphPath;
    shadergraph::ShaderGraph graph;
    shadergraph::ShaderCompileResult compileResult;
};

std::vector<shadergraph::MaterialParameter> BuildSyncedShaderGraphParameters(
    const std::vector<shadergraph::MaterialParameter>& existingParameters,
    const std::vector<shadergraph::ShaderCompileResult::Parameter>& definitions)
{
    std::unordered_map<std::string, shadergraph::MaterialParameter> existingByName;
    existingByName.reserve(existingParameters.size());
    for (const auto& parameter : existingParameters) {
        existingByName[parameter.name] = parameter;
    }

    std::vector<shadergraph::MaterialParameter> synced;
    synced.reserve(definitions.size());
    for (const auto& definition : definitions) {
        shadergraph::MaterialParameter parameter;
        parameter.name = definition.name;
        parameter.displayName = definition.displayName;
        parameter.type = definition.type;
        parameter.textureSlot = definition.textureSlot;
        parameter.value = definition.defaultValue;
        parameter.texturePath = definition.texturePath;

        auto existingIt = existingByName.find(definition.name);
        if (existingIt != existingByName.end()) {
            parameter.value = existingIt->second.value;
            if (!existingIt->second.texturePath.empty()) {
                parameter.texturePath = existingIt->second.texturePath;
            }
        }

        synced.push_back(std::move(parameter));
    }

    return synced;
}

bool BuildShaderGraphDependencySnapshot(const std::string& shaderGraphPath, ShaderGraphDependencySnapshot& snapshot)
{
    snapshot = ShaderGraphDependencySnapshot{};
    snapshot.normalizedGraphPath = NormalizeProjectPath(shaderGraphPath);
    if (snapshot.normalizedGraphPath.empty()) {
        return false;
    }

    if (!shadergraph::ShaderGraphSerializer::LoadFromFile(snapshot.normalizedGraphPath, snapshot.graph)) {
        return false;
    }

    shadergraph::ShaderGraphCodeGen codegen(snapshot.graph);
    snapshot.compileResult = codegen.Compile();
    return snapshot.compileResult.success;
}

bool RefreshShaderGraphMaterialAsset(const fs::path& assetPath, const ShaderGraphDependencySnapshot& snapshot)
{
    shadergraph::ShaderGraphMaterialDesc desc;
    if (!shadergraph::LoadShaderGraphMaterial(assetPath.string(), desc)) {
        return false;
    }

    if (NormalizeProjectPath(desc.shaderGraphPath) != snapshot.normalizedGraphPath) {
        return false;
    }

    desc.shaderGraphPath = snapshot.normalizedGraphPath;
    desc.vertexShaderName = snapshot.graph.compiledVSName;
    desc.fragmentShaderName = snapshot.graph.compiledFSName;
    desc.parameters = BuildSyncedShaderGraphParameters(desc.parameters, snapshot.compileResult.parameters);
    return shadergraph::SaveShaderGraphMaterial(assetPath.string(), desc);
}

bool RefreshMaterialAssetFromShaderGraph(const fs::path& assetPath, const ShaderGraphDependencySnapshot& snapshot)
{
    MaterialAssetDesc desc;
    if (!LoadMaterialAsset(assetPath.string(), desc)) {
        return false;
    }

    if (NormalizeProjectPath(desc.shaderGraphPath) != snapshot.normalizedGraphPath) {
        return false;
    }

    desc.shaderGraphPath = snapshot.normalizedGraphPath;

    std::unordered_map<std::string, glm::vec4> oldVec4Uniforms = desc.vec4Uniforms;
    std::unordered_map<std::string, std::string> oldTextureUniforms = desc.textureUniforms;
    desc.vec4Uniforms.clear();
    desc.textureUniforms.clear();

    for (const auto& parameter : snapshot.compileResult.parameters) {
        if (parameter.type == shadergraph::ShaderValueType::Texture2D) {
            auto oldTextureIt = oldTextureUniforms.find(parameter.name);
            desc.textureUniforms[parameter.name] = (oldTextureIt != oldTextureUniforms.end())
                ? oldTextureIt->second
                : parameter.texturePath;
        } else {
            auto oldUniformIt = oldVec4Uniforms.find(parameter.name);
            desc.vec4Uniforms[parameter.name] = (oldUniformIt != oldVec4Uniforms.end())
                ? oldUniformIt->second
                : parameter.defaultValue;
        }
    }

    if (!SaveMaterialAsset(assetPath.string(), desc)) {
        return false;
    }

    MaterialAssetCache::Invalidate(assetPath.string());
    return true;
}

bool MaterialAssetReferencesShaderGraph(const std::string& assetPath, const std::string& normalizedGraphPath)
{
    if (assetPath.empty()) {
        return false;
    }

    std::string ext = fs::path(assetPath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (ext == ".sgmat") {
        shadergraph::ShaderGraphMaterialDesc desc;
        return shadergraph::LoadShaderGraphMaterial(assetPath, desc) &&
            NormalizeProjectPath(desc.shaderGraphPath) == normalizedGraphPath;
    }

    if (ext == ".mat") {
        MaterialAssetDesc desc;
        return LoadMaterialAsset(assetPath, desc) &&
            NormalizeProjectPath(desc.shaderGraphPath) == normalizedGraphPath;
    }

    return false;
}

std::shared_ptr<Material> CreateMaterialFromAssetFile(const std::string& assetPath)
{
    if (assetPath.empty()) {
        return nullptr;
    }

    std::string ext = fs::path(assetPath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (ext == ".sgmat") {
        shadergraph::ShaderGraphMaterialDesc desc;
        if (!shadergraph::LoadShaderGraphMaterial(assetPath, desc)) {
            return nullptr;
        }
        return shadergraph::ShaderGraphMaterial::CreateFromDesc(desc);
    }

    if (ext == ".mat") {
        MaterialAssetDesc desc;
        if (!LoadMaterialAsset(assetPath, desc)) {
            return nullptr;
        }
        MaterialAssetCache::Invalidate(assetPath);
        return CreateMaterialFromAsset(desc);
    }

    return nullptr;
}
}

// =============================
// Constructor / Initialization
// =============================
UILayer::UILayer()
    : m_InspectorPanel(&m_Scene, &m_SelectedEntity),
      m_ProjectPanel(&m_Scene, this),
      m_ViewportPanel(m_Scene, &m_SelectedEntity),
      m_SceneHierarchyPanel(&m_Scene, &m_SelectedEntity),
      m_MenuBarPanel(&m_Scene, &m_SelectedEntity, &m_ProjectPanel, this),
      m_AvatarBuilderPanel(&m_Scene),
      m_WorldGenPanel(&m_Scene, &m_SelectedEntity),
      m_TerrainEvolutionPanel(&m_Scene, &m_SelectedEntity),
      m_RiverCutterPanel(&m_Scene, &m_SelectedEntity),
      m_SplineToolPanel(&m_Scene, &m_SelectedEntity),
      m_SoftbodyPainter(&m_Scene, &m_SelectedEntity),
      m_SplatmapGeneratorPanel(&m_Scene, &m_SelectedEntity),
      m_ResourceLayerPanel(&m_Scene, &m_SelectedEntity),
      m_IconGeneratorPanel(this),
      m_UIPrefabLayoutDesignerPanel(&m_Scene, this)
{
    m_ToolbarPanel = ToolbarPanel(this);
    // Initialize global ImNodes context once
    ImNodes::CreateContext();

    Logger::SetCallback([this](const std::string& msg, LogLevel level) {
        m_ConsolePanel.AddLog(msg, level);
        if (level == LogLevel::Warning) {
            m_Notifications.Push(editorui::EditorNotificationLevel::Warning, msg, 4.8f);
        } else if (level == LogLevel::Error) {
            m_FocusConsoleNextFrame = true;
            m_Notifications.Push(editorui::EditorNotificationLevel::Error, msg, 6.5f);
        }
    });

    m_LayoutInitialized = false;
    RegisterComponentDrawers();
    RegisterSampleScriptProperties();

    // Register primitive meshes with AssetLibrary
    StandardMeshManager::Instance().RegisterPrimitiveMeshes();

    CreateDebugCubeEntity();
    CreateDefaultLight();

    m_AnimationInspector = std::make_unique<AnimationInspectorPanel>(this);
    m_AnimGraphPanel.SetInspectorPanel(&m_InspectorPanel);
    m_InspectorPanel.SetAnimatorControllerEditor([this](const std::string& path){
        this->OpenAnimatorController(path);
    });
    // Register console commands once
    CommandRegistry::Instance().RegisterBuiltins();

    // Wire hierarchy prefab open callback
    m_SceneHierarchyPanel.SetOpenPrefabCallback([this](const std::string& path){
        if (!path.empty()) this->OpenPrefabEditor(path);
    });
    
    // Set UILayer context so hierarchy panel can detect prefab edit mode
    m_SceneHierarchyPanel.SetUILayerContext(this);

    m_ShaderGraphPanel.SetGraphSavedCallback([this](const std::string& path) {
        this->RefreshShaderGraphDependencies(path);
    });

    Renderer::Get().SetEditorLightingOverride(false);
    EditorSceneUndoStack::Get().Bind(&m_Scene, &m_SelectedEntity);
    RegisterEditorActions();
    RegisterEditorContextMenus();
}

UILayer::~UILayer() {
    // Free panel-level ImNodes editor contexts before destroying the global context.
    m_NodeGraphPanel.ReleaseEditorContext();
    // Destroy global ImNodes context
    ImNodes::DestroyContext();
}

void UILayer::StampEditorCameraMetadata(Scene& scene) {
    ViewportPanel::ViewportCameraState snapshot = m_ViewportPanel.GetCameraState();
    Scene::EditorViewportState stored;
    stored.Target = snapshot.Target;
    stored.Distance = snapshot.Distance;
    stored.Yaw = snapshot.Yaw;
    stored.Pitch = snapshot.Pitch;
    stored.FieldOfView = snapshot.FieldOfView;
    stored.NearClip = snapshot.NearClip;
    stored.FarClip = snapshot.FarClip;
    scene.SetEditorViewportState(stored);
}

void UILayer::ApplyEditorCameraState(Scene& scene) {
    if (!scene.HasEditorViewportState()) {
        // Reset to default camera state when scene has no saved camera position
        m_ViewportPanel.ResetToDefaultCameraState();
        return;
    }
    const Scene::EditorViewportState& stored = scene.GetEditorViewportState();
    ViewportPanel::ViewportCameraState snapshot;
    snapshot.Target = stored.Target;
    snapshot.Distance = stored.Distance;
    snapshot.Yaw = stored.Yaw;
    snapshot.Pitch = stored.Pitch;
    snapshot.FieldOfView = stored.FieldOfView;
    snapshot.NearClip = stored.NearClip;
    snapshot.FarClip = stored.FarClip;
    m_ViewportPanel.ApplyCameraState(snapshot);
}

void UILayer::ResetEditorCamera() {
    m_ViewportPanel.ResetToDefaultCameraState();
}
void UILayer::RequestLayoutReset() {
    ImGui::ClearIniSettings();
    m_EditorPreferences.SetLayoutIni("");
    m_ResetLayoutRequested = true;
    m_LayoutInitialized = false;
    m_EnforcedTabsOnce = false;
    m_Notifications.Push(editorui::EditorNotificationLevel::Info, "Workspace layout reset.");
}

void UILayer::OpenCommandPalette() {
    m_CommandPalette.Open();
}

bool UILayer::ExecuteEditorAction(const std::string& id) {
    return m_ActionRegistry.Execute(id);
}

bool UILayer::SetEditorActionChecked(const std::string& id, bool checked) {
    return m_ActionRegistry.SetChecked(id, checked);
}

const editorui::EditorActionDefinition* UILayer::FindEditorAction(const std::string& id) const {
    return m_ActionRegistry.Find(id);
}

std::vector<const editorui::EditorActionDefinition*> UILayer::GetEditorActionsForCategory(const std::string& category) const {
    return m_ActionRegistry.GetActionsForCategory(category);
}

void UILayer::QueueNotification(editorui::EditorNotificationLevel level, const std::string& message, float lifetimeSeconds) {
    m_Notifications.Push(level, message, lifetimeSeconds);
}

void UILayer::RegisterEditorActions() {
    m_ActionRegistry.Clear();

    m_ActionRegistry.Register({
        "workspace.command_palette",
        "Command Palette",
        "Workspace",
        "Ctrl+Shift+P",
        "search actions windows tools palette",
        editorui::EditorActionType::Command,
        0,
        true,
        false,
        { ImGuiKey_P, true, true, false, false, false },
        [this]() { OpenCommandPalette(); }
    });

    m_ActionRegistry.Register({
        "document.save_active",
        "Save Active Document",
        "File",
        "Ctrl+S",
        "save document scene prefab code",
        editorui::EditorActionType::Command,
        10,
        true,
        false,
        { ImGuiKey_S, true, false, false, false, false },
        [this]() { SaveActiveDocument(); },
        [this]() {
            return !m_PlayMode &&
                (GetFocusedPrefabEditor() != nullptr ||
                 GetFocusedCodeEditor() != nullptr ||
                 m_NodeGraphPanel.IsWindowFocusedOrHovered() ||
                 m_AnimGraphPanel.IsWindowFocusedOrHovered() ||
                 m_ShaderGraphPanel.IsWindowFocusedOrHovered() ||
                 m_DialogueEditorPanel.IsWindowFocusedOrHovered() ||
                 m_QuestEditorPanel.IsWindowFocusedOrHovered() ||
                 m_ViewportPanel.IsWindowFocusedOrHovered());
        }
    });

    m_ActionRegistry.Register({
        "document.save_active_as",
        "Save Active Document As",
        "File",
        "Ctrl+Shift+S",
        "save as scene",
        editorui::EditorActionType::Command,
        11,
        true,
        false,
        { ImGuiKey_S, true, true, false, false, false },
        [this]() { SaveActiveDocumentAs(); },
        [this]() {
            return !m_PlayMode &&
                (m_NodeGraphPanel.IsWindowFocusedOrHovered() ||
                 m_AnimGraphPanel.IsWindowFocusedOrHovered() ||
                 m_ShaderGraphPanel.IsWindowFocusedOrHovered() ||
                 m_DialogueEditorPanel.IsWindowFocusedOrHovered() ||
                 m_QuestEditorPanel.IsWindowFocusedOrHovered() ||
                 m_ViewportPanel.IsWindowFocusedOrHovered());
        }
    });

    m_ActionRegistry.Register({
        "scene.new",
        "New Scene",
        "File",
        "Ctrl+N",
        "new scene authoring level",
        editorui::EditorActionType::Command,
        20,
        true,
        false,
        { ImGuiKey_N, true, false, false, false, false },
        [this]() { NewScene(); },
        [this]() { return !m_PlayMode && m_ViewportPanel.IsWindowFocusedOrHovered(); }
    });

    m_ActionRegistry.Register({
        "scene.open",
        "Open Scene",
        "File",
        "Ctrl+O",
        "load scene level authoring",
        editorui::EditorActionType::Command,
        21,
        true,
        false,
        { ImGuiKey_O, true, false, false, false, false },
        [this]() { PromptLoadScene(); },
        [this]() { return !m_PlayMode && m_ViewportPanel.IsWindowFocusedOrHovered(); }
    });

    m_ActionRegistry.Register({
        "scene.save",
        "Save Scene",
        "File",
        "",
        "save current scene",
        editorui::EditorActionType::Command,
        22,
        true,
        false,
        {},
        [this]() { SaveCurrentScene(false); }
    });

    m_ActionRegistry.Register({
        "scene.save_as",
        "Save Scene As",
        "File",
        "",
        "save scene as",
        editorui::EditorActionType::Command,
        23,
        true,
        false,
        {},
        [this]() { SaveCurrentScene(true); }
    });

    m_ActionRegistry.Register({
        "workspace.project_settings",
        "Project Settings",
        "Workspace",
        "Ctrl+,",
        "project settings preferences modules appearance",
        editorui::EditorActionType::Command,
        30,
        true,
        false,
        { ImGuiKey_Comma, true, false, false, false, false },
        [this]() { PromptProjectSettings(); }
    });

    m_ActionRegistry.Register({
        "build.export_standalone",
        "Export Standalone",
        "Build",
        "Ctrl+Shift+B",
        "build export standalone pak runtime",
        editorui::EditorActionType::Command,
        40,
        true,
        false,
        { ImGuiKey_B, true, true, false, false, false },
        [this]() { m_MenuBarPanel.OpenExportDialog(); }
    });

    m_ActionRegistry.Register({
        "workspace.reset_layout",
        "Reset Layout",
        "Workspace",
        "Ctrl+Shift+R",
        "reset workspace layout docking",
        editorui::EditorActionType::Command,
        50,
        true,
        false,
        { ImGuiKey_R, true, true, false, false, false },
        [this]() { RequestLayoutReset(); }
    });

    m_ActionRegistry.Register({
        "workspace.ui_scale_increase",
        "Increase UI Scale",
        "Workspace",
        "Ctrl+=",
        "dpi ui scale larger",
        editorui::EditorActionType::Command,
        51,
        true,
        false,
        { ImGuiKey_Equal, true, false, false, false, false },
        [this]() { AdjustUIScale(0.1f); }
    });

    m_ActionRegistry.Register({
        "workspace.ui_scale_decrease",
        "Decrease UI Scale",
        "Workspace",
        "Ctrl+-",
        "dpi ui scale smaller",
        editorui::EditorActionType::Command,
        52,
        true,
        false,
        { ImGuiKey_Minus, true, false, false, false, false },
        [this]() { AdjustUIScale(-0.1f); }
    });

    m_ActionRegistry.Register({
        "play.toggle",
        "Toggle Play Mode",
        "Runtime",
        "Ctrl+P",
        "play stop preview runtime",
        editorui::EditorActionType::Command,
        60,
        true,
        false,
        { ImGuiKey_P, true, false, false, false, false },
        [this]() { m_ToolbarPanel.TogglePlayMode(); }
    });

    auto registerWindow = [this](const char* id,
                                 const char* label,
                                 const char* category,
                                 int sortOrder,
                                 std::function<void()> openFn,
                                 std::function<bool()> isOpenFn,
                                 std::function<void(bool)> setOpenFn = {}) {
        m_ActionRegistry.Register({
            id,
            label,
            category,
            "",
            std::string("open ") + label,
            editorui::EditorActionType::Window,
            sortOrder,
            true,
            true,
            {},
            std::move(openFn),
            {},
            std::move(isOpenFn),
            std::move(setOpenFn)
        });
    };

    registerWindow("window.animation_controller", "Animation Controller", "Window", 100,
        [this]() { m_AnimCtrlPanel.Open(); },
        [this]() { return m_AnimCtrlPanel.IsOpen(); },
        [this](bool open) { m_AnimCtrlPanel.SetOpen(open); });
    registerWindow("window.animation_graph", "Animation Graph", "Window", 101,
        [this]() { m_AnimGraphPanel.Open(); },
        [this]() { return m_AnimGraphPanel.IsOpen(); },
        [this](bool open) { m_AnimGraphPanel.SetOpen(open); });
    registerWindow("window.animation_timeline", "Animation Timeline", "Window", 102,
        [this]() { m_AnimTimelinePanel.Open(); },
        [this]() { return m_AnimTimelinePanel.IsOpen(); },
        [this](bool open) { m_AnimTimelinePanel.SetOpen(open); });
    registerWindow("window.node_graph", "Node Graph", "Window", 103,
        [this]() { m_NodeGraphPanel.Open(); },
        [this]() { return m_NodeGraphPanel.IsOpen(); },
        [this](bool open) { m_NodeGraphPanel.SetOpen(open); });
    registerWindow("window.shader_graph", "Shader Graph", "Window", 104,
        [this]() { m_ShaderGraphPanel.Open(); },
        [this]() { return m_ShaderGraphPanel.IsOpen(); },
        [this](bool open) { m_ShaderGraphPanel.SetOpen(open); });
    registerWindow("window.dialogue_editor", "Dialogue Editor", "Window", 105,
        [this]() { m_DialogueEditorPanel.Open(); },
        [this]() { return m_DialogueEditorPanel.IsOpen(); },
        [this](bool open) { m_DialogueEditorPanel.SetOpen(open); });
    registerWindow("window.quest_editor", "Quest Editor", "Window", 106,
        [this]() { m_QuestEditorPanel.Open(); },
        [this]() { return m_QuestEditorPanel.IsOpen(); },
        [this](bool open) { m_QuestEditorPanel.SetOpen(open); });

    m_ActionRegistry.Register({
        "window.script_registry",
        "Script Registry",
        "Window",
        "",
        "scripts registry reflection",
        editorui::EditorActionType::Window,
        110,
        true,
        true,
        {},
        [this]() { m_ShowScriptRegistry = !m_ShowScriptRegistry; },
        {},
        [this]() { return m_ShowScriptRegistry; },
        [this](bool open) { m_ShowScriptRegistry = open; }
    });

    m_ActionRegistry.Register({
        "window.asset_registry",
        "Asset Registry",
        "Window",
        "",
        "assets registry metadata",
        editorui::EditorActionType::Window,
        111,
        true,
        true,
        {},
        [this]() { m_ShowAssetRegistry = !m_ShowAssetRegistry; },
        {},
        [this]() { return m_ShowAssetRegistry; },
        [this](bool open) { m_ShowAssetRegistry = open; }
    });

    registerWindow("tool.profiler", "Profiler", "Tool", 200,
        [this]() { m_ProfilerPanel.Open(); },
        [this]() { return m_ProfilerPanel.IsOpen(); },
        [this](bool open) { m_ProfilerPanel.SetOpen(open); });
    registerWindow("tool.world_generation", "World Generation", "Tool", 201,
        [this]() { m_WorldGenPanel.Open(); },
        [this]() { return m_WorldGenPanel.IsOpen(); },
        [this](bool open) { m_WorldGenPanel.SetOpen(open); });
    registerWindow("tool.terrain_evolution", "Terrain Evolution", "Tool", 202,
        [this]() { m_TerrainEvolutionPanel.Open(); },
        [this]() { return m_TerrainEvolutionPanel.IsOpen(); },
        [this](bool open) { m_TerrainEvolutionPanel.SetOpen(open); });
    registerWindow("tool.river_cutter", "River Cutter", "Tool", 203,
        [this]() { m_RiverCutterPanel.Open(); },
        [this]() { return m_RiverCutterPanel.IsOpen(); },
        [this](bool open) { m_RiverCutterPanel.SetOpen(open); });
    registerWindow("tool.splatmap_generator", "Splatmap Generator", "Tool", 204,
        [this]() { m_SplatmapGeneratorPanel.Open(); },
        [this]() { return m_SplatmapGeneratorPanel.IsOpen(); },
        [this](bool open) { m_SplatmapGeneratorPanel.SetOpen(open); });
    registerWindow("tool.resource_layers", "Resource Layers", "Tool", 205,
        [this]() { m_ResourceLayerPanel.Open(); },
        [this]() { return m_ResourceLayerPanel.IsOpen(); },
        [this](bool open) { m_ResourceLayerPanel.SetOpen(open); });
    registerWindow("tool.texture_cleanup", "Texture Cleanup", "Tool", 206,
        [this]() { m_TextureCleanupPanel.Open(); },
        [this]() { return m_TextureCleanupPanel.IsOpen(); },
        [this](bool open) { m_TextureCleanupPanel.SetOpen(open); });
    registerWindow("tool.icon_generator", "Icon Generator", "Tool", 207,
        [this]() { m_IconGeneratorPanel.Open(); },
        [this]() { return m_IconGeneratorPanel.IsOpen(); },
        [this](bool open) { m_IconGeneratorPanel.SetOpen(open); });
    registerWindow("tool.ui_prefab_layout_designer", "UI Prefab Layout Designer", "Tool", 208,
        [this]() { m_UIPrefabLayoutDesignerPanel.Open(); },
        [this]() { return m_UIPrefabLayoutDesignerPanel.IsOpen(); },
        [this](bool open) { m_UIPrefabLayoutDesignerPanel.SetOpen(open); });
    registerWindow("tool.serializer_test", "Serializer Test", "Tool", 209,
        [this]() { m_SerializerSanityWindow.Open(); },
        [this]() { return m_SerializerSanityWindow.IsOpen(); },
        [this](bool open) { m_SerializerSanityWindow.SetOpen(open); });
}

void UILayer::RegisterEditorContextMenus() {
    m_ContextMenuRegistry.Clear();

    m_ContextMenuRegistry.RegisterProjectItem("project.copy_asset_path", 100, [this](const editorui::ProjectItemContext& context) {
        if (context.Path.empty()) {
            return false;
        }
        if (ImGui::MenuItem("Copy Asset Path")) {
            ImGui::SetClipboardText(context.Path.c_str());
            QueueNotification(editorui::EditorNotificationLevel::Info, "Copied asset path.", 1.8f);
        }
        return true;
    });

    m_ContextMenuRegistry.RegisterProjectBackground("project.copy_folder_path", 100, [this](const editorui::ProjectBackgroundContext& context) {
        if (context.FolderPath.empty()) {
            return false;
        }
        if (ImGui::MenuItem("Copy Folder Path")) {
            ImGui::SetClipboardText(context.FolderPath.c_str());
            QueueNotification(editorui::EditorNotificationLevel::Info, "Copied folder path.", 1.8f);
        }
        return true;
    });

    m_ContextMenuRegistry.RegisterHierarchyEntity("hierarchy.copy_entity_name", 100, [this](const editorui::HierarchyEntityContext& context) {
        if (!context.Context || context.Entity == INVALID_ENTITY_ID) {
            return false;
        }
        EntityData* data = context.Context->GetEntityData(context.Entity);
        if (!data || data->Name.empty()) {
            return false;
        }
        if (ImGui::MenuItem("Copy Entity Name")) {
            ImGui::SetClipboardText(data->Name.c_str());
            QueueNotification(editorui::EditorNotificationLevel::Info, "Copied entity name.", 1.8f);
        }
        return true;
    });

    m_ContextMenuRegistry.RegisterHierarchyEntity("hierarchy.copy_entity_id", 101, [this](const editorui::HierarchyEntityContext& context) {
        if (!context.Context || context.Entity == INVALID_ENTITY_ID) {
            return false;
        }
        const std::string entityId = std::to_string(context.Entity);
        if (ImGui::MenuItem("Copy Entity ID")) {
            ImGui::SetClipboardText(entityId.c_str());
            QueueNotification(editorui::EditorNotificationLevel::Info, "Copied entity ID.", 1.8f);
        }
        return true;
    });

    m_ContextMenuRegistry.RegisterHierarchyBackground("hierarchy.copy_scene_name", 100, [this](const editorui::HierarchyBackgroundContext& context) {
        if (!context.Context) {
            return false;
        }
        const std::string sceneName = m_CachedSceneTitle.empty() ? std::string("Scene") : m_CachedSceneTitle;
        if (ImGui::MenuItem("Copy Scene Name")) {
            ImGui::SetClipboardText(sceneName.c_str());
            QueueNotification(editorui::EditorNotificationLevel::Info, "Copied scene name.", 1.8f);
        }
        return true;
    });
}

float UILayer::GetBaseEditorFontSize() const {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.FontDefault) {
        return io.FontDefault->LegacySize > 0.0f ? io.FontDefault->LegacySize : ImGui::GetStyle().FontSizeBase;
    }
    if (io.Fonts && !io.Fonts->Fonts.empty() && io.Fonts->Fonts[0]) {
        return io.Fonts->Fonts[0]->LegacySize > 0.0f ? io.Fonts->Fonts[0]->LegacySize : ImGui::GetStyle().FontSizeBase;
    }
    if (ImGui::GetStyle().FontSizeBase > 0.0f) {
        return ImGui::GetStyle().FontSizeBase;
    }
    return 16.0f;
}

void UILayer::ApplyEditorWorkspacePreferences(bool loadLayout) {
    m_EditorPreferences.ApplyViewportSettings(EditorSettings::Get());

    const float scale = std::clamp(m_EditorPreferences.GetUIScale(), 0.5f, 2.0f);
    ImGui::GetIO().FontGlobalScale = scale;
    editorui::ApplyProjectEditorStyle(GetBaseEditorFontSize() * scale);

    if (!loadLayout) {
        return;
    }

    ImGui::ClearIniSettings();
    const std::string& layoutIni = m_EditorPreferences.GetLayoutIni();
    if (!layoutIni.empty()) {
        ImGui::LoadIniSettingsFromMemory(layoutIni.c_str(), layoutIni.size());
        m_LayoutInitialized = true;
    } else {
        m_LayoutInitialized = false;
    }

    m_ResetLayoutRequested = false;
    m_EnforcedTabsOnce = false;
}

void UILayer::CaptureViewportPreferences() {
    m_EditorPreferences.CaptureViewportSettings(EditorSettings::Get());
}

uint64_t UILayer::ComputeViewportSettingsHash() const {
    const EditorSettings& settings = EditorSettings::Get();
    uint64_t hash = 14695981039346656037ULL;
    hash = HashCombine64(hash, FloatBits(settings.ZoomBaseSpeed));
    hash = HashCombine64(hash, FloatBits(settings.ZoomAcceleration));
    hash = HashCombine64(hash, FloatBits(settings.ZoomMinDistance));
    hash = HashCombine64(hash, FloatBits(settings.ZoomMaxDistance));
    hash = HashCombine64(hash, static_cast<uint64_t>(settings.SmoothZoomEnabled));
    hash = HashCombine64(hash, FloatBits(settings.ZoomSmoothness));
    hash = HashCombine64(hash, FloatBits(settings.OrbitSensitivity));
    hash = HashCombine64(hash, FloatBits(settings.PanSpeedFactor));
    hash = HashCombine64(hash, FloatBits(settings.FocusDuration));
    hash = HashCombine64(hash, FloatBits(settings.FocusDistancePadding));
    hash = HashCombine64(hash, FloatBits(settings.FocusDefaultDistance));
    hash = HashCombine64(hash, static_cast<uint64_t>(settings.DeselectOnEmptyClick));
    hash = HashCombine64(hash, FloatBits(settings.PickingTolerance));
    hash = HashCombine64(hash, static_cast<uint64_t>(settings.PickingSkipHidden));
    hash = HashCombine64(hash, static_cast<uint64_t>(settings.PickingSkipLocked));
    hash = HashCombine64(hash, FloatBits(settings.GridAxisColorX.x));
    hash = HashCombine64(hash, FloatBits(settings.GridAxisColorX.y));
    hash = HashCombine64(hash, FloatBits(settings.GridAxisColorX.z));
    hash = HashCombine64(hash, FloatBits(settings.GridAxisColorX.w));
    hash = HashCombine64(hash, FloatBits(settings.GridAxisColorY.x));
    hash = HashCombine64(hash, FloatBits(settings.GridAxisColorY.y));
    hash = HashCombine64(hash, FloatBits(settings.GridAxisColorY.z));
    hash = HashCombine64(hash, FloatBits(settings.GridAxisColorY.w));
    hash = HashCombine64(hash, FloatBits(settings.GridAxisColorZ.x));
    hash = HashCombine64(hash, FloatBits(settings.GridAxisColorZ.y));
    hash = HashCombine64(hash, FloatBits(settings.GridAxisColorZ.z));
    hash = HashCombine64(hash, FloatBits(settings.GridAxisColorZ.w));
    hash = HashCombine64(hash, FloatBits(settings.GridColor.x));
    hash = HashCombine64(hash, FloatBits(settings.GridColor.y));
    hash = HashCombine64(hash, FloatBits(settings.GridColor.z));
    hash = HashCombine64(hash, FloatBits(settings.GridColor.w));
    hash = HashCombine64(hash, FloatBits(settings.GridMajorColor.x));
    hash = HashCombine64(hash, FloatBits(settings.GridMajorColor.y));
    hash = HashCombine64(hash, FloatBits(settings.GridMajorColor.z));
    hash = HashCombine64(hash, FloatBits(settings.GridMajorColor.w));
    hash = HashCombine64(hash, static_cast<uint64_t>(settings.GridMajorLineInterval));
    hash = HashCombine64(hash, static_cast<uint64_t>(settings.GridShowAxisLines));
    hash = HashCombine64(hash, static_cast<uint64_t>(settings.GridPlaneOrientation));
    hash = HashCombine64(hash, FloatBits(settings.GizmoBaseScale));
    hash = HashCombine64(hash, static_cast<uint64_t>(settings.GizmoAutoScale));
    hash = HashCombine64(hash, FloatBits(settings.GizmoMinScreenSize));
    hash = HashCombine64(hash, FloatBits(settings.GizmoMaxScreenSize));
    hash = HashCombine64(hash, static_cast<uint64_t>(settings.ShowHoverOutline));
    hash = HashCombine64(hash, FloatBits(settings.HoverOutlineColor.x));
    hash = HashCombine64(hash, FloatBits(settings.HoverOutlineColor.y));
    hash = HashCombine64(hash, FloatBits(settings.HoverOutlineColor.z));
    hash = HashCombine64(hash, FloatBits(settings.HoverOutlineColor.w));
    hash = HashCombine64(hash, FloatBits(settings.HoverOutlineThickness));
    return hash;
}

uint64_t UILayer::ComputeActionUsageHash() const {
    std::vector<std::pair<std::string, int>> usageEntries;
    usageEntries.reserve(m_ActionRegistry.GetUsageCounts().size());
    for (const auto& entry : m_ActionRegistry.GetUsageCounts()) {
        usageEntries.push_back(entry);
    }
    std::sort(usageEntries.begin(), usageEntries.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    uint64_t hash = 14695981039346656037ULL;
    for (const auto& entry : usageEntries) {
        hash = HashCombine64(hash, HashString64(entry.first));
        hash = HashCombine64(hash, static_cast<uint64_t>(entry.second));
    }
    return hash;
}

uint64_t UILayer::ComputePersistedWindowStateHash() const {
    uint64_t hash = 14695981039346656037ULL;
    for (const editorui::EditorActionDefinition* action : m_ActionRegistry.GetPersistedActions()) {
        if (!action) {
            continue;
        }
        hash = HashCombine64(hash, HashString64(action->Id));
        const bool isOpen = action->IsChecked ? action->IsChecked() : false;
        hash = HashCombine64(hash, static_cast<uint64_t>(isOpen));
    }
    return hash;
}

void UILayer::SyncEditorPersistence() {
    const uint64_t viewportHash = ComputeViewportSettingsHash();
    if (viewportHash != m_LastViewportSettingsHash) {
        m_LastViewportSettingsHash = viewportHash;
        CaptureViewportPreferences();
        m_EditorPreferences.RequestSave(editorui::EditorPreferenceGroup::Viewport);
    }

    if (ImGui::GetIO().WantSaveIniSettings) {
        size_t iniSize = 0;
        const char* ini = ImGui::SaveIniSettingsToMemory(&iniSize);
        m_EditorPreferences.SetLayoutIni(std::string(ini, iniSize));
        ImGui::GetIO().WantSaveIniSettings = false;
    }

    const uint64_t windowHash = ComputePersistedWindowStateHash();
    if (windowHash != m_LastPersistedWindowStateHash) {
        m_LastPersistedWindowStateHash = windowHash;
        m_EditorPreferences.CaptureWindowStates(m_ActionRegistry);
        m_EditorPreferences.RequestSave(editorui::EditorPreferenceGroup::Windows);
    }

    const uint64_t actionUsageHash = ComputeActionUsageHash();
    if (actionUsageHash != m_LastActionUsageHash) {
        m_LastActionUsageHash = actionUsageHash;
        m_EditorPreferences.SetActionUsageCounts(m_ActionRegistry.GetUsageCounts());
    }

    m_EditorPreferences.SaveIfDue();
}

PrefabEditorPanel* UILayer::GetFocusedPrefabEditor() {
    for (auto& panel : m_PrefabEditors) {
        if (panel && panel->IsWindowFocusedOrHovered()) {
            return panel.get();
        }
    }
    return nullptr;
}

CodeEditorPanel* UILayer::GetFocusedCodeEditor() {
    for (auto& panel : m_CodeEditors) {
        if (panel && panel->IsWindowFocusedOrHovered()) {
            return panel.get();
        }
    }
    return nullptr;
}

bool UILayer::IsSpecializedDocumentFocused() const {
    return m_NodeGraphPanel.IsWindowFocusedOrHovered() ||
           m_AnimGraphPanel.IsWindowFocusedOrHovered() ||
           m_ShaderGraphPanel.IsWindowFocusedOrHovered() ||
           m_DialogueEditorPanel.IsWindowFocusedOrHovered() ||
           m_QuestEditorPanel.IsWindowFocusedOrHovered();
}

void UILayer::AdjustUIScale(float delta) {
    const float newScale = std::clamp(m_EditorPreferences.GetUIScale() + delta, 0.5f, 2.0f);
    m_EditorPreferences.SetUIScale(newScale);
    ApplyEditorWorkspacePreferences(false);
    m_Notifications.Push(editorui::EditorNotificationLevel::Info, "UI scale: " + std::to_string(static_cast<int>(newScale * 100.0f)) + "%", 2.4f);
}

bool UILayer::SaveCurrentScene(bool saveAs) {
    std::string scenePath = saveAs ? std::string() : m_CurrentScenePath;
    if (scenePath.empty()) {
        scenePath = ShowSaveFileDialog("NewScene.scene");
    }
    if (scenePath.empty()) {
        return false;
    }

    StampEditorCameraMetadata(m_Scene);
    if (!Serializer::SaveSceneToFile(m_Scene, scenePath)) {
        m_Notifications.Push(editorui::EditorNotificationLevel::Error, "Failed to save scene.");
        return false;
    }

    m_CurrentScenePath = scenePath;
    m_Scene.ClearDirty();
    RunSerializerSanityIfEnabled(m_Scene);
    m_Notifications.Push(editorui::EditorNotificationLevel::Success, "Scene saved.");
    return true;
}

bool UILayer::SaveActiveDocument() {
    if (m_PlayMode) {
        return false;
    }

    if (PrefabEditorPanel* prefabEditor = GetFocusedPrefabEditor()) {
        const bool ok = prefabEditor->SavePrefab();
        if (ok) {
            prefabEditor->ClearDirty();
            m_Notifications.Push(editorui::EditorNotificationLevel::Success, "Prefab saved.");
        } else {
            m_Notifications.Push(editorui::EditorNotificationLevel::Error, "Failed to save prefab.");
        }
        return ok;
    }

    if (CodeEditorPanel* codeEditor = GetFocusedCodeEditor()) {
        const bool ok = codeEditor->SaveCurrent();
        m_Notifications.Push(ok ? editorui::EditorNotificationLevel::Success : editorui::EditorNotificationLevel::Error,
                             ok ? "File saved." : "Failed to save file.",
                             ok ? 2.6f : 4.5f);
        return ok;
    }

    if (m_NodeGraphPanel.IsWindowFocusedOrHovered()) {
        const bool ok = m_NodeGraphPanel.SaveCurrent();
        m_Notifications.Push(ok ? editorui::EditorNotificationLevel::Success : editorui::EditorNotificationLevel::Error,
                             ok ? "Node graph saved." : "Failed to save node graph.",
                             ok ? 2.6f : 4.5f);
        return ok;
    }

    if (m_AnimGraphPanel.IsWindowFocusedOrHovered()) {
        const bool ok = m_AnimGraphPanel.SaveCurrent();
        m_Notifications.Push(ok ? editorui::EditorNotificationLevel::Success : editorui::EditorNotificationLevel::Error,
                             ok ? "Animation controller saved." : "Failed to save animation controller.",
                             ok ? 2.6f : 4.5f);
        return ok;
    }

    if (m_ShaderGraphPanel.IsWindowFocusedOrHovered()) {
        const bool ok = m_ShaderGraphPanel.SaveCurrent();
        m_Notifications.Push(ok ? editorui::EditorNotificationLevel::Success : editorui::EditorNotificationLevel::Error,
                             ok ? "Shader graph saved." : "Failed to save shader graph.",
                             ok ? 2.6f : 4.5f);
        return ok;
    }

    if (m_DialogueEditorPanel.IsWindowFocusedOrHovered()) {
        const bool ok = m_DialogueEditorPanel.SaveCurrent();
        m_Notifications.Push(ok ? editorui::EditorNotificationLevel::Success : editorui::EditorNotificationLevel::Error,
                             ok ? "Dialogue library saved." : "Failed to save dialogue library.",
                             ok ? 2.6f : 4.5f);
        return ok;
    }

    if (m_QuestEditorPanel.IsWindowFocusedOrHovered()) {
        const bool ok = m_QuestEditorPanel.SaveCurrent();
        m_Notifications.Push(ok ? editorui::EditorNotificationLevel::Success : editorui::EditorNotificationLevel::Error,
                             ok ? "Quest database saved." : "Failed to save quest database.",
                             ok ? 2.6f : 4.5f);
        return ok;
    }

    if (!IsSpecializedDocumentFocused() && m_ViewportPanel.IsWindowFocusedOrHovered()) {
        return SaveCurrentScene(false);
    }

    return false;
}

bool UILayer::SaveActiveDocumentAs() {
    if (m_PlayMode) {
        return false;
    }

    if (m_NodeGraphPanel.IsWindowFocusedOrHovered()) {
        const bool ok = m_NodeGraphPanel.SaveCurrentAsDialog();
        m_Notifications.Push(ok ? editorui::EditorNotificationLevel::Success : editorui::EditorNotificationLevel::Error,
                             ok ? "Node graph saved." : "Failed to save node graph.",
                             ok ? 2.6f : 4.5f);
        return ok;
    }

    if (m_AnimGraphPanel.IsWindowFocusedOrHovered()) {
        const bool ok = m_AnimGraphPanel.SaveCurrentAsDialog();
        m_Notifications.Push(ok ? editorui::EditorNotificationLevel::Success : editorui::EditorNotificationLevel::Error,
                             ok ? "Animation controller saved." : "Failed to save animation controller.",
                             ok ? 2.6f : 4.5f);
        return ok;
    }

    if (m_ShaderGraphPanel.IsWindowFocusedOrHovered()) {
        const bool ok = m_ShaderGraphPanel.SaveCurrentAsDialog();
        m_Notifications.Push(ok ? editorui::EditorNotificationLevel::Success : editorui::EditorNotificationLevel::Error,
                             ok ? "Shader graph saved." : "Failed to save shader graph.",
                             ok ? 2.6f : 4.5f);
        return ok;
    }

    if (m_DialogueEditorPanel.IsWindowFocusedOrHovered()) {
        const bool ok = m_DialogueEditorPanel.SaveCurrentAsDialog();
        m_Notifications.Push(ok ? editorui::EditorNotificationLevel::Success : editorui::EditorNotificationLevel::Error,
                             ok ? "Dialogue library saved." : "Failed to save dialogue library.",
                             ok ? 2.6f : 4.5f);
        return ok;
    }

    if (m_QuestEditorPanel.IsWindowFocusedOrHovered()) {
        const bool ok = m_QuestEditorPanel.SaveCurrentAsDialog();
        m_Notifications.Push(ok ? editorui::EditorNotificationLevel::Success : editorui::EditorNotificationLevel::Error,
                             ok ? "Quest database saved." : "Failed to save quest database.",
                             ok ? 2.6f : 4.5f);
        return ok;
    }

    if (!IsSpecializedDocumentFocused() && m_ViewportPanel.IsWindowFocusedOrHovered()) {
        return SaveCurrentScene(true);
    }

    return false;
}

void UILayer::NewScene() {
    m_Scene = Scene();
    m_Scene.SetScenePath("");
    m_Scene.ClearDirty();
    m_SelectedEntity = -1;
    m_CurrentScenePath.clear();
    ResetEditorCamera();
    EditorSceneUndoStack::Get().ResetToScene(&m_Scene, &m_SelectedEntity);
    m_Notifications.Push(editorui::EditorNotificationLevel::Info, "New scene created.", 2.6f);
}

void UILayer::PromptLoadScene() {
    const std::string scenePath = ShowOpenFileDialog();
    if (!scenePath.empty()) {
        DeferSceneLoad(scenePath);
    }
}


void UILayer::LoadProject(std::string path) {
    m_ProjectPanel.LoadProject(path);
    const fs::path projectRoot = Project::GetProjectDirectory().empty()
        ? fs::path(path)
        : Project::GetProjectDirectory();
    m_EditorPreferences.Load(projectRoot);
    m_ActionRegistry.SetUsageCounts(m_EditorPreferences.GetActionUsageCounts());
    ApplyEditorWorkspacePreferences(true);
    // Apply project default font to Renderer (loaded from project file)
    Renderer::Get().SetDefaultTextFont(Project::GetDefaultFontPath());
    OnAttach();
    m_EditorPreferences.RestoreWindowStates(m_ActionRegistry);
    m_LastPersistedWindowStateHash = ComputePersistedWindowStateHash();
    m_LastViewportSettingsHash = ComputeViewportSettingsHash();
    m_LastActionUsageHash = ComputeActionUsageHash();

    auto autoRun = SerializerSanityWindow::ConsumeAutoRunOptions();
    if (autoRun.enabled) {
        if (autoRun.scenePath.empty()) {
            std::cerr << "[SerializerSanity] Auto-run requires --scene <path>" << std::endl;
            std::exit(2);
        }
        if (!Serializer::LoadSceneFromFile(autoRun.scenePath, m_Scene)) {
            std::cerr << "[SerializerSanity] Failed to load scene: " << autoRun.scenePath << std::endl;
            std::exit(2);
        }
        m_CurrentScenePath = autoRun.scenePath;
        EditorSceneUndoStack::Get().ResetToScene(&m_Scene, &m_SelectedEntity);
        m_SerializerSanityWindow.SetContext(&m_Scene);
        m_SerializerSanityWindow.RunAllChecks(m_Scene);
        int code = m_SerializerSanityWindow.AllChecksPassed() ? 0 : 1;
        if (autoRun.exitOnComplete) {
            std::exit(code);
        }
    }
}

void UILayer::OnAttach() {
    m_ScriptPanel.SetScriptSource(&g_RegisteredScriptNames);
    m_ScriptPanel.SetContext(&m_Scene);
    // New timeline panel owns its own inspector; legacy inspector-timeline wiring removed
    m_InspectorPanel.SetAvatarBuilderPanel(&m_AvatarBuilderPanel);
    m_InspectorPanel.SetSplineToolPanel(&m_SplineToolPanel);
    m_InspectorPanel.SetSoftbodyPainter(&m_SoftbodyPainter);
}

// =============================
// Main UI Render Loop
// =============================
void UILayer::ExpandHierarchyTo(EntityID id) {
    m_SceneHierarchyPanel.ExpandTo(id);
}

void UILayer::OnUIRender() {
    UpdateCachedSceneTitle();
    BeginDockspace();

    // Determine which scene should be considered "active" for editor panels
    Scene* activeScene = m_PlayMode && m_Scene.m_RuntimeScene ? m_Scene.m_RuntimeScene.get() : &m_Scene;

    const bool terrainSelectionActive = HasTerrainSelection();
    const bool navMeshSelectionActive = HasNavMeshSelection();
    const bool splineSelectionActive = HasSplineSelection();
    const bool softbodySelectionActive = HasSoftbodySelection();
    const bool viewportActive = m_ViewportPanel.IsWindowFocusedOrHovered();

    if (!terrainSelectionActive && TerrainPainter::IsBrushModeEnabled()) {
        TerrainPainter::SetBrushModeEnabled(false);
    }
    if (!navMeshSelectionActive && NavLinkPainter::IsPaintModeEnabled()) {
        NavLinkPainter::SetPaintModeEnabled(false);
    }
    if (!splineSelectionActive && m_SplineToolPanel.IsDrawModeActive()) {
        m_SplineToolPanel.StopDrawMode();
    }
    if ((!softbodySelectionActive || m_PlayMode) && m_SoftbodyPainter.IsPaintModeActive()) {
        m_SoftbodyPainter.StopPaintMode();
    }

    // B toggles spline draw mode (works from viewport or Inspector)
    if (softbodySelectionActive && !m_PlayMode && viewportActive) {
        if (Input::WasKeyPressedThisFrame('B')) {
            if (TerrainPainter::IsBrushModeEnabled()) TerrainPainter::SetBrushModeEnabled(false);
            if (NavLinkPainter::IsPaintModeEnabled()) NavLinkPainter::SetPaintModeEnabled(false);
            if (m_RiverCutterPanel.IsDrawModeActive()) m_RiverCutterPanel.StopDrawMode();
            if (m_SplineToolPanel.IsDrawModeActive()) m_SplineToolPanel.StopDrawMode();
            m_SoftbodyPainter.SetContext(activeScene);
            m_SoftbodyPainter.SetSelectedEntityPtr(&m_SelectedEntity);
            m_SoftbodyPainter.SyncSelectionTarget();
            if (m_SoftbodyPainter.IsPaintModeActive()) {
                m_SoftbodyPainter.StopPaintMode();
            } else {
                m_SoftbodyPainter.StartPaintMode();
            }
        }
    } else if (splineSelectionActive && !m_PlayMode) {
        if (Input::WasKeyPressedThisFrame('B')) {
            if (TerrainPainter::IsBrushModeEnabled()) TerrainPainter::SetBrushModeEnabled(false);
            if (NavLinkPainter::IsPaintModeEnabled()) NavLinkPainter::SetPaintModeEnabled(false);
            if (m_RiverCutterPanel.IsDrawModeActive()) m_RiverCutterPanel.StopDrawMode();
            if (m_SoftbodyPainter.IsPaintModeActive()) m_SoftbodyPainter.StopPaintMode();
            if (m_SplineToolPanel.IsDrawModeActive()) {
                m_SplineToolPanel.StopDrawMode();
            } else {
                m_SplineToolPanel.SyncSelectionTarget();
                m_SplineToolPanel.SetContext(activeScene);
                m_SplineToolPanel.SetSelectedEntityPtr(&m_SelectedEntity);
                m_SplineToolPanel.StartDrawMode();
            }
        }
    } else if (terrainSelectionActive && !m_PlayMode && viewportActive) {
        if (Input::WasKeyPressedThisFrame('B')) {
            if (m_SplineToolPanel.IsDrawModeActive()) m_SplineToolPanel.StopDrawMode();
            if (m_SoftbodyPainter.IsPaintModeActive()) m_SoftbodyPainter.StopPaintMode();
            if (m_RiverCutterPanel.IsDrawModeActive() && !TerrainPainter::IsBrushModeEnabled()) {
                m_RiverCutterPanel.StopDrawMode();
            }
            NavLinkPainter::SetPaintModeEnabled(false);
            TerrainPainter::ToggleBrushMode();
        }
    } else if (navMeshSelectionActive && !m_PlayMode && viewportActive) {
        if (Input::WasKeyPressedThisFrame('B')) {
            if (TerrainPainter::IsBrushModeEnabled()) TerrainPainter::SetBrushModeEnabled(false);
            if (m_RiverCutterPanel.IsDrawModeActive()) m_RiverCutterPanel.StopDrawMode();
            if (m_SplineToolPanel.IsDrawModeActive()) m_SplineToolPanel.StopDrawMode();
            if (m_SoftbodyPainter.IsPaintModeActive()) m_SoftbodyPainter.StopPaintMode();
            NavLinkPainter::TogglePaintMode();
        }
    }

    // Auto-disable River Cutter draw mode if brush/link/spline modes are enabled
    if ((TerrainPainter::IsBrushModeEnabled() || NavLinkPainter::IsPaintModeEnabled() || m_SplineToolPanel.IsDrawModeActive() || m_SoftbodyPainter.IsPaintModeActive()) && m_RiverCutterPanel.IsDrawModeActive()) {
        m_RiverCutterPanel.StopDrawMode();
    }

    // Sticky routing: prefer last chosen editor source (prefab or main) until another editor window becomes active
    if (!m_ActiveEditorScene) {
        m_ActiveEditorScene = activeScene;
        m_ActiveSelectedEntityPtr = &m_SelectedEntity;
    }
    // Always keep main viewport bound to main scene
    m_ViewportPanel.SetContext(activeScene);

    // Route Scene Hierarchy to the main scene always (no more fighting with prefab editors)
    m_SceneHierarchyPanel.SetContext(activeScene);
    m_SceneHierarchyPanel.SetSelectedEntityPtr(&m_SelectedEntity);
    // Inspector follows the currently active editor source (main scene by default; prefab when focused)
    m_InspectorPanel.SetContext(m_ActiveEditorScene);
    m_InspectorPanel.SetSelectedEntityPtr(m_ActiveSelectedEntityPtr ? m_ActiveSelectedEntityPtr : &m_SelectedEntity);
    m_SoftbodyPainter.SetContext(activeScene);
    m_SoftbodyPainter.SetSelectedEntityPtr(&m_SelectedEntity);
    m_SoftbodyPainter.SyncSelectionTarget();

    // Render other panels first (with granular profiling)
    { ScopedTimer t("UI/Project"); m_ProjectPanel.OnImGuiRender(); }
    SyncSelectionContext();
    { ScopedTimer t("UI/Console"); m_ConsolePanel.OnImGuiRender(); }
    { ScopedTimer t("UI/Profiler"); m_ProfilerPanel.OnImGuiRender(); }
    // Ensure serializer window has active scene context before rendering
    m_SerializerSanityWindow.SetContext(m_ActiveEditorScene);
    m_SerializerSanityWindow.OnImGuiRender();
    // Tint mask editor window (modal-like regular window)
    m_TintMaskEditor.SetContext(m_ActiveEditorScene);
    m_TintMaskEditor.OnImGuiRender();
    m_WorldGenPanel.SetContext(activeScene);
    m_WorldGenPanel.SetSelectedEntityPtr(&m_SelectedEntity);
    m_WorldGenPanel.OnImGuiRender();
    
    m_TerrainEvolutionPanel.SetContext(activeScene);
    m_TerrainEvolutionPanel.SetSelectedEntityPtr(&m_SelectedEntity);
    m_TerrainEvolutionPanel.OnImGuiRender();
    
    m_RiverCutterPanel.SetContext(activeScene);
    m_RiverCutterPanel.SetSelectedEntityPtr(&m_SelectedEntity);
    m_RiverCutterPanel.OnImGuiRender();
    m_SplineToolPanel.SetContext(activeScene);
    m_SplineToolPanel.SetSelectedEntityPtr(&m_SelectedEntity);
    m_SplineToolPanel.OnImGuiRender();
    
    m_SplatmapGeneratorPanel.SetContext(activeScene);
    m_SplatmapGeneratorPanel.SetSelectedEntityPtr(&m_SelectedEntity);
    m_SplatmapGeneratorPanel.OnImGuiRender();
    
    m_ResourceLayerPanel.SetContext(activeScene);
    m_ResourceLayerPanel.SetSelectedEntityPtr(&m_SelectedEntity);
    m_ResourceLayerPanel.OnImGuiRender();

    m_TextureCleanupPanel.OnImGuiRender();
    m_IconGeneratorPanel.OnImGuiRender();
    
    m_UIPrefabLayoutDesignerPanel.SetContext(activeScene);
    m_UIPrefabLayoutDesignerPanel.OnImGuiRender();
#if CM_DEBUG_GRASS
    TerrainGrass::RenderDebugWindow();
#endif
    if (m_FocusConsoleNextFrame) {
        ImGui::SetWindowFocus("Console");
        m_FocusConsoleNextFrame = false;
    }

    if (m_ShowScriptRegistry) {
        ScopedTimer t("UI/ScriptRegistry");
        m_ScriptPanel.OnImGuiRender(&m_ShowScriptRegistry);
    }
    { ScopedTimer t("UI/AnimController"); m_AnimCtrlPanel.OnImGuiRender(); }
    { ScopedTimer t("UI/NodeGraph"); m_NodeGraphPanel.OnImGuiRender(); }
    Scene* graphScene = m_ActiveEditorScene ? m_ActiveEditorScene : activeScene;
    EntityID* graphSelection = m_ActiveSelectedEntityPtr ? m_ActiveSelectedEntityPtr : &m_SelectedEntity;
    m_AnimGraphPanel.SetContext(graphScene, graphSelection);
    { ScopedTimer t("UI/AnimGraph"); m_AnimGraphPanel.OnImGuiRender(); }
    m_AnimTimelinePanel.SetContext(activeScene, &m_SelectedEntity);
    { ScopedTimer t("UI/AnimTimeline"); m_AnimTimelinePanel.OnImGuiRender(); }
    
    // Shader Graph Panel
    { ScopedTimer t("UI/ShaderGraph"); m_ShaderGraphPanel.OnImGuiRender(); }
    // Dialogue Editor Panel
    { ScopedTimer t("UI/DialogueEditor"); m_DialogueEditorPanel.OnImGuiRender(); }
    // Quest Editor Panel
    { ScopedTimer t("UI/QuestEditor"); m_QuestEditorPanel.OnImGuiRender(); }
    // Avatar Builder (opens as a standalone window when requested)
    { ScopedTimer t("UI/AvatarBuilder"); m_AvatarBuilderPanel.OnImGuiRender(); }

    // Main viewport
    const std::string& sceneName = m_CachedSceneTitle;
    m_ViewportPanel.SetDisplaySceneTitle(sceneName);
    m_ViewportPanel.SetSplineDrawModeActive(m_SplineToolPanel.IsDrawModeActive());
    m_ViewportPanel.SetSoftbodyPaintModeActive(m_SoftbodyPainter.IsPaintModeActive());
    m_ViewportPanel.SetRiverDrawModeActive(m_RiverCutterPanel.IsDrawModeActive());
    { ScopedTimer t("UI/Viewport"); m_ViewportPanel.OnImGuiRender(Renderer::Get().GetSceneTexture()); }

    // If a blocking overlay is active (loading scene/play mode), render it now
    RenderBlockingOverlay();

    // Service async begin-play request after UI has a chance to paint the overlay
    ProcessBeginPlayAsync();
    PollExternalRuntimePreview();

    // -------------------------------------------------
    // Runtime Stats panel (Play mode + Debug enabled)
    // -------------------------------------------------
    if (m_PlayMode && Renderer::Get().GetDebugDrawInPlayMode()) {
        Scene* statsScene = m_Scene.m_RuntimeScene ? m_Scene.m_RuntimeScene.get() : &m_Scene;

        if (m_ShowRuntimeStatsPanel) {
            ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Runtime Stats", &m_ShowRuntimeStatsPanel, ImGuiWindowFlags_AlwaysAutoResize)) {
            // FPS
            ImGui::Text("FPS: %.1f (%.2f ms)", ImGui::GetIO().Framerate, 1000.0f / (ImGui::GetIO().Framerate > 1.0f ? ImGui::GetIO().Framerate : 1.0f));

            const cm::world::RuntimeWorld* runtimeWorld = statsScene->GetRuntimeWorld();
            const auto& rendererStats = Renderer::Get().GetLastRuntimeStatsFrame();

            // Entity counts
            const auto& entities = statsScene->GetEntities();
            int entityCount = (int)entities.size();
            int meshEntities = 0, lightEntities = 0, cameraEntities = 0, scriptInstances = 0;
            int rigidBodies = 0, staticBodies = 0, characterControllers = 0;
            int gpuMorphEligibleSkinnedMeshes = 0;
            int gpuMorphActiveSkinnedMeshes = 0;

            // Material stats
            std::unordered_map<const Material*, int> uniqueMaterials;
            std::unordered_map<uint64_t, int> theoreticallySharedMaterials;
            int materialPropertyBlocksNonEmpty = 0; // per-entity primary block
            auto addMaterialReference = [&](const std::shared_ptr<Material>& material) {
                if (!material) {
                    return;
                }
                uniqueMaterials[material.get()]++;
                const MaterialEquivalenceKeyInfo keyInfo = GetMaterialEquivalenceKey(material.get());
                theoreticallySharedMaterials[keyInfo.Key]++;
            };

            for (const auto& e : entities) {
                auto* d = statsScene->GetEntityData(e.GetID());
                if (!d || !d->Visible) continue;
                if (d->Mesh) {
                    meshEntities++;
                    // Primary material
                    addMaterialReference(d->Mesh->material);
                    // Slot materials
                    for (const auto& sm : d->Mesh->materials) {
                        addMaterialReference(sm);
                    }
                    if (!d->Mesh->PropertyBlock.Empty()) materialPropertyBlocksNonEmpty++;
                    Mesh* mesh = d->Mesh->mesh.get();
                    if (d->Skinning &&
                        mesh &&
                        mesh->Dynamic &&
                        Renderer::Get().ShouldUseGpuMorphTargets(d->Skinning.get(), mesh, d->BlendShapes.get())) {
                        ++gpuMorphEligibleSkinnedMeshes;
                        if (d->BlendShapes && d->BlendShapes->CountActiveShapes() > 0u) {
                            ++gpuMorphActiveSkinnedMeshes;
                        }
                    }
                }
                if (d->Light) lightEntities++;
                if (d->Camera) cameraEntities++;
                if (!d->Scripts.empty()) scriptInstances += (int)d->Scripts.size();
                if (d->RigidBody) rigidBodies++;
                if (d->StaticBody) staticBodies++;
                if (d->CharacterController) characterControllers++;
            }

            int uniqueMaterialCount = (int)uniqueMaterials.size();
            int theoreticalUniqueMaterialCount = (int)theoreticallySharedMaterials.size();
            int totalMaterialRefs = 0;
            for (auto& kv : uniqueMaterials) totalMaterialRefs += kv.second;
            int actualSharedMaterialRefs = totalMaterialRefs - uniqueMaterialCount;
            int theoreticalSharedMaterialRefs = totalMaterialRefs - theoreticalUniqueMaterialCount;

            uint64_t renderedObjects = 0;
            uint64_t totalRenderableObjects = static_cast<uint64_t>(meshEntities);
            uint64_t culledObjects = 0;
            uint64_t visibleTerrainObjects = 0;
            uint64_t terrainObjectsInScene = 0;
            uint64_t skinnedMeshesTotal = 0;
            uint64_t skinnedMeshesCulled = 0;
            uint64_t lodNearGroups = 0;
            uint64_t lodMediumGroups = 0;
            uint64_t lodFarGroups = 0;
            uint64_t lodBeyondGroups = 0;

            if (runtimeWorld) {
                const auto& runtimeStats = runtimeWorld->GetStats();
                const auto& renderWorld = runtimeWorld->GetRenderWorld();

                visibleTerrainObjects = static_cast<uint64_t>(renderWorld.VisibleTerrainEntities.size());
                terrainObjectsInScene = static_cast<uint64_t>(runtimeStats.TerrainCount);
                renderedObjects = rendererStats.RenderedMeshObjects + visibleTerrainObjects;
                totalRenderableObjects =
                    static_cast<uint64_t>(runtimeStats.RenderableCount) + terrainObjectsInScene;
                const uint64_t culledTerrainObjects =
                    (terrainObjectsInScene > visibleTerrainObjects)
                        ? (terrainObjectsInScene - visibleTerrainObjects)
                        : 0u;
                culledObjects = rendererStats.CulledMeshObjects + culledTerrainObjects;
                skinnedMeshesTotal =
                    rendererStats.RenderedSkinnedMeshObjects + rendererStats.CulledSkinnedMeshObjects;
                skinnedMeshesCulled = rendererStats.CulledSkinnedMeshObjects;

                const auto& skinningGroups = runtimeWorld->GetSkinningGroupCaches();
                for (const auto& group : skinningGroups) {
                    auto* skeletonData = statsScene->GetEntityData(group.SkeletonSceneEntity);
                    if (!skeletonData || !skeletonData->Skeleton) {
                        continue;
                    }

                    SkeletonComponent& skeleton = *skeletonData->Skeleton;
                    if (!skeleton.LODEnabled || skeleton.LodLastDistance < skeleton.LODNearDistance) {
                        ++lodNearGroups;
                    } else if (skeleton.LodLastDistance < skeleton.LODMediumDistance) {
                        ++lodMediumGroups;
                    } else if (skeleton.LodLastDistance < skeleton.LODFarDistance) {
                        ++lodFarGroups;
                    } else {
                        ++lodBeyondGroups;
                    }
                }
            } else {
                renderedObjects = rendererStats.RenderedMeshObjects;
                culledObjects = rendererStats.CulledMeshObjects;
            }

            ImGui::SeparatorText("Scene");
            ImGui::Text("Entities: %d", entityCount);
            ImGui::Text("Meshes: %d  Lights: %d  Cameras: %d", meshEntities, lightEntities, cameraEntities);
            ImGui::Text("Scripts (instances): %d", scriptInstances);
            ImGui::Text("Rendered objects: %llu / %llu", static_cast<unsigned long long>(renderedObjects), static_cast<unsigned long long>(totalRenderableObjects));
            ImGui::Text("Objects culled: %llu", static_cast<unsigned long long>(culledObjects));
            ImGui::Text("Skinned meshes culled: %llu / %llu", static_cast<unsigned long long>(skinnedMeshesCulled), static_cast<unsigned long long>(skinnedMeshesTotal));
            ImGui::Text("GPU morphed skinned meshes: %d active / %d eligible", gpuMorphActiveSkinnedMeshes, gpuMorphEligibleSkinnedMeshes);
            ImGui::Text("Skinned LOD groups: Near %llu  Medium %llu  Far %llu  Beyond %llu",
                static_cast<unsigned long long>(lodNearGroups),
                static_cast<unsigned long long>(lodMediumGroups),
                static_cast<unsigned long long>(lodFarGroups),
                static_cast<unsigned long long>(lodBeyondGroups));

            ImGui::SeparatorText("Materials");
            ImGui::Text("Unique materials: %d actual / %d theoretical min", uniqueMaterialCount, theoreticalUniqueMaterialCount);
            ImGui::Text("Material references (all slots): %d", totalMaterialRefs);
            ImGui::Text("Shared refs: %d actual / %d theoretical", actualSharedMaterialRefs, theoreticalSharedMaterialRefs);
            ImGui::Text("Entities with overrides (PropertyBlock): %d", materialPropertyBlocksNonEmpty);

            // Physics stats (bodies)
            ImGui::SeparatorText("Physics");
            int joltBodies = 0;
            if (auto* sys = Physics::GetSystem()) {
                joltBodies = (int)sys->GetNumBodies();
            }
            ImGui::Text("Bodies: %d (Rigid:%d, Static:%d, Characters:%d)", joltBodies, rigidBodies, staticBodies, characterControllers);

            // Renderer toggles snapshot
            ImGui::SeparatorText("Renderer");
            auto& R = Renderer::Get();
            ImGui::Text("Grid:%s  Colliders:%s  AABBs:%s  UI:%s",
                R.GetShowGrid() ? "on" : "off",
                R.GetShowColliders() ? "on" : "off",
                R.GetShowAABBs() ? "on" : "off",
                R.WasUIInputConsumedThisFrame() ? "input-consumed" : "ui-active");

            // Optional: breakdown list when expanded
            if (ImGui::TreeNode("Material list (unique)")) {
                int idx = 0;
                for (auto& kv : uniqueMaterials) {
                    const Material* m = kv.first; int refs = kv.second;
                    ImGui::Text("%d. %s  (refs:%d)", ++idx, m ? m->GetName().c_str() : "<null>", refs);
                }
                ImGui::TreePop();
            }
            }
            ImGui::End();
        }
    }

    // Delete handled in hovered panels (Viewport/Hierarchy) to avoid double-triggering

    // Any open prefab editors. If one is focused/hovered, make it the sticky source for hierarchy/inspector.
    bool madeStickyThisFrame = false;
    for (auto it = m_PrefabEditors.begin(); it != m_PrefabEditors.end(); ) {
        PrefabEditorPanel* panel = it->get();
        panel->OnImGuiRender();
        bool wantsFocus = panel->IsWindowFocusedOrHovered();
        // When the prefab editor window is focused/hovered, route Inspector to the prefab's scene/selection
        if (wantsFocus && !madeStickyThisFrame) {
            m_ActiveEditorScene = panel->GetScene();
            m_ActiveSelectedEntityPtr = panel->GetSelectedEntityPtr();
            m_InspectorPanel.SetContext(m_ActiveEditorScene);
            m_InspectorPanel.SetSelectedEntityPtr(m_ActiveSelectedEntityPtr);
            madeStickyThisFrame = true;
        }
        if (!panel->IsOpen()) {
            if (m_ActiveEditorScene == panel->GetScene() || m_ActiveSelectedEntityPtr == panel->GetSelectedEntityPtr()) {
                m_ActiveEditorScene = activeScene;
                m_ActiveSelectedEntityPtr = &m_SelectedEntity;
                m_InspectorPanel.SetContext(m_ActiveEditorScene);
                m_InspectorPanel.SetSelectedEntityPtr(m_ActiveSelectedEntityPtr);
            }
            // Clear sticky editor if this was it
            if (m_StickyPrefabEditor == panel) {
                m_StickyPrefabEditor = nullptr;
            }
            it = m_PrefabEditors.erase(it);
        } else {
            ++it;
        }
    }

    // Render open code editors
    for (auto it = m_CodeEditors.begin(); it != m_CodeEditors.end(); ) {
        auto* panel = it->get();
        panel->OnImGuiRender();
        if (!panel->IsWindowFocusedOrHovered()) {
            // keep open; no explicit close tracking impl yet
        }
        ++it;
    }

    if (!m_PlayMode && !ImGui::GetIO().WantTextInput && m_ViewportPanel.IsWindowFocusedOrHovered()) {
        if (ImGui::GetIO().KeyCtrl && Input::WasKeyPressedThisFrame(KeyCode::Z)) {
            if (ImGui::GetIO().KeyShift) {
                EditorSceneUndoStack::Get().Redo();
            } else {
                EditorSceneUndoStack::Get().Undo();
            }
        } else if (ImGui::GetIO().KeyCtrl && Input::WasKeyPressedThisFrame(KeyCode::Y)) {
            EditorSceneUndoStack::Get().Redo();
        }
    }
    m_ActionRegistry.TryDispatchShortcut();

    // Remove old automatic context switching; PrefabEditor now owns its own hierarchy tab
    // Keep main panels bound to main scene unless explicitly changed elsewhere
    // If the main viewport window is focused/hovered, switch Inspector back to main scene
    if (m_ViewportPanel.IsWindowFocusedOrHovered()) {
        m_ActiveEditorScene = activeScene;
        m_ActiveSelectedEntityPtr = &m_SelectedEntity;
        m_InspectorPanel.SetContext(m_ActiveEditorScene);
        m_InspectorPanel.SetSelectedEntityPtr(m_ActiveSelectedEntityPtr);
    }

    // If no prefab editor claimed focus this frame, keep whatever sticky source we had
    // Now render the shared Scene Hierarchy and Inspector with the chosen context
    // Route Inspector to Animation inspector when a .anim is selected in Project panel
    m_SceneHierarchyPanel.SetSceneDisplayName(sceneName);
    { ScopedTimer t("UI/Hierarchy"); m_SceneHierarchyPanel.OnImGuiRender(); }
    {
        // Forward currently selected asset path to Inspector so it can show previews
        m_InspectorPanel.SetSelectedAssetPath(m_ProjectPanel.GetSelectedItemPath());
        std::string selExt = m_ProjectPanel.GetSelectedItemExtension();
        if (selExt == ".anim") {
            ImGui::Begin("Inspector");
            if (m_AnimationInspector) m_AnimationInspector->OnImGuiRender();
            ImGui::End();
        } else {
            { ScopedTimer t("UI/Inspector"); m_InspectorPanel.OnImGuiRender(); }
        }
        if (m_ShowAssetRegistry) {
            ScopedTimer t("UI/AssetRegistry");
            m_AssetRegistryPanel.OnImGuiRender(&m_ShowAssetRegistry);
        }
    }

    // Editor-only terrain painting
    // Use the same scene that Renderer uses: runtime scene in play mode, editor scene otherwise
    Scene& sceneForPainting = m_Scene.m_RuntimeScene ? *m_Scene.m_RuntimeScene : m_Scene;
    TerrainPainter::Update(
        sceneForPainting,
        m_SelectedEntity,
        m_PlayMode,
        m_ViewportPanel.IsWindowFocusedOrHovered(),
        !m_ViewportPanel.IsViewportInteractionLocked(),
        m_ViewportPanel.GetViewportCamera());

    NavLinkPainter::Update(
        sceneForPainting,
        m_SelectedEntity,
        m_PlayMode,
        m_ViewportPanel.IsWindowFocusedOrHovered(),
        !m_ViewportPanel.IsViewportInteractionLocked(),
        m_ViewportPanel.GetViewportCamera());

    // River Cutter tool - update and draw path visualization
    m_RiverCutterPanel.Update(
        m_ViewportPanel.IsWindowFocusedOrHovered(),
        m_PlayMode,
        m_ViewportPanel.GetViewportCamera());
    m_RiverCutterPanel.DrawPathVisualization();
    m_SplineToolPanel.Update(
        m_ViewportPanel.IsWindowFocusedOrHovered(),
        m_PlayMode,
        m_ViewportPanel.GetViewportCamera());
    m_SplineToolPanel.DrawSplineVisualization();
    m_SoftbodyPainter.SetContext(&sceneForPainting);
    m_SoftbodyPainter.SetSelectedEntityPtr(&m_SelectedEntity);
    m_SoftbodyPainter.SyncSelectionTarget();
    m_SoftbodyPainter.Update(
        m_ViewportPanel.IsWindowFocusedOrHovered(),
        m_PlayMode,
        m_ViewportPanel.GetViewportCamera());
    m_SoftbodyPainter.DrawVisualization(m_ViewportPanel.GetViewportCamera());

    // Resource Layer tool - update and draw eligibility preview
    m_ResourceLayerPanel.Update(
        m_ViewportPanel.IsWindowFocusedOrHovered(),
        m_PlayMode,
        m_ViewportPanel.GetViewportCamera());
    m_ResourceLayerPanel.DrawEligibilityPreview();

    m_CommandPalette.Render(m_ActionRegistry);
    m_Notifications.Render();

    // Deferred scene loading
    ProcessDeferredSceneLoad();
    EditorSceneUndoStack::Get().Update();
    SyncEditorPersistence();
}

// =============================
// Dockspace + Toolbar Layout
// =============================
void UILayer::BeginDockspace() {
    UpdateCachedSceneTitle();
    ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);
    const ImVec4 accent = style.Colors[ImGuiCol_TabSelectedOverline];
    const ImVec4 chromeBg = LerpImVec4(style.Colors[ImGuiCol_MenuBarBg], style.Colors[ImGuiCol_WindowBg], 0.62f);
    const ImVec4 chromeBgHi = LerpImVec4(chromeBg, accent, 0.02f);
    const ImVec4 chromeBorder = WithAlpha(style.Colors[ImGuiCol_Border], 0.76f);
    const ImVec4 toolbarSurface = LerpImVec4(style.Colors[ImGuiCol_ChildBg], style.Colors[ImGuiCol_WindowBg], 0.60f);
    const ImVec4 toolbarSurfaceHi = LerpImVec4(toolbarSurface, accent, 0.02f);
    const ImVec4 toolbarBorder = WithAlpha(style.Colors[ImGuiCol_Border], 0.86f);
    const ImVec4 brandColor = LerpImVec4(accent, style.Colors[ImGuiCol_Text], 0.55f);
    const ImVec4 subtleBorder = WithAlpha(style.Colors[ImGuiCol_Border], 0.42f);

    style.Colors[ImGuiCol_ResizeGrip] = transparent;
    style.Colors[ImGuiCol_ResizeGripHovered] = transparent;
    style.Colors[ImGuiCol_ResizeGripActive] = transparent;
    style.Colors[ImGuiCol_Separator] = WithAlpha(subtleBorder, 0.26f);
    style.Colors[ImGuiCol_SeparatorHovered] = WithAlpha(LerpImVec4(subtleBorder, accent, 0.16f), 0.38f);
    style.Colors[ImGuiCol_SeparatorActive] = WithAlpha(LerpImVec4(subtleBorder, accent, 0.28f), 0.54f);
    style.Colors[ImGuiCol_Border] = subtleBorder;
    style.Colors[ImGuiCol_BorderShadow] = transparent;
    style.WindowBorderSize = 0.75f;
    style.ChildBorderSize = 0.75f;
    style.TabBarBorderSize = 0.0f;
    style.DockingSeparatorSize = 1.35f;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float statusBarHeight = ImGui::GetFrameHeight();
    const float toolbarRowHeight = 36.0f;
    const float topBarHeight = ImGui::GetFrameHeight() + toolbarRowHeight + style.WindowPadding.y * 2.0f + 8.0f;
    const float dockTopOffset = topBarHeight + 2.0f;

    ImGuiWindowFlags top_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, topBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);
    // Keep top bar host undocked and always visible.
    ImGui::SetNextWindowDockID(0, ImGuiCond_Always);

    top_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    top_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::Begin("TopToolbarHost", nullptr, top_flags);
    ImGui::PopStyleVar(2);

    ImDrawList* topDrawList = ImGui::GetWindowDrawList();
    const ImVec2 topMin = ImGui::GetWindowPos();
    const ImVec2 topMax = ImVec2(topMin.x + ImGui::GetWindowSize().x, topMin.y + ImGui::GetWindowSize().y);
    topDrawList->AddRectFilledMultiColor(
        topMin,
        topMax,
        ImGui::GetColorU32(chromeBgHi),
        ImGui::GetColorU32(LerpImVec4(chromeBgHi, accent, 0.02f)),
        ImGui::GetColorU32(chromeBg),
        ImGui::GetColorU32(chromeBg));
    topDrawList->AddLine(
        topMin,
        ImVec2(topMax.x, topMin.y),
        ImGui::GetColorU32(WithAlpha(accent, 0.10f)),
        1.0f);
    topDrawList->AddLine(
        ImVec2(topMin.x, topMax.y - 1.0f),
        ImVec2(topMax.x, topMax.y - 1.0f),
        ImGui::GetColorU32(chromeBorder),
        1.0f);

    // Menu Bar
    if (ImGui::BeginMenuBar()) {
        // Branding (left)
        ImGui::PushStyleColor(ImGuiCol_Text, brandColor);
        ImGui::Text("Claymore");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        m_MenuBarPanel.OnImGuiRender();
        ImGui::EndMenuBar();
    }

    static bool sToolbarIconsLoaded = false;
    static ImTextureID sPlayIcon;
    static ImTextureID sPauseIcon;
    static ImTextureID sStopIcon;
    static ImTextureID sMoveIcon;
    static ImTextureID sRotateIcon;
    static ImTextureID sScaleIcon;
    if (!sToolbarIconsLoaded) {
        sPlayIcon  = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/play.svg"));
        sPauseIcon = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/pause.svg"));
        sStopIcon  = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/stop.svg"));
        sMoveIcon  = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/move.svg"));
        sRotateIcon = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/rotate.svg"));
        sScaleIcon = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/scale.svg"));
        sToolbarIconsLoaded = true;
    }

    auto drawGridCheckbox = [&](const char* label) {
        const bool isPlaying = m_ToolbarPanel.IsPlayMode();
        bool checkboxState = Renderer::Get().GetShowGrid();
        if (isPlaying) checkboxState = false;
        ImGui::BeginDisabled(isPlaying);
        if (ImGui::Checkbox(label, &checkboxState) && !isPlaying) {
            Renderer::Get().SetShowGrid(checkboxState);
        }
        ImGui::EndDisabled();
    };

    const ImVec4 activeButton = LerpImVec4(style.Colors[ImGuiCol_Button], accent, 0.18f);
    const ImVec4 activeButtonHovered = LerpImVec4(style.Colors[ImGuiCol_ButtonHovered], accent, 0.14f);
    const ImVec4 activeButtonActive = LerpImVec4(style.Colors[ImGuiCol_ButtonActive], accent, 0.10f);
    auto pushUnavailable = [&](){
        ImGui::PushStyleColor(ImGuiCol_Button, WithAlpha(style.Colors[ImGuiCol_Button], 0.40f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(style.Colors[ImGuiCol_ButtonHovered], 0.46f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithAlpha(style.Colors[ImGuiCol_ButtonActive], 0.52f));
    };
    auto pushActiveLight = [&](){
        ImGui::PushStyleColor(ImGuiCol_Button, activeButton);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeButtonHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeButtonActive);
    };
    auto popColors = [](){ ImGui::PopStyleColor(3); };

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    const float toolbarCardHeight = toolbarRowHeight;
    const float toolbarRounding = 6.0f;
    const ImVec2 toolbarMin = ImGui::GetCursorScreenPos();
    const ImVec2 toolbarSize(ImGui::GetContentRegionAvail().x, toolbarCardHeight);
    topDrawList->AddRectFilled(
        toolbarMin,
        ImVec2(toolbarMin.x + toolbarSize.x, toolbarMin.y + toolbarSize.y),
        ImGui::GetColorU32(toolbarSurface),
        toolbarRounding);
    topDrawList->AddLine(
        ImVec2(toolbarMin.x + 1.0f, toolbarMin.y + 1.0f),
        ImVec2(toolbarMin.x + toolbarSize.x - 1.0f, toolbarMin.y + 1.0f),
        ImGui::GetColorU32(WithAlpha(toolbarSurfaceHi, 0.90f)),
        1.0f);
    topDrawList->AddRect(
        toolbarMin,
        ImVec2(toolbarMin.x + toolbarSize.x, toolbarMin.y + toolbarSize.y),
        ImGui::GetColorU32(toolbarBorder),
        toolbarRounding);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, transparent);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, toolbarRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::BeginChild("GizmoBar", toolbarSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1.0f);

    if (m_ToolbarPanel.IsPlayMode() && m_EditorLightingOverride) {
        m_EditorLightingOverride = false;
        Renderer::Get().SetEditorLightingOverride(false);
    }

    const bool isPlaying = m_ToolbarPanel.IsPlayMode();
    const bool isPaused = m_ToolbarPanel.IsPaused();
    const ImVec2 iconSize(18, 18);
    const ImVec2 transportPadding(3, 3);
    auto drawTransportButton = [&](const char* id, ImTextureID icon, float yOffsetPx = 0.0f) {
        const float frameHeight = ImGui::GetFrameHeight();
        const float buttonHeight = iconSize.y + transportPadding.y * 2.0f;
        if (buttonHeight < frameHeight) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (frameHeight - buttonHeight) * 0.5f);
        }
        if (yOffsetPx != 0.0f) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + yOffsetPx);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, transportPadding);
        bool pressed = ImGui::ImageButton(id, icon, iconSize);
        ImGui::PopStyleVar();
        return pressed;
    };

    if (isPlaying) {
        ImGui::BeginDisabled(true);
        pushUnavailable();
        drawTransportButton("##play_gizmo", sPlayIcon);
        popColors();
        ImGui::EndDisabled();
    } else {
        if (drawTransportButton("##play_gizmo", sPlayIcon)) {
            m_ToolbarPanel.TogglePlayMode();
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Play");
    }
    ImGui::SameLine();

    if (!isPlaying) {
        ImGui::BeginDisabled(true);
        pushUnavailable();
        drawTransportButton("##pause_gizmo", sPauseIcon);
        popColors();
        ImGui::EndDisabled();
    } else {
        if (isPaused) pushActiveLight();
        if (drawTransportButton("##pause_gizmo", sPauseIcon)) {
            m_ToolbarPanel.TogglePause();
        }
        if (isPaused) popColors();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Pause");
    }
    ImGui::SameLine();

    if (!isPlaying) {
        ImGui::BeginDisabled(true);
        pushUnavailable();
        drawTransportButton("##stop_gizmo", sStopIcon);
        popColors();
        ImGui::EndDisabled();
    } else {
        if (drawTransportButton("##stop_gizmo", sStopIcon)) {
            m_ToolbarPanel.TogglePlayMode();
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Stop");
    }
    ImGui::SameLine();

    ImGui::BeginDisabled(isPlaying);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Play Mode:");
    ImGui::SameLine();
    bool binaryMode = m_ToolbarPanel.IsBinaryPlayModeEnabled();
    const char* modeLabel = binaryMode ? "Binary Preview" : "Live";
    ImGui::SetNextItemWidth(126.0f);
    if (ImGui::BeginCombo("##PlayModeSelectGizmo", modeLabel)) {
        if (ImGui::Selectable("Live", !binaryMode)) {
            m_ToolbarPanel.SetBinaryPlayModeEnabled(false);
            binaryMode = false;
        }
        if (ImGui::Selectable("Binary Preview", binaryMode)) {
            m_ToolbarPanel.SetBinaryPlayModeEnabled(true);
            binaryMode = true;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Window:");
    ImGui::SameLine();
    PlayWindowMode windowMode = m_ToolbarPanel.GetPlayWindowMode();
    const char* windowModeLabel = "Editor";
    switch (windowMode) {
        case PlayWindowMode::Windowed: windowModeLabel = "Windowed"; break;
        case PlayWindowMode::Fullscreen: windowModeLabel = "Fullscreen"; break;
        case PlayWindowMode::Editor:
        default: windowModeLabel = "Editor"; break;
    }
    ImGui::SetNextItemWidth(108.0f);
    if (ImGui::BeginCombo("##PlayWindowSelectGizmo", windowModeLabel)) {
        if (ImGui::Selectable("Editor", windowMode == PlayWindowMode::Editor)) {
            m_ToolbarPanel.SetPlayWindowMode(PlayWindowMode::Editor);
            windowMode = PlayWindowMode::Editor;
        }
        if (ImGui::Selectable("Windowed", windowMode == PlayWindowMode::Windowed)) {
            m_ToolbarPanel.SetPlayWindowMode(PlayWindowMode::Windowed);
            windowMode = PlayWindowMode::Windowed;
        }
        if (ImGui::Selectable("Fullscreen", windowMode == PlayWindowMode::Fullscreen)) {
            m_ToolbarPanel.SetPlayWindowMode(PlayWindowMode::Fullscreen);
            windowMode = PlayWindowMode::Fullscreen;
        }
        ImGui::EndCombo();
    }
    if (windowMode != PlayWindowMode::Editor) {
        m_ToolbarPanel.SetBinaryPlayModeEnabled(true);
        m_ToolbarPanel.SetUseTempPakForPlayMode(true);
        ImGui::SameLine();
        ImGui::TextDisabled("External play uses a temporary runtime build");
    } else if (binaryMode) {
        ImGui::SameLine();
        bool useTempPak = m_ToolbarPanel.UseTempPakForPlayMode();
        if (ImGui::Checkbox("Temp PAK", &useTempPak)) {
            m_ToolbarPanel.SetUseTempPakForPlayMode(useTempPak);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Build a temporary runtime PAK in .bin for this play session.");
        }
    } else {
        m_ToolbarPanel.SetUseTempPakForPlayMode(false);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();

    bool gizmos = m_ViewportPanel.GetShowGizmos();
    if (ImGui::Checkbox("Gizmos", &gizmos)) {
        m_ViewportPanel.SetShowGizmos(gizmos);
        m_ToolbarPanel.SetShowGizmosEnabled(gizmos);
        for (auto& pe : m_PrefabEditors) { (void)pe; }
    }
    ImGui::SameLine();

    if (!m_ToolbarPanel.IsPlayMode()) {
        bool overrideActive = m_EditorLightingOverride;
        if (overrideActive) {
            pushActiveLight();
        }
        if (ImGui::Button("Fog/Lights")) {
            m_EditorLightingOverride = !m_EditorLightingOverride;
            Renderer::Get().SetEditorLightingOverride(m_EditorLightingOverride);
        }
        if (overrideActive) {
            ImGui::PopStyleColor(3);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("Temporarily disable fog and scene lights in the editor viewport.");
        }
        ImGui::SameLine();
    }

    // Operation buttons (T/R/S)
    const ImVec2 gizmoIconSize(14.0f, 14.0f);
    auto drawOpBtn2 = [&](ImGuizmo::OPERATION op, const char* id, ImTextureID icon, const char* tooltip) {
        bool active = (m_ViewportPanel.GetCurrentOperation() == op);
        if (active) {
            pushActiveLight();
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 4.0f));
        if (ImGui::ImageButton(id, icon, gizmoIconSize)) {
            m_ViewportPanel.SetOperation(op);
        }
        ImGui::PopStyleVar();
        if (active) popColors();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("%s", tooltip);
        }
        ImGui::SameLine();
    };
    drawOpBtn2(ImGuizmo::TRANSLATE, "##gizmo_translate", sMoveIcon, "Translate");
    drawOpBtn2(ImGuizmo::ROTATE,    "##gizmo_rotate", sRotateIcon, "Rotate");
    drawOpBtn2(ImGuizmo::SCALE,     "##gizmo_scale", sScaleIcon, "Scale");

    // Optional: quick view options combo
    ImGui::SetNextItemWidth(84.0f);
    if (ImGui::BeginCombo("##ViewOpts2", "Options")) {
        drawGridCheckbox("Debug Grid");
        bool aabbs2 = Renderer::Get().GetShowAABBs();
        if (ImGui::Checkbox("Picking AABBs", &aabbs2)) Renderer::Get().SetShowAABBs(aabbs2);
        bool colliders2 = Renderer::Get().GetShowColliders();
        if (ImGui::Checkbox("Colliders", &colliders2)) Renderer::Get().SetShowColliders(colliders2);
        bool cameraBounds2 = Renderer::Get().GetShowCameraFrustums();
        if (ImGui::Checkbox("Camera Frustums", &cameraBounds2)) Renderer::Get().SetShowCameraFrustums(cameraBounds2);
        bool shadowAtlasDbg2 = Renderer::Get().GetShowShadowDebugOverlay();
        if (ImGui::Checkbox("Shadow Atlas Debug", &shadowAtlasDbg2)) Renderer::Get().SetShowShadowDebugOverlay(shadowAtlasDbg2);
        bool uirects2 = Renderer::Get().GetShowUIRects();
        if (ImGui::Checkbox("UI Rects", &uirects2)) Renderer::Get().SetShowUIRects(uirects2);
        uint32_t mask2 = (uint32_t)nav::debug::GetMask();
        auto toggle2 = [&](const char* label, nav::NavDrawMask bit){ bool v = (mask2 & (uint32_t)bit)!=0; if(ImGui::Checkbox(label, &v)){ if(v) mask2 |= (uint32_t)bit; else mask2 &= ~(uint32_t)bit; }};
        toggle2("Nav Triangles", nav::NavDrawMask::TriMesh);
        toggle2("Nav Polys",     nav::NavDrawMask::Polys);
        toggle2("Nav Agents",    nav::NavDrawMask::Agents);
        nav::Navigation::Get().SetDebugMask((nav::NavDrawMask)mask2);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    bool allowInPlayBtn = Renderer::Get().GetDebugDrawInPlayMode();
    if (ImGui::Checkbox("Debug", &allowInPlayBtn)) {
        Renderer::Get().SetDebugDrawInPlayMode(allowInPlayBtn);
        if (allowInPlayBtn) {
            m_ShowRuntimeStatsPanel = true;
        }
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::BeginCombo("##EditorCameraSettings", "Camera")) {
        constexpr float kDefaultEditorFov  = 60.0f;
        constexpr float kDefaultEditorNear = 0.1f;
        constexpr float kDefaultEditorFar  = 1000.0f;

        Camera* editorCamera = m_ViewportPanel.GetViewportCamera();
        if (!editorCamera) {
            ImGui::TextDisabled("Editor camera not available.");
        } else {
            const bool cameraLocked = m_ToolbarPanel.IsPlayMode();
            if (cameraLocked) {
                ImGui::BeginDisabled();
            }

            float fov = editorCamera->GetFieldOfView();
            if (ImGui::SliderFloat("Field of View", &fov, 15.0f, 120.0f, "%.1f deg")) {
                editorCamera->SetFieldOfView(fov);
            }

            float nearClip = editorCamera->GetNearClip();
            float maxNear = std::max(0.001f, editorCamera->GetFarClip() - 0.001f);
            if (ImGui::SliderFloat("Near Clip", &nearClip, 0.01f, 10.0f, "%.3f", ImGuiSliderFlags_Logarithmic)) {
                nearClip = std::clamp(nearClip, 0.001f, maxNear);
                editorCamera->SetNearClip(nearClip);
            }

            float farClip = editorCamera->GetFarClip();
            if (ImGui::SliderFloat("Far Clip", &farClip, 10.0f, 20000.0f, "%.0f", ImGuiSliderFlags_Logarithmic)) {
                float minFar = editorCamera->GetNearClip() + 0.01f;
                farClip = std::max(farClip, minFar);
                editorCamera->SetFarClip(farClip);
            }

            if (ImGui::Button("Reset Defaults")) {
                editorCamera->SetFieldOfView(kDefaultEditorFov);
                editorCamera->SetNearClip(kDefaultEditorNear);
                editorCamera->SetFarClip(kDefaultEditorFar);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("Restore the default editor camera frustum.");
            }

            if (cameraLocked) {
                ImGui::EndDisabled();
                ImGui::Spacing();
                ImGui::TextDisabled("Camera settings locked in Play Mode.");
            } else {
                ImGui::Spacing();
                ImGui::TextDisabled("Applies to the editor viewport camera.");
            }
        }
        ImGui::EndCombo();
    }

    // Shader preset dropdown (right next to Options)
    ImGui::SameLine();
    {
        Scene& s = m_Scene;
        const char* curLabel = (s.GetDefaultShaderPreset() == Scene::ShaderPreset::PBR) ? "PBR" : "PSX";
        ImGui::SetNextItemWidth(82.0f);
        if (ImGui::BeginCombo("Shader", curLabel)) {
            auto convertAllTo = [&](Scene::ShaderPreset target){
                if (target == s.GetDefaultShaderPreset()) { ImGui::EndCombo(); return; }
                // Set preset first so scene-aware creators use the new target
                s.SetDefaultShaderPreset(target);

                for (auto& e : s.GetEntities()) {
                    auto* d = s.GetEntityData(e.GetID()); if (!d || !d->Mesh) continue;
                    const bool skinned = (d->Skinning != nullptr);

                    auto convertOne = [&](std::shared_ptr<Material>& matPtr, bool isSkinned){
                        if (!matPtr) return;
                        std::string n = matPtr->GetName();
                        // Check if material is a default type (including clones like "DefaultPBR_Clone")
                        bool isDefault = (n.find("DefaultPBR") != std::string::npos || 
                                          n.find("SkinnedPBR") != std::string::npos || 
                                          n.find("PSX") != std::string::npos ||
                                          n.find("SkinnedPSX") != std::string::npos);
                        if (!isDefault) return;

                        // carry over albedo texture, tint and state flags (e.g., Alpha Blend)
                        bgfx::TextureHandle albedo = BGFX_INVALID_HANDLE;
                        bgfx::TextureHandle mrTex = BGFX_INVALID_HANDLE;
                        bgfx::TextureHandle nrmTex = BGFX_INVALID_HANDLE;
                        std::string albedoPath, mrPath, normalPath;
                        glm::vec4 tint(1,1,1,1);
                        uint64_t stateFlagsOld = matPtr->GetStateFlags();
                        if (auto pbr = std::dynamic_pointer_cast<PBRMaterial>(matPtr)) {
                            albedo = pbr->m_AlbedoTex;
                            mrTex = pbr->m_MetallicRoughnessTex;
                            nrmTex = pbr->m_NormalTex;
                            albedoPath = pbr->GetAlbedoPath();
                            mrPath = pbr->GetMetallicRoughnessPath();
                            normalPath = pbr->GetNormalPath();
                            matPtr->TryGetUniform("u_ColorTint", tint);
                        }

                        std::shared_ptr<Material> newMat = isSkinned
                            ? MaterialManager::Instance().CreateSceneSkinnedDefaultMaterial(&s)
                            : MaterialManager::Instance().CreateSceneDefaultMaterial(&s);
                        if (auto npbr = std::dynamic_pointer_cast<PBRMaterial>(newMat)) {
                            // Prefer preserving source paths so serialization writes them back
                            if (!albedoPath.empty()) npbr->SetAlbedoTextureFromPath(albedoPath);
                            else if (bgfx::isValid(albedo)) npbr->SetAlbedoTexture(albedo);

                            if (!mrPath.empty()) npbr->SetMetallicRoughnessTextureFromPath(mrPath);
                            else if (bgfx::isValid(mrTex)) npbr->SetMetallicRoughnessTexture(mrTex);

                            if (!normalPath.empty()) npbr->SetNormalTextureFromPath(normalPath);
                            else if (bgfx::isValid(nrmTex)) npbr->SetNormalTexture(nrmTex);
                        }
                        newMat->SetUniform("u_ColorTint", tint);
                        // Preserve blending/culling/depth state from the previous material
                        newMat->m_StateFlags = stateFlagsOld;
                        matPtr = newMat;
                    };

                    // Convert primary material if default
                    convertOne(d->Mesh->material, skinned);
                    // Convert all slot materials if present
                    for (auto& sm : d->Mesh->materials) convertOne(sm, skinned);
                    // Keep primary aligned with slot 0 if slots exist
                    if (!d->Mesh->materials.empty()) d->Mesh->material = d->Mesh->materials[0];
                }
            };
            bool selPBR = (s.GetDefaultShaderPreset() == Scene::ShaderPreset::PBR);
            bool selPSX = (s.GetDefaultShaderPreset() == Scene::ShaderPreset::PSX);
            if (ImGui::Selectable("PBR", selPBR)) convertAllTo(Scene::ShaderPreset::PBR);
            if (ImGui::Selectable("PSX", selPSX)) convertAllTo(Scene::ShaderPreset::PSX);
            ImGui::EndCombo();
        }

    }

    ImGui::EndChild();
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor();
    ImGui::End(); // TopToolbarHost

    // Render any modals requested by menu items after the menu bar has closed
    m_MenuBarPanel.RenderExportPopup();
    
    // Open Project Settings modal if requested this frame
    if (m_ShowProjectSettings) {
        ImGui::SetNextWindowSize(ImVec2(820, 520), ImGuiCond_Appearing);
        ImGui::OpenPopup("Project Settings");
        m_ProjectSettingsOpen = true;
        m_ShowProjectSettings = false;
    }
    // Size/constraints similar to Serializer Sanity window; ensure contents fit and can scroll
    ImGui::SetNextWindowSize(ImVec2(820, 520), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(640, 420), ImVec2(1920, 1200));
    ImGuiWindowFlags projFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
    if (ImGui::BeginPopupModal("Project Settings", &m_ProjectSettingsOpen, projFlags)) {
        // Reserve a footer area for action buttons to avoid content overlap and nested scrollbars
        float footerH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 childSize(avail.x, std::max(0.0f, avail.y - footerH));
        if (ImGui::BeginChild("##proj_body", childSize, ImGuiChildFlags_AlwaysUseWindowPadding)) {
            m_ProjectSettingsPanel.OnImGuiRenderEmbedded();
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (ImGui::Button("Close")) { m_ProjectSettingsOpen = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // Dedicated dock host below top toolbars so dock splitters cannot draw over toolbar rows.
    ImGuiWindowFlags dock_flags = ImGuiWindowFlags_NoDocking;
    dock_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    dock_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + dockTopOffset));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, std::max(1.0f, viewport->WorkSize.y - dockTopOffset)));
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("DockSpace", nullptr, dock_flags);
    ImGui::PopStyleVar(2);

    // DockSpace main area
    m_MainDockspaceID = ImGui::GetID("MyDockSpace");
    ImGuiID dockspace_id = m_MainDockspaceID;

    // Ensure the dockspace node is a root node; if corrupted (e.g., created inside a child), rebuild it
    if (ImGuiDockNode* existingNode = ImGui::DockBuilderGetNode(m_MainDockspaceID)) {
        if (existingNode->ParentNode != nullptr) {
            ImGui::DockBuilderRemoveNode(m_MainDockspaceID);
            m_LayoutInitialized = false;
        }
    }

    // Default professional layout on first run or when reset requested
    if (!m_LayoutInitialized || m_ResetLayoutRequested) {
        m_ResetLayoutRequested = false;
        m_LayoutInitialized = true;
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImVec2 vpSize = ImGui::GetContentRegionAvail();
        vpSize.y = std::max(1.0f, vpSize.y - statusBarHeight);
        if ((vpSize.x <= 0.0f || vpSize.y <= 0.0f)) {
            // Fallback to IO display size; if still invalid, delay layout until next frame
            ImVec2 ds = ImGui::GetIO().DisplaySize;
            if (ds.x > 0.0f && ds.y > 0.0f) vpSize = ds;
        }
        if (vpSize.x > 0.0f && vpSize.y > 0.0f) {
            ImGui::DockBuilderSetNodeSize(dockspace_id, vpSize);
        } else {
            // Defer layout init when size is not yet known
            m_ResetLayoutRequested = true;
        }

        ImGuiID dock_left, dock_right, dock_down;
        dock_left = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.22f, nullptr, &dockspace_id);
        dock_right = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.28f, nullptr, &dockspace_id);
        dock_down = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.26f, nullptr, &dockspace_id);

        ImGui::DockBuilderDockWindow("Scene Hierarchy", dock_left);
        ImGui::DockBuilderDockWindow("Inspector", dock_right);
        ImGui::DockBuilderDockWindow("Project", dock_down);
        ImGui::DockBuilderDockWindow("Console", dock_down);
        ImGui::DockBuilderSetNodeActiveWindow(dock_right, "Inspector");
        // Dock the main viewport using its dynamic name with a stable ID so it becomes the leading tab
        {
            std::string uniqueViewportName = m_CachedSceneTitle + " - Viewport###Viewport";
            ImGui::DockBuilderDockWindow(uniqueViewportName.c_str(), dockspace_id);
            ImGui::DockBuilderSetNodeActiveWindow(dockspace_id, uniqueViewportName.c_str());
        }
        ImGui::DockBuilderFinish(m_MainDockspaceID);
    }

    ImGui::DockSpace(m_MainDockspaceID, ImVec2(0.0f, -statusBarHeight), ImGuiDockNodeFlags_PassthruCentralNode);

    // After all windows are created and docked (and imgui restored layout), enforce leftmost+active tabs once
    if (!m_EnforcedTabsOnce) {
        EnforceDefaultActiveTabs();
    }

    // Status bar
    const ImVec4 statusBg = ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg];
    const ImVec4 modeColor = m_PlayMode ? ImVec4(0.42f, 0.90f, 0.58f, 1.0f) : ImVec4(0.62f, 0.72f, 0.86f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(statusBg.x + 0.02f, statusBg.y + 0.02f, statusBg.z + 0.02f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 2.0f));
    ImGui::BeginChild("StatusBar", ImVec2(0, statusBarHeight), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextDisabled("FPS %.1f", ImGui::GetIO().Framerate);
    ImGui::SameLine();
    ImGui::TextDisabled("Entities %d", (int)m_Scene.GetEntities().size());
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextDisabled("Mode");
    ImGui::SameLine();
    ImGui::TextColored(modeColor, "%s", m_PlayMode ? "PLAY" : "EDIT");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    const char* selName = "None";
    if (m_SelectedEntity != (EntityID)-1) {
        if (auto* data = m_Scene.GetEntityData(m_SelectedEntity)) selName = data->Name.c_str();
    }
    ImGui::TextDisabled("Selected %s", selName);
    if (!m_ProjectPanel.GetSelectedItemName().empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextDisabled("File %s", m_ProjectPanel.GetSelectedItemName().c_str());
    }
    const std::string sceneLabel = "Scene: " + m_CachedSceneTitle;
    const float sceneLabelWidth = ImGui::CalcTextSize(sceneLabel.c_str()).x;
    const float rightEdge = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightEdge - sceneLabelWidth));
    ImGui::TextDisabled("%s", sceneLabel.c_str());
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::End();
}

void UILayer::SyncSelectionContext() {
    const std::string& assetPath = m_ProjectPanel.GetSelectedItemPath();
    EntityID entitySelection = m_SelectedEntity;

    bool assetChanged = (assetPath != m_LastAssetSelection);
    bool entityChanged = (entitySelection != m_LastEntitySelectionTracked);

    if (entityChanged && entitySelection != -1) {
        m_SelectionSource = InspectorSelectionSource::Entity;
        m_LastEntitySelectionTracked = entitySelection;
        m_LastAssetSelection.clear();
        if (m_ProjectPanel.HasSelection()) {
            m_ProjectPanel.ClearSelection();
        }
        return;
    }

    if (assetChanged && !assetPath.empty()) {
        m_SelectionSource = InspectorSelectionSource::Asset;
        m_LastAssetSelection = assetPath;
        m_LastEntitySelectionTracked = -1;
        if (entitySelection != -1) {
            m_SelectedEntity = -1;
        }
        return;
    }

    if (assetChanged && assetPath.empty()) {
        m_LastAssetSelection.clear();
    }
    if (entityChanged && entitySelection == -1) {
        m_LastEntitySelectionTracked = -1;
    }

    if (!assetPath.empty()) {
        m_SelectionSource = InspectorSelectionSource::Asset;
        if (entitySelection != -1) {
            m_SelectedEntity = -1;
        }
    } else if (entitySelection != -1) {
        m_SelectionSource = InspectorSelectionSource::Entity;
        if (m_ProjectPanel.HasSelection()) {
            m_ProjectPanel.ClearSelection();
        }
    } else {
        m_SelectionSource = InspectorSelectionSource::None;
    }
}

void UILayer::UpdateCachedSceneTitle() {
    const bool isDirty = m_Scene.IsDirty();
    if (m_CachedScenePath == m_CurrentScenePath && m_CachedSceneDirty == isDirty) {
        return;
    }

    m_CachedScenePath = m_CurrentScenePath;
    m_CachedSceneDirty = isDirty;
    if (m_CurrentScenePath.empty()) {
        m_CachedSceneTitle = "Untitled";
    } else {
        m_CachedSceneTitle = fs::path(m_CurrentScenePath).stem().string();
    }
    if (isDirty) {
        m_CachedSceneTitle += "*";
    }
}

// Force specific docked tabs to be leftmost/active once at startup without stealing focus
void UILayer::EnforceDefaultActiveTabs() {
    // Defer marking as done until we actually find the target windows so this
    // keeps retrying across the first few frames until the dockspace is populated.
    bool ensuredInspector = false;
    bool ensuredViewport  = false;

    // Build the dynamic viewport title in the same way as the viewport panel.
    UpdateCachedSceneTitle();
    std::string viewportTitle = m_CachedSceneTitle + " - Viewport###Viewport";

    // Attempt to move the Viewport and Inspector tabs to the leftmost position in their tab bars
    // and make them the active tabs in their respective dock nodes.
    // Avoid calling SetWindowFocus here: that can continuously pull keyboard/mouse focus
    // away from undocked editor windows while the dockspace is still initializing.
    if (ImGuiDockNode* root = ImGui::DockBuilderGetNode(m_MainDockspaceID)) {
        // Find root-most node
        while (root && root->ParentNode) root = root->ParentNode;
        if (root) {
            ImGuiWindow* vpWin = ImGui::FindWindowByName(viewportTitle.c_str());
            if (vpWin && vpWin->DockNode) {
                ImGui::DockBuilderDockWindow(viewportTitle.c_str(), vpWin->DockNode->ID);
                ImGui::DockBuilderSetNodeActiveWindow(vpWin->DockNode->ID, viewportTitle.c_str());
                ensuredViewport = true;
            }
            ImGuiWindow* inspWin = ImGui::FindWindowByName("Inspector");
            if (inspWin && inspWin->DockNode) {
                ImGui::DockBuilderDockWindow("Inspector", inspWin->DockNode->ID);
                ImGui::DockBuilderSetNodeActiveWindow(inspWin->DockNode->ID, "Inspector");
                ensuredInspector = true;
            }
        }
    }

    // Only stop retrying once we have positively applied at least one focus operation.
    if (ensuredInspector || ensuredViewport) {
        m_EnforcedTabsOnce = true;
    }
}

// =============================
// Scene helpers
// =============================
void UILayer::CreateDebugCubeEntity() {
    auto cubeEntity = m_Scene.CreateEntity("Debug Cube");
    EntityData* data = m_Scene.GetEntityData(cubeEntity.GetID());
    data->Mesh = std::make_unique<MeshComponent>(
        StandardMeshManager::Instance().GetCubeMesh(),
        std::string("DebugCube"),
        nullptr);
    data->Mesh->meshReference = AssetReference::CreatePrimitive("Cube");
    data->Mesh->material = MaterialManager::Instance().CreateSceneDefaultMaterial(&m_Scene);
    if (!data->RenderOverrides) {
        data->RenderOverrides = std::make_unique<RenderOverridesComponent>();
    }

}

void UILayer::CreateDefaultLight() {
    auto lightEntity = m_Scene.CreateEntity("Default Light");
    EntityData* data = m_Scene.GetEntityData(lightEntity.GetID());
    data->Light = std::make_unique<LightComponent>(LightType::Directional, glm::vec3(1.0f), 1.0f);
}

// =============================
// Play Mode Toggle
// =============================
void UILayer::TogglePlayMode() {
    m_PlayMode = !m_PlayMode;
    if (m_PlayMode) {
        m_ShowRuntimeStatsPanel = true;
    }
    Scene* activeScene = m_PlayMode ? m_Scene.m_RuntimeScene.get() : &m_Scene;

    // Update panel contexts
    // Also reset sticky routing to avoid dangling pointers to the destroyed runtime scene
    m_ActiveEditorScene = activeScene;
    m_ActiveSelectedEntityPtr = &m_SelectedEntity;
    m_SceneHierarchyPanel.SetContext(activeScene);
    m_SceneHierarchyPanel.SetSelectedEntityPtr(m_ActiveSelectedEntityPtr);
    m_InspectorPanel.SetContext(activeScene);
    m_ViewportPanel.SetContext(activeScene);
    
    // Notify all prefab editors of play mode state (they should disable delta computation/saving)
    for (auto& pe : m_PrefabEditors) {
        if (pe) pe->SetPlayMode(m_PlayMode);
    }
}

void UILayer::EndRuntimePreview() {
    if (!m_RuntimePreviewActive) return;

    // Restore resolver
    if (m_PrePlayResolver) {
        Assets::SetResolver(m_PrePlayResolver);
    }
    m_RuntimePreviewResolver.reset();

    // Restore pak mount
    if (!m_PrePlayPakPath.empty()) {
        FileSystem::Instance().MountPak(m_PrePlayPakPath);
    } else {
        FileSystem::Instance().UnmountPak();
    }

    // Restore disk fallback
    FileSystem::Instance().SetAllowDiskFallback(m_PrePlayDiskFallbackAllowed);

    // Clear runtime-only registries
    cm::ModelRegistry::Instance().Clear();
    runtime::RuntimePrefabInstantiator::ResetRuntimeCaches();

    if (m_RestoreResourceManifest && !m_PrePlayResourceManifestPath.empty()) {
        if (!ResourceManifest::Get().LoadFromFile(m_PrePlayResourceManifestPath)) {
            ResourceManifest::Get().Initialize(Project::GetProjectDirectory());
            ResourceManifest::Get().Scan();
        }
    }

    m_PrePlayResolver = nullptr;
    m_PrePlayPakPath.clear();
    m_PrePlayResourceManifestPath.clear();
    m_RestoreResourceManifest = false;
    m_RuntimePreviewActive = false;
}

void UILayer::StopExternalRuntimePreview() {
    HANDLE processHandle = reinterpret_cast<HANDLE>(m_ExternalRuntimeProcessHandle);
    HANDLE threadHandle = reinterpret_cast<HANDLE>(m_ExternalRuntimeThreadHandle);

    if (processHandle) {
        DWORD exitCode = 0;
        if (GetExitCodeProcess(processHandle, &exitCode) && exitCode == STILL_ACTIVE) {
            const DWORD processId = GetProcessId(processHandle);
            HWND runtimeWindow = FindRuntimePreviewWindow(processId);
            if (runtimeWindow) {
                PostMessageW(runtimeWindow, WM_CLOSE, 0, 0);
                if (WaitForSingleObject(processHandle, 1500) != WAIT_OBJECT_0) {
                    TerminateProcess(processHandle, 0);
                }
            } else {
                TerminateProcess(processHandle, 0);
            }
        }
        CloseHandle(processHandle);
    }
    if (threadHandle) {
        CloseHandle(threadHandle);
    }

    m_ExternalRuntimeProcessHandle = nullptr;
    m_ExternalRuntimeThreadHandle = nullptr;
    m_ExternalRuntimePreviewActive = false;

    if (m_PlayMode) {
        TogglePlayMode();
    }
}

void UILayer::PollExternalRuntimePreview() {
    if (!m_ExternalRuntimePreviewActive || !m_ExternalRuntimeProcessHandle) {
        return;
    }

    HANDLE processHandle = reinterpret_cast<HANDLE>(m_ExternalRuntimeProcessHandle);
    DWORD waitResult = WaitForSingleObject(processHandle, 0);
    if (waitResult != WAIT_OBJECT_0) {
        return;
    }

    HANDLE threadHandle = reinterpret_cast<HANDLE>(m_ExternalRuntimeThreadHandle);
    if (processHandle) {
        CloseHandle(processHandle);
    }
    if (threadHandle) {
        CloseHandle(threadHandle);
    }

    m_ExternalRuntimeProcessHandle = nullptr;
    m_ExternalRuntimeThreadHandle = nullptr;
    m_ExternalRuntimePreviewActive = false;

    m_ToolbarPanel.AbortPlayMode();
    if (m_PlayMode) {
        TogglePlayMode();
    }
}

bool UILayer::LaunchExternalRuntimePreview(const std::string& runtimeExePath,
                                          const std::string& contentRootPath,
                                          const std::string& pakPath,
                                          PlayWindowMode windowMode) {
    namespace fs = std::filesystem;

    fs::path runtimeExe(runtimeExePath);
    if (runtimeExe.empty() || !fs::exists(runtimeExe)) {
        Logger::LogError("[PlayMode] Preview runtime executable not found: " + runtimeExe.string());
        return false;
    }

    fs::path contentRoot = contentRootPath.empty() ? runtimeExe.parent_path() : fs::path(contentRootPath);
    if (contentRoot.is_relative()) {
        contentRoot = runtimeExe.parent_path() / contentRoot;
    }
    contentRoot = contentRoot.lexically_normal();

    fs::path explicitPak(pakPath);
    if (explicitPak.is_relative()) {
        explicitPak = contentRoot / explicitPak;
    }
    explicitPak = explicitPak.lexically_normal();
    if (explicitPak.empty() || !fs::exists(explicitPak)) {
        Logger::LogError("[PlayMode] Preview pak not found: " + explicitPak.string());
        return false;
    }

    auto quote = [](const std::wstring& value) {
        return std::wstring(L"\"") + value + L"\"";
    };

    const std::string projName = GetProjectNameOrFallback();
    std::wstring args;
    args += L"--content-root ";
    args += quote(contentRoot.wstring());
    args += L" --pak ";
    args += quote(explicitPak.wstring());
    args += L" --title ";
    args += quote(std::wstring(projName.begin(), projName.end()) + L" Preview");
    if (windowMode == PlayWindowMode::Fullscreen) {
        args += L" --fullscreen";
    } else {
        args += L" --windowed";
        args += L" --width 1600 --height 900";
    }

    std::wstring commandLine = quote(runtimeExe.wstring()) + L" " + args;
    std::wstring workingDirectory = contentRoot.wstring();
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};

    BOOL success = CreateProcessW(runtimeExe.wstring().c_str(),
                                  commandLine.data(),
                                  nullptr,
                                  nullptr,
                                  FALSE,
                                  CREATE_NEW_PROCESS_GROUP,
                                  nullptr,
                                  workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                                  &startupInfo,
                                  &processInfo);
    if (!success) {
        Logger::LogError("[PlayMode] Failed to launch staged runtime preview executable.");
        return false;
    }

    m_ExternalRuntimeProcessHandle = processInfo.hProcess;
    m_ExternalRuntimeThreadHandle = processInfo.hThread;
    return true;
}

// =============================
// Prefab Editor Management
// =============================
void UILayer::OpenPrefabEditor(const std::string& prefabPath) {
    // If an editor for this prefab already exists, focus it instead of opening another
    for (auto& ed : m_PrefabEditors) {
        if (ed && ed->GetPrefabPath() == prefabPath) {
            ed->RequestFocus();
            m_StickyPrefabEditor = ed.get(); // Make it sticky
            return;
        }
    }
    m_PrefabEditors.emplace_back(std::make_unique<PrefabEditorPanel>(prefabPath, this));
    // Automatically make the new prefab editor the sticky one
    m_StickyPrefabEditor = m_PrefabEditors.back().get();
}

void UILayer::OpenCodeEditor(const std::string& filePath) {
    // If already open, focus
    for (auto& ed : m_CodeEditors) {
        if (ed && ed->GetFilePath() == filePath) { ed->RequestFocus(); return; }
    }
    m_CodeEditors.emplace_back(std::make_unique<CodeEditorPanel>(filePath, this));
}

void UILayer::OpenAnimatorController(const std::string& path) {
    if (path.empty()) return;
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (!m_AnimGraphPanel.OpenControllerAsset(normalized)) {
        std::cerr << "[UILayer] Failed to open animator controller: " << normalized << std::endl;
        return;
    }
    m_AnimGraphPanel.Open();
}

void UILayer::OpenShaderGraph(const std::string& path) {
    if (path.empty()) return;
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (!m_ShaderGraphPanel.OpenGraph(normalized)) {
        std::cerr << "[UILayer] Failed to open shader graph: " << normalized << std::endl;
        return;
    }
    m_ShaderGraphPanel.Open();
}

void UILayer::RefreshShaderGraphDependencies(const std::string& shaderGraphPath) {
    ShaderGraphDependencySnapshot snapshot;
    if (!BuildShaderGraphDependencySnapshot(shaderGraphPath, snapshot)) {
        Logger::LogWarning("[ShaderGraph] Skipped dependency refresh because the graph could not be recompiled.");
        return;
    }

    size_t refreshedAssetCount = 0;
    std::error_code ec;
    fs::path projectRoot = Project::GetProjectDirectory();
    if (!projectRoot.empty() && fs::exists(projectRoot / "assets", ec)) {
        projectRoot /= "assets";
    }
    ec.clear();
    if (!projectRoot.empty() && fs::exists(projectRoot, ec)) {
        for (fs::recursive_directory_iterator it(projectRoot, ec), end; it != end && !ec; it.increment(ec)) {
            if (ec || !it->is_regular_file()) {
                continue;
            }

            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            bool refreshed = false;
            if (ext == ".sgmat") {
                refreshed = RefreshShaderGraphMaterialAsset(it->path(), snapshot);
            } else if (ext == ".mat") {
                refreshed = RefreshMaterialAssetFromShaderGraph(it->path(), snapshot);
            }

            if (refreshed) {
                ++refreshedAssetCount;
            }
        }
    }

    auto rebuildMaterial = [&](const std::shared_ptr<shadergraph::ShaderGraphMaterial>& material)
        -> std::shared_ptr<Material> {
        shadergraph::ShaderGraphMaterialDesc desc;
        desc.name = material->GetName();
        desc.shaderGraphPath = snapshot.normalizedGraphPath;
        desc.vertexShaderName = snapshot.graph.compiledVSName;
        desc.fragmentShaderName = snapshot.graph.compiledFSName;
        desc.uvScale = material->GetUVScale();
        desc.uvOffset = material->GetUVOffset();
        desc.stateFlags = material->GetStateFlags();
        desc.twoSided = (material->GetStateFlags() & BGFX_STATE_CULL_MASK) == 0;
        desc.parameters = BuildSyncedShaderGraphParameters(material->GetParameters(), snapshot.compileResult.parameters);
        return shadergraph::ShaderGraphMaterial::CreateFromDesc(desc);
    };

    size_t refreshedInstanceCount = 0;
    auto refreshScene = [&](Scene* scene) {
        if (!scene) {
            return;
        }

        std::unordered_map<const Material*, std::shared_ptr<Material>> replacementCache;
        auto refreshMaterialPtr = [&](std::shared_ptr<Material>& material) {
            auto shaderGraphMaterial = std::dynamic_pointer_cast<shadergraph::ShaderGraphMaterial>(material);
            if (!shaderGraphMaterial) {
                return;
            }

            if (NormalizeProjectPath(shaderGraphMaterial->GetShaderGraphPath()) != snapshot.normalizedGraphPath) {
                return;
            }

            if (const Material* key = shaderGraphMaterial.get()) {
                auto cachedIt = replacementCache.find(key);
                if (cachedIt != replacementCache.end()) {
                    material = cachedIt->second;
                    ++refreshedInstanceCount;
                    return;
                }

                std::shared_ptr<Material> rebuilt = rebuildMaterial(shaderGraphMaterial);
                if (rebuilt) {
                    replacementCache.emplace(key, rebuilt);
                    material = rebuilt;
                    ++refreshedInstanceCount;
                }
            }
        };

        for (const Entity& entity : scene->GetEntities()) {
            EntityData* data = scene->GetEntityData(entity.GetID());
            if (!data) {
                continue;
            }

            if (data->Mesh) {
                refreshMaterialPtr(data->Mesh->material);
                for (auto& material : data->Mesh->materials) {
                    refreshMaterialPtr(material);
                }
                if (!data->Mesh->materials.empty() && !data->Mesh->material) {
                    data->Mesh->material = data->Mesh->materials.front();
                }
            }

            if (data->MeshProxy) {
                for (auto& material : data->MeshProxy->SlotMaterialOverrides) {
                    refreshMaterialPtr(material);
                }
            }
        }
    };

    refreshScene(&m_Scene);
    refreshScene(m_Scene.m_RuntimeScene.get());
    for (auto& prefabEditor : m_PrefabEditors) {
        refreshScene(prefabEditor ? prefabEditor->GetScene() : nullptr);
    }

    if (refreshedAssetCount > 0 || refreshedInstanceCount > 0) {
        Logger::Log("[ShaderGraph] Refreshed " + std::to_string(refreshedAssetCount) +
            " material asset(s) and " + std::to_string(refreshedInstanceCount) +
            " live material instance(s).");
    }
}

// =============================
// Deferred Scene Loading
// =============================
void UILayer::DeferSceneLoad(const std::string& filepath) {
    m_DeferredScenePath = filepath;
    m_HasDeferredSceneLoad = true;
    BeginBlockingOverlay("Loading Scene...");
}

void UILayer::ProcessDeferredSceneLoad() {
    if (!m_HasDeferredSceneLoad) return;

    std::cout << "[UILayer] Processing deferred scene load: " << m_DeferredScenePath << std::endl;
    if (m_Scene.LoadSceneImmediate(m_DeferredScenePath, false)) {
        std::cout << "[UILayer] Successfully loaded scene: " << m_DeferredScenePath << std::endl;
        m_SelectedEntity = -1;
        m_CurrentScenePath = m_DeferredScenePath;
        // Reset viewport camera and interaction state after reload to ensure gizmo can capture input
        m_ViewportPanel.ClearPickRequest();
        ApplyEditorCameraState(m_Scene);
        EditorSceneUndoStack::Get().ResetToScene(&m_Scene, &m_SelectedEntity);
    } else {
        std::cerr << "[UILayer] Failed to load scene: " << m_DeferredScenePath << std::endl;
    }
    m_HasDeferredSceneLoad = false;
    m_DeferredScenePath.clear();
    EndBlockingOverlay();
}

void UILayer::BeginBlockingOverlay(const std::string& label) {
    m_BlockingOverlayActive = true;
    m_BlockingOverlayLabel = label;
}

void UILayer::EndBlockingOverlay() {
    m_BlockingOverlayActive = false;
    m_BlockingOverlayLabel.clear();
}

bool UILayer::HasTerrainSelection() {
    // Brush mode only works against the editor scene (not prefab editors/runtime)
    if (m_SelectedEntity == (EntityID)-1)
        return false;
    EntityData* data = m_Scene.GetEntityData(m_SelectedEntity);
    return data && data->Terrain != nullptr;
}

bool UILayer::HasNavMeshSelection() {
    if (m_SelectedEntity == (EntityID)-1)
        return false;
    EntityData* data = m_Scene.GetEntityData(m_SelectedEntity);
    return data && data->Navigation != nullptr;
}

bool UILayer::HasSplineSelection() {
    if (m_SelectedEntity == (EntityID)-1)
        return false;
    EntityData* data = m_Scene.GetEntityData(m_SelectedEntity);
    return data && data->Spline != nullptr;
}

bool UILayer::HasSoftbodySelection() {
    if (m_SelectedEntity == (EntityID)-1)
        return false;
    EntityData* data = m_Scene.GetEntityData(m_SelectedEntity);
    return data && data->Softbody != nullptr;
}

void UILayer::RenderBlockingOverlay() {
    if (!m_BlockingOverlayActive) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0,0,0,0.35f));
    ImGui::Begin("##BlockingOverlay", nullptr, flags);
    // Center box
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 box(360, 120);
    ImVec2 cursor = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(cursor.x + (avail.x - box.x)*0.5f, cursor.y + (avail.y - box.y)*0.5f));
    ImGui::BeginChild("##LoadingBox", box, true, ImGuiWindowFlags_NoScrollbar);
    ImGui::Text("%s", m_BlockingOverlayLabel.empty()? "Loading..." : m_BlockingOverlayLabel.c_str());
    ImGui::Separator();
    
    // Indeterminate bar alternative
    static float t = 0.0f; 
    t += 0.02f; 
    if (t > 1.0f) t = 0.0f;
     
    ImGui::ProgressBar(t, ImVec2(-1, 0));
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}  

// Queue async play begin so overlay can draw before heavy work
void UILayer::RequestBeginPlayAsync(bool binaryOnly, bool useTempPak, PlayWindowMode windowMode) {
    m_PendingPlayOptions.useTempPak = useTempPak;
    m_PendingPlayOptions.binaryOnly = binaryOnly || useTempPak;
    m_PendingPlayOptions.windowMode = windowMode;
    if (windowMode == PlayWindowMode::Editor) {
        BeginBlockingOverlay(useTempPak ? "Preparing Runtime Preview..." : "Starting Play Mode...");
    } else {
        BeginBlockingOverlay(windowMode == PlayWindowMode::Fullscreen
            ? "Launching Fullscreen Runtime Preview..."
            : "Launching Windowed Runtime Preview...");
    }
    m_BeginPlayRequested = true;
}

void UILayer::ProcessBeginPlayAsync() {
    if (!m_BeginPlayRequested) return;
    // Perform the heavy work now that at least one UI frame has rendered the overlay
    
    // Clear prefab failed cache so failed prefabs can be retried in the new play session
    ClearPrefabFailedCache();
    runtime::RuntimePrefabInstantiator::ResetRuntimeCaches();
    
    auto& scene = m_Scene;
    std::shared_ptr<Scene> baseScene;
    std::string entryScenePath;
    auto abortPlay = [&](const char* message) {
        if (message && *message) {
            std::cerr << message << std::endl;
        }
        EndRuntimePreview();
        EndBlockingOverlay();
        m_BeginPlayRequested = false;
        m_ToolbarPanel.AbortPlayMode();
    };

    if (m_PendingPlayOptions.useTempPak) {
        if (scene.IsDirty() || m_CurrentScenePath.empty()) {
            abortPlay("[PlayMode] Runtime Preview requires a saved scene. Save the scene and try again.");
            return;
        }

        const bool externalPreview = m_PendingPlayOptions.windowMode != PlayWindowMode::Editor;
        const std::string projName = GetProjectNameOrFallback();
        const fs::path cacheRoot = BinaryAssetCache::Instance().GetCacheRoot();
        const fs::path previewOutputDir = externalPreview ? (cacheRoot / "runtime_preview") : cacheRoot;
        if (externalPreview) {
            std::error_code cleanupEc;
            fs::remove_all(previewOutputDir, cleanupEc);
            if (cleanupEc) {
                std::cerr << "[PlayMode] Warning: Could not fully clear preview staging directory: "
                          << previewOutputDir << " (" << cleanupEc.message() << ")" << std::endl;
            }
        }

        std::error_code createDirEc;
        fs::create_directories(previewOutputDir, createDirEc);
        if (createDirEc) {
            std::cerr << "[PlayMode] Failed to prepare preview output directory: "
                      << previewOutputDir << " (" << createDirEc.message() << ")" << std::endl;
            abortPlay("[PlayMode] Failed to prepare runtime preview output directory.");
            return;
        }

        BuildExporter::Options opts;
        opts.outputDirectory = previewOutputDir.string();
        opts.entryScenes = { m_CurrentScenePath };
        opts.validateBeforeExport = true;
        opts.compressPak = true;
        opts.pakOnly = !externalPreview;
        opts.allowPartialBinaryBuilds = true;

        if (!BuildExporter::ExportProject(opts, [this](float, const std::string& status) {
            BeginBlockingOverlay(status);
        })) {
            abortPlay("[PlayMode] Runtime Preview export failed.");
            return;
        }

        const fs::path pakPath = previewOutputDir / (projName + ".pak");
        if (externalPreview) {
            const fs::path runtimeExePath = previewOutputDir / (projName + ".exe");
            if (!LaunchExternalRuntimePreview(runtimeExePath.string(),
                                              previewOutputDir.string(),
                                              pakPath.string(),
                                              m_PendingPlayOptions.windowMode)) {
                abortPlay("[PlayMode] Failed to launch external runtime preview.");
                return;
            }

            m_ExternalRuntimePreviewActive = true;
            TogglePlayMode();
            EndBlockingOverlay();
            m_BeginPlayRequested = false;
            return;
        }

        // Mount temp pak
        m_PrePlayPakPath = FileSystem::Instance().GetMountedPakPath();
        if (!FileSystem::Instance().MountPak(pakPath.string())) {
            std::string err = std::string("[PlayMode] Failed to mount preview pak: ") + pakPath.string();
            abortPlay(err.c_str());
            return;
        }

        m_PrePlayDiskFallbackAllowed = FileSystem::Instance().IsDiskFallbackAllowed();
        FileSystem::Instance().SetAllowDiskFallback(false);

        // Swap to runtime asset resolver and load manifest for GUID/path resolution
        m_PrePlayResolver = Assets::GetResolver();
        m_RuntimePreviewResolver = std::make_unique<RuntimeAssetResolver>();
        m_RuntimePreviewActive = true;
        {
            std::string manifestText;
            if (FileSystem::Instance().ReadTextFile("game_manifest.json", manifestText)) {
                m_RuntimePreviewResolver->LoadManifest(manifestText);
                try {
                    auto j = nlohmann::json::parse(manifestText);
                    entryScenePath = j.value("entryScene", "");
                } catch (...) {}
            } else {
                abortPlay("[PlayMode] Failed to read game_manifest.json from preview pak.");
                return;
            }
        }
        {
            std::string resourceText;
            if (FileSystem::Instance().ReadTextFile("resource_manifest.json", resourceText)) {
                m_PrePlayResourceManifestPath = (BinaryAssetCache::Instance().GetCacheRoot() / "resource_manifest_editor_backup.json").string();
                m_RestoreResourceManifest = ResourceManifest::Get().SaveToFile(m_PrePlayResourceManifestPath);
                m_RuntimePreviewResolver->LoadResourceManifest(resourceText);
            }
        }

        // Load model registry from pak
        cm::ModelRegistry::Instance().Load(".bin/model_registry.bin");
    }

    if (m_PendingPlayOptions.binaryOnly && !m_PendingPlayOptions.useTempPak) {
        // Serialize current scene to a temporary binary and load from it
        std::filesystem::path tempBinPath = BinaryAssetCache::Instance().GetCacheRoot() / "playmode_temp.sceneb";
        std::filesystem::path tempStampPath = tempBinPath;
        tempStampPath += ".stamp.json";
        std::error_code ec;
        std::filesystem::create_directories(tempBinPath.parent_path(), ec);
        if (ec) {
            std::cerr << "[PlayMode] Failed to create binary cache directory: " 
                      << tempBinPath.parent_path().string() << " (" << ec.message() << ")\n";
        }

        const uint64_t sceneHash = ComputePlayModeSceneHash(scene);
        uint64_t stampedHash = 0;
        const bool canReuseTempBinary =
            sceneHash != 0 &&
            std::filesystem::exists(tempBinPath) &&
            ReadPlayModeSceneStamp(tempStampPath, stampedHash) &&
            stampedHash == sceneHash;

        if (!canReuseTempBinary) {
            if (!binary::EntityBinaryWriter::Write(scene, tempBinPath.string())) {
                abortPlay("[PlayMode] Failed to write temporary binary scene.");
                return;
            }
            WritePlayModeSceneStamp(tempStampPath, sceneHash);
        } else {
            std::cout << "[PlayMode] Reusing current temporary binary scene: "
                      << tempBinPath.string() << "\n";
        }

        baseScene = std::make_shared<Scene>();
        if (!binary::EntityBinaryLoader::Load(tempBinPath.string(), *baseScene)) {
            if (std::filesystem::exists(tempBinPath)) {
                std::error_code sizeEc;
                auto size = std::filesystem::file_size(tempBinPath, sizeEc);
                if (!sizeEc) {
                    std::cerr << "[PlayMode] Temporary binary size: " << size << " bytes\n";
                }
            }
            abortPlay("[PlayMode] Failed to load temporary binary scene.");
            return;
        }
    } else if (m_PendingPlayOptions.useTempPak) {
        if (entryScenePath.empty()) {
            abortPlay("[PlayMode] Preview pak missing entry scene.");
            return;
        }
        baseScene = std::make_shared<Scene>();
        if (!binary::EntityBinaryLoader::Load(entryScenePath, *baseScene)) {
            std::string err = std::string("[PlayMode] Failed to load entry scene from preview pak: ") + entryScenePath;
            abortPlay(err.c_str());
            return;
        }
    }

    if (baseScene) {
        PrepareSceneForPlayClone(*baseScene, "binary source");
        baseScene->ReleasePhysicsRuntimeState();
        scene.m_RuntimeScene = baseScene->RuntimeClone();
    } else {
        PrepareSceneForPlayClone(scene, "editor source");
        scene.ReleasePhysicsRuntimeState();
        scene.m_RuntimeScene = scene.RuntimeClone();
    }

    if (scene.m_RuntimeScene) {
        scene.m_RuntimeScene->m_IsPlaying = true;
        // CRITICAL: Update global scene pointer so scripts create entities in the runtime scene
        Scene::CurrentScene = scene.m_RuntimeScene.get();
        TogglePlayMode();
    }
    EndBlockingOverlay();
    m_BeginPlayRequested = false;
}

// =============================
// Prefab Editor Query Methods
// =============================
PrefabEditorPanel* UILayer::GetActivePrefabEditor() {
    // First check for focused/hovered prefab editor and make it sticky
    for (auto& pe : m_PrefabEditors) {
        if (pe && pe->IsWindowFocusedOrHovered()) {
            m_StickyPrefabEditor = pe.get();
            return pe.get();
        }
    }
    // Return the sticky prefab editor if one is set
    return m_StickyPrefabEditor;
}

void UILayer::ExitPrefabEditMode() {
    m_StickyPrefabEditor = nullptr;
}

bool UILayer::IsPrefabEditModeActive() const {
    // Check if we have a sticky prefab editor
    if (m_StickyPrefabEditor) return true;
    // Fallback to checking focused/hovered
    for (const auto& pe : m_PrefabEditors) {
        if (pe && pe->IsWindowFocusedOrHovered()) {
            return true;
        }
    }
    return false;
}

bool UILayer::AnyPrefabViewportFocused() const {
    return IsPrefabEditModeActive();
}

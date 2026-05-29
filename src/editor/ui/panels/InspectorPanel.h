#pragma once
#include <imgui.h>
#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include <bgfx/bgfx.h>
#include <nlohmann/json.hpp>
#include "core/ecs/Scene.h"
#include "EditorPanel.h"
// Legacy timeline panel removed
#include "managed/interop/ScriptReflection.h"
#include "core/ecs/ModuleComponent.h"
#include "core/ecs/ComponentRegistry.h"
#include "core/ecs/InstancerComponent.h"
#include "editor/pipeline/ModelImportSettings.h"

class AvatarBuilderPanel; // forward decl
class SplineToolPanel;
class SoftbodyPainter;

// Info about a mesh in a model for the inspector
struct ModelMeshInfo {
    std::string name;
    int materialSlotCount = 1;
    std::vector<std::string> materialSlotNames;
    bool skinned = false;
};

extern std::vector<std::string> g_RegisteredScriptNames;

class InspectorPanel : public EditorPanel {
public:
   InspectorPanel(Scene* scene, EntityID* selectedEntity)
      : m_SelectedEntity(selectedEntity) {
      SetContext(scene);
      }

   ~InspectorPanel();

    void OnImGuiRender();
    // Render inspector UI without opening its own ImGui window
    void OnImGuiRenderEmbedded();
    // External selection hook: when a project asset (e.g., scene file) is selected
   void SetSelectedAssetPath(const std::string& path);
   void SetAvatarBuilderPanel(AvatarBuilderPanel* panel) { m_AvatarBuilder = panel; }
   void SetSplineToolPanel(SplineToolPanel* panel) { m_SplineToolPanel = panel; }
   void SetSoftbodyPainter(SoftbodyPainter* painter) { m_SoftbodyPainter = painter; }
   // Allow switching the selected entity pointer at runtime (to follow active editor scene)
   void SetSelectedEntityPtr(EntityID* ptr) { m_SelectedEntity = ptr; }
   // Animator node selection bridge
   void ShowAnimatorStateProperties(const std::string& stateName,
                                   std::string& clipPath,
                                   float& speed,
                                   bool& loop,
                                   bool isDefault,
                                   std::function<void()> onMakeDefault,
                                   std::vector<std::pair<std::string, int>>* conditionsInt = nullptr,
                                   std::vector<std::tuple<std::string, int, float>>* conditionsFloat = nullptr);

   struct AnimatorStateBinding {
       std::string* Name = nullptr;
       std::string* ClipPath = nullptr;
       std::string* AssetPath = nullptr;
       float* Speed = nullptr;
       bool* Loop = nullptr;
       bool IsDefault = false;
       std::function<void()> MakeDefault;
   };
   void SetAnimatorStateBinding(const AnimatorStateBinding& binding) { m_AnimatorBinding = binding; m_HasAnimatorBinding = true; }
   void ClearAnimatorBinding() { m_HasAnimatorBinding = false; m_AnimatorBinding = {}; }
   void SetAnimatorControllerEditor(std::function<void(const std::string&)> fn) { m_OpenControllerCallback = std::move(fn); }

private:
   void DrawComponents(EntityID entity);
   void DrawAddComponentButton(EntityID entity);
   void DrawScriptComponent(const ScriptInstance& script, int index, EntityID entity);
   void DrawScriptProperty(PropertyInfo& property, void* scriptHandle, const std::string& className, EntityID entityID);
   void DrawIKEditor(EntityID entity, EntityData* data);
   void DrawLookAtEditor(EntityID entity, EntityData* data);
   void DrawInstancerComponent(cm::instancer::InstancerComponent& instancer, EntityID entity);
   void DrawSplineComponent(SplineComponent& spline, EntityID entity);
   void DrawSoftbodyComponent(SoftbodyComponent& softbody, EntityID entity);
    void DrawInspectorContents();
    void DrawGroupingControls(EntityID entity);
    void DrawEntityHeader(EntityID id);
    void DrawSceneFilePreview();
    void DrawAssetSummaryCard(const std::string& extLower);
    void DrawClayObjectInspector();
    void DrawAnimationControllerOverrideInspector();
     void DrawTexturePreviewCard();
     void ReleaseTexturePreview();
     void DrawDynamicComponentUI(EntityID entity, cm::ModuleComponent& comp, const cm::ComponentDesc* desc);
     void DrawModelInspector();
     void DrawModelOverridesSection(EntityID entity, EntityData& data);
     void LoadModelMetadata(const std::string& modelPath);
     bool SaveModelImportSettingsIfDirty();

private:
   EntityID* m_SelectedEntity = nullptr;
   bool m_ShowAddComponentPopup = false;
    bool m_HasAnimatorBinding = false;
   AnimatorStateBinding m_AnimatorBinding;
   AvatarBuilderPanel* m_AvatarBuilder = nullptr;
   SplineToolPanel* m_SplineToolPanel = nullptr;
   SoftbodyPainter* m_SoftbodyPainter = nullptr;
    // Rename state for entity name in inspector
    bool m_RenamingEntityName = false;
    char m_RenameBuffer[128] = {0};
    std::string m_SelectedAssetPath;
    bgfx::TextureHandle m_TexturePreviewHandle = BGFX_INVALID_HANDLE;
    ImVec2 m_TexturePreviewSize = ImVec2(0.0f, 0.0f);
   std::string m_TexturePreviewPath;
   std::filesystem::file_time_type m_TexturePreviewTimestamp{};
   std::function<void(const std::string&)> m_OpenControllerCallback;
   
   // Model inspector state
   std::string m_ModelInspectorPath;
   std::string m_ModelMetaPath;
   std::vector<ModelMeshInfo> m_ModelMeshes;
   ModelImportSettings m_ModelImportSettings;
   bool m_ModelMetaDirty = false;
   
   // ClayObject inspector cache
   std::string m_ClayObjCachePath;
   nlohmann::json m_ClayObjCache;
   bool m_ClayObjModified = false;
   
   // Scene file preview cache (to avoid parsing JSON every frame)
   struct ScenePreviewCache {
       std::string path;
       std::string filename;
       int entityCount = 0;
       std::vector<std::pair<uint32_t, std::string>> entitySummaries; // (id, name)
       std::vector<std::string> referencedAssets;
       bool valid = false;
   };
   ScenePreviewCache m_ScenePreviewCache;
   bool m_UndoInteractionActive = false;
   };

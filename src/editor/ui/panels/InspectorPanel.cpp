#include "InspectorPanel.h"
#include "../utility/ComponentDrawerRegistry.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui_clay_inspector.h>
#include <imgui_claymore_style.h>
#include <imgui_internal.h>
#include <string>
#include <algorithm>
#include <functional>
#include <tuple>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <initializer_list>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <climits>
#include <cfloat>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif
#include "managed/interop/ScriptSystem.h"
#include "managed/interop/ManagedScriptComponent.h"
#include "managed/interop/ScriptReflectionInterop.h"
#include "managed/interop/ScriptReflection.h"
#include "managed/interop/DotNetHost.h"
#include "managed/interop/ResourceInterop.h"
#include "managed/interop/ScriptableInterop.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/NpcScalability.h"
#include "core/ecs/AnimationComponents.h"
#include "core/ecs/ParticleEmitterSystem.h"
#include "core/world/RuntimeWorld.h"
#include "core/deformation/ArmorFitComponent.h"
#include "core/deformation/ArmorWrapLoader.h"
#include "editor/pipeline/ArmorWrapImporter.h"
#include "core/ecs/ComponentUtils.h"
#include "core/rendering/PBRMaterial.h"
#include "core/rendering/MaterialManager.h"
#include "core/rendering/MaterialAsset.h"
#include "editor/nodegraph/ShaderGraphMaterial.h"
#include "core/rendering/TextureLoader.h"
#include "core/rendering/MaterialCache.h"
#include "core/particles/SpriteLoader.h"
#include <bgfx/bgfx.h>
#include "ui/utility/TextureSlotPicker.h"
#include "ui/utility/AnimationAssetListCache.h"
#include "ui/utility/AudioAssetListCache.h"
#include "ui/utility/DialogueLibraryAssetListCache.h"
#include "core/animation/AnimatorController.h"
#include "core/animation/AnimatorControllerOverride.h"
#include "core/animation/AnimationPlayerComponent.h"
#include "core/animation/AnimatorControllerIO.h"
#include "core/animation/AnimatorControllerOverrideIO.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include "editor/ui/panels/AvatarBuilderPanel.h"
// IK
#include "core/animation/ik/IKComponent.h"
// LookAt/Aim constraints
#include "core/animation/lookat/LookAtConstraintComponent.h"
// For project asset directory discovery and animation asset helpers
#include "editor/Project.h"
#include "core/animation/AnimationSerializer.h"
#include "core/vfs/FileSystem.h"
#include <filesystem>
#include "core/ecs/ComponentRegistry.h"
#include "core/assets/ScriptableRegistry.h"
#include "editor/Project.h"
#include "core/prefab/PrefabAPI.h"
#include "core/prefab/PrefabInstanceComponent.h"
#include "editor/prefab/PrefabEditorAPI.h"
#include "core/serialization/Serializer.h"
#include "managed/interop/ComponentInterop.h"
#include "editor/pipeline/AssetLibrary.h"
#include "core/resources/ResourceManifest.h"
#include "core/ecs/InstancerComponent.h"
#include "core/assets/AssetMetadata.h"
#include "core/physics/Physics.h"
#include "core/physics/area/AreaSystem.h"
#include <chrono>
#include <ctime>
#include <sstream>
#include <pipeline/MaterialImporter.h>
#include <pipeline/ShaderImporter.h>
#include "editor/pipeline/ModelImportCache.h"
#include "editor/pipeline/AssetPipeline.h"
#include "editor/tools/SplineToolPanel.h"
#include "editor/tools/SoftbodyPainter.h"
#include "editor/undo/EditorSceneUndoStack.h"
#include "ui/utility/ProjectAssetIndex.h"
#include "core/ecs/SoftbodySystem.h"
#include "core/physics/PhysicsLayerManager.h"
#include <stb_image.h>
#include <imnodes_internal.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

static bool IsTextureExtension(const std::string& extLower) {
   if (extLower.empty()) return false;
   static const std::array<const char*, 6> kTextureExts = { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr" };
   for (const char* ext : kTextureExts) {
      if (extLower == ext) return true;
      }
   return false;
   }

static bool IsAudioExtension(const std::string& extLower) {
   if (extLower.empty()) return false;
   static const std::array<const char*, 4> kAudioExts = { ".wav", ".mp3", ".ogg", ".flac" };
   for (const char* ext : kAudioExts) {
      if (extLower == ext) return true;
      }
   return false;
   }

static std::string GetGuidAssetDisplayName(const std::string& guidString) {
   if (guidString.empty()) return "None";
   ClaymoreGUID guid = ClaymoreGUID::FromString(guidString);
   std::string path = AssetLibrary::Instance().GetPathForGUID(guid);
   if (!path.empty()) {
      std::filesystem::path fsPath(path);
      std::string name = fsPath.stem().string();
      return name.empty() ? fsPath.filename().string() : name;
      }
   if (guidString.size() >= 8) {
      return "(Missing: " + guidString.substr(0, 8) + "...)";
      }
   return "(Missing)";
   }

template <typename OptionT>
static bool RenderGuidAssetCombo(const char* comboId,
                                 const std::vector<OptionT>& options,
                                 std::string& guidString)
{
   bool changed = false;
   const std::string displayName = GetGuidAssetDisplayName(guidString);
   if (!ImGui::BeginCombo(comboId, displayName.c_str())) {
      return false;
      }

   if (ImGui::Selectable("None", guidString.empty())) {
      if (!guidString.empty()) {
         guidString.clear();
         changed = true;
         }
      }

   for (const auto& option : options) {
      if (option.guidString.empty()) {
         continue;
         }

      bool selected = (guidString == option.guidString);
      if (ImGui::Selectable(option.name.c_str(), selected)) {
         if (!selected) {
            guidString = option.guidString;
            changed = true;
            }
         }
      if (selected) ImGui::SetItemDefaultFocus();
      }

   ImGui::EndCombo();
   return changed;
}

static bool TryAssignGuidAssetDrop(std::string& guidString,
                                   const char* droppedPath,
                                   const std::function<bool(const std::string&)>& acceptsExtension)
{
   if (!droppedPath) {
      return false;
      }

   std::string ext = std::filesystem::path(droppedPath).extension().string();
   std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
   if (!acceptsExtension(ext)) {
      return false;
      }

   const ClaymoreGUID newGuid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
   if (newGuid.high == 0 && newGuid.low == 0) {
      return false;
      }

   const std::string newGuidString = newGuid.ToString();
   if (guidString == newGuidString) {
      return false;
      }

   guidString = newGuidString;
   return true;
}

static ImVec2 GetTextureDimensions(const std::string& path) {
   int w = 0, h = 0, c = 0;
   if (stbi_info(path.c_str(), &w, &h, &c) == 1) {
      return ImVec2(static_cast<float>(w), static_cast<float>(h));
      }
   return ImVec2(0.0f, 0.0f);
   }

static ImVec4 BlendVec4(const ImVec4& a, const ImVec4& b, float t) {
   return ImVec4(
      a.x + (b.x - a.x) * t,
      a.y + (b.y - a.y) * t,
      a.z + (b.z - a.z) * t,
      a.w + (b.w - a.w) * t);
}

struct ProjectAssetOption {
   std::string name;
   std::string vfsPath;
   std::string absolutePath;
};

static std::string ToVfsAssetPath(const std::string& path)
   {
   if (path.empty()) return path;
   std::string normalized = path;
   for (char& c : normalized) {
      if (c == '\\') c = '/';
      }
   if (normalized.rfind("assets/", 0) == 0) return normalized;
   size_t pos = normalized.find("/assets/");
   if (pos != std::string::npos) return normalized.substr(pos + 1);
   try {
      std::filesystem::path base = Project::GetProjectDirectory();
      std::filesystem::path pp(normalized);
      if (!base.empty() && pp.is_absolute()) {
         std::error_code ec;
         auto rel = std::filesystem::relative(pp, base, ec);
         if (!ec) {
            std::string relStr = rel.string();
            for (char& c : relStr) if (c == '\\') c = '/';
            if (relStr.find("../") == std::string::npos) return relStr;
            }
         }
      }
   catch (...) {}
   return normalized;
   }

static std::vector<ProjectAssetOption> CollectProjectAssetOptions(const std::initializer_list<const char*>& extensions)
   {
   std::vector<ProjectAssetOption> options;
   ui::ProjectAssetQuery query;
   query.extensions.reserve(extensions.size());
   for (const char* expected : extensions) {
      if (expected && *expected) {
         query.extensions.emplace_back(expected);
      }
   }

   const auto& assets = ui::GetProjectAssetEntries(query);
   options.reserve(assets.size());
   for (const ui::ProjectAssetEntry& asset : assets) {
      options.push_back({ asset.name, asset.projectRelativePath, asset.absolutePath });
   }
   return options;
   }

// Convert raw field names (camelCase, PascalCase, snake_case) to spaced, capitalized labels
static std::string PrettifyLabel(const std::string& raw)
   {
   if (raw.empty()) return raw;

   std::string spaced;
   spaced.reserve(raw.size() * 2);

   auto isUpper = [](char c) { return c >= 'A' && c <= 'Z'; };
   auto isLower = [](char c) { return c >= 'a' && c <= 'z'; };
   auto isDigit = [](char c) { return c >= '0' && c <= '9'; };

   for (size_t i = 0; i < raw.size(); ++i)
      {
      char c = raw[i];
      if (c == '_' || c == '-' || c == ' ')
         {
         if (!spaced.empty() && spaced.back() != ' ') spaced.push_back(' ');
         continue;
         }

      if (!spaced.empty())
         {
         char prev = spaced.back();
         char next = (i + 1 < raw.size()) ? raw[i + 1] : '\0';
         bool prevIsLetterOrDigit = ((prev >= 'A' && prev <= 'Z') || (prev >= 'a' && prev <= 'z') || (prev >= '0' && prev <= '9'));

         // Insert space on transitions like: aA, 0A, a0, A0, and between acronym boundary A a
         if (prevIsLetterOrDigit)
            {
            bool insert = false;
            if (isLower(prev) && isUpper(c)) insert = true;               // camelCase boundary
            else if (isDigit(prev) && !isDigit(c)) insert = true;         // digit -> letter
            else if (!isDigit(prev) && isDigit(c)) insert = true;         // letter -> digit
            else if (isUpper(prev) && isUpper(c) && isLower(next)) insert = true; // XMLh -> XML h
            if (insert && spaced.back() != ' ') spaced.push_back(' ');
            }
         }

      spaced.push_back(c);
      }

   // Title case
   std::string out; out.reserve(spaced.size());
   bool newWord = true;
   for (char c : spaced)
      {
      if (c == ' ')
         {
         if (!out.empty() && out.back() != ' ') out.push_back(' ');
         newWord = true;
         }
      else
         {
         if (newWord)
            {
            // Uppercase first letter
            if (c >= 'a' && c <= 'z') out.push_back(char(c - 'a' + 'A'));
            else out.push_back(c);
            newWord = false;
            }
         else
            {
            // Lowercase subsequent letters
            if (c >= 'A' && c <= 'Z') out.push_back(char(c - 'A' + 'a'));
            else out.push_back(c);
            }
         }
      }

   // Trim trailing space
   while (!out.empty() && out.back() == ' ') out.pop_back();
   return out;
   }

static PropertyValue GetDefaultPropertyValue(PropertyType type)
   {
   switch (type)
      {
      case PropertyType::Int:
      case PropertyType::Entity:
      case PropertyType::ComponentRef:
      case PropertyType::ScriptRef:
      case PropertyType::Enum:
         return 0;
      case PropertyType::Float:
         return 0.0f;
      case PropertyType::Bool:
         return false;
      case PropertyType::String:
      case PropertyType::Prefab:
      case PropertyType::ClayObject:
      case PropertyType::Mesh:
      case PropertyType::DialogueLibrary:
      case PropertyType::AnimatorController:
      case PropertyType::AnimatorControllerOverride:
      case PropertyType::Texture:
      case PropertyType::Audio:
         return std::string();
      case PropertyType::Vector3:
         return glm::vec3(0.0f);
      case PropertyType::List:
         return std::make_shared<ListPropertyValue>();
      case PropertyType::Struct:
         return std::make_shared<StructPropertyValue>();
      case PropertyType::Dictionary:
         return std::make_shared<DictionaryPropertyValue>();
      }

   return 0;
   }

static PropertyValue CoercePropertyValueToType(const PropertyValue& value, PropertyType type)
   {
   auto alreadyMatches = [&](auto* dummy) -> bool
      {
      using T = std::decay_t<decltype(*dummy)>;
      return std::holds_alternative<T>(value);
      };

   switch (type)
      {
      case PropertyType::Int:
      case PropertyType::Entity:
      case PropertyType::ComponentRef:
      case PropertyType::ScriptRef:
      case PropertyType::Enum:
         if (alreadyMatches((int*)nullptr)) return value;
         break;
      case PropertyType::Float:
         if (alreadyMatches((float*)nullptr)) return value;
         break;
      case PropertyType::Bool:
         if (alreadyMatches((bool*)nullptr)) return value;
         break;
      case PropertyType::String:
      case PropertyType::Prefab:
      case PropertyType::ClayObject:
      case PropertyType::Mesh:
      case PropertyType::DialogueLibrary:
      case PropertyType::AnimatorController:
      case PropertyType::AnimatorControllerOverride:
      case PropertyType::Texture:
      case PropertyType::Audio:
         if (alreadyMatches((std::string*)nullptr)) return value;
         break;
      case PropertyType::Vector3:
         if (alreadyMatches((glm::vec3*)nullptr)) return value;
         break;
      case PropertyType::List:
         if (alreadyMatches((std::shared_ptr<ListPropertyValue>*)nullptr)) return value;
         break;
      case PropertyType::Struct:
         if (alreadyMatches((std::shared_ptr<StructPropertyValue>*)nullptr)) return value;
         break;
      case PropertyType::Dictionary:
         if (alreadyMatches((std::shared_ptr<DictionaryPropertyValue>*)nullptr)) return value;
         break;
      }

   if (std::holds_alternative<std::string>(value))
      {
      return ScriptReflection::StringToPropertyValue(std::get<std::string>(value), type);
      }

   return GetDefaultPropertyValue(type);
   }

static void NormalizeListPropertyElements(PropertyInfo& property)
   {
   if (property.type != PropertyType::List ||
       !std::holds_alternative<std::shared_ptr<ListPropertyValue>>(property.currentValue))
      {
      return;
      }

   auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(property.currentValue);
   if (!listPtr)
      {
      listPtr = std::make_shared<ListPropertyValue>();
      property.currentValue = listPtr;
      }

   listPtr->elementType = property.listElementType;
   if (listPtr->elementTypeName.empty())
      {
      listPtr->elementTypeName = property.listElementTypeName;
      }

   for (PropertyValue& element : listPtr->elements)
      {
      element = CoercePropertyValueToType(element, property.listElementType);
      }
   }

// Recursively propagate a layer value to all descendants of an entity
static void PropagateLayerToChildren(Scene* scene, EntityID root, int newLayer)
   {
   auto* data = scene ? scene->GetEntityData(root) : nullptr;
   if (!data) return;
   for (EntityID child : data->Children)
      {
      if (auto* cd = scene->GetEntityData(child))
         {
         cd->Layer = newLayer;
         PropagateLayerToChildren(scene, child, newLayer);
         }
      }
   }

static std::string GetEntityDisplayName(Scene* scene, EntityID id)
   {
   if (!scene || id == INVALID_ENTITY_ID || id == 0) return "<None>";
   if (auto* data = scene->GetEntityData(id))
      {
      if (!data->Name.empty()) return data->Name;
      }
   return std::string("Entity ") + std::to_string(id);
   }

static EntityData* ResolveIKOwner(Scene* scene, EntityID selectedId, EntityData* selectedData, EntityID& outOwnerId)
   {
   outOwnerId = selectedId;
   if (!scene || !selectedData) return nullptr;
   if (selectedData->Skeleton)
      return selectedData;

   if (selectedData->Skinning && selectedData->Skinning->SkeletonRoot != (EntityID)-1)
      {
      EntityID root = selectedData->Skinning->SkeletonRoot;
      if (auto* owner = scene->GetEntityData(root))
         {
         outOwnerId = root;
         return owner;
         }
      }

   EntityID current = selectedData->Parent;
   size_t guard = 0;
   while (current != INVALID_ENTITY_ID && guard++ < 1024)
      {
      auto* parent = scene->GetEntityData(current);
      if (!parent) break;
      if (parent->Skeleton)
         {
         outOwnerId = current;
         return parent;
         }
      current = parent->Parent;
      }
   return nullptr;
   }

using BoneId = cm::animation::ik::BoneId;

struct BoneUIOption
   {
   BoneId id = -1;
   std::string label;
   };

static void BuildBoneNameTable(const SkeletonComponent& skeleton, std::vector<std::string>& outNames)
   {
   const size_t boneCount = skeleton.BoneParents.empty() ? skeleton.BoneEntities.size() : skeleton.BoneParents.size();
   outNames.assign(boneCount, std::string());
   if (!skeleton.BoneNames.empty())
      {
      const size_t copyCount = std::min(boneCount, skeleton.BoneNames.size());
      for (size_t i = 0; i < copyCount; ++i) outNames[i] = skeleton.BoneNames[i];
      }
   else
      {
      for (const auto& kv : skeleton.BoneNameToIndex)
         {
         if (kv.second >= 0 && (size_t)kv.second < outNames.size())
            outNames[kv.second] = kv.first;
         }
      }
   for (size_t i = 0; i < outNames.size(); ++i)
      {
      if (outNames[i].empty())
         outNames[i] = std::string("Bone ") + std::to_string(i);
      }
   }

static void BuildBoneOptions(const SkeletonComponent& skeleton, const std::vector<std::string>& names, std::vector<BoneUIOption>& out)
   {
   const int count = (int)names.size();
   out.clear();
   if (count <= 0) return;

   std::vector<std::vector<int>> children(count);
   std::vector<int> roots;
   for (int i = 0; i < count; ++i)
      {
      int parent = (i < (int)skeleton.BoneParents.size()) ? skeleton.BoneParents[i] : -1;
      if (parent >= 0 && parent < count)
         children[parent].push_back(i);
      else
         roots.push_back(i);
      }

   auto sortByName = [&](std::vector<int>& indices)
      {
      std::sort(indices.begin(), indices.end(), [&](int a, int b) { return names[a] < names[b]; });
      };
   sortByName(roots);
   for (auto& list : children) sortByName(list);

   std::function<void(int,int)> dfs = [&](int bone, int depth)
      {
      BoneUIOption opt;
      opt.id = bone;
      opt.label.assign(depth * 2, ' ');
      opt.label += names[bone];
      out.push_back(std::move(opt));
      for (int child : children[bone])
         dfs(child, depth + 1);
      };

   for (int r : roots)
      dfs(r, 0);
   }

static bool BuildChainFromEndpoints(const SkeletonComponent& skeleton, BoneId start, BoneId end, std::vector<BoneId>& outChain)
   {
   outChain.clear();
   if (start < 0 || end < 0) return false;
   if (start == end) return false;
   const int limit = (int)skeleton.BoneParents.size();
   std::vector<BoneId> path;
   BoneId cur = end;
   size_t guard = 0;
   while (cur >= 0 && cur < limit && guard++ < 1024)
      {
      path.push_back(cur);
      if (cur == start) break;
      int parent = skeleton.BoneParents[cur];
      if (parent == cur) break;
      cur = parent;
      }
   if (path.empty() || path.back() != start) return false;
   std::reverse(path.begin(), path.end());
   if (path.size() < 2) return false;
   if (path.size() > cm::animation::ik::kMaxChainLen)
      path.resize(cm::animation::ik::kMaxChainLen);
   outChain = path;
   return true;
   }

static bool DrawEntityReferenceField(const char* label, EntityID& value, Scene* scene)
   {
   bool changed = false;
   ImGui::TextUnformatted(label);
   ImGui::SameLine();
   ImGui::PushID(label);
   std::string buttonLabel = GetEntityDisplayName(scene, value);
   if (ImGui::Button(buttonLabel.c_str(), ImVec2(200.0f, 0.0f)))
      {
      // placeholder for future entity picker
      }
   if (ImGui::BeginDragDropTarget())
      {
      if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_ID"))
         {
         EntityID dropped = *(EntityID*)payload->Data;
         if (dropped != value)
            {
            value = dropped;
            changed = true;
            }
         }
      ImGui::EndDragDropTarget();
      }
   if (value != INVALID_ENTITY_ID && value != 0)
      {
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear"))
         {
         value = 0;
         changed = true;
         }
      }
   ImGui::PopID();
   return changed;
   }

// Draw a right-aligned three-dots menu button on the last item (typically a header)
// and invoke the provided builder inside a popup.
struct ComponentSectionHandle
   {
   ImGui::ClayHeaderBarResult bar;
   };

class ScopedComponentContent
   {
   public:
      explicit ScopedComponentContent(float indent = 12.0f, float topPadding = 6.0f)
         : m_Indent(indent)
         {
         ImGui::Dummy(ImVec2(0.0f, topPadding));
         ImGui::Indent(m_Indent);
         ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 6.0f));
         }

      ~ScopedComponentContent()
         {
         ImGui::PopStyleVar();
         ImGui::Unindent(m_Indent);
         }

   private:
      float m_Indent = 12.0f;
   };

static ComponentSectionHandle DrawComponentSection(const char* label,
   bool defaultOpen,
   const std::function<void()>& buildMenu = nullptr,
   bool* enabled = nullptr)
   {
   ComponentSectionHandle handle;
   ImGui::ClayHeaderBarConfig cfg;
   cfg.DefaultOpen = defaultOpen;
   cfg.Enabled = enabled;
   cfg.ShowOptionsButton = (bool)buildMenu;
   handle.bar = ImGui::ClayHeaderBar(label, cfg);

   if (buildMenu)
      {
      std::string popupId = std::string(label) + "##component_menu";
      if (handle.bar.optionsClicked || handle.bar.contextRequested)
         ImGui::OpenPopup(popupId.c_str());
      if (ImGui::BeginPopup(popupId.c_str()))
         {
         buildMenu();
         ImGui::EndPopup();
         }
      }

   return handle;
   }

// Top entity header with name + Tag/Layer dropdowns from Project settings
void InspectorPanel::DrawEntityHeader(EntityID id)
   {
   auto* data = m_Context->GetEntityData(id);
   if (!data) return;

   ImGui::PushID((int)id);
   ImGui::TextDisabled("GameObject");
   ImGui::Dummy(ImVec2(0.0f, 1.0f));
   ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
   char nameBuf[256];
   strncpy(nameBuf, data->Name.c_str(), sizeof(nameBuf));
   nameBuf[sizeof(nameBuf) - 1] = 0;
   if (ImGui::InputText("##entity_name", nameBuf, IM_ARRAYSIZE(nameBuf), ImGuiInputTextFlags_AutoSelectAll))
      {
      std::string desired = nameBuf;
      if (desired.empty()) desired = "Entity";
      int suffix = 1;
      std::string finalName = desired;
      bool unique = false;
      while (!unique)
         {
         unique = true;
         for (const auto& e : m_Context->GetEntities())
            {
            if (e.GetID() == id) continue;
            auto* ed = m_Context->GetEntityData(e.GetID());
            if (ed && ed->Name == finalName) { unique = false; break; }
            }
         if (!unique) finalName = desired + "_" + std::to_string(suffix++);
         }
      data->Name = finalName;
      }

   ImGui::Dummy(ImVec2(0.0f, 3.0f));

   const auto& tags = Project::GetTags();
   const auto& layers = Project::GetLayerNames();
   static bool s_RequestAddLayerModal = false;
   {
   ImGui::ClayInspectorContentScope metaScope("EntityMeta");

   std::string currentTag = data->Tag.empty() ? (tags.empty() ? std::string("Untagged") : tags[0]) : data->Tag;
   ImGui::ClayFieldDropdown(metaScope, "Tag", currentTag.c_str(), [&]() -> bool {
      bool tagChanged = false;
      if (tags.empty())
         {
         if (ImGui::Selectable("Untagged", true))
            tagChanged = false;
         }
      else
         {
         for (const auto& t : tags)
            {
            bool sel = (t == currentTag);
            if (ImGui::Selectable(t.c_str(), sel))
               {
               data->Tag = t;
               tagChanged = true;
               }
            }
         }
      return tagChanged;
      });

   int currentLayerIndex = data->Layer;
   std::string currentLayerLabel = (currentLayerIndex >= 0 && (size_t)currentLayerIndex < layers.size())
      ? layers[currentLayerIndex]
      : std::string("Layer ") + std::to_string(currentLayerIndex);

      ImGui::ClayFieldDropdown(metaScope, "Layer", currentLayerLabel.c_str(), [&]() -> bool {
         bool changed = false;
         for (int i = 0; i < (int)layers.size(); ++i)
            {
            bool sel = (i == currentLayerIndex);
            if (ImGui::Selectable(layers[i].c_str(), sel))
               {
               int old = data->Layer;
               data->Layer = i;
               if (m_Context && i != old)
                  {
                  PropagateLayerToChildren(m_Context, id, i);
                  m_Context->MarkDirty();
                  }
               changed = true;
               }
            }
         ImGui::Separator();
         if (ImGui::Selectable("Add Layer..."))
            {
            s_RequestAddLayerModal = true;
            ImGui::CloseCurrentPopup();
            }
         return changed;
         });

      if (s_RequestAddLayerModal)
         {
         ImGui::OpenPopup("AddLayerPopup");
         s_RequestAddLayerModal = false;
         }
   }

   if (ImGui::BeginPopupModal("AddLayerPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
      {
      static char s_newLayer[64] = { 0 };
      ImGui::InputText("Name", s_newLayer, IM_ARRAYSIZE(s_newLayer));
      if (ImGui::Button("Create"))
         {
         std::vector<std::string> newLayers = layers;
         newLayers.push_back(std::string(s_newLayer));
         Project::SetLayerNames(std::move(newLayers));
         Project::Save();
         s_newLayer[0] = 0;
         ImGui::CloseCurrentPopup();
         }
      ImGui::SameLine();
      if (ImGui::Button("Cancel"))
         {
         ImGui::CloseCurrentPopup();
         }
      ImGui::EndPopup();
      }

   ImGui::Dummy(ImVec2(0.0f, 3.0f));
   ImGui::Separator();
   ImGui::Dummy(ImVec2(0.0f, 2.0f));

   ImGui::PopID();
   }

bool DrawVec3Control(const char* label, glm::vec3& values, float resetValue = 0.0f) {
   bool changed = false;
   const ClayEditorTheme& theme = Clay_GetEditorTheme();
   ImGui::PushID(label);
   ImGui::Columns(2);
   ImGui::SetColumnWidth(0, 80.0f);
   ImGui::Text(label);
   ImGui::NextColumn();

   ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ 4, 0 });

   float lineHeight = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
   ImVec2 buttonSize = { lineHeight + 3.0f, lineHeight };
   float columnWidth = (ImGui::GetContentRegionAvail().x - 3 * buttonSize.x) / 3.0f;

   auto DrawAxis = [&](const char* axisLabel, float& v, const ImVec4& color) {
      const ImVec4 button = BlendVec4(color, theme.SurfaceRaised, 0.20f);
      const ImVec4 buttonHovered = BlendVec4(button, theme.SelectionAccent, 0.14f);
      const ImVec4 buttonActive = BlendVec4(button, theme.Text, 0.10f);
      ImGui::PushStyleColor(ImGuiCol_Button, button);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, buttonHovered);
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, buttonActive);
      if (ImGui::Button(axisLabel, buttonSize)) {
         v = resetValue;
         changed = true;
         }
      ImGui::PopStyleColor(3);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(columnWidth);
      changed |= ImGui::DragFloat(("##" + std::string(axisLabel)).c_str(), &v, 0.1f);
      ImGui::SameLine();
      };

   DrawAxis("X", values.x, theme.PropertyAxisX);
   DrawAxis("Y", values.y, theme.PropertyAxisY);
   DrawAxis("Z", values.z, theme.PropertyAxisZ);

   ImGui::PopStyleVar();
   ImGui::Columns(1);
   ImGui::PopID();
   return changed;
   }

void InspectorPanel::OnImGuiRender() {
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
   if (ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_None)) {
      DrawInspectorContents();
      const bool inspectorFocused =
         ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
         ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
      const bool interactionActive = ImGui::IsAnyItemActive();
      if (!m_UndoInteractionActive && m_Context && inspectorFocused && interactionActive) {
         EditorSceneUndoStack::Get().BeginScopedAction(m_Context, "Inspector Change");
         m_UndoInteractionActive = EditorSceneUndoStack::Get().IsBoundTo(m_Context);
      } else if (m_UndoInteractionActive && (!interactionActive || !m_Context)) {
         EditorSceneUndoStack::Get().EndScopedAction(m_Context);
         m_UndoInteractionActive = false;
      }
   } else if (m_UndoInteractionActive) {
      EditorSceneUndoStack::Get().EndScopedAction(m_Context);
      m_UndoInteractionActive = false;
   }
   ImGui::End();
   ImGui::PopStyleVar();
   }

void InspectorPanel::OnImGuiRenderEmbedded() {
   DrawInspectorContents();
   }

InspectorPanel::~InspectorPanel() {
   ReleaseTexturePreview();
   texturepicker::ClearCachedThumbnails();
   }

void InspectorPanel::SetSelectedAssetPath(const std::string& path) {
   if (path == m_SelectedAssetPath) return;
   m_SelectedAssetPath = path;
   if (path.empty() || path != m_TexturePreviewPath) {
      ReleaseTexturePreview();
      }
   }

void InspectorPanel::DrawInspectorContents() {
   // Timeline key inspection removed; the new animation panel owns track/key inspection UI

   // Prefer entity selection if available; otherwise, show animator binding when set
   // Background drawing temporarily disabled to test if it's causing scroll reset issues

   ImGuiWindow* window = ImGui::GetCurrentWindow();
   struct ContentSpanRecorder {
      ImGuiWindow* window = nullptr;
      float startCursorY = 0.0f;
      explicit ContentSpanRecorder(ImGuiWindow* w)
         : window(w)
         , startCursorY(w ? w->DC.CursorPos.y : 0.0f) {}
      ~ContentSpanRecorder() {
         if (!window) return;
         const float endCursorY = window->DC.CursorPos.y;
         if (endCursorY <= startCursorY)
            return;
         const float measured = endCursorY - window->DC.CursorStartPos.y;
         window->DC.CursorMaxPos.y = ImMax(window->DC.CursorMaxPos.y, endCursorY);
         window->DC.IdealMaxPos.y = ImMax(window->DC.IdealMaxPos.y, endCursorY);
         window->ContentSize.y = ImMax(window->ContentSize.y, measured);
         window->ScrollMax.y = ImMax(0.0f, window->ContentSize.y - window->Size.y);
         };
      } contentRecorder(window);

   bool hasEntitySelection = (m_SelectedEntity && *m_SelectedEntity != -1 && m_Context);
   bool hasAssetSelection = !m_SelectedAssetPath.empty();

   if (!hasAssetSelection) {
      ReleaseTexturePreview();
      }
   if (!hasEntitySelection
      && m_HasAnimatorBinding && m_AnimatorBinding.Name && !hasAssetSelection) {
      std::string dummyName = *m_AnimatorBinding.Name;
      ShowAnimatorStateProperties(dummyName,
         *m_AnimatorBinding.ClipPath,
         *m_AnimatorBinding.Speed,
         *m_AnimatorBinding.Loop,
         m_AnimatorBinding.IsDefault,
         m_AnimatorBinding.MakeDefault);
      return;
      }

   std::string assetExtLower;
   if (hasAssetSelection) {
      assetExtLower = std::filesystem::path(m_SelectedAssetPath).extension().string();
      std::transform(assetExtLower.begin(), assetExtLower.end(), assetExtLower.begin(), ::tolower);
      DrawAssetSummaryCard(assetExtLower);
      if (IsTextureExtension(assetExtLower)) {
         DrawTexturePreviewCard();
         return;
         }
      else {
         ReleaseTexturePreview();
         }
      if (assetExtLower == ".scene") { DrawSceneFilePreview(); return; }
      if (assetExtLower == ".fbx" || assetExtLower == ".gltf" || assetExtLower == ".glb" || assetExtLower == ".obj") {
         DrawModelInspector();
         return;
         }
      }

   if (hasAssetSelection && assetExtLower == ".mat") {
      namespace fs = std::filesystem;
      ImGui::Separator();
      ImGui::Text("Material: %s", std::filesystem::path(m_SelectedAssetPath).filename().string().c_str());
      MaterialAssetUnified mat{};
      bool ok = MaterialImporter::Load(m_SelectedAssetPath, mat);
      if (!ok) {
         ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Failed to load material JSON");
         return;
         }
      // Shader path field with drag-drop of .shader
      char shaderBuf[512];
      strncpy(shaderBuf, mat.shaderPath.c_str(), sizeof(shaderBuf)); shaderBuf[sizeof(shaderBuf) - 1] = 0;
      ImGui::InputText("Shader", shaderBuf, sizeof(shaderBuf));
      if (ImGui::BeginDragDropTarget()) {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
            const char* p = (const char*)payload->Data;
            if (p) {
               std::string e = fs::path(p).extension().string(); std::transform(e.begin(), e.end(), e.begin(), ::tolower);
               if (e == ".shader") {
                  strncpy(shaderBuf, p, sizeof(shaderBuf)); shaderBuf[sizeof(shaderBuf) - 1] = 0;
                  }
               }
            }
         ImGui::EndDragDropTarget();
         }
      mat.shaderPath = shaderBuf;
      // If shader set, extract meta to drive UI
      cm::ShaderMeta meta; std::string perr;
      if (!mat.shaderPath.empty()) {
         cm::ShaderImporter::ExtractMetaFromSource(mat.shaderPath, meta, perr);
         }
      // Params UI
      if (!meta.params.empty()) {
         ImGui::Separator();
         ImGui::TextDisabled("Parameters");
         for (const auto& p : meta.params) {
            glm::vec4 v = glm::vec4(0.0f);
            auto it = mat.params.find(p.name);
            if (it != mat.params.end()) v = it->second;
            if (p.uiHint.find("Color") != std::string::npos) {
               if (ImGui::ColorEdit4(p.name.c_str(), &v.x)) mat.params[p.name] = v;
               }
            else {
               // scalar vs vec4 heuristic
               if (p.type == "float") {
                  float f = v.x; if (ImGui::DragFloat(p.name.c_str(), &f, 0.01f)) { v.x = f; mat.params[p.name] = v; }
                  }
               else {
                  if (ImGui::DragFloat4(p.name.c_str(), &v.x, 0.01f)) mat.params[p.name] = v;
                  }
               }
            }
         }
      // Textures UI
      if (!meta.samplers.empty()) {
         ImGui::Separator();
         ImGui::TextDisabled("Textures");
         for (const auto& s : meta.samplers) {
            std::string key = !s.tag.empty() ? s.tag : s.name;
            std::string& path = mat.textures[key]; // ensures key exists
            ImGui::Text("%s", key.c_str()); ImGui::SameLine();
            char tbuf[512]; strncpy(tbuf, path.c_str(), sizeof(tbuf)); tbuf[sizeof(tbuf) - 1] = 0;
            ImGui::InputText((std::string("##tex_") + key).c_str(), tbuf, sizeof(tbuf));
            if (ImGui::BeginDragDropTarget()) {
               if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                  const char* p = (const char*)payload->Data; if (p) { std::string ext2 = fs::path(p).extension().string(); std::transform(ext2.begin(), ext2.end(), ext2.begin(), ::tolower); if (ext2 == ".png" || ext2 == ".jpg" || ext2 == ".jpeg" || ext2 == ".tga" || ext2 == ".bmp" || ext2 == ".hdr") { strncpy(tbuf, p, sizeof(tbuf)); tbuf[sizeof(tbuf) - 1] = 0; } }
                  }
               ImGui::EndDragDropTarget();
               }
            path = tbuf;
            }
         }
      if (ImGui::Button("Save Material")) {
         MaterialImporter::Save(m_SelectedAssetPath, mat);
         std::cout << "[Material] Saved: " << m_SelectedAssetPath << std::endl;
         }
      return;
      }

   // ClayObject (.clayobj) inspector
   if (assetExtLower == ".clayobj") {
      DrawClayObjectInspector();
      return;
      }

   if (assetExtLower == ".animoverride") {
      DrawAnimationControllerOverrideInspector();
      return;
      }

   if (hasAssetSelection) {
      ReleaseTexturePreview();
      return;
      }

   if (hasEntitySelection) {
      // Top entity header (name + Tag/Layer) - always at top, not scrollable
      DrawEntityHeader(*m_SelectedEntity);

      // Draw components directly in the main window - let the main window handle scrolling
      // This avoids the child window content size calculation issues that cause scroll resets

      // Groups (kept separate)
      DrawGroupingControls(*m_SelectedEntity);

      // Prefab Overrides panel (placed under Groups)
      {
      EntityID cur = *m_SelectedEntity;
      EntityID prefabRoot = -1;
      std::string prefabVPath;
      while (cur != -1) {
         auto* d = m_Context->GetEntityData(cur); if (!d) break;
         if (!d->PrefabSource.empty()) { prefabRoot = cur; prefabVPath = d->PrefabSource; break; }
         cur = d->Parent;
         }
      if (prefabRoot != -1 && !prefabVPath.empty()) {
         try {
            std::filesystem::path full = Project::GetProjectDirectory() / prefabVPath;
            std::string ext = full.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".prefab") {
               // Cache and gate expensive override computation behind the foldout
               static EntityID s_cachedRoot = (EntityID)-2;
               static std::vector<prefab::PropertyOverride> s_cachedOverrides;
               static bool s_haveCache = false;
               static bool s_autoRefresh = false;

               const bool rootChanged = (s_cachedRoot != prefabRoot);
               if (rootChanged) { s_cachedRoot = prefabRoot; s_haveCache = false; }

               auto prefabSection = DrawComponentSection("Prefab Overrides", false);
               if (prefabSection.bar.open) {
                  ScopedComponentContent contentScope;
                  bool doRefresh = false;
                  if (ImGui::SmallButton("Refresh")) doRefresh = true;
                  ImGui::SameLine();
                  ImGui::Checkbox("Auto-refresh (expensive)", &s_autoRefresh);

                  if (doRefresh || rootChanged || (s_autoRefresh && m_Context)) {
                     PrefabAsset base; bool baseLoaded = PrefabIO::LoadPrefab(full.string(), base);
                     if (baseLoaded) {
                        s_cachedOverrides = prefab_editor::ComputeOverrides(base, *m_Context, prefabRoot);
                        s_haveCache = true;
                        }
                     else {
                        s_cachedOverrides.clear();
                        s_haveCache = true;
                        }
                     }

                  const auto& ov = s_cachedOverrides;
                  if (!s_haveCache) {
                     ImGui::TextDisabled("(no data)");
                     }
                  else if (ov.empty()) {
                     ImGui::Text("No overrides");
                     }
                  else {
                     ImGui::Separator();
                     auto revertAll = [&]() {
                        auto* rd = m_Context->GetEntityData(prefabRoot); if (!rd) { return; }
                        TransformComponent savedXf = rd->Transform; EntityID parent = rd->Parent; std::string name = rd->Name;
                        m_Context->RemoveEntity(prefabRoot);
                        EntityID nid = InstantiatePrefabFromPath(full.string(), *m_Context);
                        if (nid != (EntityID)-1 && nid != (EntityID)0) {
                           if (auto* nd = m_Context->GetEntityData(nid)) { nd->Name = name; nd->Transform = savedXf; nd->Transform.TransformDirty = true; }
                           if (parent != (EntityID)-1) m_Context->SetParent(nid, parent);
                           m_Context->MarkTransformDirty(nid); m_Context->UpdateTransforms();
                           if (m_SelectedEntity) *m_SelectedEntity = nid;
                           }
                        s_haveCache = false; // invalidate cache after structural change
                        };
                     // Note: Individual override revert not yet implemented with new system
                     auto reapplySans = [&](int skipIndex) {
                        // TODO: Implement individual override revert with new PropertyOverride system
                        revertAll();
                        };

                     // Apply/Revert buttons (Unity-style)
                     if (ImGui::Button("Apply All")) {
                        // Apply changes back to the prefab file
                        PrefabAsset updatedAsset;
                        if (prefab_editor::BuildPrefabAssetFromScene(*m_Context, prefabRoot, updatedAsset)) {
                           if (PrefabIO::SavePrefab(full.string(), updatedAsset)) {
                              std::cout << "[Inspector] Applied changes to prefab: " << full.string() << std::endl;
                              s_haveCache = false; // Invalidate cache
                           } else {
                              std::cerr << "[Inspector] Failed to save prefab: " << full.string() << std::endl;
                           }
                        }
                     }
                     ImGui::SameLine();
                     if (ImGui::Button("Revert All")) { revertAll(); }
                     ImGui::Separator();
                     ImGui::TextDisabled("Override Count: %d", (int)ov.size());
                     ImGui::Separator();
                     for (size_t i = 0; i < ov.size(); ++i) {
                        const auto& override = ov[i];
                        ImVec4 col = ImVec4(0.95f, 0.85f, 0.45f, 1.0f); // Yellow for modifications
                        ImGui::PushStyleColor(ImGuiCol_Text, col);
                        ImGui::BulletText("[%s] %s", override.ComponentKey.c_str(), 
                                         override.TargetEntityGuid.ToString().c_str());
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        std::string btn = std::string("Revert##ov") + std::to_string(i);
                        if (ImGui::SmallButton(btn.c_str())) { reapplySans((int)i); }
                        }
                     }
                  }
               }
            }

         catch (...) {
            // Swallow errors when prefab metadata is unavailable/broken
            }
         }
      }

      DrawComponents(*m_SelectedEntity);

      // Offer to add an Animator if a skeleton exists but no AnimationPlayer is attached
      auto* data = m_Context->GetEntityData(*m_SelectedEntity);
      if (data && data->Skeleton && !data->AnimationPlayer) {
         ImGui::Separator();
         if (ImGui::Button("Add Animator to Entity")) {
            data->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>();
            m_Context->NotifyComponentChanged(*m_SelectedEntity, cm::world::RuntimeDirtyBits::Metadata);
            }
         }
      
      // Model Overrides section for model roots
      if (data && (data->ModelAssetGuid.high != 0 || data->ModelAssetGuid.low != 0)) {
         DrawModelOverridesSection(*m_SelectedEntity, *data);
      }

      }
   else {
      ImGui::Text("No entity selected.");
      }
   }

void InspectorPanel::DrawSceneFilePreview() {
   using json = nlohmann::json;
   std::string ext = std::filesystem::path(m_SelectedAssetPath).extension().string();
   std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
   if (ext != ".scene") return;

   // Only parse the scene file when selection changes (cache the results)
   if (m_ScenePreviewCache.path != m_SelectedAssetPath) {
      m_ScenePreviewCache = ScenePreviewCache{}; // Reset cache
      m_ScenePreviewCache.path = m_SelectedAssetPath;
      m_ScenePreviewCache.filename = std::filesystem::path(m_SelectedAssetPath).filename().string();
      
      try {
         std::ifstream in(m_SelectedAssetPath);
         if (in.is_open()) {
            json j; in >> j; in.close();
            if (j.contains("entities") && j["entities"].is_array()) {
               const auto& ents = j["entities"];
               m_ScenePreviewCache.entityCount = (int)ents.size();
               // Cache first N entity summaries
               int count = 0;
               for (const auto& e : ents) {
                  if (count++ > 25) break;
                  std::string name = e.value("name", std::string("<unnamed>"));
                  uint32_t id = e.value("id", 0u);
                  m_ScenePreviewCache.entitySummaries.emplace_back(id, name);
               }
               // Scan for asset-looking strings
               std::function<void(const json&)> walk = [&](const json& n) {
                  if (n.is_string()) {
                     std::string s = n.get<std::string>();
                     std::string lower = s; for (char& c : lower) c = (char)tolower(c);
                     if (lower.find("assets/") != std::string::npos || lower.find(".fbx") != std::string::npos || lower.find(".gltf") != std::string::npos || lower.find(".png") != std::string::npos)
                        m_ScenePreviewCache.referencedAssets.push_back(s);
                  }
                  else if (n.is_array()) for (const auto& e2 : n) walk(e2); 
                  else if (n.is_object()) for (auto it = n.begin(); it != n.end(); ++it) walk(it.value());
               };
               walk(j);
            }
            m_ScenePreviewCache.valid = true;
         }
      }
      catch (...) {}
   }

   // Render from cache
   ImGui::Text("Scene File Preview");
   ImGui::Separator();
   ImGui::Text("%s", m_ScenePreviewCache.filename.c_str());
   
   if (!m_ScenePreviewCache.valid) return;
   
   ImGui::Text("Entities: %d", m_ScenePreviewCache.entityCount);
   int shown = 0;
   for (const auto& [id, name] : m_ScenePreviewCache.entitySummaries) {
      ImGui::BulletText("[%u] %s", id, name.c_str());
      shown++;
   }
   if (m_ScenePreviewCache.entityCount > 25) {
      ImGui::Text("... (%d more)", m_ScenePreviewCache.entityCount - 25);
   }
   
   if (!m_ScenePreviewCache.referencedAssets.empty()) {
      ImGui::Separator();
      ImGui::Text("Referenced assets:");
      int shownA = 0;
      for (const auto& a : m_ScenePreviewCache.referencedAssets) {
         if (shownA++ > 30) { 
            ImGui::Text("... (%d more)", (int)m_ScenePreviewCache.referencedAssets.size() - 30); 
            break; 
         }
         ImGui::BulletText("%s", a.c_str());
      }
   }
}

void InspectorPanel::DrawAssetSummaryCard(const std::string& extLower) {
   if (m_SelectedAssetPath.empty()) return;
   namespace fs = std::filesystem;
   fs::path path(m_SelectedAssetPath);
   ImGui::Separator();
   ImGui::TextDisabled("Asset Summary");
   if (!fs::exists(path)) {
      ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1.0f), "File not found on disk.");
      return;
      }
   bool isDir = fs::is_directory(path);
   ImGui::Text("Name: %s", path.filename().string().c_str());
   ImGui::Text("Path: %s", path.string().c_str());
   std::error_code ec;
   fs::path rel = fs::relative(path, Project::GetProjectDirectory(), ec);
   if (!ec) {
      ImGui::Text("Project-relative: %s", rel.generic_string().c_str());
      }
   else {
      ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "Outside project directory");
      }
   if (isDir) {
      ImGui::Text("Type: Folder");
      return;
      }
   auto size = fs::file_size(path);
   ImGui::Text("Size: %.2f KB", (double)size / 1024.0);
   try {
      auto ftime = fs::last_write_time(path);
      auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
         ftime - decltype(ftime)::clock::now() + std::chrono::system_clock::now());
      std::time_t tt = std::chrono::system_clock::to_time_t(systemTime);
      char buffer[64]{};
#ifdef _WIN32
      tm localTm;
      localtime_s(&localTm, &tt);
      std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTm);
#else
      std::tm* localTm = std::localtime(&tt);
      if (localTm) std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", localTm);
#endif
      if (buffer[0] != 0) {
         ImGui::Text("Modified: %s", buffer);
         }
      }
   catch (...) {}

   ClaymoreGUID guid{};
   std::string normalized = path.string();
   std::replace(normalized.begin(), normalized.end(), '\\', '/');
   guid = AssetLibrary::Instance().GetGUIDForPath(normalized);
   if (guid.high == 0 && guid.low == 0 && !ec) {
      std::string virtualPath = rel.generic_string();
      std::replace(virtualPath.begin(), virtualPath.end(), '\\', '/');
      guid = AssetLibrary::Instance().GetGUIDForPath(virtualPath);
      }
   std::string metaPath = path.extension() == ".meta" ? path.string() : (path.string() + ".meta");
   AssetMetadata meta;
   bool metaLoaded = false;
   if (fs::exists(metaPath)) {
      try {
         std::ifstream in(metaPath);
         if (in) {
            nlohmann::json j; in >> j; meta = j.get<AssetMetadata>(); metaLoaded = true;
            }
         }
      catch (...) {}
      }
   if (!metaLoaded && (guid.high != 0 || guid.low != 0)) {
      meta.guid = guid;
      }
   if (metaLoaded && !meta.type.empty()) {
      ImGui::Text("Meta Type: %s", meta.type.c_str());
      }
   if (meta.guid.high != 0 || meta.guid.low != 0) {
      ImGui::Text("GUID: %s", meta.guid.ToString().c_str());
      }
   else if (guid.high != 0 || guid.low != 0) {
      ImGui::Text("GUID: %s", guid.ToString().c_str());
      }
   else {
      ImGui::TextColored(ImVec4(0.85f, 0.6f, 0.2f, 1.0f), "GUID: Not registered");
      }
   ImGui::Text("Extension: %s", extLower.empty() ? "<none>" : extLower.c_str());
   }

void InspectorPanel::DrawClayObjectInspector() {
   using json = nlohmann::json;
   namespace fs = std::filesystem;
   
   if (m_SelectedAssetPath.empty()) return;
   
   // Load or use cached JSON - only reload when path changes
   if (m_ClayObjCachePath != m_SelectedAssetPath) {
      m_ClayObjCachePath = m_SelectedAssetPath;
      m_ClayObjModified = false;
      try {
         std::ifstream in(m_SelectedAssetPath);
         if (!in.is_open()) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Failed to open asset file");
            m_ClayObjCache = json::object();
            return;
         }
         in >> m_ClayObjCache;
         in.close();
      } catch (const std::exception& ex) {
         ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Error loading: %s", ex.what());
         m_ClayObjCache = json::object();
         return;
      }
   }
   
   json& j = m_ClayObjCache;
   
   // Extract type info
   std::string typeName = j.value("typeName", std::string());
   std::string assetGuid = j.value("guid", std::string());
   
   // Ensure the .clayobj is registered with AssetLibrary (safety net for pre-existing files)
   if (!assetGuid.empty()) {
      ClaymoreGUID guid = ClaymoreGUID::FromString(assetGuid);
      if (guid.high != 0 || guid.low != 0) {
         ClaymoreGUID existingGuid = AssetLibrary::Instance().GetGUIDForPath(m_SelectedAssetPath);
         if (existingGuid.high == 0 && existingGuid.low == 0) {
            // GUID from file not yet registered - register it now
            std::string name = fs::path(m_SelectedAssetPath).stem().string();
            std::string vpath = m_SelectedAssetPath;
            std::replace(vpath.begin(), vpath.end(), '\\', '/');
            size_t pos = vpath.find("assets/");
            if (pos != std::string::npos) vpath = vpath.substr(pos);
            AssetReference aref(guid, 0, static_cast<int32_t>(AssetType::Scriptable));
            AssetLibrary::Instance().RegisterAsset(aref, AssetType::Scriptable, vpath, name);
            AssetLibrary::Instance().RegisterPathAlias(guid, m_SelectedAssetPath);
         }
      }
   }
   
   // Clean header - just the type name in a nice component-style header
   std::string shortTypeName = typeName;
   size_t lastDot = typeName.rfind('.');
   if (lastDot != std::string::npos) shortTypeName = typeName.substr(lastDot + 1);
   
   ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
   ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.28f, 0.28f, 0.28f, 1.0f));
   bool headerOpen = ImGui::CollapsingHeader(shortTypeName.empty() ? "ClayObject" : shortTypeName.c_str(), 
      ImGuiTreeNodeFlags_DefaultOpen);
   ImGui::PopStyleColor(2);
   
   if (!headerOpen) return;
   
   // Look up type descriptor from registry
   const ScriptableTypeDesc* typeDesc = ScriptableTypeRegistry::Get().FindByName(typeName);
   
   if (!typeDesc) {
      ImGui::Indent();
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Type not registered");
      ImGui::TextDisabled("Compile scripts to enable editing");
      ImGui::Unindent();
      return;
   }
   
   // Show instance type title
   ImGui::Indent(4.0f);
   ImGui::TextDisabled("Instance of:");
   ImGui::SameLine();
   ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s", typeName.c_str());
   ImGui::Unindent(4.0f);
   
   // Separator between metadata and serialized fields
   ImGui::Spacing();
   ImGui::Separator();
   ImGui::Spacing();
   
   // Draw editable fields
   json& fields = j["fields"];
   if (!fields.is_object()) fields = json::object();
   
   const float labelWidth = 150.0f; // slightly wider so field names don't clip
   const float availWidth = ImGui::GetContentRegionAvail().x;
   
   // Helper: ClayObject dropdown using resource registry
   auto RenderClayObjectDropdown = [&](const FieldDesc& fd, std::string& guidVal)->bool {
      bool changed = false;
      std::string displayName = "None";
      if (!guidVal.empty()) {
         ClaymoreGUID cmGuid = ClaymoreGUID::FromString(guidVal);
         std::string path = AssetLibrary::Instance().GetPathForGUID(cmGuid);
         if (!path.empty()) {
            displayName = std::filesystem::path(path).stem().string();
         } else {
            displayName = "(Missing: " + guidVal.substr(0, 8) + "...)";
         }
      }

      // If select-from-resources, show dropdown of matching resources
      bool useDropdown = (fd.flags & FieldFlags::SelectFromResources) != 0;
      if (useDropdown && !fd.auxType.empty()) {
         if (ImGui::BeginCombo("##clayobj_dropdown", displayName.c_str())) {
            // Build options
            std::vector<std::string> names{ "None" };
            std::vector<std::string> guids{ "" };
            int resourceCount = Resources_GetResourceCount(fd.auxType.c_str());
            if (resourceCount > 0) {
               std::vector<char*> namePtrs(resourceCount);
               int actualCount = Resources_GetResourceNames(fd.auxType.c_str(), namePtrs.data(), resourceCount);
               std::vector<std::string> tempNames;
               tempNames.reserve(actualCount);
               for (int i = 0; i < actualCount; ++i) tempNames.emplace_back(namePtrs[i] ? namePtrs[i] : "");
               std::vector<char*> guidPtrs(resourceCount);
               int guidCount = Resources_GetResourceGUIDs(fd.auxType.c_str(), guidPtrs.data(), resourceCount);
               for (int i = 0; i < actualCount && i < guidCount; ++i) {
                  if (guidPtrs[i] && !tempNames[i].empty()) {
                     names.push_back(tempNames[i]);
                     guids.push_back(guidPtrs[i]);
                  }
               }
            }

            int currentIdx = 0;
            for (size_t i = 0; i < guids.size(); ++i) {
               if (guids[i] == guidVal) { currentIdx = static_cast<int>(i); break; }
            }

            for (size_t i = 0; i < names.size(); ++i) {
               bool selected = (static_cast<int>(i) == currentIdx);
               if (ImGui::Selectable(names[i].c_str(), selected)) {
                  if (!selected) {
                     guidVal = guids[i];
                     changed = true;
                  }
               }
               if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
         }
      } else {
         ImGui::Button(displayName.c_str(), ImVec2(-60.0f, 0));
         if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Clear")) {
               guidVal.clear();
               changed = true;
            }
            ImGui::EndPopup();
         }
         if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
               const char* droppedPath = (const char*)payload->Data;
               if (droppedPath) {
                  std::string ext = std::filesystem::path(droppedPath).extension().string();
                  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                  if (ext == ".clayobj") {
                     ClaymoreGUID newGuid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
                     if (newGuid.high != 0 || newGuid.low != 0) {
                        guidVal = newGuid.ToString();
                        changed = true;
                     }
                  }
               }
            }
            ImGui::EndDragDropTarget();
         }
      }
      return changed;
   };

   // Helper: dialogue library dropdown listing .dlglib assets
   auto RenderDialogueDropdown = [&](std::string& dlgGuid)->bool {
      return RenderGuidAssetCombo("##dlglib", ui::GetDialogueLibraryAssetOptions(), dlgGuid);
   };

   auto RenderAudioDropdown = [&](std::string& audioGuid)->bool {
      return RenderGuidAssetCombo("##audio", ui::GetAudioAssetOptions(), audioGuid);
   };

   auto RenderPathAssetDropdown = [&](const char* comboId,
                                      std::string& assetPath,
                                      const std::initializer_list<const char*>& extensions)->bool {
      bool changed = false;
      std::string displayName = assetPath.empty() ? "None" : std::filesystem::path(assetPath).stem().string();
      if (!assetPath.empty() && displayName.empty()) {
         displayName = std::filesystem::path(assetPath).filename().string();
         }

      const auto options = CollectProjectAssetOptions(extensions);
      if (ImGui::BeginCombo(comboId, displayName.c_str())) {
         if (ImGui::Selectable("None", assetPath.empty())) {
            if (!assetPath.empty()) {
               assetPath.clear();
               changed = true;
               }
            }

         for (const auto& opt : options) {
            bool selected = (assetPath == opt.vfsPath);
            if (ImGui::Selectable(opt.name.c_str(), selected)) {
               if (!selected) {
                  assetPath = opt.vfsPath;
                  changed = true;
                  }
               }
            if (ImGui::IsItemHovered()) {
               ImGui::SetTooltip("%s", opt.vfsPath.c_str());
               }
            if (selected) ImGui::SetItemDefaultFocus();
            }

         ImGui::EndCombo();
         }

      if (ImGui::BeginDragDropTarget()) {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
            const char* droppedPath = (const char*)payload->Data;
            if (droppedPath) {
               std::string ext = std::filesystem::path(droppedPath).extension().string();
               std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
               bool matches = false;
               for (const char* expected : extensions) {
                  if (ext == expected) {
                     matches = true;
                     break;
                     }
                  }
               if (matches) {
                  std::string vfsPath = ToVfsAssetPath(droppedPath);
                  if (assetPath != vfsPath) {
                     assetPath = vfsPath;
                     changed = true;
                     }
                  }
               }
            }
         ImGui::EndDragDropTarget();
         }

      return changed;
      };

   auto NormalizeColorComponent = [](float v) {
      if (v > 1.0f) v /= 255.0f;
      return std::clamp(v, 0.0f, 1.0f);
   };

   auto ReadColorFromJson = [&](const json& node, glm::vec4& out)->bool {
      if (node.is_array() && node.size() >= 4) {
         out.x = NormalizeColorComponent(node[0].get<float>());
         out.y = NormalizeColorComponent(node[1].get<float>());
         out.z = NormalizeColorComponent(node[2].get<float>());
         out.w = NormalizeColorComponent(node[3].get<float>());
         return true;
      }
      if (node.is_object()) {
         auto readComp = [&](const char* key, float& dst)->bool {
            auto it = node.find(key);
            if (it != node.end() && it->is_number()) {
               dst = NormalizeColorComponent(it->get<float>());
               return true;
            }
            return false;
         };
         glm::vec4 v(1.0f);
         bool any = false;
         any |= readComp("r", v.x) || readComp("R", v.x);
         any |= readComp("g", v.y) || readComp("G", v.y);
         any |= readComp("b", v.z) || readComp("B", v.z);
         if (readComp("a", v.w) || readComp("A", v.w)) any = true;
         if (any) {
            out = v;
            return true;
         }
      }
      return false;
   };

   auto WriteColorToJson = [&](json& node, const glm::vec4& v) {
      node = json::array({ v.x, v.y, v.z, v.w });
   };

   // Recursive helper for struct fields (operates on json object)
   std::function<void(json&, const std::vector<FieldDesc>&)> DrawStructFields;
   DrawStructFields = [&](json& obj, const std::vector<FieldDesc>& sFields) {
      for (const auto& sub : sFields) {
         ImGui::PushID(sub.name.c_str());
         std::string pretty = sub.name;
         if (!pretty.empty()) pretty[0] = (char)toupper(pretty[0]);
         for (size_t i = 1; i < pretty.size(); ++i) { if (isupper(pretty[i])) { pretty.insert(i, " "); ++i; } }

         if (sub.arrayRank > 0) {
            json& arr = obj[sub.name];
            if (!arr.is_array()) arr = json::array();
            size_t count = arr.size();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s [%zu]", pretty.c_str(), count);
            ImGui::SameLine();
            if (ImGui::SmallButton("+")) { arr.push_back(json::object()); m_ClayObjModified = true; }
            ImGui::SameLine();
            ImGui::BeginDisabled(count == 0);
            if (ImGui::SmallButton("-") && count > 0) { arr.erase(arr.end() - 1); m_ClayObjModified = true; }
            ImGui::EndDisabled();
            for (size_t i = 0; i < arr.size(); ++i) {
               ImGui::PushID((int)i);
               ImGui::Indent();
               ImGui::TextDisabled("#%zu", i);
               ImGui::Indent();
               DrawStructFields(arr[i], sub.structFields);
               ImGui::Unindent(2);
               ImGui::PopID();
            }
         } else if (sub.type == ValueType::Struct) {
            json& child = obj[sub.name];
            if (!child.is_object()) child = json::object();
            if (ImGui::TreeNode(pretty.c_str())) {
               DrawStructFields(child, sub.structFields);
               ImGui::TreePop();
            }
         } else {
            json& val = obj[sub.name];
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(pretty.c_str());
            ImGui::SameLine(labelWidth + 6.0f); // add breathing room between label and input
            ImGui::SetNextItemWidth(availWidth - labelWidth - 12.0f);
            switch (sub.type) {
               case ValueType::Bool: {
                  bool v = val.is_boolean() ? val.get<bool>() : false;
                  if (!val.is_boolean()) { val = v; m_ClayObjModified = true; }
                  if (ImGui::Checkbox("##val", &v)) { val = v; m_ClayObjModified = true; }
                  break;
               }
               case ValueType::Int32: {
                  int v = val.is_number_integer() ? val.get<int>() : 0;
                  // Fix null in JSON: runtime loader can't deserialize null for int, so normalize to 0
                  if (!val.is_number_integer()) { val = v; m_ClayObjModified = true; }
                  if (ImGui::DragInt("##val", &v)) { val = v; m_ClayObjModified = true; }
                  break;
               }
               case ValueType::Float: {
                  float v = val.is_number() ? val.get<float>() : 0.0f;
               if (ImGui::DragFloat("##val", &v, 0.1f)) { val = v; m_ClayObjModified = true; }
                  break;
               }
               case ValueType::String: {
                  std::string v = val.is_string() ? val.get<std::string>() : "";
                  char buf[256]; strncpy(buf, v.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';
               if (ImGui::InputText("##val", buf, sizeof(buf))) { val = std::string(buf); m_ClayObjModified = true; }
                  break;
               }
               case ValueType::Color: {
                  glm::vec4 v(1.0f);
                  ReadColorFromJson(val, v);
                  if (ImGui::ColorEdit4("##val", &v.x, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_Float)) {
                     WriteColorToJson(val, v);
                     m_ClayObjModified = true;
                  }
                  break;
               }
               case ValueType::Prefab: {
                  std::string pguid = val.is_string() ? val.get<std::string>() : "";
                  std::string display = "None";
                  if (!pguid.empty()) {
                     ClaymoreGUID cmGuid = ClaymoreGUID::FromString(pguid);
                     std::string path = AssetLibrary::Instance().GetPathForGUID(cmGuid);
                     display = path.empty() ? "(Missing)" : std::filesystem::path(path).stem().string();
                  }
                  ImGui::Button(display.c_str(), ImVec2(-60.0f, 0));
                  if (ImGui::BeginDragDropTarget()) {
                     if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                        const char* droppedPath = (const char*)payload->Data;
                        if (droppedPath) {
                           std::string ext = std::filesystem::path(droppedPath).extension().string();
                           std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                           if (ext == ".prefab") {
                              ClaymoreGUID newGuid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
                              if (newGuid.high != 0 || newGuid.low != 0) { val = newGuid.ToString(); m_ClayObjModified = true; }
                           }
                        }
                     }
                     ImGui::EndDragDropTarget();
                  }
                  ImGui::SameLine();
                  if (ImGui::SmallButton("X")) { val = ""; m_ClayObjModified = true; }
                  break;
               }
               case ValueType::Texture: {
                  std::string textureRef = val.is_string() ? val.get<std::string>() : "";
                  std::string guidPart = textureRef;
                  size_t colonPos = textureRef.find(':');
                  if (colonPos != std::string::npos) {
                     guidPart = textureRef.substr(0, colonPos);
                  }

                  std::string texturePath;
                  std::string display = "Pick";
                  if (!guidPart.empty()) {
                     ClaymoreGUID cmGuid = ClaymoreGUID::FromString(guidPart);
                     texturePath = AssetLibrary::Instance().GetPathForGUID(cmGuid);
                     display = texturePath.empty() ? "(Missing)" : std::filesystem::path(texturePath).stem().string();
                  }

                  std::string popupId = "##struct_tex_picker_" + sub.name;
                  bool requestPicker = false;
                  ImGui::Button(display.c_str(), ImVec2(-60.0f, 0));
                  if (ImGui::IsItemClicked()) {
                     requestPicker = true;
                  }
                  if (ImGui::BeginDragDropTarget()) {
                     if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                        const char* droppedPath = (const char*)payload->Data;
                        if (droppedPath) {
                           std::string ext = std::filesystem::path(droppedPath).extension().string();
                           std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                           if (IsTextureExtension(ext)) {
                              ClaymoreGUID newGuid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
                              if (newGuid.high != 0 || newGuid.low != 0) { val = newGuid.ToString() + ":0"; m_ClayObjModified = true; }
                           }
                        }
                     }
                     ImGui::EndDragDropTarget();
                  }
                  if (requestPicker) {
                     ImGui::OpenPopup(popupId.c_str());
                  }
                  texturepicker::DrawTexturePickerPopup(popupId.c_str(),
                     [&](const std::string& selectedPath) {
                        ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(selectedPath);
                        if (guid.high != 0 || guid.low != 0) {
                           val = guid.ToString() + ":0";
                           m_ClayObjModified = true;
                        }
                     },
                     texturePath);
                  ImGui::SameLine();
                  if (ImGui::SmallButton("X")) { val = ""; m_ClayObjModified = true; }
                  break;
               }
               case ValueType::Audio: {
                  std::string audioGuid = val.is_string() ? val.get<std::string>() : "";
                  if (RenderAudioDropdown(audioGuid)) {
                     val = audioGuid;
                     m_ClayObjModified = true;
                  }

                  if (ImGui::BeginDragDropTarget()) {
                     if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                        std::string droppedGuid = audioGuid;
                        if (TryAssignGuidAssetDrop(droppedGuid, (const char*)payload->Data, [](const std::string& ext) {
                               return IsAudioExtension(ext);
                            })) {
                           val = droppedGuid;
                           m_ClayObjModified = true;
                           }
                     }
                     ImGui::EndDragDropTarget();
                  }
                  ImGui::SameLine();
                  if (ImGui::SmallButton("X")) { val = ""; m_ClayObjModified = true; }
                  break;
               }
               case ValueType::ClayObject: {
                  std::string guid = val.is_string() ? val.get<std::string>() : "";
                  if (RenderClayObjectDropdown(sub, guid)) { val = guid; m_ClayObjModified = true; }
                  break;
               }
               case ValueType::DialogueLibrary: {
                  std::string guid = val.is_string() ? val.get<std::string>() : "";
                  if (RenderDialogueDropdown(guid)) { val = guid; m_ClayObjModified = true; }
                  break;
               }
               case ValueType::AnimationController: {
                  std::string path = val.is_string() ? val.get<std::string>() : "";
                  if (RenderPathAssetDropdown("##animctrl", path, { ".animctrl" })) { val = path; m_ClayObjModified = true; }
                  break;
               }
               case ValueType::AnimationControllerOverride: {
                  std::string path = val.is_string() ? val.get<std::string>() : "";
                  if (RenderPathAssetDropdown("##animoverride", path, { ".animoverride" })) { val = path; m_ClayObjModified = true; }
                  break;
               }
               case ValueType::Enum: {
                  int v = val.is_number_integer() ? val.get<int>() : 0;
                  if (!val.is_number_integer()) {
                     if (!sub.enumValues.empty()) v = sub.enumValues[0];
                     val = v;
                     m_ClayObjModified = true;
                  }
                  if (!sub.enumNames.empty() && sub.enumNames.size() == sub.enumValues.size()) {
                     int currentIdx = 0;
                     for (size_t i = 0; i < sub.enumValues.size(); ++i) {
                        if (sub.enumValues[i] == v) { currentIdx = static_cast<int>(i); break; }
                     }
                     if (ImGui::BeginCombo("##val", sub.enumNames[currentIdx].c_str())) {
                        for (size_t i = 0; i < sub.enumNames.size(); ++i) {
                           bool selected = (static_cast<int>(i) == currentIdx);
                           if (ImGui::Selectable(sub.enumNames[i].c_str(), selected)) {
                              val = sub.enumValues[i];
                              m_ClayObjModified = true;
                           }
                           if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                     }
                  } else {
                     if (ImGui::DragInt("##val", &v)) { val = v; m_ClayObjModified = true; }
                  }
                  break;
               }
               default:
                  ImGui::TextDisabled("(unsupported)");
                  break;
            }
         }
         ImGui::PopID();
      }
   };

   ImGui::Indent(4.0f);
   
   for (const auto& fd : typeDesc->fields) {
      ImGui::PushID(fd.name.c_str());

      // Conditional visibility: skip drawing if ShowIf/HideIf condition not met
      // showIfValue can be a single value or pipe-separated list (OR: match any)
      if (!fd.showIfField.empty()) {
         bool match = false;
         auto it = fields.find(fd.showIfField);
         if (it != fields.end() && !it->is_null()) {
            const json& condVal = *it;
            // Split by '|' for compound values (e.g. "0|1" for ItemType.Armor | ItemType.Weapon)
            std::vector<std::string> alternatives;
            {
               std::string s = fd.showIfValue;
               size_t start = 0, pos;
               while ((pos = s.find('|', start)) != std::string::npos) {
                  alternatives.push_back(s.substr(start, pos - start));
                  start = pos + 1;
               }
               alternatives.push_back(s.substr(start));
            }
            if (alternatives.empty()) alternatives.push_back(fd.showIfValue);

            if (condVal.is_number()) {
               double current = condVal.get<double>();
               for (const std::string& alt : alternatives) {
                  try {
                     double expected = std::stod(alt);
                     if (std::abs(current - expected) < 1e-9) { match = true; break; }
                  } catch (...) {}
               }
            } else if (condVal.is_boolean()) {
               bool current = condVal.get<bool>();
               for (const std::string& alt : alternatives) {
                  if ((current && alt == "true") || (!current && alt == "false")) { match = true; break; }
               }
            } else if (condVal.is_string()) {
               std::string currentStr = condVal.get<std::string>();
               for (const std::string& alt : alternatives) {
                  if (currentStr == alt) { match = true; break; }
               }
            }
         }
         bool skip = (fd.showIfMode == 0) ? !match : match;  // 0 = ShowIf (skip when not equal), 1 = HideIf (skip when equal)
         if (skip) {
            ImGui::PopID();
            continue;
         }
      }

      // Pretty field name
      std::string pretty = fd.name;
      if (!pretty.empty()) pretty[0] = (char)toupper(pretty[0]);
      for (size_t i = 1; i < pretty.size(); ++i) {
         if (isupper(pretty[i])) { pretty.insert(i, " "); ++i; }
      }
      
      // Check if this is a list (arrayRank > 0)
      if (fd.arrayRank > 0) {
         // List field - similar to ScriptComponent list UI
         json& listData = fields[fd.name];
         if (!listData.is_array()) listData = json::array();
         bool isReadOnly = (fd.flags & FieldFlags::PopulateFromResources) != 0 || (fd.flags & FieldFlags::ReadOnly) != 0;
         
         size_t count = listData.size();
         
         ImGui::PushID("list");
         bool listOpen = ImGui::TreeNodeEx("##list_header", 
            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth,
            "%s  %zu", pretty.c_str(), count);
         
         if (listOpen) {
            int toRemove = -1;
            
            for (size_t i = 0; i < listData.size(); ++i) {
               ImGui::PushID(static_cast<int>(i));
               ImGui::BeginDisabled(isReadOnly);
               
               ImGui::AlignTextToFramePadding();
               ImGui::TextDisabled("%zu", i);
               ImGui::SameLine(40.0f);
               ImGui::SetNextItemWidth(availWidth - 80.0f);
               
               // Draw element based on the list element type (not fd.type which is List for lists)
               ValueType elementType = (fd.arrayRank > 0 && fd.listElementType != ValueType::None) 
                                      ? fd.listElementType 
                                      : fd.type;
               switch (elementType) {
                  case ValueType::Int32: {
                     int v = listData[i].is_number_integer() ? listData[i].get<int>() : 0;
                     if (ImGui::DragInt("##val", &v)) { listData[i] = v; m_ClayObjModified = true; }
                     break;
                  }
                  case ValueType::Float: {
                     float v = listData[i].is_number() ? listData[i].get<float>() : 0.0f;
                     if (ImGui::DragFloat("##val", &v, 0.1f)) { listData[i] = v; m_ClayObjModified = true; }
                     break;
                  }
                  case ValueType::Bool: {
                     bool v = listData[i].is_boolean() ? listData[i].get<bool>() : false;
                     if (!listData[i].is_boolean()) { listData[i] = v; m_ClayObjModified = true; }
                     if (ImGui::Checkbox("##val", &v)) { listData[i] = v; m_ClayObjModified = true; }
                     break;
                  }
                  case ValueType::String: {
                     std::string v = listData[i].is_string() ? listData[i].get<std::string>() : "";
                     char buf[256];
                     strncpy(buf, v.c_str(), sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
                     if (ImGui::InputText("##val", buf, sizeof(buf))) { listData[i] = std::string(buf); m_ClayObjModified = true; }
                     break;
                  }
                  case ValueType::Vec3: {
                     glm::vec3 v(0.0f);
                     if (listData[i].is_array() && listData[i].size() >= 3) {
                        v.x = listData[i][0].get<float>();
                        v.y = listData[i][1].get<float>();
                        v.z = listData[i][2].get<float>();
                     }
                     if (ImGui::DragFloat3("##val", &v.x, 0.1f)) { listData[i] = {v.x, v.y, v.z}; m_ClayObjModified = true; }
                     break;
                  }
                  case ValueType::Color: {
                     glm::vec4 v(1.0f);
                     ReadColorFromJson(listData[i], v);
                     if (ImGui::ColorEdit4("##val", &v.x, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_Float)) {
                        WriteColorToJson(listData[i], v);
                        m_ClayObjModified = true;
                     }
                     break;
                  }
                  case ValueType::Prefab: {
                     std::string prefabGuid = listData[i].is_string() ? listData[i].get<std::string>() : "";
                     std::string displayName = "None";
                     
                     if (!prefabGuid.empty()) {
                        ClaymoreGUID cmGuid = ClaymoreGUID::FromString(prefabGuid);
                        std::string path = AssetLibrary::Instance().GetPathForGUID(cmGuid);
                        if (!path.empty()) {
                           displayName = std::filesystem::path(path).stem().string();
                        } else {
                           displayName = "(Missing)";
                        }
                     }
                     
                     ImGui::Button(displayName.c_str(), ImVec2(-60.0f, 0));
                     
                     // Accept prefab file drops
                     if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                           const char* droppedPath = (const char*)payload->Data;
                           if (droppedPath) {
                              std::string ext = std::filesystem::path(droppedPath).extension().string();
                              std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                              
                              if (ext == ".prefab") {
                                 ClaymoreGUID newGuid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
                                 if (newGuid.high != 0 || newGuid.low != 0) {
                                    listData[i] = newGuid.ToString();
                                    m_ClayObjModified = true;
                                 }
                              }
                           }
                        }
                        ImGui::EndDragDropTarget();
                     }
                     break;
                  }
               case ValueType::Texture: {
                  std::string textureRef = listData[i].is_string() ? listData[i].get<std::string>() : "";
                  std::string guidPart = textureRef;
                  size_t colonPos = textureRef.find(':');
                  if (colonPos != std::string::npos) {
                     guidPart = textureRef.substr(0, colonPos);
                  }

                  std::string texturePath;
                  std::string displayName = "None";
                  if (!guidPart.empty()) {
                     ClaymoreGUID cmGuid = ClaymoreGUID::FromString(guidPart);
                     texturePath = AssetLibrary::Instance().GetPathForGUID(cmGuid);
                     if (!texturePath.empty()) {
                        displayName = std::filesystem::path(texturePath).stem().string();
                     } else {
                        displayName = "(Missing)";
                     }
                  }

                  std::string popupId = "##list_tex_picker_" + fd.name + "_" + std::to_string(i);
                  bool requestPicker = false;
                  ImGui::Button(displayName.c_str(), ImVec2(-60.0f, 0));
                  if (ImGui::IsItemClicked()) {
                     requestPicker = true;
                  }

                  if (ImGui::BeginDragDropTarget()) {
                     if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                        const char* droppedPath = (const char*)payload->Data;
                        if (droppedPath) {
                           std::string ext = std::filesystem::path(droppedPath).extension().string();
                           std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                           if (IsTextureExtension(ext)) {
                              ClaymoreGUID newGuid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
                              if (newGuid.high != 0 || newGuid.low != 0) {
                                 listData[i] = newGuid.ToString() + ":0";
                                 m_ClayObjModified = true;
                              }
                           }
                        }
                     }
                     ImGui::EndDragDropTarget();
                  }

                  if (requestPicker) {
                     ImGui::OpenPopup(popupId.c_str());
                  }

                  texturepicker::DrawTexturePickerPopup(popupId.c_str(),
                     [&](const std::string& selectedPath) {
                        ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(selectedPath);
                        if (guid.high != 0 || guid.low != 0) {
                           listData[i] = guid.ToString() + ":0";
                           m_ClayObjModified = true;
                        }
                     },
                     texturePath);
                  break;
               }
               case ValueType::Audio: {
                  std::string guid = listData[i].is_string() ? listData[i].get<std::string>() : "";
                  if (RenderAudioDropdown(guid)) {
                     listData[i] = guid;
                     m_ClayObjModified = true;
                  }

                  if (ImGui::BeginDragDropTarget()) {
                     if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                        std::string droppedGuid = guid;
                        if (TryAssignGuidAssetDrop(droppedGuid, (const char*)payload->Data, [](const std::string& ext) {
                               return IsAudioExtension(ext);
                            })) {
                           listData[i] = droppedGuid;
                           m_ClayObjModified = true;
                           }
                     }
                     ImGui::EndDragDropTarget();
                  }
                  break;
               }
               case ValueType::ClayObject: {
                  std::string guid = listData[i].is_string() ? listData[i].get<std::string>() : "";
                  if (RenderClayObjectDropdown(fd, guid)) { listData[i] = guid; m_ClayObjModified = true; }
                  break;
               }
               case ValueType::DialogueLibrary: {
                  std::string guid = listData[i].is_string() ? listData[i].get<std::string>() : "";
                  if (RenderDialogueDropdown(guid)) { listData[i] = guid; m_ClayObjModified = true; }
                  break;
               }
               case ValueType::AnimationController: {
                  std::string path = listData[i].is_string() ? listData[i].get<std::string>() : "";
                  if (RenderPathAssetDropdown("##animctrl", path, { ".animctrl" })) { listData[i] = path; m_ClayObjModified = true; }
                  break;
               }
               case ValueType::AnimationControllerOverride: {
                  std::string path = listData[i].is_string() ? listData[i].get<std::string>() : "";
                  if (RenderPathAssetDropdown("##animoverride", path, { ".animoverride" })) { listData[i] = path; m_ClayObjModified = true; }
                  break;
               }
               case ValueType::Enum: {
                  // Parse enum value from JSON (can be int or string representation)
                  int v = 0;
                  if (listData[i].is_number_integer()) {
                     v = listData[i].get<int>();
                  } else if (listData[i].is_string()) {
                     try {
                        v = std::stoi(listData[i].get<std::string>());
                     } catch (...) {
                        v = 0;
                     }
                  } else {
                     if (!fd.enumValues.empty()) v = fd.enumValues[0];
                     listData[i] = v;
                     m_ClayObjModified = true;
                  }
                  
                  // If enum metadata is available, render as dropdown
                  if (!fd.enumNames.empty() && fd.enumNames.size() == fd.enumValues.size()) {
                     // Find current selection index
                     int currentIdx = 0;
                     for (size_t j = 0; j < fd.enumValues.size(); ++j) {
                        if (fd.enumValues[j] == v) { currentIdx = static_cast<int>(j); break; }
                     }
                     
                     // Build combo items
                     std::string preview = (currentIdx < (int)fd.enumNames.size()) 
                                          ? fd.enumNames[currentIdx] : "Unknown";
                     if (ImGui::BeginCombo("##val", preview.c_str())) {
                        for (size_t j = 0; j < fd.enumNames.size(); ++j) {
                           bool selected = (static_cast<int>(j) == currentIdx);
                           if (ImGui::Selectable(fd.enumNames[j].c_str(), selected)) {
                              listData[i] = fd.enumValues[j];
                              m_ClayObjModified = true;
                           }
                           if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                     }
                  } else {
                     // Fallback to int drag if no enum metadata
                     if (ImGui::DragInt("##val", &v)) { listData[i] = v; m_ClayObjModified = true; }
                  }
                  break;
               }
               case ValueType::Struct: {
                  if (!listData[i].is_object()) listData[i] = json::object();
                  
                  // Generate label: use listElementTypeName if available, otherwise format field name
                  std::string structLabel;
                  if (!fd.listElementTypeName.empty()) {
                     // Extract just the type name (last part after dot)
                     size_t lastDot = fd.listElementTypeName.rfind('.');
                     structLabel = (lastDot != std::string::npos) ? 
                        fd.listElementTypeName.substr(lastDot + 1) : fd.listElementTypeName;
                     // Remove "Data" suffix if present for cleaner display
                     if (structLabel.size() > 4 && structLabel.substr(structLabel.size() - 4) == "Data") {
                        structLabel = structLabel.substr(0, structLabel.size() - 4) + " Data";
                     }
                  } else {
                     // Format field name: capitalize first letter and add spaces before capitals
                     structLabel = fd.name;
                     if (!structLabel.empty()) {
                        structLabel[0] = (char)toupper(structLabel[0]);
                        for (size_t j = 1; j < structLabel.size(); ++j) {
                           if (isupper(structLabel[j]) && !isupper(structLabel[j-1])) {
                              structLabel.insert(j, " ");
                              ++j;
                           }
                        }
                     }
                  }
                  
                  char nodeLabel[128];
                  snprintf(nodeLabel, sizeof(nodeLabel), "%s %zu", structLabel.c_str(), i + 1);

                  // Expandable element header + remove button.
                  // NOTE: struct fields come from fd.structFields; if empty, show a hint.
                  bool nodeOpen = ImGui::TreeNodeEx(
                     nodeLabel,
                     ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth);

                  // Right-align remove with a small inset so it doesn't hug the label
                  float removeButtonWidth = 70.0f;
                  float pad = 6.0f;
                  ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - removeButtonWidth - pad);
                  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.2f, 0.2f, 0.6f));
                  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.3f, 0.3f, 0.8f));
                  if (ImGui::SmallButton("Remove")) {
                     toRemove = static_cast<int>(i);
                  }
                  ImGui::PopStyleColor(2);

                  if (nodeOpen) {
                     if (fd.structFields.empty()) {
                        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                           "No struct metadata. Recompile scripts to refresh.");
                     } else {
                        DrawStructFields(listData[i], fd.structFields);
                     }
                     ImGui::TreePop();
                  }
                  
                  break;
               }
                  default:
                     ImGui::TextDisabled("(unsupported)");
                     break;
               }
               
               // Remove button placement: non-struct types get a compact "x" control
               if (elementType != ValueType::Struct) {
                  ImGui::SameLine(0, 4);
                  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.2f, 0.2f, 0.6f));
                  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.3f, 0.3f, 0.8f));
                  if (ImGui::Button("x", ImVec2(20, 0))) toRemove = static_cast<int>(i);
                  ImGui::PopStyleColor(2);
               }
               
               ImGui::EndDisabled();
               ImGui::PopID();
            }
            
            if (toRemove >= 0) {
               listData.erase(listData.begin() + toRemove);
               m_ClayObjModified = true;
            }
            
            if (listData.empty()) {
               ImGui::TextDisabled("  (empty)");
            }
            
            // Add/remove buttons at bottom
            ImGui::Spacing();
            float buttonWidth = 24.0f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availWidth - buttonWidth * 2 - 8.0f) * 0.5f);
            ImGui::BeginDisabled(isReadOnly);
            if (ImGui::Button("+", ImVec2(buttonWidth, 0))) {
               // Add default element
               ValueType addElementType = (fd.arrayRank > 0 && fd.listElementType != ValueType::None)
                  ? fd.listElementType
                  : fd.type;
               switch (addElementType) {
                  case ValueType::Int32: listData.push_back(0); break;
                  case ValueType::Float: listData.push_back(0.0f); break;
                  case ValueType::Bool: listData.push_back(false); break;
                  case ValueType::String: listData.push_back(""); break;
                  case ValueType::Vec3: listData.push_back({0.0f, 0.0f, 0.0f}); break;
                  case ValueType::Color: listData.push_back(json::array({1.0f, 1.0f, 1.0f, 1.0f})); break;
                  case ValueType::Prefab: listData.push_back(""); break;
                  case ValueType::Texture: listData.push_back(""); break;
                  case ValueType::Audio: listData.push_back(""); break;
                  case ValueType::ClayObject: listData.push_back(""); break;
                  case ValueType::DialogueLibrary: listData.push_back(""); break;
                  case ValueType::AnimationController: listData.push_back(""); break;
                  case ValueType::AnimationControllerOverride: listData.push_back(""); break;
                  case ValueType::Enum:
                     if (!fd.enumValues.empty()) listData.push_back(fd.enumValues[0]);
                     else listData.push_back(0);
                     break;
                  case ValueType::Struct: listData.push_back(json::object()); break;
                  default: listData.push_back(nullptr); break;
               }
               m_ClayObjModified = true;
            }
            ImGui::SameLine(0, 4);
            ImGui::BeginDisabled(count == 0 || isReadOnly);
            if (ImGui::Button("-", ImVec2(buttonWidth, 0)) && !listData.empty()) {
               listData.erase(listData.end() - 1);
               m_ClayObjModified = true;
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            
            ImGui::TreePop();
         }
         ImGui::PopID();
      }
      else {
         // Scalar field
         ImGui::AlignTextToFramePadding();
         ImGui::TextUnformatted(pretty.c_str());
         ImGui::SameLine(labelWidth);
         ImGui::SetNextItemWidth(availWidth - labelWidth - 8.0f);
         bool isReadOnly = (fd.flags & FieldFlags::PopulateFromResources) != 0 || (fd.flags & FieldFlags::ReadOnly) != 0;
         ImGui::BeginDisabled(isReadOnly);
         
         switch (fd.type) {
            case ValueType::Bool: {
               bool v = fields.value(fd.name, false);
               if (ImGui::Checkbox("##val", &v)) { fields[fd.name] = v; m_ClayObjModified = true; }
               break;
            }
            case ValueType::Int32: {
               int v = fields.value(fd.name, 0);
               if (ImGui::DragInt("##val", &v)) { fields[fd.name] = v; m_ClayObjModified = true; }
               break;
            }
            case ValueType::Int64: {
               int64_t v = fields.value(fd.name, (int64_t)0);
               if (ImGui::DragScalar("##val", ImGuiDataType_S64, &v)) { fields[fd.name] = v; m_ClayObjModified = true; }
               break;
            }
            case ValueType::Float: {
               float v = fields.value(fd.name, 0.0f);
               if (ImGui::DragFloat("##val", &v, 0.1f)) { fields[fd.name] = v; m_ClayObjModified = true; }
               break;
            }
            case ValueType::Double: {
               double v = fields.value(fd.name, 0.0);
               if (ImGui::DragScalar("##val", ImGuiDataType_Double, &v, 0.1f)) { fields[fd.name] = v; m_ClayObjModified = true; }
               break;
            }
            case ValueType::String: {
               std::string v = fields.value(fd.name, std::string());
               char buf[512];
               strncpy(buf, v.c_str(), sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
               if (ImGui::InputText("##val", buf, sizeof(buf))) { fields[fd.name] = std::string(buf); m_ClayObjModified = true; }
               break;
            }
            case ValueType::Vec3: {
               glm::vec3 v(0.0f);
               if (fields.contains(fd.name) && fields[fd.name].is_array() && fields[fd.name].size() >= 3) {
                  v.x = fields[fd.name][0].get<float>();
                  v.y = fields[fd.name][1].get<float>();
                  v.z = fields[fd.name][2].get<float>();
               }
               if (ImGui::DragFloat3("##val", &v.x, 0.1f)) { fields[fd.name] = {v.x, v.y, v.z}; m_ClayObjModified = true; }
               break;
            }
            case ValueType::Color: {
               glm::vec4 v(1.0f);
               if (fields.contains(fd.name)) {
                  ReadColorFromJson(fields[fd.name], v);
               }
               if (ImGui::ColorEdit4("##val", &v.x, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_Float)) {
                  WriteColorToJson(fields[fd.name], v);
                  m_ClayObjModified = true;
               }
               break;
            }
            case ValueType::Enum: {
               int v = fields.value(fd.name, 0);
               // If enum metadata is available, render as dropdown
               if (!fd.enumNames.empty() && fd.enumNames.size() == fd.enumValues.size()) {
                  // Find current selection index
                  int currentIdx = 0;
                  for (size_t i = 0; i < fd.enumValues.size(); ++i) {
                     if (fd.enumValues[i] == v) { currentIdx = static_cast<int>(i); break; }
                  }
                  
                  // Build combo items
                  if (ImGui::BeginCombo("##val", fd.enumNames[currentIdx].c_str())) {
                     for (size_t i = 0; i < fd.enumNames.size(); ++i) {
                        bool selected = (static_cast<int>(i) == currentIdx);
                        if (ImGui::Selectable(fd.enumNames[i].c_str(), selected)) {
                           fields[fd.name] = fd.enumValues[i];
                           m_ClayObjModified = true;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                     }
                     ImGui::EndCombo();
                  }
               } else {
                  // Fallback to int slider if no enum metadata
                  if (ImGui::DragInt("##val", &v)) { fields[fd.name] = v; m_ClayObjModified = true; }
               }
               break;
            }
            case ValueType::ClayObject: {
               std::string guid = fields.contains(fd.name) && fields[fd.name].is_string() ? fields[fd.name].get<std::string>() : "";
               if (RenderClayObjectDropdown(fd, guid)) { fields[fd.name] = guid; m_ClayObjModified = true; }
               break;
            }
            case ValueType::DialogueLibrary: {
               std::string guid = fields.contains(fd.name) && fields[fd.name].is_string() ? fields[fd.name].get<std::string>() : "";
               if (RenderDialogueDropdown(guid)) { fields[fd.name] = guid; m_ClayObjModified = true; }
               break;
            }
            case ValueType::AnimationController: {
               std::string path = fields.contains(fd.name) && fields[fd.name].is_string() ? fields[fd.name].get<std::string>() : "";
               if (RenderPathAssetDropdown("##animctrl", path, { ".animctrl" })) { fields[fd.name] = path; m_ClayObjModified = true; }
               break;
            }
            case ValueType::AnimationControllerOverride: {
               std::string path = fields.contains(fd.name) && fields[fd.name].is_string() ? fields[fd.name].get<std::string>() : "";
               if (RenderPathAssetDropdown("##animoverride", path, { ".animoverride" })) { fields[fd.name] = path; m_ClayObjModified = true; }
               break;
            }
            case ValueType::Struct: {
               json& obj = fields[fd.name];
               if (!obj.is_object()) obj = json::object();
               if (ImGui::TreeNodeEx(pretty.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth)) {
                  ImGui::Indent();
                  DrawStructFields(obj, fd.structFields);
                  ImGui::Unindent();
                  ImGui::TreePop();
               }
               break;
            }
            case ValueType::Texture: {
               std::string textureRef = fields.value(fd.name, std::string());
               std::string guidPart = textureRef;
               size_t colonPos = textureRef.find(':');
               if (colonPos != std::string::npos) {
                  guidPart = textureRef.substr(0, colonPos);
               }

               std::string texturePath;
               std::string displayName = "Pick";
               if (!guidPart.empty()) {
                  ClaymoreGUID cmGuid = ClaymoreGUID::FromString(guidPart);
                  texturePath = AssetLibrary::Instance().GetPathForGUID(cmGuid);
                  if (!texturePath.empty()) {
                     displayName = std::filesystem::path(texturePath).stem().string();
                  } else {
                     displayName = "(Missing)";
                  }
               }

               const std::string popupId = "##clayobj_texture_picker_" + fd.name;
               bool requestPicker = false;
               ImVec2 buttonSize(64.0f, 64.0f);

               bgfx::TextureHandle previewHandle = BGFX_INVALID_HANDLE;
               if (!texturePath.empty()) {
                  std::string absPath = texturePath;
                  if (!std::filesystem::path(absPath).is_absolute()) {
                     std::filesystem::path projectDir = Project::GetProjectDirectory();
                     if (!projectDir.empty()) {
                        absPath = (projectDir / absPath).string();
                     }
                  }
                  std::error_code ec;
                  if (std::filesystem::exists(absPath, ec)) {
                     TextureSpecifier spec;
                     spec.Path = absPath;
                     previewHandle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
                  }
               }

               if (bgfx::isValid(previewHandle)) {
                  if (ImGui::ImageButton("##tex", TextureLoader::ToImGuiTextureID(previewHandle), buttonSize)) {
                     requestPicker = true;
                  }
               } else {
                  if (ImGui::Button(displayName.c_str(), buttonSize)) {
                     requestPicker = true;
                  }
               }

               if (ImGui::BeginDragDropTarget()) {
                  if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                     const char* droppedPath = (const char*)payload->Data;
                     if (droppedPath) {
                        std::string ext = std::filesystem::path(droppedPath).extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (IsTextureExtension(ext)) {
                           ClaymoreGUID newGuid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
                           if (newGuid.high != 0 || newGuid.low != 0) {
                              fields[fd.name] = newGuid.ToString() + ":0";
                              m_ClayObjModified = true;
                           }
                        }
                     }
                  }
                  ImGui::EndDragDropTarget();
               }

               if (ImGui::IsItemHovered() && !texturePath.empty()) {
                  ImGui::SetTooltip("%s", texturePath.c_str());
               }

               ImGui::SameLine();
               if (ImGui::SmallButton("Clear")) {
                  fields[fd.name] = std::string();
                  m_ClayObjModified = true;
               }

               if (requestPicker) {
                  ImGui::OpenPopup(popupId.c_str());
               }

               texturepicker::DrawTexturePickerPopup(popupId.c_str(),
                  [&](const std::string& selectedPath) {
                     ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(selectedPath);
                     if (guid.high != 0 || guid.low != 0) {
                        fields[fd.name] = guid.ToString() + ":0";
                        m_ClayObjModified = true;
                     }
                  },
                  texturePath);
               break;
            }
            case ValueType::Audio: {
               std::string audioGuid = fields.value(fd.name, std::string());
               if (RenderAudioDropdown(audioGuid)) {
                  fields[fd.name] = audioGuid;
                  m_ClayObjModified = true;
               }

               if (ImGui::BeginDragDropTarget()) {
                  if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                     std::string droppedGuid = audioGuid;
                     if (TryAssignGuidAssetDrop(droppedGuid, (const char*)payload->Data, [](const std::string& ext) {
                            return IsAudioExtension(ext);
                         })) {
                        fields[fd.name] = droppedGuid;
                        m_ClayObjModified = true;
                        }
                  }
                  ImGui::EndDragDropTarget();
               }
               break;
            }
            case ValueType::Mesh: {
               // Mesh asset reference - shows display name and accepts drag-drop
               std::string meshRef = fields.value(fd.name, std::string());
               std::string displayName = "None";
               
               if (!meshRef.empty()) {
                  // Parse "GUID:fileID" format
                  std::string guidPart = meshRef;
                  int fileId = 0;
                  size_t colonPos = meshRef.find(':');
                  if (colonPos != std::string::npos) {
                     guidPart = meshRef.substr(0, colonPos);
                     try { fileId = std::stoi(meshRef.substr(colonPos + 1)); } catch(...) {}
                  }
                  
                  ClaymoreGUID cmGuid = ClaymoreGUID::FromString(guidPart);
                  std::string path = AssetLibrary::Instance().GetPathForGUID(cmGuid);
                  if (!path.empty()) {
                     std::string stem = std::filesystem::path(path).stem().string();
                     if (fileId > 0) {
                        displayName = stem + " [" + std::to_string(fileId) + "]";
                     } else {
                        displayName = stem;
                     }
                  } else {
                     displayName = "(Missing: " + guidPart.substr(0, 8) + "...)";
                  }
               }
               
               ImGui::Button(displayName.c_str(), ImVec2(-24.0f, 0));
               
               // Accept mesh file drops
               if (ImGui::BeginDragDropTarget()) {
                  if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                     const char* droppedPath = (const char*)payload->Data;
                     if (droppedPath) {
                        std::string ext = std::filesystem::path(droppedPath).extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        
                        bool isMeshFile = (ext == ".fbx" || ext == ".glb" || ext == ".gltf" || 
                                           ext == ".obj" || ext == ".dae" || ext == ".meta");
                        if (isMeshFile) {
                           ClaymoreGUID newGuid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
                           if (newGuid.high != 0 || newGuid.low != 0) {
                              std::string meshRefStr = newGuid.ToString() + ":0";
                              fields[fd.name] = meshRefStr;
                              m_ClayObjModified = true;
                           }
                        }
                     }
                  }
                  ImGui::EndDragDropTarget();
               }
               ImGui::SameLine();
               if (ImGui::SmallButton("X")) {
                  fields[fd.name] = std::string();
                  m_ClayObjModified = true;
               }
               break;
            }
            case ValueType::Prefab: {
               // Prefab asset reference - shows display name and accepts .prefab drag-drop
               std::string prefabGuid = fields.value(fd.name, std::string());
               std::string displayName = "None";
               
               if (!prefabGuid.empty()) {
                  ClaymoreGUID cmGuid = ClaymoreGUID::FromString(prefabGuid);
                  std::string path = AssetLibrary::Instance().GetPathForGUID(cmGuid);
                  if (!path.empty()) {
                     displayName = std::filesystem::path(path).stem().string();
                  } else {
                     displayName = "(Missing: " + prefabGuid.substr(0, 8) + "...)";
                  }
               }
               
               ImGui::Button(displayName.c_str(), ImVec2(-24.0f, 0));
               
               // Accept prefab file drops
               if (ImGui::BeginDragDropTarget()) {
                  if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                     const char* droppedPath = (const char*)payload->Data;
                     if (droppedPath) {
                        std::string ext = std::filesystem::path(droppedPath).extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        
                        if (ext == ".prefab") {
                           ClaymoreGUID newGuid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
                           if (newGuid.high != 0 || newGuid.low != 0) {
                              fields[fd.name] = newGuid.ToString();
                              m_ClayObjModified = true;
                           }
                        }
                     }
                  }
                  ImGui::EndDragDropTarget();
               }
               ImGui::SameLine();
               if (ImGui::SmallButton("X")) {
                  fields[fd.name] = std::string();
                  m_ClayObjModified = true;
               }
               break;
            }
            default:
               ImGui::TextDisabled("(unsupported type)");
               break;
         }
         ImGui::EndDisabled();
      }
      
      ImGui::PopID();
   }
   
   ImGui::Unindent(4.0f);
   ImGui::Spacing();
   
   // Save button - full width, professional styling
   ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.2f, 0.8f));
   ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.5f, 0.3f, 1.0f));
   ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
   
   if (ImGui::Button(m_ClayObjModified ? "Save *" : "Save", ImVec2(-1, 0))) {
      try {
         std::ofstream out(m_SelectedAssetPath);
         if (out) {
            out << j.dump(4);
            out.close();
            m_ClayObjModified = false;
            std::cout << "[ClayObject] Saved: " << m_SelectedAssetPath << std::endl;
            
            // Invalidate the cache for this clay object so the active scene uses updated values
            std::string assetGuid = j.value("guid", std::string());
            if (!assetGuid.empty()) {
               Scriptable_InvalidateCache(assetGuid.c_str());
            }
         }
      } catch (const std::exception& ex) {
         std::cerr << "[ClayObject] Save failed: " << ex.what() << std::endl;
      }
   }
   ImGui::PopStyleColor(3);
}

void InspectorPanel::DrawAnimationControllerOverrideInspector() {
   using OverrideAsset = cm::animation::AnimatorControllerOverrideAsset;
   using OverrideEntry = cm::animation::AnimatorControllerOverrideEntry;

   std::shared_ptr<OverrideAsset> loaded = cm::animation::LoadAnimatorControllerOverrideFromFile(m_SelectedAssetPath);
   OverrideAsset asset;
   if (loaded) {
      asset = *loaded;
      asset.RebuildLookup();
      }

   bool modified = false;
   const auto controllerOptions = CollectProjectAssetOptions({ ".animctrl" });

   ImGui::Separator();
   ImGui::Text("Animation Override: %s", std::filesystem::path(m_SelectedAssetPath).filename().string().c_str());
   if (!loaded) {
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "File is empty or invalid JSON. Saving will repair it.");
      }

   std::string controllerDisplay = "None";
   if (!asset.BaseControllerPath.empty()) {
      controllerDisplay = std::filesystem::path(asset.BaseControllerPath).stem().string();
      for (const auto& opt : controllerOptions) {
         if (opt.vfsPath == asset.BaseControllerPath) {
            controllerDisplay = opt.name;
            break;
            }
         }
      if (controllerDisplay.empty()) {
         controllerDisplay = std::filesystem::path(asset.BaseControllerPath).filename().string();
         }
      }

   ImGui::TextDisabled("Base Controller");
   ImGui::SetNextItemWidth(-60.0f);
   if (ImGui::BeginCombo("##anim_override_controller", controllerDisplay.c_str())) {
      bool selectedNone = asset.BaseControllerPath.empty();
      if (ImGui::Selectable("None", selectedNone)) {
         if (!asset.BaseControllerPath.empty() || !asset.Entries.empty()) {
            asset.BaseControllerPath.clear();
            asset.Entries.clear();
            modified = true;
            }
         }
      if (selectedNone) ImGui::SetItemDefaultFocus();

      for (const auto& opt : controllerOptions) {
         bool selected = (opt.vfsPath == asset.BaseControllerPath);
         if (ImGui::Selectable(opt.name.c_str(), selected)) {
            if (asset.BaseControllerPath != opt.vfsPath) {
               asset.BaseControllerPath = opt.vfsPath;
               modified = true;
               }
            }
         if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", opt.vfsPath.c_str());
            }
         if (selected) ImGui::SetItemDefaultFocus();
         }
      ImGui::EndCombo();
      }
   ImGui::SameLine();
   if (ImGui::SmallButton("Clear##anim_override_controller")) {
      if (!asset.BaseControllerPath.empty() || !asset.Entries.empty()) {
         asset.BaseControllerPath.clear();
         asset.Entries.clear();
         modified = true;
         }
      }

   if (!asset.BaseControllerPath.empty()) {
      auto controller = cm::animation::LoadAnimatorControllerFromFile(asset.BaseControllerPath);
      if (!controller) {
         ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Unable to load controller: %s", asset.BaseControllerPath.c_str());
         }
      else {
         struct OverrideRow {
            std::string sourcePath;
            std::string label;
         };

         std::vector<OverrideRow> rows;
         std::unordered_set<std::string> seenSources;
         auto addSource = [&](const std::string& sourcePath, const std::string& label) {
            const std::string normalizedSource = cm::animation::NormalizeAnimatorOverridePath(sourcePath);
            if (normalizedSource.empty()) return;
            if (!seenSources.insert(normalizedSource).second) return;

            std::string displayLabel = label;
            std::string stem = std::filesystem::path(normalizedSource).stem().string();
            if (!stem.empty()) {
               displayLabel = displayLabel.empty() ? stem : (displayLabel + " - " + stem);
               }
            if (displayLabel.empty()) displayLabel = normalizedSource;
            rows.push_back({ normalizedSource, displayLabel });
            };

         auto collectState = [&](const cm::animation::AnimatorState& state) {
            addSource(!state.AnimationAssetPath.empty() ? state.AnimationAssetPath : state.ClipPath, state.Name);
            for (size_t i = 0; i < state.Blend1DEntries.size(); ++i) {
               addSource(!state.Blend1DEntries[i].AssetPath.empty()
                            ? state.Blend1DEntries[i].AssetPath
                            : state.Blend1DEntries[i].ClipPath,
                         state.Name + " [Blend1D " + std::to_string(i + 1) + "]");
               }
            for (size_t i = 0; i < state.Blend2DEntries.size(); ++i) {
               addSource(!state.Blend2DEntries[i].AssetPath.empty()
                            ? state.Blend2DEntries[i].AssetPath
                            : state.Blend2DEntries[i].ClipPath,
                         state.Name + " [Blend2D " + std::to_string(i + 1) + "]");
               }
            };

         if (!controller->Layers.empty()) {
            for (const auto& layer : controller->Layers) {
               for (const auto& state : layer.States) {
                  collectState(state);
                  }
               }
            }
         else {
            for (const auto& state : controller->States) {
               collectState(state);
               }
            }

         std::unordered_map<std::string, std::string> existingOverrides;
         for (const auto& entry : asset.Entries) {
            const std::string key = cm::animation::NormalizeAnimatorOverridePath(entry.SourcePath);
            if (!key.empty()) existingOverrides[key] = ToVfsAssetPath(entry.OverridePath);
            }

         std::vector<OverrideEntry> rebuiltEntries;
         rebuiltEntries.reserve(rows.size());
         for (const auto& row : rows) {
            OverrideEntry entry;
            entry.SourcePath = row.sourcePath;
            auto it = existingOverrides.find(row.sourcePath);
            if (it != existingOverrides.end()) entry.OverridePath = it->second;
            rebuiltEntries.push_back(std::move(entry));
            }

         bool entriesDiffer = rebuiltEntries.size() != asset.Entries.size();
         if (!entriesDiffer) {
            for (size_t i = 0; i < rebuiltEntries.size(); ++i) {
               const std::string currentSource = cm::animation::NormalizeAnimatorOverridePath(asset.Entries[i].SourcePath);
               const std::string currentOverride = ToVfsAssetPath(asset.Entries[i].OverridePath);
               if (currentSource != rebuiltEntries[i].SourcePath ||
                   currentOverride != rebuiltEntries[i].OverridePath) {
                  entriesDiffer = true;
                  break;
                  }
               }
            }
         if (entriesDiffer) {
            asset.Entries = std::move(rebuiltEntries);
            modified = true;
            }

         asset.RebuildLookup();

         ImGui::Separator();
         ImGui::TextDisabled("Clip Overrides");
         if (rows.empty()) {
            ImGui::TextDisabled("No state clips found in the selected controller.");
            }
         else {
            const auto& animationOptions = ui::GetAnimationAssetOptions();
            for (size_t i = 0; i < rows.size(); ++i) {
               ImGui::PushID(static_cast<int>(i));

               std::string& overridePath = asset.Entries[i].OverridePath;
               std::string displayName = "None";
               if (!overridePath.empty()) {
                  displayName = std::filesystem::path(overridePath).stem().string();
                  if (displayName.empty()) displayName = std::filesystem::path(overridePath).filename().string();
                  }

               ImGui::Separator();
               ImGui::Text("%s", rows[i].label.c_str());
               ImGui::TextDisabled("%s", rows[i].sourcePath.c_str());
               ImGui::SetNextItemWidth(-60.0f);
               if (ImGui::BeginCombo("##override_anim", displayName.c_str())) {
                  bool selectedNone = overridePath.empty();
                  if (ImGui::Selectable("None", selectedNone)) {
                     if (!overridePath.empty()) {
                        overridePath.clear();
                        modified = true;
                        }
                     }
                  if (selectedNone) ImGui::SetItemDefaultFocus();

                  for (const auto& opt : animationOptions) {
                     const std::string vfsPath = ToVfsAssetPath(opt.path);
                     bool selected = (vfsPath == overridePath);
                     if (ImGui::Selectable(opt.name.c_str(), selected)) {
                        if (overridePath != vfsPath) {
                           overridePath = vfsPath;
                           modified = true;
                           }
                        }
                     if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", vfsPath.c_str());
                        }
                     if (selected) ImGui::SetItemDefaultFocus();
                     }
                  ImGui::EndCombo();
                  }

               if (ImGui::BeginDragDropTarget()) {
                  if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                     const char* droppedPath = (const char*)payload->Data;
                     if (droppedPath) {
                        std::string ext = std::filesystem::path(droppedPath).extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext == ".anim") {
                           std::string vfsPath = ToVfsAssetPath(droppedPath);
                           if (overridePath != vfsPath) {
                              overridePath = vfsPath;
                              modified = true;
                              }
                           }
                        }
                     }
                  ImGui::EndDragDropTarget();
                  }

               ImGui::SameLine();
               if (ImGui::SmallButton("X##override_anim_clear")) {
                  if (!overridePath.empty()) {
                     overridePath.clear();
                     modified = true;
                     }
                  }

               ImGui::PopID();
               }
            }
         }
      }
   else {
      ImGui::Separator();
      ImGui::TextDisabled("Select a base controller to populate the override slots.");
      }

   if (modified) {
      asset.RebuildLookup();
      cm::animation::SaveAnimatorControllerOverride(asset, m_SelectedAssetPath);
      }
   }

void InspectorPanel::DrawTexturePreviewCard() {
   namespace fs = std::filesystem;
   if (m_SelectedAssetPath.empty()) {
      ReleaseTexturePreview();
      return;
      }

   fs::path path(m_SelectedAssetPath);
   std::error_code ec;
   if (!fs::exists(path, ec)) {
      ReleaseTexturePreview();
      return;
      }

   fs::file_time_type stamp{};
   ec.clear();
   stamp = fs::last_write_time(path, ec);
   bool needsReload = !bgfx::isValid(m_TexturePreviewHandle) || path.string() != m_TexturePreviewPath;
   if (!ec && m_TexturePreviewTimestamp != stamp) {
      needsReload = true;
      }

   if (needsReload) {
      ReleaseTexturePreview();
      bgfx::TextureHandle handle = TextureLoader::Load2D(path.string());
      if (!bgfx::isValid(handle)) {
         return;
         }
      m_TexturePreviewHandle = handle;
      m_TexturePreviewPath = path.string();
      m_TexturePreviewTimestamp = ec ? fs::file_time_type{} : stamp;
      m_TexturePreviewSize = GetTextureDimensions(path.string());
      }

   if (!bgfx::isValid(m_TexturePreviewHandle)) {
      return;
      }

   ImGui::Separator();
   ImGui::TextDisabled("Preview");

   ImVec2 dims = (m_TexturePreviewSize.x > 0.0f && m_TexturePreviewSize.y > 0.0f)
      ? m_TexturePreviewSize
      : ImVec2(1.0f, 1.0f);
   const float aspect = dims.x / dims.y;
   const float maxWidth = ImGui::GetContentRegionAvail().x;
   const float desired = std::min(maxWidth, 320.0f);
   ImVec2 drawSize(desired, desired);
   if (aspect >= 1.0f) {
      drawSize.y = desired / (aspect > 0.0f ? aspect : 1.0f);
      }
   else {
      drawSize.x = desired * aspect;
      }
   drawSize.x = std::max(48.0f, drawSize.x);
   drawSize.y = std::max(48.0f, drawSize.y);

   ImVec2 cursor = ImGui::GetCursorPos();
   float offsetX = std::max(0.0f, (ImGui::GetContentRegionAvail().x - drawSize.x) * 0.5f);
   ImGui::SetCursorPosX(cursor.x + offsetX);
   ImGui::Image(TextureLoader::ToImGuiTextureID(m_TexturePreviewHandle), drawSize);
   ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
   ImGui::Text("Resolution: %.0f x %.0f", dims.x, dims.y);
   }

void InspectorPanel::ReleaseTexturePreview() {
   if (bgfx::isValid(m_TexturePreviewHandle)) {
      bgfx::destroy(m_TexturePreviewHandle);
      }
   m_TexturePreviewHandle = BGFX_INVALID_HANDLE;
   m_TexturePreviewSize = ImVec2(0.0f, 0.0f);
   m_TexturePreviewPath.clear();
   m_TexturePreviewTimestamp = std::filesystem::file_time_type{};
   }
void InspectorPanel::DrawGroupingControls(EntityID entity) {
   auto* data = m_Context->GetEntityData(entity);
   if (!data) return;

   auto section = DrawComponentSection("Groups", true);
   if (!section.bar.open) {
      return;
      }

   ScopedComponentContent contentScope;
   ImGui::Text("Groups");
   int removeIndex = -1;
   for (int i = 0; i < (int)data->Groups.size(); ++i) {
      ImGui::PushID(i);
      char buf[128];
      strncpy(buf, data->Groups[i].c_str(), sizeof(buf));
      buf[sizeof(buf) - 1] = 0;
      if (ImGui::InputText("##group", buf, sizeof(buf))) {
         data->Groups[i] = buf;
         }
      ImGui::SameLine();
      if (ImGui::SmallButton("Remove")) removeIndex = i;
      ImGui::PopID();
      }
   if (removeIndex >= 0) data->Groups.erase(data->Groups.begin() + removeIndex);
   if (ImGui::SmallButton("Add Group")) data->Groups.push_back("");
   }

void InspectorPanel::DrawIKEditor(EntityID entity, EntityData* selectedData)
   {
   if (!m_Context || !selectedData) return;

   EntityID ownerId = entity;
   EntityData* ownerData = ResolveIKOwner(m_Context, entity, selectedData, ownerId);
   if (!ownerData || !ownerData->Skeleton) return;

   auto menuBuilder = [&]() {
      if (ImGui::MenuItem("Add IK Component"))
         ownerData->IKs.emplace_back();
      };

   auto section = DrawComponentSection("Inverse Kinematics", false, menuBuilder);
   if (!section.bar.open) return;

   ScopedComponentContent contentScope;
   if (ownerId != entity)
      {
      std::string ownerName = GetEntityDisplayName(m_Context, ownerId);
      ImGui::TextDisabled("Editing skeleton root '%s'", ownerName.c_str());
      }

   SkeletonComponent* skeleton = ownerData->Skeleton.get();
   if (!skeleton)
      {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Skeleton data is missing on the root entity.");
      return;
      }

   std::vector<std::string> boneNames;
   BuildBoneNameTable(*skeleton, boneNames);
   std::vector<BoneUIOption> boneOptions;
   BuildBoneOptions(*skeleton, boneNames, boneOptions);

   if (boneOptions.empty())
      {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Skeleton has no bones to build IK chains from.");
      if (ImGui::Button("Add IK Component"))
         ownerData->IKs.emplace_back();
      return;
      }

   if (ownerData->IKs.empty())
      {
      ImGui::TextDisabled("No IK components configured for this skeleton.");
      if (ImGui::Button("Add IK Component"))
         ownerData->IKs.emplace_back();
      return;
      }

   for (int i = 0; i < (int)ownerData->IKs.size(); ++i)
      {
      auto& ik = ownerData->IKs[i];
      ImGui::PushID(i);
      std::string header = "IK Component##" + std::to_string(i);
      if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
         {
         bool removeIK = false;
         ImGui::Checkbox("Enabled", &ik.Enabled);
         ImGui::SliderFloat("Weight", &ik.Weight, 0.0f, 1.0f, "%.2f");
         ImGui::SliderFloat("Damping", &ik.Damping, 0.0f, 1.0f, "%.2f");
         ImGui::DragFloat("Tolerance (m)", &ik.Tolerance, 0.0001f, 0.0001f, 0.5f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
         ImGui::DragFloat("Max Iterations", &ik.MaxIterations, 0.5f, 1.0f, 64.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);

         bool chainSupportsTwoBone = (ik.Chain.size() == 3);
         ImGui::BeginDisabled(!chainSupportsTwoBone);
         if (ImGui::Checkbox("Use Two Bone Solver", &ik.UseTwoBone) && !chainSupportsTwoBone)
            ik.UseTwoBone = false;
         ImGui::EndDisabled();
         if (!chainSupportsTwoBone)
            {
            ImGui::SameLine();
            ImGui::TextDisabled("(requires exactly 3 bones)");
            }

         ImGui::Checkbox("Visualize", &ik.Visualize);

         DrawEntityReferenceField("Look Target", ik.TargetEntity, m_Context);
         DrawEntityReferenceField("Pole Target", ik.PoleEntity, m_Context);

         ImGui::TextUnformatted("Lock Axes");
         ImGui::SameLine();
         ImGui::Checkbox("X##LockX", &ik.LockAxisX);
         ImGui::SameLine();
         ImGui::Checkbox("Y##LockY", &ik.LockAxisY);
         ImGui::SameLine();
         ImGui::Checkbox("Z##LockZ", &ik.LockAxisZ);

         auto clearChain = [&]() {
            if (!ik.Chain.empty()) {
               ik.Chain.clear();
               ik.WasValidLastFrame = false;
               }
            };

         BoneId selectedRoot = ik.ChainRootHint;
         if (selectedRoot < 0 && !ik.Chain.empty()) selectedRoot = ik.Chain.front();
         BoneId selectedTip = ik.ChainEffectorHint;
         if (selectedTip < 0 && ik.Chain.size() >= 2) selectedTip = ik.Chain.back();

         bool rootChanged = false;
         bool tipChanged = false;

         std::string rootPreview = (selectedRoot >= 0 && (size_t)selectedRoot < boneNames.size()) ? boneNames[selectedRoot] : "<Select Bone>";
         if (ImGui::BeginCombo("Chain Root Bone", rootPreview.c_str()))
            {
            if (ImGui::Selectable("<None>", selectedRoot == -1))
               {
               selectedRoot = -1;
               rootChanged = true;
               }
            for (const auto& opt : boneOptions)
               {
               bool selected = (opt.id == selectedRoot);
               if (ImGui::Selectable(opt.label.c_str(), selected))
                  {
                  selectedRoot = opt.id;
                  rootChanged = true;
                  }
               if (selected) ImGui::SetItemDefaultFocus();
               }
            ImGui::EndCombo();
            }

         std::string tipPreview = (selectedTip >= 0 && (size_t)selectedTip < boneNames.size()) ? boneNames[selectedTip] : "<Select Bone>";
         if (ImGui::BeginCombo("Chain Effector Bone", tipPreview.c_str()))
            {
            if (ImGui::Selectable("<None>", selectedTip == -1))
               {
               selectedTip = -1;
               tipChanged = true;
               }
            for (const auto& opt : boneOptions)
               {
               bool selected = (opt.id == selectedTip);
               if (ImGui::Selectable(opt.label.c_str(), selected))
                  {
                  selectedTip = opt.id;
                  tipChanged = true;
                  }
               if (selected) ImGui::SetItemDefaultFocus();
               }
            ImGui::EndCombo();
            }

         bool hintsChanged = rootChanged || tipChanged;
         bool selectionHasBoth = (selectedRoot >= 0 && selectedTip >= 0);
         bool needsAutoRebuild = (!ik.Chain.empty() ? false : selectionHasBoth);
         bool rebuildRequested = hintsChanged || needsAutoRebuild;

         if (rebuildRequested)
            {
            ik.ChainRootHint = selectedRoot;
            ik.ChainEffectorHint = selectedTip;
            if (selectionHasBoth)
               {
               std::vector<BoneId> newChain;
               if (BuildChainFromEndpoints(*skeleton, selectedRoot, selectedTip, newChain))
                  {
                  ik.SetChain(newChain);
                  }
               else
                  {
                  // keep previous chain; show warning below
                  }
               }
            else
               {
               clearChain();
               }
            }
         else
            {
            if (ik.ChainRootHint != selectedRoot || ik.ChainEffectorHint != selectedTip)
               {
               ik.ChainRootHint = selectedRoot;
               ik.ChainEffectorHint = selectedTip;
               }
            }

         bool chainMatchesSelection = selectionHasBoth && !ik.Chain.empty()
            && ik.Chain.front() == selectedRoot
            && ik.Chain.back() == selectedTip;

         ImVec4 warnColor(1.0f, 0.4f, 0.4f, 1.0f);
         if (!selectionHasBoth)
            {
            const char* msg = "<Select both root and effector bones>";
            if (selectedRoot < 0 && selectedTip < 0) msg = "Select a root and an effector bone to build the chain.";
            else if (selectedRoot < 0) msg = "Select a root bone to start the chain.";
            else msg = "Select an effector bone that is a child of the root.";
            ImGui::TextColored(warnColor, "%s", msg);
            }
         else if (!chainMatchesSelection)
            {
            ImGui::TextColored(warnColor, "Effector must be a descendant of the root bone.");
            if (!ik.Chain.empty())
               {
               ImGui::TextDisabled("Last valid chain (%zu bones):", ik.Chain.size());
               ImGui::Indent();
               for (BoneId bone : ik.Chain)
                  {
                  if (bone >= 0 && (size_t)bone < boneNames.size())
                     ImGui::BulletText("%s", boneNames[bone].c_str());
                  else
                     ImGui::BulletText("Bone %d (missing)", bone);
                  }
               ImGui::Unindent();
               }
            }
         else
            {
            ImGui::Text("Chain (%zu bones):", ik.Chain.size());
            ImGui::Indent();
            for (BoneId bone : ik.Chain)
               {
               if (bone >= 0 && (size_t)bone < boneNames.size())
                  ImGui::BulletText("%s", boneNames[bone].c_str());
               else
                  ImGui::BulletText("Bone %d (missing)", bone);
               }
            ImGui::Unindent();
            }

         ImGui::Text("Last Error: %.4f m (%d iterations)", ik.RuntimeErrorMeters, ik.RuntimeIterations);

         if (ImGui::Button("Remove IK"))
            {
            ownerData->IKs.erase(ownerData->IKs.begin() + i);
            removeIK = true;
            }
         ImGui::TreePop();
         if (removeIK)
            {
            ImGui::PopID();
            --i;
            continue;
            }
         }
      ImGui::PopID();
      }

   if (ImGui::Button("Add IK Component"))
      ownerData->IKs.emplace_back();
   }

//------------------------------------------------------------------------------
// LookAt/Aim Constraint Editor
//------------------------------------------------------------------------------
void InspectorPanel::DrawLookAtEditor(EntityID entity, EntityData* selectedData)
   {
   if (!m_Context || !selectedData) return;

   // Resolve skeleton owner (same pattern as IK)
   EntityID ownerId = entity;
   EntityData* ownerData = selectedData;
   
   // Walk up hierarchy if this entity doesn't have a skeleton
   if (!ownerData->Skeleton) {
      EntityID parent = selectedData->Parent;
      while (parent != INVALID_ENTITY_ID) {
         auto* parentData = m_Context->GetEntityData(parent);
         if (!parentData) break;
         if (parentData->Skeleton) {
            ownerData = parentData;
            ownerId = parent;
            break;
         }
         parent = parentData->Parent;
      }
   }
   
   if (!ownerData || !ownerData->Skeleton) return;

   auto menuBuilder = [&]() {
      if (ImGui::MenuItem("Add LookAt Constraint"))
         ownerData->LookAtConstraints.emplace_back();
      };

   auto section = DrawComponentSection("LookAt / Aim Constraints", false, menuBuilder);
   if (!section.bar.open) return;

   ScopedComponentContent contentScope;
   if (ownerId != entity)
      {
      std::string ownerName = GetEntityDisplayName(m_Context, ownerId);
      ImGui::TextDisabled("Editing skeleton root '%s'", ownerName.c_str());
      }

   SkeletonComponent* skeleton = ownerData->Skeleton.get();
   if (!skeleton)
      {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Skeleton data is missing.");
      return;
      }

   std::vector<std::string> boneNames;
   BuildBoneNameTable(*skeleton, boneNames);
   std::vector<BoneUIOption> boneOptions;
   BuildBoneOptions(*skeleton, boneNames, boneOptions);

   if (ownerData->LookAtConstraints.empty())
      {
      ImGui::TextDisabled("No LookAt constraints configured.");
      if (ImGui::Button("Add LookAt Constraint"))
         ownerData->LookAtConstraints.emplace_back();
      return;
      }

   for (int i = 0; i < (int)ownerData->LookAtConstraints.size(); ++i)
      {
      auto& lac = ownerData->LookAtConstraints[i];
      ImGui::PushID(i);
      std::string header = "LookAt Constraint##" + std::to_string(i);
      if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
         {
         bool removeLookAt = false;
         
         ImGui::Checkbox("Enabled", &lac.Enabled);
         
         // Mode dropdown (LookAtPosition vs MatchRotation)
         const char* modeLabels[] = { "Look At Position", "Match Rotation" };
         int modeIdx = static_cast<int>(lac.Mode);
         if (ImGui::Combo("Mode", &modeIdx, modeLabels, 2)) {
            lac.Mode = static_cast<cm::animation::lookat::LookAtMode>(modeIdx);
         }
         if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
               "Look At Position: Rotate to face target's world position (3rd person, NPCs)\n"
               "Match Rotation: Copy target's yaw/pitch rotation (FPS cameras)");
         }
         
         ImGui::SliderFloat("Weight", &lac.Weight, 0.0f, 1.0f, "%.2f");
         
         // Target entity picker
         DrawEntityReferenceField("Look Target", lac.TargetEntity, m_Context);
         
         // Axes selection
         ImGui::Text("Axes:");
         ImGui::SameLine();
         uint8_t axesBits = static_cast<uint8_t>(lac.Axes);
         bool yawEnabled = (axesBits & static_cast<uint8_t>(cm::animation::lookat::AxisMask::Yaw)) != 0;
         bool pitchEnabled = (axesBits & static_cast<uint8_t>(cm::animation::lookat::AxisMask::Pitch)) != 0;
         bool rollEnabled = (axesBits & static_cast<uint8_t>(cm::animation::lookat::AxisMask::Roll)) != 0;
         
         if (ImGui::Checkbox("Yaw##ax", &yawEnabled)) {
            if (yawEnabled) axesBits |= static_cast<uint8_t>(cm::animation::lookat::AxisMask::Yaw);
            else axesBits &= ~static_cast<uint8_t>(cm::animation::lookat::AxisMask::Yaw);
            lac.Axes = static_cast<cm::animation::lookat::AxisMask>(axesBits);
         }
         ImGui::SameLine();
         if (ImGui::Checkbox("Pitch##ax", &pitchEnabled)) {
            if (pitchEnabled) axesBits |= static_cast<uint8_t>(cm::animation::lookat::AxisMask::Pitch);
            else axesBits &= ~static_cast<uint8_t>(cm::animation::lookat::AxisMask::Pitch);
            lac.Axes = static_cast<cm::animation::lookat::AxisMask>(axesBits);
         }
         ImGui::SameLine();
         if (ImGui::Checkbox("Roll##ax", &rollEnabled)) {
            if (rollEnabled) axesBits |= static_cast<uint8_t>(cm::animation::lookat::AxisMask::Roll);
            else axesBits &= ~static_cast<uint8_t>(cm::animation::lookat::AxisMask::Roll);
            lac.Axes = static_cast<cm::animation::lookat::AxisMask>(axesBits);
         }
         
         // Space dropdown
         const char* spaceLabels[] = { "Local", "Component", "World" };
         int spaceIdx = static_cast<int>(lac.Space);
         if (ImGui::Combo("Space", &spaceIdx, spaceLabels, 3)) {
            lac.Space = static_cast<cm::animation::lookat::LookAtSpace>(spaceIdx);
         }
         
         // Distribution mode dropdown
         const char* distLabels[] = { "Equal", "Linear", "Weighted" };
         int distIdx = static_cast<int>(lac.Distribution);
         if (ImGui::Combo("Distribution", &distIdx, distLabels, 3)) {
            lac.Distribution = static_cast<cm::animation::lookat::DistributionMode>(distIdx);
            lac.InvalidateWeights();
         }
         
         // Angle limits
         ImGui::DragFloat("Max Yaw (deg)", &lac.MaxYawDeg, 1.0f, 0.0f, 180.0f, "%.1f");
         ImGui::DragFloat("Max Pitch (deg)", &lac.MaxPitchDeg, 1.0f, 0.0f, 90.0f, "%.1f");
         ImGui::DragFloat("Max Roll (deg)", &lac.MaxRollDeg, 1.0f, 0.0f, 45.0f, "%.1f");
         
         // Smoothing
         ImGui::DragFloat("Smoothing Speed", &lac.SmoothingSpeed, 0.5f, 0.0f, 50.0f, "%.1f");
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 = instant, higher = smoother/slower response");
         
         ImGui::Checkbox("Visualize", &lac.Visualize);
         
         // Bone chain display/editing
         ImGui::Separator();
         ImGui::Text("Bone Chain (%zu bones):", lac.BoneChain.size());
         
         // List current bones
         for (int b = 0; b < (int)lac.BoneChain.size(); ++b) {
            ImGui::PushID(b);
            cm::animation::lookat::BoneId boneId = lac.BoneChain[b];
            std::string boneName = (boneId >= 0 && (size_t)boneId < boneNames.size()) 
               ? boneNames[boneId] : "Unknown";
            ImGui::BulletText("%s", boneName.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("X##rem")) {
               lac.BoneChain.erase(lac.BoneChain.begin() + b);
               lac.InvalidateWeights();
               ImGui::PopID();
               --b;
               continue;
            }
            ImGui::PopID();
         }
         
         // Add bone button with dropdown
         if (ImGui::BeginCombo("Add Bone", "<Select Bone>", ImGuiComboFlags_NoPreview)) {
            for (const auto& opt : boneOptions) {
               // Skip bones already in chain
               bool alreadyInChain = false;
               for (auto bid : lac.BoneChain) {
                  if (bid == opt.id) { alreadyInChain = true; break; }
               }
               if (alreadyInChain) continue;
               
               if (ImGui::Selectable(opt.label.c_str())) {
                  lac.BoneChain.push_back(opt.id);
                  lac.InvalidateWeights();
               }
            }
            ImGui::EndCombo();
         }
         
         // Quick-add common spine bones if available
         if (ImGui::Button("Auto-Add Spine Chain")) {
            static const char* spineNames[] = { 
               "Spine", "Spine1", "Spine2", "Chest", "UpperChest",
               "mixamorig:Spine", "mixamorig:Spine1", "mixamorig:Spine2", 
               "mixamorig:Chest", "mixamorig:UpperChest"
            };
            lac.BoneChain.clear();
            for (const char* name : spineNames) {
               auto it = skeleton->BoneNameToIndex.find(name);
               if (it != skeleton->BoneNameToIndex.end()) {
                  lac.BoneChain.push_back(it->second);
               }
            }
            lac.InvalidateWeights();
         }
         ImGui::SameLine();
         if (ImGui::Button("Auto-Add Head Chain")) {
            static const char* headNames[] = { 
               "Neck", "Head",
               "mixamorig:Neck", "mixamorig:Head"
            };
            lac.BoneChain.clear();
            for (const char* name : headNames) {
               auto it = skeleton->BoneNameToIndex.find(name);
               if (it != skeleton->BoneNameToIndex.end()) {
                  lac.BoneChain.push_back(it->second);
               }
            }
            lac.InvalidateWeights();
         }
         
         ImGui::Separator();
         if (ImGui::Button("Remove LookAt")) {
            ownerData->LookAtConstraints.erase(ownerData->LookAtConstraints.begin() + i);
            removeLookAt = true;
         }
         
         ImGui::TreePop();
         if (removeLookAt) {
            ImGui::PopID();
            --i;
            continue;
         }
         }
      ImGui::PopID();
      }

   if (ImGui::Button("Add LookAt Constraint"))
      ownerData->LookAtConstraints.emplace_back();
   }

//------------------------------------------------------------------------------
// Instancer Component Editor
//------------------------------------------------------------------------------
void InspectorPanel::DrawInstancerComponent(cm::instancer::InstancerComponent& instancer, EntityID entity)
{
   ImGui::Checkbox("Enabled", &instancer.Enabled);
   
   ImGui::Separator();
   ImGui::Text("Asset References");
   
   // Mesh Asset (drag-drop for 3D model files)
   {
      ImGui::Columns(2);
      ImGui::SetColumnWidth(0, 120.0f);
      ImGui::Text("Mesh Model");
      ImGui::NextColumn();
      
      std::string displayName = instancer.MeshPath.empty() ? "None" : 
         std::filesystem::path(instancer.MeshPath).filename().string();
      ImGui::Button(displayName.c_str(), ImVec2(-FLT_MIN, 0));
      
      if (ImGui::BeginDragDropTarget()) {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
            const char* droppedPath = (const char*)payload->Data;
            if (droppedPath) {
               std::string ext = std::filesystem::path(droppedPath).extension().string();
               std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
               if (ext == ".fbx" || ext == ".glb" || ext == ".gltf" || ext == ".obj" || ext == ".meta") {
                  ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
                  if (guid.high != 0 || guid.low != 0) {
                     instancer.MeshAsset.guid = guid;
                     instancer.MeshPath = droppedPath;
                     instancer.NeedsMeshReload = true;
                     instancer.NeedsRegeneration = true;
                  }
               }
            }
         }
         ImGui::EndDragDropTarget();
      }
      if (ImGui::IsItemHovered() && !instancer.MeshPath.empty()) {
         ImGui::SetTooltip("%s", instancer.MeshPath.c_str());
      }
      ImGui::Columns(1);
   }
   
   // Prefab Asset (drag-drop for .prefab files)
   {
      ImGui::Columns(2);
      ImGui::SetColumnWidth(0, 120.0f);
      ImGui::Text("Prefab (Swap)");
      ImGui::NextColumn();
      
      std::string displayName = instancer.PrefabPath.empty() ? "None" : 
         std::filesystem::path(instancer.PrefabPath).filename().string();
      ImGui::Button(displayName.c_str(), ImVec2(-FLT_MIN, 0));
      
      if (ImGui::BeginDragDropTarget()) {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
            const char* droppedPath = (const char*)payload->Data;
            if (droppedPath) {
               std::string ext = std::filesystem::path(droppedPath).extension().string();
               std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
               if (ext == ".prefab") {
                  ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
                  if (guid.high != 0 || guid.low != 0) {
                     instancer.PrefabAsset.guid = guid;
                     instancer.PrefabPath = droppedPath;
                  }
               }
            }
         }
         ImGui::EndDragDropTarget();
      }
      if (ImGui::IsItemHovered() && !instancer.PrefabPath.empty()) {
         ImGui::SetTooltip("%s", instancer.PrefabPath.c_str());
      }
      ImGui::Columns(1);
   }
   
   // Surface Entity reference
   {
      ImGui::Columns(2);
      ImGui::SetColumnWidth(0, 120.0f);
      ImGui::Text("Surface Entity");
      ImGui::NextColumn();
      
      std::string surfName = "None (use radius)";
      if (instancer.SurfaceEntity != INVALID_ENTITY_ID) {
         if (auto* surfData = m_Context->GetEntityData(instancer.SurfaceEntity)) {
            surfName = surfData->Name;
            if (surfData->Terrain) surfName += " (Terrain)";
         }
      }
      ImGui::Button(surfName.c_str(), ImVec2(-FLT_MIN, 0));
      
      if (ImGui::BeginDragDropTarget()) {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_ID")) {
            EntityID dropped = *(EntityID*)payload->Data;
            instancer.SurfaceEntity = dropped;
            instancer.NeedsRegeneration = true;
         }
         ImGui::EndDragDropTarget();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("X##surfclear")) {
         instancer.SurfaceEntity = INVALID_ENTITY_ID;
         instancer.NeedsRegeneration = true;
      }
      ImGui::Columns(1);
   }
   
   ImGui::Separator();
   ImGui::Text("Distribution");
   
   if (ImGui::DragInt("Seed", (int*)&instancer.Distribution.Seed, 1, 0, INT_MAX)) {
      instancer.NeedsRegeneration = true;
   }
   if (ImGui::DragFloat("Density (per m²)", &instancer.Distribution.DensityPerSquareMeter, 0.001f, 0.001f, 1.0f, "%.4f")) {
      instancer.NeedsRegeneration = true;
   }
   if (ImGui::DragFloat("Min Spacing", &instancer.Distribution.MinSpacing, 0.1f, 0.1f, 100.0f)) {
      instancer.NeedsRegeneration = true;
   }
   
   ImGui::Checkbox("Use Radius Mode", &instancer.UseRadiusMode);
   if (instancer.UseRadiusMode) {
      if (ImGui::DragFloat("Radius", &instancer.DistributionRadius, 1.0f, 1.0f, 10000.0f)) {
         instancer.NeedsRegeneration = true;
      }
   } else {
      if (ImGui::DragFloat2("Area Min", &instancer.DistributionAreaMin.x, 1.0f)) {
         instancer.NeedsRegeneration = true;
      }
      if (ImGui::DragFloat2("Area Max", &instancer.DistributionAreaMax.x, 1.0f)) {
         instancer.NeedsRegeneration = true;
      }
   }
   
   if (ImGui::TreeNode("Scale Variation")) {
      ImGui::Checkbox("Non-Uniform Scale", &instancer.Distribution.NonUniformScale);
      if (instancer.Distribution.NonUniformScale) {
         if (ImGui::DragFloat3("Min Scale Vec", &instancer.Distribution.MinScaleVec.x, 0.01f, 0.01f, 10.0f)) {
            instancer.NeedsRegeneration = true;
         }
         if (ImGui::DragFloat3("Max Scale Vec", &instancer.Distribution.MaxScaleVec.x, 0.01f, 0.01f, 10.0f)) {
            instancer.NeedsRegeneration = true;
         }
      } else {
         if (ImGui::DragFloat("Min Scale", &instancer.Distribution.MinScale, 0.01f, 0.01f, 10.0f)) {
            instancer.NeedsRegeneration = true;
         }
         if (ImGui::DragFloat("Max Scale", &instancer.Distribution.MaxScale, 0.01f, 0.01f, 10.0f)) {
            instancer.NeedsRegeneration = true;
         }
      }
      ImGui::TreePop();
   }
   
   if (ImGui::TreeNode("Rotation Variation")) {
      if (ImGui::DragFloat("Yaw Variance (°)", &instancer.Distribution.YawVarianceDegrees, 1.0f, 0.0f, 360.0f)) {
         instancer.NeedsRegeneration = true;
      }
      if (ImGui::DragFloat("Pitch Variance (°)", &instancer.Distribution.PitchVarianceDegrees, 1.0f, 0.0f, 90.0f)) {
         instancer.NeedsRegeneration = true;
      }
      if (ImGui::DragFloat("Roll Variance (°)", &instancer.Distribution.RollVarianceDegrees, 1.0f, 0.0f, 90.0f)) {
         instancer.NeedsRegeneration = true;
      }
      if (ImGui::Checkbox("Align to Slope", &instancer.Distribution.AlignToSlope)) {
         instancer.NeedsRegeneration = true;
      }
      if (instancer.Distribution.AlignToSlope) {
         if (ImGui::SliderFloat("Slope Alignment", &instancer.Distribution.SlopeAlignmentFactor, 0.0f, 1.0f)) {
            instancer.NeedsRegeneration = true;
         }
      }
      ImGui::TreePop();
   }
   
   if (ImGui::TreeNode("Slope Filter")) {
      if (ImGui::DragFloat("Min Slope (°)", &instancer.Distribution.MinSlopeDegrees, 1.0f, 0.0f, 90.0f)) {
         instancer.NeedsRegeneration = true;
      }
      if (ImGui::DragFloat("Max Slope (°)", &instancer.Distribution.MaxSlopeDegrees, 1.0f, 0.0f, 90.0f)) {
         instancer.NeedsRegeneration = true;
      }
      ImGui::TreePop();
   }
   
   if (ImGui::TreeNode("Height Offset")) {
      if (ImGui::DragFloat("Offset", &instancer.Distribution.HeightOffset, 0.01f, -10.0f, 10.0f)) {
         instancer.NeedsRegeneration = true;
      }
      if (ImGui::DragFloat("Variance", &instancer.Distribution.HeightOffsetVariance, 0.01f, 0.0f, 5.0f)) {
         instancer.NeedsRegeneration = true;
      }
      ImGui::TreePop();
   }
   
   ImGui::Separator();
   ImGui::Text("LOD / Hot-Swap");
   
   ImGui::DragFloat("Swap Distance", &instancer.Swap.SwapDistance, 1.0f, 1.0f, 1000.0f);
   if (ImGui::IsItemHovered()) ImGui::SetTooltip("Distance at which instanced meshes swap to full prefabs");
   
   ImGui::DragFloat("Hysteresis", &instancer.Swap.SwapHysteresis, 0.5f, 0.0f, 50.0f);
   if (ImGui::IsItemHovered()) ImGui::SetTooltip("Dead zone to prevent rapid swapping");
   
   ImGui::DragFloat("Cull Distance", &instancer.Swap.CullDistance, 10.0f, 10.0f, 10000.0f);
   if (ImGui::IsItemHovered()) ImGui::SetTooltip("Distance beyond which instances are not rendered");
   
   ImGui::DragInt("Max Active Prefabs", (int*)&instancer.Swap.MaxActivePrefabs, 1, 1, 1000);
   if (ImGui::IsItemHovered()) ImGui::SetTooltip("Maximum number of prefabs that can be active at once");
   
   ImGui::Separator();
   ImGui::Text("Rendering");
   
   if (ImGui::Checkbox("Use Alpha Cutout", &instancer.UseAlphaCutout)) {
      instancer.NeedsMeshReload = true;  // Force shader reload
   }
   if (ImGui::IsItemHovered()) ImGui::SetTooltip("Convert alpha-blended materials to alpha cutout (discard pixels below threshold).\nFixes depth/transparency issues and improves performance.");
   
   if (instancer.UseAlphaCutout) {
      ImGui::DragFloat("Cutout Threshold", &instancer.AlphaCutoutThreshold, 0.01f, 0.0f, 1.0f);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pixels with alpha below this value are discarded");
   }
   
   ImGui::Separator();
   ImGui::Text("Debug");
   
   ImGui::ColorEdit3("Preview Color", &instancer.PreviewColor.x);
   ImGui::Checkbox("Show Debug Markers", &instancer.ShowDebugMarkers);
   ImGui::Checkbox("Show Bounds", &instancer.ShowBounds);
   
   // Material Settings for each batch/submesh
   if (instancer.CachedMesh.Valid && !instancer.CachedMesh.Batches.empty()) {
      ImGui::Separator();
      if (ImGui::TreeNode("Material Settings")) {
         for (size_t batchIdx = 0; batchIdx < instancer.CachedMesh.Batches.size(); ++batchIdx) {
            auto& batch = instancer.CachedMesh.Batches[batchIdx];
            if (!batch.BatchMaterial) continue;
            
            auto* pbrMat = dynamic_cast<PBRMaterial*>(batch.BatchMaterial.get());
            if (!pbrMat) continue;
            
            std::string batchLabel = "Submesh " + std::to_string(batchIdx);
            if (ImGui::TreeNode(batchLabel.c_str())) {
               // Metallic/Roughness
               float metallic = pbrMat->GetMetallic();
               if (ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f)) {
                  pbrMat->SetMetallic(metallic);
               }
               
               float roughness = pbrMat->GetRoughness();
               if (ImGui::SliderFloat("Roughness", &roughness, 0.0f, 1.0f)) {
                  pbrMat->SetRoughness(roughness);
               }
               
               // Normal Scale
               float normalScale = pbrMat->GetNormalScale();
               if (ImGui::SliderFloat("Normal Strength", &normalScale, 0.0f, 2.0f)) {
                  pbrMat->SetNormalScale(normalScale);
               }
               
               // Ambient Occlusion
               float ao = pbrMat->GetAmbientOcclusion();
               if (ImGui::SliderFloat("Ambient Occlusion", &ao, 0.0f, 1.0f)) {
                  pbrMat->SetAmbientOcclusion(ao);
               }
               
               // Emission
               float emissionStrength = pbrMat->GetEmissionStrength();
               if (ImGui::SliderFloat("Emission Strength", &emissionStrength, 0.0f, 10.0f)) {
                  pbrMat->SetEmissionStrength(emissionStrength);
               }
               
               glm::vec3 emissionColor = pbrMat->GetEmissionColor();
               if (ImGui::ColorEdit3("Emission Color", &emissionColor.x)) {
                  pbrMat->SetEmissionColor(emissionColor);
               }
               
               // UV Transform
               glm::vec2 uvScale = pbrMat->GetUVScale();
               glm::vec2 uvOffset = pbrMat->GetUVOffset();
               if (ImGui::DragFloat2("UV Scale", &uvScale.x, 0.01f, 0.01f, 10.0f)) {
                  pbrMat->SetUVScale(uvScale);
               }
               if (ImGui::DragFloat2("UV Offset", &uvOffset.x, 0.01f, -10.0f, 10.0f)) {
                  pbrMat->SetUVOffset(uvOffset);
               }
               
               // Color Tint
               glm::vec4 colorTint(1.0f);
               if (pbrMat->TryGetUniform("u_ColorTint", colorTint)) {
                  if (ImGui::ColorEdit4("Color Tint", &colorTint.x)) {
                     pbrMat->SetUniform("u_ColorTint", colorTint);
                  }
               }
               
               // Texture paths (read-only info)
               if (ImGui::TreeNode("Textures (Info)")) {
                  const std::string& albedoPath = pbrMat->GetAlbedoPath();
                  const std::string& normalPath = pbrMat->GetNormalPath();
                  const std::string& mrPath = pbrMat->GetMetallicRoughnessPath();
                  const std::string& aoPath = pbrMat->GetAOPath();
                  const std::string& emissionPath = pbrMat->GetEmissionPath();
                  
                  ImGui::TextDisabled("Albedo: %s", albedoPath.empty() ? "(none)" : std::filesystem::path(albedoPath).filename().string().c_str());
                  if (!albedoPath.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", albedoPath.c_str());
                  
                  ImGui::TextDisabled("Normal: %s", normalPath.empty() ? "(none)" : std::filesystem::path(normalPath).filename().string().c_str());
                  if (!normalPath.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", normalPath.c_str());
                  
                  ImGui::TextDisabled("Metal/Rough: %s", mrPath.empty() ? "(none)" : std::filesystem::path(mrPath).filename().string().c_str());
                  if (!mrPath.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", mrPath.c_str());
                  
                  ImGui::TextDisabled("AO: %s", aoPath.empty() ? "(none)" : std::filesystem::path(aoPath).filename().string().c_str());
                  if (!aoPath.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", aoPath.c_str());
                  
                  ImGui::TextDisabled("Emission: %s", emissionPath.empty() ? "(none)" : std::filesystem::path(emissionPath).filename().string().c_str());
                  if (!emissionPath.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", emissionPath.c_str());
                  
                  ImGui::TreePop();
               }
               
               // Two-sided / Alpha Blend state flags
               bool twoSided = (batch.BatchMaterial->GetStateFlags() & (BGFX_STATE_CULL_CW | BGFX_STATE_CULL_CCW)) == 0;
               if (ImGui::Checkbox("Two-Sided", &twoSided)) {
                  if (twoSided) {
                     batch.BatchMaterial->m_StateFlags &= ~(BGFX_STATE_CULL_CW | BGFX_STATE_CULL_CCW);
                  } else {
                     batch.BatchMaterial->m_StateFlags |= BGFX_STATE_CULL_CW;
                  }
               }
               
               bool alphaBlend = (batch.BatchMaterial->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0;
               if (ImGui::Checkbox("Alpha Blend", &alphaBlend)) {
                  if (alphaBlend) {
                     batch.BatchMaterial->m_StateFlags |= BGFX_STATE_BLEND_ALPHA;
                  } else {
                     batch.BatchMaterial->m_StateFlags &= ~BGFX_STATE_BLEND_ALPHA;
                  }
               }
               
               ImGui::TreePop();
            }
         }
         ImGui::TreePop();
      }
   } else if (instancer.HasValidMesh() && !instancer.CachedMesh.Valid) {
      ImGui::Separator();
      ImGui::TextDisabled("Material settings available after mesh loads");
   }
   
   ImGui::Separator();
   ImGui::Text("Statistics");
   ImGui::TextDisabled("Total Instances: %u", instancer.Stats.TotalInstances);
   ImGui::TextDisabled("Visible: %u", instancer.Stats.VisibleInstances);
   ImGui::TextDisabled("Active Prefabs: %u", instancer.Stats.ActivePrefabs);
   ImGui::TextDisabled("Culled: %u", instancer.Stats.CulledInstances);
   ImGui::TextDisabled("Instanced Draws: %u", instancer.Stats.InstancedDraws);
   ImGui::TextDisabled("Generation Time: %.2f ms", instancer.Stats.LastGenerationTimeMs);
   
   ImGui::Separator();
   if (ImGui::Button("Regenerate Instances")) {
      instancer.NeedsRegeneration = true;
   }
   ImGui::SameLine();
   if (ImGui::Button("Clear Cache")) {
      instancer.ClearCache();
   }
}

//------------------------------------------------------------------------------
// Spline Component Editor
//------------------------------------------------------------------------------
void InspectorPanel::DrawSplineComponent(SplineComponent& spline, EntityID entity)
{
   ImGui::Text("Control Points: %zu", spline.ControlPoints.size());

   if (m_SplineToolPanel && m_SelectedEntity && *m_SelectedEntity == entity) {
      bool drawActive = m_SplineToolPanel->IsDrawModeActive();
      if (ImGui::Button(drawActive ? "Stop Draw Mode" : "Start Draw Mode (B)", ImVec2(180, 24))) {
         if (drawActive)
            m_SplineToolPanel->StopDrawMode();
         else
            m_SplineToolPanel->StartDrawMode();
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Left-click to add points, drag to move, right-click to remove. Press B in viewport to toggle.");
      ImGui::Spacing();

      float pickRadius = m_SplineToolPanel->GetPointPickRadius();
      if (ImGui::SliderFloat("Point Pick Radius", &pickRadius, 0.2f, 3.0f, "%.2f")) {
         m_SplineToolPanel->SetPointPickRadius(pickRadius);
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Radius for selecting/moving control points in draw mode");
      ImGui::Spacing();
   }

   if (ImGui::SliderInt("Spline Subdivision", &spline.SplineSubdivision, 1, 16)) {
      if (auto* data = m_Context->GetEntityData(entity); data && data->Instancer)
         data->Instancer->NeedsRegeneration = true;
   }
   if (ImGui::IsItemHovered()) ImGui::SetTooltip("Subdivisions between control points for smooth curves");
   if (ImGui::Checkbox("Closed", &spline.Closed)) {
      if (auto* data = m_Context->GetEntityData(entity); data && data->Instancer)
         data->Instancer->NeedsRegeneration = true;
   }
   if (ImGui::IsItemHovered()) ImGui::SetTooltip("Connect last point to first");
   ImGui::Spacing();
   ImGui::TextDisabled("Press B when this entity is selected to enter spline draw mode.");
}

void InspectorPanel::DrawSoftbodyComponent(SoftbodyComponent& softbody, EntityID entity)
{
   EntityData* data = m_Context ? m_Context->GetEntityData(entity) : nullptr;
   if (!data) {
      return;
   }

   bool dirty = false;
   bool rebuildRuntime = false;

   if (SoftbodySystem::EnsureAuthoringData(*data) && m_Context && !m_Context->m_IsPlaying) {
      dirty = true;
   }

   std::string supportReason;
   const bool supported = SoftbodySystem::SupportsMesh(*data, &supportReason);
   const size_t vertexCount = softbody.VertexWeights.size();
   const size_t anchorCount = static_cast<size_t>(std::count(
      softbody.AnchorVertices.begin(),
      softbody.AnchorVertices.end(),
      uint8_t(1)));

   const bool paintTarget = m_SoftbodyPainter && m_SoftbodyPainter->IsTarget(entity);
   const bool paintActive = paintTarget && m_SoftbodyPainter->IsPaintModeActive();
   const bool canPaint = paintTarget && supported && !(m_Context && m_Context->m_IsPlaying);

   if (!supported) {
      ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f), "Softbody mesh unsupported");
      ImGui::TextWrapped("%s", supportReason.c_str());
      ImGui::Spacing();
   }

   ImGui::Text("Vertices: %zu", vertexCount);
   ImGui::SameLine();
   ImGui::Text("Anchors: %zu", anchorCount);
   if (m_SoftbodyPainter) {
      ImGui::SameLine();
      ImGui::Text("Selected: %zu", m_SoftbodyPainter->GetSelectedCount());
   }
   ImGui::TextDisabled("Weight 0 stays heavy, weight 1 moves freely. Anchors always pin vertices in place.");
   ImGui::Spacing();

   if (m_SoftbodyPainter) {
      ImGui::BeginDisabled(!canPaint);
      if (ImGui::Button(paintActive ? "Stop Paint Mode" : "Enter Paint Mode", ImVec2(180.0f, 24.0f))) {
         if (paintActive) {
            m_SoftbodyPainter->StopPaintMode();
         }
         else {
            m_SoftbodyPainter->StartPaintMode();
         }
      }
      ImGui::EndDisabled();

      if (!paintTarget) {
         ImGui::TextDisabled("Select this entity in the main scene viewport to paint vertices.");
      }
      else if (m_Context && m_Context->m_IsPlaying) {
         ImGui::TextDisabled("Paint mode is editor-only and turns off in Play mode.");
      }

      ImGui::Spacing();
      ImGui::BeginDisabled(!canPaint);

      int paintMode = static_cast<int>(m_SoftbodyPainter->GetPaintMode());
      const char* paintModes[] = { "Weight", "Anchor", "Select" };
      if (ImGui::Combo("Paint Mode", &paintMode, paintModes, IM_ARRAYSIZE(paintModes))) {
         m_SoftbodyPainter->SetPaintMode(static_cast<SoftbodyPainter::PaintMode>(paintMode));
      }

      float brushRadius = m_SoftbodyPainter->GetBrushRadius();
      if (ImGui::SliderFloat("Brush Radius", &brushRadius, 0.02f, 2.0f, "%.2f")) {
         m_SoftbodyPainter->SetBrushRadius(brushRadius);
      }

      if (m_SoftbodyPainter->GetPaintMode() == SoftbodyPainter::PaintMode::Weight) {
         float paintWeight = m_SoftbodyPainter->GetPaintWeight();
         if (ImGui::SliderFloat("Paint Weight", &paintWeight, 0.0f, 1.0f, "%.2f")) {
            m_SoftbodyPainter->SetPaintWeight(paintWeight);
         }
      }

      bool showAllVertices = m_SoftbodyPainter->GetShowAllVertices();
      if (ImGui::Checkbox("Show All Vertices", &showAllVertices)) {
         m_SoftbodyPainter->SetShowAllVertices(showAllVertices);
      }

      ImGui::Text("Hovered Vertex: %d", m_SoftbodyPainter->GetHoveredVertex());
      ImGui::Text("Selected Vertices: %zu", m_SoftbodyPainter->GetSelectedCount());

      if (ImGui::Button("Reset Weights")) {
         m_SoftbodyPainter->ResetWeights();
         dirty = true;
      }
      ImGui::SameLine();
      if (ImGui::Button("Clear Anchors")) {
         m_SoftbodyPainter->ClearAnchors();
         dirty = true;
      }

      if (m_SoftbodyPainter->GetSelectedCount() > 0) {
         if (ImGui::Button("Anchor Selected")) {
            m_SoftbodyPainter->SetSelectedAnchored(true);
            dirty = true;
         }
         ImGui::SameLine();
         if (ImGui::Button("Unanchor Selected")) {
            m_SoftbodyPainter->SetSelectedAnchored(false);
            dirty = true;
         }

         if (ImGui::Button("Apply Weight To Selected")) {
            m_SoftbodyPainter->SetSelectedWeight(m_SoftbodyPainter->GetPaintWeight());
            dirty = true;
         }
      }

      ImGui::EndDisabled();
      ImGui::Spacing();
   }

   if (ImGui::Checkbox("Enabled", &softbody.Enabled)) {
      dirty = true;
      rebuildRuntime = true;
   }

   int solverIterations = static_cast<int>(softbody.SolverIterations);
   if (ImGui::SliderInt("Solver Iterations", &solverIterations, 1, 32)) {
      softbody.SolverIterations = static_cast<uint32_t>(std::max(solverIterations, 1));
      dirty = true;
      rebuildRuntime = true;
   }

   if (ImGui::DragFloat("Linear Damping", &softbody.LinearDamping, 0.005f, 0.0f, 2.0f, "%.3f")) {
      softbody.LinearDamping = std::max(0.0f, softbody.LinearDamping);
      dirty = true;
      rebuildRuntime = true;
   }
   if (ImGui::DragFloat("Friction", &softbody.Friction, 0.01f, 0.0f, 2.0f, "%.2f")) {
      softbody.Friction = std::max(0.0f, softbody.Friction);
      dirty = true;
      rebuildRuntime = true;
   }
   if (ImGui::DragFloat("Restitution", &softbody.Restitution, 0.01f, 0.0f, 1.0f, "%.2f")) {
      softbody.Restitution = glm::clamp(softbody.Restitution, 0.0f, 1.0f);
      dirty = true;
      rebuildRuntime = true;
   }
   if (ImGui::DragFloat("Pressure", &softbody.Pressure, 0.01f, -5.0f, 5.0f, "%.2f")) {
      dirty = true;
      rebuildRuntime = true;
   }
   if (ImGui::DragFloat("Gravity Factor", &softbody.GravityFactor, 0.01f, -2.0f, 4.0f, "%.2f")) {
      dirty = true;
      rebuildRuntime = true;
   }
   if (ImGui::DragFloat("Vertex Radius", &softbody.VertexRadius, 0.001f, 0.0f, 0.25f, "%.3f")) {
      softbody.VertexRadius = std::max(0.0f, softbody.VertexRadius);
      dirty = true;
      rebuildRuntime = true;
   }
   if (ImGui::DragFloat("Max Linear Velocity", &softbody.MaxLinearVelocity, 0.5f, 1.0f, 500.0f, "%.1f")) {
      softbody.MaxLinearVelocity = std::max(1.0f, softbody.MaxLinearVelocity);
      dirty = true;
      rebuildRuntime = true;
   }

   int bendMode = static_cast<int>(softbody.BendMode);
   const char* bendModes[] = { "None", "Distance", "Dihedral" };
   if (ImGui::Combo("Bend Mode", &bendMode, bendModes, IM_ARRAYSIZE(bendModes))) {
      softbody.BendMode = static_cast<SoftbodyBendMode>(bendMode);
      dirty = true;
      rebuildRuntime = true;
   }

   if (ImGui::DragFloat("Edge Compliance", &softbody.EdgeCompliance, 1.0e-6f, 0.0f, 1.0f, "%.6f")) {
      softbody.EdgeCompliance = std::max(0.0f, softbody.EdgeCompliance);
      dirty = true;
      rebuildRuntime = true;
   }
   if (ImGui::DragFloat("Shear Compliance", &softbody.ShearCompliance, 1.0e-6f, 0.0f, 1.0f, "%.6f")) {
      softbody.ShearCompliance = std::max(0.0f, softbody.ShearCompliance);
      dirty = true;
      rebuildRuntime = true;
   }
   if (ImGui::DragFloat("Bend Compliance", &softbody.BendCompliance, 1.0e-6f, 0.0f, 1.0f, "%.6f")) {
      softbody.BendCompliance = std::max(0.0f, softbody.BendCompliance);
      dirty = true;
      rebuildRuntime = true;
   }

   if (ImGui::Checkbox("Enable Long Range Attachments", &softbody.EnableLongRangeAttachments)) {
      dirty = true;
      rebuildRuntime = true;
   }
   if (softbody.EnableLongRangeAttachments) {
      if (ImGui::DragFloat("LRA Max Distance", &softbody.LRAMaxDistanceMultiplier, 0.005f, 1.0f, 3.0f, "%.3f")) {
         softbody.LRAMaxDistanceMultiplier = std::max(1.0f, softbody.LRAMaxDistanceMultiplier);
         dirty = true;
         rebuildRuntime = true;
      }
   }

   if (ImGui::Checkbox("Faces Double Sided", &softbody.FacesDoubleSided)) {
      dirty = true;
      rebuildRuntime = true;
   }
   if (ImGui::SliderFloat("Weight Floor", &softbody.WeightFloor, 0.0f, 1.0f, "%.2f")) {
      dirty = true;
      rebuildRuntime = true;
   }

   const auto& layerNames = PhysicsLayers::PhysicsLayerManager::Get().GetAllLayers();
   int currentLayer = static_cast<int>(std::min<uint32_t>(softbody.PhysicsLayer, MAX_PHYSICS_LAYERS - 1));
   std::string currentLayerName =
      (currentLayer >= 0 && currentLayer < static_cast<int>(layerNames.size()))
      ? layerNames[currentLayer]
      : softbody.PhysicsLayerName;
   if (!currentLayerName.empty()) {
      if (ImGui::BeginCombo("Physics Layer", currentLayerName.c_str())) {
         for (int i = 0; i < static_cast<int>(layerNames.size()); ++i) {
            const bool selected = i == currentLayer;
            if (ImGui::Selectable(layerNames[i].c_str(), selected)) {
               softbody.PhysicsLayer = static_cast<uint32_t>(i);
               softbody.PhysicsLayerName = layerNames[i];
               dirty = true;
               rebuildRuntime = true;
            }
            if (selected) {
               ImGui::SetItemDefaultFocus();
            }
         }
         ImGui::EndCombo();
      }
   }

   if (rebuildRuntime && !softbody.BodyID.IsInvalid()) {
      SoftbodySystem::ReleaseRuntime(*data, true);
   }
   if (dirty && m_Context) {
      m_Context->MarkDirty();
   }
}

// Timeline key inspector removed (new animation panel owns it)

void InspectorPanel::ShowAnimatorStateProperties(const std::string& stateName,
   std::string& clipPath,
   float& speed,
   bool& loop,
   bool isDefault,
   std::function<void()> onMakeDefault,
   std::vector<std::pair<std::string, int>>* conditionsInt,
   std::vector<std::tuple<std::string, int, float>>* conditionsFloat)
   {
   ImGui::Separator();
   ImGui::Text("Animator State: %s", stateName.c_str());
   if (isDefault) ImGui::TextDisabled("(Default Entry)");
   else if (ImGui::Button("Make Default")) { if (onMakeDefault) onMakeDefault(); }

   // Registered animations dropdown (project-wide .anim files)
   {
      const auto& options = ui::GetAnimationAssetOptions();
      int currentIndex = -1;
      for (int i = 0; i < static_cast<int>(options.size()); ++i) {
         if (options[i].path == clipPath) {
            currentIndex = i;
            break;
         }
      }

      const char* currentLabel = currentIndex >= 0 ? options[currentIndex].name.c_str() : "<Select Clip>";
      if (ImGui::BeginCombo("Clip", currentLabel)) {
         for (int i = 0; i < static_cast<int>(options.size()); ++i) {
            bool isSel = (i == currentIndex);
            if (ImGui::Selectable(options[i].name.c_str(), isSel)) {
               clipPath = options[i].path;
               if (m_HasAnimatorBinding && m_AnimatorBinding.AssetPath) {
                  *m_AnimatorBinding.AssetPath = options[i].path;
               }
            }
            if (isSel) ImGui::SetItemDefaultFocus();
         }
         ImGui::EndCombo();
      }
   }

   // Drag-and-drop animation file onto clip path (legacy/manual)
   char buf[260];
   strncpy(buf, clipPath.c_str(), sizeof(buf)); buf[sizeof(buf) - 1] = 0;
   ImGui::InputText("Clip Path", buf, sizeof(buf));
   if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
         const char* path = (const char*)payload->Data;
         // Accept only .anim
         if (path && strstr(path, ".anim")) {
            strncpy(buf, path, sizeof(buf)); buf[sizeof(buf) - 1] = 0;
            }
         }
      ImGui::EndDragDropTarget();
      }
   clipPath = buf;

   // Unified asset path next to legacy for migration
   if (m_HasAnimatorBinding && m_AnimatorBinding.AssetPath) {
      char abuf[260];
      strncpy(abuf, m_AnimatorBinding.AssetPath->c_str(), sizeof(abuf)); abuf[sizeof(abuf) - 1] = 0;
      ImGui::InputText("Asset Path", abuf, sizeof(abuf));
      if (ImGui::BeginDragDropTarget()) {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
            const char* path = (const char*)payload->Data;
            if (path && strstr(path, ".anim")) {
               strncpy(abuf, path, sizeof(abuf)); abuf[sizeof(abuf) - 1] = 0;
               }
            }
         ImGui::EndDragDropTarget();
         }
      *m_AnimatorBinding.AssetPath = abuf;
      }

   ImGui::DragFloat("Speed", &speed, 0.01f, 0.0f, 10.0f);
   ImGui::Checkbox("Loop", &loop);

   // Optional future: render conditions
   }

void InspectorPanel::DrawComponents(EntityID entity) {
   auto* data = m_Context->GetEntityData(entity);
   if (!data) return;

   // From here on, only components

   auto& registry = ComponentDrawerRegistry::Instance();
   registry.SetDrawContext(m_Context, entity);

   if (&data->Transform) {
      auto section = DrawComponentSection("Transform", true);
      if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Transform", &data->Transform);
         }
      }

   if (data->Mesh) {
      bool removeMesh = false;
      auto section = DrawComponentSection("Mesh", false, [&]() {
         if (ImGui::MenuItem("Remove")) {
            removeMesh = true;
            }
         });
      if (removeMesh) {
         data->Mesh->mesh = nullptr;
         data->Mesh->material.reset();
         data->Mesh.reset();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity,
               cm::world::RuntimeDirtyBits::RenderBinding |
               cm::world::RuntimeDirtyBits::Bounds);
         }
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         MeshComponent* meshComp = data->Mesh.get();
         ImGui::TextDisabled("Material View");

         struct MatOpt { std::string name; std::string path; bool isBuiltIn; };
         std::vector<MatOpt> s_options;
         s_options.reserve(32);
         s_options.push_back({ "Default PBR", "<builtin:DefaultPBR>", true });
         s_options.push_back({ "Skinned PBR", "<builtin:SkinnedPBR>", true });
         s_options.push_back({ "PSX", "<builtin:PSX>", true });
         s_options.push_back({ "Skinned PSX", "<builtin:SkinnedPSX>", true });
         const auto& materialAssets = ui::GetProjectAssetEntries(ui::MakeExtensionQuery({ ".mat", ".sgmat" }));
         for (const ui::ProjectAssetEntry& asset : materialAssets) {
            const bool shaderGraphMaterial = asset.extensionLower == ".sgmat";
            s_options.push_back({
               shaderGraphMaterial ? "[SG] " + asset.name : asset.name,
               asset.absolutePath,
               false
            });
         }

         // Ensure materials array is initialized (only modify once to avoid layout recalculation)
         // Use ImGui state storage instead of static map to avoid memory leak from destroyed entities
         ImGuiID materialsInitId = ImGui::GetID((std::string("MaterialsInitialized##") + std::to_string(entity)).c_str());
         bool initialized = ImGui::GetStateStorage()->GetInt(materialsInitId, 0) != 0;
         if (!initialized && meshComp->materials.empty() && meshComp->material) {
            meshComp->materials = { meshComp->material };
            ImGui::GetStateStorage()->SetInt(materialsInitId, 1);
            }
         else if (initialized && meshComp->materials.empty() && meshComp->material) {
            // Re-initialize if materials were cleared (but only if not currently scrolling)
            ImGuiIO& io = ImGui::GetIO();
            bool isScrolling = ImGui::IsMouseDragging(ImGuiMouseButton_Left) ||
               (std::abs(io.MouseWheel) > 0.0f);
            if (!isScrolling) {
               meshComp->materials = { meshComp->material };
               }
            }

         // Use entity-specific ID for selected slot to avoid conflicts between entities
         ImGuiID slotId = ImGui::GetID((std::string("SelectedMaterialSlot##") + std::to_string(entity)).c_str());
         // Capture state storage BEFORE BeginCombo - the popup has its own storage context
         ImGuiStorage* stateStorage = ImGui::GetStateStorage();
         int selectedSlot = stateStorage->GetInt(slotId, 0);
         int maxSlots = (int)meshComp->materials.size(); if (maxSlots <= 0) maxSlots = 1;
         if (selectedSlot >= maxSlots) {
            selectedSlot = maxSlots - 1;
            stateStorage->SetInt(slotId, selectedSlot);
            }
         auto getSlotLabel = [&](int slot) -> std::string {
            std::string label = std::string("Slot ") + std::to_string(slot);
            if (slot >= 0 && slot < (int)meshComp->MaterialSlotNames.size() && !meshComp->MaterialSlotNames[slot].empty()) {
               label += " - " + meshComp->MaterialSlotNames[slot];
            }
            return label;
         };
         std::string slotLabel = getSlotLabel(selectedSlot);
         if (ImGui::BeginCombo("Material Slot", slotLabel.c_str())) {
            for (int i = 0; i < (int)meshComp->materials.size(); ++i) {
               bool sel = (i == selectedSlot);
               std::string entry = getSlotLabel(i);
               if (ImGui::Selectable(entry.c_str(), sel)) {
                  selectedSlot = i;
                  stateStorage->SetInt(slotId, selectedSlot);
                  }
               }
            ImGui::EndCombo();
            }

         if ((size_t)selectedSlot < meshComp->materials.size()) {
            std::string curLabel = (meshComp->materials[selectedSlot] ? meshComp->materials[selectedSlot]->GetName() : std::string("<none>"));
            if (ImGui::BeginCombo("Material", curLabel.c_str())) {
               for (int i = 0; i < (int)s_options.size(); ++i) {
                  bool sel = false;
                  if (ImGui::Selectable(s_options[i].name.c_str(), sel)) {
                     std::shared_ptr<Material> newMat;
                     if (s_options[i].isBuiltIn) {
                        if (s_options[i].name == "Default PBR") newMat = MaterialManager::Instance().CreateDefaultPBRMaterial();
                        else if (s_options[i].name == "Skinned PBR") newMat = MaterialManager::Instance().CreateSkinnedPBRMaterial();
                        else if (s_options[i].name == "PSX") newMat = MaterialManager::Instance().CreatePSXMaterial();
                        else if (s_options[i].name == "Skinned PSX") newMat = MaterialManager::Instance().CreateSkinnedPSXMaterial();
                        }
                     else {
                        std::string matExt = std::filesystem::path(s_options[i].path).extension().string();
                        std::transform(matExt.begin(), matExt.end(), matExt.begin(), ::tolower);
                        if (matExt == ".sgmat") {
                           shadergraph::ShaderGraphMaterialDesc sgDesc;
                           if (shadergraph::LoadShaderGraphMaterial(s_options[i].path, sgDesc)) {
                              newMat = shadergraph::ShaderGraphMaterial::CreateFromDesc(sgDesc);
                           }
                        } else {
                           MaterialAssetDesc desc; if (LoadMaterialAsset(s_options[i].path, desc)) newMat = CreateMaterialFromAsset(desc);
                        }
                        }
                     if (newMat) {
                        meshComp->materials[selectedSlot] = newMat;
                        if (selectedSlot == 0) meshComp->material = newMat;
                        }
                     }
                  }
               ImGui::EndCombo();
               }
            }

         registry.DrawComponentUI("Mesh", meshComp);

         // Alpha rendering overrides (entity-level)
         {
            bool alphaFromMaterial = false;
            if (meshComp->material) {
               alphaFromMaterial = (meshComp->material->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0;
            } else if (!meshComp->materials.empty() && meshComp->materials[0]) {
               alphaFromMaterial = (meshComp->materials[0]->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0;
            }

            int alphaMode = 0; // 0=Material, 1=Blend, 2=Cutout
            float cutoutThreshold = 0.5f;
            if (data->RenderOverrides) {
               if (data->RenderOverrides->UseAlphaCutout) alphaMode = 2;
               else if (data->RenderOverrides->AlphaBlendEnabled) alphaMode = 1;
               cutoutThreshold = data->RenderOverrides->AlphaCutoutThreshold;
            } else {
               alphaMode = alphaFromMaterial ? 1 : 0;
            }

            ImGui::Separator();
            ImGui::TextDisabled("Alpha Rendering");
            const char* modes[] = { "Material", "Blend", "Cutout" };
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::Combo("Alpha Mode", &alphaMode, modes, IM_ARRAYSIZE(modes))) {
               if (!data->RenderOverrides) {
                  data->RenderOverrides = std::make_unique<RenderOverridesComponent>();
               }
               data->RenderOverrides->AlphaBlendEnabled = (alphaMode == 1);
               data->RenderOverrides->UseAlphaCutout = (alphaMode == 2);
               if (alphaMode == 2 && data->RenderOverrides->AlphaCutoutThreshold <= 0.0f) {
                  data->RenderOverrides->AlphaCutoutThreshold = 0.5f;
               }
               if (m_Context) {
                  m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::RenderBinding);
               }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Material = use the material's alpha state. Blend/Cutout force an override.");

            if (alphaMode == 2) {
               if (ImGui::DragFloat("Cutout Threshold", &cutoutThreshold, 0.01f, 0.0f, 1.0f)) {
                  if (!data->RenderOverrides) {
                     data->RenderOverrides = std::make_unique<RenderOverridesComponent>();
                  }
                  data->RenderOverrides->AlphaCutoutThreshold = cutoutThreshold;
                  if (m_Context) {
                     m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::RenderBinding);
                  }
               }
               if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pixels with alpha below this value are discarded");
            }
         }

         std::shared_ptr<Material> baseMat = (!meshComp->materials.empty() ? meshComp->materials[0] : meshComp->material);
         if (baseMat) {
            std::string n = baseMat->GetName();
            bool isPSX = (n == "PSX" || n == "SkinnedPSX");
            
            // Check if it's a ShaderGraphMaterial
            auto sgMat = std::dynamic_pointer_cast<shadergraph::ShaderGraphMaterial>(baseMat);
            if (sgMat) {
               ImGui::Separator();
               ImGui::TextDisabled("Shader Graph Parameters");
               ImGui::Text("Graph: %s", sgMat->GetShaderGraphPath().empty() ? "(none)" : 
                   std::filesystem::path(sgMat->GetShaderGraphPath()).filename().string().c_str());
               
               auto& params = sgMat->GetParameters();
               for (auto& param : params) {
                  ImGui::PushID(param.name.c_str());
                  
                  std::string displayLabel = param.displayName.empty() ? param.name : param.displayName;
                  
                  switch (param.type) {
                     case shadergraph::ShaderValueType::Float: {
                        float val = param.value.x;
                        if (ImGui::DragFloat(displayLabel.c_str(), &val, 0.01f)) {
                           param.value.x = val;
                        }
                        break;
                     }
                     case shadergraph::ShaderValueType::Float2: {
                        glm::vec2 val(param.value.x, param.value.y);
                        if (ImGui::DragFloat2(displayLabel.c_str(), &val.x, 0.01f)) {
                           param.value.x = val.x;
                           param.value.y = val.y;
                        }
                        break;
                     }
                     case shadergraph::ShaderValueType::Float3: {
                        glm::vec3 val(param.value.x, param.value.y, param.value.z);
                        if (ImGui::DragFloat3(displayLabel.c_str(), &val.x, 0.01f)) {
                           param.value.x = val.x;
                           param.value.y = val.y;
                           param.value.z = val.z;
                        }
                        break;
                     }
                     case shadergraph::ShaderValueType::Float4: {
                        if (ImGui::DragFloat4(displayLabel.c_str(), &param.value.x, 0.01f)) {
                           // Already modified in place
                        }
                        break;
                     }
                     case shadergraph::ShaderValueType::Color3:
                     case shadergraph::ShaderValueType::Color4: {
                        bool hasAlpha = (param.type == shadergraph::ShaderValueType::Color4);
                        ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float;
                        if (hasAlpha) {
                           if (ImGui::ColorEdit4(displayLabel.c_str(), &param.value.x, flags)) {
                              // Already modified in place
                           }
                        } else {
                           glm::vec3 col(param.value.x, param.value.y, param.value.z);
                           if (ImGui::ColorEdit3(displayLabel.c_str(), &col.x, flags)) {
                              param.value.x = col.x;
                              param.value.y = col.y;
                              param.value.z = col.z;
                           }
                        }
                        break;
                     }
                     case shadergraph::ShaderValueType::Texture2D: {
                        ImGui::Text("%s", displayLabel.c_str());
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Clear")) {
                           param.texturePath.clear();
                           param.textureHandle = BGFX_INVALID_HANDLE;
                        }
                        
                        // Show current texture preview (clickable to open picker)
                        bool requestPicker = false;
                        if (bgfx::isValid(param.textureHandle)) {
                           if (ImGui::ImageButton(("##sgTex" + param.name).c_str(), 
                                                  TextureLoader::ToImGuiTextureID(param.textureHandle), 
                                                  ImVec2(64, 64))) {
                              requestPicker = true;
                           }
                        } else {
                           if (ImGui::Button("(no texture)", ImVec2(64, 64))) {
                              requestPicker = true;
                           }
                        }
                        
                        // Drag-drop support
                        if (ImGui::BeginDragDropTarget()) {
                           if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                              const char* path = (const char*)payload->Data;
                              if (path) {
                                 std::string ext = std::filesystem::path(path).extension().string();
                                 std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                                 if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") {
                                    param.texturePath = path;
                                    TextureSpecifier spec;
                                    spec.Path = path;
                                    param.textureHandle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
                                 }
                              }
                           }
                           ImGui::EndDragDropTarget();
                        }
                        
                        // Open texture picker popup
                        std::string popupId = "SGTexPicker_" + param.name;
                        if (requestPicker) {
                           ImGui::OpenPopup(popupId.c_str());
                        }
                        texturepicker::DrawTexturePickerPopup(popupId.c_str(),
                           [&param](const std::string& selectedPath) {
                              param.texturePath = selectedPath;
                              TextureSpecifier spec;
                              spec.Path = selectedPath;
                              param.textureHandle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
                           },
                           param.texturePath);
                        
                        if (!param.texturePath.empty()) {
                           ImGui::TextDisabled("%s", std::filesystem::path(param.texturePath).filename().string().c_str());
                        }
                        break;
                     }
                     default:
                        ImGui::Text("%s: (unsupported type)", displayLabel.c_str());
                        break;
                  }
                  
                  ImGui::PopID();
               }
               
               // UV Scale/Offset
               ImGui::Separator();
               glm::vec2 uvScale = sgMat->GetUVScale();
               glm::vec2 uvOffset = sgMat->GetUVOffset();
               if (ImGui::DragFloat2("UV Scale", &uvScale.x, 0.01f)) {
                  sgMat->SetUVScale(uvScale);
               }
               if (ImGui::DragFloat2("UV Offset", &uvOffset.x, 0.01f)) {
                  sgMat->SetUVOffset(uvOffset);
               }
            }
            else if (isPSX) {
               ImGui::Separator();
               ImGui::TextDisabled("PSX Overrides (Property Block)");
               if (!meshComp->UniqueMaterial) {
                  glm::vec4 pbPsx(0, 0, 0, 0);
                  auto it = meshComp->PropertyBlock.Vec4Uniforms.find("u_psxParams");
                  if (it != meshComp->PropertyBlock.Vec4Uniforms.end()) pbPsx = it->second;
                  float jitterPB = pbPsx.x;
                  float affinePB = pbPsx.y;
                  float lightInfPB = pbPsx.z;
                  bool changed = false;
                  changed |= ImGui::SliderFloat("Override Jitter (px)", &jitterPB, 0.0f, 4.0f, "%.1f");
                  changed |= ImGui::SliderFloat("Override Affine", &affinePB, 0.0f, 1.0f, "%.2f");
                  changed |= ImGui::SliderFloat("Override Light Influence", &lightInfPB, 0.0f, 1.0f, "%.2f");
                  if (changed) meshComp->PropertyBlock.SetVector("u_psxParams", glm::vec4(jitterPB, affinePB, lightInfPB, 0.0f));

                  glm::vec4 pbToon(3.0f, 1.0f, 0.0f, 0.0f);
                  auto it2 = meshComp->PropertyBlock.Vec4Uniforms.find("u_toonParams");
                  if (it2 != meshComp->PropertyBlock.Vec4Uniforms.end()) pbToon = it2->second;
                  float bandsPB = pbToon.x;
                  float bandSoftPB = pbToon.y;
                  bool changedToon = false;
                  changedToon |= ImGui::SliderFloat("Override Shadow Bands", &bandsPB, 1.0f, 8.0f, "%.0f");
                  changedToon |= ImGui::SliderFloat("Override Smoothness", &bandSoftPB, 0.0f, 1.0f, "%.2f");
                  if (changedToon) meshComp->PropertyBlock.SetVector("u_toonParams", glm::vec4(bandsPB, bandSoftPB, 0.0f, 0.0f));
                  }
               }
            }

         bool unique = meshComp->UniqueMaterial;
         if (ImGui::Checkbox("Unique Material", &unique)) {
            if (unique && !meshComp->UniqueMaterial) {
               if (!meshComp->materials.empty() && (size_t)selectedSlot < meshComp->materials.size() && meshComp->materials[selectedSlot]) {
                  auto base = meshComp->materials[selectedSlot];
                  std::shared_ptr<Material> clone = base->Clone();
                  meshComp->materials[selectedSlot] = clone;
                  if (selectedSlot == 0) meshComp->material = clone;
                  }
               }
            meshComp->UniqueMaterial = unique;
            }

         if (!meshComp->UniqueMaterial) {
            ImGui::Separator();
            std::string overrideLabel = "Material Overrides - " + getSlotLabel(selectedSlot);
            ImGui::TextDisabled("%s", overrideLabel.c_str());
            if (meshComp->SlotPropertyBlocks.size() < meshComp->materials.size()) meshComp->SlotPropertyBlocks.resize(meshComp->materials.size());
            if (meshComp->SlotPropertyBlockTexturePaths.size() < meshComp->materials.size()) meshComp->SlotPropertyBlockTexturePaths.resize(meshComp->materials.size());

            MaterialPropertyBlock& pb = (selectedSlot < (int)meshComp->SlotPropertyBlocks.size()) ? meshComp->SlotPropertyBlocks[selectedSlot] : meshComp->PropertyBlock;
            auto& paths = (selectedSlot < (int)meshComp->SlotPropertyBlockTexturePaths.size()) ? meshComp->SlotPropertyBlockTexturePaths[selectedSlot] : meshComp->PropertyBlockTexturePaths;

            auto resolveSlotMaterial = [&]() -> std::shared_ptr<Material> {
               if (selectedSlot >= 0 && selectedSlot < (int)meshComp->materials.size() && meshComp->materials[selectedSlot])
                  return meshComp->materials[selectedSlot];
               return meshComp->material;
            };

            auto castToPbr = [&]() -> std::shared_ptr<PBRMaterial> {
               if (auto mat = resolveSlotMaterial()) {
                  return std::dynamic_pointer_cast<PBRMaterial>(mat);
               }
               return nullptr;
            };

            auto clearTextureOverride = [&](const char* sampler) {
               // Property block textures are shared/cached handles. Removing the override is
               // correct here; destroying the handle can invalidate another user of the cache.
               pb.RemoveTexture(std::string(sampler));
               paths.erase(sampler);
            };

            auto loadTextureOverride = [&](const std::string& path) -> bgfx::TextureHandle {
               TextureSpecifier spec;
               spec.Path = path;
               return AcquireTextureHandle(spec, TextureColorSpace::Linear);
            };

            auto drawTextureOverride = [&](const char* label, const char* sampler) {
               ImGui::PushID(label);
               
               bgfx::TextureHandle overrideTex = BGFX_INVALID_HANDLE;
               if (auto it = pb.Textures.find(sampler); it != pb.Textures.end()) overrideTex = it->second;
               bool hasTexture = bgfx::isValid(overrideTex);

               // Header row: label and Reset button aligned
               ImGui::Text("%s", label);
               if (hasTexture) {
                  ImGui::SameLine();
                  if (ImGui::SmallButton("Reset")) {
                     clearTextureOverride(sampler);
                     overrideTex = BGFX_INVALID_HANDLE;
                     hasTexture = false;
                  }
               }

               // Texture slot row
               const float slotSize = 56.0f;
               bool requestPicker = false;
               
               if (hasTexture) {
                  ImGui::ImageButton("##texture", TextureLoader::ToImGuiTextureID(overrideTex), ImVec2(slotSize, slotSize));
                  if (ImGui::IsItemClicked()) {
                     requestPicker = true;
                  }
                  if (ImGui::BeginDragDropTarget()) {
                     if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                        const char* path = (const char*)payload->Data;
                        if (path) {
                           bgfx::TextureHandle tex = loadTextureOverride(path);
                           if (bgfx::isValid(tex)) {
                              pb.SetTexture(sampler, tex);  // Use setter to sync both maps
                              paths[sampler] = std::string(path);
                              overrideTex = tex;
                              hasTexture = true;
                           }
                        }
                     }
                     ImGui::EndDragDropTarget();
                  }
               } else {
                  // Empty slot with border styling
                  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.17f, 1.0f));
                  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.22f, 0.25f, 1.0f));
                  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                  if (ImGui::Button("Select\nTexture", ImVec2(slotSize, slotSize))) {
                     requestPicker = true;
                  }
                  ImGui::PopStyleVar();
                  ImGui::PopStyleColor(2);
               }

               if (requestPicker) {
                  ImGui::OpenPopup("TexturePicker");
               }

               std::string currentOverridePath;
               if (auto itPath = paths.find(sampler); itPath != paths.end()) currentOverridePath = itPath->second;
               texturepicker::DrawTexturePickerPopup("TexturePicker",
                  [&](const std::string& path) {
                     bgfx::TextureHandle tex = loadTextureOverride(path);
                     if (bgfx::isValid(tex)) {
                        pb.SetTexture(sampler, tex);  // Use setter to sync both maps
                        paths[sampler] = path;
                        overrideTex = tex;
                        hasTexture = true;
                     }
                  },
                  currentOverridePath);

               ImGui::Spacing();
               ImGui::PopID();
            };

            glm::vec4 tint(1.0f);
            if (auto itTint = pb.Vec4Uniforms.find("u_ColorTint"); itTint != pb.Vec4Uniforms.end()) tint = itTint->second;
            if (ImGui::ColorEdit4("Tint", &tint.x)) {
               pb.SetVector("u_ColorTint", tint);
            }
            if (ImGui::SmallButton("Reset Tint")) {
               pb.RemoveVector("u_ColorTint");
            }

            ImGui::Spacing();
            drawTextureOverride("Albedo Texture", "s_albedo");
            drawTextureOverride("Normal Map", "s_normalMap");
            drawTextureOverride("Metallic/Roughness", "s_metallicRoughness");
            drawTextureOverride("Ambient Occlusion", "s_ao");
            drawTextureOverride("Emission Map", "s_emission");
            drawTextureOverride("Displacement Map", "s_displacement");

            if (auto pbr = castToPbr()) {
               auto getPbrScalar0Defaults = [&]() {
                  return glm::vec4(
                     pbr->GetMetallic(),
                     pbr->GetRoughness(),
                     pbr->GetAmbientOcclusion(),
                     pbr->GetNormalScale());
               };
               auto getPbrScalar1Defaults = [&]() {
                  return glm::vec4(pbr->GetEmissionStrength(), 0.0f, 0.0f, 0.0f);
               };
               auto getUvDefaults = [&]() {
                  glm::vec2 scale = pbr->GetUVScale();
                  glm::vec2 offset = pbr->GetUVOffset();
                  return glm::vec4(scale, offset);
               };
               auto getEmissionColorDefaults = [&]() {
                  return glm::vec4(pbr->GetEmissionColor(), 1.0f);
               };

               auto fetchVec4 = [&](const char* key, const glm::vec4& defaults) -> glm::vec4 {
                  if (auto it = pb.Vec4Uniforms.find(key); it != pb.Vec4Uniforms.end()) return it->second;
                  return defaults;
               };

               ImGui::Separator();
               ImGui::TextDisabled("PBR Scalars");
               glm::vec4 pbrScalars = fetchVec4("u_PBRScalar0", getPbrScalar0Defaults());
               float metallic = pbrScalars.x;
               float roughness = pbrScalars.y;
               float ao = pbrScalars.z;
               float normalScale = pbrScalars.w;
               bool edited = false;
               edited |= ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f);
               edited |= ImGui::SliderFloat("Roughness", &roughness, 0.0f, 1.0f);
               edited |= ImGui::SliderFloat("Ambient Occlusion", &ao, 0.0f, 1.0f);
               edited |= ImGui::SliderFloat("Normal Scale", &normalScale, 0.0f, 4.0f);
               if (edited) {
                  pbrScalars = glm::vec4(metallic, roughness, ao, normalScale);
                  pb.SetVector("u_PBRScalar0", pbrScalars);
               }
               if (ImGui::SmallButton("Reset##PBRScalar0")) {
                  pb.RemoveVector("u_PBRScalar0");
               }

               glm::vec4 scalar1 = fetchVec4("u_PBRScalar1", getPbrScalar1Defaults());
               float emissionStrength = scalar1.x;
               if (ImGui::SliderFloat("Emission Strength", &emissionStrength, 0.0f, 20.0f)) {
                  scalar1.x = emissionStrength;
                  pb.SetVector("u_PBRScalar1", scalar1);
               }
               if (ImGui::SmallButton("Reset##PBRScalar1")) {
                  pb.RemoveVector("u_PBRScalar1");
               }

               glm::vec4 emissionColor = fetchVec4("u_EmissionColor", getEmissionColorDefaults());
               if (ImGui::ColorEdit3("Emission Color", &emissionColor.x)) {
                  emissionColor.w = 1.0f;
                  pb.SetVector("u_EmissionColor", emissionColor);
               }
               if (ImGui::SmallButton("Reset##EmissionColor")) pb.RemoveVector("u_EmissionColor");

               glm::vec4 uvTransform = fetchVec4("u_UVTransform", getUvDefaults());
               glm::vec2 uvScale = glm::vec2(uvTransform.x, uvTransform.y);
               glm::vec2 uvOffset = glm::vec2(uvTransform.z, uvTransform.w);
               bool uvChanged = false;
               uvChanged |= ImGui::DragFloat2("UV Scale", &uvScale.x, 0.01f, 0.01f, 50.0f, "%.3f");
               uvChanged |= ImGui::DragFloat2("UV Offset", &uvOffset.x, 0.01f, -10.0f, 10.0f, "%.3f");
               if (uvChanged) {
                  uvScale = glm::max(uvScale, glm::vec2(0.0001f));
                  pb.SetVector("u_UVTransform", glm::vec4(uvScale, uvOffset));
               }
               if (ImGui::SmallButton("Reset##UVTransform")) {
                  pb.RemoveVector("u_UVTransform");
               }

               ImGui::Separator();
               ImGui::TextDisabled("Shadow Overrides");
               bool shadowOverride = pb.Vec4Uniforms.find("u_shadowReceive") != pb.Vec4Uniforms.end();
               if (ImGui::Checkbox("Override Receive Shadows", &shadowOverride)) {
                  if (!shadowOverride) {
                     pb.RemoveVector("u_shadowReceive");
                  } else {
                     pb.SetVector("u_shadowReceive", glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
                  }
               }
               if (shadowOverride) {
                  glm::vec4 receiveVec = fetchVec4("u_shadowReceive", glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
                  bool receive = receiveVec.x > 0.5f;
                  if (ImGui::Checkbox("Receive Shadows", &receive)) {
                     pb.SetVector("u_shadowReceive", glm::vec4(receive ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f));
                  }
               }
            } else {
               ImGui::TextDisabled("Extended overrides available for PBR materials.");
            }
            }
         }
         
         // ==========================================================================
         // Skeleton Binding Options (for skinned meshes)
         // ==========================================================================
         if (data->Skinning) {
            ImGui::Separator();
            ImGui::TextDisabled("Skeleton Binding");
            
            bool useParentSkeleton = data->Skinning->UseParentSkeleton;
            if (ImGui::Checkbox("Use Parent Skeleton", &useParentSkeleton)) {
               data->Skinning->UseParentSkeleton = useParentSkeleton;
               data->Skinning->ResolvedSkeletonRoot = INVALID_ENTITY_ID; // Force re-resolve
               data->Skinning->InvalidateRemap(); // Force rebuild bone remap
            }
            if (ImGui::IsItemHovered()) {
               ImGui::SetTooltip("When enabled, this skinned mesh will automatically bind to\n"
                                 "the nearest skeleton found in its parent hierarchy.\n"
                                 "Useful for armor/clothing that needs to follow a character.");
            }
            
            // Show current skeleton binding
            EntityID currentSkel = INVALID_ENTITY_ID;
            if (data->Skinning->UseParentSkeleton && data->Skinning->ResolvedSkeletonRoot != INVALID_ENTITY_ID) {
               currentSkel = data->Skinning->ResolvedSkeletonRoot;
            } else if (data->Skinning->SkeletonRoot != INVALID_ENTITY_ID && data->Skinning->SkeletonRoot != (EntityID)-1) {
               currentSkel = data->Skinning->SkeletonRoot;
            }
            
            std::string skelName = "<none>";
            if (currentSkel != INVALID_ENTITY_ID) {
               skelName = GetEntityDisplayName(m_Context, currentSkel);
            }
            ImGui::Text("Bound To: %s", skelName.c_str());
            
            // Show bone remap status when using parent skeleton
            if (data->Skinning->UseParentSkeleton) {
               if (!data->Skinning->OriginalBoneNames.empty()) {
                  ImGui::TextDisabled("Original bones: %zu", data->Skinning->OriginalBoneNames.size());
                  
                  if (!data->Skinning->BoneRemap.empty()) {
                     // Count matched bones
                     int matched = 0;
                     for (int idx : data->Skinning->BoneRemap) {
                        if (idx >= 0) ++matched;
                     }
                     if (matched == (int)data->Skinning->BoneRemap.size()) {
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), 
                           "Remap: %d/%zu bones matched", matched, data->Skinning->BoneRemap.size());
                     } else {
                        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), 
                           "Remap: %d/%zu bones matched", matched, data->Skinning->BoneRemap.size());
                     }
                  } else {
                     ImGui::TextDisabled("Remap: pending...");
                  }
               } else {
                  ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.4f, 1.0f), 
                     "No original bone names stored - re-import mesh");
               }
            }
         }

         // ==========================================================================
         // Skinned Mesh Helpers: Apply culling settings to all child meshes
         // ==========================================================================
         if (!data->Children.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Skinned Mesh Helpers");
            
            // Bounds Padding for animated meshes
            static float s_BoundsPaddingValue = 2.0f;
            ImGui::SetNextItemWidth(80.0f);
            ImGui::DragFloat("##PaddingValue", &s_BoundsPaddingValue, 0.1f, 1.0f, 5.0f, "%.1f");
            ImGui::SameLine();
            if (ImGui::Button("Apply Bounds Padding to Children")) {
               std::function<void(EntityID)> applyToChildren = [&](EntityID parentId) {
                  auto* parentData = m_Context->GetEntityData(parentId);
                  if (!parentData) return;
                  for (EntityID childId : parentData->Children) {
                     auto* childData = m_Context->GetEntityData(childId);
                     if (!childData) continue;
                     if (childData->Mesh) {
                        childData->Mesh->BoundsPadding = s_BoundsPaddingValue;
                     }
                     applyToChildren(childId);
                  }
               };
               applyToChildren(entity);
            }
            if (ImGui::IsItemHovered()) {
               ImGui::SetTooltip("Apply Bounds Padding to all child mesh entities.\n"
                                 "Recommended 1.5-2.0 for skinned/animated meshes.\n"
                                 "Expands culling bounds to account for animation poses.");
            }
            
            // Skip Frustum Culling for first-person
            if (ImGui::Button("Skip Frustum Culling on Children")) {
               std::function<void(EntityID)> applyToChildren = [&](EntityID parentId) {
                  auto* parentData = m_Context->GetEntityData(parentId);
                  if (!parentData) return;
                  for (EntityID childId : parentData->Children) {
                     auto* childData = m_Context->GetEntityData(childId);
                     if (!childData) continue;
                     if (childData->Mesh) {
                        childData->Mesh->SkipFrustumCulling = true;
                     }
                     applyToChildren(childId);
                  }
               };
               applyToChildren(entity);
            }
            if (ImGui::IsItemHovered()) {
               ImGui::SetTooltip("Enable 'Skip Frustum Culling' on all child mesh entities.\n"
                                 "Use for first-person arms that must always render.");
            }
         }
      }

   // ==========================================================================
   // Armor Fit Component (for armor wrap deformation)
   // ==========================================================================
   if (data->ArmorFit) {
      bool removeArmorFit = false;
      auto section = DrawComponentSection("Armor Fit", false, [&]() {
         if (ImGui::MenuItem("Remove")) {
            removeArmorFit = true;
         }
      });
      if (removeArmorFit) {
         data->ArmorFit.reset();
      }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         
         // =======================================================================
         // Body Entity Selector (dropdown of all mesh entities in scene)
         // =======================================================================
         {
            std::string currentBodyName = GetEntityDisplayName(m_Context, data->ArmorFit->BodyEntity);
            ImGui::Text("Body Entity:");
            ImGui::SameLine();
            
            if (ImGui::BeginCombo("##BodyEntity", currentBodyName.c_str())) {
               // Option to clear
               if (ImGui::Selectable("<None>", data->ArmorFit->BodyEntity == INVALID_ENTITY_ID)) {
                  data->ArmorFit->BodyEntity = INVALID_ENTITY_ID;
               }
               
               // List all entities with mesh components (potential body meshes)
               for (const auto& ent : m_Context->GetEntities()) {
                  EntityData* entData = m_Context->GetEntityData(ent.GetID());
                  if (!entData || !entData->Mesh || !entData->Mesh->mesh)
                     continue;
                  // Skip self
                  if (ent.GetID() == entity)
                     continue;
                  
                  std::string entName = entData->Name.empty() 
                     ? ("Entity " + std::to_string(ent.GetID())) 
                     : entData->Name;
                  
                  bool isSelected = (data->ArmorFit->BodyEntity == ent.GetID());
                  if (ImGui::Selectable(entName.c_str(), isSelected)) {
                     data->ArmorFit->BodyEntity = ent.GetID();
                  }
               }
               ImGui::EndCombo();
            }
         }
         
         // Global wrap weight slider
         ImGui::SliderFloat("Wrap Weight", &data->ArmorFit->GlobalWrapWeight, 0.0f, 1.0f);
         if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Controls how much the armor follows the body surface.\n"
                              "0 = No wrap (original skinned position)\n"
                              "1 = Full wrap (armor conforms to body surface)");
         }
         
         // =======================================================================
         // Wrap Data Loading - Dropdown of available .wrap.json files
         // =======================================================================
         ImGui::Separator();
         
         // Wrap data status
         if (data->ArmorFit->WrapData && data->ArmorFit->WrapData->IsValid()) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "Wrap data: %zu vertices", 
                              data->ArmorFit->WrapData->VertexCount());
         } else {
            ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.4f, 1.0f), "No wrap data loaded");
         }
         
         // Pull available .wrap.json files from the shared asset index instead of rescanning the project.
         namespace fs = std::filesystem;
         std::vector<std::string> availableWrapFiles;
         std::string meshSidecarPath; // The expected sidecar for this mesh
         int matchingIndex = -1;      // Index of mesh's expected wrap.json
         
         // Get the expected wrap.json path for this mesh
         if (data->Mesh && (data->Mesh->meshReference.guid.high != 0 || data->Mesh->meshReference.guid.low != 0)) {
            std::string meshPath = AssetLibrary::Instance().GetPathForGUID(data->Mesh->meshReference.guid);
            if (!meshPath.empty()) {
               meshSidecarPath = editor::ArmorWrapImporter::GetSidecarPath(meshPath);
            }
         }
         
         const auto& wrapEntries = ui::GetProjectAssetEntries(ui::MakeSuffixQuery({ ".wrap.json" }));
         availableWrapFiles.reserve(wrapEntries.size());
         for (const ui::ProjectAssetEntry& entry : wrapEntries) {
            availableWrapFiles.push_back(entry.absolutePath);
         }
         
         // Find the matching index for the mesh's expected sidecar
         if (!meshSidecarPath.empty()) {
            std::string normalizedSidecar = meshSidecarPath;
            std::replace(normalizedSidecar.begin(), normalizedSidecar.end(), '\\', '/');
            for (size_t i = 0; i < availableWrapFiles.size(); ++i) {
               if (availableWrapFiles[i] == normalizedSidecar || 
                   fs::path(availableWrapFiles[i]).filename().string() == fs::path(normalizedSidecar).filename().string()) {
                  matchingIndex = static_cast<int>(i);
                  break;
               }
            }
         }
         
         // Determine current selection based on WrapBinPath
         int currentSelection = -1;
         if (!data->ArmorFit->WrapBinPath.empty()) {
            std::string currentBinPath = data->ArmorFit->WrapBinPath;
            std::replace(currentBinPath.begin(), currentBinPath.end(), '\\', '/');
            for (size_t i = 0; i < availableWrapFiles.size(); ++i) {
               std::string candidateBin = editor::ArmorWrapImporter::GetWrapBinCachePath(availableWrapFiles[i]);
               std::replace(candidateBin.begin(), candidateBin.end(), '\\', '/');
               if (!candidateBin.empty() && candidateBin == currentBinPath) {
                  currentSelection = static_cast<int>(i);
                  break;
               }
            }
         }
         
         // Build combo display text
         std::string comboPreview = "(None)";
         if (currentSelection >= 0 && currentSelection < static_cast<int>(availableWrapFiles.size())) {
            comboPreview = fs::path(availableWrapFiles[currentSelection]).stem().string();
         }
         
         ImGui::Text("Wrap Data:");
         ImGui::SameLine();
         
         // Show indicator if there's a matching sidecar for this mesh
         if (matchingIndex >= 0 && currentSelection != matchingIndex) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "(!)");
            if (ImGui::IsItemHovered()) {
               ImGui::SetTooltip("A matching .wrap.json was found for this mesh:\n%s", 
                                 availableWrapFiles[matchingIndex].c_str());
            }
            ImGui::SameLine();
         }
         
         ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 60);
         if (ImGui::BeginCombo("##WrapDataCombo", comboPreview.c_str())) {
            // Option to clear
            if (ImGui::Selectable("(None)", currentSelection < 0)) {
               data->ArmorFit->WrapBinPath.clear();
               data->ArmorFit->WrapData.reset();
               data->ArmorFit->WrapDataValidated = false;
            }
            
            ImGui::Separator();
            
            for (size_t i = 0; i < availableWrapFiles.size(); ++i) {
               bool isSelected = (static_cast<int>(i) == currentSelection);
               std::string displayName = fs::path(availableWrapFiles[i]).stem().string();
               
               // Mark the mesh's matching sidecar
               bool isMatching = (static_cast<int>(i) == matchingIndex);
               if (isMatching) {
                  displayName += "  (mesh match)";
               }
               
               if (ImGui::Selectable(displayName.c_str(), isSelected)) {
                  // User selected this wrap.json - compile and load it
                  std::string jsonPath = availableWrapFiles[i];
                  std::string binPath = editor::ArmorWrapImporter::GetWrapBinCachePath(jsonPath);
                  
                  // Get vertex counts for validation
                  uint32_t armorVertexCount = 0;
                  uint32_t bodyTriCount = 0;
                  
                  if (data->Mesh && data->Mesh->mesh) {
                     armorVertexCount = static_cast<uint32_t>(data->Mesh->mesh->Vertices.size());
                  }
                  
                  if (data->ArmorFit->BodyEntity != INVALID_ENTITY_ID) {
                     EntityData* bodyData = m_Context->GetEntityData(data->ArmorFit->BodyEntity);
                     if (bodyData && bodyData->Mesh && bodyData->Mesh->mesh) {
                        bodyTriCount = static_cast<uint32_t>(bodyData->Mesh->mesh->Indices.size() / 3);
                     }
                  }
                  
                  // Load and compile
                  auto entries = editor::ArmorWrapImporter::LoadWrapJson(jsonPath);
                  if (!entries.empty()) {
                     bool valid = true;
                     if (armorVertexCount > 0 && bodyTriCount > 0) {
                        auto validation = editor::ArmorWrapImporter::ValidateWrapData(entries, armorVertexCount, bodyTriCount);
                        valid = validation.success;
                        if (!valid) {
                           std::cerr << "[ArmorFit] Validation warning: " << validation.errorMessage << std::endl;
                           // Still allow loading - user chose this explicitly
                           valid = true;
                        }
                     }
                     
                     if (valid) {
                        auto binary = editor::ArmorWrapImporter::ConvertToBinary(entries);
                        if (editor::ArmorWrapImporter::WriteWrapBin(binPath, binary)) {
                           auto wrapData = cm::deformation::ArmorWrapLoader::Load(binPath);
                           if (wrapData && wrapData->IsValid()) {
                              data->ArmorFit->WrapData = wrapData;
                              data->ArmorFit->WrapBinPath = binPath;
                              data->ArmorFit->WrapDataValidated = true;
                              std::cout << "[ArmorFit] Loaded: " << wrapData->VertexCount() << " vertices" << std::endl;
                           }
                        }
                     }
                  } else {
                     std::cerr << "[ArmorFit] Failed to load: " << editor::ArmorWrapImporter::GetLastError() << std::endl;
                  }
               }
               if (isSelected) {
                  ImGui::SetItemDefaultFocus();
               }
            }
            ImGui::EndCombo();
         }
         
         // Reload button (useful if JSON was re-exported from Blender)
         ImGui::SameLine();
         if (ImGui::Button("Reload") && !data->ArmorFit->WrapBinPath.empty()) {
            std::string jsonPath;
            if (currentSelection >= 0 && currentSelection < static_cast<int>(availableWrapFiles.size())) {
               jsonPath = availableWrapFiles[currentSelection];
            } else if (!meshSidecarPath.empty()) {
               jsonPath = meshSidecarPath;
            }
            if (jsonPath.empty()) {
               std::cerr << "[ArmorFit] Failed to resolve wrap.json for reload." << std::endl;
            } else {
               auto entries = editor::ArmorWrapImporter::LoadWrapJson(jsonPath);
               if (!entries.empty()) {
                  auto binary = editor::ArmorWrapImporter::ConvertToBinary(entries);
                  std::string binPath = editor::ArmorWrapImporter::GetWrapBinCachePath(jsonPath);
                  if (editor::ArmorWrapImporter::WriteWrapBin(binPath, binary)) {
                     auto wrapData = cm::deformation::ArmorWrapLoader::Load(binPath);
                     if (wrapData && wrapData->IsValid()) {
                        data->ArmorFit->WrapData = wrapData;
                        data->ArmorFit->WrapBinPath = binPath;
                        data->ArmorFit->WrapDataValidated = true;
                        std::cout << "[ArmorFit] Reloaded: " << wrapData->VertexCount() << " vertices" << std::endl;
                     }
                  }
               }
            }
         }
         if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Recompile from .wrap.json and reload\n(use after re-exporting from Blender)");
         }
         
         if (availableWrapFiles.empty()) {
            ImGui::TextDisabled("No .wrap.json files found in project");
         }
         
         ImGui::Separator();
         
         // GPU/CPU toggle
         bool useGpu = data->ArmorFit->UseGPU;
         if (ImGui::Checkbox("Use GPU Compute", &useGpu)) {
            data->ArmorFit->UseGPU = useGpu;
         }
      }
   }

   // ==========================================================================
   // Bone Attachment Component (attach non-skinned mesh to skeleton bone)
   // ==========================================================================
   if (data->BoneAttachment) {
      bool removeBoneAttachment = false;
      auto section = DrawComponentSection("Bone Attachment", false, [&]() {
         if (ImGui::MenuItem("Remove")) {
            removeBoneAttachment = true;
         }
      });
      if (removeBoneAttachment) {
         data->BoneAttachment.reset();
      }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Bone Attachment", data->BoneAttachment.get());
      }
   }

   if (data->UnifiedMorph) {
      bool remove = false;
      auto section = DrawComponentSection("Unified Morphs", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         if (ImGui::MenuItem("Rebuild from Children")) {
            // Collect all child meshes with blendshapes
            std::function<void(EntityID, std::vector<EntityID>&)> collectMeshes = [&](EntityID parentId, std::vector<EntityID>& meshes) {
               auto* parentData = m_Context->GetEntityData(parentId);
               if (!parentData) return;
               for (EntityID childId : parentData->Children) {
                  auto* childData = m_Context->GetEntityData(childId);
                  if (!childData) continue;
                  if (childData->Mesh && childData->BlendShapes && !childData->BlendShapes->Shapes.empty()) {
                     meshes.push_back(childId);
                  }
                  collectMeshes(childId, meshes);
               }
            };
            
            std::vector<EntityID> meshEntities;
            collectMeshes(entity, meshEntities);
            
            // Count blendshape names
            std::unordered_map<std::string, int> nameCounts;
            for (EntityID meshId : meshEntities) {
               auto* meshData = m_Context->GetEntityData(meshId);
               if (!meshData || !meshData->BlendShapes) continue;
               for (const auto& shape : meshData->BlendShapes->Shapes) {
                  nameCounts[shape.Name]++;
               }
            }
            
            // Rebuild unified names
            data->UnifiedMorph->Names.clear();
            for (const auto& kv : nameCounts) {
               data->UnifiedMorph->Names.push_back(kv.first);
            }
            data->UnifiedMorph->Weights.assign(data->UnifiedMorph->Names.size(), 0.0f);
            data->UnifiedMorph->MemberMeshes = meshEntities;
         }
         });
      if (remove) {
         data->UnifiedMorph.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Unified Morphs", data->UnifiedMorph.get());
         }
      }

   if (data->TintController) {
      bool remove = false;
      auto section = DrawComponentSection("Tint Controller", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->TintController.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Tint Controller", data->TintController.get());
         }
      }

   if (data->Light) {
      bool remove = false;
      auto section = DrawComponentSection("Light", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->Light.reset();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity,
               cm::world::RuntimeDirtyBits::Light |
               cm::world::RuntimeDirtyBits::RenderBinding);
         }
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Light", data->Light.get());
         }
      }

   if (data->Collider) {
      bool remove = false;
      auto section = DrawComponentSection("Collider", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->Collider.reset();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Collider", data->Collider.get());
         }
      }

   // Area3D Component
   if (data->Area) {
      bool remove = false;
      auto section = DrawComponentSection("Area", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         // Destroy Jolt physics body before removing the component
         if (Physics::Get().GetAreaSystem()) {
            Physics::Get().GetAreaSystem()->OnDestroy(Entity(entity, m_Context), *data->Area);
         }
         data->Area.reset();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Area", data->Area.get());
         }
      }

   if (data->Camera) {
      bool remove = false;
      auto section = DrawComponentSection("Camera", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->Camera.reset();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Camera", data->Camera.get());
         }
      }

   if (data->RigidBody) {
      bool remove = false;
      auto section = DrawComponentSection("RigidBody", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->RigidBody.reset();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("RigidBody", data->RigidBody.get());
         }
      }

   if (data->CharacterController) {
      bool remove = false;
      auto section = DrawComponentSection("CharacterController", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->CharacterController.reset();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("CharacterController", data->CharacterController.get());
         }
      }

   if (data->GrassDeformer) {
      bool remove = false;
      auto section = DrawComponentSection("GrassDeformer", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->GrassDeformer.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         GrassDeformerComponent& deformer = *data->GrassDeformer;
         ImGui::Checkbox("Enabled", &deformer.Enabled);
         ImGui::DragFloat("Radius", &deformer.Radius, 0.01f, 0.1f, 5.0f, "%.2f m");
         ImGui::SetItemTooltip("Deformation radius in world units");
         ImGui::DragFloat("Strength", &deformer.Strength, 0.01f, 0.0f, 2.0f, "%.2f");
         ImGui::SetItemTooltip("Deformation strength multiplier");
         ImGui::DragFloat("Height Offset", &deformer.HeightOffset, 0.01f, -2.0f, 2.0f, "%.2f m");
         ImGui::SetItemTooltip("Vertical offset from entity center (use negative to place deformer at feet)");
         ImGui::Checkbox("Use Velocity", &deformer.UseVelocity);
         ImGui::SetItemTooltip("When enabled, grass bends in the direction of movement");
         }
      }

   if (data->StaticBody) {
      bool remove = false;
      auto section = DrawComponentSection("StaticBody", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->StaticBody.reset();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("StaticBody", data->StaticBody.get());
         }
      }

   if (data->Softbody) {
      bool remove = false;
      auto section = DrawComponentSection("Softbody", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         if (m_SoftbodyPainter && m_SoftbodyPainter->IsTarget(entity)) {
            m_SoftbodyPainter->StopPaintMode();
         }
         SoftbodySystem::ReleaseRuntime(*data, true);
         data->Softbody.reset();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
      }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         DrawSoftbodyComponent(*data->Softbody, entity);
      }
   }

   if (data->Terrain) {
      auto section = DrawComponentSection("Terrain", false);
      if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Terrain", data->Terrain.get());
         }
      }

   // UI Components
   if (data->Canvas) {
      bool remove = false;
      auto section = DrawComponentSection("Canvas", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->Canvas.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Canvas", data->Canvas.get());
         }
      }

   if (data->Panel) {
      bool remove = false;
      auto section = DrawComponentSection("Panel", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->Panel.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Panel", data->Panel.get());
         }
      }

   if (data->Button) {
      bool remove = false;
      auto section = DrawComponentSection("Button", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->Button.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Button", data->Button.get());
         }
      }

   if (data->Slider) {
      bool remove = false;
      auto section = DrawComponentSection("Slider", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->Slider.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Slider", data->Slider.get());
         }
      }

   if (data->ProgressBar) {
      bool remove = false;
      auto section = DrawComponentSection("Progress Bar", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->ProgressBar.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Progress Bar", data->ProgressBar.get());
         }
      }

   if (data->Toggle) {
      bool remove = false;
      auto section = DrawComponentSection("Toggle", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->Toggle.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Toggle", data->Toggle.get());
         }
      }

   if (data->ScrollView) {
      bool remove = false;
      auto section = DrawComponentSection("Scroll View", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->ScrollView.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Scroll View", data->ScrollView.get());
         }
      }

   if (data->LayoutGroup) {
      bool remove = false;
      auto section = DrawComponentSection("Layout Group", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->LayoutGroup.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Layout Group", data->LayoutGroup.get());
         }
      }

  if (data->UIRect) {
     bool remove = false;
     auto section = DrawComponentSection("UI Rect", false, [&]() {
        if (ImGui::MenuItem("Remove")) remove = true;
        });
     if (remove) {
        data->UIRect.reset();
        }
     else if (section.bar.open) {
        ScopedComponentContent contentScope;
        registry.DrawComponentUI("UI Rect", data->UIRect.get());
        }
     }

  if (data->FitToContent) {
     bool remove = false;
     auto section = DrawComponentSection("Fit To Content", false, [&]() {
        if (ImGui::MenuItem("Remove")) remove = true;
        });
     if (remove) {
        data->FitToContent.reset();
        }
     else if (section.bar.open) {
        ScopedComponentContent contentScope;
        registry.DrawComponentUI("Fit To Content", data->FitToContent.get());
        }
     }

   if (data->UISceneCapture) {
      bool remove = false;
      auto section = DrawComponentSection("UI Scene Capture", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         if (data->Panel) {
            data->Panel->UseExternalTexture = false;
            data->Panel->ExternalTextureHandle = BGFX_INVALID_HANDLE;
         }
         data->UISceneCapture.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("UI Scene Capture", data->UISceneCapture.get());
         }
      }

   if (data->InputField) {
      bool remove = false;
      auto section = DrawComponentSection("Input Field", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->InputField.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Input Field", data->InputField.get());
         }
      }

   if (data->Dropdown) {
      bool remove = false;
      auto section = DrawComponentSection("Dropdown", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->Dropdown.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Dropdown", data->Dropdown.get());
         }
      }

   // Particle System
   if (data->Emitter) {
      bool remove = false;
      auto section = DrawComponentSection("Particle Emitter", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         if (ps::isValid(data->Emitter->Handle)) {
            ecs::ParticleEmitterSystem::Get().UnregisterEmitterOwnership(data->Emitter->Handle);
            ps::destroyEmitter(data->Emitter->Handle);
            data->Emitter->Handle = { uint16_t{UINT16_MAX} };
            }
         if (!data->Emitter->SpritePath.empty() && ps::isValid(data->Emitter->SpriteHandle)) {
            particles::ReleaseSprite(data->Emitter->SpriteHandle);
            data->Emitter->SpriteHandle = { uint16_t{UINT16_MAX} };
            data->Emitter->Uniforms.m_handle = { uint16_t{UINT16_MAX} };
         }
         data->Emitter->Uniforms.reset();
         data->Emitter->Enabled = false;
         data->Emitter.reset();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("ParticleEmitter", data->Emitter.get());
         }
      }

   // Instancer (optimized instanced rendering with prefab hot-swap)
   if (data->Instancer) {
      bool remove = false;
      auto section = DrawComponentSection("Instancer", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         if (ImGui::MenuItem("Regenerate")) data->Instancer->NeedsRegeneration = true;
         if (ImGui::MenuItem("Reload Mesh")) data->Instancer->NeedsMeshReload = true;
         });
      if (remove) {
         data->Instancer.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         DrawInstancerComponent(*data->Instancer, entity);
         }
      }

   // Spline (path for scripting, instancer distribution)
   if (data->Spline) {
      bool remove = false;
      auto section = DrawComponentSection("Spline", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->Spline.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         DrawSplineComponent(*data->Spline, entity);
         }
      }

   // Text Renderer
   if (data->Text) {
      bool remove = false;
      auto section = DrawComponentSection("TextRenderer", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->Text.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("TextRenderer", data->Text.get());
         }
      }

   // Navigation components
   if (data->Navigation) {
      bool remove = false;
      auto section = DrawComponentSection("Nav Mesh", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->Navigation.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Nav Mesh", data->Navigation.get());
         }
      }

   if (data->NavAgent) {
      bool remove = false;
      auto section = DrawComponentSection("Nav Agent", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->NavAgent.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Nav Agent", data->NavAgent.get());
         }
      }

   if (data->NavLink) {
      bool remove = false;
      auto section = DrawComponentSection("Nav Link", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->NavLink.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Nav Link", data->NavLink.get());
         }
      }

   if (data->Portal) {
      bool remove = false;
      auto section = DrawComponentSection("Portal", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->Portal.reset();
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Portal", data->Portal.get());
         }
      }

   // Audio components
   if (data->AudioSource) {
      bool remove = false;
      auto section = DrawComponentSection("Audio Source", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->AudioSource.reset();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Audio Source", data->AudioSource.get());
         }
      }

   if (data->AudioListener) {
      bool remove = false;
      auto section = DrawComponentSection("Audio Listener", false, [&]() {
         if (ImGui::MenuItem("Remove")) remove = true;
         });
      if (remove) {
         data->AudioListener.reset();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }
      else if (section.bar.open) {
         ScopedComponentContent contentScope;
         registry.DrawComponentUI("Audio Listener", data->AudioListener.get());
         }
      }

   DrawIKEditor(entity, data);
   DrawLookAtEditor(entity, data);

   // Draw script components
   for (size_t i = 0; i < data->Scripts.size(); ++i) {
      DrawScriptComponent(data->Scripts[i], static_cast<int>(i), entity);
      }

   // Skeleton tools (visible whether or not Animator exists)
   if (data->Skeleton) {
      auto section = DrawComponentSection("Skeleton", false);
      if (section.bar.open) {
         ScopedComponentContent contentScope;
         ImGui::Text("Bones: %d", (int)data->Skeleton->BoneNameToIndex.size());
         if (ImGui::Button("Open Avatar Builder")) {
            if (m_AvatarBuilder) m_AvatarBuilder->OpenForEntity(entity);
            }
         }
      }

   if (data->AnimationPlayer) {
      auto section = DrawComponentSection("Animator", false);
      if (section.bar.open) {
         ScopedComponentContent contentScope;
         // Draw component UI (includes mode and single-clip controls)
         registry.DrawComponentUI("Animator", data->AnimationPlayer.get());

         // Controller file selection (for state machine animations)
         {
            ImGui::Separator();
            ImGui::TextDisabled("Controller File (for state machines)");
            ImGui::Text("Controller: %s", data->AnimationPlayer->ControllerPath.c_str());
            // Registered controller dropdown
            {
            static int selectedCtrl = -1;
            struct COpt { std::string name; std::string vfsPath; std::string absPath; };
            std::vector<COpt> s_ctrls;
            const auto& controllerEntries = ui::GetProjectAssetEntries(ui::MakeExtensionQuery({ ".animctrl" }));
            s_ctrls.reserve(controllerEntries.size());
            for (const ui::ProjectAssetEntry& entry : controllerEntries) {
               s_ctrls.push_back({ entry.name, entry.projectRelativePath, entry.absolutePath });
            }
            // Sync selection to current - compare with normalized paths
            selectedCtrl = -1;
            std::string currentPath = data->AnimationPlayer->ControllerPath;
            for (char& c : currentPath) if (c == '\\') c = '/';
            for (int i = 0; i < (int)s_ctrls.size(); ++i) {
               if (s_ctrls[i].vfsPath == currentPath) { selectedCtrl = i; break; }
               // Also try matching by filename for backwards compatibility
               if (selectedCtrl < 0) {
                  std::string ctrlFilename = std::filesystem::path(s_ctrls[i].vfsPath).filename().string();
                  std::string currentFilename = std::filesystem::path(currentPath).filename().string();
                  if (ctrlFilename == currentFilename) { selectedCtrl = i; break; }
               }
               }
            const char* cur = (selectedCtrl >= 0 ? s_ctrls[selectedCtrl].name.c_str() : "<Select Controller>");
            if (ImGui::BeginCombo("##CtrlDropdown", cur)) {
               for (int i = 0; i < (int)s_ctrls.size(); ++i) {
                  bool sel = (i == selectedCtrl);
                  if (ImGui::Selectable(s_ctrls[i].name.c_str(), sel)) {
                     selectedCtrl = i;
                     // Store VFS-relative path for serialization
                     data->AnimationPlayer->ControllerPath = s_ctrls[i].vfsPath;
                     }
                  if (ImGui::IsItemHovered()) {
                     ImGui::SetTooltip("%s", s_ctrls[i].vfsPath.c_str());
                  }
                  }
               ImGui::EndCombo();
               }
            }
            ImGui::SameLine();
            if (ImGui::Button("Set Path")) {
               // For MVP, read from clipboard
               if (const char* clip = ImGui::GetClipboardText()) {
                  std::string path = clip;
                  for (char& c : path) if (c == '\\') c = '/';
                  data->AnimationPlayer->ControllerPath = path;
                  }
               }
            if (ImGui::Button("Load Controller")) {
               // Load JSON controller file via VFS
               std::string text;
               bool loaded = false;
               // Try VFS first
               if (FileSystem::Instance().ReadTextFile(data->AnimationPlayer->ControllerPath, text)) {
                  loaded = true;
               } else {
                  // Try resolving via project directory
                  std::filesystem::path projDir = Project::GetProjectDirectory();
                  if (!projDir.empty()) {
                     std::filesystem::path fullPath = projDir / data->AnimationPlayer->ControllerPath;
                     if (FileSystem::Instance().ReadTextFile(fullPath.string(), text)) {
                        loaded = true;
                     }
                  }
               }
               if (loaded) {
                  try {
                     nlohmann::json j = nlohmann::json::parse(text);
                     auto ctrl = std::make_shared<cm::animation::AnimatorController>();
                     nlohmann::from_json(j, *ctrl);
                     data->AnimationPlayer->Controller = ctrl;
                     data->AnimationPlayer->AnimatorInstance.SetController(ctrl);
                     data->AnimationPlayer->AnimatorInstance.ResetToDefaults();
                     data->AnimationPlayer->CurrentStateId = ctrl->DefaultState;
                  } catch (const std::exception& e) {
                     std::cerr << "[Inspector] Failed to parse controller: " << e.what() << "\n";
                  }
               } else {
                  std::cerr << "[Inspector] Failed to load controller: " << data->AnimationPlayer->ControllerPath << "\n";
               }
               }
            if (ImGui::Button("Edit Controller Asset")) {
               if (m_OpenControllerCallback && !data->AnimationPlayer->ControllerPath.empty()) {
                   m_OpenControllerCallback(data->AnimationPlayer->ControllerPath);
               }
            }
            }
         }
      }

   if (m_Context && m_Context->m_IsPlaying && data->NpcScalability.Participates) {
      auto section = DrawComponentSection("NPC Scalability", false);
      if (section.bar.open) {
         ScopedComponentContent contentScope;
         const cm::npc::ScalabilityState& scalability = data->NpcScalability;
         const char* representation = "Unknown";
         switch (scalability.Representation) {
            case cm::npc::ScalabilityRepresentation::HeroCharacter:
               representation = "HeroCharacter";
               break;
            case cm::npc::ScalabilityRepresentation::BudgetedCharacter:
               representation = "BudgetedCharacter";
               break;
            case cm::npc::ScalabilityRepresentation::DormantCharacter:
               representation = "DormantCharacter";
               break;
         }

         ImGui::Text("Tier: %s", cm::npc::TierToString(scalability.Tier));
         ImGui::Text("Representation: %s", representation);
         ImGui::Text("Distance: %.2f m", scalability.CameraDistance);
         ImGui::Text("Visible: %s", scalability.VisualVisible ? "Yes" : "No");
         ImGui::Text("Shadow relevant: %s", scalability.ShadowRelevant ? "Yes" : "No");
         ImGui::Text("Player-critical: %s", scalability.PlayerOwnedCritical ? "Yes" : "No");
         ImGui::Text("Motion-critical: %s", scalability.MotionCritical ? "Yes" : "No");

         if (scalability.CrowdEligible) {
            const uint32_t crowdRank =
               scalability.CrowdRank == std::numeric_limits<uint32_t>::max()
                  ? 0u
                  : scalability.CrowdRank;
            ImGui::Text("Crowd: %u / %u%s",
               crowdRank,
               scalability.VisibleCrowdCount,
               scalability.CrowdThrottled ? " (throttled)" : "");
         }
         else {
            ImGui::Text("Crowd: not eligible");
         }

         ImGui::Text("Animation interval: %.3f s", scalability.AnimationUpdateInterval);
         ImGui::Text("Script interval: %.3f s", scalability.ScriptUpdateInterval);
         ImGui::Text("Nav repath interval: %.3f s", scalability.NavigationRepathInterval);
         ImGui::Text("State-only schedule: %s", scalability.StateOnlyWhenScheduled ? "Yes" : "No");
         const std::string reasons = cm::npc::DescribeReasonFlags(scalability.ReasonFlags);
         ImGui::Separator();
         ImGui::TextWrapped("Reasons: %s", reasons.c_str());
      }
   }

   // Dynamic components: list them after built-ins
   if (!data->Dynamic.empty()) {
      ImGui::Separator();
      ImGui::Text("Dynamic Components");
      for (auto it = data->Dynamic.begin(); it != data->Dynamic.end(); ) {
         const cm::TypeId id = it->first;
         cm::ModuleComponent& comp = it->second;
         const cm::ComponentDesc* desc = cm::ComponentRegistry::Instance().Find(id);
         std::string header = desc ? desc->fullName : std::string("<Missing Module>");
         bool remove = false;
         auto section = DrawComponentSection(header.c_str(), false, [&]() {
            if (ImGui::MenuItem("Remove Component"))
               remove = true;
            });
         if (remove) {
            it = data->Dynamic.erase(it);
            continue;
            }
         if (section.bar.open) {
            ScopedComponentContent contentScope;
            DrawDynamicComponentUI(entity, comp, desc);
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.23f, 0.23f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.30f, 0.33f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.25f, 0.25f, 1.0f));
            if (ImGui::Button("Remove Component", ImVec2(-1, 0))) {
               it = data->Dynamic.erase(it);
               ImGui::PopStyleColor(3);
               continue;
               }
            ImGui::PopStyleColor(3);
            }
         ++it;
         }
      }

   ImGui::Separator();
   ImGui::Spacing();
   ImGui::Spacing();
   DrawAddComponentButton(entity);
   }

void InspectorPanel::DrawAddComponentButton(EntityID entity) {
   static char s_filter[128] = { 0 };
   if (ImGui::Button("Add Component", ImVec2(-1, 0))) {
      m_ShowAddComponentPopup = true;
      }

   if (m_ShowAddComponentPopup) {
      ImGui::OpenPopup("Add Component");
      m_ShowAddComponentPopup = false;
      }

   if (ImGui::BeginPopup("Add Component")) {
      ImGui::SetNextItemWidth(-1);
      ImGui::InputTextWithHint("##acfilter", "Search...", s_filter, IM_ARRAYSIZE(s_filter));
      std::string filter = s_filter;
      std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
      bool hasFilter = !filter.empty();
      auto* data = m_Context->GetEntityData(entity);
      if (!data) {
         ImGui::EndPopup();
         return;
         }

      ImGui::Text("Native Components:");
      ImGui::Separator();

      // Native components
      auto showItem = [&](const char* label) { if (!hasFilter) return true; std::string l = label; std::transform(l.begin(), l.end(), l.begin(), ::tolower); return l.find(filter) != std::string::npos; };
      if (!data->Mesh && showItem("Mesh Component") && ImGui::MenuItem("Mesh Component")) {
         data->Mesh = std::make_unique<MeshComponent>();
         if (!data->RenderOverrides) data->RenderOverrides = std::make_unique<RenderOverridesComponent>();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity,
               cm::world::RuntimeDirtyBits::RenderBinding |
               cm::world::RuntimeDirtyBits::Bounds);
         }
         }

      if (!data->Light && showItem("Light Component") && ImGui::MenuItem("Light Component")) {
         data->Light = std::make_unique<LightComponent>();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity,
               cm::world::RuntimeDirtyBits::Light |
               cm::world::RuntimeDirtyBits::RenderBinding);
         }
         }

      if (!data->Collider && showItem("Collider Component") && ImGui::MenuItem("Collider Component")) {
         data->Collider = std::make_unique<ColliderComponent>();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }

      if (!data->Camera && showItem("Camera Component") && ImGui::MenuItem("Camera Component")) {
         data->Camera = std::make_unique<CameraComponent>();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }

      if (!data->RigidBody && !data->StaticBody && !data->Softbody && showItem("RigidBody Component") && ImGui::MenuItem("RigidBody Component")) {
         data->RigidBody = std::make_unique<RigidBodyComponent>();
         EnsureCollider(data->RigidBody.get(), data);

         // If the scene is currently playing, create the physics body immediately
         if (m_Context && m_Context->m_IsPlaying && data->Collider) {
            // Get world scale for shape building (to account for parent scales)
            glm::vec3 wscale(1.0f);
            glm::vec3 wpos, wskew; glm::vec4 wpersp; glm::quat wrot;
            glm::decompose(data->Transform.WorldMatrix, wscale, wrot, wpos, wskew, wpersp);
            
            // Build the collision shape with world scale and create the Jolt body
            data->Collider->BuildShape(data->Mesh && data->Mesh->mesh ? data->Mesh->mesh.get() : nullptr, glm::abs(wscale));
            m_Context->CreatePhysicsBody(entity, data->Transform, *data->Collider);
            }
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }

      if (!data->RigidBody && !data->StaticBody && !data->Softbody && showItem("StaticBody Component") && ImGui::MenuItem("StaticBody Component")) {
         data->StaticBody = std::make_unique<StaticBodyComponent>();
         EnsureCollider(data->StaticBody.get(), data);

         // If the scene is currently playing, create the physics body immediately
         if (m_Context && m_Context->m_IsPlaying && data->Collider) {
            // Get world scale for shape building (to account for parent scales)
            glm::vec3 wscale(1.0f);
            glm::vec3 wpos, wskew; glm::vec4 wpersp; glm::quat wrot;
            glm::decompose(data->Transform.WorldMatrix, wscale, wrot, wpos, wskew, wpersp);
            
            // Build the collision shape with world scale and create the Jolt body
            data->Collider->BuildShape(data->Mesh && data->Mesh->mesh ? data->Mesh->mesh.get() : nullptr, glm::abs(wscale));
            m_Context->CreatePhysicsBody(entity, data->Transform, *data->Collider);
            }
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }

      if (!data->Softbody && !data->CharacterController && showItem("Softbody Component") && ImGui::MenuItem("Softbody Component")) {
         if (data->RigidBody && !data->RigidBody->BodyID.IsInvalid()) {
            m_Context->DestroyPhysicsBody(entity);
         }
         if (data->StaticBody && !data->StaticBody->BodyID.IsInvalid()) {
            m_Context->DestroyPhysicsBody(entity);
         }
         data->RigidBody.reset();
         data->StaticBody.reset();
         data->Softbody = std::make_unique<SoftbodyComponent>();
         if (data->Mesh && data->Mesh->mesh) {
            SoftbodySystem::EnsureAuthoringData(*data);
         }
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
      }

      if (!data->Emitter && showItem("Particle Emitter Component") && ImGui::MenuItem("Particle Emitter Component")) {
         data->Emitter = std::make_unique<ParticleEmitterComponent>();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }

      // Character controller
      if (!data->CharacterController && showItem("Character Controller Component") && ImGui::MenuItem("Character Controller Component")) {
         data->CharacterController = std::make_unique<CharacterControllerComponent>();
         // Remove physics bodies to avoid conflicts
         if (data->RigidBody && !data->RigidBody->BodyID.IsInvalid()) {
            m_Context->DestroyPhysicsBody(entity);
            data->RigidBody.reset();
            }
         if (data->StaticBody && !data->StaticBody->BodyID.IsInvalid()) {
            m_Context->DestroyPhysicsBody(entity);
            data->StaticBody.reset();
            }
         if (data->Softbody) {
            SoftbodySystem::ReleaseRuntime(*data, true);
            data->Softbody.reset();
         }
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }

      // Grass Deformer (for player/NPC grass bending)
      if (!data->GrassDeformer && showItem("Grass Deformer Component") && ImGui::MenuItem("Grass Deformer Component")) {
         data->GrassDeformer = std::make_unique<GrassDeformerComponent>();
         }
      
      // Instancer (optimized instanced rendering with prefab hot-swap)
      if (!data->Instancer && showItem("Instancer Component") && ImGui::MenuItem("Instancer Component")) {
         data->Instancer = std::make_unique<cm::instancer::InstancerComponent>();
         }

      if (!data->Spline && showItem("Spline Component") && ImGui::MenuItem("Spline Component")) {
         data->Spline = std::make_unique<SplineComponent>();
         }

      // Area3D
      if (!data->Area && showItem("Area Component") && ImGui::MenuItem("Area Component")) {
         data->Area = std::make_unique<cm::physics::AreaComponent>();
         InitializeAreaComponent(data->Area.get(), data);
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }
      
      // Armor Fit (for armor wrap deformation)
      if (!data->ArmorFit && data->Mesh && data->Skinning && showItem("Armor Fit Component") && ImGui::MenuItem("Armor Fit Component")) {
         data->ArmorFit = std::make_unique<cm::deformation::ArmorFitComponent>();
         
         // Auto-detect and load wrap.json for the mesh
         if (data->Mesh->meshReference.guid.high != 0 || data->Mesh->meshReference.guid.low != 0) {
            std::string meshPath = AssetLibrary::Instance().GetPathForGUID(data->Mesh->meshReference.guid);
            if (!meshPath.empty()) {
               std::string jsonPath = editor::ArmorWrapImporter::GetSidecarPath(meshPath);
               if (editor::ArmorWrapImporter::HasWrapSidecar(meshPath)) {
                  // Found a wrap.json for this mesh - compile and load it
                  std::string wrapBinPath = editor::ArmorWrapImporter::GetWrapBinCachePath(jsonPath);
                  
                  // Get vertex counts for validation (if body entity not set, skip validation)
                  uint32_t armorVertexCount = data->Mesh->mesh ? static_cast<uint32_t>(data->Mesh->mesh->Vertices.size()) : 0;
                  
                  // Load and convert JSON
                  auto entries = editor::ArmorWrapImporter::LoadWrapJson(jsonPath);
                  if (!entries.empty()) {
                     auto binary = editor::ArmorWrapImporter::ConvertToBinary(entries);
                     if (editor::ArmorWrapImporter::WriteWrapBin(wrapBinPath, binary)) {
                        auto wrapData = cm::deformation::ArmorWrapLoader::Load(wrapBinPath);
                        if (wrapData && wrapData->IsValid()) {
                           data->ArmorFit->WrapData = wrapData;
                           data->ArmorFit->WrapBinPath = wrapBinPath;
                           data->ArmorFit->WrapDataValidated = true;
                           std::cout << "[ArmorFit] Auto-loaded wrap data: " << wrapData->VertexCount() << " vertices from " << jsonPath << std::endl;
                        }
                     }
                  }
               }
            }
         }
         }

      // Bone Attachment (for attaching non-skinned meshes to bones without parenting)
      if (!data->BoneAttachment && showItem("Bone Attachment") && ImGui::MenuItem("Bone Attachment")) {
         data->BoneAttachment = std::make_unique<BoneAttachmentComponent>();
         // The component will auto-resolve skeleton from parent hierarchy at runtime
         }

      if (!data->Text && showItem("TextRenderer Component") && ImGui::MenuItem("TextRenderer Component")) {
         data->Text = std::make_unique<TextRendererComponent>();
         // Default to screen-space for UI usage (requires Canvas parent to render)
         data->Text->WorldSpace = false;
         }

      // Navigation components
      if (!data->Navigation && showItem("Nav Mesh Component") && ImGui::MenuItem("Nav Mesh Component")) {
         data->Navigation = std::make_unique<nav::NavMeshComponent>();
         }
      if (!data->NavAgent && showItem("Nav Agent Component") && ImGui::MenuItem("Nav Agent Component")) {
         data->NavAgent = std::make_unique<nav::NavAgentComponent>();
         }
      if (!data->NavLink && showItem("Nav Link Component") && ImGui::MenuItem("Nav Link Component")) {
         data->NavLink = std::make_unique<nav::NavLinkComponent>();
         }
      if (!data->Portal && showItem("Portal Component") && ImGui::MenuItem("Portal Component")) {
         data->Portal = std::make_unique<PortalComponent>();
         }

      // IK authoring: add a new IK block on demand
      if (showItem("IK Component")) {
         EntityID ikOwnerId = entity;
         EntityData* ikOwner = ResolveIKOwner(m_Context, entity, data, ikOwnerId);
         bool canAddIK = ikOwner && ikOwner->Skeleton != nullptr;
         if (ImGui::MenuItem("IK Component", nullptr, false, canAddIK) && canAddIK) {
            ikOwner->IKs.emplace_back();
            }
         }

      // LookAt/Aim constraint: add on skeleton owner
      if (showItem("LookAt Constraint")) {
         EntityID laOwnerId = entity;
         EntityData* laOwner = ResolveIKOwner(m_Context, entity, data, laOwnerId);
         bool canAddLA = laOwner && laOwner->Skeleton != nullptr;
         if (ImGui::MenuItem("LookAt Constraint", nullptr, false, canAddLA) && canAddLA) {
            laOwner->LookAtConstraints.emplace_back();
            }
         }

      // UI components
      if (!data->Canvas && showItem("Canvas Component") && ImGui::MenuItem("Canvas Component")) {
         data->Canvas = std::make_unique<CanvasComponent>();
         }
      if (!data->Panel && showItem("Panel Component") && ImGui::MenuItem("Panel Component")) {
         data->Panel = std::make_unique<PanelComponent>();
         }
      if (!data->Button && showItem("Button Component") && ImGui::MenuItem("Button Component")) {
         data->Button = std::make_unique<ButtonComponent>();
         }
      if (!data->Slider && showItem("Slider Component") && ImGui::MenuItem("Slider Component")) {
         data->Slider = std::make_unique<SliderComponent>();
         }
      if (!data->ProgressBar && showItem("Progress Bar Component") && ImGui::MenuItem("Progress Bar Component")) {
         data->ProgressBar = std::make_unique<ProgressBarComponent>();
         }
      if (!data->Toggle && showItem("Toggle Component") && ImGui::MenuItem("Toggle Component")) {
         data->Toggle = std::make_unique<ToggleComponent>();
         }
      if (!data->ScrollView && showItem("Scroll View Component") && ImGui::MenuItem("Scroll View Component")) {
         data->ScrollView = std::make_unique<ScrollViewComponent>();
         }
      if (!data->LayoutGroup && showItem("Layout Group Component") && ImGui::MenuItem("Layout Group Component")) {
         data->LayoutGroup = std::make_unique<LayoutGroupComponent>();
         }
      if (!data->InputField && showItem("Input Field Component") && ImGui::MenuItem("Input Field Component")) {
         data->InputField = std::make_unique<InputFieldComponent>();
         }
      if (!data->Dropdown && showItem("Dropdown Component") && ImGui::MenuItem("Dropdown Component")) {
         data->Dropdown = std::make_unique<DropdownComponent>();
         }
      if (!data->UIRect && showItem("UI Rect Component") && ImGui::MenuItem("UI Rect Component")) {
         data->UIRect = std::make_unique<UIRectComponent>();
         }
      if (!data->FitToContent && showItem("Fit To Content Component") && ImGui::MenuItem("Fit To Content Component")) {
         data->FitToContent = std::make_unique<FitToContentComponent>();
         }
      if (!data->UISceneCapture && showItem("UI Scene Capture Component") && ImGui::MenuItem("UI Scene Capture Component")) {
         data->UISceneCapture = std::make_unique<UISceneCaptureComponent>();
         }

      // Audio components
      if (!data->AudioSource && showItem("Audio Source") && ImGui::MenuItem("Audio Source")) {
         data->AudioSource = std::make_unique<AudioSourceComponent>();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }
      if (!data->AudioListener && showItem("Audio Listener") && ImGui::MenuItem("Audio Listener")) {
         data->AudioListener = std::make_unique<AudioListenerComponent>();
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }

      // Tint/Material controllers
      if (!data->TintController && showItem("Tint Controller") && ImGui::MenuItem("Tint Controller")) {
         data->TintController = std::make_unique<TintMaskController>();
         }

      // Unified Morph - rebuild from children or create empty
      if (showItem("Unified Morphs") && ImGui::BeginMenu("Unified Morphs")) {
         if (ImGui::MenuItem("Rebuild from Children")) {
            // Collect all child meshes with blendshapes
            std::function<void(EntityID, std::vector<EntityID>&)> collectMeshes = [&](EntityID parentId, std::vector<EntityID>& meshes) {
               auto* parentData = m_Context->GetEntityData(parentId);
               if (!parentData) return;
               for (EntityID childId : parentData->Children) {
                  auto* childData = m_Context->GetEntityData(childId);
                  if (!childData) continue;
                  if (childData->Mesh && childData->BlendShapes && !childData->BlendShapes->Shapes.empty()) {
                     meshes.push_back(childId);
                  }
                  collectMeshes(childId, meshes);
               }
            };
            
            std::vector<EntityID> meshEntities;
            collectMeshes(entity, meshEntities);
            
            if (!meshEntities.empty()) {
               // Count blendshape names
               std::unordered_map<std::string, int> nameCounts;
               for (EntityID meshId : meshEntities) {
                  auto* meshData = m_Context->GetEntityData(meshId);
                  if (!meshData || !meshData->BlendShapes) continue;
                  for (const auto& shape : meshData->BlendShapes->Shapes) {
                     nameCounts[shape.Name]++;
                  }
               }
               
               // Build unified names list
               std::vector<std::string> unifiedNames;
               for (const auto& kv : nameCounts) {
                  unifiedNames.push_back(kv.first);
               }
               
               if (!unifiedNames.empty()) {
                  data->UnifiedMorph = std::make_unique<UnifiedMorphComponent>();
                  data->UnifiedMorph->Names = unifiedNames;
                  data->UnifiedMorph->Weights.assign(unifiedNames.size(), 0.0f);
                  data->UnifiedMorph->MemberMeshes = meshEntities;
               }
            }
         }
         if (!data->UnifiedMorph && ImGui::MenuItem("Create Empty")) {
            data->UnifiedMorph = std::make_unique<UnifiedMorphComponent>();
         }
         ImGui::EndMenu();
      }

      ImGui::Separator();
      ImGui::Text("Script Components:");
      ImGui::Separator();

      // Script components
      for (const auto& scriptName : g_RegisteredScriptNames) {
         if (hasFilter) { std::string ln = scriptName; std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower); if (ln.find(filter) == std::string::npos) continue; }
         // Check if this script is already attached
         bool alreadyAttached = false;
         for (const auto& script : data->Scripts) {
            if (script.ClassName == scriptName) {
               alreadyAttached = true;
               break;
               }
            }

         if (!alreadyAttached && ImGui::MenuItem(scriptName.c_str())) {
            ScriptInstance instance;
            instance.ClassName = scriptName;

            // Create the script instance
            auto created = ScriptSystem::Instance().Create(scriptName);
            if (created) {
               instance.Instance = created;
               data->Scripts.push_back(instance);
               if (m_Context) {
                  m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
               }

               // Only invoke OnCreate immediately when the scene is playing.
               // In edit mode, OnCreate will run when the scene is cloned for play.
               if (m_Context && m_Context->m_IsPlaying) {
                  created->OnCreate(Entity(entity, m_Context));
                  }
               }
            else {
               std::cerr << "[Inspector] Failed to create script of type '" << scriptName << "'\n";
               }
            }
         }

      ImGui::Separator();
      ImGui::Text("Dynamic Components:");
      ImGui::Separator();
      // Build a simple flat list grouped by top-level path segment
      const auto& allRef = cm::ComponentRegistry::Instance().All();
      std::vector<const cm::ComponentDesc*> all;
      all.assign(allRef.begin(), allRef.end());
      std::sort(all.begin(), all.end(), [](const cm::ComponentDesc* a, const cm::ComponentDesc* b) {
         if (a->order != b->order) return a->order < b->order;
         return a->menuPath < b->menuPath;
         });
      for (const cm::ComponentDesc* d : all) {
         if (hasFilter) { std::string ln = d->menuPath; std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower); if (ln.find(filter) == std::string::npos) continue; }
         // Skip if already attached
         if (data->Dynamic.find(d->typeId) != data->Dynamic.end()) continue;
         if (ImGui::MenuItem(d->menuPath.c_str())) {
            m_Context->AddDynamicComponent(entity, d->typeId);
            // Set version and defaults
            if (auto* comp = m_Context->GetDynamicComponent(entity, d->typeId)) {
               comp->SetVersion(d->version);
               }
            }
         }

      ImGui::EndPopup();
      }
   }

void InspectorPanel::DrawScriptComponent(const ScriptInstance& script, int index, EntityID entity) {
   std::string headerName = script.ClassName + "##" + std::to_string(index);
   bool remove = false;
   auto section = DrawComponentSection(headerName.c_str(), false, [&]() {
      if (ImGui::MenuItem("Remove Script")) remove = true;
      });
   if (remove) {
      auto* data = m_Context->GetEntityData(entity);
      if (data && index >= 0 && index < static_cast<int>(data->Scripts.size())) {
         data->Scripts.erase(data->Scripts.begin() + index);
         if (m_Context) {
            m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
         }
         }
      return;
      }

   if (section.bar.open) {
      ScopedComponentContent contentScope;
      ImGui::PushID(index);

      ImGui::Text("Script Type: %s", script.ClassName.c_str());

      if (ImGui::Button("Remove Script")) {
         auto* data = m_Context->GetEntityData(entity);
         if (data && index >= 0 && index < static_cast<int>(data->Scripts.size())) {
            data->Scripts.erase(data->Scripts.begin() + index);
            if (m_Context) {
               m_Context->NotifyComponentChanged(entity, cm::world::RuntimeDirtyBits::Metadata);
            }
            ImGui::PopID();
            return;
            }
         }

      // Draw script properties using reflection
      if (ScriptReflection::HasProperties(script.ClassName)) {
         auto& properties = ScriptReflection::GetScriptProperties(script.ClassName);
         void* scriptHandle = nullptr;
         if (script.Instance && script.Instance->GetBackend() == ScriptBackend::Managed) {
            if (auto managed = std::dynamic_pointer_cast<ManagedScriptComponent>(script.Instance))
               scriptHandle = managed->GetHandle();
            }
         for (auto& property : properties) {
            // When scene is playing, read current value from runtime script instance
            // This allows inspector to reflect runtime changes (e.g., state machine enum changes)
            // Falls back to serialized data if runtime read fails or scene not playing
            bool readFromRuntime = false;
            
            // Only read from runtime in editor when scene is playing (inspector-only feature)
            // Runtime builds don't have GetManagedFieldPtr set up, so this code path is skipped
            if (m_Context && m_Context->m_IsPlaying && scriptHandle && GetManagedFieldPtr) {
               // Try to read from runtime instance
               int propTypeInt = static_cast<int>(property.type);
               
               // Handle all string-based types (including lists which serialize as pipe-separated strings)
               if (property.type == PropertyType::String || 
                   property.type == PropertyType::Prefab ||
                   property.type == PropertyType::ClayObject ||
                   property.type == PropertyType::Mesh ||
                   property.type == PropertyType::Texture ||
                   property.type == PropertyType::Audio ||
                   property.type == PropertyType::DialogueLibrary ||
                   property.type == PropertyType::AnimatorController ||
                   property.type == PropertyType::AnimatorControllerOverride ||
                   property.type == PropertyType::List ||
                   property.type == PropertyType::Struct ||
                   property.type == PropertyType::Dictionary) {
                  // String/list types return a pointer that needs to be freed
                  // C# writes IntPtr to boxedOut using Marshal.WriteIntPtr(boxedOut, strPtr)
                  // So boxedOut is IntPtr* (pointer to IntPtr), which is void** in C++
                  void* allocatedPtr = nullptr; // Will receive the IntPtr value
                  bool success = GetManagedFieldPtr(scriptHandle, property.name.c_str(), propTypeInt, &allocatedPtr);
                  if (success && allocatedPtr) {
                     const char* strPtr = reinterpret_cast<const char*>(allocatedPtr);
                     if (property.type == PropertyType::List) {
                        // Parse pipe-separated list string
                        std::string listStr(strPtr);
                        property.currentValue = ScriptReflection::StringToPropertyValue(listStr, property.type);
                        NormalizeListPropertyElements(property);
                        // Free memory
                        #ifdef _WIN32
                        LocalFree(allocatedPtr);
                        #else
                        free(allocatedPtr);
                        #endif
                        readFromRuntime = true;
                     } else if (property.type == PropertyType::Struct || property.type == PropertyType::Dictionary) {
                        // Struct/Dictionary serialization not fully implemented in GetManagedField
                        // Free the empty string and fall back to serialized data
                        #ifdef _WIN32
                        LocalFree(allocatedPtr);
                        #else
                        free(allocatedPtr);
                        #endif
                        // readFromRuntime stays false, will use fallback below
                     } else {
                        // String-based types (String, Prefab, ClayObject, Mesh, Texture, Audio, DialogueLibrary, AnimatorController, AnimatorControllerOverride)
                        property.currentValue = std::string(strPtr);
                        // Free memory
                        #ifdef _WIN32
                        LocalFree(allocatedPtr);
                        #else
                        free(allocatedPtr);
                        #endif
                        readFromRuntime = true;
                     }
                  }
               } else {
                  // Simple value types - use aligned buffer
                  alignas(8) char boxedBuffer[64]; // Large enough for int, float, bool, vec3
                  void* boxedPtr = boxedBuffer;
                  bool success = GetManagedFieldPtr(scriptHandle, property.name.c_str(), propTypeInt, boxedPtr);
                  if (success) {
                     property.currentValue = ScriptReflection::BoxToValue(boxedPtr, property.type);
                     readFromRuntime = true;
                  }
               }
            }
            
            // Fall back to serialized data if runtime read failed or scene not playing
            if (!readFromRuntime) {
               auto* data = m_Context ? m_Context->GetEntityData(entity) : nullptr;
               const ScriptInstance* stored = nullptr;
               if (data) {
                  for (const auto& s : data->Scripts) { if (s.ClassName == script.ClassName) { stored = &s; break; } }
                  }
               if (stored) {
                  auto it = stored->Values.find(property.name);
                  if (it != stored->Values.end()) property.currentValue = it->second;
                  }
               }
            DrawScriptProperty(property, scriptHandle, script.ClassName, entity);
            // After drawing, if a modification happened the DrawScriptProperty will write back via SetManagedField
            // and we'll persist below when updated is true.
            }
         }
      else {
         ImGui::Text("No exposed properties");
         ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "Add [SerializeField] attributes to C# properties to expose them here");
         }

      ImGui::PopID();
      }
   }

void InspectorPanel::DrawScriptProperty(PropertyInfo& property, void* scriptHandle, const std::string& className, EntityID entityID) {
   ImGui::PushID(property.name.c_str());
   bool updated = false;
   const std::string pretty = PrettifyLabel(property.name);

   switch (property.type) {
      case PropertyType::Int: {
      int value = std::get<int>(property.currentValue);
      if (ImGui::DragInt(pretty.c_str(), &value)) {
         property.currentValue = value;
         if (property.setter) {
            property.setter(value);
            }
         updated = true;
         }
      break;
      }

      case PropertyType::Float: {
      float value = std::get<float>(property.currentValue);
      if (ImGui::DragFloat(pretty.c_str(), &value, 0.1f)) {
         property.currentValue = value;
         if (property.setter) {
            property.setter(value);
            }
         updated = true;
         }
      break;
      }

      case PropertyType::Bool: {
      bool value = std::get<bool>(property.currentValue);
      if (ImGui::Checkbox(pretty.c_str(), &value)) {
         property.currentValue = value;
         if (property.setter) {
            property.setter(value);
            }
         updated = true;
         }
      break;
      }

      case PropertyType::String: {
      std::string value = std::get<std::string>(property.currentValue);
      char buffer[256];
      strncpy(buffer, value.c_str(), sizeof(buffer) - 1);
      buffer[sizeof(buffer) - 1] = '\0';

      if (ImGui::InputText(pretty.c_str(), buffer, sizeof(buffer))) {
         property.currentValue = std::string(buffer);
         if (property.setter) {
            property.setter(std::string(buffer));
            }
         updated = true;
         }
      break;
      }

      case PropertyType::Vector3: {
      glm::vec3 value = std::get<glm::vec3>(property.currentValue);
      if (DrawVec3Control(pretty.c_str(), value)) {
         property.currentValue = value;
         if (property.setter) {
            property.setter(value);
            }
         updated = true;
         }
      break;
      }

      case PropertyType::Entity: {
      int entityId = std::get<int>(property.currentValue);
      const char* btnLabel = "None";
      if (entityId != -1) {
         if (auto* entData = m_Context->GetEntityData(entityId))
            btnLabel = entData->Name.c_str();
         }
      ImGui::Columns(2);
      ImGui::SetColumnWidth(0, 120.0f);
      ImGui::Text("%s", pretty.c_str());
      ImGui::NextColumn();
      float avail = ImGui::GetContentRegionAvail().x;
      float btnW = 80.0f;
      float smallF = 24.0f;
      ImGui::SetNextItemWidth(avail - (smallF * 2 + 8.0f + 8.0f));
      ImGui::Button(btnLabel, ImVec2(-FLT_MIN, 0));
      // Right-click context menu for Clear
      if (ImGui::BeginPopupContextItem()) {
         if (ImGui::MenuItem("Clear")) {
            property.currentValue = -1;
            if (property.setter) property.setter(PropertyValue{ -1 });
            updated = true;
            }
         ImGui::EndPopup();
         }
      // Accept ENTITY_ID drops directly on the primary object field button
      if (ImGui::BeginDragDropTarget()) {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_ID")) {
            EntityID dropped = *(EntityID*)payload->Data;
            property.currentValue = static_cast<int>(dropped);
            if (property.setter) property.setter(PropertyValue{ static_cast<int>(dropped) });
            updated = true;
            }
         // Reject prefab drops (ASSET_FILE) here
         (void)ImGui::AcceptDragDropPayload("ASSET_FILE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
         ImGui::EndDragDropTarget();
         }
      ImGui::SameLine();
      if (ImGui::SmallButton("Ping")) {
         if (entityId != -1) { if (auto* ent = m_Context->GetEntityData(entityId)) { if (m_SelectedEntity) *m_SelectedEntity = entityId; } }
         }
      ImGui::SameLine();
      if (ImGui::SmallButton("X")) {
         property.currentValue = -1;
         if (property.setter) property.setter(PropertyValue{ -1 });
         updated = true;
         }
      if (ImGui::BeginDragDropTarget()) {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_ID")) {
            EntityID dropped = *(EntityID*)payload->Data;
            property.currentValue = static_cast<int>(dropped);
            if (property.setter) property.setter(PropertyValue{ static_cast<int>(dropped) });
            updated = true;
            }
         // Reject prefab drops for Entity fields to avoid implicit instantiation here
         (void)ImGui::AcceptDragDropPayload("ASSET_FILE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
         ImGui::EndDragDropTarget();
         }
      ImGui::Columns(1);
      break;
      }
      case PropertyType::ComponentRef:
      case PropertyType::ScriptRef: {
      // Display as object field that accepts ENTITY_ID, validates by component/script presence
      int entityId = std::get<int>(property.currentValue);
      const char* btnLabel = "None";
      if (entityId != -1) {
         if (auto* entData = m_Context->GetEntityData(entityId))
            btnLabel = entData->Name.c_str();
         }
      ImGui::Columns(2);
      ImGui::SetColumnWidth(0, 120.0f);
      ImGui::Text("%s", pretty.c_str());
      ImGui::NextColumn();
      float avail = ImGui::GetContentRegionAvail().x;
      float smallF = 24.0f;
      ImGui::SetNextItemWidth(avail - (smallF * 2 + 8.0f + 8.0f));
      ImGui::Button(btnLabel, ImVec2(-FLT_MIN, 0));
      // Right-click context menu for Clear
      if (ImGui::BeginPopupContextItem()) {
         if (ImGui::MenuItem("Clear")) {
            property.currentValue = -1;
            if (property.setter) property.setter(PropertyValue{ -1 });
            updated = true;
            }
         ImGui::EndPopup();
         }
      // Accept drops on the main object field button
      if (ImGui::BeginDragDropTarget()) {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_ID")) {
            EntityID dropped = *(EntityID*)payload->Data;
            bool accept = false;
            if (property.type == PropertyType::ComponentRef) {
               // Validate component by auxTypeName (managed full name). Fall back to short name.
               std::string full = property.auxTypeName;
               std::string shortName = full;
               size_t pos = shortName.rfind('.'); if (pos != std::string::npos) shortName = shortName.substr(pos + 1);
               if (m_Context) {
                  auto* d = m_Context->GetEntityData((int)dropped);
                  if (d) {
                     accept = HasComponent((int)dropped, full.c_str());
                     if (!accept) accept = HasComponent((int)dropped, shortName.c_str());
                     }
                  }
               }
            else {
               // ScriptRef: entity must have a script of given class name
               if (m_Context) {
                  auto* d = m_Context->GetEntityData((int)dropped);
                  if (d) {
                     for (const auto& s : d->Scripts) { if (s.ClassName == property.auxTypeName) { accept = true; break; } }
                     }
                  }
               }
            if (accept) {
               property.currentValue = static_cast<int>(dropped);
               if (property.setter) property.setter(PropertyValue{ static_cast<int>(dropped) });
               updated = true;
               }
            }
         ImGui::EndDragDropTarget();
         }
      ImGui::SameLine();
      if (ImGui::SmallButton("Ping")) { if (entityId != -1) { if (m_SelectedEntity) *m_SelectedEntity = entityId; } }
      ImGui::SameLine();
      if (ImGui::SmallButton("X")) { property.currentValue = -1; if (property.setter) property.setter(PropertyValue{ -1 }); updated = true; }
      if (ImGui::BeginDragDropTarget()) {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_ID")) {
            EntityID dropped = *(EntityID*)payload->Data;
            bool accept = false;
            if (property.type == PropertyType::ComponentRef) {
               // Validate component by auxTypeName (managed full name). Fall back to short name.
               std::string full = property.auxTypeName;
               std::string shortName = full;
               size_t pos = shortName.rfind('.'); if (pos != std::string::npos) shortName = shortName.substr(pos + 1);
               if (m_Context) {
                  auto* d = m_Context->GetEntityData((int)dropped);
                  if (d) {
                     accept = HasComponent((int)dropped, full.c_str());
                     if (!accept) accept = HasComponent((int)dropped, shortName.c_str());
                     }
                  }
               }
            else {
               // ScriptRef: entity must have a script of given class name
               // We only have native knowledge of script list names on the entity data
               if (m_Context) {
                  auto* d = m_Context->GetEntityData((int)dropped);
                  if (d) {
                     for (const auto& s : d->Scripts) { if (s.ClassName == property.auxTypeName) { accept = true; break; } }
                     }
                  }
               }
            if (accept) {
               property.currentValue = static_cast<int>(dropped);
               if (property.setter) property.setter(PropertyValue{ static_cast<int>(dropped) });
               updated = true;
               }
            }
         ImGui::EndDragDropTarget();
         }
      ImGui::Columns(1);
      break;
      }
      case PropertyType::Prefab: {
      // Display prefab reference field - similar to Entity but accepts .prefab files
      std::string guid = std::get<std::string>(property.currentValue);
      std::string displayName = "None";
      
      // Look up display name from asset library if we have a valid GUID
      if (!guid.empty()) {
         ClaymoreGUID cmGuid = ClaymoreGUID::FromString(guid);
         std::string path = AssetLibrary::Instance().GetPathForGUID(cmGuid);
         if (!path.empty()) {
            displayName = std::filesystem::path(path).stem().string();
         } else {
            displayName = "(Missing: " + guid.substr(0, 8) + "...)";
         }
      }
      
      ImGui::Columns(2);
      ImGui::SetColumnWidth(0, 120.0f);
      ImGui::Text("%s", pretty.c_str());
      ImGui::NextColumn();
      float avail = ImGui::GetContentRegionAvail().x;
      float smallF = 24.0f;
      ImGui::SetNextItemWidth(avail - (smallF + 8.0f));
      ImGui::Button(displayName.c_str(), ImVec2(-FLT_MIN, 0));
      // Right-click context menu for Clear
      if (ImGui::BeginPopupContextItem()) {
         if (ImGui::MenuItem("Clear")) {
            property.currentValue = std::string();
            if (property.setter) property.setter(PropertyValue{ std::string() });
            updated = true;
            }
         ImGui::EndPopup();
         }
      
      // Accept ASSET_FILE drops for .prefab files
      if (ImGui::BeginDragDropTarget()) {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
            const char* droppedPath = (const char*)payload->Data;
            if (droppedPath) {
               std::string ext = std::filesystem::path(droppedPath).extension().string();
               std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
               if (ext == ".prefab") {
                  // Convert path to GUID
                  ClaymoreGUID newGuid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
                  if (newGuid.high != 0 || newGuid.low != 0) {
                     std::string guidStr = newGuid.ToString();
                     property.currentValue = guidStr;
                     if (property.setter) property.setter(PropertyValue{ guidStr });
                     updated = true;
                  }
               }
            }
         }
         ImGui::EndDragDropTarget();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("X")) {
         property.currentValue = std::string();
         if (property.setter) property.setter(PropertyValue{ std::string() });
         updated = true;
      }
      ImGui::Columns(1);
      break;
      }

      case PropertyType::AnimatorController: {
      // Display animator controller reference field - accepts .animctrl files
      std::string path = std::get<std::string>(property.currentValue);
      std::string displayName = "None";

      if (!path.empty()) {
         if (FileSystem::Instance().Exists(path)) {
            displayName = std::filesystem::path(path).stem().string();
         } else {
            displayName = "(Missing: " + std::filesystem::path(path).filename().string() + ")";
         }
      }

      ImGui::Columns(2);
      ImGui::SetColumnWidth(0, 120.0f);
      ImGui::Text("%s", pretty.c_str());
      ImGui::NextColumn();
      float avail = ImGui::GetContentRegionAvail().x;
      float smallF = 24.0f;
      ImGui::SetNextItemWidth(avail - (smallF + 8.0f));
      ImGui::Button(displayName.c_str(), ImVec2(-FLT_MIN, 0));
      // Right-click context menu for Clear
      if (ImGui::BeginPopupContextItem()) {
         if (ImGui::MenuItem("Clear")) {
            property.currentValue = std::string();
            if (property.setter) property.setter(PropertyValue{ std::string() });
            updated = true;
            }
         ImGui::EndPopup();
         }

      // Accept ASSET_FILE drops for .animctrl files
      if (ImGui::BeginDragDropTarget()) {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
            const char* droppedPath = (const char*)payload->Data;
            if (droppedPath) {
               std::string ext = std::filesystem::path(droppedPath).extension().string();
               std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
               if (ext == ".animctrl") {
                  std::string vfsPath = ToVfsAssetPath(droppedPath);
                  property.currentValue = vfsPath;
                  if (property.setter) property.setter(PropertyValue{ vfsPath });
                  updated = true;
               }
            }
         }
         ImGui::EndDragDropTarget();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("X")) {
         property.currentValue = std::string();
         if (property.setter) property.setter(PropertyValue{ std::string() });
         updated = true;
      }
      ImGui::Columns(1);
      break;
      }

      case PropertyType::AnimatorControllerOverride: {
      std::string path = std::get<std::string>(property.currentValue);
      std::string displayName = "None";

      if (!path.empty()) {
         if (FileSystem::Instance().Exists(path)) {
            displayName = std::filesystem::path(path).stem().string();
         } else {
            displayName = "(Missing: " + std::filesystem::path(path).filename().string() + ")";
         }
      }

      ImGui::Columns(2);
      ImGui::SetColumnWidth(0, 120.0f);
      ImGui::Text("%s", pretty.c_str());
      ImGui::NextColumn();
      float avail = ImGui::GetContentRegionAvail().x;
      float smallF = 24.0f;
      ImGui::SetNextItemWidth(avail - (smallF + 8.0f));
      ImGui::Button(displayName.c_str(), ImVec2(-FLT_MIN, 0));
      if (ImGui::BeginPopupContextItem()) {
         if (ImGui::MenuItem("Clear")) {
            property.currentValue = std::string();
            if (property.setter) property.setter(PropertyValue{ std::string() });
            updated = true;
            }
         ImGui::EndPopup();
         }

      if (ImGui::BeginDragDropTarget()) {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
            const char* droppedPath = (const char*)payload->Data;
            if (droppedPath) {
               std::string ext = std::filesystem::path(droppedPath).extension().string();
               std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
               if (ext == ".animoverride") {
                  std::string vfsPath = ToVfsAssetPath(droppedPath);
                  property.currentValue = vfsPath;
                  if (property.setter) property.setter(PropertyValue{ vfsPath });
                  updated = true;
               }
            }
         }
         ImGui::EndDragDropTarget();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("X")) {
         property.currentValue = std::string();
         if (property.setter) property.setter(PropertyValue{ std::string() });
         updated = true;
      }
      ImGui::Columns(1);
      break;
      }
      
      case PropertyType::Enum: {
      // Enum dropdown with names from metadata
      int currentVal = std::get<int>(property.currentValue);
      int selectedIdx = 0;
      
      // Find the index of current value in enum values
      for (size_t i = 0; i < property.enumMeta.values.size(); ++i) {
         if (property.enumMeta.values[i] == currentVal) {
            selectedIdx = static_cast<int>(i);
            break;
         }
      }
      
      // Build combo preview string
      std::string previewStr = (selectedIdx < (int)property.enumMeta.names.size()) 
                               ? property.enumMeta.names[selectedIdx] : "Unknown";
      
      ImGui::Columns(2);
      ImGui::SetColumnWidth(0, 120.0f);
      ImGui::Text("%s", pretty.c_str());
      ImGui::NextColumn();
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::BeginCombo(("##" + property.name).c_str(), previewStr.c_str())) {
         for (size_t i = 0; i < property.enumMeta.names.size(); ++i) {
            bool isSelected = (i == (size_t)selectedIdx);
            if (ImGui::Selectable(property.enumMeta.names[i].c_str(), isSelected)) {
               property.currentValue = property.enumMeta.values[i];
               if (property.setter) property.setter(PropertyValue{ property.enumMeta.values[i] });
               updated = true;
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
         }
         ImGui::EndCombo();
      }
      ImGui::Columns(1);
      break;
      }
      
      case PropertyType::List: {
      // Check if this field is auto-populated from resources
      if (property.populateFromResources) {
         // Show read-only indicator for auto-populated resource lists
         ImGui::PushID(property.name.c_str());
         
         // Get resource count from ResourceManifest if available
         size_t resourceCount = 0;
         if (!property.listElementTypeName.empty()) {
            auto resources = ResourceManifest::Get().GetResourcesByType(property.listElementTypeName);
            resourceCount = resources.size();
         }
         
         ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 0.4f, 0.3f, 1.0f));
         ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.45f, 0.35f, 1.0f));
         bool open = ImGui::TreeNodeEx("##resource_list_header", 
            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth,
            "%s  %zu (from Resources)", pretty.c_str(), resourceCount);
         ImGui::PopStyleColor(2);
         
         if (open) {
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 0.6f, 1.0f), 
               "Auto-populated from resources/ folder");
            ImGui::TextDisabled("Type: %s", property.listElementTypeName.c_str());
            
            // List the resource names
            if (!property.listElementTypeName.empty()) {
               auto resources = ResourceManifest::Get().GetResourcesByType(property.listElementTypeName);
               for (size_t i = 0; i < resources.size(); ++i) {
                  ImGui::TextDisabled("  [%zu] %s", i, resources[i]->name.c_str());
               }
            }
            
            ImGui::TreePop();
         }
         ImGui::PopID();
         break;
      }
      
      // Unity-style expandable list with +/- buttons at bottom
      NormalizeListPropertyElements(property);
      auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(property.currentValue);
      if (!listPtr) {
         listPtr = std::make_shared<ListPropertyValue>();
         property.currentValue = listPtr;
      }
      
      const size_t count = listPtr->elements.size();
      const float availWidth = ImGui::GetContentRegionAvail().x;
      
      // Use stable ID that doesn't change when count changes (prevents collapse on add)
      ImGui::PushID(property.name.c_str());
      
      // Header row with label and count badge
      bool open = ImGui::TreeNodeEx("##list_header", 
         ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth,
         "%s  %zu", pretty.c_str(), count);
      
      if (open) {
         int toRemove = -1;
         const float indentWidth = 8.0f;
         const float labelWidth = 70.0f;
         const float removeButtonWidth = 20.0f;
         const float fieldWidth = availWidth - indentWidth - labelWidth - removeButtonWidth - 24.0f;
         
         // Draw elements
         for (size_t i = 0; i < listPtr->elements.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            
            // Element row layout: [Label] [Field-----------------] [X]
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Element %zu", i);
            ImGui::SameLine(indentWidth + labelWidth);
            ImGui::SetNextItemWidth(fieldWidth);
            
            // Draw element based on type
            switch (property.listElementType) {
               case PropertyType::Int: {
                  int val = std::get<int>(listPtr->elements[i]);
                  if (ImGui::DragInt("##val", &val)) {
                     listPtr->elements[i] = val;
                     updated = true;
                  }
                  break;
               }
               case PropertyType::Float: {
                  float val = std::get<float>(listPtr->elements[i]);
                  if (ImGui::DragFloat("##val", &val, 0.1f)) {
                     listPtr->elements[i] = val;
                     updated = true;
                  }
                  break;
               }
               case PropertyType::Bool: {
                  bool val = std::get<bool>(listPtr->elements[i]);
                  if (ImGui::Checkbox("##val", &val)) {
                     listPtr->elements[i] = val;
                     updated = true;
                  }
                  break;
               }
               case PropertyType::String: {
                  std::string val = std::get<std::string>(listPtr->elements[i]);
                  char buf[256];
                  strncpy(buf, val.c_str(), sizeof(buf) - 1);
                  buf[sizeof(buf) - 1] = '\0';
                  if (ImGui::InputText("##val", buf, sizeof(buf))) {
                     listPtr->elements[i] = std::string(buf);
                     updated = true;
                  }
                  break;
               }
               case PropertyType::Vector3: {
                  glm::vec3 val = std::get<glm::vec3>(listPtr->elements[i]);
                  if (ImGui::DragFloat3("##val", &val.x, 0.1f)) {
                     listPtr->elements[i] = val;
                     updated = true;
                  }
                  break;
               }
               case PropertyType::Enum: {
                  // List of enums uses parent property's enum metadata
                  int val = std::get<int>(listPtr->elements[i]);
                  int selIdx = 0;
                  for (size_t j = 0; j < property.enumMeta.values.size(); ++j) {
                     if (property.enumMeta.values[j] == val) { selIdx = (int)j; break; }
                  }
                  std::string preview = (selIdx < (int)property.enumMeta.names.size()) 
                                        ? property.enumMeta.names[selIdx] : "Unknown";
                  if (ImGui::BeginCombo("##val", preview.c_str())) {
                     for (size_t j = 0; j < property.enumMeta.names.size(); ++j) {
                        if (ImGui::Selectable(property.enumMeta.names[j].c_str(), j == (size_t)selIdx)) {
                           listPtr->elements[i] = property.enumMeta.values[j];
                           updated = true;
                        }
                     }
                     ImGui::EndCombo();
                  }
                  break;
               }
               case PropertyType::Entity: {
                  // Entity reference in list - droppable button that accepts ENTITY_ID drops
                  int entityId = -1;
                  if (std::holds_alternative<int>(listPtr->elements[i])) {
                     entityId = std::get<int>(listPtr->elements[i]);
                  } else {
                     // Wrong type stored - fix it
                     listPtr->elements[i] = -1;
                  }
                  
                  const char* displayName = "None";
                  if (entityId != -1) {
                     if (auto* entData = m_Context->GetEntityData(entityId)) {
                        displayName = entData->Name.c_str();
                     } else {
                        displayName = "(Missing)";
                     }
                  }
                  
                  ImGui::Button(displayName, ImVec2(fieldWidth - 28.0f, 0));
                  // Right-click context menu for Clear
                  if (ImGui::BeginPopupContextItem()) {
                     if (ImGui::MenuItem("Clear")) {
                        listPtr->elements[i] = -1;
                        updated = true;
                        }
                     ImGui::EndPopup();
                     }
                  
                  // Accept ENTITY_ID drops from hierarchy
                  if (ImGui::BeginDragDropTarget()) {
                     if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_ID")) {
                        EntityID dropped = *(EntityID*)payload->Data;
                        listPtr->elements[i] = static_cast<int>(dropped);
                        updated = true;
                     }
                     ImGui::EndDragDropTarget();
                  }
                  
                  ImGui::SameLine(0, 2);
                  if (ImGui::SmallButton("x##clear")) {
                     listPtr->elements[i] = -1;
                     updated = true;
                  }
                  break;
               }
               case PropertyType::ClayObject: {
                  // ClayScriptableObject reference in list - droppable button
                  // Safely get the GUID string (element might be wrong type from deserialization)
                  std::string guid;
                  if (std::holds_alternative<std::string>(listPtr->elements[i])) {
                     guid = std::get<std::string>(listPtr->elements[i]);
                  } else {
                     // Wrong type stored - fix it
                     listPtr->elements[i] = std::string();
                     guid = "";
                  }
                  std::string displayName = "None";
                  
                  if (!guid.empty()) {
                     ClaymoreGUID cmGuid = ClaymoreGUID::FromString(guid);
                     std::string path = AssetLibrary::Instance().GetPathForGUID(cmGuid);
                     if (!path.empty()) {
                        displayName = std::filesystem::path(path).stem().string();
                     } else {
                        displayName = "(Missing)";
                     }
                  }
                  
                  ImGui::Button(displayName.c_str(), ImVec2(fieldWidth - 28.0f, 0));
                  // Right-click context menu for Clear
                  if (ImGui::BeginPopupContextItem()) {
                     if (ImGui::MenuItem("Clear")) {
                        listPtr->elements[i] = std::string();
                        updated = true;
                        }
                     ImGui::EndPopup();
                     }
                  
                  // Accept ASSET_FILE drops for .clayobj files
                  if (ImGui::BeginDragDropTarget()) {
                     if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                        const char* droppedPath = (const char*)payload->Data;
                        if (droppedPath) {
                           std::string ext = std::filesystem::path(droppedPath).extension().string();
                           std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                           if (ext == ".clayobj") {
                              // Validate type (check typeName in .clayobj against listElementTypeName)
                              ClaymoreGUID newGuid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
                              if (newGuid.high != 0 || newGuid.low != 0) {
                                 bool typeMatch = true;
                                 // Type validation for list elements
                                 if (!property.listElementTypeName.empty()) {
                                    try {
                                       std::ifstream clayFile(droppedPath);
                                       if (clayFile.is_open()) {
                                          nlohmann::json clayJson;
                                          clayFile >> clayJson;
                                          std::string assetTypeName = clayJson.value("typeName", std::string());
                                          // For now, allow all ClayObject types - proper inheritance check needs interop
                                          typeMatch = !assetTypeName.empty();
                                       }
                                    } catch (...) {}
                                 }
                                 if (typeMatch) {
                                    std::string guidStr = newGuid.ToString();
                                    listPtr->elements[i] = guidStr;
                                    updated = true;
                                 }
                              }
                           }
                        }
                     }
                     ImGui::EndDragDropTarget();
                  }
                  
                  ImGui::SameLine(0, 2);
                  if (ImGui::SmallButton("x##clear")) {
                     listPtr->elements[i] = std::string();
                     updated = true;
                  }
                  break;
               }
               case PropertyType::Texture: {
                  std::string textureRef;
                  if (std::holds_alternative<std::string>(listPtr->elements[i])) {
                     textureRef = std::get<std::string>(listPtr->elements[i]);
                  } else {
                     listPtr->elements[i] = std::string();
                     textureRef.clear();
                  }

                  std::string guidPart = textureRef;
                  size_t colonPos = textureRef.find(':');
                  if (colonPos != std::string::npos) {
                     guidPart = textureRef.substr(0, colonPos);
                  }

                  std::string displayName = "None";
                  if (!guidPart.empty()) {
                     ClaymoreGUID cmGuid = ClaymoreGUID::FromString(guidPart);
                     std::string path = AssetLibrary::Instance().GetPathForGUID(cmGuid);
                     if (!path.empty()) {
                        displayName = std::filesystem::path(path).stem().string();
                     } else {
                        displayName = "(Missing)";
                     }
                  }

                  ImGui::Button(displayName.c_str(), ImVec2(fieldWidth - 28.0f, 0));
                  if (ImGui::BeginPopupContextItem()) {
                     if (ImGui::MenuItem("Clear")) {
                        listPtr->elements[i] = std::string();
                        updated = true;
                     }
                     ImGui::EndPopup();
                  }

                  if (ImGui::BeginDragDropTarget()) {
                     if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                        const char* droppedPath = (const char*)payload->Data;
                        if (droppedPath) {
                           std::string ext = std::filesystem::path(droppedPath).extension().string();
                           std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                           if (IsTextureExtension(ext)) {
                              ClaymoreGUID newGuid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
                              if (newGuid.high != 0 || newGuid.low != 0) {
                                 listPtr->elements[i] = newGuid.ToString() + ":0";
                                 updated = true;
                              }
                           }
                        }
                     }
                     ImGui::EndDragDropTarget();
                  }

                  ImGui::SameLine(0, 2);
                  if (ImGui::SmallButton("x##clear")) {
                     listPtr->elements[i] = std::string();
                     updated = true;
                  }
                  break;
               }
               case PropertyType::Audio: {
                  std::string guid;
                  if (std::holds_alternative<std::string>(listPtr->elements[i])) {
                     guid = std::get<std::string>(listPtr->elements[i]);
                  } else {
                     listPtr->elements[i] = std::string();
                     guid.clear();
                  }

                  std::string displayName = "None";
                  if (!guid.empty()) {
                     ClaymoreGUID cmGuid = ClaymoreGUID::FromString(guid);
                     std::string path = AssetLibrary::Instance().GetPathForGUID(cmGuid);
                     if (!path.empty()) {
                        displayName = std::filesystem::path(path).stem().string();
                     } else {
                        displayName = "(Missing)";
                     }
                  }

                  ImGui::Button(displayName.c_str(), ImVec2(fieldWidth - 28.0f, 0));
                  if (ImGui::BeginPopupContextItem()) {
                     if (ImGui::MenuItem("Clear")) {
                        listPtr->elements[i] = std::string();
                        updated = true;
                     }
                     ImGui::EndPopup();
                  }

                  if (ImGui::BeginDragDropTarget()) {
                     if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                        const char* droppedPath = (const char*)payload->Data;
                        if (droppedPath) {
                           std::string ext = std::filesystem::path(droppedPath).extension().string();
                           std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                           if (IsAudioExtension(ext)) {
                              ClaymoreGUID newGuid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
                              if (newGuid.high != 0 || newGuid.low != 0) {
                                 listPtr->elements[i] = newGuid.ToString();
                                 updated = true;
                              }
                           }
                        }
                     }
                     ImGui::EndDragDropTarget();
                  }

                  ImGui::SameLine(0, 2);
                  if (ImGui::SmallButton("x##clear")) {
                     listPtr->elements[i] = std::string();
                     updated = true;
                  }
                  break;
               }
               case PropertyType::AnimatorController:
               case PropertyType::AnimatorControllerOverride: {
                  std::string path;
                  if (std::holds_alternative<std::string>(listPtr->elements[i])) {
                     path = std::get<std::string>(listPtr->elements[i]);
                  } else {
                     listPtr->elements[i] = std::string();
                     path.clear();
                  }

                  std::string displayName = path.empty() ? "None" : std::filesystem::path(path).stem().string();
                  if (!path.empty() && displayName.empty()) {
                     displayName = std::filesystem::path(path).filename().string();
                  }

                  ImGui::Button(displayName.c_str(), ImVec2(fieldWidth - 28.0f, 0));
                  if (ImGui::BeginPopupContextItem()) {
                     if (ImGui::MenuItem("Clear")) {
                        listPtr->elements[i] = std::string();
                        updated = true;
                     }
                     ImGui::EndPopup();
                  }

                  if (ImGui::BeginDragDropTarget()) {
                     if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                        const char* droppedPath = (const char*)payload->Data;
                        if (droppedPath) {
                           std::string ext = std::filesystem::path(droppedPath).extension().string();
                           std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                           const char* expectedExt = property.listElementType == PropertyType::AnimatorController
                              ? ".animctrl"
                              : ".animoverride";
                           if (ext == expectedExt) {
                              listPtr->elements[i] = ToVfsAssetPath(droppedPath);
                              updated = true;
                           }
                        }
                     }
                     ImGui::EndDragDropTarget();
                  }

                  ImGui::SameLine(0, 2);
                  if (ImGui::SmallButton("x##clear")) {
                     listPtr->elements[i] = std::string();
                     updated = true;
                  }
                  break;
               }
               default:
                  ImGui::TextDisabled("Unsupported");
                  break;
            }
            
            // Remove button (individual element)
            ImGui::SameLine(0, 4);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.2f, 0.2f, 0.6f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.3f, 0.3f, 0.8f));
            if (ImGui::Button("x", ImVec2(removeButtonWidth, 0))) {
               toRemove = static_cast<int>(i);
            }
            ImGui::PopStyleColor(2);
            
            ImGui::PopID();
         }
         
         if (toRemove >= 0) {
            listPtr->elements.erase(listPtr->elements.begin() + toRemove);
            updated = true;
         }
         
         // Empty state
         if (listPtr->elements.empty()) {
            ImGui::TextDisabled("  (empty)");
         }
         
         // Bottom row with +/- buttons (centered)
         ImGui::Spacing();
         const float buttonWidth = 24.0f;
         const float totalButtonWidth = buttonWidth * 2 + 4.0f;
         float cursorX = ImGui::GetCursorPosX() + (availWidth - totalButtonWidth) * 0.5f - 8.0f;
         ImGui::SetCursorPosX(cursorX);
         
         if (ImGui::Button("+", ImVec2(buttonWidth, 0))) {
            // Add new element with default value based on element type
            PropertyValue newElem;
            switch (property.listElementType) {
               case PropertyType::Int: newElem = 0; break;
               case PropertyType::Float: newElem = 0.0f; break;
               case PropertyType::Bool: newElem = false; break;
               case PropertyType::String: newElem = std::string(); break;
               case PropertyType::Vector3: newElem = glm::vec3(0.0f); break;
               case PropertyType::Enum: newElem = 0; break;
               case PropertyType::Entity: newElem = -1; break; // Unassigned entity
               case PropertyType::ClayObject: newElem = std::string(); break; // Empty GUID
               case PropertyType::DialogueLibrary: newElem = std::string(); break; // Empty GUID
               case PropertyType::AnimatorController: newElem = std::string(); break;
               case PropertyType::AnimatorControllerOverride: newElem = std::string(); break;
               case PropertyType::Texture: newElem = std::string(); break;
               case PropertyType::Audio: newElem = std::string(); break;
               default: newElem = 0; break;
            }
            listPtr->elements.push_back(newElem);
            updated = true;
         }
         ImGui::SameLine(0, 4);
         ImGui::BeginDisabled(count == 0);
         if (ImGui::Button("-", ImVec2(buttonWidth, 0)) && count > 0) {
            listPtr->elements.pop_back();
            updated = true;
         }
         ImGui::EndDisabled();
         
         ImGui::TreePop();
      }
      ImGui::PopID();  // Pop stable list ID
      break;
      }
      
      case PropertyType::Struct: {
      // Expandable struct with nested fields
      auto structPtr = std::get<std::shared_ptr<StructPropertyValue>>(property.currentValue);
      if (!structPtr) {
         structPtr = std::make_shared<StructPropertyValue>();
         property.currentValue = structPtr;
      }
      
      if (ImGui::TreeNode(pretty.c_str())) {
         for (auto& subProp : property.structFields) {
            DrawScriptProperty(subProp, scriptHandle, className, entityID);
         }
         ImGui::TreePop();
      }
      break;
      }
      
      case PropertyType::ClayObject: {
      // ClayScriptableObject reference field
      std::string guid = std::get<std::string>(property.currentValue);
      std::string displayName = "None";
      
      if (!guid.empty()) {
         ClaymoreGUID cmGuid = ClaymoreGUID::FromString(guid);
         std::string path = AssetLibrary::Instance().GetPathForGUID(cmGuid);
         if (!path.empty()) {
            displayName = std::filesystem::path(path).stem().string();
         } else {
            displayName = "(Missing: " + guid.substr(0, 8) + "...)";
         }
      }
      
      ImGui::Columns(2);
      ImGui::SetColumnWidth(0, 120.0f);
      ImGui::Text("%s", pretty.c_str());
      ImGui::NextColumn();
      float avail = ImGui::GetContentRegionAvail().x;
      float smallF = 24.0f;
      ImGui::SetNextItemWidth(avail - (smallF + 8.0f));
      
      // [SelectFromResources] - render as dropdown of available resources
      if (property.selectFromResources && !property.auxTypeName.empty()) {
         // Get available resources of this type
         int resourceCount = Resources_GetResourceCount(property.auxTypeName.c_str());
         
         // Build options list
         std::vector<std::string> resourceNames;
         std::vector<std::string> resourceGuids;
         resourceNames.push_back("None"); // First option is always None
         resourceGuids.push_back("");
         
         if (resourceCount > 0) {
            // IMPORTANT: Must copy strings immediately because Resources_GetResourceNames and
            // Resources_GetResourceGUIDs share a string pool that gets cleared on each call!
            
            // Get names first and copy immediately
            std::vector<char*> namePtrs(resourceCount);
            int actualCount = Resources_GetResourceNames(property.auxTypeName.c_str(), namePtrs.data(), resourceCount);
            
            // Copy names to our vector before the next call invalidates the pointers
            std::vector<std::string> tempNames;
            tempNames.reserve(actualCount);
            for (int i = 0; i < actualCount; i++) {
               if (namePtrs[i]) {
                  tempNames.push_back(namePtrs[i]);
               } else {
                  tempNames.push_back("");
               }
            }
            
            // Now get GUIDs (this clears the string pool, invalidating namePtrs)
            std::vector<char*> guidPtrs(resourceCount);
            int guidCount = Resources_GetResourceGUIDs(property.auxTypeName.c_str(), guidPtrs.data(), resourceCount);
            
            // Copy GUIDs and pair with names
            for (int i = 0; i < actualCount && i < guidCount; i++) {
               if (guidPtrs[i] && !tempNames[i].empty()) {
                  resourceNames.push_back(tempNames[i]);
                  resourceGuids.push_back(guidPtrs[i]);
               }
            }
         }
         
         // Find current selection index
         int currentIdx = 0;
         for (size_t i = 0; i < resourceGuids.size(); i++) {
            if (resourceGuids[i] == guid) {
               currentIdx = static_cast<int>(i);
               break;
            }
         }
         
         // Render combo dropdown
         if (ImGui::BeginCombo("##clayobj_dropdown", resourceNames[currentIdx].c_str())) {
            for (size_t i = 0; i < resourceNames.size(); i++) {
               bool isSelected = (static_cast<int>(i) == currentIdx);
               if (ImGui::Selectable(resourceNames[i].c_str(), isSelected)) {
                  if (static_cast<int>(i) != currentIdx) {
                     property.currentValue = resourceGuids[i];
                     if (property.setter) property.setter(PropertyValue{ resourceGuids[i] });
                     updated = true;
                  }
               }
               if (isSelected) {
                  ImGui::SetItemDefaultFocus();
               }
            }
            ImGui::EndCombo();
         }
      } else {
         // Standard drag-drop button behavior
         ImGui::Button(displayName.c_str(), ImVec2(-FLT_MIN, 0));
         // Right-click context menu for Clear
         if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Clear")) {
               property.currentValue = std::string();
               if (property.setter) property.setter(PropertyValue{ std::string() });
               updated = true;
               }
            ImGui::EndPopup();
            }
         
         // Accept ASSET_FILE drops for .clayobj files (ClayObjects)
         if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
               const char* droppedPath = (const char*)payload->Data;
               if (droppedPath) {
                  std::string ext = std::filesystem::path(droppedPath).extension().string();
                  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                  if (ext == ".clayobj") {
                     // Validate type match by loading the asset and checking typeName against auxTypeName
                     ClaymoreGUID newGuid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
                     if (newGuid.high != 0 || newGuid.low != 0) {
                        // Load the .clayobj file to get its typeName
                        bool typeMatch = false;
                        try {
                           std::ifstream clayFile(droppedPath);
                           if (clayFile.is_open()) {
                              nlohmann::json clayJson;
                              clayFile >> clayJson;
                              std::string assetTypeName = clayJson.value("typeName", std::string());
                              // Check for exact match or if asset type is derived from expected type
                              // For now, check exact match or if assetTypeName ends with expected short name
                              if (!property.auxTypeName.empty() && !assetTypeName.empty()) {
                                 // Exact match
                                 if (assetTypeName == property.auxTypeName) {
                                    typeMatch = true;
                                 } else {
                                    // Check inheritance via interop (if available) or allow all for base ClayScriptableObject
                                    // For derived types, we need managed-side IsAssignableFrom check
                                    // TODO: Add proper inheritance check via interop
                                    // For now, allow drop and let runtime validation handle it
                                    typeMatch = true; // Temporary: allow all ClayObject types
                                 }
                              } else {
                                 typeMatch = true; // No type constraint
                              }
                           }
                        } catch (const std::exception& ex) {
                           std::cerr << "[InspectorPanel] Failed to validate ClayObject type: " << ex.what() << std::endl;
                        }
                        
                        if (typeMatch) {
                           std::string guidStr = newGuid.ToString();
                           property.currentValue = guidStr;
                           if (property.setter) property.setter(PropertyValue{ guidStr });
                           updated = true;
                        }
                     }
                  }
               }
            }
            ImGui::EndDragDropTarget();
         }
         ImGui::SameLine();
         if (ImGui::SmallButton("X")) {
            property.currentValue = std::string();
            if (property.setter) property.setter(PropertyValue{ std::string() });
            updated = true;
         }
      }
      ImGui::Columns(1);
      break;
      }
      
      case PropertyType::Mesh: {
      // Mesh asset reference field - accepts mesh file drops (FBX, GLB, etc.)
      // Value is stored as "GUID:fileID" string
      std::string meshRef = std::get<std::string>(property.currentValue);
      std::string displayName = "None";
      
      if (!meshRef.empty()) {
         // Parse "GUID:fileID" format
         std::string guidPart = meshRef;
         int fileId = 0;
         size_t colonPos = meshRef.find(':');
         if (colonPos != std::string::npos) {
            guidPart = meshRef.substr(0, colonPos);
            try { fileId = std::stoi(meshRef.substr(colonPos + 1)); } catch(...) {}
         }
         
         ClaymoreGUID cmGuid = ClaymoreGUID::FromString(guidPart);
         std::string path = AssetLibrary::Instance().GetPathForGUID(cmGuid);
         if (!path.empty()) {
            std::string stem = std::filesystem::path(path).stem().string();
            if (fileId > 0) {
               displayName = stem + " [" + std::to_string(fileId) + "]";
            } else {
               displayName = stem;
            }
         } else {
            displayName = "(Missing: " + guidPart.substr(0, 8) + "...)";
         }
      }
      
      ImGui::Columns(2);
      ImGui::SetColumnWidth(0, 120.0f);
      ImGui::Text("%s", pretty.c_str());
      ImGui::NextColumn();
      float avail = ImGui::GetContentRegionAvail().x;
      float smallF = 24.0f;
      ImGui::SetNextItemWidth(avail - (smallF + 8.0f));
      ImGui::Button(displayName.c_str(), ImVec2(-FLT_MIN, 0));
      
      // Accept ASSET_FILE drops for mesh files (.fbx, .glb, .gltf, .obj, .dae, or .meta with mesh type)
      if (ImGui::BeginDragDropTarget()) {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
            const char* droppedPath = (const char*)payload->Data;
            if (droppedPath) {
               std::string ext = std::filesystem::path(droppedPath).extension().string();
               std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
               
               // Accept common 3D model formats
               bool isMeshFile = (ext == ".fbx" || ext == ".glb" || ext == ".gltf" || 
                                  ext == ".obj" || ext == ".dae" || ext == ".meta");
               if (isMeshFile) {
                  // Get GUID for the mesh asset
                  ClaymoreGUID newGuid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
                  if (newGuid.high != 0 || newGuid.low != 0) {
                     // Default to fileID 0; user can later pick submesh if needed
                     std::string meshRefStr = newGuid.ToString() + ":0";
                     property.currentValue = meshRefStr;
                     if (property.setter) property.setter(PropertyValue{ meshRefStr });
                     updated = true;
                  }
               }
            }
         }
         ImGui::EndDragDropTarget();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("X")) {
         property.currentValue = std::string();
         if (property.setter) property.setter(PropertyValue{ std::string() });
         updated = true;
      }
      ImGui::Columns(1);
      break;
      }

      case PropertyType::Texture: {
      std::string textureRef = std::get<std::string>(property.currentValue);
      std::string guidPart = textureRef;
      size_t colonPos = textureRef.find(':');
      if (colonPos != std::string::npos) {
         guidPart = textureRef.substr(0, colonPos);
      }

      std::string displayName = "None";
      if (!guidPart.empty()) {
         ClaymoreGUID cmGuid = ClaymoreGUID::FromString(guidPart);
         std::string path = AssetLibrary::Instance().GetPathForGUID(cmGuid);
         if (!path.empty()) {
            displayName = std::filesystem::path(path).stem().string();
         } else {
            displayName = "(Missing)";
         }
      }

      ImGui::Columns(2);
      ImGui::SetColumnWidth(0, 120.0f);
      ImGui::Text("%s", pretty.c_str());
      ImGui::NextColumn();
      float avail = ImGui::GetContentRegionAvail().x;
      float smallF = 24.0f;
      ImGui::SetNextItemWidth(avail - (smallF + 8.0f));
      ImGui::Button(displayName.c_str(), ImVec2(-FLT_MIN, 0));

      if (ImGui::BeginDragDropTarget()) {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
            const char* droppedPath = (const char*)payload->Data;
            if (droppedPath) {
               std::string ext = std::filesystem::path(droppedPath).extension().string();
               std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
               if (IsTextureExtension(ext)) {
                  ClaymoreGUID newGuid = AssetLibrary::Instance().GetGUIDForPath(droppedPath);
                  if (newGuid.high != 0 || newGuid.low != 0) {
                     std::string textureRefStr = newGuid.ToString() + ":0";
                     property.currentValue = textureRefStr;
                     if (property.setter) property.setter(PropertyValue{ textureRefStr });
                     updated = true;
                  }
               }
            }
         }
         ImGui::EndDragDropTarget();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("X")) {
         property.currentValue = std::string();
         if (property.setter) property.setter(PropertyValue{ std::string() });
         updated = true;
      }
      ImGui::Columns(1);
      break;
      }

      case PropertyType::Audio: {
      std::string audioGuid = std::get<std::string>(property.currentValue);
      std::string displayName = "None";

      if (!audioGuid.empty()) {
         ClaymoreGUID cmGuid = ClaymoreGUID::FromString(audioGuid);
         std::string path = AssetLibrary::Instance().GetPathForGUID(cmGuid);
         if (!path.empty()) {
            displayName = std::filesystem::path(path).stem().string();
         } else {
            displayName = "(Missing: " + audioGuid.substr(0, 8) + "...)";
         }
      }

      ImGui::Columns(2);
      ImGui::SetColumnWidth(0, 120.0f);
      ImGui::Text("%s", pretty.c_str());
      ImGui::NextColumn();
      float avail = ImGui::GetContentRegionAvail().x;
      float smallF = 24.0f;
      ImGui::SetNextItemWidth(avail - (smallF + 8.0f));

      if (RenderGuidAssetCombo("##audio", ui::GetAudioAssetOptions(), audioGuid)) {
         property.currentValue = audioGuid;
         if (property.setter) property.setter(PropertyValue{ audioGuid });
         updated = true;
      }

      if (ImGui::BeginDragDropTarget()) {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
            std::string droppedGuid = audioGuid;
            if (TryAssignGuidAssetDrop(droppedGuid, (const char*)payload->Data, [](const std::string& ext) {
                   return IsAudioExtension(ext);
                })) {
               property.currentValue = droppedGuid;
               if (property.setter) property.setter(PropertyValue{ droppedGuid });
               updated = true;
               }
         }
         ImGui::EndDragDropTarget();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("X")) {
         property.currentValue = std::string();
         if (property.setter) property.setter(PropertyValue{ std::string() });
         updated = true;
      }
      ImGui::Columns(1);
      break;
      }
      
      case PropertyType::DialogueLibrary: {
      // Dialogue library reference field - shows dropdown of available .dlglib files
      // Value is stored as GUID string
      std::string dlgRef = std::get<std::string>(property.currentValue);
      std::string displayName = "None";
      
      if (!dlgRef.empty()) {
         ClaymoreGUID cmGuid = ClaymoreGUID::FromString(dlgRef);
         std::string path = AssetLibrary::Instance().GetPathForGUID(cmGuid);
         if (!path.empty()) {
            displayName = std::filesystem::path(path).stem().string();
         } else {
            displayName = "(Missing: " + dlgRef.substr(0, 8) + "...)";
         }
      }
      
      ImGui::Columns(2);
      ImGui::SetColumnWidth(0, 120.0f);
      ImGui::Text("%s", pretty.c_str());
      ImGui::NextColumn();
      float avail = ImGui::GetContentRegionAvail().x;
      float smallF = 24.0f;
      ImGui::SetNextItemWidth(avail - (smallF + 8.0f));
      
      if (RenderGuidAssetCombo("##dlglib", ui::GetDialogueLibraryAssetOptions(), dlgRef)) {
         property.currentValue = dlgRef;
         if (property.setter) property.setter(PropertyValue{ dlgRef });
         updated = true;
      }
      
      // Accept ASSET_FILE drops for .dlglib files
      if (ImGui::BeginDragDropTarget()) {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
            std::string droppedGuid = dlgRef;
            if (TryAssignGuidAssetDrop(droppedGuid, (const char*)payload->Data, [](const std::string& ext) {
                   return ext == ".dlglib";
                })) {
               property.currentValue = droppedGuid;
               if (property.setter) property.setter(PropertyValue{ droppedGuid });
               updated = true;
               }
         }
         ImGui::EndDragDropTarget();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("X")) {
         property.currentValue = std::string();
         if (property.setter) property.setter(PropertyValue{ std::string() });
         updated = true;
      }
      ImGui::Columns(1);
      break;
      }
      }
   if (updated)
      {
      // Persist change into the entity's per-script overrides
      if (m_Context && m_SelectedEntity && *m_SelectedEntity != -1) {
         auto* d = m_Context->GetEntityData(*m_SelectedEntity);
         if (d) {
            for (auto& s : d->Scripts) {
               if (ScriptReflection::HasProperties(s.ClassName)) {
                  s.Values[property.name] = property.currentValue;
                  }
               }
            }
         }
      // Apply to live managed instance (edit-time sync), but do not hard-reset on future loads
      if (scriptHandle && SetManagedFieldPtr) {
         // First, ensure ALL serialized fields are populated on the managed instance
         // (not just the changed one) so OnValidate has access to entity references etc.
         auto* data = m_Context ? m_Context->GetEntityData(entityID) : nullptr;
         if (data && ScriptReflection::HasProperties(className)) {
            const ScriptInstance* storedScript = nullptr;
            for (const auto& s : data->Scripts) {
               if (s.ClassName == className) { storedScript = &s; break; }
            }
            if (storedScript) {
               auto& allProperties = ScriptReflection::GetScriptProperties(className);
               for (auto& prop : allProperties) {
                  PropertyValue valueToSet = prop.defaultValue;
                  auto it = storedScript->Values.find(prop.name);
                  if (it != storedScript->Values.end()) {
                     valueToSet = it->second;
                  }
                  // Override with current value for the property being edited
                  if (prop.name == property.name) {
                     valueToSet = property.currentValue;
                  }
                  // Set the field
                  if (prop.type == PropertyType::List) {
                     static thread_local std::string listBuf;
                     listBuf = ScriptReflection::PropertyValueToString(valueToSet);
                     SetManagedFieldPtr(scriptHandle, prop.name.c_str(), (void*)listBuf.c_str());
                  } else {
                     void* boxed = ScriptReflection::ValueToBox(valueToSet);
                     SetManagedFieldPtr(scriptHandle, prop.name.c_str(), boxed);
                  }
               }
            }
         } else {
            // Fallback: just set the changed property
            if (property.type == PropertyType::List) {
               static thread_local std::string listEditSyncBuffer;
               listEditSyncBuffer = ScriptReflection::PropertyValueToString(property.currentValue);
               SetManagedFieldPtr(scriptHandle, property.name.c_str(), (void*)listEditSyncBuffer.c_str());
            } else {
               void* boxed = ScriptReflection::ValueToBox(property.currentValue);
               SetManagedFieldPtr(scriptHandle, property.name.c_str(), boxed);
            }
         }
         
         // Call OnValidate() if the script implements it - allows scripts to respond to inspector changes
         if (g_Script_Invoke) {
            g_Script_Invoke(scriptHandle, "OnValidate");
         }
      } else if (!scriptHandle && m_Context && g_Script_Create && g_Script_Invoke && g_Script_Destroy && SetManagedFieldPtr) {
         // Edit mode fallback: create a temporary instance to call OnValidate
         // (This path is rarely hit since scripts usually have Instance set)
         void* tempHandle = g_Script_Create(className.c_str());
         if (tempHandle) {
            // Bind to entity so EntityID is accessible
            if (g_Script_OnCreate) {
               g_Script_OnCreate(tempHandle, static_cast<int>(entityID));
            }
            

            // Populate ALL serialized fields from stored values (not just the one that changed)
            // This ensures OnValidate has access to all entity references like characterVisuals
            auto* data = m_Context->GetEntityData(entityID);
            if (data && ScriptReflection::HasProperties(className)) {
               // Find stored script values
               const ScriptInstance* storedScript = nullptr;
               for (const auto& s : data->Scripts) {
                  if (s.ClassName == className) { storedScript = &s; break; }
               }
               
               if (storedScript) {
                  auto& allProperties = ScriptReflection::GetScriptProperties(className);
                  for (auto& prop : allProperties) {
                     // Get stored value or use default
                     PropertyValue valueToSet = prop.defaultValue;
                     auto it = storedScript->Values.find(prop.name);
                     if (it != storedScript->Values.end()) {
                        valueToSet = it->second;
                     }
                     
                     // Override with current value if this is the property being edited
                     if (prop.name == property.name) {
                        valueToSet = property.currentValue;
                     }
                     
                     // Set the field on temp instance
                     if (prop.type == PropertyType::List) {
                        static thread_local std::string listBuf;
                        listBuf = ScriptReflection::PropertyValueToString(valueToSet);
                        SetManagedFieldPtr(tempHandle, prop.name.c_str(), (void*)listBuf.c_str());
                     } else {
                        void* boxed = ScriptReflection::ValueToBox(valueToSet);
                        SetManagedFieldPtr(tempHandle, prop.name.c_str(), boxed);
                     }
                  }
               }
            }
            
            // Call OnValidate
            g_Script_Invoke(tempHandle, "OnValidate");
            
            // Destroy the temporary instance
            g_Script_Destroy(tempHandle);
         }
      }
      }
   ImGui::PopID();
   }

void InspectorPanel::DrawDynamicComponentUI(EntityID entity, cm::ModuleComponent& comp, const cm::ComponentDesc* desc) {
   // If missing schema, show read-only message
   if (!desc) {
      ImGui::TextDisabled("Missing Module for this component. Fields are hidden.");
      return;
      }

   // Future: custom inspector hook
   // if (ManagedHasCustomInspector(desc->typeId)) { if (CallCustomInspector(...)) return; }

   for (const auto& field : comp.Fields()) {
      const std::string label = PrettifyLabel(field.name);
      switch (field.data.type) {
         case cm::ValueType::Bool: {
         bool v = std::get<bool>(field.data.value);
         if (ImGui::Checkbox(label.c_str(), &v)) comp.SetBool(field.name, v);
         break;
         }
         case cm::ValueType::Int: {
         int v = std::get<int32_t>(field.data.value);
         if (ImGui::DragInt(label.c_str(), &v, 1.0f)) comp.SetInt(field.name, v);
         break;
         }
         case cm::ValueType::Int64: {
         int64_t v = std::get<int64_t>(field.data.value);
         if (ImGui::DragScalar(label.c_str(), ImGuiDataType_S64, &v, 1.0f)) comp.SetInt64(field.name, v);
         break;
         }
         case cm::ValueType::Float: {
         float v = std::get<float>(field.data.value);
         if (ImGui::DragFloat(label.c_str(), &v, 0.1f)) comp.SetFloat(field.name, v);
         break;
         }
         case cm::ValueType::Double: {
         double v = std::get<double>(field.data.value);
         if (ImGui::DragScalar(label.c_str(), ImGuiDataType_Double, &v, 0.1f)) comp.SetDouble(field.name, v);
         break;
         }
         case cm::ValueType::String: {
         std::string v = std::get<std::string>(field.data.value);
         char buf[256]; strncpy(buf, v.c_str(), sizeof(buf)); buf[sizeof(buf) - 1] = 0;
         if (ImGui::InputText(label.c_str(), buf, sizeof(buf))) comp.SetString(field.name, std::string(buf));
         break;
         }
         case cm::ValueType::Vec2: {
         glm::vec2 v = std::get<glm::vec2>(field.data.value);
         if (ImGui::DragFloat2(label.c_str(), &v.x, 0.1f)) comp.SetVec2(field.name, v);
         break;
         }
         case cm::ValueType::Vec3: {
         glm::vec3 v = std::get<glm::vec3>(field.data.value);
         if (ImGui::DragFloat3(label.c_str(), &v.x, 0.1f)) comp.SetVec3(field.name, v);
         break;
         }
         case cm::ValueType::Vec4: {
         glm::vec4 v = std::get<glm::vec4>(field.data.value);
         if (ImGui::DragFloat4(label.c_str(), &v.x, 0.1f)) comp.SetVec4(field.name, v);
         break;
         }
         case cm::ValueType::Quat: {
         glm::quat q = std::get<glm::quat>(field.data.value);
         float wxyz[4] = { q.w, q.x, q.y, q.z };
         if (ImGui::DragFloat4(label.c_str(), wxyz, 0.01f)) {
            glm::quat nq(wxyz[0], wxyz[1], wxyz[2], wxyz[3]);
            nq = glm::normalize(nq);
            comp.SetQuat(field.name, nq);
            }
         break;
         }
         case cm::ValueType::Color: {
         cm::ColorRGBA c = std::get<cm::ColorRGBA>(field.data.value);
         glm::vec4 v = c.ToVec4();
         if (ImGui::ColorEdit4(label.c_str(), &v.x)) comp.SetColor(field.name, cm::ColorRGBA(v));
         break;
         }
         case cm::ValueType::Guid: {
         std::string v = std::get<std::string>(field.data.value);
         char buf[64]; strncpy(buf, v.c_str(), sizeof(buf)); buf[sizeof(buf) - 1] = 0;
         if (ImGui::InputText(label.c_str(), buf, sizeof(buf))) comp.SetGuid(field.name, std::string(buf));
         break;
         }
         case cm::ValueType::Enum: {
         int val = std::get<cm::EnumValue>(field.data.value).value;
         if (ImGui::DragInt(label.c_str(), &val, 1.0f)) comp.SetEnum(field.name, val);
         break;
         }
         }
      }
   }


void InspectorPanel::LoadModelMetadata(const std::string& modelPath)
{
   namespace fs = std::filesystem;
   
   if (modelPath == m_ModelInspectorPath && !m_ModelMeshes.empty())
      return; // Already loaded
   
   m_ModelInspectorPath = modelPath;
   m_ModelMeshes.clear();
   m_ModelImportSettings = ModelImportSettings{};
   m_ModelMetaDirty = false;
   
   // Find meta path
   fs::path srcPath(modelPath);
   m_ModelMetaPath = (srcPath.parent_path() / (srcPath.stem().string() + ".meta")).string();
   
   // Check if meta exists
   if (!fs::exists(m_ModelMetaPath))
   {
      // Try to build the cache first
      BuiltModelPaths built;
      if (!EnsureModelCache(modelPath, built))
      {
         std::cerr << "[ModelInspector] Failed to build model cache for: " << modelPath << std::endl;
         return;
      }
      m_ModelMetaPath = built.metaPath;
   }
   
   // Load mesh info from meta
   try {
      std::ifstream in(m_ModelMetaPath);
      if (!in.is_open()) return;
      
      nlohmann::json j;
      in >> j;
      in.close();
      
      // Extract mesh entries
      if (j.contains("entries") && j["entries"].is_array())
      {
         for (const auto& entry : j["entries"])
         {
            ModelMeshInfo info;
            info.name = entry.value("name", std::string("Mesh"));
            info.skinned = entry.value("skinned", false);
            
            // Count material slots from materials array
            if (entry.contains("materials") && entry["materials"].is_array())
            {
               info.materialSlotCount = static_cast<int>(entry["materials"].size());
            }
            else
            {
               info.materialSlotCount = 1;
            }

            if (entry.contains("slotNames") && entry["slotNames"].is_array())
            {
               for (const auto& slotName : entry["slotNames"])
               {
                  info.materialSlotNames.push_back(slotName.is_string() ? slotName.get<std::string>() : std::string());
               }
            }
            else if (entry.contains("materials") && entry["materials"].is_array())
            {
               for (const auto& matJson : entry["materials"])
               {
                  info.materialSlotNames.push_back(matJson.value("name", std::string()));
               }
            }

            if (info.materialSlotNames.size() < static_cast<size_t>(info.materialSlotCount))
            {
               info.materialSlotNames.resize(static_cast<size_t>(info.materialSlotCount));
            }
            
            m_ModelMeshes.push_back(info);
         }
      }
      
      // Also check proxies for additional mesh info
      if (j.contains("proxies") && j["proxies"].is_array())
      {
         for (const auto& proxy : j["proxies"])
         {
            std::string displayName = proxy.value("displayName", std::string());
            if (displayName.empty())
               displayName = proxy.value("name", std::string("Proxy"));
            
            // Check if we already have this mesh
            bool found = false;
            for (const auto& existing : m_ModelMeshes)
            {
               if (existing.name == displayName)
               {
                  found = true;
                  break;
               }
            }
            
            if (!found)
            {
               ModelMeshInfo info;
               info.name = displayName;
               info.skinned = proxy.value("skinned", false);
               info.materialSlotCount = proxy.contains("slots") ? static_cast<int>(proxy["slots"].size()) : 1;
               info.materialSlotNames.resize(static_cast<size_t>(info.materialSlotCount));
               m_ModelMeshes.push_back(info);
            }
         }
      }
      
      // Load import settings if present
      ModelImportSettings::LoadFromMeta(m_ModelMetaPath, m_ModelImportSettings);
   }
   catch (const std::exception& e)
    {
       std::cerr << "[ModelInspector] Failed to load meta: " << e.what() << std::endl;
    }
}

bool InspectorPanel::SaveModelImportSettingsIfDirty()
{
   if (!m_ModelMetaDirty)
   {
      return true;
   }

   if (m_ModelMetaPath.empty())
   {
      std::cerr << "[ModelInspector] Cannot save import settings: meta path is empty" << std::endl;
      return false;
   }

   if (!ModelImportSettings::SaveToMeta(m_ModelMetaPath, m_ModelImportSettings))
   {
      std::cerr << "[ModelInspector] Failed to save import settings to: " << m_ModelMetaPath << std::endl;
      return false;
   }

   m_ModelMetaDirty = false;
   std::cout << "[ModelInspector] Saved import settings to: " << m_ModelMetaPath << std::endl;
   return true;
}

void InspectorPanel::DrawModelOverridesSection(EntityID entity, EntityData& data)
{
    namespace fs = std::filesystem;
    
    ImGui::Spacing();
    ImGui::Separator();
    
    // Style the header
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.25f, 0.35f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.22f, 0.30f, 0.42f, 1.0f));
    bool headerOpen = ImGui::CollapsingHeader("Model Overrides", ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopStyleColor(2);
    
    if (!headerOpen) return;
    
    ImGui::Indent(8.0f);
    
    // --- Section 1: Deleted Model Nodes ---
    ImGui::TextDisabled("Deleted Nodes");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Nodes deleted from this model instance.\nThese will stay deleted across reimports.");
    }
    
    if (data.DeletedModelNodes.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "  No deleted nodes");
    } else {
        int restoreIndex = -1;
        for (size_t i = 0; i < data.DeletedModelNodes.size(); ++i) {
            const std::string& path = data.DeletedModelNodes[i];
            ImGui::PushID(static_cast<int>(i));
            
            // Deleted node with restore button
            ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "  %s", path.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Restore")) {
                restoreIndex = static_cast<int>(i);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Restore this node from the model file");
            }
            
            ImGui::PopID();
        }
        
        // Handle restoration (deferred to avoid iterator invalidation)
        if (restoreIndex >= 0) {
            std::string pathToRestore = data.DeletedModelNodes[restoreIndex];
            data.DeletedModelNodes.erase(data.DeletedModelNodes.begin() + restoreIndex);
            
            // Trigger model reimport to restore the node
            std::string modelPath = AssetLibrary::Instance().GetPathForGUID(data.ModelAssetGuid);
            if (!modelPath.empty()) {
                // Force reimport to recreate the node
                AssetPipeline::Instance().HotSwapModelInScene(modelPath);
                std::cout << "[ModelOverrides] Restored node: " << pathToRestore << std::endl;
            }
        }
        
        ImGui::Spacing();
        if (ImGui::Button("Clear All Deletions")) {
            data.DeletedModelNodes.clear();
            // Trigger reimport to restore all nodes
            std::string modelPath = AssetLibrary::Instance().GetPathForGUID(data.ModelAssetGuid);
            if (!modelPath.empty()) {
                AssetPipeline::Instance().HotSwapModelInScene(modelPath);
                std::cout << "[ModelOverrides] Cleared all deletions, triggering reimport" << std::endl;
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Remove all deletion records and reimport to restore all nodes");
        }
    }
    
    ImGui::Spacing();
    ImGui::Separator();
    
    // --- Section 2: Current Model Hierarchy ---
    ImGui::TextDisabled("Model Hierarchy");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Current nodes in this model instance.\nClick to select in hierarchy.");
    }
    
    // Build and display current hierarchy
    int nodeCount = 0;
    std::function<void(EntityID, int)> drawNode = [&](EntityID id, int depth) {
        auto* nodeData = m_Context->GetEntityData(id);
        if (!nodeData) return;
        
        nodeCount++;
        std::string indent(depth * 2, ' ');
        
        // Determine node type icon
        const char* icon = "";
        if (nodeData->Mesh) icon = "[M]";
        else if (nodeData->Skeleton) icon = "[S]";
        else if (nodeData->Light) icon = "[L]";
        else if (nodeData->Camera) icon = "[C]";
        
        ImGui::PushID(static_cast<int>(id));
        
        // Make the node clickable to select it
        std::string label = indent + icon + " " + nodeData->Name;
        if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_None)) {
            // Select this entity in the hierarchy
            if (m_SelectedEntity) {
                *m_SelectedEntity = id;
            }
        }
        
        ImGui::PopID();
        
        // Recurse into children
        for (EntityID child : nodeData->Children) {
            drawNode(child, depth + 1);
        }
    };
    
    // Start from model root's children (not the root itself)
    for (EntityID child : data.Children) {
        drawNode(child, 0);
    }
    
    ImGui::TextDisabled("  Total: %d nodes", nodeCount);
    
    ImGui::Spacing();
    ImGui::Separator();
    
    // --- Section 3: Model Info ---
    ImGui::TextDisabled("Model Info");
    
    std::string modelPath = AssetLibrary::Instance().GetPathForGUID(data.ModelAssetGuid);
    if (!modelPath.empty()) {
        // Truncate path for display
        std::string displayPath = modelPath;
        if (displayPath.length() > 40) {
            displayPath = "..." + displayPath.substr(displayPath.length() - 37);
        }
        ImGui::Text("  Source: %s", displayPath.c_str());
        if (ImGui::IsItemHovered() && modelPath.length() > 40) {
            ImGui::SetTooltip("%s", modelPath.c_str());
        }
    }
    ImGui::Text("  GUID: %s", data.ModelAssetGuid.ToString().c_str());
    
    ImGui::Spacing();
    if (ImGui::Button("Reimport Model")) {
        if (!modelPath.empty()) {
            BuiltModelPaths built;
            if (BuildModelCacheBlocking(modelPath, built)) {
                AssetPipeline::Instance().HotSwapModelInScene(modelPath);
                std::cout << "[ModelOverrides] Manual reimport triggered for: " << modelPath << std::endl;
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Force reimport from source file\n(Deletions will be preserved)");
    }
    
    ImGui::Unindent(8.0f);
}

void InspectorPanel::DrawModelInspector()
{
   namespace fs = std::filesystem;
   
   // Ensure metadata is loaded
   LoadModelMetadata(m_SelectedAssetPath);
   
   ImGui::Separator();
   ImGui::TextDisabled("Model Import Settings");
   ImGui::Spacing();
   
   if (m_ModelMeshes.empty())
   {
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "No meshes found. Try reimporting.");
      if (ImGui::Button("Reimport Model"))
      {
         if (!SaveModelImportSettingsIfDirty())
         {
            return;
         }
         BuiltModelPaths built;
         if (BuildModelCacheBlocking(m_SelectedAssetPath, built))
         {
            m_ModelInspectorPath.clear(); // Force reload
            LoadModelMetadata(m_SelectedAssetPath);
         }
      }
      return;
   }
   
   // General import settings
   ImGui::TextDisabled("General");
   if (ImGui::DragFloat("Import Scale", &m_ModelImportSettings.ImportScale, 0.01f, 0.001f, 100.0f, "%.3f"))
   {
      m_ModelMetaDirty = true;
   }
   if (ImGui::Checkbox("Generate Tangents", &m_ModelImportSettings.GenerateTangents))
   {
      m_ModelMetaDirty = true;
   }
   if (ImGui::Checkbox("Flip UVs", &m_ModelImportSettings.FlipUVs))
   {
      m_ModelMetaDirty = true;
   }
   
   ImGui::Spacing();
   ImGui::Separator();
   ImGui::TextDisabled("Material Presets");
   ImGui::TextWrapped("Define material overrides for when this model is instantiated.");
   ImGui::Spacing();
   
   // Use ImGui state storage for persistent selection across frames
   ImGuiStorage* storage = ImGui::GetStateStorage();
   ImGuiID meshSelectId = ImGui::GetID("##ModelInspectorMeshSelect");
   ImGuiID slotSelectId = ImGui::GetID("##ModelInspectorSlotSelect");
   
   int selectedMeshIdx = storage->GetInt(meshSelectId, 0);
   int selectedSlot = storage->GetInt(slotSelectId, 0);
   
   // Clamp selection
   if (selectedMeshIdx >= (int)m_ModelMeshes.size()) selectedMeshIdx = 0;
   const ModelMeshInfo& selectedMesh = m_ModelMeshes[selectedMeshIdx];
   if (selectedSlot >= selectedMesh.materialSlotCount) selectedSlot = 0;
   
   // Mesh dropdown
   std::string meshLabel = selectedMesh.name;
   if (selectedMesh.skinned) meshLabel += " (Skinned)";
   
   if (ImGui::BeginCombo("Mesh", meshLabel.c_str()))
   {
      for (int i = 0; i < (int)m_ModelMeshes.size(); ++i)
      {
         std::string label = m_ModelMeshes[i].name;
         if (m_ModelMeshes[i].skinned) label += " (Skinned)";
         bool isSelected = (i == selectedMeshIdx);
         if (ImGui::Selectable(label.c_str(), isSelected))
         {
            selectedMeshIdx = i;
            storage->SetInt(meshSelectId, i);
            selectedSlot = 0;
            storage->SetInt(slotSelectId, 0);
         }
         if (isSelected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
   }
   
   // Material slot dropdown
   auto getSlotLabel = [&](int slot) -> std::string
   {
      std::string label = "Slot " + std::to_string(slot);
      if (slot >= 0 &&
          slot < static_cast<int>(selectedMesh.materialSlotNames.size()) &&
          !selectedMesh.materialSlotNames[slot].empty())
      {
         label += " - " + selectedMesh.materialSlotNames[slot];
      }
      return label;
   };

   std::string slotLabel = getSlotLabel(selectedSlot);
   if (selectedMesh.materialSlotCount > 1)
   {
      if (ImGui::BeginCombo("Material Slot", slotLabel.c_str()))
      {
         for (int i = 0; i < selectedMesh.materialSlotCount; ++i)
         {
            std::string entry = getSlotLabel(i);
            bool isSelected = (i == selectedSlot);
            if (ImGui::Selectable(entry.c_str(), isSelected))
            {
               selectedSlot = i;
               storage->SetInt(slotSelectId, i);
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
         }
         ImGui::EndCombo();
      }
   }
   else
   {
      ImGui::Text("Material Slot: %s", slotLabel.c_str());
   }
   
   ImGui::Separator();
   
   // Get or create preset for selected mesh/slot
   MeshMaterialPreset* preset = m_ModelImportSettings.FindOrCreatePreset(selectedMesh.name, selectedSlot);
   
   // Custom material asset option
   if (ImGui::Checkbox("Use Custom Material Asset", &preset->UseCustomMaterial))
   {
      m_ModelMetaDirty = true;
   }
   
   if (preset->UseCustomMaterial)
   {
      char matBuf[512];
      strncpy(matBuf, preset->MaterialAssetPath.c_str(), sizeof(matBuf));
      matBuf[sizeof(matBuf) - 1] = 0;
      
      ImGui::InputText("Material Path", matBuf, sizeof(matBuf));
      
      // Drag-drop target for .mat files
      if (ImGui::BeginDragDropTarget())
      {
         if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE"))
         {
            const char* p = static_cast<const char*>(payload->Data);
            if (p)
            {
               std::string ext = fs::path(p).extension().string();
               std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
               if (ext == ".mat" || ext == ".sgmat")
               {
                  strncpy(matBuf, p, sizeof(matBuf));
                  matBuf[sizeof(matBuf) - 1] = 0;
               }
            }
         }
         ImGui::EndDragDropTarget();
      }
      
      if (preset->MaterialAssetPath != matBuf)
      {
         preset->MaterialAssetPath = matBuf;
         m_ModelMetaDirty = true;
      }
   }
   else
   {
      // Full material override view with clickable texture slots
      ImGui::TextDisabled("Texture Overrides");
      
      // Lambda to draw a clickable texture slot with picker
      auto drawTextureSlot = [&](const char* label, bool& overrideFlag, std::string& path, const char* popupId)
      {
         ImGui::PushID(label);
         
         // Capture initial state for BeginDisabled/EndDisabled balance
         bool wasDisabled = !overrideFlag;
         
         ImGui::Text("%s", label);
         ImGui::SameLine(120.0f);
         
         // Override checkbox
         if (ImGui::Checkbox("##override", &overrideFlag))
         {
            m_ModelMetaDirty = true;
         }
         if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable override");
         ImGui::SameLine();
         
         if (wasDisabled)
         {
            ImGui::BeginDisabled();
         }
         
         // Texture button with preview
         bool requestPicker = false;
         ImVec2 buttonSize(64, 64);
         
         // Try to load thumbnail if path is set
         bgfx::TextureHandle previewHandle = BGFX_INVALID_HANDLE;
         if (!path.empty())
         {
            std::string absPath = path;
            if (!fs::path(path).is_absolute())
            {
               fs::path projectDir = Project::GetProjectDirectory();
               if (!projectDir.empty())
                  absPath = (projectDir / path).string();
            }
            if (fs::exists(absPath))
            {
               TextureSpecifier spec;
               spec.Path = absPath;
               previewHandle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
            }
         }
         
         if (bgfx::isValid(previewHandle))
         {
            if (ImGui::ImageButton("##tex", TextureLoader::ToImGuiTextureID(previewHandle), buttonSize))
            {
               requestPicker = true;
            }
         }
         else
         {
            if (ImGui::Button(path.empty() ? "Pick##tex" : "...", buttonSize))
            {
               requestPicker = true;
            }
         }
         
         // Drag-drop target
         if (ImGui::BeginDragDropTarget())
         {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE"))
            {
               const char* p = static_cast<const char*>(payload->Data);
               if (p)
               {
                  std::string ext = fs::path(p).extension().string();
                  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                  if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr")
                  {
                     path = p;
                     overrideFlag = true;
                     m_ModelMetaDirty = true;
                  }
               }
            }
            ImGui::EndDragDropTarget();
         }
         
         if (ImGui::IsItemHovered() && !path.empty())
         {
            ImGui::SetTooltip("%s", path.c_str());
         }
         
         ImGui::SameLine();
         if (ImGui::SmallButton("Clear"))
         {
            path.clear();
            m_ModelMetaDirty = true;
         }
         
         if (requestPicker)
         {
            ImGui::OpenPopup(popupId);
         }
         
         texturepicker::DrawTexturePickerPopup(popupId,
            [&](const std::string& selectedPath) {
               path = selectedPath;
               overrideFlag = true;
               m_ModelMetaDirty = true;
            },
            path);
         
         if (wasDisabled)
         {
            ImGui::EndDisabled();
         }
         
         ImGui::PopID();
      };
      
      drawTextureSlot("Albedo", preset->OverrideAlbedo, preset->AlbedoPath, "AlbedoPicker");
      drawTextureSlot("Normal", preset->OverrideNormal, preset->NormalPath, "NormalPicker");
      drawTextureSlot("Met/Rough", preset->OverrideMetallicRoughness, preset->MetallicRoughnessPath, "MetRoughPicker");
      drawTextureSlot("AO", preset->OverrideAO, preset->AOPath, "AOPicker");
      drawTextureSlot("Emission", preset->OverrideEmission, preset->EmissionPath, "EmissionPicker");
      drawTextureSlot("Displacement", preset->OverrideDisplacement, preset->DisplacementPath, "DisplacementPicker");
      
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextDisabled("Material Properties");
      
      // Color tint with override checkbox
      {
         bool tintWasDisabled = !preset->OverrideTint;
         
         ImGui::Text("Color Tint");
         ImGui::SameLine(120.0f);
         if (ImGui::Checkbox("##overrideTint", &preset->OverrideTint))
         {
            m_ModelMetaDirty = true;
         }
         if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable override");
         ImGui::SameLine();
         
         if (tintWasDisabled)
         {
            ImGui::BeginDisabled();
         }
         ImGui::SetNextItemWidth(-1);
         if (ImGui::ColorEdit4("##tintColor", &preset->ColorTint.x, ImGuiColorEditFlags_AlphaBar))
         {
            preset->OverrideTint = true;
            m_ModelMetaDirty = true;
         }
         if (tintWasDisabled)
         {
            ImGui::EndDisabled();
         }
      }
      
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextDisabled("Render Flags");
      
      // Alpha mode (Opaque / Blend / Cutout)
      {
         bool alphaWasDisabled = !preset->OverrideAlphaBlend;
         int alphaMode = preset->AlphaCutout ? 2 : (preset->AlphaBlend ? 1 : 0);
         const char* alphaModes[] = { "Opaque", "Blend", "Cutout" };
         
         ImGui::Text("Alpha Mode");
         ImGui::SameLine(120.0f);
         if (ImGui::Checkbox("##overrideAlpha", &preset->OverrideAlphaBlend))
         {
            m_ModelMetaDirty = true;
         }
         if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable override");
         ImGui::SameLine();
         
         if (alphaWasDisabled)
         {
            ImGui::BeginDisabled();
         }
         ImGui::SetNextItemWidth(140.0f);
         if (ImGui::Combo("##alphaMode", &alphaMode, alphaModes, IM_ARRAYSIZE(alphaModes)))
         {
            preset->OverrideAlphaBlend = true;
            preset->AlphaBlend = (alphaMode == 1);
            preset->AlphaCutout = (alphaMode == 2);
            m_ModelMetaDirty = true;
         }
         if (alphaWasDisabled)
         {
            ImGui::EndDisabled();
         }
         
         if (!alphaWasDisabled && alphaMode == 2)
         {
            if (ImGui::DragFloat("Cutout Threshold", &preset->AlphaCutoutThreshold, 0.01f, 0.0f, 1.0f))
            {
               m_ModelMetaDirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pixels with alpha below this value are discarded");
         }
      }
      
      // Two sided
      {
         bool twoSidedWasDisabled = !preset->OverrideTwoSided;
         
         ImGui::Text("Two Sided");
         ImGui::SameLine(120.0f);
         if (ImGui::Checkbox("##overrideTwoSided", &preset->OverrideTwoSided))
         {
            m_ModelMetaDirty = true;
         }
         if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable override");
         ImGui::SameLine();
         
         if (twoSidedWasDisabled)
         {
            ImGui::BeginDisabled();
         }
         if (ImGui::Checkbox("##twoSidedValue", &preset->TwoSided))
         {
            preset->OverrideTwoSided = true;
            m_ModelMetaDirty = true;
         }
         if (twoSidedWasDisabled)
         {
            ImGui::EndDisabled();
         }
      }
   }
   
   // Save/Reimport buttons
   ImGui::Spacing();
   ImGui::Separator();
   
   // Capture dirty state before any modifications
   bool wasDirty = m_ModelMetaDirty;
   
   if (wasDirty)
   {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
   }
   
    if (ImGui::Button("Save Material Presets"))
    {
       if (SaveModelImportSettingsIfDirty())
       {
          std::cout << "[ModelInspector] Saved material presets to: " << m_ModelMetaPath << std::endl;
       }
    }
   
   if (wasDirty)
   {
      ImGui::PopStyleColor();
   }
   
   if (m_ModelMetaDirty)
   {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Unsaved changes");
   }
   
    ImGui::SameLine();
    if (ImGui::Button("Reimport"))
    {
       if (!SaveModelImportSettingsIfDirty())
       {
          return;
       }
       BuiltModelPaths built;
       if (BuildModelCacheBlocking(m_SelectedAssetPath, built))
       {
          m_ModelInspectorPath.clear(); // Force reload
          LoadModelMetadata(m_SelectedAssetPath);
      }
   }
}



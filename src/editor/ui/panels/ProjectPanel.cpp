#include "ProjectPanel.h"
#include "core/rendering/TextureLoader.h"
#include "editor/import/ModelLoader.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_clay_inspector.h>
#include <imgui_claymore_style.h>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include "editor/nodegraph/ShaderGraphMaterial.h"
#include "ui/Logger.h"
#include "core/ecs/EntityData.h"
#include "../UILayer.h"
#include "editor/pipeline/AssetLibrary.h"
#include "core/assets/AssetMetadata.h"
#include "editor/pipeline/ModelImportCache.h"
#include "core/animation/AvatarSerializer.h"
#include "core/animation/HumanoidBone.h"
#include "core/animation/AnimationTypes.h"
#include "core/animation/AnimationSerializer.h"
#include "core/ecs/AnimationComponents.h"
#include "editor/Project.h"
#include "editor/pipeline/MaterialImporter.h"
#include "editor/pipeline/ShaderImporter.h"
#include "editor/pipeline/AssetPipeline.h"
#include "editor/pipeline/BinaryAssetCache.h"
#include "assets/ScriptableRegistry.h"
#include "ui/utility/UIHelpers.h"
#include "ui/utility/ModelSnapshotUtils.h"
#include "core/rendering/Renderer.h"
#include "core/rendering/Camera.h"
#include <glm/glm.hpp>
#include <cstring>
#include "core/prefab/PrefabAsset.h"
#include "core/prefab/PrefabAPI.h"
#include "editor/prefab/PrefabEditorAPI.h"
#include "core/resourcelayer/ImposterManager.h"
#include "core/resources/ResourceManifest.h"
#include "core/ecs/SkinningSystem.h"
#include "core/ecs/ParticleEmitterSystem.h"
#include "editor/tools/WorldGraphBake.h"
#include <cctype>
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../external/stb/stb_image_write.h"
#include <random>
#include <cmath>
#include <limits>
#include <functional>

namespace fs = std::filesystem;

namespace {
// Returns a string truncated to fit within wrapWidth and maxLines, adding an ellipsis when needed.
std::string TruncateWithEllipsis(const std::string& text, float wrapWidth, int maxLines) {
    if (text.empty()) return text;
    if (maxLines < 1) maxLines = 1;
    const float maxHeight = ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(maxLines);
    ImVec2 fullSize = ImGui::CalcTextSize(text.c_str(), text.c_str() + text.size(), false, wrapWidth);
    if (fullSize.y <= maxHeight + 0.1f)
        return text;

    const std::string ellipsis = "...";
    int low = 0;
    int high = static_cast<int>(text.size());
    int best = 0;
    while (low <= high) {
        int mid = (low + high) / 2;
        ImVec2 size = ImGui::CalcTextSize(text.c_str(), text.c_str() + mid, false, wrapWidth);
        if (size.y <= maxHeight) {
            best = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    best = std::max(0, best - 1);
    std::string out = text.substr(0, best);
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) {
        out.pop_back();
    }
    out += ellipsis;
    return out;
}

ImVec4 LerpColor(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t);
}

constexpr double kThumbnailValidationInterval = 0.75;
}

std::string ProjectPanel::ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

uint32_t ProjectPanel::DetermineFilterMask(const std::filesystem::path& p, bool isDirectory) {
    if (isDirectory) return 0;
    std::string ext = ProjectPanel::ToLowerCopy(p.extension().string());
    if (ext == ".scene") return FilterScenes;
    if (ext == ".prefab") return FilterPrefabs;
    if (ext == ".json") {
        std::string norm = ProjectPanel::ToLowerCopy(p.generic_string());
        if (norm.find("assets/prefabs/") != std::string::npos) return FilterPrefabs;
    }
    if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") return FilterMeshes;
    if (ext == ".mat") return FilterMaterials;
    if (ext == ".cs") return FilterScripts;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr") return FilterTextures;
    if (ext == ".anim" || ext == ".animctrl" || ext == ".animoverride") return FilterAnimations;
    return FilterScenes | FilterPrefabs | FilterMeshes | FilterMaterials | FilterScripts | FilterTextures | FilterAnimations;
}

ProjectPanel::ProjectPanel(Scene* scene, UILayer* uiLayer)
   : m_UILayer(uiLayer)
   {
   SetContext(scene);
   m_FolderIcon = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/folder.svg"));
   m_FileIcon = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/file.svg"));
   }

ProjectPanel::~ProjectPanel() {
    // Clear thumbnails without destroying - they should have been released
    // before bgfx shutdown in Application::Shutdown(). If bgfx is already
    // shut down, attempting to destroy would cause a mutex lock crash.
    // Just clear the handles to avoid any issues.
    for (auto& kv : m_ImageThumbnails) {
        kv.second.handle = BGFX_INVALID_HANDLE;
    }
    m_ImageThumbnails.clear();
}

void ProjectPanel::LoadProject(const std::string& projectPath) {
   ReleaseAllImageThumbnails();
   m_ProjectPath = projectPath;
   m_ProjectRoot = BuildFileTree(projectPath);
   m_CurrentFolder = projectPath;
   m_FileListCacheDirty = true;
   m_SearchQuery.clear();
   m_SearchLower.clear();
   m_SearchBuffer[0] = 0;
   m_FileFilterMask = kDefaultFilterMask;
   
   // Initialize ResourceManifest for the project
   // This scans the resources/ folder and builds the manifest for the Resources API
   ResourceManifest::Get().Initialize(projectPath);
   int resourceCount = ResourceManifest::Get().Scan();
   if (resourceCount > 0) {
       std::cout << "[ProjectPanel] Loaded " << resourceCount << " resources from resources/ folder" << std::endl;
   }
   }

void ProjectPanel::OnImGuiRender() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f, 7.0f));
    if (!ImGui::Begin("Project")) { ImGui::End(); ImGui::PopStyleVar(); return; }
    const ClayEditorTheme& theme = Clay_GetEditorTheme();

   bool windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
   if (windowFocused && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_F2, false) && !m_SelectedItemPath.empty()) {
       BeginRename(m_SelectedItemPath, m_SelectedItemName.empty() ? fs::path(m_SelectedItemPath).filename().string() : m_SelectedItemName);
   }

    // Handle drag-drop anywhere on the Project panel window
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_ID", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            EntityID draggedID = *(EntityID*)payload->Data;

            // Prefer filename based on root entity name
            std::string baseName = "Prefab";
            if (m_Context) {
                EntityID rootId = draggedID;
                if (auto* ed = m_Context->GetEntityData(rootId)) {
                    while (ed && ed->Parent != -1) {
                        rootId = ed->Parent;
                        ed = m_Context->GetEntityData(rootId);
                    }
                    if (ed && !ed->Name.empty()) baseName = ed->Name;
                }
            }

            // Sanitize filename
            auto sanitize = [](std::string s) {
                const std::string invalid = "<>:\"/\\|?*";
                for (char& c : s) {
                    if (invalid.find(c) != std::string::npos) c = '_';
                }
                // Trim spaces
                size_t start = s.find_first_not_of(' ');
                size_t end = s.find_last_not_of(' ');
                if (start == std::string::npos) return std::string("Prefab");
                return s.substr(start, end - start + 1);
            };

            std::string desired = sanitize(baseName);
            if (desired.empty()) desired = "Prefab";

            // Ensure we have a valid folder; default to assets/prefabs in project root
            if (m_CurrentFolder.empty()) {
                std::string def = (Project::GetProjectDirectory() / "assets/prefabs").string();
                std::error_code ec; std::filesystem::create_directories(def, ec);
                m_CurrentFolder = def;
            }
            // Ensure unique prefab path in current folder
            std::string prefabName = desired + ".prefab";
            std::string prefabPath = m_CurrentFolder + "/" + prefabName;
            int counter = 1;
            while (fs::exists(prefabPath)) {
                prefabName = desired + "_" + std::to_string(counter++) + ".prefab";
                prefabPath = m_CurrentFolder + "/" + prefabName;
            }
            CreatePrefabFromEntity(draggedID, prefabPath);
        }
        if (ImGui::GetDragDropPayload() != nullptr) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); }
        ImGui::EndDragDropTarget();
    }

    // --- Navigation Bar ---
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));
    if (ImGui::Button("< Back") && m_CurrentFolder != m_ProjectPath) {
        m_CurrentFolder = fs::path(m_CurrentFolder).parent_path().string();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_CurrentFolder.c_str());
    ImGui::SameLine();
    if (ImGui::Button("New Material")) {
        std::string base = "Material";
        std::string outPath = m_CurrentFolder + "/" + base + ".mat";
        int c = 1; while (fs::exists(outPath)) outPath = m_CurrentFolder + "/" + base + "_" + std::to_string(c++) + ".mat";
        CreateMaterialAt(outPath, "");
    }
    ImGui::PopStyleVar(2);
    ImGui::Separator();

   // --- Search Bar ---
   ImGui::PushItemWidth(-1);
   if (ImGui::InputTextWithHint("##Search", "Search...", m_SearchBuffer, IM_ARRAYSIZE(m_SearchBuffer))) {
       m_SearchQuery = m_SearchBuffer;
       m_SearchLower = ToLowerCopy(m_SearchQuery);
   }
   ImGui::PopItemWidth();
   DrawFilterChips();
   ImGui::Separator();

    // --- Splitter for Folder Tree & File Grid ---
    static float leftWidth = 250.0f;
    const float splitterSize = 5.0f;
    const float fullWidth = ImGui::GetContentRegionAvail().x;
    const float fullHeight = ImGui::GetContentRegionAvail().y;
    constexpr float minLeft = 150.0f;
    constexpr float minRight = 220.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));

    // LEFT PANEL
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.SurfaceSidebar);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.BorderSubtle);
    ImGui::PushStyleColor(ImGuiCol_Header, theme.HeaderBg);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme.HeaderBgHovered);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, theme.SelectionFill);
    ImGui::BeginChild("FolderTree", ImVec2(leftWidth, fullHeight), true);
    ImGui::ClayHeaderStripConfig folderHeaderCfg;
    folderHeaderCfg.WidthOverride = ImGui::GetContentRegionAvail().x;
    ImGui::ClayHeaderStrip("Folders", folderHeaderCfg);
    ImGui::Dummy(ImVec2(0.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
    DrawFolderTree(m_ProjectRoot);
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor(5);

    // SPLITTER
    ImGui::SameLine();
    ImGui::ClaySplitterConfig splitterCfg;
    splitterCfg.Vertical = true;
    splitterCfg.Thickness = splitterSize;
    splitterCfg.MinPrimary = minLeft;
    splitterCfg.MinSecondary = minRight;
    splitterCfg.HoverCursor = ImGuiMouseCursor_ResizeEW;
    ImGui::ClaySplitter("Project_Splitter", &leftWidth, fullWidth, fullHeight, splitterCfg);

    ImGui::SameLine();

       // RIGHT PANEL
   ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.SurfaceRaised);
   ImGui::PushStyleColor(ImGuiCol_Border, theme.BorderStrong);
   ImGui::BeginChild("FileGrid", ImVec2(fullWidth - leftWidth - splitterSize, fullHeight), true);
   
    // Grid-level prefab drop target (background)
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_ID", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            EntityID draggedID = *(EntityID*)payload->Data;

            // Prefer filename based on root entity name
            std::string baseName = "Prefab";
            if (m_Context) {
                EntityID rootId = draggedID;
                if (auto* ed = m_Context->GetEntityData(rootId)) {
                    while (ed && ed->Parent != -1) {
                        rootId = ed->Parent;
                        ed = m_Context->GetEntityData(rootId);
                    }
                    if (ed && !ed->Name.empty()) baseName = ed->Name;
                }
            }

            // Sanitize filename
            auto sanitize = [](std::string s) {
                const std::string invalid = "<>:\"/\\|?*";
                for (char& c : s) {
                    if (invalid.find(c) != std::string::npos) c = '_';
                }
                size_t start = s.find_first_not_of(' ');
                size_t end = s.find_last_not_of(' ');
                if (start == std::string::npos) return std::string("Prefab");
                return s.substr(start, end - start + 1);
            };

            std::string desired = sanitize(baseName);
            if (desired.empty()) desired = "Prefab";

            // Ensure valid folder default
            if (m_CurrentFolder.empty()) {
                std::string def = (Project::GetProjectDirectory() / "assets/prefabs").string();
                std::error_code ec; std::filesystem::create_directories(def, ec);
                m_CurrentFolder = def;
            }
            // Ensure unique prefab path in current folder
            std::string prefabName = desired + ".prefab";
            std::string prefabPath = m_CurrentFolder + "/" + prefabName;
            int counter = 1;
            while (fs::exists(prefabPath)) {
                prefabName = desired + "_" + std::to_string(counter++) + ".prefab";
                prefabPath = m_CurrentFolder + "/" + prefabName;
            }
            CreatePrefabFromEntity(draggedID, prefabPath);
        }
        if (ImGui::GetDragDropPayload() != nullptr) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); }
        ImGui::EndDragDropTarget();
    }

    // Ensure items render from top-left of grid
    ImVec2 gridStart = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(gridStart);

    
   // Handle Delete key for selected file when grid (project panel) is hovered
   if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup)) {
       if (!m_SelectedItemPath.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
           m_PendingDeletePath = m_SelectedItemPath;
           ImGui::OpenPopup("Delete File?");
       }
   }

   // Confirmation modal for deletion
   if (ImGui::BeginPopupModal("Delete File?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
       std::string fname = m_PendingDeletePath.empty() ? std::string("") : fs::path(m_PendingDeletePath).filename().string();
       ImGui::Text("Are you sure you want to delete:\n%s", fname.c_str());
       ImGui::Separator();
       if (ImGui::Button("Delete")) {
           try {
               // Attempt to delete main file
               if (!m_PendingDeletePath.empty()) {
                   std::error_code ec;
                   fs::remove(m_PendingDeletePath, ec);
                   // Also delete sidecar .meta if present
                   fs::path metaPath = m_PendingDeletePath + ".meta";
                   fs::remove(metaPath, ec);
                   // Unregister from AssetLibrary if known
                   auto guid = AssetLibrary::Instance().GetGUIDForPath(m_PendingDeletePath);
                   if (guid.high != 0 || guid.low != 0) {
                       // We don't know fileID/type here; best-effort with 0s
                       AssetReference aref(guid, 0, 0);
                       AssetLibrary::Instance().UnregisterAsset(aref);
                   }
               }
           } catch(...) {}
           // Clear selection and refresh
           m_SelectedItemPath.clear();
           m_SelectedItemName.clear();
           m_PendingDeletePath.clear();
           m_ProjectRoot = BuildFileTree(m_ProjectPath);
           ImGui::CloseCurrentPopup();
       }
       ImGui::SameLine();
       if (ImGui::Button("Cancel")) { m_PendingDeletePath.clear(); ImGui::CloseCurrentPopup(); }
       ImGui::EndPopup();
   }

   DrawFileList(m_CurrentFolder);
   ImGui::EndChild();
   ImGui::PopStyleColor(2);
   ImGui::PopStyleVar(2);

    ImGui::End();
    ImGui::PopStyleVar();
}





void ProjectPanel::CreateMaterialAt(const std::string& materialPath, const std::string& shaderPath) {
    // Seed a material JSON. If shaderPath provided and points to .shader, set shader and pre-populate defaults via meta when available.
    MaterialAssetUnified mat; mat.name = fs::path(materialPath).stem().string();
    if (!shaderPath.empty()) {
        mat.shaderPath = shaderPath;
        // Prefill params from shader meta defaults
        cm::ShaderMeta meta; std::string err;
        if (cm::ShaderImporter::ExtractMetaFromSource(shaderPath, meta, err)) {
            for (const auto& p : meta.params) {
                glm::vec4 v(0.0f);
                if (!p.defaultValue.empty()) {
                    // parse comma-separated floats
                    std::stringstream ss(p.defaultValue); std::string tok; int idx=0; while (std::getline(ss, tok, ',') && idx<4) {
                        try { v[idx++] = std::stof(tok); } catch(...) {}
                    }
                    if (p.type == "float") { v.y=v.z=v.w=0.0f; }
                }
                mat.params[p.name] = v;
            }
            for (const auto& s : meta.samplers) {
                std::string key = !s.tag.empty() ? s.tag : s.name;
                mat.textures[key] = std::string();
            }
        }
    }
    // Save file
    if (MaterialImporter::Save(materialPath, mat)) {
        std::cout << "[ProjectPanel] Created material: " << materialPath << std::endl;
        // Emit .meta and register in AssetLibrary
        try {
            fs::path p(materialPath);
            fs::path metaPath = p; metaPath += ".meta";
            AssetMetadata meta; bool hasMeta = false;
            if (fs::exists(metaPath)) {
                std::ifstream in(metaPath.string()); if (in) { nlohmann::json j; in >> j; in.close(); meta = j.get<AssetMetadata>(); hasMeta = true; }
            }
            if (!hasMeta) { meta.guid = ClaymoreGUID::Generate(); meta.type = "material"; nlohmann::json j = meta; std::ofstream out(metaPath.string()); out << j.dump(4); }
            std::string name = p.filename().string();
            std::error_code ec; fs::path rel = fs::relative(p, Project::GetProjectDirectory(), ec);
            std::string vpath = (ec ? p.string() : rel.string()); std::replace(vpath.begin(), vpath.end(), '\\', '/');
            size_t pos = vpath.find("assets/"); if (pos != std::string::npos) vpath = vpath.substr(pos);
            AssetLibrary::Instance().RegisterAsset(AssetReference(meta.guid, 0, (int)AssetType::Material), AssetType::Material, vpath, name);
            AssetLibrary::Instance().RegisterPathAlias(meta.guid, materialPath);
        } catch(...) {}
        // Refresh tree
        m_ProjectRoot = BuildFileTree(m_ProjectPath);
    } else {
        std::cerr << "[ProjectPanel] Failed to create material: " << materialPath << std::endl;
    }
}

void ProjectPanel::CreateAnimationControllerOverrideAt(const std::string& overridePath) {
    try {
        nlohmann::json j;
        j["controller"] = "";
        j["entries"] = nlohmann::json::array();

        std::ofstream out(overridePath, std::ios::trunc);
        if (out) {
            out << j.dump(4);
        }
    } catch (...) {}
}

FileNode ProjectPanel::BuildFileTree(const std::string& path) {
   FileNode node;
   node.name = fs::path(path).filename().string();
   node.path = path;
   node.isDirectory = fs::is_directory(path);

   if (node.isDirectory) {
      for (auto& entry : fs::directory_iterator(path)) {
         // Hide generated folders from project browser
         std::string entryName = entry.path().filename().string();
         if (entryName == ".bin" || entryName == ".library" || entryName == ".git" || entryName == ".vs") {
            continue;
         }
         node.children.push_back(BuildFileTree(entry.path().string()));
         }
      }
   return node;
   }

void ProjectPanel::DrawFolderTree(FileNode& node) {
   ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
      ImGuiTreeNodeFlags_SpanAvailWidth |
      ImGuiTreeNodeFlags_SpanFullWidth |
      (m_CurrentFolder == node.path ? ImGuiTreeNodeFlags_Selected : 0);

   std::string label = node.name.empty() ? node.path : node.name;
   std::string idLabel = label + "##" + node.path;
   bool open = ImGui::TreeNodeEx(idLabel.c_str(), flags, "%s", label.c_str());
   if (ImGui::IsItemClicked()) {
      m_CurrentFolder = node.path;
      }

   if (open) {
      for (auto& child : node.children) {
         if (child.isDirectory) DrawFolderTree(child);
         }
      ImGui::TreePop();
      }
   }

bool ProjectPanel::ShouldRebuildFileListCache(const std::string& folderPath) {
   if (m_FileListCacheDirty) return true;
   if (folderPath != m_FileListCacheFolder) return true;
   const double now = ImGui::GetTime();
   if ((now - m_FileListCacheBuildTime) < kFileListRefreshInterval) {
       return false;
   }
   m_FileListCacheBuildTime = now;
   std::error_code ec;
   auto timestamp = fs::last_write_time(folderPath, ec);
   if (ec) {
       return false;
   }
   return timestamp != m_FileListCacheTimestamp;
}

void ProjectPanel::RebuildFileListCache(const std::string& folderPath) {
   m_FileListCache.clear();
   m_FileListCacheFolder = folderPath;
   m_FileListCacheBuildTime = ImGui::GetTime();
   m_FileListCacheDirty = false;
   m_VisibleFileListCacheDirty = true;
   std::error_code ec;
   m_FileListCacheTimestamp = fs::last_write_time(folderPath, ec);

   m_FileListCache.reserve(128);
   for (auto& entry : fs::directory_iterator(folderPath)) {
      CachedFileEntry item;
      item.isDir = entry.is_directory();
      item.fullPath = entry.path().string();
      item.name = entry.path().filename().string();
      item.lowerName = ToLowerCopy(item.name);
      item.extLower = ToLowerCopy(entry.path().extension().string());
      item.filterMask = item.isDir ? 0 : DetermineFilterMask(entry.path(), item.isDir);

      if (!item.isDir) {
         if (item.extLower == ".meta" || item.extLower == ".meshbin" || item.extLower == ".skelbin" || item.extLower == ".animbin"
             || item.extLower == ".avatar" || item.extLower == ".tintmask" || item.name.find(".tintmask.") != std::string::npos
             || item.extLower == ".sceneb" || item.extLower == ".matbin" || item.extLower == ".prefabb" || item.extLower == ".actrlbin") {
            item.hidden = true;
         }
      }
      m_FileListCache.push_back(std::move(item));
   }

   std::stable_sort(m_FileListCache.begin(), m_FileListCache.end(), [](const CachedFileEntry& a, const CachedFileEntry& b){
       if (a.isDir != b.isDir) return a.isDir && !b.isDir; // directories first
       return a.name < b.name; // alphabetical within groups
   });
}

bool ProjectPanel::ShouldRebuildVisibleFileListCache(const std::string& folderPath) const {
   return m_VisibleFileListCacheDirty ||
       folderPath != m_VisibleFileListCacheFolder ||
       m_SearchLower != m_VisibleFileListCacheSearch ||
       m_FileFilterMask != m_VisibleFileListCacheMask;
}

void ProjectPanel::RebuildVisibleFileListCache(const std::string& folderPath) {
   m_VisibleFileListCache.clear();
   m_VisibleFileListCacheFolder = folderPath;
   m_VisibleFileListCacheSearch = m_SearchLower;
   m_VisibleFileListCacheMask = m_FileFilterMask;
   m_VisibleFileListCacheDirty = false;
   m_VisibleFileListCache.reserve(m_FileListCache.size());

   for (size_t i = 0; i < m_FileListCache.size(); ++i) {
      const CachedFileEntry& item = m_FileListCache[i];
      if (item.hidden) continue;
      if (!m_SearchLower.empty() && item.lowerName.find(m_SearchLower) == std::string::npos) continue;
      if (!item.isDir) {
         const uint32_t filter = item.filterMask;
         if (filter != 0 && (m_FileFilterMask & filter) == 0) {
            continue;
         }
      }
      m_VisibleFileListCache.push_back(i);
   }
}

void ProjectPanel::DrawFileList(const std::string& folderPath) {
   if (folderPath.empty() || !fs::exists(folderPath)) return;
   if (ShouldRebuildFileListCache(folderPath)) {
      RebuildFileListCache(folderPath);
   }
   if (ShouldRebuildVisibleFileListCache(folderPath)) {
      RebuildVisibleFileListCache(folderPath);
   }

   // Compact asset tiles: still readable, but closer to a true engine content browser.
   const float tilePadX = 4.0f;
   const float tilePadTop = 4.0f;
   const float tilePadBottom = 4.0f;
   const float iconTextGap = 4.0f;
   const float desiredMinCellWidth = 96.0f;
   const float minThumbnail = 28.0f;
   const float maxThumbnail = 36.0f;
   const int maxLabelLines = 2;
   const float rowGap = 4.0f;

   float panelWidth = ImGui::GetContentRegionAvail().x;
   if (panelWidth <= 0.0f) panelWidth = desiredMinCellWidth;

   int columnCount = (int)(panelWidth / desiredMinCellWidth);
   if (columnCount < 1) columnCount = 1;
   float cellWidth = panelWidth / (float)columnCount;
   float thumbnailSize = std::clamp(cellWidth - tilePadX * 2.0f, minThumbnail, maxThumbnail);
   const float textLineHeight = ImGui::GetTextLineHeightWithSpacing();
   const float reservedTextHeight = textLineHeight * static_cast<float>(maxLabelLines);
   const float tileBodyHeight = tilePadTop + thumbnailSize + iconTextGap + reservedTextHeight + tilePadBottom;
   const float rowHeight = tileBodyHeight + rowGap;

   EnsureExtraIconsLoaded();

   ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingStretchSame |
                                ImGuiTableFlags_NoSavedSettings |
                                ImGuiTableFlags_NoPadOuterX |
                                ImGuiTableFlags_NoPadInnerX;
   if (ImGui::BeginTable("ProjectGrid", columnCount, tableFlags, ImVec2(panelWidth, 0.0f))) {
      for (int col = 0; col < columnCount; ++col) {
         std::string columnId = "##col" + std::to_string(col);
         ImGui::TableSetupColumn(columnId.c_str(), ImGuiTableColumnFlags_WidthStretch);
      }

      const int visibleCount = static_cast<int>(m_VisibleFileListCache.size());
      const int rowCount = (visibleCount + columnCount - 1) / columnCount;
      ImGuiListClipper clipper;
      clipper.Begin(rowCount, rowHeight);
      while (clipper.Step()) {
         for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            for (int col = 0; col < columnCount; ++col) {
               ImGui::TableSetColumnIndex(col);
               const int visibleIndex = row * columnCount + col;
               if (visibleIndex >= visibleCount) {
                  continue;
               }

               const CachedFileEntry& item = m_FileListCache[m_VisibleFileListCache[static_cast<size_t>(visibleIndex)]];
               const std::string& fileName = item.name;
               const std::string& fullPath = item.fullPath;
               const std::string& extLower = item.extLower;
               const fs::path entryPath(fullPath);
               const bool isDir = item.isDir;

               float iconWidth = thumbnailSize;
               float iconHeight = thumbnailSize;
               ImTextureID icon = isDir ? m_FolderIcon : GetFileIconForPath(fullPath);
               if (!isDir && IsImageAsset(fullPath)) {
                  ImVec2 nativeSize(0.0f, 0.0f);
                  ImTextureID preview = GetImageThumbnail(fullPath, &nativeSize);
                  if (preview) {
                     icon = preview;
                     if (nativeSize.x > 0.0f && nativeSize.y > 0.0f) {
                        float aspect = nativeSize.x / nativeSize.y;
                        if (aspect > 1.0f) {
                           iconHeight = thumbnailSize / aspect;
                           iconWidth = thumbnailSize;
                        } else {
                           iconWidth = thumbnailSize * aspect;
                           iconHeight = thumbnailSize;
                        }
                        iconWidth = std::max(12.0f, iconWidth);
                        iconHeight = std::max(12.0f, iconHeight);
                     }
                  }
               }

               ImGui::PushID(fullPath.c_str());

               const float columnWidth = std::max(ImGui::GetContentRegionAvail().x, desiredMinCellWidth);
               const float wrapWidth = std::clamp(columnWidth - tilePadX * 2.0f, 78.0f, 118.0f);
               const std::string clippedName = TruncateWithEllipsis(fileName, wrapWidth, maxLabelLines);
               const ImVec2 textSize = ImGui::CalcTextSize(clippedName.c_str(), nullptr, false, wrapWidth);
               const float visibleTextWidth = std::clamp(textSize.x, 1.0f, wrapWidth);
               const int linesUsed = std::clamp(static_cast<int>(ImCeil(textSize.y / std::max(1.0f, textLineHeight))), 1, maxLabelLines);
               const float labelTextHeight = linesUsed * textLineHeight;
               const bool isSelected = (m_SelectedItemPath == fullPath);

               const ImVec2 cellPosLocal = ImGui::GetCursorPos();
               const ImVec2 cellPosScreen = ImGui::GetCursorScreenPos();
               const ImRect highlightRect(
                  cellPosScreen,
                  ImVec2(cellPosScreen.x + columnWidth, cellPosScreen.y + tileBodyHeight));
               const float tileCenterX = highlightRect.GetCenter().x;

               ImDrawList* drawList = ImGui::GetWindowDrawList();
               drawList->ChannelsSplit(2);
               drawList->ChannelsSetCurrent(1);

               const float iconX = tileCenterX - iconWidth * 0.5f;
               const float iconY = highlightRect.Min.y + tilePadTop + (thumbnailSize - iconHeight) * 0.5f;
               ImGui::SetCursorScreenPos(ImVec2(iconX, iconY));
               ImGui::Image(icon, ImVec2(iconWidth, iconHeight));

               const float textStartX = highlightRect.Min.x + (columnWidth - visibleTextWidth) * 0.5f;
               const float textY = highlightRect.Min.y + tilePadTop + thumbnailSize + iconTextGap;
               ImGui::SetCursorScreenPos(ImVec2(textStartX, textY));
               RenderFilenameLabel(clippedName, wrapWidth);

               const float labelHitWidth = std::max(visibleTextWidth, 28.0f);
               const ImRect labelRect(
                  ImVec2(textStartX, textY),
                  ImVec2(textStartX + labelHitWidth, textY + labelTextHeight));

               ImGui::SetCursorScreenPos(highlightRect.Min);
               ImGui::InvisibleButton("##tile", highlightRect.GetSize());
               const bool hoveredTile = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
               const bool clickedTile = ImGui::IsItemClicked();
               const bool doubleClickedTile = hoveredTile && ImGui::IsMouseDoubleClicked(0);

               drawList->ChannelsSetCurrent(0);
               Clay_UI_DrawTileBackground(drawList, highlightRect, hoveredTile, isSelected);
               drawList->ChannelsMerge();

               if (ImGui::BeginPopupContextItem("file_ctx")) {
          // Open scene files
          if (!isDir && IsSceneFile(fullPath)) {
              if (ImGui::MenuItem("Open")) {
                  if (m_UILayer) m_UILayer->DeferSceneLoad(fullPath);
              }
              if (ImGui::MenuItem("Bake Into Scene Graph")) {
                  if (!cm::editor::worldgraph::BakeWorldGraphFromScene(fullPath)) {
                      Logger::LogError("[WorldGraphBake] Failed to bake scene connectivity graph.");
                  } else {
                      Logger::Log("[WorldGraphBake] Baked selected scene and connected scenes.");
                  }
              }
              ImGui::Separator();
          }
          // Reimport asset: for models trigger model cache rebuild; otherwise re-enqueue import
        if (!isDir) {
            const bool isModelAsset = (extLower == ".fbx" || extLower == ".obj" || extLower == ".gltf" || extLower == ".glb");
            cm::animation::AvatarDefinition humanoidAvatar;
            bool hasHumanoidAvatar = false;
            if (isModelAsset) {
                hasHumanoidAvatar = LoadHumanoidAvatarForModel(entryPath, humanoidAvatar);
            }
            if (ImGui::MenuItem("Reimport Asset")) {
                if (isModelAsset) {
                    ReimportModelAsset(std::filesystem::directory_entry(entryPath), false, nullptr);
                } else {
                    try {
                        std::string src = fullPath;
                        AssetPipeline::Instance().ImportAsset(src);
                        AssetPipeline::Instance().ProcessAllBlocking();
                    } catch(...) {
                        std::cerr << "[Reimport] Generic import failed." << std::endl;
                    }
                }
            }
            if (isModelAsset && hasHumanoidAvatar) {
                if (ImGui::MenuItem("Reimport Sliced Humanoid")) {
                    ReimportModelAsset(std::filesystem::directory_entry(entryPath), true, &humanoidAvatar);
                }
            }
            // Armor reimport option for skinned models
            if (isModelAsset) {
                if (ImGui::MenuItem("Reimport as Armor")) {
                    ReimportModelAsArmor(std::filesystem::directory_entry(entryPath));
                }
            }
            if (isModelAsset) {
                if (ImGui::MenuItem("Model Snapshot")) {
                    m_ModelSnapshot.modelPath = fullPath;
                    m_ModelSnapshot.popupRequested = true;
                    m_ModelSnapshot.popupJustOpened = true;
                    m_ModelSnapshot.rotation = glm::vec3(0.0f);
                    m_ModelSnapshot.scale = glm::vec3(1.0f);
                    m_ModelSnapshot.overrideResolution = false;
                    m_ModelSnapshot.resolution[0] = 128;
                    m_ModelSnapshot.resolution[1] = 128;
                }
            }
        }
          // Check if this is a shader graph file
          if (!isDir) {
              if (extLower == ".shgraph") {
                  if (ImGui::MenuItem("Create Shader Graph Material")) {
                      fs::path graphPath = entryPath;
                      fs::path matPath = graphPath.parent_path() / (graphPath.stem().string() + ".sgmat");
                      int counter = 1;
                      while (fs::exists(matPath)) {
                          matPath = graphPath.parent_path() / (graphPath.stem().string() + "_" + std::to_string(counter++) + ".sgmat");
                      }
                      
                      shadergraph::ShaderGraphMaterialDesc desc;
                      desc.name = graphPath.stem().string();
                      desc.shaderGraphPath = fullPath;
                      
                      std::string baseName = graphPath.stem().string();
                      std::replace(baseName.begin(), baseName.end(), ' ', '_');
                      baseName = "shgraph_" + baseName;
                      desc.vertexShaderName = "vs_" + baseName;
                      desc.fragmentShaderName = "fs_" + baseName;
                      
                      Logger::Log("[ShaderGraph] Creating material from graph: " + fullPath);
                      if (shadergraph::SaveShaderGraphMaterial(matPath.string(), desc)) {
                          Logger::Log("[ShaderGraph] Created material: " + matPath.string());
                          m_SelectedItemPath = matPath.string();
                      } else {
                          Logger::LogError("[ShaderGraph] FAILED to create material!");
                      }
                  }
              }
          }
          
          if (ImGui::MenuItem("Create Material")) {
              std::string destFolder = isDir ? fullPath : entryPath.parent_path().string();
              std::string base = "Material";
              std::string outPath = destFolder + "/" + base + ".mat";
              int c=1; while (fs::exists(outPath)) outPath = destFolder + "/" + base + "_" + std::to_string(c++) + ".mat";
              CreateMaterialAt(outPath, "");
          }
          // Create Tint Mask for images
          if (!isDir) {
              if (extLower == ".png" || extLower == ".jpg" || extLower == ".jpeg" || extLower == ".tga" || extLower == ".bmp" || extLower == ".hdr") {
                  if (ImGui::MenuItem("Create Tint Mask")) {
                      if (m_UILayer) {
                          m_UILayer->OpenTintMaskEditor(fullPath);
                      }
                  }
              }
          }
          
          // Rebuild Binary for assets that support it
          if (!isDir) {
              if (extLower == ".scene" || extLower == ".mat" || extLower == ".prefab" || 
                  extLower == ".anim" || extLower == ".actrl") {
                  if (ImGui::MenuItem("Rebuild Binary")) {
                      std::string assetPath = fullPath;
                      if (BinaryAssetCache::Instance().RebuildBinary(assetPath)) {
                          Logger::Log("[ProjectPanel] Rebuilt binary for: " + fileName);
                      } else {
                          Logger::LogError("[ProjectPanel] Failed to rebuild binary for: " + fileName);
                      }
                  }
              }
          }
          
          // Animation generation options for humanoid animations
          if (!isDir) {
              if (extLower == ".anim") {
                  // Try to load the animation to check if it's humanoid
                  cm::animation::AnimationClip testClip = cm::animation::LoadAnimationClip(fullPath);
                  if (testClip.IsHumanoid && !testClip.HumanoidTracks.empty()) {
                      ImGui::Separator();
                      if (ImGui::MenuItem("Generate Flipped")) {
                          GenerateFlippedAnimation(fullPath);
                      }
                      if (ImGui::MenuItem("Generate Reversed")) {
                          GenerateReversedAnimation(fullPath);
                      }
                      ImGui::Separator();
                  }
              }
          }
          
          // Bake Imposter for prefabs
          if (!isDir) {
              if (extLower == ".prefab") {
                  ImGui::Separator();
                  if (ImGui::MenuItem("Bake Imposter")) {
                      // Get the prefab GUID from its .meta file
                      std::string prefabPath = fullPath;
                      std::string metaPath = prefabPath + ".meta";
                      
                      ClaymoreGUID prefabGuid;
                      bool foundGuid = false;
                      
                      if (fs::exists(metaPath)) {
                          try {
                              std::ifstream metaFile(metaPath);
                              nlohmann::json metaJson;
                              metaFile >> metaJson;
                              metaFile.close();
                              
                              if (metaJson.contains("guid")) {
                                  metaJson.at("guid").get_to(prefabGuid);
                                  foundGuid = true;
                              }
                          } catch (const std::exception& ex) {
                              std::cerr << "[ProjectPanel] Failed to read prefab meta: " << ex.what() << std::endl;
                          }
                      }
                      
                      if (foundGuid && (prefabGuid.high != 0 || prefabGuid.low != 0)) {
                          // Clear existing imposter and bake fresh
                          cm::resourcelayer::ImposterManager::Instance().ClearImposter(prefabGuid);
                          cm::resourcelayer::ImposterCache cache;
                          if (cm::resourcelayer::ImposterManager::Instance().BakeImposter(prefabGuid, cache)) {
                              Logger::Log("[Imposter] Successfully baked imposter for: " + fileName);
                          } else {
                              Logger::LogError("[Imposter] Failed to bake imposter for: " + fileName);
                          }
                      } else {
                          Logger::LogError("[Imposter] Could not find GUID for prefab: " + fileName);
                      }
                  }
                  if (ImGui::MenuItem("Prefab Snapshot")) {
                      m_PrefabSnapshot.prefabPath = fullPath;
                      m_PrefabSnapshot.popupRequested = true;
                      m_PrefabSnapshot.popupJustOpened = true;
                      m_PrefabSnapshot.rotation = glm::vec3(0.0f);
                      m_PrefabSnapshot.scale = glm::vec3(1.0f);
                      m_PrefabSnapshot.overrideResolution = false;
                      m_PrefabSnapshot.resolution[0] = 128;
                      m_PrefabSnapshot.resolution[1] = 128;
                  }
                  ImGui::Separator();
              }
          }
          
          if (ImGui::BeginMenu("Create")) {
              std::string destFolder = isDir ? fullPath : entryPath.parent_path().string();
              if (ImGui::MenuItem("Animation Controller Override")) {
                  std::string outPath = destFolder + "/NewAnimationOverride.animoverride";
                  int c = 1;
                  while (fs::exists(outPath)) {
                      outPath = destFolder + "/NewAnimationOverride_" + std::to_string(c++) + ".animoverride";
                  }
                  CreateAnimationControllerOverrideAt(outPath);
                  m_ProjectRoot = BuildFileTree(m_ProjectPath);
              }
              // Noise texture submenu
              if (ImGui::BeginMenu("Texture")) {
                  if (ImGui::BeginMenu("Noise")) {
                      if (ImGui::MenuItem("Perlin")) {
                          CreateNoiseTexture(destFolder, "Perlin");
                      }
                      if (ImGui::MenuItem("Pixel")) {
                          CreateNoiseTexture(destFolder, "Pixel");
                      }
                      if (ImGui::MenuItem("Simplex")) {
                          CreateNoiseTexture(destFolder, "Simplex");
                      }
                      if (ImGui::MenuItem("Value")) {
                          CreateNoiseTexture(destFolder, "Value");
                      }
                      if (ImGui::MenuItem("Voronoi")) {
                          CreateNoiseTexture(destFolder, "Voronoi");
                      }
                      ImGui::EndMenu();
                  }
                  ImGui::EndMenu();
              }
              // Scriptable create submenu (managed-registered)
              const auto& types = ScriptableTypeRegistry::Get().All();
              for (auto* t : types) {
                  if (!t) continue;
                  if (ImGui::MenuItem(t->menuPath.c_str())) {
                      std::string destFolder = isDir ? fullPath : entryPath.parent_path().string();
                      std::string base = t->defaultFile.empty() ? std::string("NewAsset") : t->defaultFile;
                      std::string outPath = destFolder + "/" + base + ".clayobj";
                      int c=1; while (fs::exists(outPath)) outPath = destFolder + "/" + base + "_" + std::to_string(c++) + ".clayobj";
                      // Create empty JSON with header; defaults will be applied later by managed CreateDefault during inspector open or explicit action
                      ClaymoreGUID newGuid = ClaymoreGUID::Generate();
                      nlohmann::json j;
                      j["typeId"] = ""; // managed/native will fill in on first save
                      j["typeName"] = t->fullName;
                      j["version"] = t->version;
                      j["guid"] = newGuid.ToString();
                      j["fields"] = nlohmann::json::object();
                      std::ofstream out(outPath); if (out) out << j.dump(4);
                      
                      // Register immediately with AssetLibrary so GUID lookups work
                      std::string name = fs::path(outPath).stem().string();
                      std::string vpath = outPath;
                      std::replace(vpath.begin(), vpath.end(), '\\', '/');
                      size_t pos = vpath.find("assets/");
                      if (pos != std::string::npos) vpath = vpath.substr(pos);
                      AssetReference aref(newGuid, 0, static_cast<int32_t>(AssetType::Scriptable));
                      AssetLibrary::Instance().RegisterAsset(aref, AssetType::Scriptable, vpath, name);
                      AssetLibrary::Instance().RegisterPathAlias(newGuid, outPath);
                  }
              }
              ImGui::EndMenu();
          }
          if (ImGui::MenuItem("Rename")) {
              BeginRename(fullPath, fileName);
          }
          if (!isDir && ImGui::MenuItem("Duplicate")) {
              std::string src = fullPath;
              std::string dst = src;
              std::string stem = entryPath.stem().string();
              std::string ext  = entryPath.extension().string();
              int c=1; do { dst = (entryPath.parent_path() / (stem + "_copy" + (c>1? ("_"+std::to_string(c)) : std::string()) + ext)).string(); ++c; } while (fs::exists(dst));
              try { fs::copy_file(src, dst); } catch(...) {}
              try {
                  fs::path metaSrc = fs::path(src).string() + ".meta";
                  fs::path metaDst = fs::path(dst).string() + ".meta";
                  if (fs::exists(metaSrc)) {
                      AssetMetadata meta; { std::ifstream in(metaSrc.string()); if (in){ nlohmann::json j; in>>j; meta = j.get<AssetMetadata>(); } }
                      meta.guid = ClaymoreGUID::Generate();
                      nlohmann::json j = meta; std::ofstream out(metaDst.string()); out<<j.dump(4);
                      std::string name = fs::path(dst).filename().string();
                      std::error_code ec; fs::path rel = fs::relative(fs::path(dst), Project::GetProjectDirectory(), ec);
                      std::string vpath = (ec ? dst : rel.string()); std::replace(vpath.begin(), vpath.end(), '\\', '/');
                      size_t pos = vpath.find("assets/"); if (pos != std::string::npos) vpath = vpath.substr(pos);
                      AssetReference aref(meta.guid, 0, (int)ProjectPanel::GuessAssetTypeFromPath(dst));
                      AssetLibrary::Instance().RegisterAsset(aref, ProjectPanel::GuessAssetTypeFromPath(dst), vpath, name);
                      AssetLibrary::Instance().RegisterPathAlias(meta.guid, dst);
                  }
              } catch(...) {}
              m_ProjectRoot = BuildFileTree(m_ProjectPath);
          }
          if (ImGui::MenuItem("Copy")) { m_ClipboardPath = fullPath; m_ClipboardIsCut = false; }
          if (ImGui::MenuItem("Cut"))  { m_ClipboardPath = fullPath; m_ClipboardIsCut = true; }
          if (ImGui::MenuItem("Paste")) {
              std::string destFolder = isDir ? fullPath : entryPath.parent_path().string();
              if (!m_ClipboardPath.empty()) PasteInto(destFolder);
          }
          if (m_UILayer) {
              editorui::ProjectItemContext context;
              context.Path = fullPath;
              context.ParentFolder = entryPath.parent_path().string();
              context.ExtensionLower = extLower;
              context.IsDirectory = isDir;
              if (m_UILayer->GetEditorContextMenus().RenderProjectItem(context)) {
                  ImGui::Separator();
              }
          }
          if (!isDir && ImGui::MenuItem("Delete File")) {
              m_PendingDeletePath = fullPath;
              ImGui::OpenPopup("Delete File?");
          }
          ImGui::EndPopup();
               }

               if (ImGui::BeginDragDropTarget()) {
                  if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_ID", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                     EntityID draggedID = *(EntityID*)payload->Data;
                     if (m_CurrentFolder.empty()) {
                        std::string def = (Project::GetProjectDirectory() / "assets/prefabs").string();
                        std::error_code ec; std::filesystem::create_directories(def, ec);
                        m_CurrentFolder = def;
                     }
                     std::string baseName = fs::path(fileName).stem().string();
                     auto sanitize = [](std::string s) {
                        const std::string invalid = "<>:\"/\\|?*";
                        for (char& c : s) { if (invalid.find(c) != std::string::npos) c = '_'; }
                        size_t start = s.find_first_not_of(' ');
                        size_t end = s.find_last_not_of(' ');
                        if (start == std::string::npos) return std::string("Prefab");
                        return s.substr(start, end - start + 1);
                     };
                     std::string desired = sanitize(baseName);
                     if (desired.empty()) desired = "Prefab";
                     std::string prefabPath = m_CurrentFolder + "/" + desired + ".prefab";
                     int counter = 1;
                     while (fs::exists(prefabPath)) {
                        prefabPath = m_CurrentFolder + "/" + desired + "_" + std::to_string(counter++) + ".prefab";
                     }
                     CreatePrefabFromEntity(draggedID, prefabPath);
                  }
                  if (const ImGuiPayload* payload2 = ImGui::AcceptDragDropPayload("ASSET_FILE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                     const char* dpath = (const char*)payload2->Data;
                     if (dpath) {
                        std::string ext2 = fs::path(dpath).extension().string();
                        std::transform(ext2.begin(), ext2.end(), ext2.begin(), ::tolower);
                        if (ext2 == ".shader") {
                           std::string destFolder = isDir ? fullPath : entryPath.parent_path().string();
                           std::string base = fs::path(dpath).stem().string();
                           std::string outPath = destFolder + "/" + base + ".mat";
                           int c = 1; while (fs::exists(outPath)) outPath = destFolder + "/" + base + "_" + std::to_string(c++) + ".mat";
                           CreateMaterialAt(outPath, dpath);
                        }
                     }
                  }
                  if (ImGui::GetDragDropPayload() != nullptr) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); }
                  ImGui::EndDragDropTarget();
               }

               // Drag-drop support - check BEFORE click handling so we can detect if drag started
               if (!isDir && ImGui::BeginDragDropSource()) {
                  ImGui::SetDragDropPayload("ASSET_FILE", fullPath.c_str(), fullPath.size() + 1);
                  ImGui::Text("Placing: %s", fileName.c_str());
                  ImGui::EndDragDropSource();
                  // If we started dragging this item, mark that drag started and clear pending selection
                  // This prevents the inspector from switching to the dragged item's info
                  if (m_PendingSelectionPath == fullPath) {
                     m_DragStartedThisClick = true;
                     m_PendingSelectionPath.clear();
                     m_PendingSelectionName.clear();
                  }
               }

               if (clickedTile) {
                  // Store pending selection instead of immediately applying it
                  // This allows us to cancel if a drag operation starts
                  m_PendingSelectionPath = fullPath;
                  m_PendingSelectionName = fileName;
                  m_DragStartedThisClick = false;

                  // Check if click was on the label area (not the icon) for rename-on-slow-double-click
                  ImVec2 mousePos = ImGui::GetMousePos();
                  bool clickedOnLabel = labelRect.Contains(mousePos);

                  double now = ImGui::GetTime();
                  if (fullPath == m_LastClickedItem && clickedOnLabel) {
                     double delta = now - m_LastClickTime;
                     if (delta > 0.25 && delta < 1.0) {
                        BeginRename(fullPath, fileName);
                     }
                  }
                  m_LastClickedItem = fullPath;
                  m_LastClickTime = now;
               }

               // Finalize pending selection on mouse release if no drag occurred
               if (!m_PendingSelectionPath.empty() && m_PendingSelectionPath == fullPath) {
                  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !m_DragStartedThisClick) {
                     // Mouse was released without starting a drag - finalize selection
                     m_SelectedItemName = m_PendingSelectionName;
                     m_SelectedItemPath = m_PendingSelectionPath;
                     if (!isDir && !IsSceneFile(fullPath)) {
                        std::cout << "[Open] File clicked: " << fullPath << "\n";
                     }
                     m_PendingSelectionPath.clear();
                     m_PendingSelectionName.clear();
                  }
               }

               // Double-click: enter directory or open asset. Also clear selection after open to avoid heavy inspector previews running in background
               if (doubleClickedTile) {
                  // Clear pending selection state on double-click
                  m_PendingSelectionPath.clear();
                  m_PendingSelectionName.clear();
                  if (isDir) {
                     m_CurrentFolder = fullPath;
                  } else {
                     if (IsSceneFile(fullPath)) {
                        if (m_UILayer) m_UILayer->DeferSceneLoad(fullPath);
                     } else if (IsPrefabFile(fullPath)) {
                        if (m_UILayer) m_UILayer->OpenPrefabEditor(fullPath);
                     } else {
                        std::string norm = fullPath; std::replace(norm.begin(), norm.end(), '\\', '/');
                        std::string extlc = fs::path(norm).extension().string();
                        std::transform(extlc.begin(), extlc.end(), extlc.begin(), ::tolower);
                        if (extlc == ".json" && norm.find("/assets/prefabs/") != std::string::npos) {
                           if (m_UILayer) m_UILayer->OpenPrefabEditor(fullPath);
                        } else if (extlc == ".cs" || extlc == ".shader" || extlc == ".hlsl" || extlc == ".glsl") {
                           if (m_UILayer) m_UILayer->OpenCodeEditor(fullPath);
                        } else if (extlc == ".animctrl") {
                           if (m_UILayer) m_UILayer->OpenAnimatorController(fullPath);
                        } else if (extlc == ".shgraph") {
                           if (m_UILayer) m_UILayer->OpenShaderGraph(fullPath);
                        } else if (extlc == ".dlglib") {
                           // Open dialogue library in dialogue editor
                           if (m_UILayer) {
                              m_UILayer->GetDialogueEditorPanel().LoadDialogueLibrary(fullPath);
                              m_UILayer->GetDialogueEditorPanel().Open();
                           }
                        }
                     }
                     // Clear selection after opening a file to prevent inspector from keeping it selected
                     m_SelectedItemPath.clear();
                     m_SelectedItemName.clear();
                  }
               }

               ImGui::SetCursorPos(cellPosLocal);
               ImGui::Dummy(ImVec2(columnWidth, rowHeight));
               ImGui::PopID();
            }
         }
      }

      ImGui::EndTable();
   }

   if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup)
       && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
       && !ImGui::IsAnyItemHovered())
   {
       m_SelectedItemPath.clear();
       m_SelectedItemName.clear();
       m_PendingSelectionPath.clear();
       m_PendingSelectionName.clear();
   }

   // Background right-click context menu for empty space
   if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup)
       && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
       && !ImGui::IsAnyItemHovered())
   {
       ImGui::OpenPopup("background_ctx");
   }
   DrawBackgroundContextMenu();

   DrawRenamePopup();
   DrawModelSnapshotPopup();
   UpdateModelSnapshot();
   DrawPrefabSnapshotPopup();
   UpdatePrefabSnapshot();
}

// Pick icons based on file type
ImTextureID ProjectPanel::GetFileIconForPath(const std::string& path) const {
    std::string norm = path; std::replace(norm.begin(), norm.end(), '\\', '/');
    std::string ext = fs::path(norm).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") return m_Icon3DModel ? m_Icon3DModel : m_FileIcon;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr") return m_IconImage ? m_IconImage : m_FileIcon;
    if (ext == ".mat") return m_IconMaterial ? m_IconMaterial : m_FileIcon;
    if (ext == ".scene") return m_IconScene ? m_IconScene : m_FileIcon;
    if (ext == ".prefab") return m_IconPrefab ? m_IconPrefab : m_FileIcon;
    if (ext == ".json" && norm.find("/assets/prefabs/") != std::string::npos) return m_IconPrefab ? m_IconPrefab : m_FileIcon;
    if (ext == ".anim") return m_IconAnimation ? m_IconAnimation : m_FileIcon;
    if (ext == ".cs") return m_IconCSharp ? m_IconCSharp : m_FileIcon;
    if (ext == ".animctrl") return m_IconAnimController ? m_IconAnimController : m_FileIcon;
    if (ext == ".animoverride") return m_IconAnimController ? m_IconAnimController : m_FileIcon;
    if (ext == ".shgraph" || ext == ".sgmat") return m_IconMaterial ? m_IconMaterial : m_FileIcon;
    if (ext == ".clayobj") return m_IconCSharp ? m_IconCSharp : m_FileIcon; // ClayObject uses script icon
    if (ext == ".dlglib") return m_FileIcon; // Dialogue library
    return m_FileIcon;
}

// Inspector for the currently selected asset (right panel or inline under grid)
void ProjectPanel::DrawSelectedInspector() {
    if (m_SelectedItemPath.empty()) return;
    const std::string ext = std::filesystem::path(m_SelectedItemPath).extension().string();
    if (IsSceneFile(m_SelectedItemPath)) {
        DrawScenePreviewInspector(m_SelectedItemPath);
        return;
    }
    // Material inspector (.mat)
    {
        std::string lower = ext; std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower == ".mat") {
            ImGui::Separator();
            ImGui::Text("Material: %s", std::filesystem::path(m_SelectedItemPath).filename().string().c_str());
            MaterialAssetUnified mat{};
            bool ok = MaterialImporter::Load(m_SelectedItemPath, mat);
            if (!ok) {
                ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "Failed to load material JSON");
                return;
            }
            // Shader path field with drag-drop of .shader
            char shaderBuf[512];
            strncpy(shaderBuf, mat.shaderPath.c_str(), sizeof(shaderBuf)); shaderBuf[sizeof(shaderBuf)-1]=0;
            ImGui::InputText("Shader", shaderBuf, sizeof(shaderBuf));
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                    const char* p = (const char*)payload->Data;
                    if (p) {
                        std::string e = fs::path(p).extension().string(); std::transform(e.begin(), e.end(), e.begin(), ::tolower);
                        if (e == ".shader") {
                            strncpy(shaderBuf, p, sizeof(shaderBuf)); shaderBuf[sizeof(shaderBuf)-1]=0;
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
                    } else {
                        // scalar vs vec4 heuristic
                        if (p.type == "float") {
                            float f = v.x; if (ImGui::DragFloat(p.name.c_str(), &f, 0.01f)) { v.x = f; mat.params[p.name] = v; }
                        } else {
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
                    char tbuf[512]; strncpy(tbuf, path.c_str(), sizeof(tbuf)); tbuf[sizeof(tbuf)-1]=0;
                    ImGui::InputText((std::string("##tex_") + key).c_str(), tbuf, sizeof(tbuf));
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                            const char* p = (const char*)payload->Data; if (p) { std::string ext2 = fs::path(p).extension().string(); std::transform(ext2.begin(), ext2.end(), ext2.begin(), ::tolower); if (ext2 == ".png" || ext2 == ".jpg" || ext2 == ".jpeg" || ext2 == ".tga" || ext2 == ".bmp" || ext2 == ".hdr") { strncpy(tbuf, p, sizeof(tbuf)); tbuf[sizeof(tbuf)-1]=0; } }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    path = tbuf;
                }
            }
            // Save button
            if (ImGui::Button("Save Material")) {
                MaterialImporter::Save(m_SelectedItemPath, mat);
                std::cout << "[Material] Saved: " << m_SelectedItemPath << std::endl;
            }
            return;
        }
        // Shader Graph inspector (.shgraph)
        if (lower == ".shgraph") {
            ImGui::Separator();
            ImGui::Text("Shader Graph: %s", std::filesystem::path(m_SelectedItemPath).filename().string().c_str());
            ImGui::TextDisabled("Double-click to open in Shader Graph Editor");
            if (ImGui::Button("Open in Editor")) {
                if (m_UILayer) m_UILayer->OpenShaderGraph(m_SelectedItemPath);
            }
            ImGui::SameLine();
            if (ImGui::Button("Create Material")) {
                // Create a .sgmat file that references this shader graph
                fs::path graphPath(m_SelectedItemPath);
                fs::path matPath = graphPath.parent_path() / (graphPath.stem().string() + ".sgmat");
                int counter = 1;
                while (fs::exists(matPath)) {
                    matPath = graphPath.parent_path() / (graphPath.stem().string() + "_" + std::to_string(counter++) + ".sgmat");
                }
                
                shadergraph::ShaderGraphMaterialDesc desc;
                desc.name = graphPath.stem().string();
                desc.shaderGraphPath = m_SelectedItemPath;
                
                // Generate shader names based on graph file
                std::string baseName = graphPath.stem().string();
                std::replace(baseName.begin(), baseName.end(), ' ', '_');
                baseName = "shgraph_" + baseName;
                desc.vertexShaderName = "vs_" + baseName;
                desc.fragmentShaderName = "fs_" + baseName;
                
                Logger::Log("[ShaderGraph] Creating material from graph: " + m_SelectedItemPath);
                Logger::Log("[ShaderGraph]   shaderGraphPath: " + desc.shaderGraphPath);
                Logger::Log("[ShaderGraph]   matPath: " + matPath.string());
                
                if (shadergraph::SaveShaderGraphMaterial(matPath.string(), desc)) {
                    Logger::Log("[ShaderGraph] Created material: " + matPath.string());
                    // Select the newly created material
                    m_SelectedItemPath = matPath.string();
                } else {
                    Logger::LogError("[ShaderGraph] FAILED to create material!");
                }
            }
            return;
        }
        // Shader Graph Material inspector (.sgmat)
        if (lower == ".sgmat") {
            ImGui::Separator();
            ImGui::Text("Shader Graph Material: %s", std::filesystem::path(m_SelectedItemPath).filename().string().c_str());
            
            // Load and display the material info
            shadergraph::ShaderGraphMaterialDesc sgDesc;
            if (shadergraph::LoadShaderGraphMaterial(m_SelectedItemPath, sgDesc)) {
                ImGui::Text("Name: %s", sgDesc.name.c_str());
                ImGui::Text("Shader Graph: %s", sgDesc.shaderGraphPath.empty() ? "(none)" : sgDesc.shaderGraphPath.c_str());
                ImGui::Text("VS: %s", sgDesc.vertexShaderName.c_str());
                ImGui::Text("FS: %s", sgDesc.fragmentShaderName.c_str());
                
                if (!sgDesc.shaderGraphPath.empty()) {
                    // Check if the referenced shader graph exists
                    bool graphExists = fs::exists(sgDesc.shaderGraphPath);
                    if (!graphExists) {
                        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "WARNING: Shader graph file not found!");
                    }
                }
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Failed to load material file");
            }
            return;
        }
        
    }
}

// Lightweight preview of a scene JSON: counts and referenced asset paths
void ProjectPanel::DrawScenePreviewInspector(const std::string& scenePath) {
    // Only parse the scene file when selection changes (cache the results)
    if (m_ScenePreviewCache.path != scenePath) {
        m_ScenePreviewCache = ScenePreviewCache{}; // Reset cache
        m_ScenePreviewCache.path = scenePath;
        m_ScenePreviewCache.filename = std::filesystem::path(scenePath).filename().string();
        
        try {
            std::ifstream in(scenePath);
            if (in.is_open()) {
                nlohmann::json j; in >> j; in.close();
                if (j.contains("entities") && j["entities"].is_array()) {
                    m_ScenePreviewCache.entityCount = (int)j["entities"].size();
                }
                // Collect asset-looking strings
                std::function<void(const nlohmann::json&)> walk = [&](const nlohmann::json& n){
                    if (n.is_string()) {
                        std::string s = n.get<std::string>();
                        std::string lower = s; for(char &c:lower) c = (char)tolower(c);
                        if (lower.find("assets/") != std::string::npos || lower.find(".fbx")!=std::string::npos || lower.find(".gltf")!=std::string::npos || lower.find(".png")!=std::string::npos)
                            m_ScenePreviewCache.referencedAssets.push_back(s);
                    } else if (n.is_array()) for (auto& e : n) walk(e); 
                    else if (n.is_object()) for (auto it=n.begin(); it!=n.end(); ++it) walk(it.value());
                };
                walk(j);
                m_ScenePreviewCache.valid = true;
            }
        } catch(...) {}
    }
    
    // Render from cache
    ImGui::Separator();
    ImGui::Text("Scene: %s", m_ScenePreviewCache.filename.c_str());
    
    if (!m_ScenePreviewCache.valid) return;
    
    ImGui::Text("Entities: %d", m_ScenePreviewCache.entityCount);
    if (!m_ScenePreviewCache.referencedAssets.empty()) {
        ImGui::Text("Referenced assets:");
        for (const auto& a : m_ScenePreviewCache.referencedAssets) {
            ImGui::BulletText("%s", a.c_str());
        }
    }
}

void ProjectPanel::EnsureExtraIconsLoaded() const {
    if (m_IconsLoaded) return;
    m_Icon3DModel = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/3d_model.svg"));
    m_IconImage   = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/image.svg"));
    m_IconMaterial= TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/material.svg"));
    m_IconScene   = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/scene.svg"));
    m_IconPrefab  = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/cube.svg"));
    m_IconAnimation = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/animation.svg"));
    m_IconCSharp  = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/csharp.svg"));
    m_IconAnimController = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/anim_controller.svg"));
    m_IconsLoaded = true;
}

// Scene and prefab operations
void ProjectPanel::LoadSceneFile(const std::string& filepath) {
   if (!m_Context) {
      std::cerr << "[ProjectPanel] No scene context available for loading\n";
      return;
   }

   if (Serializer::LoadSceneFromFile(filepath, *m_Context)) {
      std::cout << "[ProjectPanel] Successfully loaded scene: " << filepath << std::endl;
   } else {
      std::cerr << "[ProjectPanel] Failed to load scene: " << filepath << std::endl;
   }
}

void ProjectPanel::CreatePrefabFromEntity(EntityID entityId, const std::string& prefabPath) {
   if (!m_Context) {
      std::cerr << "[ProjectPanel] No scene context available for prefab creation\n";
      return;
   }

   auto* entityData = m_Context->GetEntityData(entityId);
   if (!entityData) {
      std::cerr << "[ProjectPanel] Entity not found: " << entityId << std::endl;
      return;
   }

   PrefabAsset asset;
   bool built = prefab_editor::BuildPrefabAssetFromScene(*m_Context, entityId, asset);
   if (built && fs::exists(prefabPath)) {
      prefab_editor::AdoptExistingPrefabAssetGuid(prefabPath, asset);
   }
   if (built && PrefabIO::SavePrefab(prefabPath, asset)) {
      std::cout << "[ProjectPanel] Successfully created prefab: " << prefabPath << std::endl;
      // Refresh the file tree to show the new prefab
      m_ProjectRoot = BuildFileTree(m_ProjectPath);
      
      std::string vpath;
      if (prefab_editor::FinalizeSavedPrefabFromScene(*m_Context, entityId, prefabPath, asset, &vpath)) {
         std::cout << "[ProjectPanel] Source entity is now a prefab instance: "
                   << vpath << " (GUID: " << asset.Guid.ToString() << ")\n";
      } else {
         std::cerr << "[ProjectPanel] Prefab was saved but could not be linked: "
                   << prefabPath << std::endl;
      }
   } else {
      std::cerr << "[ProjectPanel] Failed to create prefab: " << prefabPath << std::endl;
   }
}

void ProjectPanel::NavigateTo(const std::string& folderPath) {
    // Normalize path and set as current folder if it exists
    std::string normalized = folderPath;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    
    // Convert to absolute path if relative
    fs::path p(normalized);
    if (p.is_relative() && !m_ProjectPath.empty()) {
        p = fs::path(m_ProjectPath) / normalized;
    }
    
    std::error_code ec;
    if (fs::exists(p, ec) && fs::is_directory(p, ec)) {
        m_CurrentFolder = p.string();
        std::cout << "[ProjectPanel] Navigated to: " << m_CurrentFolder << std::endl;
    }
}

// Helper functions for file operations
bool ProjectPanel::IsSceneFile(const std::string& filepath) const {
   std::string ext = fs::path(filepath).extension().string();
   std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
   return ext == ".scene";
}

bool ProjectPanel::IsPrefabFile(const std::string& filepath) const {
   std::string norm = filepath; std::replace(norm.begin(), norm.end(), '\\', '/');
   std::string ext = fs::path(norm).extension().string();
   std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
   if (ext == ".prefab") return true;
   if (ext == ".json" && norm.find("/assets/prefabs/") != std::string::npos) return true;
   return false;
}

// Guess asset type from path
AssetType ProjectPanel::GuessAssetTypeFromPath(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr") return AssetType::Texture;
    if (ext == ".mat") return AssetType::Material;
    if (ext == ".anim") return AssetType::Animation;
    if (ext == ".prefab") return AssetType::Prefab;
    if (ext == ".ttf" || ext == ".otf") return AssetType::Font;
    if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") return AssetType::Mesh;
    if (ext == ".clayobj") return AssetType::Scriptable;
    return AssetType::Shader;
}

void ProjectPanel::PasteInto(const std::string& destFolder) {
    try {
        if (m_ClipboardPath.empty() || destFolder.empty()) return;
        fs::path src = m_ClipboardPath;
        fs::path dst = fs::path(destFolder) / src.filename();
        if (fs::exists(dst)) {
            // disambiguate
            std::string stem = dst.stem().string();
            std::string ext = dst.extension().string();
            int c=1; do { dst = fs::path(destFolder) / (stem + "_" + std::to_string(c++) + ext); } while (fs::exists(dst));
        }
        if (m_ClipboardIsCut) { fs::rename(src, dst); }
        else { fs::copy_file(src, dst); }
        // Move/copy .meta and update registry
        fs::path metaSrc = src.string() + ".meta";
        fs::path metaDst = dst.string() + ".meta";
        if (fs::exists(metaSrc)) {
            AssetMetadata meta; { std::ifstream in(metaSrc.string()); if (in) { nlohmann::json j; in>>j; meta = j.get<AssetMetadata>(); } }
            if (m_ClipboardIsCut) { try { fs::rename(metaSrc, metaDst); } catch(...) {} }
            else { try { fs::copy_file(metaSrc, metaDst); } catch(...) {} }
            std::string name = dst.filename().string();
            std::error_code ec; fs::path rel = fs::relative(dst, Project::GetProjectDirectory(), ec);
            std::string vpath = (ec ? dst.string() : rel.string()); std::replace(vpath.begin(), vpath.end(), '\\', '/');
            size_t pos = vpath.find("assets/"); if (pos != std::string::npos) vpath = vpath.substr(pos);
            AssetType t = ProjectPanel::GuessAssetTypeFromPath(dst.string());
            AssetReference aref(meta.guid, 0, (int)t);
            AssetLibrary::Instance().RegisterAsset(aref, t, vpath, name);
            AssetLibrary::Instance().RegisterPathAlias(meta.guid, dst.string());
        }
        if (m_ClipboardIsCut) { m_ClipboardPath.clear(); m_ClipboardIsCut = false; }
        m_ProjectRoot = BuildFileTree(m_ProjectPath);
    } catch(...) {}
}

void ProjectPanel::DrawFilterChips() {
    const ClayEditorTheme& theme = Clay_GetEditorTheme();
    ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 accent = theme.SelectionAccent;
    const ImVec4 windowBg = theme.SurfaceBase;
    const ImVec4 fieldBg = theme.SurfaceInset;
    const ImVec4 text = theme.Text;
    const ImVec4 textDim = theme.TextMuted;
    const float chipSpacing = 4.0f;

    ImGui::TextDisabled("Filter");
    ImGui::Dummy(ImVec2(0.0f, 1.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(chipSpacing, chipSpacing));

    const float rowMaxX = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    bool firstChip = true;

    auto beginChip = [&](const char* label) {
        const float chipWidth = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
        if (!firstChip) {
            float nextRight = ImGui::GetCursorScreenPos().x + chipWidth;
            if (nextRight > rowMaxX) {
                ImGui::NewLine();
            } else {
                ImGui::SameLine(0.0f, chipSpacing);
            }
        }
        firstChip = false;
    };

    auto drawChipButton = [&](const char* label, bool active, const std::function<void()>& onClick) {
        beginChip(label);

        const ImVec4 bg = active ? LerpColor(theme.SurfaceRaised, accent, 0.18f) : LerpColor(fieldBg, theme.SurfaceSidebar, 0.28f);
        const ImVec4 bgHovered = active ? LerpColor(bg, accent, 0.08f) : LerpColor(bg, theme.SelectionHover, 0.28f);
        const ImVec4 border = active ? LerpColor(theme.BorderStrong, accent, 0.36f)
                                     : LerpColor(theme.BorderSubtle, windowBg, 0.12f);
        const ImVec4 labelColor = active ? text : LerpColor(text, textDim, 0.30f);

        ImGui::PushStyleColor(ImGuiCol_Button, bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bgHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, bgHovered);
        ImGui::PushStyleColor(ImGuiCol_Border, border);
        ImGui::PushStyleColor(ImGuiCol_Text, labelColor);
        if (ImGui::Button(label)) {
            onClick();
        }
        ImGui::PopStyleColor(5);
    };

    drawChipButton("All", m_FileFilterMask == kDefaultFilterMask, [&]() {
        m_FileFilterMask = kDefaultFilterMask;
    });
    drawChipButton("Scenes", (m_FileFilterMask & FilterScenes) != 0, [&]() {
        const bool enabled = (m_FileFilterMask & FilterScenes) != 0;
        if (!enabled) m_FileFilterMask |= FilterScenes;
        else if ((m_FileFilterMask & ~FilterScenes) != 0) m_FileFilterMask &= ~FilterScenes;
    });
    drawChipButton("Prefabs", (m_FileFilterMask & FilterPrefabs) != 0, [&]() {
        const bool enabled = (m_FileFilterMask & FilterPrefabs) != 0;
        if (!enabled) m_FileFilterMask |= FilterPrefabs;
        else if ((m_FileFilterMask & ~FilterPrefabs) != 0) m_FileFilterMask &= ~FilterPrefabs;
    });
    drawChipButton("Meshes", (m_FileFilterMask & FilterMeshes) != 0, [&]() {
        const bool enabled = (m_FileFilterMask & FilterMeshes) != 0;
        if (!enabled) m_FileFilterMask |= FilterMeshes;
        else if ((m_FileFilterMask & ~FilterMeshes) != 0) m_FileFilterMask &= ~FilterMeshes;
    });
    drawChipButton("Materials", (m_FileFilterMask & FilterMaterials) != 0, [&]() {
        const bool enabled = (m_FileFilterMask & FilterMaterials) != 0;
        if (!enabled) m_FileFilterMask |= FilterMaterials;
        else if ((m_FileFilterMask & ~FilterMaterials) != 0) m_FileFilterMask &= ~FilterMaterials;
    });
    drawChipButton("Scripts", (m_FileFilterMask & FilterScripts) != 0, [&]() {
        const bool enabled = (m_FileFilterMask & FilterScripts) != 0;
        if (!enabled) m_FileFilterMask |= FilterScripts;
        else if ((m_FileFilterMask & ~FilterScripts) != 0) m_FileFilterMask &= ~FilterScripts;
    });
    drawChipButton("Textures", (m_FileFilterMask & FilterTextures) != 0, [&]() {
        const bool enabled = (m_FileFilterMask & FilterTextures) != 0;
        if (!enabled) m_FileFilterMask |= FilterTextures;
        else if ((m_FileFilterMask & ~FilterTextures) != 0) m_FileFilterMask &= ~FilterTextures;
    });
    drawChipButton("Animations", (m_FileFilterMask & FilterAnimations) != 0, [&]() {
        const bool enabled = (m_FileFilterMask & FilterAnimations) != 0;
        if (!enabled) m_FileFilterMask |= FilterAnimations;
        else if ((m_FileFilterMask & ~FilterAnimations) != 0) m_FileFilterMask &= ~FilterAnimations;
    });

    ImGui::PopStyleVar(4);
    ImGui::Dummy(ImVec2(0.0f, 1.0f));
}

void ProjectPanel::RenderFilenameLabel(const std::string& label, float textWrapWidth) const {
    const float wrapPos = ImGui::GetCursorPos().x + textWrapWidth;
    ImGui::PushTextWrapPos(wrapPos);
    if (label.empty() || m_SearchLower.empty()) {
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopTextWrapPos();
        return;
    }

    const float multiLineThreshold = ImGui::GetTextLineHeightWithSpacing() + 0.5f;
    if (ImGui::CalcTextSize(label.c_str(), nullptr, false, textWrapWidth).y > multiLineThreshold) {
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopTextWrapPos();
        return;
    }

    std::string lower = ToLowerCopy(label);
    size_t pos = lower.find(m_SearchLower);
    if (pos == std::string::npos) {
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopTextWrapPos();
        return;
    }
    std::string prefix = label.substr(0, pos);
    std::string highlight = label.substr(pos, m_SearchLower.size());
    std::string suffix = label.substr(pos + m_SearchLower.size());
    ImGui::BeginGroup();
    bool firstSegment = true;
    auto emit = [&](const std::string& segment, bool highlighted) {
        if (segment.empty()) return;
        if (!firstSegment) ImGui::SameLine(0.0f, 0.0f);
        if (highlighted) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.83f, 0.35f, 1.0f));
        ImGui::TextUnformatted(segment.c_str());
        if (highlighted) ImGui::PopStyleColor();
        firstSegment = false;
    };
    emit(prefix, false);
    emit(highlight, true);
    emit(suffix, false);
    ImGui::EndGroup();
    ImGui::PopTextWrapPos();
}

void ProjectPanel::BeginRename(const std::string& fullPath, const std::string& fileName) {
    if (fullPath.empty()) return;
    m_SelectedItemPath = fullPath;
    m_SelectedItemName = fileName;
    m_PendingRenamePath = fullPath;
    m_RenameBuffer = fileName;
    m_RenamePopupJustOpened = true;
    m_RenamePopupRequested = true;
}

void ProjectPanel::DrawRenamePopup() {
    if (m_RenamePopupRequested) {
        ImGui::OpenPopup("Rename Item");
        m_RenamePopupRequested = false;
    }
    if (ImGui::BeginPopup("Rename Item")) {
        static char renameBuf[512] = {0};
        if (m_RenamePopupJustOpened) {
            std::memset(renameBuf, 0, sizeof(renameBuf));
            if (!m_RenameBuffer.empty()) {
                std::strncpy(renameBuf, m_RenameBuffer.c_str(), sizeof(renameBuf) - 1);
            } else if (!m_PendingRenamePath.empty()) {
                std::string basename = fs::path(m_PendingRenamePath).filename().string();
                std::strncpy(renameBuf, basename.c_str(), sizeof(renameBuf) - 1);
            }
            ImGui::SetKeyboardFocusHere();
            m_RenamePopupJustOpened = false;
        }
        bool enterConfirmed = false;
        ImGui::PushItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::InputText("##rename", renameBuf, sizeof(renameBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            m_RenameBuffer = std::string(renameBuf);
            enterConfirmed = true;
        }
        ImGui::PopItemWidth();
        bool confirm = enterConfirmed || ImGui::Button("OK");
        ImGui::SameLine();
        bool cancel = ImGui::Button("Cancel");
        if (confirm) {
            if (!m_PendingRenamePath.empty()) {
                fs::path src = m_PendingRenamePath;
                std::string desiredName = m_RenameBuffer;
                if (desiredName.empty()) desiredName = src.filename().string();
                // Preserve extension
                if (src.has_extension()) {
                    std::string desiredExt = fs::path(desiredName).extension().string();
                    if (desiredExt.empty()) {
                        desiredName += src.extension().string();
                    }
                }
                fs::path dst = src.parent_path() / desiredName;
                if (dst != src) {
                    try { fs::rename(src, dst); } catch(...) {}
                    try {
                        fs::path metaSrc = src.string() + ".meta";
                        fs::path metaDst = dst.string() + ".meta";
                        if (fs::exists(metaSrc)) {
                            AssetMetadata meta; { std::ifstream in(metaSrc.string()); if (in) { nlohmann::json j; in>>j; meta = j.get<AssetMetadata>(); } }
                            fs::rename(metaSrc, metaDst);
                            std::string name = dst.filename().string();
                            std::error_code ec; fs::path rel = fs::relative(dst, Project::GetProjectDirectory(), ec);
                            std::string vpath = (ec ? dst.string() : rel.string()); std::replace(vpath.begin(), vpath.end(), '\\', '/');
                            size_t pos = vpath.find("assets/"); if (pos != std::string::npos) vpath = vpath.substr(pos);
                            AssetType t = ProjectPanel::GuessAssetTypeFromPath(dst.string());
                            AssetReference aref(meta.guid, 0, (int)t);
                            AssetLibrary::Instance().RegisterAsset(aref, t, vpath, name);
                            AssetLibrary::Instance().RegisterPathAlias(meta.guid, dst.string());
                        }
                    } catch(...) {}
                    m_SelectedItemPath = dst.string();
                    m_SelectedItemName = dst.filename().string();
                }
            }
            m_PendingRenamePath.clear();
            m_RenameBuffer.clear();
            ImGui::CloseCurrentPopup();
            m_ProjectRoot = BuildFileTree(m_ProjectPath);
        }
        if (cancel) {
            m_PendingRenamePath.clear();
            m_RenameBuffer.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void ProjectPanel::DrawModelSnapshotPopup() {
    if (m_ModelSnapshot.popupRequested) {
        ImGui::OpenPopup("Model Snapshot");
        m_ModelSnapshot.popupRequested = false;
        m_ModelSnapshot.popupJustOpened = true;
    }
    if (ImGui::BeginPopupModal("Model Snapshot", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string modelName = m_ModelSnapshot.modelPath.empty()
            ? std::string("<none>")
            : fs::path(m_ModelSnapshot.modelPath).filename().string();
        ImGui::Text("Model: %s", modelName.c_str());
        ImGui::Separator();

        if (m_ModelSnapshot.popupJustOpened) {
            ImGui::SetKeyboardFocusHere();
            m_ModelSnapshot.popupJustOpened = false;
        }

        ImGui::PushItemWidth(ImGui::GetFontSize() * 14.0f);
        ImGui::DragFloat3("Rotation (deg)", &m_ModelSnapshot.rotation.x, 1.0f);
        ImGui::DragFloat3("Scale", &m_ModelSnapshot.scale.x, 0.01f, 0.001f, 1000.0f);
        ImGui::PopItemWidth();

        ImGui::Checkbox("Override Resolution", &m_ModelSnapshot.overrideResolution);
        if (!m_ModelSnapshot.overrideResolution) {
            ImGui::TextDisabled("Resolution: 128 x 128");
        }
        ImGui::BeginDisabled(!m_ModelSnapshot.overrideResolution);
        ImGui::PushItemWidth(ImGui::GetFontSize() * 10.0f);
        ImGui::InputInt2("Resolution", m_ModelSnapshot.resolution);
        ImGui::PopItemWidth();
        ImGui::EndDisabled();

        if (m_ModelSnapshot.pendingReadback) {
            ImGui::TextDisabled("Snapshot in progress...");
        }

        bool canCapture = !m_ModelSnapshot.modelPath.empty() && !m_ModelSnapshot.pendingReadback;
        if (!canCapture) ImGui::BeginDisabled();
        if (ImGui::Button("Capture")) {
            if (StartModelSnapshotCapture()) {
                ImGui::CloseCurrentPopup();
            }
        }
        if (!canCapture) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

bool ProjectPanel::StartModelSnapshotCapture() {
    if (m_ModelSnapshot.pendingReadback) {
        Logger::LogError("[ProjectPanel] Model snapshot already in progress.");
        return false;
    }
    if (m_ModelSnapshot.modelPath.empty()) {
        Logger::LogError("[ProjectPanel] Model snapshot requires a valid model path.");
        return false;
    }

    uint32_t width = 128;
    uint32_t height = 128;
    if (m_ModelSnapshot.overrideResolution) {
        width = static_cast<uint32_t>(std::max(1, m_ModelSnapshot.resolution[0]));
        height = static_cast<uint32_t>(std::max(1, m_ModelSnapshot.resolution[1]));
    }
    m_ModelSnapshot.width = width;
    m_ModelSnapshot.height = height;

    fs::path snapshotDir = Project::GetProjectDirectory() / "assets" / "snapshots";
    std::error_code ec;
    fs::create_directories(snapshotDir, ec);
    if (ec) {
        Logger::LogError("[ProjectPanel] Failed to create snapshot directory.");
        return false;
    }
    std::string stem = fs::path(m_ModelSnapshot.modelPath).stem().string();
    fs::path outPath = snapshotDir / (stem + ".png");
    m_ModelSnapshot.outputPath = outPath.string();

    m_ModelSnapshot.scene = std::make_unique<Scene>();
    m_ModelSnapshot.camera = std::make_unique<Camera>(60.0f, 1.0f, 0.1f, 1000.0f);

    Environment& env = m_ModelSnapshot.scene->GetEnvironment();
    env.ProceduralSky = false;
    env.UseSkybox = false;
    env.EnableFog = false;
    env.OutlineEnabled = false;
    env.AmbientColor = glm::vec3(0.6f);
    env.AmbientIntensity = 1.2f;

    Entity keyLight = m_ModelSnapshot.scene->CreateLight("Snapshot Key", LightType::Directional, glm::vec3(1.0f), 1.25f);
    if (auto* lightData = m_ModelSnapshot.scene->GetEntityData(keyLight.GetID())) {
        lightData->Transform.Rotation = glm::vec3(-35.0f, 45.0f, 0.0f);
        lightData->Transform.UseQuatRotation = false;
        lightData->Transform.TransformDirty = true;
    }
    Entity fillLight = m_ModelSnapshot.scene->CreateLight("Snapshot Fill", LightType::Directional, glm::vec3(0.9f), 0.65f);
    if (auto* lightData = m_ModelSnapshot.scene->GetEntityData(fillLight.GetID())) {
        lightData->Transform.Rotation = glm::vec3(-15.0f, -135.0f, 0.0f);
        lightData->Transform.UseQuatRotation = false;
        lightData->Transform.TransformDirty = true;
    }

    m_ModelSnapshot.modelRoot = m_ModelSnapshot.scene->InstantiateModel(m_ModelSnapshot.modelPath, glm::vec3(0.0f));
    if (m_ModelSnapshot.modelRoot == INVALID_ENTITY_ID) {
        Logger::LogError("[ProjectPanel] Failed to instantiate model for snapshot.");
        m_ModelSnapshot.scene.reset();
        m_ModelSnapshot.camera.reset();
        return false;
    }

    if (auto* data = m_ModelSnapshot.scene->GetEntityData(m_ModelSnapshot.modelRoot)) {
        glm::vec3 safeScale = glm::max(m_ModelSnapshot.scale, glm::vec3(0.001f));
        data->Transform.Rotation = m_ModelSnapshot.rotation;
        data->Transform.UseQuatRotation = false;
        data->Transform.Scale = safeScale;
        data->Transform.TransformDirty = true;
    }
    m_ModelSnapshot.scene->MarkTransformDirty(m_ModelSnapshot.modelRoot);
    m_ModelSnapshot.scene->UpdateTransforms();
    m_ModelSnapshot.scene->ProcessBoneAttachments();
    SkinningSystem::Update(*m_ModelSnapshot.scene);

    glm::vec3 boundsMin, boundsMax;
    snapshot_utils::ComputeModelWorldBounds(*m_ModelSnapshot.scene, m_ModelSnapshot.modelRoot, boundsMin, boundsMax);
    glm::vec3 center = 0.5f * (boundsMin + boundsMax);
    glm::vec3 extents = 0.5f * (boundsMax - boundsMin);
    extents = glm::max(extents, glm::vec3(0.001f));
    snapshot_utils::SnapshotViewFit viewFit = snapshot_utils::ComputeSnapshotViewFit(extents);
    float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const float fovDegrees = 60.0f;
    const float halfFov = glm::radians(fovDegrees * 0.5f);
    const float distHeight = viewFit.halfHeight / std::tan(halfFov);
    const float distWidth = viewFit.halfWidth / (std::tan(halfFov) * aspect);
    float distance = std::max(distHeight, distWidth) * 1.15f;
    float radius = std::max(extents.x, std::max(extents.y, extents.z));
    float farClip = std::max(100.0f, distance + radius * 4.0f);

    m_ModelSnapshot.camera->SetPerspective(fovDegrees, aspect, 0.1f, farClip);
    m_ModelSnapshot.camera->SetViewportSize(static_cast<float>(width), static_cast<float>(height));
    m_ModelSnapshot.camera->SetPosition(center + viewFit.viewDir * distance);
    m_ModelSnapshot.camera->LookAt(center, viewFit.upDir);

    m_ModelSnapshot.clearColor = 0xFF00FFFF;
    m_ModelSnapshot.clearKey[0] = 0xFF;
    m_ModelSnapshot.clearKey[1] = 0x00;
    m_ModelSnapshot.clearKey[2] = 0xFF;

    Scene* prevScene = Scene::CurrentScene;
    Scene::CurrentScene = m_ModelSnapshot.scene.get();
    bgfx::TextureHandle tex = Renderer::Get().RenderSceneToTexture(
        m_ModelSnapshot.scene.get(),
        width,
        height,
        m_ModelSnapshot.camera.get(),
        m_ModelSnapshot.viewIdBase,
        false,
        m_ModelSnapshot.clearColor);
    Scene::CurrentScene = prevScene;
    if (!bgfx::isValid(tex)) {
        Logger::LogError("[ProjectPanel] Failed to render model snapshot.");
        Renderer::Get().ReleaseOffscreenTarget(m_ModelSnapshot.viewIdBase);
        m_ModelSnapshot.scene.reset();
        m_ModelSnapshot.camera.reset();
        return false;
    }

    const bgfx::Caps* caps = bgfx::getCaps();
    if (!caps || (caps->supported & BGFX_CAPS_TEXTURE_BLIT) == 0 || (caps->supported & BGFX_CAPS_TEXTURE_READ_BACK) == 0) {
        Logger::LogError("[ProjectPanel] Snapshot readback not supported on this renderer.");
        Renderer::Get().ReleaseOffscreenTarget(m_ModelSnapshot.viewIdBase);
        m_ModelSnapshot.scene.reset();
        m_ModelSnapshot.camera.reset();
        return false;
    }

    if (bgfx::isValid(m_ModelSnapshot.readbackTexture)) {
        bgfx::destroy(m_ModelSnapshot.readbackTexture);
        m_ModelSnapshot.readbackTexture = BGFX_INVALID_HANDLE;
    }
    const uint64_t readbackFlags = BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST;
    m_ModelSnapshot.readbackTexture = bgfx::createTexture2D(
        (uint16_t)width,
        (uint16_t)height,
        false,
        1,
        bgfx::TextureFormat::RGBA8,
        readbackFlags);
    if (!bgfx::isValid(m_ModelSnapshot.readbackTexture)) {
        Logger::LogError("[ProjectPanel] Failed to create readback texture for snapshot.");
        Renderer::Get().ReleaseOffscreenTarget(m_ModelSnapshot.viewIdBase);
        m_ModelSnapshot.scene.reset();
        m_ModelSnapshot.camera.reset();
        return false;
    }

    const uint16_t blitViewId = (uint16_t)(m_ModelSnapshot.viewIdBase + 2);
    bgfx::setViewRect(blitViewId, 0, 0, (uint16_t)width, (uint16_t)height);
    bgfx::touch(blitViewId);
    bgfx::blit(blitViewId, m_ModelSnapshot.readbackTexture, 0, 0, tex, 0, 0, (uint16_t)width, (uint16_t)height);

    m_ModelSnapshot.readbackBuffer.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0u);
    m_ModelSnapshot.pendingFrame = bgfx::readTexture(m_ModelSnapshot.readbackTexture, m_ModelSnapshot.readbackBuffer.data());
    if (m_ModelSnapshot.pendingFrame == std::numeric_limits<uint32_t>::max()) {
        Logger::LogError("[ProjectPanel] Failed to request snapshot readback.");
        Renderer::Get().ReleaseOffscreenTarget(m_ModelSnapshot.viewIdBase);
        if (bgfx::isValid(m_ModelSnapshot.readbackTexture)) {
            bgfx::destroy(m_ModelSnapshot.readbackTexture);
            m_ModelSnapshot.readbackTexture = BGFX_INVALID_HANDLE;
        }
        m_ModelSnapshot.scene.reset();
        m_ModelSnapshot.camera.reset();
        m_ModelSnapshot.readbackBuffer.clear();
        return false;
    }

    m_ModelSnapshot.pendingStartFrame = Renderer::Get().GetLastSubmittedFrame();
    m_ModelSnapshot.pendingReadback = true;
    return true;
}

void ProjectPanel::UpdateModelSnapshot() {
    if (!m_ModelSnapshot.pendingReadback) return;
    const uint32_t currentFrame = Renderer::Get().GetLastSubmittedFrame();
    if (m_ModelSnapshot.pendingFrame > currentFrame) {
        const uint32_t framesWaited = currentFrame >= m_ModelSnapshot.pendingStartFrame
            ? (currentFrame - m_ModelSnapshot.pendingStartFrame)
            : 0u;
        if (framesWaited > 120) {
            Logger::LogError("[ProjectPanel] Snapshot readback timed out.");
            Renderer::Get().ReleaseOffscreenTarget(m_ModelSnapshot.viewIdBase);
            if (bgfx::isValid(m_ModelSnapshot.readbackTexture)) {
                bgfx::destroy(m_ModelSnapshot.readbackTexture);
                m_ModelSnapshot.readbackTexture = BGFX_INVALID_HANDLE;
            }
            m_ModelSnapshot.scene.reset();
            m_ModelSnapshot.camera.reset();
            m_ModelSnapshot.readbackBuffer.clear();
            m_ModelSnapshot.modelRoot = INVALID_ENTITY_ID;
            m_ModelSnapshot.pendingReadback = false;
        }
        return;
    }

    m_ModelSnapshot.pendingReadback = false;

    if (bgfx::getCaps() && bgfx::getCaps()->originBottomLeft) {
        snapshot_utils::FlipImageVertical(m_ModelSnapshot.readbackBuffer.data(),
                          static_cast<int>(m_ModelSnapshot.width),
                          static_cast<int>(m_ModelSnapshot.height),
                          4);
    }

    bool hasContent = snapshot_utils::ApplySnapshotAlphaKey(
        m_ModelSnapshot.readbackBuffer.data(),
        static_cast<int>(m_ModelSnapshot.width),
        static_cast<int>(m_ModelSnapshot.height),
        m_ModelSnapshot.clearKey[0],
        m_ModelSnapshot.clearKey[1],
        m_ModelSnapshot.clearKey[2]);
    if (!hasContent) {
        Logger::LogError("[ProjectPanel] Snapshot render produced no visible pixels.");
        const size_t pixelCount = static_cast<size_t>(m_ModelSnapshot.width) * static_cast<size_t>(m_ModelSnapshot.height);
        for (size_t i = 0; i < pixelCount; ++i) {
            m_ModelSnapshot.readbackBuffer[i * 4 + 3] = 255;
        }
    }

    if (SavePNG(m_ModelSnapshot.outputPath,
                m_ModelSnapshot.readbackBuffer.data(),
                static_cast<int>(m_ModelSnapshot.width),
                static_cast<int>(m_ModelSnapshot.height),
                4)) {
        Logger::Log("[ProjectPanel] Saved model snapshot: " + m_ModelSnapshot.outputPath);
        m_ProjectRoot = BuildFileTree(m_ProjectPath);
        m_FileListCacheDirty = true;

        try {
            fs::path p(m_ModelSnapshot.outputPath);
            fs::path metaPath = p; metaPath += ".meta";
            AssetMetadata meta;
            bool hasMeta = false;
            if (fs::exists(metaPath)) {
                std::ifstream in(metaPath.string());
                if (in) {
                    nlohmann::json j;
                    in >> j;
                    meta = j.get<AssetMetadata>();
                    hasMeta = true;
                }
            }
            if (!hasMeta) {
                meta.guid = ClaymoreGUID::Generate();
                meta.type = "texture";
                nlohmann::json j = meta;
                std::ofstream out(metaPath.string());
                out << j.dump(4);
            }
            std::string name = p.filename().string();
            std::error_code ec;
            fs::path rel = fs::relative(p, Project::GetProjectDirectory(), ec);
            std::string vpath = (ec ? p.string() : rel.string());
            std::replace(vpath.begin(), vpath.end(), '\\', '/');
            size_t pos = vpath.find("assets/");
            if (pos != std::string::npos) vpath = vpath.substr(pos);
            AssetType t = AssetType::Texture;
            AssetReference aref(meta.guid, 0, (int)t);
            AssetLibrary::Instance().RegisterAsset(aref, t, vpath, name);
            AssetLibrary::Instance().RegisterPathAlias(meta.guid, m_ModelSnapshot.outputPath);
        } catch (...) {
            std::cerr << "[ProjectPanel] Failed to register snapshot asset" << std::endl;
        }
    } else {
        Logger::LogError("[ProjectPanel] Failed to save model snapshot: " + m_ModelSnapshot.outputPath);
    }

    Renderer::Get().ReleaseOffscreenTarget(m_ModelSnapshot.viewIdBase);
    if (bgfx::isValid(m_ModelSnapshot.readbackTexture)) {
        bgfx::destroy(m_ModelSnapshot.readbackTexture);
        m_ModelSnapshot.readbackTexture = BGFX_INVALID_HANDLE;
    }
    m_ModelSnapshot.scene.reset();
    m_ModelSnapshot.camera.reset();
    m_ModelSnapshot.readbackBuffer.clear();
    m_ModelSnapshot.modelRoot = INVALID_ENTITY_ID;
}

// ============================================================================
// Prefab Snapshot (with particle system warmup support)
// ============================================================================

// Helper: recursively check if entity hierarchy contains particle emitters
static bool HierarchyHasParticleEmitters(Scene& scene, EntityID entityId) {
    EntityData* data = scene.GetEntityData(entityId);
    if (!data) return false;
    
    if (data->Emitter && data->Emitter->Enabled) {
        return true;
    }
    
    for (EntityID childId : data->Children) {
        if (HierarchyHasParticleEmitters(scene, childId)) {
            return true;
        }
    }
    return false;
}

// Helper: compute target warmup time for particle peak emission
static float ComputeParticleWarmupTime(Scene& scene, EntityID entityId) {
    float maxWarmup = 0.0f;
    
    std::function<void(EntityID)> traverse = [&](EntityID id) {
        EntityData* data = scene.GetEntityData(id);
        if (!data) return;
        
        if (data->Emitter && data->Emitter->Enabled) {
            const auto& emitter = *data->Emitter;
            // For peak emission, we want particles to have accumulated
            // Use min of Duration and average Lifetime for looping, or a reasonable time for non-looping
            float lifetime = (emitter.Lifetime.Min + emitter.Lifetime.Max) * 0.5f;
            float targetTime = emitter.Looping 
                ? std::min(emitter.Duration, lifetime)
                : std::min(emitter.Duration * 0.75f, lifetime);
            // Add a bit extra to ensure particles are well-distributed
            targetTime = std::max(targetTime, 0.5f);
            maxWarmup = std::max(maxWarmup, targetTime);
        }
        
        for (EntityID childId : data->Children) {
            traverse(childId);
        }
    };
    
    traverse(entityId);
    return maxWarmup;
}

// Helper: trigger all particle emitters in hierarchy to start playing
static void TriggerParticleEmitters(Scene& scene, EntityID entityId) {
    std::function<void(EntityID)> traverse = [&](EntityID id) {
        EntityData* data = scene.GetEntityData(id);
        if (!data) return;
        
        if (data->Emitter && data->Emitter->Enabled) {
            data->Emitter->Play();
        }
        
        for (EntityID childId : data->Children) {
            traverse(childId);
        }
    };
    
    traverse(entityId);
}

// Helper: estimate the bounding box of a particle emitter based on emission shape and particle travel
static void ComputeParticleEmitterBounds(const ParticleEmitterComponent& emitter, const glm::mat4& worldMatrix, 
                                         glm::vec3& outMin, glm::vec3& outMax) {
    glm::vec3 worldPos = glm::vec3(worldMatrix[3]);
    
    // Start with emission shape bounds
    float shapeRadius = 0.0f;
    glm::vec3 shapeExtent(0.0f);
    
    switch (emitter.Shape) {
        case ParticleEmissionShape::Point:
            shapeRadius = 0.0f;
            break;
        case ParticleEmissionShape::Sphere:
        case ParticleEmissionShape::Hemisphere:
        case ParticleEmissionShape::Circle:
        case ParticleEmissionShape::Disc:
            shapeRadius = emitter.ShapeRadius;
            break;
        case ParticleEmissionShape::Cone:
            // Cone expands outward - use radius at base
            shapeRadius = emitter.ShapeRadius;
            break;
        case ParticleEmissionShape::Box:
        case ParticleEmissionShape::Rectangle:
            shapeExtent = emitter.ShapeScale * 0.5f;
            break;
        case ParticleEmissionShape::Edge:
            shapeExtent.x = emitter.ShapeLength * 0.5f;
            break;
    }
    
    // Calculate how far particles can travel during their lifetime
    // Use MAX lifetime and MAX start speed to ensure we capture all particles
    float maxLifetime = emitter.Lifetime.Max;
    float maxSpeed = emitter.StartSpeed.Max;
    float travelDistance = maxSpeed * maxLifetime;
    
    // Account for gravity - particles can fall during their lifetime
    // Gravity pulls downward, so extend bounds in -Y direction
    float gravityDrop = 0.5f * std::abs(emitter.GravityModifier) * 9.81f * maxLifetime * maxLifetime;
    
    // Account for velocity over lifetime if enabled
    glm::vec3 velocityExtent(0.0f);
    if (emitter.VelocityOverLifetimeEnabled) {
        velocityExtent = glm::abs(emitter.LinearVelocity) * maxLifetime;
    }
    
    // Combine all extents - use travel distance as the primary extent
    // Particles can travel in any direction from emission point
    float totalRadius = shapeRadius + travelDistance;
    glm::vec3 totalExtent = shapeExtent + velocityExtent + glm::vec3(totalRadius);
    
    // Add extra extent for gravity drop (mainly in Y)
    totalExtent.y += gravityDrop;
    
    // Also consider particle size at max
    float maxSize = emitter.StartSize.Max;
    if (emitter.SizeOverLifetimeEnabled) {
        maxSize *= std::max(emitter.SizeOverLifetime.StartValue, emitter.SizeOverLifetime.EndValue);
    }
    totalExtent += glm::vec3(maxSize * 0.5f);
    
    // Apply a generous multiplier to ensure we capture most particles
    // Particles spread in 3D so we want extra margin
    totalExtent *= 1.5f;
    
    // Ensure a minimum extent so camera isn't too close
    totalExtent = glm::max(totalExtent, glm::vec3(2.0f));
    
    outMin = worldPos - totalExtent;
    outMax = worldPos + totalExtent;
    
    // Adjust for gravity direction (particles fall down)
    if (emitter.GravityModifier > 0.0f) {
        outMin.y -= gravityDrop;
    } else if (emitter.GravityModifier < 0.0f) {
        outMax.y += gravityDrop;
    }
}

// Helper: compute world bounds of an entity hierarchy (including particle emitters)
static void ComputePrefabWorldBounds(Scene& scene, EntityID rootId, glm::vec3& outMin, glm::vec3& outMax) {
    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(std::numeric_limits<float>::lowest());
    bool foundMesh = false;
    bool foundEmitter = false;
    
    // Separate tracking for mesh bounds and emitter bounds
    glm::vec3 meshMin = outMin, meshMax = outMax;
    glm::vec3 emitterMin = outMin, emitterMax = outMax;
    
    std::function<void(EntityID)> traverse = [&](EntityID id) {
        EntityData* data = scene.GetEntityData(id);
        if (!data) return;
        
        // Include this entity's position in bounds
        glm::vec3 worldPos = glm::vec3(data->Transform.WorldMatrix[3]);
        
        if (data->Mesh && data->Mesh->mesh) {
            // If has mesh, use mesh bounds transformed
            const glm::vec3 localMin = data->Mesh->mesh->BoundsMin;
            const glm::vec3 localMax = data->Mesh->mesh->BoundsMax;
            
            // Only use bounds if they're valid (max > min)
            if (localMax.x > localMin.x && localMax.y > localMin.y && localMax.z > localMin.z) {
                // Transform corners
                glm::vec3 corners[8] = {
                    {localMin.x, localMin.y, localMin.z},
                    {localMax.x, localMin.y, localMin.z},
                    {localMin.x, localMax.y, localMin.z},
                    {localMax.x, localMax.y, localMin.z},
                    {localMin.x, localMin.y, localMax.z},
                    {localMax.x, localMin.y, localMax.z},
                    {localMin.x, localMax.y, localMax.z},
                    {localMax.x, localMax.y, localMax.z}
                };
                
                for (const auto& corner : corners) {
                    glm::vec4 worldCorner = data->Transform.WorldMatrix * glm::vec4(corner, 1.0f);
                    meshMin = glm::min(meshMin, glm::vec3(worldCorner));
                    meshMax = glm::max(meshMax, glm::vec3(worldCorner));
                }
                foundMesh = true;
            }
        }
        
        // Check for particle emitter and compute its estimated bounds
        if (data->Emitter && data->Emitter->Enabled) {
            glm::vec3 emMin, emMax;
            ComputeParticleEmitterBounds(*data->Emitter, data->Transform.WorldMatrix, emMin, emMax);
            emitterMin = glm::min(emitterMin, emMin);
            emitterMax = glm::max(emitterMax, emMax);
            foundEmitter = true;
        }
        
        for (EntityID childId : data->Children) {
            traverse(childId);
        }
    };
    
    traverse(rootId);
    
    // Determine which bounds to use
    if (foundMesh && foundEmitter) {
        // Have both mesh and emitters - combine bounds but prioritize mesh for framing
        // Include emitter bounds but don't let them dominate if mesh is present
        outMin = glm::min(meshMin, emitterMin);
        outMax = glm::max(meshMax, emitterMax);
    } else if (foundMesh) {
        // Only mesh - use mesh bounds
        outMin = meshMin;
        outMax = meshMax;
    } else if (foundEmitter) {
        // Only emitters (particle-only prefab) - use emitter bounds
        outMin = emitterMin;
        outMax = emitterMax;
    } else {
        // Nothing found - use origin
        outMin = outMax = glm::vec3(0.0f);
    }
}

void ProjectPanel::DrawPrefabSnapshotPopup() {
    if (m_PrefabSnapshot.popupRequested) {
        ImGui::OpenPopup("Prefab Snapshot");
        m_PrefabSnapshot.popupRequested = false;
        m_PrefabSnapshot.popupJustOpened = true;
    }
    if (ImGui::BeginPopupModal("Prefab Snapshot", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string prefabName = m_PrefabSnapshot.prefabPath.empty()
            ? std::string("<none>")
            : fs::path(m_PrefabSnapshot.prefabPath).filename().string();
        ImGui::Text("Prefab: %s", prefabName.c_str());
        ImGui::Separator();

        if (m_PrefabSnapshot.popupJustOpened) {
            ImGui::SetKeyboardFocusHere();
            m_PrefabSnapshot.popupJustOpened = false;
        }

        ImGui::PushItemWidth(ImGui::GetFontSize() * 14.0f);
        ImGui::DragFloat3("Rotation (deg)", &m_PrefabSnapshot.rotation.x, 1.0f);
        ImGui::DragFloat3("Scale", &m_PrefabSnapshot.scale.x, 0.01f, 0.001f, 1000.0f);
        ImGui::PopItemWidth();

        ImGui::Checkbox("Override Resolution", &m_PrefabSnapshot.overrideResolution);
        if (!m_PrefabSnapshot.overrideResolution) {
            ImGui::TextDisabled("Resolution: 128 x 128");
        }
        ImGui::BeginDisabled(!m_PrefabSnapshot.overrideResolution);
        ImGui::PushItemWidth(ImGui::GetFontSize() * 10.0f);
        ImGui::InputInt2("Resolution", m_PrefabSnapshot.resolution);
        ImGui::PopItemWidth();
        ImGui::EndDisabled();

        if (m_PrefabSnapshot.pendingReadback || m_PrefabSnapshot.simulatingParticles) {
            if (m_PrefabSnapshot.simulatingParticles) {
                ImGui::TextDisabled("Warming up particles... (%.1f / %.1f s)", 
                    m_PrefabSnapshot.particleWarmupTime, 
                    m_PrefabSnapshot.particleTargetWarmup);
            } else {
                ImGui::TextDisabled("Snapshot in progress...");
            }
        }

        bool canCapture = !m_PrefabSnapshot.prefabPath.empty() && 
                          !m_PrefabSnapshot.pendingReadback && 
                          !m_PrefabSnapshot.simulatingParticles;
        if (!canCapture) ImGui::BeginDisabled();
        if (ImGui::Button("Capture")) {
            if (StartPrefabSnapshotCapture()) {
                // Don't close if we're simulating particles - will close when done
                if (!m_PrefabSnapshot.simulatingParticles) {
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        if (!canCapture) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            // Clean up any in-progress simulation
            if (m_PrefabSnapshot.scene) {
                ecs::ParticleEmitterSystem::Get().ClearSceneEmitters(m_PrefabSnapshot.scene.get());
                m_PrefabSnapshot.scene.reset();
            }
            m_PrefabSnapshot.camera.reset();
            m_PrefabSnapshot.simulatingParticles = false;
            m_PrefabSnapshot.pendingReadback = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

bool ProjectPanel::StartPrefabSnapshotCapture() {
    if (m_PrefabSnapshot.pendingReadback || m_PrefabSnapshot.simulatingParticles) {
        Logger::LogError("[ProjectPanel] Prefab snapshot already in progress.");
        return false;
    }
    if (m_PrefabSnapshot.prefabPath.empty()) {
        Logger::LogError("[ProjectPanel] No prefab path specified for snapshot.");
        return false;
    }

    uint32_t width = 128;
    uint32_t height = 128;
    if (m_PrefabSnapshot.overrideResolution) {
        width = static_cast<uint32_t>(std::max(1, m_PrefabSnapshot.resolution[0]));
        height = static_cast<uint32_t>(std::max(1, m_PrefabSnapshot.resolution[1]));
    }
    m_PrefabSnapshot.width = width;
    m_PrefabSnapshot.height = height;

    // Setup output path
    fs::path outDir = fs::path(m_PrefabSnapshot.prefabPath).parent_path() / "snapshots";
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) {
        Logger::LogError("[ProjectPanel] Failed to create snapshot directory: " + outDir.string());
        return false;
    }
    std::string stem = fs::path(m_PrefabSnapshot.prefabPath).stem().string();
    fs::path outPath = outDir / (stem + "_snapshot.png");
    m_PrefabSnapshot.outputPath = outPath.string();

    // Create temporary scene for snapshot
    m_PrefabSnapshot.scene = std::make_unique<Scene>();
    m_PrefabSnapshot.camera = std::make_unique<Camera>(60.0f, 1.0f, 0.1f, 1000.0f);

    // Setup environment
    Environment& env = m_PrefabSnapshot.scene->GetEnvironment();
    env.ProceduralSky = false;
    env.UseSkybox = false;
    env.EnableFog = false;
    env.OutlineEnabled = false;
    env.AmbientColor = glm::vec3(0.6f);
    env.AmbientIntensity = 1.2f;

    // Add lighting
    Entity keyLight = m_PrefabSnapshot.scene->CreateLight("Snapshot Key", LightType::Directional, glm::vec3(1.0f), 1.25f);
    if (auto* lightData = m_PrefabSnapshot.scene->GetEntityData(keyLight.GetID())) {
        lightData->Transform.Rotation = glm::vec3(-35.0f, 45.0f, 0.0f);
        lightData->Transform.UseQuatRotation = false;
        lightData->Transform.TransformDirty = true;
    }
    Entity fillLight = m_PrefabSnapshot.scene->CreateLight("Snapshot Fill", LightType::Directional, glm::vec3(0.9f), 0.65f);
    if (auto* fillData = m_PrefabSnapshot.scene->GetEntityData(fillLight.GetID())) {
        fillData->Transform.Rotation = glm::vec3(-15.0f, -135.0f, 0.0f);
        fillData->Transform.UseQuatRotation = false;
        fillData->Transform.TransformDirty = true;
    }

    // Instantiate the prefab
    m_PrefabSnapshot.prefabRoot = InstantiatePrefabFromPathBlocking(m_PrefabSnapshot.prefabPath, *m_PrefabSnapshot.scene);
    if (m_PrefabSnapshot.prefabRoot == INVALID_ENTITY_ID) {
        Logger::LogError("[ProjectPanel] Failed to instantiate prefab for snapshot: " + m_PrefabSnapshot.prefabPath);
        m_PrefabSnapshot.scene.reset();
        m_PrefabSnapshot.camera.reset();
        return false;
    }

    // Apply user transformations
    if (auto* data = m_PrefabSnapshot.scene->GetEntityData(m_PrefabSnapshot.prefabRoot)) {
        glm::vec3 safeScale = glm::max(m_PrefabSnapshot.scale, glm::vec3(0.001f));
        data->Transform.Rotation = m_PrefabSnapshot.rotation;
        data->Transform.UseQuatRotation = false;
        data->Transform.Scale = safeScale;
        data->Transform.TransformDirty = true;
    }
    m_PrefabSnapshot.scene->MarkTransformDirty(m_PrefabSnapshot.prefabRoot);
    m_PrefabSnapshot.scene->UpdateTransforms();
    m_PrefabSnapshot.scene->ProcessBoneAttachments();
    SkinningSystem::Update(*m_PrefabSnapshot.scene);

    // Check for particle emitters in hierarchy
    m_PrefabSnapshot.hasParticleEmitters = HierarchyHasParticleEmitters(*m_PrefabSnapshot.scene, m_PrefabSnapshot.prefabRoot);
    
    if (m_PrefabSnapshot.hasParticleEmitters) {
        // Initialize particle system for this scene and trigger emitters
        m_PrefabSnapshot.particleTargetWarmup = ComputeParticleWarmupTime(*m_PrefabSnapshot.scene, m_PrefabSnapshot.prefabRoot);
        m_PrefabSnapshot.particleWarmupTime = 0.0f;
        m_PrefabSnapshot.simulatingParticles = true;
        
        // Trigger all particle emitters to start playing
        TriggerParticleEmitters(*m_PrefabSnapshot.scene, m_PrefabSnapshot.prefabRoot);
        
        Logger::Log("[ProjectPanel] Prefab has particle emitters, warming up for " + 
                   std::to_string(m_PrefabSnapshot.particleTargetWarmup) + " seconds...");
        return true;  // Will continue in UpdatePrefabSnapshot
    }
    
    // No particles, capture immediately
    return CapturePrefabSnapshot();
}

// Internal: Actually capture the snapshot (called after particle warmup if needed)
bool ProjectPanel::CapturePrefabSnapshot() {
    uint32_t width = m_PrefabSnapshot.width;
    uint32_t height = m_PrefabSnapshot.height;

    // Compute bounds and setup camera
    glm::vec3 boundsMin, boundsMax;
    ComputePrefabWorldBounds(*m_PrefabSnapshot.scene, m_PrefabSnapshot.prefabRoot, boundsMin, boundsMax);
    glm::vec3 center = 0.5f * (boundsMin + boundsMax);
    glm::vec3 extents = 0.5f * (boundsMax - boundsMin);
    extents = glm::max(extents, glm::vec3(0.001f));
    snapshot_utils::SnapshotViewFit viewFit = snapshot_utils::ComputeSnapshotViewFit(extents);
    float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const float fovDegrees = 60.0f;
    const float halfFov = glm::radians(fovDegrees * 0.5f);
    const float distHeight = viewFit.halfHeight / std::tan(halfFov);
    const float distWidth = viewFit.halfWidth / (std::tan(halfFov) * aspect);
    float distance = std::max(distHeight, distWidth) * 1.15f;
    float radius = std::max(extents.x, std::max(extents.y, extents.z));
    float farClip = std::max(100.0f, distance + radius * 4.0f);

    m_PrefabSnapshot.camera->SetPerspective(fovDegrees, aspect, 0.1f, farClip);
    m_PrefabSnapshot.camera->SetViewportSize(static_cast<float>(width), static_cast<float>(height));
    m_PrefabSnapshot.camera->SetPosition(center + viewFit.viewDir * distance);
    m_PrefabSnapshot.camera->LookAt(center, viewFit.upDir);

    // Use black clear color - particles typically use additive blending so they show up well on black
    // BGFX clear color format is RGBA (0xRRGGBBAA)
    m_PrefabSnapshot.clearColor = 0x000000FF;  // RGBA: Black with full alpha
    m_PrefabSnapshot.clearKey[0] = 0x00;       // R
    m_PrefabSnapshot.clearKey[1] = 0x00;       // G  
    m_PrefabSnapshot.clearKey[2] = 0x00;       // B

    // Update particle system one more time right before capture
    if (m_PrefabSnapshot.hasParticleEmitters) {
        m_PrefabSnapshot.scene->UpdateTransforms();
        ecs::ParticleEmitterSystem::Get().Update(*m_PrefabSnapshot.scene, 0.0f);  // Zero dt to maintain state
    }


    Scene* prevScene = Scene::CurrentScene;
    Scene::CurrentScene = m_PrefabSnapshot.scene.get();
    bgfx::TextureHandle tex = Renderer::Get().RenderSceneToTexture(
        m_PrefabSnapshot.scene.get(),
        width,
        height,
        m_PrefabSnapshot.camera.get(),
        m_PrefabSnapshot.viewIdBase,
        false,
        m_PrefabSnapshot.clearColor);
    
    // For prefabs with particles, render particles directly without scene filtering
    // The scene-filtered render doesn't work reliably for isolated snapshot scenes
    if (m_PrefabSnapshot.hasParticleEmitters) {
        glm::mat4 viewMat = m_PrefabSnapshot.camera->GetViewMatrix();
        glm::vec3 camPos = m_PrefabSnapshot.camera->GetPosition();
        bx::Vec3 eye = { camPos.x, camPos.y, camPos.z };
        
        // Render all particles to the same view
        ecs::ParticleEmitterSystem::Get().RenderAllUnfiltered(
            m_PrefabSnapshot.viewIdBase, 
            glm::value_ptr(viewMat), 
            eye);
    }
    
    Scene::CurrentScene = prevScene;
    if (!bgfx::isValid(tex)) {
        Logger::LogError("[ProjectPanel] Failed to render prefab snapshot.");
        Renderer::Get().ReleaseOffscreenTarget(m_PrefabSnapshot.viewIdBase);
        ecs::ParticleEmitterSystem::Get().ClearSceneEmitters(m_PrefabSnapshot.scene.get());
        m_PrefabSnapshot.scene.reset();
        m_PrefabSnapshot.camera.reset();
        return false;
    }

    const bgfx::Caps* caps = bgfx::getCaps();
    if (!caps || (caps->supported & BGFX_CAPS_TEXTURE_BLIT) == 0 || (caps->supported & BGFX_CAPS_TEXTURE_READ_BACK) == 0) {
        Logger::LogError("[ProjectPanel] Prefab snapshot readback not supported on this renderer.");
        Renderer::Get().ReleaseOffscreenTarget(m_PrefabSnapshot.viewIdBase);
        ecs::ParticleEmitterSystem::Get().ClearSceneEmitters(m_PrefabSnapshot.scene.get());
        m_PrefabSnapshot.scene.reset();
        m_PrefabSnapshot.camera.reset();
        return false;
    }

    if (bgfx::isValid(m_PrefabSnapshot.readbackTexture)) {
        bgfx::destroy(m_PrefabSnapshot.readbackTexture);
        m_PrefabSnapshot.readbackTexture = BGFX_INVALID_HANDLE;
    }
    const uint64_t readbackFlags = BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST;
    m_PrefabSnapshot.readbackTexture = bgfx::createTexture2D(
        (uint16_t)width,
        (uint16_t)height,
        false,
        1,
        bgfx::TextureFormat::RGBA8,
        readbackFlags);
    if (!bgfx::isValid(m_PrefabSnapshot.readbackTexture)) {
        Logger::LogError("[ProjectPanel] Failed to create readback texture for prefab snapshot.");
        Renderer::Get().ReleaseOffscreenTarget(m_PrefabSnapshot.viewIdBase);
        ecs::ParticleEmitterSystem::Get().ClearSceneEmitters(m_PrefabSnapshot.scene.get());
        m_PrefabSnapshot.scene.reset();
        m_PrefabSnapshot.camera.reset();
        return false;
    }

    const uint16_t blitViewId = (uint16_t)(m_PrefabSnapshot.viewIdBase + 2);
    bgfx::setViewRect(blitViewId, 0, 0, (uint16_t)width, (uint16_t)height);
    bgfx::touch(blitViewId);
    bgfx::blit(blitViewId, m_PrefabSnapshot.readbackTexture, 0, 0, tex, 0, 0, (uint16_t)width, (uint16_t)height);

    m_PrefabSnapshot.readbackBuffer.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0u);
    m_PrefabSnapshot.pendingFrame = bgfx::readTexture(m_PrefabSnapshot.readbackTexture, m_PrefabSnapshot.readbackBuffer.data());
    if (m_PrefabSnapshot.pendingFrame == std::numeric_limits<uint32_t>::max()) {
        Logger::LogError("[ProjectPanel] Failed to request prefab snapshot readback.");
        Renderer::Get().ReleaseOffscreenTarget(m_PrefabSnapshot.viewIdBase);
        if (bgfx::isValid(m_PrefabSnapshot.readbackTexture)) {
            bgfx::destroy(m_PrefabSnapshot.readbackTexture);
            m_PrefabSnapshot.readbackTexture = BGFX_INVALID_HANDLE;
        }
        ecs::ParticleEmitterSystem::Get().ClearSceneEmitters(m_PrefabSnapshot.scene.get());
        m_PrefabSnapshot.scene.reset();
        m_PrefabSnapshot.camera.reset();
        m_PrefabSnapshot.readbackBuffer.clear();
        return false;
    }

    m_PrefabSnapshot.pendingStartFrame = Renderer::Get().GetLastSubmittedFrame();
    m_PrefabSnapshot.pendingReadback = true;
    m_PrefabSnapshot.simulatingParticles = false;
    return true;
}

void ProjectPanel::UpdatePrefabSnapshot() {
    // Handle particle warmup simulation
    if (m_PrefabSnapshot.simulatingParticles && m_PrefabSnapshot.scene && m_PrefabSnapshot.camera) {
        // Use frame delta time for proper simulation
        const float dt = 1.0f / 60.0f;  // Fixed timestep for consistent particle simulation
        
        // Update transforms
        m_PrefabSnapshot.scene->UpdateTransforms();
        m_PrefabSnapshot.scene->ProcessBoneAttachments();
        SkinningSystem::Update(*m_PrefabSnapshot.scene);
        
        // Update particle emitters (this also calls ps::update which steps the simulation)
        ecs::ParticleEmitterSystem::Get().Update(*m_PrefabSnapshot.scene, dt);
        
        // We need to actually render the scene during warmup to update GPU-side particle data
        // and ensure billboards are generated. Render to offscreen target (we don't save this).
        uint32_t width = m_PrefabSnapshot.width;
        uint32_t height = m_PrefabSnapshot.height;
        
        // Set up camera framing based on bounds (only on first warmup frame)
        if (m_PrefabSnapshot.particleWarmupTime < dt) {
            glm::vec3 boundsMin, boundsMax;
            ComputePrefabWorldBounds(*m_PrefabSnapshot.scene, m_PrefabSnapshot.prefabRoot, boundsMin, boundsMax);
            glm::vec3 center = 0.5f * (boundsMin + boundsMax);
            glm::vec3 extents = 0.5f * (boundsMax - boundsMin);
            extents = glm::max(extents, glm::vec3(0.5f));  // Minimum extent for particle-only prefabs
            snapshot_utils::SnapshotViewFit viewFit = snapshot_utils::ComputeSnapshotViewFit(extents);
            float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
            const float fovDegrees = 60.0f;
            const float halfFov = glm::radians(fovDegrees * 0.5f);
            const float distHeight = viewFit.halfHeight / std::tan(halfFov);
            const float distWidth = viewFit.halfWidth / (std::tan(halfFov) * aspect);
            float distance = std::max(distHeight, distWidth) * 1.15f;
            float radius = std::max(extents.x, std::max(extents.y, extents.z));
            float farClip = std::max(100.0f, distance + radius * 4.0f);

            m_PrefabSnapshot.camera->SetPerspective(fovDegrees, aspect, 0.1f, farClip);
            m_PrefabSnapshot.camera->SetViewportSize(static_cast<float>(width), static_cast<float>(height));
            m_PrefabSnapshot.camera->SetPosition(center + viewFit.viewDir * distance);
            m_PrefabSnapshot.camera->LookAt(center, viewFit.upDir);
        }
        
        // Render the scene during warmup to update particle GPU state
        Scene* prevScene = Scene::CurrentScene;
        Scene::CurrentScene = m_PrefabSnapshot.scene.get();
        Renderer::Get().RenderSceneToTexture(
            m_PrefabSnapshot.scene.get(),
            width,
            height,
            m_PrefabSnapshot.camera.get(),
            m_PrefabSnapshot.viewIdBase,
            false,
            0x000000FF);  // RGBA: Black with full alpha - particles show up well with additive blending
        Scene::CurrentScene = prevScene;
        
        m_PrefabSnapshot.particleWarmupTime += dt;
        
        // Check if warmup is complete
        if (m_PrefabSnapshot.particleWarmupTime >= m_PrefabSnapshot.particleTargetWarmup) {
            Logger::Log("[ProjectPanel] Particle warmup complete, capturing snapshot...");
            if (!CapturePrefabSnapshot()) {
                // Capture failed, clean up
                ecs::ParticleEmitterSystem::Get().ClearSceneEmitters(m_PrefabSnapshot.scene.get());
                m_PrefabSnapshot.scene.reset();
                m_PrefabSnapshot.camera.reset();
                m_PrefabSnapshot.simulatingParticles = false;
            }
        }
        return;
    }
    
    if (!m_PrefabSnapshot.pendingReadback) return;
    const uint32_t currentFrame = Renderer::Get().GetLastSubmittedFrame();
    if (m_PrefabSnapshot.pendingFrame > currentFrame) {
        const uint32_t framesWaited = currentFrame >= m_PrefabSnapshot.pendingStartFrame
            ? (currentFrame - m_PrefabSnapshot.pendingStartFrame)
            : 0u;
        if (framesWaited > 120) {
            Logger::LogError("[ProjectPanel] Prefab snapshot readback timed out.");
            Renderer::Get().ReleaseOffscreenTarget(m_PrefabSnapshot.viewIdBase);
            if (bgfx::isValid(m_PrefabSnapshot.readbackTexture)) {
                bgfx::destroy(m_PrefabSnapshot.readbackTexture);
                m_PrefabSnapshot.readbackTexture = BGFX_INVALID_HANDLE;
            }
            ecs::ParticleEmitterSystem::Get().ClearSceneEmitters(m_PrefabSnapshot.scene.get());
            m_PrefabSnapshot.scene.reset();
            m_PrefabSnapshot.camera.reset();
            m_PrefabSnapshot.readbackBuffer.clear();
            m_PrefabSnapshot.prefabRoot = INVALID_ENTITY_ID;
            m_PrefabSnapshot.pendingReadback = false;
        }
        return;
    }

    m_PrefabSnapshot.pendingReadback = false;

    if (bgfx::getCaps() && bgfx::getCaps()->originBottomLeft) {
        snapshot_utils::FlipImageVertical(m_PrefabSnapshot.readbackBuffer.data(),
                          static_cast<int>(m_PrefabSnapshot.width),
                          static_cast<int>(m_PrefabSnapshot.height),
                          4);
    }

    bool hasContent = snapshot_utils::ApplySnapshotAlphaKey(
        m_PrefabSnapshot.readbackBuffer.data(),
        static_cast<int>(m_PrefabSnapshot.width),
        static_cast<int>(m_PrefabSnapshot.height),
        m_PrefabSnapshot.clearKey[0],
        m_PrefabSnapshot.clearKey[1],
        m_PrefabSnapshot.clearKey[2]);
    if (!hasContent) {
        Logger::LogError("[ProjectPanel] Prefab snapshot render produced no visible pixels.");
        const size_t pixelCount = static_cast<size_t>(m_PrefabSnapshot.width) * static_cast<size_t>(m_PrefabSnapshot.height);
        for (size_t i = 0; i < pixelCount; ++i) {
            m_PrefabSnapshot.readbackBuffer[i * 4 + 3] = 255;
        }
    }

    if (SavePNG(m_PrefabSnapshot.outputPath,
                m_PrefabSnapshot.readbackBuffer.data(),
                static_cast<int>(m_PrefabSnapshot.width),
                static_cast<int>(m_PrefabSnapshot.height),
                4)) {
        Logger::Log("[ProjectPanel] Saved prefab snapshot: " + m_PrefabSnapshot.outputPath);
        m_ProjectRoot = BuildFileTree(m_ProjectPath);
        m_FileListCacheDirty = true;

        try {
            fs::path p(m_PrefabSnapshot.outputPath);
            fs::path metaPath = p; metaPath += ".meta";
            AssetMetadata meta;
            bool hasMeta = false;
            if (fs::exists(metaPath)) {
                std::ifstream in(metaPath.string());
                if (in) {
                    nlohmann::json j;
                    in >> j;
                    meta = j.get<AssetMetadata>();
                    hasMeta = true;
                }
            }
            if (!hasMeta) {
                meta.guid = ClaymoreGUID::Generate();
                meta.type = "texture";
                nlohmann::json j = meta;
                std::ofstream out(metaPath.string());
                out << j.dump(4);
            }
            std::string name = p.filename().string();
            std::error_code ec;
            fs::path rel = fs::relative(p, Project::GetProjectDirectory(), ec);
            std::string vpath = (ec ? p.string() : rel.string());
            std::replace(vpath.begin(), vpath.end(), '\\', '/');
            size_t pos = vpath.find("assets/");
            if (pos != std::string::npos) vpath = vpath.substr(pos);
            AssetType t = AssetType::Texture;
            AssetReference aref(meta.guid, 0, (int)t);
            AssetLibrary::Instance().RegisterAsset(aref, t, vpath, name);
            AssetLibrary::Instance().RegisterPathAlias(meta.guid, m_PrefabSnapshot.outputPath);
        } catch (...) {
            std::cerr << "[ProjectPanel] Failed to register prefab snapshot asset" << std::endl;
        }
    } else {
        Logger::LogError("[ProjectPanel] Failed to save prefab snapshot: " + m_PrefabSnapshot.outputPath);
    }

    Renderer::Get().ReleaseOffscreenTarget(m_PrefabSnapshot.viewIdBase);
    if (bgfx::isValid(m_PrefabSnapshot.readbackTexture)) {
        bgfx::destroy(m_PrefabSnapshot.readbackTexture);
        m_PrefabSnapshot.readbackTexture = BGFX_INVALID_HANDLE;
    }
    ecs::ParticleEmitterSystem::Get().ClearSceneEmitters(m_PrefabSnapshot.scene.get());
    m_PrefabSnapshot.scene.reset();
    m_PrefabSnapshot.camera.reset();
    m_PrefabSnapshot.readbackBuffer.clear();
    m_PrefabSnapshot.prefabRoot = INVALID_ENTITY_ID;
}

bool ProjectPanel::LoadHumanoidAvatarForModel(const std::filesystem::path& modelPath,
                                              cm::animation::AvatarDefinition& outAvatar) const {
    namespace fs = std::filesystem;
    fs::path avatarPath = modelPath;
    avatarPath.replace_extension(".avatar");
    if (!fs::exists(avatarPath)) {
        return false;
    }
    if (!cm::animation::LoadAvatar(outAvatar, avatarPath.string())) {
        return false;
    }
    if (outAvatar.Map.size() < cm::animation::HumanoidBoneCount) {
        return false;
    }
    if (outAvatar.Present.size() < cm::animation::HumanoidBoneCount) {
        outAvatar.Present.resize(cm::animation::HumanoidBoneCount, false);
    }
    for (uint16_t i = 0; i < cm::animation::HumanoidBoneCount; ++i) {
        cm::animation::HumanoidBone bone = static_cast<cm::animation::HumanoidBone>(i);
        if (!cm::animation::IsHumanoidBoneRequired(bone)) {
            continue;
        }
        bool mapped = i < outAvatar.Map.size() && outAvatar.Map[i].BoneIndex >= 0;
        bool present = i < outAvatar.Present.size() ? outAvatar.Present[i] : mapped;
        if (!mapped || !present) {
            return false;
        }
    }
    return true;
}

bool ProjectPanel::ReimportModelAsset(const std::filesystem::directory_entry& entry,
                                      bool slicedHumanoid,
                                      const cm::animation::AvatarDefinition* avatarOverride) {
    try {
        std::string src = entry.path().string();
        std::string name = entry.path().filename().string();
        std::error_code ec;
        fs::path rel = fs::relative(entry.path(), Project::GetProjectDirectory(), ec);
        std::string vpath = (ec ? src : rel.string());
        std::replace(vpath.begin(), vpath.end(), '\\', '/');
        size_t pos = vpath.find("assets/");
        if (pos != std::string::npos) vpath = vpath.substr(pos);

        // Invalidate any cached mesh data for this model before reimporting
        AssetLibrary::Instance().InvalidateMesh(src);
        AssetLibrary::Instance().InvalidateMesh(vpath);

        ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(vpath);
        if (guid.high == 0 && guid.low == 0) {
            guid = ClaymoreGUID::Generate();
        }
        AssetLibrary::Instance().RegisterAsset(AssetReference(guid, 0, (int)AssetType::Mesh), AssetType::Mesh, vpath, name);
        AssetLibrary::Instance().RegisterPathAlias(guid, src);

        BuiltModelPaths built{};
        bool builtOk = false;
        if (slicedHumanoid) {
            if (!avatarOverride) {
                std::cerr << "[Reimport] Humanoid slice requested without avatar." << std::endl;
                return false;
            }
            builtOk = BuildHumanoidSlicedModelCacheBlocking(src, *avatarOverride, built);
        } else {
            builtOk = BuildModelCacheBlocking(src, built);
        }
        if (!builtOk) {
            std::cerr << "[Reimport] Build failed for: " << src << std::endl;
            return false;
        }

        try {
            nlohmann::json jm;
            std::ifstream in(built.metaPath);
            if (in) { in >> jm; in.close(); }
            jm["guid"] = guid.ToString();
            std::ofstream out(built.metaPath);
            if (out) out << jm.dump(4);
        } catch(...) {}

        try {
            Model m = ModelLoader::LoadModel(src);
            if (!m.BoneNames.empty() && !m.InverseBindPoses.empty()) {
                SkeletonComponent sk;
                sk.InverseBindPoses = m.InverseBindPoses;
                sk.BoneParents = m.BoneParents;
                sk.BoneNames = m.BoneNames;
                sk.BoneNameToIndex.clear();
                for (int i = 0; i < (int)sk.BoneNames.size(); ++i) sk.BoneNameToIndex[sk.BoneNames[i]] = i;
                auto avatar = std::make_unique<cm::animation::AvatarDefinition>();
                cm::animation::avatar_builders::BuildFromSkeleton(sk, *avatar, true);
                fs::path ap = entry.path().parent_path() / (entry.path().stem().string() + ".avatar");
                cm::animation::SaveAvatar(*avatar, ap.string());
            }
        } catch(...) {}

        AssetPipeline::Instance().HotSwapModelInScene(src);
        AssetPipeline::Instance().ImportAsset(src, true);
        AssetPipeline::Instance().ProcessAllBlocking();
        m_ProjectRoot = BuildFileTree(m_ProjectPath);
        return true;
    } catch(...) {
        std::cerr << "[Reimport] Exception during reimport." << std::endl;
        return false;
    }
}

bool ProjectPanel::ReimportModelAsArmor(const std::filesystem::directory_entry& entry) {
    try {
        std::string src = entry.path().string();
        std::string name = entry.path().filename().string();
        std::error_code ec;
        fs::path rel = fs::relative(entry.path(), Project::GetProjectDirectory(), ec);
        std::string vpath = (ec ? src : rel.string());
        std::replace(vpath.begin(), vpath.end(), '\\', '/');
        size_t pos = vpath.find("assets/");
        if (pos != std::string::npos) vpath = vpath.substr(pos);

        ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(vpath);
        if (guid.high == 0 && guid.low == 0) {
            guid = ClaymoreGUID::Generate();
        }
        AssetLibrary::Instance().RegisterAsset(AssetReference(guid, 0, (int)AssetType::Mesh), AssetType::Mesh, vpath, name);
        AssetLibrary::Instance().RegisterPathAlias(guid, src);

        // Build the model cache normally
        BuiltModelPaths built{};
        if (!BuildModelCacheBlocking(src, built)) {
            std::cerr << "[ReimportAsArmor] Build failed for: " << src << std::endl;
            return false;
        }

        // Now modify the meta file to enable armor mode
        try {
            nlohmann::json jm;
            std::ifstream in(built.metaPath);
            if (in) { in >> jm; in.close(); }
            jm["guid"] = guid.ToString();
            jm["armorMode"] = true;  // Enable armor mode for this model
            std::ofstream out(built.metaPath);
            if (out) out << jm.dump(4);
            std::cout << "[ReimportAsArmor] Set armorMode=true in: " << built.metaPath << std::endl;
        } catch(...) {
            std::cerr << "[ReimportAsArmor] Failed to update meta with armorMode" << std::endl;
        }

        AssetPipeline::Instance().HotSwapModelInScene(src);
        AssetPipeline::Instance().ImportAsset(src, true);
        AssetPipeline::Instance().ProcessAllBlocking();
        m_ProjectRoot = BuildFileTree(m_ProjectPath);
        
        std::cout << "[ReimportAsArmor] Successfully reimported as armor: " << src << std::endl;
        return true;
    } catch(...) {
        std::cerr << "[ReimportAsArmor] Exception during reimport." << std::endl;
        return false;
    }
}

void ProjectPanel::ClearSelection() {
    m_SelectedItemPath.clear();
    m_SelectedItemName.clear();
    m_PendingSelectionPath.clear();
    m_PendingSelectionName.clear();
}

bool ProjectPanel::IsImageAsset(const std::string& path) const {
    return GuessAssetTypeFromPath(path) == AssetType::Texture;
}

std::string ProjectPanel::NormalizePathKey(const std::string& path) {
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return normalized;
}

ImVec2 ProjectPanel::ProbeImageDimensions(const std::string& path) const {
    int width = 0;
    int height = 0;
    int channels = 0;
    if (stbi_info(path.c_str(), &width, &height, &channels) == 1) {
        return ImVec2(static_cast<float>(width), static_cast<float>(height));
    }
    return ImVec2(0.0f, 0.0f);
}

void ProjectPanel::ReleaseAllImageThumbnails() {
    for (auto& kv : m_ImageThumbnails) {
        if (bgfx::isValid(kv.second.handle)) {
            bgfx::destroy(kv.second.handle);
            kv.second.handle = BGFX_INVALID_HANDLE;
        }
    }
    m_ImageThumbnails.clear();

    if (bgfx::isValid(m_ModelSnapshot.readbackTexture)) {
        bgfx::destroy(m_ModelSnapshot.readbackTexture);
        m_ModelSnapshot.readbackTexture = BGFX_INVALID_HANDLE;
    }
    Renderer::Get().ReleaseOffscreenTarget(m_ModelSnapshot.viewIdBase);
    m_ModelSnapshot.pendingReadback = false;
    m_ModelSnapshot.scene.reset();
    m_ModelSnapshot.camera.reset();
    m_ModelSnapshot.readbackBuffer.clear();
    m_ModelSnapshot.modelRoot = INVALID_ENTITY_ID;
}

ImTextureID ProjectPanel::GetImageThumbnail(const std::string& path, ImVec2* nativeSize) const {
    namespace fs = std::filesystem;
    std::string key = NormalizePathKey(path);
    const double now = ImGui::GetTime();
    auto it = m_ImageThumbnails.find(key);
    if (it != m_ImageThumbnails.end()) {
        bool stale = !bgfx::isValid(it->second.handle);
        const bool shouldValidate = stale ||
            it->second.lastValidationTime < 0.0 ||
            (now - it->second.lastValidationTime) >= kThumbnailValidationInterval;
        if (!shouldValidate) {
            if (nativeSize) *nativeSize = it->second.dimensions;
            return TextureLoader::ToImGuiTextureID(it->second.handle);
        }

        it->second.lastValidationTime = now;

        std::error_code ec;
        if (!fs::exists(path, ec)) {
            stale = true;
        } else {
            fs::file_time_type stamp = fs::last_write_time(path, ec);
            if (ec) {
                stamp = fs::file_time_type{};
            }
            if (!stale && stamp != fs::file_time_type{} && it->second.timestamp != stamp) {
                stale = true;
            }
            if (!stale) {
                if (nativeSize) *nativeSize = it->second.dimensions;
                return TextureLoader::ToImGuiTextureID(it->second.handle);
            }
        }

        if (bgfx::isValid(it->second.handle)) {
            bgfx::destroy(it->second.handle);
        }
        m_ImageThumbnails.erase(it);
    }

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return 0;
    }

    fs::file_time_type stamp = fs::last_write_time(path, ec);
    if (ec) stamp = fs::file_time_type{};

    bgfx::TextureHandle handle = TextureLoader::Load2D(path);
    if (!bgfx::isValid(handle)) {
        return 0;
    }

    ImageThumbnailEntry entry;
    entry.handle = handle;
    entry.dimensions = ProbeImageDimensions(path);
    entry.timestamp = stamp;
    entry.lastValidationTime = now;
    m_ImageThumbnails[key] = entry;
    if (nativeSize) *nativeSize = entry.dimensions;
    return TextureLoader::ToImGuiTextureID(handle);
}

// Noise texture generation functions
void ProjectPanel::CreateNoiseTexture(const std::string& destFolder, const std::string& noiseType, int width, int height) {
    std::vector<uint8_t> pixels(width * height * 4);
    
    if (noiseType == "Perlin") {
        GeneratePerlinNoise(pixels.data(), width, height);
    } else if (noiseType == "Pixel") {
        GeneratePixelNoise(pixels.data(), width, height);
    } else if (noiseType == "Simplex") {
        GenerateSimplexNoise(pixels.data(), width, height);
    } else if (noiseType == "Value") {
        GenerateValueNoise(pixels.data(), width, height);
    } else if (noiseType == "Voronoi") {
        GenerateVoronoiNoise(pixels.data(), width, height);
    } else {
        std::cerr << "[ProjectPanel] Unknown noise type: " << noiseType << std::endl;
        return;
    }
    
    std::string baseName = noiseType + "Noise";
    std::string outPath = destFolder + "/" + baseName + ".png";
    int counter = 1;
    while (fs::exists(outPath)) {
        outPath = destFolder + "/" + baseName + "_" + std::to_string(counter++) + ".png";
    }
    
    if (SavePNG(outPath, pixels.data(), width, height, 4)) {
        std::cout << "[ProjectPanel] Created noise texture: " << outPath << std::endl;
        // Refresh tree
        m_ProjectRoot = BuildFileTree(m_ProjectPath);
        
        // Create .meta file and register in AssetLibrary
        try {
            fs::path p(outPath);
            fs::path metaPath = p; metaPath += ".meta";
            AssetMetadata meta;
            bool hasMeta = false;
            if (fs::exists(metaPath)) {
                std::ifstream in(metaPath.string());
                if (in) {
                    nlohmann::json j;
                    in >> j;
                    in.close();
                    meta = j.get<AssetMetadata>();
                    hasMeta = true;
                }
            }
            if (!hasMeta) {
                meta.guid = ClaymoreGUID::Generate();
                meta.type = "texture";
                nlohmann::json j = meta;
                std::ofstream out(metaPath.string());
                out << j.dump(4);
            }
            std::string name = p.filename().string();
            std::error_code ec;
            fs::path rel = fs::relative(p, Project::GetProjectDirectory(), ec);
            std::string vpath = (ec ? p.string() : rel.string());
            std::replace(vpath.begin(), vpath.end(), '\\', '/');
            size_t pos = vpath.find("assets/");
            if (pos != std::string::npos) vpath = vpath.substr(pos);
            AssetLibrary::Instance().RegisterAsset(AssetReference(meta.guid, 0, (int)AssetType::Texture), AssetType::Texture, vpath, name);
            AssetLibrary::Instance().RegisterPathAlias(meta.guid, outPath);
        } catch(...) {
            std::cerr << "[ProjectPanel] Failed to create meta file for noise texture" << std::endl;
        }
    } else {
        std::cerr << "[ProjectPanel] Failed to save noise texture: " << outPath << std::endl;
    }
}

// Simple hash function for noise
static float Fract(float x) {
    return x - std::floor(x);
}

static float Hash(float n) {
    return Fract(std::sin(n) * 43758.5453f);
}

static float Hash2(glm::vec2 p) {
    return Hash(glm::dot(p, glm::vec2(12.9898f, 78.233f)));
}

// Perlin noise implementation
void ProjectPanel::GeneratePerlinNoise(uint8_t* pixels, int width, int height) {
    const float scale = 8.0f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float fx = (float)x / (float)width * scale;
            float fy = (float)y / (float)height * scale;
            
            // Simple Perlin-like noise using smooth interpolation
            int ix = (int)std::floor(fx);
            int iy = (int)std::floor(fy);
            float fx0 = fx - (float)ix;
            float fy0 = fy - (float)iy;
            float fx1 = fx0 - 1.0f;
            float fy1 = fy0 - 1.0f;
            
            // Smooth interpolation
            float u = fx0 * fx0 * (3.0f - 2.0f * fx0);
            float v = fy0 * fy0 * (3.0f - 2.0f * fy0);
            
            // Corner values
            float a = Hash2(glm::vec2(ix, iy));
            float b = Hash2(glm::vec2(ix + 1, iy));
            float c = Hash2(glm::vec2(ix, iy + 1));
            float d = Hash2(glm::vec2(ix + 1, iy + 1));
            
            // Bilinear interpolation
            float value = glm::mix(
                glm::mix(a, b, u),
                glm::mix(c, d, u),
                v
            );
            
            uint8_t gray = (uint8_t)(value * 255.0f);
            int idx = (y * width + x) * 4;
            pixels[idx + 0] = gray;
            pixels[idx + 1] = gray;
            pixels[idx + 2] = gray;
            pixels[idx + 3] = 255;
        }
    }
}

// Pixel noise (random per pixel)
void ProjectPanel::GeneratePixelNoise(uint8_t* pixels, int width, int height) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(0, 255);
    
    for (int i = 0; i < width * height * 4; i += 4) {
        uint8_t gray = static_cast<uint8_t>(dis(gen));
        pixels[i + 0] = gray;
        pixels[i + 1] = gray;
        pixels[i + 2] = gray;
        pixels[i + 3] = 255;
    }
}

// Simplex noise (simplified version)
void ProjectPanel::GenerateSimplexNoise(uint8_t* pixels, int width, int height) {
    const float scale = 10.0f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float fx = (float)x / (float)width * scale;
            float fy = (float)y / (float)height * scale;
            
            // Simplified Simplex-like noise
            float value = Hash2(glm::vec2(fx * 0.5f, fy * 0.5f));
            value = Fract(value * 2.0f);
            
            // Add some octaves for smoother appearance
            value += Hash2(glm::vec2(fx, fy)) * 0.5f;
            value += Hash2(glm::vec2(fx * 2.0f, fy * 2.0f)) * 0.25f;
            value /= 1.75f;
            
            uint8_t gray = (uint8_t)(glm::clamp(value, 0.0f, 1.0f) * 255.0f);
            int idx = (y * width + x) * 4;
            pixels[idx + 0] = gray;
            pixels[idx + 1] = gray;
            pixels[idx + 2] = gray;
            pixels[idx + 3] = 255;
        }
    }
}

// Value noise
void ProjectPanel::GenerateValueNoise(uint8_t* pixels, int width, int height) {
    const float scale = 6.0f;
    const int gridSize = 8;
    
    // Generate grid of random values
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    
    float grid[gridSize + 1][gridSize + 1];
    for (int iy = 0; iy <= gridSize; ++iy) {
        for (int ix = 0; ix <= gridSize; ++ix) {
            grid[iy][ix] = dis(gen);
        }
    }
    
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float fx = (float)x / (float)width * scale;
            float fy = (float)y / (float)height * scale;
            
            int ix = (int)std::floor(fx);
            int iy = (int)std::floor(fy);
            float fx0 = fx - (float)ix;
            float fy0 = fy - (float)iy;
            
            // Wrap grid indices
            ix = ix % gridSize;
            iy = iy % gridSize;
            int ix1 = (ix + 1) % gridSize;
            int iy1 = (iy + 1) % gridSize;
            
            // Smooth interpolation
            float u = fx0 * fx0 * (3.0f - 2.0f * fx0);
            float v = fy0 * fy0 * (3.0f - 2.0f * fy0);
            
            // Bilinear interpolation
            float value = glm::mix(
                glm::mix(grid[iy][ix], grid[iy][ix1], u),
                glm::mix(grid[iy1][ix], grid[iy1][ix1], u),
                v
            );
            
            uint8_t gray = (uint8_t)(value * 255.0f);
            int idx = (y * width + x) * 4;
            pixels[idx + 0] = gray;
            pixels[idx + 1] = gray;
            pixels[idx + 2] = gray;
            pixels[idx + 3] = 255;
        }
    }
}

// Save PNG using stb_image_write
bool ProjectPanel::SavePNG(const std::string& filepath, const uint8_t* pixels, int width, int height, int channels) {
    int stride = width * channels;
    int result = stbi_write_png(filepath.c_str(), width, height, channels, pixels, stride);
    return result != 0;
}

// Background context menu for right-clicking on empty space
void ProjectPanel::DrawBackgroundContextMenu() {
    if (ImGui::BeginPopup("background_ctx")) {
        std::string destFolder = m_CurrentFolder;
        if (destFolder.empty()) {
            destFolder = (Project::GetProjectDirectory() / "assets").string();
        }
        
        if (ImGui::MenuItem("Create Material")) {
            std::string base = "Material";
            std::string outPath = destFolder + "/" + base + ".mat";
            int c = 1;
            while (fs::exists(outPath)) {
                outPath = destFolder + "/" + base + "_" + std::to_string(c++) + ".mat";
            }
            CreateMaterialAt(outPath, "");
        }
        
        if (ImGui::BeginMenu("Create")) {
            // Noise texture submenu
            if (ImGui::BeginMenu("Texture")) {
                if (ImGui::BeginMenu("Noise")) {
                    if (ImGui::MenuItem("Perlin")) {
                        CreateNoiseTexture(destFolder, "Perlin");
                    }
                    if (ImGui::MenuItem("Pixel")) {
                        CreateNoiseTexture(destFolder, "Pixel");
                    }
                    if (ImGui::MenuItem("Simplex")) {
                        CreateNoiseTexture(destFolder, "Simplex");
                    }
                    if (ImGui::MenuItem("Value")) {
                        CreateNoiseTexture(destFolder, "Value");
                    }
                    if (ImGui::MenuItem("Voronoi")) {
                        CreateNoiseTexture(destFolder, "Voronoi");
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            
            if (ImGui::MenuItem("Animation Controller Override")) {
                std::string outPath = destFolder + "/NewAnimationOverride.animoverride";
                int c = 1;
                while (fs::exists(outPath)) {
                    outPath = destFolder + "/NewAnimationOverride_" + std::to_string(c++) + ".animoverride";
                }
                CreateAnimationControllerOverrideAt(outPath);
                m_ProjectRoot = BuildFileTree(m_ProjectPath);
            }

            // Scriptable create submenu (managed-registered)
            const auto& types = ScriptableTypeRegistry::Get().All();
            for (auto* t : types) {
                if (!t) continue;
                if (ImGui::MenuItem(t->menuPath.c_str())) {
                    std::string base = t->defaultFile.empty() ? std::string("NewAsset") : t->defaultFile;
                    std::string outPath = destFolder + "/" + base + ".clayobj";
                    int c = 1;
                    while (fs::exists(outPath)) {
                        outPath = destFolder + "/" + base + "_" + std::to_string(c++) + ".clayobj";
                    }
                    // Create empty JSON with header
                    ClaymoreGUID newGuid = ClaymoreGUID::Generate();
                    nlohmann::json j;
                    j["typeId"] = "";
                    j["typeName"] = t->fullName;
                    j["version"] = t->version;
                    j["guid"] = newGuid.ToString();
                    j["fields"] = nlohmann::json::object();
                    std::ofstream out(outPath);
                    if (out) out << j.dump(4);
                    
                    // Register immediately with AssetLibrary so GUID lookups work
                    std::string name = fs::path(outPath).stem().string();
                    std::string vpath = outPath;
                    std::replace(vpath.begin(), vpath.end(), '\\', '/');
                    size_t pos = vpath.find("assets/");
                    if (pos != std::string::npos) vpath = vpath.substr(pos);
                    AssetReference aref(newGuid, 0, static_cast<int32_t>(AssetType::Scriptable));
                    AssetLibrary::Instance().RegisterAsset(aref, AssetType::Scriptable, vpath, name);
                    AssetLibrary::Instance().RegisterPathAlias(newGuid, outPath);
                    
                    m_ProjectRoot = BuildFileTree(m_ProjectPath);
                }
            }
            ImGui::EndMenu();
        }
        
        // Paste option if clipboard has content
        if (!m_ClipboardPath.empty()) {
            if (ImGui::MenuItem("Paste")) {
                PasteInto(destFolder);
            }
        }

        if (m_UILayer) {
            editorui::ProjectBackgroundContext context;
            context.FolderPath = destFolder;
            m_UILayer->GetEditorContextMenus().RenderProjectBackground(context);
        }
        
        ImGui::EndPopup();
    }
}

// Voronoi noise - cellular/worley noise pattern
void ProjectPanel::GenerateVoronoiNoise(uint8_t* pixels, int width, int height) {
    const int numPoints = 32;
    const float scale = 1.0f;
    
    // Generate random cell points
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    
    std::vector<glm::vec2> points(numPoints);
    for (int i = 0; i < numPoints; ++i) {
        points[i] = glm::vec2(dis(gen), dis(gen));
    }
    
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            glm::vec2 uv((float)x / (float)width * scale, (float)y / (float)height * scale);
            
            // Find distance to nearest and second nearest points
            float minDist1 = 1000.0f;
            float minDist2 = 1000.0f;
            
            for (int i = 0; i < numPoints; ++i) {
                // Check point and its neighbors (for tiling)
                for (int ox = -1; ox <= 1; ++ox) {
                    for (int oy = -1; oy <= 1; ++oy) {
                        glm::vec2 offsetPoint = points[i] + glm::vec2((float)ox, (float)oy);
                        float dist = glm::length(uv - offsetPoint);
                        
                        if (dist < minDist1) {
                            minDist2 = minDist1;
                            minDist1 = dist;
                        } else if (dist < minDist2) {
                            minDist2 = dist;
                        }
                    }
                }
            }
            
            // Use F1 (nearest) for basic Voronoi - multiply to scale the output
            float value = minDist1 * 3.0f;
            value = glm::clamp(value, 0.0f, 1.0f);
            
            uint8_t gray = (uint8_t)(value * 255.0f);
            int idx = (y * width + x) * 4;
            pixels[idx + 0] = gray;
            pixels[idx + 1] = gray;
            pixels[idx + 2] = gray;
            pixels[idx + 3] = 255;
        }
    }
}

// Helper function to get the mirrored bone for a humanoid bone
static cm::animation::HumanoidBone GetMirroredBone(cm::animation::HumanoidBone bone) {
    using namespace cm::animation;
    switch (bone) {
        case HumanoidBone::LeftShoulder: return HumanoidBone::RightShoulder;
        case HumanoidBone::LeftUpperArm: return HumanoidBone::RightUpperArm;
        case HumanoidBone::LeftLowerArm: return HumanoidBone::RightLowerArm;
        case HumanoidBone::LeftHand: return HumanoidBone::RightHand;
        case HumanoidBone::LeftUpperLeg: return HumanoidBone::RightUpperLeg;
        case HumanoidBone::LeftLowerLeg: return HumanoidBone::RightLowerLeg;
        case HumanoidBone::LeftFoot: return HumanoidBone::RightFoot;
        case HumanoidBone::LeftToes: return HumanoidBone::RightToes;
        case HumanoidBone::LeftEye: return HumanoidBone::RightEye;
        case HumanoidBone::LeftThumbProx: return HumanoidBone::RightThumbProx;
        case HumanoidBone::LeftThumbInter: return HumanoidBone::RightThumbInter;
        case HumanoidBone::LeftThumbDist: return HumanoidBone::RightThumbDist;
        case HumanoidBone::LeftIndexProx: return HumanoidBone::RightIndexProx;
        case HumanoidBone::LeftIndexInter: return HumanoidBone::RightIndexInter;
        case HumanoidBone::LeftIndexDist: return HumanoidBone::RightIndexDist;
        case HumanoidBone::LeftMiddleProx: return HumanoidBone::RightMiddleProx;
        case HumanoidBone::LeftMiddleInter: return HumanoidBone::RightMiddleInter;
        case HumanoidBone::LeftMiddleDist: return HumanoidBone::RightMiddleDist;
        case HumanoidBone::LeftRingProx: return HumanoidBone::RightRingProx;
        case HumanoidBone::LeftRingInter: return HumanoidBone::RightRingInter;
        case HumanoidBone::LeftRingDist: return HumanoidBone::RightRingDist;
        case HumanoidBone::LeftLittleProx: return HumanoidBone::RightLittleProx;
        case HumanoidBone::LeftLittleInter: return HumanoidBone::RightLittleInter;
        case HumanoidBone::LeftLittleDist: return HumanoidBone::RightLittleDist;
        case HumanoidBone::LeftUpperArmTwist: return HumanoidBone::RightUpperArmTwist;
        case HumanoidBone::LeftLowerArmTwist: return HumanoidBone::RightLowerArmTwist;
        case HumanoidBone::LeftUpperLegTwist: return HumanoidBone::RightUpperLegTwist;
        case HumanoidBone::LeftLowerLegTwist: return HumanoidBone::RightLowerLegTwist;
        
        case HumanoidBone::RightShoulder: return HumanoidBone::LeftShoulder;
        case HumanoidBone::RightUpperArm: return HumanoidBone::LeftUpperArm;
        case HumanoidBone::RightLowerArm: return HumanoidBone::LeftLowerArm;
        case HumanoidBone::RightHand: return HumanoidBone::LeftHand;
        case HumanoidBone::RightUpperLeg: return HumanoidBone::LeftUpperLeg;
        case HumanoidBone::RightLowerLeg: return HumanoidBone::LeftLowerLeg;
        case HumanoidBone::RightFoot: return HumanoidBone::LeftFoot;
        case HumanoidBone::RightToes: return HumanoidBone::LeftToes;
        case HumanoidBone::RightEye: return HumanoidBone::LeftEye;
        case HumanoidBone::RightThumbProx: return HumanoidBone::LeftThumbProx;
        case HumanoidBone::RightThumbInter: return HumanoidBone::LeftThumbInter;
        case HumanoidBone::RightThumbDist: return HumanoidBone::LeftThumbDist;
        case HumanoidBone::RightIndexProx: return HumanoidBone::LeftIndexProx;
        case HumanoidBone::RightIndexInter: return HumanoidBone::LeftIndexInter;
        case HumanoidBone::RightIndexDist: return HumanoidBone::LeftIndexDist;
        case HumanoidBone::RightMiddleProx: return HumanoidBone::LeftMiddleProx;
        case HumanoidBone::RightMiddleInter: return HumanoidBone::LeftMiddleInter;
        case HumanoidBone::RightMiddleDist: return HumanoidBone::LeftMiddleDist;
        case HumanoidBone::RightRingProx: return HumanoidBone::LeftRingProx;
        case HumanoidBone::RightRingInter: return HumanoidBone::LeftRingInter;
        case HumanoidBone::RightRingDist: return HumanoidBone::LeftRingDist;
        case HumanoidBone::RightLittleProx: return HumanoidBone::LeftLittleProx;
        case HumanoidBone::RightLittleInter: return HumanoidBone::LeftLittleInter;
        case HumanoidBone::RightLittleDist: return HumanoidBone::LeftLittleDist;
        case HumanoidBone::RightUpperArmTwist: return HumanoidBone::LeftUpperArmTwist;
        case HumanoidBone::RightLowerArmTwist: return HumanoidBone::LeftLowerArmTwist;
        case HumanoidBone::RightUpperLegTwist: return HumanoidBone::LeftUpperLegTwist;
        case HumanoidBone::RightLowerLegTwist: return HumanoidBone::LeftLowerLegTwist;
        
        // Center bones remain unchanged
        default: return bone;
    }
}

// Helper function to mirror a position (negate X coordinate)
static glm::vec3 MirrorPosition(const glm::vec3& pos) {
    return glm::vec3(-pos.x, pos.y, pos.z);
}

// Helper function to mirror a rotation quaternion
// Mirroring across YZ plane: negate X component of rotation axis
static glm::quat MirrorRotation(const glm::quat& rot) {
    // For mirroring across YZ plane, we negate the X component and W component
    // This effectively mirrors the rotation axis
    return glm::quat(rot.w, -rot.x, rot.y, rot.z);
}

// Helper function to check if a bone should be mirrored (left/right bones)
static bool ShouldMirrorBone(cm::animation::HumanoidBone bone) {
    using namespace cm::animation;
    switch (bone) {
        case HumanoidBone::LeftShoulder:
        case HumanoidBone::LeftUpperArm:
        case HumanoidBone::LeftLowerArm:
        case HumanoidBone::LeftHand:
        case HumanoidBone::LeftUpperLeg:
        case HumanoidBone::LeftLowerLeg:
        case HumanoidBone::LeftFoot:
        case HumanoidBone::LeftToes:
        case HumanoidBone::LeftEye:
        case HumanoidBone::LeftThumbProx:
        case HumanoidBone::LeftThumbInter:
        case HumanoidBone::LeftThumbDist:
        case HumanoidBone::LeftIndexProx:
        case HumanoidBone::LeftIndexInter:
        case HumanoidBone::LeftIndexDist:
        case HumanoidBone::LeftMiddleProx:
        case HumanoidBone::LeftMiddleInter:
        case HumanoidBone::LeftMiddleDist:
        case HumanoidBone::LeftRingProx:
        case HumanoidBone::LeftRingInter:
        case HumanoidBone::LeftRingDist:
        case HumanoidBone::LeftLittleProx:
        case HumanoidBone::LeftLittleInter:
        case HumanoidBone::LeftLittleDist:
        case HumanoidBone::LeftUpperArmTwist:
        case HumanoidBone::LeftLowerArmTwist:
        case HumanoidBone::LeftUpperLegTwist:
        case HumanoidBone::LeftLowerLegTwist:
        case HumanoidBone::RightShoulder:
        case HumanoidBone::RightUpperArm:
        case HumanoidBone::RightLowerArm:
        case HumanoidBone::RightHand:
        case HumanoidBone::RightUpperLeg:
        case HumanoidBone::RightLowerLeg:
        case HumanoidBone::RightFoot:
        case HumanoidBone::RightToes:
        case HumanoidBone::RightEye:
        case HumanoidBone::RightThumbProx:
        case HumanoidBone::RightThumbInter:
        case HumanoidBone::RightThumbDist:
        case HumanoidBone::RightIndexProx:
        case HumanoidBone::RightIndexInter:
        case HumanoidBone::RightIndexDist:
        case HumanoidBone::RightMiddleProx:
        case HumanoidBone::RightMiddleInter:
        case HumanoidBone::RightMiddleDist:
        case HumanoidBone::RightRingProx:
        case HumanoidBone::RightRingInter:
        case HumanoidBone::RightRingDist:
        case HumanoidBone::RightLittleProx:
        case HumanoidBone::RightLittleInter:
        case HumanoidBone::RightLittleDist:
        case HumanoidBone::RightUpperArmTwist:
        case HumanoidBone::RightLowerArmTwist:
        case HumanoidBone::RightUpperLegTwist:
        case HumanoidBone::RightLowerLegTwist:
            return true;
        default:
            return false;
    }
}

void ProjectPanel::GenerateFlippedAnimation(const std::string& animPath) {
    using namespace cm::animation;
    
    // Load the original animation
    AnimationClip originalClip = LoadAnimationClip(animPath);
    if (!originalClip.IsHumanoid || originalClip.HumanoidTracks.empty()) {
        Logger::LogError("[ProjectPanel] Animation is not humanoid or has no humanoid tracks: " + animPath);
        return;
    }
    
    // Create a new flipped clip
    AnimationClip flippedClip;
    flippedClip.Name = originalClip.Name + "_Flipped";
    flippedClip.Duration = originalClip.Duration;
    flippedClip.TicksPerSecond = originalClip.TicksPerSecond;
    flippedClip.IsHumanoid = true;
    flippedClip.SourceAvatarRigName = originalClip.SourceAvatarRigName;
    flippedClip.SourceAvatarPath = originalClip.SourceAvatarPath;
    
    // Process each humanoid track
    for (const auto& [boneId, track] : originalClip.HumanoidTracks) {
        HumanoidBone bone = static_cast<HumanoidBone>(boneId);
        
        if (ShouldMirrorBone(bone)) {
            // Get the mirrored bone
            HumanoidBone mirroredBone = GetMirroredBone(bone);
            int mirroredId = static_cast<int>(mirroredBone);
            
            // Create mirrored track
            BoneTrack mirroredTrack;
            
            // Mirror position keys
            for (const auto& key : track.PositionKeys) {
                KeyframeVec3 mirroredKey;
                mirroredKey.Time = key.Time;
                mirroredKey.Value = MirrorPosition(key.Value);
                mirroredTrack.PositionKeys.push_back(mirroredKey);
            }
            
            // Mirror rotation keys
            for (const auto& key : track.RotationKeys) {
                KeyframeQuat mirroredKey;
                mirroredKey.Time = key.Time;
                mirroredKey.Value = MirrorRotation(key.Value);
                mirroredTrack.RotationKeys.push_back(mirroredKey);
            }
            
            // Scale keys remain the same (no mirroring needed)
            mirroredTrack.ScaleKeys = track.ScaleKeys;
            
            // Add to flipped clip
            flippedClip.HumanoidTracks[mirroredId] = std::move(mirroredTrack);
        } else {
            // Center bones (Hips, Spine, etc.) remain unchanged
            flippedClip.HumanoidTracks[boneId] = track;
        }
    }
    
    // Generate output filename
    fs::path sourcePath(animPath);
    std::string stem = sourcePath.stem().string();
    std::string ext = sourcePath.extension().string();
    fs::path outputPath = sourcePath.parent_path() / (stem + "_Flipped" + ext);
    
    // Ensure unique filename
    int counter = 1;
    while (fs::exists(outputPath)) {
        outputPath = sourcePath.parent_path() / (stem + "_Flipped_" + std::to_string(counter++) + ext);
    }
    
    // Save the flipped animation
    if (SaveAnimationClip(flippedClip, outputPath.string())) {
        Logger::Log("[ProjectPanel] Generated flipped animation: " + outputPath.string());
        
        // Refresh the file tree
        m_ProjectRoot = BuildFileTree(m_ProjectPath);
        
        // Create .meta file if original has one
        fs::path metaSrc = fs::path(animPath).string() + ".meta";
        if (fs::exists(metaSrc)) {
            try {
                std::ifstream in(metaSrc.string());
                nlohmann::json metaJson;
                if (in) {
                    in >> metaJson;
                    in.close();
                    
                    // Generate new GUID
                    metaJson["guid"] = ClaymoreGUID::Generate().ToString();
                    
                    fs::path metaDst = outputPath.string() + ".meta";
                    std::ofstream out(metaDst.string());
                    if (out) {
                        out << metaJson.dump(4);
                        out.close();
                    }
                }
            } catch (const std::exception& ex) {
                std::cerr << "[ProjectPanel] Failed to copy meta file: " << ex.what() << std::endl;
            }
        }
    } else {
        Logger::LogError("[ProjectPanel] Failed to save flipped animation: " + outputPath.string());
    }
}

void ProjectPanel::GenerateReversedAnimation(const std::string& animPath) {
    using namespace cm::animation;
    
    // Load the original animation
    AnimationClip originalClip = LoadAnimationClip(animPath);
    if (!originalClip.IsHumanoid || originalClip.HumanoidTracks.empty()) {
        Logger::LogError("[ProjectPanel] Animation is not humanoid or has no humanoid tracks: " + animPath);
        return;
    }
    
    // Create a new reversed clip
    AnimationClip reversedClip;
    reversedClip.Name = originalClip.Name + "_Reversed";
    reversedClip.Duration = originalClip.Duration;
    reversedClip.TicksPerSecond = originalClip.TicksPerSecond;
    reversedClip.IsHumanoid = true;
    reversedClip.SourceAvatarRigName = originalClip.SourceAvatarRigName;
    reversedClip.SourceAvatarPath = originalClip.SourceAvatarPath;
    
    // Process each humanoid track
    for (const auto& [boneId, track] : originalClip.HumanoidTracks) {
        BoneTrack reversedTrack;
        
        // Reverse position keys
        for (const auto& key : track.PositionKeys) {
            KeyframeVec3 reversedKey;
            reversedKey.Time = originalClip.Duration - key.Time;
            reversedKey.Value = key.Value;
            reversedTrack.PositionKeys.push_back(reversedKey);
        }
        
        // Reverse rotation keys
        for (const auto& key : track.RotationKeys) {
            KeyframeQuat reversedKey;
            reversedKey.Time = originalClip.Duration - key.Time;
            reversedKey.Value = key.Value;
            reversedTrack.RotationKeys.push_back(reversedKey);
        }
        
        // Reverse scale keys
        for (const auto& key : track.ScaleKeys) {
            KeyframeVec3 reversedKey;
            reversedKey.Time = originalClip.Duration - key.Time;
            reversedKey.Value = key.Value;
            reversedTrack.ScaleKeys.push_back(reversedKey);
        }
        
        // Sort keys by time (in case they weren't already sorted)
        std::sort(reversedTrack.PositionKeys.begin(), reversedTrack.PositionKeys.end(),
                  [](const KeyframeVec3& a, const KeyframeVec3& b) { return a.Time < b.Time; });
        std::sort(reversedTrack.RotationKeys.begin(), reversedTrack.RotationKeys.end(),
                  [](const KeyframeQuat& a, const KeyframeQuat& b) { return a.Time < b.Time; });
        std::sort(reversedTrack.ScaleKeys.begin(), reversedTrack.ScaleKeys.end(),
                  [](const KeyframeVec3& a, const KeyframeVec3& b) { return a.Time < b.Time; });
        
        // Add to reversed clip
        reversedClip.HumanoidTracks[boneId] = std::move(reversedTrack);
    }
    
    // Generate output filename
    fs::path sourcePath(animPath);
    std::string stem = sourcePath.stem().string();
    std::string ext = sourcePath.extension().string();
    fs::path outputPath = sourcePath.parent_path() / (stem + "_Reversed" + ext);
    
    // Ensure unique filename
    int counter = 1;
    while (fs::exists(outputPath)) {
        outputPath = sourcePath.parent_path() / (stem + "_Reversed_" + std::to_string(counter++) + ext);
    }
    
    // Save the reversed animation
    if (SaveAnimationClip(reversedClip, outputPath.string())) {
        Logger::Log("[ProjectPanel] Generated reversed animation: " + outputPath.string());
        
        // Refresh the file tree
        m_ProjectRoot = BuildFileTree(m_ProjectPath);
        
        // Create .meta file if original has one
        fs::path metaSrc = fs::path(animPath).string() + ".meta";
        if (fs::exists(metaSrc)) {
            try {
                std::ifstream in(metaSrc.string());
                nlohmann::json metaJson;
                if (in) {
                    in >> metaJson;
                    in.close();
                    
                    // Generate new GUID
                    metaJson["guid"] = ClaymoreGUID::Generate().ToString();
                    
                    fs::path metaDst = outputPath.string() + ".meta";
                    std::ofstream out(metaDst.string());
                    if (out) {
                        out << metaJson.dump(4);
                        out.close();
                    }
                }
            } catch (const std::exception& ex) {
                std::cerr << "[ProjectPanel] Failed to copy meta file: " << ex.what() << std::endl;
            }
        }
    } else {
        Logger::LogError("[ProjectPanel] Failed to save reversed animation: " + outputPath.string());
    }
}

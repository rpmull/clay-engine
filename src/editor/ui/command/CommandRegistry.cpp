#include "CommandRegistry.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>

#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/ComponentUtils.h"
#include "core/rendering/StandardMeshManager.h"
#include "core/rendering/MaterialManager.h"
#include "core/rendering/Renderer.h"
#include "editor/pipeline/AssetLibrary.h"
#include "core/assets/AssetReference.h"
#include "core/serialization/Serializer.h"
#include "editor/application.h"
#include <glm/glm.hpp>

// Helpers
static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n"); if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n"); return s.substr(b, e - b + 1);
}

static std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out; std::string tok; std::istringstream ss(line);
    while (ss >> tok) out.push_back(tok);
    return out;
}

static bool parseVec3(const std::string& s, glm::vec3& out) {
    // Format: (x,y,z) with optional spaces
    if (s.size() < 5 || s.front() != '(' || s.back() != ')') return false;
    std::string inner = s.substr(1, s.size()-2);
    std::replace(inner.begin(), inner.end(), ',', ' ');
    std::istringstream iss(inner);
    float x=0,y=0,z=0; if (!(iss >> x >> y >> z)) return false; out = glm::vec3(x,y,z); return true;
}

bool CommandRegistry::Execute(const std::string& line) {
    std::string t = trim(line);
    if (t.empty()) return false;
    auto toks = tokenize(t);
    if (toks.empty()) return false;
    auto it = m_Commands.find(toks[0]);
    if (it == m_Commands.end()) {
        std::cout << "[Console] Unknown command: " << toks[0] << std::endl;
        return false;
    }
    // Drop command name
    toks.erase(toks.begin());
    try {
        it->second.handler(toks);
    } catch (const std::exception& e) {
        std::cerr << "[Console] Command error: " << e.what() << std::endl;
    }
    return true;
}

std::vector<std::string> CommandRegistry::GetHelp() const {
    std::vector<std::string> lines; lines.reserve(m_Commands.size());
    for (const auto& kv : m_Commands) {
        lines.push_back(kv.second.name + " - " + kv.second.help);
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

void CommandRegistry::RegisterBuiltins() {
    // help
    Register("help", "List commands", [this](const std::vector<std::string>&){
        auto h = GetHelp();
        for (auto& l : h) std::cout << "[Console] " << l << std::endl;
    });

    // spawn prefab {path or name} (optional vec3)
    Register("spawn_prefab", "spawn_prefab <prefabPathOrName> [(x,y,z)]", [](const std::vector<std::string>& args){
        if (args.empty()) { std::cout << "[Console] Usage: spawn_prefab <pathOrName> [(x,y,z)]" << std::endl; return; }
        std::string token = args[0];
        glm::vec3 pos(0);
        if (args.size() >= 2) { parseVec3(args[1], pos); }
        Scene* s = Scene::CurrentScene; if (!s) return;
        // Try resolve by path first, then by name in AssetLibrary
        EntityID created = -1;
        // If looks like path to .prefab
        if (token.find(".prefab") != std::string::npos || token.find("/") != std::string::npos || token.find("\\") != std::string::npos) {
            created = s->InstantiateAsset(token, pos);
        } else {
            // Search AssetLibrary for a prefab with matching name
            auto list = AssetLibrary::Instance().GetAllAssets();
            for (auto& tup : list) {
                const std::string& name = std::get<0>(tup);
                AssetType t = std::get<2>(tup);
                if (t == AssetType::Prefab && name == token) {
                    ClaymoreGUID g = std::get<1>(tup);
                    if (auto* entry = AssetLibrary::Instance().GetAsset(g)) {
                        created = s->InstantiateAsset(entry->path, pos);
                    }
                    break;
                }
            }
        }
        if (created == -1) std::cout << "[Console] Failed to spawn prefab: " << token << std::endl;
    });

    // spawn primitive or prefab
    Register("spawn", "spawn <cube|plane|sphere|capsule | prefab <nameOrPath>> [(x,y,z)]", [](const std::vector<std::string>& args){
        if (args.empty()) { std::cout << "[Console] Usage: spawn <cube|plane|sphere|capsule | prefab <nameOrPath>> [(x,y,z)]" << std::endl; return; }
        if (args[0] == "prefab") {
            std::vector<std::string> sub;
            for (size_t i=1;i<args.size();++i) sub.push_back(args[i]);
            CommandRegistry::Instance().Execute(std::string("spawn_prefab ") + (sub.empty()?std::string(""):sub[0]) + (sub.size()>=2?std::string(" ")+sub[1]:std::string("")));
            return;
        }
        std::string kind = args[0];
        glm::vec3 pos(0); if (args.size() >= 2) parseVec3(args[1], pos);
        Scene* s = Scene::CurrentScene; if (!s) return;
        auto e = s->CreateEntity(kind);
        auto* d = s->GetEntityData(e.GetID()); if (!d) return;
        d->Transform.Position = pos;
        if (!d->Mesh) d->Mesh = std::make_unique<MeshComponent>();
        if (!d->RenderOverrides) d->RenderOverrides = std::make_unique<RenderOverridesComponent>();
        if (kind == "cube") {
            d->Mesh->mesh = StandardMeshManager::Instance().GetCubeMesh();
            d->Mesh->meshReference = AssetReference::CreatePrimitive("Cube");
        }
        else if (kind == "plane") {
            d->Mesh->mesh = StandardMeshManager::Instance().GetPlaneMesh();
            d->Mesh->meshReference = AssetReference::CreatePrimitive("Plane");
        }
        else if (kind == "sphere") {
            d->Mesh->mesh = StandardMeshManager::Instance().GetSphereMesh();
            d->Mesh->meshReference = AssetReference::CreatePrimitive("Sphere");
        }
        else if (kind == "capsule") {
            d->Mesh->mesh = StandardMeshManager::Instance().GetCapsuleMesh();
            d->Mesh->meshReference = AssetReference::CreatePrimitive("Capsule");
        }
        else { std::cout << "[Console] Unknown primitive: " << kind << std::endl; return; }
        d->Mesh->material = MaterialManager::Instance().CreateSceneDefaultMaterial(s);
    });

    // scene reset: destroy runtime and re-instantiate from editor scene
    Register("scene_reset", "Reset play mode scene from editor scene", [](const std::vector<std::string>&){
        // Only valid when running editor with UI
        if (!Application::HasInstance()) return;
        auto& app = Application::Get();
        if (!app.m_RunEditorUI) return;
        // Find UILayer scene; editor scene is uiLayer->GetScene(); runtime clone lives at editorScene.m_RuntimeScene
        // Toggle by recreating the runtime clone similar to RequestBeginPlayAsync logic
        // Grab current editor scene via Scene::Get() ownership — Application ensures CurrentScene points to runtime during play
        // Rebuild from the editor copy stored in UILayer
        // Minimal approach: stop and start play
        app.StopPlayMode();
        app.StartPlayMode();
        std::cout << "[Console] Scene reset." << std::endl;
    });

    // toggle debug true/false
    Register("toggle_debug", "toggle_debug <true|false>", [](const std::vector<std::string>& args){
        bool on = true; if (!args.empty()) on = (args[0] == "true" || args[0] == "1" || args[0] == "on");
        if (!Application::HasInstance() || !Application::Get().m_RunEditorUI) {
            Renderer::Get().SetDebugDrawInPlayMode(false);
            std::cout << "[Console] Debug in play mode is only available inside the editor." << std::endl;
            return;
        }
        Renderer::Get().SetDebugDrawInPlayMode(on);
        std::cout << "[Console] Debug in play mode: " << (on ? "on" : "off") << std::endl;
    });

    // entity {name|id} addComponent {componentName}
    Register("entity", "entity <name|id> addComponent <ComponentName>", [](const std::vector<std::string>& args){
        if (args.size() < 3 || args[1] != "addComponent") {
            std::cout << "[Console] Usage: entity <name|id> addComponent <ComponentName>" << std::endl; return;
        }
        Scene* s = Scene::CurrentScene; if (!s) return;
        // Resolve entity by ID first, then by name (first match)
        EntityID id = -1;
        try { id = (EntityID)std::stol(args[0]); } catch(...) {
            // name
            for (auto& e : s->GetEntities()) { auto* d = s->GetEntityData(e.GetID()); if (d && d->Name == args[0]) { id = e.GetID(); break; } }
        }
        if (id == -1) { std::cout << "[Console] Entity not found." << std::endl; return; }
        std::string comp = args[2];
        auto* d = s->GetEntityData(id); if (!d) return;
        if (comp == "Mesh") { if (!d->Mesh) d->Mesh = std::make_unique<MeshComponent>(); if (!d->RenderOverrides) d->RenderOverrides = std::make_unique<RenderOverridesComponent>(); }
        else if (comp == "Light") { if (!d->Light) d->Light = std::make_unique<LightComponent>(LightType::Point, glm::vec3(1.0f), 1.0f); }
        else if (comp == "Camera") { if (!d->Camera) d->Camera = std::make_unique<CameraComponent>(); }
        else if (comp == "RigidBody") {
            if (!d->RigidBody && !d->StaticBody) {
                d->RigidBody = std::make_unique<RigidBodyComponent>();
                EnsureCollider(d->RigidBody.get(), d);
            }
        }
        else if (comp == "StaticBody") {
            if (!d->StaticBody && !d->RigidBody) {
                d->StaticBody = std::make_unique<StaticBodyComponent>();
                EnsureCollider(d->StaticBody.get(), d);
            }
        }
        else if (comp == "Collider") { if (!d->Collider) d->Collider = std::make_unique<ColliderComponent>(); }
        else if (comp == "Text") { if (!d->Text) d->Text = std::make_unique<TextRendererComponent>(); }
        else { std::cout << "[Console] Unknown component: " << comp << std::endl; }
    });

    // Additional useful commands
    Register("list_entities", "List entities [contains]", [](const std::vector<std::string>& args){
        Scene* s = Scene::CurrentScene; if (!s) return; std::string filter = args.empty()?std::string(""):args[0];
        for (auto& e : s->GetEntities()) { auto* d = s->GetEntityData(e.GetID()); if (!d) continue; if (!filter.empty() && d->Name.find(filter)==std::string::npos) continue; std::cout << "[Console] [" << e.GetID() << "] " << d->Name << std::endl; }
    });
    Register("destroy", "destroy <id|name>", [](const std::vector<std::string>& args){
        if (args.empty()) { std::cout << "[Console] Usage: destroy <id|name>" << std::endl; return; }
        Scene* s = Scene::CurrentScene; if (!s) return; EntityID id = -1; try { id = (EntityID)std::stol(args[0]); } catch(...) {
            for (auto& e : s->GetEntities()) { auto* d = s->GetEntityData(e.GetID()); if (d && d->Name == args[0]) { id = e.GetID(); break; } }
        }
        if (id == -1) { std::cout << "[Console] Entity not found." << std::endl; return; }
        s->RemoveEntity(id);
    });
    Register("quit_play", "Exit play mode (editor)", [](const std::vector<std::string>&){ if (Application::HasInstance()) Application::Get().StopPlayMode(); });
}



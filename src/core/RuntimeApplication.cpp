#include "RuntimeApplication.h"
#include "core/platform/win32/Win32Window.h"
#include "core/ecs/Scene.h"
#include "core/jobs/JobSystem.h"
#include "core/jobs/Jobs.h"
#include "core/vfs/VirtualFS.h"
#include "core/vfs/PakReader.h"
#include "core/vfs/FileSystem.h"
#include "core/rendering/Renderer.h"
#include "core/rendering/TextureLoader.h"
#include "core/rendering/ShaderManager.h"
#include "core/rendering/StandardMeshManager.h"
#include "core/rendering/TerrainGrass.h"
#include "core/rendering/Terrain.h"
#include "core/physics/Physics.h"
#include "core/audio/Audio.h"
#include "core/input/Input.h"
#ifndef CLAYMORE_RUNTIME
#include "core/serialization/Serializer.h"
#endif
#include "core/serialization/EntityBinaryLoader.h"
#include "core/assets/IAssetResolver.h"
#include "core/assets/RuntimeAssetResolver.h"
#include "core/assets/ModelRegistry.h"
#include "core/utils/Time.h"
#include "core/utils/Profiler.h"
#include "core/navigation/Navigation.h"
#include "core/managed/RuntimeHost.h"
#include "core/managed/ScriptSystem.h"
#include "core/managed/ManagedScriptComponent.h"
#include "core/animation/AnimationSystem.h"
#include "core/rendering/Environment.h"
#include "core/prefab/RuntimePrefabInstantiator.h"
#include "editor/Project.h"
#include "managed/interop/DialogueInterop.h"
#include "managed/ModuleLoader.h"
#include "managed/interop/ModuleInterop.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <filesystem>
#include <thread>
#include <vector>

RuntimeApplication* RuntimeApplication::s_Instance = nullptr;

// Helper to convert string to wstring
static std::wstring ToWideString(const std::string& str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring wstr(size - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], size);
    return wstr;
}

static size_t CountPendingPrefabLoads(Scene& scene, size_t* outFailed = nullptr) {
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

static bool FinalizeInitialPrefabLoads(Scene& scene) {
    constexpr auto kMaxWait = std::chrono::seconds(5);
    constexpr int kMaxIterations = 4096;

    size_t failed = 0;
    size_t pending = CountPendingPrefabLoads(scene, &failed);
    if (pending == 0) {
        return failed == 0;
    }

    std::cout << "[Runtime] Waiting for " << pending
              << " pending prefab load(s) before entering play..." << std::endl;

    const auto start = std::chrono::steady_clock::now();
    size_t lastPending = pending;

    for (int iteration = 0; iteration < kMaxIterations && pending > 0; ++iteration) {
        runtime::RuntimePrefabInstantiator::UpdateAsync(4.0);
        scene.ProcessPendingCreations();
        scene.ProcessPendingRemovals();

        pending = CountPendingPrefabLoads(scene, &failed);
        if (pending == 0) {
            break;
        }

        if (pending != lastPending) {
            std::cout << "[Runtime] Pending prefab loads remaining: " << pending << std::endl;
            lastPending = pending;
        }

        if (std::chrono::steady_clock::now() - start > kMaxWait) {
            std::cerr << "[Runtime] WARNING: Timed out waiting for initial prefab loads; "
                      << pending << " root(s) still pending." << std::endl;
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    pending = CountPendingPrefabLoads(scene, &failed);
    if (failed > 0) {
        std::cerr << "[Runtime] WARNING: " << failed
                  << " prefab root(s) failed during initial scene load." << std::endl;
    }
    return pending == 0;
}

RuntimeApplication::RuntimeApplication(const RuntimeLaunchOptions& options)
    : m_Width(options.width), m_Height(options.height), m_LaunchOptions(options) {
    s_Instance = this;
    Init();
    InitWindow(m_LaunchOptions.width, m_LaunchOptions.height, m_LaunchOptions.title);
    InitBgfx();
}

RuntimeApplication::RuntimeApplication(int width, int height, const std::string& title)
    : RuntimeApplication(RuntimeLaunchOptions{width, height, title}) {
}

RuntimeApplication::~RuntimeApplication() {
    Shutdown();
    s_Instance = nullptr;
}

RuntimeApplication& RuntimeApplication::Get() {
    return *s_Instance;
}

void RuntimeApplication::Init() {
    std::cout << "[Runtime] ============================================" << std::endl;
    std::cout << "[Runtime] Claymore Runtime Initialization Starting" << std::endl;
    std::cout << "[Runtime] ============================================" << std::endl;

    // Get exe directory (more reliable than current_path)
    wchar_t exePathW[MAX_PATH];
    GetModuleFileNameW(NULL, exePathW, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePathW).parent_path();
    
    std::filesystem::path contentRoot = exeDir;
    if (!m_LaunchOptions.contentRoot.empty()) {
        std::error_code rootEc;
        std::filesystem::path requestedRoot(m_LaunchOptions.contentRoot);
        if (requestedRoot.is_relative()) {
            requestedRoot = exeDir / requestedRoot;
        }
        std::filesystem::path normalizedRoot = std::filesystem::weakly_canonical(requestedRoot, rootEc);
        contentRoot = rootEc ? requestedRoot.lexically_normal() : normalizedRoot;
    }

    // Set current working directory to the requested content root when present.
    // This keeps relative project/module paths behaving like exported builds while
    // still letting the runtime load managed engine binaries from the executable directory.
    std::error_code ec;
    std::filesystem::current_path(contentRoot, ec);
    if (ec) {
        std::cerr << "[Runtime] Warning: Could not set working directory to: " << contentRoot << std::endl;
    }
    
    std::cout << "[Runtime] Executable directory: " << exeDir << std::endl;
    std::cout << "[Runtime] Content root: " << contentRoot << std::endl;
    std::cout << "[Runtime] Working directory: " << std::filesystem::current_path() << std::endl;

    FileSystem::Instance().SetProjectRoot(contentRoot);
    Project::SetProjectDirectory(contentRoot);

    // Runtime must never fall back to disk outside the PAK
    FileSystem::Instance().SetAllowDiskFallback(false);

    // Mount pak file - prefer an explicit preview/export pak, otherwise fall back
    // to the first pak found under the selected content root or executable directory.
    bool pakMounted = false;
    std::string pakName;
    std::vector<std::filesystem::path> pakCandidates;
    if (!m_LaunchOptions.pakPath.empty()) {
        std::filesystem::path explicitPak(m_LaunchOptions.pakPath);
        if (explicitPak.is_relative()) {
            explicitPak = contentRoot / explicitPak;
        }
        pakCandidates.push_back(explicitPak.lexically_normal());
    } else {
        auto collectPaks = [&pakCandidates](const std::filesystem::path& dir) {
            std::error_code dirEc;
            if (!std::filesystem::exists(dir, dirEc) || dirEc) {
                return;
            }
            for (auto& entry : std::filesystem::directory_iterator(dir, dirEc)) {
                if (dirEc) break;
                if (entry.is_regular_file() && entry.path().extension() == ".pak") {
                    pakCandidates.push_back(entry.path());
                }
            }
        };

        collectPaks(contentRoot);
        if (contentRoot != exeDir) {
            collectPaks(exeDir);
        }
    }

    for (const auto& pakPath : pakCandidates) {
        auto pakReader = std::make_unique<PakReader>();
        if (pakReader->Open(pakPath.string())) {
            pakName = pakPath.filename().string();
            size_t fileCount = pakReader->GetFileCount();
            VFS::Set(std::move(pakReader));
            pakMounted = true;
            Project::RenameProject(pakPath.stem().string());
            std::cout << "[Runtime] Mounted pak file: " << pakName
                      << " (" << fileCount << " files)" << std::endl;
            break;
        }
        std::cerr << "[Runtime] Failed to open pak: " << pakPath << std::endl;
    }
    
    if (!pakMounted) {
        std::cerr << "[Runtime] ERROR: No pak file found in: " << exeDir << std::endl;
        std::cerr << "[Runtime] Please ensure a .pak file exists next to the executable." << std::endl;
    }
    
    // Initialize runtime asset resolver - must be done after VFS is set
    m_AssetResolver = std::make_unique<RuntimeAssetResolver>();
    std::cout << "[Runtime] Asset resolver initialized." << std::endl;

    // Load centralized model registry for runtime model instantiation
    // This enables Mesh.Instantiate() to create full model hierarchies at runtime
    // The registry is stored in .bin/ directory in the PAK
    if (cm::ModelRegistry::Instance().Load(".bin/model_registry.bin")) {
        std::cout << "[Runtime] Model registry loaded: " 
                  << cm::ModelRegistry::Instance().GetModelCount() << " models" << std::endl;
    } else {
        std::cout << "[Runtime] No model registry found (dynamic model instantiation may be limited)" << std::endl;
    }

    // Initialize job system
    unsigned hw = std::thread::hardware_concurrency();
    size_t workers = (hw > 2) ? (hw - 1) : 1;
    m_Jobs = std::make_unique<JobSystem>(workers);
    cm::g_JobSystem = m_Jobs.get();
    std::cout << "[Runtime] Job system initialized with " << workers << " workers." << std::endl;

    // Initialize .NET runtime for scripting
    std::cout << "[Runtime] Initializing .NET runtime..." << std::endl;
    if (!cm::runtime::InitializeDotNet()) {
        std::cerr << "[Runtime] WARNING: .NET initialization failed. Scripts will not work." << std::endl;
        std::cerr << "[Runtime] Check that ClaymoreEngine.dll and GameScripts.dll exist." << std::endl;
    } else {
        std::cout << "[Runtime] .NET runtime initialized successfully." << std::endl;
        
        // Initialize dialogue interop (matching editor's initialization)
        DialogueInterop::Initialize();
    }

    // Create scene
    m_Scene = std::make_unique<Scene>();
    Scene::CurrentScene = m_Scene.get();
    std::cout << "[Runtime] Scene created." << std::endl;
}

void RuntimeApplication::InitWindow(int width, int height, const std::string& title) {
    std::cout << "[Runtime] Creating window: " << width << "x" << height << std::endl;
    
    m_Win32Window = std::make_unique<Win32Window>();
    std::wstring wideTitle = ToWideString(title);
    const bool maximize = false;
    const bool center = (m_LaunchOptions.windowMode == RuntimeWindowMode::Windowed);
    m_Win32Window->Create(wideTitle.c_str(), width, height, true, true, maximize, center);
    m_Win32Window->SetResizeCallback([](int w, int h, bool minimized) {
        RuntimeApplication& app = RuntimeApplication::Get();
        if (minimized) {
            return;
        }

        HWND hwnd = app.GetWindowHandle();
        if (hwnd) {
            RECT rc{};
            if (GetClientRect(hwnd, &rc)) {
                const int clientWidth = rc.right - rc.left;
                const int clientHeight = rc.bottom - rc.top;
                if (clientWidth > 0 && clientHeight > 0) {
                    w = clientWidth;
                    h = clientHeight;
                }
            }
        }

        if (w <= 0 || h <= 0) {
            return;
        }

        app.m_PendingWidth = w;
        app.m_PendingHeight = h;
        app.m_PendingResize = true;
    });
    m_Window = m_Win32Window->GetHWND();

    if (m_Window) {
        RECT rc{};
        if (GetClientRect(m_Window, &rc)) {
            const int clientWidth = rc.right - rc.left;
            const int clientHeight = rc.bottom - rc.top;
            if (clientWidth > 0 && clientHeight > 0) {
                m_Width = clientWidth;
                m_Height = clientHeight;
            }
        }
    }

    if (m_LaunchOptions.windowMode == RuntimeWindowMode::Fullscreen) {
        m_Win32Window->EnterFullscreen();
    }
    
    std::cout << "[Runtime] Window created." << std::endl;
}

void RuntimeApplication::InitBgfx() {
    std::cout << "[Runtime] Initializing bgfx..." << std::endl;
    
    // Initialize renderer - this handles bgfx::init() internally
    // Match editor's initialization flow exactly
    Renderer::Get().Init(m_Width, m_Height, m_Window);
    
    // Runtime renders directly to backbuffer (no offscreen framebuffer)
    Renderer::Get().SetRenderToOffscreen(false);
    
    std::cout << "[Runtime] bgfx initialized." << std::endl;
}

void RuntimeApplication::Shutdown() {
    std::cout << "[Runtime] Shutting down..." << std::endl;
    
    // IMPORTANT: Script instances must be destroyed while .NET is still valid
    // Clear all script instances from entities BEFORE destroying scenes
    auto clearScriptsFromScene = [](Scene* scene) {
        if (!scene) return;
        for (const auto& entity : scene->GetEntities()) {
            auto* data = scene->GetEntityData(entity.GetID());
            if (data) {
                // Clear scripts - their destructors will call Script_Destroy into .NET
                data->Scripts.clear();
            }
        }
    };
    
    std::cout << "[Runtime] Clearing script instances..." << std::endl;
    clearScriptsFromScene(m_RuntimeScene.get());
    clearScriptsFromScene(m_Scene.get());
    
    // Now safe to shutdown .NET - all script instances have been destroyed
    std::cout << "[Runtime] Shutting down .NET runtime..." << std::endl;
    cm::runtime::ShutdownDotNet();
    
    // IMPORTANT: Destroy terrain physics bodies BEFORE scenes are destroyed
    // and BEFORE Physics::Shutdown() to avoid dangling references
    auto destroyTerrainPhysics = [](Scene* scene) {
        if (!scene) return;
        for (const auto& entity : scene->GetEntities()) {
            auto* data = scene->GetEntityData(entity.GetID());
            if (data && data->Terrain) {
                Terrain::DestroyPhysicsBody(*data->Terrain);
            }
        }
    };
    
    std::cout << "[Runtime] Destroying terrain physics bodies..." << std::endl;
    destroyTerrainPhysics(m_RuntimeScene.get());
    destroyTerrainPhysics(m_Scene.get());
    
    // Stop and clear scenes 
    if (m_RuntimeScene) {
        std::cout << "[Runtime] Stopping runtime scene..." << std::endl;
        runtime::RuntimePrefabInstantiator::CancelAsyncForScene(*m_RuntimeScene, true);
        m_RuntimeScene->OnStop();
        m_RuntimeScene.reset();
    }
    
    if (m_Scene) {
        std::cout << "[Runtime] Clearing source scene..." << std::endl;
        m_Scene.reset();
    }
    Scene::CurrentScene = nullptr;
    runtime::RuntimePrefabInstantiator::ResetRuntimeCaches();
    
    // Navigation cleanup if needed
    // nav::Navigation::Get() may not have Shutdown - skip if not available
    
    std::cout << "[Runtime] Shutting down audio..." << std::endl;
    Audio::Shutdown();
    
    std::cout << "[Runtime] Shutting down physics..." << std::endl;
    Physics::Get().Shutdown();
    
    // Destroy custom cursor before bgfx shutdown
    std::cout << "[Runtime] Destroying custom cursor..." << std::endl;
    DestroyCursor();
    
    std::cout << "[Runtime] Shutting down renderer..." << std::endl;
    StandardMeshManager::Instance().Shutdown();
    Renderer::Get().Shutdown();  // This calls bgfx::shutdown() internally
    
    if (m_Win32Window) {
        m_Win32Window->Destroy();
        m_Win32Window.reset();
    }
    
    cm::g_JobSystem = nullptr;
    m_Jobs.reset();
}

void RuntimeApplication::LoadEntryScene() {
    std::cout << "[Runtime] ============================================" << std::endl;
    std::cout << "[Runtime] Loading Entry Scene" << std::endl;
    std::cout << "[Runtime] ============================================" << std::endl;
    
    // Try to load entry scene from manifest
    std::string manifestText;
    if (!VFS::ReadTextFile("game_manifest.json", manifestText)) {
        std::cerr << "[Runtime] ERROR: Could not read game_manifest.json from PAK!" << std::endl;
        std::cerr << "[Runtime] Make sure the exported build includes the manifest." << std::endl;
        return;
    }
    
    std::cout << "[Runtime] Loaded manifest (" << manifestText.size() << " bytes)" << std::endl;
    
    // Load cursor settings from manifest (before scene load)
    LoadCursorSettings(manifestText);
    
    try {
        nlohmann::json j = nlohmann::json::parse(manifestText);
        
        // Load asset map into resolver
        if (m_AssetResolver) {
            m_AssetResolver->LoadManifest(manifestText);
        }
        
        // Load resource manifest for Resources API
        std::string resourceManifestText;
        if (VFS::ReadTextFile("resource_manifest.json", resourceManifestText)) {
            if (m_AssetResolver) {
                m_AssetResolver->LoadResourceManifest(resourceManifestText);
            }
            std::cout << "[Runtime] Loaded resource manifest." << std::endl;
        }
        
        // Load project modules from manifest (matching editor's LoadProjectModulesAtStartup)
        if (j.contains("modules") && j["modules"].is_array()) {
            NativeAPIs native{};
            FillModuleNativeAPIs(native);
            
            for (const auto& modEntry : j["modules"]) {
                std::string modId = modEntry.value("id", "");
                std::string modDll = modEntry.value("dll", "");
                
                if (modDll.empty()) continue;
                
                // Module DLLs are copied to the output directory (relative paths work)
                std::cout << "[Runtime] Loading module: " << modId << " from " << modDll << std::endl;
                
                ManagedAPIs managed{};
                auto* handle = ModuleLoader::LoadModule(modDll, native, &managed);
                
                if (handle) {
                    std::cout << "[Runtime] Successfully loaded module: " << modId << std::endl;
                } else {
                    std::cerr << "[Runtime] Failed to load module: " << modId << std::endl;
                }
            }
        }
        
        std::string entry = j.value("entryScene", "");
        if (entry.empty()) {
            std::cerr << "[Runtime] ERROR: No entryScene specified in manifest!" << std::endl;
            return;
        }
        
        std::cout << "[Runtime] Entry scene path: " << entry << std::endl;
        
        // Try binary scene format first (.sceneb)
        bool loaded = false;
        std::string loadedPath;
        
        // First, try the path as specified in manifest
        if (entry.find(".sceneb") != std::string::npos) {
            std::cout << "[Runtime] Attempting binary scene load: " << entry << std::endl;
            loaded = binary::EntityBinaryLoader::Load(entry, *m_Scene);
            if (loaded) {
                loadedPath = entry;
                std::cout << "[Runtime] SUCCESS: Loaded binary scene: " << entry << std::endl;
            } else {
                std::cerr << "[Runtime] Failed to load binary scene: " << entry << std::endl;
            }
        }
        
        // Try with .sceneb extension if original path was .scene
        if (!loaded) {
            std::filesystem::path binPath(entry);
            if (binPath.extension() == ".scene") {
                binPath.replace_extension(".sceneb");
                std::cout << "[Runtime] Attempting binary scene load: " << binPath << std::endl;
                loaded = binary::EntityBinaryLoader::Load(binPath.string(), *m_Scene);
                if (loaded) {
                    loadedPath = binPath.string();
                    std::cout << "[Runtime] SUCCESS: Loaded binary scene: " << binPath << std::endl;
                }
            }
        }
        
        // Try scene paths with different prefixes (VFS normalization)
        if (!loaded) {
            std::vector<std::string> pathVariants = {
                entry,
                "assets/scenes/" + std::filesystem::path(entry).filename().string(),
                "scenes/" + std::filesystem::path(entry).filename().string()
            };
            
            // Add .sceneb variants
            for (const auto& variant : std::vector<std::string>(pathVariants)) {
                std::filesystem::path vp(variant);
                if (vp.extension() != ".sceneb") {
                    pathVariants.push_back(vp.replace_extension(".sceneb").string());
                }
            }
            
            for (const auto& variant : pathVariants) {
                if (VFS::Exists(variant)) {
                    std::cout << "[Runtime] Found scene at: " << variant << std::endl;
                    if (variant.find(".sceneb") != std::string::npos) {
                        loaded = binary::EntityBinaryLoader::Load(variant, *m_Scene);
                    }
#ifndef CLAYMORE_RUNTIME
                    else {
                        loaded = Serializer::LoadSceneFromFile(variant, *m_Scene);
                    }
#endif
                    if (loaded) {
                        loadedPath = variant;
                        std::cout << "[Runtime] SUCCESS: Loaded scene: " << variant << std::endl;
                        break;
                    }
                }
            }
        }
        
#ifndef CLAYMORE_RUNTIME
        // Final fallback to JSON serializer with original path (editor only)
        if (!loaded) {
            std::cout << "[Runtime] Falling back to JSON scene loader: " << entry << std::endl;
            loaded = Serializer::LoadSceneFromFile(entry, *m_Scene);
            if (loaded) {
                loadedPath = entry;
                std::cout << "[Runtime] SUCCESS: Loaded JSON scene: " << entry << std::endl;
            } else {
                std::cerr << "[Runtime] ERROR: Failed to load scene: " << entry << std::endl;
            }
        }
#endif
        
        if (loaded && !loadedPath.empty()) {
            m_Scene->SetScenePath(loadedPath);
        }
        
        // Report what we loaded
        if (loaded) {
            if (!FinalizeInitialPrefabLoads(*m_Scene)) {
                std::cerr << "[Runtime] WARNING: Scene entered startup with unresolved prefab work."
                          << std::endl;
            }

            // Match the editor load pipeline by resolving transforms and script entity refs
            // before runtime scripts are instantiated and cloned.
            m_Scene->UpdateTransforms();
            m_Scene->ResolveScriptEntityReferencesFromMetadata();

            std::cout << "[Runtime] Scene contains " << m_Scene->GetEntities().size() << " entities" << std::endl;
            
            // Debug: List top-level entities
            int count = 0;
            for (const auto& e : m_Scene->GetEntities()) {
                auto* data = m_Scene->GetEntityData(e.GetID());
                if (data && data->Parent == INVALID_ENTITY_ID && count < 10) {
                    std::cout << "[Runtime]   - " << data->Name << std::endl;
                    count++;
                }
            }
            if (m_Scene->GetEntities().size() > 10) {
                std::cout << "[Runtime]   ... and " << (m_Scene->GetEntities().size() - 10) << " more" << std::endl;
            }
            
            // CRITICAL: Create managed script instances for all entities
            // Binary scene loading only stores class names and property values,
            // the actual managed instances must be created before RuntimeClone
            std::cout << "[Runtime] Creating script instances..." << std::endl;
            int scriptsCreated = 0;
            int scriptsFailed = 0;
            for (const auto& e : m_Scene->GetEntities()) {
                auto* data = m_Scene->GetEntityData(e.GetID());
                if (!data) continue;
                
                for (auto& script : data->Scripts) {
                    if (!script.Instance && !script.ClassName.empty()) {
                        auto created = ScriptSystem::Instance().Create(script.ClassName);
                        if (created) {
                            script.Instance = created;
                            scriptsCreated++;
                        } else {
                            std::cerr << "[Runtime] Failed to create script: " << script.ClassName 
                                      << " for entity: " << data->Name << std::endl;
                            scriptsFailed++;
                        }
                    }
                }
            }
            std::cout << "[Runtime] Script instances created: " << scriptsCreated 
                      << ", failed: " << scriptsFailed << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[Runtime] ERROR: Failed to parse manifest: " << e.what() << std::endl;
        return;
    }
    
    // Create runtime clone and enter play mode
    std::cout << "[Runtime] Creating runtime scene clone..." << std::endl;
    if (m_Scene) {
        m_Scene->ReleasePhysicsRuntimeState();
    }
    std::shared_ptr<Scene> cloned = m_Scene->RuntimeClone();
    if (!cloned) {
        std::cerr << "[Runtime] ERROR: Failed to create runtime scene clone!" << std::endl;
        return;
    }
    
    m_RuntimeScene = std::move(cloned);
    m_RuntimeScene->m_IsPlaying = true;
    Scene::CurrentScene = m_RuntimeScene.get();
    
    // Count scripts and cameras
    size_t scriptCount = 0;
    size_t meshCount = 0;
    size_t cameraCount = 0;
    CameraComponent* activeCameraComp = nullptr;
    
    for (const auto& e : m_RuntimeScene->GetEntities()) {
        auto* d = m_RuntimeScene->GetEntityData(e.GetID());
        if (!d) continue;
        
        scriptCount += d->Scripts.size();
        if (d->Mesh) meshCount++;
        if (d->Camera) {
            cameraCount++;
            if (d->Camera->Active) {
                activeCameraComp = d->Camera.get();
            }
        }
    }
    
    std::cout << "[Runtime] Runtime scene stats:" << std::endl;
    std::cout << "[Runtime]   Entities: " << m_RuntimeScene->GetEntities().size() << std::endl;
    std::cout << "[Runtime]   Meshes: " << meshCount << std::endl;
    std::cout << "[Runtime]   Scripts: " << scriptCount << std::endl;
    std::cout << "[Runtime]   Cameras: " << cameraCount << std::endl;
    
    if (!activeCameraComp) {
        std::cerr << "[Runtime] WARNING: No active camera found! Scene may not render correctly." << std::endl;
        // Try to activate the first camera found
        for (const auto& e : m_RuntimeScene->GetEntities()) {
            auto* d = m_RuntimeScene->GetEntityData(e.GetID());
            if (d && d->Camera) {
                d->Camera->Active = true;
                std::cout << "[Runtime] Auto-activated camera on entity: " << d->Name << std::endl;
                break;
            }
        }
    }
    
    std::cout << "[Runtime] Entered play mode successfully." << std::endl;
}

void RuntimeApplication::Run() {
    std::cout << "[Runtime] ============================================" << std::endl;
    std::cout << "[Runtime] Starting Main Loop" << std::endl;
    std::cout << "[Runtime] ============================================" << std::endl;
    
    // Initialize systems
    std::cout << "[Runtime] Initializing Physics..." << std::endl;
    Physics::Get().Init();
    
    std::cout << "[Runtime] Initializing Audio..." << std::endl;
    Audio::Init();
    
    std::cout << "[Runtime] Initializing Input..." << std::endl;
    Input::Init();
    
    std::cout << "[Runtime] Initializing Time..." << std::endl;
    Time::Init();
    
    // Load the game scene
    LoadEntryScene();
    
    // Verify scene is ready
    Scene* scene = Scene::CurrentScene;
    if (!scene) {
        std::cerr << "[Runtime] FATAL: No scene loaded! Exiting." << std::endl;
        return;
    }
    
    // Debug: Log script status in runtime scene
    std::cout << "[Runtime] Script verification:" << std::endl;
    for (const auto& e : scene->GetEntities()) {
        auto* d = scene->GetEntityData(e.GetID());
        if (!d || d->Scripts.empty()) continue;
        for (const auto& script : d->Scripts) {
            void* handle = nullptr;
            if (script.Instance && script.Instance->GetBackend() == ScriptBackend::Managed) {
                auto managed = std::dynamic_pointer_cast<ManagedScriptComponent>(script.Instance);
                if (managed) handle = managed->GetHandle();
            }
            std::cout << "[Runtime]   Entity '" << d->Name << "' has script '" 
                      << script.ClassName << "' instance=" << (script.Instance ? "valid" : "NULL")
                      << " handle=" << handle << std::endl;
        }
    }
    
    // Log environment settings (applied automatically during RenderScene)
    Environment& env = scene->GetEnvironment();
    std::cout << "[Runtime] Scene environment:" << std::endl;
    std::cout << "[Runtime]   Ambient color: (" << env.AmbientColor.x << ", " 
              << env.AmbientColor.y << ", " << env.AmbientColor.z << ")" << std::endl;
    std::cout << "[Runtime]   Ambient intensity: " << env.AmbientIntensity << std::endl;
    
    // Perform initial scene update to sync transforms and cameras before first render
    // This ensures the camera matrices are computed from entity transforms
    scene->UpdateTransforms();
    
    // Sync all cameras with their entity transforms
    for (const auto& entity : scene->GetEntities()) {
        auto* data = scene->GetEntityData(entity.GetID());
        if (data && data->Camera) {
            data->Camera->SyncWithTransform(data->Transform);
            float aspectRatio = static_cast<float>(m_Width) / static_cast<float>(m_Height);
            data->Camera->UpdateProjection(aspectRatio);
        }
    }
    
    // Get initial camera
    Camera* sceneCamera = scene->GetActiveCamera();
    if (sceneCamera) {
        std::cout << "[Runtime] Using scene camera." << std::endl;
        std::cout << "[Runtime]   Camera position: (" << sceneCamera->GetPosition().x << ", " 
                  << sceneCamera->GetPosition().y << ", " << sceneCamera->GetPosition().z << ")" << std::endl;
        Renderer::Get().SetCamera(sceneCamera);
    } else {
        std::cout << "[Runtime] WARNING: No scene camera, using renderer default." << std::endl;
    }
    
    std::cout << "[Runtime] Entering main loop..." << std::endl;
    
    // CRITICAL: Initialize terrain physics BEFORE the first physics step
    // The terrain physics body is normally created during RenderScene, but physics
    // steps before rendering. Without this, the character falls through terrain on frame 1.
    std::cout << "[Runtime] Initializing terrain physics..." << std::endl;
    for (const auto& entity : scene->GetEntities()) {
        auto* data = scene->GetEntityData(entity.GetID());
        if (data && data->Terrain && data->Terrain->PhysicsDirty) {
            Terrain::UpdatePhysicsBody(*data->Terrain, data->Transform.WorldMatrix, entity.GetID());
            std::cout << "[Runtime] Created terrain physics body for: " << data->Name << std::endl;
        }
    }
    
    uint64_t frameCount = 0;
    float totalTime = 0.0f;
    Profiler::Get().SetEnabled(false);
    Profiler::Get().SetTelemetryEnabled(true);
    Profiler::Get().ConfigureDefaultBudgets();
    
    while (!m_ShouldClose) {
        Profiler::Get().BeginFrame();
        const auto frameStart = std::chrono::high_resolution_clock::now();
        // Update time first, then get delta (matches editor order)
        Time::Tick();
        float dt = Time::GetDeltaTime();
        totalTime += dt;
        frameCount++;

        // Reset per-frame input state BEFORE pumping events so edge detection works correctly
        // This matches the editor's order: Input::Update() then PumpEvents()
        Input::Update();
        
        // Process window events (fills input states for this frame)
        if (m_Win32Window) {
            m_Win32Window->PumpEvents();
            if (m_Win32Window->ShouldClose()) {
                m_ShouldClose = true;
            }
            // Keep cursor centered when captured (true relative mouse mode behavior)
            m_Win32Window->CenterCursorIfCaptured();
        }
        
        // Handle resize - guard against 0×0 dimensions (e.g., minimize)
        // bgfx::reset and render target allocation can fail/crash with zero dimensions
        if (m_PendingResize) {
            m_Width = m_PendingWidth;
            m_Height = m_PendingHeight;
            m_PendingResize = false;
            
            // Only reset bgfx and renderer if dimensions are valid (>0)
            if (m_Width > 0 && m_Height > 0) {
                bgfx::reset(m_Width, m_Height, Renderer::kPresentationResetFlags);
                Renderer::Get().Resize(m_Width, m_Height);
                std::cout << "[Runtime] Window resized to: " << m_Width << "x" << m_Height << std::endl;
            }
        }
        
        // Update scene
        if (scene && scene->m_IsPlaying) {
            ScopedTimer t("Scene/Update (Game)");
            scene->Update(dt);
            
            // Update camera each frame in case it changed
            Camera* cam = scene->GetActiveCamera();
            if (cam) {
                Renderer::Get().SetCamera(cam);
            }
        }
        
        // Update grass deformation system (applies player/NPC deformation to grass)
        if (scene) {
            TerrainGrass::UpdateDeformationSystem(*scene, dt);
        }

        // Update audio system (listener position, sound cleanup)
        Audio::Update(dt);
        
        // NOTE: Physics is stepped inside Scene::Update() when m_IsPlaying is true.
        // Do NOT step physics here - that would cause double-stepping per frame.
        // Editor play mode also only steps physics once (via Scene::Update).
        
        // Render - match editor's rendering flow exactly
        // 1. BeginFrame sets up view rects, transforms, and framebuffers
        Renderer::Get().BeginFrame(0.1f, 0.1f, 0.1f);  // Same clear color as editor
        
        // 2. RenderScene draws the actual scene content
        if (scene) {
            ScopedTimer t("Renderer/RenderScene (Game)");
            Renderer::Get().RenderScene(*scene);
        }
        
        // 3. Render custom cursor on top of everything
        RenderCustomCursor();
        
        // 4. Submit frame to GPU
        {
            ScopedTimer t("Renderer/SubmitFrame");
            Renderer::Get().EndFrame();
        }
        const auto frameEnd = std::chrono::high_resolution_clock::now();
        Profiler::Get().Record(
            "Frame",
            std::chrono::duration<double, std::milli>(frameEnd - frameStart).count());
        Profiler::Get().EndFrame();
        
        // Debug: Print FPS every 5 seconds
        if (frameCount % 300 == 0 && totalTime > 0.0f) {
            float fps = frameCount / totalTime;
            std::cout << "[Runtime] FPS: " << fps << " (frame " << frameCount << ")" << std::endl;
        }
    }
    
    std::cout << "[Runtime] Main loop ended after " << frameCount << " frames." << std::endl;
}

void RuntimeApplication::LoadCursorSettings(const std::string& manifestText) {
    try {
        nlohmann::json j = nlohmann::json::parse(manifestText);
        
        if (j.contains("gameCursor") && j["gameCursor"].is_object()) {
            auto& cursor = j["gameCursor"];
            m_CursorSettings.texturePath = cursor.value("texture", std::string());
            m_CursorSettings.baseScale = cursor.value("scale", 1.0f);
            m_CursorSettings.hotspotX = cursor.value("hotspotX", 0);
            m_CursorSettings.hotspotY = cursor.value("hotspotY", 0);
            m_CursorSettings.useDPIScaling = cursor.value("useDPIScaling", true);
            
            std::cout << "[Runtime] Loading custom cursor: " << m_CursorSettings.texturePath << std::endl;
            
            // Load cursor texture from VFS (pak file)
            if (!m_CursorSettings.texturePath.empty()) {
                std::vector<uint8_t> data;
                if (VFS::ReadFile(m_CursorSettings.texturePath, data)) {
                    // Use TextureLoader to create texture from encoded image in memory
                    m_CursorTexture = TextureLoader::Load2DFromEncodedMemory(data.data(), static_cast<int>(data.size()), 
                                                                             false, TextureColorSpace::sRGB);
                    if (bgfx::isValid(m_CursorTexture)) {
                        m_UseCustomCursor = true;
                        // Get texture dimensions from bgfx
                        // Note: bgfx doesn't expose texture dimensions, so we use stb_image to decode
                        // dimensions separately. For now, assume reasonable defaults.
                        m_CursorTextureWidth = 32;
                        m_CursorTextureHeight = 32;
                        // Hide system cursor
                        ShowCursor(FALSE);
                        std::cout << "[Runtime] Custom cursor loaded" << std::endl;
                    } else {
                        std::cerr << "[Runtime] Failed to create cursor texture" << std::endl;
                    }
                } else {
                    std::cerr << "[Runtime] Failed to load cursor texture from pak: " << m_CursorSettings.texturePath << std::endl;
                }
            }
        }
        
        if (j.contains("defaultFont")) {
            std::string fontPath = j.value("defaultFont", std::string());
            if (!fontPath.empty()) {
                std::cout << "[Runtime] Using default font: " << fontPath << std::endl;
            }
            Renderer::Get().SetDefaultTextFont(fontPath);
        }
    } catch (const std::exception& e) {
        std::cerr << "[Runtime] Error parsing cursor settings: " << e.what() << std::endl;
    }
}

void RuntimeApplication::RenderCustomCursor() {
    if (!m_UseCustomCursor || !bgfx::isValid(m_CursorTexture)) return;
    
    // Get mouse position
    const auto [mouseX, mouseY] = Input::GetMousePosition();
    
    // Calculate DPI scale (Windows default is 96 DPI)
    float dpiScale = 1.0f;
    if (m_CursorSettings.useDPIScaling && m_Win32Window) {
        HDC hdc = GetDC(m_Window);
        if (hdc) {
            int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
            dpiScale = static_cast<float>(dpiX) / 96.0f;
            ReleaseDC(m_Window, hdc);
        }
    }
    
    // Calculate cursor size
    // Default cursor size is typically 32x32 at 96 DPI
    // We scale the cursor texture to match the native cursor size expectation
    float defaultCursorSize = 32.0f * dpiScale;
    float scale = m_CursorSettings.baseScale * (defaultCursorSize / std::max(m_CursorTextureWidth, m_CursorTextureHeight));
    
    float cursorW = m_CursorTextureWidth * scale;
    float cursorH = m_CursorTextureHeight * scale;
    
    // Position cursor with hotspot offset
    float hotspotX = m_CursorSettings.hotspotX * scale;
    float hotspotY = m_CursorSettings.hotspotY * scale;
    float x0 = mouseX - hotspotX;
    float y0 = mouseY - hotspotY;
    float x1 = x0 + cursorW;
    float y1 = y0 + cursorH;
    
    // Clamp to screen bounds
    if (x0 < 0) { x1 -= x0; x0 = 0; }
    if (y0 < 0) { y1 -= y0; y0 = 0; }
    if (x1 > m_Width) { x0 -= (x1 - m_Width); x1 = static_cast<float>(m_Width); }
    if (y1 > m_Height) { y0 -= (y1 - m_Height); y1 = static_cast<float>(m_Height); }
    
    // Render cursor quad using a high view ID to render on top of everything
    // This follows the pattern from Renderer's UI rendering
    const uint16_t cursorViewId = 250;  // High view ID for overlay
    
    // Set up orthographic projection for 2D rendering
    float orthoProj[16];
    bx::mtxOrtho(orthoProj, 0.0f, static_cast<float>(m_Width), static_cast<float>(m_Height), 0.0f, 0.0f, 1000.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
    
    bgfx::setViewRect(cursorViewId, 0, 0, m_Width, m_Height);
    bgfx::setViewTransform(cursorViewId, nullptr, orthoProj);
    bgfx::setViewClear(cursorViewId, BGFX_CLEAR_NONE);
    
    // Create vertex data for cursor quad
    struct CursorVertex {
        float x, y, z;
        float u, v;
        uint32_t color;
    };
    
    static bgfx::VertexLayout s_cursorLayout;
    static bool layoutInit = false;
    if (!layoutInit) {
        s_cursorLayout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .end();
        layoutInit = true;
    }
    
    // Check if we have enough space for transient buffers
    if (!bgfx::getAvailTransientVertexBuffer(4, s_cursorLayout) || !bgfx::getAvailTransientIndexBuffer(6)) {
        return;  // Not enough buffer space this frame
    }
    
    bgfx::TransientVertexBuffer tvb;
    bgfx::TransientIndexBuffer tib;
    bgfx::allocTransientVertexBuffer(&tvb, 4, s_cursorLayout);
    bgfx::allocTransientIndexBuffer(&tib, 6);
    
    CursorVertex* verts = (CursorVertex*)tvb.data;
    uint16_t* indices = (uint16_t*)tib.data;
    
    uint32_t white = 0xFFFFFFFF;
    verts[0] = { x0, y0, 0.0f, 0.0f, 0.0f, white };
    verts[1] = { x1, y0, 0.0f, 1.0f, 0.0f, white };
    verts[2] = { x1, y1, 0.0f, 1.0f, 1.0f, white };
    verts[3] = { x0, y1, 0.0f, 0.0f, 1.0f, white };
    
    indices[0] = 0; indices[1] = 1; indices[2] = 2;
    indices[3] = 0; indices[4] = 2; indices[5] = 3;
    
    // Get UI shader and sampler from renderer (we need these to be accessible)
    // For simplicity, use the same UI program that the renderer uses
    // We'll need a static uniform handle for the texture sampler
    static bgfx::UniformHandle s_cursorSampler = BGFX_INVALID_HANDLE;
    static bgfx::ProgramHandle s_cursorProgram = BGFX_INVALID_HANDLE;
    
    if (!bgfx::isValid(s_cursorSampler)) {
        s_cursorSampler = bgfx::createUniform("s_uiTex", bgfx::UniformType::Sampler);
    }
    if (!bgfx::isValid(s_cursorProgram)) {
        // Load the UI shader using ShaderManager (handles runtime/editor shader paths)
        s_cursorProgram = ShaderManager::Instance().LoadProgram("vs_ui", "fs_ui");
    }
    
    if (!bgfx::isValid(s_cursorProgram) || !bgfx::isValid(s_cursorSampler)) {
        return;  // Shader not available
    }
    
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setTexture(0, s_cursorSampler, m_CursorTexture);
    
    // Enable alpha blending for cursor transparency
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA);
    bgfx::setState(state);
    bgfx::submit(cursorViewId, s_cursorProgram);
}

void RuntimeApplication::DestroyCursor() {
    if (bgfx::isValid(m_CursorTexture)) {
        bgfx::destroy(m_CursorTexture);
        m_CursorTexture = BGFX_INVALID_HANDLE;
    }
    if (m_UseCustomCursor) {
        ShowCursor(TRUE);  // Restore system cursor
        m_UseCustomCursor = false;
    }
}

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#undef min
#undef max
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/math.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <thread>
 
#include "application.h"
#include "editor/tools/WorldGraphBake.h"

// Core includes - use core/ prefix for clarity
#include "core/rendering/Renderer.h"
#include "core/rendering/MaterialCache.h"
#include "core/rendering/ShaderManager.h"
#include "core/rendering/TextureLoader.h"
#include "core/rendering/TerrainGrass.h"
#include "core/rendering/StandardMeshManager.h"
#include "core/ecs/SkinningSystem.h"
#include "core/utils/Time.h"
#include "core/physics/Physics.h"
#include "core/platform/win32/Win32Window.h"
#include "core/vfs/FileSystem.h"
#include "core/serialization/EntityBinaryLoader.h"
#include "core/serialization/Serializer.h"
#include "core/utils/Profiler.h"
#include "core/input/Input.h"
#include "core/jobs/Jobs.h"
#include "core/audio/Audio.h"
#include "core/prefab/PrefabPrewarm.h"
#include "core/prefab/RuntimePrefabInstantiator.h"
#include "managed/interop/DialogueInterop.h"

// Editor includes
#include "editor/pipeline/AssetRegistry.h"
#include "core/assets/AssetMetadata.h"
#include "editor/ui/utility/TextureSlotPicker.h"
#include "editor/ui/imgui_backend/imgui_impl_bgfx_docking.h"
#include "backends/imgui_impl_win32.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_claymore_style.h>
#include "editor/rendering/Picking.h"
#include "editor/EditorSettings.h"
#include "editor/Project.h"
#include "editor/pipeline/AssetPipeline.h"
#include "editor/pipeline/AssetLibrary.h"
#include "editor/pipeline/AssetWatcher.h"
#include "editor/pipeline/BinaryAssetCache.h"
#include "core/resources/ResourceManifest.h"
#include "core/world/WorldGraph.h"
#include "editor/ui/UILayer.h"
#include "editor/ui/utility/EditorThemeUtils.h"

// Managed/interop includes
#include "managed/interop/DotNetHost.h"
#include "managed/ModuleLoader.h"
#include "managed/interop/ModuleInterop.h"
// Application.cpp
Application* Application::s_Instance = nullptr;

// =============================================================
// HELPER FUNCTIONS
// =============================================================
namespace {
bool FinalizeGameStartupPrefabLoads(Scene& scene)
{
    constexpr auto kMaxWait = std::chrono::seconds(5);
    constexpr int kMaxIterations = 4096;
    const auto start = std::chrono::steady_clock::now();

    auto countPending = [&scene]() {
        size_t pending = 0;
        for (const auto& entity : scene.GetEntities()) {
            const EntityData* data = scene.GetEntityData(entity.GetID());
            if (data && data->PrefabAsyncPending) {
                ++pending;
            }
        }
        return pending;
    };

    size_t pending = countPending();
    if (pending == 0) {
        return true;
    }

    std::cout << "[Game] Waiting for " << pending
              << " pending prefab load(s) before entering play..." << std::endl;

    for (int iteration = 0; iteration < kMaxIterations && pending > 0; ++iteration) {
        runtime::RuntimePrefabInstantiator::UpdateAsync(4.0);
        scene.ProcessPendingCreations();
        scene.ProcessPendingRemovals();

        pending = countPending();
        if (pending == 0) {
            return true;
        }

        if (std::chrono::steady_clock::now() - start > kMaxWait) {
            std::cerr << "[Game] WARNING: Timed out waiting for startup prefab loads." << std::endl;
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return countPending() == 0;
}

bool FinalizeGameStartupAssetWarmup(Scene& scene)
{
    TextureLoadSettings textureSettings{};
    textureSettings.MaxDimension = scene.GetEnvironment().TextureMaxDimension;
    if (SetPersistentTextureLoadSettings(textureSettings)) {
        RefreshSceneTextureOverrides(scene);
    }

    prefab::Clear();
    prefab::QueueScenePrefabs(scene);
    if (!prefab::HasPendingWork()) {
        prefab::Clear();
        return true;
    }

    constexpr auto kMaxWait = std::chrono::seconds(15);
    constexpr int kMaxIterations = 8192;
    const auto start = std::chrono::steady_clock::now();

    std::cout << "[Game] Prewarming scene dependencies before entering play..." << std::endl;

    for (int iteration = 0; iteration < kMaxIterations; ++iteration) {
        prefab::Update(8.0);
        if (!prefab::HasPendingWork()) {
            prefab::Clear();
            return true;
        }

        if (std::chrono::steady_clock::now() - start > kMaxWait) {
            std::cerr << "[Game] WARNING: Timed out while prewarming scene dependencies." << std::endl;
            prefab::Clear();
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    prefab::Clear();
    return false;
}
}

static void LoadProjectModulesAtStartup()
{
    const auto& modules = Project::GetModules();
    if (modules.empty()) {
        std::cout << "[Application] No project modules to load at startup.\n";
        return;
    }

    std::cout << "[Application] Loading " << modules.size() << " project modules at startup...\n";

    NativeAPIs native{};
    FillModuleNativeAPIs(native);

    for (const auto& module : modules) {
        if (!module.enabled) {
            std::cout << "[Application] Skipping disabled module: " << module.id << "\n";
            continue;
        }

        if (module.dll.empty()) {
            std::cerr << "[Application] Module " << module.id << " has no DLL path specified.\n";
            continue;
        }

        // Convert DLL path to absolute path
        std::filesystem::path modulePath;
        if (std::filesystem::path(module.dll).is_absolute()) {
            modulePath = module.dll;
            std::cout << "[Application] Module path is absolute: " << module.dll << "\n";
        } else {
            // Relative to project directory
            modulePath = Project::GetProjectDirectory() / module.dll;
            std::cout << "[Application] Module path is relative: " << module.dll << " -> " << modulePath << "\n";
        }

        std::cout << "[Application] Loading module: " << module.id << " from " << modulePath << "\n";
        std::cout << "[Application] Module exists: " << (std::filesystem::exists(modulePath) ? "YES" : "NO") << "\n";

        ManagedAPIs managed{};
        auto* handle = ModuleLoader::LoadModule(modulePath.string(), native, &managed);
        
        if (handle) {
            std::cout << "[Application] Successfully loaded module: " << module.id << "\n";
        } else {
            std::cerr << "[Application] Failed to load module: " << module.id << "\n";
        }
    }
}

static void PrewarmImGuiGlyphRanges(ImFont* font, const ImWchar* ranges)
{
    if (!font) {
        return;
    }

    ImFontBaked* baked = font->GetFontBaked(font->LegacySize);
    if (!baked) {
        return;
    }

    if (font->FallbackChar != 0) {
        baked->FindGlyph(font->FallbackChar);
    }
    if (font->EllipsisChar != 0) {
        baked->FindGlyph(font->EllipsisChar);
    }

    const ImWchar* glyphRanges = ranges ? ranges : ImGui::GetIO().Fonts->GetGlyphRangesDefault();

    for (const ImWchar* range = glyphRanges; range && range[0]; range += 2) {
        for (unsigned int c = range[0]; c <= range[1] && c <= IM_UNICODE_CODEPOINT_MAX; ++c) {
            baked->FindGlyph(static_cast<ImWchar>(c));
        }
    }
}

static void EnsureSystemCursorVisible()
{
#ifdef _WIN32
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    if (GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING)) {
        return;
    }
    int count = 0;
    while (ShowCursor(TRUE) < 0 && count < 10) {
        count++; // Safety limit to prevent infinite loop
    }
#endif
}

// =============================================================
// CONSTRUCTOR / INITIALIZATION
// =============================================================
Application::Application(int width, int height, const std::string& title)
    : m_width(width), m_height(height)
{

    if (s_Instance)
        throw std::runtime_error("Only one instance of Application is allowed!");

    s_Instance = this;

    // 1. If a project directory was not preselected, resolve default relative to executable
    std::filesystem::path defaultProjPath;
    if (Project::GetProjectDirectory().empty()) {
        std::filesystem::path projPath = std::filesystem::current_path();
        std::filesystem::path rawProjPath = projPath / "../../../ClayProject";
        defaultProjPath = std::filesystem::weakly_canonical(rawProjPath);
        Project::SetProjectDirectory(defaultProjPath);
        std::cout << "[Application] Set default project directory: " << defaultProjPath << std::endl;
    } else {
        defaultProjPath = Project::GetProjectDirectory();
        std::cout << "[Application] Using existing project directory: " << defaultProjPath << std::endl;
    }

    // 3. Verify it exists
    if (!std::filesystem::exists(defaultProjPath)) {
        std::cerr << "[Init] Project directory does not exist: " << defaultProjPath << std::endl;
        // Optional: std::filesystem::create_directories(defaultProjPath);
    }

    // Editor mode: register GUID→path for assets so GUID references resolve when swapping scenes
    {
        std::filesystem::path assetsDir = Project::GetProjectDirectory() / "assets";
        if (std::filesystem::exists(assetsDir)) {
            for (auto& entry : std::filesystem::recursive_directory_iterator(assetsDir)) {
                if (!entry.is_regular_file()) continue;
                std::string abs = entry.path().string();
                const AssetMetadata* meta = AssetRegistry::Instance().GetMetadata(abs);
                AssetMetadata metaLocal;
                if (!meta) {
                    // Try read sidecar .meta file
                    std::filesystem::path metaPath = entry.path(); metaPath += ".meta";
                    if (std::filesystem::exists(metaPath)) {
                        try {
                            std::ifstream mi(metaPath.string());
                            nlohmann::json mj; mi >> mj; mi.close();
                            metaLocal = mj.get<AssetMetadata>();
                            meta = &metaLocal;
                        } catch(...) {}
                    }
                }
                if (!meta) continue;
                if (meta->guid == ClaymoreGUID()) continue;
                // Normalize to virtual path (assets/..)
                std::string vpath = abs;
                std::replace(vpath.begin(), vpath.end(), '\\', '/');
                auto pos = vpath.find("assets/");
                if (pos != std::string::npos) vpath = vpath.substr(pos);
                // Infer type from extension
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                AssetType at = AssetType::Unknown;
                if (ext == ".fbx" || ext == ".gltf" || ext == ".glb" || ext == ".obj") at = AssetType::Mesh;
                else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") at = AssetType::Texture;
                else if (ext == ".prefab") at = AssetType::Prefab;
                else if (ext == ".anim") at = AssetType::Animation;
                else if (ext == ".animctrl" || ext == ".controller") at = AssetType::AnimatorController;
                else if (ext == ".mat") at = AssetType::Material;
                else if (ext == ".sc" || ext == ".glsl" || ext == ".shader") at = AssetType::Shader;
                else if (ext == ".ttf" || ext == ".otf") at = AssetType::Font;
                if (at == AssetType::Unknown) continue;
                AssetLibrary::Instance().RegisterAsset(AssetReference(meta->guid, 0, static_cast<int32_t>(at)), at, vpath, entry.path().filename().string());
                // Also map absolute path so serializers that wrote absolute paths can resolve
                AssetLibrary::Instance().RegisterPathAlias(meta->guid, abs);
            }
        }
    }

    // Try mounting a pak next to the executable for standalone mode (DO THIS EARLY)
    {
        std::filesystem::path exeDir = std::filesystem::current_path();
        bool mounted = false;
        // 1) Look for any .pak file in the directory
        for (auto& entry : std::filesystem::directory_iterator(exeDir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".pak") {
                if (FileSystem::Instance().MountPak(entry.path().string())) {
                    mounted = true; break;
                }
            }
        }
        // 2) Fallback names if none found by scan
        if (!mounted) {
            std::filesystem::path pakA = exeDir / (Project::GetProjectName() + ".pak");
            std::filesystem::path pakB = exeDir / "Game.pak";
            if (std::filesystem::exists(pakA)) {
                FileSystem::Instance().MountPak(pakA.string());
            } else if (std::filesystem::exists(pakB)) {
                FileSystem::Instance().MountPak(pakB.string());
            }
        }
    }

    // Decide runtime mode BEFORE initializing renderer so we can gate editor-only work
    // Force play-mode if export marker exists next to the executable
    bool forceGameMode = std::filesystem::exists(std::filesystem::current_path() / "game_mode_only.marker");
    m_RunEditorUI = !(FileSystem::Instance().IsPakMounted() || forceGameMode);
    // Match runtime behavior when running without editor UI
    FileSystem::Instance().SetAllowDiskFallback(m_RunEditorUI);

    // 1. Initialize Window (Win32)
    InitWindow(width, height, title);

    // 2. Initialize BGFX Renderer (shader compile gated by m_RunEditorUI)
    InitBgfx();
    if (!m_RunEditorUI) {
        Renderer::Get().SetRenderToOffscreen(true);
        Renderer::Get().SetPresentOffscreenToBackbuffer(true);
    }

    // 3. Initialize ImGui (editor mode only)
    if (m_RunEditorUI) {
        InitImGui();
    }

    // 4. Initialize Physics System
    Physics::Get().Init();

    // 5. Initialize Audio System
    Audio::Init();

    // 5.5 Initialize Dialogue System
    DialogueInterop::Initialize();

    // 6. Input Init
    Input::Init();

    // Example: bind RMB to toggle cursor capture in editor
    // (You can move this to a configurable binding in your editor layer)

    unsigned hw = std::thread::hardware_concurrency();
    size_t workers = (hw > 2) ? (hw - 1) : 1;
    m_Jobs = std::make_unique<JobSystem>(workers);
    cm::g_JobSystem = m_Jobs.get();  // Set global accessor

    // Init Dotnet
    std::filesystem::path fullPath = std::filesystem::current_path() / "ClaymoreEngine.dll";
    LoadDotnetRuntime(
       L"ClaymoreEngine.dll", // Relative or full path works due to absolute() above
       L"ClaymoreEngine.EngineEntry, ClaymoreEngine",
       L"ManagedStart"
    );
    
    // Load project modules after .NET runtime is initialized
    LoadProjectModulesAtStartup();
    
    // 4. Initialize Asset Pipeline + Watcher (editor only)
    m_AssetPipeline = std::make_unique<AssetPipeline>();
    m_AssetWatcher = std::make_unique<AssetWatcher>(*m_AssetPipeline, defaultProjPath.string());
    
    // Initialize binary asset cache for the project
    // This manages the hidden .bin folder that stores optimized binaries for play mode
    BinaryAssetCache::Instance().Initialize(defaultProjPath);
    
    // Initialize resource manifest (for resources/ folder tracking)
    // This is lazy - will only activate if resources/ folder exists
    ResourceManifest::Get().Initialize(defaultProjPath);
    if (ResourceManifest::Get().HasResourcesFolder()) {
        ResourceManifest::Get().Scan();
        std::cout << "[Application] ResourceManifest initialized with " 
                  << ResourceManifest::Get().GetAllResources().size() << " resources" << std::endl;
    }
    
    // Create and register editor asset resolver
    m_EditorAssetResolver = std::make_unique<EditorAssetResolver>();
    if (!m_RunEditorUI) {
        m_EditorAssetResolver->SetLoadMode(AssetLoadMode::PlayMode);
        m_EditorAssetResolver->SetPlayModeBinaryOnly(true);
    }
    
    if (m_RunEditorUI) {
        m_AssetWatcher->Start();
		uiLayer->LoadProject(defaultProjPath.string());
		Scene::CurrentScene = &uiLayer->GetScene();
        auto& graph = cm::world::WorldGraph::Get();
        if (!graph.LoadProjectGraph()) {
            std::cout << "[Application] worldgraph missing/invalid, baking now..." << std::endl;
            cm::editor::worldgraph::BakeWorldGraph();
        } else {
            std::cout << "[Application] Loaded existing worldgraph.json." << std::endl;
        }
    } else {
        m_GameScene = std::make_unique<Scene>();
        Scene::CurrentScene = m_GameScene.get();
    }

    // If running with a mounted pak, try to load entry scene from manifest
    if (FileSystem::Instance().IsPakMounted()) {
        std::string manifestText;
        if (FileSystem::Instance().ReadTextFile("game_manifest.json", manifestText)) {
            try {
                nlohmann::json j = nlohmann::json::parse(manifestText);
                // Load asset GUID->path map first so scene deserialization can resolve meshes/materials
                if (j.contains("assetMap") && j["assetMap"].is_array()) {
                    for (auto& rec : j["assetMap"]) {
                        std::string guidStr = rec.value("guid", "");
                        std::string vpath   = rec.value("path", "");
                        if (!guidStr.empty() && !vpath.empty()) {
                            ClaymoreGUID g = ClaymoreGUID::FromString(guidStr);
                            // Register into AssetLibrary for runtime lookup
                            AssetReference aref(g);
                            AssetLibrary::Instance().RegisterAsset(aref, AssetType::Mesh, vpath, vpath);
                        }
                    }
                }
                std::string entry = j.value("entryScene", "");
                if (!entry.empty()) {
                    bool loaded = false;
                    if (std::filesystem::path(entry).extension() == ".sceneb") {
                        loaded = binary::EntityBinaryLoader::Load(entry, *Scene::CurrentScene);
                    }
                    if (!loaded) {
                        loaded = Serializer::LoadSceneFromFile(entry, *Scene::CurrentScene);
                    }
                    if (loaded) {
                        Scene::CurrentScene->SetScenePath(entry);
                        if (m_RunEditorUI) {
                            uiLayer->SetCurrentScenePath(entry);
                        }
                    } else {
                        std::cerr << "[Init] Failed to load entry scene: " << entry << std::endl;
                    }
                }
            } catch(const std::exception& e) {
                std::cerr << "[Init] Failed parsing game_manifest.json: " << e.what() << std::endl;
            }
        }
    }

    // In exported/game mode (no editor UI), create a runtime clone and enter play
    if (!m_RunEditorUI) {
        if (m_GameScene) {
            if (!FinalizeGameStartupAssetWarmup(*m_GameScene)) {
                std::cerr << "[Game] WARNING: Startup dependency warmup did not fully complete." << std::endl;
            }
            if (!FinalizeGameStartupPrefabLoads(*m_GameScene)) {
                std::cerr << "[Game] WARNING: Startup prefab loads did not fully complete." << std::endl;
            }
            m_GameScene->ReleasePhysicsRuntimeState();
            m_GameScene->UpdateTransforms();
            m_GameScene->ResolveScriptEntityReferencesFromMetadata();
            m_RuntimeScene = m_GameScene->RuntimeClone();
            if (m_RuntimeScene) {
                m_RuntimeScene->m_IsPlaying = true;
                Scene::CurrentScene = m_RuntimeScene.get();
                // Debug: report entering play mode and script counts
                size_t scriptCount = 0;
                for (const auto& e : m_RuntimeScene->GetEntities()) {
                    auto* d = m_RuntimeScene->GetEntityData(e.GetID());
                    if (d) scriptCount += d->Scripts.size();
                }
                std::cout << "[Game] Entered play mode (runtime clone). Scripts attached: " << scriptCount << std::endl;
            }
        }
    }

    std::cout << "[Application] Initialization complete." << std::endl;
}

Application::~Application() {
    Shutdown();
}
void Application::SetMouseCaptured(bool captured) {
    if (m_Win32Window) {
        m_Win32Window->SetCursorCaptured(captured);
    }
    Input::SetRelativeMode(captured);
}

void Application::SetEditModeMouseCaptureAllowed(bool allowed) {
    m_EditModeMouseCaptureAllowed = allowed && m_RunEditorUI && !IsPlaying();
    if (!m_EditModeMouseCaptureAllowed &&
        m_RunEditorUI &&
        !IsPlaying() &&
        m_Win32Window &&
        m_Win32Window->IsCursorCaptured()) {
        SetMouseCaptured(false);
    }
}

bool Application::IsGameViewportMouseInputAllowed() const {
    if (!m_RunEditorUI) {
        return true;
    }
    if (!IsPlaying()) {
        return true;
    }
    if (Input::IsRelativeMode()) {
        return true;
    }
    return uiLayer && uiLayer->IsViewportHovered();
}

bool Application::IsPlaying() const {
    if (m_IsPlaying) {
        return true;
    }
    // Runtime clones mark the active scene as playing before the editor UI flips
    // into play mode, so trust the scene flag as well to avoid rejecting early
    // startup requests like script-driven mouse capture.
    if (Scene::CurrentScene && Scene::CurrentScene->m_IsPlaying) {
        return true;
    }
    if (m_RunEditorUI) {
        return uiLayer && uiLayer->IsPlayMode();
    }
    return false;
}


// =============================================================
// WINDOW SETUP
// =============================================================
void Application::InitWindow(int width, int height, const std::string& title) {
    std::cout << "[Application] Initializing window: " << width << "x" << height
        << " Title: " << title << std::endl;

    // Create Win32 window via our wrapper to get proper message routing and DPI/resize handling
    m_Win32Window = std::make_unique<Win32Window>();
    std::wstring wtitle(title.begin(), title.end());
    if (!m_Win32Window->Create(wtitle.c_str(), width, height, true, true, true, false)) {
        throw std::runtime_error("[Application] Failed to create Win32 window");
    }
    m_Win32Window->SetResizeCallback([](int w, int h, bool minimized)
    {
        Application& app = Application::Get();

        if (minimized)
            return;

        // WS_POPUP borderless can report transient values during maximize.
        // Instead of trusting w/h, get the actual client area.
        HWND hwnd = (HWND)app.m_window;
        if (hwnd)
        {
            RECT rc{};
            if (GetClientRect(hwnd, &rc))
            {
                int cw = rc.right - rc.left;
                int ch = rc.bottom - rc.top;
                if (cw > 0 && ch > 0)
                {
                    w = cw;
                    h = ch;
                }
            }
        }

        if (w <= 0 || h <= 0)
            return;

        app.m_PendingWidth  = w;
        app.m_PendingHeight = h;
        app.m_PendingResize = true;
    });
    m_window = m_Win32Window->GetHWND();

    // Ensure initial client size reflects the actual window (e.g., maximized) before bgfx init
    if (m_window) {
        RECT rc; if (GetClientRect((HWND)m_window, &rc)) {
            int cw = rc.right - rc.left;
            int ch = rc.bottom - rc.top;
            if (cw > 0 && ch > 0) {
                m_width = cw;
                m_height = ch;
            }
        }
    }

    std::cout << "[Application] Win32 window created successfully." << std::endl;
} 

// ============================================================= 
// BGFX SETUP
// =============================================================
void Application::InitBgfx() {
    std::cout << "[Application] Initializing bgfx..." << std::endl;
    // Only compile shaders in editor mode; standalone relies on precompiled .bin files
    if (m_RunEditorUI) {
        ShaderManager::Instance().CompileAllShaders();
    }

    Renderer::Get().Init(m_width, m_height, (void*)m_window);
    std::cout << "[Application] bgfx initialized." << std::endl;
}

// =============================================================
// IMGUI SETUP
// =============================================================
void Application::InitImGui() { 
    std::cout << "[Application] Initializing ImGui..." << std::endl;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable; // Disabled for now
    io.IniFilename = nullptr; // The editor persists layout in project-scoped workspace settings.
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigWindowsResizeFromEdges = true;

    // Style baseline
    ImGui::StyleColorsDark();

    // Font (DPI aware)
    float contentScale = 1.0f;
#ifdef _WIN32
    // Prefer per-monitor DPI if available (Win10+)
    UINT dpi = 96;
    if (m_window) {
        // GetDpiForWindow is available on Windows 10+; fall back gracefully
        HMODULE user32 = ::GetModuleHandleA("User32.dll");
        if (user32) {
            typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
            auto pGetDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow"));
            if (pGetDpiForWindow) {
                dpi = pGetDpiForWindow((HWND)m_window);
            } else {
                // Fallback to desktop DC DPI
                HDC screen = GetDC(nullptr);
                if (screen) { dpi = (UINT)GetDeviceCaps(screen, LOGPIXELSX); ReleaseDC(nullptr, screen); }
            }
        }
    }
    contentScale = (dpi > 0) ? (float)dpi / 96.0f : 1.0f;
#endif
    float baseFontSize = 16.0f * contentScale;
    editorui::ApplyProjectEditorStyle(baseFontSize);
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.OversampleH = 3;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = false;
    cfg.GlyphRanges = io.Fonts->GetGlyphRangesDefault();


    // ENABLE FOR CUSTOM FONT

    // Try Roboto if available; ImGui will fall back to default if this fails to load later
    ImFont* loadedFont = io.Fonts->AddFontFromFileTTF("assets/fonts/Roboto-Regular.ttf", baseFontSize, &cfg);
    if (!loadedFont) {
        loadedFont = io.Fonts->AddFontDefault(&cfg);
    }

    io.FontGlobalScale = 1.0f; // fonts already scaled above

    // Backend Init
    ImGui_ImplWin32_Init(m_window);
    ImGui_ImplBgfx_Init(255);

    // Preload the default Latin range so editor text doesn't depend on
    // first-use glyph baking, but avoid ImFontAtlas::Build(), which new
    // dynamic-texture backends explicitly reject once RendererHasTextures is set.
    if (loadedFont) {
        io.FontDefault = loadedFont;
        PrewarmImGuiGlyphRanges(loadedFont, cfg.GlyphRanges);
    }

    // Editor UI Layer
    uiLayer = std::make_unique<UILayer>();

    std::cout << "[Application] ImGui initialized." << std::endl;
}

// =============================================================
// MAIN LOOP
// =============================================================
void Application::Run() {
    std::cout << "[Application] Running main loop..." << std::endl;
    Time::Init();
    Profiler::Get().SetTelemetryEnabled(true);
    Profiler::Get().ConfigureDefaultBudgets();

    // In your app/game-loop bootstrap, ON THE THREAD that will call Scene::Update: 
    // SyncContextPtr is whatever function you use to sync to the main thread.
    // Used for async/awaiting tasks in C# scripts.
    if (InstallSyncContextPtr) {
       InstallSyncContextPtr();
       }

    bool shouldClose = false;
    while (!shouldClose) {
        // Profiling only runs when profiler window is open (editor mode only)
        if (m_RunEditorUI && uiLayer) {
            Profiler::Get().SetEnabled(uiLayer->GetProfilerPanel().IsOpen());
        } else {
            Profiler::Get().SetEnabled(false);
        }
        Profiler::Get().BeginFrame();
        const auto frameStart = std::chrono::high_resolution_clock::now();
        Time::Tick();

        // Reset per-frame input state BEFORE pumping events so edges are for this frame
        Input::Update();

        // Pump Win32 events non-blocking (fills input states for this frame)
        if (m_Win32Window) {
            m_Win32Window->PumpEvents();
            // Keep cursor centered when captured (true relative mouse mode behavior)
            m_Win32Window->CenterCursorIfCaptured();
        }

        // In the editor, mouse capture is normally only valid during play mode.
        // Edit-mode transform shortcuts can opt into relative mouse temporarily.
        if (m_RunEditorUI &&
            !IsPlaying() &&
            !m_EditModeMouseCaptureAllowed &&
            m_Win32Window &&
            m_Win32Window->IsCursorCaptured()) {
            SetMouseCaptured(false);
        }

        // Process pending resize at frame boundary (not inside WM_SIZE)
        if (m_PendingResize)
        {
            m_PendingResize = false;

            m_width  = m_PendingWidth;
            m_height = m_PendingHeight;

            if (m_width > 0 && m_height > 0)
            {
                bgfx::reset((uint32_t)m_width,
                            (uint32_t)m_height,
                            Renderer::kPresentationResetFlags);

                Renderer::Get().Resize((uint32_t)m_width,
                                       (uint32_t)m_height);
            }
        }

        // Check close flag
        if (m_Win32Window && m_Win32Window->ShouldClose()) shouldClose = true;

        float dt = Time::GetDeltaTime();
        // Scene reference must be determined AFTER UI may toggle play/stop
        // Do not bind a reference to *Scene::CurrentScene here.
        // --------------------------------------
        // Fullscreen toggle (game/exported mode only)
        // --------------------------------------
        if (!m_RunEditorUI) {
            // VK_F11 == 0x7A; our Input maps ASCII and VK_DELETE, so check raw VK state via GetAsyncKeyState
            static bool prevF11 = false;
            bool f11Down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
            if (f11Down && !prevF11) {
                if (m_Win32Window) m_Win32Window->ToggleFullscreen();
                // Trigger a resize for bgfx
                int w = m_Win32Window ? m_Win32Window->GetWidth() : m_width;
                int h = m_Win32Window ? m_Win32Window->GetHeight() : m_height;
                if (w > 0 && h > 0) {
                    bgfx::reset((uint32_t)w, (uint32_t)h, Renderer::kPresentationResetFlags);
                    Renderer::Get().Resize((uint32_t)w, (uint32_t)h);
                }
            }
            prevF11 = f11Down;
        }

        // --------------------------------------
        // ASSET PIPELINE PROCESSING
        // --------------------------------------
        // Handles:
        // 1. Files flagged by AssetWatcher → Queued imports
        // 2. CPU pre-processing (decoding textures/models)
        // 3. GPU uploads (executed on main thread for safety)
        m_AssetPipeline->ProcessMainThreadTasks();
        
        // Flush any pending resource changes so managed scripts can receive updates
        ResourceManifest::Get().FlushPendingChanges();

        // --------------------------------------
        // START NEW IMGUI FRAME (editor mode only)
        // --------------------------------------
        if (m_RunEditorUI) {
            ImGui_ImplWin32_NewFrame();
            ImGui_ImplBgfx_NewFrame();
            ImGui::NewFrame();
            
            // Disable Tab key for ImGui navigation - reserve it for game input
            // This must be called each frame after NewFrame()
            // ImGuiKeyOwner_Any requires a LockXXX flag to "eat" the key
            ImGui::SetKeyOwner(ImGuiKey_Tab, ImGuiKeyOwner_Any, ImGuiInputFlags_LockThisFrame);
            
            // When mouse is captured for gameplay, prevent ImGui from hovering/capturing inputs.
            // Edit-mode transform shortcuts use relative mouse too, but keep a virtual ImGui
            // mouse so ImGuizmo can continue its keyboard manipulation path.
            const bool editorTransformCapture =
                m_RunEditorUI && !IsPlaying() && m_EditModeMouseCaptureAllowed;
            if (Input::IsRelativeMode() && !editorTransformCapture) {
                ImGuiIO& io = ImGui::GetIO();
                io.WantCaptureMouse = false;
                io.WantCaptureKeyboard = false;
                // Move mouse off-screen for ImGui so panels don't highlight/hover
                io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
                ImGui::ClearActiveID();
            } else if (Input::IsRelativeMode() && editorTransformCapture) {
                ImGuiIO& io = ImGui::GetIO();
                io.WantCaptureMouse = true;
                io.WantCaptureKeyboard = true;
            }
        }

        // --------------------------------------
        // UI RENDER (Docking Panels, Inspector, etc.) (editor mode only)
        // This may toggle Play/Stop and create/destroy the runtime clone.
        // --------------------------------------
        if (m_RunEditorUI) {
            /*uiLayer->HandleCameraControls();*/
            {
                ScopedTimer t("UI");
                uiLayer->OnUIRender();
            }
        }

        // --------------------------------------
        // SCENE UPDATE (decide after UI may have toggled Play/Stop)
        // --------------------------------------
        if (m_RunEditorUI) {
            Scene& editorScene = uiLayer->GetScene();
            if (editorScene.m_RuntimeScene) {
                // Ensure CurrentScene points to runtime clone
                if (Scene::CurrentScene != editorScene.m_RuntimeScene.get()) {
                    Scene::CurrentScene = editorScene.m_RuntimeScene.get();
                    if (InstallSyncContextPtr) InstallSyncContextPtr();
                    if (ClearSyncContextPtr) ClearSyncContextPtr();
                }
                {
                    ScopedTimer t("Scene/Update (Play)");
                    editorScene.m_RuntimeScene->Update(dt);
                }
                // Update grass deformation for runtime scene
                TerrainGrass::UpdateDeformationSystem(*editorScene.m_RuntimeScene, dt);
            } else {
                // Ensure CurrentScene points back to editor scene
                if (Scene::CurrentScene != &editorScene) {
                    Scene::CurrentScene = &editorScene;
                    if (ClearSyncContextPtr) ClearSyncContextPtr();
                }
                {
                    ScopedTimer t("Scene/Update (Edit)");
                    editorScene.Update(dt);
                }
                {
                    ScopedTimer t("Skinning");
                    SkinningSystem::Update(editorScene);
                }
                // Update grass deformation for editor scene
                TerrainGrass::UpdateDeformationSystem(editorScene, dt);
            }
        } else {
            // Game mode without editor UI
            if (Scene::CurrentScene) {
                ScopedTimer t("Scene/Update (Game)");
                Scene::CurrentScene->Update(dt);
                // Update grass deformation for game scene
                TerrainGrass::UpdateDeformationSystem(*Scene::CurrentScene, dt);
            }
        }

        if (m_RunEditorUI && uiLayer) {
            const bool captureRuntimeStats =
                uiLayer->IsPlayMode() &&
                uiLayer->IsRuntimeStatsPanelOpen() &&
                Renderer::Get().GetDebugDrawInPlayMode();
            Renderer::Get().SetRuntimeStatsCaptureEnabled(captureRuntimeStats);
        } else {
            Renderer::Get().SetRuntimeStatsCaptureEnabled(false);
        }

        // Update audio system (listener tracking, sound cleanup)
        Audio::Update(dt);

        // --------------------------------------
        // SCENE RENDER
        // --------------------------------------
        {
            ScopedTimer t("Renderer/BeginFrame");
            Renderer::Get().BeginFrame(0.1f, 0.1f, 0.1f);
        }
        if (m_RunEditorUI) {
            Scene& editorScene = uiLayer->GetScene();
            Scene* sceneForOutline = nullptr;
            // Track custom cursor visibility across play/edit mode transitions
            static bool s_editorCursorHidden = false;
            static bool s_wasInPlayMode = false;
            
            // Reset cursor state when transitioning out of play mode
            bool isInPlayMode = (editorScene.m_RuntimeScene != nullptr);
            if (s_wasInPlayMode && !isInPlayMode) {
                // Just exited play mode - ensure cursor is visible
                EnsureSystemCursorVisible();
                s_editorCursorHidden = false;
            }
            s_wasInPlayMode = isInPlayMode;
            
            if (editorScene.m_RuntimeScene) {
                {
                    ScopedTimer t("Renderer/RenderScene (Play)");
                    Renderer::Get().RenderScene(*editorScene.m_RuntimeScene);
                }
                sceneForOutline = editorScene.m_RuntimeScene.get();
                
                // Render custom game cursor in play mode if enabled
                const auto& cursorSettings = Project::GetCursorSettings();
                bool hasCustomCursor = cursorSettings.previewInEditor && !cursorSettings.texturePath.empty();
                bool isViewportHovered = uiLayer->IsViewportHovered();
                bool isMouseCaptured = Input::IsRelativeMode();
                
                // When mouse is captured, Win32Window handles cursor visibility
                // When mouse is visible and we have custom cursor, show custom cursor only when hovering viewport
                bool shouldShowCustomCursor = hasCustomCursor && isViewportHovered && !isMouseCaptured;
                
                // Manage system cursor visibility:
                // - Hide system cursor when showing custom cursor (mouse visible, hovering viewport)
                // - Show system cursor otherwise (not hovering, or mouse captured, or no custom cursor)
                static bool s_wasMouseCaptured = false;
                
                // Reset cursor state when transitioning from captured to free mode
                if (s_wasMouseCaptured && !isMouseCaptured) {
                    // Just toggled to free mode - reset cursor visibility state
                    EnsureSystemCursorVisible();
                    s_editorCursorHidden = false;
                }
                s_wasMouseCaptured = isMouseCaptured;
                
                if (!isMouseCaptured) {
                    // Only manage cursor visibility when mouse is NOT captured
                    // (Win32Window handles it when captured)
                    if (shouldShowCustomCursor) {
                        // Hide system cursor when showing custom cursor (hovering viewport)
                        if (!s_editorCursorHidden) {
                            ShowCursor(FALSE);
                            s_editorCursorHidden = true;
                        }
                    } else {
                        // Show system cursor when NOT showing custom cursor
                        // This handles: not hovering viewport, no custom cursor, or toggled to free mode
                        if (s_editorCursorHidden) {
                            // Restore cursor visibility - Windows uses reference counting
                            EnsureSystemCursorVisible();
                            s_editorCursorHidden = false;
                        }
                    }
                } else {
                    // Mouse is captured - ensure we're not interfering with Win32Window's cursor management
                    // Reset our state since Win32Window handles visibility
                    if (s_editorCursorHidden) {
                        s_editorCursorHidden = false;
                    }
                }
                
                if (shouldShowCustomCursor) {
                    static bgfx::TextureHandle s_editorCursorTex = BGFX_INVALID_HANDLE;
                    static std::string s_editorCursorPath;
                    static bgfx::UniformHandle s_editorCursorSampler = BGFX_INVALID_HANDLE;
                    static bgfx::ProgramHandle s_editorCursorProgram = BGFX_INVALID_HANDLE;
                    static int s_cursorWidth = 32, s_cursorHeight = 32;
                    
                    // Reacquire cursor texture through the shared asset cache so
                    // hot-reload does not interrupt the editor cursor.
                    std::string fullPath = (Project::GetAssetDirectory() / cursorSettings.texturePath).string();
                    if (!cursorSettings.texturePath.empty()) {
                        TextureSpecifier spec;
                        spec.Path = fullPath;
                        s_editorCursorTex = AcquireTextureHandle(spec, TextureColorSpace::sRGB);
                        s_editorCursorPath = fullPath;
                        s_cursorWidth = 32;
                        s_cursorHeight = 32;
                    } else {
                        s_editorCursorTex = BGFX_INVALID_HANDLE;
                        s_editorCursorPath.clear();
                    }
                    
                    if (bgfx::isValid(s_editorCursorTex)) {
                        // Initialize shader resources
                        if (!bgfx::isValid(s_editorCursorSampler)) {
                            s_editorCursorSampler = bgfx::createUniform("s_uiTex", bgfx::UniformType::Sampler);
                        }
                        if (!bgfx::isValid(s_editorCursorProgram)) {
                            s_editorCursorProgram = ShaderManager::Instance().LoadProgram("vs_ui", "fs_ui");
                        }
                        
                        // Get mouse position in viewport-local coordinates
                        float mouseX = 0.0f, mouseY = 0.0f;
                        bool hasMousePos = Renderer::Get().GetUIMousePosition(mouseX, mouseY);
                        
                        if (bgfx::isValid(s_editorCursorProgram) && bgfx::isValid(s_editorCursorSampler) && hasMousePos) {
                            // Get viewport dimensions
                            int width = Renderer::Get().GetWidth();
                            int height = Renderer::Get().GetHeight();
                            
                            // DPI scaling
                            float dpiScale = 1.0f;
                            if (cursorSettings.useDPIScaling) {
                                HDC hdc = GetDC(NULL);
                                if (hdc) {
                                    int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
                                    dpiScale = static_cast<float>(dpiX) / 96.0f;
                                    ReleaseDC(NULL, hdc);
                                }
                            }
                            
                            // Calculate cursor size
                            float defaultCursorSize = 32.0f * dpiScale;
                            float scale = cursorSettings.baseScale * (defaultCursorSize / std::max(s_cursorWidth, s_cursorHeight));
                            float cursorW = s_cursorWidth * scale;
                            float cursorH = s_cursorHeight * scale;
                            
                            // Position with hotspot offset (hotspot is normalized 0-1)
                            float hotspotOffsetX = cursorSettings.hotspotX * cursorW;
                            float hotspotOffsetY = cursorSettings.hotspotY * cursorH;
                            float x0 = mouseX - hotspotOffsetX;
                            float y0 = mouseY - hotspotOffsetY;
                            float x1 = x0 + cursorW;
                            float y1 = y0 + cursorH;
                            
                            // Render cursor quad to scene framebuffer
                            const uint16_t cursorViewId = 4; // After debug overlay (3), before other passes
                            float orthoProj[16];
                            bx::mtxOrtho(orthoProj, 0.0f, (float)width, (float)height, 0.0f, 0.0f, 1000.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
                            bgfx::setViewRect(cursorViewId, 0, 0, uint16_t(width), uint16_t(height));
                            bgfx::setViewTransform(cursorViewId, nullptr, orthoProj);
                            bgfx::setViewClear(cursorViewId, BGFX_CLEAR_NONE);
                            bgfx::setViewFrameBuffer(cursorViewId, Renderer::Get().GetSceneFrameBuffer());
                            bgfx::touch(cursorViewId); // Ensure the view is processed
                            
                            // Vertex layout
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
                            
                            if (bgfx::getAvailTransientVertexBuffer(4, s_cursorLayout) >= 4 && 
                                bgfx::getAvailTransientIndexBuffer(6) >= 6) {
                                bgfx::TransientVertexBuffer tvb;
                                bgfx::TransientIndexBuffer tib;
                                bgfx::allocTransientVertexBuffer(&tvb, 4, s_cursorLayout);
                                bgfx::allocTransientIndexBuffer(&tib, 6);
                                
                                struct CursorVert { float x, y, z, u, v; uint32_t c; };
                                CursorVert* verts = (CursorVert*)tvb.data;
                                uint16_t* indices = (uint16_t*)tib.data;
                                
                                uint32_t white = 0xFFFFFFFF;
                                verts[0] = { x0, y0, 0.0f, 0.0f, 0.0f, white };
                                verts[1] = { x1, y0, 0.0f, 1.0f, 0.0f, white };
                                verts[2] = { x1, y1, 0.0f, 1.0f, 1.0f, white };
                                verts[3] = { x0, y1, 0.0f, 0.0f, 1.0f, white };
                                indices[0] = 0; indices[1] = 1; indices[2] = 2;
                                indices[3] = 0; indices[4] = 2; indices[5] = 3;
                                
                                bgfx::setVertexBuffer(0, &tvb);
                                bgfx::setIndexBuffer(&tib);
                                bgfx::setTexture(0, s_editorCursorSampler, s_editorCursorTex);
                                uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                                               | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA);
                                bgfx::setState(state);
                                bgfx::submit(cursorViewId, s_editorCursorProgram);
                            }
                        }
                    }
                }
            } else {
                // Restore system cursor if it was hidden during play mode
                if (s_editorCursorHidden) {
                    EnsureSystemCursorVisible();
                    s_editorCursorHidden = false;
                }
                
                {
                    ScopedTimer t("Renderer/RenderScene (Edit)");
                    Renderer::Get().RenderScene(editorScene);
                }
                sceneForOutline = &editorScene;
                // Editor-only: draw outline for selected entity
                {
                    ScopedTimer t("Renderer/DrawOutline");
                    Renderer::Get().DrawEntityOutline(editorScene, uiLayer->GetSelectedEntity());
                }
                // Editor-only: draw hover outline if enabled and not the selected entity
                {
                    const EditorSettings& settings = EditorSettings::Get();
                    EntityID hoveredEntity = uiLayer->GetHoveredEntity();
                    if (settings.ShowHoverOutline && hoveredEntity != INVALID_ENTITY_ID && hoveredEntity != uiLayer->GetSelectedEntity()) {
                        Renderer::Get().DrawEntityOutline(editorScene, hoveredEntity, settings.HoverOutlineColor, settings.HoverOutlineThickness);
                    }
                }
            }

            if (sceneForOutline) {
                ScopedTimer t("Renderer/SceneOutline");
                Renderer::Get().DrawSceneOutline(*sceneForOutline);
            }
        } else {
            Scene* sceneToRender = Scene::CurrentScene;
            {
                ScopedTimer t("Renderer/RenderScene (Game)");
                Renderer::Get().RenderScene(*sceneToRender);
            }
            {
                ScopedTimer t("Renderer/SceneOutline");
                Renderer::Get().DrawSceneOutline(*sceneToRender);
            }
        }

        // --------------------------------------
        // ENTITY PICKING (skip if UI consumed input this frame) (editor mode only, not in play mode)
        // --------------------------------------
        if (m_RunEditorUI && !IsPlaying() && !Renderer::Get().WasUIInputConsumedThisFrame()) {
            ScopedTimer t("Picking");
            Picking::Process(uiLayer->GetScene(), Renderer::Get().GetCamera());
        }
        if (m_RunEditorUI && !IsPlaying()) {
            int pickedEntity = Picking::GetLastPick();
            if (pickedEntity != -1) {
                const bool preferRootSelection = Picking::LastPickPrefersRootSelection();
                uiLayer->SetSelectedEntity(
                    Picking::ResolveSelectionEntity(pickedEntity, uiLayer->GetScene(), preferRootSelection));

                // Ensure hierarchy expands to show the selected entity
                uiLayer->GetSceneHierarchyPanel().ExpandTo(uiLayer->GetSelectedEntity());
            } else if (Picking::HadPickThisFrame() && !Picking::HadHitThisFrame()) {
                // Clear immediately on a processed miss if configured to deselect on empty clicks
                if (EditorSettings::Get().DeselectOnEmptyClick) {
                    uiLayer->SetSelectedEntity(-1);
                }
            }
        }

        // --------------------------------------
        // IMGUI RENDER PASS (editor mode only)
        // --------------------------------------
        if (m_RunEditorUI) {
            ScopedTimer t("UI/Render");
            ImGui::Render();
            
            // Ensure cursor visibility is correct after ImGui processing
            // This handles cases where ImGui might have hidden the cursor or our state got out of sync
            if (IsPlaying() && uiLayer) {
                Scene& editorScene = uiLayer->GetScene();
                if (editorScene.m_RuntimeScene) {
                    bool isMouseCaptured = Input::IsRelativeMode();
                    bool isViewportHovered = uiLayer->IsViewportHovered();
                    const auto& cursorSettings = Project::GetCursorSettings();
                    bool hasCustomCursor = cursorSettings.previewInEditor && !cursorSettings.texturePath.empty();
                    bool shouldShowCustomCursor = hasCustomCursor && isViewportHovered && !isMouseCaptured;
                    
                    // When mouse is free and we're not showing custom cursor, ensure system cursor is visible
                    // This fixes the issue where cursor is invisible outside viewport in free mode
                    if (!isMouseCaptured && !shouldShowCustomCursor) {
                        EnsureSystemCursorVisible();
                    }
                }
            }
            
            bgfx::setViewFrameBuffer(255, BGFX_INVALID_HANDLE);
            bgfx::setViewRect(255, 0, 0, uint16_t(m_width), uint16_t(m_height));
            bgfx::touch(255);
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
            ImGui_ImplBgfx_Render(255, ImGui::GetDrawData(), 0x00000000);
        }

        // --------------------------------------
        // SUBMIT FRAME
        // --------------------------------------
        {
            ScopedTimer t("Renderer/SubmitFrame");
            Renderer::Get().EndFrame();
        }
        const auto frameEnd = std::chrono::high_resolution_clock::now();
        Profiler::Get().Record(
            "Frame",
            std::chrono::duration<double, std::milli>(frameEnd - frameStart).count());
        Profiler::Get().EndFrame();

    }

    std::cout << "[Application] Main loop ended." << std::endl;
}

// =============================================================
// SHUTDOWN
// =============================================================
void Application::Shutdown() {
    if (m_ShutdownCompleted) {
        return;
    }
    m_ShutdownCompleted = true;
    std::cout << "[Application] Shutting down..." << std::endl;

    // Ensure gameplay runtime is torn down before physics to avoid Jolt
    // objects (e.g., CharacterVirtual) destroying after Physics::Shutdown.
    if (m_RunEditorUI && uiLayer) {
        Scene& editorScene = uiLayer->GetScene();
        if (editorScene.m_RuntimeScene) {
            runtime::RuntimePrefabInstantiator::CancelAsyncForScene(*editorScene.m_RuntimeScene, true);
            editorScene.m_RuntimeScene->OnStop();
            editorScene.m_RuntimeScene.reset();
            Scene::CurrentScene = &editorScene;
        }
        // Also ensure any lingering runtime resources in the active scene are released
        Scene::Get().OnStop();
    } else {
        // Game mode (non-editor): cleanup runtime scene properly
        if (m_RuntimeScene) {
            runtime::RuntimePrefabInstantiator::CancelAsyncForScene(*m_RuntimeScene, true);
            m_RuntimeScene->OnStop();
            m_RuntimeScene.reset();
        }
        if (m_GameScene) {
            m_GameScene->OnStop();
            m_GameScene.reset();
        }
        Scene::CurrentScene = nullptr;
    }
    runtime::RuntimePrefabInstantiator::ResetRuntimeCaches();

    // Shutdown Audio System
    Audio::Shutdown();

    // Shutdown Physics System (after scenes/controllers are released)
    Physics::Get().Shutdown();

    if (m_AssetWatcher) {
        m_AssetWatcher->Stop();
    }

    // Flush and persist binary cache data
    BinaryAssetCache::Instance().Shutdown();

    if (m_RunEditorUI) {
        // Release bgfx resources from panels BEFORE shutting down bgfx
        // ProjectPanel holds image thumbnails that need to be destroyed before bgfx::shutdown()
        if (uiLayer) {
            uiLayer->GetProjectPanel().ReleaseAllImageThumbnails();
        }
        // Destroy the UI layer before ImGui/bgfx teardown so panel destructors
        // (including ImNodes context destruction) run while dependencies are still alive.
        uiLayer.reset();
        // Texture picker thumbnails are destroyed here to avoid bgfx shutdown crashes.
        texturepicker::ClearCachedThumbnails();
        texturepicker::SetBgfxActive(false);
        
        ImGui_ImplBgfx_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    // Ensure renderer and bgfx threads are fully shutdown before destroying the window
    StandardMeshManager::Instance().Shutdown();
    Renderer::Get().Shutdown();
    m_Win32Window.reset();
    // JobSystem must be torn down after scene/UI destruction to avoid dangling
    // cm::g_JobSystem usage from Scene destructors.
    cm::g_JobSystem = nullptr;
    m_Jobs.reset();

    std::cout << "[Application] Shutdown complete." << std::endl;
}

Application& Application::Get() {
    if (!s_Instance)
        throw std::runtime_error("Application::Get() called before Application was created!");
    return *s_Instance;
}

// ------------------------------------------------------------
// Playmode controls (editor mode only)
// ------------------------------------------------------------
void Application::StartPlayMode() {
    if (m_IsPlaying) return;
    if (!m_RunEditorUI) return;
    if (!uiLayer) return;

    m_EditModeMouseCaptureAllowed = false;
    if (m_Win32Window && m_Win32Window->IsCursorCaptured()) {
        SetMouseCaptured(false);
    }
    Scene& editorScene = uiLayer->GetScene();
    if (editorScene.m_RuntimeScene) return;

    // Physics world is global. Detach edit-scene bodies first so runtime clone
    // is the only active source of colliders during play mode.
    editorScene.ReleasePhysicsRuntimeState();
    runtime::RuntimePrefabInstantiator::ResetRuntimeCaches();

    editorScene.m_RuntimeScene = editorScene.RuntimeClone();
    if (editorScene.m_RuntimeScene) {
        editorScene.m_RuntimeScene->m_IsPlaying = true;
        Scene::CurrentScene = editorScene.m_RuntimeScene.get();
        m_IsPlaying = true;
    }
}

void Application::StopPlayMode() {
    if (!m_IsPlaying) return;
    if (!m_RunEditorUI) return;
    if (!uiLayer) return;

    Scene& editorScene = uiLayer->GetScene();
    if (editorScene.m_RuntimeScene) {
        runtime::RuntimePrefabInstantiator::CancelAsyncForScene(*editorScene.m_RuntimeScene, true);
        editorScene.m_RuntimeScene->OnStop(); // also stops scene audio (Audio::StopAll)
    }
    editorScene.m_RuntimeScene.reset();
    Scene::CurrentScene = &editorScene;
    runtime::RuntimePrefabInstantiator::ResetRuntimeCaches();
    if (g_ClearComponentCaches) {
        g_ClearComponentCaches();
    }
    m_IsPlaying = false;
    m_EditModeMouseCaptureAllowed = false;
    
    // Ensure cursor is visible when exiting play mode
    // Reset any cursor visibility state that might have been set during play mode
    EnsureSystemCursorVisible();
    
    // If mouse was captured, release it
    if (m_Win32Window && m_Win32Window->IsCursorCaptured()) {
        SetMouseCaptured(false);
    }
}

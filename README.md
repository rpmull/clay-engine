# Claymore

Claymore is the `cmeng` engine/editor tree: a Windows-first C++17 game engine with a native editor, a stripped runtime target for packaged games, and a managed .NET scripting layer loaded through the native host.

The project builds two main executables:

- `ClaymoreEditor.exe` (output name `Claymore.exe`): the full authoring environment.
- `ClaymoreRuntime` (output name `GameRuntime.exe`): the lean runtime used for exported projects.

At a high level, the codebase is split like this:

- `src/core`: runtime-safe engine systems such as scene/ECS, rendering, physics, navigation, prefabs, resources, serialization, VFS, dialogue, quests, terrain, particles, world graph, and multiplayer hooks.
- `src/editor`: editor-only code for the project browser, asset pipeline, importers, animation tooling, shader graph tooling, prefab tools, preview panels, and build/export flow.
- `src/runtime`: runtime launch glue and managed/runtime bridge helpers.
- `managed/ClaymoreEngine`: the managed API surface that the native host loads at runtime.
- `managed/ScriptCompiler`: the Roslyn-based compiler the editor uses for managed gameplay scripts.
- `managed/Modules/Claymore.Modules.RPG`: a sample gameplay module layered on top of the engine runtime.
- `tests`: a small native headless test harness, currently centered on scene serialization and round-trip checks.

The renderer is built around bgfx. Physics is handled through Jolt Physics. Navigation is backed by Recast/Detour. Editor importers are wired through Assimp. The editor UI is Dear ImGui-based, with ImGuizmo, imnodes, and ImGuiColorTextEdit layered on top.

## Platform and Build Notes

This tree is Windows-first right now.

- The managed projects target `.NET 10`.
- The helper scripts publish for `win-x64`.
- `CMakeLists.txt` expects OpenSSL under `C:\\Program Files\\OpenSSL-Win64`.
- The bgfx artifact path is hard-wired to `external/bgfx/.build/win64_vs2022`.

There are Linux branches in the CMake file, but the supported and documented bootstrap path in this repository is the Windows one.

## Submodules and Dependency Prep

Clone with submodules from the start if you can:

```powershell
git clone --recurse-submodules <your-public-repo-url>
cd cmeng-public
```

If you already cloned without them:

```powershell
git submodule update --init --recursive
```

The public snapshot intentionally keeps only the dependency repos that are part of the current build:

- `external/bgfx`
- `external/bx`
- `external/bimg`
- `external/JoltPhysics`
- `external/assimp`
- `external/glm`
- `external/json`
- `external/imnodes`
- `external/imgui_colortextedit`
- `external/recastnavigation`

Historical submodule declarations such as `glfw` and `SDL` were not carried forward because the current CMake build does not reference them.

## Recommended Windows Bootstrap

The canonical Windows bootstrap path in this repo is the helper script:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\windows\Build-CMeng.ps1 -SkipGitPull
```

That script does the repo-specific prep work that matters here:

- syncs and initializes submodules
- ensures `external/bgfx` exists
- generates bgfx Visual Studio projects through `external\bx\tools\bin\windows\genie.exe`
- builds the required bgfx artifacts (`bgfx`, `bimg`, `bx`, and `shaderc`)
- configures the CMake project
- builds the editor target

If you want the repo to behave the way the current CMake files expect, that script is the safest path.

## Manual Build Outline

If you do not want to use the helper script, the minimum manual flow is:

1. Initialize submodules recursively.
2. Prepare bgfx so these Windows artifacts exist under `external/bgfx/.build/win64_vs2022/bin`:
   `bgfxDebug.lib`, `bgfxRelease.lib`, `bimgDebug.lib`, `bimgRelease.lib`, `bxDebug.lib`, `bxRelease.lib`, and a matching `shaderc` executable.
3. Make sure OpenSSL is installed at `C:\\Program Files\\OpenSSL-Win64`.
4. Make sure a .NET 10 SDK/runtime is installed.
5. Configure CMake:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

6. Build the editor:

```powershell
cmake --build build --config Debug --target ClaymoreEditor.exe
```

To build the stripped runtime target instead:

```powershell
cmake --build build --config Debug --target ClaymoreRuntime
```

## Notes on This Public Snapshot

This repository is meant to be source-first.

- Generated build output is intentionally not tracked.
- Generated managed binaries and helper executables were removed from this source snapshot.
- Vendored Dear ImGui and ImGuizmo sample/demo folders were dropped so the public tree stays focused on engine-used source.
- The old in-tree `ThirdPartyNotices.txt` was replaced with `LICENSES.md`.
- A minimal .NET hosting bundle is kept under `external/dotnet_hosting` because the native host loads `nethost.dll` directly.

## License

The engine source in this repository is licensed under GPL-3.0. Third-party software kept in-tree or fetched through submodules retains its own license. See `LICENSE` and `LICENSES.md`.

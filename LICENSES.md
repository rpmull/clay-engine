# Third-Party Licenses and Attributions

The engine source in this repository is licensed under GPL-3.0; see `LICENSE`.

This file tracks the third-party code intentionally kept in this public snapshot, plus the dependency repositories fetched through submodules. Each third-party project keeps its own copyright and license terms.

## Dependency Inventory

| Component | Location | Why it is here | License | Local license text / note |
| --- | --- | --- | --- | --- |
| bgfx | `external/bgfx` (submodule) | renderer backend and shader toolchain | BSD 2-Clause | `external/bgfx/LICENSE` |
| bx | `external/bx` (submodule) | bgfx support library and project generator tooling | BSD 2-Clause | `external/bx/LICENSE` |
| bimg | `external/bimg` (submodule) | bgfx image/texture support | BSD 2-Clause | `external/bimg/LICENSE` |
| Jolt Physics | `external/JoltPhysics` (submodule) | rigid body physics and collision systems | MIT | `external/JoltPhysics/LICENSE` |
| Assimp | `external/assimp` (submodule) | editor asset import pipeline | BSD 3-Clause | `external/assimp/LICENSE` |
| GLM | `external/glm` (submodule) | math library | MIT option from upstream dual license | `external/glm/copying.txt` |
| nlohmann/json | `external/json` (submodule) | JSON serialization and tooling data | MIT | `external/json/LICENSE.MIT` |
| imnodes | `external/imnodes` (submodule) | node graph UI widgets | MIT | `external/imnodes/LICENSE.md` |
| ImGuiColorTextEdit | `external/imgui_colortextedit` (submodule) | code/text editing widget in the editor | MIT | `external/imgui_colortextedit/LICENSE` |
| Recast / Detour | `external/recastnavigation` (submodule) | navmesh bake and query systems | permissive Recast license | `external/recastnavigation/License.txt` |
| Dear ImGui | `external/imgui` | core editor UI | MIT | `external/imgui/LICENSE.txt` |
| ImGuizmo | `external/ImGuizmo` | transform gizmos, curve editing, sequencer widgets | MIT | `external/ImGuizmo/LICENSE` |
| NanoSVG / NanoSVGRast | `src/core/rendering/nanosvg.h`, `src/core/rendering/nanosvgrast.h` | SVG parsing and rasterization used by engine-side texture/icon loading | zlib-style permissive license | notice kept at the top of each header |
| Roboto | `assets/fonts/Roboto-Regular.ttf` | default editor/runtime font asset | Apache 2.0 | `licenses/Apache-2.0.txt` |
| miniaudio | `external/miniaudio/miniaudio.h` | audio playback/capture backend | Public Domain or MIT-0 | license text lives in the header |
| stb headers | `external/stb` | image loading, image writing, font rasterization | Public Domain or MIT | license text lives in each header |
| Microsoft .NET hosting components | `external/dotnet_hosting` | native hosting headers plus `nethost.dll` | MIT | local files are derived from the .NET runtime hosting layer |

## Notes

- `external/assimp` carries additional upstream notices under `contrib/` and in some test asset directories. If you redistribute the full submodule contents, review those extra notices as well.
- `external/imnodes` contains its own nested `vcpkg` submodule upstream. Clone this repository with `--recurse-submodules` or run `git submodule update --init --recursive` so nested third-party content resolves cleanly.
- GLM upstream publishes `copying.txt` with both the Happy Bunny License text and an MIT alternative. This repository relies on the MIT option.
- `src/core/utils/LZ4.h` is a local implementation that references Yann Collet's LZ4 algorithm in the file header; it is not a wholesale vendored copy of upstream `lz4`.
- The vendored Dear ImGui and ImGuizmo directories were trimmed to the source the engine/editor actually uses; their upstream sample/demo folders are not part of this public snapshot.
- An older in-tree `ThirdPartyNotices.txt` was retired here in favor of this narrower manifest tied to the dependencies that remain in the public snapshot.
- Historical `glfw` and `SDL` submodule declarations were removed from the public tree because the current build does not reference them.
- This manifest covers the third-party code and font assets that are clearly identifiable from the repository contents. If any textures, models, audio, or icons under `assets/` came from external packs or marketplaces, add their provenance before mirroring or redistributing the repository.

## What to Preserve When Redistributing

If you redistribute this repository or binaries built from it:

- keep the top-level `LICENSE`
- keep this `LICENSES.md`
- keep the original license files that ship inside vendored directories and submodules
- do not remove copyright notices from third-party source files

cmake_minimum_required(VERSION 3.15)
project(Claymore)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ============================
# MSVC OPTIONS
# ============================
if(MSVC)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")  # /Z7
    add_compile_options(/Zc:__cplusplus /Zc:preprocessor /bigobj)
endif()

# ============================
# WINDOWS APP ICON
# ============================
if(WIN32)
    find_program(CLAYMORE_POWERSHELL_EXECUTABLE NAMES powershell.exe powershell pwsh.exe REQUIRED)

    set(CLAYMORE_WINDOWS_RESOURCE_DIR "${CMAKE_BINARY_DIR}/generated/windows")
    set(CLAYMORE_WINDOWS_ICON_SOURCE "${CMAKE_SOURCE_DIR}/assets/icons/claymore_icon.png")
    set(CLAYMORE_WINDOWS_ICON_ICO "${CLAYMORE_WINDOWS_RESOURCE_DIR}/claymore_icon.ico")
    set(CLAYMORE_WINDOWS_ICON_ICO_RC_PATH "${CLAYMORE_WINDOWS_ICON_ICO}")
    set(CLAYMORE_WINDOWS_ICON_RC "${CLAYMORE_WINDOWS_RESOURCE_DIR}/ClaymoreAppIcon.rc")

    file(MAKE_DIRECTORY "${CLAYMORE_WINDOWS_RESOURCE_DIR}")
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/windows/ClaymoreAppIcon.rc.in"
        "${CLAYMORE_WINDOWS_ICON_RC}"
        @ONLY
    )

    add_custom_command(
        OUTPUT "${CLAYMORE_WINDOWS_ICON_ICO}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CLAYMORE_WINDOWS_RESOURCE_DIR}"
        COMMAND "${CLAYMORE_POWERSHELL_EXECUTABLE}" -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tools/windows/Generate-WindowsIcon.ps1" -Source "${CLAYMORE_WINDOWS_ICON_SOURCE}" -Destination "${CLAYMORE_WINDOWS_ICON_ICO}"
        DEPENDS
            "${CLAYMORE_WINDOWS_ICON_SOURCE}"
            "${CMAKE_SOURCE_DIR}/tools/windows/Generate-WindowsIcon.ps1"
        COMMENT "Generating Claymore Windows icon from claymore_icon.png"
        VERBATIM
    )

    set_source_files_properties(
        "${CLAYMORE_WINDOWS_ICON_RC}"
        PROPERTIES OBJECT_DEPENDS "${CLAYMORE_WINDOWS_ICON_ICO}"
    )
endif()

# ============================
# ASSIMP CONFIGURATION (STATIC LINKING - Editor Only)
# ============================
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ASSIMP_LIBRARY_SUFFIX "" CACHE STRING "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_COLLADA_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_BLEND_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)

add_subdirectory(external/assimp)

if(MSVC)
    set_target_properties(assimp PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()


# ============================
# JOLT PHYSICS
# ============================
file(GLOB_RECURSE JOLT_SRC external/JoltPhysics/Jolt/*.cpp)
add_library(JoltPhysics STATIC ${JOLT_SRC})
target_include_directories(JoltPhysics PUBLIC external/JoltPhysics)
if(MSVC)
    target_compile_options(JoltPhysics PRIVATE /wd4244 /wd4305)
endif()
set_target_properties(JoltPhysics PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ============================
# RECAST + DETOUR
# ============================
file(GLOB_RECURSE RECAST_DETOUR_SRC CONFIGURE_DEPENDS
    external/recastnavigation/Recast/Source/*.cpp
    external/recastnavigation/Detour/Source/*.cpp
)
add_library(RecastDetour STATIC ${RECAST_DETOUR_SRC})
target_include_directories(RecastDetour PUBLIC
    external/recastnavigation/Recast/Include
    external/recastnavigation/Detour/Include
)
if(MSVC)
    target_compile_options(RecastDetour PRIVATE /wd4244 /wd4305)
endif()
set_target_properties(RecastDetour PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ============================
# OPENSSL CONFIG
# ============================
set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64")
set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include")
set(OPENSSL_USE_STATIC_LIBS FALSE)
find_package(OpenSSL REQUIRED)
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
endif()

# ============================
# IMGUI (Editor Only)
# ============================
file(GLOB IMGUI_SRC CONFIGURE_DEPENDS
    external/imgui/*.cpp
    external/imgui/backends/imgui_impl_win32.cpp
)
set(IMGUI_REQUIRED_SOURCES
    external/imgui/imgui_claymore_style.cpp
    external/imgui/imgui_clay_inspector.cpp
)
foreach(_src IN LISTS IMGUI_REQUIRED_SOURCES)
    list(FIND IMGUI_SRC ${_src} _idx)
    if (_idx EQUAL -1)
        list(APPEND IMGUI_SRC ${_src})
    endif()
endforeach()
add_library(imgui STATIC ${IMGUI_SRC})
target_include_directories(imgui PUBLIC 
    external/imgui
    external/imgui/backends
    src
)
set_target_properties(imgui PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ============================
# IMGUI BGFX BACKEND (Editor Only)
# ============================
file(GLOB IMGUI_BGFX_SRC src/editor/ui/imgui_backend/imgui_impl_bgfx_docking.cpp)
add_library(imgui_bgfx STATIC ${IMGUI_BGFX_SRC})
target_include_directories(imgui_bgfx PUBLIC
    src
    external/bgfx/include
    external/imgui
    external/imgui/backends
    external/bx/include
    external/stb
    external/ImGuizmo
    external/imnodes
    external/bimg/include
)
target_link_libraries(imgui_bgfx PRIVATE imgui OpenSSL::SSL OpenSSL::Crypto JoltPhysics)
set_target_properties(imgui_bgfx PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ============================
# CLAYMORE CORE LIBRARY
# ============================
# Runtime-safe engine core - no editor dependencies, no Assimp, no ImGui
# Includes: biome system, deformation, navigation, physics, rendering, scripting bridge, etc.
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    src/core/*.cpp
    src/core/*.h
)

# Exclude runtime-only files from editor's ClaymoreCore
# These are included separately in ClaymoreRuntimeCore via RUNTIME_INTEROP_SOURCES
list(FILTER CORE_SOURCES EXCLUDE REGEX ".*/RuntimeHost\\.cpp$")
list(FILTER CORE_SOURCES EXCLUDE REGEX ".*/RuntimeInterop\\.cpp$")
list(FILTER CORE_SOURCES EXCLUDE REGEX ".*/RuntimeApplication\\.cpp$")
list(FILTER CORE_SOURCES EXCLUDE REGEX ".*/RuntimeStubs\\.cpp$")

add_library(ClaymoreCore STATIC ${CORE_SOURCES})
target_compile_definitions(ClaymoreCore PUBLIC CLAYMORE_CORE=1)
target_include_directories(ClaymoreCore PUBLIC
    src
    src/core
    external/bgfx/include
    external/bx/include
    external/bimg/include
    external/glm
    external/json/include
    external/JoltPhysics
    external/stb
    external/imgui
    external/imgui/backends
    external/miniaudio
    external/dotnet_hosting
)
target_link_libraries(ClaymoreCore PUBLIC
    JoltPhysics
    RecastDetour
    OpenSSL::SSL
    OpenSSL::Crypto
    imgui
)
if(WIN32)
    target_link_libraries(ClaymoreCore PUBLIC
        ${CMAKE_SOURCE_DIR}/external/bgfx/.build/win64_vs2022/bin/bgfx$<IF:$<CONFIG:Debug>,Debug,Release>.lib
        ${CMAKE_SOURCE_DIR}/external/bgfx/.build/win64_vs2022/bin/bimg$<IF:$<CONFIG:Debug>,Debug,Release>.lib
        ${CMAKE_SOURCE_DIR}/external/bgfx/.build/win64_vs2022/bin/bx$<IF:$<CONFIG:Debug>,Debug,Release>.lib
        user32 gdi32 imm32 shell32
    )
elseif(UNIX AND NOT APPLE)
    target_link_libraries(ClaymoreCore PUBLIC
        ${CMAKE_SOURCE_DIR}/external/bgfx/.build/linux64_gcc/bin/libbgfx$<IF:$<CONFIG:Debug>,Debug,Release>.a
        ${CMAKE_SOURCE_DIR}/external/bgfx/.build/linux64_gcc/bin/libbimg$<IF:$<CONFIG:Debug>,Debug,Release>.a
        ${CMAKE_SOURCE_DIR}/external/bgfx/.build/linux64_gcc/bin/libbx$<IF:$<CONFIG:Debug>,Debug,Release>.a
        Threads::Threads ${CMAKE_DL_LIBS}
        X11 Xcursor Xinerama Xi Xrandr GL
    )
endif()
set_target_properties(ClaymoreCore PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ============================
# CLAYMORE MANAGED LIBRARY (Scripting Bridge)
# ============================
file(GLOB_RECURSE MANAGED_SOURCES CONFIGURE_DEPENDS
    src/managed/*.cpp
    src/managed/*.h
)
set(MANAGED_EXTRA_SOURCES
    src/core/managed/RuntimeInterop.cpp
    src/core/managed/EntityInteropLayout.cpp
)

add_library(ClaymoreManaged STATIC ${MANAGED_SOURCES} ${MANAGED_EXTRA_SOURCES})
target_compile_definitions(ClaymoreManaged PUBLIC CLAYMORE_MANAGED=1 CLAYMORE_EDITOR=1)
target_include_directories(ClaymoreManaged PUBLIC
    src
    external/dotnet_hosting
    external/json/include
    external/glm
    external/JoltPhysics
)
target_link_libraries(ClaymoreManaged PUBLIC ClaymoreCore)
set_target_properties(ClaymoreManaged PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ============================
# CLAYMORE EDITOR LIBRARY
# ============================
# Editor-only code - ImGui, asset pipeline, importers, tools
file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
    src/editor/*.cpp
    src/editor/*.h
)

# External editor dependencies
set(EDITOR_EXTERNAL_SOURCES
    external/ImGuizmo/ImGuizmo.cpp
    external/ImGuizmo/ImSequencer.cpp
    external/ImGuizmo/ImCurveEdit.cpp
    external/ImGuizmo/GraphEditor.cpp
    external/imnodes/imnodes.cpp
)

add_library(ClaymoreEditor STATIC ${EDITOR_SOURCES} ${EDITOR_EXTERNAL_SOURCES})
target_compile_definitions(ClaymoreEditor PUBLIC CLAYMORE_EDITOR=1)
target_include_directories(ClaymoreEditor PUBLIC
    src
    src/editor
    external/bgfx/include
    external/bx/include
    external/bimg/include
    external/imgui
    external/imgui/backends
    external/imgui_colortextedit
    external/bgfx/examples
    external/bgfx/examples/common
    external/glm
    external/assimp/include
    external/ImGuizmo
    external/imnodes
    external/json/include
    external/JoltPhysics
    external/dotnet_hosting
    external/stb
)
target_link_libraries(ClaymoreEditor PUBLIC
    ClaymoreCore
    ClaymoreManaged
    imgui
    imgui_bgfx
    assimp
)
set_target_properties(ClaymoreEditor PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ============================
# CLAYMORE EDITOR EXECUTABLE
# ============================
add_executable(ClaymoreEditor.exe src/main.cpp)
if(WIN32)
    target_sources(ClaymoreEditor.exe PRIVATE "${CLAYMORE_WINDOWS_ICON_RC}")
endif()
target_compile_definitions(ClaymoreEditor.exe PRIVATE CLAYMORE_EDITOR=1)
target_link_libraries(ClaymoreEditor.exe PRIVATE ClaymoreEditor)
set_target_properties(ClaymoreEditor.exe PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    OUTPUT_NAME "Claymore"
    ENABLE_EXPORTS ON
)
if(MSVC)
    set_target_properties(ClaymoreEditor.exe PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE /ENTRY:mainCRTStartup"
    )
endif()

# ============================
# CLAYMORE RUNTIME CORE (Stripped-down Core for Exported Games)
# ============================
# Same source files as ClaymoreCore but compiled with CLAYMORE_RUNTIME
# to exclude all editor-only code paths
# Exclude editor-heavy files that are not needed in runtime
# (runtime uses binary scene loading, not JSON serialization)
set(RUNTIME_CORE_SOURCES ${CORE_SOURCES})
# Only exclude the JSON serializer from serialization folder, not AnimationSerializer etc.
list(FILTER RUNTIME_CORE_SOURCES EXCLUDE REGEX ".*/serialization/Serializer\\.cpp$")
list(FILTER RUNTIME_CORE_SOURCES EXCLUDE REGEX ".*PrefabAPI\\.cpp$")

# Add essential runtime-only files and interop files needed for runtime script execution
# These provide the function pointers that managed code needs (entity manipulation, input, etc.)
set(RUNTIME_INTEROP_SOURCES
    # Runtime-only core files (excluded from CORE_SOURCES, needed here)
    src/core/managed/RuntimeHost.cpp
    src/core/managed/RuntimeInterop.cpp
    src/core/RuntimeApplication.cpp
    src/core/RuntimeStubs.cpp
    src/runtime/RuntimeProject.cpp
    src/runtime/RuntimeDotNetBridge.cpp
    src/runtime/RuntimeRendererDebug.cpp
    # Ensure MeshBinaryLoader is included (glob may miss it if added after initial config)
    src/core/serialization/MeshBinaryLoader.cpp
    # Runtime uses the same offscreen renderer helpers as the editor for UI scene captures
    src/editor/rendering/RendererOffscreen.cpp
    # Interop files for managed code
    src/managed/interop/EntityInterop.cpp
    src/managed/interop/InputInterop.cpp
    src/managed/interop/ComponentInterop.cpp
    src/managed/interop/AudioInterop.cpp
    src/managed/interop/TweenInterop.cpp
    src/managed/interop/PhysicsInterop.cpp
    src/managed/interop/ManagedInterop.cpp
    src/managed/interop/ModuleComponentInterop.cpp
    src/managed/interop/AnimationEventInterop.cpp
    src/managed/interop/DialogueInterop.cpp
    src/managed/interop/QuestInterop.cpp
    src/managed/interop/ResourceInterop.cpp
    # Asset interop files - these have #ifdef CLAYMORE_RUNTIME blocks with real implementations
    src/managed/interop/MeshInterop.cpp
    src/managed/interop/PrefabInterop.cpp
    src/managed/interop/ScriptableInterop.cpp
)
list(APPEND RUNTIME_CORE_SOURCES ${RUNTIME_INTEROP_SOURCES})

add_library(ClaymoreRuntimeCore STATIC ${RUNTIME_CORE_SOURCES})
target_compile_definitions(ClaymoreRuntimeCore PUBLIC CLAYMORE_CORE=1 CLAYMORE_RUNTIME=1)
target_include_directories(ClaymoreRuntimeCore PUBLIC
    src
    src/core
    external/bgfx/include
    external/bx/include
    external/bimg/include
    external/glm
    external/json/include
    external/JoltPhysics
    external/stb
    external/imgui
    external/imgui/backends
    external/miniaudio
    external/dotnet_hosting
)
target_link_libraries(ClaymoreRuntimeCore PUBLIC
    JoltPhysics
    RecastDetour
    OpenSSL::SSL
    OpenSSL::Crypto
    imgui
)
if(WIN32)
    target_link_libraries(ClaymoreRuntimeCore PUBLIC
        ${CMAKE_SOURCE_DIR}/external/bgfx/.build/win64_vs2022/bin/bgfx$<IF:$<CONFIG:Debug>,Debug,Release>.lib
        ${CMAKE_SOURCE_DIR}/external/bgfx/.build/win64_vs2022/bin/bimg$<IF:$<CONFIG:Debug>,Debug,Release>.lib
        ${CMAKE_SOURCE_DIR}/external/bgfx/.build/win64_vs2022/bin/bx$<IF:$<CONFIG:Debug>,Debug,Release>.lib
        user32 gdi32 imm32 shell32
    )
elseif(UNIX AND NOT APPLE)
    target_link_libraries(ClaymoreRuntimeCore PUBLIC
        ${CMAKE_SOURCE_DIR}/external/bgfx/.build/linux64_gcc/bin/libbgfx$<IF:$<CONFIG:Debug>,Debug,Release>.a
        ${CMAKE_SOURCE_DIR}/external/bgfx/.build/linux64_gcc/bin/libbimg$<IF:$<CONFIG:Debug>,Debug,Release>.a
        ${CMAKE_SOURCE_DIR}/external/bgfx/.build/linux64_gcc/bin/libbx$<IF:$<CONFIG:Debug>,Debug,Release>.a
        Threads::Threads ${CMAKE_DL_LIBS}
        X11 Xcursor Xinerama Xi Xrandr GL
    )
endif()
set_target_properties(ClaymoreRuntimeCore PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ============================
# CLAYMORE RUNTIME EXECUTABLE (Game Template)
# ============================
# This is a standalone game executable that uses ClaymoreRuntimeCore
# No editor dependencies, no ImGui, no Assimp - pure runtime
add_executable(ClaymoreRuntime src/runtime_main.cpp)
if(WIN32)
    target_sources(ClaymoreRuntime PRIVATE "${CLAYMORE_WINDOWS_ICON_RC}")
endif()
target_compile_definitions(ClaymoreRuntime PRIVATE CLAYMORE_RUNTIME=1)
target_include_directories(ClaymoreRuntime PRIVATE
    src
    external/json/include
    external/glm
    external/dotnet_hosting
)
target_link_libraries(ClaymoreRuntime PRIVATE
    ClaymoreRuntimeCore
)
set_target_properties(ClaymoreRuntime PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    OUTPUT_NAME "GameRuntime"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime"
)
if(MSVC)
    set_target_properties(ClaymoreRuntime PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
    )
endif()

# Copy runtime dependencies
add_custom_command(TARGET ClaymoreRuntime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/external/dotnet_hosting/nethost.dll $<TARGET_FILE_DIR:ClaymoreRuntime>
)

# ============================
# VS SOURCE GROUPING
# ============================
source_group(TREE ${CMAKE_SOURCE_DIR}/src/core PREFIX "Core" FILES ${CORE_SOURCES})
source_group(TREE ${CMAKE_SOURCE_DIR}/src/editor PREFIX "Editor" FILES ${EDITOR_SOURCES})
source_group(TREE ${CMAKE_SOURCE_DIR}/src/managed PREFIX "Managed" FILES ${MANAGED_SOURCES})
source_group(TREE ${CMAKE_SOURCE_DIR}/src/core PREFIX "Core" FILES ${MANAGED_EXTRA_SOURCES})
source_group("External\\ImGuizmo" FILES ${EDITOR_EXTERNAL_SOURCES})

# ============================
# COMPILE DEFINITIONS
# ============================
foreach(target ClaymoreCore ClaymoreRuntimeCore ClaymoreEditor ClaymoreManaged imgui imgui_bgfx ClaymoreEditor.exe)
    target_compile_definitions(${target} PRIVATE
        $<$<CONFIG:Debug>:BX_CONFIG_DEBUG=1>
        $<$<NOT:$<CONFIG:Debug>>:BX_CONFIG_DEBUG=0>
    )
endforeach()

# ============================
# SHADER COMPILATION
# ============================
set(IMGUI_SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders/imgui)
set(IMGUI_SHADER_OUT ${CMAKE_BINARY_DIR}/shaders/imgui)

set(SHADERC_EXE "")
set(_shaderc_candidates
    ${CMAKE_SOURCE_DIR}/tools/shaderc.exe
    ${CMAKE_BINARY_DIR}/tools/shaderc.exe
    ${CMAKE_SOURCE_DIR}/external/bgfx/.build/win64_vs2022/bin/shadercDebug.exe
    ${CMAKE_SOURCE_DIR}/external/bgfx/.build/win64_vs2022/bin/shadercRelease.exe
    ${CMAKE_SOURCE_DIR}/tools/shaderc
    ${CMAKE_SOURCE_DIR}/external/bgfx/.build/linux64_gcc/bin/shadercDebug
    ${CMAKE_SOURCE_DIR}/external/bgfx/.build/linux64_gcc/bin/shadercRelease
)
foreach(c ${_shaderc_candidates})
    if(EXISTS ${c})
        set(SHADERC_EXE ${c})
        break()
    endif()
endforeach()

if(SHADERC_EXE)
    if(WIN32)
        add_custom_target(compile_imgui_shaders ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory ${IMGUI_SHADER_OUT}
            COMMAND ${SHADERC_EXE}
                -f ${IMGUI_SHADER_DIR}/vs_ocornut_imgui.sc
                -o ${IMGUI_SHADER_OUT}/vs_ocornut_imgui.bin
                --type vertex --platform windows --profile s_5_0
                --varyingdef ${IMGUI_SHADER_DIR}/varying.def.sc
                -i ${CMAKE_SOURCE_DIR}/shaders -i ${CMAKE_SOURCE_DIR}/external/bgfx/src
            COMMAND ${SHADERC_EXE}
                -f ${IMGUI_SHADER_DIR}/fs_ocornut_imgui.sc
                -o ${IMGUI_SHADER_OUT}/fs_ocornut_imgui.bin
                --type fragment --platform windows --profile s_5_0
                --varyingdef ${IMGUI_SHADER_DIR}/varying.def.sc
                -i ${CMAKE_SOURCE_DIR}/shaders -i ${CMAKE_SOURCE_DIR}/external/bgfx/src
            DEPENDS ${IMGUI_SHADER_DIR}/vs_ocornut_imgui.sc ${IMGUI_SHADER_DIR}/fs_ocornut_imgui.sc
        )
    elseif(UNIX AND NOT APPLE)
        add_custom_target(compile_imgui_shaders ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory ${IMGUI_SHADER_OUT}
            COMMAND ${SHADERC_EXE}
                -f ${IMGUI_SHADER_DIR}/vs_ocornut_imgui.sc
                -o ${IMGUI_SHADER_OUT}/vs_ocornut_imgui.bin
                --type vertex --platform linux --profile 120
                --varyingdef ${IMGUI_SHADER_DIR}/varying.def.sc
                -i ${CMAKE_SOURCE_DIR}/shaders -i ${CMAKE_SOURCE_DIR}/external/bgfx/src
            COMMAND ${SHADERC_EXE}
                -f ${IMGUI_SHADER_DIR}/fs_ocornut_imgui.sc
                -o ${IMGUI_SHADER_OUT}/fs_ocornut_imgui.bin
                --type fragment --platform linux --profile 120
                --varyingdef ${IMGUI_SHADER_DIR}/varying.def.sc
                -i ${CMAKE_SOURCE_DIR}/shaders -i ${CMAKE_SOURCE_DIR}/external/bgfx/src
            DEPENDS ${IMGUI_SHADER_DIR}/vs_ocornut_imgui.sc ${IMGUI_SHADER_DIR}/fs_ocornut_imgui.sc
        )
    endif()
    add_dependencies(ClaymoreEditor.exe compile_imgui_shaders)
else()
    message(WARNING "shaderc not found; skipping ImGui shader compilation.")
endif()

# ============================
# POST-BUILD STEPS
# ============================
add_custom_command(TARGET ClaymoreEditor.exe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:ClaymoreEditor.exe>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${IMGUI_SHADER_OUT} $<TARGET_FILE_DIR:ClaymoreEditor.exe>/shaders/imgui
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:ClaymoreEditor.exe>/assets
)

if(WIN32)
    add_custom_command(TARGET ClaymoreEditor.exe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/external/dotnet_hosting/nethost.dll $<TARGET_FILE_DIR:ClaymoreEditor.exe>
    )
    # Also copy nethost.dll to runtime template
    add_custom_command(TARGET ClaymoreRuntime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/external/dotnet_hosting/nethost.dll $<TARGET_FILE_DIR:ClaymoreRuntime>
    )
endif()


# ============================
# MANAGED ENGINE (.NET) BUILD
# ============================
set(MANAGED_PROJECT_DIR ${CMAKE_SOURCE_DIR}/managed/ClaymoreEngine)
set(MANAGED_CSPROJ ${MANAGED_PROJECT_DIR}/ClaymoreEngine.csproj)
set(MANAGED_RUNTIME_VERSION 10.0.5)
set(PRIVATE_DOTNET_STAGE_SCRIPT ${CMAKE_SOURCE_DIR}/scripts/stage_private_dotnet_runtime.ps1)

file(GLOB_RECURSE MANAGED_CS_SOURCES CONFIGURE_DEPENDS ${MANAGED_PROJECT_DIR}/*.cs)

set(MANAGED_DEBUG_DIR ${MANAGED_PROJECT_DIR}/bin/Debug/net10.0)
set(MANAGED_RELEASE_DIR ${MANAGED_PROJECT_DIR}/bin/Release/net10.0)
set(MANAGED_PUBLISH_DIR ${MANAGED_PROJECT_DIR}/bin/Release/net10.0/win-x64/publish)
set(MANAGED_PUBLISH_STAMP ${CMAKE_BINARY_DIR}/managed_publish.stamp)

add_custom_command(
    OUTPUT ${MANAGED_PUBLISH_STAMP}
    COMMAND ${CMAKE_COMMAND} -E echo "[CMake] Publishing ClaymoreEngine with bundled private runtime..."
    COMMAND dotnet publish ${MANAGED_CSPROJ}
        -c Release -r win-x64 --self-contained false -o ${MANAGED_PUBLISH_DIR}
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${PRIVATE_DOTNET_STAGE_SCRIPT}
        -PublishDir ${MANAGED_PUBLISH_DIR}
        -RuntimeVersion ${MANAGED_RUNTIME_VERSION}
    COMMAND ${CMAKE_COMMAND} -E touch ${MANAGED_PUBLISH_STAMP}
    DEPENDS ${MANAGED_CS_SOURCES} ${MANAGED_CSPROJ}
    WORKING_DIRECTORY ${MANAGED_PROJECT_DIR}
    COMMENT "Publishing ClaymoreEngine with bundled private .NET runtime..."
    VERBATIM
)

add_custom_target(publish_managed_selfcontained DEPENDS ${MANAGED_PUBLISH_STAMP})

set(MANAGED_BUILD_STAMP ${CMAKE_BINARY_DIR}/managed_build.stamp)

add_custom_command(
    OUTPUT ${MANAGED_BUILD_STAMP}
    COMMAND ${CMAKE_COMMAND} -E echo "[CMake] Building ClaymoreEngine (Debug)..."
    COMMAND dotnet build ${MANAGED_CSPROJ} -c Debug
    COMMAND ${CMAKE_COMMAND} -E touch ${MANAGED_BUILD_STAMP}
    DEPENDS ${MANAGED_CS_SOURCES} ${MANAGED_CSPROJ}
    WORKING_DIRECTORY ${MANAGED_PROJECT_DIR}
    COMMENT "Building ClaymoreEngine (Debug)..."
    VERBATIM
)

add_custom_target(build_managed DEPENDS ${MANAGED_BUILD_STAMP})

add_dependencies(ClaymoreEditor.exe build_managed)

add_custom_command(TARGET ClaymoreEditor.exe POST_BUILD
    COMMAND ${CMAKE_COMMAND} 
        -DSRC_DEBUG="${MANAGED_DEBUG_DIR}"
        -DSRC_PUBLISH="${MANAGED_PUBLISH_DIR}"
        -DDST_DIR="$<TARGET_FILE_DIR:ClaymoreEditor.exe>"
        -P "${CMAKE_SOURCE_DIR}/cmake/CopyManagedRuntime.cmake"
    COMMENT "Copying managed engine DLLs..."
)

# ============================
# SCRIPT COMPILER (Roslyn-based C# compiler)
# ============================
set(SCRIPT_COMPILER_DIR ${CMAKE_SOURCE_DIR}/managed/ScriptCompiler)
set(SCRIPT_COMPILER_CSPROJ ${SCRIPT_COMPILER_DIR}/ScriptCompiler.csproj)
set(SCRIPT_COMPILER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/tools)

# Build and publish ScriptCompiler as self-contained to include Roslyn DLLs
set(SCRIPT_COMPILER_BUILD_STAMP ${CMAKE_BINARY_DIR}/scriptcompiler_build.stamp)

add_custom_command(
    OUTPUT ${SCRIPT_COMPILER_BUILD_STAMP}
    COMMAND ${CMAKE_COMMAND} -E echo "[CMake] Publishing ScriptCompiler (self-contained)..."
    COMMAND dotnet publish ${SCRIPT_COMPILER_CSPROJ} -c Release -r win-x64 --self-contained true -o ${SCRIPT_COMPILER_OUTPUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E touch ${SCRIPT_COMPILER_BUILD_STAMP}
    DEPENDS ${SCRIPT_COMPILER_CSPROJ} ${SCRIPT_COMPILER_DIR}/Program.cs
    WORKING_DIRECTORY ${SCRIPT_COMPILER_DIR}
    COMMENT "Publishing ScriptCompiler with Roslyn dependencies..."
    VERBATIM
)

add_custom_target(build_scriptcompiler DEPENDS ${SCRIPT_COMPILER_BUILD_STAMP})
add_dependencies(ClaymoreEditor.exe build_scriptcompiler)

# Copy shaderc (non-managed tool)
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/shaderc.exe)
    configure_file(${CMAKE_SOURCE_DIR}/tools/shaderc.exe ${CMAKE_BINARY_DIR}/tools/shaderc.exe COPYONLY)
endif()


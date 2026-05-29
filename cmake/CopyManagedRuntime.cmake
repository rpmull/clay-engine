# CopyManagedRuntime.cmake
# Stages ClaymoreEngine for native hosting as:
#   - root: nethost + ClaymoreEngine component files
#   - optional dotnet/: private bundled .NET runtime root
#
# Variables expected:
#   SRC_DEBUG   - Path to debug build output
#   SRC_PUBLISH - Path to publish output
#   DST_DIR     - Destination directory

set(USE_PUBLISH FALSE)
set(USE_BUNDLED_RUNTIME FALSE)
set(PUBLISH_HAS_BUNDLED_RUNTIME FALSE)
set(PUBLISH_IS_LEGACY_SELFCONTAINED FALSE)
set(PUBLISH_IS_USABLE FALSE)
set(DEBUG_IS_USABLE FALSE)

set(DEBUG_DLL "${SRC_DEBUG}/ClaymoreEngine.dll")
set(PUBLISH_DLL "${SRC_PUBLISH}/ClaymoreEngine.dll")

if(EXISTS "${SRC_PUBLISH}/dotnet/host/fxr" AND EXISTS "${SRC_PUBLISH}/dotnet/shared/Microsoft.NETCore.App")
    set(PUBLISH_HAS_BUNDLED_RUNTIME TRUE)
endif()

if(EXISTS "${SRC_PUBLISH}/hostfxr.dll" OR EXISTS "${SRC_PUBLISH}/coreclr.dll" OR EXISTS "${SRC_PUBLISH}/hostpolicy.dll")
    set(PUBLISH_IS_LEGACY_SELFCONTAINED TRUE)
endif()

if(EXISTS "${PUBLISH_DLL}")
    if(PUBLISH_HAS_BUNDLED_RUNTIME)
        set(PUBLISH_IS_USABLE TRUE)
    elseif(PUBLISH_IS_LEGACY_SELFCONTAINED)
        message(WARNING "[CopyManagedRuntime] Ignoring legacy self-contained publish layout at: ${SRC_PUBLISH}")
        message(WARNING "[CopyManagedRuntime] Component hosting requires a framework-dependent runtimeconfig plus optional dotnet/ runtime root.")
    else()
        set(PUBLISH_IS_USABLE TRUE)
    endif()
endif()

if(EXISTS "${DEBUG_DLL}")
    set(DEBUG_IS_USABLE TRUE)
endif()

if(PUBLISH_IS_USABLE AND DEBUG_IS_USABLE)
    file(TIMESTAMP "${PUBLISH_DLL}" PUBLISH_TIMESTAMP UTC)
    file(TIMESTAMP "${DEBUG_DLL}" DEBUG_TIMESTAMP UTC)

    if(DEBUG_TIMESTAMP STRGREATER PUBLISH_TIMESTAMP)
        set(SRC_DIR "${SRC_DEBUG}")
        message(STATUS "[CopyManagedRuntime] Using newer debug build from: ${SRC_DEBUG}")
    else()
        set(USE_PUBLISH TRUE)
        set(SRC_DIR "${SRC_PUBLISH}")
        if(PUBLISH_HAS_BUNDLED_RUNTIME)
            set(USE_BUNDLED_RUNTIME TRUE)
            message(STATUS "[CopyManagedRuntime] Using newer bundled private runtime publish from: ${SRC_PUBLISH}")
        else()
            message(STATUS "[CopyManagedRuntime] Using newer framework-dependent publish from: ${SRC_PUBLISH}")
        endif()
    endif()
elseif(PUBLISH_IS_USABLE)
    set(USE_PUBLISH TRUE)
    set(SRC_DIR "${SRC_PUBLISH}")
    if(PUBLISH_HAS_BUNDLED_RUNTIME)
        set(USE_BUNDLED_RUNTIME TRUE)
        message(STATUS "[CopyManagedRuntime] Using bundled private runtime publish from: ${SRC_PUBLISH}")
    else()
        message(STATUS "[CopyManagedRuntime] Using framework-dependent publish from: ${SRC_PUBLISH}")
    endif()
elseif(DEBUG_IS_USABLE)
    set(SRC_DIR "${SRC_DEBUG}")
    message(STATUS "[CopyManagedRuntime] Using debug build from: ${SRC_DEBUG}")
else()
    set(SRC_DIR "${SRC_DEBUG}")
    message(STATUS "[CopyManagedRuntime] Falling back to debug path: ${SRC_DEBUG}")
endif()

if(NOT EXISTS "${SRC_DIR}")
    message(WARNING "[CopyManagedRuntime] Source directory does not exist: ${SRC_DIR}")
    message(WARNING "[CopyManagedRuntime] Run 'dotnet build' or 'dotnet publish' first.")
    return()
endif()

file(MAKE_DIRECTORY "${DST_DIR}")

# Remove legacy app-local runtime files that break component hosting.
file(REMOVE
    "${DST_DIR}/hostfxr.dll"
    "${DST_DIR}/hostpolicy.dll"
    "${DST_DIR}/coreclr.dll"
    "${DST_DIR}/clrjit.dll"
    "${DST_DIR}/clrgc.dll"
    "${DST_DIR}/clretwrc.dll"
    "${DST_DIR}/mscordbi.dll"
    "${DST_DIR}/createdump.exe"
)
file(GLOB LEGACY_MSCORDACCORE_FILES "${DST_DIR}/mscordaccore*.dll")
if(LEGACY_MSCORDACCORE_FILES)
    file(REMOVE ${LEGACY_MSCORDACCORE_FILES})
endif()
file(REMOVE_RECURSE "${DST_DIR}/dotnet")

file(GLOB MANAGED_FILES "${SRC_DIR}/*")

foreach(FILE ${MANAGED_FILES})
    if(IS_DIRECTORY "${FILE}")
        continue()
    endif()

    get_filename_component(FILENAME "${FILE}" NAME)
    get_filename_component(EXT "${FILE}" EXT)

    if("${EXT}" STREQUAL ".pdb")
        continue()
    endif()

    if(FILENAME MATCHES "^assimp")
        continue()
    endif()

    if(FILENAME STREQUAL "hostfxr.dll" OR
       FILENAME STREQUAL "hostpolicy.dll" OR
       FILENAME STREQUAL "coreclr.dll" OR
       FILENAME STREQUAL "clrjit.dll" OR
       FILENAME STREQUAL "clrgc.dll" OR
       FILENAME STREQUAL "clretwrc.dll" OR
       FILENAME STREQUAL "mscordbi.dll" OR
       FILENAME STREQUAL "createdump.exe" OR
       FILENAME MATCHES "^mscordaccore")
        continue()
    endif()

    file(COPY "${FILE}" DESTINATION "${DST_DIR}")
endforeach()

if(USE_BUNDLED_RUNTIME)
    file(COPY "${SRC_DIR}/dotnet" DESTINATION "${DST_DIR}")
    message(STATUS "[CopyManagedRuntime] Copied framework-dependent managed files with bundled dotnet runtime")
else()
    message(STATUS "[CopyManagedRuntime] Copied framework-dependent managed files (users need .NET 10)")
endif()

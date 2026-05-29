#pragma once

// Compile-time logging macros for prefab and mesh loading
// In release builds (NDEBUG defined), all logging is completely compiled out
// This eliminates 8-80ms+ overhead per prefab instantiation from synchronous I/O

#include <iostream>

#ifdef NDEBUG
    // Release build - all logging compiled out
    #define PREFAB_LOG(msg) ((void)0)
    #define PREFAB_LOG_ERROR(msg) ((void)0)
    #define PREFAB_LOG_WARN(msg) ((void)0)
    #define MESH_LOG(msg) ((void)0)
    #define MESH_LOG_ERROR(msg) ((void)0)
    #define MESH_LOG_WARN(msg) ((void)0)
    #define MATERIAL_LOG(msg) ((void)0)
    #define MATERIAL_LOG_ERROR(msg) ((void)0)
#else
    // Debug build - high-volume asset logs stay opt-in to avoid editor stalls
    // during prefab/NPC spawning.
    #ifdef CLAYMORE_VERBOSE_ASSET_LOGS
    #define PREFAB_LOG(msg) std::cout << "[Prefab] " << msg << std::endl
    #define MESH_LOG(msg) std::cout << "[MeshLoader] " << msg << std::endl
    #define MATERIAL_LOG(msg) std::cout << "[Material] " << msg << std::endl
    #else
    #define PREFAB_LOG(msg) ((void)0)
    #define MESH_LOG(msg) ((void)0)
    #define MATERIAL_LOG(msg) ((void)0)
    #endif
    #define PREFAB_LOG_ERROR(msg) std::cerr << "[Prefab] ERROR: " << msg << std::endl
    #define PREFAB_LOG_WARN(msg) std::cerr << "[Prefab] WARN: " << msg << std::endl
    #define MESH_LOG_ERROR(msg) std::cerr << "[MeshLoader] ERROR: " << msg << std::endl
    #define MESH_LOG_WARN(msg) std::cerr << "[MeshLoader] WARN: " << msg << std::endl
    #define MATERIAL_LOG_ERROR(msg) std::cerr << "[Material] ERROR: " << msg << std::endl
#endif

#include "TextureLoader.h"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include "core/vfs/FileSystem.h"
#include "core/vfs/VirtualFS.h"
#include <stdexcept>
#include <algorithm>
#include <string>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <mutex>

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"

#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
#include <iostream>
#ifndef CLAYMORE_RUNTIME
#include "editor/Project.h"
#endif


// Helper to convert color space enum to bgfx flags
static uint64_t ColorSpaceToFlags(TextureColorSpace colorSpace) {
    return (colorSpace == TextureColorSpace::sRGB) ? BGFX_TEXTURE_SRGB : BGFX_TEXTURE_NONE;
}

static std::vector<uint8_t> GenerateNextMipRGBA8(const std::vector<uint8_t>& src, int srcW, int srcH, int dstW, int dstH)
{
    std::vector<uint8_t> dst(static_cast<size_t>(dstW) * static_cast<size_t>(dstH) * 4u, 0u);
    for (int y = 0; y < dstH; ++y) {
        for (int x = 0; x < dstW; ++x) {
            const int sx0 = std::min(srcW - 1, x * 2);
            const int sx1 = std::min(srcW - 1, sx0 + 1);
            const int sy0 = std::min(srcH - 1, y * 2);
            const int sy1 = std::min(srcH - 1, sy0 + 1);

            const size_t i00 = (static_cast<size_t>(sy0) * static_cast<size_t>(srcW) + static_cast<size_t>(sx0)) * 4u;
            const size_t i10 = (static_cast<size_t>(sy0) * static_cast<size_t>(srcW) + static_cast<size_t>(sx1)) * 4u;
            const size_t i01 = (static_cast<size_t>(sy1) * static_cast<size_t>(srcW) + static_cast<size_t>(sx0)) * 4u;
            const size_t i11 = (static_cast<size_t>(sy1) * static_cast<size_t>(srcW) + static_cast<size_t>(sx1)) * 4u;
            const size_t o = (static_cast<size_t>(y) * static_cast<size_t>(dstW) + static_cast<size_t>(x)) * 4u;

            for (int c = 0; c < 4; ++c) {
                const uint32_t sum = static_cast<uint32_t>(src[i00 + c]) +
                                     static_cast<uint32_t>(src[i10 + c]) +
                                     static_cast<uint32_t>(src[i01 + c]) +
                                     static_cast<uint32_t>(src[i11 + c]);
                dst[o + c] = static_cast<uint8_t>((sum + 2u) / 4u);
            }
        }
    }
    return dst;
}

static void UploadRGBA8TextureLevels(bgfx::TextureHandle handle, int width, int height, const std::vector<uint8_t>& basePixels, bool generateMips)
{
    if (!bgfx::isValid(handle) || width <= 0 || height <= 0 || basePixels.empty()) {
        return;
    }

    if (!generateMips) {
        const bgfx::Memory* mem = bgfx::copy(basePixels.data(), static_cast<uint32_t>(basePixels.size()));
        bgfx::updateTexture2D(
            handle,
            0,                /* layer */
            0,                /* mip  */
            0, 0,             /* x, y */
            static_cast<uint16_t>(width),
            static_cast<uint16_t>(height),
            mem,
            static_cast<uint16_t>(width * 4)
        );
        return;
    }

    std::vector<uint8_t> current = basePixels;
    int curW = width;
    int curH = height;
    uint8_t mipLevel = 0;
    while (true) {
        const bgfx::Memory* mem = bgfx::copy(current.data(), static_cast<uint32_t>(current.size()));
        bgfx::updateTexture2D(
            handle,
            0,                /* layer */
            mipLevel,         /* mip  */
            0, 0,             /* x, y */
            static_cast<uint16_t>(curW),
            static_cast<uint16_t>(curH),
            mem,
            static_cast<uint16_t>(curW * 4)
        );

        if (curW == 1 && curH == 1) {
            break;
        }

        const int nextW = std::max(1, curW >> 1);
        const int nextH = std::max(1, curH >> 1);
        current = GenerateNextMipRGBA8(current, curW, curH, nextW, nextH);
        curW = nextW;
        curH = nextH;
        ++mipLevel;
    }
}

namespace {
struct FilenameFallbackIndexEntry {
    std::vector<std::string> preferred;
    std::vector<std::string> any;
};

static std::mutex s_filenameIndexMutex;
static std::unordered_map<std::string, FilenameFallbackIndexEntry> s_filenameIndex;
static bool s_filenameIndexBuilt = false;
static std::mutex s_pathAliasMutex;
static std::unordered_map<std::string, std::string> s_pathAliasCache;

static std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), ::tolower);
    return value;
}

static std::string NormalizePathKey(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

#ifdef CLAYMORE_RUNTIME
static void BuildFilenameIndexRuntime() {
    if (s_filenameIndexBuilt || !VFS::Get()) return;
    
    auto allFiles = VFS::Get()->ListAllFiles();
    for (const auto& f : allFiles) {
        std::filesystem::path pakPath(f);
        std::string filename = pakPath.filename().string();
        if (filename.empty()) continue;
        
        std::string filenameLower = ToLowerCopy(filename);
        auto& entry = s_filenameIndex[filenameLower];
        if (f.find("assets/textures/") == 0 || f.find("assets/") == 0) {
            entry.preferred.push_back(f);
        } else {
            entry.any.push_back(f);
        }
    }
    
    s_filenameIndexBuilt = true;
}
#else
static void BuildFilenameIndexEditor(const std::vector<std::filesystem::path>& preferredRoots,
                                     const std::vector<std::filesystem::path>& anyRoots) {
    if (s_filenameIndexBuilt) return;
    
    auto indexRoots = [&](const std::vector<std::filesystem::path>& roots, bool preferred) {
        std::error_code ec;
        for (const auto& root : roots) {
            for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                std::string filename = it->path().filename().string();
                if (filename.empty()) continue;
                
                std::string filenameLower = ToLowerCopy(filename);
                auto& entry = s_filenameIndex[filenameLower];
                if (preferred) {
                    entry.preferred.push_back(it->path().string());
                } else {
                    entry.any.push_back(it->path().string());
                }
            }
        }
    };
    
    indexRoots(preferredRoots, true);
    indexRoots(anyRoots, false);
    s_filenameIndexBuilt = true;
}
#endif
} // namespace

std::string TextureLoader::ResolveTexturePath(const std::string& path) {
    if (path.empty()) return path;
    
    std::string key = NormalizePathKey(path);
    {
        std::lock_guard<std::mutex> lock(s_pathAliasMutex);
        auto it = s_pathAliasCache.find(key);
        if (it != s_pathAliasCache.end()) {
            return it->second;
        }
    }

    std::filesystem::path candidate(key);
    std::error_code ec;
    if (candidate.is_absolute() && std::filesystem::exists(candidate, ec)) {
        return key;
    }

    std::string stripped = VFS::StripToKnownPrefix(key);
    if (!stripped.empty()) {
        std::filesystem::path projectRoot = FileSystem::Instance().GetProjectRoot();
        if (!projectRoot.empty()) {
            std::filesystem::path projectCandidate = projectRoot / stripped;
            if (std::filesystem::exists(projectCandidate, ec)) {
                return stripped;
            }
        }
        return stripped;
    }

    std::filesystem::path projectRoot = FileSystem::Instance().GetProjectRoot();
    if (!projectRoot.empty()) {
        std::filesystem::path projectCandidate = projectRoot / key;
        if (std::filesystem::exists(projectCandidate, ec)) {
            return key;
        }
    }

    return key;
}

void TextureLoader::ResetPathCaches() {
    {
        std::lock_guard<std::mutex> lock(s_filenameIndexMutex);
        s_filenameIndex.clear();
        s_filenameIndexBuilt = false;
    }
    {
        std::lock_guard<std::mutex> lock(s_pathAliasMutex);
        s_pathAliasCache.clear();
    }
}

bgfx::TextureHandle TextureLoader::Load2DFromEncodedMemory(const void* data, int sizeBytes, bool generateMips, TextureColorSpace colorSpace)
{
    if (!data || sizeBytes <= 0) return BGFX_INVALID_HANDLE;
    
    // Ensure consistent texture orientation - stbi's flip flag is global
    stbi_set_flip_vertically_on_load(false);
    
    int width=0, height=0, channels=0;
    stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(data), sizeBytes, &width, &height, &channels, 4);
    if (!pixels) return BGFX_INVALID_HANDLE;

    uint64_t flags = ColorSpaceToFlags(colorSpace);
    
    const bgfx::TextureHandle handle = bgfx::createTexture2D(
        static_cast<uint16_t>(width),
        static_cast<uint16_t>(height),
        generateMips,
        1,
        bgfx::TextureFormat::RGBA8,
        flags,
        nullptr);

    std::vector<uint8_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    std::memcpy(rgba.data(), pixels, rgba.size());
    UploadRGBA8TextureLevels(handle, width, height, rgba, generateMips);
    stbi_image_free(pixels);
    return handle;
}

bgfx::TextureHandle TextureLoader::Load2DFromRGBA(const void* rgbaPixels, int width, int height, bool generateMips, uint32_t rowPitchBytes, TextureColorSpace colorSpace)
{
    if (!rgbaPixels || width <= 0 || height <= 0) return BGFX_INVALID_HANDLE;
    if (rowPitchBytes == 0) rowPitchBytes = static_cast<uint32_t>(width) * 4;
    
    uint64_t flags = ColorSpaceToFlags(colorSpace);
    
    const bgfx::TextureHandle handle = bgfx::createTexture2D(
        static_cast<uint16_t>(width),
        static_cast<uint16_t>(height),
        generateMips,
        1,
        bgfx::TextureFormat::RGBA8,
        flags,
        nullptr);
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    const uint8_t* srcBytes = reinterpret_cast<const uint8_t*>(rgbaPixels);
    const uint32_t tightPitch = static_cast<uint32_t>(width) * 4u;
    if (rowPitchBytes == tightPitch) {
        std::memcpy(rgba.data(), srcBytes, rgba.size());
    } else {
        for (int y = 0; y < height; ++y) {
            const uint8_t* srcRow = srcBytes + static_cast<size_t>(y) * rowPitchBytes;
            uint8_t* dstRow = rgba.data() + static_cast<size_t>(y) * tightPitch;
            std::memcpy(dstRow, srcRow, tightPitch);
        }
    }
    UploadRGBA8TextureLevels(handle, width, height, rgba, generateMips);
    return handle;
}

// Convenience functions for explicit color space
bgfx::TextureHandle TextureLoader::Load2DsRGB(const std::string& path, bool generateMips) {
    return Load2D(path, generateMips, TextureColorSpace::sRGB);
}

bgfx::TextureHandle TextureLoader::Load2DLinear(const std::string& path, bool generateMips) {
    return Load2D(path, generateMips, TextureColorSpace::Linear);
}

bgfx::TextureHandle TextureLoader::Load2D(const std::string& path, bool generateMips, TextureColorSpace colorSpace)
{
    // Ensure consistent texture orientation - stbi's flip flag is global and other systems
    // (e.g. particle sprite loader) may have changed it. Explicitly set to no flip.
    stbi_set_flip_vertically_on_load(false);
    
    int width, height, channels;
    std::vector<uint8_t> fileData;
    stbi_uc* data = nullptr;
    std::string resolvedPath = ResolveTexturePath(path);
    
    // Try VFS first (for PAK file access at runtime)
    if (VFS::Get() && VFS::Get()->ReadFile(resolvedPath, fileData)) {
        data = stbi_load_from_memory(fileData.data(), static_cast<int>(fileData.size()), &width, &height, &channels, 4);
        if (data) {
            std::cout << "[TextureLoader] Loaded from VFS: " << resolvedPath << std::endl;
        }
    }
    
    // Fallback to FileSystem (local files)
    if (!data && FileSystem::Instance().ReadFile(resolvedPath, fileData)) {
        data = stbi_load_from_memory(fileData.data(), static_cast<int>(fileData.size()), &width, &height, &channels, 4);
    }
    
    // Final fallback to direct file path (editor only - runtime should use VFS/PAK exclusively)
#ifndef CLAYMORE_RUNTIME
    if (!data && FileSystem::Instance().IsDiskFallbackAllowed()) {
        data = stbi_load(resolvedPath.c_str(), &width, &height, &channels, 4);
    }
#endif
    
#ifdef CLAYMORE_RUNTIME
    // Runtime fallback: search for texture by filename across all PAK files
    // This is a safeguard for incorrect paths in .meta files - will find the asset if it exists
    if (!data && VFS::Get()) {
        std::filesystem::path p(resolvedPath);
        std::string filename = p.filename().string();
        if (!filename.empty()) {
            std::string filenameLower = ToLowerCopy(filename);
            std::vector<std::string> searchOrder;
            size_t matchCount = 0;
            
            {
                std::lock_guard<std::mutex> lock(s_filenameIndexMutex);
                if (!s_filenameIndexBuilt) {
                    BuildFilenameIndexRuntime();
                }
                auto it = s_filenameIndex.find(filenameLower);
                if (it != s_filenameIndex.end()) {
                    const auto& preferred = it->second.preferred;
                    const auto& any = it->second.any;
                    matchCount = preferred.size() + any.size();
                    searchOrder.insert(searchOrder.end(), preferred.begin(), preferred.end());
                    searchOrder.insert(searchOrder.end(), any.begin(), any.end());
                }
            }
            
            for (const auto& candidate : searchOrder) {
                std::vector<uint8_t> altData;
                if (VFS::Get()->ReadFile(candidate, altData)) {
                    data = stbi_load_from_memory(altData.data(), static_cast<int>(altData.size()), &width, &height, &channels, 4);
                    if (data) {
                        std::cerr << "[TextureLoader] WARNING: Filename fallback used (slow path)!" << std::endl;
                        std::cerr << "[TextureLoader]   Requested: '" << path << "'" << std::endl;
                        std::cerr << "[TextureLoader]   Found at:  '" << candidate << "'" << std::endl;
                        std::cerr << "[TextureLoader]   Fix the source .meta file to use correct path." << std::endl;
                        {
                            std::lock_guard<std::mutex> lock(s_pathAliasMutex);
                            s_pathAliasCache[NormalizePathKey(path)] = candidate;
                        }
                        break;
                    }
                }
            }
            
            if (!data && matchCount > 0) {
                std::cerr << "[TextureLoader] WARNING: Found " << matchCount 
                          << " filename matches for '" << filename << "' but none could be loaded" << std::endl;
            }
        }
    }
#endif

    if (!data)
    {
#ifndef CLAYMORE_RUNTIME
        if (!FileSystem::Instance().IsDiskFallbackAllowed()) {
            return bgfx::TextureHandle{ bgfx::kInvalidHandle };
        }
        // Fallback: if original path failed, try to locate a texture with the same
        // filename inside assets/textures/** (use first match)
        // This is an editor-only fallback; runtime should have textures in PAK
        try {
            std::filesystem::path p(resolvedPath);
            const std::string fname = p.filename().string();
            if (!fname.empty()) {
                std::vector<std::filesystem::path> preferredRoots;
                std::vector<std::filesystem::path> anyRoots;
                std::error_code ec;
                // Preferred: Project asset root
                std::filesystem::path assetRoot = Project::GetAssetDirectory();
                if (!assetRoot.empty() && std::filesystem::exists(assetRoot, ec)) {
                    auto r = assetRoot / "textures";
                    if (std::filesystem::exists(r, ec)) preferredRoots.push_back(r);
                }
                // Secondary: relative 'assets/textures' from run dir
                auto rRel = std::filesystem::path("assets") / "textures";
                if (std::filesystem::exists(rRel, ec)) anyRoots.push_back(rRel);
                
                std::string fnameLower = ToLowerCopy(fname);
                std::vector<std::string> searchOrder;
                size_t matchCount = 0;
                
                {
                    std::lock_guard<std::mutex> lock(s_filenameIndexMutex);
                    if (!s_filenameIndexBuilt) {
                        BuildFilenameIndexEditor(preferredRoots, anyRoots);
                    }
                    auto it = s_filenameIndex.find(fnameLower);
                    if (it != s_filenameIndex.end()) {
                        const auto& preferred = it->second.preferred;
                        const auto& any = it->second.any;
                        matchCount = preferred.size() + any.size();
                        searchOrder.insert(searchOrder.end(), preferred.begin(), preferred.end());
                        searchOrder.insert(searchOrder.end(), any.begin(), any.end());
                    }
                }
                
                for (const auto& candidate : searchOrder) {
                    stbi_uc* alt = nullptr;
                    std::vector<uint8_t> altData;
                    if (FileSystem::Instance().ReadFile(candidate, altData)) {
                        alt = stbi_load_from_memory(altData.data(), static_cast<int>(altData.size()), &width, &height, &channels, 4);
                    } else {
                        alt = stbi_load(candidate.c_str(), &width, &height, &channels, 4);
                    }
                    if (alt) {
                        std::cout << "[TextureLoader] Fallback resolved by filename: '" << fname << "' -> " << candidate << "\n";
                        data = alt; // use fallback image
                        {
                            std::lock_guard<std::mutex> lock(s_pathAliasMutex);
                            s_pathAliasCache[NormalizePathKey(path)] = candidate;
                        }
                        break;
                    } else {
                        std::cout << "[TextureLoader] Candidate existed but failed to load: " << candidate << "\n";
                    }
                }
                
                if (!data && matchCount > 0) {
                    std::cerr << "[TextureLoader] WARNING: Found " << matchCount
                              << " filename matches for '" << fname << "' but none could be loaded" << std::endl;
                }
            }
        } catch (...) {
            // Swallow filesystem errors, continue to procedural fallbacks
        }
#endif // CLAYMORE_RUNTIME - end editor-only fallback search

        // If still not found, consider procedural debug defaults
        // Procedural fallbacks for engine default debug textures so export isn't hard-blocked by missing files
        auto ends_with = [](const std::string& s, const std::string& suffix) {
            if (suffix.size() > s.size()) return false;
            return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
        };
        std::vector<uint8_t> generated;
        // Note: debug textures use appropriate color space based on their purpose
        TextureColorSpace generatedColorSpace = colorSpace;
        if (ends_with(path, "assets/debug/white.png")) {
            width = height = 1; channels = 4; generated = { 255, 255, 255, 255 };
            generatedColorSpace = TextureColorSpace::Linear; // Engine does not use sRGB gamma correction
        } else if (ends_with(path, "assets/debug/metallic_roughness.png")) {
            // Default: non-metallic (0), roughness ~1.0 (255) in G channel; pack into RGBA as needed
            width = height = 1; channels = 4; generated = { 0, 255, 0, 255 };
            generatedColorSpace = TextureColorSpace::Linear; // Data texture
        } else if (ends_with(path, "assets/debug/normal.png")) {
            // Default normal map value (0.5,0.5,1.0) -> (128,128,255)
            width = height = 1; channels = 4; generated = { 128, 128, 255, 255 };
            generatedColorSpace = TextureColorSpace::Linear; // Data texture
        }

        if (!generated.empty()) {
            // Create texture from generated pixels with appropriate color space
            uint64_t flags = ColorSpaceToFlags(generatedColorSpace);
            bgfx::TextureHandle handle = bgfx::createTexture2D(
                static_cast<uint16_t>(width),
                static_cast<uint16_t>(height),
                generateMips,
                1,
                bgfx::TextureFormat::RGBA8,
                flags,
                nullptr);
            UploadRGBA8TextureLevels(handle, width, height, generated, generateMips);
            return handle;
        }
        if (!data) {
            std::cerr << "[TextureLoader] ERROR: Failed to load texture: " << path << std::endl;
#ifdef CLAYMORE_RUNTIME
            std::cerr << "[TextureLoader]   Not found in PAK and no filename match found." << std::endl;
#endif
            return BGFX_INVALID_HANDLE;
        }
    }

    // Create texture with appropriate color space flag
    // sRGB textures are automatically converted to linear space by GPU when sampled
    uint64_t flags = ColorSpaceToFlags(colorSpace);

    bgfx::TextureHandle handle = bgfx::createTexture2D(
        static_cast<uint16_t>(width),
        static_cast<uint16_t>(height),
        generateMips,
        1,
        bgfx::TextureFormat::RGBA8,
        flags,
        nullptr            /* no initial data */
    );

    std::vector<uint8_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    std::memcpy(rgba.data(), data, rgba.size());
    UploadRGBA8TextureLevels(handle, width, height, rgba, generateMips);

    stbi_image_free(data);
    return handle;
}


bgfx::TextureHandle TextureLoader::LoadIconTexture(const std::string& path)
{
    // Ensure consistent texture orientation - stbi's flip flag is global
    stbi_set_flip_vertically_on_load(false);
    
    // Detect file extension
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return s;
    };
    std::string lowerPath = toLower(path);
    bool isSvg = lowerPath.size() >= 4 && lowerPath.substr(lowerPath.size() - 4) == ".svg";


    if (isSvg)
    {
        // Parse SVG
        // Use 96 DPI and scale to target icon size
        constexpr float dpi = 96.0f;
        constexpr int targetSizePx = 64; // raster size; UI can scale down as needed

        // Load SVG via FileSystem if possible
        std::string svgText;
        NSVGimage* svg = nullptr;
        if (FileSystem::Instance().ReadTextFile(path, svgText)) {
            svg = nsvgParse(const_cast<char*>(svgText.c_str()), "px", dpi);
        } else {
            svg = nsvgParseFromFile(path.c_str(), "px", dpi);
        }
        if (svg == nullptr)
        {
            throw std::runtime_error("Failed to parse SVG icon: " + path);
        }

        const float w = svg->width;
        const float h = svg->height;
        const float maxDim = std::max(w, h);
        const float scale = maxDim > 0.0f ? static_cast<float>(targetSizePx) / maxDim : 1.0f;
        const int outW = std::max(1, static_cast<int>(std::ceil(w * scale)));
        const int outH = std::max(1, static_cast<int>(std::ceil(h * scale)));

        std::vector<uint8_t> rgba(static_cast<size_t>(outW) * static_cast<size_t>(outH) * 4u, 0u);

        NSVGrasterizer* rast = nsvgCreateRasterizer();
        if (rast == nullptr)
        {
            nsvgDelete(svg);
            throw std::runtime_error("Failed to create NanoSVG rasterizer for: " + path);
        }

        nsvgRasterize(rast, svg, 0.0f, 0.0f, scale, reinterpret_cast<unsigned char*>(rgba.data()), outW, outH, outW * 4);

        nsvgDeleteRasterizer(rast);
        nsvgDelete(svg);

        // Create BGFX texture
        constexpr uint64_t kFlags = BGFX_TEXTURE_NONE | BGFX_SAMPLER_UVW_CLAMP;
        bgfx::TextureHandle handle = bgfx::createTexture2D(
            static_cast<uint16_t>(outW),
            static_cast<uint16_t>(outH),
            false,
            1,
            bgfx::TextureFormat::RGBA8,
            kFlags,
            nullptr
        );

        const bgfx::Memory* mem = bgfx::copy(rgba.data(), static_cast<uint32_t>(rgba.size()));
        bgfx::updateTexture2D(handle, 0, 0, 0, 0,
                              static_cast<uint16_t>(outW),
                              static_cast<uint16_t>(outH),
                              mem,
                              static_cast<uint16_t>(outW * 4));

        return handle;
    }

    // Fallback: load raster image via stb_image
    int width, height, channels;
    std::vector<uint8_t> fileData;
    stbi_uc* data = nullptr;
    
    // Try VFS first (for PAK file access)
    if (VFS::Get() && VFS::Get()->ReadFile(path, fileData)) {
        data = stbi_load_from_memory(fileData.data(), static_cast<int>(fileData.size()), &width, &height, &channels, 4);
    }
    // Then try FileSystem (handles VFS delegation + disk fallback)
    if (!data && FileSystem::Instance().ReadFile(path, fileData)) {
        data = stbi_load_from_memory(fileData.data(), static_cast<int>(fileData.size()), &width, &height, &channels, 4);
    }
#ifndef CLAYMORE_RUNTIME
    // Direct disk fallback for editor only
    if (!data) {
        data = stbi_load(path.c_str(), &width, &height, &channels, 4);
    }
#endif
    if (!data)
    {
        throw std::runtime_error("Failed to load icon texture: " + path);
    }

    // Icons don't need mipmaps; create empty texture with clamp sampler flags.
    constexpr uint64_t kFlags = BGFX_TEXTURE_NONE | BGFX_SAMPLER_UVW_CLAMP;

    bgfx::TextureHandle handle = bgfx::createTexture2D(
        static_cast<uint16_t>(width),
        static_cast<uint16_t>(height),
        false, // no mips
        1,
        bgfx::TextureFormat::RGBA8,
        kFlags,
        nullptr /* no initial data */
    );

    const bgfx::Memory* mem = bgfx::copy(data, width * height * 4);
    bgfx::updateTexture2D(
        handle,
        0, 0, 0, 0,
        static_cast<uint16_t>(width),
        static_cast<uint16_t>(height),
        mem,
        static_cast<uint16_t>(width * 4)
    );

    stbi_image_free(data);
    return handle;
}


// Utility to convert TextureHandle to ImGui texture ID
ImTextureID TextureLoader::ToImGuiTextureID(bgfx::TextureHandle handle)
   {
   if (!bgfx::isValid(handle)) return static_cast<ImTextureID>(0);
   return static_cast<ImTextureID>(static_cast<uint64_t>(handle.idx) + 1ull);
   }

bgfx::TextureHandle TextureLoader::FromImGuiTextureID(ImTextureID textureId)
   {
   const uint64_t encoded = static_cast<uint64_t>(textureId);
   if (encoded == 0 || encoded > static_cast<uint64_t>(bgfx::kInvalidHandle))
      return BGFX_INVALID_HANDLE;
   return bgfx::TextureHandle{ static_cast<uint16_t>(encoded - 1ull) };
   }

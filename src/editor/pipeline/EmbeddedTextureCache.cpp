#include "EmbeddedTextureCache.h"

#include "editor/pipeline/AssetLibrary.h"
#include "core/assets/AssetMetadata.h"
#include "editor/Project.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cctype>

namespace fs = std::filesystem;

namespace embedded_textures
{
namespace
{
    uint64_t HashEmbeddedPayload(const EmbeddedTextureData& emb)
    {
        const uint64_t kPrime = 1099511628211ull;
        uint64_t hash = 1469598103934665603ull;
        auto mixByte = [&](uint8_t b) {
            hash ^= b;
            hash *= kPrime;
        };
        auto mix64 = [&](uint64_t value) {
            hash ^= value;
            hash *= kPrime;
        };
        mix64(static_cast<uint64_t>(emb.IsCompressed ? 1 : 0));
        mix64((static_cast<uint64_t>(static_cast<uint32_t>(emb.Width)) << 32) |
              static_cast<uint32_t>(emb.Height));
        mix64(static_cast<uint64_t>(emb.Bytes.size()));
        for (char c : emb.FormatHint) mixByte(static_cast<uint8_t>(c));
        for (uint8_t b : emb.Bytes) mixByte(b);
        return hash;
    }

    std::string NormalizeAssetPath(const fs::path& absolute)
    {
        std::string normalized = absolute.string();
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        
        // Look for "/assets/" and extract from there
        auto pos = normalized.find("/assets/");
        if (pos != std::string::npos)
            return normalized.substr(pos + 1); // Skip the leading '/'
        
        // Also check if it starts with "assets/"
        if (normalized.find("assets/") == 0)
            return normalized;

        fs::path project = Project::GetProjectDirectory();
        if (!project.empty())
        {
            std::error_code ec;
            fs::path rel = fs::relative(absolute, project, ec);
            if (!ec)
            {
                std::string relStr = rel.string();
                std::replace(relStr.begin(), relStr.end(), '\\', '/');
                // Only use if it doesn't go up directories
                if (relStr.find("../") == std::string::npos) {
                    if (relStr.rfind("assets/", 0) != 0)
                        relStr = "assets/" + relStr;
                    return relStr;
                }
            }
        }
        
        // Fallback: just use filename with assets/ prefix
        std::string filename = absolute.filename().string();
        if (!filename.empty()) {
            return "assets/" + filename;
        }
        return normalized;
    }

    bool IsSafeExtension(const std::string& ext)
    {
        if (ext.empty()) return false;
        for (char c : ext)
        {
            if (!std::isalnum(static_cast<unsigned char>(c))) return false;
        }
        return true;
    }

    std::string DetectCompressedExtension(const EmbeddedTextureData& emb)
    {
        std::string hint = emb.FormatHint;
        std::transform(hint.begin(), hint.end(), hint.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (IsSafeExtension(hint)) return "." + hint;

        const auto& bytes = emb.Bytes;
        if (bytes.size() >= 8 && std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0) return ".png";
        if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) return ".jpg";
        if (bytes.size() >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ') return ".dds";
        return ".bin";
    }

    std::string NormalizeSlashes(std::string value)
    {
        std::replace(value.begin(), value.end(), '\\', '/');
        return value;
    }

    bool IsDigitsOnly(const std::string& value)
    {
        if (value.empty()) return false;
        for (char c : value)
        {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        return true;
    }

    bool IsSafeFilenameExtension(const std::string& extWithDot)
    {
        if (extWithDot.size() < 2 || extWithDot[0] != '.') return false;
        return IsSafeExtension(extWithDot.substr(1));
    }

    std::string ExtractPreferredEmbeddedFilename(const TextureSpecifier& tex)
    {
        if (tex.Path.empty()) return {};

        std::string normalized = NormalizeSlashes(tex.Path);
        if (normalized.rfind("embedded://", 0) == 0)
        {
            size_t slash = normalized.find_last_of('/');
            if (slash == std::string::npos || slash + 1 >= normalized.size()) {
                return {};
            }

            std::string tail = normalized.substr(slash + 1);
            if (tail.empty() || IsDigitsOnly(tail) || tail.find('.') == std::string::npos) {
                return {};
            }

            return fs::path(tail).filename().string();
        }

        return fs::path(normalized).filename().string();
    }

    bool WriteRawEmbeddedTexture(const fs::path& path, const EmbeddedTextureData& emb)
    {
        if (emb.Width <= 0 || emb.Height <= 0 ||
            emb.Bytes.size() < static_cast<size_t>(emb.Width) * static_cast<size_t>(emb.Height) * 4u)
            return false;

        std::ofstream of(path, std::ios::binary | std::ios::trunc);
        if (!of.is_open()) return false;

        uint8_t header[18]{};
        header[2] = 2;
        header[12] = static_cast<uint8_t>(emb.Width & 0xFF);
        header[13] = static_cast<uint8_t>((emb.Width >> 8) & 0xFF);
        header[14] = static_cast<uint8_t>(emb.Height & 0xFF);
        header[15] = static_cast<uint8_t>((emb.Height >> 8) & 0xFF);
        header[16] = 32;
        of.write(reinterpret_cast<char*>(header), sizeof(header));
        if (!of.good()) return false;

        for (int y = 0; y < emb.Height; ++y)
        {
            for (int x = 0; x < emb.Width; ++x)
            {
                size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(emb.Width) + static_cast<size_t>(x)) * 4u;
                uint8_t bgra[4] = {
                    emb.Bytes[idx + 2],
                    emb.Bytes[idx + 1],
                    emb.Bytes[idx + 0],
                    emb.Bytes[idx + 3]
                };
                of.write(reinterpret_cast<char*>(bgra), 4);
                if (!of.good()) return false;
            }
        }
        return true;
    }

    bool WriteCompressedEmbeddedTexture(const fs::path& path, const EmbeddedTextureData& emb)
    {
        std::ofstream of(path, std::ios::binary | std::ios::trunc);
        if (!of.is_open()) return false;
        if (!emb.Bytes.empty())
            of.write(reinterpret_cast<const char*>(emb.Bytes.data()), static_cast<std::streamsize>(emb.Bytes.size()));
        return of.good();
    }

    std::string PersistEmbeddedTexture(const TextureSpecifier& tex, const std::string& modelNameHint)
    {
        if (!tex.Embedded.HasData()) return {};

        fs::path project = Project::GetProjectDirectory();
        if (project.empty()) return {};

        fs::path texDir = project / "assets" / "textures";
        if (!modelNameHint.empty()) texDir /= modelNameHint;
        else texDir /= "_embedded";

        std::error_code ec;
        fs::create_directories(texDir, ec);
        if (ec) return {};

        uint64_t hash = HashEmbeddedPayload(tex.Embedded);
        std::ostringstream oss;
        oss << "emb_auto_" << std::hex << hash;
        std::string baseName = oss.str();
        std::string ext = tex.Embedded.IsCompressed ? DetectCompressedExtension(tex.Embedded) : ".tga";

        std::string preferredFilename = ExtractPreferredEmbeddedFilename(tex);
        fs::path outputLeaf = preferredFilename.empty() ? fs::path(baseName + ext) : fs::path(preferredFilename);
        if (outputLeaf.empty())
        {
            outputLeaf = fs::path(baseName + ext);
        }

        std::string preferredExt = outputLeaf.extension().string();
        if (!IsSafeFilenameExtension(preferredExt) || preferredExt != ext)
        {
            outputLeaf.replace_extension(ext);
        }

        fs::path outPath = texDir / outputLeaf;
        bool ok = tex.Embedded.IsCompressed
            ? WriteCompressedEmbeddedTexture(outPath, tex.Embedded)
            : WriteRawEmbeddedTexture(outPath, tex.Embedded);
        if (!ok) return {};

        std::string virtualPath = NormalizeAssetPath(outPath);
        if (virtualPath.empty()) return {};

        ClaymoreGUID existingGuid = AssetLibrary::Instance().GetGUIDForPath(virtualPath);
        if (existingGuid.high == 0 && existingGuid.low == 0)
        {
            ClaymoreGUID guid = ClaymoreGUID::Generate();
            AssetReference ref(guid, 0, static_cast<int>(AssetType::Texture));
            AssetLibrary::Instance().RegisterAsset(ref, AssetType::Texture, virtualPath, outPath.filename().string());

            AssetMetadata meta;
            meta.sourcePath = virtualPath;
            meta.processedPath = virtualPath;
            meta.type = "texture";
            meta.guid = guid;
            meta.reference = ref;
            nlohmann::json mj = meta;
            std::ofstream metaFile(outPath.string() + ".meta", std::ios::binary | std::ios::trunc);
            if (metaFile.is_open()) metaFile << mj.dump(4);
        }

        return virtualPath;
    }
}

TextureSpecifier ExternalizeTexture(const TextureSpecifier& tex, const std::string& modelNameHint)
{
    TextureSpecifier sanitized = tex;
    if (sanitized.Embedded.HasData())
    {
        std::string persisted = PersistEmbeddedTexture(sanitized, modelNameHint);
        if (!persisted.empty())
        {
            sanitized.Path = persisted;
            sanitized.Embedded = EmbeddedTextureData{};
        }
    }
    return sanitized;
}

MaterialSource ExternalizeMaterialSource(const MaterialSource& src, const std::string& modelNameHint)
{
    MaterialSource sanitized = src;
    sanitized.Albedo = ExternalizeTexture(sanitized.Albedo, modelNameHint);
    sanitized.MetallicRoughness = ExternalizeTexture(sanitized.MetallicRoughness, modelNameHint);
    sanitized.Normal = ExternalizeTexture(sanitized.Normal, modelNameHint);
    sanitized.AO = ExternalizeTexture(sanitized.AO, modelNameHint);
    sanitized.Emission = ExternalizeTexture(sanitized.Emission, modelNameHint);
    return sanitized;
}

} // namespace embedded_textures

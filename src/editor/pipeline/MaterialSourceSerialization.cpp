#include "editor/pipeline/MaterialSourceSerialization.h"

#include "editor/Project.h"
#include "core/vfs/FileSystem.h"
#include "core/utils/Base64.h"
#include <filesystem>

using json = nlohmann::json;

namespace
{
    std::string ToVirtualPath(const std::string& path)
    {
        if (path.empty()) return {};
        std::string normalized = IVirtualFS::NormalizePath(path);
        std::string vpath = VFS::StripToKnownPrefix(normalized);
        if (!vpath.empty()) {
            return vpath;
        }
        
        // Try to make relative to project, but only if result is clean
        try
        {
            std::filesystem::path p(normalized);
            if (p.is_absolute())
            {
                std::filesystem::path proj = Project::GetProjectDirectory();
                if (!proj.empty())
                {
                    std::error_code ec;
                    auto rel = std::filesystem::relative(p, proj, ec);
                    if (!ec)
                    {
                        std::string relStr = IVirtualFS::NormalizePath(rel.string());
                        // Only use if it doesn't go up directories
                        if (relStr.find("../") == std::string::npos) {
                            std::string relVpath = VFS::StripToKnownPrefix(relStr);
                            return relVpath.empty() ? relStr : relVpath;
                        }
                    }
                }
            }
        }
        catch (...) {}
        
        // Fallback: extract filename and put in assets/
        auto pos = normalized.find_last_of('/');
        if (pos != std::string::npos)
        {
            std::string tail = normalized.substr(pos + 1);
            return std::string("assets/") + tail;
        }
        return normalized;
    }

    json SerializeTextureSpecifier(const TextureSpecifier& tex)
    {
        json j;
        if (!tex.Path.empty()) j["path"] = ToVirtualPath(tex.Path);
        if (!tex.Embedded.Bytes.empty())
        {
            json emb;
            emb["compressed"] = tex.Embedded.IsCompressed;
            emb["width"] = tex.Embedded.Width;
            emb["height"] = tex.Embedded.Height;
            if (!tex.Embedded.FormatHint.empty()) emb["format"] = tex.Embedded.FormatHint;
            emb["bytes"] = cm::base64::Encode(tex.Embedded.Bytes);
            j["embedded"] = std::move(emb);
        }
        return j;
    }

    TextureSpecifier DeserializeTextureSpecifier(const json& node)
    {
        TextureSpecifier spec;
        if (node.contains("path") && node["path"].is_string())
        {
        spec.Path = IVirtualFS::NormalizePath(node["path"].get<std::string>());
        }
        if (node.contains("embedded") && node["embedded"].is_object())
        {
            const auto& emb = node["embedded"];
            EmbeddedTextureData data;
            data.IsCompressed = emb.value("compressed", false);
            data.Width = emb.value("width", 0);
            data.Height = emb.value("height", 0);
            data.FormatHint = emb.value("format", std::string());
            if (emb.contains("bytes") && emb["bytes"].is_string())
            {
                std::string bytes = emb["bytes"].get<std::string>();
                cm::base64::Decode(bytes, data.Bytes);
            }
            spec.Embedded = std::move(data);
        }
        return spec;
    }
}

namespace material_serialization
{

json ToJson(const MaterialSource& source)
{
    json j;
    j["name"] = source.Name;
    j["skinned"] = source.Skinned;
    j["alphaBlend"] = source.AlphaBlend;
    j["alphaCutout"] = source.AlphaCutout;
    j["alphaCutoutThreshold"] = source.AlphaCutoutThreshold;
    j["twoSided"] = source.TwoSided;
    j["hasTint"] = source.HasTint;
    j["tint"] = { source.ColorTint.x, source.ColorTint.y, source.ColorTint.z, source.ColorTint.w };
    j["albedo"] = SerializeTextureSpecifier(source.Albedo);
    j["metallicRoughness"] = SerializeTextureSpecifier(source.MetallicRoughness);
    j["normal"] = SerializeTextureSpecifier(source.Normal);
    j["ao"] = SerializeTextureSpecifier(source.AO);
    j["emission"] = SerializeTextureSpecifier(source.Emission);
    j["displacement"] = SerializeTextureSpecifier(source.Displacement);
    return j;
}

MaterialSource FromJson(const nlohmann::json& node)
{
    MaterialSource src;
    src.Name = node.value("name", std::string());
    src.Skinned = node.value("skinned", false);
    src.AlphaBlend = node.value("alphaBlend", false);
    src.AlphaCutout = node.value("alphaCutout", false);
    src.AlphaCutoutThreshold = node.value("alphaCutoutThreshold", 0.5f);
    src.TwoSided = node.value("twoSided", false);
    src.HasTint = node.value("hasTint", false);
    if (node.contains("tint") && node["tint"].is_array() && node["tint"].size() == 4)
    {
        src.ColorTint = glm::vec4(node["tint"][0], node["tint"][1], node["tint"][2], node["tint"][3]);
    }
    if (node.contains("albedo")) src.Albedo = DeserializeTextureSpecifier(node["albedo"]);
    if (node.contains("metallicRoughness")) src.MetallicRoughness = DeserializeTextureSpecifier(node["metallicRoughness"]);
    if (node.contains("normal")) src.Normal = DeserializeTextureSpecifier(node["normal"]);
    if (node.contains("ao")) src.AO = DeserializeTextureSpecifier(node["ao"]);
    if (node.contains("emission")) src.Emission = DeserializeTextureSpecifier(node["emission"]);
    if (node.contains("displacement")) src.Displacement = DeserializeTextureSpecifier(node["displacement"]);
    if (src.AlphaCutout) {
        src.AlphaBlend = false;
    }
    return src;
}

} // namespace material_serialization



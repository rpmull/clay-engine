#pragma once
#include "IAssetResolver.h"
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

// RuntimeAssetResolver: Minimal asset resolver for exported standalone games
// Only reads from PAK files, no disk fallback, no editor features
class RuntimeAssetResolver : public IAssetResolver {
public:
    RuntimeAssetResolver();
    ~RuntimeAssetResolver() override = default;

    // Load asset map from manifest (maps GUIDs to paths)
    bool LoadManifest(const std::string& manifestJson);
    
    // Load resource manifest (for Resources API)
    bool LoadResourceManifest(const std::string& resourceManifestJson);

    // IAssetResolver interface
    std::string ResolvePath(const std::string& virtualPath) const override;
    std::string GetBinaryPath(const std::string& sourcePath) const override;
    bool IsBinaryCurrent(const std::string& sourcePath) const override;
    ClaymoreGUID GetGUID(const std::string& path) const override;
    std::string GetPathForGUID(const ClaymoreGUID& guid) const override;
    AssetLoadMode GetLoadMode() const override { return AssetLoadMode::Runtime; }
    bool AllowSourceFallback() const override { return false; }

    // Gather assets whose virtual paths start with the given prefix (e.g., "resources/")
    void GetAssetsByPathPrefix(const std::string& prefix,
                               std::vector<std::pair<ClaymoreGUID, std::string>>& out) const;

private:
    // GUID to path mapping (from manifest)
    std::unordered_map<std::string, std::string> m_GuidToPath;
    std::unordered_map<std::string, ClaymoreGUID> m_PathToGuid;
};



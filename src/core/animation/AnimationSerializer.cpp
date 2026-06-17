#include "AnimationSerializer.h"
#include <fstream>
#include <filesystem>
#include "PropertyTrack.h"
#include "AnimationAsset.h"
#include "HumanoidAvatar.h"
#include "AvatarDefinition.h"
#include "AvatarSerializer.h"
#include "AnimationAssetCache.h"
#include "core/ecs/AnimationComponents.h"
#include "core/vfs/FileSystem.h"
#include "core/assets/IAssetResolver.h"

#ifdef CLAYMORE_EDITOR
#include "editor/Project.h"
#include "editor/pipeline/BinaryAssetCache.h"
#endif

namespace {
    // Get base directory for resolving relative paths
    // In editor mode, uses project directory; in runtime, uses current path
    std::filesystem::path GetBaseDirectory() {
#ifdef CLAYMORE_EDITOR
        return Project::GetProjectDirectory();
#else
        return std::filesystem::current_path();
#endif
    }
}
#include <iostream>
#include <cmath>
#include <cctype>
#include <cstring>
#include <unordered_set>
namespace cm {
namespace animation {

namespace {

using namespace std::string_literals;

constexpr uint32_t kAnimBinMagic = 'A' | ('N' << 8) | ('I' << 16) | ('M' << 24);
constexpr uint16_t kAnimBinVersion = 4;  // v4: added humanoid track mode (v3: root motion, v2: referenceHipsHeight)

struct BinWriter {
    std::ofstream stream;
    bool ok = true;

    explicit BinWriter(const std::filesystem::path& path) {
        std::error_code ec;
        if (!path.empty()) {
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        stream.open(path, std::ios::binary | std::ios::trunc);
        ok = stream.is_open();
    }

    template <typename T>
    void Write(const T& v) {
        if (!ok) return;
        stream.write(reinterpret_cast<const char*>(&v), sizeof(T));
        ok = stream.good();
    }

    void WriteString(const std::string& str) {
        uint32_t len = static_cast<uint32_t>(str.size());
        Write(len);
        if (!ok || len == 0) return;
        stream.write(str.data(), len);
        ok = stream.good();
    }

    void WriteVec2(const glm::vec2& v) { Write(v.x); Write(v.y); }
    void WriteVec3(const glm::vec3& v) { Write(v.x); Write(v.y); Write(v.z); }
    void WriteQuat(const glm::quat& q) { Write(q.w); Write(q.x); Write(q.y); Write(q.z); }
    void WriteVec4(const glm::vec4& v) { Write(v.x); Write(v.y); Write(v.z); Write(v.w); }
};

struct BinReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    bool ok = true;

    explicit BinReader(const std::vector<uint8_t>& buffer) 
        : data(buffer.data()), size(buffer.size()), pos(0), ok(!buffer.empty()) {}

    template <typename T>
    void Read(T& out) {
        if (!ok) return;
        if (pos + sizeof(T) > size) { ok = false; return; }
        std::memcpy(&out, data + pos, sizeof(T));
        pos += sizeof(T);
    }

    std::string ReadString() {
        uint32_t len = 0;
        Read(len);
        if (!ok || len == 0) return {};
        if (pos + len > size) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(data + pos), len);
        pos += len;
        return s;
    }
};

std::filesystem::path ResolveAbsoluteAnimationPath(const std::string& requested) {
    namespace fs = std::filesystem;
    if (requested.empty()) return {};
    std::error_code ec;
    fs::path p(requested);
    auto tryResolve = [&](const fs::path& candidate) -> fs::path {
        if (candidate.empty()) return {};
        if (!fs::exists(candidate, ec)) return {};
        fs::path canonical = fs::weakly_canonical(candidate, ec);
        if (ec) canonical = candidate;
        return canonical;
    };

    if (p.is_absolute()) {
        if (auto resolved = tryResolve(p); !resolved.empty()) return resolved;
    } else {
        if (auto resolved = tryResolve(p); !resolved.empty()) return resolved;
        fs::path base = GetBaseDirectory();
        if (!base.empty()) {
            if (auto resolved = tryResolve(base / p); !resolved.empty()) return resolved;
        }
    }
    return {};
}

std::filesystem::path DeriveCompiledAnimationPath(const std::filesystem::path& source) {
    if (source.empty()) return {};
    std::filesystem::path compiled = source;
    std::string ext = compiled.extension().string();
    if (ext.empty()) {
        compiled.replace_extension(".animc");
    } else {
        compiled.replace_extension(ext + "c");
    }
    return compiled;
}

bool IsCompiledAnimationUpToDate(const std::filesystem::path& source, const std::filesystem::path& compiled) {
    if (source.empty() || compiled.empty()) return false;
    std::error_code ecSource;
    std::error_code ecCompiled;
    if (!std::filesystem::exists(compiled, ecCompiled) || ecCompiled) return false;
    if (!std::filesystem::exists(source, ecSource) || ecSource) return false;
    auto sourceTime = std::filesystem::last_write_time(source, ecSource);
    if (ecSource) return false;
    auto compiledTime = std::filesystem::last_write_time(compiled, ecCompiled);
    if (ecCompiled) return false;
    return compiledTime >= sourceTime;
}

bool OpenAnimationJson(const std::string& requested, std::string& outText, std::filesystem::path& resolved) {
    // Try VFS first (handles pak and disk)
    if (FileSystem::Instance().ReadTextFile(requested, outText)) {
        resolved = std::filesystem::path(requested);
        return true;
    }
    
    // Try absolute path resolution
    resolved = ResolveAbsoluteAnimationPath(requested);
    if (!resolved.empty() && FileSystem::Instance().ReadTextFile(resolved.string(), outText)) {
        return true;
    }
    
    // Fallback to project-relative path
    auto base = GetBaseDirectory();
    if (!base.empty()) {
        std::filesystem::path alt = base / requested;
        if (FileSystem::Instance().ReadTextFile(alt.string(), outText)) {
            resolved = alt;
            return true;
        }
    }
    resolved.clear();
    return false;
}

bool HasAvatarTracks(const AnimationAsset& asset) {
    for (const auto& track : asset.tracks) {
        if (track && track->type == TrackType::Avatar) {
            return true;
        }
    }
    return false;
}

glm::vec3 SafeComponentDivide(const glm::vec3& value,
                              const glm::vec3& divisor,
                              const glm::vec3& fallback) {
    glm::vec3 out = fallback;
    if (std::abs(divisor.x) > 1e-6f) out.x = value.x / divisor.x;
    if (std::abs(divisor.y) > 1e-6f) out.y = value.y / divisor.y;
    if (std::abs(divisor.z) > 1e-6f) out.z = value.z / divisor.z;
    return out;
}

float AverageCurveMagnitude(const CurveVec3& curve) {
    if (curve.keys.empty()) return 0.0f;
    float sum = 0.0f;
    for (const auto& key : curve.keys) {
        sum += glm::length(key.v);
    }
    return sum / static_cast<float>(curve.keys.size());
}

float AverageCurveDistanceFrom(const CurveVec3& curve, const glm::vec3& target) {
    if (curve.keys.empty()) return 0.0f;
    float sum = 0.0f;
    for (const auto& key : curve.keys) {
        sum += glm::length(key.v - target);
    }
    return sum / static_cast<float>(curve.keys.size());
}

float MaxAbsComponentDelta(const glm::vec3& value, const glm::vec3& reference) {
    const glm::vec3 delta = glm::abs(value - reference);
    return std::max(delta.x, std::max(delta.y, delta.z));
}

bool MaybeRepairBindRelativeHumanoidTrack(AssetAvatarTrack& track,
                                          const AvatarDefinition& sourceAvatar) {
    if (track.humanBoneId < 0 || track.humanBoneId >= static_cast<int>(HumanoidBoneCount)) {
        return false;
    }
    const HumanoidBone humanBone = static_cast<HumanoidBone>(track.humanBoneId);
    if (humanBone != HumanoidBone::Hips && humanBone != HumanoidBone::Root) {
        return false;
    }

    glm::vec3 legacyBindT(0.0f), legacyBindS(1.0f);
    glm::quat legacyBindR(1.0f, 0.0f, 0.0f, 0.0f);
    if (!sourceAvatar.GetAnimationBindTRS(static_cast<HumanoidBone>(track.humanBoneId),
                                          legacyBindT,
                                          legacyBindR,
                                          legacyBindS)) {
        return false;
    }

    const bool hasScaleCompensation =
        std::abs(legacyBindS.x - 1.0f) > 0.5f ||
        std::abs(legacyBindS.y - 1.0f) > 0.5f ||
        std::abs(legacyBindS.z - 1.0f) > 0.5f;
    if (!hasScaleCompensation) {
        return false;
    }

    const glm::vec3 authoringBindT = SafeComponentDivide(legacyBindT, legacyBindS, legacyBindT);
    const glm::vec3 authoringBindS(1.0f);
    const glm::vec3 translationCorrection = legacyBindT - authoringBindT;
    const glm::vec3 scaleCorrection = SafeComponentDivide(legacyBindS, authoringBindS, legacyBindS);

    bool repaired = false;

    if (!track.t.keys.empty() && glm::length(translationCorrection) > 0.05f) {
        float correctedAvg = 0.0f;
        float scaledAvg = 0.0f;
        for (const auto& key : track.t.keys) {
            correctedAvg += glm::length(key.v + translationCorrection);
            scaledAvg += glm::length((key.v + translationCorrection) * scaleCorrection);
        }
        correctedAvg /= static_cast<float>(track.t.keys.size());
        scaledAvg /= static_cast<float>(track.t.keys.size());

        const float originalAvg = AverageCurveMagnitude(track.t);
        const float originalFirst = glm::length(track.t.keys.front().v);
        const float correctedFirst = glm::length(track.t.keys.front().v + translationCorrection);
        const float scaledFirst = glm::length((track.t.keys.front().v + translationCorrection) * scaleCorrection);
        const bool correctionRemovesLargeConstantOffset =
            (correctedAvg + 1e-4f < std::max(0.02f, originalAvg * 0.35f)) ||
            (correctedFirst + 1e-4f < std::max(0.01f, originalFirst * 0.25f));
        const bool originalLooksLikeLegacyBindBake =
            originalAvg > std::max(0.25f, std::abs(legacyBindT.y) * 0.5f);
        const bool scaledCurveLooksReasonable =
            scaledAvg <= std::max(1.5f, std::abs(legacyBindT.y) * 1.25f) &&
            scaledFirst <= std::max(1.5f, std::abs(legacyBindT.y) * 1.1f);

        if (correctionRemovesLargeConstantOffset &&
            originalLooksLikeLegacyBindBake &&
            scaledCurveLooksReasonable) {
            for (auto& key : track.t.keys) {
                key.v = (key.v + translationCorrection) * scaleCorrection;
            }
            repaired = true;
        }
    }

    if (!track.s.keys.empty() && MaxAbsComponentDelta(scaleCorrection, glm::vec3(1.0f)) > 0.5f) {
        float correctedErr = 0.0f;
        for (const auto& key : track.s.keys) {
            correctedErr += glm::length((key.v * scaleCorrection) - glm::vec3(1.0f));
        }
        correctedErr /= static_cast<float>(track.s.keys.size());

        const float originalErr = AverageCurveDistanceFrom(track.s, glm::vec3(1.0f));
        if (correctedErr + 1e-4f < std::max(0.01f, originalErr * 0.35f)) {
            for (auto& key : track.s.keys) {
                key.v *= scaleCorrection;
            }
            repaired = true;
        }
    }

    return repaired;
}

bool MaybeRepairBindRelativeHumanoidTracks(AnimationAsset& asset,
                                           const AvatarDefinition& sourceAvatar) {
    if (asset.meta.humanoidTrackMode != HumanoidTrackMode::BindRelative) {
        return false;
    }

    bool repairedAny = false;
    for (auto& trackPtr : asset.tracks) {
        if (!trackPtr || trackPtr->type != TrackType::Avatar) continue;
        auto* avatarTrack = static_cast<AssetAvatarTrack*>(trackPtr.get());
        repairedAny = MaybeRepairBindRelativeHumanoidTrack(*avatarTrack, sourceAvatar) || repairedAny;
    }
    return repairedAny;
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool NameContainsFingerToken(const std::string& lowerName) {
    return lowerName.find("thumb") != std::string::npos ||
           lowerName.find("index") != std::string::npos ||
           lowerName.find("middle") != std::string::npos ||
           lowerName.find("ring") != std::string::npos ||
           lowerName.find("pinky") != std::string::npos ||
           lowerName.find("little") != std::string::npos ||
           lowerName.find("finger") != std::string::npos;
}

bool MappingLooksSuspicious(HumanoidBone bone, const std::string& boneName) {
    if (boneName.empty()) return true;
    const std::string lowerName = ToLowerAscii(boneName);

    auto contains = [&](const char* token) {
        return lowerName.find(token) != std::string::npos;
    };

    const bool expectsLeft =
        bone == HumanoidBone::LeftShoulder ||
        bone == HumanoidBone::LeftUpperArm ||
        bone == HumanoidBone::LeftLowerArm ||
        bone == HumanoidBone::LeftHand ||
        bone == HumanoidBone::LeftUpperLeg ||
        bone == HumanoidBone::LeftLowerLeg ||
        bone == HumanoidBone::LeftFoot ||
        bone == HumanoidBone::LeftToes;
    const bool expectsRight =
        bone == HumanoidBone::RightShoulder ||
        bone == HumanoidBone::RightUpperArm ||
        bone == HumanoidBone::RightLowerArm ||
        bone == HumanoidBone::RightHand ||
        bone == HumanoidBone::RightUpperLeg ||
        bone == HumanoidBone::RightLowerLeg ||
        bone == HumanoidBone::RightFoot ||
        bone == HumanoidBone::RightToes;

    if (expectsLeft && contains("right")) return true;
    if (expectsRight && contains("left")) return true;

    switch (bone) {
        case HumanoidBone::LeftHand:
        case HumanoidBone::RightHand:
        case HumanoidBone::LeftUpperArm:
        case HumanoidBone::RightUpperArm:
        case HumanoidBone::LeftLowerArm:
        case HumanoidBone::RightLowerArm:
            return NameContainsFingerToken(lowerName);
        default:
            return false;
    }
}

bool AvatarMappingLooksUsable(const AvatarDefinition& avatar) {
    constexpr HumanoidBone kRequiredBones[] = {
        HumanoidBone::Hips,
        HumanoidBone::Spine,
        HumanoidBone::Neck,
        HumanoidBone::Head,
        HumanoidBone::LeftUpperArm,
        HumanoidBone::LeftLowerArm,
        HumanoidBone::LeftHand,
        HumanoidBone::RightUpperArm,
        HumanoidBone::RightLowerArm,
        HumanoidBone::RightHand,
        HumanoidBone::LeftUpperLeg,
        HumanoidBone::LeftLowerLeg,
        HumanoidBone::LeftFoot,
        HumanoidBone::RightUpperLeg,
        HumanoidBone::RightLowerLeg,
        HumanoidBone::RightFoot,
    };

    for (HumanoidBone bone : kRequiredBones) {
        if (!avatar.IsBonePresent(bone)) {
            return false;
        }
        if (MappingLooksSuspicious(bone, avatar.GetMappedBoneName(bone))) {
            return false;
        }
    }

    return true;
}

bool ReadAnimationImportSourceFile(const std::filesystem::path& animationPath, std::string& outSourceFile) {
    outSourceFile.clear();
    if (animationPath.empty()) return false;

    std::filesystem::path metaPath = animationPath;
    metaPath += ".meta";

    std::string text;
    if (!FileSystem::Instance().ReadTextFile(metaPath.string(), text)) {
        return false;
    }

    try {
        json j = json::parse(text);
        if (!j.contains("importSettings")) return false;
        outSourceFile = j["importSettings"].value("sourceFile", "");
        return !outSourceFile.empty();
    } catch (...) {
        return false;
    }
}

void AddCandidatePath(std::vector<std::filesystem::path>& outPaths,
                      std::unordered_set<std::string>& seen,
                      std::filesystem::path path) {
    if (path.empty()) return;
    path = path.lexically_normal();
    const std::string key = path.generic_string();
    if (seen.insert(key).second) {
        outPaths.push_back(std::move(path));
    }
}

std::vector<std::filesystem::path> BuildSourceSidecarCandidates(const std::filesystem::path& animationPath,
                                                                const std::string& sourceFile,
                                                                const char* extension) {
    namespace fs = std::filesystem;

    std::vector<fs::path> candidates;
    std::unordered_set<std::string> seen;
    fs::path sourcePath(sourceFile);

    auto addWithExtension = [&](fs::path basePath) {
        if (basePath.empty()) return;
        basePath.replace_extension(extension);
        AddCandidatePath(candidates, seen, std::move(basePath));
    };

    if (!sourcePath.empty()) {
        if (sourcePath.is_absolute()) {
            addWithExtension(sourcePath);
        } else {
            addWithExtension(GetBaseDirectory() / sourcePath);
        }

        if (!animationPath.empty()) {
            addWithExtension(animationPath.parent_path() / sourcePath.filename());
        }

        const std::string generic = sourcePath.generic_string();
        const size_t assetsPos = generic.find("assets/");
        if (assetsPos != std::string::npos) {
            addWithExtension(GetBaseDirectory() / generic.substr(assetsPos));
        }
    }

    return candidates;
}

bool ReadPackedSkeleton(const std::filesystem::path& skeletonPath, SkeletonComponent& outSkeleton) {
    constexpr uint32_t kSkelBinMagic = 'B' | ('L' << 8) | ('E' << 16) | ('S' << 24);

    struct SkelHeader {
        uint32_t magic;
        uint32_t version;
        uint32_t jointCount;
    };

    std::vector<uint8_t> data;
    if (!FileSystem::Instance().ReadFile(skeletonPath.string(), data)) {
        return false;
    }
    if (data.size() < sizeof(SkelHeader)) {
        return false;
    }

    SkelHeader header{};
    std::memcpy(&header, data.data(), sizeof(SkelHeader));
    if (header.magic != kSkelBinMagic || (header.version != 1 && header.version != 2)) {
        return false;
    }

    size_t offset = sizeof(SkelHeader);
    const size_t jointCount = static_cast<size_t>(header.jointCount);
    const size_t bindBytes = jointCount * sizeof(glm::mat4);
    const size_t parentBytes = jointCount * sizeof(int);
    if (offset + bindBytes + parentBytes > data.size()) {
        return false;
    }

    outSkeleton = SkeletonComponent{};
    outSkeleton.InverseBindPoses.resize(jointCount);
    std::memcpy(outSkeleton.InverseBindPoses.data(), data.data() + offset, bindBytes);
    offset += bindBytes;

    outSkeleton.BoneParents.resize(jointCount);
    std::memcpy(outSkeleton.BoneParents.data(), data.data() + offset, parentBytes);
    offset += parentBytes;

    if (header.version >= 2) {
        if (offset + sizeof(uint32_t) > data.size()) {
            return false;
        }

        uint32_t nameCount = 0;
        std::memcpy(&nameCount, data.data() + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        outSkeleton.BoneNames.resize(nameCount);
        for (uint32_t i = 0; i < nameCount; ++i) {
            if (offset + sizeof(uint32_t) > data.size()) {
                return false;
            }
            uint32_t len = 0;
            std::memcpy(&len, data.data() + offset, sizeof(uint32_t));
            offset += sizeof(uint32_t);
            if (offset + len > data.size()) {
                return false;
            }
            outSkeleton.BoneNames[i].assign(reinterpret_cast<const char*>(data.data() + offset), len);
            offset += len;
        }
    } else {
        outSkeleton.BoneNames.resize(jointCount);
    }

    outSkeleton.BoneNameToIndex.clear();
    for (size_t i = 0; i < outSkeleton.BoneNames.size(); ++i) {
        if (!outSkeleton.BoneNames[i].empty()) {
            outSkeleton.BoneNameToIndex[outSkeleton.BoneNames[i]] = static_cast<int>(i);
        }
    }

    return true;
}

void FinalizeLoadedAnimationAsset(AnimationAsset& asset, const std::filesystem::path& animationPath) {
    asset.RebuildRuntimeView();

    if (!HasAvatarTracks(asset)) return;
    if (asset.meta.humanoidTrackMode != HumanoidTrackMode::LegacyAbsolute &&
        asset.meta.humanoidTrackMode != HumanoidTrackMode::BindRelative) {
        return;
    }

    std::string sourceFile;
    if (!ReadAnimationImportSourceFile(animationPath, sourceFile)) {
        return;
    }

    SkeletonComponent sourceSkeleton;
    bool hasSkeleton = false;
    for (const auto& skeletonPath : BuildSourceSidecarCandidates(animationPath, sourceFile, ".skelbin")) {
        if (ReadPackedSkeleton(skeletonPath, sourceSkeleton)) {
            hasSkeleton = true;
            break;
        }
    }
    if (!hasSkeleton) {
        return;
    }

    auto sourceAvatar = std::make_shared<AvatarDefinition>();
    AvatarDefinition diskAvatar;
    bool loadedAvatar = false;
    for (const auto& avatarPath : BuildSourceSidecarCandidates(animationPath, sourceFile, ".avatar")) {
        if (LoadAvatar(diskAvatar, avatarPath.string())) {
            loadedAvatar = true;
            break;
        }
    }

    if (loadedAvatar) {
        *sourceAvatar = diskAvatar;
    }

    if (loadedAvatar && AvatarMappingLooksUsable(*sourceAvatar)) {
        avatar_builders::PopulateBindDataFromSkeleton(sourceSkeleton, *sourceAvatar);
    } else {
        avatar_builders::BuildFromSkeleton(sourceSkeleton, *sourceAvatar, true);
    }

    if (!AvatarMappingLooksUsable(*sourceAvatar)) {
        return;
    }

    asset.LegacySourceAvatar = sourceAvatar;
    const uint16_t hipsIdx = static_cast<uint16_t>(HumanoidBone::Hips);
    if (sourceAvatar->IsBonePresent(HumanoidBone::Hips) && hipsIdx < sourceAvatar->BindModel.size()) {
        const float sourceHipsHeight = std::abs(sourceAvatar->BindModel[hipsIdx][3].y);
        if (sourceHipsHeight > 0.1f) {
            asset.meta.referenceHipsHeight = sourceHipsHeight;
        }
    }

    if (MaybeRepairBindRelativeHumanoidTracks(asset, *sourceAvatar)) {
        std::cout << "[AnimationSerializer] Repaired legacy bind-relative humanoid bake for '"
                  << asset.name << "' using source avatar bind data.\n";
    }
}

void WriteCurve(const CurveFloat& curve, BinWriter& writer) {
    uint32_t count = static_cast<uint32_t>(curve.keys.size());
    writer.Write(count);
    for (const auto& key : curve.keys) {
        writer.Write(key.id);
        writer.Write(key.t);
        writer.Write(key.v);
    }
}

void WriteCurve(const CurveVec2& curve, BinWriter& writer) {
    uint32_t count = static_cast<uint32_t>(curve.keys.size());
    writer.Write(count);
    for (const auto& key : curve.keys) {
        writer.Write(key.id);
        writer.Write(key.t);
        writer.WriteVec2(key.v);
    }
}

void WriteCurve(const CurveVec3& curve, BinWriter& writer) {
    uint32_t count = static_cast<uint32_t>(curve.keys.size());
    writer.Write(count);
    for (const auto& key : curve.keys) {
        writer.Write(key.id);
        writer.Write(key.t);
        writer.WriteVec3(key.v);
    }
}

void WriteCurve(const CurveQuat& curve, BinWriter& writer) {
    uint32_t count = static_cast<uint32_t>(curve.keys.size());
    writer.Write(count);
    for (const auto& key : curve.keys) {
        writer.Write(key.id);
        writer.Write(key.t);
        writer.WriteQuat(key.v);
    }
}

void WriteCurve(const CurveColor& curve, BinWriter& writer) {
    uint32_t count = static_cast<uint32_t>(curve.keys.size());
    writer.Write(count);
    for (const auto& key : curve.keys) {
        writer.Write(key.id);
        writer.Write(key.t);
        writer.WriteVec4(key.v);
    }
}

bool ReadCurve(CurveFloat& curve, BinReader& reader) {
    uint32_t count = 0;
    reader.Read(count);
    if (!reader.ok) return false;
    curve.keys.resize(count);
    for (auto& key : curve.keys) {
        reader.Read(key.id);
        reader.Read(key.t);
        reader.Read(key.v);
        if (!reader.ok) return false;
    }
    curve.cache.last = 0;
    return true;
}

bool ReadCurve(CurveVec2& curve, BinReader& reader) {
    uint32_t count = 0;
    reader.Read(count);
    if (!reader.ok) return false;
    curve.keys.resize(count);
    for (auto& key : curve.keys) {
        reader.Read(key.id);
        reader.Read(key.t);
        reader.Read(key.v.x);
        reader.Read(key.v.y);
        if (!reader.ok) return false;
    }
    curve.cache.last = 0;
    return true;
}

bool ReadCurve(CurveVec3& curve, BinReader& reader) {
    uint32_t count = 0;
    reader.Read(count);
    if (!reader.ok) return false;
    curve.keys.resize(count);
    for (auto& key : curve.keys) {
        reader.Read(key.id);
        reader.Read(key.t);
        reader.Read(key.v.x);
        reader.Read(key.v.y);
        reader.Read(key.v.z);
        if (!reader.ok) return false;
    }
    curve.cache.last = 0;
    return true;
}

bool ReadCurve(CurveQuat& curve, BinReader& reader) {
    uint32_t count = 0;
    reader.Read(count);
    if (!reader.ok) return false;
    curve.keys.resize(count);
    for (auto& key : curve.keys) {
        reader.Read(key.id);
        reader.Read(key.t);
        reader.Read(key.v.w);
        reader.Read(key.v.x);
        reader.Read(key.v.y);
        reader.Read(key.v.z);
        if (!reader.ok) return false;
    }
    curve.cache.last = 0;
    return true;
}

bool ReadCurve(CurveColor& curve, BinReader& reader) {
    uint32_t count = 0;
    reader.Read(count);
    if (!reader.ok) return false;
    curve.keys.resize(count);
    for (auto& key : curve.keys) {
        reader.Read(key.id);
        reader.Read(key.t);
        reader.Read(key.v.x);
        reader.Read(key.v.y);
        reader.Read(key.v.z);
        reader.Read(key.v.w);
        if (!reader.ok) return false;
    }
    curve.cache.last = 0;
    return true;
}

bool WriteCompiledAnimationImpl(const AnimationAsset& asset, const std::filesystem::path& compiledPath) {
    if (compiledPath.empty()) return false;
    BinWriter writer(compiledPath);
    if (!writer.ok) return false;

    writer.Write(kAnimBinMagic);
    writer.Write(kAnimBinVersion);
    writer.WriteString(asset.name);
    writer.Write(asset.meta.version);
    writer.Write(asset.meta.length);
    writer.Write(asset.meta.fps);
    writer.Write(asset.meta.referenceHipsHeight);
    writer.Write(static_cast<uint8_t>(asset.meta.humanoidTrackMode));
    
    // Root motion settings (v3+)
    writer.Write(static_cast<uint8_t>(asset.meta.rootMotion.Mode));
    uint8_t rmFlags = 0;
    if (asset.meta.rootMotion.IncludeXZ) rmFlags |= 0x01;
    if (asset.meta.rootMotion.IncludeY) rmFlags |= 0x02;
    if (asset.meta.rootMotion.IncludeRotation) rmFlags |= 0x04;
    if (asset.meta.rootMotion.OverrideGravity) rmFlags |= 0x08;
    writer.Write(rmFlags);
    writer.WriteString(asset.meta.rootMotion.RootBoneName);
    writer.Write(asset.meta.rootMotion.TotalDistanceXZ);
    writer.Write(asset.meta.rootMotion.TotalDistanceY);
    
    uint32_t trackCount = static_cast<uint32_t>(asset.tracks.size());
    writer.Write(trackCount);

    for (const auto& trackPtr : asset.tracks) {
        if (!trackPtr) continue;
        writer.Write(static_cast<uint32_t>(trackPtr->type));
        writer.Write(trackPtr->id);
        writer.WriteString(trackPtr->name);
        uint8_t muted = trackPtr->muted ? 1u : 0u;
        writer.Write(muted);
        switch (trackPtr->type) {
            case TrackType::Bone: {
                const auto* bone = static_cast<const AssetBoneTrack*>(trackPtr.get());
                writer.Write(bone->boneId);
                WriteCurve(bone->t, writer);
                WriteCurve(bone->r, writer);
                WriteCurve(bone->s, writer);
            } break;
            case TrackType::Avatar: {
                const auto* avatar = static_cast<const AssetAvatarTrack*>(trackPtr.get());
                writer.Write(avatar->humanBoneId);
                WriteCurve(avatar->t, writer);
                WriteCurve(avatar->r, writer);
                WriteCurve(avatar->s, writer);
            } break;
            case TrackType::Property: {
                const auto* prop = static_cast<const AssetPropertyTrack*>(trackPtr.get());
                writer.WriteString(prop->binding.path);
                writer.Write(prop->binding.resolvedId);
                writer.Write(static_cast<uint32_t>(prop->binding.type));
                uint32_t curveKind = static_cast<uint32_t>(prop->curve.index());
                writer.Write(curveKind);
                std::visit([&](auto&& c) { WriteCurve(c, writer); }, prop->curve);
            } break;
            case TrackType::ScriptEvent: {
                const auto* script = static_cast<const AssetScriptEventTrack*>(trackPtr.get());
                uint32_t eventCount = static_cast<uint32_t>(script->events.size());
                writer.Write(eventCount);
                for (const auto& e : script->events) {
                    writer.Write(e.id);
                    writer.Write(e.time);
                    writer.WriteString(e.className);
                    writer.WriteString(e.method);
                    writer.WriteString(e.payload.dump());
                }
            } break;
        }
        if (!writer.ok) return false;
    }
    return writer.ok;
}

bool ReadCompiledAnimationImpl(const std::filesystem::path& compiledPath, AnimationAsset& out) {
    if (compiledPath.empty()) return false;
    std::vector<uint8_t> buffer;
    if (!FileSystem::Instance().ReadFile(compiledPath.string(), buffer)) {
        return false;
    }
    BinReader reader(buffer);
    if (!reader.ok) return false;

    uint32_t magic = 0;
    uint16_t version = 0;
    reader.Read(magic);
    reader.Read(version);
    // Support v2 (legacy), v3 (root motion), and v4 (humanoid track mode).
    if (!reader.ok || magic != kAnimBinMagic || (version != 2 && version != 3 && version != kAnimBinVersion)) return false;

    out.name = reader.ReadString();
    reader.Read(out.meta.version);
    reader.Read(out.meta.length);
    reader.Read(out.meta.fps);
    reader.Read(out.meta.referenceHipsHeight);
    if (version >= 4) {
        uint8_t trackMode = 0;
        reader.Read(trackMode);
        out.meta.humanoidTrackMode = static_cast<HumanoidTrackMode>(trackMode);
    } else {
        out.meta.humanoidTrackMode = HumanoidTrackMode::LegacyAbsolute;
    }
    
    // Root motion settings (v3+)
    if (version >= 3) {
        uint8_t rmMode = 0;
        reader.Read(rmMode);
        out.meta.rootMotion.Mode = static_cast<RootMotionMode>(rmMode);
        uint8_t rmFlags = 0;
        reader.Read(rmFlags);
        out.meta.rootMotion.IncludeXZ = (rmFlags & 0x01) != 0;
        out.meta.rootMotion.IncludeY = (rmFlags & 0x02) != 0;
        out.meta.rootMotion.IncludeRotation = (rmFlags & 0x04) != 0;
        out.meta.rootMotion.OverrideGravity = (rmFlags & 0x08) != 0;
        out.meta.rootMotion.RootBoneName = reader.ReadString();
        reader.Read(out.meta.rootMotion.TotalDistanceXZ);
        reader.Read(out.meta.rootMotion.TotalDistanceY);
    } else {
        // Default for v2 files: InPlace mode (backward compatible)
        out.meta.rootMotion = RootMotionSettings{};
    }
    
    uint32_t trackCount = 0;
    reader.Read(trackCount);
    if (!reader.ok) return false;

    out.tracks.clear();
    out.tracks.reserve(trackCount);
    for (uint32_t i = 0; i < trackCount; ++i) {
        uint32_t typeValue = 0;
        reader.Read(typeValue);
        TrackType type = static_cast<TrackType>(typeValue);
        uint64_t id = 0;
        reader.Read(id);
        std::string name = reader.ReadString();
        uint8_t muted = 0;
        reader.Read(muted);
        if (!reader.ok) return false;

        std::unique_ptr<ITrack> track;
        switch (type) {
            case TrackType::Bone: {
                auto bone = std::make_unique<AssetBoneTrack>();
                bone->id = id;
                bone->name = name;
                bone->muted = muted != 0;
                reader.Read(bone->boneId);
                if (!ReadCurve(bone->t, reader) || !ReadCurve(bone->r, reader) || !ReadCurve(bone->s, reader)) return false;
                track = std::move(bone);
            } break;
            case TrackType::Avatar: {
                auto avatar = std::make_unique<AssetAvatarTrack>();
                avatar->id = id;
                avatar->name = name;
                avatar->muted = muted != 0;
                reader.Read(avatar->humanBoneId);
                if (!ReadCurve(avatar->t, reader) || !ReadCurve(avatar->r, reader) || !ReadCurve(avatar->s, reader)) return false;
                track = std::move(avatar);
            } break;
            case TrackType::Property: {
                auto prop = std::make_unique<AssetPropertyTrack>();
                prop->id = id;
                prop->name = name;
                prop->muted = muted != 0;
                prop->binding.path = reader.ReadString();
                reader.Read(prop->binding.resolvedId);
                uint32_t bindingType = 0;
                reader.Read(bindingType);
                prop->binding.type = static_cast<PropertyType>(bindingType);
                uint32_t curveKind = 0;
                reader.Read(curveKind);
                switch (curveKind) {
                    case 0: {
                        CurveFloat c;
                        if (!ReadCurve(c, reader)) return false;
                        prop->curve = std::move(c);
                    } break;
                    case 1: {
                        CurveVec2 c;
                        if (!ReadCurve(c, reader)) return false;
                        prop->curve = std::move(c);
                    } break;
                    case 2: {
                        CurveVec3 c;
                        if (!ReadCurve(c, reader)) return false;
                        prop->curve = std::move(c);
                    } break;
                    case 3: {
                        CurveQuat c;
                        if (!ReadCurve(c, reader)) return false;
                        prop->curve = std::move(c);
                    } break;
                    case 4: {
                        CurveColor c;
                        if (!ReadCurve(c, reader)) return false;
                        prop->curve = std::move(c);
                    } break;
                    default:
                        return false;
                }
                track = std::move(prop);
            } break;
            case TrackType::ScriptEvent: {
                auto script = std::make_unique<AssetScriptEventTrack>();
                script->id = id;
                script->name = name;
                script->muted = muted != 0;
                uint32_t eventCount = 0;
                reader.Read(eventCount);
                script->events.resize(eventCount);
                for (auto& evt : script->events) {
                    reader.Read(evt.id);
                    reader.Read(evt.time);
                    evt.className = reader.ReadString();
                    evt.method = reader.ReadString();
                    std::string payload = reader.ReadString();
                    try {
                        evt.payload = payload.empty() ? json::object() : json::parse(payload);
                    } catch (...) {
                        evt.payload = json::object();
                    }
                    if (!reader.ok) return false;
                }
                track = std::move(script);
            } break;
            default:
                return false;
        }
        if (!reader.ok || !track) return false;
        out.tracks.push_back(std::move(track));
    }

    return reader.ok;
}

} // namespace

// Public wrappers for the internal binary animation functions
bool WriteCompiledAnimation(const AnimationAsset& asset, const std::filesystem::path& compiledPath) {
    return WriteCompiledAnimationImpl(asset, compiledPath);
}

bool ReadCompiledAnimation(const std::filesystem::path& compiledPath, AnimationAsset& out) {
    return ReadCompiledAnimationImpl(compiledPath, out);
}

std::filesystem::path GetAnimationBinaryPath(const std::string& sourcePath) {
#ifdef CLAYMORE_EDITOR
    std::string binPath = BinaryAssetCache::Instance().GetBinaryPath(sourcePath);
    return std::filesystem::path(binPath);
#else
    // In runtime, just return the path with .animbin extension
    std::filesystem::path p(sourcePath);
    p.replace_extension(".animbin");
    return p;
#endif
}

// ---------------- Keyframes ------------------
json SerializeKeyframe(const KeyframeVec3& kf) {
    return json{{"t", kf.Time}, {"v", {kf.Value.x, kf.Value.y, kf.Value.z}}};
}

json SerializeKeyframe(const KeyframeQuat& kf) {
    return json{{"t", kf.Time}, {"v", {kf.Value.x, kf.Value.y, kf.Value.z, kf.Value.w}}};
}

json SerializeKeyframe(const KeyframeFloat& kf) {
    return json{{"t", kf.Time}, {"v", kf.Value}};
}

KeyframeVec3 DeserializeKeyframeVec3(const json& j) {
    KeyframeVec3 kf;
    kf.Time = j["t"].get<float>();
    const auto& arr = j["v"];
    kf.Value = glm::vec3(arr[0].get<float>(), arr[1].get<float>(), arr[2].get<float>());
    return kf;
}

KeyframeQuat DeserializeKeyframeQuat(const json& j) {
    KeyframeQuat kf;
    kf.Time = j["t"].get<float>();
    const auto& arr = j["v"];
    kf.Value = glm::quat(arr[3].get<float>(), arr[0].get<float>(), arr[1].get<float>(), arr[2].get<float>());
    return kf;
}

KeyframeFloat DeserializeKeyframeFloat(const json& j) {
    KeyframeFloat kf;
    kf.Time = j["t"].get<float>();
    kf.Value = j["v"].get<float>();
    return kf;
}

// --------------- Clip --------------------
json SerializeAnimationClip(const AnimationClip& clip) {
    json j;
    j["name"] = clip.Name;
    j["duration"] = clip.Duration;
    j["tps"] = clip.TicksPerSecond;

    json tracksJson;
    for (const auto& [boneName, track] : clip.BoneTracks) {
        json t;
        for (const auto& k : track.PositionKeys) t["pos"].push_back(SerializeKeyframe(k));
        for (const auto& k : track.RotationKeys) t["rot"].push_back(SerializeKeyframe(k));
        for (const auto& k : track.ScaleKeys)    t["scl"].push_back(SerializeKeyframe(k));
        tracksJson[boneName] = std::move(t);
    }
    j["tracks"] = std::move(tracksJson);
    j["humanoid"] = clip.IsHumanoid;
    if (!clip.SourceAvatarRigName.empty()) j["avatarRig"] = clip.SourceAvatarRigName;
    if (!clip.SourceAvatarPath.empty()) j["avatarPath"] = clip.SourceAvatarPath;
    if (clip.IsHumanoid && !clip.HumanoidTracks.empty()) {
        json h;
        for (const auto& [id, bt] : clip.HumanoidTracks) {
            json t;
            if (!bt.PositionKeys.empty()) { json arr = json::array(); for (const auto& k : bt.PositionKeys) arr.push_back(SerializeKeyframe(k)); t["pos"] = std::move(arr); }
            if (!bt.RotationKeys.empty()) { json arr = json::array(); for (const auto& k : bt.RotationKeys) arr.push_back(SerializeKeyframe(k)); t["rot"] = std::move(arr); }
            if (!bt.ScaleKeys.empty())    { json arr = json::array(); for (const auto& k : bt.ScaleKeys) arr.push_back(SerializeKeyframe(k)); t["scl"] = std::move(arr); }
            h[std::to_string(id)] = std::move(t);
        }
        j["humanoidTracks"] = std::move(h);
    }
    return j;
}

AnimationClip DeserializeAnimationClip(const json& j) {
    AnimationClip clip;
    clip.Name = j.value("name", "");
    clip.Duration = j.value("duration", 0.0f);
    clip.TicksPerSecond = j.value("tps", 0.0f);

    if (j.contains("tracks") && j["tracks"].is_object()) {
        for (auto it = j["tracks"].begin(); it != j["tracks"].end(); ++it) {
            BoneTrack track;
            const json& t = it.value();
            if (t.contains("pos")) {
                for (const auto& k : t["pos"]) track.PositionKeys.push_back(DeserializeKeyframeVec3(k));
            }
            if (t.contains("rot")) {
                for (const auto& k : t["rot"]) track.RotationKeys.push_back(DeserializeKeyframeQuat(k));
            }
            if (t.contains("scl")) {
                for (const auto& k : t["scl"]) track.ScaleKeys.push_back(DeserializeKeyframeVec3(k));
            }
            clip.BoneTracks[it.key()] = std::move(track);
        }
    } else if (j.contains("tracks") && j["tracks"].is_array()) {
        std::cerr << "[AnimationSerializer] Warning: 'tracks' is an array, expected object. Skipping bone tracks.\n";
    }
    clip.IsHumanoid = j.value("humanoid", false);
    clip.SourceAvatarRigName = j.value("avatarRig", "");
    clip.SourceAvatarPath = j.value("avatarPath", "");
    if (clip.IsHumanoid && j.contains("humanoidTracks") && j["humanoidTracks"].is_object()) {
        for (auto it = j["humanoidTracks"].begin(); it != j["humanoidTracks"].end(); ++it) {
            BoneTrack track; const json& t = it.value();
            if (t.contains("pos")) for (const auto& k : t["pos"]) track.PositionKeys.push_back(DeserializeKeyframeVec3(k));
            if (t.contains("rot")) for (const auto& k : t["rot"]) track.RotationKeys.push_back(DeserializeKeyframeQuat(k));
            if (t.contains("scl")) for (const auto& k : t["scl"]) track.ScaleKeys.push_back(DeserializeKeyframeVec3(k));
            int id = std::stoi(it.key());
            clip.HumanoidTracks[id] = std::move(track);
        }
    } else if (clip.IsHumanoid && j.contains("humanoidTracks") && !j["humanoidTracks"].is_object()) {
        std::cerr << "[AnimationSerializer] Warning: 'humanoidTracks' is not an object. Skipping humanoid tracks.\n";
    }
    return clip;
}

bool SaveAnimationClip(const AnimationClip& clip, const std::string& path) {
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << SerializeAnimationClip(clip).dump(4);
    return true;
}

// Helper: Convert AnimationAsset to legacy AnimationClip (reverse of WrapLegacyClipAsAsset)
static AnimationClip UnwrapAssetAsLegacyClip(const AnimationAsset& asset) {
    AnimationClip clip;
    clip.Name = asset.name;
    clip.Duration = asset.meta.length;
    clip.TicksPerSecond = asset.meta.fps;
    
    for (const auto& trackPtr : asset.tracks) {
        if (!trackPtr) continue;
        
        if (trackPtr->type == TrackType::Bone) {
            const auto* boneTrack = static_cast<const AssetBoneTrack*>(trackPtr.get());
            BoneTrack bt;
            for (const auto& k : boneTrack->t.keys) {
                KeyframeVec3 kf; kf.Time = k.t; kf.Value = k.v;
                bt.PositionKeys.push_back(kf);
            }
            for (const auto& k : boneTrack->r.keys) {
                KeyframeQuat kf; kf.Time = k.t; kf.Value = k.v;
                bt.RotationKeys.push_back(kf);
            }
            for (const auto& k : boneTrack->s.keys) {
                KeyframeVec3 kf; kf.Time = k.t; kf.Value = k.v;
                bt.ScaleKeys.push_back(kf);
            }
            clip.BoneTracks[trackPtr->name] = std::move(bt);
        }
        else if (trackPtr->type == TrackType::Avatar) {
            const auto* avatarTrack = static_cast<const AssetAvatarTrack*>(trackPtr.get());
            BoneTrack bt;
            for (const auto& k : avatarTrack->t.keys) {
                KeyframeVec3 kf; kf.Time = k.t; kf.Value = k.v;
                bt.PositionKeys.push_back(kf);
            }
            for (const auto& k : avatarTrack->r.keys) {
                KeyframeQuat kf; kf.Time = k.t; kf.Value = k.v;
                bt.RotationKeys.push_back(kf);
            }
            for (const auto& k : avatarTrack->s.keys) {
                KeyframeVec3 kf; kf.Time = k.t; kf.Value = k.v;
                bt.ScaleKeys.push_back(kf);
            }
            clip.IsHumanoid = true;
            clip.HumanoidTracks[avatarTrack->humanBoneId] = std::move(bt);
        }
    }
    return clip;
}

AnimationClip LoadAnimationClip(const std::string& path) {
    AnimationClip empty{};
    const bool shouldLoadBinary = Assets::ShouldLoadBinary();
    const bool allowSourceFallback = Assets::AllowSourceFallback();
    
    // Try loading from binary cache first
    AnimationAsset asset;
    bool loadedBinary = false;
    std::string binaryPath;
    
#ifdef CLAYMORE_EDITOR
    // Get binary cache path
    if (BinaryAssetCache::Instance().IsCurrent(path)) {
        binaryPath = BinaryAssetCache::Instance().GetBinaryPath(path);
        loadedBinary = ReadCompiledAnimation(std::filesystem::path(binaryPath), asset);
    }
#endif
    
    // Resolve the source path for binary lookups
    std::filesystem::path resolvedSource = ResolveAbsoluteAnimationPath(path);
    
    // Try .animbin extension directly (for runtime/PAK)
    if (!loadedBinary) {
        std::filesystem::path binPath = resolvedSource.empty() ? std::filesystem::path(path) : resolvedSource;
        binPath.replace_extension(".animbin");
        loadedBinary = ReadCompiledAnimation(binPath, asset);
    }
    
    // Try legacy .animc path
    if (!loadedBinary && !resolvedSource.empty()) {
        std::filesystem::path compiledPath = DeriveCompiledAnimationPath(resolvedSource);
        if (!compiledPath.empty()) {
            loadedBinary = ReadCompiledAnimation(compiledPath, asset);
        }
    }
    
    if (loadedBinary && !asset.tracks.empty()) {
        return UnwrapAssetAsLegacyClip(asset);
    }

    if (shouldLoadBinary && !allowSourceFallback) {
        std::cerr << "[AnimationSerializer] Missing binary animation clip: " << path << "\n";
        return empty;
    }
    
    // No binary found - load JSON only when source fallback is allowed.
    AnimationClip clip;
    
    std::string text;
    std::filesystem::path resolvedPath;
    if (!OpenAnimationJson(path, text, resolvedPath)) {
        std::cerr << "[AnimationSerializer] Failed to open animation clip: " << path << "\n";
        return empty;
    }
    
    try {
        json j = json::parse(text);
        
        // Check if this is the new AnimationAsset format (tracks is array)
        // or the legacy AnimationClip format (tracks is object)
        if (j.contains("tracks") && j["tracks"].is_array()) {
            // New AnimationAsset format - deserialize as asset and convert to legacy clip
            AnimationAsset assetFromJson = DeserializeAnimationAsset(j);
            clip = UnwrapAssetAsLegacyClip(assetFromJson);
        } else {
            // Legacy format with tracks as object
            clip = DeserializeAnimationClip(j);
        }
        
        if (clip.BoneTracks.empty() && clip.HumanoidTracks.empty()) {
            std::cerr << "[AnimationSerializer] Animation clip has no tracks: " << path << "\n";
            return empty;
        }
    } catch (const std::exception& e) {
        std::cerr << "[AnimationSerializer] Error parsing animation clip '" << path << "': " << e.what() << "\n";
        return empty;
    }
    
#ifdef CLAYMORE_EDITOR
    // Keep the cache warm during editor authoring, but do not rebuild on play-mode
    // load paths.
    if (Assets::GetLoadMode() == AssetLoadMode::Editor && !binaryPath.empty()) {
        AnimationAsset assetToCompile = WrapLegacyClipAsAsset(clip);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(binaryPath).parent_path(), ec);
        if (WriteCompiledAnimation(assetToCompile, std::filesystem::path(binaryPath))) {
            std::cout << "[AnimationSerializer] Compiled animation binary: " << binaryPath << std::endl;
        } else {
            std::cerr << "[AnimationSerializer] Failed to compile animation binary: " << binaryPath << std::endl;
        }
    }
#endif
    
    return clip;
}

// ---------------- Timeline Clip (property + script) ----------------
namespace {
    json SerializePropertyTrack(const PropertyTrack& t)
    {
        json j;
        j["path"] = t.PropertyPath;
        j["keys"] = json::array();
        for (const auto& k : t.Keys) j["keys"].push_back(SerializeKeyframe(k));
        return j;
    }

    PropertyTrack DeserializePropertyTrack(const json& j)
    {
        PropertyTrack t;
        t.PropertyPath = j.value("path", "");
        if (j.contains("keys"))
            for (const auto& k : j["keys"]) t.Keys.push_back(DeserializeKeyframeFloat(k));
        return t;
    }

    json SerializeScriptTrack(const ScriptEventTrack& t)
    {
        json j;
        j["name"] = t.Name;
        j["keys"] = json::array();
        for (const auto& k : t.Keys)
            j["keys"].push_back({{"t", k.Time}, {"class", k.ScriptClass}, {"method", k.Method}});
        return j;
    }

    ScriptEventTrack DeserializeScriptTrack(const json& j)
    {
        ScriptEventTrack t;
        t.Name = j.value("name", "Script Events");
        if (j.contains("keys"))
            for (const auto& kj : j["keys"]) {
                ScriptEventKey k;
                k.Time = kj.value("t", 0.0f);
                k.ScriptClass = kj.value("class", "");
                k.Method = kj.value("method", "");
                t.Keys.push_back(std::move(k));
            }
        return t;
    }
}

// Backward-compat loader: if file looks like legacy skeletal .anim (our JSON shape), allow wrapping to unified asset if requested elsewhere.

json SerializeTimelineClip(const TimelineClip& clip)
{
    json j;
    j["name"] = clip.Name;
    j["length"] = clip.Length;
    j["tracks"] = json::array();
    for (const auto& t : clip.Tracks) j["tracks"].push_back(SerializePropertyTrack(t));
    j["scriptTracks"] = json::array();
    for (const auto& t : clip.ScriptTracks) j["scriptTracks"].push_back(SerializeScriptTrack(t));
    // optional skeletal clips
    if (!clip.SkeletalClips.empty()) {
        json arr = json::array();
        for (const auto& sc : clip.SkeletalClips) arr.push_back({{"path", sc.ClipPath}, {"speed", sc.Speed}, {"loop", sc.Loop}});
        j["skeletal"] = std::move(arr);
    }
    return j;
}

TimelineClip DeserializeTimelineClip(const json& j)
{
    TimelineClip clip;
    clip.Name = j.value("name", "");
    clip.Length = j.value("length", 0.0f);
    if (j.contains("tracks"))
        for (const auto& tj : j["tracks"]) clip.Tracks.push_back(DeserializePropertyTrack(tj));
    if (j.contains("scriptTracks"))
        for (const auto& sj : j["scriptTracks"]) clip.ScriptTracks.push_back(DeserializeScriptTrack(sj));
    if (j.contains("skeletal")) {
        for (const auto& sj : j["skeletal"]) {
            TimelineClip::SkeletalClipRef sc;
            sc.ClipPath = sj.value("path", "");
            sc.Speed = sj.value("speed", 1.0f);
            sc.Loop = sj.value("loop", true);
            clip.SkeletalClips.push_back(std::move(sc));
        }
    }
    return clip;
}

bool SaveTimelineClip(const TimelineClip& clip, const std::string& path)
{
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << SerializeTimelineClip(clip).dump(4);
    return true;
}

TimelineClip LoadTimelineClip(const std::string& path)
{
    std::string text;
    if (!FileSystem::Instance().ReadTextFile(path, text)) {
        std::cerr << "[AnimationSerializer] Failed to open timeline clip: " << path << "\n";
        return TimelineClip{};
    }
    try {
        json j = json::parse(text);
        return DeserializeTimelineClip(j);
    } catch (const std::exception& e) {
        std::cerr << "[AnimationSerializer] Error parsing timeline clip '" << path << "': " << e.what() << "\n";
        return TimelineClip{};
    }
}

// ---------------- Unified AnimationAsset (v1) ----------------
json SerializeAnimationAsset(const AnimationAsset& asset)
{
    json j;
    json metaJ = { 
        {"version", asset.meta.version}, 
        {"length", asset.meta.length}, 
        {"fps", asset.meta.fps}, 
        {"referenceHipsHeight", asset.meta.referenceHipsHeight},
        {"humanoidTrackMode", static_cast<int>(asset.meta.humanoidTrackMode)}
    };
    
    // Root motion settings
    json rmJ;
    rmJ["mode"] = static_cast<int>(asset.meta.rootMotion.Mode);
    rmJ["includeXZ"] = asset.meta.rootMotion.IncludeXZ;
    rmJ["includeY"] = asset.meta.rootMotion.IncludeY;
    rmJ["includeRotation"] = asset.meta.rootMotion.IncludeRotation;
    rmJ["overrideGravity"] = asset.meta.rootMotion.OverrideGravity;
    rmJ["rootBoneName"] = asset.meta.rootMotion.RootBoneName;
    rmJ["totalDistanceXZ"] = asset.meta.rootMotion.TotalDistanceXZ;
    rmJ["totalDistanceY"] = asset.meta.rootMotion.TotalDistanceY;
    metaJ["rootMotion"] = rmJ;
    
    j["meta"] = metaJ;
    j["name"] = asset.name;
    j["tracks"] = json::array();
    for (const auto& t : asset.tracks) {
        if (!t) continue;
        json jt; jt["id"] = t->id; jt["name"] = t->name; jt["muted"] = t->muted;
        switch (t->type) {
            case TrackType::Bone: {
                jt["type"] = "Bone";
                const auto* bt = static_cast<const AssetBoneTrack*>(t.get());
                jt["boneId"] = bt->boneId;
                auto dumpCurveVec3 = [](CurveVec3 const& c){ json a = json::array(); for (auto const& k : c.keys) a.push_back({{"id", k.id}, {"t", k.t}, {"v", {k.v.x, k.v.y, k.v.z}}}); return a; };
                auto dumpCurveQuat = [](CurveQuat const& c){ json a = json::array(); for (auto const& k : c.keys) a.push_back({{"id", k.id}, {"t", k.t}, {"v", {k.v.x, k.v.y, k.v.z, k.v.w}}}); return a; };
                jt["t"] = dumpCurveVec3(bt->t);
                jt["r"] = dumpCurveQuat(bt->r);
                jt["s"] = dumpCurveVec3(bt->s);
            } break;
            case TrackType::Avatar: {
                jt["type"] = "Avatar";
                const auto* at = static_cast<const AssetAvatarTrack*>(t.get());
                jt["humanBoneId"] = at->humanBoneId;
                auto dumpCurveVec3 = [](CurveVec3 const& c){ json a = json::array(); for (auto const& k : c.keys) a.push_back({{"id", k.id}, {"t", k.t}, {"v", {k.v.x, k.v.y, k.v.z}}}); return a; };
                auto dumpCurveQuat = [](CurveQuat const& c){ json a = json::array(); for (auto const& k : c.keys) a.push_back({{"id", k.id}, {"t", k.t}, {"v", {k.v.x, k.v.y, k.v.z, k.v.w}}}); return a; };
                jt["t"] = dumpCurveVec3(at->t);
                jt["r"] = dumpCurveQuat(at->r);
                jt["s"] = dumpCurveVec3(at->s);
            } break;
            case TrackType::Property: {
                jt["type"] = "Property";
                const auto* pt = static_cast<const AssetPropertyTrack*>(t.get());
                jt["binding"] = { {"path", pt->binding.path}, {"resolvedId", pt->binding.resolvedId}, {"ptype", (int)pt->binding.type} };
                switch (pt->binding.type) {
                    case PropertyType::Float: { json a = json::array(); for (auto const& k : std::get<CurveFloat>(pt->curve).keys) a.push_back({{"id", k.id},{"t",k.t},{"v",k.v}}); jt["curve"] = a; } break;
                    case PropertyType::Vec2:  { json a = json::array(); for (auto const& k : std::get<CurveVec2>(pt->curve).keys)  a.push_back({{"id", k.id},{"t",k.t},{"v", {k.v.x, k.v.y}}}); jt["curve"] = a; } break;
                    case PropertyType::Vec3:  { json a = json::array(); for (auto const& k : std::get<CurveVec3>(pt->curve).keys)  a.push_back({{"id", k.id},{"t",k.t},{"v", {k.v.x, k.v.y, k.v.z}}}); jt["curve"] = a; } break;
                    case PropertyType::Quat:  { json a = json::array(); for (auto const& k : std::get<CurveQuat>(pt->curve).keys)  a.push_back({{"id", k.id},{"t",k.t},{"v", {k.v.x, k.v.y, k.v.z, k.v.w}}}); jt["curve"] = a; } break;
                    case PropertyType::Color: { json a = json::array(); for (auto const& k : std::get<CurveColor>(pt->curve).keys) a.push_back({{"id", k.id},{"t",k.t},{"v", {k.v.x, k.v.y, k.v.z, k.v.w}}}); jt["curve"] = a; } break;
                }
            } break;
            case TrackType::ScriptEvent: {
                jt["type"] = "ScriptEvent";
                const auto* st = static_cast<const AssetScriptEventTrack*>(t.get());
                json arr = json::array();
                for (const auto& e : st->events) arr.push_back({{"id", e.id}, {"t", e.time}, {"class", e.className}, {"method", e.method}, {"payload", e.payload}});
                jt["events"] = std::move(arr);
            } break;
        }
        j["tracks"].push_back(std::move(jt));
    }
    return j;
}

static void readCurve(json const& arr, CurveFloat& c){ for (auto const& k : arr) c.keys.push_back({k.value("id",0ull), k.value("t",0.f), k.value("v",0.f)}); }
static void readCurve(const json& arr, CurveVec2& c){ for (const auto& k : arr) c.keys.push_back({k.value("id",0ull), k.value("t",0.f), glm::vec2(k["v"][0].get<float>(), k["v"][1].get<float>())}); }
static void readCurve(const json& arr, CurveVec3& c){ for (const auto& k : arr) c.keys.push_back({k.value("id",0ull), k.value("t",0.f), glm::vec3(k["v"][0].get<float>(), k["v"][1].get<float>(), k["v"][2].get<float>())}); }
static void readCurve(const json& arr, CurveQuat& c){ for (const auto& k : arr) c.keys.push_back({k.value("id",0ull), k.value("t",0.f), glm::quat(k["v"][3].get<float>(), k["v"][0].get<float>(), k["v"][1].get<float>(), k["v"][2].get<float>())}); }
static void readCurve(const json& arr, CurveColor& c){ for (const auto& k : arr) c.keys.push_back({k.value("id",0ull), k.value("t",0.f), glm::vec4(k["v"][0].get<float>(), k["v"][1].get<float>(), k["v"][2].get<float>(), k["v"][3].get<float>())}); }

AnimationAsset DeserializeAnimationAsset(const json& j)
{
    AnimationAsset a; a.name = j.value("name", "");
    if (j.contains("meta")) { 
        a.meta.version = j["meta"].value("version", 1); 
        a.meta.length = j["meta"].value("length", 0.0f); 
        a.meta.fps = j["meta"].value("fps", 30.0f); 
        a.meta.referenceHipsHeight = j["meta"].value("referenceHipsHeight", 0.0f);
        a.meta.humanoidTrackMode = static_cast<HumanoidTrackMode>(j["meta"].value("humanoidTrackMode", 0));
        
        // Root motion settings
        if (j["meta"].contains("rootMotion")) {
            const auto& rmJ = j["meta"]["rootMotion"];
            a.meta.rootMotion.Mode = static_cast<RootMotionMode>(rmJ.value("mode", 1)); // Default InPlace
            a.meta.rootMotion.IncludeXZ = rmJ.value("includeXZ", true);
            a.meta.rootMotion.IncludeY = rmJ.value("includeY", false);
            a.meta.rootMotion.IncludeRotation = rmJ.value("includeRotation", false);
            a.meta.rootMotion.OverrideGravity = rmJ.value("overrideGravity", false);
            a.meta.rootMotion.RootBoneName = rmJ.value("rootBoneName", "");
            a.meta.rootMotion.TotalDistanceXZ = rmJ.value("totalDistanceXZ", 0.0f);
            a.meta.rootMotion.TotalDistanceY = rmJ.value("totalDistanceY", 0.0f);
        }
    }
    if (j.contains("tracks")) {
        for (const auto& jt : j["tracks"]) {
            const std::string tt = jt.value("type", "");
            if (tt == "Bone") {
                auto t = std::make_unique<AssetBoneTrack>();
                t->type = TrackType::Bone; t->id = jt.value("id", 0ull); t->name = jt.value("name", ""); t->muted = jt.value("muted", false); t->boneId = jt.value("boneId", -1);
                if (jt.contains("t")) readCurve(jt["t"], t->t);
                if (jt.contains("r")) readCurve(jt["r"], t->r);
                if (jt.contains("s")) readCurve(jt["s"], t->s);
                a.tracks.push_back(std::move(t));
            } else if (tt == "Avatar") {
                auto t = std::make_unique<AssetAvatarTrack>();
                t->type = TrackType::Avatar; t->id = jt.value("id", 0ull); t->name = jt.value("name", ""); t->muted = jt.value("muted", false); t->humanBoneId = jt.value("humanBoneId", -1);
                if (jt.contains("t")) readCurve(jt["t"], t->t);
                if (jt.contains("r")) readCurve(jt["r"], t->r);
                if (jt.contains("s")) readCurve(jt["s"], t->s);
                // Fallback naming for display if not provided
                if (t->name.empty() && t->humanBoneId >= 0) {
                    t->name = std::string("Humanoid:") + ToString(static_cast<HumanBone>(t->humanBoneId));
                }
                a.tracks.push_back(std::move(t));
            } else if (tt == "Property") {
                auto t = std::make_unique<AssetPropertyTrack>();
                t->type = TrackType::Property; t->id = jt.value("id", 0ull); t->name = jt.value("name", ""); t->muted = jt.value("muted", false);
                if (jt.contains("binding")) { t->binding.path = jt["binding"].value("path", ""); t->binding.resolvedId = jt["binding"].value("resolvedId", 0ull); t->binding.type = static_cast<PropertyType>(jt["binding"].value("ptype", 0)); }
                if (jt.contains("curve")) {
                    switch (t->binding.type) {
                        case PropertyType::Float: { CurveFloat c; readCurve(jt["curve"], c); t->curve = std::move(c); } break;
                        case PropertyType::Vec2:  { CurveVec2  c; readCurve(jt["curve"], c); t->curve = std::move(c); } break;
                        case PropertyType::Vec3:  { CurveVec3  c; readCurve(jt["curve"], c); t->curve = std::move(c); } break;
                        case PropertyType::Quat:  { CurveQuat  c; readCurve(jt["curve"], c); t->curve = std::move(c); } break;
                        case PropertyType::Color: { CurveColor c; readCurve(jt["curve"], c); t->curve = std::move(c); } break;
                    }
                }
                a.tracks.push_back(std::move(t));
            } else if (tt == "ScriptEvent") {
                auto t = std::make_unique<AssetScriptEventTrack>();
                t->type = TrackType::ScriptEvent; t->id = jt.value("id", 0ull); t->name = jt.value("name", ""); t->muted = jt.value("muted", false);
                if (jt.contains("events")) {
                    for (const auto& ej : jt["events"]) {
                        AssetScriptEvent e; e.id = ej.value("id", 0ull); e.time = ej.value("t", 0.0f); e.className = ej.value("class", ""); e.method = ej.value("method", ""); e.payload = ej.value("payload", json{});
                        t->events.push_back(std::move(e));
                    }
                }
                a.tracks.push_back(std::move(t));
            }
        }
    }
    return a;
}

bool SaveAnimationAsset(const AnimationAsset& asset, const std::string& path)
{
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << SerializeAnimationAsset(asset).dump(4);
    f.close();

#ifdef CLAYMORE_EDITOR
    // Update the binary cache
    std::string binaryPath = BinaryAssetCache::Instance().GetBinaryPath(path);
    if (!binaryPath.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(binaryPath).parent_path(), ec);
        WriteCompiledAnimation(asset, std::filesystem::path(binaryPath));
        std::cout << "[AnimationSerializer] Updated binary cache: " << binaryPath << std::endl;
    }
    InvalidateAnimationAssetCache(path);
    InvalidateAnimationAssetCache(binaryPath);
#endif

    return true;
}

AnimationAsset LoadAnimationAsset(const std::string& path)
{
    AnimationAsset asset;
    const bool shouldLoadBinary = Assets::ShouldLoadBinary();
    const bool allowSourceFallback = Assets::AllowSourceFallback();
    const bool useBinaryCache = shouldLoadBinary && (Assets::GetLoadMode() == AssetLoadMode::PlayMode);
    const std::filesystem::path resolvedAnimationPath = ResolveAbsoluteAnimationPath(path);
    const std::filesystem::path finalizePath = resolvedAnimationPath.empty() ? std::filesystem::path(path) : resolvedAnimationPath;
    
    // In play mode or runtime, try loading from binary formats
    if (shouldLoadBinary) {
#ifdef CLAYMORE_EDITOR
        // PRIORITY 1: Binary cache (.animbin in .bin folder) - this is the canonical binary format
        if (useBinaryCache) {
            std::string binaryPath = BinaryAssetCache::Instance().IsCurrent(path)
                ? BinaryAssetCache::Instance().GetBinaryPath(path)
                : std::string();
            if (!binaryPath.empty() && FileSystem::Instance().Exists(binaryPath)) {
                if (ReadCompiledAnimation(std::filesystem::path(binaryPath), asset)) {
                    FinalizeLoadedAnimationAsset(asset, finalizePath);
                    return asset;
                }
            }
        }
#endif
        // PRIORITY 2: .animbin extension directly (for runtime/PAK)
        // In runtime mode, don't use std::filesystem::exists - just try to load directly via VFS
        std::filesystem::path binPath(path);
        binPath.replace_extension(".animbin");
        
#ifdef CLAYMORE_RUNTIME
        // In runtime, try loading directly - ReadCompiledAnimation uses FileSystem which goes through VFS
        if (ReadCompiledAnimation(binPath, asset)) {
            FinalizeLoadedAnimationAsset(asset, finalizePath);
            return asset;
        }
#else
        if (ReadCompiledAnimation(binPath, asset)) {
            FinalizeLoadedAnimationAsset(asset, finalizePath);
            return asset;
        }
        
        // PRIORITY 3: Legacy .animc paths (DEPRECATED - only as fallback)
        // These are old-style compiled animations that may not have updated metadata
        if (allowSourceFallback) {
            std::filesystem::path resolvedSource = ResolveAbsoluteAnimationPath(path);
            std::filesystem::path compiledPath = resolvedSource.empty() ? std::filesystem::path() : DeriveCompiledAnimationPath(resolvedSource);
            
            if (!compiledPath.empty() && std::filesystem::exists(compiledPath)) {
                if (ReadCompiledAnimation(compiledPath, asset)) {
                    FinalizeLoadedAnimationAsset(asset, finalizePath);
                    return asset;
                }
            }
            
            // PRIORITY 4: Another .animc fallback path
            std::string animcPath = path;
            if (animcPath.size() > 5) {
                std::string ext = animcPath.substr(animcPath.size() - 5);
                if (ext != ".animc") {
                    animcPath += "c"; // .anim -> .animc
                }
            }
            if (std::filesystem::exists(animcPath)) {
                std::vector<uint8_t> compiledData;
                if (FileSystem::Instance().ReadFile(animcPath, compiledData) && !compiledData.empty()) {
                    constexpr uint32_t expectedMagic = 'A' | ('N' << 8) | ('I' << 16) | ('M' << 24);
                    if (compiledData.size() >= 6) {
                        uint32_t magic = 0;
                        uint16_t version = 0;
                        std::memcpy(&magic, compiledData.data(), 4);
                        std::memcpy(&version, compiledData.data() + 4, 2);
                        if (magic == expectedMagic && version == 1) {
                            if (ReadCompiledAnimation(std::filesystem::path(animcPath), asset)) {
                                FinalizeLoadedAnimationAsset(asset, finalizePath);
                                return asset;
                            }
                        }
                    }
                }
            }
        }
#endif
    }

    if (shouldLoadBinary && !allowSourceFallback) {
        std::cerr << "[AnimationSerializer] Missing binary animation asset: " << path << "\n";
        return AnimationAsset{};
    }

    // Runtime: binary-only loading
#ifdef CLAYMORE_RUNTIME
    std::cerr << "[AnimationSerializer] Failed to load animation asset (binary not found): " << path << "\n";
    return AnimationAsset{};
#endif

    // Editor: fallback to JSON loading
    std::string text;
    std::filesystem::path openedPath;
    if (!OpenAnimationJson(path, text, openedPath)) {
        std::cerr << "[AnimationSerializer] Failed to open animation asset: " << path << "\n";
        return AnimationAsset{};
    }

    try {
        json j = json::parse(text);
        asset = DeserializeAnimationAsset(j);
    } catch (const std::exception& e) {
        std::cerr << "[Animation] Parse error '" << path << "': " << e.what() << "\n";
        return AnimationAsset{};
    }

#ifdef CLAYMORE_EDITOR
    // In editor authoring mode, keep the cache fresh after source loads. Play mode
    // consumes prepared binaries only; rebuilding here creates runtime stalls.
    if (Assets::GetLoadMode() == AssetLoadMode::Editor &&
        !FileSystem::Instance().IsPakMounted() &&
        !asset.tracks.empty()) {
        std::string binaryPath = BinaryAssetCache::Instance().GetBinaryPath(path);
        if (!binaryPath.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(binaryPath).parent_path(), ec);
            if (WriteCompiledAnimation(asset, std::filesystem::path(binaryPath))) {
                std::cout << "[AnimationSerializer] Compiled animation binary: " << binaryPath << std::endl;
            }
        }
    }
#endif

    FinalizeLoadedAnimationAsset(asset, openedPath.empty() ? finalizePath : openedPath);
    return asset;
}

AnimationAsset WrapLegacyClipAsAsset(const AnimationClip& clip)
{
    AnimationAsset a; a.name = clip.Name; a.meta.version = 1; a.meta.fps = (clip.TicksPerSecond > 0.0f) ? clip.TicksPerSecond : 30.0f; a.meta.length = clip.Duration;
    for (const auto& kv : clip.BoneTracks) {
        const std::string& boneName = kv.first;
        const BoneTrack& bt = kv.second;
        auto t = std::make_unique<AssetBoneTrack>();
        t->name = boneName;
        for (const auto& k : bt.PositionKeys) t->t.keys.push_back({0ull, k.Time, k.Value});
        for (const auto& k : bt.RotationKeys) t->r.keys.push_back({0ull, k.Time, k.Value});
        for (const auto& k : bt.ScaleKeys)    t->s.keys.push_back({0ull, k.Time, k.Value});
        a.tracks.push_back(std::move(t));
    }
    // Include humanoid tracks by mapping to AvatarTrack if present in legacy clip
    if (!clip.HumanoidTracks.empty()) {
        for (const auto& hv : clip.HumanoidTracks) {
            int humanId = hv.first;
            const BoneTrack& bt = hv.second;
            auto t = std::make_unique<AssetAvatarTrack>();
            t->humanBoneId = humanId;
            t->name = std::string("Humanoid:") + ToString(static_cast<HumanBone>(humanId));
            for (const auto& k : bt.PositionKeys) t->t.keys.push_back({0ull, k.Time, k.Value});
            for (const auto& k : bt.RotationKeys) t->r.keys.push_back({0ull, k.Time, k.Value});
            for (const auto& k : bt.ScaleKeys)    t->s.keys.push_back({0ull, k.Time, k.Value});
            a.tracks.push_back(std::move(t));
        }
    }
    return a;
}

} // namespace animation
} // namespace cm

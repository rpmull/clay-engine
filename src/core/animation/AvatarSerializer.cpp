#include "core/animation/AvatarSerializer.h"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <fstream>
#include "core/vfs/FileSystem.h"

using json = nlohmann::json;

namespace cm {
namespace animation {

static json SerializeMat4(const glm::mat4& m)
{
    json arr = json::array();
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            arr.push_back(m[c][r]);
        }
    }
    return arr;
}

static void DeserializeMat4(const json& j, glm::mat4& out)
{
    out = glm::mat4(1.0f);
    if (!j.is_array() || j.size() != 16) return;
    size_t idx = 0;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out[c][r] = j[idx++].get<float>();
        }
    }
}

static json SerializeQuat(const glm::quat& q)
{
    return json::array({ q.w, q.x, q.y, q.z });
}

static void DeserializeQuat(const json& j, glm::quat& out)
{
    out = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (!j.is_array() || j.size() != 4) return;
    out = glm::quat(j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>());
}

template <typename T, typename SerializeFn>
static json SerializeArray(const std::vector<T>& values, SerializeFn&& serialize)
{
    json arr = json::array();
    for (const auto& value : values) {
        arr.push_back(serialize(value));
    }
    return arr;
}

static void to_json(json& j, const AvatarDefinition& a)
{
    j["rig"] = a.RigName;
    j["unitsPerMeter"] = a.UnitsPerMeter;
    j["axes"] = {
        {"up", (int)a.Axes.Up},
        {"forward", (int)a.Axes.Forward},
        {"rightHanded", a.Axes.RightHanded}
    };
    // Map
    json map = json::array();
    for (uint16_t i = 0; i < HumanoidBoneCount; ++i) {
        const auto& e = a.Map[i];
        json entry;
        entry["bone"] = (int)i;
        entry["name"] = e.BoneName;
        entry["index"] = e.BoneIndex;
        entry["present"] = a.Present[i];
        map.push_back(entry);
    }
    j["map"] = std::move(map);
    j["bindModel"] = SerializeArray(a.BindModel, SerializeMat4);
    j["bindLocal"] = SerializeArray(a.BindLocal, SerializeMat4);
    j["retargetModel"] = SerializeArray(a.RetargetModel, SerializeMat4);
    j["restOffsetRot"] = SerializeArray(a.RestOffsetRot, SerializeQuat);
}

static void from_json(const json& j, AvatarDefinition& a)
{
    a = AvatarDefinition{}; // reset sizes
    a.RigName = j.value("rig", "");
    a.UnitsPerMeter = j.value("unitsPerMeter", 1.0f);
    if (j.contains("axes")) {
        a.Axes.Up = (AvatarAxes::Axis)j["axes"].value("up", (int)AvatarAxes::Axis::Y);
        a.Axes.Forward = (AvatarAxes::Axis)j["axes"].value("forward", (int)AvatarAxes::Axis::Z);
        a.Axes.RightHanded = j["axes"].value("rightHanded", true);
    }
    if (j.contains("map") && j["map"].is_array()) {
        for (const auto& entry : j["map"]) {
            uint16_t bi = (uint16_t)entry.value("bone", 0);
            if (bi >= HumanoidBoneCount) continue;
            a.Map[bi].Bone = (HumanoidBone)bi;
            a.Map[bi].BoneName = entry.value("name", "");
            a.Map[bi].BoneIndex = entry.value("index", -1);
            a.Present[bi] = entry.contains("present")
                ? entry.value("present", false)
                : (a.Map[bi].BoneIndex >= 0);
        }
    }
    if (j.contains("bindModel") && j["bindModel"].is_array()) {
        const auto& mats = j["bindModel"];
        a.BindModel.resize(std::max<size_t>(a.BindModel.size(), mats.size()), glm::mat4(1.0f));
        for (size_t i = 0; i < mats.size() && i < a.BindModel.size(); ++i) {
            DeserializeMat4(mats[i], a.BindModel[i]);
        }
    }
    if (j.contains("bindLocal") && j["bindLocal"].is_array()) {
        const auto& mats = j["bindLocal"];
        a.BindLocal.resize(std::max<size_t>(a.BindLocal.size(), mats.size()), glm::mat4(1.0f));
        for (size_t i = 0; i < mats.size() && i < a.BindLocal.size(); ++i) {
            DeserializeMat4(mats[i], a.BindLocal[i]);
        }
    }
    if (j.contains("retargetModel") && j["retargetModel"].is_array()) {
        const auto& mats = j["retargetModel"];
        a.RetargetModel.resize(std::max<size_t>(a.RetargetModel.size(), mats.size()), glm::mat4(1.0f));
        for (size_t i = 0; i < mats.size() && i < a.RetargetModel.size(); ++i) {
            DeserializeMat4(mats[i], a.RetargetModel[i]);
        }
    }
    if (j.contains("restOffsetRot") && j["restOffsetRot"].is_array()) {
        const auto& quats = j["restOffsetRot"];
        a.RestOffsetRot.resize(std::max<size_t>(a.RestOffsetRot.size(), quats.size()), glm::quat(1, 0, 0, 0));
        for (size_t i = 0; i < quats.size() && i < a.RestOffsetRot.size(); ++i) {
            DeserializeQuat(quats[i], a.RestOffsetRot[i]);
        }
    }
}

bool SaveAvatar(const AvatarDefinition& avatar, const std::string& path)
{
    json j; to_json(j, avatar);
    std::ofstream out(path);
    if (!out) return false;
    out << j.dump(4);
    return true;
}

bool LoadAvatar(AvatarDefinition& avatar, const std::string& path)
{
    std::string text;
    if (!FileSystem::Instance().ReadTextFile(path, text)) {
        return false;
    }
    try {
        json j = json::parse(text);
        from_json(j, avatar);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace animation
} // namespace cm



#include "editor/pipeline/AnimationImportSettings.h"
#include <fstream>
#include <iostream>
#include <filesystem>

using json = nlohmann::json;

// ---------------------- AnimationImportSettings ----------------------

json AnimationImportSettings::ToJson() const
{
    json j;
    
    // Rotation correction
    switch (XAxisCorrection) {
        case RotationCorrection::None:           j["xAxisCorrection"] = "none"; break;
        case RotationCorrection::RotateX_Plus90: j["xAxisCorrection"] = "plus90"; break;
        case RotationCorrection::RotateX_Minus90:j["xAxisCorrection"] = "minus90"; break;
    }
    
    // New root motion settings (v2)
    j["rootMotionMode"] = static_cast<int>(RootMotionMode);
    j["rootMotionIncludeXZ"] = RootMotionIncludeXZ;
    j["rootMotionIncludeY"] = RootMotionIncludeY;
    j["rootMotionIncludeRotation"] = RootMotionIncludeRotation;
    j["rootMotionOverrideGravity"] = RootMotionOverrideGravity;
    
    // Legacy root motion settings (for backward compatibility)
    j["extractXZRootMotion"] = ExtractXZRootMotion;
    j["extractYRootMotion"] = ExtractYRootMotion;
    
    if (!RootMotionBoneName.empty())
        j["rootMotionBone"] = RootMotionBoneName;
    
    // Source tracking for reimport
    if (!SourceFilePath.empty())
        j["sourceFile"] = SourceFilePath;
    if (!SourceRigModelPath.empty())
        j["sourceRigModel"] = SourceRigModelPath;
    if (!SourceAvatarModelPath.empty())
        j["sourceAvatarModel"] = SourceAvatarModelPath;
    
    j["sourceAnimIndex"] = SourceAnimationIndex;
    j["useHipsAsRoot"] = UseHipsAsRoot;
    
    return j;
}

AnimationImportSettings AnimationImportSettings::FromJson(const json& j)
{
    AnimationImportSettings settings;
    
    // Rotation correction
    std::string correction = j.value("xAxisCorrection", "none");
    if (correction == "plus90")
        settings.XAxisCorrection = RotationCorrection::RotateX_Plus90;
    else if (correction == "minus90")
        settings.XAxisCorrection = RotationCorrection::RotateX_Minus90;
    else
        settings.XAxisCorrection = RotationCorrection::None;
    
    // New root motion settings (v2)
    if (j.contains("rootMotionMode")) {
        settings.RootMotionMode = static_cast<cm::animation::RootMotionMode>(j.value("rootMotionMode", 1)); // Default InPlace
        settings.RootMotionIncludeXZ = j.value("rootMotionIncludeXZ", true);
        settings.RootMotionIncludeY = j.value("rootMotionIncludeY", true);
        settings.RootMotionIncludeRotation = j.value("rootMotionIncludeRotation", false);
        settings.RootMotionOverrideGravity = j.value("rootMotionOverrideGravity", false);
    } else {
        // Migrate from legacy settings
        settings.ExtractXZRootMotion = j.value("extractXZRootMotion", false);
        settings.ExtractYRootMotion = j.value("extractYRootMotion", false);
        
        // If legacy extraction was enabled, upgrade to ApplyToEntity mode
        if (settings.ExtractXZRootMotion || settings.ExtractYRootMotion) {
            settings.RootMotionMode = cm::animation::RootMotionMode::ApplyToEntity;
            settings.RootMotionIncludeXZ = settings.ExtractXZRootMotion;
            settings.RootMotionIncludeY = settings.ExtractYRootMotion;
        }
    }
    
    // Legacy settings (kept for backward compatibility reads)
    settings.ExtractXZRootMotion = j.value("extractXZRootMotion", false);
    settings.ExtractYRootMotion = j.value("extractYRootMotion", false);
    settings.RootMotionBoneName = j.value("rootMotionBone", std::string());
    
    // Source tracking
    settings.SourceFilePath = j.value("sourceFile", std::string());
    settings.SourceRigModelPath = j.value("sourceRigModel", std::string());
    settings.SourceAvatarModelPath = j.value("sourceAvatarModel", std::string());
    settings.SourceAnimationIndex = j.value("sourceAnimIndex", 0);
    settings.UseHipsAsRoot = j.value("useHipsAsRoot", false);
    
    return settings;
}

std::string AnimationImportSettings::GetMetaPath(const std::string& animPath)
{
    return animPath + ".meta";
}

bool AnimationImportSettings::LoadFromMeta(const std::string& animPath, AnimationImportSettings& out)
{
    std::string metaPath = GetMetaPath(animPath);
    
    try
    {
        std::ifstream in(metaPath);
        if (!in.is_open())
        {
            // No meta file exists yet - use defaults
            out = AnimationImportSettings{};
            return true;
        }
        
        json j;
        in >> j;
        in.close();
        
        if (j.contains("importSettings"))
        {
            out = FromJson(j["importSettings"]);
            return true;
        }
        
        // Meta exists but no import settings section - use defaults
        out = AnimationImportSettings{};
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[AnimationImportSettings] Failed to load from " << metaPath << ": " << e.what() << std::endl;
        return false;
    }
}

bool AnimationImportSettings::SaveToMeta(const std::string& animPath, const AnimationImportSettings& settings)
{
    std::string metaPath = GetMetaPath(animPath);
    
    try
    {
        // Read existing meta (if any)
        json j;
        {
            std::ifstream in(metaPath);
            if (in.is_open())
            {
                in >> j;
                in.close();
            }
        }
        
        // Update import settings section
        j["importSettings"] = settings.ToJson();
        
        // Write back
        std::ofstream out(metaPath);
        if (!out.is_open())
        {
            std::cerr << "[AnimationImportSettings] Failed to open " << metaPath << " for writing" << std::endl;
            return false;
        }
        
        out << j.dump(4);
        out.close();
        
        std::cout << "[AnimationImportSettings] Saved import settings to " << metaPath << std::endl;
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[AnimationImportSettings] Failed to save to " << metaPath << ": " << e.what() << std::endl;
        return false;
    }
}

// ---------------------- AnimationRootMotion ----------------------

json AnimationRootMotion::ToJson() const
{
    json j;
    
    json keysArray = json::array();
    for (const auto& key : Keys)
    {
        keysArray.push_back({
            {"t", key.time},
            {"dx", key.deltaX},
            {"dy", key.deltaY},
            {"dz", key.deltaZ}
        });
    }
    j["keys"] = keysArray;
    
    j["totalDistanceXZ"] = TotalDistanceXZ;
    j["totalDistanceY"] = TotalDistanceY;
    
    return j;
}

AnimationRootMotion AnimationRootMotion::FromJson(const json& j)
{
    AnimationRootMotion rm;
    
    if (j.contains("keys") && j["keys"].is_array())
    {
        for (const auto& keyJ : j["keys"])
        {
            RootKey key;
            key.time = keyJ.value("t", 0.0f);
            key.deltaX = keyJ.value("dx", 0.0f);
            key.deltaY = keyJ.value("dy", 0.0f);
            key.deltaZ = keyJ.value("dz", 0.0f);
            rm.Keys.push_back(key);
        }
    }
    
    rm.TotalDistanceXZ = j.value("totalDistanceXZ", 0.0f);
    rm.TotalDistanceY = j.value("totalDistanceY", 0.0f);
    
    return rm;
}


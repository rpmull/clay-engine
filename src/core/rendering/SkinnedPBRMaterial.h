#pragma once
#include "PBRMaterial.h"

class SkinnedPBRMaterial : public PBRMaterial
{
public:
    SkinnedPBRMaterial(const std::string& name, bgfx::ProgramHandle program);
    
    virtual ~SkinnedPBRMaterial() = default;

    // Create a deep copy with fresh bgfx handles
    std::shared_ptr<Material> Clone() const override;

    void BindUniforms() const override;
};

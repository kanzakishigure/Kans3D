#pragma once
#include "VfxAsset.h"
namespace Kans
{
    struct ParticleSystemComponent
    {
        AssetID                 System;
        bool                    AutoActivate = true, PreviewInEditMode = true;
        std::optional<uint32_t> SeedOverride;
        Vfx::ParameterMap       UserParameters;
        std::optional<Bounds>   FixedBounds;
        float                   TimeScale = 1;
        bool                    operator==(const ParticleSystemComponent& b) const
        {
            return System == b.System && AutoActivate == b.AutoActivate && PreviewInEditMode == b.PreviewInEditMode &&
                   SeedOverride == b.SeedOverride && UserParameters == b.UserParameters && TimeScale == b.TimeScale &&
                   FixedBounds.has_value() == b.FixedBounds.has_value() &&
                   (!FixedBounds || (FixedBounds->Min == b.FixedBounds->Min && FixedBounds->Max == b.FixedBounds->Max));
        }
        bool operator!=(const ParticleSystemComponent& b) const { return !(*this == b); }
    };
    YAML::Node              EncodeParticleSystem(const ParticleSystemComponent&);
    ParticleSystemComponent DecodeParticleSystem(const YAML::Node&);
} // namespace Kans

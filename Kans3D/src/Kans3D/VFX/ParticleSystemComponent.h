#pragma once
#include "VfxAsset.h"
namespace Kans
{
    struct ParticleSystemComponent
    {
        AssetID                 System;
        bool                    AutoActivate = true, PreviewInEditMode = true;
        // Restart once complete; run the system's warmup on activation; a new seed per instance
        // (ignores SeedOverride).
        bool                    Loop = false, Prewarm = true, RandomSeed = false;
        std::optional<uint32_t> SeedOverride;
        Vfx::ParameterMap       UserParameters;
        std::optional<Bounds>   FixedBounds;
        float                   TimeScale     = 1;
        uint32_t                FieldLayer    = 0;
        uint32_t                CollisionMask = UINT32_MAX;
        bool                    operator==(const ParticleSystemComponent& b) const
        {
            return System == b.System && AutoActivate == b.AutoActivate && PreviewInEditMode == b.PreviewInEditMode &&
                   Loop == b.Loop && Prewarm == b.Prewarm && RandomSeed == b.RandomSeed &&
                   SeedOverride == b.SeedOverride && UserParameters == b.UserParameters && TimeScale == b.TimeScale &&
                   FieldLayer == b.FieldLayer && CollisionMask == b.CollisionMask &&
                   FixedBounds.has_value() == b.FixedBounds.has_value() &&
                   (!FixedBounds || (FixedBounds->Min == b.FixedBounds->Min && FixedBounds->Max == b.FixedBounds->Max));
        }
        bool operator!=(const ParticleSystemComponent& b) const { return !(*this == b); }
    };
    YAML::Node              EncodeParticleSystem(const ParticleSystemComponent&);
    ParticleSystemComponent DecodeParticleSystem(const YAML::Node&);
} // namespace Kans

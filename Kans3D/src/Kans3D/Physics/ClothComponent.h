#pragma once
#include <functional>
#include <vector>
#include "Kans3D/Asset/Asset.h"
namespace Kans
{
    // Persistent settings only. Runtime state is never copied or serialized.
    struct ClothSettings
    {
        AssetID           Asset;
        uint32_t          Section     = 0;
        bool              Enabled     = true;
        float             SubstepRate = 120;
        uint32_t          MaxSubsteps = 16, Iterations = 1;
        float             StretchCompliance = 1e-7f, BendCompliance = 1e-4f, TetherScale = 1;
        float             Damping = .02f, VelocityDrag = .1f, ParticleMass = .01f, AreaDensity = 0;
        float             GravityScale = 1, WindScale = 1, Lift = .1f, Drag = 1;
        UUID              Center       = 0;
        float             InertiaScale = 1, LinearInertiaLimit = 10, AngularInertiaLimit = 360;
        float             TeleportDistance = 2, TeleportRotation = 90;
        std::vector<UUID> ColliderGroups;
        float             Thickness    = .005f;
        bool              GroundPlane  = false;
        float             GroundHeight = 0, DisableDistance = 0;
        bool              SimulateWhenNotVisible = false;
        float             MaxDistanceScale       = 1;
    };
    struct ClothRuntime;
    struct ClothComponent : ClothSettings
    {
        Ref<ClothRuntime> Runtime;
        ClothComponent() = default;
        ClothComponent(const ClothComponent& c) : ClothSettings(c) {}
        ClothComponent& operator=(const ClothComponent& c)
        {
            if (this != &c)
            {
                ClothSettings::operator=(c);
                Runtime.reset();
            }
            return *this;
        }
        ClothComponent(ClothComponent&&) noexcept            = default;
        ClothComponent& operator=(ClothComponent&&) noexcept = default;
        void            Reset();
        void            RemapEntities(const std::function<uint64_t(uint64_t)>&);
    };
    bool        operator==(const ClothComponent&, const ClothComponent&);
    inline bool operator!=(const ClothComponent& a, const ClothComponent& b) { return !(a == b); }
} // namespace Kans

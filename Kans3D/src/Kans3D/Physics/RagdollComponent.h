#pragma once
#include "RagdollAsset.h"
namespace Kans
{
    enum class RagdollMode : uint32_t
    {
        Kinematic,
        Simulated,
        Blended
    };
    enum class RagdollRootFollowing : uint32_t
    {
        None,
        FollowPelvis
    };
    struct RagdollBodyOverride
    {
        std::string Bone;
        float       BlendWeight = 1, DriveStrength = -1, DriveDamping = -1;
        bool        operator==(const RagdollBodyOverride& b) const
        {
            return std::tie(Bone, BlendWeight, DriveStrength, DriveDamping) ==
                   std::tie(b.Bone, b.BlendWeight, b.DriveStrength, b.DriveDamping);
        }
    };
    struct RagdollComponent
    {
        AssetID              Asset;
        bool                 Enabled       = true;
        RagdollMode          Mode          = RagdollMode::Kinematic;
        RagdollRootFollowing RootFollowing = RagdollRootFollowing::None;
        bool                 FollowYaw     = false;
        float                DriveStrength = 1, ImpactThreshold = 5;
        float                TeleportDistance = 2, TeleportRotation = 90; // Zero disables the corresponding jump check.
        std::vector<RagdollBodyOverride> Bodies;
        // Optional code binding, like the Animator's SkeletonSource. No SDK state is stored in components.
        Ref<const RagdollAsset> Source;
    };
    inline bool operator==(const RagdollComponent& a, const RagdollComponent& b)
    {
        return std::tie(a.Asset,
                        a.Enabled,
                        a.Mode,
                        a.RootFollowing,
                        a.FollowYaw,
                        a.DriveStrength,
                        a.ImpactThreshold,
                        a.TeleportDistance,
                        a.TeleportRotation,
                        a.Bodies) == std::tie(b.Asset,
                                              b.Enabled,
                                              b.Mode,
                                              b.RootFollowing,
                                              b.FollowYaw,
                                              b.DriveStrength,
                                              b.ImpactThreshold,
                                              b.TeleportDistance,
                                              b.TeleportRotation,
                                              b.Bodies);
    }
    inline bool operator!=(const RagdollComponent& a, const RagdollComponent& b) { return !(a == b); }
    template<class C, class F>
    void RagdollFields(C& c, F f)
    {
        f("Asset", c.Asset);
        f("Enabled", c.Enabled);
        f("Mode", c.Mode);
        f("RootFollowing", c.RootFollowing);
        f("FollowYaw", c.FollowYaw);
        f("DriveStrength", c.DriveStrength);
        f("ImpactThreshold", c.ImpactThreshold);
        f("TeleportDistance", c.TeleportDistance);
        f("TeleportRotation", c.TeleportRotation);
        f("Bodies", c.Bodies);
    }
} // namespace Kans

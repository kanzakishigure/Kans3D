#pragma once
#include "DeformationComponents.h"
namespace Kans
{
    void                 CacheSceneIKInput(const IKComponent&, const AnimationPose&);
    const AnimationPose& SceneIKInput(const IKComponent&, const AnimationPose& fallback);
    struct FootPlacementState
    {
        struct Leg
        {
            bool      Previous = false, Locked = false, ToePivot = false;
            glm::vec3 PreviousHeel {0}, PreviousToe {0}, Pin {0}, AnimatedPin {0}, ReleaseOffset {0};
            glm::vec3 PreviousAnimatedHeel {0}, PreviousAnimatedToe {0};
            glm::quat PinRotation {1, 0, 0, 0};
            float     Release = 0;
        };
        struct Probe
        {
            glm::vec3 Start {0}, End {0}, Hit {0}, Normal {0, 1, 0}, Target {0};
            bool      HasHit = false, Locked = false;
        };
        std::vector<glm::mat4> BindModel;
        std::vector<Leg>       Legs;
        std::vector<Probe>     Probes;
        float                  Pelvis = 0, PelvisVelocity = 0;
    };
    // Reads contact exclusively from the immutable incoming animation pose and original rig targets.
    std::vector<TwoBoneIK> PlaceFeet(const SkeletonData&,
                                     const IKComponent&,
                                     const FootPlacementIK&,
                                     float weight,
                                     AnimationPose&,
                                     const AnimationPose& animated,
                                     const glm::mat4&,
                                     const Animator::IKEntityResolver&,
                                     const IKEvaluationContext&,
                                     FootPlacementState&);
} // namespace Kans

#pragma once
#include "Kans3D/Core/UUID.h"
#include "Animation.h"
namespace Kans
{
    struct SkinningPose;
    // Authoring fields are copied/serialized. Evaluation caches are private to each scene instance.
    struct AnimatorComponent
    {
        AssetID                                          Skeleton, Controller;
        std::optional<AnimationControllerData>           InlineController;
        bool                                             Enabled = true, ApplyRootMotion = false;
        double                                           Speed = 1;
        std::map<std::string, double>                    Parameters;
        Ref<const SkeletonAsset>                         SkeletonSource;
        Ref<const AnimationControllerAsset>              ControllerSource;
        std::map<AssetID, Ref<const AnimationClipAsset>> ClipSources;
        std::map<AssetID, Ref<const ActionAsset>>        ActionSources;
        Ref<Animator>                                    Player;
        Ref<SkinningPose>                                SkinPose;
        std::vector<UUID>                                JointEntities;
        std::vector<AnimationEventOccurrence>            Events; // Current update; references retained clip data.
        AnimationPose                                    PreviewPose;
        AnimationPose                                    SpringPose;
        uint64_t                                         PoseVersion = 0;
        uint64_t                                         LastSpringDiscontinuity = 0;
        std::optional<AssetError>                        LastError;
        AnimatorComponent() = default;
        AnimatorComponent(const AnimatorComponent&);
        AnimatorComponent& operator=(const AnimatorComponent&);
        AnimatorComponent(AnimatorComponent&&) noexcept            = default;
        AnimatorComponent& operator=(AnimatorComponent&&) noexcept = default;
    };
    bool operator==(const AnimatorComponent&, const AnimatorComponent&);
} // namespace Kans

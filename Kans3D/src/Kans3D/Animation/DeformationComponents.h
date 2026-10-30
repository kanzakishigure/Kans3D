#pragma once
#include "Kans3D/Core/UUID.h"
#include "Animation.h"
namespace Kans
{
    struct SkinningPose;
    struct IKConstraint
    {
        std::string                       Name;
        bool                              Enabled = true;
        float                             Weight  = 1;
        std::variant<TwoBoneIK, LookAtIK> Data    = TwoBoneIK {};
    };
    struct IKRuntime;
    struct IKComponent
    {
        std::vector<IKConstraint> Constraints;
        // Optional for standalone models. Otherwise resolved from Animator/the model prefab.
        AssetID                  Skeleton;
        Ref<const SkeletonAsset> SkeletonSource;
        mutable Ref<IKRuntime>   Runtime;
        IKComponent() = default;
        IKComponent(const IKComponent&);
        IKComponent& operator=(const IKComponent&);
        IKComponent(IKComponent&&) noexcept            = default;
        IKComponent& operator=(IKComponent&&) noexcept = default;
        void         ImportLegacy(const AnimationControllerData&);
        void         RemapEntities(const std::function<uint64_t(uint64_t)>&);
    };
    struct ShapeKeysComponent
    {
        std::map<std::string, float> Weights;
        AssetID                      Expression, Skeleton;
        Ref<const ExpressionAsset>   ExpressionSource;
        Ref<const SkeletonAsset>     SkeletonSource;
        bool                         ApplyPreset(const std::string& name, float weight = 1);
    };
    // Per-scene cache for roots without playback. Never serialized or copied.
    struct ModelPoseComponent
    {
        Ref<const SkeletonAsset> SkeletonSource;
        Ref<SkinningPose>        SkinPose;
        AnimationPose            Pose;
        AnimationPose            SpringPose;
        std::vector<UUID>        JointEntities;
        uint64_t                 PoseVersion = 0;
    };
    bool operator==(const IKComponent&, const IKComponent&);
    bool operator==(const ShapeKeysComponent&, const ShapeKeysComponent&);
    void EvaluateIKConstraints(const SkeletonData&,
                               const IKComponent&,
                               AnimationPose&,
                               const glm::mat4& world,
                               const Animator::IKEntityResolver&);
} // namespace Kans

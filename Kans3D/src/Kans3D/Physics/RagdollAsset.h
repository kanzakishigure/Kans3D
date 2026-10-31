#pragma once
#include <yaml-cpp/yaml.h>
#include "Kans3D/Animation/AnimationData.h"
#include "Kans3D/Asset/Runtime/MeshData.h"
#include "Shapes.h"
namespace Kans
{
    using RagdollPrimitive = std::variant<Physics::Sphere, Physics::Capsule, Physics::Box>;
    struct RagdollBody
    {
        std::string                   Bone;
        std::vector<RagdollPrimitive> Shapes;
        float                         Mass = 1, Density = 1000;
        bool                          UseDensity    = false;
        float                         LinearDamping = .05f, AngularDamping = .1f;
        AssetID                       Material;
        std::optional<uint32_t>       Layer;
    };
    struct RagdollConstraint
    {
        std::string Bone;
        // Child-bone frame. Its world bind frame is also used to derive the parent's frame.
        Physics::ShapeOffset Frame;
        // Authored degrees. +X twist, +Y swing1, +Z swing2, matching PhysX articulation axes.
        float TwistLow = -20, TwistHigh = 20, SwingY = 30, SwingZ = 30;
        float LimitStiffness = 0, LimitDamping = 0, DriveStrength = 0, DriveDamping = 0;
    };
    struct RagdollData
    {
        AssetID                                          Skeleton;
        uint64_t                                         SkeletonSignature = 0;
        uint32_t                                         DefaultLayer      = 0;
        std::vector<RagdollBody>                         Bodies;
        std::vector<RagdollConstraint>                   Constraints;
        std::vector<std::pair<std::string, std::string>> DisabledCollisionPairs;
    };
    struct RagdollGenerationOptions
    {
        float MinimumBoneLength = .03f, LeafLengthFactor = .5f, RadiusFraction = .15f, TotalMass = 70;
        std::vector<std::string> SkipBones;
        bool                     PreferBox = false;
        RagdollConstraint        DefaultLimits;
    };
    struct RagdollBinding
    {
        std::vector<uint32_t> Bones;
        std::vector<int32_t>  Parents;
        std::string           Error;
        explicit              operator bool() const { return Error.empty(); }
    };
    class RagdollAsset final : public Asset
    {
    public:
        explicit RagdollAsset(RagdollData data) : m_Data(std::move(data)) {}
        static AssetType   GetStaticType() { return AssetType::Ragdoll; }
        AssetType          GetAssetType() const override { return GetStaticType(); }
        const RagdollData& GetData() const { return m_Data; }

    private:
        RagdollData m_Data;
    };
    uint64_t       RagdollSkeletonSignature(const SkeletonData&);
    float          RagdollVolume(const RagdollPrimitive&);
    RagdollData    GenerateRagdoll(const SkeletonData&,
                                   AssetID         skeleton,
                                   const MeshData* mesh            = nullptr,
                                   const RagdollGenerationOptions& = {});
    RagdollBinding BindRagdoll(const RagdollData&, const SkeletonData&, AssetID skeleton);
    void           ValidateRagdollData(const RagdollData&);
    YAML::Node     EncodeRagdollNode(const RagdollData&);
    RagdollData    DecodeRagdollNode(const YAML::Node&);
} // namespace Kans

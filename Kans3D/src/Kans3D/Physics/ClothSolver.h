#pragma once
#include "Kans3D/Renderer/Resource/MeshKey.h"
#include "Kans3D/Renderer/Resource/Skinning.h"
#include "ClothAsset.h"
#include "ClothComponent.h"
#include "ForceFields.h"
#include "SecondaryMotion.h"
namespace Kans
{
    struct ClothCollider
    {
        UUID                         ID = 0;
        Physics::MotionColliderShape Shape;
        bool                         Inside = false;
        glm::mat4                    Previous {1}, Current {1}, Step {1};
    };
    // Particle arrays and index constraints are independent of Scene and the graphics backend.
    struct ClothRuntime
    {
        Ref<const ClothAsset>      Source;
        Ref<const MeshAsset>       Mesh;
        Physics::SubstepClock      Clock;
        std::vector<glm::vec3>     Positions, Previous, Velocities, PredictedFrom, RenderPositions, Normals;
        std::vector<glm::vec3>     Anchors, AnchorNormals, BasePositions, BaseNormals, OldBasePositions, OldBaseNormals;
        std::vector<float>         InvMass, StretchLambda, BendLambda;
        std::vector<glm::mat4>     Palette, OldPalette, StepPalette;
        std::vector<ClothCollider> Colliders;
        std::vector<uint32_t>      StreamVertices;
        Ref<DynamicVertexStream>   Stream;
        glm::mat4                  Center {1}, OldCenter {1}, Root {1}, OldRoot {1}, SolvedRoot {1};
        glm::vec3                  Gravity {0, -9.81f, 0};
        uint64_t                   ResetEpoch = 0, LastImpulseSequence = 0;
        bool                       Active = false, ResetPending = true, BindingValid = false, Warned = false;
        bool                       SourceOverride = false; // Source stays bound instead of the stored asset.
        uint32_t                   BindingErrors  = 0;
        // Set on the main thread before dispatch. Endpoints are retained even if no substep is due.
        void Initialize(Ref<const ClothAsset>, Ref<const MeshAsset>);
        void SetFrame(const SkinningPose*, const glm::mat4& root, const glm::mat4& center);
        // Current-frame world-space animation anchors for painting/preview, including morphs. Call after
        // SetFrame/update.
        void GetSkinnedAnchors(std::vector<glm::vec3>& output) const;
        void Advance(const ClothSettings&,
                     const ForceFieldSet&,
                     double seconds,
                     bool   reset,
                     bool   paused     = false,
                     bool   singleStep = false);

    private:
        // Exact skin matrices are shared by particles with identical joint/weight tuples. Inverse-transpose
        // is applied AFTER blending, preserving normals under nonuniform scales and blended rotations.
        std::vector<MeshSkinVertex> SkinWeights;
        std::vector<uint32_t>       ParticleSkin;
        std::vector<glm::mat4>      StepSkins, CachedSkinPalette;
        std::vector<glm::mat3>      StepNormalMatrices;
        std::vector<JointTransform> JointPalette, OldJointPalette;
        glm::mat4                   CachedSkinTransform {1};
        glm::mat3                   CachedRestMatrix {1};
        std::vector<float>          StretchRest, BendRest;
        float                       CachedTetherRestScale = 1;
        bool                        SkinCacheValid = false, RestCacheValid = false;
        glm::vec3                   CurrentAnchor(uint32_t particle) const;
        void                        SkinAnchors(float t, const glm::mat4& inverseCenter);
        void                        Output(float alpha);
    };
} // namespace Kans

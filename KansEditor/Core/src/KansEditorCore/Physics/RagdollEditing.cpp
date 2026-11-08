#include "KansEditorCore/Physics/RagdollEditing.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <sstream>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <yaml-cpp/yaml.h>
#include "Kans3D/Animation/AnimatorComponent.h"
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Core/TimeStep.h"
#include "Kans3D/Physics/PhysicsEquality.h"
#include "Kans3D/Physics/PhysicsWorld.h"
#include "Kans3D/Renderer/DebugDraw.h"
#include "Kans3D/Renderer/RenderWorld.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"

namespace Kans::RagdollEditing
{
    // ---------------------------------------------------------------- queries

    namespace
    {
        bool SameBody(const RagdollBody& a, const RagdollBody& b)
        {
            if (std::tie(a.Bone, a.Mass, a.Density, a.UseDensity, a.LinearDamping, a.AngularDamping, a.Material, a.Layer) !=
                std::tie(b.Bone, b.Mass, b.Density, b.UseDensity, b.LinearDamping, b.AngularDamping, b.Material, b.Layer))
                return false;
            if (a.Shapes.size() != b.Shapes.size())
                return false;
            for (size_t i = 0; i < a.Shapes.size(); ++i)
            {
                if (a.Shapes[i].index() != b.Shapes[i].index())
                    return false;
                const bool same = std::visit(
                    [&](const auto& s) {
                        using T = std::decay_t<decltype(s)>;
                        return s == std::get<T>(b.Shapes[i]);
                    },
                    a.Shapes[i]);
                if (!same)
                    return false;
            }
            return true;
        }
        bool SameConstraint(const RagdollConstraint& a, const RagdollConstraint& b)
        {
            return a.Bone == b.Bone && a.Frame == b.Frame &&
                   std::tie(a.TwistLow, a.TwistHigh, a.SwingY, a.SwingZ, a.LimitStiffness, a.LimitDamping,
                            a.DriveStrength, a.DriveDamping) ==
                       std::tie(b.TwistLow, b.TwistHigh, b.SwingY, b.SwingZ, b.LimitStiffness, b.LimitDamping,
                                b.DriveStrength, b.DriveDamping);
        }
    } // namespace

    bool Equal(const RagdollData& a, const RagdollData& b)
    {
        if (a.Skeleton != b.Skeleton || a.SkeletonSignature != b.SkeletonSignature || a.DefaultLayer != b.DefaultLayer ||
            a.Bodies.size() != b.Bodies.size() || a.Constraints.size() != b.Constraints.size() ||
            a.DisabledCollisionPairs != b.DisabledCollisionPairs)
            return false;
        for (size_t i = 0; i < a.Bodies.size(); ++i)
            if (!SameBody(a.Bodies[i], b.Bodies[i]))
                return false;
        for (size_t i = 0; i < a.Constraints.size(); ++i)
            if (!SameConstraint(a.Constraints[i], b.Constraints[i]))
                return false;
        return true;
    }

    int FindBody(const RagdollData& data, const std::string& bone)
    {
        for (size_t i = 0; i < data.Bodies.size(); ++i)
            if (data.Bodies[i].Bone == bone)
                return int(i);
        return -1;
    }

    int FindConstraint(const RagdollData& data, const std::string& bone)
    {
        for (size_t i = 0; i < data.Constraints.size(); ++i)
            if (data.Constraints[i].Bone == bone)
                return int(i);
        return -1;
    }

    std::vector<glm::mat4> BindFrames(const SkeletonData& skeleton)
    {
        std::vector<glm::mat4> model(skeleton.Joints.size()), frames(skeleton.Joints.size());
        for (size_t i = 0; i < skeleton.Joints.size(); ++i)
        {
            const int32_t parent = skeleton.Joints[i].Parent;
            model[i] = (parent >= 0 && parent < int32_t(i) ? model[size_t(parent)] : glm::mat4(1.0f)) *
                       skeleton.Joints[i].BindLocal.Matrix();
            frames[i] = PhysicsEditing::JointFrame(model[i]);
        }
        return frames;
    }

    int OwningBody(const RagdollData& data, const SkeletonData& skeleton, int32_t joint)
    {
        for (int32_t j = joint; j >= 0 && j < int32_t(skeleton.Joints.size()); j = skeleton.Joints[size_t(j)].Parent)
            if (int body = FindBody(data, skeleton.Joints[size_t(j)].Name); body >= 0)
                return body;
        return -1;
    }

    // ---------------------------------------------------------------- generation

    std::variant<RagdollData, std::string> Generate(const SkeletonData&              skeleton,
                                                    AssetID                          skeletonID,
                                                    const MeshData*                  mesh,
                                                    const RagdollGenerationOptions& options)
    {
        try
        {
            return GenerateRagdoll(skeleton, skeletonID, mesh, options);
        }
        catch (const std::exception& e)
        {
            return std::string(e.what());
        }
    }

    std::variant<RagdollData, std::string> Regenerate(const RagdollData&               current,
                                                      const SkeletonData&              skeleton,
                                                      AssetID                          skeletonID,
                                                      const MeshData*                  mesh,
                                                      const RagdollGenerationOptions& options,
                                                      const std::vector<std::string>& bones)
    {
        auto generated = Generate(skeleton, skeletonID, mesh, options);
        if (auto* error = std::get_if<std::string>(&generated))
            return *error;
        const auto& fresh = std::get<RagdollData>(generated);
        RagdollData result;
        if (bones.empty())
            result = fresh;
        else
        {
            result                   = current;
            result.Skeleton          = fresh.Skeleton;
            result.SkeletonSignature = fresh.SkeletonSignature;
            const auto selected      = [&](const std::string& bone) {
                return std::find(bones.begin(), bones.end(), bone) != bones.end();
            };
            // Selected bones take the new body (or lose theirs); the others stay as edited.
            result.Bodies.erase(std::remove_if(result.Bodies.begin(),
                                               result.Bodies.end(),
                                               [&](const RagdollBody& b) { return selected(b.Bone); }),
                                result.Bodies.end());
            result.Constraints.erase(std::remove_if(result.Constraints.begin(),
                                                    result.Constraints.end(),
                                                    [&](const RagdollConstraint& c) { return selected(c.Bone); }),
                                     result.Constraints.end());
            for (const auto& body : fresh.Bodies)
                if (selected(body.Bone))
                    result.Bodies.push_back(body);
            for (const auto& constraint : fresh.Constraints)
                if (selected(constraint.Bone))
                    result.Constraints.push_back(constraint);
            // A kept body whose parent body changed may now need a constraint: take the new one.
            for (const auto& constraint : fresh.Constraints)
                if (FindConstraint(result, constraint.Bone) < 0 && FindBody(result, constraint.Bone) >= 0)
                    result.Constraints.push_back(constraint);
        }
        // Skeleton order, as generation writes them (parents before children).
        const auto order = [&](const std::string& bone) { return skeleton.FindJoint(bone); };
        std::stable_sort(result.Bodies.begin(), result.Bodies.end(), [&](const auto& a, const auto& b) {
            return order(a.Bone) < order(b.Bone);
        });
        std::stable_sort(result.Constraints.begin(), result.Constraints.end(), [&](const auto& a, const auto& b) {
            return order(a.Bone) < order(b.Bone);
        });
        // Constraints only on bodies with a body above them; pairs only between bodies that remain.
        result.Constraints.erase(std::remove_if(result.Constraints.begin(),
                                                result.Constraints.end(),
                                                [&](const RagdollConstraint& c) {
                                                    const int32_t joint = skeleton.FindJoint(c.Bone);
                                                    return FindBody(result, c.Bone) < 0 || joint < 0 ||
                                                           OwningBody(result, skeleton, skeleton.Joints[size_t(joint)].Parent) < 0;
                                                }),
                                 result.Constraints.end());
        result.DisabledCollisionPairs.clear();
        for (const auto& pair : current.DisabledCollisionPairs)
            if (FindBody(result, pair.first) >= 0 && FindBody(result, pair.second) >= 0)
                result.DisabledCollisionPairs.push_back(pair);
        return result;
    }

    // ---------------------------------------------------------------- bodies

    PrimitiveKind KindOf(const RagdollPrimitive& primitive)
    {
        if (std::holds_alternative<Physics::Sphere>(primitive))
            return PrimitiveKind::Sphere;
        return std::holds_alternative<Physics::Capsule>(primitive) ? PrimitiveKind::Capsule : PrimitiveKind::Box;
    }

    const char* KindName(PrimitiveKind kind)
    {
        switch (kind)
        {
            case PrimitiveKind::Sphere:
                return "Sphere";
            case PrimitiveKind::Capsule:
                return "Capsule";
            default:
                return "Box";
        }
    }

    Physics::ShapeOffset OffsetOf(const RagdollPrimitive& primitive)
    {
        return std::visit([](const auto& s) { return s.Offset; }, primitive);
    }

    void SetOffset(RagdollPrimitive& primitive, const Physics::ShapeOffset& offset)
    {
        std::visit([&](auto& s) { s.Offset = offset; }, primitive);
    }

    RagdollPrimitive NewPrimitive(PrimitiveKind kind, float size)
    {
        const float half = std::max(PhysicsEditing::MinimumSize, size * 0.5f);
        switch (kind)
        {
            case PrimitiveKind::Sphere:
                return Physics::Sphere {half, {}};
            case PrimitiveKind::Capsule:
                return Physics::Capsule {half * 0.5f, half * 0.5f, Physics::Axis::Y, {}};
            default:
                return Physics::Box {glm::vec3(half), {}};
        }
    }

    RagdollPrimitive Convert(const RagdollPrimitive& primitive, PrimitiveKind kind)
    {
        // The primitive's half extents along its offset axes.
        glm::vec3 half(0.0f);
        int       axis = 1;
        std::visit(
            [&](const auto& s) {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, Physics::Sphere>)
                    half = glm::vec3(std::abs(s.Radius));
                else if constexpr (std::is_same_v<T, Physics::Capsule>)
                {
                    axis       = int(s.Direction);
                    half       = glm::vec3(std::abs(s.Radius));
                    half[axis] = std::abs(s.HalfHeight) + std::abs(s.Radius);
                }
                else
                {
                    half = glm::abs(s.HalfExtents);
                    axis = half.x >= half.y && half.x >= half.z ? 0 : half.y >= half.z ? 1 : 2;
                }
            },
            primitive);
        const auto offset = OffsetOf(primitive);
        half              = glm::max(half, glm::vec3(PhysicsEditing::MinimumSize));
        switch (kind)
        {
            case PrimitiveKind::Sphere:
                return Physics::Sphere {std::max(half.x, std::max(half.y, half.z)), offset};
            case PrimitiveKind::Capsule: {
                float radius = 0.0f;
                for (int i = 0; i < 3; ++i)
                    if (i != axis)
                        radius = std::max(radius, half[i]);
                return Physics::Capsule {radius, std::max(0.0f, half[axis] - radius), Physics::Axis(axis), offset};
            }
            default:
                return Physics::Box {half, offset};
        }
    }

    bool AddBody(RagdollData& data, const SkeletonData& skeleton, const std::string& bone, const RagdollConstraint& limits)
    {
        const int32_t joint = skeleton.FindJoint(bone);
        if (joint < 0 || FindBody(data, bone) >= 0)
            return false;
        const auto frames = BindFrames(skeleton);
        // Along the bone to its farthest child, or a short stub at a leaf.
        glm::vec3 head(frames[size_t(joint)][3]), tail = head + glm::vec3(frames[size_t(joint)][1]) * 0.1f;
        float     farthest = -1.0f;
        for (size_t j = 0; j < skeleton.Joints.size(); ++j)
            if (skeleton.Joints[j].Parent == joint && glm::distance(head, glm::vec3(frames[j][3])) > farthest)
                farthest = glm::distance(head, glm::vec3(frames[j][3])), tail = glm::vec3(frames[j][3]);
        const glm::mat4 inverse = glm::inverse(frames[size_t(joint)]);
        const glm::vec3 a = glm::vec3(inverse * glm::vec4(head, 1.0f)), b = glm::vec3(inverse * glm::vec4(tail, 1.0f));
        const float     length = std::max(0.02f, glm::length(b - a));
        Physics::Capsule capsule;
        capsule.Radius          = std::max(0.01f, length * 0.15f);
        capsule.HalfHeight      = std::max(0.0f, length * 0.5f - capsule.Radius);
        capsule.Offset.Position = (a + b) * 0.5f;
        if (glm::length(b - a) > 1e-6f)
            capsule.Offset.Rotation = glm::rotation(glm::vec3(0, 1, 0), glm::normalize(b - a));
        RagdollBody body;
        body.Bone   = bone;
        body.Shapes = {capsule};
        data.Bodies.push_back(body);
        std::stable_sort(data.Bodies.begin(), data.Bodies.end(), [&](const auto& x, const auto& y) {
            return skeleton.FindJoint(x.Bone) < skeleton.FindJoint(y.Bone);
        });
        // Joined to the body above it, and bodies below it now join this one (they have constraints).
        if (OwningBody(data, skeleton, skeleton.Joints[size_t(joint)].Parent) >= 0 && FindConstraint(data, bone) < 0)
        {
            RagdollConstraint constraint = limits;
            constraint.Bone              = bone;
            constraint.Frame             = {};
            data.Constraints.push_back(constraint);
        }
        for (size_t j = 0; j < skeleton.Joints.size(); ++j)
            if (int32_t(j) != joint && FindBody(data, skeleton.Joints[j].Name) >= 0 &&
                FindConstraint(data, skeleton.Joints[j].Name) < 0 &&
                OwningBody(data, skeleton, skeleton.Joints[j].Parent) >= 0)
            {
                RagdollConstraint constraint = limits;
                constraint.Bone              = skeleton.Joints[j].Name;
                constraint.Frame             = {};
                data.Constraints.push_back(constraint);
            }
        std::stable_sort(data.Constraints.begin(), data.Constraints.end(), [&](const auto& x, const auto& y) {
            return skeleton.FindJoint(x.Bone) < skeleton.FindJoint(y.Bone);
        });
        return true;
    }

    bool RemoveBody(RagdollData& data, const std::string& bone)
    {
        const int body = FindBody(data, bone);
        if (body < 0)
            return false;
        data.Bodies.erase(data.Bodies.begin() + body);
        if (const int constraint = FindConstraint(data, bone); constraint >= 0)
            data.Constraints.erase(data.Constraints.begin() + constraint);
        auto& pairs = data.DisabledCollisionPairs;
        pairs.erase(std::remove_if(pairs.begin(),
                                   pairs.end(),
                                   [&](const auto& pair) { return pair.first == bone || pair.second == bone; }),
                    pairs.end());
        return true;
    }

    std::vector<PhysicsEditing::ShapeHandle> Handles(const RagdollPrimitive& primitive)
    {
        return std::visit([](const auto& s) { return PhysicsEditing::Handles(s); }, primitive);
    }

    RagdollPrimitive DragHandle(const RagdollPrimitive& start, int id, float distance)
    {
        return std::visit([&](const auto& s) { return RagdollPrimitive(PhysicsEditing::DragHandle(s, id, distance)); },
                          start);
    }

    // ---------------------------------------------------------------- limits

    glm::mat4 ConstraintFrame(const glm::mat4& boneFrame, const RagdollConstraint& constraint)
    {
        return PhysicsEditing::JointFrame(boneFrame * constraint.Frame.Matrix());
    }

    glm::vec3 TwistPoint(const glm::mat4& frame, float degrees, float radius)
    {
        const float angle = glm::radians(degrees);
        return glm::vec3(frame * glm::vec4(0.0f, radius * std::cos(angle), radius * std::sin(angle), 1.0f));
    }

    glm::vec3 SwingPoint(const glm::mat4& frame, int axis, float degrees, float radius)
    {
        const glm::vec3 about = axis == 2 ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::vec3 local = glm::angleAxis(glm::radians(degrees), about) * glm::vec3(radius, 0, 0);
        return glm::vec3(frame * glm::vec4(local, 1.0f));
    }

    std::optional<float> TwistAngleAt(const glm::mat4& frame, glm::vec3 origin, glm::vec3 direction)
    {
        return PhysicsEditing::HingeAngleAt(frame, origin, direction);
    }

    std::optional<float> SwingAngleAt(const glm::mat4& frame, int axis, glm::vec3 origin, glm::vec3 direction)
    {
        // The swing about +Y sweeps +X across the XZ plane (normal +Y); about +Z, the XY plane.
        const glm::vec3 normal = glm::normalize(glm::vec3(frame[axis == 2 ? 2 : 1]));
        const glm::vec3 side   = glm::normalize(glm::vec3(frame[axis == 2 ? 1 : 2]));
        const glm::vec3 twist  = glm::normalize(glm::vec3(frame[0]));
        const glm::vec3 centre = glm::vec3(frame[3]);
        const float     facing = glm::dot(direction, normal);
        if (std::abs(facing) < 1e-6f)
            return std::nullopt;
        const float t = glm::dot(centre - origin, normal) / facing;
        if (!(t >= 0.0f))
            return std::nullopt;
        const glm::vec3 offset = origin + direction * t - centre;
        const float     x = glm::dot(offset, twist), s = glm::dot(offset, side);
        if (std::abs(x) < 1e-9f && std::abs(s) < 1e-9f)
            return std::nullopt;
        return glm::degrees(std::atan2(std::abs(s), x));
    }

    void SetTwistLimit(RagdollConstraint& constraint, bool high, float degrees)
    {
        if (!std::isfinite(degrees))
            return;
        degrees = std::clamp(degrees, -MaxTwist, MaxTwist);
        if (high)
            constraint.TwistHigh = std::max(degrees, constraint.TwistLow);
        else
            constraint.TwistLow = std::min(degrees, constraint.TwistHigh);
    }

    void SetSwingLimit(RagdollConstraint& constraint, int axis, float degrees)
    {
        if (!std::isfinite(degrees))
            return;
        (axis == 2 ? constraint.SwingZ : constraint.SwingY) = std::clamp(degrees, MinSwing, MaxSwing);
    }

    // ---------------------------------------------------------------- collision pairs

    bool IsCollisionDisabled(const RagdollData& data, const std::string& a, const std::string& b)
    {
        for (const auto& pair : data.DisabledCollisionPairs)
            if ((pair.first == a && pair.second == b) || (pair.first == b && pair.second == a))
                return true;
        return false;
    }

    bool DisableCollision(RagdollData& data, const std::string& a, const std::string& b)
    {
        if (a == b || FindBody(data, a) < 0 || FindBody(data, b) < 0 || IsCollisionDisabled(data, a, b))
            return false;
        data.DisabledCollisionPairs.emplace_back(a, b);
        return true;
    }

    bool EnableCollision(RagdollData& data, const std::string& a, const std::string& b)
    {
        auto&      pairs  = data.DisabledCollisionPairs;
        const auto before = pairs.size();
        pairs.erase(std::remove_if(pairs.begin(),
                                   pairs.end(),
                                   [&](const auto& p) {
                                       return (p.first == a && p.second == b) || (p.first == b && p.second == a);
                                   }),
                    pairs.end());
        return pairs.size() != before;
    }

    // ---------------------------------------------------------------- validation

    std::string Problem(const RagdollData& data, const SkeletonData* skeleton, AssetID skeletonID)
    {
        if (skeleton)
            return BindRagdoll(data, *skeleton, skeletonID).Error;
        try
        {
            ValidateRagdollData(data);
        }
        catch (const std::exception& e)
        {
            return e.what();
        }
        return {};
    }

    // ---------------------------------------------------------------- drawing

    void DrawBody(DebugDraw& draw, const glm::mat4& boneFrame, const RagdollBody& body, glm::vec4 color)
    {
        for (const auto& shape : body.Shapes)
            std::visit([&](const auto& s) { draw.Shape(Physics::Shape(s), boneFrame, color); }, shape);
    }

    void DrawLimits(DebugDraw& draw, const glm::mat4& frame, const RagdollConstraint& c, float radius)
    {
        const glm::vec3 origin = glm::vec3(frame[3]);
        draw.Arrow(origin, glm::vec3(frame * glm::vec4(radius * 1.3f, 0, 0, 1)), {1, 0.3f, 0.3f, 1});
        // Twist: the arc between the limits about +X.
        for (int n = 0; n < 24; ++n)
            draw.Line(TwistPoint(frame, glm::mix(c.TwistLow, c.TwistHigh, float(n) / 24), radius * 0.6f),
                      TwistPoint(frame, glm::mix(c.TwistLow, c.TwistHigh, float(n + 1) / 24), radius * 0.6f),
                      LimitColor);
        draw.Line(origin, TwistPoint(frame, c.TwistLow, radius * 0.6f), LimitColor);
        draw.Line(origin, TwistPoint(frame, c.TwistHigh, radius * 0.6f), LimitColor);
        // Swing: the elliptical cone the twist axis may reach (Swing Y about +Y, Swing Z about +Z).
        const float y = glm::radians(c.SwingY), z = glm::radians(c.SwingZ);
        const auto  point = [&](float t) {
            const glm::quat swing = glm::angleAxis(y * std::sin(t), glm::vec3(0, 1, 0)) *
                                   glm::angleAxis(z * std::cos(t), glm::vec3(0, 0, 1));
            return glm::vec3(frame * glm::vec4(swing * glm::vec3(radius, 0, 0), 1.0f));
        };
        const glm::vec4 cone {0.3f, 0.9f, 1.0f, 1.0f};
        for (int n = 0; n < 32; ++n)
        {
            const float t = n * glm::two_pi<float>() / 32;
            draw.Line(point(t), point((n + 1) * glm::two_pi<float>() / 32), cone);
            if (n % 8 == 0)
                draw.Line(origin, point(t), cone);
        }
    }

    SceneRagdollCounts DrawSceneRagdolls(DebugDraw&                      draw,
                                         const Scene&                    scene,
                                         PhysicsDebugFlags               flags,
                                         const std::unordered_set<UUID>& hidden)
    {
        SceneRagdollCounts counts;
        if (!(uint32_t(flags) & uint32_t(PhysicsDebugFlags::Ragdolls)) || scene.GetPhysicsWorld())
            return counts;
        AssetManager*               assets = scene.GetAssetManager();
        std::function<void(Entity)> visit  = [&](Entity e) {
            if (hidden.count(e.GetUUID()))
                return;
            for (Entity child : scene.GetChildren(e))
                visit(child);
            if (!e.HasComponent<RagdollComponent>() || !e.HasComponent<AnimatorComponent>())
                return;
            const auto& c = e.GetComponent<RagdollComponent>();
            const auto& a = e.GetComponent<AnimatorComponent>();
            if (!c.Enabled || !a.SkeletonSource)
                return;
            // The asset as the runtime would take it: the loaded asset, else the code-bound source.
            Ref<const RagdollAsset> asset;
            if (c.Asset.IsValid() && assets)
                asset = assets->Request<RagdollAsset>(c.Asset).Asset;
            if (!asset)
                asset = c.Source;
            if (!asset)
                return;
            const auto&    data     = asset->GetData();
            const auto&    skeleton = a.SkeletonSource->GetData();
            RagdollBinding binding;
            try
            {
                binding = BindRagdoll(data, skeleton, a.Skeleton);
            }
            catch (const std::exception&)
            {
                return;
            }
            if (!binding || binding.Bones.size() != data.Bodies.size())
                return;
            // The animated model pose when the Animator has evaluated one, else the bind pose.
            const bool             animated = a.PreviewPose.Model.size() == skeleton.Joints.size();
            std::vector<glm::mat4> bind;
            if (!animated)
            {
                bind.resize(skeleton.Joints.size());
                for (size_t i = 0; i < skeleton.Joints.size(); ++i)
                {
                    const int32_t parent = skeleton.Joints[i].Parent;
                    bind[i] = (parent >= 0 && parent < int32_t(i) ? bind[size_t(parent)] : glm::mat4(1.0f)) *
                              skeleton.Joints[i].BindLocal.Matrix();
                }
            }
            const glm::mat4 world = scene.GetWorldTransform(e);
            for (size_t j = 0; j < data.Bodies.size(); ++j)
            {
                const uint32_t bone = binding.Bones[j];
                if (bone >= skeleton.Joints.size() ||
                    (bone < a.JointEntities.size() && hidden.count(a.JointEntities[bone])))
                    continue;
                // The body's frame as the runtime places its actor: position and rotation only.
                const glm::mat4 frame = PhysicsEditing::JointFrame(world * (animated ? a.PreviewPose.Model[bone] : bind[bone]));
                DrawBody(draw, frame, data.Bodies[j], BodyColor);
                ++counts.Bodies;
                if (binding.Parents[j] < 0)
                    continue;
                const int constraint = FindConstraint(data, data.Bodies[j].Bone);
                if (constraint < 0)
                    continue;
                const auto&     limits = data.Constraints[size_t(constraint)];
                const glm::mat4 joint  = ConstraintFrame(frame, limits);
                const glm::vec3 origin = glm::vec3(joint[3]);
                // The frame's swing axes (DrawLimits draws the twist axis).
                draw.Line(origin, glm::vec3(joint * glm::vec4(0, SceneLimitRadius * 0.7f, 0, 1)), {0.3f, 1.0f, 0.3f, 1.0f});
                draw.Line(origin, glm::vec3(joint * glm::vec4(0, 0, SceneLimitRadius * 0.7f, 1)), {0.3f, 0.5f, 1.0f, 1.0f});
                DrawLimits(draw, joint, limits, SceneLimitRadius);
                ++counts.Joints;
            }
        };
        for (Entity root : scene.GetRootEntities())
            visit(root);
        return counts;
    }

    // ---------------------------------------------------------------- files

    std::variant<RagdollData, AssetError> LoadRagdollFile(const std::filesystem::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return AssetError {AssetErrorCode::IoError, "Cannot read " + file.u8string()};
        std::stringstream text;
        text << in.rdbuf();
        try
        {
            return DecodeRagdollNode(YAML::Load(text.str()));
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, file.filename().u8string() + ": " + e.what()};
        }
    }

    std::optional<AssetError> SaveRagdollFile(const std::filesystem::path& file, const RagdollData& data)
    {
        std::string text;
        try
        {
            YAML::Emitter out;
            out << EncodeRagdollNode(data);
            text = std::string(out.c_str()) + "\n";
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidArgument, e.what()};
        }
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        if (!(stream << text) || !stream.flush())
            return AssetError {AssetErrorCode::IoError, "Cannot write " + file.u8string()};
        return std::nullopt;
    }

    std::variant<std::filesystem::path, AssetError> CreateRagdollFile(const std::filesystem::path& directory,
                                                                      const std::string&           stem,
                                                                      const RagdollData&           data)
    {
        const std::string name = stem.empty() ? "Ragdoll" : stem;
        auto              file = directory / std::filesystem::u8path(name + ".ragdoll");
        for (int i = 2; std::filesystem::exists(file); ++i)
            file = directory / std::filesystem::u8path(name + std::to_string(i) + ".ragdoll");
        if (auto error = SaveRagdollFile(file, data))
            return *error;
        return file;
    }

    // ---------------------------------------------------------------- session

    void RagdollSession::Begin(std::filesystem::path file, RagdollData data)
    {
        m_Open      = true;
        m_File      = std::move(file);
        m_Data      = std::move(data);
        m_Committed = m_Data;
        m_Undo.clear();
        m_Redo.clear();
        m_SavedStep = m_NextStep = 0;
        ++m_Version;
    }

    void RagdollSession::Close()
    {
        m_Open = false;
        m_File.clear();
        m_Data = m_Committed = {};
        m_Undo.clear();
        m_Redo.clear();
        m_SavedStep = m_NextStep = 0;
        ++m_Version;
    }

    void RagdollSession::Change(const RagdollData& data)
    {
        if (!m_Open || Equal(data, m_Data))
            return;
        m_Data = data;
        ++m_Version;
    }

    bool RagdollSession::Commit(const std::string& label)
    {
        if (!m_Open || Equal(m_Data, m_Committed))
            return false;
        m_Undo.push_back({m_Committed, m_Data, label, ++m_NextStep});
        m_Redo.clear();
        m_Committed = m_Data;
        return true;
    }

    bool RagdollSession::Apply(const std::string& label, const std::function<bool(RagdollData&)>& edit)
    {
        if (!m_Open)
            return false;
        Commit();
        RagdollData copy = m_Data;
        if (!edit(copy) || Equal(copy, m_Data))
            return false;
        Change(copy);
        return Commit(label);
    }

    bool RagdollSession::Undo()
    {
        Commit();
        if (m_Undo.empty())
            return false;
        Step step = std::move(m_Undo.back());
        m_Undo.pop_back();
        m_Data = m_Committed = step.Before;
        m_Redo.push_back(std::move(step));
        ++m_Version;
        return true;
    }

    bool RagdollSession::Redo()
    {
        Commit();
        if (m_Redo.empty())
            return false;
        Step step = std::move(m_Redo.back());
        m_Redo.pop_back();
        m_Data = m_Committed = step.After;
        m_Undo.push_back(std::move(step));
        ++m_Version;
        return true;
    }

    std::vector<std::string> RagdollSession::UndoLabels() const
    {
        std::vector<std::string> labels;
        for (auto it = m_Undo.rbegin(); it != m_Undo.rend(); ++it)
            labels.push_back(it->Label);
        return labels;
    }

    std::optional<AssetError> RagdollSession::Save()
    {
        if (!m_Open)
            return AssetError {AssetErrorCode::InvalidArgument, "No ragdoll is open"};
        Commit();
        if (auto error = SaveRagdollFile(m_File, m_Data))
            return error;
        m_SavedStep = CurrentStep();
        return std::nullopt;
    }

    void RagdollSession::RequestClose(std::function<void()> closed)
    {
        m_Closing.Request(m_Open && IsDirty(), "closing " + m_File.filename().u8string(), [this, closed] {
            Close();
            if (closed)
                closed();
        });
    }

    void RagdollSession::ResolveClose(UnsavedChanges::Choice choice, const std::function<bool()>& save)
    {
        m_Closing.Resolve(choice, [&] {
            const bool saved = save ? save() : !Save().has_value();
            return saved ? UnsavedChanges::SaveResult::Saved : UnsavedChanges::SaveResult::Failed;
        });
    }

    // ---------------------------------------------------------------- create ragdoll

    std::variant<CreateSource, std::string> SourceFromImport(const std::vector<ProductRecord>& products,
                                                             SourceAssetID                     import,
                                                             AssetID                           skeleton,
                                                             AssetManager&                     assets,
                                                             const std::string&                target)
    {
        CreateSource result;
        result.Source.Skeleton = skeleton;
        std::vector<AssetID> meshes;
        for (const auto& product : products)
        {
            if (product.Metadata.assetID.GetSourceID() != import || product.Target != target)
                continue;
            if (product.Metadata.Type == AssetType::Skeleton && !skeleton.IsValid() && !result.Source.Skeleton.IsValid())
                result.Source.Skeleton = product.Metadata.assetID;
            else if (product.Metadata.Type == AssetType::Model && !result.Source.Model.IsValid())
                result.Source.Model = product.Metadata.assetID;
            else if (product.Metadata.Type == MeshAsset::GetStaticType())
                meshes.push_back(product.Metadata.assetID);
        }
        if (!result.Source.Skeleton.IsValid())
            return std::string("The import has no skeleton");
        auto loaded = assets.Load<SkeletonAsset>(result.Source.Skeleton);
        if (const auto* error = std::get_if<AssetError>(&loaded))
            return error->Message;
        result.Source.SkeletonSource = std::get<Ref<const SkeletonAsset>>(loaded);
        // Products are listed in no particular order: the lowest local ID is the import's first mesh.
        std::sort(meshes.begin(), meshes.end(), [](AssetID a, AssetID b) { return a.GetLocalID() < b.GetLocalID(); });
        for (const AssetID id : meshes)
            if (auto mesh = assets.Load<MeshAsset>(id); std::holds_alternative<Ref<const MeshAsset>>(mesh))
                if (const auto& m = std::get<Ref<const MeshAsset>>(mesh);
                    !m->GetData().Skin.empty() && m->GetData().Skeleton == result.Source.Skeleton)
                {
                    result.Mesh = m;
                    break;
                }
        return result;
    }

    std::variant<CreateSource, std::string> SourceFromEntity(const Scene&                      scene,
                                                             Entity                            entity,
                                                             AssetManager*                     assets,
                                                             const std::vector<ProductRecord>& products,
                                                             const std::string&                target)
    {
        if (!entity)
            return std::string("No entity is selected");
        const std::string name = entity.GetComponent<TagComponent>().Tag;
        if (!entity.HasComponent<AnimatorComponent>())
            return name + " has no Animator, so there is no skeleton to build a ragdoll on";
        const auto& a = entity.GetComponent<AnimatorComponent>();
        if (!a.Skeleton.IsValid() && !a.SkeletonSource)
            return name + "'s Animator has no skeleton";
        CreateSource result;
        // The import's model and mesh when the skeleton is a product; the entity's own come first.
        if (a.Skeleton.IsValid() && assets)
            if (auto import = SourceFromImport(products, a.Skeleton.GetSourceID(), a.Skeleton, *assets, target);
                std::holds_alternative<CreateSource>(import))
                result = std::get<CreateSource>(std::move(import));
        result.Source.Skeleton = a.Skeleton;
        if (a.SkeletonSource)
            result.Source.SkeletonSource = a.SkeletonSource;
        if (!result.Source.SkeletonSource)
        {
            if (!assets)
                return name + "'s Animator skeleton is not loaded";
            auto loaded = assets->Load<SkeletonAsset>(a.Skeleton);
            if (const auto* error = std::get_if<AssetError>(&loaded))
                return name + "'s Animator skeleton did not load: " + error->Message;
            result.Source.SkeletonSource = std::get<Ref<const SkeletonAsset>>(loaded);
        }
        result.Source.Controller = a.Controller;
        if (entity.HasComponent<PrefabInstanceComponent>())
            result.Source.Model = entity.GetComponent<PrefabInstanceComponent>().Prefab;
        // A skinned mesh under the entity on the same skeleton fits the radii.
        Ref<const MeshAsset>        own;
        std::function<void(Entity)> find = [&](Entity e) {
            if (own)
                return;
            if (auto m = scene.ResolveMesh(e); m && !m->GetData().Skin.empty() && m->GetData().Skeleton == a.Skeleton)
                own = m;
            for (Entity child : scene.GetChildren(e))
                find(child);
        };
        find(entity);
        if (own)
            result.Mesh = own;
        return result;
    }

    // ---------------------------------------------------------------- preview

    RagdollPreview::RagdollPreview() : m_Scene(CreateRef<Scene>("Ragdoll preview")) {}

    RagdollPreview::~RagdollPreview()
    {
        if (m_Scene)
        {
            m_Scene->OnRuntimeStop();
            m_Scene->SetPrefabAssetManager(nullptr);
        }
    }

    bool RagdollPreview::HasSkeleton() const { return bool(m_Source.SkeletonSource); }

    const SkeletonData* RagdollPreview::GetSkeleton() const
    {
        return m_Source.SkeletonSource ? &m_Source.SkeletonSource->GetData() : nullptr;
    }

    void RagdollPreview::SetSource(const PreviewSource& source, AssetManager* assets)
    {
        m_Source = source;
        m_Assets = assets;
        Build();
    }

    void RagdollPreview::Build()
    {
        if (m_Scene)
            m_Scene->OnRuntimeStop();
        m_Simulating = false;
        // A fresh scene each time: nothing of a run survives a reset.
        AssetManager* assets = m_Assets;
        if (m_Scene)
            m_Scene->SetPrefabAssetManager(nullptr);
        auto scene = CreateRef<Scene>("Ragdoll preview");
        if (m_Scene)
        {
            // Keep the render world's look (the viewport client set it up).
            scene->GetRenderWorld().Settings        = m_Scene->GetRenderWorld().Settings;
            scene->GetRenderWorld().EnvironmentPath = m_Scene->GetRenderWorld().EnvironmentPath;
            scene->GetRenderWorld().EnvironmentRoot = m_Scene->GetRenderWorld().EnvironmentRoot;
        }
        m_Scene = scene;
        if (assets)
        {
            m_Scene->SetPrefabAssetManager(assets);
            m_Scene->GetRenderWorld().SetAssetManager(assets);
        }
        m_Bind.clear();
        m_Character = 0;
        if (!m_Source.SkeletonSource)
            return;
        m_Bind = BindFrames(m_Source.SkeletonSource->GetData());
        Entity character = m_Scene->CreateEntity("Character");
        m_Character      = character.GetUUID();
        if (m_Source.Model.IsValid())
            character.AddComponent<PrefabInstanceComponent>().Prefab = m_Source.Model;
        auto& animator          = character.AddComponent<AnimatorComponent>();
        animator.Skeleton       = m_Source.Skeleton;
        animator.SkeletonSource = m_Source.SkeletonSource;
        animator.Controller     = m_Source.Controller;
        // The ground just under the lowest joint, so Simulate drops the ragdoll onto it.
        float lowest = 0.0f;
        for (size_t i = 0; i < m_Bind.size(); ++i)
            lowest = i ? std::min(lowest, m_Bind[i][3].y) : m_Bind[i][3].y;
        m_Ground      = lowest - 0.25f;
        Entity ground = m_Scene->CreateEntity("Ground");
        ground.GetComponent<TransformComponent>().Position = {0.0f, m_Ground - 0.5f, 0.0f};
        ground.AddComponent<RigidBodyComponent>().Type     = RigidBodyType::Static;
        ground.AddComponent<BoxColliderComponent>().Shape.HalfExtents = {50.0f, 0.5f, 50.0f};
    }

    void RagdollPreview::Simulate(const RagdollData& data)
    {
        Build();
        Entity character = m_Scene->GetEntityByUUID(m_Character);
        if (!character)
            return;
        // A copy of the working data: edits made while it runs do not reach it until the next run.
        auto& ragdoll         = character.AddComponent<RagdollComponent>();
        ragdoll.Source        = CreateRef<RagdollAsset>(data);
        ragdoll.Mode          = RagdollMode::Simulated;
        ragdoll.DriveStrength = m_PoseMatching && CanPoseMatch() ? 1.0f : 0.0f;
        SceneRuntimeOptions options;
        options.RunScripts = false;
        m_Scene->OnRuntimeStart(options);
        m_Simulating = true;
    }

    void RagdollPreview::Reset() { Build(); }

    void RagdollPreview::Step(double seconds)
    {
        if (m_Simulating && m_Scene && seconds > 0.0)
            m_Scene->OnUpdate(TimeStep(float(seconds)));
    }

    void RagdollPreview::SetPoseMatching(bool on)
    {
        m_PoseMatching = on;
        if (!m_Simulating)
            return;
        if (Entity character = m_Scene->GetEntityByUUID(m_Character); character && character.HasComponent<RagdollComponent>())
            character.PatchComponent<RagdollComponent>([&](RagdollComponent& c) {
                c.DriveStrength = on && CanPoseMatch() ? 1.0f : 0.0f;
                c.Mode          = on && CanPoseMatch() ? RagdollMode::Blended : RagdollMode::Simulated;
            });
    }

    std::optional<glm::mat4> RagdollPreview::BodyFrame(const std::string& bone) const
    {
        if (!m_Source.SkeletonSource)
            return std::nullopt;
        if (m_Simulating && m_Scene->GetPhysicsWorld())
        {
            if (auto state = m_Scene->GetPhysicsWorld()->GetRagdollBodyState(m_Character, bone))
                return glm::translate(glm::mat4(1.0f), state->Position) * glm::mat4_cast(state->Rotation);
            return std::nullopt;
        }
        const int32_t joint = m_Source.SkeletonSource->GetData().FindJoint(bone);
        if (joint < 0 || size_t(joint) >= m_Bind.size())
            return std::nullopt;
        return m_Bind[size_t(joint)];
    }

    std::vector<std::string> RagdollPreview::GetErrors() const
    {
        if (m_Scene && m_Scene->GetPhysicsWorld())
            return m_Scene->GetPhysicsWorld()->GetErrors();
        return {};
    }
} // namespace Kans::RagdollEditing

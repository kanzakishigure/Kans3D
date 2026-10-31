#include "kspch.h"
#include "ClothSolver.h"
#include "ClothFields.h"
namespace Kans
{
    namespace
    {
        glm::vec3 Unit(glm::vec3 v, glm::vec3 fallback = {0, 1, 0})
        {
            const float d = glm::dot(v, v);
            return d > 1e-16f ? v / std::sqrt(d) : fallback;
        }
        glm::vec3            Vec(const std::array<float, 3>& a) { return {a[0], a[1], a[2]}; }
        std::array<float, 3> Array(glm::vec3 v) { return {v.x, v.y, v.z}; }
        void                 Distance(std::vector<glm::vec3>&   p,
                                      const std::vector<float>& w,
                                      const ClothDistance&      c,
                                      float                     compliance,
                                      float                     dt,
                                      float&                    lambda,
                                      bool                      inequality = false)
        {
            const auto  delta  = p[c.A] - p[c.B];
            const float length = glm::length(delta), sum = w[c.A] + w[c.B];
            if (length < 1e-12f || sum == 0)
                return;
            const float alpha = compliance / (dt * dt), C = length - c.Rest;
            float       dl = (-C - alpha * lambda) / (sum + alpha);
            if (inequality)
            {
                const float next = std::min(0.f, lambda + dl);
                dl               = next - lambda;
                lambda           = next;
            }
            else
                lambda += dl;
            const auto correction = (dl / length) * delta;
            p[c.A] += w[c.A] * correction;
            p[c.B] -= w[c.B] * correction;
        }
    } // namespace
    bool operator==(const ClothComponent& a, const ClothComponent& b)
    {
        return std::tie(a.Asset,
                        a.Section,
                        a.Enabled,
                        a.SubstepRate,
                        a.MaxSubsteps,
                        a.Iterations,
                        a.StretchCompliance,
                        a.BendCompliance,
                        a.TetherScale,
                        a.Damping,
                        a.VelocityDrag,
                        a.ParticleMass,
                        a.AreaDensity,
                        a.GravityScale,
                        a.WindScale,
                        a.Lift,
                        a.Drag,
                        a.Center,
                        a.InertiaScale,
                        a.LinearInertiaLimit,
                        a.AngularInertiaLimit,
                        a.TeleportDistance,
                        a.TeleportRotation,
                        a.ColliderGroups,
                        a.Thickness,
                        a.GroundPlane,
                        a.GroundHeight,
                        a.DisableDistance,
                        a.SimulateWhenNotVisible,
                        a.MaxDistanceScale) == std::tie(b.Asset,
                                                        b.Section,
                                                        b.Enabled,
                                                        b.SubstepRate,
                                                        b.MaxSubsteps,
                                                        b.Iterations,
                                                        b.StretchCompliance,
                                                        b.BendCompliance,
                                                        b.TetherScale,
                                                        b.Damping,
                                                        b.VelocityDrag,
                                                        b.ParticleMass,
                                                        b.AreaDensity,
                                                        b.GravityScale,
                                                        b.WindScale,
                                                        b.Lift,
                                                        b.Drag,
                                                        b.Center,
                                                        b.InertiaScale,
                                                        b.LinearInertiaLimit,
                                                        b.AngularInertiaLimit,
                                                        b.TeleportDistance,
                                                        b.TeleportRotation,
                                                        b.ColliderGroups,
                                                        b.Thickness,
                                                        b.GroundPlane,
                                                        b.GroundHeight,
                                                        b.DisableDistance,
                                                        b.SimulateWhenNotVisible,
                                                        b.MaxDistanceScale);
    }
    void ClothComponent::Reset()
    {
        if (Runtime)
            Runtime->ResetPending = true;
    }
    void ClothComponent::RemapEntities(const std::function<uint64_t(uint64_t)>& f)
    {
        Center = f(Center);
        for (auto& id : ColliderGroups)
            id = f(id);
        Reset();
    }
    void ClothRuntime::Initialize(Ref<const ClothAsset> source, Ref<const MeshAsset> mesh)
    {
        Source        = std::move(source);
        Mesh          = std::move(mesh);
        const auto& d = Source->GetData();
        const auto& m = Mesh->GetData();
        const auto  n = d.RestPositions.size();
        Positions.resize(n);
        Previous.resize(n);
        Velocities.resize(n);
        PredictedFrom.resize(n);
        RenderPositions.resize(n);
        Normals.resize(n);
        Anchors.resize(n);
        AnchorNormals.resize(n);
        BasePositions.resize(n);
        BaseNormals.resize(n);
        OldBasePositions.resize(n);
        OldBaseNormals.resize(n);
        InvMass.resize(n);
        StretchLambda.resize(d.Stretch.size());
        BendLambda.resize(d.Bend.size());
        StretchRest.resize(d.Stretch.size());
        BendRest.resize(d.Bend.size());
        RestCacheValid = SkinCacheValid = false;
        SkinWeights.clear();
        ParticleSkin.assign(n, 0);
        if (!m.Skin.empty())
        {
            using Key = std::pair<std::array<uint32_t, 4>, std::array<float, 4>>;
            std::map<Key, uint32_t> groups;
            for (size_t i = 0; i < n; ++i)
            {
                const auto& weights = m.Skin[d.Representatives[i]];
                const auto [at, added] =
                    groups.emplace(Key {weights.Joints, weights.Weights}, uint32_t(SkinWeights.size()));
                if (added)
                    SkinWeights.push_back(weights);
                ParticleSkin[i] = at->second;
            }
        }
        const auto groups = std::max(size_t(1), SkinWeights.size());
        StepSkins.resize(groups);
        StepNormalMatrices.resize(groups);
        Stream               = CreateRef<DynamicVertexStream>();
        auto& out            = Stream->Data;
        out.MaterialSlots    = m.MaterialSlots;
        out.Material         = m.Material;
        out.AuthoredMaterial = m.AuthoredMaterial;
        Stream->Section      = d.Section;
        out.Sections.resize(1);
        out.Sections[0].MaterialSlot = m.Sections[d.Section].MaterialSlot;
        std::vector<uint32_t> remap(m.Vertices.size(), UINT32_MAX);
        StreamVertices.clear();
        for (uint32_t v = 0; v < d.RenderToSim.size(); ++v)
            if (d.RenderToSim[v] != UINT32_MAX)
            {
                remap[v] = uint32_t(out.Vertices.size());
                out.Vertices.push_back(m.Vertices[v]);
                StreamVertices.push_back(v);
            }
        const auto& s = m.Sections[d.Section];
        for (uint32_t i = s.FirstIndex; i < s.FirstIndex + s.IndexCount; ++i)
            out.Indices.push_back(remap[m.Indices[i]]);
        out.Sections[0].IndexCount = uint32_t(out.Indices.size());
        out.SmoothNormals.resize(out.Vertices.size());
        ResetPending = true;
        Clock.Reset();
    }
    void ClothRuntime::SetFrame(const SkinningPose* pose, const glm::mat4& root, const glm::mat4& center)
    {
        OldCenter       = Center;
        auto reference  = JointTransform::FromMatrix(center);
        reference.Scale = glm::vec3(1);
        Center          = reference.Matrix();
        OldRoot         = Root;
        Root            = root;
        OldPalette      = Palette;
        OldJointPalette = JointPalette;
        if (pose)
        {
            if (Palette != pose->Palette)
            {
                Palette = pose->Palette;
                JointPalette.resize(Palette.size());
                for (size_t j = 0; j < Palette.size(); ++j)
                    JointPalette[j] = JointTransform::FromMatrix(Palette[j]);
            }
        }
        else
        {
            Palette.clear();
            JointPalette.clear();
        }
        StepPalette.resize(Palette.size());
        OldBasePositions = BasePositions;
        OldBaseNormals   = BaseNormals;
        const auto& d    = Source->GetData();
        const auto& m    = Mesh->GetData();
        for (size_t i = 0; i < BasePositions.size(); ++i)
        {
            const auto v     = d.Representatives[i];
            BasePositions[i] = Vec(m.Vertices[v].Position);
            BaseNormals[i]   = Vec(m.Vertices[v].Normal);
        }
        for (const auto& target : m.MorphTargets)
        {
            float weight = target.DefaultWeight;
            if (pose)
                for (size_t k = 0; k < pose->ShapeKeys.size(); ++k)
                    if (pose->ShapeKeys[k].Name == target.Name && k < pose->MorphWeights.size())
                    {
                        weight = pose->MorphWeights[k];
                        break;
                    }
            if (weight == 0)
                continue;
            for (const auto& delta : target.Deltas)
            {
                const auto i = d.RenderToSim[delta.Vertex];
                if (i != UINT32_MAX && d.Representatives[i] == delta.Vertex)
                {
                    BasePositions[i] += weight * Vec(delta.Position);
                    BaseNormals[i] += weight * Vec(delta.Normal);
                }
            }
        }
    }
    void ClothRuntime::SkinAnchors(float t, const glm::mat4& inverseCenter)
    {
        const auto& d = Source->GetData();
        const auto& m = Mesh->GetData();
        for (size_t j = 0; j < Palette.size(); ++j)
        {
            if (OldPalette.size() != Palette.size() || OldPalette[j] == Palette[j])
                StepPalette[j] = Palette[j];
            else
            {
                const auto&    a = OldJointPalette[j];
                const auto&    b = JointPalette[j];
                JointTransform out;
                out.Translation = glm::mix(a.Translation, b.Translation, t);
                out.Rotation    = glm::slerp(a.Rotation, b.Rotation, t);
                out.Scale       = glm::mix(a.Scale, b.Scale, t);
                StepPalette[j]  = out.Matrix();
            }
        }
        const auto transform = inverseCenter * Physics::InterpolateMotionTransform(OldRoot, Root, t);
        if (!SkinCacheValid || CachedSkinTransform != transform || CachedSkinPalette != StepPalette)
        {
            for (size_t group = 0; group < StepSkins.size(); ++group)
            {
                glm::mat4 skin(1);
                if (!SkinWeights.empty() && !StepPalette.empty())
                {
                    skin                = glm::mat4(0);
                    const auto& weights = SkinWeights[group];
                    for (unsigned j = 0; j < 4; ++j)
                        if (weights.Weights[j] > 0 && weights.Joints[j] < StepPalette.size())
                            skin += weights.Weights[j] * StepPalette[weights.Joints[j]];
                    skin *= m.SkinBindTransform;
                }
                StepSkins[group]          = skin;
                StepNormalMatrices[group] = glm::transpose(glm::inverse(glm::mat3(transform * skin)));
            }
            CachedSkinTransform = transform;
            CachedSkinPalette   = StepPalette;
            SkinCacheValid      = true;
        }
        for (size_t i = 0; i < Anchors.size(); ++i)
        {
            const auto  p     = OldBasePositions[i] + (BasePositions[i] - OldBasePositions[i]) * t,
                        n     = OldBaseNormals[i] + (BaseNormals[i] - OldBaseNormals[i]) * t;
            const auto  group = ParticleSkin[i];
            const auto& skin  = StepSkins[group];
            Anchors[i]        = glm::vec3(transform * glm::vec4(glm::vec3(skin * glm::vec4(p, 1)), 1));
            AnchorNormals[i]  = Unit(StepNormalMatrices[group] * n);
        }
    }
    void ClothRuntime::Advance(const ClothSettings& c,
                               const ForceFieldSet& fields,
                               double               seconds,
                               bool                 reset,
                               bool                 paused,
                               bool                 singleStep)
    {
        const auto&  d    = Source->GetData();
        const float  dt   = 1.f / std::clamp(c.SubstepRate, 1.f, 1000.f);
        const double step = 1. / std::clamp(c.SubstepRate, 1.f, 1000.f);
        reset             = reset || ResetPending;
        ResetPending      = false;
        if (reset)
        {
            Clock.Reset();
            OldCenter        = Center;
            OldRoot          = Root;
            OldPalette       = Palette;
            OldJointPalette  = JointPalette;
            OldBasePositions = BasePositions;
            OldBaseNormals   = BaseNormals;
            SkinAnchors(1, glm::inverse(Center));
            Positions = Anchors;
            Previous  = Anchors;
            std::fill(Velocities.begin(), Velocities.end(), glm::vec3(0));
            SolvedRoot = glm::inverse(Center) * Root;
            for (auto& col : Colliders)
                col.Previous = col.Current;
        }

        if (c.AreaDensity > 0)
        {
            std::fill(InvMass.begin(), InvMass.end(), 0.f);
            const auto matrix =
                glm::mat3(glm::inverse(Center) * Root *
                          (Mesh->GetData().Skin.empty() ? glm::mat4(1) : Mesh->GetData().SkinBindTransform));
            for (size_t triangle = 0; triangle < d.Triangles.size(); triangle += 3)
            {
                const auto  a = d.Triangles[triangle], b = d.Triangles[triangle + 1], v = d.Triangles[triangle + 2];
                const float area = glm::length(glm::cross(matrix * (d.RestPositions[b] - d.RestPositions[a]),
                                                          matrix * (d.RestPositions[v] - d.RestPositions[a]))) /
                                   6;
                InvMass[a] += area;
                InvMass[b] += area;
                InvMass[v] += area;
            }
        }
        for (size_t i = 0; i < InvMass.size(); ++i)
            InvMass[i] = d.MaxDistance[i] * c.MaxDistanceScale <= 0 ?
                             0 :
                             1 / std::max(1e-8f, c.AreaDensity > 0 ? c.AreaDensity * InvMass[i] : c.ParticleMass);
        const double oldRemainder = Clock.Remainder, frame = std::isfinite(seconds) && seconds > 0 ? seconds : 0;
        const auto   steps = reset || paused ? 0 : Clock.Accumulate(frame, step, std::max(1u, c.MaxSubsteps));
        for (uint32_t sub = 0; sub < steps; ++sub)
        {
            const float t      = frame > 0 ? float(std::clamp(((sub + 1) * step - oldRemainder) / frame, 0., 1.)) : 1;
            const auto  center = Physics::InterpolateMotionTransform(OldCenter, Center, t),
                        inverseCenter = glm::inverse(center);
            SkinAnchors(t, inverseCenter);
            const auto  relative  = inverseCenter * Physics::InterpolateMotionTransform(OldRoot, Root, t),
                        transport = relative * glm::inverse(SolvedRoot);
            const auto  motion    = JointTransform::FromMatrix(transport);
            const float angle     = 2 * std::acos(std::clamp(std::abs(motion.Rotation.w), 0.f, 1.f));
            const float linear    = glm::length(motion.Translation);
            float       inertia   = std::clamp(c.InertiaScale, 0.f, 1.f);
            if (linear > 0 && c.LinearInertiaLimit > 0)
                inertia = std::min(inertia, c.LinearInertiaLimit * dt / linear);
            if (angle > 0 && c.AngularInertiaLimit > 0)
                inertia = std::min(inertia, glm::radians(c.AngularInertiaLimit) * dt / angle);
            const auto restMatrix =
                glm::mat3(relative * (Mesh->GetData().Skin.empty() ? glm::mat4(1) : Mesh->GetData().SkinBindTransform));
            if (!RestCacheValid || CachedRestMatrix != restMatrix)
            {
                auto lengths = [&](const auto& constraints, auto& rests) {
                    for (size_t j = 0; j < constraints.size(); ++j)
                    {
                        const auto& k = constraints[j];
                        rests[j]      = glm::length(restMatrix * (d.RestPositions[k.A] - d.RestPositions[k.B]));
                    }
                };
                lengths(d.Stretch, StretchRest);
                lengths(d.Bend, BendRest);
                CachedTetherRestScale =
                    std::max({glm::length(restMatrix[0]), glm::length(restMatrix[1]), glm::length(restMatrix[2])});
                CachedRestMatrix = restMatrix;
                RestCacheValid   = true;
            }
            const float tetherRestScale = CachedTetherRestScale;
            SolvedRoot                  = relative;
            Previous                    = Positions;
            for (size_t i = 0; i < Positions.size(); ++i)
            {
                Positions[i]     = glm::mix(glm::vec3(transport * glm::vec4(Positions[i], 1)), Positions[i], inertia);
                Velocities[i]    = glm::mix(glm::mat3(transport) * Velocities[i], Velocities[i], inertia);
                Previous[i]      = Positions[i]; // Interpolate in the transported reference frame, as spring bones do.
                PredictedFrom[i] = Positions[i];
                if (InvMass[i] == 0)
                {
                    Positions[i]  = Anchors[i];
                    Velocities[i] = glm::vec3(0);
                }
                else
                    Velocities[i] += glm::mat3(inverseCenter) * (Gravity * c.GravityScale) * dt;
            }
            // Fixed triangle order; distribute area-based aerodynamic force equally to its vertices.
            for (size_t tri = 0; tri < d.Triangles.size(); tri += 3)
            {
                const auto  a = d.Triangles[tri], b = d.Triangles[tri + 1], e = d.Triangles[tri + 2];
                const auto  cross     = glm::cross(Positions[b] - Positions[a], Positions[e] - Positions[a]);
                const float twiceArea = glm::length(cross);
                if (twiceArea < 1e-12f)
                    continue;
                const auto  n = cross / twiceArea, point = (Positions[a] + Positions[b] + Positions[e]) / 3.f;
                const auto  air = glm::mat3(inverseCenter) *
                                  fields.SampleWind(glm::vec3(center * glm::vec4(point, 1)), Clock.Time) * c.WindScale;
                const auto  v   = air - (Velocities[a] + Velocities[b] + Velocities[e]) / 3.f;
                const float speed = glm::length(v);
                if (speed < 1e-8f)
                    continue;
                const float facing = glm::dot(n, v) / speed;
                const auto  lift   = Unit(n - v * (glm::dot(n, v) / (speed * speed)), glm::vec3(0));
                const auto  force  = .5f * 1.225f * (twiceArea * .5f) * speed * speed *
                                     (c.Drag * std::abs(facing) * (v / speed) + c.Lift * facing * lift) / 3.f;
                for (auto i : {a, b, e})
                    if (InvMass[i] > 0)
                        Velocities[i] += force * InvMass[i] * dt;
            }
            for (size_t i = 0; i < Positions.size(); ++i)
                if (InvMass[i] > 0)
                    Positions[i] += Velocities[i] * dt;
            std::fill(StretchLambda.begin(), StretchLambda.end(), 0);
            std::fill(BendLambda.begin(), BendLambda.end(), 0);
            for (auto& col : Colliders)
                col.Step = Physics::InterpolateMotionTransform(col.Previous, col.Current, t);
            for (uint32_t iteration = 0; iteration < std::max(1u, c.Iterations); ++iteration)
            {
                auto solve = [&](const auto& constraints,
                                 const auto& rests,
                                 auto&       lambdas,
                                 float       compliance,
                                 const auto& multipliers) {
                    for (size_t j = 0; j < constraints.size(); ++j)
                    {
                        auto k                = constraints[j];
                        k.Rest                = rests[j];
                        const float stiffness = multipliers.empty() ? 1 : (multipliers[k.A] + multipliers[k.B]) * .5f;
                        if (stiffness > 0)
                            Distance(Positions, InvMass, k, std::max(0.f, compliance) / stiffness, dt, lambdas[j]);
                    }
                };
                solve(d.Stretch, StretchRest, StretchLambda, c.StretchCompliance, d.StretchMultiplier);
                solve(d.Bend, BendRest, BendLambda, c.BendCompliance, d.BendMultiplier);
                for (auto tether : d.Tethers)
                {
                    tether.Rest *= std::max(1.f, c.TetherScale) * tetherRestScale;
                    float lambda = 0;
                    Distance(Positions, InvMass, tether, 0, dt, lambda, true);
                }
                for (size_t i = 0; i < Positions.size(); ++i)
                {
                    if (InvMass[i] == 0)
                    {
                        Positions[i] = Anchors[i];
                        continue;
                    }
                    const float maxDistance = d.MaxDistance[i] * std::max(0.f, c.MaxDistanceScale);
                    const auto  offset      = Positions[i] - Anchors[i];
                    const float length      = glm::length(offset);
                    if (length > maxDistance)
                        Positions[i] = Anchors[i] + offset * (maxDistance / length);
                    const float radius = d.BackstopRadius[i];
                    if (radius > 0)
                    {
                        const auto sphere = Anchors[i] - AnchorNormals[i] * (d.BackstopDistance[i] + radius);
                        const auto delta  = Positions[i] - sphere;
                        if (glm::dot(delta, delta) < radius * radius)
                            Positions[i] = sphere + Unit(delta, AnchorNormals[i]) * radius;
                    }
                    auto world = glm::vec3(center * glm::vec4(Positions[i], 1));
                    for (const auto& col : Colliders)
                        world = Physics::ProjectMotionParticle(col.Shape, col.Step, col.Inside, world, c.Thickness);
                    if (c.GroundPlane)
                        world.y = std::max(world.y, c.GroundHeight + c.Thickness);
                    Positions[i] = glm::vec3(inverseCenter * glm::vec4(world, 1));
                }
            }
            const float damping =
                std::pow(1 - std::clamp(c.Damping, 0.f, 1.f), dt * 120) * std::exp(-std::max(0.f, c.VelocityDrag) * dt);
            for (size_t i = 0; i < Positions.size(); ++i)
                Velocities[i] = InvMass[i] > 0 ? (Positions[i] - PredictedFrom[i]) * (damping / dt) : glm::vec3(0);
            ++Clock.Steps;
            Clock.Time += step;
        }
        if (!paused || reset)
            Output(reset || singleStep ? 1 : float(Clock.Alpha(step)));
    }
    glm::vec3 ClothRuntime::CurrentAnchor(uint32_t particle) const
    {
        glm::mat4   skin(1);
        const auto& mesh = Mesh->GetData();
        if (!mesh.Skin.empty() && !Palette.empty())
        {
            skin                = glm::mat4(0);
            const auto& weights = mesh.Skin[Source->GetData().Representatives[particle]];
            for (unsigned j = 0; j < 4; ++j)
                if (weights.Weights[j] > 0 && weights.Joints[j] < Palette.size())
                    skin += weights.Weights[j] * Palette[weights.Joints[j]];
            skin *= mesh.SkinBindTransform;
        }
        return glm::vec3(skin * glm::vec4(BasePositions[particle], 1));
    }
    void ClothRuntime::GetSkinnedAnchors(std::vector<glm::vec3>& output) const
    {
        output.resize(BasePositions.size());
        for (uint32_t i = 0; i < output.size(); ++i)
            output[i] = glm::vec3(Root * glm::vec4(CurrentAnchor(i), 1));
    }
    void ClothRuntime::Output(float alpha)
    {
        const auto& d           = Source->GetData();
        const auto  inverseRoot = glm::inverse(Root) * Center;

        for (size_t i = 0; i < Positions.size(); ++i)
        {
            if (InvMass[i] == 0)
            {
                // Kinematic vertices must share the current GPU-skinned surface, including frames with no substep.
                // Free vertices retain the fixed-step interpolation below.
                RenderPositions[i] = CurrentAnchor(uint32_t(i));
            }
            else
                RenderPositions[i] =
                    glm::vec3(inverseRoot * glm::vec4(Previous[i] + (Positions[i] - Previous[i]) * alpha, 1));
        }
        std::fill(Normals.begin(), Normals.end(), glm::vec3(0));
        for (size_t t = 0; t < d.Triangles.size(); t += 3)
        {
            const uint32_t ids[] = {d.Triangles[t], d.Triangles[t + 1], d.Triangles[t + 2]};
            const auto     n     = Unit(glm::cross(RenderPositions[ids[1]] - RenderPositions[ids[0]],
                                                   RenderPositions[ids[2]] - RenderPositions[ids[0]]),
                                        glm::vec3(0));
            for (unsigned j = 0; j < 3; ++j)
            {
                const auto  a           = RenderPositions[ids[(j + 1) % 3]] - RenderPositions[ids[j]],
                            b           = RenderPositions[ids[(j + 2) % 3]] - RenderPositions[ids[j]];
                const float denominator = glm::length(a) * glm::length(b);
                if (denominator > 1e-12f)
                    Normals[ids[j]] += n * std::acos(std::clamp(glm::dot(a, b) / denominator, -1.f, 1.f));
            }
        }
        auto&  out = Stream->Data;
        Bounds bounds;
        for (size_t v = 0; v < StreamVertices.size(); ++v)
        {
            const auto i         = d.RenderToSim[StreamVertices[v]];
            auto&      vertex    = out.Vertices[v];
            vertex.Position      = Array(RenderPositions[i]);
            const auto n         = Unit(Normals[i]);
            vertex.Normal        = Array(n);
            out.SmoothNormals[v] = vertex.Normal;
            const auto&     src  = Mesh->GetData().Vertices[StreamVertices[v]].Tangent;
            const glm::vec3 authored(src[0], src[1], src[2]); // tangent xyz, preserve handedness
            const auto      tangent =
                Unit(authored - n * glm::dot(authored, n), Unit(glm::cross(n, glm::vec3(0, 0, 1)), {1, 0, 0}));
            vertex.Tangent = {tangent.x, tangent.y, tangent.z, vertex.Tangent[3]};
            bounds.Expand(RenderPositions[i]);
        }
        out.LocalBounds             = bounds;
        out.Sections[0].LocalBounds = bounds;
        ++Stream->Version;
    }
} // namespace Kans

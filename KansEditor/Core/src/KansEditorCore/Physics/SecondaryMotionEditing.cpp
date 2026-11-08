#include "KansEditorCore/Physics/SecondaryMotionEditing.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <set>
#include <sstream>
#include <yaml-cpp/yaml.h>
#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "Kans3D/Physics/PhysicsSerialization.h"
#include "Kans3D/Renderer/DebugDraw.h"
#include "Kans3D/Scene/Scene.h"

namespace Kans::SecondaryMotionEditing
{
    namespace
    {
        AssetError Error(AssetErrorCode code, std::string message) { return AssetError {code, std::move(message)}; }

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return text;
        }

        std::variant<std::string, AssetError> ReadText(const std::filesystem::path& file)
        {
            std::ifstream stream(file, std::ios::binary);
            if (!stream)
                return Error(AssetErrorCode::NotFound, "Cannot read " + file.filename().u8string());
            std::stringstream text;
            text << stream.rdbuf();
            return text.str();
        }

        std::optional<AssetError> WriteText(const std::filesystem::path& file, const std::string& text)
        {
            std::error_code ignored;
            if (file.has_parent_path())
                std::filesystem::create_directories(file.parent_path(), ignored);
            std::ofstream stream(file, std::ios::binary | std::ios::trunc);
            if (!stream)
                return Error(AssetErrorCode::IoError, "Cannot write " + file.filename().u8string());
            stream << text;
            stream.flush();
            if (!stream)
                return Error(AssetErrorCode::IoError, "Cannot write " + file.filename().u8string());
            return std::nullopt;
        }

        std::filesystem::path UniqueFile(const std::filesystem::path& directory,
                                         const std::string&           stem,
                                         const std::string&           extension)
        {
            auto file = directory / std::filesystem::u8path(stem + extension);
            for (int n = 2; std::filesystem::exists(file); ++n)
                file = directory / std::filesystem::u8path(stem + std::to_string(n) + extension);
            return file;
        }

        glm::vec3 Position(const glm::mat4& m) { return glm::vec3(m[3]); }

        // Two unit vectors perpendicular to `n` and to each other.
        void Basis(glm::vec3 n, glm::vec3& u, glm::vec3& v)
        {
            const glm::vec3 helper = std::abs(n.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            u                      = glm::normalize(glm::cross(helper, n));
            v                      = glm::cross(n, u);
        }

        // The keys a preset file may hold, per section.
        const std::vector<std::string>& SpringChainKeys()
        {
            static const std::vector<std::string> keys = {"Stiffness",
                                                          "Drag",
                                                          "GravityPower",
                                                          "GravityDirection",
                                                          "HitRadius",
                                                          "RotationAxis",
                                                          "AngleLimit",
                                                          "StiffnessCurve",
                                                          "DragCurve",
                                                          "GravityPowerCurve",
                                                          "GravityDirectionCurve",
                                                          "HitRadiusCurve",
                                                          "RotationAxisCurve",
                                                          "AngleLimitCurve"};
            return keys;
        }
        const std::vector<std::string>& SpringComponentKeys()
        {
            static const std::vector<std::string> keys = {"InertiaScale", "WindScale", "SubstepRate", "MaxSubsteps"};
            return keys;
        }
        const std::vector<std::string>& ClothKeys()
        {
            static const std::vector<std::string> keys = {"StretchCompliance",
                                                          "BendCompliance",
                                                          "TetherScale",
                                                          "Damping",
                                                          "VelocityDrag",
                                                          "ParticleMass",
                                                          "AreaDensity",
                                                          "GravityScale",
                                                          "WindScale",
                                                          "Lift",
                                                          "Drag",
                                                          "InertiaScale",
                                                          "LinearInertiaLimit",
                                                          "AngularInertiaLimit",
                                                          "SubstepRate",
                                                          "MaxSubsteps",
                                                          "Iterations",
                                                          "Thickness"};
            return keys;
        }

        YAML::Node Pick(const YAML::Node& from, const std::vector<std::string>& keys)
        {
            YAML::Node out(YAML::NodeType::Map);
            for (const auto& key : keys)
                if (from[key])
                    out[key] = from[key];
            return out;
        }

        void RequireOnly(const YAML::Node& node, const std::vector<std::string>& keys, const char* section)
        {
            if (!node.IsMap())
                throw std::invalid_argument(std::string(section) + " must be a map");
            for (auto entry : node)
            {
                const auto key = entry.first.as<std::string>();
                if (std::find(keys.begin(), keys.end(), key) == keys.end())
                    throw std::invalid_argument(std::string(section) + " cannot set " + key);
            }
        }
    } // namespace

    // ---------------------------------------------------------------- chains

    ResolvedChain ResolveChain(const Scene& scene, const SpringBoneChain& chain)
    {
        ResolvedChain result;
        auto&         path = result.Path;
        const Entity  root = uint64_t(chain.RootBone) ? scene.GetEntityByUUID(chain.RootBone) : Entity {};
        if (root)
        {
            if (uint64_t(chain.EndBone))
            {
                for (Entity e = scene.GetEntityByUUID(chain.EndBone); e;)
                {
                    path.push_back(e);
                    if (e == root)
                        break;
                    const UUID parent = e.GetComponent<RelationshipComponent>().Parent;
                    e                 = uint64_t(parent) ? scene.GetEntityByUUID(parent) : Entity {};
                }
                if (path.empty() || path.back() != root)
                    path.clear();
                std::reverse(path.begin(), path.end());
            }
            else
                for (Entity e = root; e;)
                {
                    path.push_back(e);
                    const auto& children = e.GetComponent<RelationshipComponent>().Children;
                    e = children.empty() ? Entity {} : scene.GetEntityByUUID(children.front());
                }
        }
        for (size_t i = 0; i < path.size(); ++i)
            if (std::find(chain.ExcludedBones.begin(), chain.ExcludedBones.end(), path[i].GetUUID()) !=
                chain.ExcludedBones.end())
            {
                path.resize(i);
                break;
            }
        result.Simulated = path.empty() ? 0 : path.size() - (chain.EndExtension > 0 ? 0 : 1);
        return result;
    }

    void ChainPicker::Begin(UUID owner, size_t chain)
    {
        m_Owner = owner;
        m_Chain = chain;
        m_Root  = 0;
    }

    void ChainPicker::Cancel() { Begin(0, 0); }

    const char* ChainPicker::Prompt() const
    {
        return HasRoot() ? "Select the end bone (the root or a bone below it)" : "Select the root bone";
    }

    ChainPicker::Result ChainPicker::Pick(const Scene& scene, Entity picked, SpringBoneChain& chain)
    {
        if (!IsActive() || !picked || picked.GetUUID() == m_Owner)
            return Result::Ignored;
        if (!HasRoot())
        {
            m_Root = picked.GetUUID();
            return Result::RootSet;
        }
        const Entity root = scene.GetEntityByUUID(m_Root);
        if (!root || !(picked == root || scene.IsDescendantOf(picked, root)))
            return Result::Rejected;
        chain.RootBone = m_Root;
        chain.EndBone  = picked.GetUUID();
        Cancel();
        return Result::Done;
    }

    Entity FeedChainPicker(ChainPicker& picker, Scene& scene, Entity selected, ChainPicker::Result* result)
    {
        if (result)
            *result = ChainPicker::Result::Ignored;
        if (!picker.IsActive())
            return {};
        Entity owner = scene.GetEntityByUUID(picker.Owner());
        if (!owner || !owner.HasComponent<SpringBoneComponent>() ||
            picker.Chain() >= owner.GetComponent<SpringBoneComponent>().Chains.size())
        {
            picker.Cancel();
            return {};
        }
        if (!selected || selected == owner)
            return {};
        const size_t index = picker.Chain();
        SpringBoneChain chain  = owner.GetComponent<SpringBoneComponent>().Chains[index];
        const auto      picked = picker.Pick(scene, selected, chain);
        if (result)
            *result = picked;
        if (picked == ChainPicker::Result::Done)
            owner.PatchComponent<SpringBoneComponent>([&](SpringBoneComponent& c) { c.Chains[index] = chain; });
        return owner;
    }

    // ---------------------------------------------------------------- curves

    void ResetCurve(TimelineCurve& curve, glm::vec4 value)
    {
        curve.Keys.clear();
        for (double time : {0.0, 1.0})
        {
            TimelineKey key;
            key.Time  = time;
            key.Value = value;
            curve.Keys.push_back(key);
        }
    }

    size_t InsertCurveKey(TimelineCurve& curve, double time, glm::vec4 fallback)
    {
        time = std::clamp(time, 0.0, 1.0);
        for (size_t i = 0; i < curve.Keys.size(); ++i)
            if (std::abs(curve.Keys[i].Time - time) < 1e-4)
                return i;
        TimelineKey key;
        key.Time   = time;
        key.Value  = curve.Sample(time, fallback);
        auto where = std::upper_bound(
            curve.Keys.begin(), curve.Keys.end(), time, [](double t, const TimelineKey& k) { return t < k.Time; });
        const auto inserted = curve.Keys.insert(where, key);
        return size_t(inserted - curve.Keys.begin());
    }

    void MoveCurveKey(TimelineCurve& curve, size_t index, double time, glm::vec4 value)
    {
        if (index >= curve.Keys.size())
            return;
        constexpr double gap  = 1e-3;
        auto&            keys = curve.Keys;
        if (index == 0)
            time = 0.0;
        else if (index + 1 == keys.size())
            time = 1.0;
        else
            time = std::clamp(time, keys[index - 1].Time + gap, keys[index + 1].Time - gap);
        keys[index].Time  = time;
        keys[index].Value = value;
    }

    bool RemoveCurveKey(TimelineCurve& curve, size_t index)
    {
        if (index == 0 || index + 1 >= curve.Keys.size())
            return false;
        curve.Keys.erase(curve.Keys.begin() + std::ptrdiff_t(index));
        return true;
    }

    // ---------------------------------------------------------------- colliders

    std::vector<PhysicsEditing::ShapeHandle> Handles(const Physics::Plane& plane)
    {
        const float     length = glm::length(plane.Normal);
        const glm::vec3 normal = length > 1e-6f ? plane.Normal / length : glm::vec3(0, 1, 0);
        return {{0, glm::vec3(0.0f), normal}};
    }

    Physics::Plane DragHandle(const Physics::Plane& start, int id, float distance)
    {
        Physics::Plane result = start;
        if (id != 0 || !std::isfinite(distance))
            return result;
        const glm::vec3 normal = Handles(start)[0].Direction;
        result.Offset.Position = start.Offset.Position + start.Offset.Rotation * (start.Offset.Scale * normal * distance);
        return result;
    }

    std::vector<PhysicsEditing::ShapeHandle> Handles(const Physics::MotionColliderShape& shape)
    {
        return std::visit(
            [](const auto& s) {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, Physics::Plane>)
                    return Handles(s);
                else
                    return PhysicsEditing::Handles(s);
            },
            shape);
    }

    Physics::MotionColliderShape DragHandle(const Physics::MotionColliderShape& start, int id, float distance)
    {
        return std::visit(
            [&](const auto& s) -> Physics::MotionColliderShape {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, Physics::Plane>)
                    return DragHandle(s, id, distance);
                else
                    return PhysicsEditing::DragHandle(s, id, distance);
            },
            start);
    }

    bool SupportsInside(const Physics::MotionColliderShape& shape)
    {
        return std::holds_alternative<Physics::Sphere>(shape) || std::holds_alternative<Physics::Capsule>(shape);
    }

    void DrawMotionCollider(DebugDraw& draw, Scene& scene, Entity entity, glm::vec4 color)
    {
        if (!entity || !entity.HasComponent<MotionColliderComponent>())
            return;
        const auto&     c     = entity.GetComponent<MotionColliderComponent>();
        const glm::mat4 world = scene.GetWorldTransform(entity);
        const auto      tick  = [&](glm::vec3 surface, glm::vec3 inward, float length) {
            draw.Line(surface, surface + inward * length, color);
        };
        std::visit(
            [&](const auto& s) {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, Physics::Plane>)
                {
                    const glm::mat4 m = world * s.Offset.Matrix();
                    glm::vec3       normal =
                        glm::transpose(glm::inverse(glm::mat3(m))) * (glm::length(s.Normal) > 1e-6f ? s.Normal : glm::vec3(0, 1, 0));
                    normal = glm::normalize(normal);
                    glm::vec3 u, v;
                    Basis(normal, u, v);
                    const float scale = std::max({glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])),
                                                  glm::length(glm::vec3(m[2])), 1e-3f});
                    const float     half   = PlaneGridHalfSize * scale;
                    const glm::vec3 centre = Position(m);
                    constexpr int   lines  = 5;
                    for (int i = 0; i < lines; ++i)
                    {
                        const float t = -half + 2 * half * float(i) / float(lines - 1);
                        draw.Line(centre + u * t - v * half, centre + u * t + v * half, color);
                        draw.Line(centre + v * t - u * half, centre + v * t + u * half, color);
                    }
                    draw.Arrow(centre, centre + normal * half, color);
                }
                else
                {
                    draw.Shape(Physics::Shape(s), world, color);
                    if (!c.Inside)
                        return;
                    const auto pose = Physics::TransformShape(s.Offset, world);
                    if constexpr (std::is_same_v<T, Physics::Sphere>)
                    {
                        const float radius = Physics::ScaledRadius(s, pose);
                        for (int axis = 0; axis < 3; ++axis)
                            for (float sign : {-1.0f, 1.0f})
                            {
                                glm::vec3 direction(0.0f);
                                direction[axis]          = sign;
                                const glm::vec3 outward = pose.Rotation * direction;
                                tick(pose.Position + outward * radius, -outward, radius * 0.25f);
                            }
                    }
                    else if constexpr (std::is_same_v<T, Physics::Capsule>)
                    {
                        glm::vec3 a, b;
                        float     radius = 0;
                        Physics::CapsuleSegment(s, pose, a, b, radius);
                        const glm::vec3 axis = glm::length(b - a) > 1e-6f ? glm::normalize(b - a) : glm::vec3(0, 1, 0);
                        glm::vec3       u, v;
                        Basis(axis, u, v);
                        tick(b + axis * radius, -axis, radius * 0.25f);
                        tick(a - axis * radius, axis, radius * 0.25f);
                        for (glm::vec3 point : {a, (a + b) * 0.5f, b})
                            for (glm::vec3 side : {u, -u, v, -v})
                                tick(point + side * radius, -side, radius * 0.25f);
                    }
                }
            },
            c.Shape);
    }

    std::vector<Entity> GroupColliders(Scene& scene, const std::vector<UUID>& groups)
    {
        std::vector<Entity> result;
        std::set<uint64_t>  seen;
        std::function<void(Entity)> visit = [&](Entity e) {
            if (e.HasComponent<MotionColliderComponent>() && seen.insert(uint64_t(e.GetUUID())).second)
                result.push_back(e);
            for (Entity child : scene.GetChildren(e))
                visit(child);
        };
        for (UUID id : groups)
            if (Entity group = uint64_t(id) ? scene.GetEntityByUUID(id) : Entity {})
                visit(group);
        return result;
    }

    void DrawSpringChains(DebugDraw& draw, Scene& scene, Entity owner, glm::vec4 color)
    {
        if (!owner || !owner.HasComponent<SpringBoneComponent>())
            return;
        for (const auto& chain : owner.GetComponent<SpringBoneComponent>().Chains)
        {
            const auto resolved = ResolveChain(scene, chain);
            const auto& path    = resolved.Path;
            std::vector<glm::vec3> points;
            for (Entity bone : path)
                points.push_back(Position(scene.GetSpringBoneRenderTransform(bone)));
            if (chain.EndExtension > 0 && !points.empty())
            {
                // The extension continues the last bone's direction (up its local Y when it has no parent bone).
                const glm::mat4 last = scene.GetSpringBoneRenderTransform(path.back());
                glm::vec3       direction =
                    points.size() > 1 ? points.back() - points[points.size() - 2] : glm::vec3(last[1]);
                if (glm::length(direction) > 1e-6f)
                    points.push_back(points.back() + glm::normalize(direction) * chain.EndExtension);
            }
            for (size_t i = 0; i + 1 < points.size(); ++i)
                draw.Line(points[i], points[i + 1], color);
            // Each simulated joint collides at its tip with the (curved) hit radius.
            for (size_t i = 0; i < resolved.Simulated && i + 1 < points.size(); ++i)
            {
                const float u = resolved.Simulated > 1 ? float(i) / float(resolved.Simulated - 1) : 0.0f;
                const float r = std::max(0.0f, chain.HitRadiusCurve.Sample(u, glm::vec4(chain.HitRadius)).x);
                if (r > 0)
                    draw.Sphere(points[i + 1], r, color);
            }
        }
    }

    void DrawSecondaryMotion(DebugDraw& draw, Scene& scene, Entity selected, const std::unordered_set<UUID>& hidden)
    {
        if (!selected || hidden.count(selected.GetUUID()))
            return;
        std::vector<UUID> groups;
        if (selected.HasComponent<SpringBoneComponent>())
        {
            DrawSpringChains(draw, scene, selected, ChainColor);
            const auto& g = selected.GetComponent<SpringBoneComponent>().ColliderGroups;
            groups.insert(groups.end(), g.begin(), g.end());
        }
        if (selected.HasComponent<ClothComponent>())
        {
            const auto& g = selected.GetComponent<ClothComponent>().ColliderGroups;
            groups.insert(groups.end(), g.begin(), g.end());
        }
        for (Entity collider : GroupColliders(scene, groups))
            if (collider != selected && !hidden.count(collider.GetUUID()))
                DrawMotionCollider(draw, scene, collider, ColliderColor * glm::vec4(1, 1, 1, 0.7f));
        DrawMotionCollider(draw, scene, selected, ColliderColor);
    }

    // ---------------------------------------------------------------- maps

    const char* MapName(ClothMap map)
    {
        switch (map)
        {
            case ClothMap::MaxDistance:
                return "Max Distance";
            case ClothMap::BackstopDistance:
                return "Backstop Distance";
            case ClothMap::BackstopRadius:
                return "Backstop Radius";
            case ClothMap::StretchMultiplier:
                return "Stretch Multiplier";
            case ClothMap::BendMultiplier:
                return "Bend Multiplier";
        }
        return "?";
    }

    std::vector<float> MapValues(const ClothData& data, ClothMap map)
    {
        const size_t n = data.RestPositions.size();
        switch (map)
        {
            case ClothMap::MaxDistance:
                return data.MaxDistance;
            case ClothMap::BackstopDistance:
                return data.BackstopDistance;
            case ClothMap::BackstopRadius:
                return data.BackstopRadius;
            case ClothMap::StretchMultiplier:
                return data.StretchMultiplier.empty() ? std::vector<float>(n, 1.0f) : data.StretchMultiplier;
            case ClothMap::BendMultiplier:
                return data.BendMultiplier.empty() ? std::vector<float>(n, 1.0f) : data.BendMultiplier;
        }
        return {};
    }

    float ClampMapValue(ClothMap map, float value)
    {
        if (!std::isfinite(value))
            return 0.0f;
        return map == ClothMap::BackstopDistance ? value : std::max(0.0f, value);
    }

    std::vector<std::vector<uint32_t>> Neighbours(const ClothData& data)
    {
        std::vector<std::vector<uint32_t>> result(data.RestPositions.size());
        for (size_t t = 0; t + 2 < data.Triangles.size(); t += 3)
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t a = data.Triangles[t + size_t(k)], b = data.Triangles[t + size_t((k + 1) % 3)];
                result[a].push_back(b);
                result[b].push_back(a);
            }
        for (auto& list : result)
        {
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
        }
        return result;
    }

    std::vector<float> MapFromVertexColors(const ClothData& data, const MeshData& mesh, int channel, float scale)
    {
        std::vector<float> values(data.RestPositions.size(), std::numeric_limits<float>::infinity());
        channel = std::clamp(channel, 0, 3);
        for (size_t v = 0; v < data.RenderToSim.size() && v < mesh.Vertices.size(); ++v)
        {
            const uint32_t particle = data.RenderToSim[v];
            if (particle == UINT32_MAX)
                continue;
            const float value = mesh.Vertices[v].Color[size_t(channel)] * scale;
            values[particle]  = std::min(values[particle], std::isfinite(value) ? std::max(0.0f, value) : 0.0f);
        }
        for (float& value : values)
            if (!std::isfinite(value))
                value = 0.0f;
        return values;
    }

    glm::vec4 HeatColor(float t)
    {
        t = std::isfinite(t) ? std::clamp(t, 0.0f, 1.0f) : 0.0f;
        const glm::vec3 low {0.15f, 0.35f, 1.0f}, mid {0.2f, 0.9f, 0.3f}, high {1.0f, 0.2f, 0.15f};
        return glm::vec4(t < 0.5f ? glm::mix(low, mid, t * 2.0f) : glm::mix(mid, high, t * 2.0f - 1.0f), 1.0f);
    }

    // ---------------------------------------------------------------- tools

    float BrushWeight(float distance, float radius, float falloff)
    {
        if (!(radius > 0) || !(distance < radius))
            return 0.0f;
        const float inner = radius * (1.0f - std::clamp(falloff, 0.0f, 1.0f));
        if (distance <= inner)
            return 1.0f;
        const float t = (distance - inner) / (radius - inner);
        return 1.0f - t * t * (3.0f - 2.0f * t);
    }

    void ApplyBrush(ClothMap                                  map,
                    std::vector<float>&                       values,
                    const std::vector<glm::vec3>&             positions,
                    const std::vector<std::vector<uint32_t>>& neighbours,
                    glm::vec3                                 centre,
                    const Brush&                              brush)
    {
        const std::vector<float> before   = values;
        const float              strength = std::clamp(brush.Strength, 0.0f, 1.0f);
        for (size_t i = 0; i < values.size() && i < positions.size(); ++i)
        {
            const float w = BrushWeight(glm::distance(positions[i], centre), brush.Radius, brush.Falloff) * strength;
            if (w <= 0)
                continue;
            float& value = values[i];
            switch (brush.Mode)
            {
                case BrushMode::Add:
                    value = before[i] + brush.Value * w;
                    break;
                case BrushMode::Subtract:
                    value = before[i] - brush.Value * w;
                    break;
                case BrushMode::Set:
                    value = before[i] + (brush.Value - before[i]) * w;
                    break;
                case BrushMode::Smooth:
                {
                    if (i >= neighbours.size() || neighbours[i].empty())
                        break;
                    float sum = 0;
                    for (uint32_t n : neighbours[i])
                        sum += before[n];
                    const float average = sum / float(neighbours[i].size());
                    value               = before[i] + (average - before[i]) * w;
                    break;
                }
            }
            value = ClampMapValue(map, value);
        }
    }

    std::vector<uint32_t> ConnectedRegion(const std::vector<float>&                 values,
                                          const std::vector<std::vector<uint32_t>>& neighbours,
                                          uint32_t                                  seed,
                                          float                                     tolerance)
    {
        std::vector<uint32_t> region;
        if (seed >= values.size())
            return region;
        std::vector<bool>     visited(values.size(), false);
        std::vector<uint32_t> stack {seed};
        visited[seed] = true;
        while (!stack.empty())
        {
            const uint32_t i = stack.back();
            stack.pop_back();
            region.push_back(i);
            if (i >= neighbours.size())
                continue;
            for (uint32_t n : neighbours[i])
                if (n < values.size() && !visited[n] && std::abs(values[n] - values[seed]) <= tolerance)
                {
                    visited[n] = true;
                    stack.push_back(n);
                }
        }
        std::sort(region.begin(), region.end());
        return region;
    }

    void ApplyGradient(std::vector<float>&           values,
                       const std::vector<glm::vec3>& positions,
                       glm::vec3                     a,
                       glm::vec3                     b,
                       float                         start,
                       float                         end)
    {
        const glm::vec3 axis    = b - a;
        const float     length2 = glm::dot(axis, axis);
        for (size_t i = 0; i < values.size() && i < positions.size(); ++i)
        {
            const float t = length2 > 1e-12f ? std::clamp(glm::dot(positions[i] - a, axis) / length2, 0.0f, 1.0f) : 0.0f;
            values[i]     = start + (end - start) * t;
        }
    }

    std::optional<ParticleHit> PickParticle(const ClothData&              data,
                                            const std::vector<glm::vec3>& positions,
                                            glm::vec3                     origin,
                                            glm::vec3                     direction)
    {
        std::optional<ParticleHit> best;
        for (size_t t = 0; t + 2 < data.Triangles.size(); t += 3)
        {
            const uint32_t ids[3] = {data.Triangles[t], data.Triangles[t + 1], data.Triangles[t + 2]};
            if (ids[0] >= positions.size() || ids[1] >= positions.size() || ids[2] >= positions.size())
                continue;
            const glm::vec3 p0 = positions[ids[0]], e1 = positions[ids[1]] - p0, e2 = positions[ids[2]] - p0;
            const glm::vec3 h   = glm::cross(direction, e2);
            const float     det = glm::dot(e1, h);
            if (std::abs(det) < 1e-12f)
                continue;
            const float     inv = 1.0f / det;
            const glm::vec3 s   = origin - p0;
            const float     u   = glm::dot(s, h) * inv;
            if (u < 0 || u > 1)
                continue;
            const glm::vec3 q = glm::cross(s, e1);
            const float     v = glm::dot(direction, q) * inv;
            if (v < 0 || u + v > 1)
                continue;
            const float distance = glm::dot(e2, q) * inv;
            if (distance <= 0 || (best && distance >= best->Distance))
                continue;
            ParticleHit hit;
            hit.Point    = origin + direction * distance;
            hit.Distance = distance;
            float nearest = std::numeric_limits<float>::max();
            for (uint32_t id : ids)
                if (const float d = glm::distance(positions[id], hit.Point); d < nearest)
                    nearest = d, hit.Particle = id;
            best = hit;
        }
        return best;
    }

    // ---------------------------------------------------------------- painting session

    void ClothPaintSession::Begin(UUID entity, AssetID asset, ClothData data, std::filesystem::path file)
    {
        Cancel();
        m_Entity = entity;
        m_Asset  = asset;
        m_Data   = std::move(data);
        m_File   = std::move(file);
    }

    void ClothPaintSession::Cancel()
    {
        m_Entity = 0;
        m_Asset  = {};
        m_File.clear();
        m_Data = {};
        m_Stroke.reset();
        m_Undo.clear();
        m_Redo.clear();
        m_Version = 0;
        m_SavedStep = 0;
        m_Pose                     = {};
        m_PoseVersion              = UINT64_MAX;
        m_Positions.clear();
        ActiveMap = ClothMap::MaxDistance;
    }

    void ClothPaintSession::BeginStroke() { m_Stroke = MapValues(m_Data, ActiveMap); }

    bool ClothPaintSession::Write(ClothMap map, const std::vector<float>& values)
    {
        try
        {
            WriteClothMap(m_Data, map, values);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    bool ClothPaintSession::EndStroke()
    {
        if (!m_Stroke)
            return false;
        std::vector<float> after = std::move(*m_Stroke);
        m_Stroke.reset();
        std::vector<float> before = MapValues(m_Data, ActiveMap);
        if (after == before || !Write(ActiveMap, after))
            return false;
        m_Undo.push_back({ActiveMap, std::move(before), std::move(after), ++m_NextStep});
        m_Redo.clear();
        ++m_Version;
        return true;
    }

    bool ClothPaintSession::Undo()
    {
        if (m_Undo.empty() || !Write(m_Undo.back().Map, m_Undo.back().Before))
            return false;
        m_Redo.push_back(std::move(m_Undo.back()));
        m_Undo.pop_back();
        ++m_Version;
        return true;
    }

    bool ClothPaintSession::Redo()
    {
        if (m_Redo.empty() || !Write(m_Redo.back().Map, m_Redo.back().After))
            return false;
        m_Undo.push_back(std::move(m_Redo.back()));
        m_Redo.pop_back();
        ++m_Version;
        return true;
    }

    std::optional<AssetError> ClothPaintSession::Save()
    {
        if (!IsActive())
            return Error(AssetErrorCode::InvalidArgument, "Not painting");
        if (auto error = SaveClothFile(m_File, m_Data))
            return error;
        m_SavedStep = CurrentStep();
        return std::nullopt;
    }

    Ref<ClothAsset> ClothPaintSession::MakeAsset() const
    {
        auto asset     = CreateRef<ClothAsset>(m_Data);
        asset->assetID = m_Asset;
        return asset;
    }

    const std::vector<glm::vec3>& ClothPaintSession::UpdatePose(Scene& scene, Entity entity, Ref<const MeshAsset> mesh)
    {
        if (!mesh || !entity || !ValidateClothBinding(m_Data, m_Data.Mesh, mesh->GetData(), m_Data.Section))
        {
            m_Positions = m_Data.RestPositions;
            return m_Positions;
        }
        if (m_PoseVersion != m_Version || m_Pose.Mesh != mesh)
        {
            m_Pose.Initialize(MakeAsset(), mesh);
            m_PoseVersion = m_Version;
        }
        const Entity root    = scene.GetDeformationRoot(entity);
        const bool   skinned = !mesh->GetData().Skin.empty();
        const auto   world   = scene.GetSpringBoneRenderTransform(skinned && root ? root : entity);
        m_Pose.SetFrame(scene.GetSkinningPose(entity).get(), world, glm::mat4(1.0f));
        m_Pose.GetSkinnedAnchors(m_Positions);
        return m_Positions;
    }

    // ---------------------------------------------------------------- .cloth files

    std::variant<ClothData, AssetError> LoadClothFile(const std::filesystem::path& file)
    {
        auto text = ReadText(file);
        if (auto* error = std::get_if<AssetError>(&text))
            return *error;
        try
        {
            return DecodeClothNode(YAML::Load(std::get<std::string>(text)));
        }
        catch (const std::exception& e)
        {
            return Error(AssetErrorCode::InvalidData, file.filename().u8string() + ": " + e.what());
        }
    }

    std::optional<AssetError> SaveClothFile(const std::filesystem::path& file, const ClothData& data)
    {
        std::string text;
        try
        {
            YAML::Emitter out;
            out << EncodeClothNode(data);
            text = std::string(out.c_str()) + "\n";
        }
        catch (const std::exception& e)
        {
            return Error(AssetErrorCode::InvalidData, e.what());
        }
        return WriteText(file, text);
    }

    std::variant<std::filesystem::path, AssetError> CreateClothFile(const std::filesystem::path& directory,
                                                                    const std::string&           stem,
                                                                    AssetID                      meshID,
                                                                    const MeshData&              mesh,
                                                                    uint32_t                     section)
    {
        ClothData data;
        try
        {
            data = CreateClothForSection(meshID, mesh, section);
        }
        catch (const std::exception& e)
        {
            return Error(AssetErrorCode::InvalidData, e.what());
        }
        const auto file = UniqueFile(directory, stem.empty() ? "Cloth" : stem, ".cloth");
        if (auto error = SaveClothFile(file, data))
            return *error;
        return file;
    }

    std::string BindingProblem(const ClothData& data, AssetID meshID, const MeshData& mesh, uint32_t section)
    {
        if (data.Mesh != meshID)
            return "The asset was made for another mesh";
        if (data.Section != section)
            return "The asset was made for section " + std::to_string(data.Section);
        if (section >= mesh.Sections.size())
            return "The mesh has no section " + std::to_string(section);
        if (data.VertexCount != mesh.Vertices.size())
            return "The vertex count changed (asset " + std::to_string(data.VertexCount) + ", mesh " +
                   std::to_string(mesh.Vertices.size()) + "); rebuild the .cloth asset";
        if (!ValidateClothBinding(data, meshID, mesh, section))
            return "The section's topology or skinning changed; rebuild the .cloth asset";
        return {};
    }

    std::vector<std::string> SectionLabels(const MeshData& mesh)
    {
        std::vector<std::string> labels;
        for (size_t i = 0; i < mesh.Sections.size(); ++i)
        {
            std::string label = "Section " + std::to_string(i);
            const auto  slot  = mesh.Sections[i].MaterialSlot;
            if (slot < mesh.MaterialSlots.size() && !mesh.MaterialSlots[slot].Name.empty())
                label += "  (" + mesh.MaterialSlots[slot].Name + ")";
            labels.push_back(std::move(label));
        }
        return labels;
    }

    // ---------------------------------------------------------------- presets

    std::filesystem::path BuiltInPresetDirectory()
    {
        return KansFileSystem::GetResoucesFolder() / "Presets" / "SecondaryMotion";
    }

    std::filesystem::path ProjectPresetDirectory(const std::filesystem::path& projectRoot)
    {
        return projectRoot / "Settings" / "Presets";
    }

    std::variant<MotionPreset, AssetError> LoadPresetFile(const std::filesystem::path& file, PresetOrigin origin)
    {
        auto text = ReadText(file);
        if (auto* error = std::get_if<AssetError>(&text))
            return *error;
        try
        {
            const YAML::Node root = YAML::Load(std::get<std::string>(text));
            if (!root.IsMap())
                throw std::invalid_argument("not a map");
            for (auto entry : root)
            {
                const auto key = entry.first.as<std::string>();
                if (key != "Order" && key != "SpringChain" && key != "SpringComponent" && key != "Cloth")
                    throw std::invalid_argument("unknown key " + key);
            }
            MotionPreset preset;
            preset.Name   = file.stem().u8string();
            preset.Label  = preset.Name;
            preset.Origin = origin;
            preset.File   = file;
            if (root["SpringChain"] || root["SpringComponent"])
            {
                YAML::Node component(YAML::NodeType::Map);
                if (root["SpringComponent"])
                {
                    RequireOnly(root["SpringComponent"], SpringComponentKeys(), "SpringComponent");
                    component = YAML::Clone(root["SpringComponent"]);
                }
                YAML::Node chains(YAML::NodeType::Sequence);
                if (root["SpringChain"])
                {
                    RequireOnly(root["SpringChain"], SpringChainKeys(), "SpringChain");
                    chains.push_back(YAML::Clone(root["SpringChain"]));
                }
                else
                    chains.push_back(YAML::Node(YAML::NodeType::Map));
                component["Chains"] = chains;
                SpringBoneComponent decoded;
                PhysicsYaml::Decode(component, decoded);
                preset.Spring = CaptureSpringPreset(decoded, 0);
            }
            if (root["Cloth"])
            {
                RequireOnly(root["Cloth"], ClothKeys(), "Cloth");
                ClothComponent decoded;
                PhysicsYaml::Decode(root["Cloth"], decoded);
                preset.Cloth = CaptureClothPreset(decoded);
            }
            if (!preset.Spring && !preset.Cloth)
                throw std::invalid_argument("no spring or cloth values");
            return preset;
        }
        catch (const std::exception& e)
        {
            return Error(AssetErrorCode::InvalidData, file.filename().u8string() + ": " + e.what());
        }
    }

    std::optional<AssetError> SavePresetFile(const std::filesystem::path& file, const MotionPreset& preset)
    {
        if (!preset.Spring && !preset.Cloth)
            return Error(AssetErrorCode::InvalidArgument, "A preset needs spring or cloth values");
        std::string text;
        try
        {
            YAML::Node root(YAML::NodeType::Map);
            if (preset.Spring)
            {
                SpringBoneComponent component;
                component.Chains.push_back(SpringBoneChain {});
                ApplySpringPreset(component, *preset.Spring);
                const YAML::Node encoded = PhysicsYaml::Encode(component);
                root["SpringChain"]      = Pick(encoded["Chains"][0], SpringChainKeys());
                root["SpringComponent"]  = Pick(encoded, SpringComponentKeys());
            }
            if (preset.Cloth)
            {
                ClothComponent component;
                ApplyClothPreset(component, *preset.Cloth);
                root["Cloth"] = Pick(PhysicsYaml::Encode(component), ClothKeys());
            }
            YAML::Emitter out;
            out << root;
            text = std::string(out.c_str()) + "\n";
        }
        catch (const std::exception& e)
        {
            return Error(AssetErrorCode::InvalidData, e.what());
        }
        return WriteText(file, text);
    }

    std::vector<MotionPreset> ListPresets(const std::filesystem::path& builtInDirectory,
                                          const std::filesystem::path& projectDirectory,
                                          std::vector<std::string>*    errors)
    {
        const auto scan = [&](const std::filesystem::path& directory, PresetOrigin origin) {
            std::vector<std::pair<int, MotionPreset>> found;
            std::error_code                           ec;
            if (directory.empty() || !std::filesystem::is_directory(directory, ec))
                return found;
            for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
            {
                if (!entry.is_regular_file(ec) || entry.path().extension() != PresetExtension)
                    continue;
                auto loaded = LoadPresetFile(entry.path(), origin);
                if (auto* error = std::get_if<AssetError>(&loaded))
                {
                    if (errors)
                        errors->push_back(error->Message);
                    continue;
                }
                int order = std::numeric_limits<int>::max();
                if (origin == PresetOrigin::BuiltIn)
                    try
                    {
                        if (auto node = YAML::LoadFile(entry.path().string())["Order"])
                            order = node.as<int>();
                    }
                    catch (const std::exception&)
                    {
                    }
                found.emplace_back(order, std::move(std::get<MotionPreset>(loaded)));
            }
            std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
                return a.first != b.first ? a.first < b.first : Lower(a.second.Name) < Lower(b.second.Name);
            });
            return found;
        };
        std::vector<MotionPreset> presets;
        std::set<std::string>     builtInNames;
        for (auto& [order, preset] : scan(builtInDirectory, PresetOrigin::BuiltIn))
        {
            builtInNames.insert(Lower(preset.Name));
            presets.push_back(std::move(preset));
        }
        for (auto& [order, preset] : scan(projectDirectory, PresetOrigin::Project))
        {
            if (builtInNames.count(Lower(preset.Name)))
                preset.Label = preset.Name + " (project)";
            presets.push_back(std::move(preset));
        }
        return presets;
    }

    std::optional<std::string> PresetNameProblem(const std::string& name, const std::vector<MotionPreset>& existing)
    {
        if (name.empty() || name.size() > 64)
            return "Give the preset a name (up to 64 characters)";
        if (name.front() == ' ' || name.back() == ' ' || name.front() == '.')
            return "A preset name cannot start or end with a space or start with a dot";
        for (unsigned char c : name)
            if (c < 32 || std::string("\\/:*?\"<>|").find(char(c)) != std::string::npos)
                return "A preset name cannot contain \\ / : * ? \" < > |";
        for (const auto& preset : existing)
            if (Lower(preset.Name) == Lower(name))
                return preset.Origin == PresetOrigin::BuiltIn ? "\"" + preset.Name + "\" is a built-in preset" :
                                                                "A project preset is already called \"" + preset.Name + "\"";
        return std::nullopt;
    }

    std::variant<std::filesystem::path, AssetError> SaveProjectPreset(const std::filesystem::path&     projectDirectory,
                                                                      const std::string&               name,
                                                                      MotionPreset                     preset,
                                                                      const std::vector<MotionPreset>& existing)
    {
        if (auto problem = PresetNameProblem(name, existing))
            return Error(AssetErrorCode::InvalidArgument, *problem);
        const auto file = projectDirectory / std::filesystem::u8path(name + PresetExtension);
        if (std::filesystem::exists(file))
            return Error(AssetErrorCode::DuplicatePath, file.filename().u8string() + " already exists");
        if (auto error = SavePresetFile(file, preset))
            return *error;
        return file;
    }

    std::optional<AssetError> RenameProjectPreset(const MotionPreset&              preset,
                                                  const std::string&               name,
                                                  const std::vector<MotionPreset>& existing)
    {
        if (preset.Origin != PresetOrigin::Project)
            return Error(AssetErrorCode::InvalidArgument, "Built-in presets cannot be renamed");
        if (name == preset.Name)
            return std::nullopt;
        std::vector<MotionPreset> others;
        for (const auto& p : existing)
            if (!(p.Origin == PresetOrigin::Project && p.File == preset.File))
                others.push_back(p);
        if (auto problem = PresetNameProblem(name, others))
            return Error(AssetErrorCode::InvalidArgument, *problem);
        std::error_code ec;
        const auto      target = preset.File.parent_path() / std::filesystem::u8path(name + PresetExtension);
        // A case-only rename on a case-insensitive file system is the same file.
        if (std::filesystem::exists(target, ec) && !std::filesystem::equivalent(target, preset.File, ec))
            return Error(AssetErrorCode::DuplicatePath, target.filename().u8string() + " already exists");
        std::filesystem::rename(preset.File, target, ec);
        if (ec)
            return Error(AssetErrorCode::IoError, "Cannot rename " + preset.File.filename().u8string());
        return std::nullopt;
    }

    std::optional<AssetError> DeleteProjectPreset(const MotionPreset& preset)
    {
        if (preset.Origin != PresetOrigin::Project)
            return Error(AssetErrorCode::InvalidArgument, "Built-in presets cannot be deleted");
        std::error_code ec;
        if (!std::filesystem::remove(preset.File, ec) || ec)
            return Error(AssetErrorCode::IoError, "Cannot delete " + preset.File.filename().u8string());
        return std::nullopt;
    }

    void ApplySpringPreset(SpringBoneChain& chain, const SpringPreset& preset)
    {
        const auto& p                = preset.Chain;
        chain.Stiffness              = p.Stiffness;
        chain.Drag                   = p.Drag;
        chain.GravityPower           = p.GravityPower;
        chain.GravityDirection       = p.GravityDirection;
        chain.HitRadius              = p.HitRadius;
        chain.RotationAxis           = p.RotationAxis;
        chain.AngleLimit             = p.AngleLimit;
        chain.StiffnessCurve         = p.StiffnessCurve;
        chain.DragCurve              = p.DragCurve;
        chain.GravityPowerCurve      = p.GravityPowerCurve;
        chain.GravityDirectionCurve  = p.GravityDirectionCurve;
        chain.HitRadiusCurve         = p.HitRadiusCurve;
        chain.RotationAxisCurve      = p.RotationAxisCurve;
        chain.AngleLimitCurve        = p.AngleLimitCurve;
    }

    void ApplySpringPreset(SpringBoneComponent& component, const SpringPreset& preset)
    {
        for (auto& chain : component.Chains)
            ApplySpringPreset(chain, preset);
        component.InertiaScale = preset.InertiaScale;
        component.WindScale    = preset.WindScale;
        component.SubstepRate  = preset.SubstepRate;
        component.MaxSubsteps  = preset.MaxSubsteps;
    }

    void ApplyClothPreset(ClothSettings& s, const ClothSettings& p)
    {
        s.StretchCompliance   = p.StretchCompliance;
        s.BendCompliance      = p.BendCompliance;
        s.TetherScale         = p.TetherScale;
        s.Damping             = p.Damping;
        s.VelocityDrag        = p.VelocityDrag;
        s.ParticleMass        = p.ParticleMass;
        s.AreaDensity         = p.AreaDensity;
        s.GravityScale        = p.GravityScale;
        s.WindScale           = p.WindScale;
        s.Lift                = p.Lift;
        s.Drag                = p.Drag;
        s.InertiaScale        = p.InertiaScale;
        s.LinearInertiaLimit  = p.LinearInertiaLimit;
        s.AngularInertiaLimit = p.AngularInertiaLimit;
        s.SubstepRate         = p.SubstepRate;
        s.MaxSubsteps         = p.MaxSubsteps;
        s.Iterations          = p.Iterations;
        s.Thickness           = p.Thickness;
    }

    SpringPreset CaptureSpringPreset(const SpringBoneComponent& component, size_t chain)
    {
        SpringPreset preset;
        if (chain < component.Chains.size())
            ApplySpringPreset(preset.Chain, SpringPreset {component.Chains[chain]});
        preset.InertiaScale = component.InertiaScale;
        preset.WindScale    = component.WindScale;
        preset.SubstepRate  = component.SubstepRate;
        preset.MaxSubsteps  = component.MaxSubsteps;
        return preset;
    }

    ClothSettings CaptureClothPreset(const ClothSettings& settings)
    {
        ClothSettings preset;
        ApplyClothPreset(preset, settings);
        return preset;
    }

    // ---------------------------------------------------------------- preview

    void SetPreview(Scene& scene, bool enabled)
    {
        if (scene.IsSecondaryMotionPreviewEnabled() != enabled)
            scene.SetSecondaryMotionPreviewEnabled(enabled);
    }

    void StepPreview(Scene& scene, double seconds)
    {
        if (!scene.IsSecondaryMotionPreviewEnabled())
            return;
        scene.UpdateSpringBones(seconds);
        scene.UpdateCloth(seconds);
    }
} // namespace Kans::SecondaryMotionEditing

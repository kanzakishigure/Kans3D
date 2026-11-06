#include "PhysicsEditing.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <yaml-cpp/yaml.h>
#include "Kans3D/Asset/Runtime/MeshData.h"
#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "Kans3D/Physics/PhysicsEquality.h"
#include "Kans3D/Physics/PhysicsMaterial.h"
#include "Kans3D/Project/ProjectFile.h"
#include "Kans3D/Renderer/DebugDraw.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"

namespace Kans::PhysicsEditing
{
    // ---------------------------------------------------------------- layers

    int ColliderLayerIndex(const std::optional<uint32_t>& layer) { return layer && *layer < 32 ? int(*layer) + 1 : 0; }

    std::optional<uint32_t> ColliderLayerFromIndex(int index)
    {
        if (index <= 0 || index > 32)
            return std::nullopt;
        return uint32_t(index - 1);
    }

    std::vector<std::string> LayerLabels(const PhysicsSettings& settings)
    {
        std::vector<std::string> labels;
        labels.reserve(32);
        for (uint32_t i = 0; i < 32; ++i)
            labels.push_back(std::to_string(i) + "  " + settings.LayerNames[i]);
        return labels;
    }

    std::string MaskSummary(uint32_t mask, const PhysicsSettings& settings)
    {
        if (mask == UINT32_MAX)
            return "Everything";
        if (mask == 0)
            return "Nothing";
        int      count = 0;
        uint32_t last  = 0;
        for (uint32_t i = 0; i < 32; ++i)
            if (mask & (1u << i))
                ++count, last = i;
        if (count == 1)
            return settings.LayerNames[last];
        return "Mixed (" + std::to_string(count) + " layers)";
    }

    std::vector<uint32_t> MatrixLayers(const PhysicsSettings& settings, bool all)
    {
        const PhysicsSettings defaults;
        std::vector<uint32_t> layers;
        for (uint32_t i = 0; i < 32; ++i)
            if (all || i == 0 || settings.LayerNames[i] != defaults.LayerNames[i])
                layers.push_back(i);
        return layers;
    }

    void ToggleLayerCollision(PhysicsSettings& settings, uint32_t a, uint32_t b)
    {
        settings.SetLayerCollision(a, b, !settings.LayersCollide(a, b));
    }

    // ---------------------------------------------------------------- materials

    MaterialSource EffectiveMaterialSource(const ColliderMaterial& material, bool assetLoaded)
    {
        if (material.Asset.IsValid())
            return assetLoaded ? MaterialSource::Asset : MaterialSource::ProjectDefault;
        return material.UseDefault ? MaterialSource::ProjectDefault : MaterialSource::Inline;
    }

    const char* MaterialSourceName(MaterialSource source)
    {
        switch (source)
        {
            case MaterialSource::Asset:
                return "Physics material asset";
            case MaterialSource::Inline:
                return "Inline values";
            default:
                return "Project default";
        }
    }

    // ---------------------------------------------------------------- joints

    uint32_t JointGroups(JointType type)
    {
        switch (type)
        {
            case JointType::Hinge:
                return HingeLimit | HingeMotor | HingeSpring;
            case JointType::Slider:
                return SliderLimit | SliderDrive;
            case JointType::Spherical:
                return SphericalCone;
            case JointType::Distance:
                return DistanceRange;
            case JointType::D6:
                return D6Motion | D6Limits | D6Drives;
            default:
                return 0;
        }
    }

    Entity ResolveBodyA(Scene& scene, Entity joint)
    {
        if (!joint || !joint.HasComponent<JointComponent>())
            return {};
        const UUID set = joint.GetComponent<JointComponent>().BodyA;
        if (uint64_t(set))
            return scene.GetEntityByUUID(set);
        return AutoBodyA(scene, joint);
    }

    Entity AutoBodyA(Scene& scene, Entity joint)
    {
        for (Entity e = joint; e; e = scene.GetParent(e))
            if (e.HasComponent<RigidBodyComponent>())
                return e;
        return {};
    }

    // ---------------------------------------------------------------- shape handles

    namespace
    {
        glm::vec3 Unit(int axis, float sign)
        {
            glm::vec3 v(0.0f);
            v[axis] = sign;
            return v;
        }
        // Moves the shape's offset by `local` (offset space) as its matrix would.
        void Shift(Physics::ShapeOffset& offset, const Physics::ShapeOffset& start, glm::vec3 local)
        {
            offset.Position = start.Position + start.Rotation * (start.Scale * local);
        }
    } // namespace

    std::vector<ShapeHandle> Handles(const Physics::Box& box)
    {
        std::vector<ShapeHandle> handles;
        const glm::vec3          half = glm::abs(box.HalfExtents);
        for (int axis = 0; axis < 3; ++axis)
            for (int positive = 0; positive < 2; ++positive)
            {
                const glm::vec3 direction = Unit(axis, positive ? 1.0f : -1.0f);
                handles.push_back({axis * 2 + positive, direction * half[axis], direction});
            }
        return handles;
    }

    std::vector<ShapeHandle> Handles(const Physics::Sphere& sphere)
    {
        std::vector<ShapeHandle> handles;
        for (int axis = 0; axis < 3; ++axis)
            for (int positive = 0; positive < 2; ++positive)
            {
                const glm::vec3 direction = Unit(axis, positive ? 1.0f : -1.0f);
                handles.push_back({axis * 2 + positive, direction * std::abs(sphere.Radius), direction});
            }
        return handles;
    }

    std::vector<ShapeHandle> Handles(const Physics::Capsule& capsule)
    {
        std::vector<ShapeHandle> handles;
        const int                axis   = int(capsule.Direction);
        const float              radius = std::abs(capsule.Radius), cap = std::abs(capsule.HalfHeight) + radius;
        for (int positive = 0; positive < 2; ++positive)
        {
            const glm::vec3 direction = Unit(axis, positive ? 1.0f : -1.0f);
            handles.push_back({positive, direction * cap, direction});
        }
        int id = 2;
        for (int side : {(axis + 1) % 3, (axis + 2) % 3})
            for (int positive = 0; positive < 2; ++positive)
            {
                const glm::vec3 direction = Unit(side, positive ? 1.0f : -1.0f);
                handles.push_back({id++, direction * radius, direction});
            }
        return handles;
    }

    Physics::Box DragHandle(const Physics::Box& start, int id, float distance)
    {
        Physics::Box result = start;
        if (id < 0 || id > 5 || !std::isfinite(distance))
            return result;
        const int   axis         = id / 2;
        const float sign         = id % 2 ? 1.0f : -1.0f;
        const float before       = std::abs(start.HalfExtents[axis]);
        const float after        = std::max(MinimumSize, before + distance * 0.5f);
        result.HalfExtents       = glm::abs(start.HalfExtents);
        result.HalfExtents[axis] = after;
        // The opposite face stays where it was: the centre moves half the change.
        Shift(result.Offset, start.Offset, Unit(axis, sign * (after - before)));
        return result;
    }

    Physics::Sphere DragHandle(const Physics::Sphere& start, int id, float distance)
    {
        Physics::Sphere result = start;
        if (id < 0 || id > 5 || !std::isfinite(distance))
            return result;
        result.Radius = std::max(MinimumSize, std::abs(start.Radius) + distance);
        return result;
    }

    Physics::Capsule DragHandle(const Physics::Capsule& start, int id, float distance)
    {
        Physics::Capsule result = start;
        if (id < 0 || id > 5 || !std::isfinite(distance))
            return result;
        if (id >= 2)
        {
            result.Radius = std::max(MinimumSize, std::abs(start.Radius) + distance);
            return result;
        }
        const int   axis   = int(start.Direction);
        const float sign   = id == 1 ? 1.0f : -1.0f;
        const float before = std::abs(start.HalfHeight);
        const float after  = std::max(0.0f, before + distance * 0.5f);
        result.HalfHeight  = after;
        Shift(result.Offset, start.Offset, Unit(axis, sign * (after - before)));
        return result;
    }

    // ---------------------------------------------------------------- hinge handles

    glm::mat4 JointFrame(const glm::mat4& world)
    {
        const auto pose = Physics::TransformShape({}, world);
        return glm::translate(glm::mat4(1.0f), pose.Position) * glm::mat4_cast(pose.Rotation);
    }

    glm::vec3 HingeArcPoint(const glm::mat4& frame, float degrees)
    {
        const float angle = glm::radians(degrees);
        return glm::vec3(frame *
                         glm::vec4(0.0f, LimitArcRadius * std::cos(angle), LimitArcRadius * std::sin(angle), 1.0f));
    }

    std::optional<float> HingeAngleAt(const glm::mat4& frame, glm::vec3 origin, glm::vec3 direction)
    {
        const glm::vec3 normal = glm::normalize(glm::vec3(frame[0]));
        const glm::vec3 centre = glm::vec3(frame[3]);
        const float     facing = glm::dot(direction, normal);
        if (std::abs(facing) < 1e-6f)
            return std::nullopt;
        const float t = glm::dot(centre - origin, normal) / facing;
        if (!(t >= 0.0f))
            return std::nullopt;
        const glm::vec3 offset = origin + direction * t - centre;
        const float     y = glm::dot(offset, glm::vec3(frame[1])), z = glm::dot(offset, glm::vec3(frame[2]));
        if (std::abs(y) < 1e-9f && std::abs(z) < 1e-9f)
            return std::nullopt;
        return glm::degrees(std::atan2(z, y));
    }

    // ---------------------------------------------------------------- gizmo lines

    namespace
    {
        template<typename T>
        void DrawPrimitive(DebugDraw& draw, Entity entity, const glm::mat4& world, glm::vec4 color)
        {
            if (entity.HasComponent<T>())
                draw.Shape(entity.GetComponent<T>().Shape, world, color);
        }
    } // namespace

    void DrawColliders(DebugDraw& draw, Scene& scene, Entity entity, glm::vec4 color)
    {
        if (!entity)
            return;
        const glm::mat4 world = scene.GetWorldTransform(entity);
        DrawPrimitive<BoxColliderComponent>(draw, entity, world, color);
        DrawPrimitive<SphereColliderComponent>(draw, entity, world, color);
        DrawPrimitive<CapsuleColliderComponent>(draw, entity, world, color);
        if (entity.HasComponent<MeshColliderComponent>())
        {
            // Mesh colliders show the bounds of their points or mesh.
            const auto& collider = entity.GetComponent<MeshColliderComponent>();
            Bounds      bounds;
            if (collider.Shape.Points.empty())
                if (auto mesh = scene.ResolveMesh(entity))
                    bounds = mesh->GetData().LocalBounds;
            draw.Shape(collider.Shape, world, color, bounds);
        }
    }

    void DrawController(DebugDraw& draw, Scene& scene, Entity entity, glm::vec4 color)
    {
        if (!entity || !entity.HasComponent<CharacterControllerComponent>())
            return;
        const auto&     c      = entity.GetComponent<CharacterControllerComponent>();
        const glm::vec3 centre = glm::vec3(scene.GetWorldTransform(entity)[3]) + c.Center;
        const glm::vec3 up  = glm::length(c.UpDirection) > 1e-6f ? glm::normalize(c.UpDirection) : glm::vec3(0, 1, 0);
        float           top = 0.0f;
        if (c.Shape == ControllerShape::Capsule)
        {
            const glm::vec3 half = up * (c.Height * 0.5f);
            draw.Capsule(centre - half, centre + half, c.Radius, color);
            top = c.Height * 0.5f + c.Radius;
        }
        else
        {
            // The box stands along the up direction (its local +Y).
            const glm::quat rotation(glm::vec3(0, 1, 0), up);
            draw.Box(
                glm::translate(glm::mat4(1.0f), centre) * glm::mat4_cast(rotation), glm::abs(c.HalfExtents), color);
            top = std::abs(c.HalfExtents.y);
        }
        draw.Arrow(centre + up * top, centre + up * (top + 0.5f), {0.4f, 1.0f, 0.4f, 1.0f});
    }

    void DrawJoint(DebugDraw& draw, Scene& scene, Entity entity)
    {
        if (!entity || !entity.HasComponent<JointComponent>())
            return;
        const auto&     c      = entity.GetComponent<JointComponent>();
        const glm::mat4 frame  = JointFrame(scene.GetWorldTransform(entity));
        const glm::vec3 origin = glm::vec3(frame[3]);
        const glm::vec4 color {1.0f, 0.8f, 0.1f, 1.0f};
        const auto      at = [&](glm::vec3 local) { return glm::vec3(frame * glm::vec4(local, 1.0f)); };
        draw.Arrow(origin, at({0.4f, 0, 0}), {1, 0, 0, 1});
        draw.Arrow(origin, at({0, 0.4f, 0}), {0, 1, 0, 1});
        draw.Arrow(origin, at({0, 0, 0.4f}), {0, 0.4f, 1, 1});
        if (Entity a = ResolveBodyA(scene, entity))
            draw.Line(origin, glm::vec3(scene.GetWorldTransform(a)[3]), BodyAColor);
        if (Entity b = uint64_t(c.BodyB) ? scene.GetEntityByUUID(c.BodyB) : Entity {})
            draw.Line(origin, glm::vec3(scene.GetWorldTransform(b)[3]), BodyBColor);
        else
            draw.Cross(origin, 0.08f, BodyBColor); // Attached to the world.
        if (c.Type == JointType::Hinge || c.Type == JointType::D6)
        {
            const float low  = c.Type == JointType::Hinge ? c.HingeLowerAngle : c.D6TwistLower;
            const float high = c.Type == JointType::Hinge ? c.HingeUpperAngle : c.D6TwistUpper;
            for (int n = 0; n < 32; ++n)
                draw.Line(HingeArcPoint(frame, glm::mix(low, high, float(n) / 32)),
                          HingeArcPoint(frame, glm::mix(low, high, float(n + 1) / 32)),
                          color);
            draw.Line(origin, HingeArcPoint(frame, low), color);
            draw.Line(origin, HingeArcPoint(frame, high), color);
        }
        if (c.Type == JointType::Spherical || c.Type == JointType::D6)
        {
            const float y     = glm::radians(c.Type == JointType::Spherical ? c.SphericalYAngle : c.D6SwingY);
            const float z     = glm::radians(c.Type == JointType::Spherical ? c.SphericalZAngle : c.D6SwingZ);
            const auto  point = [&](float angle) {
                const glm::quat swing = glm::angleAxis(y * std::sin(angle), glm::vec3(0, 1, 0)) *
                                        glm::angleAxis(z * std::cos(angle), glm::vec3(0, 0, 1));
                return at(swing * glm::vec3(LimitArcRadius, 0, 0));
            };
            for (int n = 0; n < 32; ++n)
            {
                draw.Line(point(n * glm::two_pi<float>() / 32), point((n + 1) * glm::two_pi<float>() / 32), color);
                if (n % 8 == 0)
                    draw.Line(origin, point(n * glm::two_pi<float>() / 32), color);
            }
        }
        if (c.Type == JointType::Slider)
            draw.Line(at({c.SliderLowerLimit, 0, 0}), at({c.SliderUpperLimit, 0, 0}), color);
        if (c.Type == JointType::Distance)
        {
            // The allowed distance range of BodyB from the joint.
            if (c.DistanceMaxEnabled)
                draw.Sphere(origin, c.DistanceMax, color);
            if (c.DistanceMinEnabled && c.DistanceMin > 0.0f)
                draw.Sphere(origin, c.DistanceMin, color * glm::vec4(1, 1, 1, 0.5f));
        }
    }

    // ---------------------------------------------------------------- .physmat files

    std::variant<PhysicsMaterialValues, AssetError> LoadPhysicsMaterialFile(const std::filesystem::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return AssetError {AssetErrorCode::IoError, "Cannot read " + file.u8string()};
        std::stringstream text;
        text << in.rdbuf();
        try
        {
            return DecodePhysicsMaterialNode(YAML::Load(text.str()));
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, file.filename().u8string() + ": " + e.what()};
        }
    }

    std::optional<AssetError> SavePhysicsMaterialFile(const std::filesystem::path& file,
                                                      const PhysicsMaterialValues& values)
    {
        std::string text;
        try
        {
            YAML::Emitter out;
            out << EncodePhysicsMaterialNode(values);
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

    std::variant<std::filesystem::path, AssetError> CreatePhysicsMaterialFile(const std::filesystem::path& directory)
    {
        auto file = directory / "NewPhysicsMaterial.physmat";
        for (int i = 2; std::filesystem::exists(file); ++i)
            file = directory / ("NewPhysicsMaterial" + std::to_string(i) + ".physmat");
        if (auto error = SavePhysicsMaterialFile(file, {}))
            return *error;
        return file;
    }

    // ---------------------------------------------------------------- project settings

    std::optional<AssetError> ProjectPhysicsSettings::Load(const std::filesystem::path& projectFile)
    {
        auto loaded = LoadProjectFile(projectFile);
        if (const auto* error = std::get_if<AssetError>(&loaded))
            return *error;
        m_File     = projectFile;
        m_Settings = std::get<ProjectFile>(loaded).Physics;
        m_Dirty    = false;
        return std::nullopt;
    }

    std::optional<std::string> ProjectPhysicsSettings::Apply(const PhysicsSettings& settings)
    {
        auto validated = settings;
        try
        {
            validated.Validate();
        }
        catch (const std::exception& e)
        {
            return std::string(e.what());
        }
        KansFileSystem::SetProjectPhysicsSettings(validated);
        if (!(validated == m_Settings))
            m_Dirty = true;
        m_Settings = std::move(validated);
        return std::nullopt;
    }

    std::optional<AssetError> ProjectPhysicsSettings::Save()
    {
        if (m_File.empty())
            return AssetError {AssetErrorCode::InvalidArgument, "No project file to save physics settings to"};
        auto loaded = LoadProjectFile(m_File);
        if (const auto* error = std::get_if<AssetError>(&loaded))
            return *error;
        auto project    = std::get<ProjectFile>(std::move(loaded));
        project.Physics = m_Settings;
        if (auto error = SaveProjectFile(m_File, project))
            return error;
        m_Dirty = false;
        return std::nullopt;
    }
} // namespace Kans::PhysicsEditing

#include "KansEditorCore/Physics/VehicleFieldEditing.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <functional>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include "Kans3D/Asset/Runtime/MeshData.h"
#include "Kans3D/Renderer/DebugDraw.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"
#include "KansEditorCore/Physics/SecondaryMotionEditing.h"

namespace Kans::VehicleFieldEditing
{
    // ---------------------------------------------------------------- wheels

    bool IsWheelName(const std::string& name)
    {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return lower.find("wheel") != std::string::npos || lower.find("tire") != std::string::npos ||
               lower.find("tyre") != std::string::npos;
    }

    glm::mat4 ChassisFrame(Scene& scene, Entity vehicle) { return PhysicsEditing::JointFrame(scene.GetWorldTransform(vehicle)); }

    std::vector<VehicleWheel> WheelsFromChildren(Scene& scene, Entity vehicle, const VehicleComponent& component)
    {
        std::vector<VehicleWheel> wheels;
        if (!vehicle)
            return wheels;
        const glm::mat4 toVehicle = glm::inverse(ChassisFrame(scene, vehicle));
        std::function<void(Entity)> visit = [&](Entity e) {
            for (Entity child : scene.GetChildren(e))
            {
                visit(child);
                if (!IsWheelName(child.GetComponent<TagComponent>().Tag))
                    continue;
                const UUID id = child.GetUUID();
                if (std::any_of(component.Wheels.begin(), component.Wheels.end(), [&](const auto& w) { return w.Visual == id; }))
                    continue;
                const glm::mat4 local = toVehicle * scene.GetWorldTransform(child);
                VehicleWheel    wheel;
                wheel.Visual         = id;
                wheel.AttachPosition = glm::vec3(local[3]);
                if (auto mesh = scene.ResolveMesh(child))
                {
                    // The bounds in the vehicle's frame: the axle runs along X, the radius across Y and Z.
                    const auto& bounds = mesh->GetData().LocalBounds;
                    glm::vec3   low(FLT_MAX), high(-FLT_MAX);
                    for (int corner = 0; corner < 8; ++corner)
                    {
                        const glm::vec3 p((corner & 1) ? bounds.Max.x : bounds.Min.x,
                                          (corner & 2) ? bounds.Max.y : bounds.Min.y,
                                          (corner & 4) ? bounds.Max.z : bounds.Min.z);
                        const glm::vec3 v = glm::vec3(local * glm::vec4(p, 1.0f)) - wheel.AttachPosition;
                        low = glm::min(low, v), high = glm::max(high, v);
                    }
                    const glm::vec3 size = high - low;
                    if (size.y > 0.0f && size.z > 0.0f)
                    {
                        wheel.Radius = std::max(size.y, size.z) * 0.5f;
                        wheel.Width  = std::max(0.01f, size.x);
                    }
                }
                wheels.push_back(wheel);
            }
        };
        visit(vehicle);
        // Axles from front (+Z) to back, wheels within a few centimetres sharing one.
        std::vector<float> axles;
        for (const auto& w : wheels)
            if (std::none_of(axles.begin(), axles.end(), [&](float z) { return std::abs(z - w.AttachPosition.z) < 0.05f; }))
                axles.push_back(w.AttachPosition.z);
        std::sort(axles.begin(), axles.end(), std::greater<float>());
        for (auto& w : wheels)
        {
            for (size_t a = 0; a < axles.size(); ++a)
                if (std::abs(axles[a] - w.AttachPosition.z) < 0.05f)
                    w.Axle = uint32_t(a);
            w.MaxSteerAngle   = w.Axle == 0 && axles.size() > 1 ? 30.0f : 0.0f;
            w.HandbrakeTorque = axles.size() > 1 && w.Axle + 1 == axles.size() ? 3000.0f : 0.0f;
        }
        return wheels;
    }

    size_t AddWheelsFromChildren(Scene& scene, Entity vehicle, VehicleComponent& component)
    {
        const auto wheels = WheelsFromChildren(scene, vehicle, component);
        component.Wheels.insert(component.Wheels.end(), wheels.begin(), wheels.end());
        return wheels.size();
    }

    // ---------------------------------------------------------------- gearbox

    bool GearsOrdered(const std::vector<float>& ratios)
    {
        for (size_t i = 0; i < ratios.size(); ++i)
            if (!(ratios[i] >= MinimumGearRatio) || (i && !(ratios[i] < ratios[i - 1])))
                return false;
        return true;
    }

    void AddGear(std::vector<float>& ratios)
    {
        if (ratios.empty())
            ratios.push_back(1.0f);
        else
            ratios.push_back(std::max(MinimumGearRatio, ratios.back() * 0.8f));
        // Gears already at the minimum make room: each one above stays a step higher than the next.
        for (size_t i = ratios.size() - 1; i > 0; --i)
            ratios[i - 1] = std::max(ratios[i - 1], ratios[i] + 0.01f);
    }

    bool RemoveGear(std::vector<float>& ratios, size_t index)
    {
        if (ratios.size() <= 1 || index >= ratios.size())
            return false;
        ratios.erase(ratios.begin() + std::ptrdiff_t(index));
        return true;
    }

    void SetGearRatio(std::vector<float>& ratios, size_t index, float ratio)
    {
        if (index >= ratios.size() || !std::isfinite(ratio))
            return;
        constexpr float gap  = 1e-3f;
        const float     low  = index + 1 < ratios.size() ? ratios[index + 1] + gap : MinimumGearRatio;
        const float     high = index > 0 ? ratios[index - 1] - gap : 1000.0f;
        ratios[index]        = std::clamp(ratio, low, std::max(low, high));
    }

    void ResetTorqueCurve(TimelineCurve& curve)
    {
        // A typical petrol engine: weak at idle, peak torque two thirds up, falling off at the limiter.
        SecondaryMotionEditing::ResetCurve(curve, glm::vec4(0.6f));
        SecondaryMotionEditing::MoveCurveKey(curve, 1, 1.0, glm::vec4(0.75f));
        const size_t peak = SecondaryMotionEditing::InsertCurveKey(curve, 0.65, glm::vec4(1.0f));
        SecondaryMotionEditing::MoveCurveKey(curve, peak, 0.65, glm::vec4(1.0f));
    }

    void ClampTorqueCurve(TimelineCurve& curve)
    {
        for (auto& key : curve.Keys)
        {
            key.Time  = std::clamp(key.Time, 0.0, 1.0);
            key.Value = glm::clamp(key.Value, glm::vec4(0.0f), glm::vec4(1.0f));
        }
        std::stable_sort(curve.Keys.begin(), curve.Keys.end(), [](const auto& a, const auto& b) { return a.Time < b.Time; });
    }

    TimelineCurve FalloffCurveKeys(const std::array<float, 8>& samples)
    {
        TimelineCurve curve;
        for (size_t i = 0; i < samples.size(); ++i)
        {
            TimelineKey key;
            key.Time  = double(i) / 7.0;
            key.Value = glm::vec4(samples[i]);
            curve.Keys.push_back(key);
        }
        return curve;
    }

    std::array<float, 8> SampleFalloffCurve(const TimelineCurve& curve)
    {
        std::array<float, 8> samples {};
        for (size_t i = 0; i < samples.size(); ++i)
            samples[i] = std::max(0.0f, curve.Sample(double(i) / 7.0, glm::vec4(1.0f)).x);
        return samples;
    }

    // ---------------------------------------------------------------- wheel handles

    std::vector<PhysicsEditing::ShapeHandle> WheelHandles(const VehicleWheel& wheel)
    {
        const glm::vec3 at = wheel.AttachPosition;
        return {{0, at + glm::vec3(0, std::abs(wheel.Radius), 0), {0, 1, 0}},
                {1, at + glm::vec3(std::abs(wheel.Width) * 0.5f + 0.1f, 0, 0), {1, 0, 0}},
                {2, at - glm::vec3(0, std::abs(wheel.Radius) + 0.1f, 0), {0, 1, 0}},
                {3, at + glm::vec3(0, 0, std::abs(wheel.Radius) + 0.1f), {0, 0, 1}}};
    }

    VehicleWheel DragWheelHandle(const VehicleWheel& start, int id, float distance)
    {
        VehicleWheel result = start;
        if (!std::isfinite(distance) || id < 0 || id > 3)
            return result;
        if (id == 0)
            result.Radius = std::max(PhysicsEditing::MinimumSize * 10.0f, std::abs(start.Radius) + distance);
        else
            result.AttachPosition[id - 1] = start.AttachPosition[id - 1] + distance;
        return result;
    }

    // ---------------------------------------------------------------- field handles

    FieldShapeValues ShapeOf(const WindZoneComponent& wind)
    {
        return {wind.Shape, wind.Radius, wind.FalloffBand, wind.HalfExtents, true};
    }

    FieldShapeValues ShapeOf(const ForceFieldComponent& field)
    {
        return {field.Shape, field.Radius, 0.0f, field.HalfExtents, false};
    }

    void Apply(WindZoneComponent& wind, const FieldShapeValues& values)
    {
        wind.Radius      = values.Radius;
        wind.HalfExtents = values.HalfExtents;
        wind.FalloffBand = values.Band;
    }

    void Apply(ForceFieldComponent& field, const FieldShapeValues& values)
    {
        field.Radius      = values.Radius;
        field.HalfExtents = values.HalfExtents;
    }

    std::vector<PhysicsEditing::ShapeHandle> FieldHandles(const FieldShapeValues& v)
    {
        std::vector<PhysicsEditing::ShapeHandle> handles;
        if (v.Shape == FieldShape::Sphere)
            handles = PhysicsEditing::Handles(Physics::Sphere {v.Radius, {}});
        else if (v.Shape == FieldShape::Box)
            handles = PhysicsEditing::Handles(Physics::Box {v.HalfExtents, {}});
        else
            return handles;
        if (v.HasBand)
        {
            const float outer = v.Shape == FieldShape::Sphere ? std::abs(v.Radius) : std::abs(v.HalfExtents.x);
            handles.push_back({BandHandle, glm::vec3(outer * (1.0f - std::clamp(v.Band, 0.0f, 1.0f)), 0, 0), {1, 0, 0}});
        }
        return handles;
    }

    FieldShapeValues DragFieldHandle(const FieldShapeValues& start, int id, float distance)
    {
        FieldShapeValues result = start;
        if (!std::isfinite(distance) || start.Shape == FieldShape::Infinite)
            return result;
        if (id == BandHandle)
        {
            if (!start.HasBand)
                return result;
            const float outer = start.Shape == FieldShape::Sphere ? std::abs(start.Radius) : std::abs(start.HalfExtents.x);
            if (outer <= 0.0f)
                return result;
            const float inner = std::clamp(outer * (1.0f - std::clamp(start.Band, 0.0f, 1.0f)) + distance, 0.0f, outer);
            result.Band       = 1.0f - inner / outer;
            return result;
        }
        if (id < 0 || id > 5)
            return result;
        if (start.Shape == FieldShape::Sphere)
            result.Radius = std::max(PhysicsEditing::MinimumSize, std::abs(start.Radius) + distance);
        else
        {
            // The field stays centred on its entity: both faces of the axis move.
            result.HalfExtents         = glm::abs(start.HalfExtents);
            result.HalfExtents[id / 2] = std::max(PhysicsEditing::MinimumSize, result.HalfExtents[id / 2] + distance);
        }
        return result;
    }

    // ---------------------------------------------------------------- gizmo lines

    namespace
    {
        void Circle(DebugDraw& draw, glm::vec3 centre, glm::vec3 u, glm::vec3 v, float radius, glm::vec4 color, int segments = 24)
        {
            for (int n = 0; n < segments; ++n)
            {
                const float a = n * glm::two_pi<float>() / segments, b = (n + 1) * glm::two_pi<float>() / segments;
                draw.Line(centre + (u * std::cos(a) + v * std::sin(a)) * radius,
                          centre + (u * std::cos(b) + v * std::sin(b)) * radius,
                          color);
            }
        }
    } // namespace

    void DrawVehicle(DebugDraw& draw, Scene& scene, Entity vehicle, int selectedWheel)
    {
        if (!vehicle || !vehicle.HasComponent<VehicleComponent>())
            return;
        const auto&     c     = vehicle.GetComponent<VehicleComponent>();
        const glm::mat4 frame = ChassisFrame(scene, vehicle);
        const glm::vec3 x = glm::vec3(frame[0]), y = glm::vec3(frame[1]), z = glm::vec3(frame[2]);
        for (size_t i = 0; i < c.Wheels.size(); ++i)
        {
            const auto&     w      = c.Wheels[i];
            const glm::vec4 color  = int(i) == selectedWheel ? SelectedWheelColor : WheelColor;
            const glm::vec3 centre = glm::vec3(frame * glm::vec4(w.AttachPosition, 1.0f));
            const float     half = std::abs(w.Width) * 0.5f, radius = std::abs(w.Radius);
            Circle(draw, centre - x * half, y, z, radius, color);
            Circle(draw, centre + x * half, y, z, radius, color);
            for (int k = 0; k < 4; ++k)
            {
                const float     a = k * glm::half_pi<float>();
                const glm::vec3 r = (y * std::cos(a) + z * std::sin(a)) * radius;
                draw.Line(centre - x * half + r, centre + x * half + r, color);
            }
            glm::vec3 down = glm::vec3(frame * glm::vec4(w.SuspensionDirection, 0.0f));
            if (glm::length(down) > 1e-6f)
                down = glm::normalize(down);
            draw.Line(centre, centre + down * w.SuspensionTravel, {1, 1, 0, 1});
            draw.Cross(centre + down * w.SuspensionTravel, 0.03f, {1, 1, 0, 1});
        }
    }

    void DrawField(DebugDraw& draw, Scene& scene, Entity entity)
    {
        if (!entity)
            return;
        const bool wind = entity.HasComponent<WindZoneComponent>(), field = entity.HasComponent<ForceFieldComponent>();
        if (!wind && !field)
            return;
        const glm::mat4 world = scene.GetWorldTransform(entity);
        const glm::mat4 frame = PhysicsEditing::JointFrame(world);
        const glm::vec3 scale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2])));
        const glm::vec3 centre = glm::vec3(frame[3]);
        FieldShapeValues v      = wind ? ShapeOf(entity.GetComponent<WindZoneComponent>()) :
                                         ShapeOf(entity.GetComponent<ForceFieldComponent>());
        const float maxScale    = std::max(scale.x, std::max(scale.y, scale.z));
        const auto  shape       = [&](float factor, glm::vec4 color) {
            if (v.Shape == FieldShape::Sphere)
                draw.Sphere(centre, std::abs(v.Radius) * maxScale * factor, color);
            else if (v.Shape == FieldShape::Box)
                draw.Box(frame, glm::abs(v.HalfExtents) * scale * factor, color);
        };
        shape(1.0f, FieldColor);
        if (v.Shape == FieldShape::Infinite)
            draw.Cross(centre, 0.3f, FieldColor);
        if (v.HasBand && v.Band > 0.0f && v.Shape != FieldShape::Infinite)
            shape(1.0f - std::clamp(v.Band, 0.0f, 1.0f), FieldColor * glm::vec4(1, 1, 1, 0.5f));
        const glm::vec3 local = wind ? entity.GetComponent<WindZoneComponent>().Direction :
                                       entity.GetComponent<ForceFieldComponent>().Direction;
        glm::vec3 axis = glm::vec3(frame * glm::vec4(local, 0.0f));
        if (glm::length(axis) < 1e-6f)
            return;
        axis              = glm::normalize(axis);
        const float reach = v.Shape == FieldShape::Sphere ? std::abs(v.Radius) * maxScale :
                            v.Shape == FieldShape::Box    ? glm::length(v.HalfExtents * scale) * 0.6f :
                                                            1.0f;
        const FieldType type = wind ? FieldType::Wind : entity.GetComponent<ForceFieldComponent>().Type;
        if (type == FieldType::Wind || type == FieldType::Directional)
            draw.Arrow(centre, centre + axis * reach, {1.0f, 0.9f, 0.3f, 1.0f});
        if (type == FieldType::Vortex)
        {
            // The rotation axis and a ring around it showing the swirl's sense (+ about the axis).
            draw.Line(centre - axis * reach, centre + axis * reach, {1.0f, 0.9f, 0.3f, 1.0f});
            const glm::vec3 u = glm::normalize(std::abs(axis.y) < 0.9f ? glm::cross(axis, glm::vec3(0, 1, 0)) :
                                                                         glm::cross(axis, glm::vec3(1, 0, 0)));
            const glm::vec3 w = glm::cross(axis, u);
            Circle(draw, centre, u, w, reach * 0.6f, {1.0f, 0.9f, 0.3f, 1.0f}, 32);
            const glm::vec3 at = centre + u * reach * 0.6f;
            draw.Arrow(at, at + w * reach * 0.25f, {1.0f, 0.9f, 0.3f, 1.0f});
        }
    }

    // ---------------------------------------------------------------- field arrows

    ForceFieldSet BuildFieldSet(Scene& scene, const std::unordered_set<UUID>& hidden)
    {
        ForceFieldSet set;
        std::function<void(Entity)> visit = [&](Entity e) {
            if (hidden.count(e.GetUUID()) || !scene.IsEntityActive(e))
                return;
            if (e.HasComponent<WindZoneComponent>())
                set.AddWind(e.GetComponent<WindZoneComponent>(), scene.GetWorldTransform(e));
            if (e.HasComponent<ForceFieldComponent>())
                set.AddField(e.GetComponent<ForceFieldComponent>(), scene.GetWorldTransform(e));
            for (Entity child : scene.GetChildren(e))
                visit(child);
        };
        for (Entity root : scene.GetRootEntities())
            visit(root);
        return set;
    }

    void DrawFieldArrows(DebugDraw& draw, const ForceFieldSet& fields, glm::vec3 centre, int n, float spacing, double time)
    {
        if (n <= 0 || !(spacing > 0.0f))
            return;
        const float start = -0.5f * float(n - 1);
        for (int x = 0; x < n; ++x)
            for (int y = 0; y < n; ++y)
                for (int z = 0; z < n; ++z)
                {
                    const glm::vec3 p = centre + (glm::vec3(float(x), float(y), float(z)) + start) * spacing;
                    const auto      s = fields.Sample(p, {}, time, 0, FieldSystem::RigidBodies);
                    const glm::vec3 v = s.Wind + s.Force + s.Acceleration;
                    if (glm::length(v) > 1e-6f)
                        draw.Arrow(p, p + glm::normalize(v) * spacing * 0.5f, FieldColor);
                }
    }
} // namespace Kans::VehicleFieldEditing

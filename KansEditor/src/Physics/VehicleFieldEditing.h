#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "Kans3D/Physics/ForceFields.h"
#include "Kans3D/Physics/VehicleComponent.h"
#include "Kans3D/Scene/Entity.h"
#include "PhysicsEditing.h"

namespace Kans
{
    class DebugDraw;
    class Scene;
} // namespace Kans

// The vehicle and force-field editors' logic, kept apart from ImGui so it can be tested headless:
// wheels from child entities, the gear table and torque curve rules, the wheel and field handles,
// their gizmo lines and the edit-mode field set the field arrows sample.
namespace Kans::VehicleFieldEditing
{
    // ---- Wheels ----

    // A child counts as a wheel when its name contains "wheel", "tire" or "tyre" (any case).
    bool IsWheelName(const std::string& name);
    // Wheels for the vehicle's descendants named as wheels that no wheel uses as its visual yet:
    // the attach position is the child's position in the vehicle's frame, the radius and width
    // come from its mesh bounds (the wheel turns about the vehicle's X axis). Axles go front (+Z)
    // to back; the front axle steers, the rear one has the handbrake.
    std::vector<VehicleWheel> WheelsFromChildren(Scene& scene, Entity vehicle, const VehicleComponent& component);
    // Appends them; how many were added.
    size_t AddWheelsFromChildren(Scene& scene, Entity vehicle, VehicleComponent& component);
    // The vehicle's frame: its world position and rotation, without scale (wheels attach in it).
    glm::mat4 ChassisFrame(Scene& scene, Entity vehicle);

    // ---- Gearbox and engine ----

    // Forward gears run from the highest ratio down. Adding appends a gear below the last one;
    // removing keeps at least one gear; setting a ratio keeps it between its neighbours.
    inline constexpr float MinimumGearRatio = 0.05f;
    void AddGear(std::vector<float>& ratios);
    bool RemoveGear(std::vector<float>& ratios, size_t index);
    void SetGearRatio(std::vector<float>& ratios, size_t index, float ratio);
    bool GearsOrdered(const std::vector<float>& ratios);
    // The torque curve over normalised RPM: keys in [0, 1] for both RPM and torque fraction.
    void ResetTorqueCurve(TimelineCurve& curve);
    void ClampTorqueCurve(TimelineCurve& curve);

    // A force field's Curve falloff is eight uniform samples over normalised distance; the curve
    // widget edits them as keys at i / 7, and the edited curve is sampled back at those points.
    TimelineCurve        FalloffCurveKeys(const std::array<float, 8>& samples);
    std::array<float, 8> SampleFalloffCurve(const TimelineCurve& curve);

    // ---- Wheel handles ----

    // Handle 0 sets the radius (on the wheel's top); 1..3 move the attach position along the
    // vehicle's X, Y and Z axes. Positions and directions are in the vehicle's frame.
    std::vector<PhysicsEditing::ShapeHandle> WheelHandles(const VehicleWheel& wheel);
    VehicleWheel                             DragWheelHandle(const VehicleWheel& start, int id, float distance);

    // ---- Field handles ----

    // Sphere: six radius handles; box: six faces that resize symmetrically (the field stays centred
    // on its entity); wind zones also get a falloff-band handle (ID 10) on the inner shape's +X.
    // Positions and directions are in the entity's frame. Infinite fields have none.
    struct FieldShapeValues
    {
        FieldShape Shape = FieldShape::Sphere;
        float      Radius = 1.0f, Band = 0.0f;
        glm::vec3  HalfExtents {1.0f};
        bool       HasBand = false;
    };
    FieldShapeValues ShapeOf(const WindZoneComponent& wind);
    FieldShapeValues ShapeOf(const ForceFieldComponent& field);
    void             Apply(WindZoneComponent& wind, const FieldShapeValues& values);
    void             Apply(ForceFieldComponent& field, const FieldShapeValues& values);
    inline constexpr int BandHandle = 10;
    std::vector<PhysicsEditing::ShapeHandle> FieldHandles(const FieldShapeValues& values);
    FieldShapeValues DragFieldHandle(const FieldShapeValues& start, int id, float distance);

    // ---- Gizmo lines ----

    inline constexpr glm::vec4 WheelColor {0.1f, 0.8f, 1.0f, 1.0f};
    inline constexpr glm::vec4 SelectedWheelColor {1.0f, 0.85f, 0.2f, 1.0f};
    inline constexpr glm::vec4 FieldColor {0.0f, 0.8f, 1.0f, 1.0f};
    // Each wheel as a cylinder at its attach position with the suspension travel as a line; the
    // selected wheel (-1: none) is highlighted.
    void DrawVehicle(DebugDraw& draw, Scene& scene, Entity vehicle, int selectedWheel);
    // The field's shape, its falloff band as an inner shape, the direction arrow (wind and
    // directional) and the rotation-axis ring (vortex).
    void DrawField(DebugDraw& draw, Scene& scene, Entity entity);

    // ---- Edit-mode field arrows ----

    // The fields of the scene's active entities, as the physics world builds them while playing.
    ForceFieldSet BuildFieldSet(Scene& scene);
    // An arrow per grid point (n x n x n, `spacing` apart, centred on `centre`) pointing along the
    // summed wind, force and acceleration a rigid body on layer 0 feels there.
    void DrawFieldArrows(DebugDraw&           draw,
                         const ForceFieldSet& fields,
                         glm::vec3            centre,
                         int                  n,
                         float                spacing,
                         double               time);
} // namespace Kans::VehicleFieldEditing

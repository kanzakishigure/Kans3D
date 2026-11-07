#include <imgui.h>
#include <algorithm>
#include <functional>
#include <optional>
#include "Kans3D/Scene/Components.h"
#include "ComponentEditors.h"
#include "UI/EditorIcons.h"
#include "UI/MaterialSlotRow.h"
#include "UI/PropertyGrid.h"

namespace Kans::ComponentEditors
{
    namespace
    {
        using PropertyGrid::Int;

        // The parameters a shape type uses, as listed for ShapeDesc.
        bool DrawShapeParameters(ShapeDesc& d)
        {
            bool changed = false;
            const auto size = [&](const char* label, float& value) {
                changed |= PropertyGrid::Float(label, value, 0.01f, 0.001f, 10000.0f, "%.3f m");
            };
            switch (d.Type)
            {
                case ShapeType::Cube:
                    changed |= PropertyGrid::Vec3("Size", d.Size, 1.0f, 0.01f);
                    changed |= Int("Subdivisions X", d.Subdivisions.x, 1, 64);
                    changed |= Int("Subdivisions Y", d.Subdivisions.y, 1, 64);
                    changed |= Int("Subdivisions Z", d.Subdivisions.z, 1, 64);
                    break;
                case ShapeType::Plane:
                    size("Width", d.Width);
                    size("Depth", d.Depth);
                    changed |= Int("Subdivisions X", d.Subdivisions.x, 1, 64);
                    changed |= Int("Subdivisions Z", d.Subdivisions.z, 1, 64);
                    break;
                case ShapeType::Sphere:
                    size("Radius", d.Radius);
                    changed |= Int("Segments", d.Segments, 3, 128);
                    changed |= Int("Rings", d.Rings, 2, 64);
                    break;
                case ShapeType::Cylinder:
                case ShapeType::Pipe:
                    size("Radius", d.Radius);
                    if (d.Type == ShapeType::Pipe)
                        size("Thickness", d.Thickness);
                    size("Height", d.Height);
                    changed |= Int("Sides", d.Sides, 3, 128);
                    changed |= Int("Height segments", d.HeightSegments, 1, 64);
                    break;
                case ShapeType::Cone:
                    size("Radius", d.Radius);
                    size("Height", d.Height);
                    changed |= Int("Sides", d.Sides, 3, 128);
                    break;
                case ShapeType::Torus:
                    size("Major radius", d.MajorRadius);
                    size("Minor radius", d.MinorRadius);
                    changed |= Int("Rows", d.Rows, 3, 128);
                    changed |= Int("Columns", d.Columns, 3, 128);
                    break;
                case ShapeType::Stairs:
                    size("Width", d.Width);
                    size("Height", d.Height);
                    size("Depth", d.Depth);
                    changed |= Int("Steps", d.Steps, 1, 128);
                    changed |= PropertyGrid::Checkbox("Side walls", d.SideWalls);
                    break;
                case ShapeType::Arch:
                    size("Outer radius", d.OuterRadius);
                    size("Thickness", d.Thickness);
                    size("Depth", d.Depth);
                    changed |= Int("Sides", d.Sides, 3, 128);
                    changed |= PropertyGrid::Float("Angle", d.Angle, 0.5f, 0.1f, 360.0f, "%.1f deg");
                    break;
                case ShapeType::Prism:
                    changed |= PropertyGrid::Vec3("Size", d.Size, 1.0f, 0.01f);
                    break;
            }
            return changed;
        }
    } // namespace

    const char* ShapeName(ShapeType type)
    {
        static const char* names[] = {
            "Cube", "Plane", "Sphere", "Cylinder", "Cone", "Torus", "Stairs", "Arch", "Pipe", "Prism"};
        return names[int(type)];
    }

    void DrawEditableMesh(Entity entity, ComponentUI& ui)
    {
        const auto& component = entity.GetComponent<EditableMeshComponent>();
        if (PropertyGrid::BeginGroup("Mesh"))
        {
            if (component.Shape)
            {
                PropertyGrid::Text("Shape", ShapeName(component.Shape->Type));
                ShapeDesc shape = *component.Shape;
                if (DrawShapeParameters(shape))
                    entity.PatchComponent<EditableMeshComponent>([&](EditableMeshComponent& c) {
                        c.Shape = shape;
                        RegenerateShape(c);
                    });
            }
            else
                PropertyGrid::Text("Shape", "Edited (shape parameters no longer apply)", true);
            PropertyGrid::Text("Geometry",
                               std::to_string(component.Mesh.Positions.size()) + " vertices, " +
                                   std::to_string(component.Mesh.Faces.size()) + " faces",
                               true);
            PropertyGrid::EndGroup();
        }

        // One slot per material index the faces use (faces pick their slot in face mode).
        const auto          slots = MaterialSlotCount(component);
        std::vector<size_t> faces(slots, 0);
        for (const auto& face : component.Mesh.Faces)
            ++faces[face.Material];
        if (PropertyGrid::BeginGroup(("Materials (" + std::to_string(slots) + ")").c_str()))
        {
            PropertyGrid::BeginFree();
            std::optional<uint32_t> removed;
            for (uint32_t slot = 0; slot < slots; ++slot)
            {
                ImGui::PushID(int(slot));
                const AssetID current = slot < component.Materials.size() ? component.Materials[slot] : AssetID {};
                AssetID       chosen;
                const auto detail = faces[slot] ? std::to_string(faces[slot]) + (faces[slot] > 1 ? " faces" : " face") :
                                                  std::string("unused");
                const auto remove =
                    slots > 1 ? std::function<void()>([&removed, slot] { removed = slot; }) : std::function<void()>();
                if (Widgets::MaterialSlotRow(ui,
                                             "Slot " + std::to_string(slot),
                                             detail,
                                             {{entity.GetUUID(), slot}},
                                             {},
                                             current,
                                             chosen,
                                             remove))
                    entity.PatchComponent<EditableMeshComponent>([&](EditableMeshComponent& c) {
                        c.Materials.resize(std::max<size_t>(c.Materials.size(), slot + 1));
                        c.Materials[slot] = chosen;
                    });
                ImGui::PopID();
            }
            if (removed)
                entity.PatchComponent<EditableMeshComponent>(
                    [&](EditableMeshComponent& c) { RemoveMaterialSlot(c, *removed); });
            if (ImGui::Button(ICON_FA_PLUS "  Add slot", {-FLT_MIN, 0.0f}))
                entity.PatchComponent<EditableMeshComponent>([&](EditableMeshComponent& c) {
                    c.Materials.resize(std::max<size_t>(c.Materials.size(), slots) + 1);
                });
            PropertyGrid::EndFree();
            PropertyGrid::EndGroup();
        }
        if (PropertyGrid::BeginGroup("Lighting"))
        {
            bool cast = component.CastShadows, receive = component.ReceiveShadows;
            if (PropertyGrid::Checkbox("Cast shadows", cast) | PropertyGrid::Checkbox("Receive shadows", receive))
                entity.PatchComponent<EditableMeshComponent>([&](EditableMeshComponent& c) {
                    c.CastShadows    = cast;
                    c.ReceiveShadows = receive;
                });
            PropertyGrid::EndGroup();
        }
        // Read by post-process materials; folded until used.
        if (PropertyGrid::BeginGroup("Rendering", nullptr, nullptr, component.RenderCustomDepth))
        {
            bool custom  = component.RenderCustomDepth;
            int  stencil = component.CustomStencil;
            if (PropertyGrid::Checkbox("Custom depth", custom) |
                (custom && PropertyGrid::Int("Custom stencil", stencil, 0, 255)))
                entity.PatchComponent<EditableMeshComponent>([&](EditableMeshComponent& c) {
                    c.RenderCustomDepth = custom;
                    c.CustomStencil     = uint8_t(std::clamp(stencil, 0, 255));
                });
            PropertyGrid::EndGroup();
        }
    }

    void CreateShape(Entity entity, ShapeType type)
    {
        EditableMeshComponent component;
        component.Shape       = ShapeDesc {};
        component.Shape->Type = type;
        RegenerateShape(component);
        // Rest the new shape on the ground plane instead of half below it.
        float bottom = 0.0f;
        if (!component.Mesh.Positions.empty())
        {
            bottom = component.Mesh.Positions.front().y;
            for (const auto& p : component.Mesh.Positions)
                bottom = std::min(bottom, p.y);
        }
        entity.AddComponent<EditableMeshComponent>(std::move(component));
        entity.PatchComponent<TransformComponent>([&](TransformComponent& t) { t.Position.y -= bottom; });
    }
} // namespace Kans::ComponentEditors

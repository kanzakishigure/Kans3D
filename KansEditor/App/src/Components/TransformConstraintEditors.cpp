#include <imgui.h>
#include <algorithm>
#include "Kans3D/Scene/Components.h"
#include "ComponentEditors.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans::ComponentEditors
{
    void DrawAimConstraint(Entity entity, ComponentUI& ui)
    {
        const AimConstraintComponent defaults;
        const float                  sourceWeight = 1;
        const int                    defaultAim = int(defaults.AimAxis), defaultUp = int(defaults.UpAxis);
        entity.PatchComponent<AimConstraintComponent>([&](auto& c) {
            if (PropertyGrid::Begin("##Aim"))
            {
                PropertyGrid::Checkbox("Active", c.Active, &defaults.Active);
                if (PropertyGrid::Float("Weight", c.Weight, .01f, 0, 1, "%.3f", &defaults.Weight))
                    c.Weight = std::clamp(c.Weight, 0.f, 1.f);
                PropertyGrid::End();
            }
            if (PropertyGrid::BeginGroup("Sources"))
            {
                for (size_t i = 0; i < c.Sources.size(); ++i)
                {
                    ImGui::PushID(int(i));
                    auto&    source = c.Sources[i];
                    uint64_t id     = uint64_t(source.Entity);
                    if (EntityField("Target", id, ui.GetScene()))
                        source.Entity = UUID(id);
                    if (PropertyGrid::Float("Source weight", source.Weight, .01f, 0, 1000, "%.3f", &sourceWeight))
                        source.Weight = std::max(0.f, source.Weight);
                    PropertyGrid::InputText("Joint", source.Joint);
                    PropertyGrid::Vec3("Target offset", source.Offset);
                    PropertyGrid::BeginFree();
                    bool remove = ImGui::SmallButton(ICON_FA_TRASH " Remove source");
                    PropertyGrid::EndFree();
                    if (remove)
                        c.Sources.erase(c.Sources.begin() + i--);
                    ImGui::PopID();
                }
                PropertyGrid::BeginFree();
                if (c.Sources.size() < 256 && ImGui::SmallButton(ICON_FA_PLUS " Source"))
                    c.Sources.push_back({});
                PropertyGrid::EndFree();
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Axes"))
            {
                const char* axes[] = {"+X", "+Y", "+Z", "-X", "-Y", "-Z"};
                int         aim = int(c.AimAxis), up = int(c.UpAxis);
                if (PropertyGrid::Combo("Aim axis", axes, 6, aim, &defaultAim))
                {
                    c.AimAxis = ConstraintAxis(aim);
                    if (aim % 3 == up % 3)
                        c.UpAxis = ConstraintAxis((aim + 1) % 3);
                }
                if (PropertyGrid::Combo("Up axis", axes, 6, up, &defaultUp) && up % 3 != int(c.AimAxis) % 3)
                    c.UpAxis = ConstraintAxis(up);
                if (PropertyGrid::Vec3("World up", c.WorldUp, glm::vec3(0, 1, 0)) && glm::length(c.WorldUp) < 1e-6f)
                    c.WorldUp = {0, 1, 0};
                uint64_t id = uint64_t(c.UpObject);
                if (EntityField("Up object", id, ui.GetScene()))
                    c.UpObject = UUID(id);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Limits", &c.LimitsEnabled))
            {
                PropertyGrid::Vec3("Minimum degrees", c.MinRotation, -180);
                PropertyGrid::Vec3("Maximum degrees", c.MaxRotation, 180);
                c.MaxRotation = glm::max(c.MinRotation, c.MaxRotation);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Smoothing"))
            {
                if (PropertyGrid::Float("Max degrees/s",
                                        c.MaxAngularSpeed,
                                        1,
                                        0,
                                        10000,
                                        "%.1f (0 = unlimited)",
                                        &defaults.MaxAngularSpeed))
                    c.MaxAngularSpeed = std::max(0.f, c.MaxAngularSpeed);
                PropertyGrid::EndGroup();
            }
        });
    }
} // namespace Kans::ComponentEditors

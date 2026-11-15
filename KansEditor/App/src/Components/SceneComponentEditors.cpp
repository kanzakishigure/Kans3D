#include "ComponentEditors.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Script/ScriptEngine.h"
#include "UI/BloomSettingsWidget.h"
#include "UI/PropertyGrid.h"
#include "UI/ShadowSettingsWidget.h"

namespace Kans::ComponentEditors
{
    void DrawTransform(Entity entity, ComponentUI&)
    {
        if (!PropertyGrid::Begin("##Transform"))
            return;
        // Uniform scaling stays on for the session, for every entity.
        static bool linkedScale = false;
        // Edits go through PatchComponent so the render world sees them; it only
        // notifies on an actual change.
        entity.PatchComponent<TransformComponent>([](TransformComponent& c) {
            PropertyGrid::Vec3("Position", c.Position);
            glm::vec3 rotation = glm::degrees(c.Rotation);
            if (PropertyGrid::Vec3("Rotation", rotation, 0.0f, 0.5f))
                c.Rotation = glm::radians(rotation);
            PropertyGrid::Vec3("Scale", c.Scale, 1.0f, 0.01f, &linkedScale);
        });
        PropertyGrid::End();
    }

    void DrawCamera(Entity entity, ComponentUI& ui)
    {
        static SceneCamera defaults {}; // Its getters are not const.
        entity.PatchComponent<CameraComponent>([&](CameraComponent& c) {
            auto& camera = c.SceneCamera;
            if (PropertyGrid::BeginGroup("Basics"))
            {
                PropertyGrid::Checkbox("Primary", c.Primary);
                static const bool fixedDefault = false;
                PropertyGrid::Checkbox("Fixed aspect", c.FixedAspectRatio, &fixedDefault);
                const char* projections[]     = {"Perspective", "Orthographic"};
                int         projection        = int(camera.GetProjectionType());
                const int   projectionDefault = int(defaults.GetProjectionType());
                if (PropertyGrid::Combo("Projection", projections, 2, projection, &projectionDefault))
                    camera.SetProjectionType(SceneCamera::ProjectionType(projection));
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Lens"))
            {
                if (camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective)
                {
                    const float fovDefault = defaults.GetPerspectiveVerticalFOV(), nearDefault = defaults.GetPerspectiveNearClip(),
                                farDefault = defaults.GetPerspectiveFarClip();
                    float fov = camera.GetPerspectiveVerticalFOV();
                    if (PropertyGrid::Float("Vertical FOV", fov, 0.1f, 1.0f, 179.0f, "%.1f°", &fovDefault))
                        camera.SetPerspectiveVerticalFOV(fov);
                    float nearClip = camera.GetPerspectiveNearClip();
                    if (PropertyGrid::Float("Near clip", nearClip, 0.01f, 0.001f, 1000.0f, "%.3f m", &nearDefault))
                        camera.SetPerspectiveNearClip(nearClip);
                    float farClip = camera.GetPerspectiveFarClip();
                    if (PropertyGrid::Float("Far clip", farClip, 1.0f, 0.01f, 100000.0f, "%.1f m", &farDefault))
                        camera.SetPerspectiveFarClip(farClip);
                }
                else
                {
                    const float sizeDefault = defaults.GetOrthographicSize(), nearDefault = defaults.GetOrthographicNearClip(),
                                farDefault = defaults.GetOrthographicFarClip();
                    float size = camera.GetOrthographicSize();
                    if (PropertyGrid::Float("Size", size, 0.1f, 0.01f, 10000.0f, "%.3f m", &sizeDefault))
                        camera.SetOrthographicSize(size);
                    float nearClip = camera.GetOrthographicNearClip();
                    if (PropertyGrid::Float("Near clip", nearClip, 0.1f, 0.0f, 0.0f, "%.3f m", &nearDefault))
                        camera.SetOrthographicNearClip(nearClip);
                    float farClip = camera.GetOrthographicFarClip();
                    if (PropertyGrid::Float("Far clip", farClip, 0.1f, 0.0f, 0.0f, "%.3f m", &farDefault))
                        camera.SetOrthographicFarClip(farClip);
                }
                PropertyGrid::EndGroup();
            }
            {
                // Sharpness and the reactive settings are post-processing: changing them here gives the
                // camera a post override (a copy of the scene's settings), like editing it below.
                auto antiAliasing = camera.GetAntiAliasing();
                auto upscaling    = camera.GetUpscaling();
                auto settings     = camera.ResolvePostProcess(ui.GetScene() ? ui.GetScene()->GetPostProcessSettings() :
                                                                              PostProcessSettings {});
                const auto defaultMode = defaults.GetAntiAliasing();
                const auto edits =
                    DrawAntiAliasingGroup(antiAliasing, upscaling, settings.Temporal, nullptr, &defaultMode);
                if (edits.Mode)
                    camera.SetAntiAliasing(antiAliasing);
                if (edits.Upscaling)
                    camera.SetUpscaling(upscaling);
                if (edits.Temporal)
                    camera.SetPostProcessOverride(settings);
            }
            // Post-processing normally comes from the scene; an override applies to this camera only.
            bool overridden = camera.GetPostProcessOverride().has_value();
            bool toggled    = false;
            const bool open = PropertyGrid::BeginGroup("Override post", &overridden, &toggled);
            if (toggled)
                camera.SetPostProcessOverride(
                    overridden ? std::optional<PostProcessSettings>(
                                     ui.GetScene() ? ui.GetScene()->GetPostProcessSettings() : PostProcessSettings {}) :
                                 std::nullopt);
            if (open)
            {
                if (camera.GetPostProcessOverride())
                {
                    auto settings = *camera.GetPostProcessOverride();
                    if (DrawPostProcessSettings(
                            settings,
                            [&](AssetID& dirt) { return ui.AssetPicker("Lens dirt", AssetType::Texture, dirt); },
                            [&](const char* label, AssetID& id) {
                                return ui.AssetPicker(label, AssetType::Material, id);
                            },
                            false))
                        camera.SetPostProcessOverride(settings);
                }
                PropertyGrid::EndGroup();
            }
        });
    }

    namespace
    {
        void DrawEmission(LightEmission& c, const LightEmission& defaults, const char* unit)
        {
            PropertyGrid::Color3("Color (linear)", c.Color, &defaults.Color);
            PropertyGrid::Checkbox("Use temperature", c.UseTemperature, &defaults.UseTemperature);
            if (c.UseTemperature)
                PropertyGrid::Float("Temperature (K)", c.Temperature, 10, 1000, 40000, "%.0f", &defaults.Temperature);
            PropertyGrid::Float(unit, c.Intensity, .1f, 0, 100000000, "%.3f", &defaults.Intensity);
            PropertyGrid::Float("Diffuse scale", c.DiffuseScale, .01f, 0, 1, "%.3f", &defaults.DiffuseScale);
            PropertyGrid::Float("Specular scale", c.SpecularScale, .01f, 0, 1, "%.3f", &defaults.SpecularScale);
            PropertyGrid::Float(
                "Volumetric scattering", c.VolumetricScattering, .01f, 0, 1000, "%.3f", &defaults.VolumetricScattering);
        }
        template<class T>
        void DrawPunctual(T& c, const T& defaults, float solidAngle)
        {
            int         unit    = int(c.Unit);
            const char* names[] = {"Candela", "Lumens"};
            if (PropertyGrid::Combo("Intensity unit", names, 2, unit))
            {
                c.Intensity = FromCandela(ToCandela(c.Intensity, c.Unit, solidAngle), LightUnit(unit), solidAngle);
                c.Unit      = LightUnit(unit);
            }
            DrawEmission(c, defaults, c.Unit == LightUnit::Candela ? "Intensity (cd)" : "Intensity (lm)");
            PropertyGrid::Float("Range (m)", c.Range, .1f, 0, 10000, "%.2f", &defaults.Range);
        }
    } // namespace
    void DrawFogVolume(Entity entity, ComponentUI&)
    {
        static const FogVolumeComponent defaults;
        entity.PatchComponent<FogVolumeComponent>([](auto& c) {
            if (PropertyGrid::BeginGroup("Medium"))
            {
                const char* shapes[] = {"Box", "Sphere"};
                int         shape    = int(c.Shape);
                if (PropertyGrid::Combo("Shape", shapes, 2, shape))
                    c.Shape = FogVolumeShape(shape);
                PropertyGrid::Float("Density", c.Density, .001f, 0, 100, "%.4f", &defaults.Density);
                PropertyGrid::Color3("Albedo", c.Albedo, &defaults.Albedo);
                PropertyGrid::Float("Edge falloff", c.EdgeFalloff, .01f, 0, 1, "%.3f", &defaults.EdgeFalloff);
                PropertyGrid::EndGroup();
            }
        });
    }
    void DrawDirectionalLight(Entity entity, ComponentUI&)
    {
        static const DirLightComponent defaults = [] {
            DirLightComponent light;
            DefaultDirectionalLight(light);
            return light;
        }();
        entity.PatchComponent<DirLightComponent>([](DirLightComponent& c) {
            if (PropertyGrid::BeginGroup("Light"))
            {
                PropertyGrid::Vec3("Direction", c.Direction, defaults.Direction, 0.01f);
                DrawEmission(c, defaults, "Intensity (lux)");
                PropertyGrid::EndGroup();
            }
            auto shadows = c.Shadows;
            if (DrawShadowSettings(shadows, true))
                c.Shadows = shadows.Validated();
        });
    }

    void DrawPointLight(Entity entity, ComponentUI&)
    {
        static const PointLightComponent defaults = [] {
            PointLightComponent light;
            DefaultPointLight(light);
            return light;
        }();
        entity.PatchComponent<PointLightComponent>([](PointLightComponent& c) {
            if (PropertyGrid::BeginGroup("Light"))
            {
                DrawPunctual(c, defaults, PointSolidAngle());
                PropertyGrid::Text("Position", "From the Transform", true);
                PropertyGrid::EndGroup();
            }
            auto shadows = c.Shadows;
            if (DrawShadowSettings(shadows, false))
                c.Shadows = shadows.Validated();
        });
    }

    void DrawSpotLight(Entity entity, ComponentUI& ui)
    {
        static const SpotLightComponent defaults;
        entity.PatchComponent<SpotLightComponent>([&](auto& c) {
            if (PropertyGrid::BeginGroup("Light"))
            {
                DrawPunctual(c, defaults, SpotSolidAngle(c.InnerConeAngle, c.OuterConeAngle));
                PropertyGrid::Float("Inner cone (degrees)",
                                    c.InnerConeAngle,
                                    .1f,
                                    0,
                                    c.OuterConeAngle,
                                    "%.1f",
                                    &defaults.InnerConeAngle);
                PropertyGrid::Float(
                    "Outer cone (degrees)", c.OuterConeAngle, .1f, .01f, 89, "%.1f", &defaults.OuterConeAngle);
                c.InnerConeAngle = std::min(c.InnerConeAngle, c.OuterConeAngle);
                PropertyGrid::Text("Aim", "Transform local -Z", true);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Cookie"))
            {
                ui.AssetPicker("Texture", AssetType::Texture, c.Cookie);
                PropertyGrid::Float(
                    "Rotation (degrees)", c.CookieRotation, .5f, 0, 0, "%.1f", &defaults.CookieRotation);
                PropertyGrid::Float("Scale X", c.CookieScale.x, .01f, .001f, 100, "%.3f", &defaults.CookieScale.x);
                PropertyGrid::Float("Scale Y", c.CookieScale.y, .01f, .001f, 100, "%.3f", &defaults.CookieScale.y);
                PropertyGrid::EndGroup();
            }
            auto shadows = c.Shadows;
            if (DrawShadowSettings(shadows, false))
                c.Shadows = shadows.Validated();
        });
    }
    void DrawAreaLight(Entity entity, ComponentUI& ui)
    {
        static const AreaLightComponent defaults;
        entity.PatchComponent<AreaLightComponent>([&](auto& c) {
            // ComponentUI and PropertyGrid provide the shared EditorTheme tokens.
            if (PropertyGrid::BeginGroup("Light"))
            {
                int         shape = int(c.Shape), unit = int(c.Unit);
                const char* shapes[] = {"Rectangle", "Disc"};
                const char* units[]  = {"Nits (cd/m²)", "Lumens"};
                if (PropertyGrid::Combo("Shape", shapes, 2, shape))
                    c.Shape = AreaLightShape(shape);
                const float area = AreaLightArea(c.Shape, c.Width, c.Height, c.Radius);
                if (PropertyGrid::Combo("Intensity unit", units, 2, unit))
                {
                    float radiance = AreaLightRadiance(c.Intensity, c.Unit, area, c.TwoSided);
                    c.Unit         = AreaLightUnit(unit);
                    c.Intensity =
                        c.Unit == AreaLightUnit::Nits ? radiance : radiance * LightPi * area * (c.TwoSided ? 2 : 1);
                }
                DrawEmission(c, defaults, c.Unit == AreaLightUnit::Nits ? "Intensity (cd/m²)" : "Intensity (lm)");
                PropertyGrid::Float("Range (m)", c.Range, .1f, 0, 10000, "%.2f", &defaults.Range);
                PropertyGrid::Checkbox("Two sided", c.TwoSided, &defaults.TwoSided);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Shape"))
            {
                if (c.Shape == AreaLightShape::Rectangle)
                {
                    PropertyGrid::Float("Width (m)", c.Width, .01f, .001f, 10000, "%.3f", &defaults.Width);
                    PropertyGrid::Float("Height (m)", c.Height, .01f, .001f, 10000, "%.3f", &defaults.Height);
                    ui.AssetPicker("Source texture", AssetType::Texture, c.SourceTexture);
                }
                else
                    PropertyGrid::Float("Radius (m)", c.Radius, .01f, .001f, 10000, "%.3f", &defaults.Radius);
                PropertyGrid::Text("Facing", "Transform local -Z", true);
                PropertyGrid::EndGroup();
            }
        });
    }

    void DrawScript(Entity entity, ComponentUI&)
    {
        if (!PropertyGrid::Begin("##Script"))
            return;
        entity.PatchComponent<ScriptComponent>([](ScriptComponent& c) {
            const bool exists = ScriptEngine::EntityClassExists(c.ClassName);
            // Classes come from the generated registry of the loaded script assemblies.
            const auto classes = ScriptEngine::GetEntityClasses();
            if (!classes.empty() && PropertyGrid::Row("Class", "C# entity class (Scripts > Build & Reload Scripts)"))
            {
                if (ImGui::BeginCombo("##ClassList", c.ClassName.empty() ? "Select..." : c.ClassName.c_str()))
                {
                    for (const auto& name : classes)
                        if (ImGui::Selectable(name.c_str(), name == c.ClassName))
                            c.ClassName = name;
                    ImGui::EndCombo();
                }
            }
            if (!PropertyGrid::Row(classes.empty() ? "Class" : "Name", "Name of a C# entity class"))
                return;
            char buffer[256] = {};
            c.ClassName.copy(buffer, sizeof(buffer) - 1);
            if (!exists)
                ImGui::PushStyleColor(ImGuiCol_Text, ErrorColor);
            if (ImGui::InputText("##Class", buffer, sizeof(buffer)))
                c.ClassName = buffer;
            if (!exists)
                ImGui::PopStyleColor();
            if (!exists)
                PropertyGrid::Text("",
                                   c.ClassName.empty() ? "Enter a class name." : "No entity class with this name.",
                                   true);
        });
        PropertyGrid::End();
    }
} // namespace Kans::ComponentEditors

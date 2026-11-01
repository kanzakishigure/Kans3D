#include "kspch.h"
#include "TimelineProperty.h"
#include <charconv>
#include "Kans3D/Scene/Entity.h"
namespace Kans
{
    bool TimelineHasComponent(Entity e, const std::string& name)
    {
        if (name == "TransformComponent")
            return e.HasComponent<TransformComponent>();
        if (name == "AnimatorComponent")
            return e.HasComponent<AnimatorComponent>();
        if (name == "ParticleSystemComponent")
            return e.HasComponent<ParticleSystemComponent>();
        if (name == "CameraComponent")
            return e.HasComponent<CameraComponent>();
        if (name == "PointLightComponent")
            return e.HasComponent<PointLightComponent>();
        if (name == "DirLightComponent")
            return e.HasComponent<DirLightComponent>();
        if (name == "TextComponent")
            return e.HasComponent<TextComponent>();
        if (name == "SpriteRendererComponent")
            return e.HasComponent<SpriteRendererComponent>();
        if (name == "MeshComponent")
            return e.HasComponent<MeshComponent>();
        if (name == "EditableMeshComponent")
            return e.HasComponent<EditableMeshComponent>();
        if (name == "PrefabInstanceComponent")
            return e.HasComponent<PrefabInstanceComponent>();
        if (name == "MaterialParameterDriversComponent")
            return e.HasComponent<MaterialParameterDriversComponent>();
        if (name == "ScriptComponent")
            return e.HasComponent<ScriptComponent>();
        if (name == "ActiveComponent")
            return e.HasComponent<ActiveComponent>();
        if (name == "IKComponent")
            return e.HasComponent<IKComponent>();
        if (name == "ShapeKeysComponent")
            return e.HasComponent<ShapeKeysComponent>();
        return false;
    }
    bool TimelineProperty(Scene& scene, Entity e, const std::string& path, glm::vec4& value, bool write)
    {
        if (!e)
            return false;
        std::string field   = path;
        int         channel = -1;
        if (path.size() > 2 && path[path.size() - 2] == '.')
        {
            std::string suffix = "xyzw";
            auto        i      = suffix.find(path.back());
            if (i == std::string::npos)
                i = std::string("rgba").find(path.back());
            if (i != std::string::npos)
            {
                channel = int(i);
                field   = path.substr(0, path.size() - 2);
            }
        }
        if (field.rfind("Material.", 0) == 0)
        {
            auto dot = field.find('.', 9);
            if (dot == std::string::npos || dot + 1 == field.size())
                return false;
            uint32_t slot   = 0;
            auto     result = std::from_chars(field.data() + 9, field.data() + dot, slot);
            if (result.ec != std::errc() || result.ptr != field.data() + dot)
                return false;
            std::string name = field.substr(dot + 1);
            if (!e.HasComponent<MaterialParameterDriversComponent>())
            {
                if (!write)
                {
                    value = {};
                    return true;
                }
                e.AddComponent<MaterialParameterDriversComponent>();
            }
            auto& drivers = e.GetComponent<MaterialParameterDriversComponent>();
            scene.UpdateMaterialParameterDrivers(e);
            glm::vec4  effective(0);
            auto       table  = drivers.SlotValues.find(slot);
            const auto values = table == drivers.SlotValues.end() ? drivers.Values : table->second;
            if (values)
                for (const auto& v : *values)
                    if (v.Parameter == name && v.Resolved)
                        effective = {v.Value[0], v.Value[1], v.Value[2], v.Value[3]};
            if (write)
            {
                glm::vec4 output = value;
                if (channel >= 0)
                {
                    output          = effective;
                    output[channel] = value.x;
                }
                auto& overrides = drivers.SlotOverrides[slot];
                auto  found =
                    std::find_if(overrides.begin(), overrides.end(), [&](const auto& v) { return v.first == name; });
                std::array<float, 4> v {output.x, output.y, output.z, output.w};
                if (found == overrides.end())
                    overrides.emplace_back(name, v);
                else
                    found->second = v;
                scene.UpdateMaterialParameterDrivers(e);
            }
            else
                value = channel >= 0 ? glm::vec4(effective[channel], 0, 0, 0) : effective;
            return true;
        }
        auto access = [&](auto& v) {
            using T             = std::decay_t<decltype(v)>;
            constexpr int count = std::is_same_v<T, float>     ? 1 :
                                  std::is_same_v<T, glm::vec2> ? 2 :
                                  std::is_same_v<T, glm::vec3> ? 3 :
                                                                 4;
            if (channel >= count)
                return false;
            auto get = [&](int i) -> float& {
                if constexpr (std::is_same_v<T, float>)
                    return v;
                else
                    return v[i];
            };
            if (channel >= 0)
            {
                if (write)
                    get(channel) = value.x;
                else
                    value = {get(channel), 0, 0, 0};
            }
            else
            {
                if (write)
                    for (int i = 0; i < count; ++i)
                        get(i) = value[i];
                else
                {
                    value = {};
                    for (int i = 0; i < count; ++i)
                        value[i] = get(i);
                }
            }
            return true;
        };
        auto component = [&](auto type, auto member) {
            using T = decltype(type);
            if (!e.HasComponent<T>())
                return false;
            bool ok = false;
            if (write)
                e.PatchComponent<T>([&](auto& c) { ok = access(c.*member); });
            else
                ok = access(e.GetComponent<T>().*member);
            return ok;
        };
        if (field == "Transform.Position")
            return component(TransformComponent {}, &TransformComponent::Position);
        if (field == "Transform.Rotation")
            return component(TransformComponent {}, &TransformComponent::Rotation);
        if (field == "Transform.Scale")
            return component(TransformComponent {}, &TransformComponent::Scale);
        if (field == "Text.Size")
            return component(TextComponent {}, &TextComponent::Size);
        if (field == "Text.Color")
            return component(TextComponent {}, &TextComponent::Color);
        if (field == "Text.WrapWidth")
            return component(TextComponent {}, &TextComponent::WrapWidth);
        if (field == "Text.BoxHeight")
            return component(TextComponent {}, &TextComponent::BoxHeight);
        if (field == "Text.LineSpacing")
            return component(TextComponent {}, &TextComponent::LineSpacing);
        if (field == "Text.LetterSpacing")
            return component(TextComponent {}, &TextComponent::LetterSpacing);
        if (field == "Sprite.Color")
            return component(SpriteRendererComponent {}, &SpriteRendererComponent::Color);
        if (field == "Sprite.TilingFactor")
            return component(SpriteRendererComponent {}, &SpriteRendererComponent::TilingFactor);
        if (field == "Sprite.UVRect")
            return component(SpriteRendererComponent {}, &SpriteRendererComponent::UVRect);
        if (field == "PointLight.Diffuse_Intensity")
            return component(PointLightComponent {}, &PointLightComponent::Diffuse_Intensity);
        if (field == "PointLight.Specular_Intensity")
            return component(PointLightComponent {}, &PointLightComponent::Specular_Intensity);
        if (field == "PointLight.Ambient_Intensity")
            return component(PointLightComponent {}, &PointLightComponent::Ambient_Intensity);
        if (field == "DirLight.Diffuse_Intensity")
            return component(DirLightComponent {}, &DirLightComponent::Diffuse_Intensity);
        if (field == "DirLight.Specular_Intensity")
            return component(DirLightComponent {}, &DirLightComponent::Specular_Intensity);
        if (field == "DirLight.Ambient_Intensity")
            return component(DirLightComponent {}, &DirLightComponent::Ambient_Intensity);
        if (field == "DirLight.Direction")
            return component(DirLightComponent {}, &DirLightComponent::Direction);
        if (field == "Light.Intensity" || field == "Light.DiffuseIntensity")
        {
            if (e.HasComponent<PointLightComponent>())
                return component(PointLightComponent {}, &PointLightComponent::Diffuse_Intensity);
            if (e.HasComponent<DirLightComponent>())
                return component(DirLightComponent {}, &DirLightComponent::Diffuse_Intensity);
            return false;
        }
        if (field.rfind("Camera.", 0) == 0 && e.HasComponent<CameraComponent>())
        {
            auto& c = e.GetComponent<CameraComponent>().SceneCamera;
#define CAMERA(N) \
    if (field == "Camera." #N) \
    { \
        if (write) \
            c.Set##N(value.x); \
        else \
            value = {c.Get##N(), 0, 0, 0}; \
        return true; \
    }
            CAMERA(PerspectiveVerticalFOV)
            CAMERA(PerspectiveNearClip)
            CAMERA(PerspectiveFarClip)
            CAMERA(OrthographicSize) CAMERA(OrthographicNearClip)
                CAMERA(OrthographicFarClip) if (field == "Camera.Exposure")
            {
                if (write)
                    c.SetExposure(value.x);
                else
                    value = {static_cast<const Camera&>(c).GetExposure(), 0, 0, 0};
                return true;
            }
#undef CAMERA
        }
        if (e.HasComponent<TextComponent>())
        {
            bool ok    = false;
            auto apply = [&](TextComponent& c) {
                if (field == "Text.Outline.Width")
                    ok = access(c.Outline.Width);
                if (field == "Text.Outline.Color")
                    ok = access(c.Outline.Color);
                if (field == "Text.Shadow.Offset")
                    ok = access(c.Shadow.Offset);
                if (field == "Text.Shadow.Softness")
                    ok = access(c.Shadow.Softness);
                if (field == "Text.Shadow.Color")
                    ok = access(c.Shadow.Color);
            };
            if (write)
                e.PatchComponent<TextComponent>(apply);
            else
                apply(e.GetComponent<TextComponent>());
            return ok;
        }
        return false;
    }
} // namespace Kans

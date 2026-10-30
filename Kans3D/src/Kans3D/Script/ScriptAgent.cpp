#include "kspch.h"
#include "ScriptAgent.h"
#include "Kans3D/Input/Input.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Entity.h"
#include "Kans3D/Scene/Scene.h"
#include "ScriptAnimation.h"
#include "ScriptEngine.h"
#include "ScriptTimeline.h"
#include "ScriptUI.h"

namespace Kans
{
    namespace
    {
        Entity Resolve(uint64_t id)
        {
            auto* scene = ScriptEngine::GetSceneContext();
            return scene ? scene->GetEntityByUUID(id) : Entity {};
        }
        void KANS_SCRIPT_CALL NativeLog(int32_t level, const char* message) noexcept
        {
            try
            {
                if (level == 2)
                {
                    CORE_ERROR("{}", message);
                }
                else if (level == 1)
                {
                    CORE_WARN("{}", message);
                }
                else
                {
                    CORE_INFO("{}", message);
                }
            }
            catch (...)
            {}
        }
        int32_t KANS_SCRIPT_CALL HasComponent(uint64_t id, uint32_t type) noexcept
        {
            try
            {
                auto entity = Resolve(id);
                if (!entity)
                    return 0;
                switch (Scripting::ComponentType(type))
                {
                    case Scripting::ComponentType::RigidBodyComponent:
                        return entity.HasComponent<RigidBodyComponent>();
                    case Scripting::ComponentType::BoxColliderComponent:
                        return entity.HasComponent<BoxColliderComponent>();
                    case Scripting::ComponentType::SphereColliderComponent:
                        return entity.HasComponent<SphereColliderComponent>();
                    case Scripting::ComponentType::CapsuleColliderComponent:
                        return entity.HasComponent<CapsuleColliderComponent>();
                    case Scripting::ComponentType::MeshColliderComponent:
                        return entity.HasComponent<MeshColliderComponent>();
                    case Scripting::ComponentType::GlobalWindComponent:
                        return entity.HasComponent<GlobalWindComponent>();
                    case Scripting::ComponentType::TimelinePlayerComponent:
                        return entity.HasComponent<TimelinePlayerComponent>();
                    case Scripting::ComponentType::AnimatorComponent:
                        return entity.HasComponent<AnimatorComponent>();
                    case Scripting::ComponentType::TransformComponent:
                        return entity.HasComponent<TransformComponent>();
                    case Scripting::ComponentType::MaterialComponent:
                        return 0; // Retired native component.
                    case Scripting::ComponentType::CameraComponent:
                        return entity.HasComponent<CameraComponent>();
                    case Scripting::ComponentType::TextComponent:
                        return entity.HasComponent<TextComponent>();
                    case Scripting::ComponentType::SpriteRendererComponent:
                        return entity.HasComponent<SpriteRendererComponent>();
                    case Scripting::ComponentType::UIScreenComponent:
                        return entity.HasComponent<UIScreenComponent>();
                    case Scripting::ComponentType::UIWorldComponent:
                        return entity.HasComponent<UIWorldComponent>();
                    case Scripting::ComponentType::ScriptComponent:
                        return entity.HasComponent<ScriptComponent>();
                }
            }
            catch (...)
            {}
            return 0;
        }
        void KANS_SCRIPT_CALL NativeGetTranslation(uint64_t id, Scripting::Vector3* out) noexcept
        {
            *out = {};
            try
            {
                if (auto entity = Resolve(id); entity && entity.HasComponent<TransformComponent>())
                {
                    auto v = entity.GetComponent<TransformComponent>().Position;
                    *out   = {v.x, v.y, v.z};
                }
            }
            catch (...)
            {}
        }
        void KANS_SCRIPT_CALL NativeSetTranslation(uint64_t id, const Scripting::Vector3* v) noexcept
        {
            try
            {
                if (auto entity = Resolve(id); entity && entity.HasComponent<TransformComponent>())
                    ScriptAgent::SetTranslation(entity, {v->X, v->Y, v->Z});
            }
            catch (...)
            {}
        }
        void KANS_SCRIPT_CALL NativeGetRotation(uint64_t id, Scripting::Vector3* out) noexcept
        {
            *out = {};
            try
            {
                if (auto entity = Resolve(id); entity && entity.HasComponent<TransformComponent>())
                {
                    auto v = entity.GetComponent<TransformComponent>().Rotation;
                    *out   = {v.x, v.y, v.z};
                }
            }
            catch (...)
            {}
        }
        void KANS_SCRIPT_CALL NativeSetRotation(uint64_t id, const Scripting::Vector3* v) noexcept
        {
            try
            {
                if (auto entity = Resolve(id); entity && entity.HasComponent<TransformComponent>())
                    ScriptAgent::SetRotation(entity, {v->X, v->Y, v->Z});
            }
            catch (...)
            {}
        }
        void KANS_SCRIPT_CALL SetMaterialParameter(uint64_t id, const char* name, const float* value) noexcept
        {
            try
            {
                auto entity = Resolve(id);
                if (!entity || !name || !value)
                    return;
                auto&                      drivers = entity.HasComponent<MaterialParameterDriversComponent>() ?
                                                         entity.GetComponent<MaterialParameterDriversComponent>() :
                                                         entity.AddComponent<MaterialParameterDriversComponent>();
                const std::array<float, 4> v {value[0], value[1], value[2], value[3]};
                for (auto& entry : drivers.Overrides)
                    if (entry.first == name)
                    {
                        entry.second = v;
                        return;
                    }
                drivers.Overrides.emplace_back(name, v);
            }
            catch (...)
            {}
        }
        void KANS_SCRIPT_CALL ClearMaterialParameter(uint64_t id, const char* name) noexcept
        {
            try
            {
                auto entity = Resolve(id);
                if (!entity || !name || !entity.HasComponent<MaterialParameterDriversComponent>())
                    return;
                auto& overrides = entity.GetComponent<MaterialParameterDriversComponent>().Overrides;
                overrides.erase(std::remove_if(overrides.begin(),
                                               overrides.end(),
                                               [&](const auto& entry) { return entry.first == name; }),
                                overrides.end());
            }
            catch (...)
            {}
        }
        int32_t KANS_SCRIPT_CALL GetMaterialParameter(uint64_t id, const char* name, float* out) noexcept
        {
            if (!out)
                return 0;
            std::fill_n(out, 4, 0.0f);
            try
            {
                auto entity = Resolve(id);
                if (!entity || !name || !entity.HasComponent<MaterialParameterDriversComponent>())
                    return 0;
                ScriptEngine::GetSceneContext()->UpdateMaterialParameterDrivers(entity);
                const auto& values = *entity.GetComponent<MaterialParameterDriversComponent>().Values;
                // Match the renderer: the last resolved binding with this name wins.
                for (auto value = values.rbegin(); value != values.rend(); ++value)
                    if (value->Resolved && value->Parameter == name)
                    {
                        std::copy(value->Value.begin(), value->Value.end(), out);
                        return 1;
                    }
            }
            catch (...)
            {}
            return 0;
        }
        int32_t KANS_SCRIPT_CALL KeyPressed(int32_t key) noexcept
        {
            try
            {
                return Input::IsKeyPressed(KeyCode(key));
            }
            catch (...)
            {
                return 0;
            }
        }
    } // namespace
    void ScriptAgent::SetTranslation(Entity entity, const glm::vec3& value)
    {
        entity.PatchComponent<TransformComponent>([&](auto& transform) { transform.Position = value; });
    }
    void ScriptAgent::SetRotation(Entity entity, const glm::vec3& value)
    {
        entity.PatchComponent<TransformComponent>([&](auto& transform) { transform.Rotation = value; });
    }
    Scripting::NativeApi ScriptAgent::CreateApi()
    {
        Scripting::NativeApi api {Scripting::ApiVersion,
                                  sizeof(Scripting::NativeApi),
                                  &NativeLog,
                                  &HasComponent,
                                  &NativeGetTranslation,
                                  &NativeSetTranslation,
                                  &NativeGetRotation,
                                  &NativeSetRotation,
                                  &KeyPressed,
                                  &SetMaterialParameter,
                                  &ClearMaterialParameter,
                                  &GetMaterialParameter};
        ScriptUI::Install(api);
        ScriptAnimation::Install(api);
        ScriptTimeline::Install(api);
        return api;
    }
} // namespace Kans

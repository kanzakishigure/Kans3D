#include "kspch.h"
#include "ScriptVfx.h"
#include "Kans3D/Scene/Entity.h"
#include "Kans3D/VFX/VfxWorld.h"
#include "ScriptEngine.h"
namespace Kans::ScriptVfx
{
    namespace
    {
        int32_t KANS_SCRIPT_CALL Command(uint64_t                id,
                                         int32_t                 op,
                                         int32_t                 type,
                                         const char*             text,
                                         Scripting::Value*       v,
                                         const Scripting::Value* second) noexcept
        {
            try
            {
                auto scene = ScriptEngine::GetSceneContext();
                if (!scene || !v)
                    return 0;
                auto& world = scene->GetVfxWorld();
                if (op == 100)
                {
                    if (!text || !second)
                        return -1;
                    auto asset = AssetID::FromString(text);
                    if (!asset.IsValid())
                        return -1;
                    auto entity = world.Spawn(asset,
                                              {v->X, v->Y, v->Z},
                                              glm::quat(second->W, second->X, second->Y, second->Z),
                                              second->Integer != 0);
                    v->Integer  = int64_t(uint64_t(entity.GetUUID()));
                    return 1;
                }
                auto entity = scene->GetEntityByUUID(id);
                if (!entity || !entity.HasComponent<ParticleSystemComponent>())
                    return 0;
                auto instance = world.Find(id);
                if (!instance)
                {
                    world.Tick(0);
                    instance = world.Find(id);
                }
                if (!instance)
                    return 0;
                switch (op)
                {
                    case 0:
                        instance->Activate(v->Integer != 0);
                        break;
                    case 1:
                        instance->Deactivate();
                        break;
                    case 2:
                        instance->DeactivateImmediate();
                        break;
                    case 3:
                        instance->Pause();
                        break;
                    case 4:
                        instance->Resume();
                        break;
                    case 5:
                        v->Integer = instance->IsActive();
                        break;
                    case 6:
                        v->Integer = instance->IsComplete();
                        break;
                    case 7:
                        v->Integer = instance->ParticleCount();
                        break;
                    case 8:
                        if (!text || v->Integer < 0 || uint64_t(v->Integer) > UINT32_MAX)
                            return -1;
                        instance->Emit(text, uint32_t(v->Integer));
                        break;
                    case 10:
                    case 11: {
                        if (!text || type < 0 || type > int(Vfx::Type::Position))
                            return -1;
                        auto t = Vfx::Type(type);
                        if (op == 10)
                        {
                            Vfx::Value value;
                            value.Kind    = t;
                            value.Data    = {v->X, v->Y, v->Z, v->W};
                            value.Integer = int32_t(v->Integer);
                            instance->Set(text, value);
                        }
                        else
                        {
                            auto value = instance->Get(text, t);
                            v->X       = value.Data.x;
                            v->Y       = value.Data.y;
                            v->Z       = value.Data.z;
                            v->W       = value.Data.w;
                            v->Integer = value.Integer;
                        }
                        break;
                    }
                    default:
                        return -1;
                }
                return 1;
            }
            catch (const std::invalid_argument& e)
            {
                return std::string(e.what()).find("type mismatch") != std::string::npos ? -2 : -1;
            }
            catch (...)
            {
                return -1;
            }
        }
    } // namespace
    void Install(Scripting::NativeApi& api) { api.VfxCommand = Command; }
} // namespace Kans::ScriptVfx

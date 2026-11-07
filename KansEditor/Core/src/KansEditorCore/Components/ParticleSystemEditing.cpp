#include "KansEditorCore/Components/ParticleSystemEditing.h"
#include <algorithm>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Scene/Scene.h"

namespace Kans::ParticleSystemEditing
{
    namespace
    {
        // Card-driven preview state per entity (editor runtime only, never serialized).
        std::map<UUID, PreviewState>& States()
        {
            static std::map<UUID, PreviewState> states;
            return states;
        }
        Vfx::SystemInstance* Running(Scene& scene, Entity e)
        {
            if (!e || !e.HasComponent<ParticleSystemComponent>())
                return nullptr;
            auto& world = scene.GetVfxWorld();
            if (world.IsPlaying())
                return nullptr;
            auto* instance = world.Find(e.GetUUID());
            if (!instance)
            {
                world.Tick(0, true);
                instance = world.Find(e.GetUUID());
            }
            return instance;
        }
    } // namespace

    UserParameters ListUserParameters(const ParticleSystemComponent& c, const Vfx::CompiledSystem& system)
    {
        UserParameters r;
        Vfx::MatchUserParameters(system, c.UserParameters, &r.Warnings);
        for (const auto& p : system.Settings.UserParameters)
        {
            UserParameterRow row;
            row.Spec  = p;
            auto at   = c.UserParameters.find(p.Name);
            row.Overridden = at != c.UserParameters.end() && at->second.Kind == p.Kind;
            row.Value = row.Overridden ? at->second : p.Default.value_or(Vfx::Value::Vector({}, p.Kind));
            r.Rows.push_back(std::move(row));
        }
        return r;
    }

    ParticleSystemComponent SetOverride(ParticleSystemComponent c, const Vfx::CompiledSystem& system, const std::string& name,
                                        const Vfx::Value& value)
    {
        c.UserParameters = Vfx::MatchUserParameters(system, c.UserParameters);
        const auto& declared = system.Settings.UserParameters;
        auto at = std::find_if(declared.begin(), declared.end(), [&](const auto& p) { return p.Name == name; });
        if (at != declared.end() && at->Kind == value.Kind)
            c.UserParameters[name] = value;
        return c;
    }

    ParticleSystemComponent ResetOverride(ParticleSystemComponent c, const Vfx::CompiledSystem& system, const std::string& name)
    {
        c.UserParameters = Vfx::MatchUserParameters(system, c.UserParameters);
        c.UserParameters.erase(name);
        return c;
    }

    Ref<const VfxSystemAsset> ResolveSystem(Scene& scene, Entity e, AssetManager* assets)
    {
        if (!e || !e.HasComponent<ParticleSystemComponent>())
            return nullptr;
        const auto id = e.GetComponent<ParticleSystemComponent>().System;
        if (auto* instance = scene.GetVfxWorld().Find(e.GetUUID()))
            if (instance->Asset() && instance->Asset()->assetID == id)
                return instance->Asset();
        if (assets && id.IsValid())
        {
            auto result = assets->Request<VfxSystemAsset>(id);
            if (result.IsReady())
                return result.Asset;
        }
        return nullptr;
    }

    PreviewState State(Scene& scene, Entity e)
    {
        auto at = States().find(e.GetUUID());
        return at == States().end() ? PreviewState::Authored : at->second;
    }

    bool Play(Scene& scene, Entity e)
    {
        auto* i = Running(scene, e);
        if (!i)
            return false;
        i->TimelineDesiredAge = false;
        if (!i->IsSpawning() || i->IsComplete())
            i->Activate(true);
        i->Resume();
        States()[e.GetUUID()] = PreviewState::Playing;
        return true;
    }

    bool Pause(Scene& scene, Entity e)
    {
        auto* i = Running(scene, e);
        if (!i)
            return false;
        i->Pause();
        States()[e.GetUUID()] = PreviewState::Paused;
        return true;
    }

    bool Restart(Scene& scene, Entity e)
    {
        auto* i = Running(scene, e);
        if (!i)
            return false;
        i->TimelineDesiredAge = false;
        i->Activate(true);
        States()[e.GetUUID()] = PreviewState::Playing;
        return true;
    }

    bool Seek(Scene& scene, Entity e, double seconds, float fixedStep)
    {
        auto* i = Running(scene, e);
        if (!i)
            return false;
        // The card owns stepping while scrubbing, as the Timeline particle track does.
        i->TimelineDesiredAge = true;
        i->SeekAge(std::max(0.0, seconds), fixedStep, 60.0, scene.GetWorldTransform(e));
        States()[e.GetUUID()] = PreviewState::Scrubbing;
        return true;
    }

    void Stop(Scene& scene, Entity e)
    {
        States().erase(e.GetUUID());
        auto* i = Running(scene, e);
        if (!i)
            return;
        i->TimelineDesiredAge = false;
        i->Resume();
        if (e.GetComponent<ParticleSystemComponent>().AutoActivate)
            i->Activate(true);
        else
            i->DeactivateImmediate();
    }

    double Age(Scene& scene, Entity e)
    {
        auto* i = e && e.HasComponent<ParticleSystemComponent>() ? scene.GetVfxWorld().Find(e.GetUUID()) : nullptr;
        return i ? i->Age() : 0.0;
    }

    std::vector<EmitterCount> Counts(Scene& scene, Entity e)
    {
        std::vector<EmitterCount> r;
        auto* i = e && e.HasComponent<ParticleSystemComponent>() ? scene.GetVfxWorld().Find(e.GetUUID()) : nullptr;
        if (i)
            for (const auto& x : i->Emitters())
                r.push_back({x.Compiled->Settings.Name, x.ParticleCount, x.Compiled->Settings.Capacity});
        return r;
    }
} // namespace Kans::ParticleSystemEditing

#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "Kans3D/Scene/Entity.h"
#include "Kans3D/VFX/VfxWorld.h"

namespace Kans
{
    class AssetManager;
}

// ImGui-free editing behind the Particle System card: the assigned system's user parameters with
// the component's overrides (stored on the component and serialized with the scene), and the
// in-editor preview that plays, pauses, restarts and scrubs an effect in the edited scene without
// touching serialized state.
namespace Kans::ParticleSystemEditing
{
    struct UserParameterRow
    {
        Vfx::Parameter Spec;               // Name, type, default and range from the system.
        bool           Overridden = false; // The component stores a value.
        Vfx::Value     Value;              // The override, or the system default.
    };
    struct UserParameters
    {
        std::vector<UserParameterRow> Rows;
        // Overrides the system no longer declares (renamed, removed or retyped): dropped on the next
        // edit and ignored at runtime.
        std::vector<std::string> Warnings;
    };
    UserParameters ListUserParameters(const ParticleSystemComponent&, const Vfx::CompiledSystem&);
    // The component with one override set (or reset to the system default); stale overrides dropped.
    ParticleSystemComponent SetOverride(ParticleSystemComponent, const Vfx::CompiledSystem&, const std::string& name,
                                        const Vfx::Value& value);
    ParticleSystemComponent ResetOverride(ParticleSystemComponent, const Vfx::CompiledSystem&, const std::string& name);

    // The compiled system the entity's component names: the running instance's, or the loaded asset.
    Ref<const VfxSystemAsset> ResolveSystem(Scene&, Entity, AssetManager*);

    // ---- In-editor preview (edit mode only) ----
    enum class PreviewState
    {
        Authored, // The edit-mode preview runs as authored.
        Playing,
        Paused,
        Scrubbing // Seeked: the card owns the time until Play or Stop.
    };
    PreviewState State(Scene&, Entity);
    // Each returns false when the effect has no running instance (no asset, or edit-mode preview off).
    bool         Play(Scene&, Entity);
    bool         Pause(Scene&, Entity);
    bool         Restart(Scene&, Entity);
    // Desired age in fixed steps, like the Timeline particle track.
    bool         Seek(Scene&, Entity, double seconds, float fixedStep = 1.f / 60);
    // Back to the authored state: a fresh run as the component describes it.
    void         Stop(Scene&, Entity);
    double       Age(Scene&, Entity);
    // Live particle counts per emitter (name, count, capacity).
    struct EmitterCount
    {
        std::string Name;
        uint32_t    Count = 0, Capacity = 0;
    };
    std::vector<EmitterCount> Counts(Scene&, Entity);
} // namespace Kans::ParticleSystemEditing

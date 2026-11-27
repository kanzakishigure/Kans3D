#include "kspch.h"
#include <limits>
#include <glm/gtx/quaternion.hpp>
#include "Kans3D/Animation/ContactTracks.h"
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Asset/Runtime/MaterialParameterCollection.h"
#include "Kans3D/Audio/MusicSetPlayer.h"
#include "Kans3D/Renderer/Shading/Skinning.h"
#include "Kans3D/Scene/Entity.h"
#include "Kans3D/Script/ScriptEngine.h"
#include "Kans3D/Script/ScriptTimeline.h"
#include "Kans3D/VFX/VfxWorld.h"
#include "Timeline.h"
#include "TimelineAudioPlayer.h"
#include "TimelineProperty.h"
namespace Kans
{
    namespace
    {
        void ResetBoundSecondaryMotion(Scene&                             scene,
                                       const TimelinePlayerComponent*     component,
                                       const TimelineData*                data,
                                       const std::map<std::string, UUID>& spawned)
        {
            const bool bound =
                (component && !component->Bindings.empty()) || (data && !data->Bindings.empty()) || !spawned.empty();
            scene.ResetTransformConstraints();
            if (!bound)
            {
                scene.ResetSecondaryMotion();
                return;
            }
            if (component)
                for (const auto& [binding, id] : component->Bindings)
                    scene.ResetSecondaryMotion(scene.GetEntityByUUID(id));
            for (const auto& [binding, id] : spawned)
                scene.ResetSecondaryMotion(scene.GetEntityByUUID(id));
        }
        AnimatorComponent RuntimeCopy(const AnimatorComponent& a)
        {
            AnimatorComponent b(a);
            b.Player        = a.Player;
            b.SkinPose      = a.SkinPose;
            b.JointEntities = a.JointEntities;
            b.Events        = a.Events;
            b.PreviewPose   = a.PreviewPose;
            b.PoseVersion   = a.PoseVersion;
            b.LastError     = a.LastError;
            return b;
        }
        template<class T>
        void Save(Entity e, std::optional<T>& value)
        {
            if (e.HasComponent<T>())
                value = e.GetComponent<T>();
        }
        template<class T>
        void Restore(Entity e, const std::optional<T>& value)
        {
            if (value)
            {
                if (e.HasComponent<T>())
                    e.SetComponent<T>(*value);
                else
                    e.AddComponent<T>(*value);
            }
            else if (e.HasComponent<T>())
                e.RemoveComponent<T>();
        }
        template<class T>
        Ref<const T> Load(Scene& scene, AssetID id)
        {
            if (auto assets = scene.GetAssetManager(); assets && id.IsValid())
            {
                auto loaded = assets->Load<T>(id);
                if (auto a = std::get_if<Ref<const T>>(&loaded))
                    return *a;
            }
            return {};
        }
    } // namespace
    struct TimelineBoundState
    {
        struct ParticleState
        {
            UUID                       Entity {0};
            const Vfx::SystemInstance* Instance = nullptr;
            Ref<const VfxSystemAsset>  Source;
            size_t                     Section   = size_t(-1);
            double                     Time      = -1;
            bool                       WasActive = false, WasDesiredAge = false;
            Vfx::ParameterMap          Parameters;
        };
        std::map<std::string, ParticleState> Particles;
        void                                 ReleaseParticles(Scene& scene)
        {
            for (const auto& [binding, state] : Particles)
                if (auto instance = scene.GetVfxWorld().Find(state.Entity);
                    instance && instance == state.Instance && instance->Asset() == state.Source)
                {
                    instance->TimelineDesiredAge = state.WasDesiredAge;
                    instance->Deactivate();
                }
        }
        struct Entry
        {
            TransformComponent                                                            Transform;
            std::optional<ActiveComponent>                                                Active;
            std::optional<CameraComponent>                                                Camera;
            std::optional<PointLightComponent>                                            Point;
            std::optional<DirLightComponent>                                              Directional;
            std::optional<SpotLightComponent>                                             Spot;
            std::optional<AreaLightComponent>                                             Area;
            std::optional<TextComponent>                                                  Text;
            std::optional<SpriteRendererComponent>                                        Sprite;
            std::optional<PrefabInstanceComponent>                                        Prefab;
            std::optional<AnimatorComponent>                                              AnimatorData;
            std::optional<Animator>                                                       AnimatorValue;
            std::optional<ModelPoseComponent>                                             ModelPose;
            std::optional<AimConstraintComponent>                                         AimConstraint;
            std::optional<IKComponent>                                                    IK;
            Ref<IKRuntime>                                                                IKState;
            std::optional<MaterialParameterDriversComponent>                              Drivers;
            std::vector<std::pair<std::string, std::array<float, 4>>>                     Overrides;
            std::map<uint32_t, std::vector<std::pair<std::string, std::array<float, 4>>>> SlotOverrides;
            Ref<InstanceMaterialParameters>                                               Values;
            std::optional<InstanceMaterialParameters>                                     ValueCopy;
            std::map<uint32_t, Ref<InstanceMaterialParameters>>                           SlotValues;
            std::map<uint32_t, InstanceMaterialParameters>                                SlotValueCopies;
            std::optional<SkinningPose>                                                   Skin;
        };
        std::map<UUID, Entry>                             Entities;
        std::map<UUID, bool>                              Cameras;
        std::map<UUID, Animator>                          Bases;
        std::set<UUID>                                    EvaluatedAnimators;
        bool                                              LiveAdvance = false;
        std::map<std::string, glm::vec4>                  MaterialParameters;
        std::map<std::pair<UUID, std::string>, glm::vec4> Properties;
    };
    TimelinePlayerComponent::TimelinePlayerComponent(const TimelinePlayerComponent& c) :
        Timeline(c.Timeline), Bindings(c.Bindings), PlayOnStart(c.PlayOnStart), Loop(c.Loop), Speed(c.Speed),
        WrapMode(c.WrapMode), ClockSource(c.ClockSource), AudioClockTrack(c.AudioClockTrack),
        AudioOffset(c.AudioOffset), Source(c.Source)
    {}
    TimelinePlayerComponent& TimelinePlayerComponent::operator=(const TimelinePlayerComponent& c)
    {
        if (this != &c)
        {
            TimelinePlayerComponent copy(c);
            *this = std::move(copy);
        }
        return *this;
    }
    bool operator==(const TimelinePlayerComponent& a, const TimelinePlayerComponent& b)
    {
        return a.Timeline == b.Timeline && a.Bindings == b.Bindings && a.PlayOnStart == b.PlayOnStart &&
               a.Loop == b.Loop && a.Speed == b.Speed && a.WrapMode == b.WrapMode && a.ClockSource == b.ClockSource &&
               a.AudioClockTrack == b.AudioClockTrack && a.AudioOffset == b.AudioOffset;
    }
    TimelinePlayer::TimelinePlayer(Scene& scene, UUID owner, Ref<const TimelineAsset> source) :
        m_Scene(scene), m_Owner(owner), m_Source(std::move(source))
    {}
    TimelinePlayer::~TimelinePlayer() =
        default; // Scene owns destruction order; cleanup is explicit in Stop/runtime stop.
    TimelinePlayerComponent* TimelinePlayer::Component() const
    {
        auto e = m_Scene.GetEntityByUUID(m_Owner);
        return e && e.HasComponent<TimelinePlayerComponent>() ? &e.GetComponent<TimelinePlayerComponent>() : nullptr;
    }
    bool TimelinePlayer::Fail(std::string text)
    {
        m_Error = AssetError {AssetErrorCode::InvalidData, "Timeline: " + text};
        return false;
    }
    uint64_t TimelinePlayer::Subscribe(EventSink sink)
    {
        auto id = m_NextToken++;
        m_Events.emplace(id, std::move(sink));
        return id;
    }
    uint64_t TimelinePlayer::SubscribeFinished(std::function<void()> sink)
    {
        auto id = m_NextToken++;
        m_Finished.emplace(id, std::move(sink));
        return id;
    }
    void TimelinePlayer::Unsubscribe(uint64_t token)
    {
        m_Events.erase(token);
        m_Finished.erase(token);
    }
    bool TimelinePlayer::CaptureEntity(UUID id)
    {
        if (m_State->Entities.count(id))
            return true;
        auto e = m_Scene.GetEntityByUUID(id);
        if (!e)
            return Fail("bound entity is missing");
        auto& s     = m_State->Entities[id];
        s.Transform = e.GetComponent<TransformComponent>();
        Save(e, s.Active);
        Save(e, s.Camera);
        Save(e, s.Point);
        Save(e, s.Directional);
        Save(e, s.Spot);
        Save(e, s.Area);
        Save(e, s.Text);
        Save(e, s.Sprite);
        Save(e, s.Prefab);
        Save(e, s.ModelPose);
        Save(e, s.Drivers);
        Save(e, s.AimConstraint);
        Save(e, s.IK);
        if (s.IK)
            s.IKState = e.GetComponent<IKComponent>().Runtime;
        if (e.HasComponent<AnimatorComponent>())
        {
            const auto& a  = e.GetComponent<AnimatorComponent>();
            s.AnimatorData = RuntimeCopy(a);
            if (a.Player)
                s.AnimatorValue = *a.Player;
            if (a.SkinPose)
                s.Skin = *a.SkinPose;
        }
        else if (s.ModelPose && s.ModelPose->SkinPose)
            s.Skin = *s.ModelPose->SkinPose;
        if (s.Drivers)
        {
            const auto& d   = e.GetComponent<MaterialParameterDriversComponent>();
            s.Overrides     = d.Overrides;
            s.SlotOverrides = d.SlotOverrides;
            s.Values        = d.Values;
            s.SlotValues    = d.SlotValues;
            if (d.Values)
                s.ValueCopy = *d.Values;
            for (const auto& [slot, values] : d.SlotValues)
                if (values)
                    s.SlotValueCopies[slot] = *values;
        }
        for (auto child : m_Scene.GetChildren(e))
            if (!CaptureEntity(child.GetUUID()))
                return false;
        return true;
    }
    bool TimelinePlayer::CaptureBoundState()
    {
        if (m_State)
            return true;
        if (!m_Source || !Component())
            return Fail("missing asset or player component");
        auto valid = ValidateTimeline(m_Source->GetData());
        if (auto e = std::get_if<AssetError>(&valid))
        {
            m_Error = *e;
            return false;
        }
        m_State = std::make_unique<TimelineBoundState>();
        for (const auto& b : m_Source->GetData().Bindings)
            if (b.Kind == TimelineBindingKind::Possessable)
            {
                auto found = Component()->Bindings.find(b.ID);
                if (found == Component()->Bindings.end() || !CaptureEntity(found->second))
                {
                    m_State.reset();
                    return Fail("unresolved binding " + b.ID);
                }
            }
        for (const auto& track : m_Source->GetData().Properties)
            if (const auto names = MaterialParameterPath(track.Path))
            {
                glm::vec4   base;
                const auto* parameter = m_Scene.FindMaterialParameter(names->first, names->second);
                if (!parameter || (track.UseTimelineTime && parameter->Type != MaterialParameterType::Scalar) ||
                    !TimelineProperty(m_Scene, {}, track.Path, base, false))
                {
                    m_State.reset();
                    return Fail("invalid collection property " + track.Path);
                }
                m_State->MaterialParameters.emplace(track.Path, base);
            }
        auto cameras = [&](auto&& self, Entity e) -> void {
            if (e.HasComponent<CameraComponent>())
                m_State->Cameras[e.GetUUID()] = e.GetComponent<CameraComponent>().Primary;
            for (auto child : m_Scene.GetChildren(e))
                self(self, child);
        };
        for (auto e : m_Scene.GetRootEntities())
            cameras(cameras, e);
        return true;
    }
    void TimelinePlayer::RestoreCameras()
    {
        if (!m_State)
            return;
        for (const auto& [id, primary] : m_State->Cameras)
            if (auto e = m_Scene.GetEntityByUUID(id); e && e.HasComponent<CameraComponent>())
                e.GetComponent<CameraComponent>().Primary = primary;
    }
    void TimelinePlayer::RemoveSpawned()
    {
        for (const auto& [binding, id] : m_Spawned)
            if (auto e = m_Scene.GetEntityByUUID(id))
            {
                auto erase = [&](auto&& self, Entity node) -> void {
                    for (auto child : m_Scene.GetChildren(node))
                        self(self, child);
                    if (m_State)
                    {
                        m_State->Entities.erase(node.GetUUID());
                        m_State->Bases.erase(node.GetUUID());
                    }
                };
                erase(erase, e);
                m_Scene.DestroyEntity(e);
            }
        m_Spawned.clear();
    }
    void TimelinePlayer::RestoreBoundState()
    {
        m_Audio.reset();
        m_Playing = false;
        m_Scene.ResetTransformConstraints();
        RemoveSpawned();
        if (!m_State)
            return;
        RestoreCameras();
        for (auto& [path, value] : m_State->MaterialParameters)
            TimelineProperty(m_Scene, {}, path, value, true);
        for (const auto& [binding, state] : m_State->Particles)
            if (auto instance = m_Scene.GetVfxWorld().Find(state.Entity);
                instance && instance == state.Instance && instance->Asset() == state.Source)
            {
                instance->TimelineDesiredAge = state.WasDesiredAge;
                instance->DeactivateImmediate();
                for (const auto& [name, value] : state.Parameters)
                    instance->Set(name, value);
                if (state.WasActive)
                    instance->Activate();
            }
        for (const auto& [id, s] : m_State->Entities)
            if (auto e = m_Scene.GetEntityByUUID(id))
            {
                e.SetComponent<TransformComponent>(s.Transform);
                if (s.Active)
                    m_Scene.SetEntityActive(e, s.Active->Active);
                else
                {
                    m_Scene.SetEntityActive(e, true);
                    if (e.HasComponent<ActiveComponent>())
                        e.RemoveComponent<ActiveComponent>();
                }
                Restore(e, s.Camera);
                Restore(e, s.Point);
                Restore(e, s.Directional);
                Restore(e, s.Spot);
                Restore(e, s.Area);
                Restore(e, s.Text);
                Restore(e, s.Sprite);
                Restore(e, s.Prefab);
                if (s.AnimatorData)
                {
                    auto a = RuntimeCopy(*s.AnimatorData);
                    if (a.Player && s.AnimatorValue)
                        *a.Player = *s.AnimatorValue;
                    if (a.SkinPose && s.Skin)
                        *a.SkinPose = *s.Skin;
                    if (e.HasComponent<AnimatorComponent>())
                        e.GetComponent<AnimatorComponent>() = std::move(a);
                    else
                        e.AddComponent<AnimatorComponent>(std::move(a));
                }
                else if (e.HasComponent<AnimatorComponent>())
                    e.RemoveComponent<AnimatorComponent>();
                if (e.HasComponent<AnimatorComponent>())
                    e.PatchComponent<AnimatorComponent>([](auto&) {});
                Restore(e, s.ModelPose);
                Restore(e, s.AimConstraint);
                Restore(e, s.IK);
                if (s.IK)
                    e.GetComponent<IKComponent>().Runtime = s.IKState;
                if (s.ModelPose && s.ModelPose->SkinPose && s.Skin)
                    *s.ModelPose->SkinPose = *s.Skin;
                Restore(e, s.Drivers);
                if (s.Drivers)
                {
                    auto& d         = e.GetComponent<MaterialParameterDriversComponent>();
                    d.Overrides     = s.Overrides;
                    d.SlotOverrides = s.SlotOverrides;
                    d.Values        = s.Values;
                    d.SlotValues    = s.SlotValues;
                    if (d.Values && s.ValueCopy)
                        *d.Values = *s.ValueCopy;
                    for (const auto& [slot, v] : s.SlotValueCopies)
                        if (d.SlotValues[slot])
                            *d.SlotValues[slot] = v;
                }
            }
        m_State.reset();
        m_Error.reset();
        m_Time      = 0;
        m_Entered   = false;
        m_Completed = false;
        m_Fired.clear();
        m_Scene.UpdateBounds();
    }
    bool TimelinePlayer::SetBinding(const std::string& name, UUID id)
    {
        if (!Component() || !m_Source || !id || !m_Scene.GetEntityByUUID(id))
            return false;
        for (const auto& b : m_Source->GetData().Bindings)
            if (b.Name == name || b.ID == name)
            {
                if (b.Kind != TimelineBindingKind::Possessable)
                    return false;
                // Release the old actor before taking a new baseline, including while paused in preview.
                bool   playing = m_Playing, captured = bool(m_State), entered = m_Entered, completed = m_Completed;
                double time  = m_Time;
                auto   fired = m_Fired;
                RestoreBoundState();
                Component()->Bindings[b.ID] = id;
                if (captured && (!CaptureBoundState() || !Evaluate(time)))
                    return false;
                m_Time      = time;
                m_Playing   = playing;
                m_Entered   = entered;
                m_Completed = completed;
                m_Fired     = std::move(fired);
                return true;
            }
        return false;
    }
    bool TimelinePlayer::EvaluateAnimation(const TimelineAnimationTrack& track, UUID id, double t)
    {
        auto e = m_Scene.GetEntityByUUID(id);
        if (!e)
            return true;
        Ref<const SkeletonAsset> skeleton;
        Animator*                animator = nullptr;
        if (e.HasComponent<AnimatorComponent>())
        {
            // An explicit play/scrub must work on a cold CPU asset cache on this call.
            auto&   authored   = e.GetComponent<AnimatorComponent>();
            AssetID skeletonID = authored.Skeleton;
            if (!skeletonID.IsValid() && e.HasComponent<PrefabInstanceComponent>())
            {
                const auto& prefab = e.GetComponent<PrefabInstanceComponent>();
                if (prefab.Source)
                    skeletonID = prefab.Source->GetMeshData().Skeleton;
            }
            if (!authored.SkeletonSource && skeletonID.IsValid())
                authored.SkeletonSource = Load<SkeletonAsset>(m_Scene, skeletonID);
            if (!authored.InlineController && !authored.ControllerSource && authored.Controller.IsValid())
                authored.ControllerSource = Load<AnimationControllerAsset>(m_Scene, authored.Controller);
            const auto* controller = authored.InlineController ? &*authored.InlineController :
                                     authored.ControllerSource ? &authored.ControllerSource->GetData() :
                                                                 nullptr;
            if (controller)
                for (const auto& motion : controller->Motions)
                    if (motion.Clip.IsValid() && !motion.InlineClip && !authored.ClipSources.count(motion.Clip))
                    {
                        auto source = Load<AnimationClipAsset>(m_Scene, motion.Clip);
                        if (!source)
                            return Fail("missing controller clip");
                        authored.ClipSources[motion.Clip] = source;
                    }
            if (!m_Scene.PrepareAnimator(e))
                return Fail("cannot prepare Animator");
            auto& a  = e.GetComponent<AnimatorComponent>();
            skeleton = a.SkeletonSource;
            if (!m_State->Bases.count(id))
                m_State->Bases.emplace(id, *a.Player);
            if (m_State->EvaluatedAnimators.insert(id).second && !m_State->LiveAdvance)
            {
                *a.Player = m_State->Bases.at(id);
                a.Player->Advance(t);
            }
            animator = a.Player.get();
        }
        else
        {
            AssetID skeletonID;
            for (const auto& b : m_Source->GetData().Bindings)
                if (b.ID == track.Binding)
                    skeletonID = b.Skeleton;
            if (e.HasComponent<ModelPoseComponent>())
                skeleton = e.GetComponent<ModelPoseComponent>().SkeletonSource;
            if (!skeleton && e.HasComponent<IKComponent>())
            {
                const auto& c = e.GetComponent<IKComponent>();
                skeleton      = c.SkeletonSource;
                if (!skeletonID.IsValid())
                    skeletonID = c.Skeleton;
            }
            if (!skeleton && e.HasComponent<ShapeKeysComponent>())
            {
                const auto& c = e.GetComponent<ShapeKeysComponent>();
                skeleton      = c.SkeletonSource;
                if (!skeletonID.IsValid())
                    skeletonID = c.Skeleton;
            }
            if (!skeletonID.IsValid() && e.HasComponent<PrefabInstanceComponent>())
            {
                const auto& c = e.GetComponent<PrefabInstanceComponent>();
                if (c.Source)
                    skeletonID = c.Source->GetMeshData().Skeleton;
            }
            if (!skeletonID.IsValid())
            {
                auto mesh = m_Scene.ResolveMesh(e);
                if (!mesh && e.HasComponent<MeshComponent>() && m_Scene.GetAssetManager())
                {
                    Load<MeshAsset>(m_Scene, e.GetComponent<MeshComponent>().Mesh);
                    mesh = m_Scene.ResolveMesh(e);
                }
                if (mesh)
                    skeletonID = mesh->GetData().Skeleton;
            }
            if (!skeleton && skeletonID.IsValid())
            {
                if (!Skeletons.count(skeletonID))
                    Skeletons[skeletonID] = Load<SkeletonAsset>(m_Scene, skeletonID);
                skeleton = Skeletons[skeletonID];
            }
        }
        if (!skeleton)
            return Fail("animation binding has no skeleton");
        AnimationPose mixed;
        RestPose(skeleton->GetData(), mixed);
        // Normalize automatic overlap weights before authored envelopes. Three-way overlaps
        // must not expose the underlying state machine merely because pairwise products sum below one.
        auto overlapWeight = [&](const TimelineAnimationSection& section) {
            float weight = 1;
            for (const auto& other : track.Clips)
                if (&other != &section && other.Start != section.Start &&
                    TimelineRange {other.Start, other.End}.Contains(t))
                {
                    if (other.Start < section.Start)
                    {
                        double end = std::min(section.End, other.End);
                        weight *= float(std::clamp((t - section.Start) / (end - section.Start), 0.0, 1.0));
                    }
                    else
                    {
                        double end = std::min(section.End, other.End);
                        weight *= float(std::clamp((end - t) / (end - other.Start), 0.0, 1.0));
                    }
                }
            return weight;
        };
        float automaticTotal = 0;
        for (const auto& section : track.Clips)
            if (TimelineRange {section.Start, section.End}.Contains(t))
                automaticTotal += overlapWeight(section);
        float total = 0;
        for (const auto& section : track.Clips)
            if (TimelineRange {section.Start, section.End}.Contains(t))
            {
                if (!Clips.count(section.Clip) && e.HasComponent<AnimatorComponent>())
                {
                    auto& sources = e.GetComponent<AnimatorComponent>().ClipSources;
                    if (auto found = sources.find(section.Clip); found != sources.end())
                        Clips[section.Clip] = found->second;
                }
                if (!Clips.count(section.Clip))
                    Clips[section.Clip] = Load<AnimationClipAsset>(m_Scene, section.Clip);
                auto source = Clips[section.Clip];
                if (!source)
                    return Fail("missing animation clip");
                const auto& clip = source->GetData();
                double      out  = section.ClipOut == 0 ? clip.Duration : section.ClipOut;
                if (section.ClipIn >= out || out > clip.Duration)
                    return Fail("clip trim exceeds duration");
                float weight = 1;
                if (section.BlendIn > 0)
                    weight = std::min(weight, float((t - section.Start) / section.BlendIn));
                if (section.BlendOut > 0)
                    weight = std::min(weight, float((section.End - t) / section.BlendOut));
                if (automaticTotal > 0)
                    weight *= overlapWeight(section) / automaticTotal;
                if (weight <= 0)
                    continue;
                AnimationPose sample;
                auto          bound = BindAnimationClip(skeleton->GetData(), clip);
                SampleAnimation(skeleton->GetData(),
                                bound,
                                std::clamp(section.ClipIn + (t - section.Start) * section.Speed, section.ClipIn, out),
                                false,
                                sample);
                if (animator)
                    if (auto settings = FindClipRetarget(animator->Retargets, bound.Clip))
                        ApplyClipRetarget(
                            skeleton->GetData(),
                            bound,
                            *settings,
                            std::clamp(section.ClipIn + (t - section.Start) * section.Speed, section.ClipIn, out),
                            sample);
                if (total == 0)
                    mixed = std::move(sample);
                else
                    BlendAnimationPoses(mixed, sample, weight / (total + weight), mixed);
                total += weight;
            }
        if (animator)
        {
            std::string slot = track.Slot;
            if (slot.empty())
            {
                for (const auto& s : animator->GetActions())
                    if (s.Slot == "Timeline")
                        slot = s.Slot;
                if (slot.empty())
                    for (const auto& s : animator->GetActions())
                        if (!s.Slot.empty())
                        {
                            slot = s.Slot;
                            break;
                        }
            }
            if (slot.empty())
                slot = "Timeline";
            if (total > 0)
            {
                if (!animator->SetTimelinePose(slot, mixed, std::min(total, 1.f)))
                    return Fail("Animator has no requested slot " + slot);
            }
            else
                animator->ClearTimelinePose(slot);
            m_Scene.ApplyAnimatorPose(e, animator->GetPose());
        }
        else
        {
            if (total < 1)
            {
                AnimationPose rest;
                RestPose(skeleton->GetData(), rest);
                BlendAnimationPoses(rest, mixed, total, mixed);
            }
            if (!m_Scene.m_DeferIK)
                m_Scene.EvaluatePoseIK(e, skeleton->GetData(), mixed);
            else
                BuildModelPose(skeleton->GetData(), mixed);
            if (!e.HasComponent<ModelPoseComponent>())
                e.AddComponent<ModelPoseComponent>();
            auto& state          = e.GetComponent<ModelPoseComponent>();
            state.SkeletonSource = skeleton;
            state.Pose           = mixed;
            state.JointEntities.resize(skeleton->GetData().Joints.size());
            for (size_t j = 0; j < state.JointEntities.size(); ++j)
                state.JointEntities[j] = Scene::PrefabEntityUUID(id, skeleton->GetData().Joints[j].NodeKey);
            m_Scene.ApplyModelPose(
                e, skeleton->GetData(), mixed, state.JointEntities, state.SkinPose, state.PoseVersion);
        }
        return true;
    }
    bool TimelinePlayer::EvaluateParticles(const TimelineParticleTrack& track, UUID id, double time)
    {
        auto entity = m_Scene.GetEntityByUUID(id);
        if (!entity.HasComponent<ParticleSystemComponent>())
            return Fail("particle track " + track.Binding + " requires ParticleSystemComponent");
        auto& world    = m_Scene.GetVfxWorld();
        auto  instance = world.Find(id);
        if (!instance)
        {
            auto system = entity.GetComponent<ParticleSystemComponent>().System;
            auto found  = ParticleSystems.find(system);
            auto source = found == ParticleSystems.end() ? Load<VfxSystemAsset>(m_Scene, system) : found->second;
            if (!source || !world.Bind(entity, source))
                return Fail("particle track " + track.Binding + " missing particle system");
            instance = world.Find(id);
        }
        if (auto validation = ValidateTimelineParticleParameters(track, *instance->Asset());
            auto error      = std::get_if<AssetError>(&validation))
        {
            m_Error = *error;
            return false;
        }
        auto& state = m_State->Particles[track.Binding];
        if (state.Instance != instance || state.Entity != id || state.Source != instance->Asset())
        {
            state               = {};
            state.Entity        = id;
            state.Instance      = instance;
            state.Source        = instance->Asset();
            state.WasActive     = instance->IsActive();
            state.WasDesiredAge = instance->TimelineDesiredAge;
            for (const auto& p : track.Parameters)
                state.Parameters[p.Name] = instance->Get(p.Name, p.Type);
            instance->DeactivateImmediate();
        }
        instance->TimelineDesiredAge = track.DesiredAge;
        auto parameters              = [&](double t) {
            for (const auto& p : track.Parameters)
                instance->Set(p.Name, Vfx::Value::Vector(p.Curve.Sample(t, state.Parameters.at(p.Name).Data), p.Type));
        };
        size_t selected = size_t(-1);
        for (size_t i = 0; i < track.Sections.size(); ++i)
            if (time >= track.Sections[i].Start)
                selected = i;
        if (selected == size_t(-1))
        {
            if (state.Time < 0 || state.Section != selected)
                instance->DeactivateImmediate();
        }
        else
        {
            const auto& section = track.Sections[selected];
            bool        changed =
                state.Section != selected || (state.Time >= section.End && time < section.End &&
                                              (!track.DesiredAge || section.OnEnd == TimelineParticleOnEnd::Kill));
            if (track.DesiredAge)
            {
                const double duration = section.End - section.Start;
                const bool   kill     = time >= section.End && section.OnEnd == TimelineParticleOnEnd::Kill;
                if (kill)
                {
                    if (changed || state.Time < section.End)
                        instance->DeactivateImmediate();
                }
                else
                {
                    // Re-evaluate curves at every historical tick, so replay equals straight fixed-step playback.
                    parameters(section.Start);
                    instance->SeekAge(time - section.Start,
                                      track.FixedTick,
                                      track.MaxSimulationTime,
                                      m_Scene.GetWorldTransform(entity),
                                      changed,
                                      [&](double age) {
                                          parameters(section.Start + age);
                                          if (age + track.FixedTick * 1e-6 >= duration)
                                              instance->Deactivate();
                                      });
                    if (time >= section.End)
                        instance->Deactivate();
                }
            }
            else if (time >= section.End)
            {
                if (section.OnEnd == TimelineParticleOnEnd::Finish)
                    instance->Deactivate();
                else if (changed || state.Time < section.End)
                    instance->DeactivateImmediate();
            }
            else if (changed || time < state.Time)
            {
                parameters(time);
                instance->Activate();
            }
        }
        state.Section = selected;
        state.Time    = time;
        parameters(time);
        return true;
    }
    bool TimelinePlayer::Evaluate(double t)
    {
        if (!std::isfinite(t))
            return Fail("non-finite evaluation time");
        if (!m_State || !m_State->LiveAdvance)
            ResetBoundSecondaryMotion(m_Scene, Component(), m_Source ? &m_Source->GetData() : nullptr, m_Spawned);
        if (!CaptureBoundState())
            return false;
        m_Error.reset();
        const auto& data = m_Source->GetData();
        t                = std::clamp(t, 0.0, data.Length);
        auto* component  = Component();
        if (!component)
            return false;
        using Binding = std::pair<std::string, UUID>;
        std::vector<Binding> bindings(component->Bindings.begin(), component->Bindings.end());
        auto                 findBinding = [&](const std::string& id) {
            return std::lower_bound(bindings.begin(), bindings.end(), id, [](const Binding& entry, const auto& key) {
                return entry.first < key;
            });
        };
        for (const auto& b : data.Bindings)
            if (b.Kind == TimelineBindingKind::Spawnable)
            {
                const bool exists = TimelineBindingRange(data, b).Contains(t);
                if (exists && !m_Spawned.count(b.ID))
                {
                    auto prefab = Load<PrefabAsset>(m_Scene, b.Prefab);
                    if (m_Scene.GetAssetManager() && !prefab)
                        return Fail("missing spawnable prefab");
                    if (prefab)
                    {
                        for (const auto& node : prefab->GetEntities())
                            if (node.Mesh.IsValid() && !Load<MeshAsset>(m_Scene, node.Mesh))
                                return Fail("missing spawnable mesh");
                        auto skeletonID = prefab->GetMeshData().Skeleton;
                        if (skeletonID.IsValid())
                        {
                            auto s = Load<SkeletonAsset>(m_Scene, skeletonID);
                            if (!s)
                                return Fail("missing spawnable skeleton");
                            Skeletons[skeletonID] = s;
                        }
                    }
                    auto e = m_Scene.InstantiatePrefab(b.Prefab, b.SpawnTransform.Translation);
                    if (b.Particles)
                        e.AddComponent<ParticleSystemComponent>(*b.Particles);
                    if (prefab)
                        e.GetComponent<PrefabInstanceComponent>().Source = prefab;
                    TransformComponent tr;
                    tr.Position = b.SpawnTransform.Translation;
                    tr.Rotation = glm::eulerAngles(b.SpawnTransform.Rotation);
                    tr.Scale    = b.SpawnTransform.Scale;
                    e.SetComponent<TransformComponent>(tr);
                    m_Spawned[b.ID] = e.GetUUID();
                    m_Scene.UpdatePrefabs();
                    CaptureEntity(e.GetUUID());
                }
                else if (!exists && m_Spawned.count(b.ID))
                {
                    auto id = m_Spawned.at(b.ID);
                    if (auto e = m_Scene.GetEntityByUUID(id))
                    {
                        auto erase = [&](auto&& self, Entity node) -> void {
                            for (auto child : m_Scene.GetChildren(node))
                                self(self, child);
                            m_State->Entities.erase(node.GetUUID());
                            m_State->Bases.erase(node.GetUUID());
                        };
                        erase(erase, e);
                        m_Scene.DestroyEntity(e);
                    }
                    m_Spawned.erase(b.ID);
                }
                auto       binding          = findBinding(b.ID);
                const bool existsInBindings = binding != bindings.end() && binding->first == b.ID;
                if (auto spawned = m_Spawned.find(b.ID); spawned != m_Spawned.end())
                {
                    if (existsInBindings)
                        binding->second = spawned->second;
                    else
                        bindings.emplace(binding, b.ID, spawned->second);
                }
                else if (existsInBindings)
                    bindings.erase(binding);
            }
        auto entity = [&](const std::string& b) {
            auto found = findBinding(b);
            return found != bindings.end() && found->first == b ? m_Scene.GetEntityByUUID(found->second) : Entity {};
        };
        for (const auto& b : data.Bindings)
            if (auto e = entity(b.ID); e && e.HasComponent<PrefabInstanceComponent>())
            {
                auto& prefab = e.GetComponent<PrefabInstanceComponent>();
                if (prefab.Prefab.IsValid() && !prefab.Expanded && m_Scene.GetAssetManager())
                {
                    auto source = Load<PrefabAsset>(m_Scene, prefab.Prefab);
                    if (!source)
                        return Fail("missing possessable prefab");
                    for (const auto& node : source->GetEntities())
                        if (node.Mesh.IsValid() && !Load<MeshAsset>(m_Scene, node.Mesh))
                            return Fail("missing possessable mesh");
                    auto skeletonID = source->GetMeshData().Skeleton;
                    if (skeletonID.IsValid() && !Load<SkeletonAsset>(m_Scene, skeletonID))
                        return Fail("missing possessable skeleton");
                    e.PatchComponent<PrefabInstanceComponent>([&](auto& c) { c.Source = source; });
                    m_Scene.UpdatePrefabs();
                    for (auto child : m_Scene.GetChildren(e))
                        if (!CaptureEntity(child.GetUUID()))
                            return false;
                }
            }
        for (const auto& b : data.Bindings)
            if (auto e = entity(b.ID))
                for (const auto& c : b.ExpectedComponents)
                    if (!TimelineHasComponent(e, c))
                        return Fail("binding " + b.ID + " expected " + c);
        for (const auto& tr : data.Transforms)
            if (auto e = entity(tr.Binding))
            {
                const auto&        base   = m_State->Entities.at(e.GetUUID()).Transform;
                TransformComponent result = base;
                if (tr.Relative)
                {
                    if (!tr.Position.Keys.empty() || !tr.Rotation.Keys.empty() || !tr.Scale.Keys.empty())
                    {
                        JointTransform delta;
                        delta.Translation = glm::vec3(tr.Position.Sample(t));
                        delta.Rotation    = glm::quat(glm::vec3(tr.Rotation.Sample(t)));
                        delta.Scale       = glm::vec3(tr.Scale.Sample(t, glm::vec4(1)));
                        auto matrix       = base.GetTransform() * delta.Matrix();
                        result.Position   = glm::vec3(matrix[3]);
                        result.Rotation   = glm::eulerAngles(glm::quat(base.Rotation) * delta.Rotation);
                        result.Scale      = base.Scale * delta.Scale;
                        if (std::abs(glm::determinant(matrix)) > std::numeric_limits<float>::epsilon())
                        {
                            auto local      = JointTransform::FromMatrix(matrix);
                            result.Position = local.Translation;
                            result.Rotation = glm::eulerAngles(local.Rotation);
                            result.Scale    = local.Scale;
                        }
                        result.ImportedMatrix   = matrix;
                        result.ImportedPosition = result.Position;
                        result.ImportedRotation = result.Rotation;
                        result.ImportedScale    = result.Scale;
                    }
                }
                else
                {
                    result.Position = glm::vec3(tr.Position.Sample(t, glm::vec4(base.Position, 0)));
                    result.Rotation = glm::vec3(tr.Rotation.Sample(t, glm::vec4(base.Rotation, 0)));
                    result.Scale    = glm::vec3(tr.Scale.Sample(t, glm::vec4(base.Scale, 0)));
                }
                e.SetComponent<TransformComponent>(result);
            }
        for (const auto& tr : data.Activations)
            if (auto e = entity(tr.Binding))
            {
                bool active =
                    std::any_of(tr.Ranges.begin(), tr.Ranges.end(), [&](const auto& r) { return r.Contains(t); });
                m_Scene.SetEntityActive(e, active);
            }
        // Legacy RGB channel tracks decompose radiance into colour and magnitude.
        // Reusing last frame's decomposition accumulates rounding in untouched channels.
        // Start light tracks from the captured component and capture every property base
        // before writing any track, so seeks and forward playback give identical values.
        auto lightPath = [](const std::string& path) {
            return path.rfind("Light.", 0) == 0 || path.rfind("PointLight.", 0) == 0 ||
                   path.rfind("DirLight.", 0) == 0 || path.rfind("SpotLight.", 0) == 0 ||
                   path.rfind("AreaLight.", 0) == 0;
        };
        std::vector<UUID> restoredLights;
        for (const auto& tr : data.Properties)
            if (lightPath(tr.Path))
                if (auto e = entity(tr.Binding); e)
                {
                    const auto id = e.GetUUID();
                    if (std::find(restoredLights.begin(), restoredLights.end(), id) != restoredLights.end())
                        continue;
                    restoredLights.push_back(id);
                    if (auto captured = m_State->Entities.find(e.GetUUID()); captured != m_State->Entities.end())
                    {
                        if (captured->second.Point)
                            e.SetComponent<PointLightComponent>(*captured->second.Point);
                        if (captured->second.Directional)
                            e.SetComponent<DirLightComponent>(*captured->second.Directional);
                        if (captured->second.Spot)
                            e.SetComponent<SpotLightComponent>(*captured->second.Spot);
                        if (captured->second.Area)
                            e.SetComponent<AreaLightComponent>(*captured->second.Area);
                    }
                }
        for (const auto& tr : data.Properties)
            if (lightPath(tr.Path))
                if (auto e = entity(tr.Binding))
                {
                    auto key = std::make_pair(e.GetUUID(), tr.Path);
                    if (!m_State->Properties.count(key))
                    {
                        glm::vec4 base;
                        if (!TimelineProperty(m_Scene, e, tr.Path, base, false))
                            return Fail("unknown property " + tr.Path);
                        m_State->Properties[key] = base;
                    }
                }
        for (const auto& tr : data.Properties)
        {
            if (tr.Binding.empty())
            {
                auto value = tr.UseTimelineTime ? glm::vec4(float(t), 0, 0, 0) :
                                                  tr.Curve.Sample(t, m_State->MaterialParameters.at(tr.Path));
                if (!TimelineProperty(m_Scene, {}, tr.Path, value, true))
                    return Fail("cannot write property " + tr.Path);
            }
            else if (auto e = entity(tr.Binding))
            {
                auto key = std::make_pair(e.GetUUID(), tr.Path);
                if (!m_State->Properties.count(key))
                {
                    glm::vec4 base;
                    if (!TimelineProperty(m_Scene, e, tr.Path, base, false))
                        return Fail("unknown property " + tr.Path);
                    m_State->Properties[key] = base;
                }
                auto value = tr.Curve.Sample(t, m_State->Properties.at(key));
                if (!TimelineProperty(m_Scene, e, tr.Path, value, true))
                    return Fail("cannot write property " + tr.Path);
            }
        }
        m_State->EvaluatedAnimators.clear();
        for (const auto& tr : data.Animations)
            if (auto e = entity(tr.Binding))
                if (!EvaluateAnimation(tr, e.GetUUID(), t))
                    return false;
        if (!m_Scene.m_DeferIK)
        {
            bool           models = false;
            std::vector<UUID> refreshed;
            for (const auto& track : data.Properties)
                if (track.Path.rfind("IK.", 0) == 0)
                    if (auto e = entity(track.Binding); e && !m_State->EvaluatedAnimators.count(e.GetUUID()))
                    {
                        const auto id = e.GetUUID();
                        if (std::find(refreshed.begin(), refreshed.end(), id) != refreshed.end())
                            continue;
                        refreshed.push_back(id);
                        if (e.HasComponent<AnimatorComponent>() && m_Scene.PrepareAnimator(e))
                            m_Scene.ApplyAnimatorPose(e, e.GetComponent<AnimatorComponent>().Player->GetPose());
                        else
                            models = true;
                    }
            if (models)
                m_Scene.UpdateModelPoses();
        }
        for (const auto& tr : data.Particles)
            if (auto e = entity(tr.Binding))
                if (!EvaluateParticles(tr, e.GetUUID(), t))
                    return false;
        RestoreCameras();
        for (const auto& cut : data.CameraCuts)
            if (TimelineRange {cut.Start, cut.End}.Contains(t))
            {
                auto target = entity(cut.Binding);
                if (!target || !target.HasComponent<CameraComponent>())
                    return Fail("camera cut target is not a camera");
                for (const auto& [id, primary] : m_State->Cameras)
                    if (auto e = m_Scene.GetEntityByUUID(id); e && e.HasComponent<CameraComponent>())
                        e.GetComponent<CameraComponent>().Primary = false;
                for (const auto& [b, id] : m_Spawned)
                    if (auto e = m_Scene.GetEntityByUUID(id); e && e.HasComponent<CameraComponent>())
                        e.GetComponent<CameraComponent>().Primary = false;
                target.GetComponent<CameraComponent>().Primary = true;
            }
        if (!m_Scene.m_DeferIK && !m_State->LiveAdvance)
            m_Scene.UpdateTransformConstraints(0);
        m_Scene.UpdateBounds();
        return true;
    }
    bool TimelinePlayer::Play()
    {
        if (m_Playing)
            return true;
        if (m_Completed)
        {
            Stop();
            m_Time = 0;
        }
        if (!CaptureBoundState() || !Evaluate(m_Time))
            return false;
        m_Playing   = true;
        m_Completed = false;
        UpdateAudio(true);
        if (!m_Entered)
        {
            m_Entered = true;
            CrossEvents(m_Time == 0 ? -1 : m_Time, m_Time, m_Time == 0);
        }
        if (m_Source->GetData().Length == 0)
            Finish();
        return true;
    }
    void TimelinePlayer::Pause()
    {
        m_Playing = false;
        if (m_Audio)
            m_Audio->Pause();
    }

    AudioVoiceHandle TimelinePlayer::GetAudioVoice(size_t track) const
    {
        return m_Audio ? m_Audio->GetTrackVoice(track) : AudioVoiceHandle {};
    }

    TimelineAudioStats TimelinePlayer::GetAudioStats() const
    {
        return m_Audio ? m_Audio->GetStats() : TimelineAudioStats {};
    }

    void TimelinePlayer::UpdateAudio(bool seek)
    {
        auto* engine    = m_Scene.GetAudioEngine();
        auto* component = Component();
        if (!engine || !component || m_Source->GetData().Audio.empty() || m_Scene.m_AudioPaused)
            return;
        if (!m_Playing)
        {
            if (seek && m_Audio)
                m_Audio->Scrub(m_Time);
            return;
        }
        if (!m_Audio)
        {
            m_Audio = std::make_unique<TimelineAudioPlayer>(
                *engine,
                m_Source,
                [this](AssetID id) -> Ref<const AudioClipAsset> {
                    if (auto it = AudioClips.find(id); it != AudioClips.end())
                        return it->second;
                    if (auto* assets = m_Scene.GetAssetManager())
                    {
                        auto clip = assets->Request<AudioClipAsset>(id);
                        if (clip.IsReady())
                            return clip.Asset;
                    }
                    return {};
                },
                [this](const std::string& binding) -> std::optional<AudioSpatialParams> {
                    UUID id = 0;
                    if (auto spawned = m_Spawned.find(binding); spawned != m_Spawned.end())
                        id = spawned->second;
                    else if (const auto* c = Component())
                        if (auto bound = c->Bindings.find(binding); bound != c->Bindings.end())
                            id = bound->second;
                    const auto entity = m_Scene.GetEntityByUUID(id);
                    if (!entity || !m_Scene.IsEntityActive(entity))
                        return std::nullopt;
                    auto spatial          = m_Scene.GetAudioSpatialParams(entity);
                    spatial.DopplerFactor = 0;
                    return spatial;
                },
                m_Owner);
            seek = true;
        }
        const auto master = component->ClockSource == TimelineClockSource::Audio ?
                                std::optional<size_t>(component->AudioClockTrack) :
                                std::nullopt;
        if (seek)
            m_Audio->Play(m_Time, component->Speed, component->AudioOffset, master);
        else
            m_Audio->Update(m_Time, component->Speed, component->AudioOffset, master);
    }
    void TimelinePlayer::Stop()
    {
        m_Audio.reset();
        m_Playing = false;
        auto c    = Component();
        if (c && c->WrapMode == TimelineWrapMode::Reset)
            RestoreBoundState();
        else
        {
            RestoreCameras();
            RemoveSpawned();
            if (m_State)
            {
                m_State->ReleaseParticles(m_Scene);
                for (const auto& [id, base] : m_State->Bases)
                    if (auto e = m_Scene.GetEntityByUUID(id); e && e.HasComponent<AnimatorComponent>())
                    {
                        auto& a = e.GetComponent<AnimatorComponent>();
                        if (a.Player)
                            for (const auto& s : a.Player->GetActions())
                                if (!s.Slot.empty())
                                    a.Player->ClearTimelinePose(s.Slot);
                    }
            }
            m_State.reset();
        }
        m_Time      = 0;
        m_Completed = false;
        m_Entered   = false;
        m_Fired.clear();
    }
    void TimelinePlayer::CrossEvents(double from, double to, bool includeZero)
    {
        if (to < from)
            return;
        const auto& events = m_Source->GetData().Events;
        for (size_t i = 0; i < events.size(); ++i)
        {
            const auto e = events[i];
            if ((e.Time > from || (includeZero && e.Time == 0)) && e.Time <= to && m_Fired.insert(i).second)
            {
                auto sinks = m_Events;
                for (const auto& [id, sink] : sinks)
                    if (m_Events.count(id) && sink)
                        sink(e);
                if (ScriptEngine::GetSceneContext() == &m_Scene)
                {
                    UUID target = 0;
                    if (auto c = Component())
                    {
                        auto found = c->Bindings.find(e.Binding);
                        if (found != c->Bindings.end())
                            target = found->second;
                    }
                    if (auto found = m_Spawned.find(e.Binding); found != m_Spawned.end())
                        target = found->second;
                    ScriptTimeline::Queue(m_Owner, target, e, false);
                }
                if (!m_Playing)
                    break;
            }
        }
    }
    bool TimelinePlayer::Seek(double time)
    {
        if (!std::isfinite(time) || !m_Source)
            return false;
        ResetBoundSecondaryMotion(m_Scene, Component(), &m_Source->GetData(), m_Spawned);
        double to = std::clamp(time, 0.0, m_Source->GetData().Length), from = m_Time;
        if (!Evaluate(to))
            return false;
        m_Time = to;
        if (const auto* c = Component(); c && c->ClockSource == TimelineClockSource::MusicSet)
            if (auto* music = m_Scene.GetMusicSetPlayer(m_Scene.GetEntityByUUID(m_Owner)))
                music->SeekSeconds(std::max(0.0, to - c->AudioOffset));
        UpdateAudio(true);
        // Per pass dedup survives backward seeks: rewinding alone does not rearm a marker.
        if (m_Playing && to > from)
            CrossEvents(from, to);
        if (m_Playing && to == m_Source->GetData().Length && !(Component() && Component()->Loop))
            Finish();
        return true;
    }
    void TimelinePlayer::Finish()
    {
        if (m_Completed)
            return;
        m_Audio.reset();
        m_Playing   = false;
        m_Completed = true;
        if (Component() && Component()->WrapMode == TimelineWrapMode::Reset)
        {
            double end = m_Time;
            RestoreBoundState();
            m_Time      = end;
            m_Completed = true;
        }
        else
        {
            RestoreCameras();
            RemoveSpawned();
            if (m_State)
                m_State->ReleaseParticles(m_Scene);
        }
        if (ScriptEngine::GetSceneContext() == &m_Scene)
            ScriptTimeline::Queue(m_Owner, 0, {}, true);
        auto sinks = m_Finished;
        for (const auto& [id, sink] : sinks)
            if (m_Finished.count(id) && sink)
                sink();
    }
    void TimelinePlayer::Advance(double seconds)
    {
        auto c = Component();
        if (!m_Playing || !c || !std::isfinite(seconds) || seconds < 0 || !std::isfinite(c->Speed) || c->Speed < 0)
            return;
        double remaining = seconds * c->Speed, length = m_Source->GetData().Length;
        if (!std::isfinite(remaining))
            return;
        if (length == 0)
        {
            Finish();
            return;
        }
        const auto evaluate = [&](double time) {
            if (m_State)
                m_State->LiveAdvance = true;
            const bool evaluated = Evaluate(time);
            if (m_State)
                m_State->LiveAdvance = false;
            if (!evaluated)
            {
                m_Playing = false;
                m_Audio.reset();
            }
            return evaluated;
        };
        std::optional<double> clock;
        if (c->Speed > 0)
        {
            if (m_Audio && !m_Scene.m_AudioPaused && c->ClockSource == TimelineClockSource::Audio)
                clock = m_Audio->GetClockTime(c->AudioClockTrack);
            else if (c->ClockSource == TimelineClockSource::MusicSet)
                if (const auto* music = m_Scene.GetMusicSetPlayer(m_Scene.GetEntityByUUID(m_Owner)))
                    if (auto position = music->GetPosition())
                        clock = position->Seconds + c->AudioOffset;
        }
        if (clock)
        {
            const auto target = std::clamp(*clock, 0.0, length);
            if (target < m_Time)
            {
                m_Time = target;
                if (evaluate(m_Time))
                    UpdateAudio();
                return;
            }
            remaining = target - m_Time;
        }
        bool wrapped = false;
        while (m_Playing && remaining > 0)
        {
            double step = std::min(remaining, length - m_Time), from = m_Time;
            m_Time += step;
            remaining -= step;
            // Scene advances the live Animator before timelines. Preserve script parameters,
            // state transitions and action clocks during playback; explicit Evaluate/Seek samples
            // the captured baseline for deterministic scrubbing instead.
            if (!evaluate(m_Time))
                return;
            CrossEvents(from, m_Time);
            if (!m_Playing)
                return;
            if (m_Time == length)
            {
                if (!(Component() && Component()->Loop))
                {
                    Finish();
                    return;
                }
                m_Time = 0;
                m_Fired.clear();
                if (!evaluate(0))
                    return;
                wrapped = true;
                CrossEvents(-1, 0, true);
            }
        }
        if (m_Playing)
            UpdateAudio(wrapped);
    }
} // namespace Kans

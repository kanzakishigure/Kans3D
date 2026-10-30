#include "kspch.h"
#include <glm/gtx/quaternion.hpp>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Renderer/RenderWorld.h"
#include "Kans3D/Renderer/Resource/Skinning.h"
#include "Kans3D/Script/ScriptAnimation.h"
#include "Kans3D/Script/ScriptEngine.h"
#include "Entity.h"
#include "Scene.h"
namespace Kans
{
    namespace
    {
        const AnimationControllerData& RestController()
        {
            static const auto data = [] {
                AnimationControllerData c;
                AnimationMotion         m;
                m.InlineClip.emplace();
                c.Motions.push_back(m);
                AnimationLayer l;
                l.Name = "Rest";
                l.States.push_back({"Rest", 0, 1, false});
                c.Layers.push_back(l);
                return c;
            }();
            return data;
        }
    } // namespace
    bool Scene::PrepareAnimator(Entity root)
    {
        if (!root || !root.HasComponent<AnimatorComponent>())
            return false;
        auto& a = root.GetComponent<AnimatorComponent>();
        if (a.Player)
            return true;
        AssetID skeleton = a.Skeleton;
        if (!skeleton.IsValid() && root.HasComponent<PrefabInstanceComponent>())
        {
            const auto& prefab = root.GetComponent<PrefabInstanceComponent>();
            if (prefab.Source)
                skeleton = prefab.Source->GetMeshData().Skeleton;
        }
        if (!a.SkeletonSource && m_PrefabAssets && skeleton.IsValid())
        {
            auto loaded = m_PrefabAssets->Request<SkeletonAsset>(skeleton);
            if (!loaded.IsReady())
                return false;
            a.SkeletonSource = loaded.Asset;
        }
        if (!a.SkeletonSource)
            return false;
        if (!a.InlineController && !a.ControllerSource && a.Controller.IsValid() && m_PrefabAssets)
        {
            auto loaded = m_PrefabAssets->Request<AnimationControllerAsset>(a.Controller);
            if (!loaded.IsReady())
                return false;
            a.ControllerSource = loaded.Asset;
        }
        if (!a.InlineController && a.Controller.IsValid() && !a.ControllerSource)
            return false;
        // A prepared player borrows immutable asset data. Retain a copy of inline authoring data so
        // ECS component relocation and editor edits cannot invalidate its pointers.
        if (a.InlineController)
            a.ControllerSource = CreateRef<AnimationControllerAsset>(*a.InlineController);
        const auto& controller = a.ControllerSource ? a.ControllerSource->GetData() : RestController();
        for (const auto& motion : controller.Motions)
            if (motion.Clip.IsValid() && !motion.InlineClip && !a.ClipSources.count(motion.Clip))
            {
                if (!m_PrefabAssets)
                    return false;
                auto loaded = m_PrefabAssets->Request<AnimationClipAsset>(motion.Clip);
                if (!loaded.IsReady())
                    return false;
                a.ClipSources.emplace(motion.Clip, loaded.Asset);
            }
        if (!root.HasComponent<IKComponent>() &&
            (!controller.TwoBoneConstraints.empty() || !controller.LookAtConstraints.empty()))
        {
            auto& ik          = root.AddComponent<IKComponent>();
            ik.Skeleton       = skeleton;
            ik.SkeletonSource = a.SkeletonSource;
            ik.ImportLegacy(controller);
            if (!a.InlineController && Log::GetCoreLogger())
                CORE_WARN("Controller asset {} contains legacy IK; copied to IKComponent",
                          a.Controller.IsValid() ? a.Controller.ToString() : a.ControllerSource->assetID.ToString());
        }
        if (a.InlineController)
        {
            a.InlineController->TwoBoneConstraints.clear();
            a.InlineController->LookAtConstraints.clear();
        }
        auto player                  = CreateRef<Animator>();
        player->EvaluateControllerIK = false;
        if (root.HasComponent<ShapeKeysComponent>())
            player->BaseShapeKeyWeights = root.GetComponent<ShapeKeysComponent>().Weights;
        auto valid = player->Prepare(
            a.SkeletonSource->GetData(),
            controller,
            [this, rootID = root.GetUUID()](AssetID id) -> const AnimationClipData* {
                auto entity = GetEntityByUUID(rootID);
                if (!entity || !entity.HasComponent<AnimatorComponent>())
                    return nullptr;
                auto& sources = entity.GetComponent<AnimatorComponent>().ClipSources;
                if (!sources.count(id) && m_PrefabAssets)
                {
                    // An explicit play request must start on this fixed step, including a cold asset cache.
                    auto loaded = m_PrefabAssets->Load<AnimationClipAsset>(id);
                    if (auto asset = std::get_if<Ref<const AnimationClipAsset>>(&loaded))
                        sources.emplace(id, *asset);
                }
                auto found = sources.find(id);
                return found == sources.end() ? nullptr : &found->second->GetData();
            },
            [this, rootID = root.GetUUID()](AssetID id) -> const ActionData* {
                auto entity = GetEntityByUUID(rootID);
                if (!entity || !entity.HasComponent<AnimatorComponent>())
                    return nullptr;
                auto& sources = entity.GetComponent<AnimatorComponent>().ActionSources;
                if (!sources.count(id) && m_PrefabAssets)
                {
                    auto loaded = m_PrefabAssets->Load<ActionAsset>(id);
                    if (auto asset = std::get_if<Ref<const ActionAsset>>(&loaded))
                        sources.emplace(id, *asset);
                }
                auto found = sources.find(id);
                return found == sources.end() ? nullptr : &found->second->GetData();
            });
        if (auto* error = std::get_if<AssetError>(&valid))
        {
            a.LastError = *error;
            return false;
        }
        for (const auto& p : a.Parameters)
        {
            const auto found = std::find_if(controller.Parameters.begin(),
                                            controller.Parameters.end(),
                                            [&](const auto& entry) { return entry.Name == p.first; });
            if (found == controller.Parameters.end())
                continue;
            switch (found->Type)
            {
                case AnimationParameterType::Float:
                    player->SetFloat(p.first, float(p.second));
                    break;
                case AnimationParameterType::Int:
                    if (!std::isfinite(p.second) || p.second < INT_MIN || p.second > INT_MAX ||
                        std::floor(p.second) != p.second)
                    {
                        a.LastError =
                            AssetError {AssetErrorCode::InvalidData, "Animator integer parameter is out of range"};
                        return false;
                    }
                    player->SetInt(p.first, int(p.second));
                    break;
                case AnimationParameterType::Bool:
                    player->SetBool(p.first, p.second != 0);
                    break;
                case AnimationParameterType::Trigger:
                    if (p.second != 0)
                        player->SetTrigger(p.first);
                    break;
            }
        }
        player->SetIKEntityResolver([this](uint64_t id, glm::mat4& world) {
            auto entity = GetEntityByUUID(UUID(id));
            if (!entity)
                return false;
            world = GetWorldTransform(entity);
            return true;
        });
        player->SetIKWorldTransform(GetWorldTransform(root));
        player->ExtractRootMotion = a.ApplyRootMotion;
        player->Speed             = a.Speed;
        player->RebuildPose();
        a.SkinPose = CreateRef<SkinningPose>();
        a.SkinPose->Palette.resize(a.SkeletonSource->GetData().Joints.size());
        a.SkinPose->ShapeKeys = a.SkeletonSource->GetData().ShapeKeys;
        a.SkinPose->MorphWeights.resize(a.SkinPose->ShapeKeys.size());
        a.Player = std::move(player);
        a.Events.reserve(64);
        a.JointEntities.resize(a.SkeletonSource->GetData().Joints.size());
        a.PreviewPose.Resize(a.JointEntities.size());
        const auto& joints = a.SkeletonSource->GetData().Joints;
        for (size_t j = 0; j < joints.size(); ++j)
            a.JointEntities[j] = PrefabEntityUUID(root.GetUUID(), joints[j].NodeKey);
        a.LastError.reset();
        ApplyAnimatorPose(root, a.Player->GetPose());
        return true;
    }
    void Scene::ResetAnimator(Entity root)
    {
        if (root && root.HasComponent<AnimatorComponent>())
        {
            ResetSecondaryMotion(root);
            auto& a = root.GetComponent<AnimatorComponent>();
            a.Player.reset();
            a.ControllerSource.reset();
            a.ClipSources.clear();
            a.ActionSources.clear();
            if (a.Skeleton.IsValid() && a.SkeletonSource && a.SkeletonSource->assetID != a.Skeleton)
                a.SkeletonSource.reset();
            a.Events.clear();
            a.JointEntities.clear();
        }
    }
    void Scene::ApplyAnimatorPose(Entity root, const AnimationPose& input)
    {
        auto& a              = root.GetComponent<AnimatorComponent>();
        a.PreviewPose        = input;
        auto&       pose     = a.PreviewPose;
        const auto& skeleton = a.SkeletonSource->GetData();
        if (!a.InlineController && !a.ControllerSource && (!a.Player || !a.Player->HasTimelinePose()))
            for (size_t j = 0; j < skeleton.Joints.size(); ++j)
                pose.Local[j] = skeleton.Joints[j].BindLocal;
        if (root.HasComponent<IKComponent>())
            EvaluateIKConstraints(skeleton,
                                  root.GetComponent<IKComponent>(),
                                  pose,
                                  GetWorldTransform(root),
                                  [this](uint64_t id, glm::mat4& world) {
                                      auto target = GetEntityByUUID(UUID(id));
                                      if (!target)
                                          return false;
                                      world = GetWorldTransform(target);
                                      return true;
                                  });
        else
            BuildModelPose(skeleton, pose);
        ApplyModelPose(root, skeleton, pose, a.JointEntities, a.SkinPose, a.PoseVersion);
    }
    void Scene::ApplyModelPose(Entity                   root,
                               const SkeletonData&      skeleton,
                               const AnimationPose&     pose,
                               const std::vector<UUID>& joints,
                               Ref<SkinningPose>&       skinPose,
                               uint64_t&                version,
                               bool                     writeTransforms)
    {
        const bool catalogChanged =
            skinPose && (skinPose->ShapeKeys.size() != skeleton.ShapeKeys.size() ||
                         !std::equal(skinPose->ShapeKeys.begin(),
                                     skinPose->ShapeKeys.end(),
                                     skeleton.ShapeKeys.begin(),
                                     [](const auto& a, const auto& b) { return a.Name == b.Name; }));
        if (!skinPose || catalogChanged)
            skinPose = CreateRef<SkinningPose>();
        skinPose->ShapeKeys = skeleton.ShapeKeys;
        const bool old      = m_ReconcilingPrefab;
        m_ReconcilingPrefab = true;
        for (size_t j = 0; writeTransforms && j < joints.size() && j < pose.Local.size(); ++j)
        {
            auto bone = GetEntityByUUID(joints[j]);
            if (!bone)
                continue;
            const auto&        local = pose.Local[j];
            TransformComponent t;
            t.Position                              = local.Translation;
            t.Rotation                              = glm::eulerAngles(local.Rotation);
            t.Scale                                 = local.Scale;
            t.ImportedMatrix                        = local.Matrix();
            t.ImportedPosition                      = t.Position;
            t.ImportedRotation                      = t.Rotation;
            t.ImportedScale                         = t.Scale;
            bone.GetComponent<TransformComponent>() = t;
            if (root.HasComponent<PrefabInstanceComponent>())
            {
                auto& nodes = root.GetComponent<PrefabInstanceComponent>().Nodes;
                auto  found = nodes.find(skeleton.Joints[j].NodeKey);
                if (found != nodes.end())
                    found->second.AppliedTransform = t;
            }
        }
        m_ReconcilingPrefab = old;
        ++version;
        skinPose->Palette = pose.Palette;
        // Morph-only meshes use the same shader/palette layout with one identity joint.
        if (skinPose->Palette.empty())
            skinPose->Palette.emplace_back(1.f);
        skinPose->MorphWeights = pose.MorphWeights;
        skinPose->Version      = version;
        // Pose changes preserve render topology. Refresh transforms and conservative bounds in place,
        // avoiding dirty-set nodes and complete proxy reconstruction for every bone every frame.
        const auto rootWorld = GetWorldTransform(root);
        auto refresh = [&](auto&& self, Entity entity, const glm::mat4& relative, const glm::mat4& world) -> Bounds {
            Bounds     bounds;
            const auto record = m_RenderWorld->m_Records.find(entity);
            if (record != m_RenderWorld->m_Records.end())
            {
                auto&      r = record->second;
                const bool skin =
                    r.MeshSource && !r.MeshSource->GetData().Skin.empty() && GetDeformationRoot(entity) == root;
                const auto geometryWorld = skin ? rootWorld : world;
                const auto animated =
                    r.MeshSource ? DeformedBounds(r.MeshSource->GetData(), skinPose.get()) : Bounds {};
                if (r.Mesh)
                {
                    r.Mesh->Transform = geometryWorld;
                    r.OutlineTransform =
                        skin || entity.HasComponent<PrefabOwnedComponent>() ?
                            rootWorld * glm::scale(glm::mat4(1), glm::vec3(1.007f)) * (skin ? glm::mat4(1) : relative) :
                            world * glm::scale(glm::mat4(1), glm::vec3(1.007f));
                }
                for (auto& draw : r.Draws)
                {
                    draw.Transform = geometryWorld;
                    if (r.MeshSource && (skin || !r.MeshSource->GetData().MorphTargets.empty()))
                    {
                        draw.SkinPose    = skinPose;
                        draw.LocalBounds = animated.IsValid() ?
                                               animated :
                                               r.MeshSource->GetData().Sections.at(draw.Section).LocalBounds;
                    }
                    draw.WorldBounds = draw.LocalBounds.Transformed(geometryWorld);
                    bounds.Expand(draw.WorldBounds);
                }
                if (r.Point)
                    r.Point->Position = glm::vec3(world[3]);
                if (r.Sprite)
                    r.Sprite->Transform = world;
                m_RenderWorld->m_PendingStats.Updated += r.Draws.size();
            }
            for (auto id : entity.GetComponent<RelationshipComponent>().Children)
                if (auto child = GetEntityByUUID(id))
                {
                    auto local = child.GetComponent<TransformComponent>().GetTransform();
                    if (!writeTransforms)
                    {
                        const auto found = std::find(joints.begin(), joints.end(), child.GetUUID());
                        if (found != joints.end())
                            local = pose.Local[size_t(found - joints.begin())].Matrix();
                    }
                    const auto childRelative = relative * local;
                    // Owned bones use root * model grouping; ordinary attachments use parentWorld * local,
                    // exactly as GetWorldTransform does (also preserving byte-exact cached validation).
                    const bool owned       = child.HasComponent<PrefabOwnedComponent>() &&
                                             child.GetComponent<PrefabOwnedComponent>().Instance == root.GetUUID();
                    const auto childWorld  = owned ? rootWorld * childRelative : world * local;
                    const auto childBounds = self(self, child, childRelative, childWorld);
                    m_ChildBounds[entt::entity(entity)].Set(child, childBounds);
                    bounds.Expand(childBounds);
                }
            entity.GetComponent<BoundsComponent>().World = bounds;
            ++m_RenderWorld->m_PendingStats.BoundsUpdated;
            return bounds;
        };
        auto bounds = refresh(refresh, root, glm::mat4(1), rootWorld);
        for (auto child = root, parent = GetParent(child); parent; child = parent, parent = GetParent(parent))
        {
            m_ChildBounds[entt::entity(parent)].Set(child, bounds);
            bounds = m_ChildBounds[entt::entity(parent)].Aggregate();
            for (const auto& draw : m_RenderWorld->GetEntityDraws(parent))
                bounds.Expand(draw.WorldBounds);
            parent.GetComponent<BoundsComponent>().World = bounds;
        }
    }
    void Scene::UpdateAnimations(double seconds)
    {
        for (auto handle : m_Registry.view<AnimatorComponent>())
        {
            Entity root {handle, this};
            auto&  a = root.GetComponent<AnimatorComponent>();
            a.Events.clear();
            if (!IsEntityActive(root) || !a.Enabled || !PrepareAnimator(root))
                continue;
            if (root.HasComponent<ShapeKeysComponent>())
                a.Player->BaseShapeKeyWeights = root.GetComponent<ShapeKeysComponent>().Weights;
            else
                a.Player->BaseShapeKeyWeights.clear();
            a.Player->SetIKWorldTransform(GetWorldTransform(root));
            a.Player->ExtractRootMotion = a.ApplyRootMotion;
            a.Player->Speed             = a.Speed;
            a.Player->Advance(
                seconds,
                [&](const auto& event) { a.Events.push_back(event); },
                [this, &root](const auto& delta) {
                    auto& animator = root.GetComponent<AnimatorComponent>();
                    if (!animator.ApplyRootMotion)
                        return;
                    const auto matrix          = root.GetComponent<TransformComponent>().GetTransform() *
                                                 glm::translate(glm::mat4(1), delta.Translation) *
                                                 glm::mat4_cast(delta.Rotation);
                    const auto local           = JointTransform::FromMatrix(matrix);
                    auto&      transform       = root.GetComponent<TransformComponent>();
                    transform.Position         = local.Translation;
                    transform.Rotation         = glm::eulerAngles(local.Rotation);
                    transform.Scale            = local.Scale;
                    transform.ImportedMatrix   = matrix;
                    transform.ImportedPosition = transform.Position;
                    transform.ImportedRotation = transform.Rotation;
                    transform.ImportedScale    = transform.Scale;
                    animator.Player->SetIKWorldTransform(GetWorldTransform(root));
                });
            // Keep the Advance event sink within std::function's inline storage (one capture).
            if (ScriptEngine::GetSceneContext() == this)
                for (const auto& event : a.Events)
                    ScriptAnimation::Queue(root.GetUUID(), event);
            if (a.LastSpringDiscontinuity != a.Player->DiscontinuityVersion)
            {
                a.LastSpringDiscontinuity = a.Player->DiscontinuityVersion;
                ResetSecondaryMotion(root);
            }
            ApplyAnimatorPose(root, a.Player->GetPose());
        }
        UpdateModelPoses();
        UpdateBounds();
    }
    bool Scene::PreviewAnimation(Entity root, size_t layer, uint32_t state, double time)
    {
        RestoreSpringBoneTransforms();
        ResetSecondaryMotion(root);
        if (!PrepareAnimator(root))
            return false;
        auto& a = root.GetComponent<AnimatorComponent>();
        if (root.HasComponent<ShapeKeysComponent>())
            a.Player->BaseShapeKeyWeights = root.GetComponent<ShapeKeysComponent>().Weights;
        if (!a.Player->EvaluateState(layer, state, time, a.PreviewPose))
            return false;
        ApplyAnimatorPose(root, a.PreviewPose);
        UpdateBounds();
        return true;
    }
    Entity Scene::GetDeformationRoot(Entity entity) const
    {
        for (auto e = entity; e; e = GetParent(e))
            if (e.HasComponent<AnimatorComponent>() || e.HasComponent<IKComponent>() ||
                e.HasComponent<ShapeKeysComponent>() || e.HasComponent<ModelPoseComponent>())
                return e;
        return {};
    }
    Ref<const SkinningPose> Scene::GetSkinningPose(Entity entity) const
    {
        auto root = GetDeformationRoot(entity);
        if (!root)
            return {};
        if (root.HasComponent<AnimatorComponent>())
            return root.GetComponent<AnimatorComponent>().SkinPose;
        return root.HasComponent<ModelPoseComponent>() ? root.GetComponent<ModelPoseComponent>().SkinPose :
                                                         Ref<SkinningPose>();
    }
    void Scene::UpdateModelPoses()
    {
        // Expression references are useful with or without playback.
        for (auto handle : m_Registry.view<ShapeKeysComponent>())
        {
            auto& c = m_Registry.get<ShapeKeysComponent>(handle);
            if (c.ExpressionSource && c.ExpressionSource->assetID != c.Expression)
                c.ExpressionSource.reset();
            if (m_PrefabAssets && c.Expression.IsValid() && !c.ExpressionSource)
            {
                auto loaded = m_PrefabAssets->Request<ExpressionAsset>(c.Expression);
                if (loaded.IsReady())
                    c.ExpressionSource = loaded.Asset;
            }
        }
        auto evaluate = [&](Entity root) {
            if (root.HasComponent<AnimatorComponent>())
                return;
            AssetID                  skeletonID;
            Ref<const SkeletonAsset> skeleton;
            if (root.HasComponent<IKComponent>())
            {
                const auto& c = root.GetComponent<IKComponent>();
                skeletonID    = c.Skeleton;
                skeleton      = c.SkeletonSource;
            }
            if (root.HasComponent<ShapeKeysComponent>())
            {
                const auto& c = root.GetComponent<ShapeKeysComponent>();
                if (!skeletonID.IsValid())
                    skeletonID = c.Skeleton;
                if (!skeleton)
                    skeleton = c.SkeletonSource;
            }
            if (!skeletonID.IsValid() && root.HasComponent<PrefabInstanceComponent>())
            {
                const auto& c = root.GetComponent<PrefabInstanceComponent>();
                if (c.Source)
                    skeletonID = c.Source->GetMeshData().Skeleton;
            }
            if (!skeletonID.IsValid())
                if (auto mesh = ResolveMesh(root))
                    skeletonID = mesh->GetData().Skeleton;
            if (!skeleton && skeletonID.IsValid())
            {
                if (!m_PrefabAssets)
                    return;
                auto loaded = m_PrefabAssets->Request<SkeletonAsset>(skeletonID);
                if (!loaded.IsReady())
                    return;
                skeleton = loaded.Asset;
            }
            if (!skeleton)
            {
                SkeletonData data;
                for (auto entity : GetMeshEntities(root))
                    if (auto mesh = ResolveMesh(entity))
                        for (const auto& key : mesh->GetData().MorphTargets)
                            if (data.FindShapeKey(key.Name) < 0)
                                data.ShapeKeys.push_back({key.Name, key.DefaultWeight});
                if (data.ShapeKeys.empty())
                    return;
                skeleton = CreateRef<SkeletonAsset>(std::move(data));
            }
            auto& state          = m_Registry.get_or_emplace<ModelPoseComponent>(entt::entity(root));
            state.SkeletonSource = skeleton;
            const auto& data     = skeleton->GetData();
            RestPose(data, state.Pose);
            state.JointEntities.resize(data.Joints.size());
            for (size_t j = 0; j < data.Joints.size(); ++j)
            {
                state.Pose.Local[j]    = data.Joints[j].BindLocal;
                state.JointEntities[j] = PrefabEntityUUID(root.GetUUID(), data.Joints[j].NodeKey);
            }
            if (root.HasComponent<ShapeKeysComponent>())
                for (const auto& [name, value] : root.GetComponent<ShapeKeysComponent>().Weights)
                    if (const auto k = data.FindShapeKey(name); k >= 0)
                        state.Pose.MorphWeights[k] = std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 0;
            if (root.HasComponent<IKComponent>())
                EvaluateIKConstraints(data,
                                      root.GetComponent<IKComponent>(),
                                      state.Pose,
                                      GetWorldTransform(root),
                                      [this](uint64_t id, glm::mat4& world) {
                                          auto target = GetEntityByUUID(UUID(id));
                                          if (!target)
                                              return false;
                                          world = GetWorldTransform(target);
                                          return true;
                                      });
            else
                BuildModelPose(data, state.Pose);
            ApplyModelPose(root, data, state.Pose, state.JointEntities, state.SkinPose, state.PoseVersion);
        };
        for (auto handle : m_Registry.view<IKComponent>())
            evaluate({handle, this});
        for (auto handle : m_Registry.view<ShapeKeysComponent>(entt::exclude<IKComponent>))
            evaluate({handle, this});
    }
} // namespace Kans

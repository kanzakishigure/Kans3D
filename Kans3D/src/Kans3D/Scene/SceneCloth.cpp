#include "kspch.h"
#include "Kans3D/Core/ParallelFor.h"
#include "Kans3D/Physics/ClothSolver.h"
#include "Kans3D/Physics/PhysicsSettings.h"
#include "Kans3D/Renderer/RenderWorld.h"
#include "Kans3D/Renderer/Resource/InternalShader.h"
#include "Entity.h"
#include "MotionUpdateCache.h"
#include "Scene.h"
namespace Kans
{
    struct ClothUpdateCache
    {
        struct Work
        {
            ClothRuntime*         Runtime;
            const ClothComponent* Settings;
            bool                  Reset;
        };
        std::vector<Work>    Items;
        Scope<ParallelFor>   Workers;
        unsigned             WorkerCount = UINT32_MAX;
        const ForceFieldSet* Fields      = nullptr;
        double               Seconds     = 0;
        bool                 Paused = false, SingleStep = false;
    };
    const ClothRuntime* Scene::GetClothState(Entity e) const
    {
        return e && e.HasComponent<ClothComponent>() ? e.GetComponent<ClothComponent>().Runtime.get() : nullptr;
    }
    bool Scene::BindCloth(Entity e, Ref<const ClothAsset> asset, Ref<const MeshAsset> mesh)
    {
        if (!e || !asset || !e.HasComponent<ClothComponent>())
            return false;
        if (!mesh)
            mesh = ResolveMesh(e);
        if (!mesh)
            return false;
        auto& c = e.GetComponent<ClothComponent>();
        if (!c.Runtime)
            c.Runtime = CreateRef<ClothRuntime>();
        auto&      r              = *c.Runtime;
        const bool sectionMatches = !e.HasComponent<MeshComponent>() || !e.GetComponent<MeshComponent>().Section ||
                                    *e.GetComponent<MeshComponent>().Section == c.Section;
        if (!sectionMatches ||
            !ValidateClothBinding(asset->GetData(),
                                  e.HasComponent<MeshComponent>() ? e.GetComponent<MeshComponent>().Mesh :
                                                                    mesh->assetID,
                                  mesh->GetData(),
                                  c.Section))
        {
            r.BindingValid = false;
            r.Active       = false;
            if (!r.Warned)
            {
                CORE_ERROR("Cloth {}: source mesh, section, vertex count or topology changed; rebuild the .cloth asset",
                           uint64_t(e.GetUUID()));
                r.Warned = true;
                ++r.BindingErrors;
            }
            return false;
        }
        c.Asset = asset->assetID;
        r.Initialize(std::move(asset), std::move(mesh));
        r.BindingValid = true;
        r.Warned       = false;
        return true;
    }
    void Scene::ResetClothMotion(Entity scope)
    {
        for (auto h : m_Registry.view<ClothComponent>())
        {
            Entity e {h, this};
            auto   root = GetDeformationRoot(e);
            if (e == scope || IsDescendantOf(e, scope) || root == scope || (root && IsDescendantOf(root, scope)))
                e.GetComponent<ClothComponent>().Reset();
        }
    }
    void Scene::OnClothRemoved(entt::registry&, entt::entity h)
    {
        if (!m_SuppressNotifications)
            DirtySubtree({h, this});
    }
    void Scene::ClearClothStreams()
    {
        for (auto h : m_Registry.view<ClothComponent>())
            if (auto r = m_Registry.get<ClothComponent>(h).Runtime)
            {
                r->Active       = false;
                r->ResetPending = true;
            }
        PublishClothStreams();
    }
    void Scene::PublishClothStreams()
    {
        for (auto h : m_Registry.view<ClothComponent>())
        {
            auto found = m_RenderWorld->m_Records.find(h);
            if (found == m_RenderWorld->m_Records.end())
                continue;
            Entity      e {h, this};
            auto&       record = found->second;
            const auto& c      = e.GetComponent<ClothComponent>();
            const auto  r      = c.Runtime;
            const bool  active = c.Enabled && r && r->Active && r->BindingValid && r->Stream;
            for (auto& draw : record.Draws)
                if (draw.Section == c.Section || draw.Mesh.Stream)
                {
                    if (active && draw.Section == c.Section)
                    {
                        draw.Mesh = MeshKey(Ref<const DynamicVertexStream>(r->Stream));
                        draw.SkinPose.reset();
                        draw.Transform   = r->Root;
                        draw.LocalBounds = r->Stream->Data.LocalBounds;
                    }
                    else
                    {
                        draw.Mesh     = record.Key;
                        draw.SkinPose = GetSkinningPose(e);
                        if (record.MeshSource)
                        {
                            const auto& data = record.MeshSource->GetData();
                            const auto  root = GetDeformationRoot(e);
                            draw.Transform   = !data.Skin.empty() && root ? GetSpringBoneRenderTransform(root) :
                                                                            GetSpringBoneRenderTransform(e);
                            draw.LocalBounds = (!data.Skin.empty() || !data.MorphTargets.empty()) ?
                                                   DeformedBounds(data, draw.SkinPose.get()) :
                                                   data.Sections[draw.Section].LocalBounds;
                        }
                    }
                    draw.WorldBounds = draw.LocalBounds.Transformed(draw.Transform);
                }
            Bounds bounds = m_ChildBounds[h].Aggregate();
            for (const auto& draw : record.Draws)
                bounds.Expand(draw.WorldBounds);
            e.GetComponent<BoundsComponent>().World = bounds;
            for (auto child = e, parent = GetParent(e); parent; child = parent, parent = GetParent(parent))
            {
                m_ChildBounds[entt::entity(parent)].Set(child, bounds);
                bounds = m_ChildBounds[entt::entity(parent)].Aggregate();
                for (const auto& draw : m_RenderWorld->GetEntityDraws(parent))
                    bounds.Expand(draw.WorldBounds);
                parent.GetComponent<BoundsComponent>().World = bounds;
            }
        }
    }
    void Scene::UpdateCloth(double seconds)
    {
        if (m_Registry.view<ClothComponent>().empty())
        {
            m_MotionSnapshotReady = false;
            return;
        }
        const bool playing = bool(m_PhysicsWorld), preview = !playing && m_SpringBonePreview;
        if (!playing && !preview)
        {
            ClearClothStreams();
            m_MotionSnapshotReady = false;
            return;
        }
        if (!m_MotionSnapshotReady)
            PrepareMotionSnapshot();
        m_MotionSnapshotReady = false;
        auto& snapshot        = *m_SpringBoneUpdate;
        if (!m_ClothUpdate)
            m_ClothUpdate = CreateRef<ClothUpdateCache>();
        auto&          batch = *m_ClothUpdate;
        const unsigned workers =
            std::min(GetPhysicsSettings().WorkerThreads, std::max(1u, std::thread::hardware_concurrency()));
        if (batch.WorkerCount != workers)
        {
            batch.Workers     = CreateScope<ParallelFor>(workers);
            batch.WorkerCount = workers;
        }
        batch.Items.clear();
        batch.Fields                  = &snapshot.Fields;
        batch.Seconds                 = seconds;
        batch.Paused                  = playing && m_PhysicsPaused && !m_PhysicsSingleStep;
        batch.SingleStep              = m_PhysicsSingleStep;
        auto                   camera = GetCameraEntity();
        glm::vec3              cameraPosition(0);
        std::optional<Frustum> frustum;
        if (camera)
        {
            auto world     = GetWorldTransform(camera);
            cameraPosition = glm::vec3(world[3]);
            const auto projection =
                DeviceProjection(camera.GetComponent<CameraComponent>().SceneCamera.GetProjectionMatrix(),
                                 {RenderHardware::ClipYDirection::Down,
                                  RenderHardware::DepthRange::ZeroToOne,
                                  RenderHardware::FramebufferOrigin::UpperLeft});
            frustum.emplace(projection * glm::inverse(world));
        }
        for (auto h : m_Registry.view<ClothComponent>())
        {
            Entity e {h, this};
            auto&  c = e.GetComponent<ClothComponent>();
            if (!c.Runtime)
                c.Runtime = CreateRef<ClothRuntime>();
            auto&                 r     = *c.Runtime;
            Ref<const ClothAsset> asset = r.Source;
            if (asset && asset->assetID != c.Asset)
            {
                asset.reset();
                r.Active       = false;
                r.BindingValid = false;
            }
            if (m_PrefabAssets && c.Asset.IsValid())
            {
                auto status = m_PrefabAssets->Request<ClothAsset>(c.Asset);
                if (status.IsReady())
                    asset = status.Asset;
            }
            if (!asset)
            {
                r.Active = false;
                continue;
            }
            auto mesh = ResolveMesh(e);
            if (!mesh && r.Mesh)
                mesh = r.Mesh;
            const bool renderMatches =
                !e.HasComponent<MeshComponent>() ||
                (e.GetComponent<MeshComponent>().Mesh == asset->GetData().Mesh &&
                 (!e.GetComponent<MeshComponent>().Section || *e.GetComponent<MeshComponent>().Section == c.Section));
            if (asset && mesh &&
                (!renderMatches || r.Source != asset || r.Mesh != mesh || !r.BindingValid ||
                 r.Source->GetData().Section != c.Section))
            {
                if (!BindCloth(e, asset, mesh))
                    continue;
            }
            if (!r.BindingValid || !r.Source || !r.Mesh)
            {
                r.Active = false;
                continue;
            }
            auto       root      = GetDeformationRoot(e);
            const auto rootWorld = GetSpringBoneRenderTransform(!r.Mesh->GetData().Skin.empty() && root ? root : e);
            auto       bounds    = DeformedBounds(r.Mesh->GetData(), GetSkinningPose(e).get()).Transformed(rootWorld);
            if (r.Active && r.Stream)
                bounds.Expand(r.Stream->Data.LocalBounds.Transformed(rootWorld));
            const bool lod =
                camera &&
                ((c.DisableDistance > 0 && glm::distance(cameraPosition, bounds.Center()) > c.DisableDistance) ||
                 (!c.SimulateWhenNotVisible && frustum && !frustum->Intersects(bounds)));
            if (!c.Enabled || lod || !snapshot.Active(snapshot.Get(e.GetUUID(), e)))
            {
                r.Active = false;
                continue;
            }
            auto&      center       = snapshot.Get(c.Center);
            const auto centerWorld  = center.Value ? snapshot.World(center) : glm::mat4(1);
            const auto previousRoot = r.Root, previousCenter = r.Center;
            r.SetFrame(GetSkinningPose(e).get(), rootWorld, centerWorld);
            r.Gravity  = GetPhysicsSettings().Gravity;
            bool reset = !r.Active || r.ResetEpoch != m_SecondaryMotionResetEpoch;
            if (r.Active)
            {
                const auto  oldRelative = glm::inverse(previousCenter) * previousRoot,
                            newRelative = glm::inverse(r.Center) * rootWorld;
                const float distance    = glm::distance(glm::vec3(oldRelative[3]), glm::vec3(newRelative[3]));
                const auto  a           = JointTransform::FromMatrix(oldRelative).Rotation,
                            b           = JointTransform::FromMatrix(newRelative).Rotation;
                const float angle       = glm::degrees(2 * std::acos(std::clamp(std::abs(glm::dot(a, b)), 0.f, 1.f)));
                reset                   = reset || (c.TeleportDistance > 0 && distance > c.TeleportDistance) ||
                                          (c.TeleportRotation > 0 && angle > c.TeleportRotation);
            }
            snapshot.ColliderUnion.clear();
            for (auto id : c.ColliderGroups)
            {
                const auto& members = snapshot.Gather(id);
                snapshot.ColliderUnion.insert(snapshot.ColliderUnion.end(), members.begin(), members.end());
            }
            std::sort(snapshot.ColliderUnion.begin(), snapshot.ColliderUnion.end());
            snapshot.ColliderUnion.erase(std::unique(snapshot.ColliderUnion.begin(), snapshot.ColliderUnion.end()),
                                         snapshot.ColliderUnion.end());
            size_t count = 0;
            for (auto index : snapshot.ColliderUnion)
            {
                auto& shared = snapshot.Colliders[index];
                if (!shared.WorldReady)
                {
                    shared.World      = snapshot.World(*shared.Transform);
                    shared.WorldReady = true;
                }
                if (count == r.Colliders.size())
                    r.Colliders.emplace_back();
                auto& col    = r.Colliders[count++];
                col.Previous = col.ID == shared.ID ? col.Current : shared.World;
                col.ID       = shared.ID;
                col.Current  = shared.World;
                col.Shape    = shared.Data.Shape;
                col.Inside   = shared.Data.Inside;
            }
            r.Colliders.resize(count);
            r.Active     = true;
            r.ResetEpoch = m_SecondaryMotionResetEpoch;
            batch.Items.push_back({&r, &c, reset});
        }
        batch.Workers->Run(batch.Items.size(), &batch, [](void* context, size_t i) {
            auto& b = *static_cast<ClothUpdateCache*>(context);
            auto& w = b.Items[i];
            w.Runtime->Advance(*w.Settings, *b.Fields, b.Seconds, w.Reset, b.Paused, b.SingleStep);
        });
        PublishClothStreams();
    }
} // namespace Kans

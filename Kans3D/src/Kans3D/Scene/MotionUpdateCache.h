#pragma once
#include "Kans3D/Physics/ForceFields.h"
#include "Entity.h"
#include "Scene.h"
namespace Kans
{
    // Storage persists to reuse capacity; frame stamps invalidate all entity, hierarchy and shape snapshots.
    // Neither hash iteration nor group traversal order reaches the solver: collider unions use registry order.
    struct SpringBoneUpdateCache
    {
        enum Ready : uint16_t
        {
            LocalReady          = 1,
            WorldReady          = 2,
            RelativeReady       = 4,
            ActiveReady         = 8,
            RenderLocalReady    = 16,
            RenderWorldReady    = 32,
            RenderRelativeReady = 64
        };
        struct Entry
        {
            UUID      ID = 0, Parent = 0, PrefabRoot = 0, RelativeRoot = 0, RenderRelativeRoot = 0;
            Entity    Value;
            uint64_t  Frame         = 0;
            uint16_t  Flags         = 0;
            uint32_t  ColliderIndex = UINT32_MAX;
            bool      Active = false, Simulated = false, LocalValid = false, Imported = false, Owned = false;
            glm::vec3 Position {0}, Rotation {0}, Scale {1};
            glm::mat4 Local {1}, World {1}, Relative {1}, RenderLocal {1}, RenderWorld {1}, RenderRelative {1};
        };
        struct Group
        {
            uint64_t              Frame = 0;
            std::vector<uint32_t> Members;
        };
        struct FrameCollider
        {
            UUID                    ID = 0;
            MotionColliderComponent Data;
            Entry*                  Transform = nullptr;
            glm::mat4               World {1};
            bool                    WorldReady = false;
        };
        explicit SpringBoneUpdateCache(Scene& scene) : SceneOwner(scene) {}
        Scene&                          SceneOwner;
        uint64_t                        Frame = 0;
        std::unordered_map<UUID, Entry> Entities;
        std::unordered_map<UUID, Group> Groups;
        std::vector<FrameCollider>      Colliders;
        std::vector<uint32_t>           ColliderUnion;
        ForceFieldSet                   Fields;
        void                            Begin()
        {
            ++Frame;
            Colliders.clear();
            ColliderUnion.clear();
            Fields.Clear();
            // Retire unused IDs after topology edits without allocating in a stable scene.
            if (Frame % 256 == 0)
            {
                for (auto i = Entities.begin(); i != Entities.end();)
                    if (i->second.Frame + 256 < Frame)
                        i = Entities.erase(i);
                    else
                        ++i;
                for (auto i = Groups.begin(); i != Groups.end();)
                    if (i->second.Frame + 256 < Frame)
                        i = Groups.erase(i);
                    else
                        ++i;
            }
        }
        Entry& Get(UUID id, Entity known = {})
        {
            auto& e = Entities.try_emplace(id).first->second;
            if (e.Frame != Frame)
            {
                e.ID            = id;
                e.Frame         = Frame;
                e.Flags         = 0;
                e.ColliderIndex = UINT32_MAX;
                e.Simulated     = false;
                e.Value         = known ? known : SceneOwner.GetEntityByUUID(id);
                e.Parent        = e.Value ? e.Value.GetComponent<RelationshipComponent>().Parent : UUID(0);
                e.Owned         = e.Value && e.Value.HasComponent<PrefabOwnedComponent>();
                e.PrefabRoot    = e.Owned ? e.Value.GetComponent<PrefabOwnedComponent>().Instance : UUID(0);
            }
            return e;
        }
        const glm::mat4& Local(Entry& e)
        {
            if (!(e.Flags & LocalReady))
            {
                const auto& t        = e.Value.GetComponent<TransformComponent>();
                const bool  imported = t.ImportedMatrix && t.Position == t.ImportedPosition &&
                                       t.Rotation == t.ImportedRotation && t.Scale == t.ImportedScale;
                if (imported)
                    e.Local = *t.ImportedMatrix;
                else if (!e.LocalValid || e.Imported || e.Position != t.Position || e.Rotation != t.Rotation ||
                         e.Scale != t.Scale)
                    e.Local = t.GetTransform();
                e.Position   = t.Position;
                e.Rotation   = t.Rotation;
                e.Scale      = t.Scale;
                e.Imported   = imported;
                e.LocalValid = true;
                e.Flags |= LocalReady;
            }
            return e.Local;
        }
        const glm::mat4& Relative(Entry& e, UUID root)
        {
            if (!(e.Flags & RelativeReady) || e.RelativeRoot != root)
            {
                auto& parent   = Get(e.Parent);
                e.Relative     = parent.Value && parent.ID != root ? Relative(parent, root) * Local(e) : Local(e);
                e.RelativeRoot = root;
                e.Flags |= RelativeReady;
            }
            return e.Relative;
        }
        const glm::mat4& World(Entry& e)
        {
            if (!(e.Flags & WorldReady))
            {
                if (e.Owned)
                {
                    auto& root = Get(e.PrefabRoot);
                    e.World    = root.Value ? World(root) * Relative(e, e.PrefabRoot) : Local(e);
                }
                else
                {
                    auto& parent = Get(e.Parent);
                    e.World      = parent.Value ? World(parent) * Local(e) : Local(e);
                }
                e.Flags |= WorldReady;
            }
            return e.World;
        }
        bool Active(Entry& e)
        {
            if (!(e.Flags & ActiveReady))
            {
                e.Active = bool(e.Value) &&
                           (!e.Value.HasComponent<ActiveComponent>() || e.Value.GetComponent<ActiveComponent>().Active);
                if (e.Active && e.Parent != UUID(0))
                {
                    auto& parent = Get(e.Parent);
                    if (parent.Value)
                        e.Active = Active(parent);
                }
                e.Flags |= ActiveReady;
            }
            return e.Active;
        }
        const std::vector<uint32_t>& Gather(UUID id)
        {
            auto& group = Groups.try_emplace(id).first->second;
            if (group.Frame != Frame)
            {
                group.Frame = Frame;
                group.Members.clear();
                auto visit = [&](auto&& self, Entry& e) -> void {
                    if (!Active(e))
                        return;
                    if (e.ColliderIndex != UINT32_MAX)
                        group.Members.push_back(e.ColliderIndex);
                    for (auto child : e.Value.GetComponent<RelationshipComponent>().Children)
                        self(self, Get(child));
                };
                visit(visit, Get(id));
                std::sort(group.Members.begin(), group.Members.end());
            }
            return group.Members;
        }
        template<class F>
        const glm::mat4& RenderRelative(Entry& e, UUID root, F& local)
        {
            if (!(e.Flags & RenderRelativeReady) || e.RenderRelativeRoot != root)
            {
                auto& parent = Get(e.Parent);
                e.RenderRelative =
                    parent.Value && parent.ID != root ? RenderRelative(parent, root, local) * local(e) : local(e);
                e.RenderRelativeRoot = root;
                e.Flags |= RenderRelativeReady;
            }
            return e.RenderRelative;
        }
        template<class F>
        const glm::mat4& RenderWorld(Entry& e, F& local)
        {
            if (!(e.Flags & RenderWorldReady))
            {
                if (e.Owned)
                {
                    auto& root = Get(e.PrefabRoot);
                    e.RenderWorld =
                        root.Value ? RenderWorld(root, local) * RenderRelative(e, e.PrefabRoot, local) : local(e);
                }
                else
                {
                    auto& parent  = Get(e.Parent);
                    e.RenderWorld = parent.Value ? RenderWorld(parent, local) * local(e) : local(e);
                }
                e.Flags |= RenderWorldReady;
            }
            return e.RenderWorld;
        }
    };
} // namespace Kans

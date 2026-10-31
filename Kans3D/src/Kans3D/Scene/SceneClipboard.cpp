#include "kspch.h"
#include "SceneClipboard.h"
#include <functional>
#include <set>
#include <yaml-cpp/yaml.h>
#include "SceneSerializer.h"

namespace Kans
{
    // Keep creation, hierarchy edits and component notifications on Scene's normal
    // paths. Suppress prefab override capture only while installing a new forest.
    struct SceneClipboardAccess
    {
        static bool Owns(Scene& scene, Entity entity)
        {
            return entity && entity.m_Scene == &scene && scene.m_Registry.valid(entity.m_EntityHandle);
        }
        template<typename T>
        static void CloneComponent(Entity source, Entity target)
        {
            if constexpr (!std::is_same_v<T, IDComponent> && !std::is_same_v<T, BoundsComponent> &&
                          !std::is_same_v<T, NativeScriptComponent>)
                if (source.HasComponent<T>())
                {
                    if (target.HasComponent<T>())
                        target.SetComponent<T>(source.GetComponent<T>());
                    else
                        target.AddComponent<T>(source.GetComponent<T>());
                    if constexpr (std::is_same_v<T, AnimatorComponent>)
                        target.GetComponent<T>().JointEntities = source.GetComponent<T>().JointEntities;
                }
        }
        template<typename... T>
        static void Clone(Entity source, Entity target, ComponentGroup<T...>)
        {
            (CloneComponent<T>(source, target), ...);
        }
        static void FinishCopy(Scene& scratch, Scene& source, const std::vector<Entity>& ordered)
        {
            scratch.m_ReconcilingPrefab = true;
            for (auto original : ordered)
            {
                auto copy   = scratch.GetEntityByUUID(original.GetUUID());
                auto parent = source.GetParent(original);
                scratch.SetParent(copy, parent ? scratch.GetEntityByUUID(parent.GetUUID()) : Entity {}, {}, false);
                if (copy.HasComponent<PrefabOwnedComponent>())
                    scratch.m_CapturePrefabNodes.insert(copy);
            }
            scratch.m_ReconcilingPrefab = false;
            scratch.CapturePrefabOverrides();
            for (auto original : ordered)
            {
                auto copy = scratch.GetEntityByUUID(original.GetUUID());
                if (copy && copy.HasComponent<PrefabInstanceComponent>())
                    scratch.ExpandPrefab(copy);
            }
        }
        static std::vector<Entity>
        Insert(Scene& destination, Scene& source, const std::vector<Entity>& ordered, Entity parent)
        {
            const bool previous             = destination.m_ReconcilingPrefab;
            destination.m_ReconcilingPrefab = true;
            std::vector<Entity> created, roots;
            try
            {
                created.reserve(ordered.size());
                for (auto entity : ordered)
                {
                    auto copy = destination.CreateEntityWithID(entity.GetUUID(), "", false);
                    created.push_back(copy);
                    Clone(entity, copy, AllComponents {});
                    if (copy.HasComponent<PrefabInstanceComponent>())
                        copy.GetComponent<PrefabInstanceComponent>().DrawOrder = 0;
                }
                for (auto entity : ordered)
                {
                    auto copy  = destination.GetEntityByUUID(entity.GetUUID());
                    auto owner = source.GetParent(entity);
                    destination.SetParent(
                        copy, owner ? destination.GetEntityByUUID(owner.GetUUID()) : parent, {}, false);
                    if (!owner)
                        roots.push_back(copy);
                }
                destination.m_ReconcilingPrefab = previous;
                if (parent && parent.HasComponent<PrefabOwnedComponent>())
                    for (auto root : roots)
                    {
                        const auto& owned = parent.GetComponent<PrefabOwnedComponent>();
                        if (!root.HasComponent<PrefabAttachmentComponent>())
                            root.AddComponent<PrefabAttachmentComponent>();
                        root.GetComponent<PrefabAttachmentComponent>() = {owned.Instance, owned.NodeKey};
                    }
                return roots;
            }
            catch (...)
            {
                destination.m_ReconcilingPrefab = true;
                for (auto it = created.rbegin(); it != created.rend(); ++it)
                    if (Owns(destination, *it))
                        destination.DestroyEntity(*it);
                destination.m_ReconcilingPrefab = previous;
                throw;
            }
        }
    };
} // namespace Kans

namespace Kans::SceneClipboard
{
    namespace
    {
        constexpr std::string_view Header   = "KansClipboard: 1\n";
        constexpr size_t           MaxBytes = 64ull * 1024 * 1024;
        void                       Require(bool value, const char* message)
        {
            if (!value)
                throw std::runtime_error(message);
        }
        std::vector<Entity> Ordered(Scene& scene)
        {
            std::vector<Entity> result;
            auto                visit = [&](auto&& self, Entity entity) -> void {
                result.push_back(entity);
                for (auto child : scene.GetChildren(entity))
                    self(self, child);
            };
            for (auto root : scene.GetRootEntities())
                visit(visit, root);
            return result;
        }
    } // namespace

    bool IsClipboardText(std::string_view text)
    {
        return text.size() <= MaxBytes && text.substr(0, Header.size()) == Header;
    }

    std::string Copy(Scene& scene, const std::vector<Entity>& entities)
    {
        std::set<UUID> selected, included;
        for (auto entity : entities)
            if (SceneClipboardAccess::Owns(scene, entity))
                selected.insert(entity.GetUUID());
        std::vector<Entity> ordered;
        auto                visit = [&](auto&& self, Entity entity) -> void {
            if (!included.insert(entity.GetUUID()).second)
                return;
            ordered.push_back(entity);
            for (auto child : scene.GetChildren(entity))
                self(self, child);
        };
        for (auto entity : entities)
        {
            if (!SceneClipboardAccess::Owns(scene, entity))
                continue;
            bool covered = false;
            for (auto ancestor = scene.GetParent(entity); ancestor; ancestor = scene.GetParent(ancestor))
                covered |= selected.count(ancestor.GetUUID()) != 0;
            if (!covered)
                visit(visit, entity);
        }

        // Capture pending prefab edits on a private copy; Copy must not flush or
        // reconcile the source scene (including its render-world dirty queues).
        Scene scratch(scene.GetName());
        // Insert only links within the forest; selected roots lose their old parent.
        for (auto entity : ordered)
        {
            auto copy = scratch.CreateEntityWithID(entity.GetUUID(), "", false);
            SceneClipboardAccess::Clone(entity, copy, AllComponents {});
            if (copy.HasComponent<PrefabOwnedComponent>() &&
                !included.count(copy.GetComponent<PrefabOwnedComponent>().Instance))
                copy.RemoveComponent<PrefabOwnedComponent>();
            if (copy.HasComponent<PrefabAttachmentComponent>() &&
                !included.count(copy.GetComponent<PrefabAttachmentComponent>().Instance))
                copy.RemoveComponent<PrefabAttachmentComponent>();
        }
        SceneClipboardAccess::FinishCopy(scratch, scene, ordered);
        YAML::Emitter out;
        out << YAML::BeginMap << YAML::Key << "KansClipboard" << YAML::Value << 1 << YAML::Key << "Scene" << YAML::Value
            << scene.GetName() << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;
        SceneSerializer serializer(nullptr);
        for (auto entity : Ordered(scratch))
            serializer.SerializeEntity(out, entity, true);
        out << YAML::EndSeq << YAML::EndMap;
        return std::string(out.c_str()) + '\n';
    }

    std::variant<std::vector<Entity>, AssetError> Paste(Scene& scene, std::string_view text, Entity parent)
    {
        try
        {
            Require(!parent || SceneClipboardAccess::Owns(scene, parent), "Invalid clipboard destination parent");
            Require(!text.empty() && text.size() <= MaxBytes && text.find('\0') == std::string_view::npos,
                    "Invalid clipboard text");
            // Read the version before enforcing the supported header so version
            // errors identify the supplied version, including future versions.
            Require(text.substr(0, 15) == "KansClipboard: ", "Not Kans entity clipboard text");
            const auto documents = YAML::LoadAll(std::string(text));
            Require(documents.size() == 1, "Clipboard requires one YAML document");
            auto root = documents.front();
            Require(root.IsMap() && root["KansClipboard"] && root["KansClipboard"].IsScalar(),
                    "Missing clipboard version");
            const auto version = root["KansClipboard"].as<std::string>();
            if (version != "1")
                return AssetError {AssetErrorCode::UnsupportedVersion,
                                   "Unsupported Kans clipboard version: " + version.substr(0, 2048)};
            Require(IsClipboardText(text), "Invalid clipboard header");
            Require(root.size() == 3 && root["Scene"] && root["Entities"], "Invalid clipboard fields");
            auto nodes = root["Entities"];
            Require(nodes.IsSequence() && nodes.size() <= 65536, "Invalid clipboard entities");
            std::map<uint64_t, YAML::Node> byID;
            for (auto node : nodes)
            {
                Require(node.IsMap() && node["Entity"], "Invalid clipboard entity");
                const auto id = node["Entity"].as<uint64_t>();
                Require(id && byID.emplace(id, node).second, "Duplicate/zero clipboard identity");
            }
            // Validate ownership/hierarchy before component construction calls any
            // world-transform helpers. This also bounds recursive Scene traversal.
            for (const auto& [id, node] : byID)
            {
                std::set<uint64_t> ancestors;
                auto               current = id;
                while (byID.at(current)["Parent"])
                {
                    current = byID.at(current)["Parent"].as<uint64_t>();
                    Require(byID.count(current) && current != id && ancestors.insert(current).second &&
                                ancestors.size() <= 256,
                            "Invalid clipboard hierarchy");
                }
                if (const auto owned = node["PrefabOwnedComponent"])
                    Require(ancestors.count(owned["Instance"].as<uint64_t>()), "Invalid prefab owner hierarchy");
            }
            std::map<uint64_t, uint64_t> remap;
            std::set<uint64_t>           reserved;
            for (const auto& [id, node] : byID)
                reserved.insert(id);
            // Parts retain the deterministic instance/key identity used by prefab
            // reconciliation. Retry the forest's new IDs on a hash collision.
            bool allocated = false;
            for (unsigned attempt = 0; attempt < 32 && !allocated; ++attempt)
            {
                remap.clear();
                auto                              used = reserved;
                std::set<uint64_t>                visiting;
                std::function<uint64_t(uint64_t)> allocate = [&](uint64_t id) -> uint64_t {
                    if (remap.count(id))
                        return remap.at(id);
                    Require(visiting.insert(id).second, "Cyclic prefab ownership");
                    const auto node = byID.at(id);
                    uint64_t   value;
                    if (const auto owned = node["PrefabOwnedComponent"])
                    {
                        const auto owner = owned["Instance"].as<uint64_t>();
                        Require(byID.count(owner) && byID.at(owner)["PrefabInstanceComponent"],
                                "Missing clipboard prefab instance");
                        const auto instance = allocate(owner);
                        if (!instance)
                            return 0;
                        value = Scene::PrefabEntityUUID(UUID(instance), owned["Node"].as<std::string>());
                    }
                    else
                        do
                        {
                            value = UUID();
                        } while (!value || used.count(value) || scene.GetEntityByUUID(UUID(value)));
                    visiting.erase(id);
                    if (used.count(value) || scene.GetEntityByUUID(UUID(value)))
                        return 0;
                    used.insert(value);
                    remap[id] = value;
                    return value;
                };
                allocated = true;
                for (const auto& [id, node] : byID)
                    if (!allocate(id))
                    {
                        allocated = false;
                        break;
                    }
            }
            Require(allocated, "Could not allocate clipboard identities");
            auto reference = [&](YAML::Node value) {
                // Optional references (e.g. an IK constraint without a pole) are simply absent.
                if (!value || !value.IsScalar())
                    return;
                const auto id = value.as<uint64_t>();
                value         = remap.count(id) ? remap.at(id) : (scene.GetEntityByUUID(UUID(id)) ? id : 0);
            };
            for (auto node : nodes)
            {
                node["Entity"] = remap.at(node["Entity"].as<uint64_t>());
                if (node["Parent"])
                {
                    const auto id = node["Parent"].as<uint64_t>();
                    Require(remap.count(id), "Clipboard parent outside copied forest");
                    node["Parent"] = remap.at(id);
                }
                if (auto owned = node["PrefabOwnedComponent"])
                    reference(owned["Instance"]);
                if (auto attachment = node["PrefabParent"])
                {
                    reference(attachment["Instance"]);
                    if (!attachment["Instance"].as<uint64_t>())
                        node.remove("PrefabParent");
                }
                auto jointReferences = [&](YAML::Node joint) {
                    if (joint)
                    {
                        reference(joint["BodyA"]);
                        reference(joint["BodyB"]);
                    }
                };
                auto springReferences = [&](YAML::Node spring) {
                    if (!spring)
                        return;
                    reference(spring["Center"]);
                    for (auto id : spring["ColliderGroups"])
                        reference(id);
                    // Cloth shares center/groups, but has no chain field. Const lookup must not add it to YAML.
                    for (auto chain : std::as_const(spring)["Chains"])
                    {
                        reference(chain["RootBone"]);
                        reference(chain["EndBone"]);
                        for (auto id : chain["ExcludedBones"])
                            reference(id);
                    }
                };
                springReferences(node["SpringBoneComponent"]);
                springReferences(node["ClothComponent"]);
                auto vehicleReferences = [&](YAML::Node vehicle) {
                    if (vehicle)
                        for (auto wheel : vehicle["Wheels"])
                            reference(wheel["Visual"]);
                };
                vehicleReferences(node["VehicleComponent"]);
                if (auto prefab = node["PrefabInstanceComponent"])
                    for (auto change : prefab["Overrides"])
                    {
                        springReferences(change["SpringBoneComponent"]);
                        springReferences(change["ClothComponent"]);
                        vehicleReferences(change["VehicleComponent"]);
                    }
                jointReferences(node["JointComponent"]);
                if (auto prefab = node["PrefabInstanceComponent"])
                    for (auto change : prefab["Overrides"])
                        jointReferences(change["JointComponent"]);
                if (auto drivers = node["MaterialParameterDriversComponent"])
                    for (auto binding : drivers["Bindings"])
                        reference(binding["SourceEntity"]);
                if (auto ik = node["IKComponent"])
                    for (auto constraint : ik["Constraints"])
                    {
                        reference(constraint["Data"]["target"]["entity"]);
                        if (constraint["Type"].as<std::string>() == "TwoBone")
                            reference(constraint["Data"]["pole_entity"]);
                    }
                if (auto timeline = node["TimelinePlayerComponent"])
                    for (auto binding : timeline["Bindings"])
                        reference(binding.second);
                if (auto animator = node["AnimatorComponent"])
                {
                    if (auto joints = animator["JointEntities"])
                        for (size_t i = 0; i < joints.size(); ++i)
                            reference(joints[i]);
                    if (auto controller = animator["InlineController"])
                    {
                        for (auto constraint : controller["two_bone_ik"])
                        {
                            reference(constraint["target"]["entity"]);
                            reference(constraint["pole_entity"]);
                        }
                        for (auto constraint : controller["look_at_ik"])
                            reference(constraint["target"]["entity"]);
                    }
                }
            }
            YAML::Node document;
            document["Version"]  = 1;
            document["Scene"]    = root["Scene"];
            document["Entities"] = nodes;
            auto decoded         = SceneSerializer::DeserializeNodeChecked(document, true);
            if (auto error = std::get_if<AssetError>(&decoded))
                return *error;
            auto candidate = std::get<Ref<Scene>>(decoded);
            auto ordered   = Ordered(*candidate);
            return SceneClipboardAccess::Insert(scene, *candidate, ordered, parent);
        }
        catch (const std::exception& error)
        {
            return AssetError {AssetErrorCode::InvalidData, std::string(error.what()).substr(0, 2048)};
        }
        catch (...)
        {
            return AssetError {AssetErrorCode::InvalidData, "Invalid entity clipboard data"};
        }
    }
} // namespace Kans::SceneClipboard

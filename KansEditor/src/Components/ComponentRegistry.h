#pragma once
#include <any>
#include <functional>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Asset/AssetType.h"
#include "Kans3D/Scene/Entity.h"

namespace Kans
{
    class AssetManager;
    class Scene;
    class Texture2D;
    namespace SecondaryMotionEditing
    {
        class ChainPicker;
    }

    // The parts a material slot row stands for: each entity's subtree sections using that slot.
    using SlotParts = std::vector<std::pair<UUID, uint32_t>>;

    // Services a component editor draws with; implemented by the Properties panel.
    class ComponentUI
    {
    public:
        virtual ~ComponentUI() = default;

        virtual Scene*        GetScene()  = 0;
        virtual AssetManager* GetAssets() = 0;
        virtual void          Select(Entity entity) = 0;
        // Picks a published asset of `type` (also by dropping its source file); true when `id` changed.
        virtual bool AssetPicker(const char* label, AssetType type, AssetID& id) = 0;
        // Picks a replacement material; true with an invalid ID to restore the imported one.
        virtual bool        MaterialPicker(const char* label, AssetID imported, AssetID current, AssetID& chosen) = 0;
        virtual std::string MaterialName(AssetID material) = 0;
        // The combo of MaterialPicker without its row label.
        virtual bool MaterialCombo(AssetID imported, AssetID current, AssetID& chosen) = 0;
        // Rendered preview of a material; null while it renders or when it has no source.
        virtual Ref<Texture2D> MaterialThumbnail(AssetID material) = 0;
        // The material file selected in the Content Browser, or invalid.
        virtual AssetID BrowserMaterial() = 0;
        // A material file dropped on the last item this frame, or invalid.
        virtual AssetID AcceptMaterialDrop() = 0;
        // Viewport feedback for a slot row: hovered this frame, pinned highlight, isolated.
        virtual void HoverSlot(const SlotParts& parts)           = 0;
        virtual bool IsSlotHighlighted(const SlotParts& parts)   = 0;
        virtual void ToggleSlotHighlight(const SlotParts& parts) = 0;
        virtual bool IsSlotIsolated(const SlotParts& parts)      = 0;
        virtual void ToggleSlotIsolate(const SlotParts& parts)   = 0;
        // A material that is its own file opens in the Material Editor; one inside a model cannot.
        virtual bool CanOpenMaterial(AssetID material) = 0;
        virtual void OpenMaterial(AssetID material)    = 0;
        // Selects the asset's source file (a model for its embedded materials) in the Content Browser.
        virtual void RevealAsset(AssetID asset) = 0;
        // Writes an instance of `parent` beside its source and assigns it to `parts` once imported.
        virtual void CreateMaterialInstance(AssetID parent, const SlotParts& parts) = 0;
        // Shows the Animator graph window (for the selected model).
        virtual void OpenAnimatorGraph() {}
        // Shows the Timeline window for this Timeline Player.
        virtual void OpenTimeline(Entity) {}
        // Cloth: writes a .cloth asset for `section` of the entity's mesh (beside the mesh's source, or
        // in the Content Browser's open folder) and assigns it once imported; enters or reports the
        // viewport's painting mode.
        virtual void CreateClothAsset(Entity, uint32_t /*section*/, bool /*browserFolder*/) {}
        virtual void PaintCloth(Entity) {}
        virtual bool IsPaintingCloth(Entity) { return false; }
        // Spring chains' "Pick from Hierarchy" (null when the panel cannot pick).
        virtual SecondaryMotionEditing::ChainPicker* GetChainPicker() { return nullptr; }
        // Ragdolls: open a .ragdoll in the ragdoll editor; generate one for a skinned entity (its
        // Animator's skeleton) and assign it once imported.
        virtual void OpenRagdollEditor(AssetID /*ragdoll*/) {}
        virtual void CreateRagdoll(Entity) {}
        // Vehicles: the wheel row selected in the card, which the viewport highlights (-1: none).
        virtual int  GetSelectedWheel(Entity) { return -1; }
        virtual void SelectWheel(Entity, int /*wheel*/) {}
    };

    // Everything the editor knows about one component type: how it is named, shown,
    // added, reset, copied and drawn. The Properties panel, its Add Component menu,
    // the Hierarchy's icons and its Create menu are all generated from these entries,
    // so a new component is registered in one place.
    struct ComponentEntry
    {
        std::type_index Type = typeid(void);
        std::string     Name;
        const char*     Icon = nullptr;
        std::string     Category;
        // Lower ranks decide the Hierarchy icon when an entity has several components.
        int IconRank = 1000;

        std::function<bool(Entity)> Has;
        // Add Component: adds the component with its editor defaults.
        std::function<void(Entity)> Add;
        // Empty when the component can always be added; otherwise whether it can be to this entity.
        std::function<bool(Entity)> CanAdd;
        // Empty when the component cannot be removed; otherwise whether it can be for this entity.
        std::function<bool(Entity)> CanRemove;
        std::function<void(Entity)> Remove;
        // Empty when Reset is not offered.
        std::function<void(Entity)> Reset;
        std::function<void(Entity, ComponentUI&)> Draw;
        // Copy/Paste values; empty when not offered.
        std::function<std::any(Entity)>              Copy;
        std::function<void(Entity, const std::any&)> Paste;
        // Prefab parts: whether this component carries an override, and how to revert it.
        std::function<bool(Entity, Scene&)> Overridden;
        std::function<void(Entity, Scene&)> Revert;
        // Hierarchy Create menu: the new entity's name (empty: not offered) and its setup.
        std::string                 CreateName;
        std::function<void(Entity)> Create;
        int                         CreateOrder = 0;
        // A Create submenu of several variants (e.g. "3D Object" with one item per shape),
        // used instead of CreateName/Create when not empty.
        struct CreateItem
        {
            std::string                 Name;
            const char*                 Icon = nullptr;
            std::function<void(Entity)> Create;
        };
        std::string             CreateMenu;
        std::vector<CreateItem> CreateItems;
    };

    class ComponentRegistry
    {
    public:
        // The editor's components, in the order the Properties panel shows them.
        static const ComponentRegistry& Get();

        const std::vector<ComponentEntry>& Entries() const { return m_Entries; }
        const ComponentEntry*              Find(std::type_index type) const;
        // The most telling component's icon (see IconRank); a plain entity icon otherwise.
        const char* IconFor(Entity entity) const;

        // Registers T with the default operations (add, remove, reset to T{}, copy and
        // paste through PatchComponent); the caller overrides what differs.
        template<typename T>
        ComponentEntry& Register(std::string name, const char* icon, std::string category)
        {
            ComponentEntry entry;
            entry.Type      = typeid(T);
            entry.Name      = std::move(name);
            entry.Icon      = icon;
            entry.Category  = std::move(category);
            entry.Has       = [](Entity e) { return e.HasComponent<T>(); };
            entry.Add       = [](Entity e) { e.AddComponent<T>(); };
            entry.CanRemove = [](Entity) { return true; };
            entry.Remove    = [](Entity e) { e.RemoveComponent<T>(); };
            entry.Reset     = [](Entity e) { e.PatchComponent<T>([](T& c) { c = T(); }); };
            entry.Copy      = [](Entity e) { return std::any(e.GetComponent<T>()); };
            entry.Paste     = [](Entity e, const std::any& value) {
                if (const auto* copied = std::any_cast<T>(&value))
                    e.PatchComponent<T>([&](T& c) { c = *copied; });
            };
            m_Entries.push_back(std::move(entry));
            return m_Entries.back();
        }

    private:
        std::vector<ComponentEntry> m_Entries;
    };

    // Registers the built-in components (ComponentEditors.cpp).
    void RegisterBuiltinComponents(ComponentRegistry& registry);
} // namespace Kans

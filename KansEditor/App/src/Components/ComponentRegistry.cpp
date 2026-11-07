#include "ComponentRegistry.h"
#include "UI/EditorIcons.h"

namespace Kans
{
    const ComponentRegistry& ComponentRegistry::Get()
    {
        static const ComponentRegistry registry = [] {
            ComponentRegistry built;
            RegisterBuiltinComponents(built);
            return built;
        }();
        return registry;
    }

    const ComponentEntry* ComponentRegistry::Find(std::type_index type) const
    {
        for (const auto& entry : m_Entries)
            if (entry.Type == type)
                return &entry;
        return nullptr;
    }

    const char* ComponentRegistry::IconFor(Entity entity) const
    {
        const ComponentEntry* best = nullptr;
        for (const auto& entry : m_Entries)
            if (entry.Has(entity) && (!best || entry.IconRank < best->IconRank))
                best = &entry;
        return best ? best->Icon : ICON_FA_VECTOR_SQUARE;
    }
} // namespace Kans

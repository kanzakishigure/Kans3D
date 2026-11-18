#include "ComponentRegistry.h"
#include <algorithm>
#include <stdexcept>
namespace Kans
{
    const ComponentRegistry& ComponentRegistry::Get() { return Runtime(); }
    ComponentRegistry&       ComponentRegistry::Runtime()
    {
        static ComponentRegistry registry;
        return registry;
    }
    void ComponentRegistry::RegisterEntry(ComponentEntry entry)
    {
        if (entry.Name.empty() || !entry.Has || !entry.Add || !entry.Remove || !entry.Serialize || !entry.Deserialize)
            throw std::runtime_error("Plugin component requires name, operations and serialization");
        for (const auto& current : m_Entries)
            if (current.Name == entry.Name || current.Type == entry.Type)
                throw std::runtime_error("Duplicate component " + entry.Name);
        m_Entries.push_back(std::move(entry));
    }
    void ComponentRegistry::Unregister(const std::string& name)
    {
        m_Entries.erase(
            std::remove_if(m_Entries.begin(), m_Entries.end(), [&](const auto& e) { return e.Name == name; }),
            m_Entries.end());
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
        return best ? best->Icon : "\xef\x97\x8b";
    }
} // namespace Kans

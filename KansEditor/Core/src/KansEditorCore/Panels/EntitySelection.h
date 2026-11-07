#pragma once
#include <vector>
#include "Kans3D/Core/UUID.h"

namespace Kans
{
    // The editor's entity selection: an ordered set with a primary entity (the one
    // Properties and the gizmo follow) and an anchor for Shift ranges.
    class EntitySelection
    {
    public:
        // Plain click selects one entity; Ctrl toggles it; Shift selects the range
        // from the anchor to it in `order` (the entities as shown).
        void Click(UUID id, bool ctrl, bool shift, const std::vector<UUID>& order);
        void Set(UUID id);
        void Clear();
        // Keeps only entities that still exist.
        template<typename Exists>
        void Retain(Exists exists)
        {
            std::vector<UUID> kept;
            for (auto id : m_Items)
                if (exists(id))
                    kept.push_back(id);
            m_Items = std::move(kept);
            if (!Contains(m_Primary))
                m_Primary = m_Items.empty() ? UUID(0) : m_Items.back();
        }

        bool                     Contains(UUID id) const;
        bool                     Empty() const { return m_Items.empty(); }
        size_t                   Size() const { return m_Items.size(); }
        UUID                     Primary() const { return m_Primary; }
        // In the order they were selected.
        const std::vector<UUID>& Items() const { return m_Items; }

    private:
        std::vector<UUID> m_Items;
        UUID              m_Primary = 0, m_Anchor = 0;
    };
} // namespace Kans

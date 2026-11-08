#include "KansEditorCore/UI/TitlebarRegions.h"

namespace Kans
{
    void TitlebarRegions::Begin(const TitlebarRect& band)
    {
        m_Recording.HasBand     = true;
        m_Recording.Band        = band;
        m_Recording.HasMaximize = false;
        m_Recording.Clients.clear();
    }

    void TitlebarRegions::AddClient(const TitlebarRect& rect) { m_Recording.Clients.push_back(rect); }

    void TitlebarRegions::SetMaximize(const TitlebarRect& rect)
    {
        m_Recording.HasMaximize = true;
        m_Recording.Maximize    = rect;
    }

    void TitlebarRegions::Publish() { m_Published = m_Recording; }

    void TitlebarRegions::Clear()
    {
        m_Recording = {};
        m_Published = {};
    }

    TitlebarHit TitlebarRegions::HitTest(float x, float y) const
    {
        const auto& layout = m_Published;
        if (!layout.HasBand || !layout.Band.Contains(x, y))
            return TitlebarHit::Client;
        if (layout.HasMaximize && layout.Maximize.Contains(x, y))
            return TitlebarHit::Maximize;
        for (const auto& client : layout.Clients)
            if (client.Contains(x, y))
                return TitlebarHit::Client;
        return TitlebarHit::Caption;
    }
} // namespace Kans

#pragma once

#include <cstddef>
#include <vector>
#include "Kans3D/Asset/Asset.h"

namespace Kans
{
    class AudioHRTFAsset final : public Asset
    {
    public:
        AudioHRTFAsset(std::vector<std::byte> sofa, bool normalizeVolume) :
            m_Sofa(std::move(sofa)), m_NormalizeVolume(normalizeVolume)
        {}

        static AssetType              GetStaticType() { return AssetType::AudioHRTF; }
        AssetType                     GetAssetType() const override { return GetStaticType(); }
        const std::vector<std::byte>& GetSOFA() const { return m_Sofa; }
        bool                          NormalizeVolume() const { return m_NormalizeVolume; }

    private:
        std::vector<std::byte> m_Sofa;
        bool                   m_NormalizeVolume;
    };
} // namespace Kans

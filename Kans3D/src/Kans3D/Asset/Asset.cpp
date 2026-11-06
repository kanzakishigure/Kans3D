
#include "kspch.h"
#include "Asset.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include "Kans3D/Core/Hash.h"
#include "Kans3D/Core/UUID.h"
#include "Kans3D/Core/Utilities/StringUtils.h"

namespace Kans
{

    AssetPath::AssetPath(const std::filesystem::path& path) : m_Path(path), m_Hash(Hash::None)
    {
        normalize();

        if (m_Path.empty())
        {
            m_Path = std::filesystem::path {};
            m_Hash = 0;
            return;
        }

        m_Hash = Hash::Generate64MD5Hash(m_Path.generic_u8string());
    }
    void AssetPath::normalize()
    {
        if (m_Path.empty())
        {
            return;
        }

        if (m_Path.is_absolute() || m_Path.has_root_name() || m_Path.has_root_directory())
        {
            m_Path.clear();
            return;
        }

        for (const auto& component : m_Path)
        {
            if (component == std::filesystem::path(".."))
            {
                m_Path.clear();
                return;
            }
        }

        m_Path = m_Path.lexically_normal();
        if (m_Path.empty() || m_Path == ".")
        {
            m_Path.clear();
            return;
        }
    }
    std::string SourceAssetID::ToString() const { return Utils::Uint64ToHex(m_ID); }

    SourceAssetID::SourceAssetID() : m_ID(UUID::None), m_Hash(0) {}
    SourceAssetID SourceAssetID::FromString(const std::string& idStr)
    {
        uint64_t uuid;
        if (idStr.empty() || !Utils::HexToUint64(idStr, uuid) || uuid == UUID::None)
        {
            return SourceAssetID {};
        }
        SourceAssetID id = {};
        id.m_ID          = uuid;
        id.m_Hash        = Hash::Generate64MD5Hash(id.ToString());

        return id;
    }
    SourceAssetID SourceAssetID::Generate()
    {
        SourceAssetID sourceID {};
        while (sourceID.m_ID == UUID::None)
        {
            sourceID.m_ID = UUID {};
        }
        sourceID.m_Hash = Hash::Generate64MD5Hash(sourceID.ToString());
        return sourceID;
    }
    AssetID::AssetID() : m_sourceID(), m_localID(UUID::None) {}
    AssetID::AssetID(const SourceAssetID sourceAssetID, const uint64_t localID) :
        m_sourceID(sourceAssetID), m_localID(localID)
    {}
    AssetID AssetID::FromString(const std::string& assetIDStr)
    {
        if (assetIDStr.size() != 33 || assetIDStr[16] != ':')
            return {};

        const std::string sourceIDStr = assetIDStr.substr(0, 16);
        const std::string localIDStr  = assetIDStr.substr(17, 16);
        uint64_t          localID     = UUID::None;
        SourceAssetID     sourceID    = SourceAssetID::FromString(sourceIDStr);
        if (!sourceID.IsValid())
        {
            return AssetID {};
        }
        if (!Utils::HexToUint64(localIDStr, localID) || localID == UUID::None)
        {
            return AssetID {};
        }
        return AssetID {sourceID, localID};
    }
    std::string AssetID::ToString() const { return m_sourceID.ToString() + ':' + Utils::Uint64ToHex(m_localID); }
} // namespace Kans

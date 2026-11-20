#include "AudioPreviewCache.h"
#include <algorithm>
#include <fstream>
#include "Kans3D/Asset/Product/AudioProduct.h"
#include "KansEditorCore/EditorAssetImportService.h"

namespace Kans
{
    std::vector<uint8_t> AudioPeakColumns(const std::vector<uint8_t>& peaks, size_t columns)
    {
        std::vector<uint8_t> out(peaks.empty() ? 0 : columns, 0);
        for (size_t x = 0; x < out.size(); ++x)
        {
            const size_t first = x * peaks.size() / columns;
            const size_t last  = std::max(first + 1, (x + 1) * peaks.size() / columns);
            for (size_t i = first; i < last && i < peaks.size(); ++i)
                out[x] = std::max(out[x], peaks[i]);
        }
        return out;
    }

    const AudioClipSummary* AudioPreviewCache::Find(const Ref<const AssetRegistry>& registry,
                                                    const std::filesystem::path&    productDirectory,
                                                    SourceAssetID                   source)
    {
        if (!registry)
            return nullptr;
        if (m_Indexed != registry)
        {
            m_Indexed = registry;
            m_Records.clear();
            for (auto& record : registry->GetAllProducts())
                if (record.Target == EditorAssetTarget && record.Metadata.Type == AssetType::AudioClip)
                    m_Records.emplace(record.Metadata.assetID.GetSourceID(), std::move(record));
        }
        const auto record = m_Records.find(source);
        if (record == m_Records.end())
            return nullptr;
        const auto& product = record->second;
        const auto  key     = product.Metadata.assetID.ToString() + "#" + product.Location.ContentDigest;
        auto [found, added] = m_Summaries.try_emplace(key);
        if (added)
        {
            // Only the header and the envelope: the encoded audio can be hundreds of megabytes.
            std::vector<std::byte> bytes(
                size_t(std::min<uint64_t>(AudioProductPreviewBytes, product.Location.ByteSize)));
            std::ifstream file(productDirectory / product.Location.RelativeFile.GetPath(), std::ios::binary);
            file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
            if (file && size_t(file.gcount()) == bytes.size())
            {
                auto decoded = DecodeAudioProductPreview(bytes.data(), bytes.size(), product.Location.ByteSize);
                if (auto* data = std::get_if<AudioClipData>(&decoded))
                    found->second = AudioClipSummary {product.Metadata.assetID, std::move(*data)};
            }
        }
        return found->second ? &*found->second : nullptr;
    }

    void AudioPreviewCache::Clear()
    {
        m_Indexed.reset();
        m_Records.clear();
        m_Summaries.clear();
    }
} // namespace Kans

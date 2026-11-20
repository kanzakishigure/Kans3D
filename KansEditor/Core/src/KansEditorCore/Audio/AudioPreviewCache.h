#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "Kans3D/Asset/AssetRegistry.h"
#include "Kans3D/Asset/Runtime/AudioClipData.h"

namespace Kans
{
    // What the editor shows of an imported clip without loading its audio: the header and the
    // peak envelope stored in the Product (Data.Encoded stays empty).
    struct AudioClipSummary
    {
        AssetID       ID;
        AudioClipData Data;

        double Seconds() const
        {
            return Data.Info.SampleRate ? double(Data.Info.LengthFrames) / Data.Info.SampleRate : 0.0;
        }
    };

    // `columns` peaks of an envelope: each column is the largest peak of the envelope's buckets
    // it covers (a bucket wider than a column repeats), so a short click stays visible at any size.
    std::vector<uint8_t> AudioPeakColumns(const std::vector<uint8_t>& peaks, size_t columns);

    // Summaries of the published editor AudioClips by source, read from the first bytes of each
    // Product file (DecodeAudioProductPreview) once per Product content: a reimport reads again.
    class AudioPreviewCache
    {
    public:
        // The clip of `source` in `registry`, whose Products live in `productDirectory`; null when
        // the source has no published clip or its Product cannot be read.
        const AudioClipSummary* Find(const Ref<const AssetRegistry>& registry,
                                     const std::filesystem::path&    productDirectory,
                                     SourceAssetID                   source);
        void                    Clear();

    private:
        Ref<const AssetRegistry>                                         m_Indexed;
        std::unordered_map<SourceAssetID, ProductRecord>                 m_Records;
        std::unordered_map<std::string, std::optional<AudioClipSummary>> m_Summaries; // By ID and content digest.
    };
} // namespace Kans

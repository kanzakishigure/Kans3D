#pragma once
#include <cstddef>
#include <memory>
#include <variant>
#include <vector>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Asset/AssetError.h"

namespace Kans
{
    enum class VideoMatrix : uint8_t
    {
        BT601,
        BT709
    };
    enum class VideoRange : uint8_t
    {
        Limited,
        Full
    };
    enum class VideoTransfer : uint8_t
    {
        BT709,
        SRGB,
        Gamma22,
        Gamma28
    };
    enum class VideoPrimaries : uint8_t
    {
        BT709,
        SMPTE170M,
        BT470BG
    };

    struct VideoClipInfo
    {
        uint32_t       Width = 0, Height = 0;
        uint32_t       RateNumerator = 0, RateDenominator = 1;
        double         Duration   = 0;
        uint64_t       FrameCount = 0; // Zero when the container does not supply a count.
        std::string    Codec, PixelFormat;
        VideoMatrix    Matrix    = VideoMatrix::BT709;
        VideoRange     Range     = VideoRange::Limited;
        VideoTransfer  Transfer  = VideoTransfer::BT709;
        VideoPrimaries Primaries = VideoPrimaries::BT709;
        bool           HasAudio  = false;
    };

    using VideoBytes                                     = std::vector<std::byte>;
    inline constexpr uint64_t VideoProductMaximumBytes   = 512ull * 1024 * 1024;
    inline constexpr char     VideoDecodingUnavailable[] = "video decoding unavailable (requires shared LGPL FFmpeg)";

    struct VideoClipData
    {
        VideoClipInfo                     Info;
        std::shared_ptr<const VideoBytes> Encoded;
    };

    class VideoClipAsset final : public Asset
    {
    public:
        explicit VideoClipAsset(VideoClipData data) : m_Data(std::move(data)) {}
        static AssetType     GetStaticType() { return AssetType::VideoClip; }
        AssetType            GetAssetType() const override { return GetStaticType(); }
        const VideoClipData& GetData() const { return m_Data; }

    private:
        VideoClipData m_Data;
    };

    bool                                    IsVideoDecodingAvailable();
    std::variant<VideoClipInfo, AssetError> ProbeVideo(const VideoBytes&);
    std::variant<VideoBytes, AssetError>    EncodeVideoProduct(const VideoClipData&);
    std::variant<VideoClipData, AssetError> DecodeVideoProduct(const std::byte*, size_t);
} // namespace Kans

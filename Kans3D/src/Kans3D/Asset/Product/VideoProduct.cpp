#include "kspch.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include "Kans3D/Video/VideoClip.h"

namespace Kans
{
    namespace
    {
        constexpr uint32_t Magic       = 0x4449564b; // KVID
        constexpr size_t   HeaderSize  = 48;
        constexpr size_t   MaximumName = 32;

        template<typename T>
        void Write(VideoBytes& bytes, T value)
        {
            for (size_t i = 0; i < sizeof(T); ++i)
                bytes.push_back(static_cast<std::byte>((value >> (8 * i)) & 255));
        }

        template<typename T>
        T Read(const std::byte*& cursor)
        {
            T value = 0;
            for (size_t i = 0; i < sizeof(T); ++i)
                value |= T(std::to_integer<uint8_t>(*cursor++)) << (8 * i);
            return value;
        }

        bool Valid(const VideoClipInfo& info)
        {
            return info.Width && info.Height && info.Width <= 8192 && info.Height <= 8192 && !(info.Width % 2) &&
                   !(info.Height % 2) && info.RateNumerator && info.RateDenominator && std::isfinite(info.Duration) &&
                   info.Duration > 0 && info.Codec.size() > 0 && info.Codec.size() <= MaximumName &&
                   info.PixelFormat.size() > 0 && info.PixelFormat.size() <= MaximumName &&
                   uint8_t(info.Matrix) <= uint8_t(VideoMatrix::BT709) &&
                   uint8_t(info.Range) <= uint8_t(VideoRange::Full) &&
                   uint8_t(info.Transfer) <= uint8_t(VideoTransfer::Gamma28) &&
                   uint8_t(info.Primaries) <= uint8_t(VideoPrimaries::BT470BG);
        }

        AssetError Invalid() { return {AssetErrorCode::InvalidData, "Invalid VideoClip product"}; }
    } // namespace

    std::variant<VideoBytes, AssetError> EncodeVideoProduct(const VideoClipData& clip)
    {
        const auto& info = clip.Info;
        if (!Valid(info) || !clip.Encoded || clip.Encoded->empty() ||
            clip.Encoded->size() > VideoProductMaximumBytes - HeaderSize - info.Codec.size() - info.PixelFormat.size())
            return Invalid();
        VideoBytes bytes;
        bytes.reserve(HeaderSize + info.Codec.size() + info.PixelFormat.size() + clip.Encoded->size());
        Write(bytes, Magic);
        Write(bytes, uint16_t(1));
        Write(bytes, uint16_t(AssetType::VideoClip));
        Write(bytes, info.Width);
        Write(bytes, info.Height);
        Write(bytes, info.RateNumerator);
        Write(bytes, info.RateDenominator);
        uint64_t duration;
        static_assert(sizeof(duration) == sizeof(double));
        std::memcpy(&duration, &info.Duration, sizeof(duration));
        Write(bytes, duration);
        Write(bytes, info.FrameCount);
        Write(bytes, uint8_t(info.Matrix));
        Write(bytes, uint8_t(info.Range));
        Write(bytes, uint8_t(info.Transfer));
        Write(bytes, uint8_t(uint8_t(info.HasAudio) | (uint8_t(info.Primaries) << 1)));
        Write(bytes, uint16_t(info.Codec.size()));
        Write(bytes, uint16_t(info.PixelFormat.size()));
        for (auto* name : {&info.Codec, &info.PixelFormat})
            for (const char c : *name)
                bytes.push_back(static_cast<std::byte>(c));
        bytes.insert(bytes.end(), clip.Encoded->begin(), clip.Encoded->end());
        return bytes;
    }

    namespace
    {
        // The header's metadata and its size (names included); bytes past the header are not read.
        std::variant<std::pair<VideoClipInfo, size_t>, AssetError> ParseHeader(const std::byte* bytes, size_t size)
        {
            if (!bytes || size < HeaderSize)
                return Invalid();
            const auto* cursor = bytes;
            if (Read<uint32_t>(cursor) != Magic)
                return Invalid();
            if (Read<uint16_t>(cursor) != 1)
                return AssetError {AssetErrorCode::UnsupportedVersion, "Unsupported VideoClip product version"};
            if (Read<uint16_t>(cursor) != uint16_t(AssetType::VideoClip))
                return Invalid();
            VideoClipInfo info;
            info.Width           = Read<uint32_t>(cursor);
            info.Height          = Read<uint32_t>(cursor);
            info.RateNumerator   = Read<uint32_t>(cursor);
            info.RateDenominator = Read<uint32_t>(cursor);
            const auto duration  = Read<uint64_t>(cursor);
            std::memcpy(&info.Duration, &duration, sizeof(duration));
            info.FrameCount       = Read<uint64_t>(cursor);
            info.Matrix           = VideoMatrix(Read<uint8_t>(cursor));
            info.Range            = VideoRange(Read<uint8_t>(cursor));
            info.Transfer         = VideoTransfer(Read<uint8_t>(cursor));
            const auto flags      = Read<uint8_t>(cursor);
            info.HasAudio         = (flags & 1) != 0;
            info.Primaries        = VideoPrimaries(flags >> 1);
            const auto codecSize  = Read<uint16_t>(cursor);
            const auto formatSize = Read<uint16_t>(cursor);
            if (codecSize > MaximumName || formatSize > MaximumName || size < HeaderSize + codecSize + formatSize)
                return Invalid();
            info.Codec.assign(reinterpret_cast<const char*>(cursor), codecSize);
            cursor += codecSize;
            info.PixelFormat.assign(reinterpret_cast<const char*>(cursor), formatSize);
            if (!Valid(info))
                return Invalid();
            return std::make_pair(std::move(info), HeaderSize + codecSize + formatSize);
        }
    } // namespace

    std::variant<VideoClipInfo, AssetError> DecodeVideoProductInfo(const std::byte* bytes, size_t size)
    {
        static_assert(VideoProductInfoBytes == HeaderSize + 2 * MaximumName);
        auto header = ParseHeader(bytes, std::min(size, VideoProductInfoBytes));
        if (auto* error = std::get_if<AssetError>(&header))
            return std::move(*error);
        return std::move(std::get<0>(header).first);
    }

    std::variant<VideoClipData, AssetError> DecodeVideoProduct(const std::byte* bytes, size_t size)
    {
        if (!bytes || size <= HeaderSize || size > VideoProductMaximumBytes)
            return Invalid();
        auto header = ParseHeader(bytes, size);
        if (auto* error = std::get_if<AssetError>(&header))
            return std::move(*error);
        auto& [info, used] = std::get<0>(header);
        // A product holds a stream after its header.
        if (size <= used)
            return Invalid();
        VideoClipData clip;
        clip.Info    = std::move(info);
        clip.Encoded = std::make_shared<const VideoBytes>(bytes + used, bytes + size);
        return clip;
    }
} // namespace Kans

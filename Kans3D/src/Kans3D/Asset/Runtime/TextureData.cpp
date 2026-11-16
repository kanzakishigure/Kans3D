#include "kspch.h"
#include "TextureData.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace Kans
{
    namespace
    {
        constexpr uint64_t kMaximumPixels = 512 * 1024 * 1024 - 48;
        uint32_t           Extent(uint32_t base, uint32_t level) { return (std::max)(base >> level, 1u); }
        uint32_t           FullLevels(const TextureData& texture)
        {
            uint32_t size   = (std::max)({texture.Width, texture.Height, texture.Depth});
            uint32_t levels = 0;
            do
            {
                ++levels;
                size >>= 1;
            } while (size);
            return levels;
        }
        uint32_t Stride(TexturePixelFormat format)
        {
            switch (format)
            {
                case TexturePixelFormat::R8:
                    return 1;
                case TexturePixelFormat::RG8:
                    return 2;
                case TexturePixelFormat::RGBA8:
                    return 4;
                case TexturePixelFormat::RGBA32Float:
                    return 16;
            }
            return 0;
        }
        bool ValidLayout(const TextureData& t)
        {
            if (!t.Width || !t.Height || !t.Depth || !t.Layers || t.Width > 16384 || t.Height > 16384 ||
                t.Depth > 16384 || t.Layers > 2048 || !Stride(t.Format) ||
                uint16_t(t.Kind) > uint16_t(TextureKind::Volume3D) ||
                uint16_t(t.Usage) > uint16_t(TextureUsage::Normal) || uint16_t(t.ColorSpace) > 1 ||
                uint16_t(t.AlphaMode) > 2 || uint16_t(t.Origin) > 1 || uint16_t(t.Filter) > 1 || uint16_t(t.Wrap) > 2)
                return false;
            if ((t.Kind == TextureKind::Texture2D && (t.Depth != 1 || t.Layers != 1)) ||
                (t.Kind == TextureKind::Array2D && t.Depth != 1) ||
                (t.Kind == TextureKind::Volume3D && t.Layers != 1) ||
                ((t.Kind == TextureKind::Cube || t.Kind == TextureKind::CubeArray) &&
                 (t.Width != t.Height || t.Depth != 1 || t.Layers % 6 ||
                  (t.Kind == TextureKind::Cube && t.Layers != 6))))
                return false;
            if (t.ColorSpace == TextureColorSpace::SRGB &&
                (t.Format != TexturePixelFormat::RGBA8 || t.Usage != TextureUsage::Color))
                return false;
            if (t.Usage == TextureUsage::Normal &&
                ((t.Format != TexturePixelFormat::RGBA8 && t.Format != TexturePixelFormat::RGBA32Float) ||
                 t.ColorSpace != TextureColorSpace::Linear))
                return false;
            if ((t.Format == TexturePixelFormat::R8 || t.Format == TexturePixelFormat::RG8) &&
                t.AlphaMode != TextureAlphaMode::Opaque)
                return false;
            return t.AlphaMode != TextureAlphaMode::Premultiplied || t.Usage == TextureUsage::Color;
        }
        float ReadFloat(const std::byte* bytes)
        {
            static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                          "Texture floats require IEEE binary32");
            uint32_t bits = 0;
            for (unsigned i = 0; i < 4; ++i)
                bits |= uint32_t(std::to_integer<uint8_t>(bytes[i])) << (i * 8);
            float value;
            std::memcpy(&value, &bits, 4);
            return value;
        }
        void WriteFloat(std::byte* bytes, float value)
        {
            uint32_t bits;
            std::memcpy(&bits, &value, 4);
            for (unsigned i = 0; i < 4; ++i)
                bytes[i] = static_cast<std::byte>((bits >> (i * 8)) & 255);
        }
        double Linear(double value)
        {
            return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        }
        double SRGB(double value)
        {
            return value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
        }
        const std::array<double, 256>& LinearByteTable()
        {
            // Immutable, thread-safe initialization avoids a power operation for
            // each source channel in every filter footprint.
            static const auto table = [] {
                std::array<double, 256> values {};
                for (size_t i = 0; i < values.size(); ++i)
                    values[i] = Linear(double(i) / 255.0);
                return values;
            }();
            return table;
        }
        std::array<double, 4> Pixel(const std::byte* bytes, const TextureData& t)
        {
            std::array<double, 4> out {0, 0, 0, 1};
            if (t.Format == TexturePixelFormat::RGBA32Float)
                for (unsigned i = 0; i < 4; ++i)
                    out[i] = ReadFloat(bytes + i * 4);
            else
                for (unsigned i = 0; i < Stride(t.Format); ++i)
                    out[i] = std::to_integer<uint8_t>(bytes[i]) / 255.0;
            if (t.ColorSpace == TextureColorSpace::SRGB)
                for (unsigned i = 0; i < 3; ++i)
                    out[i] = LinearByteTable()[std::to_integer<uint8_t>(bytes[i])];
            if (t.Usage == TextureUsage::Normal)
                for (unsigned i = 0; i < 3; ++i)
                    out[i] = out[i] * 2 - 1;
            return out;
        }
        void StorePixel(std::byte* bytes, std::array<double, 4> value, const TextureData& t)
        {
            if (t.ColorSpace == TextureColorSpace::SRGB)
                for (unsigned i = 0; i < 3; ++i)
                    value[i] = SRGB(value[i]);
            if (t.Format == TexturePixelFormat::RGBA32Float)
                for (unsigned i = 0; i < 4; ++i)
                    WriteFloat(bytes + i * 4, static_cast<float>(value[i]));
            else
                for (unsigned i = 0; i < Stride(t.Format); ++i)
                    bytes[i] = static_cast<std::byte>(std::lround(std::clamp(value[i], 0.0, 1.0) * 255));
        }
        TextureMipData FilterLevel(const TextureData&            out,
                                   const std::vector<std::byte>& previous,
                                   uint32_t                      pw,
                                   uint32_t                      ph,
                                   uint32_t                      pd,
                                   uint32_t                      w,
                                   uint32_t                      h,
                                   uint32_t                      d)
        {
            TextureMipData mip;
            const auto     stride = Stride(out.Format);
            mip.Pixels.resize(size_t(w) * h * d * out.Layers * stride);
            for (uint32_t layer = 0; layer < out.Layers; ++layer)
                for (uint32_t z = 0; z < d; ++z)
                    for (uint32_t y = 0; y < h; ++y)
                        for (uint32_t x = 0; x < w; ++x)
                        {
                            const double          x0 = double(x) * pw / w, x1 = double(x + 1) * pw / w;
                            const double          y0 = double(y) * ph / h, y1 = double(y + 1) * ph / h;
                            const double          z0 = double(z) * pd / d, z1 = double(z + 1) * pd / d;
                            std::array<double, 4> sum {}, plain {};
                            double                weights = 0, alphaWeights = 0;
                            for (uint32_t pz = static_cast<uint32_t>(z0); pz < static_cast<uint32_t>(std::ceil(z1));
                                 ++pz)
                                for (uint32_t py = static_cast<uint32_t>(y0); py < static_cast<uint32_t>(std::ceil(y1));
                                     ++py)
                                    for (uint32_t px = static_cast<uint32_t>(x0);
                                         px < static_cast<uint32_t>(std::ceil(x1));
                                         ++px)
                                    {
                                        const double weight =
                                            ((std::min)(x1, double(px + 1)) - (std::max)(x0, double(px))) *
                                            ((std::min)(y1, double(py + 1)) - (std::max)(y0, double(py))) *
                                            ((std::min)(z1, double(pz + 1)) - (std::max)(z0, double(pz)));
                                        const size_t index     = ((size_t(layer) * pd + pz) * ph + py) * pw + px;
                                        const auto   pixel     = Pixel(previous.data() + index * stride, out);
                                        const bool   associate = out.Usage == TextureUsage::Color &&
                                                                 out.AlphaMode == TextureAlphaMode::Straight;
                                        for (unsigned channel = 0; channel < 4; ++channel)
                                        {
                                            sum[channel] +=
                                                pixel[channel] * weight * ((associate && channel < 3) ? pixel[3] : 1);
                                            plain[channel] += pixel[channel] * weight;
                                        }
                                        weights += weight;
                                        alphaWeights += weight * pixel[3];
                                    }
                            for (unsigned channel = 0; channel < 4; ++channel)
                                sum[channel] /= weights;
                            if (out.Usage == TextureUsage::Color && out.AlphaMode == TextureAlphaMode::Straight)
                                for (unsigned channel = 0; channel < 3; ++channel)
                                    // A fully transparent block keeps its average color instead of black.
                                    sum[channel] = alphaWeights > 0 ? sum[channel] * weights / alphaWeights :
                                                                      plain[channel] / weights;
                            if (out.Usage == TextureUsage::Normal)
                            {
                                const double length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
                                for (unsigned channel = 0; channel < 3; ++channel)
                                    sum[channel] =
                                        ((length > 1e-12 ? sum[channel] / length : (channel == 2 ? 1.0 : 0.0)) + 1) *
                                        0.5;
                            }
                            const size_t index = ((size_t(layer) * d + z) * h + y) * w + x;
                            StorePixel(mip.Pixels.data() + index * stride, sum, out);
                        }
            return mip;
        }
    } // namespace

    std::variant<uint64_t, AssetError> TextureMipByteSize(const TextureData& texture, uint32_t level)
    {
        if (!ValidLayout(texture) || level >= FullLevels(texture))
            return AssetError {AssetErrorCode::InvalidData, "Invalid Texture layout/level"};
        uint64_t size = Stride(texture.Format);
        for (uint32_t count : {Extent(texture.Width, level),
                               Extent(texture.Height, level),
                               Extent(texture.Depth, level),
                               texture.Layers})
        {
            if (count > kMaximumPixels / size)
                return AssetError {AssetErrorCode::InvalidData, "Texture level exceeds byte limit"};
            size *= count;
        }
        return size;
    }

    std::variant<std::monostate, AssetError> ValidateTextureData(const TextureData& texture)
    {
        if (texture.RenderTexture)
            return ValidateRenderTexture(*texture.RenderTexture);
        if (!ValidLayout(texture) || texture.Mips.empty() || texture.Mips.size() > FullLevels(texture))
            return AssetError {AssetErrorCode::InvalidData, "Invalid Texture metadata/mip count"};
        uint64_t total = 0;
        for (uint32_t level = 0; level < texture.Mips.size(); ++level)
        {
            auto size = TextureMipByteSize(texture, level);
            if (const auto* error = std::get_if<AssetError>(&size))
                return *error;
            const auto& bytes = texture.Mips[level].Pixels;
            if (bytes.size() != std::get<uint64_t>(size) || bytes.size() > kMaximumPixels - total)
                return AssetError {AssetErrorCode::InvalidData, "Invalid Texture mip length/total byte limit"};
            total += bytes.size();
            if (texture.Format == TexturePixelFormat::RGBA32Float)
                for (size_t i = 0; i < bytes.size(); i += 4)
                {
                    const auto value = ReadFloat(bytes.data() + i);
                    if (!std::isfinite(value) ||
                        (texture.Usage == TextureUsage::Color && i % 16 == 12 && (value < 0 || value > 1)) ||
                        (texture.Usage == TextureUsage::Normal && i % 16 != 12 && (value < 0 || value > 1)))
                        return AssetError {AssetErrorCode::InvalidData, "Invalid Texture float/Color alpha"};
                }
        }
        return std::monostate {};
    }

    std::variant<TextureData, AssetError> GenerateTextureMips(const TextureData& texture)
    {
        if (texture.RenderTexture)
            return AssetError {AssetErrorCode::InvalidArgument, "RenderTexture mipmaps are generated on the GPU"};
        auto validated = ValidateTextureData(texture);
        if (const auto* error = std::get_if<AssetError>(&validated))
            return *error;
        if (texture.Kind == TextureKind::Cube || texture.Kind == TextureKind::CubeArray)
            return AssetError {AssetErrorCode::NotImplemented, "Cube mip generation requires seam-aware filtering"};
        // Plan the complete chain before copying/allocating pixels. Do not copy
        // supplied lower levels only to discard them during regeneration.
        uint64_t planned = 0;
        for (uint32_t level = 0; level < FullLevels(texture); ++level)
        {
            auto size = TextureMipByteSize(texture, level);
            if (const auto* error = std::get_if<AssetError>(&size))
                return *error;
            if (std::get<uint64_t>(size) > kMaximumPixels - planned)
                return AssetError {AssetErrorCode::InvalidData, "Generated Texture chain exceeds byte limit"};
            planned += std::get<uint64_t>(size);
        }
        TextureData out {texture.Kind,
                         texture.Format,
                         texture.ColorSpace,
                         texture.Usage,
                         texture.AlphaMode,
                         texture.Origin,
                         texture.Width,
                         texture.Height,
                         texture.Depth,
                         texture.Layers,
                         {texture.Mips.front()}};
        out.Filter = texture.Filter;
        out.Wrap   = texture.Wrap;
        for (uint32_t level = 1; level < FullLevels(out); ++level)
            out.Mips.push_back(FilterLevel(out,
                                           out.Mips.back().Pixels,
                                           Extent(out.Width, level - 1),
                                           Extent(out.Height, level - 1),
                                           Extent(out.Depth, level - 1),
                                           Extent(out.Width, level),
                                           Extent(out.Height, level),
                                           Extent(out.Depth, level)));
        auto valid = ValidateTextureData(out);
        if (const auto* error = std::get_if<AssetError>(&valid))
            return *error;
        return out;
    }

    std::variant<TextureData, AssetError> ResizeTexture(const TextureData& texture, uint32_t maxSize)
    {
        if (texture.RenderTexture)
            return AssetError {AssetErrorCode::InvalidArgument, "Resize the RenderTexture specification instead"};
        auto validated = ValidateTextureData(texture);
        if (auto* error = std::get_if<AssetError>(&validated))
            return *error;
        if (texture.Kind != TextureKind::Texture2D)
            return AssetError {AssetErrorCode::NotImplemented, "Resize requires a 2D texture"};
        if (!maxSize || std::max(texture.Width, texture.Height) <= maxSize)
            return texture;
        TextureData out     = texture;
        const auto  largest = std::max(texture.Width, texture.Height);
        out.Width           = std::max(1u, uint32_t((uint64_t(texture.Width) * maxSize + largest / 2) / largest));
        out.Height          = std::max(1u, uint32_t((uint64_t(texture.Height) * maxSize + largest / 2) / largest));
        out.Mips.clear();
        auto size = TextureMipByteSize(out, 0);
        if (auto* error = std::get_if<AssetError>(&size))
            return *error;
        out.Mips.push_back(
            FilterLevel(out, texture.Mips.front().Pixels, texture.Width, texture.Height, 1, out.Width, out.Height, 1));
        return out;
    }
} // namespace Kans

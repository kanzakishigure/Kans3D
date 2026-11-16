#include "kspch.h"
#include "TextureProduct.h"

namespace Kans
{
    namespace
    {
        constexpr size_t kHeader = 48, kMaximumBytes = 512 * 1024 * 1024;
        template<typename T>
        void Write(std::vector<std::byte>& out, T value)
        {
            for (size_t i = 0; i < sizeof(T); ++i)
                out.push_back(static_cast<std::byte>((value >> (i * 8)) & 255));
        }
        template<typename T>
        T Read(const std::byte*& cursor)
        {
            T value = 0;
            for (size_t i = 0; i < sizeof(T); ++i)
                value |= T(std::to_integer<uint8_t>(*cursor++)) << (i * 8);
            return value;
        }
    } // namespace

    std::variant<std::vector<std::byte>, AssetError> EncodeTextureProduct(const TextureData& texture)
    {
        auto valid = ValidateTextureData(texture);
        if (const auto* error = std::get_if<AssetError>(&valid))
            return *error;
        if (texture.RenderTexture)
        {
            auto text = SerializeRenderTexture(*texture.RenderTexture);
            if (auto* error = std::get_if<AssetError>(&text))
                return *error;
            const auto  source = std::string("KRTX") + std::get<std::string>(text);
            const auto* begin  = reinterpret_cast<const std::byte*>(source.data());
            return std::vector<std::byte>(begin, begin + source.size());
        }
        uint64_t payload = 0;
        for (const auto& mip : texture.Mips)
            payload += mip.Pixels.size();
        std::vector<std::byte> bytes;
        bytes.reserve(static_cast<size_t>(kHeader + payload));
        Write(bytes, uint32_t(0x5845544b)); // KTEX
        Write(bytes, uint16_t(2));
        Write(bytes, uint16_t(AssetType::Texture));
        Write(bytes, uint16_t(texture.Kind));
        Write(bytes, uint16_t(texture.Format));
        Write(bytes, uint16_t(texture.ColorSpace));
        Write(bytes, uint16_t(texture.Usage));
        Write(bytes, texture.Width);
        Write(bytes, texture.Height);
        Write(bytes, texture.Depth);
        Write(bytes, texture.Layers);
        Write(bytes, uint32_t(texture.Mips.size()));
        Write(bytes, uint16_t(texture.AlphaMode));
        // v2 packs sampler settings into the former origin field. Linear/repeat
        // leave every byte other than the version identical to v1.
        Write(bytes,
              uint16_t(uint16_t(texture.Origin) | (uint16_t(texture.Filter) << 1) | (uint16_t(texture.Wrap) << 2)));
        Write(bytes, payload);
        for (const auto& mip : texture.Mips)
            bytes.insert(bytes.end(), mip.Pixels.begin(), mip.Pixels.end());
        return bytes;
    }

    std::variant<TextureData, AssetError> DecodeTextureProduct(const std::byte* bytes, size_t size)
    {
        if (bytes && size > 4 && size <= 1024 * 1024 && std::memcmp(bytes, "KRTX", 4) == 0)
        {
            auto parsed = ParseRenderTexture(std::string(reinterpret_cast<const char*>(bytes + 4), size - 4));
            if (auto* error = std::get_if<AssetError>(&parsed))
                return *error;
            TextureData out;
            out.RenderTexture = std::get<RenderTextureSpecification>(parsed);
            out.Width         = out.RenderTexture->Width;
            out.Height        = out.RenderTexture->Height;
            out.Wrap          = TextureWrap::Clamp;
            out.ColorSpace    = out.RenderTexture->Format == RenderTextureFormat::RGBA8SRGB ? TextureColorSpace::SRGB :
                                                                                              TextureColorSpace::Linear;
            return out;
        }
        if (!bytes || size < kHeader || size > kMaximumBytes)
            return AssetError {AssetErrorCode::InvalidData, "Invalid Texture Product byte length"};
        auto cursor = bytes;
        if (Read<uint32_t>(cursor) != 0x5845544b)
            return AssetError {AssetErrorCode::InvalidData, "Invalid Texture Product magic"};
        const auto version = Read<uint16_t>(cursor);
        if (version != 1 && version != 2)
            return AssetError {AssetErrorCode::UnsupportedVersion, "Unsupported Texture Product version"};
        if (Read<uint16_t>(cursor) != uint16_t(AssetType::Texture))
            return AssetError {AssetErrorCode::InvalidData, "Invalid Texture Product type"};
        TextureData out;
        out.Kind           = TextureKind(Read<uint16_t>(cursor));
        out.Format         = TexturePixelFormat(Read<uint16_t>(cursor));
        out.ColorSpace     = TextureColorSpace(Read<uint16_t>(cursor));
        out.Usage          = TextureUsage(Read<uint16_t>(cursor));
        out.Width          = Read<uint32_t>(cursor);
        out.Height         = Read<uint32_t>(cursor);
        out.Depth          = Read<uint32_t>(cursor);
        out.Layers         = Read<uint32_t>(cursor);
        const auto levels  = Read<uint32_t>(cursor);
        out.AlphaMode      = TextureAlphaMode(Read<uint16_t>(cursor));
        const auto sampling = Read<uint16_t>(cursor);
        if ((version == 1 && sampling > 1) || (sampling & ~uint16_t(15)))
            return AssetError {AssetErrorCode::InvalidData, "Invalid Texture Product sampler flags"};
        out.Origin         = TextureOrigin(sampling & 1);
        out.Filter         = TextureFilter((sampling >> 1) & 1);
        out.Wrap           = TextureWrap(sampling >> 2);
        const auto payload = Read<uint64_t>(cursor);
        if (!levels || levels > 15 || payload != size - kHeader)
            return AssetError {AssetErrorCode::InvalidData, "Invalid Texture Product payload/mip count"};
        uint64_t            expected = 0;
        std::vector<size_t> sizes;
        for (uint32_t level = 0; level < levels; ++level)
        {
            auto count = TextureMipByteSize(out, level);
            if (const auto* error = std::get_if<AssetError>(&count))
                return *error;
            const auto value = std::get<uint64_t>(count);
            if (value > payload - expected)
                return AssetError {AssetErrorCode::InvalidData, "Truncated Texture Product mip payload"};
            expected += value;
            sizes.push_back(static_cast<size_t>(value));
        }
        if (expected != payload)
            return AssetError {AssetErrorCode::InvalidData, "Texture Product has trailing payload bytes"};
        // All layout/length checks precede pixel allocation.
        for (auto count : sizes)
        {
            out.Mips.push_back({std::vector<std::byte>(cursor, cursor + count)});
            cursor += count;
        }
        auto valid = ValidateTextureData(out);
        if (const auto* error = std::get_if<AssetError>(&valid))
            return *error;
        return out;
    }
} // namespace Kans

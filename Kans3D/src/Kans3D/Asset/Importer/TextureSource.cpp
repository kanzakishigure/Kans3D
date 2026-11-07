#include "kspch.h"
#include "TextureSource.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace
{
    constexpr size_t    DecoderBudget = size_t(1024) * 1024 * 1024;
    thread_local size_t decoderBytes  = 0;
    struct alignas(std::max_align_t) AllocationHeader
    {
        size_t Size;
    };
    void* ImageAllocate(size_t size)
    {
        if (size > DecoderBudget - decoderBytes ||
            size > (std::numeric_limits<size_t>::max)() - sizeof(AllocationHeader))
            return nullptr;
        auto* header = static_cast<AllocationHeader*>(std::malloc(sizeof(AllocationHeader) + size));
        if (!header)
            return nullptr;
        header->Size = size;
        decoderBytes += size;
        return header + 1;
    }
    void ImageFree(void* pointer)
    {
        if (!pointer)
            return;
        auto* header = static_cast<AllocationHeader*>(pointer) - 1;
        decoderBytes -= header->Size;
        std::free(header);
    }
    void* ImageResize(void* pointer, size_t size)
    {
        if (!pointer)
            return ImageAllocate(size);
        auto*        previous = static_cast<AllocationHeader*>(pointer) - 1;
        const size_t oldSize  = previous->Size;
        if (size > DecoderBudget - (decoderBytes - oldSize) ||
            size > (std::numeric_limits<size_t>::max)() - sizeof(AllocationHeader))
            return nullptr;
        auto* next = static_cast<AllocationHeader*>(std::realloc(previous, sizeof(AllocationHeader) + size));
        if (!next)
            return nullptr;
        next->Size   = size;
        decoderBytes = decoderBytes - oldSize + size;
        return next + 1;
    }
} // namespace

// Private linkage is essential: legacy callers mutate the public stb decoder.
// Do not call any stb configuration setters in this translation unit.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_HDR
#define STBI_MAX_DIMENSIONS 16384
#define STBI_MALLOC(size) ImageAllocate(size)
#define STBI_REALLOC_SIZED(pointer, oldSize, size) ImageResize(pointer, size)
#define STBI_FREE(pointer) ImageFree(pointer)
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <stb_image.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace Kans
{
    namespace
    {
        constexpr uint64_t SourceBudget = 256 * 1024 * 1024;
        constexpr uint64_t PixelBudget  = 512 * 1024 * 1024 - 48;

        uint32_t Big32(const unsigned char* data)
        {
            return uint32_t(data[0]) << 24 | uint32_t(data[1]) << 16 | uint32_t(data[2]) << 8 | data[3];
        }
        uint32_t PngCRC(const unsigned char* data, size_t size)
        {
            static const auto table = [] {
                std::array<uint32_t, 256> result {};
                for (uint32_t i = 0; i < result.size(); ++i)
                {
                    uint32_t value = i;
                    for (unsigned bit = 0; bit < 8; ++bit)
                        value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0u);
                    result[i] = value;
                }
                return result;
            }();
            uint32_t crc = 0xffffffffu;
            for (size_t i = 0; i < size; ++i)
                crc = table[(crc ^ data[i]) & 255] ^ (crc >> 8);
            return crc ^ 0xffffffffu;
        }

        std::variant<std::monostate, AssetError> CheckExif(const unsigned char* data, size_t size)
        {
            if (size < 8 || !((data[0] == 'I' && data[1] == 'I') || (data[0] == 'M' && data[1] == 'M')))
                return AssetError {AssetErrorCode::InvalidData, "Malformed image EXIF header"};
            const bool little = data[0] == 'I';
            const auto read16 = [&](size_t at) {
                return little ? uint16_t(data[at] | uint16_t(data[at + 1]) << 8) :
                                uint16_t(uint16_t(data[at]) << 8 | data[at + 1]);
            };
            const auto read32 = [&](size_t at) {
                return little ? uint32_t(read16(at)) | uint32_t(read16(at + 2)) << 16 :
                                uint32_t(read16(at)) << 16 | read16(at + 2);
            };
            if (read16(2) != 42)
                return AssetError {AssetErrorCode::InvalidData, "Malformed image EXIF signature"};
            const uint32_t offset = read32(4);
            if (offset > size - 2)
                return AssetError {AssetErrorCode::InvalidData, "Malformed image EXIF directory"};
            const size_t count = read16(offset);
            if (count > (size - offset - 2) / 12)
                return AssetError {AssetErrorCode::InvalidData, "Truncated image EXIF entries"};
            for (size_t i = 0; i < count; ++i)
            {
                const size_t at = offset + 2 + i * 12;
                if (read16(at) != 274)
                    continue;
                if (read16(at + 2) != 3 || read32(at + 4) != 1)
                    return AssetError {AssetErrorCode::InvalidData, "Malformed EXIF orientation"};
                if (read16(at + 8) != 1)
                    return AssetError {AssetErrorCode::NotImplemented,
                                       "EXIF rotation/reflection import is not supported"};
            }
            return std::monostate {};
        }

        std::variant<std::optional<TextureColorSpace>, AssetError>
        CheckMetadata(const unsigned char* bytes, size_t size, const TextureImportSettings& settings)
        {
            std::optional<TextureColorSpace> space;
            const unsigned char              signature[8] {137, 80, 78, 71, 13, 10, 26, 10};
            if (size >= 8 && std::memcmp(bytes, signature, 8) == 0)
            {
                size_t                  offset = 8;
                bool                    first = true, ended = false, srgb = false;
                std::optional<uint32_t> gamma;
                while (offset < size)
                {
                    if (size - offset < 12)
                        return AssetError {AssetErrorCode::InvalidData, "Truncated PNG chunk"};
                    const uint32_t length = Big32(bytes + offset);
                    if (length > size - offset - 12)
                        return AssetError {AssetErrorCode::InvalidData, "PNG chunk length exceeds source"};
                    const auto* type = bytes + offset + 4;
                    const auto* data = type + 4;
                    if (PngCRC(type, size_t(length) + 4) != Big32(data + length))
                        return AssetError {AssetErrorCode::InvalidData, "PNG chunk CRC mismatch"};
                    if (first && (std::memcmp(type, "IHDR", 4) != 0 || length != 13))
                        return AssetError {AssetErrorCode::InvalidData, "PNG must start with IHDR"};
                    first = false;
                    if (std::memcmp(type, "CgBI", 4) == 0 || std::memcmp(type, "acTL", 4) == 0)
                        return AssetError {AssetErrorCode::NotImplemented, "CgBI/animated PNG import is not supported"};
                    if (std::memcmp(type, "iCCP", 4) == 0 && settings.Usage == TextureUsage::Color)
                        return AssetError {AssetErrorCode::NotImplemented,
                                           "ICC color profile conversion is not supported"};
                    if (std::memcmp(type, "cICP", 4) == 0 && settings.Usage == TextureUsage::Color)
                        return AssetError {AssetErrorCode::NotImplemented, "PNG cICP conversion is not supported"};
                    if (std::memcmp(type, "cHRM", 4) == 0)
                    {
                        if (length != 32)
                            return AssetError {AssetErrorCode::InvalidData, "Invalid PNG chromaticity metadata"};
                        constexpr std::array<uint32_t, 8> standard {
                            31270, 32900, 64000, 33000, 30000, 60000, 15000, 6000};
                        // cHRM stores xy coordinates in units of 1/100000. Allow
                        // one unit for tools that round/truncate sRGB/Rec.709
                        // primaries and the D65 white point differently.
                        constexpr uint32_t tolerance = 1;
                        for (size_t i = 0; i < standard.size(); ++i)
                        {
                            const auto value      = Big32(data + i * 4);
                            const auto difference = value > standard[i] ? value - standard[i] : standard[i] - value;
                            if (settings.Usage == TextureUsage::Color && difference > tolerance)
                                return AssetError {AssetErrorCode::NotImplemented,
                                                   "Non-sRGB PNG primaries are not supported"};
                        }
                    }
                    if (std::memcmp(type, "eXIf", 4) == 0)
                    {
                        auto checked = CheckExif(data, length);
                        if (const auto* error = std::get_if<AssetError>(&checked))
                            return *error;
                    }
                    if (std::memcmp(type, "sRGB", 4) == 0)
                    {
                        if (length != 1 || data[0] > 3)
                            return AssetError {AssetErrorCode::InvalidData, "Invalid PNG sRGB metadata"};
                        srgb = true;
                    }
                    if (std::memcmp(type, "gAMA", 4) == 0)
                    {
                        if (length != 4 || Big32(data) == 0)
                            return AssetError {AssetErrorCode::InvalidData, "Invalid PNG gamma metadata"};
                        gamma = Big32(data);
                    }
                    offset += size_t(length) + 12;
                    if (std::memcmp(type, "IEND", 4) == 0)
                    {
                        if (length != 0 || offset != size)
                            return AssetError {AssetErrorCode::InvalidData, "Invalid PNG end/trailing bytes"};
                        ended = true;
                        break;
                    }
                }
                if (!ended)
                    return AssetError {AssetErrorCode::InvalidData, "Missing PNG end chunk"};
                if (srgb)
                    space = TextureColorSpace::SRGB;
                else if (gamma == 100000)
                    space = TextureColorSpace::Linear;
                else if (gamma == 45455)
                    space = TextureColorSpace::SRGB;
                else if (gamma && settings.Usage == TextureUsage::Color && !settings.ColorSpace)
                    return AssetError {AssetErrorCode::NotImplemented,
                                       "PNG gamma requires an explicit supported color_space"};
            }
            else if (size >= 2 && bytes[0] == 255 && bytes[1] == 216)
            {
                size_t at    = 2;
                bool   ended = false;
                while (at < size)
                {
                    if (bytes[at++] != 255)
                        return AssetError {AssetErrorCode::InvalidData, "Malformed JPEG marker"};
                    while (at < size && bytes[at] == 255)
                        ++at;
                    if (at == size)
                        return AssetError {AssetErrorCode::InvalidData, "Truncated JPEG marker"};
                    const unsigned marker = bytes[at++];
                    if (marker == 217)
                    {
                        if (at != size)
                            return AssetError {AssetErrorCode::InvalidData, "Trailing JPEG bytes"};
                        ended = true;
                        break;
                    }
                    if (marker == 216 || marker == 1 || (marker >= 208 && marker <= 215))
                        continue;
                    if (size - at < 2)
                        return AssetError {AssetErrorCode::InvalidData, "Truncated JPEG segment"};
                    const size_t length = size_t(bytes[at]) * 256 + bytes[at + 1];
                    if (length < 2 || length > size - at)
                        return AssetError {AssetErrorCode::InvalidData, "Invalid JPEG segment size"};
                    const auto*  data    = bytes + at + 2;
                    const size_t payload = length - 2;
                    if (marker == 226 && payload >= 12 && std::memcmp(data, "ICC_PROFILE\0", 12) == 0 &&
                        settings.Usage == TextureUsage::Color)
                        return AssetError {AssetErrorCode::NotImplemented,
                                           "ICC color profile conversion is not supported"};
                    if (marker == 225 && payload >= 6 && std::memcmp(data, "Exif\0\0", 6) == 0)
                    {
                        auto checked = CheckExif(data + 6, payload - 6);
                        if (const auto* error = std::get_if<AssetError>(&checked))
                            return *error;
                    }
                    if ((marker == 192 || marker == 193 || marker == 194) && payload >= 6 && data[5] == 4)
                        return AssetError {AssetErrorCode::NotImplemented, "CMYK/YCCK JPEG import is not supported"};
                    at += length;
                    if (marker == 218)
                    {
                        while (at < size)
                        {
                            if (bytes[at] != 255)
                            {
                                ++at;
                                continue;
                            }
                            const size_t markerStart = at++;
                            while (at < size && bytes[at] == 255)
                                ++at;
                            if (at == size)
                                break;
                            if (bytes[at] == 0 || (bytes[at] >= 208 && bytes[at] <= 215))
                            {
                                ++at;
                                continue;
                            }
                            at = markerStart;
                            break;
                        }
                    }
                }
                if (!ended)
                    return AssetError {AssetErrorCode::InvalidData, "Missing JPEG end marker"};
            }
            return space;
        }

        std::variant<std::monostate, AssetError>
        CheckRasterPayload(const unsigned char* bytes, size_t size, int width, int height, bool hdr)
        {
            const auto truncated = [] {
                return AssetError {AssetErrorCode::InvalidData, "Truncated or invalid raster payload"};
            };
            const auto little16 = [&](size_t at) { return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8; };
            const auto little32 = [&](size_t at) { return little16(at) | little16(at + 2) << 16; };
            if (size >= 2 && bytes[0] == 'B' && bytes[1] == 'M')
            {
                if (size < 26)
                    return truncated();
                const uint32_t offset = little32(10), header = little32(14);
                if (header != 12 && header != 40 && header != 56 && header != 108 && header != 124)
                    return AssetError {AssetErrorCode::NotImplemented, "Unsupported BMP header"};
                if (header > size - 14 || offset < header + 14 || offset > size)
                    return truncated();
                const uint32_t bits = little16(header == 12 ? 24 : 28);
                const uint64_t row  = (uint64_t(width) * bits + 31) / 32 * 4;
                if (!bits || row * uint64_t(height) > size - offset)
                    return truncated();
                if (bits < 16)
                {
                    if (bits != 1 && bits != 4 && bits != 8)
                        return truncated();
                    const uint32_t palette = (offset - header - 14) / (header == 12 ? 3 : 4);
                    if (!palette || palette > 256)
                        return truncated();
                    for (uint32_t y = 0; y < uint32_t(height); ++y)
                        for (uint32_t x = 0; x < uint32_t(width); ++x)
                        {
                            const unsigned packed = bytes[offset + y * row + uint64_t(x) * bits / 8];
                            const unsigned shift  = 8 - bits - ((x * bits) % 8);
                            if (((packed >> shift) & ((1u << bits) - 1)) >= palette)
                                return truncated();
                        }
                }
            }
            else if (hdr)
            {
                // stb supports modern scanline RLE and raw RGBE. Validate every
                // packet before decoding; its raw fallback otherwise accepts EOF.
                size_t at    = 0;
                bool   blank = false;
                while (at < size)
                {
                    const size_t start = at;
                    while (at < size && bytes[at] != '\n')
                        ++at;
                    if (at == size)
                        return truncated();
                    const size_t line = at++ - start;
                    if (blank)
                        break; // resolution line
                    blank = line == 0 || (line == 1 && bytes[start] == '\r');
                }
                if (!blank || at == size)
                    return truncated();
                if (width < 8 || width >= 32768 || size - at < 4 || bytes[at] != 2 || bytes[at + 1] != 2 ||
                    (bytes[at + 2] & 128))
                {
                    if (uint64_t(width) * height * 4 > size - at)
                        return truncated();
                }
                else
                    for (int y = 0; y < height; ++y)
                    {
                        if (size - at < 4 || bytes[at] != 2 || bytes[at + 1] != 2 ||
                            uint32_t(bytes[at + 2]) * 256 + bytes[at + 3] != uint32_t(width))
                            return truncated();
                        at += 4;
                        for (unsigned channel = 0; channel < 4; ++channel)
                        {
                            uint32_t produced = 0;
                            while (produced < uint32_t(width))
                            {
                                if (at == size)
                                    return truncated();
                                const unsigned packet   = bytes[at++];
                                const unsigned count    = packet > 128 ? packet - 128 : packet;
                                const unsigned consumed = packet > 128 ? 1 : count;
                                if (!count || count > uint32_t(width) - produced || consumed > size - at)
                                    return truncated();
                                produced += count;
                                at += consumed;
                            }
                        }
                    }
            }
            else if (!(size >= 8 && bytes[0] == 137 && bytes[1] == 'P') &&
                     !(size >= 2 && bytes[0] == 255 && bytes[1] == 216))
            {
                // TGA is the remaining enabled decoder (no reliable magic).
                if (size < 18)
                    return truncated();
                if (bytes[17] & 0xd0)
                    return AssetError {AssetErrorCode::NotImplemented, "Right-origin/interleaved TGA is not supported"};
                const unsigned type = bytes[2], bits = bytes[16];
                if (!bits || bits > 32)
                    return truncated();
                if ((bits == 15 || bits == 16) && (bytes[17] & 15) && type != 3 && type != 11)
                    return AssetError {AssetErrorCode::NotImplemented, "Packed TGA alpha import is not supported"};
                size_t at = 18 + bytes[0];
                if (at > size)
                    return truncated();
                if (bytes[1])
                {
                    if (little16(3) != 0)
                        return AssetError {AssetErrorCode::NotImplemented,
                                           "Nonzero TGA palette origin is not supported"};
                    if (!little16(5))
                        return truncated();
                    const size_t palette = little16(5) * ((bytes[7] + 7) / 8);
                    if (palette > size - at)
                        return truncated();
                    at += palette;
                }
                const unsigned stride       = (bits + 7) / 8;
                const auto     validIndices = [&](size_t offset, size_t count) {
                    if (!bytes[1])
                        return true;
                    for (size_t i = 0; i < count; ++i)
                    {
                        const auto index = bits == 8 ? uint32_t(bytes[offset + i]) : little16(offset + i * stride);
                        if (index >= little16(5))
                            return false;
                    }
                    return true;
                };
                uint64_t remaining = uint64_t(width) * height;
                if (type < 8)
                {
                    if (remaining * stride > size - at)
                        return truncated();
                    if (!validIndices(at, static_cast<size_t>(remaining)))
                        return truncated();
                }
                else
                    while (remaining)
                    {
                        if (at == size)
                            return truncated();
                        const unsigned packet   = bytes[at++];
                        const unsigned count    = (packet & 127) + 1;
                        const unsigned consumed = (packet & 128) ? stride : count * stride;
                        if (count > remaining || consumed > size - at)
                            return truncated();
                        if (!validIndices(at, (packet & 128) ? 1 : count))
                            return truncated();
                        at += consumed;
                        remaining -= count;
                    }
            }
            return std::monostate {};
        }

        float ToLinear(float value)
        {
            return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
        }
        float ToSRGB(float value)
        {
            return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
        }
        void AppendFloat(std::vector<std::byte>& bytes, float value)
        {
            static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                          "Texture source floats require IEEE binary32");
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            for (unsigned shift = 0; shift < 32; shift += 8)
                bytes.push_back(static_cast<std::byte>((bits >> shift) & 255));
        }
    } // namespace

    std::variant<SourceInventory, AssetError> InspectTextureSource(const std::filesystem::path& file)
    {
        if (!file.is_absolute())
            return AssetError {AssetErrorCode::InvalidArgument, "Inspection requires an absolute source path"};
        auto read = ReadProductFile(file, SourceBudget);
        if (auto* error = std::get_if<AssetError>(&read))
            return *error;
        const auto& bytes = std::get<std::vector<std::byte>>(read);
        if (bytes.empty())
            return AssetError {AssetErrorCode::InvalidData, "Texture source is empty"};
        const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
        int         w = 0, h = 0, c = 0;
        if (!stbi_info_from_memory(data, int(bytes.size()), &w, &h, &c) || w <= 0 || h <= 0 || w > 16384 || h > 16384)
            return AssetError {AssetErrorCode::InvalidData, "Unsupported or malformed Texture source"};
        const bool       hdr = stbi_is_hdr_from_memory(data, int(bytes.size())) != 0;
        InspectedTexture texture;
        texture.Width    = w;
        texture.Height   = h;
        texture.Channels = c;
        texture.BitDepth = hdr ? 32 : stbi_is_16_bit_from_memory(data, int(bytes.size())) ? 16 : 8;
        texture.HasAlpha = c == 2 || c == 4;
        texture.Format   = bytes.size() >= 8 && data[0] == 137 && data[1] == 'P' ? "PNG" :
                           bytes.size() >= 2 && data[0] == 255 && data[1] == 216 ? "JPEG" :
                           bytes.size() >= 2 && data[0] == 'B' && data[1] == 'M' ? "BMP" :
                           hdr                                                   ? "HDR" :
                                                                                   "TGA";
        std::string name = file.stem().u8string();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        texture.LikelyNormalMap    = name.find("normal") != std::string::npos ||
                                     (name.size() >= 2 && name.compare(name.size() - 2, 2, "_n") == 0) ||
                                     (name.size() >= 4 && name.compare(name.size() - 4, 4, "_nrm") == 0);
        texture.NormalMapHeuristic = "Filename contains normal or ends with _n / _nrm (case-insensitive).";
        SourceInventory result;
        result.Texture = std::move(texture);
        return result;
    }

    std::variant<TextureData, AssetError> ParseTextureSource(const std::filesystem::path& file,
                                                             const TextureImportSettings& settings)
    {
        if (const auto checked = ValidateTextureImportSettings(settings); std::holds_alternative<AssetError>(checked))
            return std::get<AssetError>(checked);
        if (!file.is_absolute())
            return AssetError {AssetErrorCode::InvalidArgument, "Texture source path must be absolute"};
        std::error_code error;
        const auto      status = std::filesystem::status(file, error);
        if (status.type() == std::filesystem::file_type::not_found)
            return AssetError {AssetErrorCode::NotFound, "Texture source not found: " + file.u8string()};
        if (error)
            return AssetError {AssetErrorCode::IoError, "Cannot inspect Texture source: " + error.message()};
        if (!std::filesystem::is_regular_file(status))
            return AssetError {AssetErrorCode::InvalidArgument, "Texture source must be a regular file"};
        auto read = ReadProductFile(file, SourceBudget);
        if (const auto* failure = std::get_if<AssetError>(&read))
            return *failure;
        const auto& owned = std::get<std::vector<std::byte>>(read);
        if (owned.empty())
            return AssetError {AssetErrorCode::InvalidData, "Texture source is empty"};
        const auto* bytes    = reinterpret_cast<const unsigned char*>(owned.data());
        const int   length   = static_cast<int>(owned.size());
        auto        metadata = CheckMetadata(bytes, owned.size(), settings);
        if (const auto* failure = std::get_if<AssetError>(&metadata))
            return *failure;
        int width = 0, height = 0, channels = 0;
        if (!stbi_info_from_memory(bytes, length, &width, &height, &channels) || width <= 0 || height <= 0)
            return AssetError {AssetErrorCode::InvalidData, "Unsupported or malformed Texture source"};
        const bool hdr  = stbi_is_hdr_from_memory(bytes, length) != 0;
        const bool wide = !hdr && stbi_is_16_bit_from_memory(bytes, length) != 0;
        if (const auto checked = CheckRasterPayload(bytes, owned.size(), width, height, hdr);
            std::holds_alternative<AssetError>(checked))
            return std::get<AssetError>(checked);
        if (hdr && settings.ColorSpace == TextureColorSpace::SRGB)
            return AssetError {AssetErrorCode::InvalidArgument, "HDR source must use linear color_space"};
        const auto sourceSpace = settings.ColorSpace.value_or(
            hdr || settings.Usage != TextureUsage::Color ?
                TextureColorSpace::Linear :
                std::get<std::optional<TextureColorSpace>>(metadata).value_or(TextureColorSpace::SRGB));
        TextureData result;
        result.Width      = static_cast<uint32_t>(width);
        result.Height     = static_cast<uint32_t>(height);
        result.Format     = hdr || wide ? TexturePixelFormat::RGBA32Float : TexturePixelFormat::RGBA8;
        result.ColorSpace = hdr || wide ? TextureColorSpace::Linear : sourceSpace;
        result.Usage      = settings.Usage;
        result.Filter     = settings.Filter;
        result.Wrap       = settings.Wrap;
        result.AlphaMode  = settings.AlphaMode.value_or(channels == 2 || channels == 4 ? TextureAlphaMode::Straight :
                                                                                        TextureAlphaMode::Opaque);
        result.Origin     = settings.FlipY ? TextureOrigin::BottomLeft : TextureOrigin::TopLeft;
        auto baseSize     = TextureMipByteSize(result, 0);
        if (const auto* failure = std::get_if<AssetError>(&baseSize))
            return *failure;
        uint64_t total = 0;
        for (uint32_t level = 0;; ++level)
        {
            auto size = TextureMipByteSize(result, level);
            if (const auto* failure = std::get_if<AssetError>(&size))
                return *failure;
            const uint64_t count = std::get<uint64_t>(size);
            if (count > PixelBudget - total)
                return AssetError {AssetErrorCode::InvalidData, "Generated Texture chain exceeds pixel budget"};
            total += count;
            if (!settings.GenerateMips || ((result.Width >> level) <= 1 && (result.Height >> level) <= 1))
                break;
        }
        struct DecodedPixels
        {
            void* Value;
            ~DecodedPixels() { stbi_image_free(Value); }
        } decoded {hdr  ? static_cast<void*>(stbi_loadf_from_memory(bytes, length, &width, &height, &channels, 4)) :
                   wide ? static_cast<void*>(stbi_load_16_from_memory(bytes, length, &width, &height, &channels, 4)) :
                          static_cast<void*>(stbi_load_from_memory(bytes, length, &width, &height, &channels, 4))};
        if (!decoded.Value)
        {
            const char* reason = stbi_failure_reason();
            return AssetError {AssetErrorCode::InvalidData,
                               file.u8string() +
                                   ": Texture decode failed: " + (reason ? reason : "unknown decoder error")};
        }
        if (width != static_cast<int>(result.Width) || height != static_cast<int>(result.Height))
            return AssetError {AssetErrorCode::InvalidData, "Texture decode dimensions changed"};
        result.Mips.resize(1);
        auto& pixels = result.Mips[0].Pixels;
        pixels.reserve(static_cast<size_t>(std::get<uint64_t>(baseSize)));
        for (uint32_t y = 0; y < result.Height; ++y)
            for (uint32_t x = 0; x < result.Width; ++x)
            {
                const size_t at = (size_t(settings.FlipY ? result.Height - y - 1 : y) * result.Width + x) * 4;
                float        values[4];
                for (unsigned c = 0; c < 4; ++c)
                    values[c] = hdr  ? static_cast<const float*>(decoded.Value)[at + c] :
                                wide ? static_cast<const stbi_us*>(decoded.Value)[at + c] / 65535.0f :
                                       static_cast<const stbi_uc*>(decoded.Value)[at + c] / 255.0f;
                if (settings.NormalFlipY)
                    values[1] = 1.0f - values[1];
                if (sourceSpace == TextureColorSpace::SRGB &&
                    (wide || result.AlphaMode == TextureAlphaMode::Premultiplied))
                    for (unsigned c = 0; c < 3; ++c)
                        values[c] = ToLinear(values[c]);
                if (result.AlphaMode == TextureAlphaMode::Premultiplied)
                    for (unsigned c = 0; c < 3; ++c)
                        values[c] *= values[3];
                if (!hdr && !wide && sourceSpace == TextureColorSpace::SRGB &&
                    result.AlphaMode == TextureAlphaMode::Premultiplied)
                    for (unsigned c = 0; c < 3; ++c)
                        values[c] = ToSRGB(values[c]);
                for (float value : values)
                {
                    if (hdr || wide)
                        AppendFloat(pixels, value);
                    else
                        pixels.push_back(static_cast<std::byte>(
                            static_cast<unsigned>((std::clamp)(std::round(value * 255.0f), 0.0f, 255.0f))));
                }
            }
        if (const auto checked = ValidateTextureData(result); std::holds_alternative<AssetError>(checked))
            return std::get<AssetError>(checked);
        if (settings.MaxSize && std::max(result.Width, result.Height) > settings.MaxSize)
        {
            auto resized = ResizeTexture(result, settings.MaxSize);
            if (auto* failure = std::get_if<AssetError>(&resized))
                return *failure;
            result = std::get<TextureData>(std::move(resized));
        }
        if (settings.GenerateMips)
            return GenerateTextureMips(result);
        return result;
    }
} // namespace Kans

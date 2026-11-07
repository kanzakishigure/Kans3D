#include "kspch.h"
#include "TextureFile.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"

// Private decoder: display loading must not inherit or change another caller's stb flip/gamma settings.
// Keep all stb formats enabled for display loading.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS 16384
#include <stb_image.h>

namespace Kans
{
    std::variant<TextureData, AssetError> DecodeTextureFileForDisplay(const std::filesystem::path& absolutePath,
                                                                      TextureColorSpace            colorSpace)
    {
        auto read = ReadProductFile(absolutePath, 256 * 1024 * 1024);
        if (auto* error = std::get_if<AssetError>(&read))
            return *error;
        const auto& file = std::get<std::vector<std::byte>>(read);
        if (file.empty())
            return AssetError {AssetErrorCode::InvalidData, "Texture source is empty"};
        const auto* source = reinterpret_cast<const stbi_uc*>(file.data());
        const int   length = static_cast<int>(file.size());
        int         width = 0, height = 0, channels = 0;
        if (!stbi_info_from_memory(source, length, &width, &height, &channels) || width <= 0 || height <= 0)
            return AssetError {AssetErrorCode::InvalidData, "Unsupported or malformed texture source"};
        const bool  hdr = stbi_is_hdr_from_memory(source, length) != 0;
        TextureData data;
        data.Width      = static_cast<uint32_t>(width);
        data.Height     = static_cast<uint32_t>(height);
        data.Format     = hdr ? TexturePixelFormat::RGBA32Float : TexturePixelFormat::RGBA8;
        data.ColorSpace = hdr ? TextureColorSpace::Linear : colorSpace;
        data.AlphaMode  = channels == 2 || channels == 4 ? TextureAlphaMode::Straight : TextureAlphaMode::Opaque;
        data.Origin     = TextureOrigin::TopLeft;
        auto size       = TextureMipByteSize(data, 0);
        if (auto* error = std::get_if<AssetError>(&size))
            return *error;
        const auto byteCount = std::get<uint64_t>(size);
        if (byteCount > 512 * 1024 * 1024)
            return AssetError {AssetErrorCode::InvalidData, "Texture source exceeds display pixel budget"};
        struct Pixels
        {
            void* Data;
            ~Pixels() { stbi_image_free(Data); }
        } pixels {hdr ? static_cast<void*>(stbi_loadf_from_memory(source, length, &width, &height, &channels, 4)) :
                        static_cast<void*>(stbi_load_from_memory(source, length, &width, &height, &channels, 4))};
        if (!pixels.Data)
        {
            const char* reason = stbi_failure_reason();
            return AssetError {AssetErrorCode::InvalidData, reason ? reason : "Texture decode failed"};
        }
        if (uint32_t(width) != data.Width || uint32_t(height) != data.Height)
            return AssetError {AssetErrorCode::InvalidData, "Texture decode dimensions changed"};
        data.Mips.resize(1);
        auto& bytes = data.Mips[0].Pixels;
        bytes.resize(size_t(byteCount));
        if (!hdr)
            std::memcpy(bytes.data(), pixels.Data, bytes.size());
        else
            for (size_t at = 0; at < bytes.size(); at += 4)
            {
                uint32_t word;
                std::memcpy(&word, static_cast<const float*>(pixels.Data) + at / 4, 4);
                for (unsigned b = 0; b < 4; ++b)
                    bytes[at + b] = static_cast<std::byte>(word >> (8 * b));
            }
        return data;
    }
} // namespace Kans

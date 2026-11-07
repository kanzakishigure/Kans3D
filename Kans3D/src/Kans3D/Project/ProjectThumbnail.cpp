#include "kspch.h"
#include "ProjectThumbnail.h"
#include <cmath>
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "Project.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../../vendor/GLFW/deps/stb_image_write.h"

namespace Kans
{
    namespace
    {
        std::filesystem::path ThumbnailPath(const ProjectConfig& config)
        {
            return config.RootDirectory / config.CacheDirectory / "Thumbnail.png";
        }
    } // namespace
    std::optional<std::filesystem::path> FindProjectThumbnail(const ProjectConfig& config)
    {
        std::error_code error;
        auto            path = ThumbnailPath(config);
        return std::filesystem::is_regular_file(path, error) ? std::optional<std::filesystem::path>(path) :
                                                               std::nullopt;
    }
    std::optional<AssetError> WriteProjectThumbnail(const ProjectConfig& config,
                                                    const uint8_t*       pixels,
                                                    size_t               pixelBytes,
                                                    uint32_t             width,
                                                    uint32_t             height)
    {
        if (!pixels || !width || !height || uint64_t(width) * height > SIZE_MAX / 4 ||
            uint64_t(width) * height * 4 > pixelBytes)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid thumbnail RGBA8 buffer"};
        const double         scale = std::min({1.0, 512.0 / width, 288.0 / height});
        const auto           w     = std::max(1u, uint32_t(std::lround(width * scale)));
        const auto           h     = std::max(1u, uint32_t(std::lround(height * scale)));
        std::vector<uint8_t> resized(size_t(w) * h * 4);
        // Bilinear pixel-center sampling. Integer rounding changes aspect by at most one pixel.
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const double sx = std::clamp((x + 0.5) * width / w - 0.5, 0.0, double(width - 1));
                const double sy = std::clamp((y + 0.5) * height / h - 0.5, 0.0, double(height - 1));
                const auto   x0 = uint32_t(sx), x1 = std::min(x0 + 1, width - 1);
                const auto   y0 = uint32_t(sy), y1 = std::min(y0 + 1, height - 1);
                for (unsigned c = 0; c < 4; ++c)
                {
                    const auto sample = [&](uint32_t px, uint32_t py) {
                        return pixels[(size_t(py) * width + px) * 4 + c];
                    };
                    const double top    = sample(x0, y0) * (1 - (sx - x0)) + sample(x1, y0) * (sx - x0);
                    const double bottom = sample(x0, y1) * (1 - (sx - x0)) + sample(x1, y1) * (sx - x0);
                    resized[(size_t(y) * w + x) * 4 + c] =
                        uint8_t(std::lround(top * (1 - (sy - y0)) + bottom * (sy - y0)));
                }
            }
        std::vector<std::byte> png;
        const auto             append = [](void* context, void* bytes, int count) {
            auto& output = *static_cast<std::vector<std::byte>*>(context);
            auto* begin  = static_cast<std::byte*>(bytes);
            output.insert(output.end(), begin, begin + count);
        };
        if (!stbi_write_png_to_func(append, &png, int(w), int(h), 4, resized.data(), int(w * 4)))
            return AssetError {AssetErrorCode::IoError, "Cannot encode project thumbnail PNG"};
        const auto      path = ThumbnailPath(config);
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            return AssetError {AssetErrorCode::IoError, error.message()};
        auto temporary = path;
        temporary += "." + SourceAssetID::Generate().ToString() + ".tmp";
        auto written = WriteProductFile(temporary, png);
        if (auto* failure = std::get_if<AssetError>(&written))
            return *failure;
        struct Cleanup
        {
            std::filesystem::path Path;
            ~Cleanup()
            {
                std::error_code ignored;
                std::filesystem::remove(Path, ignored);
            }
        } cleanup {temporary};
        auto replaced = ReplaceProductCatalog(temporary, path);
        if (auto* failure = std::get_if<AssetError>(&replaced))
            return *failure;
        return {};
    }
} // namespace Kans

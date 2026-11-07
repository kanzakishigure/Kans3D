#include "KansEditorCore/Panels/ContentBrowser/ThumbnailCache.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "Kans3D/Asset/Importer/TextureSource.h"
#include "KansEditorCore/Panels/ContentBrowser/BrowserModel.h"

namespace Kans::Browser
{
    namespace
    {
        constexpr size_t Capacity       = 512; // Thumbnails kept on the GPU.
        constexpr size_t UploadsPerFrame = 4;
    } // namespace

    ThumbnailCache::ThumbnailCache() : m_Worker([this] { Work(); }) {}

    ThumbnailCache::~ThumbnailCache()
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Stop = true;
        }
        m_Wake.notify_all();
        m_Worker.join();
    }

    bool ThumbnailCache::Supports(const fs::path& file)
    {
        const auto extension = Lowercase(file.extension().u8string());
        return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" ||
               extension == ".tga" || extension == ".hdr";
    }

    Ref<Texture2D> ThumbnailCache::Get(const fs::path& file, fs::file_time_type modified, uint32_t size)
    {
        const std::string key = file.u8string() + "#" + std::to_string(size);
        auto [it, added]      = m_Entries.try_emplace(key);
        auto& entry           = it->second;
        if (added)
            entry.Use = m_Recent.insert(m_Recent.begin(), key);
        else
            m_Recent.splice(m_Recent.begin(), m_Recent, entry.Use);
        const bool stale = added || entry.Modified != modified;
        if (stale && !entry.Pending)
        {
            entry.Modified = modified;
            entry.Pending  = true;
            entry.Failed   = false;
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_Requests.push_front({file, modified, size, key});
            }
            m_Wake.notify_one();
        }
        return entry.Failed ? nullptr : entry.Texture;
    }

    void ThumbnailCache::Update()
    {
        std::vector<Decoded> finished;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            const size_t count = std::min(UploadsPerFrame, m_Finished.size());
            finished.assign(std::make_move_iterator(m_Finished.begin()),
                            std::make_move_iterator(m_Finished.begin() + count));
            m_Finished.erase(m_Finished.begin(), m_Finished.begin() + count);
        }
        for (auto& decoded : finished)
        {
            const auto found = m_Entries.find(decoded.Key);
            if (found == m_Entries.end())
                continue;
            auto& entry   = found->second;
            entry.Pending = false;
            if (entry.Modified != decoded.Modified)
                continue; // Changed again; the next Get re-queues it.
            if (decoded.Pixels.empty())
            {
                entry.Failed = true;
                continue;
            }
            TextureData image;
            image.Width      = decoded.Width;
            image.Height     = decoded.Height;
            image.Format     = TexturePixelFormat::RGBA8;
            image.ColorSpace = TextureColorSpace::Linear; // Show stored values as they are.
            image.Mips       = {{std::move(decoded.Pixels)}};
            auto uploaded    = Texture2D::CreateFromProduct(image);
            if (auto* texture = std::get_if<Ref<Texture2D>>(&uploaded))
                entry.Texture = *texture;
            else
                entry.Failed = true;
        }
        // Forget the least recently used thumbnails.
        while (m_Entries.size() > Capacity)
        {
            const auto key = m_Recent.back();
            m_Recent.pop_back();
            m_Entries.erase(key);
        }
    }

    void ThumbnailCache::Work()
    {
        while (true)
        {
            Request request;
            {
                std::unique_lock<std::mutex> lock(m_Mutex);
                m_Wake.wait(lock, [&] { return m_Stop || !m_Requests.empty(); });
                if (m_Stop)
                    return;
                request = std::move(m_Requests.front());
                m_Requests.pop_front();
            }
            auto decoded = Decode(request);
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Finished.push_back(std::move(decoded));
        }
    }

    ThumbnailCache::Decoded ThumbnailCache::Decode(const Request& request)
    {
        Decoded result;
        result.Key      = request.Key;
        result.Modified = request.Modified;
        TextureImportSettings settings;
        settings.GenerateMips = false;
        auto parsed           = ParseTextureSource(request.File, settings);
        if (!std::holds_alternative<TextureData>(parsed))
            return result;
        const auto& image = std::get<TextureData>(parsed);
        if (!image.Width || !image.Height || image.Mips.empty())
            return result;
        const bool floating = image.Format == TexturePixelFormat::RGBA32Float;
        const auto& pixels  = image.Mips[0].Pixels;
        // Source texel as 8-bit RGBA; float images are tone mapped and gamma encoded.
        const auto texel = [&](uint32_t x, uint32_t y, float out[4]) {
            const size_t index = (size_t(y) * image.Width + x) * 4;
            for (int c = 0; c < 4; ++c)
            {
                if (!floating)
                {
                    out[c] = std::to_integer<uint8_t>(pixels[index + c]);
                    continue;
                }
                float value = 0;
                std::memcpy(&value, pixels.data() + (index + c) * 4, 4);
                if (c < 3)
                    value = std::pow(std::max(value, 0.0f) / (1.0f + std::max(value, 0.0f)), 1.0f / 2.2f);
                out[c] = std::clamp(value, 0.0f, 1.0f) * 255.0f;
            }
        };
        const float scale = std::min(1.0f, float(request.Size) / float(std::max(image.Width, image.Height)));
        result.Width      = std::max(1u, uint32_t(std::lround(image.Width * scale)));
        result.Height     = std::max(1u, uint32_t(std::lround(image.Height * scale)));
        result.Pixels.resize(size_t(result.Width) * result.Height * 4);
        // Box filter: average the source texels each thumbnail texel covers.
        for (uint32_t y = 0; y < result.Height; ++y)
        {
            const uint32_t y0 = uint32_t(uint64_t(y) * image.Height / result.Height);
            const uint32_t y1 = std::max(y0 + 1, uint32_t(uint64_t(y + 1) * image.Height / result.Height));
            for (uint32_t x = 0; x < result.Width; ++x)
            {
                const uint32_t x0 = uint32_t(uint64_t(x) * image.Width / result.Width);
                const uint32_t x1 = std::max(x0 + 1, uint32_t(uint64_t(x + 1) * image.Width / result.Width));
                float          sum[4] = {};
                for (uint32_t sy = y0; sy < y1; ++sy)
                    for (uint32_t sx = x0; sx < x1; ++sx)
                    {
                        float value[4];
                        texel(sx, sy, value);
                        for (int c = 0; c < 4; ++c)
                            sum[c] += value[c];
                    }
                const float count = float((y1 - y0) * (x1 - x0));
                for (int c = 0; c < 4; ++c)
                    result.Pixels[(size_t(y) * result.Width + x) * 4 + c] =
                        std::byte(uint8_t(std::lround(sum[c] / count)));
            }
        }
        return result;
    }
} // namespace Kans::Browser

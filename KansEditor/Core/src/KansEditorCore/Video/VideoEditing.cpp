#include "kspch.h"
#include "KansEditorCore/Video/VideoEditing.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"
#include "KansEditorCore/EditorAssetImportService.h"

namespace Kans::VideoEditing
{
    namespace
    {
        std::optional<bool> DecodingOverride;

        std::string Trimmed(double value)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%.2f", value);
            std::string result = text;
            while (!result.empty() && result.back() == '0')
                result.pop_back();
            if (!result.empty() && result.back() == '.')
                result.pop_back();
            return result;
        }
    } // namespace

    double FrameRate(const VideoClipInfo& info)
    {
        return info.RateNumerator && info.RateDenominator ? double(info.RateNumerator) / info.RateDenominator : 0.0;
    }

    std::string CodecName(std::string_view codec)
    {
        std::string lower(codec);
        std::transform(
            lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        static const std::pair<const char*, const char*> names[] {{"h264", "H.264"},
                                                                  {"hevc", "H.265"},
                                                                  {"h265", "H.265"},
                                                                  {"vp8", "VP8"},
                                                                  {"vp9", "VP9"},
                                                                  {"av1", "AV1"},
                                                                  {"mpeg4", "MPEG-4"},
                                                                  {"mpeg2video", "MPEG-2"},
                                                                  {"prores", "ProRes"}};
        for (const auto& [id, name] : names)
            if (lower == id)
                return name;
        std::string upper(codec);
        std::transform(
            upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        return upper;
    }

    std::string FormatClock(double seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0)
            seconds = 0;
        // Tenths, rounded once so 59.96 s reads 1:00.0 rather than 0:60.0.
        const auto tenths = static_cast<long long>(std::llround(seconds * 10.0));
        const auto hours = tenths / 36000, minutes = tenths / 600 % 60, whole = tenths / 10 % 60, tenth = tenths % 10;
        char       text[48];
        if (hours)
            std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld.%lld", hours, minutes, whole, tenth);
        else
            std::snprintf(text, sizeof(text), "%lld:%02lld.%lld", minutes, whole, tenth);
        return text;
    }

    std::string FormatRate(double fps) { return Trimmed(fps) + " fps"; }

    std::string MetadataLine(const VideoClipInfo& info)
    {
        // U+00D7 and U+00B7 are in the editor font's Latin-1 range.
        std::string line = std::to_string(info.Width) + "\xc3\x97" + std::to_string(info.Height);
        const char* dot  = " \xc2\xb7 ";
        if (const double fps = FrameRate(info); fps > 0)
            line += dot + FormatRate(fps);
        line += dot + FormatClock(info.Duration);
        if (!info.Codec.empty())
            line += dot + CodecName(info.Codec);
        if (info.HasAudio)
            line += std::string(dot) + "has audio (not played)";
        return line;
    }

    double OffsetRate(const VideoClipInfo* info)
    {
        const double fps = info ? FrameRate(*info) : 0.0;
        return fps > 0 ? fps : 30.0;
    }

    double OffsetFrames(double seconds, double fps, bool whole)
    {
        const double frames = seconds * fps;
        return whole ? std::round(frames) : frames;
    }

    double FramesToSeconds(double frames, double fps) { return fps > 0 ? frames / fps : 0.0; }

    double StepFrames(double seconds, int frames, double fps) { return seconds + FramesToSeconds(frames, fps); }

    bool IsVideoTarget(const RenderTextureSpecification& specification)
    {
        return specification.Update == RenderTextureUpdate::Video;
    }

    RenderTextureSpecification VideoTargetSpecification(const VideoClipInfo& info)
    {
        RenderTextureSpecification specification;
        specification.Material   = {};
        specification.Width      = std::max(1u, info.Width);
        specification.Height     = std::max(1u, info.Height);
        specification.Format     = RenderTextureFormat::RGBA8SRGB;
        specification.Mips       = false;
        specification.Update     = RenderTextureUpdate::Video;
        specification.Rate       = 0;
        specification.ClearColor = {0, 0, 0, 1};
        return specification;
    }

    std::variant<std::filesystem::path, AssetError>
    CreateRenderTextureFile(const std::filesystem::path&      directory,
                            const std::string&                stem,
                            const RenderTextureSpecification& specification)
    {
        auto text = SerializeRenderTexture(specification);
        if (auto* error = std::get_if<AssetError>(&text))
            return std::move(*error);
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        auto file = directory / std::filesystem::u8path(stem + ".rendertexture");
        for (int i = 2; std::filesystem::exists(file, error); ++i)
            file = directory / std::filesystem::u8path(stem + std::to_string(i) + ".rendertexture");
        std::ofstream output(file, std::ios::binary);
        output << std::get<std::string>(text);
        output.close();
        if (!output)
            return AssetError {AssetErrorCode::IoError, "Cannot write " + file.filename().u8string()};
        return file;
    }

    std::optional<RenderTextureUpdate> RenderTextureIndex::UpdateOf(const std::filesystem::path& file)
    {
        std::error_code error;
        if (file.extension() != ".rendertexture")
            return std::nullopt;
        const auto written = std::filesystem::last_write_time(file, error);
        if (error)
            return std::nullopt;
        auto& entry = m_Files[file.u8string()];
        if (entry.Written != written || entry.Written == std::filesystem::file_time_type {})
        {
            entry.Written = written;
            entry.Update.reset();
            std::ifstream      input(file, std::ios::binary);
            std::ostringstream text;
            text << input.rdbuf();
            const auto parsed = ParseRenderTexture(text.str());
            if (const auto* specification = std::get_if<RenderTextureSpecification>(&parsed))
                entry.Update = specification->Update;
        }
        return entry.Update;
    }

    std::vector<std::string> PlayersTargeting(const Scene& scene, AssetID texture)
    {
        std::vector<std::string> names;
        if (!texture.IsValid())
            return names;
        const std::function<void(Entity)> visit = [&](Entity e) {
            if (e.HasComponent<VideoPlayerComponent>() && e.GetComponent<VideoPlayerComponent>().Target == texture)
                names.push_back(e.GetComponent<TagComponent>().Tag);
            for (Entity child : scene.GetChildren(e))
                visit(child);
        };
        for (Entity root : scene.GetRootEntities())
            visit(root);
        return names;
    }

    bool PendingTargets::Assign(Scene& scene, const std::function<AssetID(const std::filesystem::path&)>& published)
    {
        bool assigned = false;
        m_Pending.erase(std::remove_if(m_Pending.begin(),
                                       m_Pending.end(),
                                       [&](const std::pair<std::filesystem::path, UUID>& pending) {
                                           Entity player = scene.GetEntityByUUID(pending.second);
                                           if (!player || !player.HasComponent<VideoPlayerComponent>())
                                               return true;
                                           const AssetID target = published(pending.first);
                                           if (!target.IsValid())
                                               return false;
                                           player.PatchComponent<VideoPlayerComponent>(
                                               [&](VideoPlayerComponent& c) { c.Target = target; });
                                           assigned = true;
                                           return true;
                                       }),
                        m_Pending.end());
        return assigned;
    }

    bool DecodingAvailable() { return DecodingOverride.value_or(IsVideoDecodingAvailable()); }

    void OverrideDecodingAvailable(std::optional<bool> available) { DecodingOverride = available; }

    std::string PlayerError(const Scene* scene, Entity player)
    {
        if (!DecodingAvailable())
            return VideoDecodingUnavailable;
        return scene && player ? scene->GetVideoPlayerError(player) : std::string {};
    }

    std::vector<std::pair<std::string, std::string>> StatsRows(const VideoPlayerStats& stats)
    {
        char decoded[32], upload[32];
        std::snprintf(decoded, sizeof(decoded), "%.1f fps", stats.DecodedFPS);
        std::snprintf(upload, sizeof(upload), "%.2f ms", stats.UploadMilliseconds);
        return {{"Players", std::to_string(stats.ActivePlayers)},
                {"Decoded", decoded},
                {"Dropped frames", std::to_string(stats.DroppedFrames)},
                {"Ring fill", std::to_string(stats.RingFill) + " / " + std::to_string(stats.RingCapacity)},
                {"Upload", upload}};
    }

    const ClipSummary* ClipInfoCache::Find(const Ref<const AssetRegistry>& registry,
                                           const std::filesystem::path&    productDirectory,
                                           SourceAssetID                   source)
    {
        if (!registry)
            return nullptr;
        if (registry != m_Indexed)
        {
            m_Indexed = registry;
            m_Records.clear();
            for (auto& record : registry->GetAllProducts())
                if (record.Target == EditorAssetTarget && record.Metadata.Type == AssetType::VideoClip)
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
            std::vector<std::byte> bytes(size_t(std::min<uint64_t>(VideoProductInfoBytes, product.Location.ByteSize)));
            std::ifstream          file(productDirectory / product.Location.RelativeFile.GetPath(), std::ios::binary);
            file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
            if (file && size_t(file.gcount()) == bytes.size())
                if (auto  decoded = DecodeVideoProductInfo(bytes.data(), bytes.size());
                    auto* info    = std::get_if<VideoClipInfo>(&decoded))
                    found->second = ClipSummary {product.Metadata.assetID, std::move(*info)};
        }
        return found->second ? &*found->second : nullptr;
    }

    void ClipInfoCache::Clear()
    {
        m_Indexed.reset();
        m_Records.clear();
        m_Summaries.clear();
    }
} // namespace Kans::VideoEditing

namespace Kans
{
    void VideoPreviewSession::Begin(Scene& scene, Entity player, double seconds)
    {
        if (!player || !player.HasComponent<VideoPlayerComponent>())
            return;
        if (!IsPreviewing(player))
        {
            End();
            m_Scene  = &scene;
            m_Player = player.GetUUID();
        }
        m_Playing = false;
        SetTime(seconds);
    }

    void VideoPreviewSession::SetTime(double seconds)
    {
        if (!std::isfinite(seconds))
            return;
        const double duration = Duration();
        m_Time                = std::max(0.0, duration > 0 ? std::min(seconds, duration) : seconds);
    }

    void VideoPreviewSession::Play()
    {
        if (!m_Scene)
            return;
        // Playing from the end starts over.
        if (const double duration = Duration(); duration > 0 && m_Time >= duration)
            m_Time = 0;
        m_Playing = true;
    }

    void VideoPreviewSession::Pause() { m_Playing = false; }

    void VideoPreviewSession::End()
    {
        if (m_Scene)
            if (Entity player = Player())
                m_Scene->EndVideoPreview(player);
        m_Scene   = nullptr;
        m_Player  = 0;
        m_Time    = 0;
        m_Playing = false;
    }

    bool VideoPreviewSession::IsPreviewing(Entity player) const
    {
        return m_Scene && player && player.GetUUID() == m_Player && m_Scene->GetEntityByUUID(m_Player) == player;
    }

    bool VideoPreviewSession::Update(double seconds)
    {
        Entity player = Player();
        if (!player)
        {
            // Destroyed, or its Video Player removed: the scene dropped its playback already.
            m_Scene   = nullptr;
            m_Player  = 0;
            m_Playing = false;
            return false;
        }
        const auto&  component = player.GetComponent<VideoPlayerComponent>();
        const double duration  = Duration();
        if (m_Playing && std::isfinite(seconds) && duration > 0)
        {
            m_Time += seconds * std::max(0.0, component.Speed);
            if (m_Time >= duration)
            {
                if (component.Loop)
                    m_Time = std::fmod(m_Time, duration);
                else
                    m_Time = duration, m_Playing = false;
            }
        }
        return m_Scene->PreviewVideo(player, m_Time);
    }

    void VideoPreviewSession::Sync(const Scene* scene, Entity selected, bool playing)
    {
        if (!m_Scene)
            return;
        if (scene != m_Scene)
        {
            // The scene was closed or replaced; its players went with it.
            m_Scene   = nullptr;
            m_Player  = 0;
            m_Playing = false;
            return;
        }
        if (playing || !IsPreviewing(selected))
            End();
    }

    double VideoPreviewSession::Duration() const
    {
        Entity player = Player();
        if (!player)
            return 0;
        auto*      assets = m_Scene->GetAssetManager();
        const auto clip =
            assets ? assets->GetLoaded<VideoClipAsset>(player.GetComponent<VideoPlayerComponent>().Clip) : nullptr;
        return clip ? clip->GetData().Info.Duration : 0;
    }

    Entity VideoPreviewSession::Player() const
    {
        if (!m_Scene)
            return {};
        Entity player = m_Scene->GetEntityByUUID(m_Player);
        return player && player.HasComponent<VideoPlayerComponent>() ? player : Entity {};
    }
} // namespace Kans

#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
#include "Kans3D/Asset/AssetError.h"
#include "Kans3D/Asset/AssetRegistry.h"
#include "Kans3D/Asset/Runtime/RenderTexture.h"
#include "Kans3D/Scene/Entity.h"
#include "Kans3D/Video/VideoClip.h"
#include "Kans3D/Video/VideoPlayer.h"

namespace Kans
{
    class Scene;
}

// Editor-side video helpers without UI: what the Video Player card, the Content Browser, the Render
// Texture panel and the Stats panel show, the files they create and the edit-mode preview.
namespace Kans::VideoEditing
{
    // Frames per second of the clip's stream; 0 when it has no rate.
    double FrameRate(const VideoClipInfo& info);
    // A codec's display name: h264 -> H.264, hevc -> H.265, vp9 -> VP9; others in capitals.
    std::string CodecName(std::string_view codec);
    // m:ss.d, or h:mm:ss.d from an hour on (3:50.7).
    std::string FormatClock(double seconds);
    // "24 fps", "29.97 fps".
    std::string FormatRate(double fps);
    // "1920×1080 · 24 fps · 3:50.7 · H.264", with " · has audio (not played)" when the file has sound.
    std::string MetadataLine(const VideoClipInfo& info);

    // The rate offsets are shown at: the clip's, or 30 fps while it is unknown.
    double OffsetRate(const VideoClipInfo* info);
    // An offset in frames at `fps` (rounded to whole frames with `whole`), and back.
    double OffsetFrames(double seconds, double fps, bool whole = false);
    double FramesToSeconds(double frames, double fps);
    // The offset `frames` frames later (earlier when negative), keeping its fraction of a frame.
    double StepFrames(double seconds, int frames, double fps);

    // ---- Render texture targets ----
    bool IsVideoTarget(const RenderTextureSpecification& specification);
    // A Video render texture the size of the clip: no material, no mips, opaque black until a frame arrives.
    RenderTextureSpecification VideoTargetSpecification(const VideoClipInfo& info);
    // Writes `specification` to a new <stem>.rendertexture in `directory` (numbered when taken); the editor
    // imports it like any saved source.
    std::variant<std::filesystem::path, AssetError>
    CreateRenderTextureFile(const std::filesystem::path&      directory,
                            const std::string&                stem,
                            const RenderTextureSpecification& specification);

    // Whether .rendertexture sources are Video targets, read once per file version.
    class RenderTextureIndex
    {
    public:
        // The file's update mode; none when it is not a readable .rendertexture.
        std::optional<RenderTextureUpdate> UpdateOf(const std::filesystem::path& file);
        bool IsVideoTarget(const std::filesystem::path& file) { return UpdateOf(file) == RenderTextureUpdate::Video; }

    private:
        struct Entry
        {
            std::filesystem::file_time_type    Written {};
            std::optional<RenderTextureUpdate> Update;
        };
        std::unordered_map<std::string, Entry> m_Files;
    };

    // The names of the scene's Video Players whose target is `texture`, in hierarchy order.
    std::vector<std::string> PlayersTargeting(const Scene& scene, AssetID texture);

    // Targets written by "Create target", assigned to their players once the build publishes them.
    class PendingTargets
    {
    public:
        void Add(std::filesystem::path file, UUID player) { m_Pending.push_back({std::move(file), player}); }
        // Assigns each published file (`published` gives its Texture, invalid while it builds) to its player and
        // drops it; files of players that are gone are dropped too. True when a player was assigned.
        bool Assign(Scene& scene, const std::function<AssetID(const std::filesystem::path&)>& published);
        bool Empty() const { return m_Pending.empty(); }
        void Clear() { m_Pending.clear(); }

    private:
        std::vector<std::pair<std::filesystem::path, UUID>> m_Pending;
    };

    // ---- Errors and stats ----
    // Whether decoding works in this build; tests may pretend it does not.
    bool DecodingAvailable();
    void OverrideDecodingAvailable(std::optional<bool> available);
    // What the card's Error row shows: the unavailable message, or the player's own error.
    std::string PlayerError(const Scene* scene, Entity player);
    // The Stats panel's Video rows: players, decoded fps, dropped frames, ring fill, upload time.
    std::vector<std::pair<std::string, std::string>> StatsRows(const VideoPlayerStats& stats);

    // ---- Content Browser ----
    struct ClipSummary
    {
        AssetID       ID;
        VideoClipInfo Info;
    };
    // The published editor VideoClip of a source, read from the first bytes of its Product (the stream stays
    // on disk) once per Product content: a reimport reads again.
    class ClipInfoCache
    {
    public:
        const ClipSummary* Find(const Ref<const AssetRegistry>& registry,
                                const std::filesystem::path&    productDirectory,
                                SourceAssetID                   source);
        void               Clear();

    private:
        Ref<const AssetRegistry>                                    m_Indexed;
        std::unordered_map<SourceAssetID, ProductRecord>            m_Records;
        std::unordered_map<std::string, std::optional<ClipSummary>> m_Summaries;
    };
} // namespace Kans::VideoEditing

namespace Kans
{
    // The Video Player card's edit-mode preview: one player at a time shows the clip at the preview time in its
    // target (and so in the viewport), while playing or paused, until it ends. Update requests the frame each
    // editor frame; Sync ends the preview when its scene goes away, the player is deselected or Play starts.
    class VideoPreviewSession
    {
    public:
        // The owner ends the preview (End) before the scene goes away; destruction does not touch it.
        VideoPreviewSession()                                      = default;
        ~VideoPreviewSession()                                     = default;
        VideoPreviewSession(const VideoPreviewSession&)            = delete;
        VideoPreviewSession& operator=(const VideoPreviewSession&) = delete;

        // Previews `player` (ending another one's preview) from `seconds`, paused.
        void   Begin(Scene& scene, Entity player, double seconds = 0);
        void   SetTime(double seconds);
        void   Play();
        void   Pause();
        void   End();
        bool   IsActive() const { return m_Scene != nullptr; }
        bool   IsPreviewing(Entity player) const;
        bool   IsPlaying() const { return m_Scene && m_Playing; }
        double Time() const { return m_Time; }
        // Advances a playing preview by `seconds` at the player's speed (wrapping when it loops, pausing at the
        // end otherwise) and requests the frame; true while the scene accepts it (false while the clip loads).
        bool Update(double seconds);
        // `selected` is the editor's selection; `playing` is true in Play or Simulate.
        void Sync(const Scene* scene, Entity selected, bool playing);

    private:
        double Duration() const;
        Entity Player() const;

        Scene* m_Scene   = nullptr;
        UUID   m_Player  = 0;
        double m_Time    = 0;
        bool   m_Playing = false;
    };
} // namespace Kans

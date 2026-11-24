#pragma once
#include <array>
#include <functional>
#include "VideoClip.h"

namespace Kans
{
    struct VideoFrame
    {
        double         Time = 0, End = 0;
        uint64_t       Serial    = 0;
        VideoMatrix    Matrix    = VideoMatrix::BT709;
        VideoRange     Range     = VideoRange::Limited;
        VideoTransfer  Transfer  = VideoTransfer::BT709;
        VideoPrimaries Primaries = VideoPrimaries::BT709;
        // Tight Y rows followed by interleaved UV rows. Both use width bytes per row.
        std::vector<uint8_t> Planes;
    };

    struct VideoColorConstants
    {
        std::array<float, 4> Range;  // Y offset, Y scale, UV offset, UV scale.
        std::array<float, 4> Matrix; // R from Cr, G from Cb/Cr, B from Cb.
    };

    VideoColorConstants                 GetVideoColorConstants(VideoMatrix, VideoRange);
    std::array<std::array<float, 4>, 3> GetVideoGamutConstants(VideoPrimaries);

    struct VideoDecoderStats
    {
        uint64_t Decoded = 0, Dropped = 0;
        unsigned RingFill   = 0;
        double   DecodedFPS = 0;
    };

    class VideoDecoder
    {
    public:
        static constexpr unsigned RingSize = 4;
        // The worker calls this before decoding a frame. Used by tests to control decode progress.
        using DecodeHook = std::function<void()>;

        explicit VideoDecoder(Ref<const VideoClipAsset>, DecodeHook = {});
        ~VideoDecoder();
        VideoDecoder(const VideoDecoder&)            = delete;
        VideoDecoder& operator=(const VideoDecoder&) = delete;

        // Main-thread polling. Returns the last presented frame, including while a seek is pending.
        // The returned pointer stays valid until the next call or destruction.
        const VideoFrame* Sample(double seconds, bool seek = false);
        VideoDecoderStats Stats() const;
        std::string       Error() const;
        static unsigned   ActiveWorkers();

    private:
        struct State;
        std::unique_ptr<State> m_State;
        bool                   m_SeekPending = false;
    };
} // namespace Kans

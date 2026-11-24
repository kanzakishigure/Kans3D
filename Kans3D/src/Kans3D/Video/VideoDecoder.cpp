#include "kspch.h"
#include "VideoDecoder.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

#if KANS_HAS_FFMPEG
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
}
#endif

namespace Kans
{
    namespace
    {
        std::atomic<unsigned> Workers {0};

#if KANS_HAS_FFMPEG
        constexpr unsigned MaximumDimension = 8192;

        bool SupportedFormat(AVPixelFormat format)
        {
            return format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_NV12;
        }

        bool SupportedMatrix(AVColorSpace space)
        {
            return space == AVCOL_SPC_UNSPECIFIED || space == AVCOL_SPC_BT709 || space == AVCOL_SPC_SMPTE170M ||
                   space == AVCOL_SPC_BT470BG;
        }

        bool SupportedTransfer(AVColorTransferCharacteristic transfer)
        {
            return transfer == AVCOL_TRC_UNSPECIFIED || transfer == AVCOL_TRC_BT709 ||
                   transfer == AVCOL_TRC_SMPTE170M || transfer == AVCOL_TRC_IEC61966_2_1 ||
                   transfer == AVCOL_TRC_GAMMA22 || transfer == AVCOL_TRC_GAMMA28;
        }

        bool SupportedPrimaries(AVColorPrimaries color)
        {
            return color == AVCOL_PRI_UNSPECIFIED || color == AVCOL_PRI_BT709 || color == AVCOL_PRI_SMPTE170M ||
                   color == AVCOL_PRI_BT470BG;
        }

        VideoPrimaries Primaries(AVColorPrimaries color)
        {
            if (color == AVCOL_PRI_SMPTE170M)
                return VideoPrimaries::SMPTE170M;
            if (color == AVCOL_PRI_BT470BG)
                return VideoPrimaries::BT470BG;
            return VideoPrimaries::BT709;
        }

        VideoMatrix Matrix(AVColorSpace space, int height)
        {
            if (space == AVCOL_SPC_UNSPECIFIED)
                return height > 576 ? VideoMatrix::BT709 : VideoMatrix::BT601;
            return space == AVCOL_SPC_BT709 ? VideoMatrix::BT709 : VideoMatrix::BT601;
        }

        VideoTransfer Transfer(AVColorTransferCharacteristic transfer)
        {
            switch (transfer)
            {
                case AVCOL_TRC_IEC61966_2_1:
                    return VideoTransfer::SRGB;
                case AVCOL_TRC_GAMMA22:
                    return VideoTransfer::Gamma22;
                case AVCOL_TRC_GAMMA28:
                    return VideoTransfer::Gamma28;
                default:
                    return VideoTransfer::BT709;
            }
        }

        // Custom byte IO confines demuxing to the packaged product and supports seeking without temp files.
        class Input
        {
        public:
            explicit Input(const VideoBytes& bytes, const std::atomic<bool>* stopping = nullptr) :
                Bytes(bytes), Stopping(stopping)
            {
                auto* buffer = static_cast<unsigned char*>(av_malloc(32 * 1024));
                if (!buffer)
                    return;
                IO = avio_alloc_context(buffer, 32 * 1024, 0, this, Read, nullptr, Seek);
                if (!IO)
                {
                    av_free(buffer);
                    return;
                }
                Format = avformat_alloc_context();
                if (!Format)
                    return;
                Format->pb = IO;
                Format->flags |= AVFMT_FLAG_CUSTOM_IO;
                Format->io_open = [](AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**) {
                    return AVERROR(EPERM);
                };
                Format->interrupt_callback = {Interrupt, this};
                AVDictionary* options      = nullptr;
                // MOV reference movies and other external resources are outside the product.
                av_dict_set(&options, "enable_drefs", "0", 0);
                av_dict_set(&options, "protocol_whitelist", "", 0);
                const int opened = avformat_open_input(&Format, nullptr, nullptr, &options);
                av_dict_free(&options);
                if (opened < 0)
                    return;
                if (avformat_find_stream_info(Format, nullptr) < 0)
                    return;
                const std::string_view demuxer = Format->iformat->name;
                if (demuxer != "mov,mp4,m4a,3gp,3g2,mj2" && demuxer != "matroska,webm")
                {
                    Error = "Unsupported video container";
                    return;
                }
                for (unsigned i = 0; i < Format->nb_streams; ++i)
                {
                    const auto* stream = Format->streams[i];
                    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                        !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
                    {
                        Index = int(i);
                        break;
                    }
                }
                Error = Index < 0 ? "No video stream" : "";
            }

            ~Input()
            {
                avformat_close_input(&Format);
                if (IO)
                    av_freep(&IO->buffer);
                avio_context_free(&IO);
            }
            Input(const Input&)            = delete;
            Input& operator=(const Input&) = delete;

            AVStream*        Stream() const { return Format->streams[Index]; }
            AVFormatContext* Format = nullptr;
            int              Index  = -1;
            std::string      Error  = "Cannot open or probe video container";

        private:
            static int Interrupt(void* opaque)
            {
                const auto& input = *static_cast<Input*>(opaque);
                return input.Stopping && input.Stopping->load(std::memory_order_relaxed);
            }
            static int Read(void* opaque, uint8_t* output, int count)
            {
                auto& input = *static_cast<Input*>(opaque);
                if (Interrupt(opaque))
                    return AVERROR_EXIT;
                const auto available = std::min<size_t>(count, input.Bytes.size() - input.Position);
                if (!available)
                    return AVERROR_EOF;
                std::memcpy(output, input.Bytes.data() + input.Position, available);
                input.Position += available;
                return int(available);
            }
            static int64_t Seek(void* opaque, int64_t offset, int whence)
            {
                auto&      input = *static_cast<Input*>(opaque);
                const auto size  = int64_t(input.Bytes.size());
                whence &= ~AVSEEK_FORCE;
                if (whence == AVSEEK_SIZE)
                    return size;
                if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
                    return AVERROR(EINVAL);
                const int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? int64_t(input.Position) : size;
                if (offset < -base || offset > size - base)
                    return AVERROR(EINVAL);
                input.Position = size_t(base + offset);
                return int64_t(input.Position);
            }

            const VideoBytes&        Bytes;
            const std::atomic<bool>* Stopping;
            size_t                   Position = 0;
            AVIOContext*             IO       = nullptr;
        };

        // Software decoders. Audio packets are discarded here. A future audio
        // generator will need a separate audio demux/decode path outside the audio callback.
        const AVCodec* FindDecoder(AVCodecID id)
        {
            switch (id)
            {
                case AV_CODEC_ID_H264:
                case AV_CODEC_ID_HEVC:
                case AV_CODEC_ID_MPEG4:
                    return avcodec_find_decoder_by_name(avcodec_get_name(id));
                case AV_CODEC_ID_VP8:
                case AV_CODEC_ID_VP9:
                    if (const auto* native = avcodec_find_decoder_by_name(avcodec_get_name(id)))
                        return native;
                    return avcodec_find_decoder_by_name(id == AV_CODEC_ID_VP9 ? "libvpx-vp9" : "libvpx");
                case AV_CODEC_ID_AV1:
                    if (const auto* dav1d = avcodec_find_decoder_by_name("libdav1d"))
                        return dav1d;
                    return avcodec_find_decoder_by_name("libaom-av1");
                default:
                    return nullptr;
            }
        }

        struct FreeCodec
        {
            void operator()(AVCodecContext* p) const { avcodec_free_context(&p); }
        };
        using CodecContext = std::unique_ptr<AVCodecContext, FreeCodec>;

        CodecContext OpenCodec(const AVCodecParameters& parameters)
        {
            const auto* decoder     = FindDecoder(parameters.codec_id);
            const auto  unavailable = [&] {
                return std::string("Unsupported or unavailable video codec: ") + avcodec_get_name(parameters.codec_id);
            };
            if (!decoder)
                throw std::runtime_error(unavailable());
            CodecContext codec(avcodec_alloc_context3(decoder));
            if (!codec || avcodec_parameters_to_context(codec.get(), &parameters) < 0)
                throw std::runtime_error("Cannot initialize video decoder");
            codec->thread_count = 2;
            codec->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
            codec->max_pixels   = int64_t(MaximumDimension) * MaximumDimension;
            codec->get_format   = [](AVCodecContext*, const AVPixelFormat* formats) {
                for (; *formats != AV_PIX_FMT_NONE; ++formats)
                    if (SupportedFormat(*formats))
                        return *formats;
                return AV_PIX_FMT_NONE;
            };
            const int opened = avcodec_open2(codec.get(), decoder, nullptr);
            if (opened < 0)
            {
                char reason[AV_ERROR_MAX_STRING_SIZE] {};
                av_strerror(opened, reason, sizeof(reason));
                const auto prefix = opened == AVERROR(EPERM) || opened == AVERROR(ENOSYS) ?
                                        unavailable() :
                                        std::string("Cannot open video decoder");
                throw std::runtime_error(prefix + " (" + reason + ")");
            }
            return codec;
        }

        class Reader
        {
        public:
            Reader(const VideoClipData& clip, const std::atomic<bool>& stopping) :
                Source(*clip.Encoded, &stopping), Info(clip.Info), Stopping(stopping)
            {
                if (!Source.Error.empty())
                    throw std::runtime_error(Source.Error);
                Codec = OpenCodec(*Source.Stream()->codecpar);
                if (!Frame || !Packet)
                    throw std::runtime_error("Cannot allocate video decoder buffers");
                Origin = Source.Stream()->start_time == AV_NOPTS_VALUE ? 0 : Source.Stream()->start_time;
            }

            bool CanSkipAhead(double decoded, double target) const
            {
                const auto* entry =
                    avformat_index_get_entry_from_timestamp(Source.Stream(), Timestamp(target), AVSEEK_FLAG_BACKWARD);
                return entry && (entry->flags & AVINDEX_KEYFRAME) && entry->timestamp > Timestamp(decoded);
            }

            void Seek(double time)
            {
                const auto timestamp = Timestamp(time);
                if (av_seek_frame(Source.Format, Source.Index, timestamp, AVSEEK_FLAG_BACKWARD) < 0 &&
                    av_seek_frame(Source.Format, Source.Index, Origin, AVSEEK_FLAG_BACKWARD) < 0)
                    throw std::runtime_error("Cannot seek video");
                avcodec_flush_buffers(Codec.get());
                av_packet_unref(Packet.get());
                Draining = false;
            }

            bool Next(VideoFrame& output)
            {
                while (!Stopping.load(std::memory_order_relaxed))
                {
                    const int received = avcodec_receive_frame(Codec.get(), Frame.get());
                    if (received == 0)
                    {
                        Copy(output);
                        return true;
                    }
                    if (received == AVERROR_EOF)
                        return false;
                    if (received != AVERROR(EAGAIN) || Draining)
                        throw std::runtime_error("Video frame decode failed");
                    // The prior receive drained the decoder. The next input can be submitted immediately.
                    int result;
                    do
                    {
                        av_packet_unref(Packet.get());
                        result = av_read_frame(Source.Format, Packet.get());
                    } while (result >= 0 && Packet->stream_index != Source.Index &&
                             !Stopping.load(std::memory_order_relaxed));
                    if (Stopping.load(std::memory_order_relaxed))
                        return false;
                    if (result < 0 && result != AVERROR_EOF)
                        throw std::runtime_error("Video packet read failed");
                    Draining = result == AVERROR_EOF;
                    if (avcodec_send_packet(Codec.get(), Draining ? nullptr : Packet.get()) < 0)
                        throw std::runtime_error("Video packet decode failed");
                }
                return false;
            }

        private:
            int64_t Timestamp(double time) const
            {
                const auto timeBase = av_q2d(Source.Stream()->time_base);
                const auto ticks    = std::floor(time / timeBase);
                // Use a strict upper bound before converting to signed ticks.
                const auto maxTicks = int64_t(std::numeric_limits<int64_t>::max() / 2);
                const auto delta    = ticks < double(maxTicks) && ticks >= 0 ? int64_t(ticks) : maxTicks;
                return Origin > std::numeric_limits<int64_t>::max() - delta ? std::numeric_limits<int64_t>::max() :
                                                                              Origin + delta;
            }

            void Copy(VideoFrame& output)
            {
                const auto& f      = *Frame;
                const auto  format = AVPixelFormat(f.format);
                if (!SupportedFormat(format) || !SupportedMatrix(f.colorspace) || !SupportedTransfer(f.color_trc) ||
                    !SupportedPrimaries(f.color_primaries) || f.width != int(Info.Width) ||
                    f.height != int(Info.Height))
                    throw std::runtime_error("Unsupported video frame format or mid-stream format change");
                if (f.best_effort_timestamp == AV_NOPTS_VALUE)
                    throw std::runtime_error("Video frame has no presentation timestamp");
                output.Time   = (double(f.best_effort_timestamp) - double(Origin)) * av_q2d(Source.Stream()->time_base);
                output.Matrix = f.colorspace == AVCOL_SPC_UNSPECIFIED ? Info.Matrix : Matrix(f.colorspace, f.height);
                output.Range  = format == AV_PIX_FMT_YUVJ420P || f.color_range == AVCOL_RANGE_JPEG ? VideoRange::Full :
                                f.color_range == AVCOL_RANGE_MPEG ? VideoRange::Limited :
                                                                    Info.Range;
                output.Transfer = f.color_trc == AVCOL_TRC_UNSPECIFIED ? Info.Transfer : Transfer(f.color_trc);
                output.Primaries =
                    f.color_primaries == AVCOL_PRI_UNSPECIFIED ? Info.Primaries : Primaries(f.color_primaries);
                const size_t width = Info.Width, height = Info.Height;
                auto*        y  = output.Planes.data();
                auto*        uv = y + width * height;
                for (size_t row = 0; row < height; ++row)
                    std::memcpy(y + row * width, f.data[0] + ptrdiff_t(row) * f.linesize[0], width);
                for (size_t row = 0; row < height / 2; ++row)
                {
                    const auto* u = f.data[1] + ptrdiff_t(row) * f.linesize[1];
                    if (format == AV_PIX_FMT_NV12)
                        std::memcpy(uv + row * width, u, width);
                    else
                    {
                        const auto* v = f.data[2] + ptrdiff_t(row) * f.linesize[2];
                        for (size_t col = 0; col < width / 2; ++col)
                        {
                            uv[row * width + 2 * col]     = u[col];
                            uv[row * width + 2 * col + 1] = v[col];
                        }
                    }
                }
            }

            struct FreeFrame
            {
                void operator()(AVFrame* p) const { av_frame_free(&p); }
            };
            struct FreePacket
            {
                void operator()(AVPacket* p) const { av_packet_free(&p); }
            };
            Input                                 Source;
            const VideoClipInfo&                  Info;
            const std::atomic<bool>&              Stopping;
            CodecContext                          Codec;
            std::unique_ptr<AVFrame, FreeFrame>   Frame {av_frame_alloc()};
            std::unique_ptr<AVPacket, FreePacket> Packet {av_packet_alloc()};
            int64_t                               Origin   = 0;
            bool                                  Draining = false;
        };
#endif
    } // namespace

    bool IsVideoDecodingAvailable()
    {
#if KANS_HAS_FFMPEG && defined(KANS_FFMPEG_ALLOW_GPL)
        return true; // A GPL-enabled system FFmpeg accepted for a local, non-distributed build.
#elif KANS_HAS_FFMPEG
        return std::strncmp(avformat_license(), "LGPL", 4) == 0 && std::strncmp(avcodec_license(), "LGPL", 4) == 0 &&
               std::strncmp(avutil_license(), "LGPL", 4) == 0;
#else
        return false;
#endif
    }

    std::variant<VideoClipInfo, AssetError> ProbeVideo(const VideoBytes& bytes)
    {
#if KANS_HAS_FFMPEG
        if (IsVideoDecodingAvailable())
        {
            Input input(bytes);
            if (!input.Error.empty())
                return AssetError {AssetErrorCode::InvalidData, input.Error};
            const auto& stream = *input.Stream();
            const auto& codec  = *stream.codecpar;
            auto invalid = [](const std::string& reason) { return AssetError {AssetErrorCode::InvalidData, reason}; };
            try
            {
                OpenCodec(codec);
            }
            catch (const std::runtime_error& error)
            {
                return invalid(error.what());
            }
            if (codec.width <= 0 || codec.height <= 0 || codec.width > int(MaximumDimension) ||
                codec.height > int(MaximumDimension) || codec.width % 2 || codec.height % 2)
                return invalid("Video requires positive, even dimensions up to 8192");
            if (!SupportedFormat(AVPixelFormat(codec.format)))
                return invalid("Unsupported video pixel format (expected 8-bit YUV420P or NV12)");
            if (!SupportedMatrix(codec.color_space) || !SupportedTransfer(codec.color_trc) ||
                !SupportedPrimaries(codec.color_primaries))
                return invalid("Unsupported video colour space or transfer (expected SDR BT.601/BT.709)");
            const auto   rate     = av_guess_frame_rate(input.Format, input.Stream(), nullptr);
            const double duration = stream.duration > 0 ? double(stream.duration) * av_q2d(stream.time_base) :
                                                          double(input.Format->duration) / AV_TIME_BASE;
            if (rate.num <= 0 || rate.den <= 0 || !std::isfinite(duration) || duration <= 0)
                return invalid("Video has no usable frame rate or duration");
            VideoClipInfo info;
            info.Width           = codec.width;
            info.Height          = codec.height;
            info.RateNumerator   = rate.num;
            info.RateDenominator = rate.den;
            info.Duration        = duration;
            info.FrameCount      = stream.nb_frames > 0 ? uint64_t(stream.nb_frames) : 0;
            info.Codec           = avcodec_get_name(codec.codec_id);
            info.PixelFormat     = av_get_pix_fmt_name(AVPixelFormat(codec.format));
            info.Matrix          = Matrix(codec.color_space, codec.height);
            info.Range           = codec.color_range == AVCOL_RANGE_JPEG || codec.format == AV_PIX_FMT_YUVJ420P ?
                                       VideoRange::Full :
                                       VideoRange::Limited;
            info.Transfer        = Transfer(codec.color_trc);
            info.Primaries       = Primaries(codec.color_primaries);
            for (unsigned i = 0; i < input.Format->nb_streams; ++i)
                info.HasAudio |= input.Format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
            return info;
        }
#else
        (void)bytes;
#endif
        return AssetError {AssetErrorCode::NotImplemented, VideoDecodingUnavailable};
    }

    VideoColorConstants GetVideoColorConstants(VideoMatrix matrix, VideoRange range)
    {
        const float kr      = matrix == VideoMatrix::BT709 ? 0.2126f : 0.299f;
        const float kb      = matrix == VideoMatrix::BT709 ? 0.0722f : 0.114f;
        const float kg      = 1.f - kr - kb;
        const bool  limited = range == VideoRange::Limited;
        return {
            {limited ? 16.f / 255.f : 0.f, limited ? 255.f / 219.f : 1.f, 128.f / 255.f, limited ? 255.f / 224.f : 1.f},
            {2.f * (1.f - kr), -2.f * kb * (1.f - kb) / kg, -2.f * kr * (1.f - kr) / kg, 2.f * (1.f - kb)}};
    }

    std::array<std::array<float, 4>, 3> GetVideoGamutConstants(VideoPrimaries primaries)
    {
        // Linear source RGB to linear sRGB. Compute these as inverse(sRGB-to-XYZ)
        // times source-to-XYZ from the standard chromaticities, using their common D65 white.
        if (primaries == VideoPrimaries::SMPTE170M)
            return {{{.93954206f, .05018136f, .01027658f, 0},
                     {.01777222f, .96579286f, .01643491f, 0},
                     {-.00162160f, -.00436975f, 1.00599135f, 0}}};
        if (primaries == VideoPrimaries::BT470BG)
            return {{{1.04404321f, -.04404321f, 0, 0}, {0, 1, 0, 0}, {0, .01179338f, .98820662f, 0}}};
        return {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    }

    struct VideoDecoder::State
    {
        mutable std::mutex               Mutex;
        std::condition_variable          Changed;
        std::atomic<bool>                Stopping {false};
        std::thread                      Worker;
        std::array<VideoFrame, RingSize> Ring;
        VideoFrame                       Display;
        unsigned                         Head = 0, Fill = 0;
        uint64_t                         Generation = 0, NextSerial = 0, Decoded = 0, Dropped = 0;
        bool                             HasTarget = false;
        double                           Target = 0, SeekTarget = 0, FPS = 0;
        std::string                      Failure;

#if KANS_HAS_FFMPEG
        void Run(const VideoClipData& clip, const DecodeHook& hook)
        {
            Reader       reader(clip, Stopping);
            const size_t bytes   = size_t(clip.Info.Width) * clip.Info.Height * 3 / 2;
            auto         pending = VideoFrame {};
            auto         next    = VideoFrame {};
            pending.Planes.resize(bytes);
            next.Planes.resize(bytes);
            for (auto& frame : Ring)
                frame.Planes.resize(bytes);
            // Display is owned by the polling thread. Preallocate it before publishing any frames.
            {
                std::lock_guard guard(Mutex);
                Display.Planes.resize(bytes);
            }
            uint64_t generation = 0, rateFrames = 0;
            auto     rateStart  = std::chrono::steady_clock::now();
            double   seekTarget = 0;
            bool     primed = false, eof = false, catchUp = false;
            auto     read = [&](VideoFrame& frame) {
                if (hook)
                    hook();
                if (!reader.Next(frame))
                    return false;
                ++rateFrames;
                return true;
            };

            std::unique_lock lock(Mutex);
            while (!Stopping.load(std::memory_order_relaxed))
            {
                Changed.wait(lock, [&] {
                    return Stopping.load(std::memory_order_relaxed) ||
                           (HasTarget && (generation != Generation || (!eof && Fill < RingSize)));
                });
                if (Stopping.load(std::memory_order_relaxed))
                    break;
                const bool seek        = generation != Generation;
                generation             = Generation;
                const double requested = Target;
                if (seek)
                {
                    seekTarget = SeekTarget;
                    catchUp    = false;
                }
                lock.unlock();
                // Finish preroll to the last seek target before trying to catch up again.
                // A DTS-based index can appear newer than the pending PTS while seeking back into the same GOP.
                const bool skip = !seek && primed && pending.Time >= seekTarget && requested - pending.Time > .5 &&
                                  reader.CanSkipAhead(pending.Time, requested);
                if (skip)
                {
                    seekTarget = requested;
                    catchUp    = true;
                }
                if (seek || skip)
                {
                    reader.Seek(seekTarget);
                    primed = eof = false;
                }
                if (!primed)
                    primed = read(pending);
                // Some container indexes use decode timestamps. A keyframe can be before the
                // target in the index and still start after it in presentation time.
                if ((seek || skip) && primed && seekTarget > 0 && pending.Time > seekTarget)
                {
                    reader.Seek(0);
                    primed = read(pending);
                }
                const bool ready = primed;
                if (ready)
                {
                    eof         = !read(next);
                    pending.End = eof ? std::numeric_limits<double>::infinity() : next.Time;
                }
                else
                    eof = true;
                lock.lock();
                Decoded += rateFrames;
                const auto now = std::chrono::steady_clock::now();
                if (rateFrames != 0)
                {
                    const auto elapsed = std::chrono::duration<double>(now - rateStart).count();
                    if (elapsed > 0)
                    {
                        const auto sample = double(rateFrames) / elapsed;
                        FPS               = FPS == 0 ? sample : FPS * .9 + sample * .1;
                    }
                }
                rateFrames = 0;
                rateStart  = now;
                if (Generation != generation || !ready)
                    continue;
                if (skip)
                    ++Dropped; // The predecoded pending frame was bypassed by the newer keyframe.
                if (pending.End > seekTarget && pending.End > pending.Time)
                {
                    pending.Serial = ++NextSerial;
                    std::swap(Ring[(Head + Fill) % RingSize], pending);
                    ++Fill;
                    catchUp = false;
                }
                else if (catchUp && pending.End <= seekTarget)
                    ++Dropped;
                if (!eof)
                    std::swap(pending, next);
            }
        }
#endif
    };

    VideoDecoder::VideoDecoder(Ref<const VideoClipAsset> clip, DecodeHook hook) : m_State(std::make_unique<State>())
    {
#if KANS_HAS_FFMPEG
        if (IsVideoDecodingAvailable() && clip && clip->GetData().Encoded)
        {
            m_State->Worker = std::thread([state = m_State.get(), clip = std::move(clip), hook = std::move(hook)] {
                ++Workers;
                try
                {
                    state->Run(clip->GetData(), hook);
                }
                catch (const std::exception& error)
                {
                    std::lock_guard guard(state->Mutex);
                    state->Failure = error.what();
                }
                --Workers;
            });
            return;
        }
#else
        (void)clip;
        (void)hook;
#endif
        m_State->Failure = VideoDecodingUnavailable;
    }

    VideoDecoder::~VideoDecoder()
    {
        {
            std::lock_guard guard(m_State->Mutex);
            m_State->Stopping.store(true, std::memory_order_relaxed);
        }
        m_State->Changed.notify_one();
        if (m_State->Worker.joinable())
            m_State->Worker.join();
    }

    const VideoFrame* VideoDecoder::Sample(double seconds, bool seek)
    {
        m_SeekPending |= seek;
        auto&            s = *m_State;
        std::unique_lock lock(s.Mutex, std::try_to_lock);
        if (lock && std::isfinite(seconds))
        {
            seconds = std::max(0.0, seconds);
            if (m_SeekPending || (!s.HasTarget && seconds > 0) || (s.HasTarget && seconds < s.Target))
            {
                s.Fill = s.Head = 0;
                s.SeekTarget    = seconds;
                ++s.Generation;
            }
            m_SeekPending     = false;
            s.HasTarget       = true;
            s.Target          = seconds;
            unsigned eligible = 0;
            while (eligible < s.Fill && s.Ring[(s.Head + eligible) % RingSize].Time <= seconds)
                ++eligible;
            if (eligible)
            {
                s.Dropped += eligible - 1;
                std::swap(s.Display, s.Ring[(s.Head + eligible - 1) % RingSize]);
                s.Head = (s.Head + eligible) % RingSize;
                s.Fill -= eligible;
            }
            lock.unlock();
            s.Changed.notify_one();
        }
        return s.Display.Serial ? &s.Display : nullptr;
    }

    VideoDecoderStats VideoDecoder::Stats() const
    {
        std::unique_lock lock(m_State->Mutex, std::try_to_lock);
        return lock ? VideoDecoderStats {m_State->Decoded, m_State->Dropped, m_State->Fill, m_State->FPS} :
                      VideoDecoderStats {};
    }

    std::string VideoDecoder::Error() const
    {
        std::unique_lock lock(m_State->Mutex, std::try_to_lock);
        return lock ? m_State->Failure : std::string {};
    }

    unsigned VideoDecoder::ActiveWorkers() { return Workers.load(); }
} // namespace Kans

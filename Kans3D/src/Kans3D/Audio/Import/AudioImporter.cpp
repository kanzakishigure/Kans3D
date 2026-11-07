#include "kspch.h"
#include "AudioImporter.h"
#include <miniaudio.h>
#include <cmath>
#include <cstring>
#include "Kans3D/Asset/Product/AudioProduct.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "AudioImportSettings.h"

namespace Kans
{
    namespace
    {
        using Bytes = std::vector<std::byte>;
        bool At(const Bytes& bytes, size_t offset, const char* text, size_t count)
        {
            return offset <= bytes.size() && count <= bytes.size() - offset &&
                   std::memcmp(bytes.data() + offset, text, count) == 0;
        }
        bool HasMpegFrameHeader(const Bytes& bytes)
        {
            if (bytes.size() < 4)
                return false;
            const auto first  = std::to_integer<uint8_t>(bytes[0]);
            const auto second = std::to_integer<uint8_t>(bytes[1]);
            const auto third  = std::to_integer<uint8_t>(bytes[2]);
            const auto fourth = std::to_integer<uint8_t>(bytes[3]);
            // Accept free-format bitrate index zero; only reserved header values are excluded.
            return first == 0xff && (second & 0xe0) == 0xe0 && ((second >> 3) & 3) != 1 && (second & 6) != 0 &&
                   (third >> 4) != 15 && ((third >> 2) & 3) != 3 && (fourth & 3) != 2;
        }

        struct WavInput
        {
            const Bytes&     Data;
            size_t           Cursor = 0;
            static ma_result Read(ma_decoder* decoder, void* out, size_t count, size_t* read)
            {
                auto&      input     = *static_cast<WavInput*>(decoder->pUserData);
                const auto available = std::min(count, input.Data.size() - input.Cursor);
                if (available)
                    std::memcpy(out, input.Data.data() + input.Cursor, available);
                input.Cursor += available;
                if (read)
                    *read = available;
                return available || count == 0 ? MA_SUCCESS : MA_AT_END;
            }
            static ma_result Seek(ma_decoder* decoder, ma_int64 offset, ma_seek_origin origin)
            {
                // No physical-EOF hint: keep the WAV data chunk's declared length for full-decode validation.
                // The memory decoder otherwise silently shortens that length on truncated input.
                if (origin == ma_seek_origin_end)
                    return MA_NOT_IMPLEMENTED;
                auto&      input = *static_cast<WavInput*>(decoder->pUserData);
                const auto base  = origin == ma_seek_origin_start ? 0 : input.Cursor;
                if (offset < -static_cast<ma_int64>(base))
                    return MA_BAD_SEEK;
                const auto remaining = input.Data.size() - base;
                input.Cursor         = offset > static_cast<ma_int64>(remaining) ?
                                           input.Data.size() :
                                           static_cast<size_t>(static_cast<ma_int64>(base) + offset);
                return MA_SUCCESS;
            }
        };

        // Vorbis needs complete Ogg pages and a final EOS page. WAV/MP3 validation belongs to the decoder.
        bool CompleteContainer(const Bytes& bytes, AudioEncoding encoding)
        {
            if (encoding == AudioEncoding::Vorbis)
            {
                size_t offset = 0;
                bool   eos    = false;
                while (offset < bytes.size())
                {
                    if (bytes.size() - offset < 27 || !At(bytes, offset, "OggS", 4))
                        return false;
                    const auto segments = std::to_integer<uint8_t>(bytes[offset + 26]);
                    if (bytes.size() - offset - 27 < segments)
                        return false;
                    size_t count = 27 + segments;
                    for (unsigned i = 0; i < segments; ++i)
                        count += std::to_integer<uint8_t>(bytes[offset + 27 + i]);
                    if (count > bytes.size() - offset)
                        return false;
                    eos = (std::to_integer<uint8_t>(bytes[offset + 5]) & 4) != 0;
                    offset += count;
                }
                return eos;
            }
            return true;
        }

        std::variant<AudioClipData, AssetError> Parse(Bytes bytes, const AudioImportSettings& settings)
        {
            AudioClipData      data;
            ma_encoding_format format;
            if (At(bytes, 0, "RIFF", 4))
            {
                data.Encoding = AudioEncoding::Wav;
                format        = ma_encoding_format_wav;
            }
            else if (At(bytes, 0, "OggS", 4))
            {
                data.Encoding = AudioEncoding::Vorbis;
                format        = ma_encoding_format_vorbis;
            }
            else if (At(bytes, 0, "fLaC", 4))
            {
                data.Encoding = AudioEncoding::Flac;
                format        = ma_encoding_format_flac;
            }
            else if (At(bytes, 0, "ID3", 3) || HasMpegFrameHeader(bytes))
            {
                data.Encoding = AudioEncoding::Mp3;
                format        = ma_encoding_format_mp3;
            }
            else
                return AssetError {AssetErrorCode::InvalidData, "Unsupported audio format"};
            const auto invalid = AssetError {AssetErrorCode::InvalidData, "Invalid or truncated Audio source"};
            if (!CompleteContainer(bytes, data.Encoding))
                return invalid;
            ma_decoder decoder;
            WavInput   input {bytes};
            auto       config      = ma_decoder_config_init(ma_format_f32, 0, 0);
            config.encodingFormat  = format;
            const auto initialized = data.Encoding == AudioEncoding::Wav ?
                                         ma_decoder_init(WavInput::Read, WavInput::Seek, &input, &config, &decoder) :
                                         ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &decoder);
            if (initialized != MA_SUCCESS)
                return invalid;
            struct Cleanup
            {
                ma_decoder* Decoder;
                ~Cleanup() { ma_decoder_uninit(Decoder); }
            } cleanup {&decoder};
            ma_uint64 length = 0;
            if (ma_decoder_get_data_format(&decoder, nullptr, &data.Info.Channels, &data.Info.SampleRate, nullptr, 0) !=
                    MA_SUCCESS ||
                !data.Info.Channels || !data.Info.SampleRate)
                return invalid;
            std::vector<float> pcm(4096 * data.Info.Channels);
            if (data.Encoding == AudioEncoding::Mp3)
            {
                // Xing/VBR headers can retain the original length after a partial final frame.
                // Count the frames this decoder actually emits, then rewind for the waveform pass.
                for (;;)
                {
                    ma_uint64  read   = 0;
                    const auto result = ma_decoder_read_pcm_frames(&decoder, pcm.data(), 4096, &read);
                    if ((result != MA_SUCCESS && result != MA_AT_END) || read > UINT64_MAX / 1024 - length)
                        return invalid;
                    length += read;
                    if (read == 0 || result == MA_AT_END)
                        break;
                }
            }
            else if (ma_decoder_get_length_in_pcm_frames(&decoder, &length) != MA_SUCCESS)
                return invalid;
            if (!length || length > UINT64_MAX / 1024 || ma_decoder_seek_to_pcm_frame(&decoder, 0) != MA_SUCCESS)
                return invalid;
            data.Info.LengthFrames = length;
            data.LoadMode          = settings.LoadMode.value_or(
                length >= uint64_t(data.Info.SampleRate) * 10 ? AudioLoadMode::Stream : AudioLoadMode::Decompress);
            data.Volume        = settings.Volume;
            const double begin = double(settings.LoopBegin) * data.Info.SampleRate;
            const double end = settings.LoopEnd == 0 ? double(length) : double(settings.LoopEnd) * data.Info.SampleRate;
            if (begin >= end || begin > double(length) + 0.5 || end > double(length) + 0.5)
                return AssetError {AssetErrorCode::InvalidArgument, "Audio loop exceeds source length"};
            // Round seconds to the nearest source frame; validate again after rounding.
            data.LoopBegin = static_cast<uint64_t>(std::round(begin));
            data.LoopEnd   = static_cast<uint64_t>(std::round(end));
            if (data.LoopBegin >= data.LoopEnd || data.LoopEnd > length)
                return AssetError {AssetErrorCode::InvalidArgument, "Audio loop contains no source frames"};
            const auto         buckets = std::min<uint64_t>(1024, length);
            std::vector<float> peaks(buckets, 0);
            uint64_t           total = 0;
            for (;;)
            {
                ma_uint64  read   = 0;
                const auto result = ma_decoder_read_pcm_frames(&decoder, pcm.data(), 4096, &read);
                if ((result != MA_SUCCESS && result != MA_AT_END) || read > length - total)
                    return invalid;
                for (uint64_t frame = 0; frame < read; ++frame)
                {
                    double mono = 0;
                    for (unsigned ch = 0; ch < data.Info.Channels; ++ch)
                    {
                        const auto sample = pcm[frame * data.Info.Channels + ch];
                        if (!std::isfinite(sample))
                            return invalid;
                        mono += sample;
                    }
                    const auto bucket = (total + frame) * buckets / length;
                    peaks[bucket]     = std::max(peaks[bucket], float(std::abs(mono / data.Info.Channels)));
                }
                total += read;
                if (read == 0 || result == MA_AT_END)
                    break;
            }
            if (total != length)
                return invalid;
            for (auto peak : peaks)
                data.Waveform.push_back(static_cast<uint8_t>(std::lround(std::clamp(peak, 0.f, 1.f) * 255)));
            data.Encoded = std::make_shared<const Bytes>(std::move(bytes));
            return data;
        }
    } // namespace

    std::variant<ImportResult, AssetError> AudioImporter::Import(const ImportContext& context) const
    {
        if (!context.SourceID.IsValid() || !context.SourceFile.is_absolute() || !context.StagingDirectory.is_absolute())
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid Audio import context"};
        auto settings = ParseAudioImportSettings(context.Settings);
        if (auto* error = std::get_if<AssetError>(&settings))
            return *error;
        std::error_code error;
        const auto      status = std::filesystem::symlink_status(context.StagingDirectory, error);
        if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_empty(context.StagingDirectory, error) || error)
            return AssetError {AssetErrorCode::InvalidArgument, "Audio staging must be an empty exclusive directory"};
        auto allocated = AllocateLocalIDs(context.PreviousLocalIDs, {"audio:root"});
        if (auto* failure = std::get_if<AssetError>(&allocated))
            return *failure;
        auto source = ReadProductFile(context.SourceFile, AudioProductMaximumBytes - 64 - 1024);
        if (auto* failure = std::get_if<AssetError>(&source))
            return *failure;
        auto parsed = Parse(std::get<Bytes>(std::move(source)), std::get<AudioImportSettings>(settings));
        if (auto* failure = std::get_if<AssetError>(&parsed))
            return *failure;
        auto encoded = EncodeAudioProduct(std::get<AudioClipData>(parsed));
        if (auto* failure = std::get_if<AssetError>(&encoded))
            return *failure;
        ImportResult result;
        result.LocalIDs  = std::get<LocalIDMap>(std::move(allocated));
        const auto local = result.LocalIDs.at("audio:root");
        result.Products.push_back({local, AssetType::AudioClip, std::to_string(local) + ".kaud", {}});
        auto written =
            WriteProductFile(context.StagingDirectory / result.Products[0].StagedFile, std::get<Bytes>(encoded));
        if (auto* failure = std::get_if<AssetError>(&written))
            return *failure;
        return result;
    }
} // namespace Kans

#include "kspch.h"
#include "AudioProduct.h"
#include <cstring>

namespace Kans
{
    namespace
    {
        constexpr size_t Header = 64, MaximumBytes = AudioProductMaximumBytes;
        template<typename T>
        void Write(std::vector<std::byte>& out, T value)
        {
            for (size_t i = 0; i < sizeof(T); ++i)
                out.push_back(static_cast<std::byte>((value >> (8 * i)) & 255));
        }
        template<typename T>
        T Read(const std::byte*& cursor)
        {
            T value = 0;
            for (size_t i = 0; i < sizeof(T); ++i)
                value |= T(std::to_integer<uint8_t>(*cursor++)) << (8 * i);
            return value;
        }
    } // namespace

    std::variant<std::vector<std::byte>, AssetError> EncodeAudioProduct(const AudioClipData& data)
    {
        if (auto valid = ValidateAudioClipData(data); auto* error = std::get_if<AssetError>(&valid))
            return *error;
        std::vector<std::byte> bytes;
        bytes.reserve(Header + data.Waveform.size() + data.Encoded->size());
        Write(bytes, uint32_t(0x4455414b)); // KAUD
        Write(bytes, uint16_t(1));
        Write(bytes, uint16_t(AssetType::AudioClip));
        Write(bytes, uint16_t(data.Encoding));
        Write(bytes, uint16_t(data.LoadMode));
        Write(bytes, data.Info.Channels);
        Write(bytes, data.Info.SampleRate);
        Write(bytes, data.Info.LengthFrames);
        uint32_t volume;
        std::memcpy(&volume, &data.Volume, sizeof(volume));
        Write(bytes, volume);
        Write(bytes, data.LoopBegin);
        Write(bytes, data.LoopEnd);
        Write(bytes, uint32_t(data.Waveform.size()));
        Write(bytes, uint64_t(data.Encoded->size()));
        Write(bytes, uint32_t(0)); // Reserved, must be zero in v1.
        for (auto peak : data.Waveform)
            bytes.push_back(static_cast<std::byte>(peak));
        bytes.insert(bytes.end(), data.Encoded->begin(), data.Encoded->end());
        return bytes;
    }

    namespace
    {
        // The header and the waveform peaks of the first `available` bytes of a Product that is `size`
        // bytes long; `payload` receives the encoded byte count and `rest` points at the encoded bytes.
        std::variant<AudioClipData, AssetError>
        DecodeHeader(const std::byte* bytes, size_t available, uint64_t size, uint64_t& payload, const std::byte*& rest)
        {
            // The fixed header check covers truncation at every scalar field before Read is called.
            if (!bytes || available < Header || available > size || size > MaximumBytes)
                return AssetError {AssetErrorCode::InvalidData, "Invalid Audio Product byte length"};
            auto cursor = bytes;
            if (Read<uint32_t>(cursor) != 0x4455414b)
                return AssetError {AssetErrorCode::InvalidData, "Invalid Audio Product magic"};
            if (Read<uint16_t>(cursor) != 1)
                return AssetError {AssetErrorCode::UnsupportedVersion, "Unsupported Audio Product version"};
            if (Read<uint16_t>(cursor) != uint16_t(AssetType::AudioClip))
                return AssetError {AssetErrorCode::InvalidData, "Invalid Audio Product type"};
            AudioClipData data;
            data.Encoding          = AudioEncoding(Read<uint16_t>(cursor));
            data.LoadMode          = AudioLoadMode(Read<uint16_t>(cursor));
            data.Info.Channels     = Read<uint32_t>(cursor);
            data.Info.SampleRate   = Read<uint32_t>(cursor);
            data.Info.LengthFrames = Read<uint64_t>(cursor);
            const auto volume      = Read<uint32_t>(cursor);
            std::memcpy(&data.Volume, &volume, sizeof(volume));
            data.LoopBegin      = Read<uint64_t>(cursor);
            data.LoopEnd        = Read<uint64_t>(cursor);
            const auto peaks    = Read<uint32_t>(cursor);
            payload             = Read<uint64_t>(cursor);
            const auto reserved = Read<uint32_t>(cursor);
            if (auto valid = ValidateAudioClipInfo(data); auto* error = std::get_if<AssetError>(&valid))
                return *error;
            if (reserved || peaks != std::min<uint64_t>(1024, data.Info.LengthFrames) || peaks > size - Header ||
                payload == 0 || payload != size - Header - peaks || peaks > available - Header)
                return AssetError {AssetErrorCode::InvalidData, "Invalid Audio Product waveform/payload length"};
            data.Waveform.reserve(peaks);
            for (uint32_t i = 0; i < peaks; ++i)
                data.Waveform.push_back(std::to_integer<uint8_t>(*cursor++));
            rest = cursor;
            return data;
        }
    } // namespace

    std::variant<AudioClipData, AssetError> DecodeAudioProduct(const std::byte* bytes, size_t size)
    try
    {
        uint64_t         payload = 0;
        const std::byte* cursor  = nullptr;
        auto             decoded = DecodeHeader(bytes, size, size, payload, cursor);
        if (auto* error = std::get_if<AssetError>(&decoded))
            return *error;
        auto& data = std::get<AudioClipData>(decoded);
        // One owned payload allocation; loaders and engines subsequently share it.
        data.Encoded = std::make_shared<const std::vector<std::byte>>(cursor, cursor + payload);
        if (auto valid = ValidateAudioClipData(data); auto* error = std::get_if<AssetError>(&valid))
            return *error;
        return std::move(data);
    }
    catch (const std::bad_alloc&)
    {
        return AssetError {AssetErrorCode::InvalidData, "Audio Product allocation exceeds available memory"};
    }

    std::variant<AudioClipData, AssetError>
    DecodeAudioProductPreview(const std::byte* bytes, size_t available, uint64_t productSize)
    try
    {
        uint64_t         payload = 0;
        const std::byte* cursor  = nullptr;
        return DecodeHeader(bytes, available, productSize, payload, cursor);
    }
    catch (const std::bad_alloc&)
    {
        return AssetError {AssetErrorCode::InvalidData, "Audio Product allocation exceeds available memory"};
    }
} // namespace Kans

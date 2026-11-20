#pragma once
#include "Kans3D/Asset/Runtime/AudioClipData.h"

namespace Kans
{
    [[nodiscard]] std::variant<std::vector<std::byte>, AssetError> EncodeAudioProduct(const AudioClipData& data);
    [[nodiscard]] std::variant<AudioClipData, AssetError> DecodeAudioProduct(const std::byte* bytes, size_t size);
    // The bytes a preview needs from the start of a Product: the header and the largest waveform.
    inline constexpr size_t AudioProductPreviewBytes = 64 + 1024;
    // The metadata and waveform peaks from the first `available` bytes of a Product that is
    // `productSize` bytes long, without the encoded audio (Encoded stays empty): editors read the
    // waveform of a long clip without loading it. Checks the header like DecodeAudioProduct.
    [[nodiscard]] std::variant<AudioClipData, AssetError>
    DecodeAudioProductPreview(const std::byte* bytes, size_t available, uint64_t productSize);
} // namespace Kans

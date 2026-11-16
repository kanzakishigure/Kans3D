#include "kspch.h"
#include "Texture.h"
#include <cstring>
#include <stdexcept>
#include <glm/gtc/packing.hpp>
#include "Kans3D/RHI/Backend.h"
#include "TextureFile.h"
namespace Kans
{
    Texture2D::Texture2D(std::shared_ptr<RenderHardware::Device>         device,
                         RenderHardware::Handle<RenderHardware::Texture> image,
                         const RenderHardware::SamplerDesc&              sampling,
                         std::filesystem::path                           path) :
        m_Device(std::move(device)), m_Image(std::move(image)), m_Sampling(sampling), m_Path(std::move(path))
    {
        if (!m_Device || !m_Image || m_Image->GetDesc().Dimension != RenderHardware::TextureDimension::Texture2D)
            throw std::invalid_argument("Texture2D requires a device and a 2D image");
    }
    uintptr_t Texture2D::GetImGuiTextureID() const { return m_Device->GetImGuiTextureID(m_Image, &m_Sampling); }
    std::variant<Ref<Texture2D>, AssetError>
    Texture2D::CreateFromProduct(const TextureData& data, std::shared_ptr<RenderHardware::Device> device)
    {
        if (data.RenderTexture)
            return AssetError {AssetErrorCode::InvalidArgument, "RenderTexture requires RenderAssetCache"};
        auto valid = ValidateTextureData(data);
        if (auto* error = std::get_if<AssetError>(&valid))
            return *error;
        if (data.Kind != TextureKind::Texture2D)
            return AssetError {AssetErrorCode::NotImplemented, "Only 2D product textures are supported"};
        if (!device)
        {
            if (RenderHardware::GetBackend() == RenderHardware::Backend::None)
                return AssetError {AssetErrorCode::NotImplemented, "Texture upload is unavailable for this backend"};
            try
            {
                device = RenderHardware::GetSharedDevice();
            }
            catch (const std::exception& e)
            {
                return AssetError {AssetErrorCode::InvalidArgument, e.what()};
            }
        }
        try
        {
            using namespace RenderHardware;
            TextureDesc desc;
            desc.Width     = data.Width;
            desc.Height    = data.Height;
            desc.MipLevels = uint32_t(data.Mips.size());
            desc.Usage     = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::CopySource |
                             RenderHardware::TextureUsage::CopyDestination;
            switch (data.Format)
            {
                case TexturePixelFormat::R8:
                    desc.PixelFormat = Format::R8Unorm;
                    break;
                case TexturePixelFormat::RG8:
                    desc.PixelFormat = Format::RG8Unorm;
                    break;
                case TexturePixelFormat::RGBA8:
                    desc.PixelFormat =
                        data.ColorSpace == TextureColorSpace::SRGB ? Format::RGBA8Srgb : Format::RGBA8Unorm;
                    break;
                case TexturePixelFormat::RGBA32Float:
                    desc.PixelFormat = Format::RGBA32Float;
                    break;
            }
            auto image    = device->CreateTexture(desc);
            auto commands = device->CreateCommandList();
            for (uint32_t mip = 0; mip < desc.MipLevels; ++mip)
            {
                const auto&            source = data.Mips[mip].Pixels;
                std::vector<std::byte> bytes(source.size());
                const size_t           height = std::max(1u, data.Height >> mip), row = source.size() / height;
                // Authored UV v=0 addresses the bottom of an imported image on
                // every backend. Framebuffer/copy Origin controls rendered image
                // rows, not the UV convention of mesh/material asset data.
                const bool flip = data.Origin == TextureOrigin::TopLeft;
                for (size_t y = 0; y < height; ++y)
                {
                    const auto* input = source.data() + (flip ? height - 1 - y : y) * row;
                    if (data.Format != TexturePixelFormat::RGBA32Float)
                        std::memcpy(bytes.data() + y * row, input, row);
                    else
                        for (size_t x = 0; x < row; x += 4)
                        {
                            uint32_t bits = 0;
                            for (size_t b = 0; b < 4; ++b)
                                bits |= uint32_t(std::to_integer<uint8_t>(input[x + b])) << (b * 8);
                            std::memcpy(bytes.data() + y * row + x, &bits, 4);
                        }
                }
                auto upload = device->CreateBuffer({bytes.size(), BufferUsage::CopySource, MemoryUsage::Upload});
                upload->Write(0, bytes.data(), bytes.size());
                commands->CopyBufferToTexture(upload, 0, image, {mip, 0});
            }
            device->Submit(commands);
            device->WaitIdle();
            SamplerDesc sampling;
            sampling.Min = sampling.Mag = sampling.Mip =
                data.Filter == TextureFilter::Nearest ? Filter::Nearest : Filter::Linear;
            sampling.U = sampling.V = sampling.W = data.Wrap == TextureWrap::Clamp  ? AddressMode::ClampToEdge :
                                                   data.Wrap == TextureWrap::Mirror ? AddressMode::MirrorRepeat :
                                                                                      AddressMode::Repeat;
            return CreateRef<Texture2D>(std::move(device), std::move(image), sampling);
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, e.what()};
        }
    }

    namespace
    {
        RenderHardware::SamplerDesc Sampling(const TextureSpecification& spec)
        {
            RenderHardware::SamplerDesc sampling;
            sampling.Min = spec.Minf;
            sampling.Mag = spec.Maxf;
            sampling.Mip = spec.Minf;
            sampling.U = sampling.V = sampling.W = spec.Wrap;
            if (!spec.GenerateMips)
                sampling.MaxLod = 0;
            return sampling;
        }
    }
    Ref<Texture2D> Texture2D::Create(const TextureSpecification& spec, const std::filesystem::path& path)
    {
        // Preserve the caller's path spelling, but resolve relative files against the current working directory.
        std::error_code pathError;
        auto            absolutePath = std::filesystem::absolute(path, pathError);
        const auto      colorSpace   = spec.SRGB || spec.Format == RenderHardware::Format::RGBA8Srgb ?
                                           TextureColorSpace::SRGB :
                                           TextureColorSpace::Linear;
        auto            decoded =
            pathError ?
                std::variant<TextureData, AssetError>(AssetError {AssetErrorCode::IoError, pathError.message()}) :
                DecodeTextureFileForDisplay(absolutePath, colorSpace);
        if (auto* error = std::get_if<AssetError>(&decoded))
        {
            if (Log::GetCoreLogger())
                CORE_ERROR(
                    "Cannot load texture '{}': {}. Using magenta checker fallback.", path.string(), error->Message);
            else
                std::cerr << "Cannot load texture '" << path.string() << "': " << error->Message
                          << ". Using magenta checker fallback.\n";
            auto fallbackSpec  = spec;
            fallbackSpec.Width = fallbackSpec.Height = 2;
            fallbackSpec.Format                      = RenderHardware::Format::RGBA8Unorm;
            fallbackSpec.SRGB                        = false;
            uint8_t pixels[]                         = {255, 0, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 0, 255, 255};
            auto    fallback                         = Create(fallbackSpec, Buffer(pixels, sizeof(pixels)));
            fallback->m_Path                         = path;
            return fallback;
        }
        auto data = std::get<TextureData>(std::move(decoded));
        if (data.Format == TexturePixelFormat::RGBA32Float)
        {
            // The old file loader stored HDR files at half precision, unlike authored full-float Products.
            auto       device = RenderHardware::GetSharedDevice();
            const bool            flip   = data.Origin == TextureOrigin::TopLeft;
            std::vector<uint16_t> half(size_t(data.Width) * data.Height * 4);
            for (size_t y = 0; y < data.Height; ++y)
                for (size_t x = 0; x < size_t(data.Width) * 4; ++x)
                {
                    auto* bytes =
                        data.Mips[0].Pixels.data() + ((flip ? data.Height - 1 - y : y) * data.Width * 4 + x) * 4;
                    uint32_t word = 0;
                    for (unsigned b = 0; b < 4; ++b)
                        word |= uint32_t(std::to_integer<uint8_t>(bytes[b])) << (8 * b);
                    float value;
                    std::memcpy(&value, &word, 4);
                    half[y * data.Width * 4 + x] = glm::packHalf1x16(value);
                }
            auto hdrSpec    = spec;
            hdrSpec.Width   = data.Width;
            hdrSpec.Height  = data.Height;
            hdrSpec.Format  = RenderHardware::Format::RGBA16Float;
            hdrSpec.SRGB    = false;
            auto texture    = Create(hdrSpec, Buffer(half.data(), uint32_t(half.size() * sizeof(uint16_t))));
            texture->m_Path = path;
            return texture;
        }
        uint32_t levels = 1;
        if (spec.GenerateMips)
            for (auto size = std::max(data.Width, data.Height); size > 1; size >>= 1)
                ++levels;
        for (uint32_t level = 1; level < levels; ++level)
            data.Mips.push_back({std::vector<std::byte>(std::get<uint64_t>(TextureMipByteSize(data, level)))});
        auto uploaded = CreateFromProduct(data);
        if (auto* error = std::get_if<AssetError>(&uploaded))
            throw std::runtime_error(error->Message);
        auto texture        = std::get<Ref<Texture2D>>(std::move(uploaded));
        texture->m_Path     = path;
        texture->m_Sampling = Sampling(spec);
        if (levels > 1)
        {
            auto commands = texture->m_Device->CreateCommandList();
            commands->GenerateMips(texture->m_Image);
            texture->m_Device->Submit(commands);
            texture->m_Device->WaitIdle();
        }
        return texture;
    }
    Ref<Texture2D> Texture2D::Create(const TextureSpecification& spec, Buffer bytes)
    {
        auto                        device = RenderHardware::GetSharedDevice();
        RenderHardware::TextureDesc desc;
        desc.Width       = spec.Width;
        desc.Height      = spec.Height;
        desc.PixelFormat = spec.SRGB && spec.Format == RenderHardware::Format::RGBA8Unorm ?
                               RenderHardware::Format::RGBA8Srgb :
                               spec.Format;
        desc.Usage       = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::CopySource |
                           RenderHardware::TextureUsage::CopyDestination;
        if (spec.GenerateMips)
            for (auto size = std::max(desc.Width, desc.Height); size > 1; size >>= 1)
                ++desc.MipLevels;
        auto image = device->CreateTexture(desc);
        if (bytes.Data && bytes.Size)
        {
            auto upload = device->CreateBuffer(
                {bytes.Size, RenderHardware::BufferUsage::CopySource, RenderHardware::MemoryUsage::Upload});
            upload->Write(0, bytes.Data, bytes.Size);
            auto commands = device->CreateCommandList();
            commands->CopyBufferToTexture(upload, 0, image, {});
            if (desc.MipLevels > 1)
                commands->GenerateMips(image);
            device->Submit(commands);
            device->WaitIdle();
        }
        return CreateRef<Texture2D>(std::move(device), std::move(image), Sampling(spec));
    }
} // namespace Kans

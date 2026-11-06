#include "kspch.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
#include <mutex>
#include <tuple>
#include "Kans3D/Asset/Importer/ShaderImporter.h"
#include "Kans3D/Renderer/Shading/ComputeShaders.h"
#include "Kans3D/Renderer/Shading/Skinning.h"

namespace Kans
{
    const InternalShaderData&
    GetInternalShaderData(const std::string& name, const std::string& vertex, const std::string& fragment)
    {
        static std::mutex                                                                      mutex;
        static std::map<std::tuple<std::string, std::string, std::string>, InternalShaderData> cache;
        const std::lock_guard<std::mutex>                                                      lock(mutex);
        const auto key = std::make_tuple(name, vertex, fragment);
        if (auto found = cache.find(key); found != cache.end())
            return found->second;
        InternalShaderData result;
        try
        {
            result = CompileEmbeddedShaderProgram("KansShaderProgram 1\nvertex=Internal.vert\nfragment=Internal.frag\n",
                                                  {{"Internal.vert", vertex}, {"Internal.frag", fragment}});
        }
        catch (const std::exception& error)
        {
            result = AssetError {AssetErrorCode::InvalidData, error.what()};
        }
        if (auto* error = std::get_if<AssetError>(&result))
            error->Message = "Internal shader '" + name + "': " + error->Message;
        return cache.emplace(key, std::move(result)).first->second;
    }

    std::variant<RenderHardware::Handle<RenderHardware::Shader>, AssetError>
    CreateInternalShader(RenderHardware::Device& device,
                         const std::string&      name,
                         const std::string&      vertex,
                         const std::string&      fragment)
    {
        const auto& product = GetInternalShaderData(name, vertex, fragment);
        if (auto* error = std::get_if<AssetError>(&product))
            return *error;
        try
        {
            return device.CreateShader(std::get<ShaderData>(product));
        }
        catch (const std::exception& error)
        {
            return AssetError {AssetErrorCode::InvalidData, "Internal shader '" + name + "': " + error.what()};
        }
    }

    RenderHardware::Handle<RenderHardware::Shader> GetSkinnedInternalProgram(RenderHardware::Device& device,
                                                                             const std::string&      name,
                                                                             const std::string&      vertex,
                                                                             const std::string&      fragment)
    {
        using Key = std::tuple<const RenderHardware::Device*, std::string, std::string, std::string>;
        static std::map<Key, std::weak_ptr<RenderHardware::Shader>> cache;
        const Key                                                   key {&device, name, vertex, fragment};
        if (auto shader = cache[key].lock())
            return shader;
        const auto& source = GetInternalShaderData(name, vertex, fragment);
        if (auto* error = std::get_if<AssetError>(&source))
            throw std::runtime_error(error->Message);
        auto converted = MakeSkinnedShader(std::get<ShaderData>(source));
        if (auto* error = std::get_if<AssetError>(&converted))
            throw std::runtime_error(error->Message);
        auto shader = device.CreateShader(std::get<ShaderData>(converted));
        cache[key]  = shader;
        return shader;
    }

    const InternalShaderData& GetInternalComputeShaderData(const std::string& name, const std::string& source)
    {
        static std::mutex                                                        mutex;
        static std::map<std::pair<std::string, std::string>, InternalShaderData> cache;
        const std::lock_guard<std::mutex>                                        lock(mutex);
        const auto                                                               key = std::make_pair(name, source);
        if (auto found = cache.find(key); found != cache.end())
            return found->second;
        auto product =
            CompileEmbeddedShaderProgram("KansShaderProgram 1\ncompute=Internal.comp\n", {{"Internal.comp", source}});
        if (auto* error = std::get_if<AssetError>(&product))
            error->Message = "Compute shader '" + name + "': " + error->Message;
        return cache.emplace(key, std::move(product)).first->second;
    }
    const InternalShaderData& GetBuiltinComputeShaderData(const std::string& name)
    {
        if (name == "CompactAlive")
            return GetInternalComputeShaderData(name, ComputeShaders::CompactAlive);
        if (name == "CompactAliveFinalize")
            return GetInternalComputeShaderData(name, ComputeShaders::CompactAliveFinalize);
        throw std::invalid_argument("Unknown builtin compute shader: " + name);
    }
    RenderHardware::Handle<RenderHardware::Shader> CreateBuiltinComputeShader(RenderHardware::Device& device,
                                                                              const std::string&      name)
    {
        static std::map<std::pair<const RenderHardware::Device*, std::string>, std::weak_ptr<RenderHardware::Shader>>
                   cache;
        const auto key = std::make_pair(&device, name);
        if (auto shader = cache[key].lock())
            return shader;
        const auto& data = GetBuiltinComputeShaderData(name);
        if (const auto* error = std::get_if<AssetError>(&data))
            throw std::runtime_error(error->Message);
        auto shader = device.CreateShader(std::get<ShaderData>(data));
        cache[key]  = shader;
        return shader;
    }

    glm::mat4 DeviceProjection(const glm::mat4& projection, RenderHardware::CoordinateConventions coordinates)
    {
        glm::mat4 conversion(1);
        if (coordinates.ClipY == RenderHardware::ClipYDirection::Down)
            conversion[1][1] = -1;
        if (coordinates.Depth == RenderHardware::DepthRange::ZeroToOne)
        {
            conversion[2][2] = 0.5f;
            conversion[3][2] = 0.5f;
        }
        return conversion * projection;
    }

    glm::mat4 DeviceTextureProjection(const glm::mat4& projection, RenderHardware::CoordinateConventions coordinates)
    {
        glm::mat4 origin(1);
        if (coordinates.Origin == RenderHardware::FramebufferOrigin::UpperLeft)
            origin[1][1] = -1;
        return origin * DeviceProjection(projection, coordinates);
    }

    uint32_t DeviceRow(uint32_t row, uint32_t height, RenderHardware::CoordinateConventions coordinates)
    {
        return coordinates.Origin == RenderHardware::FramebufferOrigin::LowerLeft ? row : height - row - 1;
    }
} // namespace Kans

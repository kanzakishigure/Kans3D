#include "kspch.h"
#include "VfxImporter.h"
#include <cstring>
#include "Kans3D/Asset/Product/MeshProduct.h"
#include "Kans3D/Asset/Product/TextureProduct.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "Kans3D/VFX/VfxCompiler.h"
namespace Kans
{
    namespace
    {
        YAML::Node ReadVfx(const std::filesystem::path& path)
        {
            auto bytes = ReadProductFile(path, 128 * 1024 * 1024);
            if (auto e = std::get_if<AssetError>(&bytes))
                throw std::invalid_argument(e->Message);
            const auto& b = std::get<std::vector<std::byte>>(bytes);
            return YAML::Load(std::string(reinterpret_cast<const char*>(b.data()), b.size()));
        }
        void Dependencies(const YAML::Node&                               n,
                          ImportInputs&                                   inputs,
                          const std::map<AssetID, std::filesystem::path>& builtins)
        {
            if (n.IsMap())
                for (const auto& p : n)
                {
                    auto key = p.first.as<std::string>();
                    if (p.second.IsScalar() &&
                        (key == "Script" || key == "Function" || key == "Reference" || key == "Parent" ||
                         key == "Material" || key == "Mesh" || key == "Asset"))
                    {
                        auto id = AssetID::FromString(p.second.as<std::string>());
                        if (!id.IsValid())
                            continue;
                        if (auto b = builtins.find(id); b != builtins.end())
                        {
                            if (inputs.BuiltinSourceFiles.emplace(b->second, id.GetSourceID()).second)
                                Dependencies(ReadVfx(b->second), inputs, builtins);
                        }
                        else
                            inputs.ProductsWithDependencies[id] =
                                key == "Script" || key == "Function"  ? AssetType::VfxModule :
                                key == "Reference" || key == "Parent" ? AssetType::VfxEmitter :
                                key == "Material"                     ? AssetType::Material :
                                key == "Mesh"                         ? AssetType::StaticMesh :
                                                (n["Kind"] && n["Kind"].as<std::string>() != "Texture2D" ?
                                                     AssetType::VfxModule :
                                                     AssetType::Texture);
                    }
                    else
                        Dependencies(p.second, inputs, builtins);
                }
            else if (n.IsSequence())
                for (const auto& p : n)
                    Dependencies(p, inputs, builtins);
        }
    } // namespace
    std::variant<ImportInputs, AssetError> VfxImporter::DiscoverInputs(const ImportContext& c) const
    {
        try
        {
            auto         n = ReadVfx(c.SourceFile);
            ImportInputs inputs;
            Dependencies(n, inputs, Vfx::FindBuiltinModules());
            return inputs;
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, e.what()};
        }
    }
    std::variant<SourceInventory, AssetError> VfxImporter::Inspect(const ImportContext& c) const
    {
        try
        {
            auto n   = ReadVfx(c.SourceFile);
            auto ext = c.SourceFile.extension().string();
            if (ext == ".vfxsystem")
                Vfx::DecodeSystem(n);
            else if (ext == ".vfxemitter")
                Vfx::DecodeEmitter(n);
            else
                Vfx::DecodeModule(n);
            return SourceInventory {};
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, e.what()};
        }
    }
    std::variant<ImportResult, AssetError> VfxImporter::Import(const ImportContext& c) const
    {
        if (!c.SourceID.IsValid() || !c.SourceFile.is_absolute() || !c.StagingDirectory.is_absolute() ||
            !c.Settings.empty())
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid VFX import context"};
        try
        {
            auto status = std::filesystem::symlink_status(c.StagingDirectory);
            if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status) ||
                !std::filesystem::is_empty(c.StagingDirectory))
                return AssetError {AssetErrorCode::InvalidArgument, "VFX staging must be empty and exclusive"};
            auto                          n   = ReadVfx(c.SourceFile);
            auto                          ext = c.SourceFile.extension().string();
            AssetType                     type;
            std::string                   key;
            std::map<AssetID, YAML::Node> dependencies;
            for (const auto& [id, bytes] : c.ProductBytes)
            {
                try
                {
                    auto product = YAML::Load(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                    if (!product.IsMap())
                        continue;
                    dependencies[id] = product;
                    for (const auto& script : product["BuiltinModules"])
                        dependencies[AssetID::FromString(script.first.as<std::string>())] = script.second;
                }
                catch (const YAML::Exception&)
                {}
            }
            // Only declared built-in inputs are read. Players decode the compiled product directly.
            for (const auto& [path, id] : c.SourceInputs)
                if (path.extension() == ".vfxmodule")
                {
                    auto module = Vfx::DecodeModule(ReadVfx(path));
                    if (module.Identity.IsValid())
                        dependencies[module.Identity] = Vfx::EncodeModule(module);
                }
            auto lookup = [&](AssetID id) {
                auto p = dependencies.find(id);
                if (p == dependencies.end())
                    throw std::invalid_argument("Missing VFX dependency: " + id.ToString());
                return p->second;
            };
            std::function<void(YAML::Node)> bake = [&](YAML::Node node) {
                if (node.IsMap())
                {
                    if (node["Kind"] && node["Asset"])
                    {
                        auto kind = node["Kind"].as<std::string>();
                        auto id   = AssetID::FromString(node["Asset"].as<std::string>());
                        if (id.IsValid() && (kind == "Curve" || kind == "Gradient" || kind == "Texture2D"))
                        {
                            Vfx::InterfaceBinding binding;
                            binding.Kind  = kind;
                            binding.Asset = id;
                            if (kind == "Texture2D")
                            {
                                auto at = c.ProductBytes.find(id);
                                if (at == c.ProductBytes.end())
                                    throw std::invalid_argument("Missing VFX texture " + id.ToString());
                                auto decoded = DecodeTextureProduct(at->second.data(), at->second.size());
                                if (auto e = std::get_if<AssetError>(&decoded))
                                    throw std::invalid_argument(e->Message);
                                const auto& texture = std::get<TextureData>(decoded);
                                if (texture.Kind != TextureKind::Texture2D)
                                    throw std::invalid_argument("VFX Texture2D interface requires a 2D texture");
                                binding.Width       = texture.Width;
                                binding.Height      = texture.Height;
                                uint32_t components = texture.Format == TexturePixelFormat::R8  ? 1 :
                                                      texture.Format == TexturePixelFormat::RG8 ? 2 :
                                                                                                  4;
                                auto&    pixels     = texture.Mips.at(0).Pixels;
                                for (uint32_t j = 0; j < texture.Width * texture.Height; ++j)
                                {
                                    glm::vec4 value(0, 0, 0, 1);
                                    for (uint32_t k = 0; k < components; ++k)
                                    {
                                        if (texture.Format == TexturePixelFormat::RGBA32Float)
                                            std::memcpy(&value[k], pixels.data() + (j * 4 + k) * 4, 4);
                                        else
                                            value[k] = float(uint8_t(pixels[j * components + k])) / 255.f;
                                    }
                                    if (texture.ColorSpace == TextureColorSpace::SRGB)
                                        for (int k = 0; k < 3; ++k)
                                            value[k] = value[k] <= .04045f ?
                                                           value[k] / 12.92f :
                                                           std::pow((value[k] + .055f) / 1.055f, 2.4f);
                                    binding.Values.push_back(value);
                                }
                            }
                            else
                            {
                                auto source = Vfx::DecodeModule(lookup(id));
                                auto at = std::find_if(source.Nodes.begin(), source.Nodes.end(), [&](const auto& v) {
                                    return v.Op == "Sample" && v.Interface.Kind == kind && !v.Interface.Values.empty();
                                });
                                if (at == source.Nodes.end())
                                    throw std::invalid_argument("Referenced VFX curve has no sample table: " +
                                                                id.ToString());
                                binding       = at->Interface;
                                binding.Asset = id;
                            }
                            auto baked = Vfx::EncodeInterface(binding);
                            for (const auto& field : baked)
                                node[field.first.as<std::string>()] = field.second;
                        }
                    }
                    for (auto field : node)
                        bake(field.second);
                }
                else if (node.IsSequence())
                    for (auto child : node)
                        bake(child);
            };
            bake(n);
            for (auto& [id, node] : dependencies)
                bake(node);
            if (ext == ".vfxsystem")
            {
                auto s        = Vfx::DecodeSystem(n);
                auto compiled = Vfx::Compile(
                    s,
                    [&](AssetID id) { return Vfx::DecodeEmitter(lookup(id)); },
                    [&](AssetID id) { return Vfx::DecodeModule(lookup(id)); },
                    AssetID(c.SourceID, 1),
                    [&](AssetID id) {
                        auto at = c.ProductBytes.find(id);
                        if (at == c.ProductBytes.end())
                            throw std::invalid_argument("Missing mesh asset");
                        auto decoded = DecodeMeshProduct(at->second.data(), at->second.size());
                        if (auto error = std::get_if<AssetError>(&decoded))
                            throw std::invalid_argument(error->Message);
                        return std::get<MeshData>(std::move(decoded));
                    });
                if (!compiled)
                    return AssetError {AssetErrorCode::InvalidData, Vfx::FormatDiagnostics(compiled.Errors)};
                n    = Vfx::EncodeProduct(compiled.Product);
                type = AssetType::VfxSystem;
                key  = "vfxsystem:root";
            }
            else if (ext == ".vfxemitter")
            {
                auto e = Vfx::DecodeEmitter(n);
                Vfx::MergeEmitter(e, [&](AssetID id) { return Vfx::DecodeEmitter(lookup(id)); });
                n    = Vfx::EncodeEmitter(e);
                type = AssetType::VfxEmitter;
                key  = "vfxemitter:root";
            }
            else
            {
                n    = Vfx::EncodeModule(Vfx::DecodeModule(n));
                type = AssetType::VfxModule;
                key  = "vfxmodule:root";
            }
            if (type != AssetType::VfxSystem)
            {
                YAML::Node builtin(YAML::NodeType::Map);
                for (const auto& [id, node] : dependencies)
                    if (node["Usage"] && node["AssetID"] &&
                        AssetID::FromString(node["AssetID"].as<std::string>()).IsValid())
                        builtin[id.ToString()] = node;
                n["BuiltinModules"] = builtin;
            }
            auto ids = AllocateLocalIDs(c.PreviousLocalIDs, {key});
            if (auto e = std::get_if<AssetError>(&ids))
                return *e;
            ImportResult out;
            out.LocalIDs            = std::get<LocalIDMap>(ids);
            auto              local = out.LocalIDs.at(key);
            ProductDescriptor p {local, type, std::to_string(local) + ".kvfx", {}};
            auto              discovered = DiscoverInputs(c);
            if (auto e = std::get_if<AssetError>(&discovered))
                return *e;
            for (const auto& [id, t] : std::get<ImportInputs>(discovered).ProductsWithDependencies)
                p.Dependencies.push_back(id);
            // Preserve transitive build dependencies in the product graph for hot reload.
            for (const auto& [id, bytes] : c.ProductBytes)
                if (std::find(p.Dependencies.begin(), p.Dependencies.end(), id) == p.Dependencies.end())
                    p.Dependencies.push_back(id);
            auto                   text = YAML::Dump(n);
            std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(text.data()),
                                         reinterpret_cast<const std::byte*>(text.data() + text.size()));
            auto                   written = WriteProductFile(c.StagingDirectory / p.StagedFile, bytes);
            if (auto e = std::get_if<AssetError>(&written))
                return *e;
            out.Products.push_back(p);
            return out;
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, e.what()};
        }
    }
} // namespace Kans

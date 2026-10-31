#include "kspch.h"
#include "MeshProduct.h"

#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace Kans
{
    namespace
    {
        constexpr uint32_t kMagic        = 0x48534d4b; // KMSH, little endian
        constexpr uint32_t kVersion      = 8;
        constexpr size_t   kMaximumBytes = 512 * 1024 * 1024;
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                      "Mesh Products require IEEE 754 binary32");

        template<typename T>
        void Write(std::vector<std::byte>& out, T value)
        {
            for (size_t i = 0; i < sizeof(T); ++i)
                out.push_back(static_cast<std::byte>((value >> (i * 8)) & 255));
        }
        void WriteFloat(std::vector<std::byte>& out, float value)
        {
            uint32_t bits;
            std::memcpy(&bits, &value, 4);
            Write(out, bits);
        }
        template<class T>
        void WriteFloats(std::vector<std::byte>& out, const T& values)
        {
            for (float value : values)
                WriteFloat(out, value);
        }
        void WriteBounds(std::vector<std::byte>& out, const Bounds& bounds)
        {
            for (int i = 0; i < 3; ++i)
                WriteFloat(out, bounds.Min[i]);
            for (int i = 0; i < 3; ++i)
                WriteFloat(out, bounds.Max[i]);
        }
        void WriteID(std::vector<std::byte>& out, AssetID id)
        {
            Write(out, id.GetSourceID().GetUUID());
            Write(out, id.GetLocalID());
        }
        void WriteString(std::vector<std::byte>& out, const std::string& value)
        {
            Write(out, uint32_t(value.size()));
            for (unsigned char c : value)
                out.push_back(std::byte(c));
        }
        struct Reader
        {
            const std::byte* Cursor;
            size_t           Remaining;
            template<class T>
            T Read()
            {
                if (Remaining < sizeof(T))
                    throw std::runtime_error("Truncated Mesh Product");
                T value = 0;
                for (size_t i = 0; i < sizeof(T); ++i)
                    value |= static_cast<T>(std::to_integer<uint8_t>(*Cursor++)) << (i * 8);
                Remaining -= sizeof(T);
                return value;
            }
            float Float()
            {
                auto  bits = Read<uint32_t>();
                float value;
                std::memcpy(&value, &bits, 4);
                return value;
            }
            template<class T>
            void Floats(T& values)
            {
                for (float& value : values)
                    value = Float();
            }
            Bounds Box()
            {
                Bounds bounds;
                for (int i = 0; i < 3; ++i)
                    bounds.Min[i] = Float();
                for (int i = 0; i < 3; ++i)
                    bounds.Max[i] = Float();
                return bounds;
            }
            AssetID ID()
            {
                const auto         source = Read<uint64_t>(), local = Read<uint64_t>();
                std::ostringstream id;
                id << std::hex << std::setw(16) << std::setfill('0') << source;
                return AssetID(SourceAssetID::FromString(id.str()), local);
            }
            std::string String()
            {
                auto count = Read<uint32_t>();
                if (count > 4096 || count > Remaining)
                    throw std::runtime_error("Invalid Mesh Product string");
                std::string result(reinterpret_cast<const char*>(Cursor), count);
                Cursor += count;
                Remaining -= count;
                return result;
            }
        };
    } // namespace

    std::variant<std::vector<std::byte>, AssetError> EncodeMeshProduct(const MeshData& mesh)
    {
        auto valid = ValidateMeshData(mesh);
        if (auto* error = std::get_if<AssetError>(&valid))
            return *error;
        size_t size = 56 + mesh.Vertices.size() * 64 + mesh.Indices.size() * 4 + mesh.Sections.size() * 36;
        for (const auto& slot : mesh.MaterialSlots)
            size += 40 + slot.Name.size() + slot.ObjectKey.size();
        std::vector<std::byte> bytes;
        bytes.reserve(size);
        Write(bytes, kMagic);
        Write(bytes,
              !mesh.ImportedClothMaxDistance.empty()         ? kVersion :
              mesh.HasSmoothNormals()                        ? 7u :
              !mesh.MorphTargets.empty()                     ? 6u :
              mesh.VertexFormat == MeshVertexFormat::Skinned ? 5u :
                                                               3u);
        Write(bytes, static_cast<uint32_t>(AssetType::StaticMesh));
        Write(bytes, static_cast<uint32_t>(mesh.VertexFormat));
        Write(bytes, uint32_t(mesh.Vertices.size()));
        Write(bytes, uint32_t(mesh.Indices.size()));
        Write(bytes, uint32_t(mesh.Sections.size()));
        Write(bytes, uint32_t(mesh.MaterialSlots.size()));
        WriteBounds(bytes, mesh.LocalBounds);
        for (const auto& vertex : mesh.Vertices)
        {
            WriteFloats(bytes, vertex.Position);
            WriteFloats(bytes, vertex.Normal);
            WriteFloats(bytes, vertex.UV);
            WriteFloats(bytes, vertex.Color);
            WriteFloats(bytes, vertex.Tangent);
        }
        for (uint32_t index : mesh.Indices)
            Write(bytes, index);
        for (const auto& section : mesh.Sections)
        {
            Write(bytes, section.FirstIndex);
            Write(bytes, section.IndexCount);
            Write(bytes, section.MaterialSlot);
            WriteBounds(bytes, section.LocalBounds);
        }
        for (const auto& slot : mesh.MaterialSlots)
        {
            WriteString(bytes, slot.Name);
            WriteString(bytes, slot.ObjectKey);
            WriteID(bytes, slot.DefaultMaterial);
            WriteID(bytes, slot.AuthoredMaterial);
        }
        if (mesh.VertexFormat == MeshVertexFormat::Skinned)
        {
            WriteID(bytes, mesh.Skeleton);
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    WriteFloat(bytes, mesh.SkinBindTransform[c][r]);
            Write(bytes, uint32_t(mesh.JointBounds.size()));
            for (const auto& v : mesh.Skin)
            {
                for (auto j : v.Joints)
                    Write(bytes, j);
                WriteFloats(bytes, v.Weights);
            }
            for (const auto& b : mesh.JointBounds)
            {
                Write(bytes, uint32_t(b.IsValid()));
                if (b.IsValid())
                    WriteBounds(bytes, b);
            }
        }
        if (!mesh.MorphTargets.empty() || mesh.HasSmoothNormals() || !mesh.ImportedClothMaxDistance.empty())
        {
            Write(bytes, uint32_t(mesh.MorphTargets.size()));
            for (const auto& t : mesh.MorphTargets)
            {
                WriteString(bytes, t.Name);
                WriteFloat(bytes, t.DefaultWeight);
                WriteBounds(bytes, t.PositionBounds);
                Write(bytes, uint32_t(t.Deltas.size()));
                for (const auto& d : t.Deltas)
                {
                    Write(bytes, d.Vertex);
                    WriteFloats(bytes, d.Position);
                    WriteFloats(bytes, d.Normal);
                    WriteFloats(bytes, d.Tangent);
                }
            }
        }
        if (mesh.HasSmoothNormals() || !mesh.ImportedClothMaxDistance.empty())
        {
            Write(bytes, uint32_t(mesh.SmoothNormals.size()));
            for (const auto& n : mesh.SmoothNormals)
                WriteFloats(bytes, n);
        }
        if (!mesh.ImportedClothMaxDistance.empty())
        {
            Write(bytes, uint32_t(mesh.ImportedClothMaxDistance.size()));
            WriteFloats(bytes, mesh.ImportedClothMaxDistance);
        }
        return bytes;
    }

    std::variant<MeshData, AssetError> DecodeMeshProduct(const std::byte* bytes, size_t size)
    {
        if (!bytes || size < 56 || size > kMaximumBytes)
            return AssetError {AssetErrorCode::InvalidData, "Invalid Mesh Product byte length"};
        try
        {
            Reader reader {bytes, size};
            if (reader.Read<uint32_t>() != kMagic)
                return AssetError {AssetErrorCode::InvalidData, "Invalid Mesh Product magic"};
            const auto version = reader.Read<uint32_t>();
            if (version != kVersion && version != 3 && version != 5 && version != 6 && version != 7)
                return AssetError {AssetErrorCode::UnsupportedVersion, "Unsupported Mesh Product version"};
            if (reader.Read<uint32_t>() != static_cast<uint32_t>(AssetType::StaticMesh))
                return AssetError {AssetErrorCode::InvalidData, "Invalid Mesh Product type"};
            MeshData mesh;
            mesh.VertexFormat   = static_cast<MeshVertexFormat>(reader.Read<uint32_t>());
            const auto vertices = reader.Read<uint32_t>(), indices = reader.Read<uint32_t>();
            const auto sections = reader.Read<uint32_t>(), slots = reader.Read<uint32_t>();
            if (!vertices || !indices || !sections || !slots || sections > 1000000 || slots > 1000000 ||
                56ull + vertices * 64ull + indices * 4ull + sections * 36ull + slots * 40ull > size)
                return AssetError {AssetErrorCode::InvalidData, "Invalid Mesh Product counts"};
            mesh.LocalBounds = reader.Box();
            mesh.Vertices.resize(vertices);
            mesh.Indices.resize(indices);
            mesh.Sections.resize(sections);
            mesh.MaterialSlots.resize(slots);
            for (auto& vertex : mesh.Vertices)
            {
                reader.Floats(vertex.Position);
                reader.Floats(vertex.Normal);
                reader.Floats(vertex.UV);
                reader.Floats(vertex.Color);
                reader.Floats(vertex.Tangent);
            }
            for (auto& index : mesh.Indices)
                index = reader.Read<uint32_t>();
            for (auto& section : mesh.Sections)
            {
                section.FirstIndex   = reader.Read<uint32_t>();
                section.IndexCount   = reader.Read<uint32_t>();
                section.MaterialSlot = reader.Read<uint32_t>();
                section.LocalBounds  = reader.Box();
            }
            for (auto& slot : mesh.MaterialSlots)
            {
                slot.Name             = reader.String();
                slot.ObjectKey        = reader.String();
                slot.DefaultMaterial  = reader.ID();
                slot.AuthoredMaterial = reader.ID();
            }
            if (version == 5 || (version >= 6 && mesh.VertexFormat == MeshVertexFormat::Skinned))
            {
                if (mesh.VertexFormat != MeshVertexFormat::Skinned)
                    throw std::runtime_error("Invalid skinned mesh format");
                mesh.Skeleton = reader.ID();
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        mesh.SkinBindTransform[c][r] = reader.Float();
                const auto joints = reader.Read<uint32_t>();
                if (!joints || joints > 65536 || vertices * 32ull + joints * 4ull > reader.Remaining)
                    throw std::runtime_error("Invalid skin counts");
                mesh.Skin.resize(vertices);
                mesh.JointBounds.resize(joints);
                for (auto& v : mesh.Skin)
                {
                    for (auto& j : v.Joints)
                        j = reader.Read<uint32_t>();
                    reader.Floats(v.Weights);
                }
                for (auto& b : mesh.JointBounds)
                {
                    const auto valid = reader.Read<uint32_t>();
                    if (valid > 1)
                        throw std::runtime_error("Invalid joint bounds");
                    if (valid)
                        b = reader.Box();
                }
            }
            else if (mesh.VertexFormat != MeshVertexFormat::PositionNormalUVColorTangent)
                throw std::runtime_error("Invalid legacy mesh format");
            if (version >= 6)
            {
                const auto count = reader.Read<uint32_t>();
                if (count > 65536 || count * 36ull > reader.Remaining)
                    throw std::runtime_error("Invalid morph target count");
                mesh.MorphTargets.resize(count);
                for (auto& t : mesh.MorphTargets)
                {
                    t.Name            = reader.String();
                    t.DefaultWeight   = reader.Float();
                    t.PositionBounds  = reader.Box();
                    const auto deltas = reader.Read<uint32_t>();
                    if (deltas > vertices || deltas * 40ull > reader.Remaining)
                        throw std::runtime_error("Invalid sparse morph count");
                    t.Deltas.resize(deltas);
                    for (auto& d : t.Deltas)
                    {
                        d.Vertex = reader.Read<uint32_t>();
                        reader.Floats(d.Position);
                        reader.Floats(d.Normal);
                        reader.Floats(d.Tangent);
                    }
                }
            }
            if (version >= 7)
            {
                const auto count = reader.Read<uint32_t>();
                if ((count && count != vertices) || count * 12ull > reader.Remaining)
                    throw std::runtime_error("Invalid smooth normal stream count");
                mesh.SmoothNormals.resize(count);
                for (auto& n : mesh.SmoothNormals)
                    reader.Floats(n);
            }
            if (version >= 8)
            {
                const auto count = reader.Read<uint32_t>();
                if (count != vertices || count * 4ull > reader.Remaining)
                    throw std::runtime_error("Invalid cloth distance stream count");
                mesh.ImportedClothMaxDistance.resize(count);
                reader.Floats(mesh.ImportedClothMaxDistance);
            }
            if (reader.Remaining)
                return AssetError {AssetErrorCode::InvalidData, "Trailing Mesh Product bytes"};
            // Source compatibility for the editor's single-part API.
            mesh.Material         = mesh.MaterialSlots.front().DefaultMaterial;
            mesh.AuthoredMaterial = mesh.MaterialSlots.front().AuthoredMaterial;
            auto valid            = ValidateMeshData(mesh);
            if (auto* error = std::get_if<AssetError>(&valid))
                return *error;
            return mesh;
        }
        catch (const std::runtime_error& error)
        {
            return AssetError {AssetErrorCode::InvalidData, error.what()};
        }
    }
} // namespace Kans

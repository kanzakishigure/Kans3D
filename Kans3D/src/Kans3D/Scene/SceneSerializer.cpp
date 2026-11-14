#include "kspch.h"
#include "SceneSerializer.h"
#include <cmath>
#include <cstring>
#include <functional>
#include <set>
#include <yaml-cpp/yaml.h>
#include "Kans3D/Animation/AnimationProduct.h"
#include "Kans3D/Animation/DeformationProduct.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "Kans3D/Physics/PhysicsSerialization.h"
#include "Kans3D/UI/UIAsset.h"
#include "Entity.h"
namespace Kans
{
    namespace
    {
        constexpr uint64_t SceneBytes    = 64ull * 1024 * 1024;
        constexpr uint64_t EditableBytes = 32ull * 1024 * 1024;
        void               Require(bool valid, const char* message)
        {
            if (!valid)
                throw std::runtime_error(message);
        }
        struct SceneAllocationBudget
        {
            uint64_t Nodes = 0, TextBytes = 0, MeshBytes = 0;
            void     AddEditable(uint64_t bytes, uint64_t attributes, uint64_t materials)
            {
                Require(bytes <= EditableBytes - MeshBytes, "Scene editable payload byte limit");
                MeshBytes += bytes;
                Nodes += 16 + attributes * 16 + materials;
                Require(Nodes <= 1024 * 1024, "Scene aggregate allocation limit");
            }
            void AddEditable(const EditableMeshComponent& component)
            {
                uint64_t bytes      = component.Mesh.Positions.size() * 12ull + component.Mesh.Faces.size() * 4ull;
                uint64_t attributes = 0;
                for (const auto& face : component.Mesh.Faces)
                {
                    bytes += face.Indices.size() * 4ull;
                    if (face.Material || face.SmoothingGroup || !(face.UV == FaceUV {}))
                        ++attributes;
                }
                AddEditable(bytes, attributes, component.Materials.size());
            }
            void Add(size_t textBytes, const ProductModelComponent* model)
            {
                Nodes += 64;
                TextBytes += textBytes;
                if (model)
                {
                    auto valid = ValidateProductModelComponent(*model);
                    if (auto error = std::get_if<AssetError>(&valid))
                        throw std::runtime_error(error->Message);
                    Nodes += model->Overrides.size() * 8;
                }
                Require(Nodes <= 1024 * 1024 && TextBytes <= SceneBytes, "Scene aggregate allocation limit");
            }
        };
        void Keys(const YAML::Node& node, std::initializer_list<const char*> allowed)
        {
            Require(node.IsMap() && node.size() <= allowed.size(), "Invalid Scene mapping");
            std::set<std::string> seen;
            for (const auto& pair : node)
            {
                Require(pair.first.IsScalar(), "Non-scalar Scene key");
                auto key = pair.first.as<std::string>();
                Require(seen.insert(key).second &&
                            std::any_of(allowed.begin(), allowed.end(), [&](const char* item) { return key == item; }),
                        "Duplicate/unknown Scene field");
            }
        }
        std::string Text(const YAML::Node& node)
        {
            Require(node.IsScalar(), "Missing Scene string");
            auto value = node.as<std::string>();
            Require(value.size() <= 4096 && value.find('\0') == std::string::npos, "Invalid Scene string");
            return value;
        }
        float Number(const YAML::Node& node)
        {
            Require(node.IsScalar(), "Missing Scene number");
            auto value = node.as<float>();
            Require(std::isfinite(value), "Non-finite Scene number");
            return value;
        }
        AudioSourceComponent DecodeAudioSource(const YAML::Node& node)
        {
            Keys(node,
                 {"Clip",
                  "Volume",
                  "Pitch",
                  "PlayOnStart",
                  "Loop",
                  "Spatial",
                  "Pan",
                  "Attenuation",
                  "MinDistance",
                  "MaxDistance",
                  "Rolloff",
                  "DopplerFactor",
                  "ConeInnerAngle",
                  "ConeOuterAngle",
                  "ConeOuterGain"});
            AudioSourceComponent c;
            c.Clip                 = AssetID::FromString(Text(node["Clip"]));
            c.Volume               = Number(node["Volume"]);
            c.Pitch                = Number(node["Pitch"]);
            c.PlayOnStart          = node["PlayOnStart"].as<bool>();
            c.Loop                 = node["Loop"].as<bool>();
            c.Spatial              = node["Spatial"].as<bool>();
            c.Pan                  = Number(node["Pan"]);
            const auto attenuation = node["Attenuation"].as<int>();
            Require(attenuation >= 0 && attenuation <= int(AudioAttenuationModel::Exponential),
                    "Invalid audio attenuation model");
            c.Attenuation    = static_cast<AudioAttenuationModel>(attenuation);
            c.MinDistance    = Number(node["MinDistance"]);
            c.MaxDistance    = Number(node["MaxDistance"]);
            c.Rolloff        = Number(node["Rolloff"]);
            c.DopplerFactor  = Number(node["DopplerFactor"]);
            c.ConeInnerAngle = Number(node["ConeInnerAngle"]);
            c.ConeOuterAngle = Number(node["ConeOuterAngle"]);
            c.ConeOuterGain  = Number(node["ConeOuterGain"]);
            Require(c.Volume >= 0 && c.Volume <= 4 && c.Pitch > 0 && c.Pitch <= 4 && c.Pan >= -1 && c.Pan <= 1 &&
                        c.MinDistance > 0 && c.MaxDistance >= c.MinDistance && c.Rolloff >= 0 && c.DopplerFactor >= 0 &&
                        c.ConeInnerAngle >= 0 && c.ConeInnerAngle <= c.ConeOuterAngle && c.ConeOuterAngle <= 360 &&
                        c.ConeOuterGain >= 0 && c.ConeOuterGain <= 1,
                    "Invalid audio source range");
            return c;
        }
        uint64_t UInt(const YAML::Node& node)
        {
            Require(node.IsScalar(), "Missing Scene integer");
            auto value = node.Scalar();
            Require(!value.empty() && value.size() <= 20 &&
                        std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }),
                    "Invalid unsigned Scene integer");
            return node.as<uint64_t>();
        }
        uint32_t SceneIndex(const YAML::Node& node)
        {
            auto value = UInt(node);
            Require(value <= UINT32_MAX, "Scene index overflow");
            return uint32_t(value);
        }
        glm::vec3 Vec3(const YAML::Node& node)
        {
            Require(node.IsSequence() && node.size() == 3, "Invalid Scene vec3");
            return {Number(node[0]), Number(node[1]), Number(node[2])};
        }
        glm::vec4 Vec4(const YAML::Node& node)
        {
            Require(node.IsSequence() && node.size() == 4, "Invalid Scene color");
            return {Number(node[0]), Number(node[1]), Number(node[2]), Number(node[3])};
        }
        template<typename T>
        YAML::Node Vector(T value, size_t count)
        {
            YAML::Node node;
            for (size_t i = 0; i < count; ++i)
            {
                Require(std::isfinite(value[i]), "Non-finite Scene vector");
                node.push_back(value[i]);
            }
            return node;
        }
        // An empty string is an unassigned reference.
        AssetID Identity(const YAML::Node& node)
        {
            const auto text = Text(node);
            if (text.empty())
                return {};
            auto value = AssetID::FromString(text);
            Require(value.IsValid(), "Invalid Scene Product identity");
            return value;
        }
        std::string IdentityText(AssetID id) { return id.IsValid() ? id.ToString() : std::string(); }
        YAML::Node  EncodeModel(const ProductModelComponent& component)
        {
            auto valid = ValidateProductModelComponent(component);
            if (auto error = std::get_if<AssetError>(&valid))
                throw std::runtime_error(error->Message);
            YAML::Node node;
            node["Model"]     = IdentityText(component.Model);
            node["Overrides"] = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& item : component.Overrides)
            {
                YAML::Node value;
                value["Authored"]    = IdentityText(item.Authored);
                value["Replacement"] = IdentityText(item.Replacement);
                node["Overrides"].push_back(value);
            }
            return node;
        }
        ProductModelComponent DecodeModel(const YAML::Node& node)
        {
            // DrawUniforms was written by an earlier format and is ignored.
            Keys(node, {"Model", "Overrides", "DrawUniforms"});
            ProductModelComponent result;
            result.Model   = Identity(node["Model"]);
            auto overrides = node["Overrides"];
            Require(overrides.IsSequence() && overrides.size() <= 64, "Invalid Scene Material overrides");
            for (const auto& value : overrides)
            {
                Keys(value, {"Authored", "Replacement"});
                result.Overrides.push_back({Identity(value["Authored"]), Identity(value["Replacement"])});
            }
            auto valid = ValidateProductModelComponent(result);
            if (auto error = std::get_if<AssetError>(&valid))
                throw std::runtime_error(error->Message);
            return result;
        }
        YAML::Node EncodeTransform(const TransformComponent& transform)
        {
            YAML::Node value;
            value["Position"] = Vector(transform.Position, 3);
            value["Rotation"] = Vector(transform.Rotation, 3);
            value["Scale"]    = Vector(transform.Scale, 3);
            // Preserve an imported matrix without a TRS round trip.
            if (transform.ImportedMatrix)
            {
                const auto matrix = transform.GetTransform();
                for (size_t c = 0; c < 4; ++c)
                    for (size_t r = 0; r < 4; ++r)
                        value["Matrix"].push_back(matrix[c][r]);
            }
            return value;
        }
        TransformComponent DecodeTransform(const YAML::Node& value)
        {
            Keys(value, {"Position", "Rotation", "Scale", "Matrix"});
            TransformComponent result {Vec3(value["Position"]), Vec3(value["Rotation"]), Vec3(value["Scale"])};
            if (const auto matrix = value["Matrix"])
            {
                Require(matrix.IsSequence() && matrix.size() == 16, "Invalid transform matrix");
                glm::mat4 decoded;
                for (size_t c = 0; c < 4; ++c)
                    for (size_t r = 0; r < 4; ++r)
                        decoded[c][r] = Number(matrix[c * 4 + r]);
                result.ImportedMatrix   = decoded;
                result.ImportedPosition = result.Position;
                result.ImportedRotation = result.Rotation;
                result.ImportedScale    = result.Scale;
            }
            return result;
        }
        YAML::Node EncodeShadows(const ShadowSettings& settings)
        {
            const auto s = settings.Validated();
            YAML::Node n;
            n["Enabled"]       = s.Enabled;
            n["Resolution"]    = s.Resolution;
            n["ConstantBias"]  = s.ConstantBias;
            n["SlopeBias"]     = s.SlopeBias;
            n["NormalOffset"]  = s.NormalOffset;
            n["Filter"]        = uint32_t(s.Filter);
            n["AngularRadius"] = s.AngularRadius;
            n["SourceRadius"]  = s.SourceRadius;
            n["Cascades"]      = s.Cascades;
            n["Lambda"]        = s.Lambda;
            n["MaxDistance"]   = s.MaxDistance;
            n["Blend"]         = s.Blend;
            return n;
        }
        ShadowSettings DecodeShadows(const YAML::Node& n, ShadowSettings s = {})
        {
            if (!n)
                return s;
            Keys(n,
                 {"Enabled",
                  "Resolution",
                  "ConstantBias",
                  "SlopeBias",
                  "NormalOffset",
                  "Filter",
                  "AngularRadius",
                  "SourceRadius",
                  "Cascades",
                  "Lambda",
                  "MaxDistance",
                  "Blend"});
            if (n["Enabled"])
                s.Enabled = n["Enabled"].as<bool>();
            if (n["Resolution"])
                s.Resolution = SceneIndex(n["Resolution"]);
            if (n["ConstantBias"])
                s.ConstantBias = Number(n["ConstantBias"]);
            if (n["SlopeBias"])
                s.SlopeBias = Number(n["SlopeBias"]);
            if (n["NormalOffset"])
                s.NormalOffset = Number(n["NormalOffset"]);
            if (n["Filter"])
                s.Filter = static_cast<ShadowFilter>(SceneIndex(n["Filter"]));
            if (n["AngularRadius"])
                s.AngularRadius = Number(n["AngularRadius"]);
            if (n["SourceRadius"])
                s.SourceRadius = Number(n["SourceRadius"]);
            if (n["Cascades"])
                s.Cascades = SceneIndex(n["Cascades"]);
            if (n["Lambda"])
                s.Lambda = Number(n["Lambda"]);
            if (n["MaxDistance"])
                s.MaxDistance = Number(n["MaxDistance"]);
            if (n["Blend"])
                s.Blend = Number(n["Blend"]);
            return s.Validated();
        }
        YAML::Node EncodeText(const TextComponent& c)
        {
            YAML::Node n;
            n["Text"]           = c.Text;
            n["Font"]           = IdentityText(c.Font);
            n["Language"]       = c.Language;
            n["Size"]           = c.Size;
            n["Color"]          = Vector(c.Color, 4);
            n["Horizontal"]     = uint32_t(c.Alignment.Horizontal);
            n["Vertical"]       = uint32_t(c.Alignment.Vertical);
            n["WrapWidth"]      = c.WrapWidth;
            n["BoxHeight"]      = c.BoxHeight;
            n["LineSpacing"]    = c.LineSpacing;
            n["LetterSpacing"]  = c.LetterSpacing;
            n["Overflow"]       = uint32_t(c.Overflow);
            n["OutlineWidth"]   = c.Outline.Width;
            n["OutlineColor"]   = Vector(c.Outline.Color, 4);
            n["ShadowOffset"]   = Vector(c.Shadow.Offset, 2);
            n["ShadowSoftness"] = c.Shadow.Softness;
            n["ShadowColor"]    = Vector(c.Shadow.Color, 4);
            n["Billboard"]      = uint32_t(c.Billboard);
            n["AlwaysOnTop"]    = c.AlwaysOnTop;
            n["DoubleSided"]    = c.DoubleSided;
            n["CastShadows"]    = c.CastShadows;
            return n;
        }
        TextComponent DecodeText(const YAML::Node& n)
        {
            Keys(n, {"Text",         "Font",         "Language",     "Size",           "Color",         "Horizontal",
                     "Vertical",     "WrapWidth",    "BoxHeight",    "LineSpacing",    "LetterSpacing", "Overflow",
                     "OutlineWidth", "OutlineColor", "ShadowOffset", "ShadowSoftness", "ShadowColor",   "Billboard",
                     "AlwaysOnTop",  "DoubleSided",  "CastShadows"});
            TextComponent c;
            Require(n["Text"].IsScalar(), "Missing text content");
            c.Text = n["Text"].as<std::string>();
            Require(c.Text.size() <= 1024 * 1024 && c.Text.find('\0') == std::string::npos, "Invalid text content");
            c.Font       = Identity(n["Font"]);
            c.Language   = Text(n["Language"]);
            c.Size       = Number(n["Size"]);
            c.Color      = Vec4(n["Color"]);
            const auto h = SceneIndex(n["Horizontal"]), v = SceneIndex(n["Vertical"]), o = SceneIndex(n["Overflow"]),
                       b = SceneIndex(n["Billboard"]);
            Require(h <= 2 && v <= 3 && o <= 3 && b <= 2, "Invalid text enum");
            c.Alignment.Horizontal = TextHorizontalAlignment(h);
            c.Alignment.Vertical   = TextPivotVertical(v);
            c.Overflow             = TextOverflow(o);
            c.Billboard            = TextBillboard(b);
            c.WrapWidth            = Number(n["WrapWidth"]);
            c.BoxHeight            = Number(n["BoxHeight"]);
            c.LineSpacing          = Number(n["LineSpacing"]);
            c.LetterSpacing        = Number(n["LetterSpacing"]);
            c.Outline.Width        = Number(n["OutlineWidth"]);
            c.Outline.Color        = Vec4(n["OutlineColor"]);
            Require(n["ShadowOffset"].IsSequence() && n["ShadowOffset"].size() == 2, "Invalid text shadow offset");
            c.Shadow.Offset   = {Number(n["ShadowOffset"][0]), Number(n["ShadowOffset"][1])};
            c.Shadow.Softness = Number(n["ShadowSoftness"]);
            c.Shadow.Color    = Vec4(n["ShadowColor"]);
            c.AlwaysOnTop     = n["AlwaysOnTop"].as<bool>();
            c.DoubleSided     = n["DoubleSided"].as<bool>();
            c.CastShadows     = n["CastShadows"].as<bool>();
            Require(c.Size > 0 && c.WrapWidth >= 0 && c.BoxHeight >= 0 && c.LineSpacing > 0 && c.Outline.Width >= 0 &&
                        c.Shadow.Softness >= 0,
                    "Invalid text dimensions");
            return c;
        }
        YAML::Node EncodeMesh(const MeshComponent& mesh)
        {
            YAML::Node value;
            value["Mesh"]        = IdentityText(mesh.Mesh);
            value["CastShadows"] = mesh.CastShadows;
            if (mesh.RenderCustomDepth)
                value["RenderCustomDepth"] = true;
            if (mesh.CustomStencil)
                value["CustomStencil"] = uint32_t(mesh.CustomStencil);
            value["ReceiveShadows"] = mesh.ReceiveShadows;
            value["Materials"]      = YAML::Node(YAML::NodeType::Sequence);
            Require(mesh.Materials.size() <= 65536, "Too many mesh materials");
            for (auto material : mesh.Materials)
                value["Materials"].push_back(IdentityText(material));
            if (mesh.Section)
                value["Section"] = *mesh.Section;
            return value;
        }
        MeshComponent DecodeMesh(const YAML::Node& value)
        {
            Keys(value,
                 {"Mesh",
                  "Materials",
                  "Section",
                  "CastShadows",
                  "ReceiveShadows",
                  "RenderCustomDepth",
                  "CustomStencil"});
            MeshComponent mesh;
            mesh.Mesh = Identity(value["Mesh"]);
            if (value["RenderCustomDepth"])
                mesh.RenderCustomDepth = value["RenderCustomDepth"].as<bool>();
            if (value["CustomStencil"])
            {
                const auto stencil = value["CustomStencil"].as<uint32_t>();
                Require(stencil <= 255, "Invalid custom stencil");
                mesh.CustomStencil = uint8_t(stencil);
            }
            if (value["CastShadows"])
                mesh.CastShadows = value["CastShadows"].as<bool>();
            if (value["ReceiveShadows"])
                mesh.ReceiveShadows = value["ReceiveShadows"].as<bool>();
            Require(value["Materials"].IsSequence() && value["Materials"].size() <= 65536, "Invalid mesh materials");
            for (const auto& material : value["Materials"])
                mesh.Materials.push_back(Identity(material));
            if (value["Section"])
                mesh.Section = SceneIndex(value["Section"]);
            return mesh;
        }
        const char* ShapeNames[] =
            {"Cube", "Plane", "Sphere", "Cylinder", "Cone", "Torus", "Stairs", "Arch", "Pipe", "Prism"};
        const std::pair<const char*, float ShapeDesc::*> ShapeFloats[] = {{"Width", &ShapeDesc::Width},
                                                                          {"Depth", &ShapeDesc::Depth},
                                                                          {"Height", &ShapeDesc::Height},
                                                                          {"Radius", &ShapeDesc::Radius},
                                                                          {"MajorRadius", &ShapeDesc::MajorRadius},
                                                                          {"MinorRadius", &ShapeDesc::MinorRadius},
                                                                          {"OuterRadius", &ShapeDesc::OuterRadius},
                                                                          {"Thickness", &ShapeDesc::Thickness},
                                                                          {"Angle", &ShapeDesc::Angle}};
        const std::pair<const char*, int ShapeDesc::*>   ShapeInts[] = {{"Segments", &ShapeDesc::Segments},
                                                                        {"Rings", &ShapeDesc::Rings},
                                                                        {"Sides", &ShapeDesc::Sides},
                                                                        {"HeightSegments", &ShapeDesc::HeightSegments},
                                                                        {"Rows", &ShapeDesc::Rows},
                                                                        {"Columns", &ShapeDesc::Columns},
                                                                        {"Steps", &ShapeDesc::Steps}};
        YAML::Node                                       FlowVector(glm::vec2 v)
        {
            auto node = Vector(v, 2);
            node.SetStyle(YAML::EmitterStyle::Flow);
            return node;
        }
        YAML::Node EncodeShape(ShapeDesc d)
        {
            Sanitize(d);
            const ShapeDesc defaults;
            YAML::Node      node;
            node["Type"] = ShapeNames[int(d.Type)];
            if (d.Size != defaults.Size)
                node["Size"] = Vector(d.Size, 3);
            if (d.Subdivisions != defaults.Subdivisions)
                for (int i = 0; i < 3; ++i)
                    node["Subdivisions"].push_back(d.Subdivisions[i]);
            for (auto [name, member] : ShapeFloats)
                if (d.*member != defaults.*member)
                    node[name] = d.*member;
            for (auto [name, member] : ShapeInts)
                if (d.*member != defaults.*member)
                    node[name] = d.*member;
            if (!d.SideWalls)
                node["SideWalls"] = false;
            node.SetStyle(YAML::EmitterStyle::Flow);
            return node;
        }
        ShapeDesc DecodeShape(const YAML::Node& node)
        {
            Keys(node, {"Type",        "Size",           "Subdivisions", "Width",     "Depth", "Height",   "Radius",
                        "MajorRadius", "MinorRadius",    "OuterRadius",  "Thickness", "Angle", "Segments", "Rings",
                        "Sides",       "HeightSegments", "Rows",         "Columns",   "Steps", "SideWalls"});
            ShapeDesc d;
            auto      type = Text(node["Type"]);
            auto      it   = std::find(std::begin(ShapeNames), std::end(ShapeNames), type);
            Require(it != std::end(ShapeNames), "Unknown shape type");
            d.Type = ShapeType(it - std::begin(ShapeNames));
            if (node["Size"])
                d.Size = Vec3(node["Size"]);
            if (node["Subdivisions"])
            {
                auto values = node["Subdivisions"];
                Require(values.IsSequence() && values.size() == 3, "Invalid shape subdivisions");
                for (int i = 0; i < 3; ++i)
                    d.Subdivisions[i] = values[i].as<int>();
            }
            for (auto [name, member] : ShapeFloats)
                if (node[name])
                    d.*member = Number(node[name]);
            for (auto [name, member] : ShapeInts)
                if (node[name])
                    d.*member = node[name].as<int>();
            if (node["SideWalls"])
                d.SideWalls = node["SideWalls"].as<bool>();
            Sanitize(d);
            return d;
        }
        constexpr char Base64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string    EncodeWords(const std::vector<uint32_t>& words)
        {
            std::string  text;
            const size_t bytes = words.size() * 4;
            text.reserve((bytes + 2) / 3 * 4);
            const auto byte = [&](size_t index) { return (words[index / 4] >> (8 * (index % 4))) & 255u; };
            for (size_t i = 0; i < bytes; i += 3)
            {
                uint32_t value = byte(i) << 16;
                if (i + 1 < bytes)
                    value |= byte(i + 1) << 8;
                if (i + 2 < bytes)
                    value |= byte(i + 2);
                text += Base64Alphabet[value >> 18];
                text += Base64Alphabet[(value >> 12) & 63];
                text += i + 1 < bytes ? Base64Alphabet[(value >> 6) & 63] : '=';
                text += i + 2 < bytes ? Base64Alphabet[value & 63] : '=';
            }
            return text;
        }
        uint64_t PackedBytes(const YAML::Node& node)
        {
            Require(node.IsScalar(), "Missing packed editable array");
            const auto& text = node.Scalar();
            Require(text.size() % 4 == 0 && text.size() <= (EditableBytes + 2) / 3 * 4,
                    "Invalid packed editable array size");
            uint64_t bytes = text.size() / 4 * 3;
            if (!text.empty() && text.back() == '=')
                --bytes;
            if (text.size() > 1 && text[text.size() - 2] == '=')
                --bytes;
            Require(bytes % 4 == 0, "Packed editable array is not a uint32 stream");
            return bytes;
        }
        std::vector<uint32_t> DecodeWords(const YAML::Node& node, uint64_t bytes)
        {
            static const auto table = [] {
                std::array<int, 256> values;
                values.fill(-1);
                for (int i = 0; i < 64; ++i)
                    values[uint8_t(Base64Alphabet[i])] = i;
                return values;
            }();
            const auto&           text = node.Scalar();
            std::vector<uint32_t> words(size_t(bytes / 4), 0);
            size_t                cursor = 0;
            for (size_t i = 0; i < text.size(); i += 4)
            {
                uint32_t value   = 0;
                int      padding = 0;
                for (int j = 0; j < 4; ++j)
                {
                    const auto c = uint8_t(text[i + j]);
                    if (c == '=')
                    {
                        Require(i + 4 == text.size() && j >= 2, "Invalid base64 padding");
                        ++padding;
                        value <<= 6;
                    }
                    else
                    {
                        Require(padding == 0 && table[c] >= 0, "Invalid base64 character");
                        value = (value << 6) | uint32_t(table[c]);
                    }
                }
                Require((padding != 1 || (value & 255) == 0) && (padding != 2 || (value & 65535) == 0),
                        "Noncanonical base64 padding bits");
                for (int j = 0; j < 3 - padding; ++j)
                {
                    Require(cursor < bytes, "Invalid packed editable length");
                    words[cursor / 4] |= ((value >> (16 - 8 * j)) & 255u) << (8 * (cursor % 4));
                    ++cursor;
                }
            }
            Require(cursor == bytes, "Invalid packed editable length");
            return words;
        }
        YAML::Node EncodeEditable(const EditableMeshComponent& c)
        {
            auto error = c.Mesh.Validate();
            if (!error.empty())
                throw std::runtime_error(error);
            Require(c.Materials.size() <= 65536, "Too many editable mesh materials");
            YAML::Node            node;
            std::vector<uint32_t> positions, sizes, indices;
            positions.reserve(c.Mesh.Positions.size() * 3);
            sizes.reserve(c.Mesh.Faces.size());
            for (auto p : c.Mesh.Positions)
                for (int axis = 0; axis < 3; ++axis)
                {
                    uint32_t bits;
                    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
                    std::memcpy(&bits, &p[axis], 4);
                    positions.push_back(bits);
                }
            for (uint32_t f = 0; f < c.Mesh.Faces.size(); ++f)
            {
                const auto& face = c.Mesh.Faces[f];
                sizes.push_back(uint32_t(face.Indices.size()));
                indices.insert(indices.end(), face.Indices.begin(), face.Indices.end());
                if (!face.Material && !face.SmoothingGroup && face.UV == FaceUV {})
                    continue;
                YAML::Node value;
                value["Face"] = f;
                if (face.Material)
                    value["Material"] = face.Material;
                if (face.SmoothingGroup)
                    value["SmoothingGroup"] = face.SmoothingGroup;
                if (face.UV.Scale != glm::vec2(1))
                    value["Scale"] = FlowVector(face.UV.Scale);
                if (face.UV.Offset != glm::vec2(0))
                    value["Offset"] = FlowVector(face.UV.Offset);
                if (face.UV.Rotation != 0)
                    value["Rotation"] = face.UV.Rotation;
                value.SetStyle(YAML::EmitterStyle::Flow);
                node["FaceAttributes"].push_back(value);
            }
            node["Positions"] = EncodeWords(positions);
            node["FaceSizes"] = EncodeWords(sizes);
            node["Indices"]   = EncodeWords(indices);
            if (c.Shape)
                node["Shape"] = EncodeShape(*c.Shape);
            node["Materials"] = YAML::Node(YAML::NodeType::Sequence);
            for (auto material : c.Materials)
                node["Materials"].push_back(IdentityText(material));
            node["Materials"].SetStyle(YAML::EmitterStyle::Flow);
            if (c.RenderCustomDepth)
                node["RenderCustomDepth"] = true;
            if (c.CustomStencil)
                node["CustomStencil"] = uint32_t(c.CustomStencil);
            if (!c.CastShadows)
                node["CastShadows"] = false;
            if (!c.ReceiveShadows)
                node["ReceiveShadows"] = false;
            return node;
        }
        EditableMeshComponent DecodeEditable(const YAML::Node& node, SceneAllocationBudget& budget)
        {
            Keys(node,
                 {"Positions",
                  "FaceSizes",
                  "Indices",
                  "FaceAttributes",
                  "Shape",
                  "Materials",
                  "CastShadows",
                  "RenderCustomDepth",
                  "CustomStencil",
                  "ReceiveShadows"});
            const auto positions = node["Positions"], sizes = node["FaceSizes"], indices = node["Indices"],
                       attributes = node["FaceAttributes"], materials = node["Materials"];
            const auto positionBytes = PackedBytes(positions), sizeBytes = PackedBytes(sizes),
                       indexBytes = PackedBytes(indices);
            Require(positionBytes % 12 == 0 && sizeBytes / 4 <= indexBytes / 12 && materials.IsSequence() &&
                        materials.size() <= 65536 &&
                        (!attributes || (attributes.IsSequence() && attributes.size() <= sizeBytes / 4)),
                    "Invalid packed editable array counts");
            // Charge all decoded bytes before allocating any decoded arrays or polygon storage.
            budget.AddEditable(
                positionBytes + sizeBytes + indexBytes, attributes ? attributes.size() : 0, materials.size());
            const auto faceSizes = DecodeWords(sizes, sizeBytes);
            uint64_t   corners   = 0;
            for (auto count : faceSizes)
            {
                Require(count >= 3 && count <= indexBytes / 4 - corners, "Invalid packed face size");
                corners += count;
            }
            Require(corners == indexBytes / 4, "Packed face sizes do not match indices");
            const auto            positionWords = DecodeWords(positions, positionBytes),
                                  indexWords    = DecodeWords(indices, indexBytes);
            EditableMeshComponent c;
            c.Mesh.Positions.resize(size_t(positionBytes / 12));
            for (size_t i = 0; i < positionWords.size(); ++i)
                std::memcpy(&c.Mesh.Positions[i / 3][i % 3], &positionWords[i], 4);
            for (auto index : indexWords)
                Require(index < c.Mesh.Positions.size(), "Packed face index out of range");
            c.Mesh.Faces.resize(faceSizes.size());
            size_t cursor = 0;
            for (size_t f = 0; f < faceSizes.size(); ++f)
            {
                c.Mesh.Faces[f].Indices.assign(indexWords.begin() + cursor, indexWords.begin() + cursor + faceSizes[f]);
                cursor += faceSizes[f];
            }
            std::set<uint32_t> seen;
            if (attributes)
                for (const auto& value : attributes)
                {
                    Keys(value, {"Face", "Material", "SmoothingGroup", "Scale", "Offset", "Rotation"});
                    auto f = SceneIndex(value["Face"]);
                    Require(f < c.Mesh.Faces.size() && seen.insert(f).second, "Invalid/duplicate face attribute index");
                    auto& face = c.Mesh.Faces[f];
                    if (value["Material"])
                        face.Material = SceneIndex(value["Material"]);
                    if (value["SmoothingGroup"])
                        face.SmoothingGroup = SceneIndex(value["SmoothingGroup"]);
                    const auto vec2 = [&](const YAML::Node& v) {
                        Require(v.IsSequence() && v.size() == 2, "Invalid editable UV vector");
                        return glm::vec2(Number(v[0]), Number(v[1]));
                    };
                    if (value["Scale"])
                        face.UV.Scale = vec2(value["Scale"]);
                    if (value["Offset"])
                        face.UV.Offset = vec2(value["Offset"]);
                    if (value["Rotation"])
                        face.UV.Rotation = Number(value["Rotation"]);
                }
            if (node["Shape"])
                c.Shape = DecodeShape(node["Shape"]);
            for (const auto& material : materials)
                c.Materials.push_back(Identity(material));
            if (node["RenderCustomDepth"])
                c.RenderCustomDepth = node["RenderCustomDepth"].as<bool>();
            if (node["CustomStencil"])
            {
                const auto stencil = node["CustomStencil"].as<uint32_t>();
                Require(stencil <= 255, "Invalid custom stencil");
                c.CustomStencil = uint8_t(stencil);
            }
            if (node["CastShadows"])
                c.CastShadows = node["CastShadows"].as<bool>();
            if (node["ReceiveShadows"])
                c.ReceiveShadows = node["ReceiveShadows"].as<bool>();
            if (auto error = c.Mesh.Validate(); !error.empty())
                throw std::runtime_error(error);
            return c;
        }
        YAML::Node EncodeUIScreen(const UIScreenComponent& c)
        {
            YAML::Node n;
            n["UI"]        = c.UI.ToString();
            n["SortOrder"] = c.SortOrder;
            n["Visible"]   = c.Visible;
            return n;
        }
        YAML::Node EncodeUIWorld(const UIWorldComponent& c)
        {
            Require(std::isfinite(c.WorldSize.x) && std::isfinite(c.WorldSize.y) && c.WorldSize.x > 0 &&
                        c.WorldSize.y > 0 && int(c.Billboard) >= 0 && int(c.Billboard) <= 2,
                    "Invalid world UI size/billboard");
            YAML::Node n;
            n["UI"]          = c.UI.ToString();
            n["WorldSize"]   = UIEncode(c.WorldSize);
            n["SortOrder"]   = c.SortOrder;
            n["Visible"]     = c.Visible;
            n["Billboard"]   = int(c.Billboard);
            n["AlwaysOnTop"] = c.AlwaysOnTop;
            n["DoubleSided"] = c.DoubleSided;
            return n;
        }
        UIScreenComponent DecodeUIScreen(const YAML::Node& n)
        {
            Keys(n, {"UI", "SortOrder", "Visible"});
            UIScreenComponent c;
            c.UI        = Identity(n["UI"]);
            c.SortOrder = n["SortOrder"].as<int>(0);
            c.Visible   = n["Visible"].as<bool>(true);
            return c;
        }
        UIWorldComponent DecodeUIWorld(const YAML::Node& n)
        {
            Keys(n, {"UI", "WorldSize", "Billboard", "AlwaysOnTop", "DoubleSided", "SortOrder", "Visible"});
            UIWorldComponent c;
            c.UI          = Identity(n["UI"]);
            c.WorldSize   = UIVec2(n["WorldSize"], c.WorldSize);
            c.SortOrder   = n["SortOrder"].as<int>(0);
            c.Visible     = n["Visible"].as<bool>(true);
            c.Billboard   = TextBillboard(n["Billboard"].as<int>(0));
            c.AlwaysOnTop = n["AlwaysOnTop"].as<bool>(false);
            c.DoubleSided = n["DoubleSided"].as<bool>(true);
            EncodeUIWorld(c);
            return c;
        }
        YAML::Node EncodePrefab(const PrefabInstanceComponent& prefab)
        {
            YAML::Node value;
            value["Prefab"]    = IdentityText(prefab.Prefab);
            value["DrawOrder"] = prefab.DrawOrder;
            value["Overrides"] = YAML::Node(YAML::NodeType::Sequence);
            Require(prefab.Overrides.size() <= 65536, "Too many prefab overrides");
            for (const auto& [key, change] : prefab.Overrides)
            {
                Require(!key.empty() && key.size() <= 4096, "Invalid prefab node key");
                if (change.Empty())
                    continue;
                YAML::Node item;
                item["Node"]    = key;
                item["Removed"] = change.Removed;
                if (change.RenderCustomDepth)
                    item["RenderCustomDepth"] = *change.RenderCustomDepth;
                if (change.CustomStencil)
                    item["CustomStencil"] = uint32_t(*change.CustomStencil);
                if (change.RigidBody)
                    item["RigidBodyComponent"] = PhysicsYaml::Encode(*change.RigidBody);
                if (change.BoxCollider)
                    item["BoxColliderComponent"] = PhysicsYaml::Encode(*change.BoxCollider);
                if (change.SphereCollider)
                    item["SphereColliderComponent"] = PhysicsYaml::Encode(*change.SphereCollider);
                if (change.CapsuleCollider)
                    item["CapsuleColliderComponent"] = PhysicsYaml::Encode(*change.CapsuleCollider);
                if (change.MeshCollider)
                    item["MeshColliderComponent"] = PhysicsYaml::Encode(*change.MeshCollider);
                if (change.CharacterController)
                    item["CharacterControllerComponent"] = PhysicsYaml::Encode(*change.CharacterController);
                if (change.Joint)
                    item["JointComponent"] = PhysicsYaml::Encode(*change.Joint);
                if (change.GlobalWind)
                    item["WindZoneComponent"] = PhysicsYaml::Encode(*change.GlobalWind);
                if (change.ForceField)
                    item["ForceFieldComponent"] = PhysicsYaml::Encode(*change.ForceField);
                if (change.Ragdoll)
                    item["RagdollComponent"] = PhysicsYaml::Encode(*change.Ragdoll);
                if (change.Particles)
                    item["ParticleSystemComponent"] = EncodeParticleSystem(*change.Particles);
                if (change.Vehicle)
                    item["VehicleComponent"] = PhysicsYaml::Encode(*change.Vehicle);
                if (change.MotionCollider)
                    item["MotionColliderComponent"] = PhysicsYaml::Encode(*change.MotionCollider);
                if (change.SpringBone)
                    item["SpringBoneComponent"] = PhysicsYaml::Encode(*change.SpringBone);
                if (change.Cloth)
                    item["ClothComponent"] = PhysicsYaml::Encode(*change.Cloth);
                if (change.UIScreen)
                    item["UIScreen"] = EncodeUIScreen(*change.UIScreen);
                if (change.UIWorld)
                    item["UIWorld"] = EncodeUIWorld(*change.UIWorld);
                if (change.LocalTransform)
                    item["Transform"] = EncodeTransform(*change.LocalTransform);
                item["Materials"] = YAML::Node(YAML::NodeType::Sequence);
                for (const auto& [slot, material] : change.Materials)
                {
                    Require(slot < 65536, "Prefab material slot limit");
                    YAML::Node binding;
                    binding["Slot"]     = slot;
                    binding["Material"] = IdentityText(material);
                    item["Materials"].push_back(binding);
                }
                value["Overrides"].push_back(item);
            }
            if (prefab.LegacyPending)
                value["LegacyPending"] = EncodeModel(*prefab.LegacyPending);
            return value;
        }
        PrefabInstanceComponent DecodePrefab(const YAML::Node& value)
        {
            Keys(value, {"Prefab", "DrawOrder", "Overrides", "LegacyPending"});
            PrefabInstanceComponent prefab;
            prefab.Prefab    = Identity(value["Prefab"]);
            prefab.DrawOrder = UInt(value["DrawOrder"]);
            Require(prefab.DrawOrder < UINT64_MAX, "Invalid prefab draw order");
            const auto overrides = value["Overrides"];
            Require(overrides.IsSequence() && overrides.size() <= 65536, "Invalid prefab overrides");
            for (const auto& item : overrides)
            {
                Keys(item,
                     {"Node",
                      "Removed",
                      "Transform",
                      "Materials",
                      "RenderCustomDepth",
                      "CustomStencil",
                      "RigidBodyComponent",
                      "BoxColliderComponent",
                      "SphereColliderComponent",
                      "CapsuleColliderComponent",
                      "MeshColliderComponent",
                      "WindZoneComponent",
                      "ForceFieldComponent",
                      "RagdollComponent",
                      "ParticleSystemComponent",
                      "VehicleComponent",
                      "MotionColliderComponent",
                      "ClothComponent",
                      "SpringBoneComponent",
                      "CharacterControllerComponent",
                      "JointComponent",
                      "UIScreen",
                      "UIWorld"});
                const auto key = Text(item["Node"]);
                Require(!key.empty() && !prefab.Overrides.count(key), "Duplicate/empty prefab override key");
                auto& change   = prefab.Overrides[key];
                change.Removed = item["Removed"].as<bool>();
                if (item["RigidBodyComponent"])
                {
                    RigidBodyComponent c;
                    PhysicsYaml::Decode(item["RigidBodyComponent"], c);
                    change.RigidBody = c;
                }
                if (item["BoxColliderComponent"])
                {
                    BoxColliderComponent c;
                    PhysicsYaml::Decode(item["BoxColliderComponent"], c);
                    change.BoxCollider = c;
                }
                if (item["SphereColliderComponent"])
                {
                    SphereColliderComponent c;
                    PhysicsYaml::Decode(item["SphereColliderComponent"], c);
                    change.SphereCollider = c;
                }
                if (item["CapsuleColliderComponent"])
                {
                    CapsuleColliderComponent c;
                    PhysicsYaml::Decode(item["CapsuleColliderComponent"], c);
                    change.CapsuleCollider = c;
                }
                if (item["MeshColliderComponent"])
                {
                    MeshColliderComponent c;
                    PhysicsYaml::Decode(item["MeshColliderComponent"], c);
                    change.MeshCollider = c;
                }
                if (item["CharacterControllerComponent"])
                {
                    CharacterControllerComponent c;
                    PhysicsYaml::Decode(item["CharacterControllerComponent"], c);
                    change.CharacterController = c;
                }
                if (item["JointComponent"])
                {
                    JointComponent c;
                    PhysicsYaml::Decode(item["JointComponent"], c);
                    change.Joint = c;
                }
                if (item["WindZoneComponent"])
                {
                    WindZoneComponent c;
                    PhysicsYaml::Decode(item["WindZoneComponent"], c);
                    change.GlobalWind = c;
                }
                if (item["ForceFieldComponent"])
                {
                    ForceFieldComponent c;
                    PhysicsYaml::Decode(item["ForceFieldComponent"], c);
                    change.ForceField = c;
                }
                if (item["RagdollComponent"])
                {
                    RagdollComponent c;
                    PhysicsYaml::Decode(item["RagdollComponent"], c);
                    change.Ragdoll = c;
                }
                if (item["ParticleSystemComponent"])
                    change.Particles = DecodeParticleSystem(item["ParticleSystemComponent"]);
                if (item["VehicleComponent"])
                {
                    VehicleComponent c;
                    PhysicsYaml::Decode(item["VehicleComponent"], c);
                    change.Vehicle = c;
                }
                if (item["MotionColliderComponent"])
                {
                    MotionColliderComponent c;
                    PhysicsYaml::Decode(item["MotionColliderComponent"], c);
                    change.MotionCollider = c;
                }
                if (item["SpringBoneComponent"])
                {
                    SpringBoneComponent c;
                    PhysicsYaml::Decode(item["SpringBoneComponent"], c);
                    change.SpringBone = c;
                }
                if (item["ClothComponent"])
                {
                    ClothComponent c;
                    PhysicsYaml::Decode(item["ClothComponent"], c);
                    change.Cloth = c;
                }
                if (item["UIScreen"])
                    change.UIScreen = DecodeUIScreen(item["UIScreen"]);
                if (item["UIWorld"])
                    change.UIWorld = DecodeUIWorld(item["UIWorld"]);
                if (item["RenderCustomDepth"])
                    change.RenderCustomDepth = item["RenderCustomDepth"].as<bool>();
                if (item["CustomStencil"])
                {
                    const auto stencil = item["CustomStencil"].as<uint32_t>();
                    Require(stencil <= 255, "Invalid prefab custom stencil");
                    change.CustomStencil = uint8_t(stencil);
                }
                if (item["Transform"])
                    change.LocalTransform = DecodeTransform(item["Transform"]);
                Require(item["Materials"].IsSequence() && item["Materials"].size() <= 65536,
                        "Invalid prefab slot overrides");
                for (const auto& binding : item["Materials"])
                {
                    Keys(binding, {"Slot", "Material"});
                    const auto slot = SceneIndex(binding["Slot"]);
                    Require(slot < 65536 && !change.Materials.count(slot), "Invalid/duplicate prefab slot");
                    change.Materials[slot] = Identity(binding["Material"]);
                }
            }
            if (value["LegacyPending"])
                prefab.LegacyPending = DecodeModel(value["LegacyPending"]);
            return prefab;
        }
        YAML::Node EncodeBloom(const BloomSettings& settings)
        {
            // Validate before clamping so a corrupt camera cannot silently persist NaNs.
            Require(std::isfinite(settings.Threshold) && std::isfinite(settings.Knee) &&
                        std::isfinite(settings.Clamp) && std::isfinite(settings.Intensity) &&
                        std::isfinite(settings.Scatter) && std::isfinite(settings.DirtIntensity),
                    "Non-finite Scene bloom");
            Vector(settings.Tint, 3);
            const auto b = settings.Validated();
            YAML::Node node;
            node["Enabled"]              = b.Enabled;
            node["Threshold"]            = b.Threshold;
            node["Knee"]                 = b.Knee;
            node["Clamp"]                = b.Clamp;
            node["Intensity"]            = b.Intensity;
            node["Scatter"]              = b.Scatter;
            node["Tint"]                 = Vector(b.Tint, 3);
            node["HighQualityFiltering"] = b.HighQualityFiltering;
            node["Mode"]                 = b.Mode == BloomSettings::EnergyConserving ? "EnergyConserving" : "Additive";
            node["DirtTexture"]          = IdentityText(b.DirtTexture);
            node["DirtIntensity"]        = b.DirtIntensity;
            return node;
        }
        BloomSettings DecodeBloom(const YAML::Node& node)
        {
            Keys(node,
                 {"Enabled",
                  "Threshold",
                  "Knee",
                  "Clamp",
                  "Intensity",
                  "Scatter",
                  "Tint",
                  "HighQualityFiltering",
                  "Mode",
                  "DirtTexture",
                  "DirtIntensity"});
            BloomSettings b;
            if (node["Enabled"])
                b.Enabled = node["Enabled"].as<bool>();
            if (node["Threshold"])
                b.Threshold = Number(node["Threshold"]);
            if (node["Knee"])
                b.Knee = Number(node["Knee"]);
            if (node["Clamp"])
                b.Clamp = Number(node["Clamp"]);
            if (node["Intensity"])
                b.Intensity = Number(node["Intensity"]);
            if (node["Scatter"])
                b.Scatter = Number(node["Scatter"]);
            if (node["Tint"])
                b.Tint = Vec3(node["Tint"]);
            if (node["HighQualityFiltering"])
                b.HighQualityFiltering = node["HighQualityFiltering"].as<bool>();
            if (node["Mode"])
            {
                auto mode = Text(node["Mode"]);
                Require(mode == "Additive" || mode == "EnergyConserving", "Invalid Scene bloom mode");
                b.Mode = mode == "Additive" ? BloomSettings::Additive : BloomSettings::EnergyConserving;
            }
            if (node["DirtTexture"])
                b.DirtTexture = Identity(node["DirtTexture"]);
            if (node["DirtIntensity"])
                b.DirtIntensity = Number(node["DirtIntensity"]);
            return b.Validated();
        }
        YAML::Node EncodePostProcess(const PostProcessSettings& input)
        {
            const auto settings = input.Validated();
            Require(std::isfinite(input.Exposure) && std::isfinite(input.FilterSize) &&
                        uint32_t(input.ToneMapping) <= 1,
                    "Invalid post-process settings");
            YAML::Node node;
            node["Exposure"] = settings.Exposure;
            for (const auto& entry : settings.Materials)
            {
                YAML::Node material;
                material["Material"] = IdentityText(entry.Material);
                if (!entry.Enabled)
                    material["Enabled"] = false;
                node["Materials"].push_back(material);
            }
            if (!(settings.Temporal == TemporalReconstructionSettings {}))
            {
                node["Temporal"]["Sharpness"]           = settings.Temporal.Sharpness;
                node["Temporal"]["ReactiveScale"]       = settings.Temporal.ReactiveScale;
                node["Temporal"]["ReactiveThreshold"]   = settings.Temporal.ReactiveThreshold;
                node["Temporal"]["ReactiveBinaryValue"] = settings.Temporal.ReactiveBinaryValue;
            }
            node["Bloom"]       = EncodeBloom(input.Bloom);
            node["Tonemapping"] = settings.ToneMapping == ToneMappingMode::ACES ? "ACES" : "Standard";
            if (settings.FilterSize != 0)
                node["FilterSize"] = settings.FilterSize;
            return node;
        }
        PostProcessSettings DecodePostProcess(const YAML::Node& node)
        {
            Keys(node, {"Exposure", "Bloom", "Tonemapping", "FilterSize", "Materials", "Temporal"});
            PostProcessSettings settings;
            if (node["Materials"])
            {
                const auto entries = node["Materials"];
                Require(entries.IsSequence() && entries.size() <= 65536, "Invalid post materials");
                for (const auto& entry : entries)
                {
                    Keys(entry, {"Material", "Enabled"});
                    const auto id = Identity(entry["Material"]);
                    Require(id.IsValid(), "Invalid post material identity");
                    settings.Materials.push_back({id, entry["Enabled"] ? entry["Enabled"].as<bool>() : true});
                }
            }
            if (const auto temporal = node["Temporal"])
            {
                Keys(temporal, {"Sharpness", "ReactiveScale", "ReactiveThreshold", "ReactiveBinaryValue"});
                if (temporal["Sharpness"])
                    settings.Temporal.Sharpness = Number(temporal["Sharpness"]);
                if (temporal["ReactiveScale"])
                    settings.Temporal.ReactiveScale = Number(temporal["ReactiveScale"]);
                if (temporal["ReactiveThreshold"])
                    settings.Temporal.ReactiveThreshold = Number(temporal["ReactiveThreshold"]);
                if (temporal["ReactiveBinaryValue"])
                    settings.Temporal.ReactiveBinaryValue = Number(temporal["ReactiveBinaryValue"]);
            }
            if (node["Exposure"])
                settings.Exposure = Number(node["Exposure"]);
            if (node["Bloom"])
                settings.Bloom = DecodeBloom(node["Bloom"]);
            if (node["FilterSize"])
                settings.FilterSize = Number(node["FilterSize"]);
            if (node["Tonemapping"])
            {
                const auto mode = node["Tonemapping"].as<std::string>();
                Require(mode == "ACES" || mode == "Standard", "Invalid tonemapping mode");
                settings.ToneMapping = mode == "ACES" ? ToneMappingMode::ACES : ToneMappingMode::Standard;
            }
            return settings.Validated();
        }

        void ValidateCamera(SceneCamera& camera)
        {
            Require(std::isfinite(static_cast<const Camera&>(camera).GetExposure()) &&
                        std::isfinite(camera.GetAspectRatio()) && camera.GetAspectRatio() > 0 &&
                        std::isfinite(camera.GetPerspectiveVerticalFOV()) && camera.GetPerspectiveVerticalFOV() > 0 &&
                        camera.GetPerspectiveVerticalFOV() < 180 && std::isfinite(camera.GetPerspectiveNearClip()) &&
                        std::isfinite(camera.GetPerspectiveFarClip()) && camera.GetPerspectiveNearClip() > 0 &&
                        camera.GetPerspectiveFarClip() > camera.GetPerspectiveNearClip() &&
                        std::isfinite(camera.GetOrthographicSize()) && camera.GetOrthographicSize() > 0 &&
                        std::isfinite(camera.GetOrthographicNearClip()) &&
                        std::isfinite(camera.GetOrthographicFarClip()) &&
                        camera.GetOrthographicFarClip() > camera.GetOrthographicNearClip() &&
                        (camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective ||
                         camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic),
                    "Invalid Scene camera");
        }
        YAML::Node EncodeEntity(Entity entity, bool clipboard = false)
        {
            Require(entity.HasComponent<TagComponent>() && entity.HasComponent<TransformComponent>(),
                    "Incomplete Scene entity");
            YAML::Node node;
            node["Entity"] = uint64_t(entity.GetComponent<IDComponent>().ID);
            // Written in hierarchy order, so children follow their parent in sibling order.
            if (const UUID parent = entity.GetComponent<RelationshipComponent>().Parent)
                node["Parent"] = uint64_t(parent);
            auto name = entity.GetComponent<TagComponent>().Tag;
            Require(name.size() <= 4096 && name.find('\0') == std::string::npos, "Invalid Scene tag");
            node["TagComponent"]["Tag"] = name;
            const auto& transform       = entity.GetComponent<TransformComponent>();
            auto        value           = node["TransformComponent"];
            value["Position"]           = Vector(transform.Position, 3);
            value["Rotation"]           = Vector(transform.Rotation, 3);
            value["Scale"]              = Vector(transform.Scale, 3);
            node["TransformComponent"]  = EncodeTransform(transform);
            if (clipboard && entity.HasComponent<PrefabOwnedComponent>())
            {
                const auto& owned                        = entity.GetComponent<PrefabOwnedComponent>();
                node["PrefabOwnedComponent"]["Instance"] = uint64_t(owned.Instance);
                node["PrefabOwnedComponent"]["Node"]     = owned.NodeKey;
                node["PrefabOwnedComponent"]["Order"]    = owned.Order;
            }
            if (entity.HasComponent<PrefabInstanceComponent>())
                node["PrefabInstanceComponent"] = EncodePrefab(entity.GetComponent<PrefabInstanceComponent>());
            if (entity.HasComponent<RigidBodyComponent>())
                node["RigidBodyComponent"] = PhysicsYaml::Encode(entity.GetComponent<RigidBodyComponent>());
            if (entity.HasComponent<BoxColliderComponent>())
                node["BoxColliderComponent"] = PhysicsYaml::Encode(entity.GetComponent<BoxColliderComponent>());
            if (entity.HasComponent<SphereColliderComponent>())
                node["SphereColliderComponent"] = PhysicsYaml::Encode(entity.GetComponent<SphereColliderComponent>());
            if (entity.HasComponent<CapsuleColliderComponent>())
                node["CapsuleColliderComponent"] = PhysicsYaml::Encode(entity.GetComponent<CapsuleColliderComponent>());
            if (entity.HasComponent<MeshColliderComponent>())
                node["MeshColliderComponent"] = PhysicsYaml::Encode(entity.GetComponent<MeshColliderComponent>());
            if (entity.HasComponent<CharacterControllerComponent>())
                node["CharacterControllerComponent"] =
                    PhysicsYaml::Encode(entity.GetComponent<CharacterControllerComponent>());
            if (entity.HasComponent<JointComponent>())
                node["JointComponent"] = PhysicsYaml::Encode(entity.GetComponent<JointComponent>());
            if (entity.HasComponent<WindZoneComponent>())
                node["WindZoneComponent"] = PhysicsYaml::Encode(entity.GetComponent<WindZoneComponent>());
            if (entity.HasComponent<ForceFieldComponent>())
                node["ForceFieldComponent"] = PhysicsYaml::Encode(entity.GetComponent<ForceFieldComponent>());
            if (entity.HasComponent<RagdollComponent>())
                node["RagdollComponent"] = PhysicsYaml::Encode(entity.GetComponent<RagdollComponent>());
            if (entity.HasComponent<ParticleSystemComponent>())
                node["ParticleSystemComponent"] = EncodeParticleSystem(entity.GetComponent<ParticleSystemComponent>());
            if (entity.HasComponent<VehicleComponent>())
                node["VehicleComponent"] = PhysicsYaml::Encode(entity.GetComponent<VehicleComponent>());
            if (entity.HasComponent<MotionColliderComponent>())
                node["MotionColliderComponent"] = PhysicsYaml::Encode(entity.GetComponent<MotionColliderComponent>());
            if (entity.HasComponent<SpringBoneComponent>())
                node["SpringBoneComponent"] = PhysicsYaml::Encode(entity.GetComponent<SpringBoneComponent>());
            if (entity.HasComponent<ClothComponent>())
                node["ClothComponent"] = PhysicsYaml::Encode(entity.GetComponent<ClothComponent>());
            if (entity.HasComponent<AudioSourceComponent>())
            {
                const auto& c       = entity.GetComponent<AudioSourceComponent>();
                auto        v       = node["AudioSourceComponent"];
                v["Clip"]           = c.Clip.ToString();
                v["Attenuation"]    = int(c.Attenuation);
                v["Volume"]         = c.Volume;
                v["Pitch"]          = c.Pitch;
                v["PlayOnStart"]    = c.PlayOnStart;
                v["Loop"]           = c.Loop;
                v["Spatial"]        = c.Spatial;
                v["Pan"]            = c.Pan;
                v["MinDistance"]    = c.MinDistance;
                v["MaxDistance"]    = c.MaxDistance;
                v["Rolloff"]        = c.Rolloff;
                v["DopplerFactor"]  = c.DopplerFactor;
                v["ConeInnerAngle"] = c.ConeInnerAngle;
                v["ConeOuterAngle"] = c.ConeOuterAngle;
                v["ConeOuterGain"]  = c.ConeOuterGain;
            }
            if (entity.HasComponent<AudioListenerComponent>())
                node["AudioListenerComponent"]["Active"] = entity.GetComponent<AudioListenerComponent>().Active;
            if (entity.HasComponent<MaterialParameterDriversComponent>())
            {
                auto list = node["MaterialParameterDriversComponent"]["Bindings"];
                list      = YAML::Node(YAML::NodeType::Sequence);
                for (const auto& binding : entity.GetComponent<MaterialParameterDriversComponent>().Bindings)
                {
                    YAML::Node item;
                    item["Parameter"]        = binding.Parameter;
                    item["SourceEntity"]     = uint64_t(binding.SourceEntity);
                    const char* quantities[] = {"WorldPosition", "WorldForward", "WorldUp", "WorldRight"};
                    Require(uint32_t(binding.Quantity) < 4, "Invalid material driver quantity");
                    item["Quantity"] = quantities[uint32_t(binding.Quantity)];
                    list.push_back(item);
                }
            }
            if (entity.HasComponent<IKComponent>())
                node["IKComponent"] = EncodeIKComponent(entity.GetComponent<IKComponent>());
            if (entity.HasComponent<ShapeKeysComponent>())
            {
                const auto& c   = entity.GetComponent<ShapeKeysComponent>();
                auto        v   = node["ShapeKeysComponent"];
                v["Skeleton"]   = c.Skeleton.ToString();
                v["Expression"] = c.Expression.ToString();
                v["Weights"]    = c.Weights;
                Require(c.Weights.size() <= 4096, "Too many shape-key weights");
                for (const auto& [name, value] : c.Weights)
                    Require(!name.empty() && std::isfinite(value), "Invalid shape-key weight");
            }
            if (entity.HasComponent<ActiveComponent>())
                node["ActiveComponent"] = entity.GetComponent<ActiveComponent>().Active;
            if (entity.HasComponent<TimelinePlayerComponent>())
            {
                const auto& c = entity.GetComponent<TimelinePlayerComponent>();
                YAML::Node  v;
                v["Timeline"]    = c.Timeline.ToString();
                v["PlayOnStart"] = c.PlayOnStart;
                v["Loop"]        = c.Loop;
                v["Speed"]       = c.Speed;
                v["WrapMode"]    = c.WrapMode == TimelineWrapMode::Reset ? "Reset" : "Hold";
                v["Bindings"]    = YAML::Node(YAML::NodeType::Map);
                for (const auto& [id, entityID] : c.Bindings)
                    v["Bindings"][id] = uint64_t(entityID);
                node["TimelinePlayerComponent"] = v;
            }
            if (entity.HasComponent<AnimatorComponent>() &&
                (!entity.HasComponent<PrefabInstanceComponent>() ||
                 !entity.GetComponent<PrefabInstanceComponent>().IsGeneratedAnimator(
                     entity.GetComponent<AnimatorComponent>())))
            {
                const auto& a        = entity.GetComponent<AnimatorComponent>();
                auto        v        = node["AnimatorComponent"];
                v["Skeleton"]        = a.Skeleton.ToString();
                v["Controller"]      = a.Controller.ToString();
                v["Enabled"]         = a.Enabled;
                v["ApplyRootMotion"] = a.ApplyRootMotion;
                v["Speed"]           = a.Speed;
                v["Parameters"]      = a.Parameters;
                if (a.InlineController)
                    v["InlineController"] = EncodeAnimationControllerNode(*a.InlineController);
                else if (clipboard && a.ControllerSource)
                    v["InlineController"] = EncodeAnimationControllerNode(a.ControllerSource->GetData());
                if (clipboard)
                {
                    auto joints = v["JointEntities"];
                    joints      = YAML::Node(YAML::NodeType::Sequence);
                    for (UUID joint : a.JointEntities)
                        joints.push_back(uint64_t(joint));
                }
            }
            Require(!(entity.HasComponent<MeshComponent>() && entity.HasComponent<EditableMeshComponent>()),
                    "Mesh components are mutually exclusive");
            if (entity.HasComponent<EditableMeshComponent>())
                node["EditableMeshComponent"] = EncodeEditable(entity.GetComponent<EditableMeshComponent>());
            if (entity.HasComponent<UIScreenComponent>())
                node["UIScreenComponent"] = EncodeUIScreen(entity.GetComponent<UIScreenComponent>());
            if (entity.HasComponent<UIWorldComponent>())
                node["UIWorldComponent"] = EncodeUIWorld(entity.GetComponent<UIWorldComponent>());
            if (entity.HasComponent<TextComponent>())
                node["TextComponent"] = EncodeText(entity.GetComponent<TextComponent>());
            if (entity.HasComponent<MeshComponent>())
                node["MeshComponent"] = EncodeMesh(entity.GetComponent<MeshComponent>());
            if (entity.HasComponent<PrefabAttachmentComponent>())
            {
                const auto& attachment           = entity.GetComponent<PrefabAttachmentComponent>();
                node["PrefabParent"]["Instance"] = uint64_t(attachment.Instance);
                node["PrefabParent"]["Node"]     = attachment.NodeKey;
            }
            if (entity.HasComponent<CameraComponent>())
            {
                auto& component = entity.GetComponent<CameraComponent>();
                auto& camera    = component.SceneCamera;
                ValidateCamera(camera);
                value.reset(node["CameraComponent"]);
                value["ProjectionType"]       = uint32_t(camera.GetProjectionType());
                value["PerspectiveFOV"]       = camera.GetPerspectiveVerticalFOV();
                value["PerspectiveNearClip"]  = camera.GetPerspectiveNearClip();
                value["PerspectiveFarClip"]   = camera.GetPerspectiveFarClip();
                value["OrthographicSize"]     = camera.GetOrthographicSize();
                value["OrthographicNearClip"] = camera.GetOrthographicNearClip();
                value["OrthographicFarClip"]  = camera.GetOrthographicFarClip();
                value["AspectRatio"]          = camera.GetAspectRatio();
                if (camera.GetPostProcessOverride())
                    value["PostProcessOverride"] = EncodePostProcess(*camera.GetPostProcessOverride());
                if (!(camera.GetUpscaling() == UpscalingSettings {}))
                {
                    value["Upscaling"]["Mode"]        = uint32_t(camera.GetUpscaling().Mode);
                    value["Upscaling"]["CustomRatio"] = camera.GetUpscaling().CustomRatio;
                }
                if (camera.GetAntiAliasing() != AntiAliasing::None)
                    value["AntiAliasing"] = uint32_t(camera.GetAntiAliasing());
                value["Primary"]          = component.Primary;
                value["FixedAspectRatio"] = component.FixedAspectRatio;
            }
            if (entity.HasComponent<DirLightComponent>())
            {
                const auto& light = entity.GetComponent<DirLightComponent>();
                value.reset(node["DirLightComponent"]);
                value["Direction"]          = Vector(light.Direction, 3);
                value["Diffuse_Intensity"]  = Vector(light.Diffuse_Intensity, 3);
                value["Specular_Intensity"] = Vector(light.Specular_Intensity, 3);
                value["Ambient_Intensity"]  = Vector(light.Ambient_Intensity, 3);
                value["Shadows"]            = EncodeShadows(light.Shadows);
            }
            if (entity.HasComponent<PointLightComponent>())
            {
                const auto& light = entity.GetComponent<PointLightComponent>();
                value.reset(node["PointLightComponent"]);
                value["Diffuse_Intensity"]  = Vector(light.Diffuse_Intensity, 3);
                value["Specular_Intensity"] = Vector(light.Specular_Intensity, 3);
                value["Ambient_Intensity"]  = Vector(light.Ambient_Intensity, 3);
                value["Shadows"]            = EncodeShadows(light.Shadows);
            }
            if (entity.HasComponent<ScriptComponent>())
            {
                const auto& name = entity.GetComponent<ScriptComponent>().ClassName;
                Require(name.size() <= 4096 && name.find('\0') == std::string::npos, "Invalid Script name");
                node["ScriptComponent"]["ClassName"] = name;
            }
            if (entity.HasComponent<SpriteRendererComponent>())
            {
                const auto& sprite = entity.GetComponent<SpriteRendererComponent>();

                value.reset(node["SpriteRendererComponent"]);
                value["Color"] = Vector(sprite.Color, 4);
                Require(std::isfinite(sprite.TilingFactor) && sprite.TilingFactor >= 0, "Invalid Sprite tiling");
                value["TilingFactor"] = sprite.TilingFactor;
                value["TextureAsset"] = sprite.Texture.ToString();
                value["UVRect"]       = Vector(sprite.UVRect, 4);
                value["FlipX"]        = sprite.FlipX;
                value["FlipY"]        = sprite.FlipY;
                value["SortingOrder"] = sprite.SortingOrder;
            }
            return node;
        }
    } // namespace
    YAML::Node SceneSerializer::SerializeNode(const Scene& scene, bool includeGenerated)
    {
        auto& registry = scene.m_Registry;
        auto  name     = scene.GetName();
        Require(name.size() <= 4096 && name.find('\0') == std::string::npos, "Invalid Scene name");
        YAML::Node root;
        root["Version"]     = 1;
        root["Scene"]       = name;
        root["PostProcess"] = EncodePostProcess(scene.GetPostProcessSettings());
        if (scene.HasEnvironmentSettings())
        {
            const auto& settings    = scene.GetEnvironmentSettings();
            auto        environment = root["Environment"];
            environment["Map"]      = settings.Map.generic_string(); // Empty explicitly disables IBL.
            if (settings.DiffuseIntensity != 1)
                environment["DiffuseIntensity"] = settings.DiffuseIntensity;
            if (settings.SpecularIntensity != 1)
                environment["SpecularIntensity"] = settings.SpecularIntensity;
            if (settings.Rotation != 0)
                environment["Rotation"] = settings.Rotation;
            if (settings.SkyboxIntensity != 1)
                environment["SkyboxIntensity"] = settings.SkyboxIntensity;
            if (!settings.ShowSkybox)
                environment["ShowSkybox"] = false;
            if (settings.BackgroundColor != glm::vec3(0))
                environment["BackgroundColor"] = Vector(settings.BackgroundColor, 3);
        }
        root["Entities"] = YAML::Node(YAML::NodeType::Sequence);
        std::map<uint64_t, Entity> entities;
        SceneAllocationBudget      budget;
        budget.TextBytes = name.size();
        registry.each([&](auto handle) {
            Entity entity {handle, const_cast<Scene*>(&scene)};
            Require(entity.HasComponent<IDComponent, TagComponent, TransformComponent>(), "Incomplete Scene entity");
            if (!includeGenerated && entity.HasComponent<PrefabOwnedComponent>())
                return;
            auto id = uint64_t(entity.GetComponent<IDComponent>().ID);
            Require(id && entities.emplace(id, entity).second && entities.size() <= 65536,
                    "Invalid/duplicate Scene identity");
            auto textBytes = entity.GetComponent<TagComponent>().Tag.size();
            if (entity.HasComponent<TextComponent>())
                textBytes += entity.GetComponent<TextComponent>().Text.size() +
                             entity.GetComponent<TextComponent>().Language.size();
            if (entity.HasComponent<ScriptComponent>())
                textBytes += entity.GetComponent<ScriptComponent>().ClassName.size();
            const auto* prefab = registry.try_get<PrefabInstanceComponent>(handle);
            budget.Add(textBytes, prefab && prefab->LegacyPending ? &*prefab->LegacyPending : nullptr);
            if (const auto* editable = registry.try_get<EditableMeshComponent>(handle))
                budget.AddEditable(*editable);
        });
        // Parents before children, siblings in order.
        size_t                      encoded = 0;
        std::function<void(Entity)> write   = [&](Entity entity) {
            if (includeGenerated || !entity.HasComponent<PrefabOwnedComponent>())
            {
                root["Entities"].push_back(EncodeEntity(entity));
                ++encoded;
            }
            for (Entity child : scene.GetChildren(entity))
                write(child);
        };
        for (Entity entity : scene.GetRootEntities())
            write(entity);
        Require(encoded == entities.size(), "Scene hierarchy does not reach every entity");
        return root;
    }
    SceneSerializer::SceneSerializer(const Ref<Scene>& scene) : m_Scene(scene) {}
    std::variant<std::monostate, AssetError> SceneSerializer::SerializeChecked(const std::filesystem::path& path)
    {
        if (!m_Scene || path.empty())
            return AssetError {AssetErrorCode::InvalidArgument, "Scene/path absent"};
        std::filesystem::path temporary;
        try
        {
            m_Scene->UpdatePrefabs();
            // Capture attachments before filtering out generated entities.
            for (auto handle : m_Scene->m_Registry.view<RelationshipComponent>())
            {
                Entity entity {handle, m_Scene.get()};
                Entity parent = m_Scene->GetParent(entity);
                if (!entity.HasComponent<PrefabOwnedComponent>() && parent &&
                    parent.HasComponent<PrefabOwnedComponent>())
                {
                    const auto& owner = parent.GetComponent<PrefabOwnedComponent>();
                    if (!entity.HasComponent<PrefabAttachmentComponent>())
                        entity.AddComponent<PrefabAttachmentComponent>();
                    entity.GetComponent<PrefabAttachmentComponent>() = {owner.Instance, owner.NodeKey};
                }
            }
            if (!m_Scene->m_Registry.view<NativeScriptComponent>().empty())
                return AssetError {AssetErrorCode::NotImplemented,
                                   "Scenes with native script components cannot be saved"};
            auto          root = SerializeNode(*m_Scene);
            YAML::Emitter output;
            output.SetFloatPrecision(std::numeric_limits<float>::max_digits10);
            output.SetDoublePrecision(std::numeric_limits<double>::max_digits10);
            output << root;
            Require(output.good() && output.size() <= SceneBytes, "Scene serialization exceeds byte limit");
            std::error_code error;
            auto            status = std::filesystem::symlink_status(path, error);
            if (error && error != std::errc::no_such_file_or_directory)
                return AssetError {AssetErrorCode::IoError, error.message()};
            Require(!std::filesystem::is_symlink(status) &&
                        (!std::filesystem::exists(status) || std::filesystem::is_regular_file(status)),
                    "Scene destination not a regular file");
            temporary = path;
            temporary += "." + SourceAssetID::Generate().ToString() + ".tmp";
            auto first   = reinterpret_cast<const std::byte*>(output.c_str());
            auto written = WriteProductFile(temporary, {first, first + output.size()});
            if (auto failure = std::get_if<AssetError>(&written))
            {
                std::filesystem::remove(temporary, error);
                return *failure;
            }
            auto replaced = ReplaceProductCatalog(temporary, path);
            if (auto failure = std::get_if<AssetError>(&replaced))
            {
                std::filesystem::remove(temporary, error);
                return *failure;
            }
            return std::monostate {};
        }
        catch (const std::exception& error)
        {
            if (!temporary.empty())
            {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
            }
            return AssetError {AssetErrorCode::InvalidData, std::string(error.what()).substr(0, 2048)};
        }
    }
    std::variant<Ref<Scene>, AssetError> SceneSerializer::DeserializeChecked(const std::filesystem::path& path)
    {
        auto input = ReadProductFile(path, SceneBytes);
        if (auto error = std::get_if<AssetError>(&input))
            return *error;
        try
        {
            auto& bytes = std::get<std::vector<std::byte>>(input);
            Require(!bytes.empty(), "Empty Scene file");
            const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            Require(text.find('\0') == std::string::npos, "NUL in Scene file");
            auto documents = YAML::LoadAll(text);
            Require(documents.size() == 1, "Scene requires one YAML document");
            auto root = documents.front();
            return DeserializeNodeChecked(root);
        }
        catch (const std::exception& error)
        {
            return AssetError {AssetErrorCode::InvalidData, std::string(error.what()).substr(0, 2048)};
        }
    }
    std::variant<Ref<Scene>, AssetError> SceneSerializer::DeserializeNodeChecked(const YAML::Node& root, bool clipboard)
    {
        try
        {
            Keys(root, {"Version", "Scene", "Entities", "Environment", "PostProcess"});
            Require(UInt(root["Version"]) == 1, "Unsupported Scene version");
            auto entities = root["Entities"];
            Require(entities.IsSequence() && entities.size() <= 65536, "Invalid Scene entities");
            auto candidate = CreateRef<Scene>(Text(root["Scene"]));
            if (auto environment = root["Environment"])
            {
                Keys(environment,
                     {"Map",
                      "DiffuseIntensity",
                      "SpecularIntensity",
                      "Rotation",
                      "SkyboxIntensity",
                      "ShowSkybox",
                      "BackgroundColor"});
                EnvironmentSettings settings;
                if (environment["Map"])
                    settings.Map = Text(environment["Map"]);
                if (environment["DiffuseIntensity"])
                    settings.DiffuseIntensity = environment["DiffuseIntensity"].as<float>();
                if (environment["SpecularIntensity"])
                    settings.SpecularIntensity = environment["SpecularIntensity"].as<float>();
                if (environment["Rotation"])
                    settings.Rotation = environment["Rotation"].as<float>();
                if (environment["SkyboxIntensity"])
                    settings.SkyboxIntensity = environment["SkyboxIntensity"].as<float>();
                if (environment["ShowSkybox"])
                    settings.ShowSkybox = environment["ShowSkybox"].as<bool>();
                if (environment["BackgroundColor"])
                    settings.BackgroundColor = Vec3(environment["BackgroundColor"]);
                candidate->SetEnvironmentSettings(std::move(settings));
            }
            std::set<uint64_t>    seen;
            SceneAllocationBudget budget;
            budget.TextBytes = candidate->GetName().size();
            // (child, parent) links in file order, applied once every entity exists.
            std::vector<std::pair<uint64_t, uint64_t>> parents;
            for (const auto& node : entities)
            {
                Keys(node,
                     {"Entity",
                      "Parent",
                      "TagComponent",
                      "TransformComponent",
                      "ProductModelComponent",
                      "PrefabInstanceComponent",
                      "AnimatorComponent",
                      "TimelinePlayerComponent",
                      "ActiveComponent",
                      "IKComponent",
                      "ShapeKeysComponent",
                      "MaterialParameterDriversComponent",
                      "RigidBodyComponent",
                      "BoxColliderComponent",
                      "SphereColliderComponent",
                      "CapsuleColliderComponent",
                      "MeshColliderComponent",
                      "WindZoneComponent",
                      "ForceFieldComponent",
                      "RagdollComponent",
                      "ParticleSystemComponent",
                      "VehicleComponent",
                      "MotionColliderComponent",
                      "ClothComponent",
                      "SpringBoneComponent",
                      "CharacterControllerComponent",
                      "JointComponent",
                      "AudioSourceComponent",
                      "AudioListenerComponent",
                      "TextComponent",
                      "UIScreenComponent",
                      "UIWorldComponent",
                      "MeshComponent",
                      "EditableMeshComponent",
                      "PrefabParent",
                      "PrefabOwnedComponent",
                      "CameraComponent",
                      "DirLightComponent",
                      "PointLightComponent",
                      "ScriptComponent",
                      "SpriteRendererComponent"});
                auto id = UInt(node["Entity"]);
                Require(id && seen.insert(id).second, "Duplicate/zero Scene identity");
                if (node["Parent"])
                    parents.emplace_back(id, UInt(node["Parent"]));
                Keys(node["TagComponent"], {"Tag"});
                Keys(node["TransformComponent"], {"Position", "Rotation", "Scale", "Matrix"});
                auto tag       = Text(node["TagComponent"]["Tag"]);
                auto textBytes = tag.size();
                if (node["TextComponent"])
                {
                    const auto text = DecodeText(node["TextComponent"]);
                    textBytes += text.Text.size() + text.Language.size();
                }
                if (node["ScriptComponent"])
                    textBytes += Text(node["ScriptComponent"]["ClassName"]).size();
                std::optional<ProductModelComponent> model;
                if (node["ProductModelComponent"])
                    model = DecodeModel(node["ProductModelComponent"]);
                budget.Add(textBytes, model ? &*model : nullptr);
                auto entity = candidate->CreateEntityWithID(UUID(id), tag, true);
                // Entity creation supplies a default display name; persisted tags
                // retain their exact authored value, including an empty string.
                entity.GetComponent<TagComponent>().Tag = std::move(tag);
                auto value                              = node["TransformComponent"];
                entity.SetComponent<TransformComponent>(DecodeTransform(value));
                if (node["PrefabOwnedComponent"])
                {
                    Require(clipboard, "Prefab ownership is only valid in clipboard entities");
                    const auto owned = node["PrefabOwnedComponent"];
                    Keys(owned, {"Instance", "Node", "Order"});
                    auto instance = UInt(owned["Instance"]);
                    auto key      = Text(owned["Node"]);
                    Require(instance && !key.empty(), "Invalid prefab ownership");
                    entity.AddComponent<PrefabOwnedComponent>(UUID(instance), key, SceneIndex(owned["Order"]));
                }
                if (node["PrefabInstanceComponent"])
                {
                    Require(!model, "Scene entity has both prefab and legacy model");
                    entity.AddComponent<PrefabInstanceComponent>(DecodePrefab(node["PrefabInstanceComponent"]));
                }
                if (node["RigidBodyComponent"])
                {
                    RigidBodyComponent c;
                    PhysicsYaml::Decode(node["RigidBodyComponent"], c);
                    entity.AddComponent<RigidBodyComponent>(c);
                }
                if (node["BoxColliderComponent"])
                {
                    BoxColliderComponent c;
                    PhysicsYaml::Decode(node["BoxColliderComponent"], c);
                    entity.AddComponent<BoxColliderComponent>(c);
                }
                if (node["SphereColliderComponent"])
                {
                    SphereColliderComponent c;
                    PhysicsYaml::Decode(node["SphereColliderComponent"], c);
                    entity.AddComponent<SphereColliderComponent>(c);
                }
                if (node["CapsuleColliderComponent"])
                {
                    CapsuleColliderComponent c;
                    PhysicsYaml::Decode(node["CapsuleColliderComponent"], c);
                    entity.AddComponent<CapsuleColliderComponent>(c);
                }
                if (node["MeshColliderComponent"])
                {
                    MeshColliderComponent c;
                    PhysicsYaml::Decode(node["MeshColliderComponent"], c);
                    entity.AddComponent<MeshColliderComponent>(c);
                }
                if (node["CharacterControllerComponent"])
                {
                    CharacterControllerComponent c;
                    PhysicsYaml::Decode(node["CharacterControllerComponent"], c);
                    entity.AddComponent<CharacterControllerComponent>(c);
                }
                if (node["JointComponent"])
                {
                    JointComponent c;
                    PhysicsYaml::Decode(node["JointComponent"], c);
                    entity.AddComponent<JointComponent>(c);
                }
                if (node["WindZoneComponent"])
                {
                    WindZoneComponent c;
                    PhysicsYaml::Decode(node["WindZoneComponent"], c);
                    entity.AddComponent<WindZoneComponent>(c);
                }
                if (node["ForceFieldComponent"])
                {
                    ForceFieldComponent c;
                    PhysicsYaml::Decode(node["ForceFieldComponent"], c);
                    entity.AddComponent<ForceFieldComponent>(c);
                }
                if (node["RagdollComponent"])
                {
                    RagdollComponent c;
                    PhysicsYaml::Decode(node["RagdollComponent"], c);
                    entity.AddComponent<RagdollComponent>(c);
                }
                if (node["ParticleSystemComponent"])
                    entity.AddComponent<ParticleSystemComponent>(DecodeParticleSystem(node["ParticleSystemComponent"]));
                if (node["VehicleComponent"])
                {
                    VehicleComponent c;
                    PhysicsYaml::Decode(node["VehicleComponent"], c);
                    entity.AddComponent<VehicleComponent>(c);
                }
                if (node["MotionColliderComponent"])
                {
                    MotionColliderComponent c;
                    PhysicsYaml::Decode(node["MotionColliderComponent"], c);
                    entity.AddComponent<MotionColliderComponent>(c);
                }
                if (node["SpringBoneComponent"])
                {
                    SpringBoneComponent c;
                    PhysicsYaml::Decode(node["SpringBoneComponent"], c);
                    entity.AddComponent<SpringBoneComponent>(c);
                }
                if (node["ClothComponent"])
                {
                    ClothComponent c;
                    PhysicsYaml::Decode(node["ClothComponent"], c);
                    entity.AddComponent<ClothComponent>(c);
                }
                if (node["AudioSourceComponent"])
                    entity.AddComponent<AudioSourceComponent>(DecodeAudioSource(node["AudioSourceComponent"]));
                if (node["AudioListenerComponent"])
                {
                    Keys(node["AudioListenerComponent"], {"Active"});
                    entity.AddComponent<AudioListenerComponent>().Active =
                        node["AudioListenerComponent"]["Active"].as<bool>();
                }
                if (const auto drivers = node["MaterialParameterDriversComponent"])
                {
                    Keys(drivers, {"Bindings"});
                    const auto list = drivers["Bindings"];
                    Require(list && list.IsSequence() && list.size() <= 4096, "Invalid material drivers");
                    MaterialParameterDriversComponent component;
                    for (const auto& item : list)
                    {
                        Keys(item, {"Parameter", "SourceEntity", "Quantity"});
                        MaterialParameterBinding binding;
                        binding.Parameter = item["Parameter"].as<std::string>();
                        Require(!binding.Parameter.empty() && binding.Parameter.size() <= 4096,
                                "Invalid parameter name");
                        binding.SourceEntity = item["SourceEntity"].as<uint64_t>();
                        const auto quantity  = item["Quantity"].as<std::string>();
                        if (quantity == "WorldPosition")
                            binding.Quantity = MaterialParameterQuantity::WorldPosition;
                        else if (quantity == "WorldForward")
                            binding.Quantity = MaterialParameterQuantity::WorldForward;
                        else if (quantity == "WorldUp")
                            binding.Quantity = MaterialParameterQuantity::WorldUp;
                        else if (quantity == "WorldRight")
                            binding.Quantity = MaterialParameterQuantity::WorldRight;
                        else
                            throw std::runtime_error("Invalid material driver quantity");
                        component.Bindings.push_back(std::move(binding));
                    }
                    entity.AddComponent<MaterialParameterDriversComponent>(std::move(component));
                }
                if (node["IKComponent"])
                    entity.AddComponent<IKComponent>(DecodeIKComponent(node["IKComponent"]));
                if (node["ShapeKeysComponent"])
                {
                    const auto v = node["ShapeKeysComponent"];
                    Keys(v, {"Skeleton", "Expression", "Weights"});
                    ShapeKeysComponent c;
                    if (v["Skeleton"])
                        c.Skeleton = AssetID::FromString(v["Skeleton"].as<std::string>());
                    if (v["Expression"])
                        c.Expression = AssetID::FromString(v["Expression"].as<std::string>());
                    Require(v["Weights"].IsMap() && v["Weights"].size() <= 4096, "Invalid shape-key weights");
                    c.Weights = v["Weights"].as<std::map<std::string, float>>();
                    for (const auto& [name, value] : c.Weights)
                        Require(!name.empty() && std::isfinite(value), "Invalid shape-key weight");
                    entity.AddComponent<ShapeKeysComponent>(std::move(c));
                }
                if (node["ActiveComponent"])
                    entity.AddComponent<ActiveComponent>().Active = node["ActiveComponent"].as<bool>();
                if (node["TimelinePlayerComponent"])
                {
                    const auto v = node["TimelinePlayerComponent"];
                    Keys(v, {"Timeline", "PlayOnStart", "Loop", "Speed", "WrapMode", "Bindings"});
                    TimelinePlayerComponent c;
                    c.Timeline    = Identity(v["Timeline"]);
                    c.PlayOnStart = v["PlayOnStart"].as<bool>();
                    c.Loop        = v["Loop"].as<bool>();
                    c.Speed       = v["Speed"].as<double>();
                    Require(std::isfinite(c.Speed) && c.Speed >= 0, "Invalid timeline speed");
                    const auto wrap = v["WrapMode"].as<std::string>();
                    Require(wrap == "Hold" || wrap == "Reset", "Invalid timeline wrap mode");
                    c.WrapMode = wrap == "Reset" ? TimelineWrapMode::Reset : TimelineWrapMode::Hold;
                    Require(v["Bindings"].IsMap() && v["Bindings"].size() <= 4096, "Invalid timeline bindings");
                    for (const auto& b : v["Bindings"])
                    {
                        auto id = Text(b.first);
                        Require(!id.empty() && !c.Bindings.count(id), "Invalid/duplicate timeline binding");
                        c.Bindings[id] = UUID(UInt(b.second));
                    }
                    entity.AddComponent<TimelinePlayerComponent>(std::move(c));
                }
                if (node["AnimatorComponent"])
                {
                    const auto v = node["AnimatorComponent"];
                    Keys(v,
                         {"Skeleton",
                          "Controller",
                          "Enabled",
                          "ApplyRootMotion",
                          "Speed",
                          "Parameters",
                          "InlineController",
                          "JointEntities"});
                    AnimatorComponent a;
                    a.Skeleton        = AssetID::FromString(v["Skeleton"].as<std::string>());
                    a.Controller      = AssetID::FromString(v["Controller"].as<std::string>());
                    a.Enabled         = v["Enabled"].as<bool>();
                    a.ApplyRootMotion = v["ApplyRootMotion"].as<bool>();
                    a.Speed           = v["Speed"].as<double>();
                    Require(std::isfinite(a.Speed), "Non-finite animator speed");
                    Require(v["Parameters"].IsMap() && v["Parameters"].size() <= 4096, "Invalid animator parameters");
                    a.Parameters = v["Parameters"].as<std::map<std::string, double>>();
                    for (const auto& p : a.Parameters)
                        Require(std::isfinite(p.second), "Non-finite animator parameter");
                    if (v["InlineController"])
                        a.InlineController = DecodeAnimationControllerNode(v["InlineController"]);
                    if (v["JointEntities"])
                    {
                        Require(clipboard && v["JointEntities"].IsSequence() && v["JointEntities"].size() <= 65536,
                                "Invalid clipboard animator joints");
                        for (const auto& joint : v["JointEntities"])
                            a.JointEntities.emplace_back(UInt(joint));
                    }
                    if (a.InlineController)
                    {
                        auto& c = *a.InlineController;
                        if (!entity.HasComponent<IKComponent>() &&
                            (!c.TwoBoneConstraints.empty() || !c.LookAtConstraints.empty()))
                        {
                            auto& ik    = entity.AddComponent<IKComponent>();
                            ik.Skeleton = a.Skeleton;
                            ik.ImportLegacy(c);
                        }
                        c.TwoBoneConstraints.clear();
                        c.LookAtConstraints.clear();
                    }
                    entity.AddComponent<AnimatorComponent>(std::move(a));
                }
                Require(!(node["MeshComponent"] && node["EditableMeshComponent"]),
                        "Mesh components are mutually exclusive");
                if (node["EditableMeshComponent"])
                    entity.AddComponent<EditableMeshComponent>(DecodeEditable(node["EditableMeshComponent"], budget));
                if (node["UIScreenComponent"])
                    entity.AddComponent<UIScreenComponent>(DecodeUIScreen(node["UIScreenComponent"]));
                if (node["UIWorldComponent"])
                    entity.AddComponent<UIWorldComponent>(DecodeUIWorld(node["UIWorldComponent"]));
                if (node["TextComponent"])
                    entity.AddComponent<TextComponent>(DecodeText(node["TextComponent"]));
                if (node["MeshComponent"])
                    entity.AddComponent<MeshComponent>(DecodeMesh(node["MeshComponent"]));
                if (node["PrefabParent"])
                {
                    const auto attachment = node["PrefabParent"];
                    Keys(attachment, {"Instance", "Node"});
                    const UUID instance = UInt(attachment["Instance"]);
                    const auto key      = Text(attachment["Node"]);
                    Require(instance && !key.empty(), "Invalid prefab parent");
                    entity.AddComponent<PrefabAttachmentComponent>(instance, key);
                }
                if (model)
                {
                    auto& prefab         = entity.AddComponent<PrefabInstanceComponent>();
                    prefab.Prefab        = model->Model;
                    prefab.LegacyPending = std::move(*model);
                }
                if (node["CameraComponent"])
                {
                    value.reset(node["CameraComponent"]);
                    Keys(value,
                         {"ProjectionType",
                          "PerspectiveFOV",
                          "PerspectiveNearClip",
                          "PerspectiveFarClip",
                          "OrthographicSize",
                          "OrthographicNearClip",
                          "OrthographicFarClip",
                          "AspectRatio",
                          "PostProcessOverride",
                          "Exposure",
                          "Bloom",
                          "AntiAliasing",
                          "Upscaling",
                          "Primary",
                          "FixedAspectRatio"});
                    auto type = SceneIndex(value["ProjectionType"]);
                    Require(type <= 1, "Invalid Scene camera type");
                    auto&      component = entity.AddComponent<CameraComponent>();
                    auto&      camera    = component.SceneCamera;
                    const auto aspect    = Number(value["AspectRatio"]);
                    // GLM asserts on epsilon before ValidateCamera can reject it.
                    Require(aspect > 0 && aspect != std::numeric_limits<float>::epsilon(),
                            "Invalid Scene camera aspect ratio");
                    camera.SetAspectRatio(aspect);
                    if (value["PostProcessOverride"])
                        camera.SetPostProcessOverride(DecodePostProcess(value["PostProcessOverride"]));
                    if (value["Exposure"])
                        camera.SetExposure(Number(value["Exposure"]));
                    if (value["Bloom"])
                        camera.SetBloomSettings(DecodeBloom(value["Bloom"]));
                    if (const auto upscale = value["Upscaling"])
                    {
                        Keys(upscale, {"Mode", "CustomRatio"});
                        UpscalingSettings settings;
                        if (upscale["Mode"])
                        {
                            const auto mode = SceneIndex(upscale["Mode"]);
                            Require(mode <= uint32_t(UpscalingMode::Custom), "Invalid camera upscaling mode");
                            settings.Mode = static_cast<UpscalingMode>(mode);
                        }
                        if (upscale["CustomRatio"])
                            settings.CustomRatio = Number(upscale["CustomRatio"]);
                        camera.SetUpscaling(settings);
                    }
                    if (value["AntiAliasing"])
                    {
                        const auto aa = SceneIndex(value["AntiAliasing"]);
                        Require(aa <= uint32_t(AntiAliasing::TAA), "Invalid camera anti-aliasing mode");
                        camera.SetAntiAliasing(static_cast<AntiAliasing>(aa));
                    }
                    camera.SetPerspectiveVerticalFOV(Number(value["PerspectiveFOV"]));
                    camera.SetPerspectiveNearClip(Number(value["PerspectiveNearClip"]));
                    camera.SetPerspectiveFarClip(Number(value["PerspectiveFarClip"]));
                    camera.SetOrthographicSize(Number(value["OrthographicSize"]));
                    camera.SetOrthographicNearClip(Number(value["OrthographicNearClip"]));
                    camera.SetOrthographicFarClip(Number(value["OrthographicFarClip"]));
                    camera.SetProjectionType(static_cast<SceneCamera::ProjectionType>(type));
                    ValidateCamera(camera);
                    component.Primary          = value["Primary"].as<bool>();
                    component.FixedAspectRatio = value["FixedAspectRatio"].as<bool>();
                }
                if (node["DirLightComponent"])
                {
                    value.reset(node["DirLightComponent"]);
                    Keys(value,
                         {"Direction", "Diffuse_Intensity", "Specular_Intensity", "Ambient_Intensity", "Shadows"});
                    auto& light              = entity.AddComponent<DirLightComponent>();
                    light.Direction          = Vec3(value["Direction"]);
                    light.Diffuse_Intensity  = Vec3(value["Diffuse_Intensity"]);
                    light.Specular_Intensity = Vec3(value["Specular_Intensity"]);
                    light.Ambient_Intensity  = Vec3(value["Ambient_Intensity"]);
                    light.Shadows            = DecodeShadows(value["Shadows"]);
                }
                if (node["PointLightComponent"])
                {
                    value.reset(node["PointLightComponent"]);
                    Keys(value, {"Diffuse_Intensity", "Specular_Intensity", "Ambient_Intensity", "Shadows"});
                    auto& light              = entity.AddComponent<PointLightComponent>();
                    light.Diffuse_Intensity  = Vec3(value["Diffuse_Intensity"]);
                    light.Specular_Intensity = Vec3(value["Specular_Intensity"]);
                    light.Ambient_Intensity  = Vec3(value["Ambient_Intensity"]);
                    light.Shadows            = DecodeShadows(value["Shadows"], ShadowSettings::PointDefaults());
                }
                if (node["ScriptComponent"])
                {
                    Keys(node["ScriptComponent"], {"ClassName"});
                    entity.AddComponent<ScriptComponent>(Text(node["ScriptComponent"]["ClassName"]));
                }
                if (node["SpriteRendererComponent"])
                {
                    value.reset(node["SpriteRendererComponent"]);
                    Keys(value,
                         {"Color",
                          "TilingFactor",
                          "Texture",
                          "TextureAsset",
                          "UVRect",
                          "FlipX",
                          "FlipY",
                          "SortingOrder"});
                    auto& sprite        = entity.AddComponent<SpriteRendererComponent>();
                    sprite.Color        = Vec4(value["Color"]);
                    sprite.TilingFactor = Number(value["TilingFactor"]);
                    Require(sprite.TilingFactor >= 0, "Invalid Sprite tiling");
                    // Legacy Texture was a runtime object/path: only TextureAsset has persistent identity.
                    if (value["TextureAsset"])
                        sprite.Texture = Identity(value["TextureAsset"]);
                    if (value["UVRect"])
                        sprite.UVRect = Vec4(value["UVRect"]);
                    sprite.FlipX        = value["FlipX"].as<bool>(false);
                    sprite.FlipY        = value["FlipY"].as<bool>(false);
                    sprite.SortingOrder = value["SortingOrder"].as<int>(0);
                }
            }
            if (root["PostProcess"])
                candidate->SetPostProcessSettings(DecodePostProcess(root["PostProcess"]));
            else
            {
                PostProcessSettings migrated;
                for (auto handle : candidate->m_Registry.view<CameraComponent>())
                {
                    const auto& component = candidate->m_Registry.get<CameraComponent>(handle);
                    if (component.Primary)
                    {
                        migrated = component.SceneCamera.ResolvePostProcess(PostProcessSettings {});
                        break;
                    }
                }
                candidate->SetPostProcessSettings(migrated);
                for (auto handle : candidate->m_Registry.view<CameraComponent>())
                {
                    auto&      camera   = candidate->m_Registry.get<CameraComponent>(handle).SceneCamera;
                    const auto previous = camera.ResolvePostProcess(PostProcessSettings {});
                    camera.SetPostProcessOverride(previous == migrated ? std::nullopt :
                                                                         std::optional<PostProcessSettings>(previous));
                }
            }
            candidate->m_ReconcilingPrefab = true;
            for (const auto& [child, parent] : parents)
            {
                Entity entity = candidate->GetEntityByUUID(UUID(child));
                Entity owner  = candidate->GetEntityByUUID(UUID(parent));
                if (!owner && entity.HasComponent<PrefabAttachmentComponent>())
                {
                    const auto& attachment = entity.GetComponent<PrefabAttachmentComponent>();
                    Entity      root       = candidate->GetEntityByUUID(attachment.Instance);
                    Require(root && root.HasComponent<PrefabInstanceComponent>() &&
                                Scene::PrefabEntityUUID(attachment.Instance, attachment.NodeKey) == UUID(parent),
                            "Invalid pending prefab parent");
                    owner = root;
                }
                Require(owner && candidate->SetParent(entity, owner, {}, false), "Invalid Scene entity parent");
            }
            candidate->m_ReconcilingPrefab = false;
            if (!clipboard)
                candidate->UpdatePrefabs();
            return candidate;
        }
        catch (const std::exception& error)
        {
            return AssetError {AssetErrorCode::InvalidData, std::string(error.what()).substr(0, 2048)};
        }
    }
    void SceneSerializer::Serialize(const std::filesystem::path& path)
    {
        auto value = SerializeChecked(path);
        if (auto error = std::get_if<AssetError>(&value))
            CORE_ERROR("Scene save: {0}", error->Message);
    }
    void SceneSerializer::Deserialize(const std::filesystem::path& path)
    {
        auto value = DeserializeChecked(path);
        if (auto error = std::get_if<AssetError>(&value))
            CORE_ERROR("Scene load: {0}", error->Message);
        else
            m_Scene = std::get<Ref<Scene>>(std::move(value));
    }
    void SceneSerializer::SerializeEntity(YAML::Emitter& output, Entity entity, bool clipboard)
    {
        output << EncodeEntity(entity, clipboard);
    }
    void SceneSerializer::SerializeRuntime(const std::filesystem::path& path) { Serialize(path); }
    void SceneSerializer::DeserializeEntity(YAML::Node&, Ref<Scene>)
    {
        CORE_ERROR("Use checked whole-Scene deserialization");
    }
} // namespace Kans

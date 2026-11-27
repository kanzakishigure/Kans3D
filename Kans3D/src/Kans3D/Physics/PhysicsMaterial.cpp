#include "kspch.h"
#include "PhysicsMaterial.h"
#include <cstring>
#include <set>
#include "Kans3D/Asset/Loader/Loader.h"
namespace Kans
{
    namespace
    {
        constexpr const char* PresetNames[] = {"Generic",
                                               "Brick",
                                               "Concrete",
                                               "Ceramic",
                                               "Gravel",
                                               "Carpet",
                                               "Glass",
                                               "Plaster",
                                               "Wood",
                                               "Metal",
                                               "Rock"};

        // Steam Audio 4.8.1, IPLMaterial standard materials (phonon.h).
        constexpr AcousticMaterial Presets[] = {{},
                                                {{.03f, .04f, .07f}, {.015f, .015f, .015f}, .05f},
                                                {{.05f, .07f, .08f}, {.015f, .002f, .001f}, .05f},
                                                {{.01f, .02f, .02f}, {.060f, .044f, .011f}, .05f},
                                                {{.60f, .70f, .80f}, {.031f, .012f, .008f}, .05f},
                                                {{.24f, .69f, .73f}, {.020f, .005f, .003f}, .05f},
                                                {{.06f, .03f, .02f}, {.060f, .044f, .011f}, .05f},
                                                {{.12f, .06f, .04f}, {.056f, .056f, .004f}, .05f},
                                                {{.11f, .07f, .06f}, {.070f, .014f, .005f}, .05f},
                                                {{.20f, .07f, .06f}, {.200f, .025f, .010f}, .05f},
                                                {{.13f, .20f, .24f}, {.015f, .002f, .001f}, .05f}};
        static_assert(std::size(Presets) == std::size(PresetNames));
    } // namespace

    const char* AcousticMaterialPresetName(AcousticMaterialPreset preset)
    {
        return PresetNames[unsigned(preset) < std::size(PresetNames) ? unsigned(preset) : 0];
    }

    AcousticMaterial GetAcousticMaterialPreset(AcousticMaterialPreset preset)
    {
        return Presets[unsigned(preset) < std::size(Presets) ? unsigned(preset) : 0];
    }

    void SanitizeAcousticMaterial(AcousticMaterial&         material,
                                  std::vector<std::string>* reasons,
                                  const AcousticMaterial&   defaults)
    {
        constexpr const char* bands[]  = {" low", " mid", " high"};
        const auto            sanitize = [&](float& value, float replacement, const char* field, const char* band) {
            if (value >= 0 && value <= 1)
                return;
            if (reasons)
                reasons->push_back(std::string("Acoustics.") + field + band +
                                   (std::isnan(value) ? " is NaN and uses its preset value" : " clamped to [0, 1]"));
            value = std::isnan(value) ? replacement : std::clamp(value, 0.f, 1.f);
        };
        for (size_t band = 0; band < 3; ++band)
        {
            sanitize(material.Absorption[band], defaults.Absorption[band], "Absorption", bands[band]);
            sanitize(material.Transmission[band], defaults.Transmission[band], "Transmission", bands[band]);
        }
        sanitize(material.Scattering, defaults.Scattering, "Scattering", "");
    }

    YAML::Node EncodeAcousticMaterial(const AcousticMaterial& material)
    {
        auto values = material;
        SanitizeAcousticMaterial(values);
        YAML::Node node;
        for (size_t i = 0; i < std::size(Presets); ++i)
            if (values == Presets[i])
            {
                node["Preset"] = PresetNames[i];
                return node;
            }
        for (size_t band = 0; band < 3; ++band)
        {
            node["Absorption"].push_back(values.Absorption[band]);
            node["Transmission"].push_back(values.Transmission[band]);
        }
        node["Scattering"] = values.Scattering;
        return node;
    }

    AcousticMaterial DecodeAcousticMaterial(const YAML::Node& node, std::vector<std::string>* reasons)
    {
        if (!node.IsMap())
            throw std::runtime_error("Acoustics must be a map");
        std::set<std::string> keys;
        for (const auto& entry : node)
        {
            const auto key = entry.first.as<std::string>();
            if (!keys.insert(key).second ||
                (key != "Preset" && key != "Absorption" && key != "Transmission" && key != "Scattering"))
                throw std::runtime_error("Unknown or duplicate Acoustics field: " + key);
        }
        AcousticMaterial defaults;
        if (const auto preset = node["Preset"])
        {
            const auto name  = preset.as<std::string>();
            const auto found = std::find(std::begin(PresetNames), std::end(PresetNames), name);
            if (found == std::end(PresetNames))
                throw std::runtime_error("Unknown Acoustics preset: " + name);
            defaults = Presets[found - std::begin(PresetNames)];
        }
        auto       values = defaults;
        const auto read   = [&](const char* key, auto& coefficients) {
            if (const auto field = node[key])
            {
                if (!field.IsSequence() || field.size() != coefficients.size())
                    throw std::runtime_error(std::string("Acoustics.") + key + " requires three bands");
                for (size_t band = 0; band < coefficients.size(); ++band)
                    coefficients[band] = field[band].as<float>();
            }
        };
        read("Absorption", values.Absorption);
        read("Transmission", values.Transmission);
        if (const auto scattering = node["Scattering"])
            values.Scattering = scattering.as<float>();
        SanitizeAcousticMaterial(values, reasons, defaults);
        return values;
    }

    std::variant<std::monostate, AssetError> ValidatePhysicsMaterial(const PhysicsMaterialValues& m)
    {
        if (!std::isfinite(m.StaticFriction) || m.StaticFriction < 0 || !std::isfinite(m.DynamicFriction) ||
            m.DynamicFriction < 0 || !std::isfinite(m.Restitution) || m.Restitution < 0 || m.Restitution > 1 ||
            unsigned(m.FrictionCombine) > 3 || unsigned(m.RestitutionCombine) > 3)
            return AssetError {AssetErrorCode::InvalidData, "Invalid physics material coefficients or combine mode"};
        return std::monostate {};
    }
    namespace
    {
        const char* Modes[] = {"Average", "Min", "Multiply", "Max"};
        void        Check(const PhysicsMaterialValues& m)
        {
            auto v = ValidatePhysicsMaterial(m);
            if (auto e = std::get_if<AssetError>(&v))
                throw std::runtime_error(e->Message);
        }
        PhysicsCombineMode Mode(const YAML::Node& n)
        {
            if (!n)
                return PhysicsCombineMode::Average;
            auto text = n.as<std::string>();
            for (unsigned i = 0; i < 4; ++i)
                if (text == Modes[i])
                    return PhysicsCombineMode(i);
            throw std::runtime_error("Unknown physics material combine mode: " + text);
        }
    } // namespace
    YAML::Node EncodePhysicsMaterialNode(const PhysicsMaterialValues& m)
    {
        Check(m);
        YAML::Node n;
        n["StaticFriction"]     = m.StaticFriction;
        n["DynamicFriction"]    = m.DynamicFriction;
        n["Restitution"]        = m.Restitution;
        n["FrictionCombine"]    = Modes[unsigned(m.FrictionCombine)];
        n["RestitutionCombine"] = Modes[unsigned(m.RestitutionCombine)];
        n["Acoustics"]          = EncodeAcousticMaterial(m.Acoustics);
        return n;
    }
    PhysicsMaterialValues DecodePhysicsMaterialNode(const YAML::Node& n, std::vector<std::string>* reasons)
    {
        if (!n.IsMap())
            throw std::runtime_error("Physics material must be a map");
        std::set<std::string> keys;
        for (auto item : n)
        {
            auto key = item.first.as<std::string>();
            if (!keys.insert(key).second ||
                (key != "StaticFriction" && key != "DynamicFriction" && key != "Restitution" &&
                 key != "FrictionCombine" && key != "RestitutionCombine" && key != "Acoustics"))
                throw std::runtime_error("Unknown or duplicate physics material field: " + key);
        }
        PhysicsMaterialValues m;
        m.StaticFriction     = n["StaticFriction"].as<float>();
        m.DynamicFriction    = n["DynamicFriction"].as<float>();
        m.Restitution        = n["Restitution"].as<float>();
        m.FrictionCombine    = Mode(n["FrictionCombine"]);
        m.RestitutionCombine = Mode(n["RestitutionCombine"]);
        if (const auto acoustics = n["Acoustics"])
            m.Acoustics = DecodeAcousticMaterial(acoustics, reasons);
        Check(m);
        return m;
    }
    std::variant<std::vector<std::byte>, AssetError> EncodePhysicsMaterialProduct(const PhysicsMaterialValues& m)
    {
        auto valid = ValidatePhysicsMaterial(m);
        if (auto e = std::get_if<AssetError>(&valid))
            return *e;
        std::vector<std::byte> out;
        auto                   word = [&](uint32_t v) {
            for (unsigned i = 0; i < 4; ++i)
                out.push_back(std::byte((v >> (8 * i)) & 255));
        };
        word(0x4d504b);
        word(2);
        const auto real = [&](float v) {
            uint32_t bits;
            std::memcpy(&bits, &v, 4);
            word(bits);
        };
        for (float v : {m.StaticFriction, m.DynamicFriction, m.Restitution})
            real(v);
        word(uint32_t(m.FrictionCombine));
        word(uint32_t(m.RestitutionCombine));
        auto acoustics = m.Acoustics;
        SanitizeAcousticMaterial(acoustics);
        for (float value : acoustics.Absorption)
            real(value);
        for (float value : acoustics.Transmission)
            real(value);
        real(acoustics.Scattering);
        return out;
    }
    std::variant<PhysicsMaterialValues, AssetError> DecodePhysicsMaterialProduct(const std::byte* data, size_t size)
    {
        auto error = AssetError {AssetErrorCode::InvalidData, "Invalid physics material Product"};
        if (!data || (size != 28 && size != 56))
            return error;
        auto word = [&](unsigned offset) {
            uint32_t v = 0;
            for (unsigned i = 0; i < 4; ++i)
                v |= uint32_t(data[offset + i]) << (8 * i);
            return v;
        };
        const auto version = word(4);
        if (word(0) != 0x4d504b || version != (size == 28 ? 1u : 2u) || word(20) > 3 || word(24) > 3)
            return error;
        PhysicsMaterialValues m;
        const auto            real = [&](unsigned offset, float& value) {
            const auto bits = word(offset);
            std::memcpy(&value, &bits, sizeof(value));
        };
        real(8, m.StaticFriction);
        real(12, m.DynamicFriction);
        real(16, m.Restitution);
        if (version == 2)
        {
            for (unsigned band = 0; band < 3; ++band)
            {
                real(28 + band * 4, m.Acoustics.Absorption[band]);
                real(40 + band * 4, m.Acoustics.Transmission[band]);
            }
            real(52, m.Acoustics.Scattering);
            SanitizeAcousticMaterial(m.Acoustics);
        }
        m.FrictionCombine    = PhysicsCombineMode(word(20));
        m.RestitutionCombine = PhysicsCombineMode(word(24));
        auto valid           = ValidatePhysicsMaterial(m);
        if (auto e = std::get_if<AssetError>(&valid))
            return *e;
        return m;
    }
    std::variant<Ref<PhysicsMaterialAsset>, AssetError>
    Loader<PhysicsMaterialAsset>::Load(const AssetLoadContext& c) const
    {
        if (!c.ID.IsValid() || c.Type != AssetType::PhysicsMaterial)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid physics material load context"};
        auto data = DecodePhysicsMaterialProduct(c.Data, c.Size);
        if (auto e = std::get_if<AssetError>(&data))
            return *e;
        auto asset     = CreateRef<PhysicsMaterialAsset>(std::get<PhysicsMaterialValues>(data));
        asset->assetID = c.ID;
        return asset;
    }
} // namespace Kans

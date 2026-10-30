#include "kspch.h"
#include "PhysicsMaterial.h"
#include <cstring>
#include <set>
#include "Kans3D/Asset/Loader/Loader.h"
namespace Kans
{
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
        return n;
    }
    PhysicsMaterialValues DecodePhysicsMaterialNode(const YAML::Node& n)
    {
        if (!n.IsMap())
            throw std::runtime_error("Physics material must be a map");
        std::set<std::string> keys;
        for (auto item : n)
        {
            auto key = item.first.as<std::string>();
            if (!keys.insert(key).second ||
                (key != "StaticFriction" && key != "DynamicFriction" && key != "Restitution" &&
                 key != "FrictionCombine" && key != "RestitutionCombine"))
                throw std::runtime_error("Unknown or duplicate physics material field: " + key);
        }
        PhysicsMaterialValues m;
        m.StaticFriction     = n["StaticFriction"].as<float>();
        m.DynamicFriction    = n["DynamicFriction"].as<float>();
        m.Restitution        = n["Restitution"].as<float>();
        m.FrictionCombine    = Mode(n["FrictionCombine"]);
        m.RestitutionCombine = Mode(n["RestitutionCombine"]);
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
        word(1);
        for (float v : {m.StaticFriction, m.DynamicFriction, m.Restitution})
        {
            uint32_t bits;
            std::memcpy(&bits, &v, 4);
            word(bits);
        }
        word(uint32_t(m.FrictionCombine));
        word(uint32_t(m.RestitutionCombine));
        return out;
    }
    std::variant<PhysicsMaterialValues, AssetError> DecodePhysicsMaterialProduct(const std::byte* data, size_t size)
    {
        auto error = AssetError {AssetErrorCode::InvalidData, "Invalid physics material Product"};
        if (!data || size != 28)
            return error;
        auto word = [&](unsigned offset) {
            uint32_t v = 0;
            for (unsigned i = 0; i < 4; ++i)
                v |= uint32_t(data[offset + i]) << (8 * i);
            return v;
        };
        if (word(0) != 0x4d504b || word(4) != 1 || word(20) > 3 || word(24) > 3)
            return error;
        PhysicsMaterialValues m;
        float*                values[] = {&m.StaticFriction, &m.DynamicFriction, &m.Restitution};
        for (unsigned i = 0; i < 3; ++i)
        {
            auto bits = word(8 + i * 4);
            std::memcpy(values[i], &bits, 4);
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

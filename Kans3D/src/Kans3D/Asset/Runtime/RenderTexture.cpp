#include "kspch.h"
#include "RenderTexture.h"
#include <cmath>
#include <yaml-cpp/yaml.h>
namespace Kans
{
    std::variant<std::monostate, AssetError> ValidateRenderTexture(const RenderTextureSpecification& s)
    {
        if (!s.Width || !s.Height || s.Width > 16384 || s.Height > 16384 || uint32_t(s.Format) > 1 ||
            uint32_t(s.Update) > 2 || !std::isfinite(s.Rate) || s.Rate < 0 ||
            !std::all_of(s.ClearColor.begin(), s.ClearColor.end(), [](float c) { return std::isfinite(c); }))
            return AssetError {AssetErrorCode::InvalidData,
                               "Invalid RenderTexture size, format, update or clear color"};
        return std::monostate {};
    }
    std::variant<RenderTextureSpecification, AssetError> ParseRenderTexture(const std::string& text)
    {
        try
        {
            const auto n = YAML::Load(text);
            if (!n.IsMap() || !n["RenderTexture"] || n["RenderTexture"].as<int>() != 1)
                throw std::runtime_error("Expected RenderTexture: 1");
            RenderTextureSpecification s;
            if (n["Material"] && !n["Material"].as<std::string>().empty())
            {
                s.Material = AssetID::FromString(n["Material"].as<std::string>());
                if (!s.Material.IsValid())
                    throw std::runtime_error("Invalid RenderTexture Material reference");
            }
            if (n["Width"])
                s.Width = n["Width"].as<uint32_t>();
            if (n["Height"])
                s.Height = n["Height"].as<uint32_t>();
            if (n["Mips"])
                s.Mips = n["Mips"].as<bool>();
            if (n["Rate"])
                s.Rate = n["Rate"].as<double>();
            if (n["Format"])
            {
                const auto f = n["Format"].as<std::string>();
                if (f != "RGBA8SRGB" && f != "RGBA16F")
                    throw std::runtime_error("Unknown RenderTexture Format");
                s.Format = f == "RGBA16F" ? RenderTextureFormat::RGBA16F : RenderTextureFormat::RGBA8SRGB;
            }
            if (n["Update"])
            {
                const auto u = n["Update"].as<std::string>();
                if (u != "OnLoad" && u != "Realtime" && u != "OnDemand")
                    throw std::runtime_error("Unknown RenderTexture Update");
                s.Update = u == "OnLoad"   ? RenderTextureUpdate::OnLoad :
                           u == "OnDemand" ? RenderTextureUpdate::OnDemand :
                                             RenderTextureUpdate::Realtime;
            }
            if (n["ClearColor"])
            {
                if (!n["ClearColor"].IsSequence() || n["ClearColor"].size() != 4)
                    throw std::runtime_error("ClearColor requires four components");
                for (size_t i = 0; i < 4; ++i)
                    s.ClearColor[i] = n["ClearColor"][i].as<float>();
            }
            if (auto valid = ValidateRenderTexture(s); std::holds_alternative<AssetError>(valid))
                return std::get<AssetError>(valid);
            return s;
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, e.what()};
        }
    }
    std::variant<std::string, AssetError> SerializeRenderTexture(const RenderTextureSpecification& s)
    {
        if (auto valid = ValidateRenderTexture(s); std::holds_alternative<AssetError>(valid))
            return std::get<AssetError>(valid);
        YAML::Node n;
        n["RenderTexture"] = 1;
        n["Material"]      = s.Material.IsValid() ? s.Material.ToString() : "";
        n["Width"]         = s.Width;
        n["Height"]        = s.Height;
        n["Format"]        = s.Format == RenderTextureFormat::RGBA16F ? "RGBA16F" : "RGBA8SRGB";
        n["Mips"]          = s.Mips;
        n["Update"]        = s.Update == RenderTextureUpdate::OnLoad   ? "OnLoad" :
                             s.Update == RenderTextureUpdate::OnDemand ? "OnDemand" :
                                                                         "Realtime";
        n["Rate"]          = s.Rate;
        for (float c : s.ClearColor)
            n["ClearColor"].push_back(c);
        YAML::Emitter out;
        out << n;
        return std::string(out.c_str()) + "\n";
    }
    bool RenderTextureSchedule::Due(const RenderTextureSpecification& s,
                                    uint64_t                          frame,
                                    double                            time,
                                    uint64_t                          requests) const
    {
        if (LastFrame == frame || !std::isfinite(time))
            return false;
        if (s.Update == RenderTextureUpdate::OnDemand)
            return requests != LastRequest;
        if (s.Update == RenderTextureUpdate::OnLoad)
            return Updates == 0;
        return Updates == 0 || s.Rate == 0 || time < LastTime || time - LastTime + 1e-9 >= 1.0 / s.Rate;
    }
    void RenderTextureSchedule::Complete(uint64_t frame, double time, uint64_t requests)
    {
        LastFrame   = frame;
        LastTime    = time;
        LastRequest = requests;
        ++Updates;
    }
} // namespace Kans

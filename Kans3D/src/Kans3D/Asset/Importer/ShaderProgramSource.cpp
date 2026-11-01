#include "kspch.h"
#include "ShaderProgramSource.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <sstream>

namespace Kans
{
    std::variant<ShaderDomainSource, AssetError> ParseShaderProgramDomain(const std::string& text)
    {
        ShaderDomainSource result;
        bool               domain = false, stage = false, output = false, pass = false;
        std::istringstream lines(text);
        std::string        line;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.rfind("domain=", 0) == 0)
            {
                if (domain || line != "domain=post_process")
                    return AssetError {AssetErrorCode::InvalidData, "Invalid or duplicate Shader domain"};
                domain        = true;
                result.Domain = ShaderDomain::PostProcess;
            }
            if (line.rfind("stage=", 0) == 0)
            {
                if (stage || (line != "stage=before_tonemap" && line != "stage=after_tonemap"))
                    return AssetError {AssetErrorCode::InvalidData, "Invalid or duplicate post-process stage"};
                stage = true;
                result.Stage =
                    line == "stage=before_tonemap" ? PostProcessStage::BeforeToneMap : PostProcessStage::AfterToneMap;
            }
            output |= line.rfind("output_space=", 0) == 0;
            pass |= line.rfind("pass ", 0) == 0;
        }
        if (domain != stage || (domain && (output || pass)))
            return AssetError {AssetErrorCode::InvalidData,
                               "Post-process requires domain and stage, forbids output_space and passes"};
        return result;
    }
    std::variant<ShaderOutputSpace, AssetError> ParseShaderOutputSpace(const std::string& text)
    {
        ShaderOutputSpace  result = ShaderOutputSpace::Display;
        bool               found = false, inPass = false;
        std::istringstream lines(text);
        std::string        line;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.rfind("pass ", 0) == 0)
                inPass = true;
            if (line == "endpass")
                inPass = false;
            if (line.rfind("output_space=", 0) != 0)
                continue;
            const auto value = line.substr(13);
            if (found || inPass || (value != "display" && value != "linear_hdr"))
                return AssetError {AssetErrorCode::InvalidData, "Invalid or duplicate Shader output_space"};
            found  = true;
            result = value == "linear_hdr" ? ShaderOutputSpace::LinearHDR : ShaderOutputSpace::Display;
        }
        return result;
    }
    std::variant<std::vector<ShaderStageSource>, AssetError> ParseShaderProgramSource(const std::string& text)
    {
        if (text.empty() || text.size() > 65536 || text.find('\0') != std::string::npos)
            return AssetError {AssetErrorCode::InvalidData, "Invalid Shader program text budget"};
        std::istringstream lines(text);
        std::string        line;
        const auto         next = [&] {
            if (!std::getline(lines, line))
                return false;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            return true;
        };
        if (!next() || line != "KansShaderProgram 1")
            return AssetError {AssetErrorCode::UnsupportedVersion, "Unsupported Shader program header"};
        auto domain = ParseShaderProgramDomain(text);
        if (auto* error = std::get_if<AssetError>(&domain))
            return *error;
        const bool post        = std::get<ShaderDomainSource>(domain).Domain == ShaderDomain::PostProcess;
        auto outputSpace = ParseShaderOutputSpace(text);
        if (auto* error = std::get_if<AssetError>(&outputSpace))
            return *error;
        auto passes = ParseShaderProgramPasses(text);
        if (auto* error = std::get_if<AssetError>(&passes))
            return *error;
        bool                           inPass = false;
        std::vector<ShaderStageSource> result;
        std::set<ShaderProductStage>   stages;
        while (next())
        {
            if (line.rfind("pass ", 0) == 0)
            {
                inPass = true;
                continue;
            }
            if (line == "endpass")
            {
                inPass = false;
                continue;
            }
            if (inPass)
                continue;
            if (line.empty() || line[0] == '#' || line.rfind("default ", 0) == 0 || line.rfind("hint ", 0) == 0 ||
                line.rfind("texture ", 0) == 0 || line.rfind("group ", 0) == 0 || line.rfind("output_space=", 0) == 0 ||
                line.rfind("domain=", 0) == 0 || line.rfind("stage=", 0) == 0)
                continue;
            const auto split = line.find('=');
            if (split == std::string::npos)
                return AssetError {AssetErrorCode::InvalidData, "Invalid Shader program stage field"};
            const auto key   = line.substr(0, split);
            const auto path  = line.substr(split + 1);
            const auto stage = key == "vertex"   ? ShaderProductStage::Vertex :
                               key == "fragment" ? ShaderProductStage::Fragment :
                                                   ShaderProductStage::Compute;
            if ((key != "vertex" && key != "fragment" && key != "compute") || !stages.insert(stage).second ||
                path.size() > 4096 || path.find('\\') != std::string::npos || path.find(':') != std::string::npos)
                return AssetError {AssetErrorCode::InvalidData, "Invalid or duplicate Shader program stage"};
            AssetPath logical(std::filesystem::u8path(path));
            if (!logical.IsValid() ||
                std::any_of(path.begin(), path.end(), [](unsigned char c) { return c < 32 || c == 127; }))
                return AssetError {AssetErrorCode::InvalidData, "Invalid Shader program stage path"};
            result.push_back({stage, std::move(logical)});
        }
        if (post && (stages.size() != 1 || !stages.count(ShaderProductStage::Fragment)))
            return AssetError {AssetErrorCode::InvalidData, "Post-process requires exactly one fragment stage"};
        if (stages.count(ShaderProductStage::Compute) && stages.size() != 1)
            return AssetError {AssetErrorCode::InvalidData, "Compute shader cannot mix with graphics stages"};
        if (!post && ((stages.count(ShaderProductStage::Compute) && stages.size() != 1) ||
                      (!stages.count(ShaderProductStage::Compute) &&
                       (stages.size() != 2 || !stages.count(ShaderProductStage::Vertex) ||
                        !stages.count(ShaderProductStage::Fragment)))))
            return AssetError {AssetErrorCode::InvalidData, "Shader program requires vertex+fragment or compute"};
        if (!std::get<std::vector<ShaderPassSource>>(passes).empty() && stages.count(ShaderProductStage::Compute))
            return AssetError {AssetErrorCode::InvalidData, "Additional passes require a graphics main pass"};
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.Stage < b.Stage; });
        return result;
    }

    std::variant<std::vector<ShaderPassSource>, AssetError> ParseShaderProgramPasses(const std::string& text)
    {
        std::vector<ShaderPassSource> result;
        std::set<std::string>         names, fields;
        std::istringstream            lines(text);
        std::string                   line;
        bool                          open = false;
        const auto                    failure =
            [&](const std::string& message) -> std::variant<std::vector<ShaderPassSource>, AssetError> {
            return AssetError {AssetErrorCode::InvalidData,
                               "Shader pass " + (open ? "'" + result.back().Name + "': " : "") + message};
        };
        if (text.size() > 65536 || text.find('\0') != std::string::npos)
            return failure("invalid text budget");
        const auto token = [](const std::string& name) {
            return !name.empty() && name.size() <= 256 &&
                   std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
        };
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;
            if (line.rfind("pass ", 0) == 0)
            {
                const auto name = line.substr(5);
                if (open || !token(name) || name == "Main" || !names.insert(name).second || result.size() >= 32)
                    return failure("invalid, duplicate or nested pass declaration");
                result.push_back({name, {}, {}, {}});
                fields.clear();
                open = true;
                continue;
            }
            if (line == "endpass")
            {
                if (!open)
                    return failure("unexpected endpass");
                if (result.back().Tag.empty() || !fields.count("vertex") || !fields.count("fragment"))
                    return failure("requires tag, vertex and fragment");
                open = false;
                continue;
            }
            if (!open)
                continue;
            const auto split = line.find('=');
            if (split == std::string::npos)
                return failure("expected key=value");
            const auto key = line.substr(0, split), value = line.substr(split + 1);
            if (!fields.insert(key).second)
                return failure("duplicate field '" + key + "'");
            auto&      pass      = result.back();
            auto&      state     = pass.State;
            const auto enumValue = [&](const std::vector<std::string>& values, auto& target) {
                auto found = std::find(values.begin(), values.end(), value);
                if (found == values.end())
                    return false;
                target = static_cast<std::decay_t<decltype(target)>>(found - values.begin());
                return true;
            };
            const auto boolean = [&](bool& target) {
                if (value != "on" && value != "off")
                    return false;
                target = value == "on";
                return true;
            };
            bool valid = true;
            if (key == "vertex" || key == "fragment")
            {
                auto stage =
                    ParseShaderProgramSource("KansShaderProgram 1\nvertex=" + value + "\nfragment=" + value + "\n");
                if (std::holds_alternative<AssetError>(stage))
                    return failure("invalid stage path");
                pass.Stages.push_back({key == "vertex" ? ShaderProductStage::Vertex : ShaderProductStage::Fragment,
                                       AssetPath(std::filesystem::u8path(value))});
            }
            else if (key == "tag")
            {
                valid    = token(value);
                pass.Tag = value;
            }
            else if (key == "cull")
                valid = enumValue({"none", "front", "back"}, state.Cull);
            else if (key == "depth_test")
                valid = boolean(state.DepthTest);
            else if (key == "depth_write")
                valid = boolean(state.DepthWrite);
            else if (key == "depth_compare")
                valid = enumValue(
                    {"never", "less", "equal", "less_equal", "greater", "not_equal", "greater_equal", "always"},
                    state.DepthCompare);
            else if (key == "blend")
            {
                if (value == "opaque")
                    state.Blend = false;
                else if (value == "alpha")
                {
                    state.Blend            = true;
                    state.SourceColor      = ShaderBlendFactor::SourceAlpha;
                    state.DestinationColor = ShaderBlendFactor::OneMinusSourceAlpha;
                    state.SourceAlpha      = ShaderBlendFactor::Zero;
                    state.DestinationAlpha = ShaderBlendFactor::One;
                }
                else
                    valid = boolean(state.Blend);
            }
            else if (key == "order")
            {
                valid = value == "after_main" && (pass.Tag.empty() || pass.Tag == "AfterMain");
                if (valid)
                    pass.Tag = "AfterMain";
            }
            else if (key == "src_color" || key == "dst_color" || key == "src_alpha" || key == "dst_alpha")
            {
                auto& factor = key == "src_color" ? state.SourceColor :
                               key == "dst_color" ? state.DestinationColor :
                               key == "src_alpha" ? state.SourceAlpha :
                                                    state.DestinationAlpha;
                valid        = enumValue({"zero",
                                          "one",
                                          "src_color",
                                          "one_minus_src_color",
                                          "src_alpha",
                                          "one_minus_src_alpha",
                                          "dst_color",
                                          "one_minus_dst_color",
                                          "dst_alpha",
                                          "one_minus_dst_alpha"},
                                         factor);
            }
            else if (key == "color_write")
            {
                state.ColorWriteMask = 0;
                if (value != "none")
                    for (char c : value)
                    {
                        auto bit = std::string("rgba").find(c);
                        if (bit == std::string::npos || (state.ColorWriteMask & (1u << bit)))
                        {
                            valid = false;
                            break;
                        }
                        state.ColorWriteMask |= 1u << bit;
                    }
                valid &= !value.empty();
            }
            else
                return failure("unknown field '" + key + "'");
            if (!valid)
                return failure("invalid value for '" + key + "'");
        }
        if (open)
            return failure("missing endpass");
        for (auto& pass : result)
            std::sort(
                pass.Stages.begin(), pass.Stages.end(), [](const auto& a, const auto& b) { return a.Stage < b.Stage; });
        return result;
    }

    std::variant<ShaderParameterDefaults, AssetError> ParseShaderProgramDefaults(const std::string& text)
    {
        if (text.size() > 65536 || text.find('\0') != std::string::npos)
            return AssetError {AssetErrorCode::InvalidData, "Invalid Shader program text budget"};
        ShaderParameterDefaults result;
        std::istringstream      lines(text);
        std::string             line;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.rfind("default ", 0) != 0)
                continue;
            const auto split = line.find('=');
            std::istringstream name(line.substr(8, split == std::string::npos ? std::string::npos : split - 8));
            std::string        key, extra;
            if (split == std::string::npos || !(name >> key) || (name >> extra))
                return AssetError {AssetErrorCode::InvalidData, "Shader default must be 'default <name> = <values>'"};
            std::istringstream  input(line.substr(split + 1));
            std::vector<double> values;
            std::string         token;
            while (input >> token)
            {
                char*  end   = nullptr;
                double value = std::strtod(token.c_str(), &end);
                if (token == "true" || token == "false")
                    value = token == "true" ? 1.0 : 0.0;
                else if (!end || *end || !std::isfinite(value))
                    return AssetError {AssetErrorCode::InvalidData, "Shader default '" + key + "' has a non-number"};
                values.push_back(value);
            }
            if (values.empty() || values.size() > 4096 || key.size() > 256 || !result.emplace(key, values).second)
                return AssetError {AssetErrorCode::InvalidData, "Invalid or duplicate Shader default '" + key + "'"};
        }
        return result;
    }

    std::variant<ShaderParameterHints, AssetError> ParseShaderProgramHints(const std::string& text)
    {
        if (text.size() > 65536 || text.find('\0') != std::string::npos)
            return AssetError {AssetErrorCode::InvalidData, "Invalid Shader program text budget"};
        ShaderParameterHints result;
        std::istringstream   lines(text);
        std::string          line;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.rfind("hint ", 0) != 0)
                continue;
            const auto         split = line.find('=');
            std::istringstream name(line.substr(5, split == std::string::npos ? std::string::npos : split - 5));
            std::string        key, extra, kind;
            if (split == std::string::npos || !(name >> key) || (name >> extra))
                return AssetError {AssetErrorCode::InvalidData, "Shader hint must be 'hint <name> = <widget>'"};
            std::istringstream  input(line.substr(split + 1));
            ShaderParameterHint hint;
            input >> kind;
            if (kind == "toggle")
                hint.Type = ShaderParameterHint::Kind::Toggle;
            else if (kind == "color")
                hint.Type = ShaderParameterHint::Kind::Color;
            else if (kind == "range" && (input >> hint.Min >> hint.Max) && std::isfinite(hint.Min) &&
                     std::isfinite(hint.Max) && hint.Min < hint.Max)
                hint.Type = ShaderParameterHint::Kind::Range;
            else
                return AssetError {AssetErrorCode::InvalidData,
                                   "Shader hint '" + key + "' must be toggle, color or range <min> <max>"};
            if ((input >> extra) || key.size() > 256 || !result.emplace(key, hint).second)
                return AssetError {AssetErrorCode::InvalidData, "Invalid or duplicate Shader hint '" + key + "'"};
        }
        return result;
    }
    std::variant<ShaderTextureFallbacks, AssetError> ParseShaderProgramTextureFallbacks(const std::string& text)
    {
        if (text.size() > 65536 || text.find('\0') != std::string::npos)
            return AssetError {AssetErrorCode::InvalidData, "Invalid Shader program text budget"};
        ShaderTextureFallbacks result;
        std::istringstream     lines(text);
        std::string            line;
        while (std::getline(lines, line))
        {
            if (line.rfind("texture ", 0) != 0)
                continue;
            const auto         split = line.find('=');
            std::istringstream name(line.substr(8, split == std::string::npos ? std::string::npos : split - 8));
            std::istringstream value(split == std::string::npos ? "" : line.substr(split + 1));
            std::string        key, role, extra;
            if (split == std::string::npos || !(name >> key) || (name >> extra) || !(value >> role) ||
                (value >> extra) || key.size() > 256 || result.count(key) ||
                (role != "white" && role != "black" && role != "normal"))
                return AssetError {AssetErrorCode::InvalidData,
                                   "Shader texture must be 'texture <name> = white | black | normal'"};
            result.emplace(key,
                           role == "normal" ? ShaderTextureFallback::Normal :
                           role == "black"  ? ShaderTextureFallback::Black :
                                              ShaderTextureFallback::White);
        }
        return result;
    }
    std::variant<std::vector<MaterialParameterGroup>, AssetError> ParseShaderProgramGroups(const std::string& text)
    {
        if (text.size() > 65536 || text.find('\0') != std::string::npos)
            return AssetError {AssetErrorCode::InvalidData, "Invalid Shader program text budget"};
        std::vector<MaterialParameterGroup> result;
        std::set<std::string>               names, members;
        std::istringstream                  lines(text);
        std::string                         line;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.rfind("group ", 0) != 0)
                continue;
            const auto split = line.find('=');
            if (split == std::string::npos)
                return AssetError {AssetErrorCode::InvalidData, "Shader group requires '='"};
            auto       name  = line.substr(6, split - 6);
            const auto first = name.find_first_not_of(" \t"), last = name.find_last_not_of(" \t");
            if (first == std::string::npos)
                return AssetError {AssetErrorCode::InvalidData, "Shader group requires a name"};
            name = name.substr(first, last - first + 1);
            if (name.size() > 256 || !names.insert(name).second ||
                std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
                return AssetError {AssetErrorCode::InvalidData, "Invalid or duplicate Shader group name"};
            MaterialParameterGroup group {name, {}};
            std::istringstream     input(line.substr(split + 1));
            std::string            member;
            while (input >> member)
            {
                if (member.size() > 256 || !members.insert(member).second)
                    return AssetError {AssetErrorCode::InvalidData, "Repeated Shader group member '" + member + "'"};
                group.Members.push_back(member);
            }
            if (group.Members.empty())
                return AssetError {AssetErrorCode::InvalidData, "Shader group requires members"};
            result.push_back(std::move(group));
        }
        return result;
    }
} // namespace Kans

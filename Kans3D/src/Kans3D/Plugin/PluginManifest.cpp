#include "PluginManifest.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

namespace Kans
{
    namespace
    {
        struct Version
        {
            uint64_t    Major = 0, Minor = 0, Patch = 0;
            std::string Pre;
        };
        Version Parse(const std::string& text)
        {
            static const std::regex pattern(
                R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$)");
            std::smatch m;
            if (!std::regex_match(text, m, pattern))
                throw std::runtime_error("Invalid semver: " + text);
            Version            v {std::stoull(m[1]), std::stoull(m[2]), std::stoull(m[3]), m[4]};
            std::istringstream parts(v.Pre);
            for (std::string part; std::getline(parts, part, '.');)
                if (part.size() > 1 && part.front() == '0' && part.find_first_not_of("0123456789") == std::string::npos)
                    throw std::runtime_error("Invalid semver prerelease: " + text);
            return v;
        }
        int Compare(const Version& a, const Version& b)
        {
            for (auto pair : {std::pair<uint64_t, uint64_t> {a.Major, b.Major}, {a.Minor, b.Minor}, {a.Patch, b.Patch}})
                if (pair.first != pair.second)
                    return pair.first < pair.second ? -1 : 1;
            if (a.Pre == b.Pre)
                return 0;
            if (a.Pre.empty() || b.Pre.empty())
                return a.Pre.empty() ? 1 : -1;
            std::istringstream x(a.Pre), y(b.Pre);
            std::string        p, q;
            while (true)
            {
                bool px = bool(std::getline(x, p, '.')), qy = bool(std::getline(y, q, '.'));
                if (!px || !qy)
                    return px ? 1 : qy ? -1 : 0;
                if (p == q)
                    continue;
                bool pn = p.find_first_not_of("0123456789") == std::string::npos;
                bool qn = q.find_first_not_of("0123456789") == std::string::npos;
                if (pn != qn)
                    return pn ? -1 : 1;
                if (pn && p.size() != q.size())
                    return p.size() < q.size() ? -1 : 1;
                return p < q ? -1 : 1;
            }
        }
        bool Range(const Version& v, const std::string& text)
        {
            bool   any   = false;
            size_t start = 0;
            do
            {
                const auto         end = text.find("||", start);
                std::istringstream tokens(text.substr(start, end == std::string::npos ? end : end - start));
                bool               match = true, nonempty = false;
                for (std::string token; tokens >> token;)
                {
                    nonempty = true;
                    if (token == "*")
                        continue;
                    auto pos = token.find_first_not_of("<>=^~");
                    if (pos == std::string::npos)
                        throw std::runtime_error("Missing range version");
                    const auto op    = token.substr(0, pos);
                    const auto bound = Parse(token.substr(pos));
                    const int  c     = Compare(v, bound);
                    bool       term;
                    if (op.empty() || op == "=")
                        term = c == 0;
                    else if (op == ">=")
                        term = c >= 0;
                    else if (op == "<=")
                        term = c <= 0;
                    else if (op == ">")
                        term = c > 0;
                    else if (op == "<")
                        term = c < 0;
                    else if (op == "^" || op == "~")
                    {
                        Version upper = bound;
                        upper.Pre.clear();
                        if (op == "~")
                        {
                            ++upper.Minor;
                            upper.Patch = 0;
                        }
                        else if (bound.Major)
                        {
                            ++upper.Major;
                            upper.Minor = upper.Patch = 0;
                        }
                        else if (bound.Minor)
                        {
                            ++upper.Minor;
                            upper.Patch = 0;
                        }
                        else
                            ++upper.Patch;
                        term = c >= 0 && Compare(v, upper) < 0;
                    }
                    else
                        throw std::runtime_error("Unknown range operator");
                    match &= term;
                }
                if (!nonempty)
                    throw std::runtime_error("Empty version range");
                any |= match;
                if (end == std::string::npos)
                    break;
                start = end + 2;
            } while (true);
            return any;
        }
        void Mapping(const YAML::Node& node, const std::string& name)
        {
            if (!node.IsMap())
                throw std::runtime_error(name + " must be a mapping");
            std::set<std::string> keys;
            for (const auto& pair : node)
                if (!pair.first.IsScalar() || !keys.insert(pair.first.as<std::string>()).second)
                    throw std::runtime_error(name + " contains duplicate or invalid keys");
        }
        std::filesystem::path Directory(const YAML::Node& node, const char* key)
        {
            if (!node[key])
                return key;
            const auto text = node[key].as<std::string>();
            if (text.empty())
                return {};
            std::filesystem::path path = std::filesystem::u8path(text);
            if (path.is_absolute() || text.find_first_of("\\:") != std::string::npos)
                throw std::runtime_error(std::string(key) + " must be a relative directory");
            for (const auto& part : path)
                if (part == "..")
                    throw std::runtime_error(std::string(key) + " must stay inside the plugin");
            return path.lexically_normal();
        }
    } // namespace
    bool IsPluginName(const std::string& name)
    {
        static const std::regex pattern("^[A-Za-z_][A-Za-z0-9_]*$");
        return name != "Kans3D" && std::regex_match(name, pattern);
    }
    bool IsPluginVersion(const std::string& text)
    {
        try
        {
            Parse(text);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
    bool IsPluginVersionRange(const std::string& text)
    {
        try
        {
            Range({}, text);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
    bool PluginVersionMatches(const std::string& version, const std::string& range)
    {
        try
        {
            return Range(Parse(version), range);
        }
        catch (...)
        {
            return false;
        }
    }
    std::variant<PluginManifest, AssetError> LoadPluginManifest(const std::filesystem::path& file,
                                                                const std::string&           engineVersion)
    {
        try
        {
            std::ifstream input(file);
            if (!input)
                return AssetError {AssetErrorCode::IoError, file.u8string() + ": Cannot read plugin manifest"};
            const auto docs = YAML::LoadAll(input);
            if (docs.size() != 1)
                throw std::runtime_error("Expected one manifest");
            const auto node = docs.front();
            Mapping(node, "Manifest");
            for (const char* field :
                 {"Name", "FriendlyName", "Version", "Description", "EngineVersion", "Type", "EnabledByDefault"})
                if (!node[field] || !node[field].IsScalar() || node[field].Scalar().empty())
                    throw std::runtime_error(std::string("Missing or invalid field ") + field);
            const std::set<std::string> allowed = {"Name",
                                                   "FriendlyName",
                                                   "Version",
                                                   "Description",
                                                   "EngineVersion",
                                                   "Type",
                                                   "EnabledByDefault",
                                                   "Dependencies",
                                                   "Modules",
                                                   "Shaders",
                                                   "Content"};
            for (const auto& pair : node)
                if (!allowed.count(pair.first.as<std::string>()))
                    throw std::runtime_error("Unknown manifest field " + pair.first.as<std::string>());
            PluginManifest m;
            m.Name = node["Name"].as<std::string>();
            if (!IsPluginName(m.Name))
                throw std::runtime_error("Invalid plugin Name " + m.Name);
            m.FriendlyName = node["FriendlyName"].as<std::string>();
            m.Description  = node["Description"].as<std::string>();
            m.Version      = node["Version"].as<std::string>();
            if (!IsPluginVersion(m.Version))
                throw std::runtime_error("Invalid semver Version " + m.Version);
            m.EngineVersion = node["EngineVersion"].as<std::string>();
            if (!IsPluginVersionRange(m.EngineVersion))
                throw std::runtime_error("Invalid EngineVersion range " + m.EngineVersion);
            if (!PluginVersionMatches(engineVersion, m.EngineVersion))
                throw std::runtime_error("Incompatible EngineVersion " + m.EngineVersion + " (engine " + engineVersion +
                                         ")");
            auto type = node["Type"].as<std::string>();
            if (type != "Runtime" && type != "Editor")
                throw std::runtime_error("Type must be Runtime or Editor");
            m.Type             = type == "Editor" ? PluginType::Editor : PluginType::Runtime;
            m.EnabledByDefault = node["EnabledByDefault"].as<bool>();
            if (node["Dependencies"])
            {
                Mapping(node["Dependencies"], "Dependencies");
                for (const auto& pair : node["Dependencies"])
                {
                    auto name = pair.first.as<std::string>(), range = pair.second.as<std::string>();
                    if (!IsPluginName(name) || !IsPluginVersionRange(range))
                        throw std::runtime_error("Invalid dependency " + name + ": " + range);
                    m.Dependencies.emplace(name, range);
                }
            }
            if (node["Modules"])
            {
                if (!node["Modules"].IsSequence())
                    throw std::runtime_error("Modules must be a list");
                std::set<std::string> seen;
                for (const auto& value : node["Modules"])
                {
                    auto name = value.as<std::string>();
                    if (!IsPluginName(name) || !seen.insert(name).second)
                        throw std::runtime_error("Invalid or duplicate module " + name);
                    m.Modules.push_back(name);
                }
            }
            m.Shaders = Directory(node, "Shaders");
            m.Content = Directory(node, "Content");
            for (const auto& path : {m.Shaders, m.Content})
                if (!path.empty() && std::filesystem::exists(file.parent_path() / path))
                {
                    const auto base     = std::filesystem::canonical(file.parent_path());
                    const auto target   = std::filesystem::canonical(base / path);
                    const auto relative = target.lexically_relative(base);
                    if (!std::filesystem::is_directory(target))
                        throw std::runtime_error(path.u8string() + " is not a directory");
                    for (const auto& part : relative)
                        if (part == "..")
                            throw std::runtime_error(path.u8string() + " escapes the plugin directory");
                }
            return m;
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, file.u8string() + ": " + e.what()};
        }
    }
    PluginResolution DiscoverPlugins(const std::filesystem::path&       engineRoot,
                                     const std::filesystem::path&       projectRoot,
                                     const std::map<std::string, bool>& overrides,
                                     const std::string&                 engineVersion)
    {
        PluginResolution result;
        for (bool owned : {false, true})
        {
            const auto& root = owned ? projectRoot : engineRoot;
            if (root.empty())
                continue;
            try
            {
                if (!std::filesystem::exists(root))
                    continue;
                std::vector<std::filesystem::path> files;
                for (const auto& entry : std::filesystem::directory_iterator(root))
                    if (entry.is_directory() && std::filesystem::exists(entry.path() / "kansplugin.yaml"))
                        files.push_back(entry.path() / "kansplugin.yaml");
                std::sort(files.begin(), files.end());
                for (const auto& file : files)
                {
                    DiscoveredPlugin plugin;
                    plugin.Directory    = std::filesystem::canonical(file.parent_path());
                    plugin.ProjectOwned = owned;
                    auto loaded         = LoadPluginManifest(file, engineVersion);
                    if (const auto* error = std::get_if<AssetError>(&loaded))
                    {
                        plugin.Manifest.Name = file.parent_path().filename().u8string();
                        plugin.Errors.push_back(error->Message);
                    }
                    else
                        plugin.Manifest = std::get<PluginManifest>(std::move(loaded));
                    const auto name = plugin.Manifest.Name;
                    if (auto found = result.Plugins.find(name); found != result.Plugins.end())
                    {
                        if (owned && !found->second.ProjectOwned)
                            result.Diagnostics.push_back("Project plugin " + name + " overrides engine plugin");
                        else
                        {
                            found->second.Errors.push_back("Duplicate plugin " + name);
                            continue;
                        }
                    }
                    result.Plugins[name] = std::move(plugin);
                }
            }
            catch (const std::exception& e)
            {
                result.Diagnostics.push_back(root.u8string() + ": " + e.what());
            }
        }
        ResolvePlugins(result, overrides);
        return result;
    }
    void ResolvePlugins(PluginResolution& result, const std::map<std::string, bool>& overrides)
    {
        result.Order.clear();
        result.Diagnostics.erase(
            std::remove_if(result.Diagnostics.begin(),
                           result.Diagnostics.end(),
                           [](const auto& text) { return text.rfind("Unknown plugin override ", 0) == 0; }),
            result.Diagnostics.end());
        for (auto& [name, plugin] : result.Plugins)
            plugin.Errors.erase(std::remove_if(plugin.Errors.begin(),
                                               plugin.Errors.end(),
                                               [](const auto& text) {
                                                   for (const char* prefix : {"Unknown dependency ",
                                                                              "Dependency ",
                                                                              "Runtime plugin cannot depend ",
                                                                              "Disabled dependency "})
                                                       if (text.rfind(prefix, 0) == 0)
                                                           return true;
                                                   return false;
                                               }),
                                plugin.Errors.end());
        for (const auto& [name, on] : overrides)
            if (!result.Plugins.count(name))
                result.Diagnostics.push_back("Unknown plugin override " + name);
        for (auto& [name, plugin] : result.Plugins)
        {
            auto found = overrides.find(name);
            plugin.Enabled =
                plugin.Errors.empty() && (found == overrides.end() ? plugin.Manifest.EnabledByDefault : found->second);
            for (const auto& [dep, range] : plugin.Manifest.Dependencies)
            {
                auto target = result.Plugins.find(dep);
                if (target == result.Plugins.end())
                    plugin.Errors.push_back("Unknown dependency " + dep);
                else if (!PluginVersionMatches(target->second.Manifest.Version, range))
                    plugin.Errors.push_back("Dependency " + dep + " does not satisfy " + range);
                else if (plugin.Manifest.Type == PluginType::Runtime &&
                         target->second.Manifest.Type == PluginType::Editor)
                    plugin.Errors.push_back("Runtime plugin cannot depend on Editor plugin " + dep);
            }
        }
        std::map<std::string, int>              marks;
        std::vector<std::string>                stack;
        std::function<void(const std::string&)> visit = [&](const std::string& name) {
            if (marks[name] == 2)
                return;
            if (marks[name] == 1)
            {
                auto        first = std::find(stack.begin(), stack.end(), name);
                std::string chain;
                for (auto it = first; it != stack.end(); ++it)
                    chain += *it + " -> ";
                chain += name;
                for (auto it = first; it != stack.end(); ++it)
                    result.Plugins.at(*it).Errors.push_back("Dependency cycle: " + chain);
                return;
            }
            marks[name] = 1;
            stack.push_back(name);
            auto& plugin = result.Plugins.at(name);
            for (const auto& [dep, range] : plugin.Manifest.Dependencies)
                if (result.Plugins.count(dep))
                    visit(dep);
            if (!plugin.Errors.empty())
                plugin.Enabled = false;
            for (const auto& [dep, range] : plugin.Manifest.Dependencies)
                if (result.Plugins.count(dep) && !result.Plugins.at(dep).Enabled)
                {
                    plugin.Errors.push_back("Disabled dependency " + dep + " disables " + name);
                    plugin.Enabled = false;
                }
            stack.pop_back();
            marks[name] = 2;
            if (plugin.Enabled)
                result.Order.push_back(name);
        };
        for (const auto& [name, plugin] : result.Plugins)
            visit(name);
        // A cycle may have invalidated a node visited earlier in the active stack.
        bool changed;
        do
        {
            changed = false;
            for (auto& [name, plugin] : result.Plugins)
                for (const auto& [dep, range] : plugin.Manifest.Dependencies)
                    if (plugin.Enabled && result.Plugins.count(dep) && !result.Plugins.at(dep).Enabled)
                    {
                        plugin.Enabled = false;
                        plugin.Errors.push_back("Disabled dependency " + dep + " disables " + name);
                        changed = true;
                    }
        } while (changed);
        result.Order.erase(std::remove_if(result.Order.begin(),
                                          result.Order.end(),
                                          [&](const auto& name) { return !result.Plugins.at(name).Enabled; }),
                           result.Order.end());
    }
} // namespace Kans

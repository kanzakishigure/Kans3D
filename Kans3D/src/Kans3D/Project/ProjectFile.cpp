#include "ProjectFile.h"
#include <algorithm>
#include <fstream>
#include <limits>
#include "Kans3D/Physics/PhysicsSerialization.h"
#include "ProjectYaml.h"

namespace Kans
{
    namespace fs = std::filesystem;
    namespace
    {
        // Treat Windows separators/drive roots consistently even when reading a project on Linux.
        bool RelativePath(const fs::path& path)
        {
            auto text = path.generic_u8string();
            std::replace(text.begin(), text.end(), '\\', '/');
            if (text.find('\0') != std::string::npos || (!text.empty() && text.front() == '/') ||
                (text.size() >= 2 && text[1] == ':') || path.has_root_path())
                return false;
            for (const auto& component : fs::u8path(text))
                if (component == "..")
                    return false;
            return true;
        }
        std::optional<AssetError> Validate(const fs::path& file, const ProjectFile& project)
        {
            if (project.Name.empty())
                return ProjectYaml::Error(AssetErrorCode::InvalidData, file, "Missing or empty Name");
            if (project.FormatVersion > 1)
                return ProjectYaml::Error(AssetErrorCode::UnsupportedVersion, file, "Unsupported FormatVersion");
            if (!RelativePath(project.StartupScene) || !RelativePath(project.AssetDirectory) ||
                !RelativePath(project.CacheDirectory))
                return ProjectYaml::Error(
                    AssetErrorCode::InvalidData, file, "Project paths must be relative without '..' components");
            return std::nullopt;
        }
    } // namespace

    bool IsValidProjectName(const std::string& name)
    {
        return !name.empty() && name != "." && name != ".." &&
               std::none_of(name.begin(), name.end(), [](unsigned char c) {
                   return c < 32 || c == 127 || std::string("<>:\"/\\|?*").find(c) != std::string::npos;
               });
    }

    std::variant<ProjectFile, AssetError> LoadProjectFile(const fs::path& kproj)
    {
        try
        {
            std::ifstream input(kproj);
            if (!input)
                return ProjectYaml::Error(AssetErrorCode::IoError, kproj, "Cannot read project file");
            const auto documents = YAML::LoadAll(input);
            if (input.bad())
                return ProjectYaml::Error(AssetErrorCode::IoError, kproj, "Cannot read project file");
            if (documents.size() != 1 || !documents.front().IsMap())
                return ProjectYaml::Error(AssetErrorCode::InvalidData, kproj, "Expected one project YAML mapping");
            const auto            node = documents.front();
            std::set<std::string> keys;
            for (const auto& pair : node)
                if (!pair.first.IsScalar() || !keys.insert(pair.first.as<std::string>()).second)
                    return ProjectYaml::Error(AssetErrorCode::InvalidData, kproj, "Invalid or duplicate project key");
            ProjectFile project;
            project.Name = node["Name"].as<std::string>();
            if (node["FormatVersion"])
                project.FormatVersion = node["FormatVersion"].as<uint32_t>();
            if (node["StartupScene"])
                project.StartupScene = fs::u8path(node["StartupScene"].as<std::string>());
            if (node["AssetDirectory"])
                project.AssetDirectory = fs::u8path(node["AssetDirectory"].as<std::string>());
            if (node["CacheDirectory"])
                project.CacheDirectory = fs::u8path(node["CacheDirectory"].as<std::string>());
            if (node["PhysicsSettings"])
                PhysicsYaml::Decode(node["PhysicsSettings"], project.Physics);
            if (auto error = Validate(kproj, project))
                return *error;
            return project;
        }
        catch (const YAML::BadFile& e)
        {
            return ProjectYaml::Error(AssetErrorCode::IoError, kproj, e.what());
        }
        catch (const std::exception& e)
        {
            return ProjectYaml::Error(AssetErrorCode::InvalidData, kproj, e.what());
        }
    }

    std::optional<AssetError> SaveProjectFile(const fs::path& kproj, const ProjectFile& project)
    {
        if (auto error = Validate(kproj, project))
            return error;
        try
        {
            PhysicsYaml::Encode(project.Physics);
        }
        catch (const std::exception& e)
        {
            return ProjectYaml::Error(AssetErrorCode::InvalidData, kproj, e.what());
        }
        YAML::Emitter out;
        out.SetFloatPrecision(std::numeric_limits<float>::max_digits10);
        out.SetDoublePrecision(std::numeric_limits<double>::max_digits10);
        out << YAML::BeginMap << YAML::Key << "PhysicsSettings" << YAML::Value << PhysicsYaml::Encode(project.Physics)
            << YAML::Key << "Name" << YAML::Value << project.Name << YAML::Key << "FormatVersion" << YAML::Value
            << project.FormatVersion << YAML::Key << "StartupScene" << YAML::Value
            << project.StartupScene.generic_u8string() << YAML::Key << "AssetDirectory" << YAML::Value
            << project.AssetDirectory.generic_u8string() << YAML::Key << "CacheDirectory" << YAML::Value
            << project.CacheDirectory.generic_u8string() << YAML::EndMap;
        return ProjectYaml::Save(kproj, out);
    }

    std::variant<fs::path, AssetError> FindProjectFile(const fs::path& directory)
    {
        try
        {
            fs::path found;
            for (const auto& entry : fs::directory_iterator(directory))
                if (entry.path().extension() == ".kproj" && entry.is_regular_file())
                {
                    if (!found.empty())
                        return ProjectYaml::Error(AssetErrorCode::InvalidData, directory, "Multiple .kproj files");
                    found = entry.path();
                }
            if (found.empty())
                return ProjectYaml::Error(AssetErrorCode::NotFound, directory, "No .kproj file");
            return found;
        }
        catch (const fs::filesystem_error& e)
        {
            return ProjectYaml::Error(AssetErrorCode::IoError, directory, e.what());
        }
    }

    ProjectConfig MakeProjectConfig(const fs::path& root, const ProjectFile& project)
    {
        ProjectConfig config;
        config.Physics        = project.Physics;
        config.Name           = project.Name;
        config.RootDirectory  = fs::absolute(root).lexically_normal();
        config.AssetDirectory = project.AssetDirectory;
        config.CacheDirectory = project.CacheDirectory;
        return config;
    }

    std::variant<fs::path, AssetError>
    CreateProjectFromTemplate(const fs::path& templateDir, const fs::path& parentDir, const std::string& name)
    {
        const auto target = parentDir / fs::u8path(name);
        if (!IsValidProjectName(name))
            return ProjectYaml::Error(AssetErrorCode::InvalidArgument, target, "Invalid project name");
        bool created = false;
        try
        {
            auto found = FindProjectFile(templateDir);
            if (auto* error = std::get_if<AssetError>(&found))
                return *error;
            const auto original = std::get<fs::path>(found);
            auto       loaded   = LoadProjectFile(original);
            if (auto* error = std::get_if<AssetError>(&loaded))
                return *error;
            auto       project         = std::get<ProjectFile>(loaded);
            const auto sourceRoot      = fs::canonical(templateDir);
            const auto destinationRoot = fs::weakly_canonical(fs::absolute(target));
            // Copying a template into itself would recursively copy the new destination forever.
            auto relative = destinationRoot.lexically_relative(sourceRoot);
            if (!relative.empty() && *relative.begin() != "..")
                return ProjectYaml::Error(AssetErrorCode::InvalidArgument, target, "Target is inside the template");
            if (fs::exists(target) && (!fs::is_directory(target) || fs::is_symlink(target) || !fs::is_empty(target)))
                return ProjectYaml::Error(
                    AssetErrorCode::InvalidArgument, target, "Target already exists and is not empty");
            if (!fs::exists(target))
            {
                // Do not claim ownership if another caller won the race to create this directory.
                created = fs::create_directory(target);
                if (!created)
                    return ProjectYaml::Error(AssetErrorCode::IoError, target, "Could not create target directory");
            }
            auto cache = project.CacheDirectory.lexically_normal();
            if (cache.filename().empty())
                cache = cache.parent_path();
            for (fs::recursive_directory_iterator it(sourceRoot), end; it != end; ++it)
            {
                const auto local = it->path().lexically_relative(sourceRoot);
                if (it->is_directory() && (local == cache || local == "Scripts/bin" || local == "Scripts/obj"))
                {
                    it.disable_recursion_pending();
                    continue;
                }
                if (it->is_symlink())
                    fs::copy_symlink(it->path(), target / local);
                else if (it->is_directory())
                    fs::create_directory(target / local);
                else
                    fs::copy_file(it->path(), target / local);
            }
            const auto renamed = target / fs::u8path(name + ".kproj");
            const auto copied  = target / original.filename();
            if (copied != renamed)
                fs::rename(copied, renamed);
            project.Name = name;
            if (auto error = SaveProjectFile(renamed, project))
            {
                if (created)
                {
                    std::error_code ec;
                    fs::remove_all(target, ec);
                }
                return *error;
            }
            return renamed;
        }
        catch (const fs::filesystem_error& e)
        {
            if (created)
            {
                std::error_code ec;
                fs::remove_all(target, ec);
            }
            return ProjectYaml::Error(AssetErrorCode::IoError, target, e.what());
        }
    }
} // namespace Kans

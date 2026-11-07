#include "KansEditorCore/Launcher/LauncherModel.h"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include "Kans3D/Project/ProjectFile.h"

namespace Kans::Launcher
{
    namespace fs = std::filesystem;
    namespace
    {
        AssetError Error(AssetErrorCode code, const fs::path& path, const std::string& message)
        {
            return {code, path.generic_u8string() + ": " + message};
        }
        bool HoldsProjectFile(const fs::path& folder)
        {
            std::error_code ec;
            for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
                if (it->path().extension() == ".kproj" && it->is_regular_file(ec))
                    return true;
            return false;
        }
        void FindProjects(const fs::path& folder, int depth, std::vector<std::string>& found)
        {
            std::error_code ec;
            for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
            {
                if (!it->is_directory(ec) || it->is_symlink(ec))
                    continue;
                if (HoldsProjectFile(it->path()))
                    found.push_back(it->path().lexically_relative(folder).generic_u8string());
                else if (depth > 1)
                    for (const auto& name : [&] {
                             std::vector<std::string> inner;
                             FindProjects(it->path(), depth - 1, inner);
                             return inner;
                         }())
                        found.push_back(it->path().filename().generic_u8string() + "/" + name);
            }
        }
        // A project may neither hold other projects nor sit inside one: their asset folders and
        // caches would overlap, and the same assets would be imported twice under one ID.
        std::optional<AssetError> NestingProblem(const fs::path& root)
        {
            std::vector<std::string> inner;
            FindProjects(root, 2, inner);
            if (!inner.empty())
            {
                std::sort(inner.begin(), inner.end());
                std::string names;
                for (const auto& name : inner)
                    names += (names.empty() ? "" : ", ") + name;
                return Error(AssetErrorCode::InvalidArgument,
                             root,
                             "This folder holds other projects (" + names + "); open one of them instead");
            }
            for (auto parent = root.parent_path(); !parent.empty(); parent = parent.parent_path())
            {
                if (HoldsProjectFile(parent))
                    return Error(AssetErrorCode::InvalidArgument,
                                 root,
                                 "This folder is inside the project at " + parent.generic_u8string());
                if (parent == parent.parent_path())
                    break;
            }
            return std::nullopt;
        }
    } // namespace

    Selection ResolveSelection(const fs::path& path)
    {
        std::error_code ec;
        if (fs::is_directory(path, ec))
        {
            auto found = FindProjectFile(path);
            if (auto* error = std::get_if<AssetError>(&found))
            {
                if (error->Code == AssetErrorCode::NotFound)
                {
                    const auto folder = fs::absolute(path).lexically_normal();
                    if (auto problem = NestingProblem(folder))
                        return *problem;
                    return MissingProjectFile {folder};
                }
                return *error;
            }
            return ResolveSelection(std::get<fs::path>(found));
        }
        if (!fs::is_regular_file(path, ec))
            return Error(AssetErrorCode::NotFound, path, "Not found");
        if (path.extension() != ".kproj")
            return Error(AssetErrorCode::InvalidArgument, path, "Not a project file (.kproj)");
        // Opening one of several project files in a folder would be ambiguous about its assets.
        auto unique = FindProjectFile(path.parent_path());
        if (auto* error = std::get_if<AssetError>(&unique))
            return *error;
        auto loaded = LoadProjectFile(path);
        if (auto* error = std::get_if<AssetError>(&loaded))
            return *error;
        const auto file = fs::absolute(path).lexically_normal();
        if (auto problem = NestingProblem(file.parent_path()))
            return *problem;
        return file;
    }

    std::variant<fs::path, AssetError> CreateProjectFile(const fs::path& folder)
    {
        const auto root = fs::absolute(folder).lexically_normal();
        auto       name = (root.has_filename() ? root.filename() : root.parent_path().filename()).u8string();
        if (!IsValidProjectName(name))
            return Error(AssetErrorCode::InvalidArgument, root, "The folder name cannot be used as a project name");
        auto existing = FindProjectFile(root);
        if (std::holds_alternative<fs::path>(existing))
            return Error(AssetErrorCode::InvalidArgument, root, "The folder already has a project file");
        if (auto problem = NestingProblem(root))
            return *problem;
        ProjectFile project;
        project.Name = name;
        std::error_code ec;
        fs::create_directories(root / project.AssetDirectory, ec);
        if (ec)
            return Error(AssetErrorCode::IoError, root / project.AssetDirectory, ec.message());
        const auto file = root / fs::u8path(name + ".kproj");
        if (auto error = SaveProjectFile(file, project))
            return *error;
        return file;
    }

    std::vector<ProjectTemplate> ListTemplates(const fs::path& templatesDir)
    {
        std::vector<ProjectTemplate> templates;
        std::error_code              ec;
        for (fs::directory_iterator it(templatesDir, ec), end; !ec && it != end; it.increment(ec))
        {
            if (!it->is_directory(ec))
                continue;
            auto found = FindProjectFile(it->path());
            if (auto* file = std::get_if<fs::path>(&found))
            {
                auto loaded = LoadProjectFile(*file);
                if (auto* project = std::get_if<ProjectFile>(&loaded))
                    templates.push_back({project->Name, it->path()});
            }
        }
        std::sort(templates.begin(), templates.end(), [](const auto& a, const auto& b) {
            if ((a.Name == "Blank") != (b.Name == "Blank"))
                return a.Name == "Blank";
            return a.Name < b.Name;
        });
        return templates;
    }

    std::optional<std::string> NewProjectProblem(const fs::path& location, const std::string& name)
    {
        if (name.empty())
            return "Enter a project name.";
        if (!IsValidProjectName(name))
            return "The name cannot contain < > : \" / \\ | ? * or control characters.";
        if (location.empty())
            return "Choose a location.";
        if (!location.is_absolute())
            return "The location must be a full path.";
        std::error_code ec;
        const auto      target = location / fs::u8path(name);
        if (fs::exists(target, ec) && (!fs::is_directory(target, ec) || !fs::is_empty(target, ec)))
            return "A non-empty folder with this name already exists.";
        if (fs::exists(location, ec) && !fs::is_directory(location, ec))
            return "The location is a file, not a folder.";
        return std::nullopt;
    }

    std::string UniqueProjectName(const fs::path& location, const std::string& base)
    {
        for (int i = 1;; ++i)
        {
            auto name = i == 1 ? base : base + std::to_string(i);
            if (!NewProjectProblem(location, name))
                return name;
            if (i > 999)
                return base;
        }
    }

    fs::path DefaultLocation(const std::vector<fs::path>& recentProjectFiles, const fs::path& home)
    {
        // A project lives in <location>/<Name>/<Name>.kproj.
        for (const auto& file : recentProjectFiles)
        {
            std::error_code ec;
            const auto      location = file.parent_path().parent_path();
            if (!location.empty() && fs::is_directory(location, ec))
                return location;
        }
        return home / "Kans3D Projects";
    }

    fs::path HomeDirectory()
    {
#ifdef _WIN32
        if (const auto* profile = std::getenv("USERPROFILE"); profile && *profile)
            return fs::u8path(profile);
#else
        if (const auto* home = std::getenv("HOME"); home && *home)
            return fs::u8path(home);
#endif
        std::error_code ec;
        return fs::current_path(ec);
    }

    std::string FormatLastOpened(int64_t now, int64_t then)
    {
        const auto elapsed = now - then;
        const auto plural  = [](int64_t count, const char* unit) {
            return std::to_string(count) + " " + unit + (count == 1 ? "" : "s") + " ago";
        };
        if (then <= 0)
            return "never";
        if (elapsed < 60)
            return "just now";
        if (elapsed < 3600)
            return plural(elapsed / 60, "minute");
        if (elapsed < 86400)
            return plural(elapsed / 3600, "hour");
        if (elapsed < 2 * 86400)
            return "yesterday";
        if (elapsed < 30 * 86400)
            return plural(elapsed / 86400, "day");
        std::time_t time = static_cast<std::time_t>(then);
        std::tm     local {};
#ifdef _WIN32
        localtime_s(&local, &time);
#else
        localtime_r(&time, &local);
#endif
        char text[32];
        std::strftime(text, sizeof(text), "%Y-%m-%d", &local);
        return text;
    }

    LaunchArguments ParseLaunchArguments(int argc, const char* const* argv)
    {
        LaunchArguments arguments;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) != "--project")
                continue;
            if (!arguments.Project.empty() || i + 1 >= argc || std::string(argv[i + 1]).empty() ||
                std::string(argv[i + 1]).rfind("--", 0) == 0)
            {
                arguments.Error   = "--project requires exactly one project path";
                arguments.Project.clear();
                return arguments;
            }
            arguments.Project = fs::absolute(fs::u8path(argv[++i])).lexically_normal();
        }
        return arguments;
    }

    fs::path LayoutFile(const fs::path& projectRoot, const fs::path& cacheDirectory)
    {
        return (projectRoot / cacheDirectory / "Editor" / "imgui.ini").lexically_normal();
    }
} // namespace Kans::Launcher

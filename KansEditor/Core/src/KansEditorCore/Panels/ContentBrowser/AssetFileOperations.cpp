#include "KansEditorCore/Panels/ContentBrowser/AssetFileOperations.h"
#include <set>
#include "KansEditorCore/Panels/ContentBrowser/BrowserModel.h"

namespace Kans::Browser
{
    namespace
    {
        fs::path Normal(const fs::path& path)
        {
            std::error_code error;
            auto            result = fs::weakly_canonical(path, error);
            return error ? path.lexically_normal() : result;
        }

        fs::path Sidecar(const fs::path& file) { return fs::path(file.native() + fs::path(".kmeta").native()); }

        AssetError Failure(AssetErrorCode code, std::string message) { return AssetError {code, std::move(message)}; }
    } // namespace

    fs::path AssetFileOperations::AssetRoot() const
    {
        return m_Service ? Normal(m_Service->GetConfig().AssetDirectory) : fs::path();
    }

    std::optional<AssetError> AssetFileOperations::RequireIdle() const
    {
        if (!m_Service)
            return Failure(AssetErrorCode::InvalidArgument, "The project is not open");
        auto snapshot = m_Service->GetSnapshot();
        if (const auto* error = std::get_if<AssetError>(&snapshot))
            return *error;
        if (std::get<EditorAssetImportSnapshot>(snapshot).State == EditorAssetImportState::Building)
            return Failure(AssetErrorCode::InvalidArgument, "An import is running; try again when it finishes");
        return std::nullopt;
    }

    std::optional<AssetError> AssetFileOperations::CheckInside(const fs::path& path) const
    {
        const auto root = AssetRoot();
        if (Normal(path) == root || !IsWithin(Normal(path), root))
            return Failure(AssetErrorCode::InvalidArgument, path.u8string() + " is not inside the asset folder");
        return std::nullopt;
    }

    std::optional<SourceAssetMetadata> AssetFileOperations::FindSource(const fs::path& file) const
    {
        if (!m_Service)
            return std::nullopt;
        const AssetPath logical(Normal(file).lexically_relative(AssetRoot()));
        auto            sources = m_Service->GetSources();
        if (const auto* list = std::get_if<std::vector<SourceAssetMetadata>>(&sources))
            for (const auto& source : *list)
                if (source.Exists && source.Path == logical)
                    return source;
        return std::nullopt;
    }

    std::vector<SourceAssetMetadata> AssetFileOperations::SourcesWithin(const fs::path& directory) const
    {
        std::vector<SourceAssetMetadata> result;
        if (!m_Service)
            return result;
        const auto root   = AssetRoot();
        const auto folder = Normal(directory);
        auto       sources = m_Service->GetSources();
        if (const auto* list = std::get_if<std::vector<SourceAssetMetadata>>(&sources))
            for (const auto& source : *list)
                if (source.Exists && IsWithin((root / source.Path.GetPath()).lexically_normal(), folder))
                    result.push_back(source);
        return result;
    }

    std::variant<std::monostate, AssetError> AssetFileOperations::Refresh()
    {
        return m_Service->RefreshSources();
    }

    std::variant<std::monostate, AssetError> AssetFileOperations::MoveOne(const fs::path& from, const fs::path& to)
    {
        std::error_code error;
        if (fs::exists(to, error))
            return Failure(AssetErrorCode::DuplicatePath, to.filename().u8string() + " already exists");
        if (!fs::is_directory(from, error))
            if (const auto source = FindSource(from))
                // The project moves the file and its sidecar together, with a journal.
                return m_Service->MoveSourceAsset(source->sourceAssetID,
                                                  AssetPath(Normal(to).lexically_relative(AssetRoot())));
        // Folders keep the sidecars next to their files; a rescan then finds every
        // source at its new path with the same identity.
        fs::rename(from, to, error);
        if (!error && !fs::is_directory(to) && fs::exists(Sidecar(from)))
            fs::rename(Sidecar(from), Sidecar(to), error);
        if (error)
            return Failure(AssetErrorCode::IoError, "Cannot move " + from.filename().u8string() + ": " + error.message());
        return std::monostate {};
    }

    std::variant<fs::path, AssetError> AssetFileOperations::CreateFolder(const fs::path& parent, const std::string& name)
    {
        if (auto error = RequireIdle())
            return *error;
        if (!ValidName(name))
            return Failure(AssetErrorCode::InvalidArgument, "Invalid folder name");
        if (Normal(parent) != AssetRoot())
            if (auto error = CheckInside(parent))
                return *error;
        const auto      folder = UniqueChildPath(parent, name, "");
        std::error_code error;
        if (!fs::create_directory(folder, error))
            return Failure(AssetErrorCode::IoError, "Cannot create folder: " + error.message());
        return folder;
    }

    std::variant<fs::path, AssetError> AssetFileOperations::Rename(const fs::path& item, const std::string& name)
    {
        if (auto error = RequireIdle())
            return *error;
        if (auto error = CheckInside(item))
            return *error;
        if (!ValidName(name))
            return Failure(AssetErrorCode::InvalidArgument, "Invalid name: " + name);
        const auto target = item.parent_path() / fs::u8path(name);
        if (target == item)
            return target;
        auto moved = MoveOne(item, target);
        if (const auto* error = std::get_if<AssetError>(&moved))
            return *error;
        auto refreshed = Refresh();
        if (const auto* error = std::get_if<AssetError>(&refreshed))
            return *error;
        return target;
    }

    std::variant<std::vector<fs::path>, AssetError> AssetFileOperations::Move(const std::vector<fs::path>& items,
                                                                              const fs::path&              directory)
    {
        if (auto error = RequireIdle())
            return *error;
        if (Normal(directory) != AssetRoot())
            if (auto error = CheckInside(directory))
                return *error;
        std::vector<fs::path>     moved;
        std::optional<AssetError> failure;
        for (const auto& item : items)
        {
            if (auto error = CheckInside(item))
            {
                failure = *error;
                break;
            }
            if (IsWithin(Normal(directory), Normal(item)))
            {
                failure = Failure(AssetErrorCode::InvalidArgument,
                                  "Cannot move " + item.filename().u8string() + " into itself");
                break;
            }
            const auto target = directory / item.filename();
            if (Normal(target) == Normal(item))
                continue;
            auto result = MoveOne(item, target);
            if (const auto* error = std::get_if<AssetError>(&result))
            {
                failure = *error;
                break;
            }
            moved.push_back(target);
        }
        auto refreshed = Refresh();
        if (failure)
            return *failure;
        if (const auto* error = std::get_if<AssetError>(&refreshed))
            return *error;
        return moved;
    }

    std::variant<std::vector<fs::path>, AssetError> AssetFileOperations::Copy(const std::vector<fs::path>& items,
                                                                              const fs::path&              directory)
    {
        if (auto error = RequireIdle())
            return *error;
        if (Normal(directory) != AssetRoot())
            if (auto error = CheckInside(directory))
                return *error;
        std::vector<fs::path>                                         copies;
        std::vector<std::pair<SourceImportConfiguration, fs::path>> settings; // Import settings to carry over.
        const auto keepSettings = [&](const fs::path& original, const fs::path& copy) {
            if (const auto source = FindSource(original);
                source && (!source->Import.Importer.empty() || !source->Import.Settings.empty()))
                settings.emplace_back(source->Import, copy);
        };
        std::error_code error;
        for (const auto& item : items)
        {
            if (auto failure = CheckInside(item))
                return *failure;
            if (fs::is_directory(item, error))
            {
                if (IsWithin(Normal(directory), Normal(item)))
                    return Failure(AssetErrorCode::InvalidArgument,
                                   "Cannot copy " + item.filename().u8string() + " into itself");
                const auto target = UniqueChildPath(directory, item.filename().u8string(), "");
                fs::create_directories(target, error);
                // Sidecars are not copied: each copy is a new source.
                for (fs::recursive_directory_iterator it(item, error), end; !error && it != end; it.increment(error))
                {
                    if (IsHidden(it->path()))
                        continue;
                    const auto destination = target / it->path().lexically_relative(item);
                    if (it->is_directory())
                        fs::create_directories(destination, error);
                    else if (fs::copy_file(it->path(), destination, error))
                        keepSettings(it->path(), destination);
                }
                if (error)
                    return Failure(AssetErrorCode::IoError, "Cannot copy folder: " + error.message());
                copies.push_back(target);
                continue;
            }
            const auto target =
                UniqueChildPath(directory, item.stem().u8string(), item.extension().u8string());
            if (!fs::copy_file(item, target, error))
                return Failure(AssetErrorCode::IoError, "Cannot copy " + item.filename().u8string() + ": " + error.message());
            keepSettings(item, target);
            copies.push_back(target);
        }
        auto refreshed = Refresh();
        if (const auto* failure = std::get_if<AssetError>(&refreshed))
            return *failure;
        for (const auto& [configuration, copy] : settings)
            if (const auto source = FindSource(copy))
            {
                auto set = m_Service->SetSourceImportConfiguration(source->sourceAssetID, configuration);
                if (const auto* failure = std::get_if<AssetError>(&set))
                    return *failure;
            }
        return copies;
    }

    std::variant<DeletionPreview, AssetError> AssetFileOperations::PlanDelete(const std::vector<fs::path>& items) const
    {
        if (!m_Service)
            return Failure(AssetErrorCode::InvalidArgument, "The project is not open");
        DeletionPreview           preview;
        std::set<SourceAssetID>   deleted;
        std::vector<SourceAssetID> sources;
        std::error_code           error;
        for (const auto& item : items)
        {
            if (fs::is_directory(item, error))
            {
                ++preview.Folders;
                for (fs::recursive_directory_iterator it(item, error), end; !error && it != end; it.increment(error))
                    if (!it->is_directory() && !IsHidden(it->path()))
                        ++preview.Files;
                for (const auto& source : SourcesWithin(item))
                    if (deleted.insert(source.sourceAssetID).second)
                        sources.push_back(source.sourceAssetID);
                continue;
            }
            ++preview.Files;
            if (const auto source = FindSource(item); source && deleted.insert(source->sourceAssetID).second)
                sources.push_back(source->sourceAssetID);
        }
        std::set<SourceAssetID> listed;
        for (const auto id : sources)
        {
            auto plan = m_Service->PlanSourceAssetDeletion(id);
            if (const auto* failure = std::get_if<AssetError>(&plan))
                return *failure;
            for (const auto& dependent : std::get<SourceAssetDeletionPlan>(plan).Dependents)
                if (!deleted.count(dependent.sourceAssetID) && listed.insert(dependent.sourceAssetID).second)
                    preview.Dependents.push_back(dependent);
        }
        return preview;
    }

    std::variant<std::monostate, AssetError> AssetFileOperations::Delete(const std::vector<fs::path>& items)
    {
        if (auto error = RequireIdle())
            return *error;
        for (const auto& item : items)
        {
            if (auto error = CheckInside(item))
                return *error;
            std::error_code error;
            const bool      folder = fs::is_directory(item, error);
            // Sources go through the project so their Products are unpublished.
            std::vector<SourceAssetMetadata> sources;
            if (folder)
                sources = SourcesWithin(item);
            else if (auto source = FindSource(item))
                sources.push_back(*source);
            for (const auto& source : sources)
            {
                auto removed = m_Service->DeleteSourceAsset(source.sourceAssetID);
                if (const auto* failure = std::get_if<AssetError>(&removed))
                    return *failure;
            }
            if (folder)
                fs::remove_all(item, error);
            else if (fs::exists(item, error))
            {
                fs::remove(item, error);
                if (!error && fs::exists(Sidecar(item)))
                    fs::remove(Sidecar(item), error);
            }
            if (error)
                return Failure(AssetErrorCode::IoError, "Cannot delete " + item.filename().u8string() + ": " + error.message());
        }
        return Refresh();
    }
} // namespace Kans::Browser

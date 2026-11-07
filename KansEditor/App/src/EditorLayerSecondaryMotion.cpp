#include <imgui.h>
#include "KansEditorCore/EditorAssetImportService.h"
#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "Components/ComponentEditors.h"
#include "EditorLayer.h"
#include "KansEditorCore/Physics/SecondaryMotionEditing.h"
#include "UI/EditorIcons.h"
#include "KansEditorCore/UI/SourceBuild.h"

namespace Kans
{
    namespace Edit = SecondaryMotionEditing;

    void EditorLayer::BindSecondaryMotion()
    {
        PropertiesPanel::ClothActions actions;
        actions.CreateAsset = [this](Entity entity, uint32_t section, bool browserFolder) {
            CreateClothAsset(entity, section, browserFolder);
        };
        actions.Paint      = [this](Entity entity) { BeginClothPaint(entity); };
        actions.IsPainting = [this](Entity entity) { return m_ViewportPanel.GetClothPaint().IsPainting(entity); };
        m_PropertiesPanel.SetClothActions(std::move(actions));
        m_ViewportPanel.GetClothPaint().SetSavedCallback([this](const std::filesystem::path& file) {
            CORE_INFO("{}: {}", file.filename().u8string(), BuildSavedSource(m_AssetImports.get(), file));
            m_ContentBrowserPanel.Refresh();
        });
    }

    std::filesystem::path EditorLayer::SourceFileOf(AssetID asset)
    {
        if (!m_AssetImports || !asset.IsValid())
            return {};
        auto sources = m_AssetImports->GetSources();
        if (const auto* list = std::get_if<std::vector<SourceAssetMetadata>>(&sources))
            for (const auto& source : *list)
                if (source.sourceAssetID == asset.GetSourceID())
                    return m_AssetImports->GetConfig().AssetDirectory / source.Path.GetPath();
        return {};
    }

    AssetID EditorLayer::FindPublished(const std::filesystem::path& file, AssetType type)
    {
        if (!m_AssetImports)
            return {};
        std::error_code error;
        const auto      wanted  = std::filesystem::weakly_canonical(file, error);
        auto            sources = m_AssetImports->GetSources();
        if (const auto* list = std::get_if<std::vector<SourceAssetMetadata>>(&sources))
            for (const auto& source : *list)
                if (std::filesystem::weakly_canonical(m_AssetImports->GetConfig().AssetDirectory / source.Path.GetPath(),
                                                      error) == wanted)
                    for (const auto& product : m_AssetImports->GetRuntimeAssetRegistry()->GetAllProducts())
                        if (product.Metadata.assetID.GetSourceID() == source.sourceAssetID &&
                            product.Metadata.Type == type && product.Target == EditorAssetTarget)
                            return product.Metadata.assetID;
        return {};
    }

    void EditorLayer::CreateClothAsset(Entity entity, uint32_t section, bool browserFolder)
    {
        if (!m_ActiveScene || !entity || m_Play.IsActive())
            return;
        auto mesh = m_ActiveScene->ResolveMesh(entity);
        if (!mesh)
        {
            Record(AssetError {AssetErrorCode::NotFound, "The entity has no loaded mesh"});
            return;
        }
        const AssetID meshID =
            entity.HasComponent<MeshComponent>() ? entity.GetComponent<MeshComponent>().Mesh : mesh->assetID;
        const auto& data   = mesh->GetData();
        const auto  source = SourceFileOf(meshID);
        std::filesystem::path directory =
            browserFolder || source.empty() ? m_ContentBrowserPanel.GetCurrentFolder() : source.parent_path();
        if (directory.empty() && m_AssetImports)
            directory = m_AssetImports->GetConfig().AssetDirectory;
        // "<model>_<slot>.cloth", so the file says which part it simulates.
        std::string stem = source.empty() ? entity.GetComponent<TagComponent>().Tag : source.stem().u8string();
        if (section < data.Sections.size() && data.Sections[section].MaterialSlot < data.MaterialSlots.size() &&
            !data.MaterialSlots[data.Sections[section].MaterialSlot].Name.empty())
            stem += "_" + data.MaterialSlots[data.Sections[section].MaterialSlot].Name;
        else
            stem += "_Section" + std::to_string(section);
        for (char& c : stem)
            if (std::string("\\/:*?\"<>|").find(c) != std::string::npos)
                c = '_';
        auto created = Edit::CreateClothFile(directory, stem, meshID, data, section);
        if (const auto* error = std::get_if<AssetError>(&created))
        {
            Record(*error);
            return;
        }
        const auto file = std::get<std::filesystem::path>(created);
        CORE_INFO("{}: {}", file.filename().u8string(), BuildSavedSource(m_AssetImports.get(), file));
        m_PendingCloth.push_back({file, entity.GetUUID(), section});
        m_ContentBrowserPanel.Refresh();
    }

    void EditorLayer::AssignPendingCloth()
    {
        bool assigned = false;
        m_PendingCloth.erase(std::remove_if(m_PendingCloth.begin(),
                                            m_PendingCloth.end(),
                                            [&](const PendingCloth& pending) {
                                                const AssetID cloth = FindPublished(pending.File, AssetType::Cloth);
                                                if (!cloth.IsValid())
                                                    return false;
                                                if (m_ActiveScene)
                                                    if (Entity e = m_ActiveScene->GetEntityByUUID(pending.Entity);
                                                        e && e.HasComponent<ClothComponent>())
                                                    {
                                                        e.PatchComponent<ClothComponent>([&](ClothComponent& c) {
                                                            c.Asset   = cloth;
                                                            c.Section = pending.Section;
                                                        });
                                                        assigned = true;
                                                    }
                                                return true;
                                            }),
                             m_PendingCloth.end());
        // The assignment is one undoable step.
        if (assigned && !m_Play.IsActive())
            m_History.Record(*m_ActiveScene);
    }

    void EditorLayer::BeginClothPaint(Entity entity)
    {
        if (!m_ActiveScene || !entity || !entity.HasComponent<ClothComponent>() || m_Play.IsActive())
            return;
        const auto& cloth = entity.GetComponent<ClothComponent>();
        const auto  file  = SourceFileOf(cloth.Asset);
        if (file.empty())
        {
            Record(AssetError {AssetErrorCode::NotFound, "The cloth asset has no source file to paint"});
            return;
        }
        auto loaded = Edit::LoadClothFile(file);
        if (const auto* error = std::get_if<AssetError>(&loaded))
        {
            Record(*error);
            return;
        }
        FinishClothPaint();
        m_ViewportPanel.GetClothPaint().Begin(entity, cloth.Asset, std::get<ClothData>(std::move(loaded)), file);
        m_HierarchyPanel.SetSelected(entity);
    }

    void EditorLayer::FinishClothPaint()
    {
        auto& paint = m_ViewportPanel.GetClothPaint();
        if (!paint.IsActive())
            return;
        if (paint.Session().IsDirty())
            paint.Save();
        paint.Cancel(m_ActiveScene.get());
    }

    void EditorLayer::OpenClothFile(const std::filesystem::path& file)
    {
        std::error_code error;
        const Entity    selected = m_HierarchyPanel.GetSelected();
        if (selected && selected.HasComponent<ClothComponent>())
        {
            const auto source = SourceFileOf(selected.GetComponent<ClothComponent>().Asset);
            if (!source.empty() && std::filesystem::equivalent(source, file, error))
            {
                BeginClothPaint(selected);
                return;
            }
        }
        m_ClothInfo = file;
        m_ClothInfoData.reset();
        m_ClothInfoError.clear();
        auto loaded = Edit::LoadClothFile(file);
        if (const auto* failed = std::get_if<AssetError>(&loaded))
            m_ClothInfoError = failed->Message;
        else
            m_ClothInfoData = std::get<ClothData>(std::move(loaded));
    }

    void EditorLayer::DrawClothInfo()
    {
        if (!m_ClothInfo)
            return;
        bool open = true;
        ImGui::SetNextWindowSize({360.0f, 0.0f}, ImGuiCond_Appearing);
        if (ImGui::Begin(ICON_FA_SHIRT "  Cloth Asset###ClothInfo", &open, ImGuiWindowFlags_NoDocking))
        {
            ImGui::TextUnformatted(m_ClothInfo->filename().u8string().c_str());
            if (!m_ClothInfoError.empty())
                ImGui::TextColored(ComponentEditors::ErrorColor, "%s", m_ClothInfoError.c_str());
            else if (m_ClothInfoData)
            {
                const auto& d    = *m_ClothInfoData;
                const auto  mesh = SourceFileOf(d.Mesh);
                size_t      pins = 0;
                for (float value : d.MaxDistance)
                    pins += value == 0.0f;
                ImGui::Text("Mesh: %s", mesh.empty() ? d.Mesh.ToString().c_str() : mesh.filename().u8string().c_str());
                ImGui::Text("Section: %u", d.Section);
                ImGui::Text("Particles: %zu (%zu pinned)", d.RestPositions.size(), pins);
                ImGui::Text("Triangles: %zu   Tethers: %zu", d.Triangles.size() / 3, d.Tethers.size());
                ImGui::TextDisabled("Select an entity whose Cloth uses this asset to paint it.");
            }
        }
        ImGui::End();
        if (!open)
        {
            m_ClothInfo.reset();
            m_ClothInfoData.reset();
        }
    }
} // namespace Kans

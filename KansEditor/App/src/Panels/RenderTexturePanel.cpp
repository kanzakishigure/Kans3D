#include "RenderTexturePanel.h"
#include <imgui.h>
#include <fstream>
#include <sstream>
#include "Kans3D/Core/Application.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Renderer/Renderer.h"
#include "Kans3D/Scene/Scene.h"
#include "KansEditorCore/EditorAssetImportService.h"
#include "KansEditorCore/UI/SourceBuild.h"
#include "KansEditorCore/Video/VideoEditing.h"
#include "UI/PropertyGrid.h"
namespace Kans
{
    bool RenderTexturePanel::Open(const std::filesystem::path& file)
    {
        std::ifstream input(file, std::ios::binary);
        if (!input)
        {
            m_Status = "Cannot open render texture";
            return false;
        }
        std::ostringstream text;
        text << input.rdbuf();
        auto loaded = ParseRenderTexture(text.str());
        if (auto* e = std::get_if<AssetError>(&loaded))
        {
            m_Status = e->Message;
            return false;
        }
        m_File   = file;
        m_Values = m_Saved = std::get<RenderTextureSpecification>(loaded);
        m_Dirty            = false;
        m_Status.clear();
        return true;
    }
    void RenderTexturePanel::Save()
    {
        if (m_Service && m_Service->IsReadOnly(m_File))
        {
            m_Status = "Plugin content is read-only.";
            return;
        }
        if (m_File.empty())
            return;
        auto text = SerializeRenderTexture(m_Values);
        if (auto* e = std::get_if<AssetError>(&text))
        {
            m_Status = e->Message;
            return;
        }
        std::ofstream output(m_File, std::ios::binary | std::ios::trunc);
        output << std::get<std::string>(text);
        output.close();
        if (!output)
        {
            m_Status = "Cannot save render texture";
            return;
        }
        m_Saved  = m_Values;
        m_Dirty  = false;
        m_Status = BuildSavedSource(m_Service, m_File);
    }
    AssetID RenderTexturePanel::TextureID() const
    {
        AssetID id;
        if (m_Service && m_Cache)
            if (auto sources = m_Service->GetSources();
                std::holds_alternative<std::vector<SourceAssetMetadata>>(sources))
                for (const auto& source : std::get<std::vector<SourceAssetMetadata>>(sources))
                    if (source.Path.GetPath() == m_File.lexically_relative(m_Service->GetConfig().RootDirectory /
                                                                           m_Service->GetConfig().AssetDirectory))
                        for (const auto& p : m_Cache->GetAssetManager().GetRegistry()->GetAllProducts())
                            if (p.Metadata.Type == AssetType::Texture &&
                                p.Metadata.assetID.GetSourceID() == source.sourceAssetID)
                                id = p.Metadata.assetID;
        return id;
    }

    void RenderTexturePanel::Draw(const Scene* scene)
    {
        m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (m_File.empty())
        {
            ImGui::TextDisabled("Open a .rendertexture file from the Content Browser.");
            return;
        }
        ImGui::TextUnformatted((m_File.filename().u8string() + (m_Dirty ? " *" : "")).c_str());
        ImGui::BeginDisabled(!m_Dirty);
        if (ImGui::Button("Save"))
            Save();
        ImGui::SameLine();
        if (ImGui::Button("Revert"))
        {
            m_Values = m_Saved;
            m_Dirty  = false;
        }
        ImGui::EndDisabled();
        if (!m_Status.empty())
            ImGui::TextDisabled("%s", m_Status.c_str());
        const bool video = m_Values.Update == RenderTextureUpdate::Video;
        if (PropertyGrid::BeginGroup("Texture"))
        {
            // A Video target's image comes from a Video Player, not from a material.
            if (video)
                PropertyGrid::Text("Material", "Not used: a Video Player writes the frames", true);
            else if (PropertyGrid::Row("Material"))
            {
                const auto registry = m_Service ? m_Service->GetRuntimeAssetRegistry() : nullptr;
                const auto label    = m_Values.Material.IsValid() ? m_Values.Material.ToString() : std::string("None");
                if (ImGui::BeginCombo("##material", label.c_str()))
                {
                    if (ImGui::Selectable("None", !m_Values.Material.IsValid()))
                    {
                        m_Values.Material = {};
                        m_Dirty           = true;
                    }
                    if (registry)
                        for (const auto& p : registry->GetAllProducts())
                            if (p.Metadata.Type == AssetType::Material)
                            {
                                std::string name = p.Metadata.assetID.ToString();
                                if (auto sources = m_Service->GetSources();
                                    std::holds_alternative<std::vector<SourceAssetMetadata>>(sources))
                                    for (const auto& source : std::get<std::vector<SourceAssetMetadata>>(sources))
                                        if (source.sourceAssetID == p.Metadata.assetID.GetSourceID())
                                            name = source.Path.ToString();
                                ImGui::PushID(p.Metadata.assetID.ToString().c_str());
                                if (ImGui::Selectable(name.c_str(), p.Metadata.assetID == m_Values.Material))
                                {
                                    m_Values.Material = p.Metadata.assetID;
                                    m_Dirty           = true;
                                }
                                ImGui::PopID();
                            }
                    ImGui::EndCombo();
                }
            }
            int         w = int(m_Values.Width), h = int(m_Values.Height), format = int(m_Values.Format);
            const char* formats[] = {"RGBA8 sRGB", "RGBA16F"};
            m_Dirty |= PropertyGrid::Int("Width", w, 1, 16384);
            m_Dirty |= PropertyGrid::Int("Height", h, 1, 16384);
            m_Values.Width  = uint32_t(w);
            m_Values.Height = uint32_t(h);
            m_Dirty |= PropertyGrid::Combo("Format", formats, 2, format);
            m_Values.Format = RenderTextureFormat(format);
            m_Dirty |= PropertyGrid::Checkbox("Mips", m_Values.Mips);
            glm::vec4 clear(
                m_Values.ClearColor[0], m_Values.ClearColor[1], m_Values.ClearColor[2], m_Values.ClearColor[3]);
            if (PropertyGrid::Color4("Clear color", clear))
            {
                for (int i = 0; i < 4; ++i)
                    m_Values.ClearColor[i] = clear[i];
                m_Dirty = true;
            }
            PropertyGrid::EndGroup();
        }
        if (PropertyGrid::BeginGroup("Update"))
        {
            int         mode    = int(m_Values.Update);
            const char* modes[] = {"On Load", "Realtime", "On Demand", "Video"};
            m_Dirty |= PropertyGrid::Combo("Mode", modes, 4, mode);
            m_Values.Update = RenderTextureUpdate(mode);
            if (m_Values.Update == RenderTextureUpdate::Realtime)
            {
                float rate = float(m_Values.Rate);
                m_Dirty |= PropertyGrid::Float("Rate (Hz)", rate, 1, 0, 1000, "%.2f");
                m_Values.Rate = rate;
            }
            else if (m_Values.Update == RenderTextureUpdate::Video)
            {
                // Which players of the open scene write into it (the first one to render owns it).
                const auto players =
                    scene ? VideoEditing::PlayersTargeting(*scene, TextureID()) : std::vector<std::string> {};
                std::string names;
                for (const auto& name : players)
                    names += (names.empty() ? "" : ", ") + name;
                PropertyGrid::Text("Driven by",
                                   players.empty() ? std::string("No Video Player in the scene targets it") :
                                                     "Video Player: " + names,
                                   players.empty());
            }
            PropertyGrid::EndGroup();
        }
        if (PropertyGrid::BeginGroup("Live preview"))
        {
            PropertyGrid::BeginFree();
            if (m_Cache)
            {
                const AssetID id      = TextureID();
                const auto    texture = id.IsValid() ? m_Cache->GetTexture(id) : nullptr;
                if (texture)
                {
                    if (video)
                        ImGui::TextDisabled("Shows the frame of the player that owns it.");
                    else if (ImGui::Button("Request update"))
                        if (const auto cpu = m_Cache->GetAssetManager().GetLoaded<TextureAsset>(id))
                            cpu->RequestUpdate();
                    const auto* app  = Application::TryGet();
                    const float time = app ? app->GetTime() : float(ImGui::GetTime());
                    m_Cache->UpdateRenderTextures(*Renderer::Acquire(m_Cache->GetDevice()),
                                                  app ? app->GetFrameNumber() : uint64_t(ImGui::GetFrameCount()),
                                                  time,
                                                  scene ? scene->CaptureMaterialParameters() : nullptr);
                    const float width = ImGui::GetContentRegionAvail().x;
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Colors().Inset);
                    ImGui::BeginChild("Preview", ImVec2(width, width * texture->GetHeight() / texture->GetWidth()));
                    ImGui::Image(ImTextureID(texture->GetImGuiTextureID()), ImGui::GetContentRegionAvail());
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                }
                else
                    ImGui::TextDisabled("Save to preview the texture.");
            }
            PropertyGrid::EndFree();
            PropertyGrid::EndGroup();
        }
        if (m_Dirty && m_Focused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
            Save();
    }
} // namespace Kans

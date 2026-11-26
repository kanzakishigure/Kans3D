#include "AudioMixerPanel.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include "Kans3D/Asset/AssetRegistry.h"
#include "Kans3D/Audio/AudioBusEffectFields.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Project/ProjectFile.h"
#include "KansEditorCore/EditorAssetImportService.h"
#include "KansEditorCore/UI/SourceBuild.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans
{
    using namespace AudioMixerEditing;
    using EditorTheme::Px;

    namespace
    {
        constexpr float NegativeInfinity = -std::numeric_limits<float>::infinity();
        constexpr char  EffectPayload[]  = "KANS_MIXER_EFFECT";

        ImVec4 WithAlpha(ImVec4 color, float alpha) { return {color.x, color.y, color.z, alpha}; }
        ImVec4 Mix(ImVec4 a, ImVec4 b, float t)
        {
            return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
        }

        ImRect LastItem() { return {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()}; }

        std::filesystem::path Canonical(const std::filesystem::path& path)
        {
            std::error_code error;
            auto            result = std::filesystem::weakly_canonical(path, error);
            return error ? path : result;
        }

        // "CutoffHz" -> "Cutoff", "RoomSize" -> "Room size": the field name without its unit, spaced.
        void FieldLabel(const char* name, char* out, size_t size)
        {
            size_t length = std::strlen(name);
            for (const char* unit : {"Hz", "Db", "Ms"})
            {
                const size_t n = std::strlen(unit);
                if (length > n && std::strcmp(name + length - n, unit) == 0)
                    length -= n;
            }
            size_t o = 0;
            for (size_t i = 0; i < length && o + 2 < size; ++i)
            {
                const char c = name[i];
                if (i > 0 && c >= 'A' && c <= 'Z')
                {
                    out[o++] = ' ';
                    out[o++] = char(c - 'A' + 'a');
                }
                else
                    out[o++] = c;
            }
            out[o] = '\0';
        }

        const char* FieldFormat(const char* name)
        {
            const size_t length = std::strlen(name);
            const auto   ends   = [&](const char* unit) {
                const size_t n = std::strlen(unit);
                return length > n && std::strcmp(name + length - n, unit) == 0;
            };
            return ends("Hz") ? "%.0f Hz" : ends("Db") ? "%.1f dB" : ends("Ms") ? "%.1f ms" : "%.2f";
        }

        bool IsDynamics(const AudioBusEffectParameters& params)
        {
            return std::holds_alternative<AudioCompressor>(params) || std::holds_alternative<AudioLimiter>(params) ||
                   std::holds_alternative<AudioDucking>(params);
        }

        // A toggle button in the strip's S/M/B row, filled with `on` while active.
        bool ToggleButton(const char* label, const char* tooltip, bool active, ImVec4 on, float width)
        {
            const auto& p = EditorTheme::Colors();
            ImGui::PushStyleColor(ImGuiCol_Button, active ? on : p.Inset);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? Mix(on, p.Text, 0.15f) : p.Hover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, active ? Mix(on, p.Text, 0.25f) : p.Pressed);
            ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(0.06f, 0.06f, 0.07f, 1.0f) : p.TextDim);
            const bool clicked = ImGui::Button(label, {width, 0});
            ImGui::PopStyleColor(4);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tooltip);
            return clicked;
        }
    } // namespace

    // ------------------------------------------------------------------ binding and files

    void
    AudioMixerPanel::Bind(AudioEngine* engine, EditorAssetImportService* service, std::filesystem::path projectFile)
    {
        m_Engine      = engine;
        m_Service     = service;
        m_ProjectFile = std::move(projectFile);
        m_Document.Bind(engine);
        m_Output.Bind(engine);
        m_Opened = false; // Opens the project's layout on the next draw.
        m_File.clear();
        m_PendingProjectLayout = false;
    }

    bool AudioMixerPanel::Open(const std::filesystem::path& file)
    {
        std::ifstream stream(file, std::ios::binary);
        if (!stream)
        {
            m_Status = "Cannot read " + file.filename().u8string();
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        auto              parsed = ParseAudioBusLayout(text);
        if (const auto* error = std::get_if<AssetError>(&parsed))
        {
            m_Status = file.filename().u8string() + ": " + error->Message;
            return false;
        }
        if (m_Document.IsDirty() && !m_File.empty() && Canonical(m_File) != Canonical(file))
            Save(); // Switching files keeps the edits instead of dropping them.
        m_Opened = true;
        m_File   = file;
        std::error_code ec;
        m_FileTime = std::filesystem::last_write_time(file, ec);
        m_Pending.reset();
        m_Document.Reset(std::get<AudioBusLayout>(std::move(parsed)));
        m_Error.clear();
        m_Status.clear();
        m_PendingProjectLayout = false;
        return true;
    }

    void AudioMixerPanel::OpenUnsaved(AudioBusLayout layout)
    {
        m_Opened = true;
        m_File.clear();
        m_Pending.reset();
        m_Document.Reset(std::move(layout), false);
        m_Error.clear();
        m_PendingProjectLayout = false;
    }

    std::optional<std::filesystem::path> AudioMixerPanel::CreateLayout(const std::filesystem::path& directory)
    {
        auto file = directory / "AudioBusLayout.audiobus";
        for (int i = 2; std::filesystem::exists(file); ++i)
            file = directory / ("AudioBusLayout" + std::to_string(i) + ".audiobus");
        const auto    text = SerializeAudioBusLayout(AudioBusLayout::Default());
        std::ofstream stream(file, std::ios::binary);
        if (!std::holds_alternative<std::string>(text) || !(stream << std::get<std::string>(text)) || !stream.flush())
        {
            m_Status = "Cannot create " + file.u8string();
            return std::nullopt;
        }
        stream.close();
        if (!Open(file))
            return std::nullopt;
        m_Status = BuildSavedSource(m_Service, file);
        return file;
    }

    bool AudioMixerPanel::Save()
    {
        if (m_File.empty())
        {
            m_Status = "Use New layout to save these buses to a file.";
            return false;
        }
        if (m_Service && m_Service->IsReadOnly(m_File))
        {
            m_Status = "Plugin content is read-only.";
            return false;
        }
        auto text = SerializeAudioBusLayout(m_Document.Get());
        if (const auto* error = std::get_if<AssetError>(&text))
        {
            m_Status = error->Message;
            return false;
        }
        std::ofstream stream(m_File, std::ios::binary | std::ios::trunc);
        if (!(stream << std::get<std::string>(text)) || !stream.flush())
        {
            m_Status = "Cannot write " + m_File.u8string();
            return false;
        }
        stream.close();
        std::error_code ec;
        m_FileTime = std::filesystem::last_write_time(m_File, ec);
        m_Document.MarkSaved();
        m_Status = BuildSavedSource(m_Service, m_File);
        return true;
    }

    std::vector<std::filesystem::path> AudioMixerPanel::ProjectLayouts() const
    {
        std::vector<std::filesystem::path> files;
        if (!m_Service)
            return files;
        auto sources = m_Service->GetSources();
        if (const auto* list = std::get_if<std::vector<SourceAssetMetadata>>(&sources))
            for (const auto& source : *list)
                if (source.Exists && source.Path.GetPath().extension() == ".audiobus")
                    files.push_back(m_Service->ResolveSourcePath(source.Path));
        std::sort(files.begin(), files.end());
        return files;
    }

    AssetID AudioMixerPanel::PublishedLayout(const std::filesystem::path& file) const
    {
        if (!m_Service || file.empty())
            return {};
        auto sources = m_Service->GetSources();
        if (const auto* list = std::get_if<std::vector<SourceAssetMetadata>>(&sources))
        {
            const auto target = Canonical(file);
            for (const auto& source : *list)
                if (source.Exists && Canonical(m_Service->ResolveSourcePath(source.Path)) == target)
                    if (const auto registry = m_Service->GetRuntimeAssetRegistry())
                        for (const auto& record : registry->GetAllProducts())
                            if (record.Metadata.assetID.GetSourceID() == source.sourceAssetID &&
                                record.Metadata.Type == AssetType::AudioBusLayout && record.Target == EditorAssetTarget)
                                return record.Metadata.assetID;
        }
        return {};
    }

    std::optional<std::string> AudioMixerPanel::SetAsProjectLayout()
    {
        if (m_File.empty())
            return "Save the layout to a file first (New layout).";
        if (m_ProjectFile.empty())
            return "No project is open.";
        if (m_Document.IsDirty() && !Save())
            return m_Status;
        const AssetID layout = PublishedLayout(m_File);
        if (!layout.IsValid())
        {
            // Still importing: finished once the product appears (checked while drawing).
            m_PendingProjectLayout = true;
            return std::nullopt;
        }
        auto loaded = LoadProjectFile(m_ProjectFile);
        if (const auto* error = std::get_if<AssetError>(&loaded))
            return error->Message;
        auto project           = std::get<ProjectFile>(std::move(loaded));
        project.AudioBusLayout = layout;
        if (auto error = SaveProjectFile(m_ProjectFile, project))
            return error->Message;
        m_PendingProjectLayout = false;
        m_ProjectLayout        = layout;
        m_Status               = m_File.filename().u8string() + " is the project's layout.";
        if (m_OnProjectLayout)
            m_OnProjectLayout(layout);
        return std::nullopt;
    }

    void AudioMixerPanel::EnsureDocument()
    {
        if (m_Opened)
            return;
        m_Opened = true;
        if (m_ProjectLayout.IsValid() && m_Service)
        {
            auto sources = m_Service->GetSources();
            if (const auto* list = std::get_if<std::vector<SourceAssetMetadata>>(&sources))
                for (const auto& source : *list)
                    if (source.Exists && source.sourceAssetID == m_ProjectLayout.GetSourceID() &&
                        Open(m_Service->ResolveSourcePath(source.Path)))
                        return;
        }
        // No layout file yet: the buses the engine plays (the default layout).
        OpenUnsaved(m_Engine && !m_Engine->GetBuses().empty() ? AudioBusLayout {m_Engine->GetBuses()} :
                                                                AudioBusLayout::Default());
    }

    void AudioMixerPanel::ReloadIfChangedOnDisk()
    {
        m_DiskCheck += ImGui::GetIO().DeltaTime;
        if (m_DiskCheck < 1.0f || m_File.empty() || m_Document.IsDirty())
            return;
        m_DiskCheck = 0.0f;
        std::error_code ec;
        const auto      time = std::filesystem::last_write_time(m_File, ec);
        if (!ec && time != m_FileTime)
        {
            const auto file = m_File;
            Open(file); // Edited outside the editor: play and show the new text.
        }
    }

    // ------------------------------------------------------------------ edits

    void AudioMixerPanel::Edit(const std::function<std::optional<std::string>(AudioBusLayout&)>& change, size_t bus)
    {
        // Continuous while a drag (or a field) is held: one undo step for the whole gesture.
        auto next = m_Pending ? *m_Pending : m_Document.Get();
        if (auto error = change(next))
        {
            m_Error    = *error;
            m_ErrorBus = bus;
            return;
        }
        m_PendingContinuous = (m_Pending ? m_PendingContinuous : true) && ImGui::IsAnyItemActive();
        m_PendingBus        = bus;
        m_Pending           = std::move(next);
    }

    void AudioMixerPanel::CommitPending()
    {
        if (!m_Pending)
            return;
        auto layout = std::move(*m_Pending);
        m_Pending.reset();
        if (auto error = m_Document.Commit(std::move(layout), m_PendingContinuous))
        {
            m_Error    = *error;
            m_ErrorBus = m_PendingBus;
        }
        else if (m_PendingBus == m_ErrorBus || m_PendingBus == SIZE_MAX)
            m_Error.clear();
    }

    // ------------------------------------------------------------------ drawing

    void AudioMixerPanel::Draw()
    {
        m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        m_MenuBoxes.clear();
        EnsureDocument();
        ReloadIfChangedOnDisk();
        if (m_PendingProjectLayout && PublishedLayout(m_File).IsValid())
            if (auto error = SetAsProjectLayout())
                m_Status = *error;
        const auto& io = ImGui::GetIO();
        if (m_Focused && io.KeyCtrl && !io.WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, true))
                io.KeyShift ? Redo() : Undo();
            else if (ImGui::IsKeyPressed(ImGuiKey_Y, true))
                Redo();
            else if (ImGui::IsKeyPressed(ImGuiKey_S, false) && m_Document.IsDirty())
                Save();
        }
        DrawHeader();

        // Rebuilt when the layout changes; the strips reuse their storage frame to frame.
        const auto& layout = m_Document.Get();
        if (m_OrderRevision != m_Document.GetRevision())
        {
            m_Order         = TreeOrder(layout);
            m_OrderRevision = m_Document.GetRevision();
            m_Meters.resize(layout.Buses.size());
            m_Boxes.resize(m_Order.size());
        }
        // The Output pane takes the right side; the strips scroll in the rest.
        const float outputWidth = m_ShowOutput ? Px(380.0f) : 0.0f;
        ImGui::BeginChild("##strips",
                          {m_ShowOutput ? -(outputWidth + ImGui::GetStyle().ItemSpacing.x) : 0.0f, 0},
                          ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        // Every strip reads the layout as it was at the start of the frame; edits commit after them.
        const size_t count = m_Order.size();
        for (size_t i = 0; i < count && i < m_Boxes.size(); ++i)
        {
            if (i > 0)
                ImGui::SameLine();
            if (m_Order[i].Index < layout.Buses.size())
                DrawStrip(m_Order[i].Index, m_Order[i].Depth, m_Boxes[i]);
        }
        ImGui::EndChild();
        if (m_ShowOutput)
        {
            ImGui::SameLine();
            ImGui::BeginChild("##output", {outputWidth, 0}, ImGuiChildFlags_Borders);
            m_Output.Draw();
            ImGui::EndChild();
        }
        CommitPending();
        if (!ImGui::IsAnyItemActive())
            m_Document.EndGesture();
    }

    void AudioMixerPanel::DrawHeader()
    {
        const auto& p = EditorTheme::Colors();
        const bool  isProject =
            !m_File.empty() && m_ProjectLayout.IsValid() && m_ProjectLayout == PublishedLayoutCached();
        // Without a file the buses live only in the engine until New layout writes them.
        std::string preview = m_File.empty() ? std::string("Unsaved layout") : m_File.filename().u8string();
        if (m_Document.IsDirty() && !m_File.empty())
            preview += " *";
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(p.TypeAudio, ICON_FA_SLIDERS);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Px(220.0f));
        if (ImGui::BeginCombo("##layout", preview.c_str()))
        {
            if (ImGui::IsWindowAppearing())
                m_LayoutChoices = ProjectLayouts();
            if (m_LayoutChoices.empty())
                ImGui::TextDisabled("No .audiobus files in the project");
            for (const auto& file : m_LayoutChoices)
                if (ImGui::Selectable(file.filename().u8string().c_str(), Canonical(file) == Canonical(m_File)))
                    Open(file);
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered() && !m_File.empty())
            ImGui::SetTooltip("%s%s", m_File.u8string().c_str(), isProject ? "\nThe project's layout" : "");
        if (isProject)
        {
            ImGui::SameLine();
            ImGui::TextColored(p.Warning, ICON_FA_STAR);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The project plays this layout");
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLUS " New layout") && m_Service)
            CreateLayout(m_Service->GetConfig().RootDirectory / m_Service->GetConfig().AssetDirectory);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A new .audiobus with the default buses, in the asset folder");
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_Document.IsDirty() || m_File.empty());
        if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save"))
            Save();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(m_File.empty() || isProject || m_PendingProjectLayout);
        if (ImGui::Button(ICON_FA_STAR " Set as project layout"))
            if (auto error = SetAsProjectLayout())
                m_Status = *error;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(m_PendingProjectLayout ? "Waiting for the import to finish" :
                                                       "Writes this layout into the project file");
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_Document.CanUndo());
        if (ImGui::Button(ICON_FA_ROTATE_LEFT "##undo"))
            Undo();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Undo mixer edit (Ctrl+Z)");
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_Document.CanRedo());
        if (ImGui::Button(ICON_FA_ROTATE_RIGHT "##redo"))
            Redo();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Redo mixer edit (Ctrl+Y)");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLUS " Bus"))
            Edit([](AudioBusLayout& layout) {
                AddBus(layout);
                return std::optional<std::string>();
            });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Adds a bus under Master");
        ImGui::SameLine();
        if (ToggleButton(ICON_FA_HEADPHONES " Output##output",
                         "Output device, speakers and the player's volume (per-user settings, not the project)",
                         m_ShowOutput,
                         p.AccentHover,
                         0.0f))
            m_ShowOutput = !m_ShowOutput;
        m_OutputToggle     = LastItem();
        const char* status = !m_Status.empty()      ? m_Status.c_str() :
                             m_PendingProjectLayout ? "Importing; it becomes the project's layout once imported." :
                             !m_Engine              ? "Audio is unavailable: edits are not heard." :
                                                      nullptr;
        if (status)
        {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", status);
        }
        if (!m_Error.empty() && m_ErrorBus == SIZE_MAX)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, p.Danger);
            ImGui::TextUnformatted(m_Error.c_str());
            ImGui::PopStyleColor();
        }
    }

    AssetID AudioMixerPanel::PublishedLayoutCached()
    {
        // The project's layout star: looked up again only when the file or the registry changes.
        const auto registry = m_Service ? m_Service->GetRuntimeAssetRegistry() : nullptr;
        if (m_PublishedFor != m_File || m_PublishedRegistry != registry.get())
        {
            m_PublishedFor      = m_File;
            m_PublishedRegistry = registry.get();
            m_Published         = PublishedLayout(m_File);
        }
        return m_Published;
    }

    void AudioMixerPanel::DrawStrip(size_t index, int depth, StripBoxes& boxes)
    {
        const auto& p      = EditorTheme::Colors();
        const auto& layout = m_Document.Get();
        const auto& bus    = layout.Buses[index];
        const bool  master = bus.Name == MasterAudioBus;
        if (boxes.Bus != bus.Name)
            boxes.Bus = bus.Name;
        ImGui::PushID(int(index));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, p.Raised);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(4.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {Px(8.0f), Px(8.0f)});
        ImGui::BeginChild("##strip", {Px(280.0f), 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        boxes.Strip       = ImGui::GetCurrentWindow()->Rect();
        auto*       draw  = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;

        // Depth cue: Master in the accent, buses below it in the audio colour, deeper ones dimmer,
        // with one notch per level on the left of the band.
        {
            const ImVec2 min = boxes.Strip.Min;
            const ImVec4 tone =
                master ? p.Accent : Mix(p.TypeAudio, p.Raised, std::min(0.6f, 0.25f * float(std::max(0, depth - 1))));
            draw->AddRectFilled(min,
                                {boxes.Strip.Max.x, min.y + Px(4.0f)},
                                ImGui::GetColorU32(tone),
                                Px(4.0f),
                                ImDrawFlags_RoundCornersTop);
            for (int d = 0; d < depth; ++d)
                draw->AddRectFilled({min.x + Px(8.0f) + Px(6.0f) * float(d), min.y + Px(6.0f)},
                                    {min.x + Px(11.0f) + Px(6.0f) * float(d), min.y + Px(9.0f)},
                                    ImGui::GetColorU32(tone));
        }
        ImGui::Dummy({0, Px(4.0f)});

        // Name (rename inline) and delete.
        {
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer), "%s", m_Renaming == index ? m_RenameText.c_str() : bus.Name.c_str());
            const float button = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(width - button - ImGui::GetStyle().ItemSpacing.x);
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.05f);
            if (ImGui::InputText("##name", buffer, sizeof(buffer), master ? ImGuiInputTextFlags_ReadOnly : 0))
            {
                m_Renaming   = index;
                m_RenameText = buffer;
                auto probe   = layout;
                if (auto error = RenameBus(probe, index, m_RenameText))
                    m_Error = *error, m_ErrorBus = index;
                else if (m_ErrorBus == index)
                    m_Error.clear();
            }
            ImGui::PopFont();
            boxes.Name = LastItem();
            if (ImGui::IsItemHovered() && master)
                ImGui::SetTooltip("Master is the root of the layout and keeps its name");
            if (ImGui::IsItemDeactivated() && m_Renaming == index)
            {
                const std::string name = m_RenameText;
                m_Renaming             = SIZE_MAX;
                Edit([&](AudioBusLayout& next) { return RenameBus(next, index, name); }, index);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(master);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            const bool remove = ImGui::Button(ICON_FA_TRASH_CAN "##delete", {button, button});
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
            boxes.Delete = LastItem();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(master ? "Master cannot be deleted" :
                                           "Delete this bus (its children move to its parent)");
            if (remove)
                Edit([&](AudioBusLayout& next) { return RemoveBus(next, index); }, index);
        }
        if (!m_Error.empty() && m_ErrorBus == index)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, p.Danger);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(m_Error.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        // Parent.
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Parent");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::BeginDisabled(master);
            if (ImGui::BeginCombo("##parent", master ? "None (output)" : bus.Parent.c_str()))
            {
                const auto below = Subtree(layout, index);
                for (const auto& entry : TreeOrder(layout))
                {
                    if (std::find(below.begin(), below.end(), entry.Index) != below.end())
                        continue;
                    const auto& name = layout.Buses[entry.Index].Name;
                    ImGui::Indent(Px(10.0f) * float(entry.Depth) + 1.0f);
                    if (ImGui::Selectable(name.c_str(), name == bus.Parent))
                        Edit([&, parent = name](AudioBusLayout& next) { return SetParent(next, index, parent); },
                             index);
                    ImGui::Unindent(Px(10.0f) * float(entry.Depth) + 1.0f);
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            boxes.Parent = LastItem();
        }

        // Solo / Mute / Bypass.
        {
            const float third = (width - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
            if (ToggleButton(
                    "S##solo", "Solo: hear only soloed buses (and what feeds them)", bus.Solo, p.Warning, third))
                Edit([&, solo = !bus.Solo](AudioBusLayout& next) {
                    next.Buses[index].Solo = solo;
                    return std::optional<std::string>();
                });
            boxes.Solo = LastItem();
            ImGui::SameLine();
            if (ToggleButton("M##mute", "Mute", bus.Mute, p.Danger, third))
                Edit([&, mute = !bus.Mute](AudioBusLayout& next) {
                    next.Buses[index].Mute = mute;
                    return std::optional<std::string>();
                });
            boxes.Mute = LastItem();
            ImGui::SameLine();
            if (ToggleButton("B##bypass", "Bypass the effect chain", bus.Bypass, p.Accent, third))
                Edit([&, bypass = !bus.Bypass](AudioBusLayout& next) {
                    next.Buses[index].Bypass = bypass;
                    return std::optional<std::string>();
                });
            boxes.Bypass = LastItem();
        }
        // Centre pin: everything through this bus plays from the centre speaker (dialogue, UI voices).
        {
            bool pin = bus.PinToCenter;
            if (ImGui::Checkbox("Pin to center##pin", &pin))
                Edit([&, pin](AudioBusLayout& next) {
                    next.Buses[index].PinToCenter = pin;
                    return std::optional<std::string>();
                });
            boxes.PinToCenter = LastItem();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Play this bus from the centre speaker (in stereo, half to each side), e.g. "
                                  "dialogue");
        }
        // Game time: whether the bus bends with the time scale and holds while the game is paused.
        {
            bool follow = bus.FollowTimeScale;
            if (ImGui::Checkbox("Follow time scale##timeScale", &follow))
                Edit([&, follow](AudioBusLayout& next) {
                    next.Buses[index].FollowTimeScale = follow;
                    return std::optional<std::string>();
                });
            boxes.FollowTimeScale = LastItem();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Slow motion lowers this bus's pitch with the scene's time scale (0.05x to 4x); "
                                  "a time scale of 0 holds it, e.g. gameplay effects");
            bool pauses = bus.PausesWithGame;
            if (ImGui::Checkbox("Pauses with game##pauses", &pauses))
                Edit([&, pauses](AudioBusLayout& next) {
                    next.Buses[index].PausesWithGame = pauses;
                    return std::optional<std::string>();
                });
            boxes.PausesWithGame = LastItem();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Held while the game is paused and resumed where it stopped. Turn off for UI and "
                                  "menu buses, which keep playing while the game is paused");
        }
        // In Play: the engine holding this bus's voices for the pause, or bending them with the time scale.
        boxes.PlayStatus = m_Engine ? BusPlayStatus(*m_Engine, bus.Name) : std::string();
        if (!boxes.PlayStatus.empty())
        {
            const bool paused = boxes.PlayStatus == "Paused";
            ImGui::TextColored(paused ? p.Warning : p.AccentHover,
                               "%s %s",
                               paused ? ICON_FA_PAUSE : ICON_FA_FORWARD_FAST,
                               boxes.PlayStatus.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(paused ? "The game is paused: this bus's voices are held" :
                                           "This bus follows the scene's time scale: its pitch bends with it");
        }
        else
            ImGui::Dummy({0, ImGui::GetTextLineHeight()}); // Keeps the fader in place when a note appears.

        DrawFader(index, boxes);
        // Sends and effects: groups in the strip's own grid (narrow label column).
        PropertyGrid::Begin("##strip", 78.0f);
        DrawSends(index, boxes);
        DrawEffects(index, boxes);
        PropertyGrid::End();
        ImGui::EndChild();
        ImGui::PopID();
    }

    void AudioMixerPanel::DrawFader(size_t index, StripBoxes& boxes)
    {
        const auto&  p      = EditorTheme::Colors();
        const auto&  bus    = m_Document.Get().Buses[index];
        auto*        draw   = ImGui::GetWindowDrawList();
        const float  height = Px(180.0f), scaleWidth = Px(28.0f), faderWidth = Px(24.0f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float  width  = ImGui::GetContentRegionAvail().x;
        const float  top = origin.y + Px(8.0f), bottom = top + height;
        // The fader and the meters share one scale (MeterPosition is the fader law).
        const auto faderY = [&](float position) { return bottom - position * height; };
        const auto meterY = faderY;

        // dB scale on the left: fader positions of the marked levels.
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.72f);
        for (float db : {6.0f, 0.0f, -6.0f, -12.0f, -24.0f, -40.0f, -60.0f})
        {
            char text[8];
            std::snprintf(text, sizeof(text), db > 0 ? "+%.0f" : "%.0f", db);
            const float  y    = faderY(DbToFader(db));
            const ImVec2 size = ImGui::CalcTextSize(text);
            draw->AddText({origin.x + scaleWidth - size.x - Px(4.0f), y - size.y * 0.5f},
                          ImGui::GetColorU32(db == 0.0f ? p.Text : p.TextDim),
                          text);
        }
        {
            const ImVec2 size = ImGui::CalcTextSize("-inf");
            draw->AddText({origin.x + scaleWidth - size.x - Px(4.0f), bottom - size.y * 0.5f},
                          ImGui::GetColorU32(p.TextDim),
                          "-inf");
        }
        ImGui::PopFont();

        // The fader: drag (relative, so a click does not jump), double-click for 0 dB.
        const float faderX = origin.x + scaleWidth;
        ImGui::SetCursorScreenPos({faderX, top - Px(6.0f)});
        ImGui::InvisibleButton("##fader", {faderWidth, height + Px(12.0f)});
        boxes.Fader            = LastItem();
        const float   position = DbToFader(bus.VolumeDb);
        auto*         storage  = ImGui::GetStateStorage();
        const ImGuiID startID = ImGui::GetID("##faderStart"), mouseID = ImGui::GetID("##faderMouse");
        if (ImGui::IsItemActivated())
        {
            storage->SetFloat(startID, position);
            storage->SetFloat(mouseID, ImGui::GetIO().MousePos.y);
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            Edit([&](AudioBusLayout& next) {
                next.Buses[index].VolumeDb = 0.0f;
                return std::optional<std::string>();
            });
        else if (ImGui::IsItemActive())
        {
            // Only a moved mouse changes the level (the law's round trip is not exact).
            const float moved = storage->GetFloat(mouseID) - ImGui::GetIO().MousePos.y;
            const float next  = std::clamp(storage->GetFloat(startID) + moved / height, 0.0f, 1.0f);
            const float db    = FaderToDb(next);
            if (moved != 0.0f && db != bus.VolumeDb)
                Edit([&](AudioBusLayout& layout) {
                    layout.Buses[index].VolumeDb = db;
                    return std::optional<std::string>();
                });
        }
        if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
            ImGui::SetTooltip("Drag to set the level; double-click for 0 dB");
        const bool  active = ImGui::IsItemActive();
        const float center = faderX + faderWidth * 0.5f;
        draw->AddRectFilled(
            {center - Px(2.0f), top}, {center + Px(2.0f), bottom}, ImGui::GetColorU32(p.Inset), Px(2.0f));
        draw->AddLine({faderX + Px(2.0f), faderY(DbToFader(0.0f))},
                      {faderX + faderWidth - Px(2.0f), faderY(DbToFader(0.0f))},
                      ImGui::GetColorU32(p.TextDim),
                      1.0f);
        {
            const float y = faderY(position);
            draw->AddRectFilled({faderX, y - Px(6.0f)},
                                {faderX + faderWidth, y + Px(6.0f)},
                                ImGui::GetColorU32(active ? p.AccentHover : p.Text),
                                Px(2.0f));
            draw->AddLine(
                {faderX + Px(3.0f), y}, {faderX + faderWidth - Px(3.0f), y}, ImGui::GetColorU32(p.Raised), 2.0f);
        }

        // Meters: one bar per output channel, peak-hold mark and a clip lamp above each.
        auto& meter = m_Meters[index];
        if (m_Engine && m_Engine->ReadBusMeter(bus.Name, m_MeterRead))
            meter.Update(m_MeterRead, ImGui::GetIO().DeltaTime);
        else
        {
            m_MeterRead.Channels.clear();
            meter.Update(m_MeterRead, ImGui::GetIO().DeltaTime);
        }
        const size_t channels = std::max<size_t>(1, meter.GetChannelCount());
        boxes.Meters          = meter.GetChannelCount();
        const float meterX    = faderX + faderWidth + Px(8.0f);
        const float meterW    = std::max(Px(8.0f), width - (meterX - origin.x));
        const float gap       = channels > 4 ? Px(2.0f) : Px(3.0f);
        const float barW      = std::min(Px(12.0f), (meterW - gap * float(channels - 1)) / float(channels));
        const float clipH     = Px(5.0f);
        const ImU32 green = ImGui::GetColorU32(p.Success), yellow = ImGui::GetColorU32(p.Warning),
                    red = ImGui::GetColorU32(p.Danger);
        for (size_t c = 0; c < channels; ++c)
        {
            const float x0 = meterX + float(c) * (barW + gap), x1 = x0 + barW;
            draw->AddRectFilled({x0, top}, {x1, bottom}, ImGui::GetColorU32(p.Inset), Px(1.0f));
            if (c < meter.GetChannelCount())
            {
                const auto& state = meter.GetChannel(c);
                const float level = MeterPosition(state.LevelDb);
                // Green to -12 dBFS, yellow to 0, red above.
                const float y12 = meterY(MeterPosition(-12.0f)), y0 = meterY(MeterPosition(0.0f)), yl = meterY(level);
                if (level > 0.0f)
                {
                    draw->AddRectFilled({x0, std::max(yl, y12)}, {x1, bottom}, green);
                    if (yl < y12)
                        draw->AddRectFilled({x0, std::max(yl, y0)}, {x1, y12}, yellow);
                    if (yl < y0)
                        draw->AddRectFilled({x0, yl}, {x1, y0}, red);
                }
                if (const float hold = MeterPosition(state.HoldDb); hold > 0.0f)
                    draw->AddLine({x0, meterY(hold)},
                                  {x1, meterY(hold)},
                                  state.HoldDb > 0.0f ? red : ImGui::GetColorU32(p.Text),
                                  Px(1.5f));
                draw->AddRectFilled({x0, top - clipH - Px(2.0f)},
                                    {x1, top - Px(2.0f)},
                                    state.Clip ? red : ImGui::GetColorU32(p.Inset),
                                    Px(1.0f));
            }
        }
        // The clip lamps: click to clear.
        ImGui::SetCursorScreenPos({meterX, top - clipH - Px(2.0f)});
        ImGui::InvisibleButton("##clip", {std::max(1.0f, meterW), clipH});
        if (ImGui::IsItemClicked())
            meter.ClearClip();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Clip: a block peaked above 0 dBFS. Click to clear.");
        ImGui::SetCursorScreenPos({meterX, top});
        ImGui::Dummy({std::max(1.0f, meterW), height});
        if (ImGui::IsItemHovered() && meter.GetChannelCount())
        {
            ImGui::BeginTooltip();
            for (size_t c = 0; c < meter.GetChannelCount(); ++c)
            {
                const float db = meter.GetChannel(c).LevelDb;
                if (db > -200.0f)
                    ImGui::Text("Channel %d: %.1f dBFS", int(c + 1), db);
                else
                    ImGui::Text("Channel %d: silent", int(c + 1));
            }
            ImGui::EndTooltip();
        }

        // The level as a number under the fader.
        ImGui::SetCursorScreenPos({origin.x, bottom + Px(10.0f)});
        const bool closed = bus.VolumeDb == NegativeInfinity;
        float      value  = closed ? FaderMinDb : bus.VolumeDb;
        ImGui::SetNextItemWidth(width);
        if (ImGui::DragFloat("##volume",
                             &value,
                             0.1f,
                             FaderMinDb,
                             FaderMaxDb,
                             closed ? "-inf dB" : "%.1f dB",
                             ImGuiSliderFlags_AlwaysClamp))
            Edit([&](AudioBusLayout& layout) {
                layout.Buses[index].VolumeDb = value;
                return std::optional<std::string>();
            });
        boxes.Volume = LastItem();
        if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
            ImGui::SetTooltip("Level in dB (-80 to +6; the fader's bottom is -inf)");
    }

    void AudioMixerPanel::DrawSends(size_t index, StripBoxes& boxes)
    {
        const auto& p      = EditorTheme::Colors();
        const auto& layout = m_Document.Get();
        const auto& bus    = layout.Buses[index];
        boxes.AddSend      = {};
        if (!PropertyGrid::BeginGroup("Sends"))
            return;
        for (size_t s = 0; s < bus.Sends.size(); ++s)
        {
            const auto& send = bus.Sends[s];
            ImGui::PushID(int(s));
            if (PropertyGrid::Row(send.Target.c_str(), "Send target"))
            {
                const float button = ImGui::GetFrameHeight();
                const float pre    = Px(34.0f);
                ImGui::SetNextItemWidth(
                    std::max(Px(30.0f),
                             ImGui::GetContentRegionAvail().x - pre - button - 2.0f * ImGui::GetStyle().ItemSpacing.x));
                const bool closed = send.LevelDb == NegativeInfinity;
                float      level  = closed ? FaderMinDb : send.LevelDb;
                if (ImGui::DragFloat("##level",
                                     &level,
                                     0.1f,
                                     FaderMinDb,
                                     FaderMaxDb,
                                     closed ? "-inf" : "%.1f dB",
                                     ImGuiSliderFlags_AlwaysClamp))
                    Edit([&](AudioBusLayout& next) {
                        next.Buses[index].Sends[s].LevelDb = level;
                        return std::optional<std::string>();
                    });
                ImGui::SameLine();
                const bool preFader = send.Tap == AudioSendTap::PreFader;
                if (ToggleButton(preFader ? "Pre" : "Post",
                                 preFader ? "Pre-fader: sent before this bus's fader (click for post)" :
                                            "Post-fader: follows this bus's fader (click for pre)",
                                 preFader,
                                 p.AccentHover,
                                 pre))
                    Edit([&](AudioBusLayout& next) {
                        next.Buses[index].Sends[s].Tap = preFader ? AudioSendTap::PostFader : AudioSendTap::PreFader;
                        return std::optional<std::string>();
                    });
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                if (ImGui::Button(ICON_FA_XMARK "##remove", {button, button}))
                    Edit([&](AudioBusLayout& next) {
                        auto& sends = next.Buses[index].Sends;
                        sends.erase(sends.begin() + std::ptrdiff_t(s));
                        return std::optional<std::string>();
                    });
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Remove this send");
            }
            ImGui::PopID();
        }
        PropertyGrid::BeginFree();
        if (ImGui::SmallButton(ICON_FA_PLUS " Send"))
            ImGui::OpenPopup("##addSend");
        boxes.AddSend = LastItem();
        if (ImGui::BeginPopup("##addSend"))
        {
            bool any = false;
            for (const auto& entry : TreeOrder(layout))
            {
                const auto& target = layout.Buses[entry.Index].Name;
                auto        probe  = layout;
                probe.Buses[index].Sends.push_back({target, 0.0f, AudioSendTap::PostFader});
                if (entry.Index == index || ValidateAudioBusLayout(probe))
                    continue; // Itself, or a target that would feed back.
                any = true;
                if (ImGui::MenuItem(target.c_str()))
                    Edit([&, target](AudioBusLayout& next) {
                        next.Buses[index].Sends.push_back({target, 0.0f, AudioSendTap::PostFader});
                        return std::optional<std::string>();
                    });
                m_MenuBoxes.emplace_back(target, LastItem());
            }
            if (!any)
                ImGui::TextDisabled("No bus can take a send without a loop");
            ImGui::EndPopup();
        }
        PropertyGrid::EndFree();
        PropertyGrid::EndGroup();
    }

    void AudioMixerPanel::DrawEffects(size_t index, StripBoxes& boxes)
    {
        const auto& p      = EditorTheme::Colors();
        const auto& layout = m_Document.Get();
        const auto& bus    = layout.Buses[index];
        boxes.AddEffect    = {};
        boxes.Effects.resize(bus.Effects.size());
        if (!PropertyGrid::BeginGroup("Effects"))
        {
            for (auto& effect : boxes.Effects)
                effect = {};
            return;
        }
        for (size_t e = 0; e < bus.Effects.size(); ++e)
        {
            const auto& effect = bus.Effects[e];
            auto&       box    = boxes.Effects[e];
            ImGui::PushID(int(e));
            char title[96];
            if (const auto* custom = std::get_if<AudioCustomDSP>(&effect.Parameters))
                std::snprintf(title, sizeof(title), "%s", custom->Name.c_str());
            else
                std::snprintf(
                    title, sizeof(title), "%s", std::string(AudioEffectTypes[effect.Parameters.index()]).c_str());
            bool       enabled = effect.Enabled, toggled = false;
            const bool dynamics = IsDynamics(effect.Parameters);
            PropertyGrid::SetNextGroupTrailer(Px(dynamics ? 74.0f : 44.0f));
            const bool open = PropertyGrid::BeginGroup(title, &enabled, &toggled, false);
            ImVec2     bmin, bmax;
            box = {};
            if (PropertyGrid::LastGroupHeading(bmin, bmax))
            {
                box.Heading       = {bmin, bmax};
                box.Enable        = {{bmin.x + Px(16.0f), bmin.y}, {bmin.x + Px(16.0f) + bmax.y - bmin.y, bmax.y}};
                auto*       draw  = ImGui::GetWindowDrawList();
                const float textY = bmin.y + (bmax.y - bmin.y - ImGui::GetFontSize()) * 0.5f;
                // Remove, at the right end.
                const ImRect  remove({bmax.x - Px(20.0f), bmin.y}, {bmax.x, bmax.y});
                const ImGuiID removeID = ImGui::GetID("##remove");
                if (ImGui::ItemAdd(remove, removeID))
                {
                    bool hovered = false, held = false;
                    if (ImGui::ButtonBehavior(remove, removeID, &hovered, &held))
                        Edit([&](AudioBusLayout& next) {
                            auto& effects = next.Buses[index].Effects;
                            effects.erase(effects.begin() + std::ptrdiff_t(e));
                            return std::optional<std::string>();
                        });
                    draw->AddText({remove.Min.x + Px(5.0f), textY},
                                  ImGui::GetColorU32(hovered ? p.Danger : p.TextDim),
                                  ICON_FA_XMARK);
                    if (hovered)
                        ImGui::SetTooltip("Remove this effect");
                }
                box.Remove = remove;
                // The grip: drag onto another effect of this bus to move it there.
                const ImRect  grip({bmax.x - Px(38.0f), bmin.y}, {bmax.x - Px(20.0f), bmax.y});
                const ImGuiID gripID = ImGui::GetID("##grip");
                if (ImGui::ItemAdd(grip, gripID))
                {
                    bool hovered = false, held = false;
                    ImGui::ButtonBehavior(grip, gripID, &hovered, &held);
                    draw->AddText({grip.Min.x + Px(6.0f), textY},
                                  ImGui::GetColorU32(hovered || held ? p.Text : p.TextDim),
                                  ICON_FA_ELLIPSIS_VERTICAL);
                    if (hovered && !held)
                        ImGui::SetTooltip("Drag onto another effect to reorder");
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                    {
                        const size_t payload[2] = {index, e};
                        ImGui::SetDragDropPayload(EffectPayload, payload, sizeof(payload));
                        ImGui::TextUnformatted(title);
                        ImGui::EndDragDropSource();
                    }
                }
                box.Grip = grip;
                if (ImGui::BeginDragDropTargetCustom(ImRect(bmin, bmax), ImGui::GetID("##drop")))
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(EffectPayload))
                    {
                        size_t dragged[2];
                        std::memcpy(dragged, payload->Data, sizeof(dragged));
                        if (dragged[0] == index)
                            Edit([&, from = dragged[1]](AudioBusLayout& next) {
                                return MoveEffect(next, index, from, e);
                            });
                    }
                    ImGui::EndDragDropTarget();
                }
                // Gain reduction of a dynamics effect: a bar filling from the right, 0..24 dB.
                if (dynamics)
                {
                    const float reduction =
                        m_Engine ? m_Engine->GetBusEffectGainReductionDb(bus.Name, e).value_or(0.0f) : 0.0f;
                    const ImRect bar({bmax.x - Px(70.0f), bmin.y + Px(7.0f)}, {bmax.x - Px(42.0f), bmax.y - Px(7.0f)});
                    draw->AddRectFilled(bar.Min, bar.Max, ImGui::GetColorU32(p.Inset), Px(2.0f));
                    const float fill = std::clamp(reduction / 24.0f, 0.0f, 1.0f);
                    if (fill > 0.0f)
                        draw->AddRectFilled({bar.Max.x - fill * bar.GetWidth(), bar.Min.y},
                                            bar.Max,
                                            ImGui::GetColorU32(p.Warning),
                                            Px(2.0f));
                    box.Reduction = bar;
                    if (ImGui::IsMouseHoveringRect(bar.Min, bar.Max))
                        ImGui::SetTooltip("Gain reduction: %.1f dB", reduction);
                }
            }
            if (toggled)
                Edit([&](AudioBusLayout& next) {
                    next.Buses[index].Effects[e].Enabled = enabled;
                    return std::optional<std::string>();
                });
            if (open)
            {
                DrawEffectParameters(index, e);
                PropertyGrid::EndGroup();
            }
            ImGui::PopID();
        }
        PropertyGrid::BeginFree();
        if (ImGui::SmallButton(ICON_FA_PLUS " Effect"))
            ImGui::OpenPopup("##addEffect");
        boxes.AddEffect = LastItem();
        if (ImGui::BeginPopup("##addEffect"))
        {
            const auto add = [&](AudioBusEffect effect) {
                if (auto* ducking = std::get_if<AudioDucking>(&effect.Parameters))
                    for (const auto& other : layout.Buses)
                        if (other.Name != bus.Name)
                        {
                            ducking->SourceBus = other.Name; // Listens to another bus by default.
                            break;
                        }
                Edit([&](AudioBusLayout& next) {
                    next.Buses[index].Effects.push_back(effect);
                    return std::optional<std::string>();
                });
            };
            for (size_t type = 0; type + 1 < AudioEffectTypes.size(); ++type)
            {
                const std::string name(AudioEffectTypes[type]);
                if (ImGui::MenuItem(name.c_str()))
                    add(MakeEffect(type));
                m_MenuBoxes.emplace_back(name, LastItem());
            }
            ImGui::Separator();
            ImGui::TextDisabled("Custom DSP");
            const auto names = m_Engine ? m_Engine->GetCustomDSPNames() : std::vector<std::string> {};
            for (const auto& name : names)
            {
                if (ImGui::MenuItem(name.c_str()))
                    add(MakeEffect(AudioEffectTypes.size() - 1, name));
                m_MenuBoxes.emplace_back(name, LastItem());
            }
            if (names.empty())
                ImGui::TextDisabled("None registered; type a name:");
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%s", m_CustomName.c_str());
            ImGui::SetNextItemWidth(Px(160.0f));
            if (ImGui::InputTextWithHint("##custom", "Custom DSP name", buffer, sizeof(buffer)))
                m_CustomName = buffer;
            if (ImGui::IsItemDeactivatedAfterEdit() && ImGui::IsKeyPressed(ImGuiKey_Enter) && !m_CustomName.empty())
            {
                add(MakeEffect(AudioEffectTypes.size() - 1, m_CustomName));
                m_CustomName.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        PropertyGrid::EndFree();
        PropertyGrid::EndGroup();
    }

    void AudioMixerPanel::DrawEffectParameters(size_t index, size_t e)
    {
        const auto& p      = EditorTheme::Colors();
        const auto& layout = m_Document.Get();
        const auto& bus    = layout.Buses[index];
        auto        params = bus.Effects[e].Parameters;
        // The type's defaults, in field order, for the rows' reset buttons.
        std::array<float, 8> defaults {};
        {
            const AudioBusEffectParameters fresh = MakeEffect(params.index()).Parameters;
            size_t                         n     = 0;
            std::visit(
                [&](const auto& value) {
                    VisitAudioEffectFields(value, [&](const char*, const auto& field, auto...) {
                        using F = std::decay_t<decltype(field)>;
                        if constexpr (!std::is_same_v<F, std::string>)
                            if (n < defaults.size())
                                defaults[n++] = float(field);
                    });
                },
                fresh);
        }
        bool   changed = false;
        size_t n       = 0;
        std::visit(
            [&](auto& value) {
                VisitAudioEffectFields(value, [&](const char* name, auto& field, auto... range) {
                    using F = std::decay_t<decltype(field)>;
                    char label[48];
                    FieldLabel(name, label, sizeof(label));
                    if constexpr (std::is_same_v<F, float>)
                    {
                        const auto [min, max] = std::array<float, 2> {float(range)...};
                        const float reset     = defaults[std::min(n++, defaults.size() - 1)];
                        changed |= PropertyGrid::Float(
                            label, field, (max - min) / 400.0f, min, max, FieldFormat(name), &reset);
                    }
                    else if constexpr (std::is_same_v<F, uint32_t>)
                    {
                        const auto [min, max] = std::array<uint32_t, 2> {uint32_t(range)...};
                        int       value       = int(field);
                        const int reset       = int(defaults[std::min(n++, defaults.size() - 1)]);
                        if (PropertyGrid::Int(label, value, int(min), int(max), &reset))
                        {
                            field   = uint32_t(std::clamp(value, int(min), int(max)));
                            changed = true;
                        }
                    }
                    else if (std::is_same_v<std::decay_t<decltype(value)>, AudioDucking>)
                    {
                        // The sidechain source: another bus of this layout.
                        if (PropertyGrid::Row("Source", "The bus whose level ducks this one"))
                            if (ImGui::BeginCombo("##source", field.c_str()))
                            {
                                for (const auto& other : layout.Buses)
                                    if (other.Name != bus.Name &&
                                        ImGui::Selectable(other.Name.c_str(), other.Name == field))
                                    {
                                        field   = other.Name;
                                        changed = true;
                                    }
                                ImGui::EndCombo();
                            }
                    }
                    else if (PropertyGrid::Row("Name", "The registered DSP callback this slot runs"))
                    {
                        char buffer[64];
                        std::snprintf(buffer, sizeof(buffer), "%s", field.c_str());
                        ImGui::PushID("custom");
                        ImGui::InputText("##name", buffer, sizeof(buffer));
                        if (ImGui::IsItemDeactivatedAfterEdit() && buffer[0])
                        {
                            field   = buffer;
                            changed = true;
                        }
                        ImGui::PopID();
                        const auto names = m_Engine ? m_Engine->GetCustomDSPNames() : std::vector<std::string> {};
                        if (m_Engine && std::find(names.begin(), names.end(), field) == names.end())
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, p.Warning);
                            ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " Not registered: passes audio through");
                            ImGui::PopStyleColor();
                        }
                    }
                });
            },
            params);
        if (IsDynamics(params) && PropertyGrid::Row("Reduction", "Gain reduction right now"))
        {
            const float reduction = m_Engine ? m_Engine->GetBusEffectGainReductionDb(bus.Name, e).value_or(0.0f) : 0.0f;
            const ImVec2 at       = ImGui::GetCursorScreenPos();
            const float  w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight();
            auto*        draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(
                {at.x, at.y + Px(5.0f)}, {at.x + w, at.y + h - Px(5.0f)}, ImGui::GetColorU32(p.Inset), Px(2.0f));
            const float fill = std::clamp(reduction / 24.0f, 0.0f, 1.0f);
            if (fill > 0.0f)
                draw->AddRectFilled({at.x + w - fill * w, at.y + Px(5.0f)},
                                    {at.x + w, at.y + h - Px(5.0f)},
                                    ImGui::GetColorU32(p.Warning),
                                    Px(2.0f));
            char text[24];
            std::snprintf(text, sizeof(text), "-%.1f dB", reduction);
            draw->AddText(
                {at.x + Px(4.0f), at.y + (h - ImGui::GetFontSize()) * 0.5f}, ImGui::GetColorU32(p.Text), text);
            ImGui::Dummy({w, h});
        }
        if (changed)
            Edit(
                [&](AudioBusLayout& next) {
                    next.Buses[index].Effects[e].Parameters = params;
                    return std::optional<std::string>();
                },
                index);
    }
} // namespace Kans

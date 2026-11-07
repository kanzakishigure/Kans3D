#include "ProjectBrowser.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include "Kans3D/Project/ProjectFile.h"
#include "Kans3D/Project/ProjectThumbnail.h"
#include "KansEditorCore/Launcher/ProjectGrid.h"
#include "UI/EditorIcons.h"
#include "UI/Widgets.h"

namespace Kans
{
    namespace fs = std::filesystem;
    namespace
    {
        const ImVec4& Accent    = EditorTheme::Colors().Accent;
        const ImVec4& ErrorText = EditorTheme::Colors().Danger;

        ImFont* BoldFont()
        {
            auto& fonts = ImGui::GetIO().Fonts->Fonts;
            return fonts.Size > 1 ? fonts[1] : ImGui::GetFont();
        }

        void Heading(const char* text, float size = 20.0f)
        {
            ImGui::PushFont(BoldFont(), size);
            ImGui::TextUnformatted(text);
            ImGui::PopFont();
        }

        // Accent-filled main action button.
        bool PrimaryButton(const char* label, bool enabled = true)
        {
            ImGui::BeginDisabled(!enabled);
            ImGui::PushStyleColor(ImGuiCol_Button, Accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Colors().AccentHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Colors().AccentPressed);
            const bool pressed = ImGui::Button(label);
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
            return pressed;
        }

        float ButtonWidth(const char* label)
        {
            return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        }

        bool ContainsIgnoreCase(const std::string& text, const std::string& needle)
        {
            return std::search(text.begin(), text.end(), needle.begin(), needle.end(), [](unsigned char a, unsigned char b) {
                       return std::tolower(a) == std::tolower(b);
                   }) != text.end();
        }

        const char* TemplateDescription(const std::string& name)
        {
            if (name == "Blank")
                return "An empty scene with a camera and lights.";
            if (name == "Showcase")
                return "Sample scene with PBR and toon materials and HDR skies.";
            return "";
        }
    } // namespace

    ProjectBrowser::ProjectBrowser(fs::path templatesDir)
        : m_TemplatesDir(std::move(templatesDir)), m_Recent(RecentProjects::DefaultStorageFile())
    {
        Refresh();
    }

    void ProjectBrowser::Refresh()
    {
        if (auto error = m_Recent.Load())
            m_Error = error->Message;
        m_Templates      = Launcher::ListTemplates(m_TemplatesDir);
        m_SelectedRecent = m_Recent.Entries().empty() ? -1 : 0;
        RefreshCards();
        m_Result.reset();
        ResetNewProject();
        // Start where there is something to do.
        m_Page = m_Recent.Entries().empty() ? Page::New : Page::Recent;
    }

    void ProjectBrowser::SetPage(Page page)
    {
        m_Page = page;
        m_Error.clear();
    }

    void ProjectBrowser::ResetNewProject()
    {
        std::vector<fs::path> recent;
        for (const auto& entry : m_Recent.Entries())
            recent.push_back(entry.ProjectFile);
        m_Location         = Launcher::DefaultLocation(recent, Launcher::HomeDirectory()).u8string();
        m_Name             = Launcher::UniqueProjectName(fs::u8path(m_Location), "NewProject");
        m_SelectedTemplate = 0;
    }

    void ProjectBrowser::BrowseForProject()
    {
        const auto& entries = m_Recent.Entries();
        m_Browser.Open(FolderBrowser::Mode::Project,
                       entries.empty() ? Launcher::HomeDirectory() : entries.front().ProjectFile.parent_path().parent_path(),
                       "Open Project");
        m_BrowseTarget = BrowseTarget::Project;
    }

    void ProjectBrowser::Request(const fs::path& path)
    {
        m_Error.clear();
        auto selection = Launcher::ResolveSelection(path);
        if (auto* file = std::get_if<fs::path>(&selection))
            m_Result = *file;
        else if (auto* missing = std::get_if<Launcher::MissingProjectFile>(&selection))
        {
            m_MissingProjectFolder = missing->Folder;
            m_AskCreateProjectFile = true;
        }
        else
            m_Error = std::get<AssetError>(selection).Message;
    }

    std::optional<fs::path> ProjectBrowser::Draw()
    {
        const float sidebar = 210.0f;
        DrawSidebar(sidebar);
        ImGui::SameLine(0, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24.0f, 20.0f});
        if (ImGui::BeginChild("##ProjectBrowserMain", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding))
        {
            if (m_Page == Page::Recent)
                DrawRecent();
            else
                DrawNew();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        DrawDialogs();
        auto result = m_Result;
        m_Result.reset();
        return result;
    }

    void ProjectBrowser::DrawSidebar(float width)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Colors().App);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12.0f, 20.0f});
        ImGui::BeginChild("##ProjectBrowserSidebar", {width, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::Indent(4.0f);
        Heading("Kans3D", 24.0f);
        ImGui::TextDisabled("Projects");
        ImGui::Unindent(4.0f);
        ImGui::Dummy({0, 12.0f});

        const auto item = [&](const char* label, Page page) {
            const bool selected = m_Page == page;
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, {0.0f, 0.5f});
            if (ImGui::Selectable(label, selected, 0, {0, ImGui::GetFrameHeight() + 6.0f}))
                SetPage(page);
            ImGui::PopStyleVar();
            if (selected) // Accent bar marks the active page.
            {
                auto* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(ImGui::GetItemRectMin(),
                                    {ImGui::GetItemRectMin().x + 3.0f, ImGui::GetItemRectMax().y},
                                    ImGui::GetColorU32(Accent));
            }
        };
        item(ICON_FA_CLOCK_ROTATE_LEFT "   Recent Projects", Page::Recent);
        item(ICON_FA_PLUS "   New Project", Page::New);

        // Open sits at the bottom, apart from the pages.
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(),
                                      ImGui::GetWindowHeight() - ImGui::GetFrameHeight() - ImGui::GetStyle().WindowPadding.y));
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open Project...", {-FLT_MIN, 0}))
            BrowseForProject();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    void ProjectBrowser::RefreshCards()
    {
        m_Cards.clear();
        for (const auto& entry : m_Recent.Entries())
        {
            Card card;
            card.Exists = RecentProjects::Exists(entry);
            if (card.Exists)
                if (auto loaded = LoadProjectFile(entry.ProjectFile); auto* file = std::get_if<ProjectFile>(&loaded))
                    if (auto thumbnail = FindProjectThumbnail(MakeProjectConfig(entry.ProjectFile.parent_path(), *file)))
                    {
                        std::error_code ec;
                        card.Thumbnail = *thumbnail;
                        card.Modified  = fs::last_write_time(*thumbnail, ec);
                    }
            m_Cards.push_back(std::move(card));
        }
    }

    void ProjectBrowser::DrawRecent()
    {
        Heading("Recent Projects");
        const auto& entries = m_Recent.Entries();
        if (!entries.empty())
        {
            ImGui::SameLine();
            const float searchWidth = std::min(EditorTheme::Px(260.0f), ImGui::GetContentRegionAvail().x * 0.5f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - searchWidth);
            ImGui::SetNextItemWidth(searchWidth);
            ImGui::InputTextWithHint("##Search", ICON_FA_MAGNIFYING_GLASS "  Search", &m_Search);
        }
        ImGui::Dummy({0, EditorTheme::Px(6.0f)});

        if (entries.empty())
        {
            ImGui::Dummy({0, 40.0f});
            ImGui::TextDisabled("No recent projects yet.");
            ImGui::Dummy({0, 8.0f});
            if (PrimaryButton(ICON_FA_PLUS "  New Project"))
                SetPage(Page::New);
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open Project..."))
                BrowseForProject();
            return;
        }
        if (m_Cards.size() != entries.size())
            RefreshCards();
        if (!m_Thumbnails)
            m_Thumbnails = std::make_unique<Browser::ThumbnailCache>();
        m_Thumbnails->Update();

        // The cards the search leaves, in list order; the selection moves among them.
        std::vector<int> visible;
        for (int i = 0; i < int(entries.size()); ++i)
            if (m_Search.empty() || ContainsIgnoreCase(entries[i].Name, m_Search) ||
                ContainsIgnoreCase(entries[i].ProjectFile.parent_path().u8string(), m_Search))
                visible.push_back(i);
        const auto at           = std::find(visible.begin(), visible.end(), m_SelectedRecent);
        const int  selectedSlot = at == visible.end() ? -1 : int(at - visible.begin());

        const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + EditorTheme::Px(8.0f);
        int         remove = -1;
        // A little inset keeps the outermost hairlines and the selection outline unclipped.
        const float inset = EditorTheme::Px(EditorTheme::Space1);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {inset, inset});
        if (ImGui::BeginChild("##RecentGrid", {0, -footer}, ImGuiChildFlags_AlwaysUseWindowPadding))
        {
            const float gap  = EditorTheme::Px(EditorTheme::Space3);
            const auto  grid = Launcher::ComputeProjectGrid(
                ImGui::GetContentRegionAvail().x, EditorTheme::Px(220.0f), EditorTheme::Px(260.0f), gap);
            const float thumbnail = IM_ROUND(grid.CardWidth * 9.0f / 16.0f);
            const float line      = ImGui::GetTextLineHeight();
            const float strip     = IM_ROUND(line * 3.0f + EditorTheme::Px(EditorTheme::Space1) * 2.0f +
                                         EditorTheme::Px(EditorTheme::Space2) * 2.0f);
            const ImVec2 card {grid.CardWidth, thumbnail + strip};

            // Arrow keys move the selection; typing in the search box keeps them.
            const bool keys = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() &&
                              !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
            const std::pair<ImGuiKey, Launcher::GridMove> moves[] = {{ImGuiKey_LeftArrow, Launcher::GridMove::Left},
                                                                     {ImGuiKey_RightArrow, Launcher::GridMove::Right},
                                                                     {ImGuiKey_UpArrow, Launcher::GridMove::Up},
                                                                     {ImGuiKey_DownArrow, Launcher::GridMove::Down}};
            for (const auto& [key, move] : moves)
                if (keys && !visible.empty() && ImGui::IsKeyPressed(key))
                {
                    m_SelectedRecent =
                        visible[Launcher::MoveGridSelection(selectedSlot, int(visible.size()), grid.Columns, move)];
                    m_ScrollToSelection = true;
                }

            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true); // Arrows belong to the grid, not ImGui navigation.
            for (int slot = 0; slot < int(visible.size()); ++slot)
            {
                const int column = slot % grid.Columns, row = slot / grid.Columns;
                DrawCard(visible[slot],
                         {IM_ROUND(origin.x + column * (card.x + gap)), IM_ROUND(origin.y + row * (card.y + gap))},
                         card,
                         thumbnail,
                         remove);
            }
            ImGui::PopItemFlag();
            const int rows = (int(visible.size()) + grid.Columns - 1) / grid.Columns;
            ImGui::SetCursorScreenPos(origin);
            ImGui::Dummy({1.0f, rows > 0 ? rows * card.y + (rows - 1) * gap : 0.0f});
            if (visible.empty())
                ImGui::TextDisabled("No projects match \"%s\".", m_Search.c_str());
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        m_ScrollToSelection = false;

        if (remove >= 0)
        {
            m_Recent.Remove(entries[remove].ProjectFile);
            if (auto error = m_Recent.Save())
                m_Error = error->Message;
            RefreshCards();
            m_SelectedRecent = std::min(m_SelectedRecent, int(m_Recent.Entries().size()) - 1);
        }

        ImGui::Dummy({0, 4.0f});
        if (!m_Error.empty())
            ImGui::TextColored(ErrorText, ICON_FA_TRIANGLE_EXCLAMATION "  %s", m_Error.c_str());
        else
            ImGui::NewLine();
        const auto& current = m_Recent.Entries();
        const bool  shown   = std::find(visible.begin(), visible.end(), m_SelectedRecent) != visible.end();
        const bool  canOpen = remove < 0 && shown && m_SelectedRecent < int(m_Cards.size()) && m_Cards[m_SelectedRecent].Exists;
        const char* openLabel = ICON_FA_FOLDER_OPEN "  Open";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ButtonWidth(openLabel));
        if (PrimaryButton(openLabel, canOpen) ||
            (canOpen && !ImGui::IsAnyItemActive() &&
             !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
             (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))))
            Request(current[m_SelectedRecent].ProjectFile);
    }

    void ProjectBrowser::DrawCard(int index, ImVec2 min, ImVec2 size, float thumbnailHeight, int& remove)
    {
        const auto&  entry    = m_Recent.Entries()[index];
        const auto&  info     = m_Cards[index];
        const auto&  theme    = EditorTheme::Colors();
        const bool   selected = m_SelectedRecent == index;
        const ImVec2 max {min.x + size.x, min.y + size.y};
        const float  rounding = EditorTheme::Px(6.0f);
        auto*        draw     = ImGui::GetWindowDrawList();
        ImGui::PushID(index);

        ImGui::SetCursorScreenPos(min);
        ImGui::InvisibleButton("##Card", size, ImGuiButtonFlags_AllowOverlap);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            m_SelectedRecent = index;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && info.Exists)
            Request(entry.ProjectFile);
        if (selected && m_ScrollToSelection)
            ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
        const bool menuOpen = ImGui::IsPopupOpen("##CardMenu");
        const bool hovered  = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                             ImGui::IsMouseHoveringRect(min, max);

        // A tone step above the page; lighter under the pointer.
        const auto mix = [](ImVec4 a, ImVec4 b, float t) {
            return ImVec4 {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f};
        };
        const ImVec4 body = hovered ? theme.Hover : theme.Raised;
        draw->AddRectFilled(min, max, ImGui::GetColorU32(body), rounding);

        // 16:9 thumbnail, or a tinted placeholder with the project icon.
        const ImVec2   thumbMax {max.x, min.y + thumbnailHeight};
        const float    dim     = info.Exists ? 1.0f : 0.45f;
        Ref<Texture2D> texture = info.Thumbnail.empty() || !m_Thumbnails ?
                                     nullptr :
                                     m_Thumbnails->Get(info.Thumbnail, info.Modified, 512);
        if (texture && texture->GetWidth() && texture->GetHeight())
        {
            // Fill the frame, cropping the longer side (textures store the bottom row first).
            const float aspect = float(texture->GetWidth()) / float(texture->GetHeight());
            const float frame  = size.x / thumbnailHeight;
            float       u = 0.0f, v = 0.0f;
            if (aspect > frame)
                u = (1.0f - frame / aspect) * 0.5f;
            else
                v = (1.0f - aspect / frame) * 0.5f;
            draw->AddImageRounded(static_cast<ImTextureID>(uintptr_t(texture->GetImGuiTextureID())),
                                  min,
                                  thumbMax,
                                  {u, 1.0f - v},
                                  {1.0f - u, v},
                                  ImGui::GetColorU32(ImVec4 {1, 1, 1, dim}),
                                  rounding,
                                  ImDrawFlags_RoundCornersTop);
        }
        else
        {
            draw->AddRectFilled(min, thumbMax, ImGui::GetColorU32(mix(body, theme.Accent, 0.10f)), rounding, ImDrawFlags_RoundCornersTop);
            ImGui::PushFont(nullptr, EditorTheme::Px(40.0f));
            Widgets::CenterGlyph(ICON_FA_CUBE,
                                 min,
                                 thumbMax,
                                 ImGui::GetColorU32(info.Exists ? ImVec4 {theme.Accent.x, theme.Accent.y, theme.Accent.z, 0.7f} :
                                                                  theme.TextOff));
            ImGui::PopFont();
        }

        // Darker strip with the name, the path and when it was last opened.
        draw->AddRectFilled({min.x, thumbMax.y}, max, ImGui::GetColorU32(mix(body, theme.App, 0.45f)), rounding, ImDrawFlags_RoundCornersBottom);
        const float pad   = EditorTheme::Px(EditorTheme::Space3);
        const float line  = ImGui::GetTextLineHeight();
        const float textX = min.x + pad;
        const float textW = size.x - pad * 2.0f;
        float       y     = thumbMax.y + EditorTheme::Px(EditorTheme::Space2);
        ImGui::PushFont(BoldFont(), 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, info.Exists ? theme.Text : theme.TextOff);
        ImGui::RenderTextEllipsis(draw, {textX, y}, {textX + textW, y + line}, textX + textW, entry.Name.c_str(), nullptr, nullptr);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        y += line + EditorTheme::Px(EditorTheme::Space1);
        const auto folder = entry.ProjectFile.parent_path().u8string();
        const auto path   = Launcher::EllipsizeMiddle(folder, textW, [](std::string_view text) {
            return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
        });
        draw->AddText({textX, y}, ImGui::GetColorU32(info.Exists ? theme.TextDim : theme.TextOff), path.c_str());
        if (hovered && ImGui::IsMouseHoveringRect({textX, y}, {textX + textW, y + line}))
            ImGui::SetTooltip("%s", entry.ProjectFile.u8string().c_str());
        y += line + EditorTheme::Px(EditorTheme::Space1);
        const auto when = Launcher::FormatLastOpened(static_cast<int64_t>(std::time(nullptr)), entry.LastOpened);
        draw->AddText({textX, y}, ImGui::GetColorU32(theme.TextOff), when.c_str());

        // Layering: a hairline on every card, lighter under the pointer; the selection gets
        // an accent outline with a soft inner glow. (Cards are the exception to "no lines".)
        if (selected)
        {
            const ImVec4 glow {theme.Accent.x, theme.Accent.y, theme.Accent.z, 0.28f};
            const float  o = EditorTheme::Px(1.0f);
            draw->AddRect({min.x + o * 2.0f, min.y + o * 2.0f}, {max.x - o * 2.0f, max.y - o * 2.0f},
                          ImGui::GetColorU32(glow), std::max(0.0f, rounding - o * 2.0f), 0, EditorTheme::Px(2.0f));
            draw->AddRect(min, max, ImGui::GetColorU32(theme.Accent), rounding, 0, EditorTheme::Px(2.0f));
        }
        else
        {
            const ImVec4 hairline = hovered ? ImVec4 {1, 1, 1, 0.22f} : ImVec4 {1, 1, 1, 0.08f};
            draw->AddRect(min, max, ImGui::GetColorU32(hairline), rounding, 0, 1.0f);
        }

        // A missing project is dimmed and badged; it cannot be opened.
        if (!info.Exists)
        {
            const char*  text  = ICON_FA_TRIANGLE_EXCLAMATION "  Missing";
            const ImVec2 label = ImGui::CalcTextSize(text);
            const ImVec2 bmin {min.x + EditorTheme::Px(EditorTheme::Space2), min.y + EditorTheme::Px(EditorTheme::Space2)};
            const ImVec2 bmax {bmin.x + label.x + EditorTheme::Px(EditorTheme::Space2) * 2.0f,
                               bmin.y + label.y + EditorTheme::Px(EditorTheme::Space1) * 2.0f};
            draw->AddRectFilled(bmin, bmax, ImGui::GetColorU32(ImVec4 {theme.Danger.x, theme.Danger.y, theme.Danger.z, 0.9f}),
                                EditorTheme::Px(4.0f));
            draw->AddText({bmin.x + EditorTheme::Px(EditorTheme::Space2), bmin.y + EditorTheme::Px(EditorTheme::Space1)},
                          IM_COL32_WHITE,
                          text);
        }

        // The card's actions, top right, while the pointer is over it.
        if (hovered || menuOpen)
        {
            const float  button = EditorTheme::Px(26.0f);
            const ImVec2 at {max.x - button - EditorTheme::Px(EditorTheme::Space2), min.y + EditorTheme::Px(EditorTheme::Space2)};
            ImGui::SetCursorScreenPos(at);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4 {0, 0, 0, 0.45f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4 {0, 0, 0, 0.65f});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4 {0, 0, 0, 0.8f});
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, EditorTheme::Px(4.0f));
            if (ImGui::Button("##CardActions", {button, button}))
            {
                m_SelectedRecent = index;
                ImGui::OpenPopup("##CardMenu");
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            Widgets::CenterGlyph(ICON_FA_ELLIPSIS_VERTICAL, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32_WHITE);
        }
        if (ImGui::BeginPopup("##CardMenu"))
        {
            Widgets::PushMenuStyle();
            if (Widgets::IconMenuItem(ICON_FA_FOLDER_OPEN, "Open", nullptr, false, info.Exists))
                Request(entry.ProjectFile);
            if (Widgets::IconMenuItem(ICON_FA_XMARK, "Remove from List"))
                remove = index;
            Widgets::PopMenuStyle();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    void ProjectBrowser::DrawNew()
    {
        Heading("New Project");
        ImGui::TextDisabled("Start from a template.");
        ImGui::Dummy({0, 8.0f});

        if (m_Templates.empty())
            ImGui::TextColored(ErrorText, ICON_FA_TRIANGLE_EXCLAMATION "  No templates found in %s",
                               m_TemplatesDir.u8string().c_str());
        // Template cards.
        const float cardWidth  = 220.0f;
        const float cardHeight = 128.0f;
        for (int i = 0; i < int(m_Templates.size()); ++i)
        {
            ImGui::PushID(i);
            if (i > 0)
                ImGui::SameLine(0, 12.0f);
            const bool selected = m_SelectedTemplate == i;
            const auto top      = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##Template", {cardWidth, cardHeight}))
                m_SelectedTemplate = i;
            auto*       draw    = ImGui::GetWindowDrawList();
            const bool  hovered = ImGui::IsItemHovered();
            const ImVec2 bottom {top.x + cardWidth, top.y + cardHeight};
            draw->AddRectFilled(top, bottom, ImGui::GetColorU32(hovered ? EditorTheme::Colors().Hover : EditorTheme::Colors().Raised), 6.0f);
            if (selected)
                draw->AddRect(top, bottom, ImGui::GetColorU32(Accent), 6.0f, 0, 2.0f);
            ImGui::PushFont(nullptr, 28.0f);
            draw->AddText({top.x + 14.0f, top.y + 14.0f},
                          ImGui::GetColorU32(selected ? Accent : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)),
                          m_Templates[i].Name == "Blank" ? ICON_FA_FILE : ICON_FA_SHAPES);
            ImGui::PopFont();
            ImGui::PushFont(BoldFont(), 0.0f);
            draw->AddText({top.x + 14.0f, top.y + 56.0f}, ImGui::GetColorU32(ImGuiCol_Text), m_Templates[i].Name.c_str());
            ImGui::PopFont();
            draw->AddText(nullptr,
                          0.0f,
                          {top.x + 14.0f, top.y + 56.0f + ImGui::GetTextLineHeight() + 4.0f},
                          ImGui::GetColorU32(ImGuiCol_TextDisabled),
                          TemplateDescription(m_Templates[i].Name),
                          nullptr,
                          cardWidth - 28.0f);
            ImGui::PopID();
        }
        ImGui::Dummy({0, 16.0f});

        // Name and location, laid out as a two-column form.
        const float labelWidth = 110.0f;
        const float fieldWidth = std::min(560.0f, ImGui::GetContentRegionAvail().x - labelWidth);
        const auto  label      = [&](const char* text) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(text);
            ImGui::SameLine(labelWidth);
        };
        label("Project name");
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::InputText("##Name", &m_Name);
        label("Location");
        const char* browse = ICON_FA_FOLDER "  Browse...";
        ImGui::SetNextItemWidth(fieldWidth - ButtonWidth(browse) - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText("##Location", &m_Location);
        ImGui::SameLine();
        if (ImGui::Button(browse))
        {
            m_Browser.Open(FolderBrowser::Mode::Folder, fs::u8path(m_Location), "Choose Location");
            m_BrowseTarget = BrowseTarget::Location;
        }

        const auto location = fs::u8path(m_Location);
        const auto problem  = Launcher::NewProjectProblem(location, m_Name);
        ImGui::Dummy({0, 4.0f});
        ImGui::SetCursorPosX(labelWidth); // Window-relative, like SameLine(labelWidth) above.
        if (problem)
            ImGui::TextColored(ErrorText, ICON_FA_TRIANGLE_EXCLAMATION "  %s", problem->c_str());
        else
            ImGui::TextDisabled("Creates %s", (location / fs::u8path(m_Name) / fs::u8path(m_Name + ".kproj")).u8string().c_str());
        if (!m_Error.empty())
        {
            ImGui::SetCursorPosX(labelWidth);
            ImGui::TextColored(ErrorText, ICON_FA_TRIANGLE_EXCLAMATION "  %s", m_Error.c_str());
        }

        ImGui::Dummy({0, 12.0f});
        ImGui::SetCursorPosX(labelWidth);
        const bool canCreate = !problem && m_SelectedTemplate < int(m_Templates.size());
        if (PrimaryButton(ICON_FA_PLUS "  Create Project", canCreate))
        {
            m_Error.clear();
            std::error_code ec;
            fs::create_directories(location, ec);
            auto created = CreateProjectFromTemplate(m_Templates[m_SelectedTemplate].Directory, location, m_Name);
            if (auto* file = std::get_if<fs::path>(&created))
                m_Result = *file;
            else
                m_Error = std::get<AssetError>(created).Message;
        }
    }

    void ProjectBrowser::DrawDialogs()
    {
        if (auto chosen = m_Browser.Draw())
        {
            if (m_BrowseTarget == BrowseTarget::Project)
                Request(*chosen);
            else if (m_BrowseTarget == BrowseTarget::Location)
                m_Location = chosen->u8string();
            m_BrowseTarget = BrowseTarget::None;
        }

        if (m_AskCreateProjectFile)
        {
            ImGui::OpenPopup("Create Project File?");
            m_AskCreateProjectFile = false;
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        if (ImGui::BeginPopupModal("Create Project File?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        {
            const auto name = m_MissingProjectFolder.filename().u8string();
            ImGui::Text("%s has no project file.", m_MissingProjectFolder.u8string().c_str());
            ImGui::Text("Create %s.kproj there and open the folder as a project?", name.c_str());
            ImGui::Dummy({0, 6.0f});
            if (PrimaryButton("Create and Open"))
            {
                auto created = Launcher::CreateProjectFile(m_MissingProjectFolder);
                if (auto* file = std::get_if<fs::path>(&created))
                    m_Result = *file;
                else
                    m_Error = std::get<AssetError>(created).Message;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
} // namespace Kans

#include "UIDesignerPanel.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <misc/cpp/imgui_stdlib.h>
#include "Kans3D/ImGui/EditorTheme.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"
#include "KansEditorCore/UI/SourceBuild.h"

namespace Kans
{
    namespace
    {
        constexpr float TreeWidth = 240.0f, DetailsWidth = 320.0f;

        struct Preset
        {
            const char* Name;
            float       Width, Height;
        };
        const Preset Resolutions[] {{"1920 x 1080", 1920, 1080},
                                    {"1280 x 720", 1280, 720},
                                    {"2560 x 1440", 2560, 1440},
                                    {"1080 x 1920", 1080, 1920},
                                    {"750 x 1334", 750, 1334},
                                    {"2048 x 1536", 2048, 1536}};

        const char* TypeName(UIWidgetType type)
        {
            static const char* names[] {
                "Panel", "Image", "Text", "UI", "Button", "Toggle", "Slider", "Scroll View", "Input Field"};
            return names[int(type)];
        }
        const char* TypeIcon(UIWidgetType type)
        {
            switch (type)
            {
                case UIWidgetType::Panel:
                    return ICON_FA_SQUARE;
                case UIWidgetType::Image:
                    return ICON_FA_IMAGE;
                case UIWidgetType::Text:
                    return ICON_FA_FONT;
                case UIWidgetType::UIInstance:
                    return ICON_FA_DESKTOP;
                case UIWidgetType::Button:
                    return ICON_FA_HAND_POINTER;
                case UIWidgetType::Toggle:
                    return ICON_FA_SQUARE_CHECK;
                case UIWidgetType::Slider:
                    return ICON_FA_SLIDERS;
                case UIWidgetType::ScrollView:
                    return ICON_FA_LIST;
                case UIWidgetType::InputField:
                    return ICON_FA_I_CURSOR;
            }
            return ICON_FA_SQUARE;
        }

        // The colour a widget shows when its Color property is not set.
        glm::vec4 DefaultFill(UIWidgetType type)
        {
            switch (type)
            {
                case UIWidgetType::Image:
                    return {1, 1, 1, 1};
                case UIWidgetType::Button:
                    return {.24f, .25f, .29f, 1};
                case UIWidgetType::Toggle:
                    return {.2f, .2f, .23f, 1};
                case UIWidgetType::Slider:
                    return {.2f, .2f, .23f, 1};
                case UIWidgetType::InputField:
                    return {.11f, .11f, .12f, 1};
                default:
                    return {0, 0, 0, 0};
            }
        }

        uint64_t MaxID(const std::vector<UIWidget>& widgets)
        {
            uint64_t id = 0;
            for (const auto& w : widgets)
                id = std::max({id, w.ID, MaxID(w.Children)});
            return id;
        }

        bool Contains(const UIWidget& w, uint64_t id)
        {
            return w.ID == id ||
                   std::any_of(w.Children.begin(), w.Children.end(), [&](const auto& c) { return Contains(c, id); });
        }

        UIWidget Make(UIWidgetType type, std::string name, glm::vec2 size)
        {
            UIWidget w;
            w.Type            = type;
            w.Name            = std::move(name);
            w.Rect.AnchorsMin = w.Rect.AnchorsMax = {.5f, .5f};
            w.Rect.Size                           = size;
            return w;
        }

        // Fills the parent, inset by `inset` on every side.
        void Stretch(UIWidget& w, float inset = 0)
        {
            w.Rect.AnchorsMin = {0, 0};
            w.Rect.AnchorsMax = {1, 1};
            w.Rect.Offsets    = glm::vec4(inset);
        }

        ImU32 Color(glm::vec4 c, float alpha = 1) { return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, c.a * alpha)); }
        ImU32 Color(const ImVec4& c, float alpha = 1) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }
    } // namespace

    bool UIDesignerPanel::Open(const std::filesystem::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        std::string   text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in && !in.eof())
        {
            m_Status = "Cannot read " + file.filename().u8string();
            return false;
        }
        auto parsed = ParseUISource(reinterpret_cast<const std::byte*>(text.data()), text.size());
        if (auto* error = std::get_if<AssetError>(&parsed))
        {
            m_Status = file.filename().u8string() + ": " + error->Message;
            return false;
        }
        m_File        = file;
        m_Data        = std::get<UIData>(std::move(parsed));
        m_Loaded      = true;
        m_Dirty       = false;
        m_Selected    = 0;
        m_Resolution  = m_Data.ReferenceResolution;
        m_LayoutDirty = true;
        m_Status.clear();
        return true;
    }

    void UIDesignerPanel::Save()
    {
        if (!m_Loaded)
            return;
        if (auto problem = ValidateUI(m_Data); std::holds_alternative<AssetError>(problem))
        {
            m_Status = "Not saved: " + std::get<AssetError>(problem).Message;
            return;
        }
        auto text = SerializeUISource(m_Data);
        if (auto* error = std::get_if<AssetError>(&text))
        {
            m_Status = "Not saved: " + error->Message;
            return;
        }
        std::ofstream out(m_File, std::ios::binary | std::ios::trunc);
        out << std::get<std::string>(text);
        if (!out)
        {
            m_Status = "Cannot write " + m_File.filename().u8string();
            return;
        }
        out.close();
        m_Dirty  = false;
        m_Status = BuildSavedSource(m_Service, m_File);
    }

    void UIDesignerPanel::Changed()
    {
        m_Dirty       = true;
        m_LayoutDirty = true;
    }

    const UIInstance& UIDesignerPanel::Layout()
    {
        if (!m_Layout || m_LayoutDirty)
        {
            m_Layout = std::make_unique<UIInstance>(CreateRef<const UIAsset>(m_Data));
            (void)m_Layout->Refresh(); // Builds the widget tree; a failed build shows an empty canvas.
            m_LayoutDirty = false;
        }
        m_Layout->Update(m_Resolution);
        return *m_Layout;
    }

    UIWidget* UIDesignerPanel::Find(uint64_t id, std::vector<UIWidget>** siblings, UIWidget** parent)
    {
        std::function<UIWidget*(std::vector<UIWidget>&, UIWidget*)> search = [&](std::vector<UIWidget>& list,
                                                                                 UIWidget* owner) -> UIWidget* {
            for (auto& w : list)
            {
                if (w.ID == id)
                {
                    if (siblings)
                        *siblings = &list;
                    if (parent)
                        *parent = owner;
                    return &w;
                }
                if (auto* found = search(w.Children, &w))
                    return found;
            }
            return nullptr;
        };
        return id ? search(m_Data.Widgets, nullptr) : nullptr;
    }

    const UIRuntimeWidget* UIDesignerPanel::RuntimeOf(uint64_t id)
    {
        std::function<const UIRuntimeWidget*(const std::vector<UIRuntimeWidget>&)> search =
            [&](const std::vector<UIRuntimeWidget>& list) -> const UIRuntimeWidget* {
            for (const auto& w : list)
            {
                if (w.Data.ID == id)
                    return &w;
                if (auto* found = search(w.Children))
                    return found;
            }
            return nullptr;
        };
        return search(Layout().Widgets());
    }

    glm::vec4 UIDesignerPanel::ParentBounds(uint64_t id)
    {
        UIWidget* parent = nullptr;
        Find(id, nullptr, &parent);
        if (parent)
            if (const auto* rt = RuntimeOf(parent->ID))
                return rt->Bounds;
        return {0, 0, m_Resolution.x, m_Resolution.y};
    }

    uint64_t UIDesignerPanel::AddWidget(UIWidgetType type)
    {
        uint64_t   id    = MaxID(m_Data.Widgets) + 1;
        const auto named = [&](const char* base) { return std::string(base) + std::to_string(id); };
        UIWidget   w;
        switch (type)
        {
            case UIWidgetType::Panel:
                w                     = Make(type, named("Panel"), {400, 300});
                w.Properties["Color"] = UIEncode(glm::vec4(.1f, .1f, .12f, .85f));
                break;
            case UIWidgetType::Image:
                w = Make(type, named("Image"), {200, 200});
                break;
            case UIWidgetType::Text:
                w                     = Make(type, named("Text"), {320, 60});
                w.Properties["Text"]  = "Text";
                w.Properties["Size"]  = 32;
                w.Properties["Color"] = UIEncode(glm::vec4(1));
                break;
            case UIWidgetType::Button: {
                w = Make(type, named("Button"), {240, 64});
                // Its label is a stretched Text child, so it can be styled like any text.
                UIWidget label = Make(UIWidgetType::Text, "Label", {0, 0});
                label.ID       = id + 1;
                Stretch(label);
                label.Properties["Text"]                = "Button";
                label.Properties["Size"]                = 28;
                label.Properties["HorizontalAlignment"] = "Center";
                label.Properties["VerticalAlignment"]   = "Center";
                w.Children.push_back(label);
                break;
            }
            case UIWidgetType::Toggle:
                w = Make(type, named("Toggle"), {40, 40});
                break;
            case UIWidgetType::Slider:
                w                     = Make(type, named("Slider"), {320, 32});
                w.Properties["Min"]   = 0;
                w.Properties["Max"]   = 1;
                w.Properties["Value"] = .5f;
                break;
            case UIWidgetType::ScrollView: {
                w                 = Make(type, named("ScrollView"), {400, 300});
                UIWidget viewport = Make(UIWidgetType::Panel, "Viewport", {0, 0});
                viewport.ID       = id + 1;
                Stretch(viewport);
                viewport.Properties["ClipChildren"] = true;
                UIWidget content                    = Make(UIWidgetType::Panel, "Content", {400, 600});
                content.ID                          = id + 2;
                content.Rect.AnchorsMin = content.Rect.AnchorsMax = {0, 0};
                content.Rect.Pivot                                = {0, 0};
                viewport.Children.push_back(content);
                w.Children.push_back(viewport);
                w.Properties["Viewport"] = "Viewport";
                w.Properties["Content"]  = "Viewport/Content";
                break;
            }
            case UIWidgetType::InputField:
                w                           = Make(type, named("InputField"), {320, 56});
                w.Properties["Placeholder"] = "Enter text...";
                w.Properties["Size"]        = 24;
                break;
            case UIWidgetType::UIInstance:
                w = Make(type, named("UI"), {400, 300});
                break;
        }
        w.ID              = id;
        UIWidget* parent  = Find(m_Selected);
        auto&     targets = parent ? parent->Children : m_Data.Widgets;
        targets.push_back(std::move(w));
        m_Selected = id;
        Changed();
        return id;
    }

    bool UIDesignerPanel::DeleteSelected()
    {
        std::vector<UIWidget>* siblings = nullptr;
        UIWidget*              parent   = nullptr;
        UIWidget*              w        = Find(m_Selected, &siblings, &parent);
        if (!w)
            return false;
        siblings->erase(siblings->begin() + (w - siblings->data()));
        m_Selected = parent ? parent->ID : 0;
        Changed();
        return true;
    }

    void UIDesignerPanel::MoveSelected(glm::vec2 delta, bool resize)
    {
        UIWidget* w = Find(m_Selected);
        if (!w || delta == glm::vec2(0))
            return;
        const float scale = std::max(Layout().PixelScale(), 1e-6f);
        const auto  d     = delta / scale;
        auto&       r     = w->Rect;
        for (int i = 0; i < 2; ++i)
        {
            const bool stretched = r.AnchorsMin[i] != r.AnchorsMax[i];
            if (!resize)
            {
                if (stretched)
                    r.Offsets[i] += d[i], r.Offsets[i + 2] -= d[i];
                else
                    r.Position[i] += d[i];
            }
            else if (stretched)
                r.Offsets[i + 2] -= d[i];
            else
            {
                // The top-left corner stays put: the pivot moves with a share of the growth.
                const float grown = std::max(1.0f, r.Size[i] + d[i]) - r.Size[i];
                r.Size[i] += grown;
                r.Position[i] += grown * r.Pivot[i];
            }
        }
        Changed();
    }

    void UIDesignerPanel::SetAnchors(glm::vec2 min, glm::vec2 max)
    {
        UIWidget*   w  = Find(m_Selected);
        const auto* rt = w ? RuntimeOf(w->ID) : nullptr;
        if (!w || !rt)
            return;
        const auto      parent = ParentBounds(w->ID);
        const float     scale  = std::max(Layout().PixelScale(), 1e-6f);
        const glm::vec2 parentSize {parent.z - parent.x, parent.w - parent.y};
        const glm::vec2 localMin {rt->Bounds.x - parent.x, rt->Bounds.y - parent.y};
        const glm::vec2 size {rt->Bounds.z - rt->Bounds.x, rt->Bounds.w - rt->Bounds.y};
        auto&           r = w->Rect;
        for (int i = 0; i < 2; ++i)
        {
            if (min[i] == max[i])
            {
                r.Size[i]     = size[i] / scale;
                r.Position[i] = (localMin[i] + size[i] * r.Pivot[i] - parentSize[i] * min[i]) / scale;
            }
            else
            {
                r.Offsets[i]     = (localMin[i] - parentSize[i] * min[i]) / scale;
                r.Offsets[i + 2] = (parentSize[i] * max[i] - (localMin[i] + size[i])) / scale;
            }
        }
        r.AnchorsMin = min;
        r.AnchorsMax = max;
        Changed();
    }

    void UIDesignerPanel::Draw()
    {
        if (!m_Loaded)
        {
            ImGui::TextDisabled("%s",
                                m_Status.empty() ?
                                    "Double-click a UI asset (.kui) in the Content Browser to design it." :
                                    m_Status.c_str());
            return;
        }
        DrawToolbar();
        const float tree = EditorTheme::Px(TreeWidth), details = EditorTheme::Px(DetailsWidth);
        if (ImGui::BeginChild("##left", {tree, 0}, ImGuiChildFlags_Borders))
        {
            DrawPalette();
            ImGui::Separator();
            DrawTree(m_Data.Widgets);
            // Dropping a widget on empty space moves it to the root.
            const ImVec2 rest = ImGui::GetContentRegionAvail();
            ImGui::InvisibleButton("##root", {std::max(rest.x, 1.0f), std::max(rest.y, 1.0f)});
            if (ImGui::IsItemClicked())
                m_Selected = 0;
            if (ImGui::BeginDragDropTarget())
            {
                if (const auto* payload = ImGui::AcceptDragDropPayload("ui_widget"))
                {
                    const uint64_t         id       = *static_cast<const uint64_t*>(payload->Data);
                    std::vector<UIWidget>* siblings = nullptr;
                    if (UIWidget* w = Find(id, &siblings))
                    {
                        UIWidget moved = *w;
                        siblings->erase(siblings->begin() + (w - siblings->data()));
                        m_Data.Widgets.push_back(std::move(moved));
                        Changed();
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##canvas",
                              {ImGui::GetContentRegionAvail().x - details - ImGui::GetStyle().ItemSpacing.x, 0},
                              ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
            DrawCanvas();
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##details", {0, 0}, ImGuiChildFlags_Borders))
            DrawDetails();
        ImGui::EndChild();
    }

    void UIDesignerPanel::DrawToolbar()
    {
        ImGui::BeginDisabled(!m_Dirty);
        if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save"))
            Save();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s", m_File.filename().u8string().c_str(), m_Dirty ? " *" : "");
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        char current[48];
        std::snprintf(current, sizeof(current), "%.0f x %.0f", m_Resolution.x, m_Resolution.y);
        ImGui::SetNextItemWidth(EditorTheme::Px(150.0f));
        if (ImGui::BeginCombo("##resolution", current))
        {
            if (ImGui::Selectable("Reference resolution"))
                SetResolution(m_Data.ReferenceResolution);
            for (const auto& r : Resolutions)
                if (ImGui::Selectable(r.Name, m_Resolution == glm::vec2(r.Width, r.Height)))
                    SetResolution({r.Width, r.Height});
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Preview resolution (the asset's layout adapts to it)");
        ImGui::SameLine();
        ImGui::Checkbox(ICON_FA_MAGNET, &m_Snap);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Snap to whole pixels and to the parent's edges and centre");
        if (!m_Status.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", m_Status.c_str());
        }
    }

    void UIDesignerPanel::DrawPalette()
    {
        ImGui::TextDisabled("Add (inside the selection)");
        const UIWidgetType types[] {UIWidgetType::Panel,
                                    UIWidgetType::Image,
                                    UIWidgetType::Text,
                                    UIWidgetType::Button,
                                    UIWidgetType::Toggle,
                                    UIWidgetType::Slider,
                                    UIWidgetType::ScrollView,
                                    UIWidgetType::InputField,
                                    UIWidgetType::UIInstance};
        const float        width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
        for (size_t i = 0; i < std::size(types); ++i)
        {
            if (i % 3)
                ImGui::SameLine();
            const std::string label = std::string(TypeIcon(types[i])) + "##add" + std::to_string(i);
            if (ImGui::Button(label.c_str(), {width, ImGui::GetFrameHeight() * 1.3f}))
                AddWidget(types[i]);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", TypeName(types[i]));
        }
    }

    void UIDesignerPanel::DrawTree(std::vector<UIWidget>& widgets)
    {
        std::optional<std::pair<uint64_t, int>> move; // Widget and direction among its siblings.
        for (auto& w : widgets)
        {
            ImGui::PushID(reinterpret_cast<const void*>(uintptr_t(w.ID)));
            ImGuiTreeNodeFlags flags =
                ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
            if (w.Children.empty())
                flags |= ImGuiTreeNodeFlags_Leaf;
            if (w.ID == m_Selected)
                flags |= ImGuiTreeNodeFlags_Selected;
            const std::string label = std::string(TypeIcon(w.Type)) + "  " + w.Name;
            if (!w.Visible)
                ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().TextOff);
            const bool open = ImGui::TreeNodeEx("##node", flags, "%s", label.c_str());
            if (!w.Visible)
                ImGui::PopStyleColor();
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                m_Selected = w.ID;
            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload("ui_widget", &w.ID, sizeof(w.ID));
                ImGui::TextUnformatted(label.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget())
            {
                // Dropped widgets become children of this one (never of their own descendants).
                if (const auto* payload = ImGui::AcceptDragDropPayload("ui_widget"))
                {
                    const uint64_t         id       = *static_cast<const uint64_t*>(payload->Data);
                    std::vector<UIWidget>* siblings = nullptr;
                    UIWidget*              dragged  = Find(id, &siblings);
                    if (dragged && !Contains(*dragged, w.ID))
                    {
                        UIWidget moved = *dragged;
                        siblings->erase(siblings->begin() + (dragged - siblings->data()));
                        if (UIWidget* target = Find(w.ID))
                            target->Children.push_back(std::move(moved));
                        Changed();
                        ImGui::EndDragDropTarget();
                        if (open)
                            ImGui::TreePop();
                        ImGui::PopID();
                        return; // The tree changed under the loop.
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem("##menu"))
            {
                m_Selected = w.ID;
                if (ImGui::MenuItem(ICON_FA_ARROW_UP "  Move up"))
                    move = {w.ID, -1};
                if (ImGui::MenuItem(ICON_FA_ARROW_DOWN "  Move down"))
                    move = {w.ID, 1};
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_TRASH "  Delete", "Del"))
                    move = {w.ID, 0};
                ImGui::EndPopup();
            }
            if (open)
            {
                DrawTree(w.Children);
                ImGui::TreePop();
            }
            ImGui::PopID();
            if (move)
                break;
        }
        if (!move)
            return;
        auto [id, direction] = *move;
        if (direction == 0)
        {
            m_Selected = id;
            DeleteSelected();
            return;
        }
        std::vector<UIWidget>* siblings = nullptr;
        if (UIWidget* w = Find(id, &siblings))
        {
            const long index = long(w - siblings->data()), to = index + direction;
            if (to >= 0 && to < long(siblings->size()))
                std::swap((*siblings)[size_t(index)], (*siblings)[size_t(to)]), Changed();
        }
    }

    void UIDesignerPanel::DrawWidget(const UIRuntimeWidget& w, ImVec2 origin, float zoom)
    {
        if (!w.Data.Visible)
            return;
        auto*        draw = ImGui::GetWindowDrawList();
        const ImVec2 a {origin.x + w.Bounds.x * zoom, origin.y + w.Bounds.y * zoom};
        const ImVec2 b {origin.x + w.Bounds.z * zoom, origin.y + w.Bounds.w * zoom};
        const auto&  d       = w.Data;
        const float  opacity = w.Opacity;
        const auto   fill    = UIVec4(d.Properties["Color"], DefaultFill(d.Type));
        if (fill.a > 0)
            draw->AddRectFilled(a, b, Color(fill, opacity), d.Type == UIWidgetType::Button ? 4.0f * zoom : 0.0f);
        const auto& p = EditorTheme::Colors();
        // Each type's schematic content.
        const float scale = std::max(m_Layout ? m_Layout->PixelScale() : 1.0f, 1e-3f);
        const auto  text  = [&](const std::string& value, glm::vec4 color, float size) {
            if (value.empty())
                return;
            const float  px     = std::clamp(size * scale * zoom, 4.0f, 300.0f);
            ImFont*      font   = ImGui::GetFont();
            const auto   h      = UIProperty<std::string>(d, "HorizontalAlignment", "Left");
            const auto   v      = UIProperty<std::string>(d, "VerticalAlignment", "Top");
            const bool   wrap   = UIProperty<bool>(d, "Wrap", true);
            const ImVec2 extent = font->CalcTextSizeA(px, FLT_MAX, wrap ? b.x - a.x : 0.0f, value.c_str());
            ImVec2       at     = a;
            if (h == "Center")
                at.x = (a.x + b.x - extent.x) * 0.5f;
            else if (h == "Right")
                at.x = b.x - extent.x;
            if (v == "Center")
                at.y = (a.y + b.y - extent.y) * 0.5f;
            else if (v == "Bottom")
                at.y = b.y - extent.y;
            const ImVec4 clip {a.x, a.y, b.x, b.y};
            draw->AddText(font, px, at, Color(color, opacity), value.c_str(), nullptr, wrap ? b.x - a.x : 0.0f, &clip);
        };
        switch (d.Type)
        {
            case UIWidgetType::Text:
                text(UIProperty<std::string>(d, "Text", ""),
                     UIVec4(d.Properties["Color"], glm::vec4(1)),
                     UIProperty<float>(d, "Size", 16));
                break;
            case UIWidgetType::Image:
                if (!UIProperty<std::string>(d, "Texture", "").empty())
                    draw->AddText({a.x + 4, a.y + 4}, Color(p.TextDim), ICON_FA_IMAGE);
                break;
            case UIWidgetType::Toggle:
                if (UIProperty<bool>(d, "IsOn", false))
                    draw->AddText({(a.x + b.x) * 0.5f - 6, (a.y + b.y) * 0.5f - 8}, Color(p.Accent), ICON_FA_CHECK);
                break;
            case UIWidgetType::Slider: {
                const float min = UIProperty<float>(d, "Min", 0), max = UIProperty<float>(d, "Max", 1);
                const float t =
                    max > min ? std::clamp((UIProperty<float>(d, "Value", 0) - min) / (max - min), 0.0f, 1.0f) : 0;
                const float y = (a.y + b.y) * 0.5f, x = a.x + (b.x - a.x) * t;
                draw->AddRectFilled({a.x, y - 2 * zoom}, {x, y + 2 * zoom}, Color(p.Accent));
                draw->AddCircleFilled({x, y}, std::max(3.0f, (b.y - a.y) * 0.4f), Color(p.Text));
                break;
            }
            case UIWidgetType::InputField: {
                const auto value = UIProperty<std::string>(d, "Text", "");
                text(value.empty() ? UIProperty<std::string>(d, "Placeholder", "") : value,
                     value.empty() ? glm::vec4(1, 1, 1, .4f) : UIVec4(d.Properties["TextColor"], glm::vec4(1)),
                     UIProperty<float>(d, "Size", 16));
                break;
            }
            case UIWidgetType::UIInstance:
                draw->AddRect(a, b, Color(p.TypeScene, 0.8f), 0, 0, 1.5f);
                draw->AddText({a.x + 4, a.y + 4}, Color(p.TypeScene), ICON_FA_DESKTOP " UI");
                break;
            default:
                break;
        }
        // A faint outline marks every widget's rectangle.
        draw->AddRect(a, b, Color(p.TextDim, 0.25f));
        const bool clip = UIProperty<bool>(d, "ClipChildren", false);
        if (clip)
            draw->PushClipRect(a, b, true);
        for (const auto& child : w.Children)
            DrawWidget(child, origin, zoom);
        if (clip)
            draw->PopClipRect();
    }

    void UIDesignerPanel::DrawCanvas()
    {
        const auto&    p     = EditorTheme::Colors();
        const ImGuiIO& io    = ImGui::GetIO();
        auto*          draw  = ImGui::GetWindowDrawList();
        const ImVec2   avail = ImGui::GetContentRegionAvail(), start = ImGui::GetCursorScreenPos();
        const float  zoom = std::max(0.01f, std::min((avail.x - 24) / m_Resolution.x, (avail.y - 24) / m_Resolution.y));
        const ImVec2 origin {start.x + (avail.x - m_Resolution.x * zoom) * 0.5f,
                             start.y + (avail.y - m_Resolution.y * zoom) * 0.5f};
        const ImVec2 end {origin.x + m_Resolution.x * zoom, origin.y + m_Resolution.y * zoom};
        draw->AddRectFilled(origin, end, Color(p.Inset));
        const auto& layout = Layout();
        for (const auto& w : layout.Widgets())
            DrawWidget(w, origin, zoom);
        draw->AddRect(origin, end, Color(p.TextDim, 0.6f));

        ImGui::SetCursorScreenPos(start);
        ImGui::InvisibleButton("##surface", {std::max(avail.x, 1.0f), std::max(avail.y, 1.0f)});
        const bool      hovered = ImGui::IsItemHovered();
        const glm::vec2 mouse {(io.MousePos.x - origin.x) / zoom, (io.MousePos.y - origin.y) / zoom};
        // The topmost visible widget under the mouse (children draw over their parents).
        m_Hovered                                                     = 0;
        std::function<void(const std::vector<UIRuntimeWidget>&)> pick = [&](const std::vector<UIRuntimeWidget>& list) {
            for (const auto& w : list)
            {
                if (!w.Data.Visible)
                    continue;
                if (mouse.x >= w.Bounds.x && mouse.y >= w.Bounds.y && mouse.x <= w.Bounds.z && mouse.y <= w.Bounds.w)
                    m_Hovered = w.Data.ID;
                pick(w.Children);
            }
        };
        if (hovered)
            pick(layout.Widgets());
        const auto* selected = RuntimeOf(m_Selected);
        const float handle   = EditorTheme::Px(4.0f);
        ImVec2      corner {};
        if (selected)
            corner = {origin.x + selected->Bounds.z * zoom, origin.y + selected->Bounds.w * zoom};
        const bool overCorner = selected && std::abs(io.MousePos.x - corner.x) <= handle * 2 &&
                                std::abs(io.MousePos.y - corner.y) <= handle * 2;
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if (overCorner)
                m_Drag = Drag::Resize;
            else
            {
                m_Selected = m_Hovered;
                m_Drag     = m_Hovered ? Drag::Move : Drag::None;
            }
            m_DragLast = mouse;
            m_DragRest = {0, 0};
        }
        if (overCorner || m_Drag == Drag::Resize)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
        m_Guides.clear();
        if (m_Drag != Drag::None)
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                m_Drag = Drag::None;
            else if (mouse != m_DragLast)
            {
                glm::vec2 raw  = mouse - m_DragLast + m_DragRest;
                glm::vec2 step = m_Snap ? glm::round(raw) : raw;
                m_DragRest     = raw - step;
                m_DragLast     = mouse;
                MoveSelected(step, m_Drag == Drag::Resize);
                // Guides: an edge or the centre near the parent's edges or centre snaps to them.
                if (m_Snap && m_Drag == Drag::Move)
                    if (const auto* rt = RuntimeOf(m_Selected))
                    {
                        const auto parent = ParentBounds(m_Selected);
                        glm::vec2  adjust {0};
                        for (int axis = 0; axis < 2; ++axis)
                        {
                            const float lo = rt->Bounds[axis], hi = rt->Bounds[axis + 2];
                            const float plo = parent[axis], phi = parent[axis + 2];
                            const float mine[] {lo, (lo + hi) * 0.5f, hi}, theirs[] {plo, (plo + phi) * 0.5f, phi};
                            float       best = 6.0f / zoom;
                            for (float m : mine)
                                for (float t : theirs)
                                    if (std::abs(t - m) < best)
                                    {
                                        best                 = std::abs(t - m);
                                        adjust[axis]         = t - m;
                                        const glm::vec4 line = axis == 0 ? glm::vec4(t, parent.y, t, parent.w) :
                                                                           glm::vec4(parent.x, t, parent.z, t);
                                        if (m_Guides.size() <= size_t(axis))
                                            m_Guides.resize(size_t(axis) + 1);
                                        m_Guides[size_t(axis)] = line;
                                    }
                        }
                        if (adjust != glm::vec2(0))
                        {
                            MoveSelected(adjust);
                            m_DragRest -= adjust;
                        }
                    }
            }
        }
        // Keyboard: arrows nudge (Shift: 10 px), Delete removes.
        if (ImGui::IsWindowFocused() && m_Selected)
        {
            const float step = io.KeyShift ? 10.0f : 1.0f;
            glm::vec2   nudge {0};
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
                nudge.x -= step;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
                nudge.x += step;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                nudge.y -= step;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                nudge.y += step;
            if (nudge != glm::vec2(0))
                MoveSelected(nudge);
            if (ImGui::IsKeyPressed(ImGuiKey_Delete))
                DeleteSelected();
        }
        // Hover and selection outlines, the resize handle, the pivot and the anchors.
        if (const auto* hover = RuntimeOf(m_Hovered); hover && m_Hovered != m_Selected)
            draw->AddRect({origin.x + hover->Bounds.x * zoom, origin.y + hover->Bounds.y * zoom},
                          {origin.x + hover->Bounds.z * zoom, origin.y + hover->Bounds.w * zoom},
                          Color(p.Accent, 0.5f));
        if (const auto* sel = RuntimeOf(m_Selected))
        {
            const ImVec2 a {origin.x + sel->Bounds.x * zoom, origin.y + sel->Bounds.y * zoom};
            const ImVec2 b {origin.x + sel->Bounds.z * zoom, origin.y + sel->Bounds.w * zoom};
            draw->AddRect(a, b, Color(p.Accent), 0, 0, 2.0f);
            for (ImVec2 c : {a, ImVec2(b.x, a.y), ImVec2(a.x, b.y), b})
                draw->AddRectFilled({c.x - handle, c.y - handle},
                                    {c.x + handle, c.y + handle},
                                    Color(c.x == b.x && c.y == b.y ? p.Accent : p.Text));
            const auto&  r = sel->Data.Rect;
            const ImVec2 pivot {a.x + (b.x - a.x) * r.Pivot.x, a.y + (b.y - a.y) * r.Pivot.y};
            draw->AddCircle(pivot, handle * 1.2f, Color(p.Warning), 0, 2.0f);
            const auto parent = ParentBounds(m_Selected);
            for (glm::vec2 anchor : {r.AnchorsMin,
                                     r.AnchorsMax,
                                     glm::vec2(r.AnchorsMin.x, r.AnchorsMax.y),
                                     glm::vec2(r.AnchorsMax.x, r.AnchorsMin.y)})
            {
                const ImVec2 at {origin.x + (parent.x + (parent.z - parent.x) * anchor.x) * zoom,
                                 origin.y + (parent.y + (parent.w - parent.y) * anchor.y) * zoom};
                draw->AddTriangleFilled({at.x, at.y},
                                        {at.x - handle, at.y - handle * 2},
                                        {at.x + handle, at.y - handle * 2},
                                        Color(p.Success));
            }
        }
        for (const auto& g : m_Guides)
            if (g != glm::vec4(0))
                draw->AddLine({origin.x + g.x * zoom, origin.y + g.y * zoom},
                              {origin.x + g.z * zoom, origin.y + g.w * zoom},
                              Color(p.Danger),
                              1.0f);
        draw->AddText({start.x + 6, start.y + 4},
                      Color(p.TextDim),
                      (std::to_string(int(m_Resolution.x)) + " x " + std::to_string(int(m_Resolution.y)) + "  " +
                       std::to_string(int(std::lround(zoom * 100))) + "%")
                          .c_str());
    }

    void UIDesignerPanel::DrawAnchorPresets()
    {
        // Rows: top, middle, bottom, stretch; columns: left, centre, right, stretch.
        if (!PropertyGrid::Row("Anchors"))
            return;
        const float     cell = ImGui::GetFrameHeight() * 1.1f;
        const glm::vec2 values[] {{0, 0}, {.5f, .5f}, {1, 1}, {0, 1}};
        UIWidget*       w     = Find(m_Selected);
        const ImVec2    start = ImGui::GetCursorScreenPos(); // Every row starts in the value column.
        for (int row = 0; row < 4; ++row)
        {
            ImGui::SetCursorScreenPos({start.x, start.y + row * (cell + 2)});
            for (int col = 0; col < 4; ++col)
            {
                if (col)
                    ImGui::SameLine(0, 2);
                const glm::vec2 min {values[col].x, values[row].x}, max {values[col].y, values[row].y};
                const bool      current = w && w->Rect.AnchorsMin == min && w->Rect.AnchorsMax == max;
                ImGui::PushID(row * 4 + col);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##preset", current, 0, {cell, cell}))
                    SetAnchors(min, max);
                // A small box showing where the anchors sit (a bar for stretched axes).
                auto*        draw = ImGui::GetWindowDrawList();
                const float  pad = cell * 0.2f, size = cell - 2 * pad;
                const ImVec2 lo {at.x + pad + size * min.x, at.y + pad + size * min.y};
                const ImVec2 hi {at.x + pad + size * max.x, at.y + pad + size * max.y};
                draw->AddRect({at.x + pad, at.y + pad},
                              {at.x + pad + size, at.y + pad + size},
                              Color(EditorTheme::Colors().TextDim, 0.6f));
                draw->AddRectFilled({lo.x - 2, lo.y - 2}, {hi.x + 2, hi.y + 2}, Color(EditorTheme::Colors().Accent));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Anchor min (%.1f, %.1f), max (%.1f, %.1f); the widget stays where it is",
                                      min.x,
                                      min.y,
                                      max.x,
                                      max.y);
                ImGui::PopID();
            }
        }
    }

    bool UIDesignerPanel::AssetCombo(const char* label, AssetType type, std::string& value)
    {
        if (!PropertyGrid::Row(label))
            return false;
        const AssetID id      = AssetID::FromString(value);
        std::string   current = value.empty() ? "(none)" : value;
        for (const auto& choice : m_Choices)
            if (choice.ID == id)
                current = choice.Name;
        bool changed = false;
        ImGui::PushID(label);
        if (ImGui::BeginCombo("##asset", current.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::Selectable("(none)", value.empty()))
                value.clear(), changed = true;
            for (const auto& choice : m_Choices)
                if (choice.Type == type && ImGui::Selectable(choice.Name.c_str(), choice.ID == id))
                    value = choice.ID.ToString(), changed = true;
            ImGui::EndCombo();
        }
        ImGui::PopID();
        return changed;
    }

    void UIDesignerPanel::DrawProperties(UIWidget& w)
    {
        bool       changed = false;
        auto&      props   = w.Properties;
        const auto number =
            [&](const char* name, float fallback, float speed, float min, float max, const char* format = "%.2f") {
                float value = UIProperty<float>(w, name, fallback);
                if (PropertyGrid::Float(name, value, speed, min, max, format))
                    props[name] = value, changed = true;
            };
        const auto integer = [&](const char* name, int fallback, int min, int max) {
            int value = UIProperty<int>(w, name, fallback);
            if (PropertyGrid::Int(name, value, min, max))
                props[name] = value, changed = true;
        };
        const auto flag = [&](const char* name, bool fallback) {
            bool value = UIProperty<bool>(w, name, fallback);
            if (PropertyGrid::Checkbox(name, value))
                props[name] = value, changed = true;
        };
        const auto text = [&](const char* name, bool multiline = false) {
            std::string value = UIProperty<std::string>(w, name, "");
            if (!PropertyGrid::Row(name))
                return;
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool edited = multiline ? ImGui::InputTextMultiline((std::string("##") + name).c_str(),
                                                                      &value,
                                                                      {-FLT_MIN, ImGui::GetTextLineHeight() * 3.5f}) :
                                            ImGui::InputText((std::string("##") + name).c_str(), &value);
            if (edited)
                props[name] = value, changed = true;
        };
        const auto color = [&](const char* name, glm::vec4 fallback) {
            glm::vec4 value = UIVec4(props[name], fallback);
            if (PropertyGrid::Color4(name, value))
                props[name] = UIEncode(value), changed = true;
        };
        const auto choice = [&](const char* name, std::initializer_list<const char*> options) {
            const auto               current = UIProperty<std::string>(w, name, *options.begin());
            std::vector<const char*> items(options);
            int                      index = 0;
            for (int i = 0; i < int(items.size()); ++i)
                if (current == items[size_t(i)])
                    index = i;
            if (PropertyGrid::Combo(name, items.data(), int(items.size()), index))
                props[name] = std::string(items[size_t(index)]), changed = true;
        };
        const auto asset = [&](const char* name, AssetType type) {
            std::string value = UIProperty<std::string>(w, name, "");
            if (AssetCombo(name, type, value))
            {
                if (value.empty())
                    props.remove(name);
                else
                    props[name] = value;
                changed = true;
            }
        };
        const auto textStyle = [&]() {
            asset("Font", AssetType::Font);
            number("Size", 16, 0.5f, 1, 500, "%.0f px");
            choice("HorizontalAlignment", {"Left", "Center", "Right"});
            choice("VerticalAlignment", {"Top", "Center", "Bottom"});
            flag("Wrap", true);
            number("LineSpacing", 1, 0.01f, 0.1f, 5);
            number("LetterSpacing", 0, 0.1f, -50, 50);
        };
        if (PropertyGrid::BeginGroup(TypeName(w.Type)))
        {
            switch (w.Type)
            {
                case UIWidgetType::Panel:
                    color("Color", {0, 0, 0, 0});
                    asset("Texture", AssetType::Texture);
                    break;
                case UIWidgetType::Image:
                    color("Color", {1, 1, 1, 1});
                    asset("Texture", AssetType::Texture);
                    flag("PreserveAspect", false);
                    break;
                case UIWidgetType::Text:
                    text("Text", true);
                    color("Color", {1, 1, 1, 1});
                    textStyle();
                    break;
                case UIWidgetType::Button:
                    color("Color", DefaultFill(w.Type));
                    asset("Texture", AssetType::Texture);
                    flag("Interactable", true);
                    number("TransitionDuration", .1f, 0.01f, 0, 2, "%.2f s");
                    break;
                case UIWidgetType::Toggle:
                    flag("IsOn", false);
                    text("Group");
                    color("Color", DefaultFill(w.Type));
                    flag("Interactable", true);
                    break;
                case UIWidgetType::Slider:
                    number("Min", 0, 0.01f, -1e6f, 1e6f);
                    number("Max", 1, 0.01f, -1e6f, 1e6f);
                    number("Value", 0, 0.01f, -1e6f, 1e6f);
                    flag("WholeNumbers", false);
                    flag("Interactable", true);
                    break;
                case UIWidgetType::ScrollView:
                    text("Viewport");
                    text("Content");
                    flag("Horizontal", false);
                    flag("Vertical", true);
                    number("WheelSpeed", 30, 1, 0, 1000);
                    break;
                case UIWidgetType::InputField:
                    text("Text");
                    text("Placeholder");
                    integer("CharacterLimit", 0, 0, 100000);
                    flag("Multiline", false);
                    flag("Password", false);
                    color("Color", DefaultFill(w.Type));
                    color("TextColor", {1, 1, 1, 1});
                    textStyle();
                    break;
                case UIWidgetType::UIInstance:
                    asset("UI", AssetType::UI);
                    break;
            }
            PropertyGrid::EndGroup();
        }
        // Children placed by a layout instead of their own rectangles.
        if (PropertyGrid::BeginGroup("Layout", nullptr, nullptr, props["Layout"].IsDefined()))
        {
            choice("Layout", {"None", "Horizontal", "Vertical", "Grid"});
            number("Spacing", 0, 0.5f, 0, 1000, "%.0f px");
            flag("FitWidth", false);
            flag("FitHeight", false);
            flag("IgnoreLayout", false);
            PropertyGrid::EndGroup();
        }
        if (PropertyGrid::BeginGroup("Display", nullptr, nullptr, false))
        {
            number("Opacity", 1, 0.01f, 0, 1);
            flag("ClipChildren", false);
            flag("BlockPointer", false);
            PropertyGrid::EndGroup();
        }
        if (changed)
            Changed();
    }

    void UIDesignerPanel::DrawDetails()
    {
        UIWidget* w = Find(m_Selected);
        if (!w)
        {
            // The asset's own settings.
            if (PropertyGrid::BeginGroup("Canvas"))
            {
                bool changed = false;
                if (PropertyGrid::Row("Reference size"))
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    changed |= ImGui::DragFloat2("##reference", &m_Data.ReferenceResolution.x, 1, 16, 16384, "%.0f");
                }
                const char* modes[] {"Constant pixel size", "Scale with screen size"};
                int         mode = int(m_Data.ScaleMode);
                if (PropertyGrid::Combo("Scale mode", modes, 2, mode))
                    m_Data.ScaleMode = UIScaleMode(mode), changed = true;
                if (m_Data.ScaleMode == UIScaleMode::ScaleWithScreenSize)
                    changed |= PropertyGrid::Float("Match width/height", m_Data.Match, 0.01f, 0, 1);
                if (changed)
                    Changed();
                PropertyGrid::EndGroup();
            }
            ImGui::TextDisabled("Select a widget to edit it.");
            return;
        }
        bool changed = false;
        if (PropertyGrid::BeginGroup("Widget"))
        {
            if (PropertyGrid::Row("Name"))
            {
                ImGui::SetNextItemWidth(-FLT_MIN);
                changed |= ImGui::InputText("##name", &w->Name);
            }
            PropertyGrid::Text("Type", TypeName(w->Type), true);
            changed |= PropertyGrid::Checkbox("Visible", w->Visible);
            changed |= PropertyGrid::Checkbox("Enabled", w->Enabled);
            changed |= PropertyGrid::Int("Sort order", w->SortOrder, -1000, 1000);
            PropertyGrid::EndGroup();
        }
        if (PropertyGrid::BeginGroup("Rect"))
        {
            DrawAnchorPresets();
            w               = Find(m_Selected); // Anchoring rewrites the rect.
            auto&      r    = w->Rect;
            const auto pair = [&](const char* label, glm::vec2& value, float speed, float min, float max) {
                if (!PropertyGrid::Row(label))
                    return;
                ImGui::SetNextItemWidth(-FLT_MIN);
                changed |= ImGui::DragFloat2((std::string("##") + label).c_str(), &value.x, speed, min, max, "%.2f");
            };
            const bool stretchX = r.AnchorsMin.x != r.AnchorsMax.x, stretchY = r.AnchorsMin.y != r.AnchorsMax.y;
            if (!stretchX || !stretchY)
            {
                pair("Position", r.Position, 1, -1e5f, 1e5f);
                pair("Size", r.Size, 1, 0, 1e5f);
            }
            if (stretchX || stretchY)
            {
                if (PropertyGrid::Row("Insets", "Left, top, right, bottom (from the stretched anchors)"))
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    changed |= ImGui::DragFloat4("##offsets", &r.Offsets.x, 1, -1e5f, 1e5f, "%.0f");
                }
            }
            pair("Anchor min", r.AnchorsMin, 0.01f, 0, 1);
            pair("Anchor max", r.AnchorsMax, 0.01f, 0, 1);
            pair("Pivot", r.Pivot, 0.01f, 0, 1);
            changed |= PropertyGrid::Float("Rotation", r.Rotation, 0.5f, -360, 360, "%.1f deg");
            pair("Scale", r.Scale, 0.01f, 0, 100);
            PropertyGrid::EndGroup();
        }
        if (changed)
            Changed();
        DrawProperties(*w);
        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_TRASH "  Delete widget", {-FLT_MIN, 0}))
            DeleteSelected();
    }
} // namespace Kans

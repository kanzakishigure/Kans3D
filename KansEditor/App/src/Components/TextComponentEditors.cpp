#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include "Kans3D/Scene/Components.h"
#include "ComponentEditors.h"
#include "UI/PropertyGrid.h"

namespace Kans::ComponentEditors
{
    void DrawText(Entity entity, ComponentUI& ui)
    {
        static const TextComponent defaults;
        entity.PatchComponent<TextComponent>([&](TextComponent& c) {
            if (PropertyGrid::BeginGroup("Text"))
            {
                if (PropertyGrid::Row("Text", "Enter starts a new line"))
                {
                    const float lines  = float(std::count(c.Text.begin(), c.Text.end(), '\n') + 1);
                    const float height = ImGui::GetTextLineHeight() * std::clamp(lines, 2.0f, 8.0f) +
                                         ImGui::GetStyle().FramePadding.y * 2;
                    ImGui::InputTextMultiline("##text", &c.Text, ImVec2(-1, height));
                }
                ui.AssetPicker("Font", AssetType::Font, c.Font);
                if (!c.Font.IsValid())
                    PropertyGrid::Text("", "Engine default: Noto Sans CJK", true);

                // The language picks the CJK region of the font (glyph shapes differ between regions).
                static const char* const languages[] = {"", "zh-Hant", "zh-Hans", "zh-HK", "ja", "ko"};
                static const char* const names[]     = {"Default (Traditional Chinese)",
                                                        "Traditional Chinese",
                                                        "Simplified Chinese",
                                                        "Hong Kong",
                                                        "Japanese",
                                                        "Korean"};
                int                      language    = -1;
                for (int i = 0; i < IM_ARRAYSIZE(languages); ++i)
                    if (c.Language == languages[i])
                        language = i;
                if (language < 0)
                    PropertyGrid::InputText("Language", c.Language); // A tag the list does not know: keep it editable.
                else
                {
                    const int reset = 0;
                    if (PropertyGrid::Combo("Language", names, IM_ARRAYSIZE(names), language, &reset))
                        c.Language = languages[language];
                }
                PropertyGrid::Float("Size", c.Size, 0.005f, 0.001f, 100.0f, "%.3f", &defaults.Size);
                PropertyGrid::Color4("Color", c.Color, &defaults.Color);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Layout"))
            {
                static const char* const horizontal[] = {"Left", "Center", "Right"};
                static const char* const vertical[]   = {"Top", "Middle", "Bottom", "Baseline"};
                static const char* const overflow[]   = {"Overflow", "Wrap", "Ellipsis", "Shrink to Fit"};
                int       h = int(c.Alignment.Horizontal), v = int(c.Alignment.Vertical), o = int(c.Overflow);
                const int hReset = int(defaults.Alignment.Horizontal), vReset = int(defaults.Alignment.Vertical),
                          oReset = int(defaults.Overflow);
                if (PropertyGrid::Combo("Align", horizontal, IM_ARRAYSIZE(horizontal), h, &hReset))
                    c.Alignment.Horizontal = TextHorizontalAlignment(h);
                if (PropertyGrid::Combo("Pivot", vertical, IM_ARRAYSIZE(vertical), v, &vReset))
                    c.Alignment.Vertical = TextPivotVertical(v);
                if (PropertyGrid::Combo("Overflow", overflow, IM_ARRAYSIZE(overflow), o, &oReset))
                    c.Overflow = TextOverflow(o);
                PropertyGrid::Float("Wrap Width",
                                    c.WrapWidth,
                                    0.01f,
                                    0.0f,
                                    1000.0f,
                                    c.WrapWidth > 0 ? "%.3f" : "None",
                                    &defaults.WrapWidth);
                PropertyGrid::Float("Box Height",
                                    c.BoxHeight,
                                    0.01f,
                                    0.0f,
                                    1000.0f,
                                    c.BoxHeight > 0 ? "%.3f" : "None",
                                    &defaults.BoxHeight);
                PropertyGrid::Float("Line Spacing", c.LineSpacing, 0.01f, 0.1f, 10.0f, "%.2fx", &defaults.LineSpacing);
                PropertyGrid::Float(
                    "Letter Spacing", c.LetterSpacing, 0.005f, -1.0f, 1.0f, "%.3f em", &defaults.LetterSpacing);
                PropertyGrid::EndGroup();
            }
            // Effects start closed unless in use.
            if (PropertyGrid::BeginGroup("Outline", nullptr, nullptr, c.Outline.Width > 0))
            {
                PropertyGrid::Float("Width", c.Outline.Width, 0.002f, 0.0f, 0.5f, "%.3f em", &defaults.Outline.Width);
                PropertyGrid::Color4("Color", c.Outline.Color, &defaults.Outline.Color);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Shadow", nullptr, nullptr, c.Shadow.Color.a > 0))
            {
                const bool modified = c.Shadow.Offset != defaults.Shadow.Offset;
                if (PropertyGrid::Row("Offset", "In em, along the text's right and up", modified))
                {
                    ImGui::DragFloat2("##offset", &c.Shadow.Offset.x, 0.002f, -1.0f, 1.0f, "%.3f");
                    if (PropertyGrid::RowReset(modified))
                        c.Shadow.Offset = defaults.Shadow.Offset;
                }
                PropertyGrid::Float(
                    "Softness", c.Shadow.Softness, 0.002f, 0.0f, 0.5f, "%.3f em", &defaults.Shadow.Softness);
                PropertyGrid::Color4("Color", c.Shadow.Color, &defaults.Shadow.Color);
                if (c.Shadow.Color.a <= 0.0f)
                    PropertyGrid::Text("", "Invisible until the colour has alpha", true);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Rendering"))
            {
                static const char* const billboard[] = {"None", "Face Camera", "Upright"};
                int                      b           = int(c.Billboard);
                const int                bReset      = int(defaults.Billboard);
                if (PropertyGrid::Combo("Billboard", billboard, IM_ARRAYSIZE(billboard), b, &bReset))
                    c.Billboard = TextBillboard(b);
                PropertyGrid::Checkbox("Always on Top", c.AlwaysOnTop, &defaults.AlwaysOnTop);
                PropertyGrid::Checkbox("Double Sided", c.DoubleSided, &defaults.DoubleSided);
                PropertyGrid::Checkbox("Cast Shadows", c.CastShadows, &defaults.CastShadows);
                PropertyGrid::EndGroup();
            }
        });
    }
} // namespace Kans::ComponentEditors

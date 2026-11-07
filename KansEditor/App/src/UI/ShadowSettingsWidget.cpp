#include "ShadowSettingsWidget.h"
#include <iterator>
#include <string>
#include <vector>
#include "UI/PropertyGrid.h"

namespace Kans
{
    namespace
    {
        // A combo over fixed choices; `value` keeps its own value when not among them.
        bool ChoiceCombo(const char* label, uint32_t& value, const uint32_t* choices, int count)
        {
            std::vector<std::string> names;
            int                      index = -1;
            for (int i = 0; i < count; ++i)
            {
                names.push_back(std::to_string(choices[i]));
                if (choices[i] == value)
                    index = i;
            }
            std::vector<const char*> items;
            for (const auto& name : names)
                items.push_back(name.c_str());
            if (index < 0)
            {
                items.push_back("(custom)");
                index = count;
            }
            if (!PropertyGrid::Combo(label, items.data(), int(items.size()), index) || index >= count)
                return false;
            value = choices[index];
            return true;
        }
    } // namespace

    bool DrawShadowSettings(ShadowSettings& settings, bool directional)
    {
        const ShadowSettings defaults = directional ? ShadowSettings {} : ShadowSettings::PointDefaults();
        bool                 toggled  = false;
        if (!PropertyGrid::BeginGroup("Shadows", &settings.Enabled, &toggled))
            return toggled;
        bool        changed       = toggled;
        const char* filters[]     = {"PCF 3x3", "PCF 5x5 tent", "PCSS (contact hardening)"};
        int         filter        = int(settings.Filter);
        const int   filterDefault = int(defaults.Filter);
        if (PropertyGrid::Combo("Filter", filters, 3, filter, &filterDefault))
        {
            settings.Filter = ShadowFilter(filter);
            changed         = true;
        }
        constexpr uint32_t resolutions[] = {512, 1024, 2048, 4096};
        changed |= ChoiceCombo("Resolution", settings.Resolution, resolutions, int(std::size(resolutions)));
        if (directional)
        {
            constexpr uint32_t cascades[] = {1, 2, 3, 4};
            changed |= ChoiceCombo("Cascades", settings.Cascades, cascades, int(std::size(cascades)));
            changed |= PropertyGrid::Float("Max distance", settings.MaxDistance, 1.0f, 0.1f, 10000.0f, "%.1f m", &defaults.MaxDistance);
            changed |= PropertyGrid::Float("Split lambda", settings.Lambda, 0.01f, 0.0f, 1.0f, "%.3f", &defaults.Lambda);
            changed |= PropertyGrid::Float("Cascade blend", settings.Blend, 0.01f, 0.0f, 0.5f, "%.3f", &defaults.Blend);
            if (settings.Filter == ShadowFilter::PCSS)
                changed |= PropertyGrid::Float("Sun size", settings.AngularRadius, 0.01f, 0.0f, 10.0f, "%.2f°", &defaults.AngularRadius);
        }
        else if (settings.Filter == ShadowFilter::PCSS)
            changed |= PropertyGrid::Float("Source radius", settings.SourceRadius, 0.005f, 0.0f, 10.0f, "%.3f m", &defaults.SourceRadius);
        changed |= PropertyGrid::Float("Constant bias", settings.ConstantBias, 0.05f, 0.0f, 32.0f, "%.3f", &defaults.ConstantBias);
        changed |= PropertyGrid::Float("Slope bias", settings.SlopeBias, 0.05f, 0.0f, 32.0f, "%.3f", &defaults.SlopeBias);
        changed |= PropertyGrid::Float("Normal offset", settings.NormalOffset, 0.001f, 0.0f, 1.0f, "%.3f m", &defaults.NormalOffset);
        PropertyGrid::EndGroup();
        return changed;
    }
} // namespace Kans

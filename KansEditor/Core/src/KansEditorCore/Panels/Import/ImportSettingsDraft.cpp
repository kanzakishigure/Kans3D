#include "KansEditorCore/Panels/Import/ImportSettingsDraft.h"
#include <algorithm>
#include <set>

namespace Kans
{
    void ImportSettingsDraft::Reset(ImportSettingsSchema schema, std::map<std::string, std::string> saved)
    {
        m_Schema   = std::move(schema);
        m_Saved    = std::move(saved);
        m_Settings = m_Saved;
    }

    std::string ImportSettingsDraft::Default(const std::string& key) const
    {
        const auto* setting = m_Schema.Find(key);
        return setting ? setting->Default : std::string();
    }

    std::string ImportSettingsDraft::Effective(const std::map<std::string, std::string>& settings,
                                               const std::string&                        key) const
    {
        const auto found = settings.find(key);
        return found != settings.end() ? found->second : Default(key);
    }

    std::string ImportSettingsDraft::Get(const std::string& key) const { return Effective(m_Settings, key); }

    void ImportSettingsDraft::Set(const std::string& key, const std::string& value)
    {
        if (value.empty() || value == Default(key))
            m_Settings.erase(key);
        else
            m_Settings[key] = value;
    }

    bool ImportSettingsDraft::IsDefault(const std::string& key) const { return Get(key) == Default(key); }

    void ImportSettingsDraft::ApplyPreset(const ImportPreset& preset)
    {
        for (const auto& [key, value] : preset.Settings)
            Set(key, value);
    }

    const ImportPreset* ImportSettingsDraft::MatchingPreset() const
    {
        for (const auto& preset : m_Schema.Presets)
            if (std::all_of(preset.Settings.begin(), preset.Settings.end(), [&](const auto& setting) {
                    return Get(setting.first) == setting.second;
                }))
                return &preset;
        return nullptr;
    }

    std::vector<std::string> ImportSettingsDraft::Changes() const
    {
        std::set<std::string> keys;
        for (const auto& [key, value] : m_Saved)
            keys.insert(key);
        for (const auto& [key, value] : m_Settings)
            keys.insert(key);
        std::vector<std::string> changed;
        for (const auto& key : keys)
            if (Effective(m_Saved, key) != Effective(m_Settings, key))
                changed.push_back(key);
        return changed;
    }

    std::vector<std::string> ImportSettingsDraft::Groups() const
    {
        std::vector<std::string> groups;
        for (const auto& setting : m_Schema.Settings)
            if (!setting.KeyPattern && std::find(groups.begin(), groups.end(), setting.Group) == groups.end())
                groups.push_back(setting.Group);
        return groups;
    }
} // namespace Kans

#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "Kans3D/Asset/Importer/ImportSettingsSchema.h"

namespace Kans
{
    // The import settings being edited for one source: the saved settings plus
    // unsaved changes, read through the importer's schema so absent keys show
    // their defaults. A value equal to its default is stored as an absent key.
    class ImportSettingsDraft
    {
    public:
        void Reset(ImportSettingsSchema schema, std::map<std::string, std::string> saved);

        const ImportSettingsSchema&               Schema() const { return m_Schema; }
        const std::map<std::string, std::string>& Settings() const { return m_Settings; }
        const std::map<std::string, std::string>& Saved() const { return m_Saved; }

        // The value of `key`, or its schema default (empty when the schema does not know it).
        std::string Get(const std::string& key) const;
        bool        GetBool(const std::string& key) const { return Get(key) == "true"; }
        // Stores `value`; the default (or an empty value) removes the key.
        void Set(const std::string& key, const std::string& value);
        void SetBool(const std::string& key, bool value) { Set(key, value ? "true" : "false"); }
        bool IsDefault(const std::string& key) const;

        // Sets the preset's keys and leaves the others.
        void ApplyPreset(const ImportPreset& preset);
        // The first preset whose keys all hold their preset values, if any.
        const ImportPreset* MatchingPreset() const;

        // Keys whose effective value differs from the saved settings.
        std::vector<std::string> Changes() const;
        bool                     IsDirty() const { return !Changes().empty(); }
        void                     Revert() { m_Settings = m_Saved; }
        // Raw edits (the developer section): any key, kept as typed.
        void SetRaw(const std::string& key, const std::string& value) { m_Settings[key] = value; }
        void EraseRaw(const std::string& key) { m_Settings.erase(key); }

        // The groups of the schema's fixed (non-pattern) settings, in first-use order.
        std::vector<std::string> Groups() const;

    private:
        std::string Default(const std::string& key) const;
        std::string Effective(const std::map<std::string, std::string>& settings, const std::string& key) const;

        ImportSettingsSchema               m_Schema;
        std::map<std::string, std::string> m_Saved, m_Settings;
    };
} // namespace Kans

#include "MaterialEditorPanel.h"
#include <fstream>
#include <set>
#include <imgui.h>
#include "Kans3D/Asset/Importer/ImporterRegistry.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "Kans3D/Asset/Runtime/StandardShader.h"
#include "UI/EditorIcons.h"

namespace Kans
{
    namespace
    {
        bool LooksLikeColor(const std::string& name)
        {
            std::string lower(name);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            for (const char* word : {"color", "colour", "tint", "albedo"})
                if (lower.find(word) != std::string::npos)
                    return true;
            return false;
        }

        std::optional<AssetError> WriteText(const std::filesystem::path& file, const std::string& text)
        {
            std::ofstream output(file, std::ios::binary | std::ios::trunc);
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            output.close();
            if (!output)
                return AssetError {AssetErrorCode::IoError, "Cannot write " + file.u8string()};
            return std::nullopt;
        }

        std::filesystem::path Canonical(const std::filesystem::path& path)
        {
            std::error_code error;
            auto            result = std::filesystem::weakly_canonical(path, error);
            return error ? path.lexically_normal() : result;
        }

        MaterialSource DeclaredValues(MaterialSource source, const MaterialSchema& schema)
        {
            for (auto it = source.Parameters.begin(); it != source.Parameters.end();)
                it = std::any_of(schema.Parameters.begin(),
                                 schema.Parameters.end(),
                                 [&](const auto& p) { return p.Name == it->first; }) ?
                         std::next(it) :
                         source.Parameters.erase(it);
            for (auto it = source.Textures.begin(); it != source.Textures.end();)
                it = std::find(schema.Textures.begin(), schema.Textures.end(), it->first) != schema.Textures.end() ?
                         std::next(it) :
                         source.Textures.erase(it);
            return source;
        }
    } // namespace

    void MaterialEditorPanel::Open(const std::filesystem::path& file)
    {
        m_Dirty  = false;
        m_File   = file;
        m_Source = {};
        m_Status.clear();
        m_Building = false;
        m_PendingTexture.reset();
        m_Thumbnails.clear();
        m_PreviewKey.clear();
        m_PreviewImage.reset();
        m_PreviewTexture  = 0;
        m_PreviewComplete = false;
        m_PreviewError.clear();
        auto bytes = ReadProductFile(file, 1024 * 1024);
        if (const auto* error = std::get_if<AssetError>(&bytes))
        {
            m_Status = error->Message;
            return;
        }
        const auto& data   = std::get<std::vector<std::byte>>(bytes);
        auto        parsed = ParseMaterialSource(data.data(), data.size());
        if (const auto* error = std::get_if<AssetError>(&parsed))
            m_Status = error->Message + " (saving replaces the file)";
        else
            m_Source = std::get<MaterialSource>(std::move(parsed));
        ResetHistory();
    }

    void MaterialEditorPanel::ResetHistory()
    {
        m_UndoStack.clear();
        m_RedoStack.clear();
        m_CommittedText = SerializeMaterialSource(m_Source);
        m_SavedText     = m_CommittedText;
        m_Dirty         = false;
    }

    void MaterialEditorPanel::TrackEdits()
    {
        if (ImGui::IsAnyItemActive())
            return;
        const auto text = SerializeMaterialSource(m_Source);
        if (text != m_CommittedText)
        {
            m_UndoStack.push_back(std::move(m_CommittedText));
            if (m_UndoStack.size() > MaxHistory)
                m_UndoStack.erase(m_UndoStack.begin());
            m_RedoStack.clear();
            m_CommittedText = text;
        }
        m_Dirty = m_CommittedText != m_SavedText;
    }

    void MaterialEditorPanel::Undo()
    {
        TrackEdits(); // A pending edit becomes a step first.
        if (m_UndoStack.empty())
            return;
        m_RedoStack.push_back(std::move(m_CommittedText));
        m_CommittedText = std::move(m_UndoStack.back());
        m_UndoStack.pop_back();
        auto parsed = ParseMaterialSource(reinterpret_cast<const std::byte*>(m_CommittedText.data()),
                                          m_CommittedText.size());
        if (auto* source = std::get_if<MaterialSource>(&parsed))
            m_Source = std::move(*source);
        m_Dirty = m_CommittedText != m_SavedText;
    }

    void MaterialEditorPanel::Redo()
    {
        TrackEdits();
        if (m_RedoStack.empty())
            return;
        m_UndoStack.push_back(std::move(m_CommittedText));
        m_CommittedText = std::move(m_RedoStack.back());
        m_RedoStack.pop_back();
        auto parsed = ParseMaterialSource(reinterpret_cast<const std::byte*>(m_CommittedText.data()),
                                          m_CommittedText.size());
        if (auto* source = std::get_if<MaterialSource>(&parsed))
            m_Source = std::move(*source);
        m_Dirty = m_CommittedText != m_SavedText;
    }

    void MaterialEditorPanel::Create(const std::filesystem::path& directory)
    {
        std::filesystem::path file;
        for (int index = 0; index < 1000; ++index)
        {
            file = directory / ("NewMaterial" + (index ? std::to_string(index) : std::string()) + ".material");
            std::error_code error;
            if (!std::filesystem::exists(file, error))
                break;
        }
        MaterialSource source;
        source.Name = file.stem().u8string();
        if (auto error = WriteText(file, SerializeMaterialSource(source)))
        {
            m_Status = error->Message;
            return;
        }
        if (m_Service)
            (void)m_Service->RefreshSources();
        Open(file);
    }

    std::string MaterialEditorPanel::ChoiceName(AssetID id) const
    {
        if (id == SixWaySmokeShaderID())
            return "Kans3D/SixWaySmoke";
        for (const auto& choice : m_Choices)
            if (choice.ID == id)
                return choice.Name;
        return id.ToString();
    }

    const MaterialSchema* MaterialEditorPanel::Schema()
    {
        if (IsInstance() && !ParentEffective())
        {
            m_SchemaError = m_ParentError.empty() ? "Loading parent material..." : m_ParentError;
            return nullptr;
        }
        const AssetID shaderID = IsInstance() ? ParentEffective()->Shader : m_Source.Shader;
        if (IsStandardShader(shaderID))
        {
            if (!m_Schema || m_SchemaShader)
            {
                m_SchemaShader.reset();
                m_Schema = std::get<MaterialSchema>(DescribeMaterial(nullptr));
            }
            return &*m_Schema;
        }
        if (!m_Assets)
        {
            m_SchemaError = "Assets are not loaded yet.";
            return nullptr;
        }
        auto status = m_Assets->Request<ShaderDataAsset>(shaderID);
        if (status.Error)
        {
            m_SchemaError = status.Error->Message;
            return nullptr;
        }
        if (!status.IsReady())
        {
            m_SchemaError = "Loading shader...";
            return nullptr;
        }
        if (status.Asset != m_SchemaShader || !m_Schema)
        {
            m_SchemaShader = status.Asset;
            auto described = DescribeMaterial(&status.Asset->GetData());
            if (auto* error = std::get_if<AssetError>(&described))
            {
                m_Schema.reset();
                m_SchemaError = error->Message;
                return nullptr;
            }
            m_Schema = std::get<MaterialSchema>(std::move(described));
        }
        return m_Schema ? &*m_Schema : nullptr;
    }

    void MaterialEditorPanel::DrawParameter(const MaterialParameterInfo& parameter, const MaterialSchema& schema)
    {
        std::vector<double> values;
        const bool          instance   = IsInstance();
        const bool          overridden = m_Source.Parameters.count(parameter.Name) > 0;
        const auto*         parent     = instance ? ParentEffective() : nullptr;
        if (const auto found = m_Source.Parameters.find(parameter.Name); found != m_Source.Parameters.end())
            values = found->second;
        else if (parent && parent->Parameters.count(parameter.Name))
            values = parent->Parameters.at(parameter.Name);
        else if (const auto fallback = schema.Defaults.find(parameter.Name); fallback != schema.Defaults.end())
            values = fallback->second;
        values.resize(parameter.ValueCount(), 0.0);

        using Hint            = ShaderParameterHint::Kind;
        const auto hint       = parameter.Hint ? std::optional<Hint>(parameter.Hint->Type) : std::nullopt;
        const bool toggle     = hint == Hint::Toggle || (!hint && parameter.Type == ShaderUniformType::Boolean);
        const bool floating   = parameter.Type == ShaderUniformType::Float;
        const auto chunks     = parameter.ArrayCount * parameter.Columns;
        const int  width      = static_cast<int>(parameter.Components);
        // Without a hint, color-named vec3/vec4 floats get a color picker.
        const bool color = chunks == 1 && floating && width >= 3 &&
                           (hint == Hint::Color || (!hint && LooksLikeColor(parameter.Name)));

        bool changed = false;
        ImGui::PushID(parameter.Name.c_str());
        if (instance)
        {
            if (OverrideMarker(overridden))
            {
                m_Source.Parameters.erase(parameter.Name);
                m_Dirty = true;
                ImGui::PopID();
                return;
            }
            ImGui::SameLine();
            ImGui::BeginGroup();
        }
        // Inherited values are dimmed; editing one makes it an override.
        if (instance && !overridden)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.6f);
        for (uint32_t chunk = 0; chunk < chunks; ++chunk)
        {
            std::string label = parameter.Name;
            if (parameter.ArrayCount > 1)
                label += "[" + std::to_string(chunk / parameter.Columns) + "]";
            if (parameter.Columns > 1)
                label += " col " + std::to_string(chunk % parameter.Columns);
            double* slice = values.data() + size_t(chunk) * width;
            ImGui::PushID(static_cast<int>(chunk));
            if (toggle)
            {
                for (int i = 0; i < width; ++i)
                {
                    bool flag = slice[i] != 0.0;
                    ImGui::PushID(i);
                    if (ImGui::Checkbox(i + 1 == width ? label.c_str() : "##flag", &flag))
                    {
                        slice[i] = flag ? 1.0 : 0.0;
                        changed  = true;
                    }
                    ImGui::PopID();
                    if (i + 1 != width)
                        ImGui::SameLine();
                }
            }
            else if (floating)
            {
                float f[4] = {};
                for (int i = 0; i < width; ++i)
                    f[i] = static_cast<float>(slice[i]);
                bool edited = false;
                if (color)
                    edited = width == 4 ? ImGui::ColorEdit4(label.c_str(), f, ImGuiColorEditFlags_Float) :
                                          ImGui::ColorEdit3(label.c_str(), f, ImGuiColorEditFlags_Float);
                else if (hint == Hint::Range)
                {
                    const float min = float(parameter.Hint->Min), max = float(parameter.Hint->Max);
                    edited = ImGui::SliderScalarN(label.c_str(), ImGuiDataType_Float, f, width, &min, &max);
                }
                else
                    edited = ImGui::DragScalarN(label.c_str(), ImGuiDataType_Float, f, width, 0.01f);
                if (edited)
                    for (int i = 0; i < width; ++i)
                        slice[i] = f[i];
                changed |= edited;
            }
            else
            {
                const bool signedValue = parameter.Type == ShaderUniformType::Signed;
                int32_t    s[4]        = {};
                uint32_t   u[4]        = {};
                for (int i = 0; i < width; ++i)
                {
                    s[i] = static_cast<int32_t>(slice[i]);
                    u[i] = static_cast<uint32_t>(std::max(0.0, slice[i]));
                }
                void*            data = signedValue ? static_cast<void*>(s) : static_cast<void*>(u);
                const ImGuiDataType type = signedValue ? ImGuiDataType_S32 : ImGuiDataType_U32;
                bool                edited;
                if (hint == Hint::Range)
                {
                    const int32_t  smin = int32_t(parameter.Hint->Min), smax = int32_t(parameter.Hint->Max);
                    const uint32_t umin = uint32_t(std::max(0.0, parameter.Hint->Min)),
                                   umax = uint32_t(std::max(0.0, parameter.Hint->Max));
                    edited = ImGui::SliderScalarN(label.c_str(),
                                                  type,
                                                  data,
                                                  width,
                                                  signedValue ? static_cast<const void*>(&smin) : &umin,
                                                  signedValue ? static_cast<const void*>(&smax) : &umax);
                }
                else
                    edited = ImGui::DragScalarN(label.c_str(), type, data, width, 0.1f);
                if (edited)
                    for (int i = 0; i < width; ++i)
                        slice[i] = signedValue ? double(s[i]) : double(u[i]);
                changed |= edited;
            }
            ImGui::PopID();
        }
        if (instance && !overridden)
            ImGui::PopStyleVar();
        if (instance)
            ImGui::EndGroup();
        ImGui::PopID();
        if (changed)
        {
            m_Source.Parameters[parameter.Name] = std::move(values);
            m_Dirty                             = true;
        }
    }

    void MaterialEditorPanel::SetTexture(const std::string& slot, AssetID texture)
    {
        auto found = m_Source.Textures.find(slot);
        if (texture.IsValid() ? found != m_Source.Textures.end() && found->second == texture :
                                found == m_Source.Textures.end())
            return;
        if (texture.IsValid() || IsInstance())
            m_Source.Textures[slot] = texture; // An instance's empty entry clears the inherited texture.
        else
            m_Source.Textures.erase(slot);
        m_Dirty = true;
    }

    void MaterialEditorPanel::DrawThumbnail(AssetID texture, float size)
    {
        Thumbnail* thumbnail = nullptr;
        if (texture.IsValid() && m_Assets)
        {
            auto  status = m_Assets->Request<TextureAsset>(texture);
            auto& entry  = m_Thumbnails[texture];
            if (status.IsReady() && status.Asset != entry.Source)
            {
                entry.Source  = status.Asset;
                auto uploaded = Texture2D::CreateFromProduct(status.Asset->GetData());
                entry.Texture =
                    std::holds_alternative<Ref<Texture2D>>(uploaded) ? std::get<Ref<Texture2D>>(uploaded) : nullptr;
            }
            if (entry.Texture)
                thumbnail = &entry;
        }
        if (!thumbnail)
        {
            ImGui::Button(texture.IsValid() ? "..." : "None", {size, size});
            return;
        }
        // Uploads store the image bottom row first (OpenGL convention).
        const ImVec2 uv0(0, 1), uv1(1, 0);
        const auto   id = (ImTextureID)(uintptr_t)thumbnail->Texture->GetImGuiTextureID();
        ImGui::Image(id, {size, size}, uv0, uv1);
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            const float width  = float(thumbnail->Texture->GetWidth());
            const float height = float(thumbnail->Texture->GetHeight());
            const float scale  = std::min(256.0f / std::max(width, height), 1.0f);
            ImGui::Image(id, {width * scale, height * scale}, uv0, uv1);
            ImGui::Text("%s", ChoiceName(texture).c_str());
            ImGui::TextDisabled("%u x %u", thumbnail->Texture->GetWidth(), thumbnail->Texture->GetHeight());
            ImGui::EndTooltip();
        }
    }

    void MaterialEditorPanel::DrawTexture(const std::string& name)
    {
        constexpr float Size       = 48.0f;
        const auto      found      = m_Source.Textures.find(name);
        const bool      instance   = IsInstance();
        const bool      overridden = found != m_Source.Textures.end();
        AssetID         current    = overridden ? found->second : AssetID {};
        if (instance && !overridden)
            if (const auto* parent = ParentEffective())
                if (const auto inherited = parent->Textures.find(name); inherited != parent->Textures.end())
                    current = inherited->second;

        ImGui::PushID(name.c_str());
        if (instance)
        {
            if (OverrideMarker(overridden))
            {
                m_Source.Textures.erase(name);
                m_Dirty = true;
                ImGui::PopID();
                return;
            }
            ImGui::SameLine();
        }
        if (instance && !overridden)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.6f);
        ImGui::BeginGroup();
        DrawThumbnail(current, Size);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(name.c_str());
        const bool pending = m_PendingTexture && m_PendingTexture->first == name;
        const std::string preview =
            pending ? "Importing..." : current.IsValid() ? ChoiceName(current) : std::string("(none)");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##texture", preview.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::IsWindowAppearing())
            {
                m_TextureFilter.clear();
                ImGui::SetKeyboardFocusHere();
            }
            char filter[256] = {};
            m_TextureFilter.copy(filter, sizeof(filter) - 1);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputTextWithHint("##filter", "Search", filter, sizeof(filter)))
                m_TextureFilter = filter;
            std::string needle(m_TextureFilter);
            std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            std::vector<const EditorAssetChoice*> matches;
            for (const auto& choice : m_Choices)
            {
                if (choice.Type != AssetType::Texture)
                    continue;
                std::string haystack(choice.Name);
                std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (haystack.find(needle) != std::string::npos)
                    matches.push_back(&choice);
            }
            if (ImGui::Selectable("(none)", !current.IsValid()))
                SetTexture(name, {});
            // Only visible rows load thumbnails.
            constexpr float  Row = 28.0f;
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(matches.size()), Row + ImGui::GetStyle().ItemSpacing.y);
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const auto& choice = *matches[i];
                    ImGui::PushID(i);
                    const float start = ImGui::GetCursorPosX();
                    if (ImGui::Selectable(
                            "##choice", choice.ID == current, ImGuiSelectableFlags_AllowOverlap, {0, Row}))
                        SetTexture(name, choice.ID);
                    // Thumbnail and name are drawn over the selectable row.
                    ImGui::SameLine(start);
                    DrawThumbnail(choice.ID, Row);
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(choice.Name.c_str());
                    ImGui::PopID();
                }
            if (matches.empty())
                ImGui::TextDisabled("No imported textures match.");
            ImGui::EndCombo();
        }
        ImGui::EndGroup();
        ImGui::EndGroup();
        if (instance && !overridden)
            ImGui::PopStyleVar();

        // Drop an image from the content browser on the row.
        if (ImGui::BeginDragDropTarget())
        {
            if (const auto* payload = ImGui::AcceptDragDropPayload("asset_payload"))
                AssignDroppedFile(name,
                                  std::filesystem::u8path(std::string(
                                      static_cast<const char*>(payload->Data), size_t(payload->DataSize))));
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }

    void MaterialEditorPanel::AssignDroppedFile(const std::string& slot, const std::filesystem::path& file)
    {
        const auto dropped = Canonical(file);
        // Already imported: use its Texture Product.
        for (const auto& choice : m_Choices)
            if (choice.Type == AssetType::Texture && choice.SourcePath == dropped)
            {
                SetTexture(slot, choice.ID);
                return;
            }
        if (!m_Service)
            return;
        const auto findSource = [&]() -> std::optional<SourceAssetMetadata> {
            auto sources = m_Service->GetSources();
            if (!std::holds_alternative<std::vector<SourceAssetMetadata>>(sources))
                return std::nullopt;
            const auto& root = m_Service->GetConfig().AssetDirectory;
            for (const auto& source : std::get<std::vector<SourceAssetMetadata>>(sources))
                if (source.Exists && Canonical(root / source.Path.GetPath()) == dropped)
                    return source;
            return std::nullopt;
        };
        auto source = findSource();
        if (!source)
        {
            (void)m_Service->RefreshSources();
            source = findSource();
        }
        if (!source)
        {
            m_Status = file.filename().u8string() + " is not a project asset.";
            return;
        }
        const auto registry = CreateDefaultImporterRegistry();
        const auto importer = std::get<ImporterRegistry>(registry).Resolve(file, source->Import.Importer);
        if (std::holds_alternative<AssetError>(importer) ||
            std::get<RegisteredImporter>(importer).Name != "TextureImporter")
        {
            m_Status = file.filename().u8string() + " is not a texture.";
            return;
        }
        AssetBuildOptions options;
        options.Target = EditorAssetTarget;
        auto started   = m_Service->Start(source->sourceAssetID, options);
        if (const auto* error = std::get_if<AssetError>(&started))
        {
            m_Status = "Cannot import " + file.filename().u8string() + ": " + error->Message;
            return;
        }
        m_PendingTexture = std::make_pair(slot, source->sourceAssetID);
        m_Status         = "Importing " + file.filename().u8string() + "...";
    }

    void MaterialEditorPanel::FinishPendingTexture()
    {
        if (!m_PendingTexture || !m_Service)
            return;
        auto snapshot = m_Service->GetSnapshot();
        const auto* status = std::get_if<EditorAssetImportSnapshot>(&snapshot);
        if (!status || status->State == EditorAssetImportState::Building)
            return;
        const auto [slot, source] = *m_PendingTexture;
        // The editor refreshes the asset choices when the build completes.
        for (const auto& choice : m_Choices)
            if (choice.Type == AssetType::Texture && choice.ID.GetSourceID() == source)
            {
                SetTexture(slot, choice.ID);
                m_PendingTexture.reset();
                m_Status = "Texture imported.";
                return;
            }
        if (status->Source == source && status->State != EditorAssetImportState::Succeeded)
        {
            m_PendingTexture.reset();
            m_Status = status->Error ? "Texture import failed: " + status->Error->Message :
                                       std::string("Texture import did not finish.");
        }
    }

    void MaterialEditorPanel::Save()
    {
        // Values the current Shader does not declare would fail the import.
        if (const auto* schema = Schema())
        {
            m_Source = DeclaredValues(m_Source, *schema);
            TrackEdits();
        }
        if (auto error = WriteText(m_File, SerializeMaterialSource(m_Source)))
        {
            m_Status = error->Message;
            return;
        }
        m_SavedText = SerializeMaterialSource(m_Source);
        m_Dirty     = m_SavedText != m_CommittedText;
        if (!m_Service)
            return;
        const auto findSource = [&]() -> std::optional<SourceAssetID> {
            auto sources = m_Service->GetSources();
            if (!std::holds_alternative<std::vector<SourceAssetMetadata>>(sources))
                return std::nullopt;
            const auto  file = Canonical(m_File);
            const auto& root = m_Service->GetConfig().AssetDirectory;
            for (const auto& source : std::get<std::vector<SourceAssetMetadata>>(sources))
                if (source.Exists && Canonical(root / source.Path.GetPath()) == file)
                    return source.sourceAssetID;
            return std::nullopt;
        };
        auto source = findSource();
        if (!source)
        {
            (void)m_Service->RefreshSources();
            source = findSource();
        }
        if (!source)
        {
            m_Status = "Saved, but the file is not a project asset.";
            return;
        }
        AssetBuildOptions options;
        options.Target            = EditorAssetTarget;
        options.RebuildDependents = true; // Instances of this material update too.
        auto started              = m_Service->Start(*source, options);
        if (const auto* error = std::get_if<AssetError>(&started))
            m_Status = "Saved; build not started: " + error->Message;
        else
        {
            m_Status   = "Building...";
            m_Building = true;
        }
    }

    bool MaterialEditorPanel::OverrideMarker(bool overridden)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              overridden ? ImVec4(0.35f, 0.7f, 1.0f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const bool clicked = ImGui::SmallButton(overridden ? ICON_FA_CIRCLE "##override" : ICON_FA_CIRCLE_DOT "##override") &&
                             overridden;
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(overridden ? "Overridden. Click to use the parent's value." :
                                           "Inherited from the parent. Edit to override.");
        return clicked;
    }

    const MaterialSource* MaterialEditorPanel::ParentEffective()
    {
        if (!IsInstance() || !m_Assets)
            return nullptr;
        if (m_ParentFor != m_Source.Parent)
        {
            m_ParentFor = m_Source.Parent;
            m_ParentAsset.reset();
            m_ParentShaderAsset.reset();
            m_ParentEffective.reset();
            m_ParentError.clear();
        }
        const auto material = m_Assets->Request<MaterialDataAsset>(m_Source.Parent);
        if (material.Error)
        {
            m_ParentError = "Parent material: " + material.Error->Message;
            m_ParentEffective.reset();
            return nullptr;
        }
        if (!material.IsReady())
            return m_ParentEffective ? &*m_ParentEffective : nullptr; // Keep showing the last values while it reloads.
        const auto&                data   = material.Asset->GetData();
        const ShaderData*          shader = &GetStandardShader();
        Ref<const ShaderDataAsset> shaderAsset;
        if (!IsStandardShader(data.Shader))
        {
            const auto status = m_Assets->Request<ShaderDataAsset>(data.Shader);
            if (status.Error)
            {
                m_ParentError = "Parent shader: " + status.Error->Message;
                m_ParentEffective.reset();
                return nullptr;
            }
            if (!status.IsReady())
                return m_ParentEffective ? &*m_ParentEffective : nullptr;
            shaderAsset = status.Asset;
            shader      = &status.Asset->GetData();
        }
        if (material.Asset != m_ParentAsset || shaderAsset != m_ParentShaderAsset || !m_ParentEffective)
        {
            m_ParentAsset       = material.Asset;
            m_ParentShaderAsset = shaderAsset;
            auto effective      = EditableMaterial(data, *shader);
            if (const auto* error = std::get_if<AssetError>(&effective))
            {
                m_ParentError = "Parent material: " + error->Message;
                m_ParentEffective.reset();
            }
            else
            {
                m_ParentEffective = std::get<MaterialSource>(std::move(effective));
                m_ParentError.clear();
            }
        }
        return m_ParentEffective ? &*m_ParentEffective : nullptr;
    }

    void MaterialEditorPanel::DrawParent()
    {
        const std::string current = IsInstance() ? ChoiceName(m_Source.Parent) : std::string("None");
        if (ImGui::BeginCombo("Parent", current.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::Selectable("None", !IsInstance()) && IsInstance())
            {
                // Becoming standalone keeps the current effective values.
                if (const auto* parent = ParentEffective())
                {
                    const ShaderData* shader = m_SchemaShader && !IsStandardShader(parent->Shader) ?
                                                   &m_SchemaShader->GetData() :
                                                   nullptr;
                    auto effective = ApplyMaterialOverrides(*parent, m_Source, shader);
                    if (auto* source = std::get_if<MaterialSource>(&effective))
                    {
                        source->Name = m_Source.Name;
                        m_Source     = std::move(*source);
                        m_Dirty      = true;
                    }
                    else
                        m_Status = std::get<AssetError>(effective).Message;
                }
                else
                    m_Status = "The parent is not loaded; cannot keep its values.";
            }
            for (const auto& choice : m_Choices)
            {
                if (choice.Type != AssetType::Material || choice.ID == m_Source.Parent)
                    continue;
                if (ImGui::Selectable(choice.Name.c_str(), false))
                {
                    // An instance stores only overrides: start from none.
                    m_Source.Parent = choice.ID;
                    m_Source.Shader = {};
                    m_Source.Parameters.clear();
                    m_Source.Textures.clear();
                    m_Source.RenderOverrides = {};
                    m_Dirty                  = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(IsInstance() ? "Values not overridden here come from this material." :
                                             "Choose a material to make this an instance of it.");
        if (IsInstance() && !m_ParentError.empty())
            ImGui::TextColored({0.95f, 0.38f, 0.35f, 1.0f}, "%s", m_ParentError.c_str());
    }

    void MaterialEditorPanel::DrawParameters(const MaterialSchema& schema)
    {
        if (schema.Parameters.empty() && schema.Textures.empty())
        {
            ImGui::TextDisabled("This shader has no material parameters.");
            return;
        }
        const auto drawMember = [&](const std::string& name) {
            for (const auto& parameter : schema.Parameters)
                if (parameter.Name == name)
                {
                    DrawParameter(parameter, schema);
                    return;
                }
            if (std::find(schema.Textures.begin(), schema.Textures.end(), name) != schema.Textures.end())
                DrawTexture(name);
        };
        std::set<std::string> grouped;
        for (const auto& group : schema.Groups)
        {
            for (const auto& member : group.Members)
                grouped.insert(member);
            ImGui::PushID(group.Name.c_str());
            if (ImGui::CollapsingHeader(group.Name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                for (const auto& member : group.Members)
                    drawMember(member);
            ImGui::PopID();
        }
        // Whatever the Shader does not group: all of it for shaders without groups.
        std::vector<std::string> rest;
        for (const auto& parameter : schema.Parameters)
            if (!grouped.count(parameter.Name))
                rest.push_back(parameter.Name);
        const size_t parameters = rest.size();
        for (const auto& texture : schema.Textures)
            if (!grouped.count(texture))
                rest.push_back(texture);
        if (rest.empty())
            return;
        if (!schema.Groups.empty() && !ImGui::CollapsingHeader("Other", ImGuiTreeNodeFlags_DefaultOpen))
            return;
        for (size_t i = 0; i < rest.size(); ++i)
        {
            if (i == parameters && schema.Groups.empty())
            {
                ImGui::Spacing();
                ImGui::TextDisabled("Textures");
            }
            drawMember(rest[i]);
        }
    }

    void MaterialEditorPanel::DrawPreview()
    {
        if (!m_Preview)
            return;
        const auto* schema = Schema();
        if (!schema)
            m_PreviewComplete = false;
        const float side = std::floor(std::clamp(ImGui::GetContentRegionAvail().x, 64.0f, 256.0f));
        // Re-render when the edited source, the view or the Shader asset changes, and
        // until every texture has loaded.
        const auto key = SerializeMaterialSource(m_Source) + "|" + std::to_string(int(m_PreviewShape)) + "|" +
                         std::to_string(m_PreviewRotation.x) + "|" + std::to_string(m_PreviewRotation.y) + "|" +
                         std::to_string(side) + "|" + std::to_string(uintptr_t(m_SchemaShader.get())) + "|" +
                         std::to_string(uintptr_t(m_ParentAsset.get()));
        if (key != m_PreviewKey || !m_PreviewComplete)
        {
            const ShaderData* shader    = nullptr;
            const auto*       parent    = IsInstance() ? ParentEffective() : nullptr;
            const AssetID     shaderID  = parent ? parent->Shader : m_Source.Shader;
            bool              ready     = schema && (!IsInstance() || parent);
            if (ready && !IsStandardShader(shaderID))
            {
                ready  = m_SchemaShader && m_Schema;
                shader = ready ? &m_SchemaShader->GetData() : nullptr;
            }
            if (ready)
            {
                // Keep other shaders' values in the editable source and undo snapshots.
                // Only the selected shader's declarations participate in validation/preview.
                const auto                               source    = DeclaredValues(m_Source, *schema);
                std::variant<MaterialSource, AssetError> effective = source;
                if (parent)
                    effective = ApplyMaterialOverrides(*parent, source, shader);
                auto resolved = std::holds_alternative<AssetError>(effective) ?
                                    std::variant<MaterialData, AssetError>(std::get<AssetError>(effective)) :
                                    ResolveMaterialSource(std::get<MaterialSource>(effective), shader);
                if (const auto* error = std::get_if<AssetError>(&resolved))
                {
                    m_PreviewError    = error->Message;
                    m_PreviewComplete = true; // Nothing to wait for until the source changes.
                }
                else
                {
                    MaterialPreviewOptions options;
                    options.Size      = {uint32_t(side), uint32_t(side)};
                    options.Shape     = m_PreviewShape;
                    options.Rotation  = m_PreviewRotation;
                    const auto result = m_Preview->RenderImage(std::get<MaterialData>(resolved), shader, options);
                    m_PreviewImage    = result.Target;
                    m_PreviewComplete = result.Complete || !result.Target;
                    m_PreviewError.clear();
                }
                m_PreviewKey = key;
            }
        }

        // Centered square; dragging it rotates the shape.
        const float  indent = (ImGui::GetContentRegionAvail().x - side) * 0.5f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(indent, 0.0f));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##Preview", {side, side});
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
        {
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            m_PreviewRotation.x += delta.x * 0.01f;
            m_PreviewRotation.y = std::clamp(m_PreviewRotation.y + delta.y * 0.01f, -1.5f, 1.5f);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drag to rotate. Shows unsaved changes.");
        ImDrawList* draw = ImGui::GetWindowDrawList();
        // Export on every use, including cached previews. The UI frame retains this
        // image/descriptor through GPU completion even if another draw resizes the target.
        m_PreviewTexture =
            m_PreviewImage ? m_Preview->GetViewTarget().GetDevice()->GetImGuiTextureID(m_PreviewImage) : 0;
        if (m_PreviewTexture)
            draw->AddImage(
                static_cast<ImTextureID>(uintptr_t(m_PreviewTexture)), at, {at.x + side, at.y + side}, {0, 1}, {1, 0});
        else
            draw->AddRectFilled(at, {at.x + side, at.y + side}, IM_COL32(40, 40, 40, 255));
        if (!m_PreviewError.empty() || !m_PreviewComplete)
        {
            const char* note = m_PreviewError.empty() ? "Loading..." : "Preview unavailable";
            draw->AddText({at.x + 6.0f, at.y + 4.0f}, IM_COL32(230, 230, 230, 200), note);
        }
        if (!m_PreviewError.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", m_PreviewError.c_str());

        // Shape choice under the image.
        const struct
        {
            MaterialPreviewShape Shape;
            const char*          Name;
        } shapes[] = {{MaterialPreviewShape::Sphere, "Sphere"},
                      {MaterialPreviewShape::Cube, "Cube"},
                      {MaterialPreviewShape::Plane, "Plane"}};
        const float width = (side - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(indent, 0.0f));
        for (const auto& shape : shapes)
        {
            if (shape.Shape != shapes[0].Shape)
                ImGui::SameLine();
            const bool active = m_PreviewShape == shape.Shape;
            if (active)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(shape.Name, {width, 0.0f}))
                m_PreviewShape = shape.Shape;
            if (active)
                ImGui::PopStyleColor();
        }
        ImGui::Spacing();
    }

    void MaterialEditorPanel::DrawRenderState()
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Rendering");
        // An instance edits a copy of its effective state; changed fields become overrides.
        const bool          instance = IsInstance();
        const auto*         parent   = instance ? ParentEffective() : nullptr;
        MaterialRenderState effective = parent ? parent->Render : m_Source.Render;
        if (instance)
        {
            const auto& overrides = m_Source.RenderOverrides;
            if (overrides.AlphaMode)
                effective.AlphaMode = *overrides.AlphaMode;
            if (overrides.AlphaCutoff)
                effective.AlphaCutoff = *overrides.AlphaCutoff;
            if (overrides.DoubleSided)
                effective.DoubleSided = *overrides.DoubleSided;
            const bool any = overrides.AlphaMode || overrides.AlphaCutoff || overrides.DoubleSided;
            if (any)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Use parent's"))
                {
                    m_Source.RenderOverrides = {};
                    m_Dirty                  = true;
                }
            }
        }
        const MaterialRenderState before = effective;
        auto&                     render = instance ? effective : m_Source.Render;
        const struct
        {
            MaterialAlphaMode Mode;
            const char*       Name;
            const char*       Help;
        } modes[] = {
            {MaterialAlphaMode::Opaque, "Opaque", "Alpha is ignored."},
            {MaterialAlphaMode::Mask,
             "Cutout",
             "Pixels with alpha below the cutoff are discarded (foliage, hair cards)."},
            {MaterialAlphaMode::Blend,
             "Transparent",
             "Blended over what is behind, drawn after opaque parts from far to near (glass, fades)."},
            {MaterialAlphaMode::Additive, "Additive", "Adds color weighted by alpha."},
            {MaterialAlphaMode::Premultiplied, "Premultiplied", "Uses color already multiplied by alpha."},
            {MaterialAlphaMode::Dithered,
             "Dithered",
             "Order-independent transparency: alpha becomes coverage (MSAA) or a fixed dither pattern, and "
             "depth is written (overlapping hair and face parts; Blender Eevee's \"Dithered\")."},
        };
        const auto* current = &modes[0];
        for (const auto& mode : modes)
            if (mode.Mode == render.AlphaMode)
                current = &mode;
        if (ImGui::BeginCombo("Alpha mode", current->Name))
        {
            for (const auto& mode : modes)
            {
                if (ImGui::Selectable(mode.Name, mode.Mode == render.AlphaMode))
                {
                    render.AlphaMode = mode.Mode;
                    m_Dirty          = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", mode.Help);
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s%s",
                              current->Help,
                              !IsStandardShader(m_Source.Shader) && render.AlphaMode != MaterialAlphaMode::Opaque ?
                                  "\nCustom shaders apply it through KansAlphaClip / KansOutputAlpha "
                                  "(#include <Kans3D/Alpha.glsl>)." :
                                  "");
        if (render.AlphaMode == MaterialAlphaMode::Mask &&
            ImGui::SliderFloat("Alpha cutoff", &render.AlphaCutoff, 0.0f, 1.0f, "%.2f"))
        {
            render.AlphaCutoff = std::clamp(render.AlphaCutoff, 0.0f, 1.0f);
            m_Dirty            = true;
        }
        if (ImGui::Checkbox("Double-sided", &render.DoubleSided))
            m_Dirty = true;
        if (instance)
        {
            auto& overrides = m_Source.RenderOverrides;
            if (render.AlphaMode != before.AlphaMode)
                overrides.AlphaMode = render.AlphaMode;
            if (render.AlphaCutoff != before.AlphaCutoff)
                overrides.AlphaCutoff = render.AlphaCutoff;
            if (render.DoubleSided != before.DoubleSided)
                overrides.DoubleSided = render.DoubleSided;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draw back faces too, lit with the flipped normal (leaves, cloth, thin planes).");
        DrawPasses(instance, parent);
    }

    void MaterialEditorPanel::DrawPasses(bool instance, const MaterialSource* parent)
    {
        // Additional passes declared by the shader (e.g. an outline hull); a material can switch
        // them off. An instance can switch off more, not re-enable what its parent disabled.
        if (!m_SchemaShader || m_SchemaShader->GetData().Passes.empty())
            return;
        const auto& passes = m_SchemaShader->GetData().Passes;
        const auto  listed = [](const std::vector<std::string>& names, const std::string& name) {
            return std::find(names.begin(), names.end(), name) != names.end();
        };
        ImGui::Spacing();
        ImGui::TextDisabled("Shader passes");
        for (const auto& pass : passes)
        {
            // The shadow pass follows Receive/Cast settings, not this list.
            if (pass.Tag == "ShadowCaster")
                continue;
            const bool inherited = instance && parent && listed(parent->DisabledPasses, pass.Name);
            bool       enabled   = !inherited && !listed(m_Source.DisabledPasses, pass.Name);
            ImGui::BeginDisabled(inherited);
            if (ImGui::Checkbox(pass.Name.c_str(), &enabled))
            {
                auto& disabled = m_Source.DisabledPasses;
                if (enabled)
                    disabled.erase(std::remove(disabled.begin(), disabled.end(), pass.Name), disabled.end());
                else
                    disabled.push_back(pass.Name);
                m_Dirty = true;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(inherited ? "Disabled by the parent material." : "Shader pass (tag %s).",
                                  pass.Tag.c_str());
        }
    }

    AssetID MaterialEditorPanel::PublishedMaterial() const
    {
        std::error_code error;
        const auto      file = std::filesystem::weakly_canonical(m_File, error);
        if (error)
            return {};
        for (const auto& choice : m_Choices)
            if (choice.Type == AssetType::Material && choice.SourcePath == file)
                return choice.ID;
        return {};
    }

    void MaterialEditorPanel::DrawUsedBy()
    {
        if (!m_Usage)
            return;
        const AssetID material = PublishedMaterial();
        const auto    uses     = material.IsValid() ? m_Usage(material) : std::vector<MaterialUse> {};
        ImGui::Separator();
        const auto title = "Used by (" + std::to_string(uses.size()) + ")###UsedBy";
        if (!ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            return;
        if (!material.IsValid())
        {
            ImGui::TextDisabled("Not imported yet: save to use it in scenes.");
            return;
        }
        if (uses.empty())
        {
            ImGui::TextDisabled("No mesh in the open scene draws this material.");
            return;
        }
        // Scrolls past a dozen rows: a character can use one material on many parts.
        const float rows = float(std::min<size_t>(uses.size(), 12));
        if (ImGui::BeginChild("##UsedBy", {0, rows * ImGui::GetTextLineHeightWithSpacing() + 4.0f}))
        {
            for (const auto& use : uses)
            {
                ImGui::PushID(int(use.Slot));
                ImGui::PushID(std::to_string(uint64_t(use.Entity)).c_str());
                if (ImGui::Selectable(use.EntityName.c_str()) && m_Select)
                    m_Select(use.Entity);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Select %s", use.EntityName.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("%s%s", use.SlotName.c_str(), use.Overridden ? "  (assigned here)" : "");
                ImGui::PopID();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    void MaterialEditorPanel::Draw()
    {
        if (m_File.empty())
        {
            ImGui::TextWrapped("Double-click a .material file in the content browser, or right-click a folder "
                               "there and choose Create Material.");
            if (!m_Status.empty())
                ImGui::TextWrapped("%s", m_Status.c_str());
            return;
        }
        ImGui::TextUnformatted(m_File.filename().u8string().c_str());
        if (m_Dirty)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(modified)");
        }

        char name[256] = {};
        m_Source.Name.copy(name, sizeof(name) - 1);
        if (ImGui::InputText("Name", name, sizeof(name)))
        {
            m_Source.Name = name;
            m_Dirty       = true;
        }

        DrawParent();
        const AssetID     shownShader = IsInstance() && ParentEffective() ? ParentEffective()->Shader : m_Source.Shader;
        const std::string shaderName  = !IsStandardShader(shownShader) ? ChoiceName(shownShader) : "Standard";
        // Disable only for instances: a disabled combo never opens, so the pair cannot
        // straddle the combo's popup window (ImGui asserts on a mismatched stack there).
        if (IsInstance())
            ImGui::BeginDisabled();
        const bool shaderOpen = ImGui::BeginCombo("Shader", shaderName.c_str());
        if (IsInstance())
            ImGui::EndDisabled();
        if (IsInstance() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Inherited from the parent material.");
        if (shaderOpen)
        {
            if (ImGui::Selectable("Standard", IsStandardShader(m_Source.Shader)))
            {
                m_Source.Shader = {};
                m_Dirty         = true;
            }
            if (ImGui::Selectable("Kans3D/SixWaySmoke", m_Source.Shader == SixWaySmokeShaderID()))
            {
                m_Source.Shader = SixWaySmokeShaderID();
                m_Dirty         = true;
            }
            for (const auto& choice : m_Choices)
                if (choice.Type == AssetType::Shader &&
                    ImGui::Selectable(choice.Name.c_str(), choice.ID == m_Source.Shader))
                {
                    m_Source.Shader = choice.ID;
                    m_Dirty         = true;
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Shaders appear here once imported. Import a .shaderprog (vertex + fragment)\n"
                              "that follows the material contract, e.g. assets/shaders/Toon/Toon.shaderprog.");

        FinishPendingTexture();
        const auto* schema = Schema();
        DrawPreview();
        DrawRenderState();
        ImGui::Separator();
        if (schema)
            DrawParameters(*schema);
        else
            ImGui::TextWrapped("%s", m_SchemaError.c_str());

        DrawUsedBy();
        ImGui::Separator();
        TrackEdits();
        m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const ImGuiIO& io = ImGui::GetIO();
        if (m_Focused && io.KeyCtrl && !io.WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, true))
                io.KeyShift ? Redo() : Undo();
            else if (ImGui::IsKeyPressed(ImGuiKey_Y, true))
                Redo();
            else if (ImGui::IsKeyPressed(ImGuiKey_S, false))
                Save();
        }
        ImGui::BeginDisabled(m_UndoStack.empty());
        if (ImGui::Button("Undo"))
            Undo();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Undo material edit (Ctrl+Z)");
        ImGui::SameLine();
        ImGui::BeginDisabled(m_RedoStack.empty());
        if (ImGui::Button("Redo"))
            Redo();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Redo material edit (Ctrl+Y)");
        ImGui::SameLine();
        if (ImGui::Button("Save"))
            Save();
        ImGui::SameLine();
        if (ImGui::Button("Revert"))
            Open(m_File);

        if (m_Building && m_Service)
        {
            auto snapshot = m_Service->GetSnapshot();
            if (const auto* status = std::get_if<EditorAssetImportSnapshot>(&snapshot);
                status && status->State != EditorAssetImportState::Building)
            {
                m_Building = false;
                if (status->State == EditorAssetImportState::Succeeded)
                    m_Status = "Saved and imported.";
                else
                    m_Status = status->Error ? status->Error->Message : std::string("Import did not finish.");
            }
        }
        if (!m_Status.empty())
            ImGui::TextWrapped("%s", m_Status.c_str());
    }
} // namespace Kans

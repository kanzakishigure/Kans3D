#include "KansEditorCore/EditorViewState.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <yaml-cpp/yaml.h>
#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "KansEditorCore/EditorVisibilityStore.h"

namespace Kans
{
    namespace
    {
        constexpr int Version = 1;

        // Reads one field; a missing or mistyped value keeps `value` (the default).
        template<class T>
        void Read(const YAML::Node& node, const char* key, T& value)
        {
            try
            {
                if (const auto field = node[key])
                    value = field.as<T>();
            }
            catch (const std::exception&)
            {}
        }
        void ReadFloat(const YAML::Node& node, const char* key, float& value, float low, float high)
        {
            float read = value;
            Read(node, key, read);
            if (std::isfinite(read))
                value = std::clamp(read, low, high);
        }
        template<class Enum>
        void ReadEnum(const YAML::Node& node, const char* key, Enum& value, int count)
        {
            int read = int(value);
            Read(node, key, read);
            if (read >= 0 && read < count)
                value = Enum(read);
        }
        template<class T, int N>
        bool ReadVector(const YAML::Node& node, const char* key, glm::vec<N, T>& value)
        {
            try
            {
                const auto field = node[key];
                if (!field || !field.IsSequence() || field.size() != size_t(N))
                    return false;
                glm::vec<N, T> read;
                for (int i = 0; i < N; ++i)
                    read[i] = field[size_t(i)].template as<T>();
                for (int i = 0; i < N; ++i)
                    if (!std::isfinite(read[i]))
                        return false;
                value = read;
                return true;
            }
            catch (const std::exception&)
            {
                return false;
            }
        }
        void ReadColor(const YAML::Node& node, const char* key, glm::vec4& value)
        {
            glm::vec4 read = value;
            if (ReadVector(node, key, read))
                value = glm::clamp(read, glm::vec4(0.0f), glm::vec4(1.0f));
        }
        template<int N>
        void Write(YAML::Emitter& out, const char* key, const glm::vec<N, float>& value)
        {
            out << YAML::Key << key << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (int i = 0; i < N; ++i)
                out << value[i];
            out << YAML::EndSeq;
        }

        ViewportOptions ReadOptions(const YAML::Node& node)
        {
            ViewportOptions o;
            if (!node || !node.IsMap())
                return o;
            Read(node, "AmbientOcclusionBuffer", o.AmbientOcclusionBuffer);
            int reflection = o.ReflectionDebug;
            Read(node, "ReflectionDebug", reflection);
            if (reflection >= 0 && reflection <= 2)
                o.ReflectionDebug = reflection;
            ReadEnum(node, "ViewMode", o.ViewMode, int(SceneViewMode::ShadowCascades) + 1);
            Read(node, "ShowIcons", o.ShowIcons);
            Read(node, "ShowStats", o.ShowStats);
            ReadFloat(node, "GizmoSize", o.GizmoSize, 60.0f, 220.0f);
            Read(node, "RPicksScaleTool", o.RPicksScaleTool);
            Read(node, "Grid", o.Grid);
            if (const auto grid = node["GridLook"]; grid && grid.IsMap())
            {
                auto& g = o.GridLook;
                ReadFloat(grid, "CellSize", g.CellSize, 0.01f, 1000.0f);
                int major = int(g.MajorEvery);
                Read(grid, "MajorEvery", major);
                g.MajorEvery = uint32_t(std::clamp(major, 2, 100));
                ReadColor(grid, "MinorColor", g.MinorColor);
                ReadColor(grid, "MajorColor", g.MajorColor);
                ReadColor(grid, "XAxisColor", g.XAxisColor);
                ReadColor(grid, "YAxisColor", g.YAxisColor);
                ReadColor(grid, "ZAxisColor", g.ZAxisColor);
                ReadFloat(grid, "LineWidth", g.LineWidth, 0.5f, 4.0f);
                ReadFloat(grid, "FadeDistance", g.FadeDistance, 5.0f, 10000.0f);
            }
            Read(node, "Outline", o.Outline);
            Read(node, "Stencil", o.Stencil);
            Read(node, "DebugNormals", o.DebugNormals);
            uint32_t physics = o.PhysicsView;
            Read(node, "PhysicsView", physics);
            o.PhysicsView = physics & 0x1FFu; // The PhysicsDebugFlags bits.
            Read(node, "ShowAllAudio", o.ShowAllAudio);
            Read(node, "ShowOcclusionRays", o.ShowOcclusionRays);
            Read(node, "EditorCameraInPlay", o.EditorCameraInPlay);
            int resolution = o.GameResolution;
            Read(node, "GameResolution", resolution);
            if (resolution >= 0 && resolution < 64)
                o.GameResolution = resolution;
            int tool = o.Tool;
            Read(node, "Tool", tool);
            if (tool >= 0 && tool <= 4)
                o.Tool = uint8_t(tool);
            ReadEnum(node, "Orientation", o.Orientation, 3);
            ReadEnum(node, "Pivot", o.Pivot, 2);
            Read(node, "Snap", o.Snap);
            ReadFloat(node, "SnapMove", o.SnapMove, 0.01f, 360.0f);
            ReadFloat(node, "SnapRotate", o.SnapRotate, 0.01f, 360.0f);
            ReadFloat(node, "SnapScale", o.SnapScale, 0.01f, 360.0f);
            return o;
        }

        void WriteOptions(YAML::Emitter& out, const ViewportOptions& o)
        {
            out << YAML::Key << "Viewport" << YAML::Value << YAML::BeginMap;
            out << YAML::Key << "AmbientOcclusionBuffer" << YAML::Value << o.AmbientOcclusionBuffer;
            out << YAML::Key << "ReflectionDebug" << YAML::Value << o.ReflectionDebug;
            out << YAML::Key << "ViewMode" << YAML::Value << int(o.ViewMode);
            out << YAML::Key << "ShowIcons" << YAML::Value << o.ShowIcons;
            out << YAML::Key << "ShowStats" << YAML::Value << o.ShowStats;
            out << YAML::Key << "GizmoSize" << YAML::Value << o.GizmoSize;
            out << YAML::Key << "RPicksScaleTool" << YAML::Value << o.RPicksScaleTool;
            out << YAML::Key << "Grid" << YAML::Value << o.Grid;
            out << YAML::Key << "GridLook" << YAML::Value << YAML::BeginMap;
            out << YAML::Key << "CellSize" << YAML::Value << o.GridLook.CellSize;
            out << YAML::Key << "MajorEvery" << YAML::Value << o.GridLook.MajorEvery;
            Write(out, "MinorColor", o.GridLook.MinorColor);
            Write(out, "MajorColor", o.GridLook.MajorColor);
            Write(out, "XAxisColor", o.GridLook.XAxisColor);
            Write(out, "YAxisColor", o.GridLook.YAxisColor);
            Write(out, "ZAxisColor", o.GridLook.ZAxisColor);
            out << YAML::Key << "LineWidth" << YAML::Value << o.GridLook.LineWidth;
            out << YAML::Key << "FadeDistance" << YAML::Value << o.GridLook.FadeDistance;
            out << YAML::EndMap;
            out << YAML::Key << "Outline" << YAML::Value << o.Outline;
            out << YAML::Key << "Stencil" << YAML::Value << o.Stencil;
            out << YAML::Key << "DebugNormals" << YAML::Value << o.DebugNormals;
            out << YAML::Key << "PhysicsView" << YAML::Value << o.PhysicsView;
            out << YAML::Key << "ShowAllAudio" << YAML::Value << o.ShowAllAudio;
            out << YAML::Key << "ShowOcclusionRays" << YAML::Value << o.ShowOcclusionRays;
            out << YAML::Key << "EditorCameraInPlay" << YAML::Value << o.EditorCameraInPlay;
            out << YAML::Key << "GameResolution" << YAML::Value << o.GameResolution;
            out << YAML::Key << "Tool" << YAML::Value << int(o.Tool);
            out << YAML::Key << "Orientation" << YAML::Value << int(o.Orientation);
            out << YAML::Key << "Pivot" << YAML::Value << int(o.Pivot);
            out << YAML::Key << "Snap" << YAML::Value << o.Snap;
            out << YAML::Key << "SnapMove" << YAML::Value << o.SnapMove;
            out << YAML::Key << "SnapRotate" << YAML::Value << o.SnapRotate;
            out << YAML::Key << "SnapScale" << YAML::Value << o.SnapScale;
            out << YAML::EndMap;
        }

        // A camera entry, or nothing when its position or angles cannot be read.
        std::optional<EditorViewCamera> ReadCamera(const YAML::Node& node)
        {
            if (!node || !node.IsMap())
                return std::nullopt;
            EditorViewCamera camera;
            auto&            c        = camera.Camera;
            const float      unset    = std::numeric_limits<float>::quiet_NaN();
            float            distance = unset, yaw = unset, pitch = unset;
            Read(node, "Distance", distance);
            Read(node, "Yaw", yaw);
            Read(node, "Pitch", pitch);
            if (!ReadVector(node, "Focus", c.Focus) || !std::isfinite(distance) || !std::isfinite(yaw) ||
                !std::isfinite(pitch))
                return std::nullopt;
            c.Distance = distance;
            c.Yaw      = yaw;
            c.Pitch    = pitch;
            ReadFloat(node, "FlySpeed", c.FlySpeed, EditorCamera::MinFlySpeed, EditorCamera::MaxFlySpeed);
            Read(node, "Orthographic", c.Orthographic);
            Read(node, "View", camera.View);
            return camera;
        }
    } // namespace

    bool operator==(const EditorViewCamera& a, const EditorViewCamera& b)
    {
        const auto& x = a.Camera;
        const auto& y = b.Camera;
        return x.Focus == y.Focus && x.Distance == y.Distance && x.Yaw == y.Yaw && x.Pitch == y.Pitch &&
               x.FlySpeed == y.FlySpeed && x.Orthographic == y.Orthographic && a.View == b.View;
    }

    bool operator==(const ViewportOptions& a, const ViewportOptions& b)
    {
        const auto& g    = a.GridLook;
        const auto& h    = b.GridLook;
        const bool  grid = g.Enabled == h.Enabled && g.CellSize == h.CellSize && g.MajorEvery == h.MajorEvery &&
                           g.MinorColor == h.MinorColor && g.MajorColor == h.MajorColor &&
                           g.XAxisColor == h.XAxisColor && g.YAxisColor == h.YAxisColor &&
                           g.ZAxisColor == h.ZAxisColor && g.LineWidth == h.LineWidth &&
                           g.FadeDistance == h.FadeDistance;
        return grid && a.AmbientOcclusionBuffer == b.AmbientOcclusionBuffer && a.ReflectionDebug == b.ReflectionDebug &&
               a.ViewMode == b.ViewMode && a.ShowIcons == b.ShowIcons && a.ShowStats == b.ShowStats &&
               a.GizmoSize == b.GizmoSize && a.RPicksScaleTool == b.RPicksScaleTool && a.Grid == b.Grid &&
               a.Outline == b.Outline && a.Stencil == b.Stencil && a.DebugNormals == b.DebugNormals &&
               a.PhysicsView == b.PhysicsView && a.ShowAllAudio == b.ShowAllAudio &&
               a.ShowOcclusionRays == b.ShowOcclusionRays && a.EditorCameraInPlay == b.EditorCameraInPlay &&
               a.GameResolution == b.GameResolution && a.Tool == b.Tool && a.Orientation == b.Orientation &&
               a.Pivot == b.Pivot && a.Snap == b.Snap && a.SnapMove == b.SnapMove && a.SnapRotate == b.SnapRotate &&
               a.SnapScale == b.SnapScale;
    }

    EditorViewStateStore::EditorViewStateStore(std::filesystem::path file) : m_File(std::move(file)) {}

    std::filesystem::path EditorViewStateStore::File() const
    {
        if (!m_File.empty())
            return m_File;
        const auto& root = KansFileSystem::GetProjectRoot();
        return root.empty() ? std::filesystem::path {} : root / "Cache/Editor/ViewState.yaml";
    }

    void EditorViewStateStore::Load()
    {
        m_Options = {};
        m_Cameras.clear();
        m_Dirty = false;
        try
        {
            std::ifstream input(File());
            if (!input)
                return;
            const auto root = YAML::Load(input);
            if (!root.IsMap())
                return;
            // Newer files are read field by field too: what this version knows still applies.
            m_Options = ReadOptions(root["Viewport"]);
            if (const auto cameras = root["Cameras"]; cameras && cameras.IsMap())
                for (const auto& entry : cameras)
                {
                    std::string key;
                    try
                    {
                        key = EditorVisibilityStore::NormalizeKey(entry.first.as<std::string>());
                    }
                    catch (const std::exception&)
                    {
                        continue;
                    }
                    if (auto camera = ReadCamera(entry.second); camera && !key.empty())
                        m_Cameras[key] = std::move(*camera);
                }
        }
        catch (const std::exception&)
        {
            // An unparsable file: the defaults set above stay.
        }
    }

    std::optional<EditorViewCamera> EditorViewStateStore::BindScene(std::string key)
    {
        m_Key = EditorVisibilityStore::NormalizeKey(std::move(key));
        return Camera(m_Key);
    }

    std::optional<EditorViewCamera> EditorViewStateStore::Camera(const std::string& key) const
    {
        const auto found = m_Cameras.find(EditorVisibilityStore::NormalizeKey(key));
        if (found == m_Cameras.end())
            return std::nullopt;
        return found->second;
    }

    void EditorViewStateStore::RenameScene(std::string key)
    {
        key = EditorVisibilityStore::NormalizeKey(std::move(key));
        if (key.empty() || key == m_Key)
            return;
        if (const auto found = m_Cameras.find(m_Key); found != m_Cameras.end())
        {
            m_Cameras[key] = found->second;
            m_Dirty        = true;
        }
        m_Key = std::move(key);
    }

    void EditorViewStateStore::Track(const EditorViewCamera& camera,
                                     const ViewportOptions&  options,
                                     bool                    playing,
                                     double                  seconds)
    {
        bool changed = false;
        if (options != m_Options)
        {
            m_Options = options;
            changed   = true;
        }
        if (!playing && !m_Key.empty())
        {
            const auto found = m_Cameras.find(m_Key);
            if (found == m_Cameras.end() || found->second != camera)
            {
                m_Cameras[m_Key] = camera;
                changed          = true;
            }
        }
        if (changed)
        {
            m_Dirty      = true;
            m_LastChange = seconds;
        }
        else if (m_Dirty && seconds - m_LastChange >= DebounceSeconds && !Save())
            m_LastChange = seconds; // Retried a debounce later, not every frame.
    }

    bool EditorViewStateStore::Flush() { return !m_Dirty || Save(); }

    bool EditorViewStateStore::Save()
    {
        const auto file = File();
        if (file.empty())
            return false;
        try
        {
            YAML::Emitter out;
            out.SetFloatPrecision(9); // Floats read back bit-exact.
            out << YAML::BeginMap << YAML::Key << "Version" << YAML::Value << Version;
            WriteOptions(out, m_Options);
            out << YAML::Key << "Cameras" << YAML::Value << YAML::BeginMap;
            for (const auto& [key, camera] : m_Cameras)
            {
                const auto& c = camera.Camera;
                out << YAML::Key << key << YAML::Value << YAML::BeginMap;
                Write(out, "Focus", c.Focus);
                out << YAML::Key << "Distance" << YAML::Value << c.Distance;
                out << YAML::Key << "Yaw" << YAML::Value << c.Yaw;
                out << YAML::Key << "Pitch" << YAML::Value << c.Pitch;
                out << YAML::Key << "FlySpeed" << YAML::Value << c.FlySpeed;
                out << YAML::Key << "Orthographic" << YAML::Value << c.Orthographic;
                out << YAML::Key << "View" << YAML::Value << camera.View;
                out << YAML::EndMap;
            }
            out << YAML::EndMap << YAML::EndMap;
            if (!out.good() || !EditorVisibilityStore::WriteFile(file, out.c_str()))
                return false;
            m_Dirty = false;
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }
} // namespace Kans

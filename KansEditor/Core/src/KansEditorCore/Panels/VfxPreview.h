#pragma once
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "Kans3D/Renderer/RenderStatistics.h"
#include "Kans3D/Scene/Entity.h"
#include "Kans3D/Scene/Scene.h"
#include "Kans3D/VFX/VfxWorld.h"

// ImGui-free authoring preview of a VFX system, in the manner of Niagara's preview viewport and
// debugger: a private scene holding the effect (and optional floor, sphere and sun), a system
// instance built from the latest compile product, deterministic playback with a fixed step and a
// scrub that replays from the start, preview-only user parameter values, solo and mute, per-emitter
// statistics, an on-demand attribute capture (the only particle readback), Grid3D slices read back
// at most a few times a second, and debug lines. VfxSystemEditorPanel draws it.
namespace Kans::VfxPreview
{
    // The asset ID the preview's product carries (its component names it).
    AssetID PreviewAssetID();

    struct EmitterStats
    {
        std::string Handle, Name;
        bool        Gpu = false;
        uint32_t    Count = 0, Capacity = 0;
        uint64_t    SpawnedThisFrame = 0; // Particles spawned by the last frame's steps.
        double      CpuMilliseconds  = 0; // This emitter's share of the last step.
        uint64_t    MemoryBytes      = 0; // Attribute storage.
    };

    // ---- Attribute spreadsheet ----
    struct Capture
    {
        std::string                          Emitter; // Handle GUID.
        std::vector<std::string>             Columns; // Attribute names.
        std::vector<Vfx::Type>               Kinds;
        std::vector<std::vector<Vfx::Value>> Rows;    // The first particles in alive order.
        uint32_t                             Alive = 0; // Every living particle (rows hold at most the limit).
        double                               Time  = 0;
        std::string                          Error;
        // The attribute values of a row by name (nothing when the column is missing).
        std::optional<Vfx::Value> Get(size_t row, const std::string& column) const;
    };
    // The number of scalar components a column shows (x, y, z, w).
    uint32_t ColumnComponents(Vfx::Type);
    float    ComponentOf(const Vfx::Value&, uint32_t component);
    // The row order by one component of a column; ties keep the capture order.
    std::vector<size_t> SortRows(const Capture&, size_t column, uint32_t component, bool ascending);
    // The rows of `rows` whose column component lies in [min, max].
    std::vector<size_t> FilterRows(const Capture&, const std::vector<size_t>& rows, size_t column, uint32_t component,
                                   float min, float max);

    // ---- Grid3D slices ----
    struct GridView
    {
        std::string Emitter, Grid, Attribute; // Handle GUID, data interface ("Emitter.Velocity"), attribute.
        int         Axis      = 2;            // The slice's normal: 0 X, 1 Y, 2 Z.
        int         Index     = 0;
        uint32_t    Component = 0;
        bool        operator==(const GridView& b) const
        {
            return Emitter == b.Emitter && Grid == b.Grid && Attribute == b.Attribute && Axis == b.Axis &&
                   Index == b.Index && Component == b.Component;
        }
        bool operator!=(const GridView& b) const { return !(*this == b); }
    };
    struct GridSlice
    {
        GridView           View;
        glm::uvec2         Size {0};  // Cells along the slice's u and v axes.
        std::vector<float> Values;    // u fastest.
        float              Min = 0, Max = 0;
        glm::uvec3         Resolution {0};
        glm::vec3          WorldBounds {0};
        float              At(uint32_t u, uint32_t v) const { return Values[u + Size.x * v]; }
    };
    // A slice of a grid's storage (attribute-major, x fastest, one vec4 word per cell): normal X
    // spans (y, z), Y spans (x, z) and Z spans (x, y). The index is clamped to the grid.
    GridSlice ExtractSlice(const std::vector<glm::vec4>& words, const Vfx::InterfaceBinding& grid, const GridView& view);
    // Reads a grid's current values (one vec4 per cell and attribute); empty when unavailable.
    using GridReader = std::function<std::vector<glm::vec4>(Vfx::EmitterInstance&, const std::string& grid)>;

    // ---- Debug draw ----
    struct DebugOptions
    {
        bool  Velocity = false, Bounds = true, Origins = true, Contacts = false, VectorFields = false;
        float VelocityScale = 0.1f;
    };
    struct DebugLine
    {
        glm::vec3 From {0}, To {0};
        uint32_t  Color = 0xffffffff; // ABGR.
    };

    // The GPU time of the VFX passes (simulation, sorting, drawing) of a completed timed frame;
    // nothing when the device has no timestamps.
    std::optional<double> VfxGpuMilliseconds(const FrameStatistics&);

    // The time a system runs before it completes (the longest finite EmitterState duration with its
    // delay and loops); nothing when it runs forever.
    std::optional<double> SystemLifetime(const Vfx::CompiledSystem&);

    class Session
    {
    public:
        static constexpr double SliceInterval   = 0.25; // Seconds between grid readbacks while playing.
        static constexpr int    MaxStepsPerFrame = 16;  // A slow frame drops the rest instead of spiralling.

        Session();
        ~Session();
        Session(const Session&)            = delete;
        Session& operator=(const Session&) = delete;

        const Ref<Scene>& GetScene() const { return m_Scene; }
        Entity            GetEntity() const { return m_Entity; }

        // ---- Product ----
        // Rebuilds the instance from a compile product and replays to the current time with the
        // preview's parameter values; on failure the previous instance stays and the preview is
        // outdated with the reason.
        bool                 SetProduct(Vfx::CompiledSystem product);
        // A compile failed: the last good preview stays, marked outdated.
        void                 SetOutdated(std::string reason);
        bool                 IsOutdated() const { return m_Outdated; }
        const std::string&   OutdatedReason() const { return m_Reason; }
        Vfx::SystemInstance* Instance() const;
        const Vfx::CompiledSystem* Product() const;
        size_t               Builds() const { return m_Builds; }

        // ---- Playback ----
        void     Play();
        void     Pause();
        bool     IsPlaying() const { return m_Playing; }
        // One fixed step, paused (with auto capture, captures afterwards).
        void     StepFrame();
        // Back to 0 with the same seed and warmup.
        void     Restart();
        // Deterministic: reset, warmup, then fixed steps to the target (forwards from the current
        // state when the target is later).
        void     Seek(double seconds);
        // Plays `realSeconds` of wall time at the speed in fixed steps; loops or stops at the duration.
        void     Advance(double realSeconds);
        double   Time() const { return double(m_Steps) * double(m_FixedStep); }
        uint64_t Steps() const { return m_Steps; }
        void     SetSpeed(float speed); // 0.1 to 4.
        float    Speed() const { return m_Speed; }
        void     SetFixedStep(float seconds); // 1/1000 to 1/10; replays to the current time.
        float    FixedStep() const { return m_FixedStep; }
        void     SetDuration(double seconds);
        double   Duration() const { return m_Duration; }
        bool     Loop = true;
        // The camera position (distance culling) and frame refresh: colliders, fields and the sun.
        glm::vec3 Camera {0, 0, 5};

        // ---- User parameters (preview values; the asset keeps its defaults) ----
        bool                     SetUserParameter(const std::string& name, const Vfx::Value& value);
        void                     ClearUserParameter(const std::string& name);
        const Vfx::ParameterMap& UserOverrides() const { return m_UserOverrides; }

        // ---- Events ----
        // Adds `count` particles to an emitter's next step (by handle GUID).
        bool Emit(const std::string& handle, uint32_t count);

        // ---- Solo and mute (the preview compiles a masked copy; the asset is unchanged) ----
        void                         SetMuted(const std::string& handle, bool muted);
        bool                         IsMuted(const std::string& handle) const { return m_Muted.count(handle) != 0; }
        void                         SetSolo(const std::string& handle); // "" clears.
        const std::string&           Solo() const { return m_Solo; }
        bool                         HasMask() const { return !m_Solo.empty() || !m_Muted.empty(); }
        // The system with muted emitters (or every emitter but the soloed one) disabled.
        Vfx::System                  Masked(const Vfx::System&) const;
        // What the mask was when the product was compiled (a change needs a new preview compile).
        uint64_t                     MaskRevision() const { return m_MaskRevision; }

        // ---- Statistics ----
        std::vector<EmitterStats> Stats() const;
        // GPU time of the preview's VFX passes in the last timed frame (nothing: no timestamps).
        std::optional<double> GpuMilliseconds;

        // ---- Spreadsheet ----
        // Reads the first `limit` particles of an emitter once (GPU emitters wait for the device).
        Capture                         CaptureEmitter(const std::string& handle, size_t limit = 64);
        const std::optional<Capture>&   LastCapture() const { return m_Capture; }
        bool                            AutoCapture = false;
        std::string                     CaptureHandle; // The emitter Step captures with auto capture on.
        size_t                          CaptureLimit = 64;
        // Explicit readbacks done (captures and grid slices): nothing else reads particle data back.
        size_t                          Readbacks() const { return m_Readbacks; }

        // ---- Grid3D slices ----
        void                            SetGridView(std::optional<GridView> view);
        const std::optional<GridView>&  GetGridView() const { return m_GridView; }
        // Reads the slice when the view changed, after a step while paused, or at most every
        // SliceInterval while playing; call once a frame with the time in seconds.
        void                            UpdateSlice(double now);
        const std::optional<GridSlice>& Slice() const { return m_Slice; }
        void                            SetGridReader(GridReader reader) { m_GridReader = std::move(reader); }
        // Grid data interfaces of an emitter (names), for the slice picker.
        std::vector<std::string>        Grids(const std::string& handle) const;

        // ---- Debug draw ----
        std::vector<DebugLine> DebugLines(const DebugOptions&) const;

        // ---- Preview scene ----
        void SetFloor(bool shown);
        void SetSphere(bool shown);
        void SetSun(bool shown, glm::vec3 direction);
        bool HasFloor() const { return bool(m_Floor); }
        bool HasSphere() const { return bool(m_Sphere); }
        bool HasSun() const { return bool(m_Sun); }

    private:
        Vfx::EmitterInstance* Find(const std::string& handle) const;
        void                  StepOnce();
        void                  Refresh();
        void                  ApplyOverrides();
        void                  BeginFrameSpawns();
        void                  EndFrameSpawns();

        Ref<Scene>                       m_Scene;
        Entity                           m_Entity, m_Floor, m_Sphere, m_Sun;
        Ref<VfxSystemAsset>              m_Asset;
        size_t                           m_Builds = 0;
        bool                             m_Outdated = false;
        std::string                      m_Reason;
        bool                             m_Playing  = true;
        uint64_t                         m_Steps    = 0;
        double                           m_Pending  = 0; // Fractional steps owed by Advance.
        float                            m_Speed    = 1, m_FixedStep = 1.f / 60;
        double                           m_Duration = 5;
        bool                             m_DurationSet = false;
        Vfx::ParameterMap                m_UserOverrides;
        std::set<std::string>            m_Muted;
        std::string                      m_Solo;
        uint64_t                         m_MaskRevision = 0;
        std::vector<uint64_t>            m_SpawnBefore, m_SpawnedThisFrame;
        std::optional<Capture>           m_Capture;
        size_t                           m_Readbacks = 0;
        std::optional<GridView>          m_GridView;
        std::optional<GridSlice>         m_Slice;
        GridReader                       m_GridReader;
        bool                             m_SliceDue   = false;
        uint64_t                         m_SliceSteps = 0, m_SliceBuild = 0;
        double                           m_SliceTime  = -1e9;
    };
} // namespace Kans::VfxPreview

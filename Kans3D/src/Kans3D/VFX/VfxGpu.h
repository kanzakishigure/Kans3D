#pragma once
#include "Kans3D/Renderer/Graph/RenderGraph.h"
#include "VfxWorld.h"
namespace Kans::Vfx
{
    // Storage is SoA: one 16-byte word per attribute per slot. Integers retain their bits.
    class GpuEmitter
    {
    public:
        GpuEmitter(RenderHardware::Device&, const CompiledEmitter&);
        void        AddPasses(RenderGraph&, EmitterInstance&);
        void        Poll(EmitterInstance&);
        static void AddSystemPasses(RenderGraph&, SystemInstance&, RenderHardware::Device&);
        std::map<std::string, RenderHardware::Handle<RenderHardware::Buffer>> EventBuffers;
        RenderHardware::Handle<RenderHardware::Buffer>                        EventDispatchArgs;
        void                                           UploadCpu(RenderGraph&, const EmitterInstance&);
        RenderHardware::Handle<RenderHardware::Buffer> SortForView(RenderGraph&,
                                                                   const EmitterInstance&,
                                                                   const Renderer&,
                                                                   const glm::mat4& view,
                                                                   const glm::mat4& owner,
                                                                   bool             transparent);
        struct MeshBin
        {
            RenderHardware::Handle<RenderHardware::Buffer> Indices, Arguments;
        };
        MeshBin BinMesh(RenderGraph&,
                        const Renderer&,
                        uint32_t entry,
                        const std::vector<MeshSection>&,
                        RenderHardware::Handle<RenderHardware::Buffer> indices);
        static std::vector<uint32_t>
             SortCpu(const EmitterInstance&, const Renderer&, const glm::mat4& view, const glm::mat4& owner);
        bool HasPendingReadback() const { return Pending[0] || Pending[1] || Pending[2]; }
        RenderHardware::Handle<RenderHardware::Buffer> Attributes, Alive, DrawArgs, DispatchArgs, Counters;
        RenderHardware::Handle<RenderHardware::Buffer> Flags, Free;
        RenderHardware::Device&                        Device;

    private:
        struct Workspace
        {
            std::weak_ptr<const void>                      Graph;
            RenderHardware::Handle<RenderHardware::Buffer> Output, Pairs, Dispatch, Uniform;
            RenderHardware::Handle<RenderHardware::Buffer> Counts, Scan, Gather;
            const Renderer*                                RendererKey = nullptr;
            RenderHardware::Buffer*                        Input       = nullptr;
            std::vector<MeshBin>                           Bins;
        };
        std::vector<Workspace>                         SortWorkspaces, BinWorkspaces;
        RenderHardware::Handle<RenderHardware::Buffer> BoundsPartials;
        Workspace&                                     Acquire(std::vector<Workspace>&, RenderGraph&);
        struct Kernel
        {
            RenderHardware::Handle<RenderHardware::ComputePipeline> Pipeline;
            RenderHardware::Handle<RenderHardware::BindGroupLayout> Layout;
        };
        Kernel SortPrepare, SortKeys, SortStep, SortOutput, BinCount, BinScan, BinScatter, BinArguments;
        Kernel Spawn, Update, Prepare, PrepareEvent, Compact, BoundsKernel, BoundsFinish;
        std::map<Stage, Kernel>                                                  StageKernels;
        std::vector<Kernel>                                                      HandlerKernels;
        std::map<const Program*, std::vector<glm::vec4>>                         InterfaceData;
        std::map<const Program*, RenderHardware::Handle<RenderHardware::Buffer>> InterfaceBuffers;
        RenderHardware::Handle<RenderHardware::Buffer>                           Count;
        std::array<RenderHardware::Handle<RenderHardware::Buffer>, 3>            Readbacks;
        std::array<bool, 3>                                                      Pending {};
        uint32_t                                                                 ReadbackCursor = 0;
        std::array<uint64_t, 3>                                                  ReadbackSequence {};
        std::array<uint64_t, 3>                                                  ReadbackSpawnRevisions {};
        uint64_t                                       SubmittedSequence = 0, LatestSequence = 0;
        const CompiledEmitter&                         Compiled;
        bool                                           Initialized = false;
        RenderHardware::Handle<RenderHardware::Buffer> EmitterState;
        Kernel                                         Make(const std::string&,
                                                            const std::vector<RenderHardware::BindGroupLayoutEntry>&,
                                                            uint32_t,
                                                            const std::vector<uint32_t>* spirv = nullptr);
    };
    glm::uvec4 Pack(const Value&);
    Value      Unpack(glm::uvec4, Type);
} // namespace Kans::Vfx

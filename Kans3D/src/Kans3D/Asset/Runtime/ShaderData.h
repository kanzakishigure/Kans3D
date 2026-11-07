#pragma once
#include <array>
#include <map>
#include <variant>
#include <vector>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Asset/AssetError.h"

namespace Kans
{
    enum class ShaderProductStage : uint32_t
    {
        Vertex   = 0,
        Fragment = 4,
        Compute  = 5
    };
    struct ShaderModuleCode
    {
        ShaderProductStage    Stage      = ShaderProductStage::Vertex;
        std::string           EntryPoint = "main";
        std::vector<uint32_t> Words;
    };
    // Target contract: SPIR-V for Vulkan 1.2; no native handles or source files.
    struct ShaderProgramCode
    {
        std::vector<ShaderModuleCode> Modules;
    };
    enum class ShaderBindingKind
    {
        UniformBuffer,
        CombinedImageSampler,
        StorageBuffer,
        ReadOnlyStorageBuffer,
        StorageImage,
        SampledImage,
        Sampler
    };
    enum class ShaderImageDimension
    {
        Texture2D,
        Cube,
        Texture3D
    };
    enum class ShaderUniformType
    {
        Float,
        Signed,
        Unsigned,
        Boolean,
        Struct
    };
    struct ShaderUniformMember
    {
        std::string       Name;
        uint64_t          Offset = 0, Bytes = 0;
        ShaderUniformType Type       = ShaderUniformType::Float;
        uint32_t          Components = 1, Columns = 1, ArrayCount = 1;
        uint64_t          ArrayStride = 0, MatrixStride = 0;
        bool              RowMajor = false;
        // Child offsets are relative to this struct, not to the enclosing block.
        std::vector<ShaderUniformMember> Members;
    };
    struct ShaderResourceBinding
    {
        ShaderBindingKind                Kind;
        uint32_t                         Set = 0, Binding = 0, Count = 1;
        uint64_t                         BlockBytes = 0;
        ShaderImageDimension             Dimension  = ShaderImageDimension::Texture2D; // Samplers only.
        std::string                      Name;
        std::vector<ShaderUniformMember> Members;
    };
    enum class ShaderInterfaceScalar
    {
        Float,
        Signed,
        Unsigned
    };
    struct ShaderInterfaceVariable
    {
        uint32_t              Location = 0, Components = 1, Columns = 1;
        ShaderInterfaceScalar Scalar = ShaderInterfaceScalar::Float;
        std::string           Name;
    };
    struct ShaderStageReflection
    {
        ShaderProductStage                   Stage;
        std::vector<ShaderResourceBinding>   Bindings;
        std::vector<ShaderInterfaceVariable> Inputs, Outputs;
        uint32_t                             PushConstantBytes = 0;
        std::array<uint32_t, 3>              WorkgroupSize {};
    };
    // Default values of material parameters (uniform members by name), declared by
    // the Shader source; scalars in declaration order, as in MaterialSource.
    using ShaderParameterDefaults = std::map<std::string, std::vector<double>>;

    // How editors present a material parameter, declared by the Shader source.
    // Needed for booleans in particular: uniform-block bools compile to uint.
    struct ShaderParameterHint
    {
        enum class Kind : uint32_t
        {
            Toggle = 1,
            Color  = 2,
            Range  = 3,
        };
        Kind   Type = Kind::Toggle;
        double Min = 0, Max = 1; // Range only.

        bool operator==(const ShaderParameterHint& other) const
        {
            return Type == other.Type && Min == other.Min && Max == other.Max;
        }
    };
    using ShaderParameterHints = std::map<std::string, ShaderParameterHint>;

    enum class ShaderTextureFallback : uint32_t
    {
        White  = 0,
        Black  = 1,
        Normal = 2,
    };
    using ShaderTextureFallbacks = std::map<std::string, ShaderTextureFallback>;

    struct MaterialParameterGroup
    {
        std::string              Name;
        std::vector<std::string> Members;
    };

    enum class ShaderCullMode : uint32_t
    {
        None,
        Front,
        Back
    };
    enum class ShaderDepthCompare : uint32_t
    {
        Never,
        Less,
        Equal,
        LessEqual,
        Greater,
        NotEqual,
        GreaterEqual,
        Always
    };
    enum class ShaderBlendFactor : uint32_t
    {
        Zero,
        One,
        SourceColor,
        OneMinusSourceColor,
        SourceAlpha,
        OneMinusSourceAlpha,
        DestinationColor,
        OneMinusDestinationColor,
        DestinationAlpha,
        OneMinusDestinationAlpha
    };
    // Material-owned state, independent of a device or frame's main-pass state.
    struct ShaderPassState
    {
        ShaderCullMode     Cull      = ShaderCullMode::Back;
        bool               DepthTest = true, DepthWrite = true;
        ShaderDepthCompare DepthCompare = ShaderDepthCompare::Less;
        bool               Blend        = false;
        ShaderBlendFactor  SourceColor = ShaderBlendFactor::One, DestinationColor = ShaderBlendFactor::Zero;
        ShaderBlendFactor  SourceAlpha = ShaderBlendFactor::One, DestinationAlpha = ShaderBlendFactor::Zero;
        uint32_t           ColorWriteMask = 7; // RGBA bits; preserve scene alpha by default.
    };
    struct ShaderPass
    {
        std::string                        Name, Tag;
        ShaderProgramCode                  Code;
        ShaderPassState                    State;
        std::vector<ShaderStageReflection> Reflection;
        std::map<ShaderProductStage, std::string> DebugSources {};
    };
    enum class ShaderOutputSpace : uint32_t
    {
        Display   = 0,
        LinearHDR = 1
    };
    enum class ShaderDomain : uint32_t
    {
        Surface,
        PostProcess
    };
    enum class PostProcessStage : uint32_t
    {
        BeforeToneMap,
        AfterToneMap
    };
    struct ShaderData
    {
        ShaderProgramCode       Code;
        std::vector<ShaderPass> Passes;
        ShaderParameterDefaults ParameterDefaults;
        ShaderParameterHints    ParameterHints;
        // Derived from validated bytecode, never trusted caller-supplied metadata.
        std::vector<ShaderStageReflection> Reflection;
        ShaderTextureFallbacks             TextureFallbacks;
        std::vector<MaterialParameterGroup> Groups;
        ShaderOutputSpace                   OutputSpace = ShaderOutputSpace::Display;
        ShaderDomain                        Domain      = ShaderDomain::Surface;
        PostProcessStage                    PostStage   = PostProcessStage::BeforeToneMap;
        // Runtime diagnostics only; excluded from the serialized product.
        std::string                               DebugName {};
        std::map<ShaderProductStage, std::string> DebugSources {};
    };
    std::variant<std::vector<ShaderStageReflection>, AssetError> InspectShaderProgram(const ShaderProgramCode& code);
    class ShaderDataAsset                                        final : public Asset
    {
    public:
        explicit ShaderDataAsset(ShaderData data) : m_Data(std::move(data)) {}
        static AssetType  GetStaticType() { return AssetType::Shader; }
        AssetType         GetAssetType() const override { return GetStaticType(); }
        const ShaderData& GetData() const { return m_Data; }

    private:
        ShaderData m_Data;
    };
} // namespace Kans

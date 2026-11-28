#include "KansEditorCore/Profiling/BenchmarkScene.h"
#include <cmath>
#include <random>
#include "Kans3D/Asset/Importer/MaterialSource.h"
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Asset/Product/ContentDigest.h"
#include "Kans3D/Asset/Product/MeshProduct.h"
#include "Kans3D/Asset/Product/ModelProduct.h"
#include "Kans3D/Renderer/Resource/MeshFactory.h"

namespace Kans::Benchmark
{
    namespace
    {
        const SourceAssetID Source = SourceAssetID::FromString("be4c422300000001");
        const AssetID       Cube {Source, 1};
        constexpr uint32_t  MaterialCount = 7;
        constexpr double    Turn          = 6.28318530717958647692;

        AssetID Material(uint32_t index) { return AssetID(Source, index + 2); }

        template<class T>
        T Checked(std::variant<T, AssetError> value)
        {
            if (const auto* error = std::get_if<AssetError>(&value))
                throw std::runtime_error(error->Message);
            return std::get<T>(std::move(value));
        }

        class MemoryProducts final : public IAssetProvider
        {
        public:
            std::vector<ProductRecord> Records;

            void Add(AssetID id, AssetType type, std::vector<std::byte> bytes)
            {
                ProductRecord record;
                record.Metadata.assetID       = id;
                record.Metadata.Type          = type;
                record.Location.RelativeFile  = AssetPath("complex-" + std::to_string(Records.size()));
                record.Location.ByteSize      = bytes.size();
                record.Location.ContentDigest = ComputeContentDigest(bytes);
                m_Bytes.emplace(id, std::move(bytes));
                Records.push_back(std::move(record));
            }

            std::variant<std::vector<std::byte>, AssetError> Read(const ProductRecord& record) const override
            {
                return m_Bytes.at(record.Metadata.assetID);
            }

        private:
            std::map<AssetID, std::vector<std::byte>> m_Bytes;
        };
    } // namespace

    std::unique_ptr<AssetManager> PrepareComplexAssets()
    {
        auto products = CreateRef<MemoryProducts>();
        products->Add(Cube, AssetType::StaticMesh, Checked(EncodeMeshProduct(MeshFactory::CreateCubeData({1, 1, 1}))));
        const glm::vec4 colors[MaterialCount] {{.15f, .45f, .9f, 1},
                                               {.8f, .08f, .12f, 1},
                                               {.06f, .6f, .24f, 1},
                                               {.8f, .55f, .14f, 1},
                                               {.4f, .16f, .6f, 1},
                                               {.5f, .53f, .6f, 1},
                                               {.08f, .64f, .82f, .43f}};
        for (uint32_t i = 0; i < MaterialCount; ++i)
        {
            MaterialSource source;
            const auto&    color           = colors[i];
            source.Parameters["BaseColor"] = {color.r, color.g, color.b, color.a};
            if (i == MaterialCount - 1)
                source.Render.AlphaMode = MaterialAlphaMode::Blend;
            const auto data = Checked(ResolveMaterialSource(source, nullptr));
            products->Add(Material(i), AssetType::Material, Checked(EncodeMaterialProduct(data)));
        }
        auto assets = std::make_unique<AssetManager>(
            products, CreateRef<const AssetRegistry>(Checked(AssetRegistry::FromProducts(products->Records))));
        Checked(assets->Load<MeshAsset>(Cube));
        for (uint32_t i = 0; i < MaterialCount; ++i)
            Checked(assets->Load<MaterialDataAsset>(Material(i)));
        return assets;
    }

    ComplexSceneData BuildComplex(const ComplexOptions& options)
    {
        ComplexSceneData scene {CreateRef<Scene>("Benchmark Complex"), {}};
        scene.Animated.reserve(options.Instances / 8 + options.Lights);
        std::mt19937 random(options.Seed);
        // Use exact binary fractions from mt19937 so the standard library's distribution is irrelevant.
        const auto     unit    = [&] { return float(random() >> 8u) * (1.f / 16777216.f); };
        const uint32_t columns = uint32_t(std::ceil(std::sqrt(double(options.Instances))));
        const auto     coord   = [&](uint32_t index) { return (float(index) - float(columns - 1) * .5f) * 1.65f; };
        for (uint32_t i = 0; i < options.Instances; ++i)
        {
            auto          entity = scene.World->CreateEntityWithID(UUID(uint64_t(i) + 1), "Instance", false);
            MeshComponent mesh;
            mesh.Mesh              = Cube;
            const bool transparent = i % 13 == 0;
            mesh.Materials   = {Material(transparent ? MaterialCount - 1 : uint32_t(random() % (MaterialCount - 1)))};
            mesh.CastShadows = !transparent;
            entity.AddComponent<MeshComponent>(std::move(mesh));
            auto& transform    = entity.GetComponent<TransformComponent>();
            transform.Position = {coord(i % columns) + .35f * unit(), .7f * unit(), coord(i / columns) + .35f * unit()};
            transform.Rotation.y = float(Turn) * unit();
            transform.Scale      = {.65f + .45f * unit(), .7f + 1.8f * unit(), .65f + .45f * unit()};
            if (i % 8 == 0)
                scene.Animated.push_back({entity, transform.Position, transform.Rotation, float(Turn) * unit(), false});
            else
                transform.RenderMobility = Mobility::Static;
        }

        auto sun = scene.World->CreateEntityWithID(UUID(uint64_t(options.Instances) + 1), "Sun", false);
        sun.PatchComponent<TransformComponent>([](auto& transform) { transform.Rotation = {-.95f, -.55f, 0}; });
        auto& light              = sun.AddComponent<DirLightComponent>();
        light.Direction          = {.4f, -1.f, -.35f};
        light.Intensity          = 3.5f;
        light.Shadows.Enabled    = true;
        light.Shadows.Resolution = 512;
        light.Shadows.Cascades   = 4;
        light.Shadows.Filter     = ShadowFilter::PCF3x3;

        const float span = (std::max)(4.f, float(columns) * .78f);
        for (uint32_t i = 0; i < options.Lights; ++i)
        {
            auto entity = scene.World->CreateEntityWithID(UUID(uint64_t(options.Instances) + 2 + i), "Light", false);
            const bool      shadowed = i < options.ShadowedLights;
            const glm::vec3 color {.3f + unit() * .7f, .25f + unit() * .75f, .4f + unit() * .6f};
            auto&           transform = entity.GetComponent<TransformComponent>();
            transform.Position        = {(2.f * unit() - 1.f) * span, 2.5f + unit() * 3.f, (2.f * unit() - 1.f) * span};
            if (i % 2 == 0)
            {
                auto& spot              = entity.AddComponent<SpotLightComponent>();
                spot.Color              = color;
                spot.Range              = 14.f;
                spot.Intensity          = 90.f;
                spot.Shadows.Enabled    = shadowed;
                spot.Shadows.Resolution = 256;
                spot.Shadows.Filter     = ShadowFilter::PCF3x3;
                transform.Rotation      = {-1.18f, float(Turn) * unit(), 0};
                scene.Animated.push_back({entity, transform.Position, transform.Rotation, float(Turn) * unit(), true});
            }
            else
            {
                auto& point              = entity.AddComponent<PointLightComponent>();
                point.Color              = color;
                point.Range              = 10.f;
                point.Intensity          = 42.f;
                point.Shadows.Enabled    = shadowed;
                point.Shadows.Resolution = 128;
                point.Shadows.Filter     = ShadowFilter::PCF3x3;
                if (shadowed)
                    scene.Animated.push_back(
                        {entity, transform.Position, transform.Rotation, float(Turn) * unit(), false});
            }
        }
        PostProcessSettings post;
        post.Bloom.Enabled = true;
        scene.World->SetPostProcessSettings(post);
        return scene;
    }

    void ComplexSceneData::Advance(double time) const
    {
        for (const auto& motion : Animated)
        {
            const float phase  = float(time) + motion.Phase;
            auto        entity = motion.Target;
            entity.PatchComponent<TransformComponent>([&](auto& transform) {
                if (motion.Head)
                {
                    transform.Rotation.x = motion.Rotation.x + .3f * std::sin(1.7f * phase);
                    transform.Rotation.y = motion.Rotation.y + .8f * std::sin(1.1f * phase);
                }
                else
                {
                    transform.Position =
                        motion.Position + glm::vec3(.7f * std::sin(phase), .32f * std::sin(phase * 2), 0);
                    transform.Rotation.y = motion.Rotation.y + .45f * std::sin(phase);
                }
            });
        }
    }

    glm::vec3 OrbitPosition(const Bounds& bounds, double phase)
    {
        const auto   center    = bounds.IsValid() ? bounds.Center() : glm::vec3(0);
        const float  radius    = bounds.IsValid() ? (std::max)(bounds.Radius(), 2.f) : 2.f;
        const double angle     = phase * Turn;
        const float  distance  = radius * float(1.45 + 1.3 * std::cos(angle));
        const float  elevation = float(.28 + .10 * std::sin(2 * angle));
        return center + distance * glm::vec3(float(std::cos(angle)), elevation, float(std::sin(angle)));
    }
} // namespace Kans::Benchmark

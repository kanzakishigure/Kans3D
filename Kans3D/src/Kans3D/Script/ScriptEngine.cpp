#include "kspch.h"
#include "ScriptEngine.h"
#include "Kans3D/FileSystem/FileSystem.h"
#include "Kans3D/Physics/PhysicsWorld.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Entity.h"
#include "Kans3D/Scene/Scene.h"
#include "ScriptAgent.h"
#include "ScriptAnimation.h"
#include "ScriptTimeline.h"
#include "ScriptUI.h"
#if defined(_WIN32)
#include <Windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#if KANS_HAS_DOTNET
#include <coreclr_delegates.h>
#include <hostfxr.h>
#include <nethost.h>
#endif

namespace Kans
{
    namespace
    {
        Scripting::ManagedApi                  s_Api;
        Scripting::NativeApi                   s_Native;
        Scripting::InitializeFn                s_Initialize  = nullptr;
        Scene*                                 s_Scene       = nullptr;
        bool                                   s_Initialized = false;
        ScriptBackend                          s_Backend     = ScriptBackend::Disabled;
        std::unordered_map<uint64_t, uint64_t> s_Instances;
        std::vector<uint64_t>                  s_UpdateOrder;
        std::string                            s_Error;
        uint64_t                               s_UpdateTransitions = 0;
        std::filesystem::path                  ExecutableDirectory()
        {
#if defined(_WIN32)
            std::vector<wchar_t> path(32768);
            auto                 length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
            return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
#else
            return std::filesystem::canonical("/proc/self/exe").parent_path();
#endif
        }
        void* OpenLibrary(const std::filesystem::path& path)
        {
#if defined(_WIN32)
            return reinterpret_cast<void*>(LoadLibraryW(path.c_str()));
#else
            return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        }
        void* Symbol(void* library, const char* name)
        {
#if defined(_WIN32)
            return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
            return dlsym(library, name);
#endif
        }
        bool Fail(const std::string& message)
        {
            s_Error = message;
            CORE_WARN("Scripting disabled/error: {}", message);
            return false;
        }
#if KANS_HAS_DOTNET
        Scripting::InitializeFn HostedInitialize(const std::filesystem::path& assembly)
        {
            // Keep hostfxr and runtime delegates alive across engine shutdown/re-init.
            static Scripting::InitializeFn cached = nullptr;
            if (cached)
                return cached;
            std::filesystem::path  root = ExecutableDirectory() / "dotnet";
            std::vector<char_t>    hostPath(32768);
            size_t                 size = hostPath.size();
            get_hostfxr_parameters parameters {sizeof(get_hostfxr_parameters), nullptr, root.c_str()};
            int                    status = -1;
            if (std::filesystem::is_directory(root))
                status = get_hostfxr_path(hostPath.data(), &size, &parameters);
            if (status != 0)
            {
                root.clear();
                size   = hostPath.size();
                status = get_hostfxr_path(hostPath.data(), &size, nullptr);
            }
            if (status != 0)
                return nullptr;
            void* library = OpenLibrary(hostPath.data());
            if (!library)
                return nullptr;
            auto initialize = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
                Symbol(library, "hostfxr_initialize_for_runtime_config"));
            auto getDelegate =
                reinterpret_cast<hostfxr_get_runtime_delegate_fn>(Symbol(library, "hostfxr_get_runtime_delegate"));
            auto close = reinterpret_cast<hostfxr_close_fn>(Symbol(library, "hostfxr_close"));
            if (!initialize || !getDelegate || !close)
                return nullptr;
            auto config = assembly;
            config.replace_extension(".runtimeconfig.json");
            hostfxr_initialize_parameters initParameters {
                sizeof(hostfxr_initialize_parameters), nullptr, root.empty() ? nullptr : root.c_str()};
            hostfxr_handle context = nullptr;
            status                 = initialize(config.c_str(), &initParameters, &context);
            if (status < 0 || !context)
                return nullptr;
            load_assembly_fn        load = nullptr;
            get_function_pointer_fn get  = nullptr;
            status                       = getDelegate(context, hdt_load_assembly, reinterpret_cast<void**>(&load));
            if (status == 0)
                status = getDelegate(context, hdt_get_function_pointer, reinterpret_cast<void**>(&get));
            close(context);
            // .NET 8+ load_assembly loads into the default ALC. The older combined
            // load_assembly_and_get_function_pointer would create an isolated component ALC.
            if (status != 0 || !load || !get || load(assembly.c_str(), nullptr, nullptr) != 0)
                return nullptr;
#if defined(_WIN32)
            const char_t* type   = L"Kans.Interop.Host, ScriptCore";
            const char_t* method = L"Initialize";
#else
            const char_t* type   = "Kans.Interop.Host, ScriptCore";
            const char_t* method = "Initialize";
#endif
            status =
                get(type, method, UNMANAGEDCALLERSONLY_METHOD, nullptr, nullptr, reinterpret_cast<void**>(&cached));
            return status == 0 ? cached : nullptr;
        }
#endif
        void CollectUpdateTree(Scene* scene, Entity entity)
        {
            if (!scene->IsEntityActive(entity))
                return;
            auto instance = s_Instances.find(entity.GetUUID());
            if (instance != s_Instances.end())
                s_UpdateOrder.push_back(instance->second);
            for (auto child : scene->GetChildren(entity))
                CollectUpdateTree(scene, child);
        }
        void CollectUpdateOrder(Scene* scene)
        {
            s_UpdateOrder.clear();
            s_UpdateOrder.reserve(s_Instances.size());
            for (auto entity : scene->GetRootEntities())
                CollectUpdateTree(scene, entity);
        }
        void CreateTree(Scene* scene, Entity entity)
        {
            if (entity.HasComponent<ScriptComponent>())
                ScriptEngine::OnCreateEntity(entity);
            for (auto child : scene->GetChildren(entity))
                CreateTree(scene, child);
        }
    } // namespace

    bool ScriptEngine::Init(const ScriptInitOptions& options)
    {
        if (s_Initialized)
            return true;
        s_Error.clear();
        s_Backend = options.Backend;
        if (s_Backend == ScriptBackend::Disabled)
            return Fail("backend unavailable (disabled by init option)");
        try
        {
            if (s_Backend == ScriptBackend::CoreCLR)
            {
#if KANS_HAS_DOTNET
                auto assembly =
                    options.CoreAssembly.empty() ? KansFileSystem::GetScriptAssemblyPath() : options.CoreAssembly;
                if (assembly.empty() || !std::filesystem::exists(assembly))
                    return Fail("core assembly missing: " + assembly.string());
                s_Initialize = HostedInitialize(std::filesystem::absolute(assembly));
#else
                return Fail("dotnet was not available at configure time");
#endif
            }
            else
            {
                // NativeAOT runtime libraries cannot safely be unloaded. Keep each loaded
                // library mapped for process lifetime, just as we retain hosted CoreCLR.
                static std::unordered_map<std::string, void*> libraries;
                auto  path    = std::filesystem::absolute(options.AotLibrary).string();
                auto& library = libraries[path];
                if (!library)
                    library = OpenLibrary(path);
                if (!library)
                    return Fail("cannot load NativeAOT library: " + path);
                s_Initialize = reinterpret_cast<Scripting::InitializeFn>(Symbol(library, "kans_scripts_initialize"));
            }
            if (!s_Initialize)
                return Fail("runtime/bootstrap lookup failed");
            s_Native = ScriptAgent::CreateApi();
            if (options.Logger)
                s_Native.Log = options.Logger;
            if (options.KeyPressed)
                s_Native.IsKeyPressed = options.KeyPressed;
            s_Api = {};
            if (s_Initialize(&s_Native, sizeof(s_Native), &s_Api, sizeof(s_Api)) != 0 ||
                s_Api.Version != Scripting::ApiVersion || s_Api.Size != sizeof(s_Api))
                return Fail("function table ABI mismatch");
            s_Initialized = true;
            return true;
        }
        catch (const std::exception& error)
        {
            return Fail(error.what());
        }
    }
    void ScriptEngine::ShutDown()
    {
        if (s_Initialized)
        {
            OnRuntimeEnd();
            s_Api.Shutdown();
        }
        s_Initialized = false;
        s_Api         = {};
        s_Initialize  = nullptr;
    }
    bool ScriptEngine::IsInitialized() { return s_Initialized; }
    bool ScriptEngine::LoadAppAssembly(const std::filesystem::path& path)
    {
        if (!s_Initialized)
            return Fail("backend is not initialized");
        if (s_Backend == ScriptBackend::NativeAot)
            return Fail("NativeAOT uses linked scripts; assembly loading is unsupported");
        if (s_Scene)
            return Fail("assembly loading is forbidden during Play");
        return s_Api.LoadAssembly(std::filesystem::absolute(path).u8string().c_str()) == 0 ||
               Fail("user assembly load failed; previous registry retained");
    }
    bool ScriptEngine::ReloadAppAssembly()
    {
        if (!s_Initialized)
            return Fail("backend is not initialized");
        if (s_Backend == ScriptBackend::NativeAot)
            return Fail("NativeAOT reload is unsupported");
        if (s_Scene)
            return Fail("reload is forbidden during Play");
        return s_Api.ReloadAssembly() == 0 || Fail("reload failed; previous assembly retained");
    }
    bool ScriptEngine::EntityClassExists(const std::string& name)
    {
        return s_Initialized && s_Api.ClassExists(name.c_str()) != 0;
    }
    std::vector<std::string> ScriptEngine::GetEntityClasses()
    {
        std::vector<std::string> result;
        if (!s_Initialized)
            return result;
        int count = s_Api.ClassCount();
        for (int i = 0; i < count; ++i)
        {
            int size = s_Api.ClassName(i, nullptr, 0);
            if (size <= 0)
                continue;
            std::string name(size, '\0');
            if (s_Api.ClassName(i, name.data(), size) == size)
            {
                name.pop_back();
                result.push_back(std::move(name));
            }
        }
        return result;
    }
    void ScriptEngine::OnRuntimeStart(Scene* scene)
    {
        if (!s_Initialized || s_Scene)
            return;
        ScriptUI::Reset();
        ScriptAnimation::Reset();
        ScriptTimeline::Reset();
        scene->UI().Reset();
        s_Scene = scene;
        s_Api.SetPlaying(1);
        for (auto entity : scene->GetRootEntities())
            CreateTree(scene, entity);
    }
    void ScriptEngine::OnRuntimeEnd()
    {
        if (s_Initialized)
            s_Api.SetPlaying(0);
        ScriptUI::Reset();
        ScriptAnimation::Reset();
        ScriptTimeline::Reset();
        if (s_Scene)
            s_Scene->UI().Reset();
        s_Instances.clear();
        s_Scene = nullptr;
    }
    void ScriptEngine::OnCreateEntity(Entity entity)
    {
        if (!s_Initialized || !s_Scene || s_Instances.count(entity.GetUUID()))
            return;
        auto handle = s_Api.Create(entity.GetUUID(), entity.GetComponent<ScriptComponent>().ClassName.c_str());
        if (handle)
            s_Instances.emplace(entity.GetUUID(), handle);
    }
    void ScriptEngine::OnDestroyEntity(Entity entity)
    {
        if (!s_Initialized || !s_Scene)
            return;
        auto found = s_Instances.find(entity.GetUUID());
        if (found != s_Instances.end())
        {
            s_Api.Destroy(found->second);
            s_Instances.erase(found);
        }
    }
    void ScriptEngine::OnUpdate(TimeStep ts)
    {
        if (s_Initialized && s_Scene)
        {
            ScriptUI::BeginFrame();
            ScriptAnimation::BeginFrame();
            ScriptTimeline::BeginFrame();
            ++s_UpdateTransitions;
            CollectUpdateOrder(s_Scene);
            s_Api.Update(s_UpdateOrder.data(), int32_t(s_UpdateOrder.size()), float(ts));
        }
    }
    void ScriptEngine::OnFixedUpdate(Scene& scene, float dt)
    {
        if (!s_Initialized || s_Scene != &scene)
            return;
        CollectUpdateOrder(&scene);
        s_Api.FixedUpdate(s_UpdateOrder.data(), int32_t(s_UpdateOrder.size()), dt);
    }
    void ScriptEngine::OnPhysicsEvent(Scene& scene, const PhysicsPairEvent& pair, bool trigger)
    {
        if (!s_Initialized || s_Scene != &scene)
            return;
        auto dispatch = [&](bool reverse) {
            UUID           body  = reverse ? pair.BodyB : pair.BodyA;
            UUID           other = reverse ? pair.BodyA : pair.BodyB;
            std::set<UUID> recipients {body};
            for (auto [a, b] : pair.Colliders)
                recipients.insert(reverse ? b : a);
            for (auto id : recipients)
            {
                auto entity   = scene.GetEntityByUUID(id);
                auto instance = s_Instances.find(id);
                if (!entity || !scene.IsEntityActive(entity) || instance == s_Instances.end())
                    continue;
                uint32_t mask = 1u << (uint32_t(pair.Phase) + (trigger ? 4 : 1));
                if (!(s_Api.PhysicsCallbacks(instance->second) & mask))
                    continue;
                // One reusable native contact buffer. Borrowed only for this synchronous call, never retained by C#.
                static thread_local std::vector<Scripting::PhysicsContactData> contacts;
                contacts.clear();
                float sign   = reverse ? -1.f : 1.f;
                auto  vector = [](glm::vec3 v) { return Scripting::Vector3 {v.x, v.y, v.z}; };
                for (const auto& c : pair.Contacts)
                    contacts.push_back({vector(c.Position),
                                        vector(c.Normal * sign),
                                        vector(c.Impulse * sign),
                                        c.Separation,
                                        uint64_t(reverse ? c.ColliderB : c.ColliderA),
                                        uint64_t(reverse ? c.ColliderA : c.ColliderB)});
                Scripting::PhysicsEventData data;
                data.Other         = other;
                data.Collider      = reverse ? pair.ColliderB : pair.ColliderA;
                data.OtherCollider = reverse ? pair.ColliderA : pair.ColliderB;
                for (auto [a, b] : pair.Colliders)
                    if ((reverse ? b : a) == id)
                    {
                        data.Collider      = id;
                        data.OtherCollider = reverse ? a : b;
                        break;
                    }
                data.Impulse          = vector(pair.TotalImpulse * sign);
                data.RelativeVelocity = vector(pair.RelativeVelocity * sign);
                data.Contacts         = contacts.data();
                data.Count            = int32_t(contacts.size());
                data.Phase            = int32_t(pair.Phase);
                data.Trigger          = trigger;
                data.Removed          = pair.Removed;
                s_Api.PhysicsEvent(instance->second, &data);
            }
        };
        dispatch(false);
        dispatch(true);
    }
    Scene*             ScriptEngine::GetSceneContext() { return s_Scene; }
    uint64_t           ScriptEngine::GetUpdateTransitionCount() { return s_UpdateTransitions; }
    int64_t            ScriptEngine::GetManagedAllocatedBytes() { return s_Initialized ? s_Api.AllocatedBytes() : -1; }
    int                ScriptEngine::LastUnloadCollected() { return s_Initialized ? s_Api.LastUnloadCollected() : -1; }
    const std::string& ScriptEngine::GetLastError() { return s_Error; }
    bool               ScriptEngine::CheckApiCompatibility(uint32_t version, int nativeSize, int managedSize)
    {
        if (!s_Initialize)
            return false;
        auto native    = s_Native;
        native.Version = version;
        Scripting::ManagedApi managed;
        return s_Initialize(&native, nativeSize, &managed, managedSize) == 0;
    }
} // namespace Kans

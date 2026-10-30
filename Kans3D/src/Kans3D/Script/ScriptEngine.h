#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include "Kans3D/Core/TimeStep.h"
#include "Kans3D/Core/UUID.h"
#include "ScriptInterop.h"

namespace Kans
{
    class Entity;
    class Scene;
    struct PhysicsPairEvent;
    struct ControllerColliderHit;
    struct JointBreakEvent;
    enum class ScriptBackend
    {
        CoreCLR,
        NativeAot,
        Disabled
    };
    struct ScriptInitOptions
    {
        ScriptBackend         Backend = ScriptBackend::CoreCLR;
        std::filesystem::path CoreAssembly;
        std::filesystem::path AotLibrary;
        // Optional logger/input adapters for headless consumers. Native callbacks must not throw.
        void(KANS_SCRIPT_CALL* Logger)(int32_t, const char*) = nullptr;
        int32_t(KANS_SCRIPT_CALL* KeyPressed)(int32_t)       = nullptr;
    };

    // Runtime lookup: first <executable directory>/dotnet (nethost hostfxr path lookup
    // AND hostfxr_initialize_parameters.dotnet_root), then nethost's system installation.
    // CoreCLR and the default-context API assembly live for the process lifetime. Shutdown
    // releases script instances and collectible user contexts; re-init reuses CoreCLR.
    // API calls, scene callbacks and reload run on the engine's main thread.
    class ScriptEngine
    {
    public:
        static bool                     Init(const ScriptInitOptions& options = {});
        static void                     ShutDown();
        static bool                     IsInitialized();
        static bool                     LoadAppAssembly(const std::filesystem::path& path);
        static bool                     ReloadAppAssembly();
        static bool                     EntityClassExists(const std::string& name);
        static std::vector<std::string> GetEntityClasses();
        static void                     OnRuntimeStart(Scene* scene);
        static void                     OnRuntimeEnd();
        static void                     OnCreateEntity(Entity entity);
        static void                     OnDestroyEntity(Entity entity);
        static void                     OnUpdate(TimeStep ts);
        static void                     OnFixedUpdate(Scene&, float);
        static void                     OnControllerHit(Scene&, UUID, const ControllerColliderHit&);
        static void                     OnJointBreak(Scene&, const JointBreakEvent&);
        static float                    GetPhysicsDeltaTime();
        static void                     OnPhysicsEvent(Scene&, const PhysicsPairEvent&, bool trigger);
        static Scene*                   GetSceneContext();
        static uint64_t                 GetUpdateTransitionCount();
        static int64_t                  GetManagedAllocatedBytes();
        static int                      LastUnloadCollected();
        static const std::string&       GetLastError();
        // Exercises the actual bootstrap validator without replacing the active API table.
        static bool CheckApiCompatibility(uint32_t version, int nativeSize, int managedSize);
    };
} // namespace Kans

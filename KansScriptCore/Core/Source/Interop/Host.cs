using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using System.Threading;

namespace Kans.Interop
{
    public static unsafe class Host
    {
        private sealed class Instance
        {
            internal GCHandle Root;
            internal ulong Handle;
            internal string Class = "";
            internal bool Disabled;
            internal uint PhysicsMask;
            internal Entity? PhysicsOther, PhysicsCollider, PhysicsOtherCollider;
            internal Joint? BrokenJoint;
        }
        private static readonly List<Instance> s_Instances = new();
        private static readonly Dictionary<ulong, Instance> s_Handles = new();
        private static ulong s_NextHandle;
        private static bool s_Playing;
        private static IScriptRegistry? s_Registry;
        private static ScriptContext? s_Context;
        private static string? s_Path;
        // The loaded user assembly, not the context object: only Unload() releases the assembly.
        private static Assembly? s_Assembly;
        private static WeakReference? s_Unloaded;

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int Initialize(NativeApi* native, int nativeSize, ManagedApi* managed, int managedSize)
            => InitializeTables(native, nativeSize, managed, managedSize);

        public static int InitializeTables(NativeApi* native, int nativeSize, ManagedApi* managed, int managedSize)
        {
            if (native == null || managed == null || nativeSize != sizeof(NativeApi) || managedSize != sizeof(ManagedApi) ||
                native->Version != NativeApi.ApiVersion || native->Size != sizeof(NativeApi) ||
                managed->Version != NativeApi.ApiVersion || managed->Size != sizeof(ManagedApi)) return -1;
            if (native->Log == null || native->HasComponent == null || native->GetTranslation == null ||
                native->SetTranslation == null || native->GetRotation == null || native->SetRotation == null || native->IsKeyPressed == null ||
                native->SetMaterialParameter == null || native->ClearMaterialParameter == null || native->GetMaterialParameter == null || native->AddComponent == null || native->DestroyEntity == null ||
                native->ComponentProperty == null || native->WidgetProperty == null || native->ReadUIEvent == null || native->UIUtility == null || native->AnimationCommand == null || native->ReadAnimationEvent == null || native->TimelineCommand == null || native->ReadTimelineEvent == null || native->PhysicsCommand == null || native->PhysicsQuery == null) return -1;
            InternalCalls.Api = *native;
            *managed = new ManagedApi {
                Version = NativeApi.ApiVersion, Size = (uint)sizeof(ManagedApi), Shutdown = &Shutdown, LoadAssembly = &LoadAssembly,
                ReloadAssembly = &ReloadAssembly, ClassCount = &ClassCount, ClassName = &ClassName,
                ClassExists = &ClassExists, Create = &Create, Destroy = &Destroy, Update = &Update,
                SetPlaying = &SetPlaying, AllocatedBytes = &AllocatedBytes, LastUnloadCollected = &LastUnloadCollected, FixedUpdate = &FixedUpdate, PhysicsEvent = &PhysicsEvent, PhysicsCallbacks = &PhysicsCallbacks, ControllerHit = &ControllerHit, JointBreak = &JointBreak
            };
            if (!RuntimeFeature.IsDynamicCodeSupported) s_Registry = ScriptRegistry.Pending;
            return 0;
        }

        private static void Error(Exception exception, Entity? entity = null, string? name = null, ulong id = 0)
        {
            try { Log.Error($"Script exception entity={entity?.ID ?? id} class={name ?? "<host>"}: {exception}"); }
            catch { /* No exception, including a logger failure, may cross an unmanaged boundary. */ }
        }
        internal static void InvokeTimeline(ulong target, string method, string payload)
        {
            foreach (var instance in s_Instances.ToArray())
            {
                if (instance.Disabled || !instance.Root.IsAllocated || instance.Root.Target is not Entity e || e.ID != target) continue;
                using var scope = new UI.ScriptScope(instance.Handle);
                try { s_Registry?.InvokeTimeline(e, method, payload); }
                catch (Exception ex) { Error(ex, e, instance.Class); }
            }
        }
        private static string Read(byte* text) => Marshal.PtrToStringUTF8((nint)text) ?? "";

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void Shutdown()
        {
            try
            {
                EndInstances();
                s_Playing = false;
                if (RuntimeFeature.IsDynamicCodeSupported)
                {
                    DetachContext();
                    CollectOldContext();
                    s_Path = null;
                }
            }
            catch (Exception e) { Error(e); }
        }

        private static void EndInstances()
        {
            UI.SetPlaying(false);
            AnimationEvents.Reset();
            TimelineEvents.Reset();
            foreach (var instance in s_Instances.ToArray()) Release(instance);
            s_Instances.Clear();
            s_Handles.Clear();
        }
        private static void Release(Instance instance)
        {
            if (!instance.Root.IsAllocated) return;
            var entity = (Entity)instance.Root.Target!;
            using var scope = new UI.ScriptScope(instance.Handle);
            try { if (!instance.Disabled) entity.DestroyScript(); }
            catch (Exception e) { Error(e, entity, instance.Class); }
            finally { UI.RemoveOwner(instance.Handle); AnimationEvents.RemoveOwner(instance.Handle); TimelineEvents.RemoveOwner(instance.Handle); instance.Root.Free(); }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static int LoadAssembly(byte* path)
        {
            try
            {
                if (s_Playing || !RuntimeFeature.IsDynamicCodeSupported) return -1;
                return LoadHosted(Read(path));
            }
            catch (Exception e) { Error(e); return -1; }
        }
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static int ReloadAssembly()
        {
            try
            {
                if (s_Playing || !RuntimeFeature.IsDynamicCodeSupported || s_Path == null) return -1;
                return LoadHosted(s_Path);
            }
            catch (Exception e) { Error(e); return -1; }
        }

        // Dynamic loading is confined to the hosted cold path. NativeAOT removes this branch.
        [UnconditionalSuppressMessage("Trimming", "IL2026", Justification = "Only called when dynamic code is supported; JIT assemblies are not trimmed.")]
        [UnconditionalSuppressMessage("AOT", "IL3050", Justification = "Only called when RuntimeFeature.IsDynamicCodeSupported.")]
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int LoadHosted(string path)
        {
            path = Path.GetFullPath(path);
            var candidate = new ScriptContext(path);
            try
            {
                ScriptRegistry.Pending = null;
                using var bytes = new MemoryStream(File.ReadAllBytes(path));
                Assembly assembly = candidate.LoadFromStream(bytes);
                // Forces the generated module initializer, without scanning types or methods.
                RuntimeHelpers.RunModuleConstructor(assembly.ManifestModule.ModuleHandle);
                var registry = ScriptRegistry.Pending ?? throw new InvalidOperationException("No generated script registry. Reference KansScriptCore.Generators as an analyzer.");
                ScriptRegistry.Pending = null;
                EndInstances();
                DetachContext();
                s_Context = candidate;
                s_Assembly = assembly;
                s_Registry = registry;
                s_Path = path;
            }
            catch
            {
                ScriptRegistry.Pending = null;
                candidate.Unload();
                throw;
            }
            CollectOldContext();
            return 0;
        }

        private sealed class ScriptContext : AssemblyLoadContext
        {
            private readonly AssemblyDependencyResolver m_Resolver;
            internal ScriptContext(string path) : base(isCollectible: true) { m_Resolver = new(path); }
            [UnconditionalSuppressMessage("Trimming", "IL2026", Justification = "Only used by the JIT-only collectible loader.")]
            [UnconditionalSuppressMessage("AOT", "IL3050", Justification = "Only used by the JIT-only collectible loader.")]
            protected override Assembly? Load(AssemblyName name)
            {
                if (name.Name == typeof(Entity).Assembly.GetName().Name) return typeof(Entity).Assembly;
                string? path = m_Resolver.ResolveAssemblyToPath(name);
                // Stream loading avoids locks on Windows and allows the next build to replace DLLs.
                if (path == null) return null;
                using var bytes = new MemoryStream(File.ReadAllBytes(path));
                return LoadFromStream(bytes);
            }
            protected override nint LoadUnmanagedDll(string name)
            {
                string? path = m_Resolver.ResolveUnmanagedDllToPath(name);
                return path == null ? 0 : LoadUnmanagedDllFromPath(path);
            }
        }
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static void DetachContext()
        {
            s_Registry = null;
            ScriptRegistry.Pending = null;
            if (s_Context == null) return;
            s_Unloaded = new WeakReference(s_Assembly ?? (object)s_Context);
            s_Context.Unload();
            s_Context = null;
            s_Assembly = null;
        }
        private static void CollectOldContext()
        {
            for (int attempt = 0; s_Unloaded?.IsAlive == true && attempt < 10; ++attempt)
            {
                // Do not wait indefinitely for script finalizers; an uncooperative finalizer
                // must result in a warning instead of blocking the editor reload.
                GC.Collect();
                if (s_Unloaded.IsAlive) Thread.Sleep(10);
            }
            if (s_Unloaded?.IsAlive == true) Log.Warn("Previous script AssemblyLoadContext is still alive after bounded collection.");
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static int ClassCount() { try { return s_Registry?.Names.Length ?? 0; } catch (Exception e) { Error(e); return 0; } }
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static int ClassName(int index, byte* buffer, int capacity)
        {
            try
            {
                string name = s_Registry!.Names[index];
                int size = Encoding.UTF8.GetByteCount(name) + 1;
                if (buffer != null && capacity >= size)
                {
                    Encoding.UTF8.GetBytes(name, new Span<byte>(buffer, capacity - 1));
                    buffer[size - 1] = 0;
                }
                return size;
            }
            catch (Exception e) { Error(e); return -1; }
        }
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static int ClassExists(byte* name)
        {
            try { return Array.IndexOf(s_Registry?.Names ?? Array.Empty<string>(), Read(name)) >= 0 ? 1 : 0; }
            catch (Exception e) { Error(e); return 0; }
        }
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ulong Create(ulong id, byte* name)
        {
            Entity? entity = null;
            string? className = null;
            Instance? instance = null;
            try
            {
                if (!s_Playing) return 0;
                className = Read(name);
                entity = s_Registry?.Create(className);
                if (entity == null) return 0;
                entity.Attach(id);
                instance = new Instance { Root = GCHandle.Alloc(entity), Handle = ++s_NextHandle, Class = className };
                instance.PhysicsMask = s_Registry!.PhysicsCallbacks(entity);
                if ((instance.PhysicsMask & ~1u) != 0)
                { instance.PhysicsOther = new Entity(); instance.PhysicsCollider = new Entity(); instance.PhysicsOtherCollider = new Entity(); }
                if ((instance.PhysicsMask & (1u << 8)) != 0) instance.BrokenJoint = new Joint { Entity = entity };
                s_Instances.Add(instance);
                s_Handles.Add(instance.Handle, instance);
                using var scope = new UI.ScriptScope(instance.Handle);
                entity.Create();
                return instance.Handle;
            }
            catch (Exception e)
            {
                if (instance != null) { instance.Disabled = true; UI.RemoveOwner(instance.Handle); AnimationEvents.RemoveOwner(instance.Handle); TimelineEvents.RemoveOwner(instance.Handle); }
                Error(e, entity, className, id);
                return instance?.Handle ?? 0;
            }
        }
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void Destroy(ulong handle)
        {
            try
            {
                if (!s_Handles.Remove(handle, out var instance)) return;
                s_Instances.Remove(instance);
                Release(instance);
            }
            catch (Exception e) { Error(e); }
        }
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void Update(ulong* handles, int count, float ts)
        {
            try
            {
                if (!s_Playing) return;
                UI.Drain();
                AnimationEvents.Drain();
                TimelineEvents.Drain();
                if (!s_Playing) return;
                // Native supplies handles in current scene order; dictionary lookups allocate nothing.
                for (int i = 0; i < count; ++i)
                {
                    if (!s_Handles.TryGetValue(handles[i], out var instance)) continue;
                    if (instance.Disabled) continue;
                    var entity = (Entity)instance.Root.Target!;
                    using var scope = new UI.ScriptScope(instance.Handle);
                    try { entity.Update(ts); }
                    catch (Exception e) { instance.Disabled = true; UI.RemoveOwner(instance.Handle); AnimationEvents.RemoveOwner(instance.Handle); TimelineEvents.RemoveOwner(instance.Handle); Error(e, entity, instance.Class); }
                }
            }
            catch (Exception e) { Error(e); }
        }
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static uint PhysicsCallbacks(ulong handle)
        { try { return s_Handles.TryGetValue(handle, out var i) && !i.Disabled ? i.PhysicsMask : 0; } catch { return 0; } }
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void FixedUpdate(ulong* handles, int count, float dt)
        {
            try
            {
                if (!s_Playing) return;
                for (int n = 0; n < count && s_Playing; ++n)
                {
                    if (!s_Handles.TryGetValue(handles[n], out var i) || i.Disabled || (i.PhysicsMask & 1) == 0) continue;
                    var e = (Entity)i.Root.Target!;
                    using var scope = new UI.ScriptScope(i.Handle);
                    try { e.FixedUpdate(dt); } catch (Exception ex) { i.Disabled = true; Error(ex,e,i.Class); }
                }
            } catch (Exception ex) { Error(ex); }
        }
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void PhysicsEvent(ulong handle, PhysicsEventData* data)
        {
            try
            {
                if (!s_Playing || data == null || !s_Handles.TryGetValue(handle,out var i) || i.Disabled) return;
                uint bit = 1u << (data->Phase + (data->Trigger != 0 ? 4 : 1));
                if ((i.PhysicsMask & bit) == 0) return;
                var e = (Entity)i.Root.Target!;
                using var scope = new UI.ScriptScope(i.Handle);
                i.PhysicsOther!.Attach(data->Other); i.PhysicsCollider!.Attach(data->Collider); i.PhysicsOtherCollider!.Attach(data->OtherCollider);
                try
                {
                    if (data->Trigger != 0) e.TriggerEvent(data->Phase,i.PhysicsOther);
                    else e.PhysicsEvent(data->Phase,new Collision(i.PhysicsOther,i.PhysicsCollider,i.PhysicsOtherCollider,
                        new ReadOnlySpan<ContactPoint>(data->Contacts,data->Count),data->Impulse,data->RelativeVelocity,data->Removed!=0));
                } catch (Exception ex) { i.Disabled = true; Error(ex,e,i.Class); }
            } catch (Exception ex) { Error(ex); }
        }
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void ControllerHit(ulong handle, ControllerHitData* data)
        {
            try
            {
                if (!s_Playing || data == null || !s_Handles.TryGetValue(handle,out var i) || i.Disabled || (i.PhysicsMask & (1u << 7)) == 0) return;
                var e = (Entity)i.Root.Target!;
                using var scope = new UI.ScriptScope(i.Handle);
                i.PhysicsOther!.Attach(data->Body); i.PhysicsCollider!.Attach(data->Collider);
                try { e.ControllerHit(new ControllerColliderHit(i.PhysicsCollider,i.PhysicsOther,data->Point,data->Normal,data->MoveDirection,data->MoveLength)); }
                catch (Exception ex) { i.Disabled = true; Error(ex,e,i.Class); }
            } catch (Exception ex) { Error(ex); }
        }
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void JointBreak(ulong handle, ulong id)
        {
            try
            {
                if (!s_Playing || !s_Handles.TryGetValue(handle,out var i) || i.Disabled || (i.PhysicsMask & (1u << 8)) == 0) return;
                var e = (Entity)i.Root.Target!;
                using var scope = new UI.ScriptScope(i.Handle);
                try { e.JointBreak(i.BrokenJoint!); } catch (Exception ex) { i.Disabled = true; Error(ex,e,i.Class); }
            } catch (Exception ex) { Error(ex); }
        }
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void SetPlaying(int playing)
        {
            try
            {
                s_Playing = playing != 0;
                if (!s_Playing) EndInstances();
                else UI.SetPlaying(true);
            }
            catch (Exception e) { Error(e); }
        }
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static long AllocatedBytes() => GC.GetAllocatedBytesForCurrentThread();
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static int LastUnloadCollected() => s_Unloaded == null ? -1 : s_Unloaded.IsAlive ? 0 : 1;
    }
}

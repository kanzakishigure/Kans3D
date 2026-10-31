using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kans.Interop
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct PropertyValue
    {
        public float X, Y, Z, W;
        public long Integer;
    }
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct WidgetAddress
    {
        public ulong Entity;
        public int Screen, Reserved;
        public byte* Path;
        public byte* Key;
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct UIEventData
    {
        public ulong Entity;
        public int Screen, Type, ValueKind, Reserved;
        public PropertyValue Payload;
        public float PositionX, PositionY, DeltaX, DeltaY;
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct AnimationEventData
    {
        public ulong Entity;
        public double Time;
        public int Kind, Interrupted;
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct TimelineEventData
    {
        public ulong Player, Target;
        public double Time;
        public int Finished, Reserved;
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct PhysicsQueryData
    {
        public uint Kind, LayerMask, Flags, Triggers;
        public ulong Ignore;
        public Vector3 Origin, Direction, HalfExtents;
        public float Distance, Radius, HalfHeight;
        public float RotationX, RotationY, RotationZ, RotationW;
        public uint Axis;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct ControllerHitData
    {
        public ulong Collider, Body;
        public Vector3 Point, Normal, MoveDirection;
        public float MoveLength;
    }
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct PhysicsEventData
    {
        public ulong Other, Collider, OtherCollider;
        public Vector3 Impulse, RelativeVelocity;
        public ContactPoint* Contacts;
        public int Count, Phase, Trigger, Removed;
    }
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct NativeApi
    {
        public const uint ApiVersion = 10;
        public uint Version, Size;
        public delegate* unmanaged[Cdecl]<int, byte*, void> Log;
        public delegate* unmanaged[Cdecl]<ulong, uint, int> HasComponent;
        public delegate* unmanaged[Cdecl]<ulong, Vector3*, void> GetTranslation;
        public delegate* unmanaged[Cdecl]<ulong, Vector3*, void> SetTranslation;
        public delegate* unmanaged[Cdecl]<ulong, Vector3*, void> GetRotation;
        public delegate* unmanaged[Cdecl]<ulong, Vector3*, void> SetRotation;
        public delegate* unmanaged[Cdecl]<int, int> IsKeyPressed;
        public delegate* unmanaged[Cdecl]<ulong, byte*, float*, void> SetMaterialParameter;
        public delegate* unmanaged[Cdecl]<ulong, byte*, void> ClearMaterialParameter;
        public delegate* unmanaged[Cdecl]<ulong, byte*, float*, int> GetMaterialParameter;
        internal delegate* unmanaged[Cdecl]<ulong, uint, int> AddComponent;
        internal delegate* unmanaged[Cdecl]<ulong, int> DestroyEntity;
        internal delegate* unmanaged[Cdecl]<ulong, uint, byte*, int, PropertyValue*, byte*, int, int> ComponentProperty;
        internal delegate* unmanaged[Cdecl]<WidgetAddress*, byte*, int, PropertyValue*, byte*, int, int> WidgetProperty;
        internal delegate* unmanaged[Cdecl]<int, UIEventData*, int, byte*, int, int> ReadUIEvent;
        internal delegate* unmanaged[Cdecl]<int, WidgetAddress*, PropertyValue*, int> UIUtility;
        internal delegate* unmanaged[Cdecl]<ulong, int, byte*, byte*, byte*, byte*, float, float, float, int> AnimationCommand;
        internal delegate* unmanaged[Cdecl]<int, AnimationEventData*, int, byte*, int, int> ReadAnimationEvent;
        internal delegate* unmanaged[Cdecl]<ulong, int, byte*, ulong, double*, int> TimelineCommand;
        internal delegate* unmanaged[Cdecl]<int, TimelineEventData*, int, byte*, int, int> ReadTimelineEvent;
        internal delegate* unmanaged[Cdecl]<ulong, int, int, PropertyValue*, Vector3*, byte*, int> PhysicsCommand;
        internal delegate* unmanaged[Cdecl]<PhysicsQueryData*, RaycastHit*, int, int, int> PhysicsQuery;
    }
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ManagedApi
    {
        public uint Version, Size;
        public delegate* unmanaged[Cdecl]<void> Shutdown;
        public delegate* unmanaged[Cdecl]<byte*, int> LoadAssembly;
        public delegate* unmanaged[Cdecl]<int> ReloadAssembly;
        public delegate* unmanaged[Cdecl]<int> ClassCount;
        public delegate* unmanaged[Cdecl]<int, byte*, int, int> ClassName;
        public delegate* unmanaged[Cdecl]<byte*, int> ClassExists;
        public delegate* unmanaged[Cdecl]<ulong, byte*, ulong> Create;
        public delegate* unmanaged[Cdecl]<ulong, void> Destroy;
        public delegate* unmanaged[Cdecl]<ulong*, int, float, void> Update;
        public delegate* unmanaged[Cdecl]<int, void> SetPlaying;
        // Diagnostic hooks, also useful for validating packaged builds.
        public delegate* unmanaged[Cdecl]<long> AllocatedBytes;
        public delegate* unmanaged[Cdecl]<int> LastUnloadCollected;
        public delegate* unmanaged[Cdecl]<ulong*, int, float, void> FixedUpdate;
        public delegate* unmanaged[Cdecl]<ulong, PhysicsEventData*, void> PhysicsEvent;
        public delegate* unmanaged[Cdecl]<ulong, uint> PhysicsCallbacks;
        public delegate* unmanaged[Cdecl]<ulong, ControllerHitData*, void> ControllerHit;
        public delegate* unmanaged[Cdecl]<ulong, ulong, void> JointBreak;
    }

    public interface IScriptRegistry
    {
        string[] Names { get; }
        Entity? Create(string name);
        bool InvokeTimeline(Entity target, string method, string payload) => false;
        uint PhysicsCallbacks(Entity target) => 0;
    }
    public static class ScriptRegistry
    {
        [System.ThreadStatic] internal static IScriptRegistry? Pending;
        // A module initializer in each generated user assembly registers its static factories.
        public static void Register(IScriptRegistry registry) => Pending = registry;
    }
}

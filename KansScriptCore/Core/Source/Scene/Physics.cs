using System;
using System.Runtime.InteropServices;
using System.Text;
using Kans.Interop;

namespace Kans
{
    public enum RigidBodyType { Static, Dynamic, Kinematic }
    public enum ForceMode { Force, Impulse, VelocityChange, Acceleration }
    public enum QueryTriggerInteraction : uint { UseGlobal, Ignore, Collide }
    public enum CapsuleAxis { X, Y, Z }
    [Flags] public enum RigidbodyLocks { None=0, PositionX=1, PositionY=2, PositionZ=4, RotationX=8, RotationY=16, RotationZ=32 }
    public struct QueryFilter
    {
        public uint LayerMask;
        public QueryTriggerInteraction Triggers;
        public Entity? IgnoreEntity;
        public bool IncludeStatic, IncludeDynamic, IncludeKinematic;
        public QueryFilter() { LayerMask=uint.MaxValue; Triggers=QueryTriggerInteraction.UseGlobal; IgnoreEntity=null; IncludeStatic=IncludeDynamic=IncludeKinematic=true; }
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct RaycastHit
    {
        public ulong ColliderID, BodyID;
        public Vector3 Point, Normal;
        public float Distance;
        public uint FaceIndex;
        public uint Flags, Reserved;
        public bool InitialOverlap => (Flags & 1) != 0;
        public Entity Collider => new Entity(ColliderID);
        public Entity Body => new Entity(BodyID);
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct ContactPoint
    {
        public Vector3 Position, Normal, Impulse;
        public float Separation;
        public ulong ColliderID, OtherColliderID;
    }
    /// <summary>Borrowed contact data and entity wrappers are valid only during the callback. Copy values to retain them.</summary>
    public readonly ref struct Collision
    {
        public readonly Entity OtherEntity, Collider, OtherCollider;
        public readonly ReadOnlySpan<ContactPoint> Contacts;
        public readonly Vector3 Impulse, RelativeVelocity;
        public readonly bool Removed;
        internal Collision(Entity other, Entity collider, Entity otherCollider, ReadOnlySpan<ContactPoint> contacts, Vector3 impulse, Vector3 velocity, bool removed)
        { OtherEntity=other; Collider=collider; OtherCollider=otherCollider; Contacts=contacts; Impulse=impulse; RelativeVelocity=velocity; Removed=removed; }
    }
    internal static unsafe class PhysicsCalls
    {
        internal static PropertyValue Read(ulong id, int op, int mode=0)
        { PropertyValue v=default; Check(InternalCalls.Api.PhysicsCommand(id,op,mode,&v,null,null)); return v; }
        internal static void Write(ulong id, int op, PropertyValue v, int mode=1024, Vector3 second=default, string? text=null)
        {
            if (text?.Contains('\0') == true) throw new ArgumentException("Asset reference cannot contain NUL.");
            byte[]? bytes=text==null ? null : Encoding.UTF8.GetBytes(text+"\0");
            fixed(byte* p=bytes) Check(InternalCalls.Api.PhysicsCommand(id,op,mode,&v,&second,p));
        }
        private static void Check(int result) { if(result<0) throw new ArgumentException("Invalid physics operation or value."); }
        internal static PropertyValue V(Vector3 v) => new() { X=v.X,Y=v.Y,Z=v.Z };
        internal static Vector3 V(PropertyValue v) => new(v.X,v.Y,v.Z);
        internal static PropertyValue Q(Quaternion v) => new() { X=v.X,Y=v.Y,Z=v.Z,W=v.W };
        internal static string Asset(PropertyValue v)
        {
            ulong local=*(ulong*)&v.X;
            return v.Integer==0 || local==0 ? "" : $"{unchecked((ulong)v.Integer):x16}:{local:x16}";
        }
    }
    public class Rigidbody : Component
    {
        public RigidBodyType Type { get => (RigidBodyType)PhysicsCalls.Read(Entity.ID,10).Integer; set => PhysicsCalls.Write(Entity.ID,10,new(){Integer=(long)value}); }
        public float Mass { get => PhysicsCalls.Read(Entity.ID,11).X; set => PhysicsCalls.Write(Entity.ID,11,new(){X=value}); }
        public float LinearDamping { get => PhysicsCalls.Read(Entity.ID,12).X; set => PhysicsCalls.Write(Entity.ID,12,new(){X=value}); }
        public float AngularDamping { get => PhysicsCalls.Read(Entity.ID,13).X; set => PhysicsCalls.Write(Entity.ID,13,new(){X=value}); }
        public float GravityScale { get => PhysicsCalls.Read(Entity.ID,14).X; set => PhysicsCalls.Write(Entity.ID,14,new(){X=value}); }
        /// <summary>World velocity in units per second.</summary>
        public Vector3 LinearVelocity { get => PhysicsCalls.V(PhysicsCalls.Read(Entity.ID,15)); set => PhysicsCalls.Write(Entity.ID,15,PhysicsCalls.V(value)); }
        /// <summary>World angular velocity in radians per second.</summary>
        public Vector3 AngularVelocity { get => PhysicsCalls.V(PhysicsCalls.Read(Entity.ID,16)); set => PhysicsCalls.Write(Entity.ID,16,PhysicsCalls.V(value)); }
        public Vector3 CenterOfMass => PhysicsCalls.V(PhysicsCalls.Read(Entity.ID,17));
        public bool IsSleeping => PhysicsCalls.Read(Entity.ID,18).Integer!=0;
        public RigidbodyLocks Locks { get => (RigidbodyLocks)PhysicsCalls.Read(Entity.ID,19).Integer; set => PhysicsCalls.Write(Entity.ID,19,new(){Integer=(long)value}); }
        public void AddForce(Vector3 force, ForceMode mode=ForceMode.Force) => PhysicsCalls.Write(Entity.ID,20,PhysicsCalls.V(force),(int)mode);
        public void AddTorque(Vector3 torque, ForceMode mode=ForceMode.Force) => PhysicsCalls.Write(Entity.ID,21,PhysicsCalls.V(torque),(int)mode);
        public void AddForceAtPosition(Vector3 force, Vector3 position, ForceMode mode=ForceMode.Force) => PhysicsCalls.Write(Entity.ID,22,PhysicsCalls.V(force),(int)mode,position);
        public void MovePosition(Vector3 position) => PhysicsCalls.Write(Entity.ID,23,PhysicsCalls.V(position),0);
        /// <summary>Quaternion rotation in XYZW order; applies as a target on the next fixed step.</summary>
        public void MoveRotation(Quaternion rotation) => PhysicsCalls.Write(Entity.ID,24,PhysicsCalls.Q(rotation),0);
        public void Sleep() => PhysicsCalls.Write(Entity.ID,25,default,1);
        public void WakeUp() => PhysicsCalls.Write(Entity.ID,25,default,0);
    }
    public class RigidBodyComponent : Rigidbody { }
    public abstract class Collider : Component
    {
        internal abstract int Operation { get; }
        private protected PropertyValue Read(int p) => PhysicsCalls.Read(Entity.ID,Operation,p);
        private protected void Write(int p, PropertyValue v, string? text=null) => PhysicsCalls.Write(Entity.ID,Operation,v,p|1024,text:text);
        public bool IsTrigger { get => Read(10).Integer!=0; set => Write(10,new(){Integer=value?1:0}); }
        /// <summary>-1 inherits the owning body's layer.</summary>
        public int Layer { get => (int)Read(11).Integer; set => Write(11,new(){Integer=value}); }
        /// <summary>Canonical asset ID. Loaded asset > inline override > project default; failed asset uses project default.</summary>
        public string Material { get => PhysicsCalls.Asset(Read(12)); set => Write(12,default,value); }
        public Vector3 Center { get => PhysicsCalls.V(Read(3)); set => Write(3,PhysicsCalls.V(value)); }
        public Quaternion Rotation { get { var v=Read(4); return new(v.X,v.Y,v.Z,v.W); } set => Write(4,PhysicsCalls.Q(value)); }
        public Vector3 Scale { get => PhysicsCalls.V(Read(5)); set => Write(5,PhysicsCalls.V(value)); }
    }
    public class BoxCollider : Collider
    { internal override int Operation=>40; public Vector3 HalfExtents { get=>PhysicsCalls.V(Read(0)); set=>Write(0,PhysicsCalls.V(value)); } }
    public class SphereCollider : Collider
    { internal override int Operation=>41; public float Radius { get=>Read(0).X; set=>Write(0,new(){X=value}); } }
    public class CapsuleCollider : Collider
    {
        internal override int Operation=>42;
        public float Radius { get=>Read(0).X; set=>Write(0,new(){X=value}); }
        public float HalfHeight { get=>Read(1).X; set=>Write(1,new(){X=value}); }
        public CapsuleAxis Direction { get=>(CapsuleAxis)Read(2).Integer; set=>Write(2,new(){Integer=(long)value}); }
    }
    public class MeshCollider : Collider
    {
        internal override int Operation=>43;
        public bool Convex { get=>Read(13).Integer!=0; set=>Write(13,new(){Integer=value?1:0}); }
        public string Mesh { get=>PhysicsCalls.Asset(Read(14)); set=>Write(14,default,value); }
        public uint Section { get=>(uint)Read(15).Integer; set=>Write(15,new(){Integer=value}); }
    }
    public class BoxColliderComponent : BoxCollider { }
    public class SphereColliderComponent : SphereCollider { }
    public class CapsuleColliderComponent : CapsuleCollider { }
    public class MeshColliderComponent : MeshCollider { }
    public static unsafe class LayerMask
    {
        public static uint GetMask(params string[] names)
        {
            uint mask=0;
            foreach(var name in names)
            {
                ArgumentNullException.ThrowIfNull(name);
                if(name.Contains('\0')) throw new ArgumentException("Layer name cannot contain NUL.");
                var bytes=Encoding.UTF8.GetBytes(name+"\0"); PropertyValue v=default;
                fixed(byte* p=bytes) { int layer=InternalCalls.Api.PhysicsCommand(0,3,0,&v,null,p)-1; if(layer>=0 && layer<32) mask|=1u<<layer; }
            }
            return mask;
        }
    }
    public static unsafe class Physics
    {
        public static Vector3 Gravity { get=>PhysicsCalls.V(PhysicsCalls.Read(0,0)); set=>PhysicsCalls.Write(0,1,PhysicsCalls.V(value),0); }
        public static void IgnoreLayerCollision(int a,int b,bool ignore=true) => PhysicsCalls.Write(0,2,new(){Integer=a,X=ignore?1:0},b);
        private static PhysicsQueryData Request(uint kind,Vector3 position,Vector3 direction,float distance,QueryFilter? filter)
        {
            QueryFilter f=filter??new QueryFilter();
            return new(){Kind=kind,Origin=position,Direction=direction,Distance=distance,LayerMask=f.LayerMask,Triggers=(uint)f.Triggers,
                Ignore=f.IgnoreEntity?.ID??0,Flags=(f.IncludeStatic?1u:0)|(f.IncludeDynamic?2u:0)|(f.IncludeKinematic?4u:0),RotationW=1,Axis=1};
        }
        private static PhysicsQueryData Sphere(Vector3 origin,float radius,Vector3 direction,float distance,QueryFilter? f)
        { var q=Request(1,origin,direction,distance,f);q.Radius=radius;return q; }
        private static PhysicsQueryData Capsule(Vector3 origin,float radius,float halfHeight,Quaternion rotation,Vector3 direction,float distance,QueryFilter? f)
        { var q=Sphere(origin,radius,direction,distance,f);q.Kind=2;q.HalfHeight=halfHeight;Rotate(ref q,rotation);return q; }
        private static PhysicsQueryData Box(Vector3 origin,Vector3 half,Quaternion rotation,Vector3 direction,float distance,QueryFilter? f)
        { var q=Request(3,origin,direction,distance,f);q.HalfExtents=half;Rotate(ref q,rotation);return q; }
        private static void Rotate(ref PhysicsQueryData q,Quaternion rotation) { q.RotationX=rotation.X;q.RotationY=rotation.Y;q.RotationZ=rotation.Z;q.RotationW=rotation.W; }
        private static bool Single(PhysicsQueryData q,out RaycastHit hit)
        { RaycastHit h=default;bool found=InternalCalls.Api.PhysicsQuery(&q,&h,1,2)>0;hit=h;return found; }
        private static RaycastHit[] All(PhysicsQueryData q,int overlap=0)
        {
            int count=InternalCalls.Api.PhysicsQuery(&q,null,0,overlap);if(count<=0)return Array.Empty<RaycastHit>();
            var hits=new RaycastHit[count];fixed(RaycastHit* p=hits) InternalCalls.Api.PhysicsQuery(&q,p,count,overlap);return hits;
        }
        /// <summary>Writes as many sorted hits as fit and returns the total hit count, allowing callers to reuse storage.</summary>
        public static int RaycastNonAlloc(Vector3 origin,Vector3 direction,float distance,Span<RaycastHit> hits,QueryFilter? filter=null)
        { var q=Request(0,origin,direction,distance,filter);fixed(RaycastHit* p=hits)return InternalCalls.Api.PhysicsQuery(&q,p,hits.Length,0); }
        public static bool Raycast(Vector3 origin,Vector3 direction,float distance,out RaycastHit hit,QueryFilter? filter=null) => Single(Request(0,origin,direction,distance,filter),out hit);
        public static RaycastHit[] RaycastAll(Vector3 origin,Vector3 direction,float distance,QueryFilter? filter=null) => All(Request(0,origin,direction,distance,filter));
        public static bool SphereCast(Vector3 origin,float radius,Vector3 direction,float distance,out RaycastHit hit,QueryFilter? filter=null) => Single(Sphere(origin,radius,direction,distance,filter),out hit);
        public static RaycastHit[] SphereCastAll(Vector3 origin,float radius,Vector3 direction,float distance,QueryFilter? filter=null) => All(Sphere(origin,radius,direction,distance,filter));
        public static bool CapsuleCast(Vector3 center,float radius,float halfHeight,Quaternion rotation,Vector3 direction,float distance,out RaycastHit hit,QueryFilter? filter=null) => Single(Capsule(center,radius,halfHeight,rotation,direction,distance,filter),out hit);
        public static RaycastHit[] CapsuleCastAll(Vector3 center,float radius,float halfHeight,Quaternion rotation,Vector3 direction,float distance,QueryFilter? filter=null) => All(Capsule(center,radius,halfHeight,rotation,direction,distance,filter));
        public static bool BoxCast(Vector3 center,Vector3 halfExtents,Quaternion rotation,Vector3 direction,float distance,out RaycastHit hit,QueryFilter? filter=null) => Single(Box(center,halfExtents,rotation,direction,distance,filter),out hit);
        public static RaycastHit[] BoxCastAll(Vector3 center,Vector3 halfExtents,Quaternion rotation,Vector3 direction,float distance,QueryFilter? filter=null) => All(Box(center,halfExtents,rotation,direction,distance,filter));
        private static Entity[] Overlaps(PhysicsQueryData q)
        { var hits=All(q,1);var entities=new Entity[hits.Length];for(int i=0;i<hits.Length;++i)entities[i]=new(hits[i].ColliderID);return entities; }
        public static Entity[] OverlapSphere(Vector3 center,float radius,QueryFilter? filter=null) => Overlaps(Sphere(center,radius,default,0,filter));
        public static Entity[] OverlapBox(Vector3 center,Vector3 halfExtents,Quaternion rotation,QueryFilter? filter=null) => Overlaps(Box(center,halfExtents,rotation,default,0,filter));
        public static Entity[] OverlapCapsule(Vector3 center,float radius,float halfHeight,Quaternion rotation,QueryFilter? filter=null) => Overlaps(Capsule(center,radius,halfHeight,rotation,default,0,filter));
    }
}

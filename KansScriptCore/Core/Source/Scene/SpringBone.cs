using Kans.Interop;
namespace Kans
{
    public class SpringBone : Component
    {
        private PropertyValue Read(int p) => PhysicsCalls.Read(Entity.ID,70,p);
        private void Write(int p,PropertyValue value) => PhysicsCalls.Write(Entity.ID,70,value,p|1024);
        public bool Enabled { get => Read(0).Integer!=0; set => Write(0,new(){Integer=value?1:0}); }
        public float InertiaScale { get => Read(1).X; set => Write(1,new(){X=value}); }
        public float WindScale { get => Read(2).X; set => Write(2,new(){X=value}); }
        public void Reset() => PhysicsCalls.Write(Entity.ID,71,default,0);
        private unsafe float Chain(int index,int property,bool write,float value=0)
        {
            PropertyValue v=new(){Integer=index,X=value};
            PhysicsCalls.Check(InternalCalls.Api.PhysicsCommand(Entity.ID,72,property|(write?1024:0),&v,null,null));
            return v.X;
        }
        public float GetChainStiffness(int index) => Chain(index,0,false);
        public void SetChainStiffness(int index,float multiplier) => Chain(index,0,true,multiplier);
        public float GetChainDrag(int index) => Chain(index,1,false);
        public void SetChainDrag(int index,float multiplier) => Chain(index,1,true,multiplier);
        public float GetChainGravity(int index) => Chain(index,2,false);
        public void SetChainGravity(int index,float multiplier) => Chain(index,2,true,multiplier);
    }
    public class SpringBoneComponent : SpringBone { }
    public enum MotionColliderShape { Sphere, Capsule, Plane, Box }
    public class MotionCollider : Component
    {
        private PropertyValue Read(int p) => PhysicsCalls.Read(Entity.ID,73,p);
        private void Write(int p,PropertyValue value) => PhysicsCalls.Write(Entity.ID,73,value,p|1024);
        public MotionColliderShape Shape { get => (MotionColliderShape)Read(0).Integer; set => Write(0,new(){Integer=(long)value}); }
        public bool Inside { get => Read(1).Integer!=0; set => Write(1,new(){Integer=value?1:0}); }
        public Vector3 Offset { get => PhysicsCalls.V(Read(2)); set => Write(2,PhysicsCalls.V(value)); }
        public Vector4 Rotation { get {var v=Read(3);return new(v.X,v.Y,v.Z,v.W);} set => Write(3,new(){X=value.X,Y=value.Y,Z=value.Z,W=value.W}); }
        public Vector3 Scale { get => PhysicsCalls.V(Read(4)); set => Write(4,PhysicsCalls.V(value)); }
        public float Radius { get => Read(5).X; set => Write(5,new(){X=value}); }
        public float HalfHeight { get => Read(6).X; set => Write(6,new(){X=value}); }
        public CapsuleAxis Direction { get => (CapsuleAxis)Read(7).Integer; set => Write(7,new(){Integer=(long)value}); }
        public Vector3 HalfExtents { get => PhysicsCalls.V(Read(9)); set => Write(9,PhysicsCalls.V(value)); }
        public Vector3 Normal { get => PhysicsCalls.V(Read(8)); set => Write(8,PhysicsCalls.V(value)); }
    }
    public class MotionColliderComponent : MotionCollider { }
}

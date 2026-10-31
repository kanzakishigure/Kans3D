using Kans.Interop;
using System.Text;
namespace Kans
{
    public enum RagdollMode { Kinematic, Simulated, Blended }
    public enum RagdollRootFollowing { None, FollowPelvis }
    public class Ragdoll : Component
    {
        private PropertyValue Read(int p) => PhysicsCalls.Read(Entity.ID,80,p);
        private void Write(int p,PropertyValue v) => PhysicsCalls.Write(Entity.ID,80,v,p|1024);
        public RagdollMode Mode { get => (RagdollMode)Read(0).Integer; set => Write(0,new(){Integer=(long)value}); }
        public float DriveStrength { get => Read(1).X; set => Write(1,new(){X=value}); }
        public bool Enabled { get => Read(2).Integer!=0; set => Write(2,new(){Integer=value?1:0}); }
        public RagdollRootFollowing RootFollowing { get => (RagdollRootFollowing)Read(3).Integer; set => Write(3,new(){Integer=(long)value}); }
        public bool FollowYaw { get => Read(4).Integer!=0; set => Write(4,new(){Integer=value?1:0}); }
        public float TeleportDistance { get => Read(5).X; set => Write(5,new(){X=value}); }
        public float TeleportRotation { get => Read(6).X; set => Write(6,new(){X=value}); }
        public void Simulate() => PhysicsCalls.Write(Entity.ID,81,default,0);
        public void Simulate(string fromBone) => PhysicsCalls.Write(Entity.ID,81,default,0,text:fromBone);
        public void BlendToAnimation(float duration) => PhysicsCalls.Write(Entity.ID,82,new(){X=duration},0);
        public void SetBlendWeight(string bone,float weight,bool includeChildren=false) => PhysicsCalls.Write(Entity.ID,83,new(){X=weight},includeChildren?1:0,text:bone);
        public void AddImpulse(string bone,Vector3 impulse,Vector3 worldPoint) => PhysicsCalls.Write(Entity.ID,84,PhysicsCalls.V(impulse),0,worldPoint,bone);
        public void AddForce(string bone,Vector3 force,ForceMode mode=ForceMode.Force) => PhysicsCalls.Write(Entity.ID,85,PhysicsCalls.V(force),(int)mode,text:bone);
        public void SetBodyDrive(string bone,float strength,float damping) => PhysicsCalls.Write(Entity.ID,88,new(){X=strength,Y=damping},0,text:bone);
        private unsafe PropertyValue Body(string bone,int property) {
            var bytes=Encoding.UTF8.GetBytes(bone+"\0");PropertyValue v=default;
            fixed(byte* p=bytes)PhysicsCalls.Check(InternalCalls.Api.PhysicsCommand(Entity.ID,86,property,&v,null,p));return v;
        }
        public Vector3 GetBodyPosition(string bone) => PhysicsCalls.V(Body(bone,0));
        public Quaternion GetBodyRotation(string bone) {var v=Body(bone,1);return new(v.X,v.Y,v.Z,v.W);}
        public Vector3 GetBodyVelocity(string bone) => PhysicsCalls.V(Body(bone,2));
        public void ResetRagdoll() => PhysicsCalls.Write(Entity.ID,87,default,0);
    }
    public class RagdollComponent : Ragdoll { }
}

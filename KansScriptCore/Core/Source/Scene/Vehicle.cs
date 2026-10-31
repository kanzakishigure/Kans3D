using Kans.Interop;
namespace Kans
{
    public struct VehicleWheelState
    {
        public bool Grounded;
        public ulong GroundEntityId;
        public Entity GroundEntity => new(GroundEntityId);
        public Vector3 ContactPoint,ContactNormal;
        public float SuspensionCompression,LongitudinalSlip,LateralSlip,AngularSpeed;
    }
    public class Vehicle : Component
    {
        private PropertyValue Read(int p) => PhysicsCalls.Read(Entity.ID,90,p);
        private void Write(int p,PropertyValue v) => PhysicsCalls.Write(Entity.ID,90,v,p|1024);
        public float Throttle {get=>Read(0).X;set=>Write(0,new(){X=value});}
        public float Brake {get=>Read(1).X;set=>Write(1,new(){X=value});}
        public float Steer {get=>Read(2).X;set=>Write(2,new(){X=value});}
        public float Handbrake {get=>Read(3).X;set=>Write(3,new(){X=value});}
        public int TargetGear {get=>(int)Read(4).Integer;set=>Write(4,new(){Integer=value});}
        public bool AutomaticGearbox {get=>Read(5).Integer!=0;set=>Write(5,new(){Integer=value?1:0});}
        public float ForwardSpeed=>PhysicsCalls.Read(Entity.ID,91).X;
        public float EngineRpm=>PhysicsCalls.Read(Entity.ID,91).Y;
        public float TotalDriveTorque=>PhysicsCalls.Read(Entity.ID,91).Z;
        public int CurrentGear=>(int)PhysicsCalls.Read(Entity.ID,91).Integer;
        public unsafe VehicleWheelState GetWheelState(int index) {
            PropertyValue a=new(){Integer=index},b=new(){Integer=index};Vector3 normal=default;
            PhysicsCalls.Check(InternalCalls.Api.PhysicsCommand(Entity.ID,92,0,&a,&normal,null));
            PhysicsCalls.Check(InternalCalls.Api.PhysicsCommand(Entity.ID,93,0,&b,null,null));
            return new(){Grounded=a.W!=0,GroundEntityId=(ulong)a.Integer,ContactPoint=PhysicsCalls.V(a),ContactNormal=normal,SuspensionCompression=b.X,LongitudinalSlip=b.Y,LateralSlip=b.Z,AngularSpeed=b.W};
        }
    }
    public class VehicleComponent : Vehicle { }
}

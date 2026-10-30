using System;
using Kans.Interop;
namespace Kans
{
    [Flags] public enum CollisionFlags { None=0, Sides=1, Above=2, Below=4 }
    public enum ControllerShape { Capsule, Box }
    public enum NonWalkableMode { PreventClimbing, ForceSliding }
    public enum JointType { Fixed, Hinge, Slider, Spherical, Distance, D6 }
    public enum JointMotion { Locked, Limited, Free }
    public enum JointAxis { X, Y, Z, Twist, SwingY, SwingZ }
    public enum JointDrive { X, Y, Z, Swing, Twist, Slerp }
    public readonly struct ControllerColliderHit
    {
        public readonly Entity Collider, Body;
        public readonly Vector3 Point, Normal, MoveDirection;
        public readonly float MoveLength;
        internal ControllerColliderHit(Entity collider,Entity body,Vector3 point,Vector3 normal,Vector3 direction,float length)
        { Collider=collider;Body=body;Point=point;Normal=normal;MoveDirection=direction;MoveLength=length; }
    }
    /// <summary>Gravity-free kinematic movement. Prefer OnFixedUpdate. OnUpdate moves immediately using frame dt;
    /// interpolation cannot roll it back. Settings use world units; Height is the capsule cylindrical height.
    /// UpDirection and Center are world-space. Resize preserves the foot position; box resize takes full height.</summary>
    public unsafe class CharacterController : Component
    {
        private PropertyValue Read(int p) => PhysicsCalls.Read(Entity.ID,50,p);
        private void Write(int p,PropertyValue v) => PhysicsCalls.Write(Entity.ID,50,v,p|1024);
        private Vector3 m_GravityVelocity;
        public ControllerShape Shape { get => (ControllerShape)Read(0).Integer; set => Write(0,new(){Integer=(long)value}); }
        public float Radius { get => Read(1).X; set => Write(1,new(){X=value}); }
        public float Height { get => Read(2).X; set => Write(2,new(){X=value}); }
        public Vector3 HalfExtents { get => PhysicsCalls.V(Read(3)); set => Write(3,PhysicsCalls.V(value)); }
        public Vector3 Center { get => PhysicsCalls.V(Read(4)); set => Write(4,PhysicsCalls.V(value)); }
        public uint Layer { get => (uint)Read(5).Integer; set => Write(5,new(){Integer=(long)value}); }
        public uint CollisionMask { get => (uint)Read(6).Integer; set => Write(6,new(){Integer=(long)value}); }
        public float SlopeLimit { get => Read(7).X; set => Write(7,new(){X=value}); }
        public NonWalkableMode NonWalkable { get => (NonWalkableMode)Read(8).Integer; set => Write(8,new(){Integer=(long)value}); }
        public float StepOffset { get => Read(9).X; set => Write(9,new(){X=value}); }
        public float SkinWidth { get => Read(10).X; set => Write(10,new(){X=value}); }
        public float MinMoveDistance { get => Read(11).X; set => Write(11,new(){X=value}); }
        public Vector3 UpDirection { get => PhysicsCalls.V(Read(12)); set => Write(12,PhysicsCalls.V(value)); }
        public bool PushRigidBodies { get => Read(13).Integer!=0; set => Write(13,new(){Integer=value?1:0}); }
        public float Mass { get => Read(14).X; set => Write(14,new(){X=value}); }
        public float PushForceScale { get => Read(15).X; set => Write(15,new(){X=value}); }
        public float SnapDistance { get => Read(16).X; set => Write(16,new(){X=value}); }
        public bool FollowPlatforms { get => Read(17).Integer!=0; set => Write(17,new(){Integer=value?1:0}); }
        private PropertyValue State(int op) => PhysicsCalls.Read(Entity.ID,op);
        public bool IsGrounded => State(56).Integer!=0;
        public Vector3 Velocity => PhysicsCalls.V(State(55));
        public Vector3 GroundNormal => PhysicsCalls.V(State(56));
        public Vector3 GroundPoint => PhysicsCalls.V(State(57));
        public Entity? GroundCollider => State(57).Integer==0?null:new Entity(unchecked((ulong)State(57).Integer));
        public CollisionFlags CollisionFlags => (CollisionFlags)State(55).Integer;
        public CollisionFlags Move(Vector3 displacement)
        { var v=PhysicsCalls.V(displacement); PhysicsCalls.Check(InternalCalls.Api.PhysicsCommand(Entity.ID,51,0,&v,null,null));return (CollisionFlags)v.Integer; }
        public bool SimpleMove(Vector3 speed)
        {
            float dt=State(58).X; var up=UpDirection;up=up*(1/MathF.Sqrt(up.X*up.X+up.Y*up.Y+up.Z*up.Z));
            if(IsGrounded) m_GravityVelocity=default;
            m_GravityVelocity += Physics.Gravity*dt;
            Move((speed+up*(-(speed.X*up.X+speed.Y*up.Y+speed.Z*up.Z))+m_GravityVelocity)*dt);return IsGrounded;
        }
        public bool Resize(float height) => ResizeCall(52,height);
        public bool CanResize(float height) => ResizeCall(53,height);
        private bool ResizeCall(int op,float height)
        { PropertyValue v=new(){X=height};PhysicsCalls.Check(InternalCalls.Api.PhysicsCommand(Entity.ID,op,0,&v,null,null));return v.Integer!=0; }
        public void Teleport(Vector3 position) {m_GravityVelocity=default;PhysicsCalls.Write(Entity.ID,54,PhysicsCalls.V(position),0);}
    }
    public class CharacterControllerComponent : CharacterController { }
    /// <summary>Joint entity world transform captures both local frames. Editing that transform during play
    /// recaptures frames; body actor replacements preserve them. Angles are degrees; angular velocities are radians/s.
    /// BodyA null resolves the nearest rigid body on this entity or ancestors; BodyB null anchors to the world.</summary>
    public class Joint : Component
    {
        private PropertyValue Read(int p) => PhysicsCalls.Read(Entity.ID,60,p);
        private void Write(int p,PropertyValue v) => PhysicsCalls.Write(Entity.ID,60,v,p|1024);
        public JointType Type { get => (JointType)Read(0).Integer; set => Write(0,new(){Integer=(long)value}); }
        public Entity? BodyA { get => Read(1).Integer==0?null:new Entity(unchecked((ulong)Read(1).Integer)); set => Write(1,new(){Integer=unchecked((long)(value?.ID??0))}); }
        public Entity? BodyB { get => Read(2).Integer==0?null:new Entity(unchecked((ulong)Read(2).Integer)); set => Write(2,new(){Integer=unchecked((long)(value?.ID??0))}); }
        public bool EnableCollision { get => Read(3).Integer!=0; set => Write(3,new(){Integer=value?1:0}); }
        public float BreakForce { get => Read(4).X; set => Write(4,new(){X=value}); }
        public float BreakTorque { get => Read(5).X; set => Write(5,new(){X=value}); }
        public float InvMassScaleA { get => Read(6).X; set => Write(6,new(){X=value}); }
        public float InvMassScaleB { get => Read(7).X; set => Write(7,new(){X=value}); }
        public float InvInertiaScaleA { get => Read(8).X; set => Write(8,new(){X=value}); }
        public float InvInertiaScaleB { get => Read(9).X; set => Write(9,new(){X=value}); }
        public bool HingeLimitEnabled { get => Read(10).Integer!=0; set => Write(10,new(){Integer=value?1:0}); }
        public float HingeLowerAngle { get => Read(11).X; set => Write(11,new(){X=value}); }
        public float HingeUpperAngle { get => Read(12).X; set => Write(12,new(){X=value}); }
        public float HingeLimitStiffness { get => Read(13).X; set => Write(13,new(){X=value}); }
        public float HingeLimitDamping { get => Read(14).X; set => Write(14,new(){X=value}); }
        public bool HingeMotorEnabled { get => Read(15).Integer!=0; set => Write(15,new(){Integer=value?1:0}); }
        public float HingeMotorVelocity { get => Read(16).X; set => Write(16,new(){X=value}); }
        public float HingeMotorMaxForce { get => Read(17).X; set => Write(17,new(){X=value}); }
        public bool HingeFreeSpin { get => Read(18).Integer!=0; set => Write(18,new(){Integer=value?1:0}); }
        public bool HingeSpringEnabled { get => Read(19).Integer!=0; set => Write(19,new(){Integer=value?1:0}); }
        public float HingeTargetAngle { get => Read(20).X; set => Write(20,new(){X=value}); }
        public float HingeSpringStiffness { get => Read(21).X; set => Write(21,new(){X=value}); }
        public float HingeSpringDamping { get => Read(22).X; set => Write(22,new(){X=value}); }
        public bool SliderLimitEnabled { get => Read(23).Integer!=0; set => Write(23,new(){Integer=value?1:0}); }
        public float SliderLowerLimit { get => Read(24).X; set => Write(24,new(){X=value}); }
        public float SliderUpperLimit { get => Read(25).X; set => Write(25,new(){X=value}); }
        public float SliderLimitStiffness { get => Read(26).X; set => Write(26,new(){X=value}); }
        public float SliderLimitDamping { get => Read(27).X; set => Write(27,new(){X=value}); }
        public bool SliderDriveEnabled { get => Read(28).Integer!=0; set => Write(28,new(){Integer=value?1:0}); }
        public float SliderTargetPosition { get => Read(29).X; set => Write(29,new(){X=value}); }
        public float SliderTargetVelocity { get => Read(30).X; set => Write(30,new(){X=value}); }
        public float SliderDriveStiffness { get => Read(31).X; set => Write(31,new(){X=value}); }
        public float SliderDriveDamping { get => Read(32).X; set => Write(32,new(){X=value}); }
        public float SliderDriveMaxForce { get => Read(33).X; set => Write(33,new(){X=value}); }
        public bool SphericalLimitEnabled { get => Read(34).Integer!=0; set => Write(34,new(){Integer=value?1:0}); }
        public float SphericalYAngle { get => Read(35).X; set => Write(35,new(){X=value}); }
        public float SphericalZAngle { get => Read(36).X; set => Write(36,new(){X=value}); }
        public float SphericalLimitStiffness { get => Read(37).X; set => Write(37,new(){X=value}); }
        public float SphericalLimitDamping { get => Read(38).X; set => Write(38,new(){X=value}); }
        public bool DistanceMinEnabled { get => Read(39).Integer!=0; set => Write(39,new(){Integer=value?1:0}); }
        public bool DistanceMaxEnabled { get => Read(40).Integer!=0; set => Write(40,new(){Integer=value?1:0}); }
        public float DistanceMin { get => Read(41).X; set => Write(41,new(){X=value}); }
        public float DistanceMax { get => Read(42).X; set => Write(42,new(){X=value}); }
        public float DistanceStiffness { get => Read(43).X; set => Write(43,new(){X=value}); }
        public float DistanceDamping { get => Read(44).X; set => Write(44,new(){X=value}); }
        public float DistanceTolerance { get => Read(45).X; set => Write(45,new(){X=value}); }
        public Vector3 D6LinearLower { get => PhysicsCalls.V(Read(47)); set => Write(47,PhysicsCalls.V(value)); }
        public Vector3 D6LinearUpper { get => PhysicsCalls.V(Read(48)); set => Write(48,PhysicsCalls.V(value)); }
        public float D6LinearStiffness { get => Read(49).X; set => Write(49,new(){X=value}); }
        public float D6LinearDamping { get => Read(50).X; set => Write(50,new(){X=value}); }
        public float D6TwistLower { get => Read(51).X; set => Write(51,new(){X=value}); }
        public float D6TwistUpper { get => Read(52).X; set => Write(52,new(){X=value}); }
        public float D6SwingY { get => Read(53).X; set => Write(53,new(){X=value}); }
        public float D6SwingZ { get => Read(54).X; set => Write(54,new(){X=value}); }
        public float D6AngularStiffness { get => Read(55).X; set => Write(55,new(){X=value}); }
        public float D6AngularDamping { get => Read(56).X; set => Write(56,new(){X=value}); }
        public Vector3 DrivePosition { get => PhysicsCalls.V(Read(57)); set => Write(57,PhysicsCalls.V(value)); }
        public Quaternion DriveRotation { get => Rotation(Read(58)); set => Write(58,PhysicsCalls.Q(value)); }
        public Vector3 DriveLinearVelocity { get => PhysicsCalls.V(Read(59)); set => Write(59,PhysicsCalls.V(value)); }
        public Vector3 DriveAngularVelocity { get => PhysicsCalls.V(Read(60)); set => Write(60,PhysicsCalls.V(value)); }
        public bool SlerpDrive { get => Read(64).Integer!=0; set => Write(64,new(){Integer=value?1:0}); }
        public JointMotion GetD6Motion(JointAxis axis) => (JointMotion)Read(46+((int)axis<<7)).Integer;
        public void SetD6Motion(JointAxis axis,JointMotion value) => Write(46+((int)axis<<7),new(){Integer=(long)value});
        public float GetDriveStiffness(JointDrive axis) => Read(61+((int)axis<<7)).X;
        public void SetDriveStiffness(JointDrive axis,float value) => Write(61+((int)axis<<7),new(){X=value});
        public float GetDriveDamping(JointDrive axis) => Read(62+((int)axis<<7)).X;
        public void SetDriveDamping(JointDrive axis,float value) => Write(62+((int)axis<<7),new(){X=value});
        public float GetDriveMaxForce(JointDrive axis) => Read(63+((int)axis<<7)).X;
        public void SetDriveMaxForce(JointDrive axis,float value) => Write(63+((int)axis<<7),new(){X=value});
        private static Quaternion Rotation(PropertyValue v) => new(v.X,v.Y,v.Z,v.W);
        public bool IsBroken => PhysicsCalls.Read(Entity.ID,62).Integer!=0;
        public float CurrentAngle => PhysicsCalls.Read(Entity.ID,62).X;
        public float CurrentPosition => PhysicsCalls.Read(Entity.ID,62).Y;
        public Vector3 CurrentForce => PhysicsCalls.V(PhysicsCalls.Read(Entity.ID,63));
        public Vector3 CurrentTorque => PhysicsCalls.V(PhysicsCalls.Read(Entity.ID,64));
        public void Reset() => PhysicsCalls.Write(Entity.ID,61,default,0);
    }
    public class JointComponent : Joint { }
}

using System;
using Kans.Interop;
namespace Kans {
    public enum FieldShape { Infinite, Sphere, Box }
    public enum FieldFalloff { None, Linear, InverseSquare, Curve }
    public enum FieldType { Wind, Radial, Directional, Vortex, Noise, Drag }
    [Flags] public enum FieldSystems { RigidBodies=1,Cloth=2,SpringBones=4,Particles=8,All=15 }
public class ForceField : Component {
 private PropertyValue Read(int p)=>PhysicsCalls.Read(Entity.ID,95,p);
 private void Write(int p,PropertyValue v)=>PhysicsCalls.Write(Entity.ID,95,v,p|1024);
public FieldType Type {get=>(FieldType)Read(0).Integer;set=>Write(0,new(){Integer=(long)value});}
public FieldShape Shape {get=>(FieldShape)Read(1).Integer;set=>Write(1,new(){Integer=(long)value});}
public FieldFalloff Falloff {get=>(FieldFalloff)Read(2).Integer;set=>Write(2,new(){Integer=(long)value});}
public Vector3 Direction {get=>PhysicsCalls.V(Read(3));set=>Write(3,PhysicsCalls.V(value));}
public Vector3 HalfExtents {get=>PhysicsCalls.V(Read(4));set=>Write(4,PhysicsCalls.V(value));}
public Vector3 Scroll {get=>PhysicsCalls.V(Read(5));set=>Write(5,PhysicsCalls.V(value));}
public float Radius {get=>Read(6).X;set=>Write(6,new(){X=value});}
public float Strength {get=>Read(7).X;set=>Write(7,new(){X=value});}
public float InwardPull {get=>Read(8).X;set=>Write(8,new(){X=value});}
public float Frequency {get=>Read(9).X;set=>Write(9,new(){X=value});}
public bool Acceleration {get=>Read(10).Integer!=0;set=>Write(10,new(){Integer=value?1:0});}
public bool CurlNoise {get=>Read(11).Integer!=0;set=>Write(11,new(){Integer=value?1:0});}
public uint LayerMask {get=>(uint)Read(12).Integer;set=>Write(12,new(){Integer=(long)value});}
public FieldSystems AffectMask {get=>(FieldSystems)Read(13).Integer;set=>Write(13,new(){Integer=(long)value});}
public uint Seed {get=>(uint)Read(14).Integer;set=>Write(14,new(){Integer=(long)value});}
}
public class ForceFieldComponent : ForceField { }
public class WindZone : Component {
 private PropertyValue Read(int p)=>PhysicsCalls.Read(Entity.ID,96,p);
 private void Write(int p,PropertyValue v)=>PhysicsCalls.Write(Entity.ID,96,v,p|1024);
public FieldShape Shape {get=>(FieldShape)Read(0).Integer;set=>Write(0,new(){Integer=(long)value});}
public Vector3 Direction {get=>PhysicsCalls.V(Read(1));set=>Write(1,PhysicsCalls.V(value));}
public float Strength {get=>Read(2).X;set=>Write(2,new(){X=value});}
public float TurbulenceAmplitude {get=>Read(3).X;set=>Write(3,new(){X=value});}
public float TurbulenceFrequency {get=>Read(4).X;set=>Write(4,new(){X=value});}
public float PulseMagnitude {get=>Read(5).X;set=>Write(5,new(){X=value});}
public float PulseFrequency {get=>Read(6).X;set=>Write(6,new(){X=value});}
public float Radius {get=>Read(7).X;set=>Write(7,new(){X=value});}
public float FalloffBand {get=>Read(8).X;set=>Write(8,new(){X=value});}
public Vector3 HalfExtents {get=>PhysicsCalls.V(Read(9));set=>Write(9,PhysicsCalls.V(value));}
public uint LayerMask {get=>(uint)Read(10).Integer;set=>Write(10,new(){Integer=(long)value});}
public FieldSystems AffectMask {get=>(FieldSystems)Read(11).Integer;set=>Write(11,new(){Integer=(long)value});}
}
public class WindZoneComponent : WindZone { }
}

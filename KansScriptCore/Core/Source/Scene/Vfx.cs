using System;
using System.Text;
using Kans.Interop;
namespace Kans
{
    public class ParticleSystem : Component
    {
        internal static unsafe PropertyValue Call(ulong entity,int op,PropertyValue value=default,string? name=null,int type=0)
        {
            if(name?.Contains('\0')==true)throw new ArgumentException("VFX name cannot contain NUL.",nameof(name));
            byte[]? bytes=name==null?null:Encoding.UTF8.GetBytes(name+"\0");
            int result;fixed(byte* text=bytes)result=InternalCalls.Api.VfxCommand(entity,op,type,text,&value,null);
            if(result==-2)throw new ArgumentException("VFX user parameter type mismatch: "+name);
            if(result<0)throw new ArgumentException("Unknown VFX name or invalid value: "+name);
            if(result==0)throw new InvalidOperationException("VFX system is unavailable.");
            return value;
        }
        public void Activate(bool reset=true)=>Call(Entity.ID,0,new(){Integer=reset?1:0});
        public void Deactivate()=>Call(Entity.ID,1);
        public void DeactivateImmediate()=>Call(Entity.ID,2);
        public void Pause()=>Call(Entity.ID,3);
        public void Resume()=>Call(Entity.ID,4);
        public bool IsActive=>Call(Entity.ID,5).Integer!=0;
        public bool IsComplete=>Call(Entity.ID,6).Integer!=0;
        // GPU particle counts arrive asynchronously, normally one or two retired frames later.
        public uint ParticleCount=>(uint)Call(Entity.ID,7).Integer;
        public void Emit(string emitterName,uint count)=>Call(Entity.ID,8,new(){Integer=count},emitterName);
        public void SetFloat(string name,float value)=>Call(Entity.ID,10,new(){X=value},name,0);
        public float GetFloat(string name)=>Call(Entity.ID,11,default,name,0).X;
        public void SetInt(string name,int value)=>Call(Entity.ID,10,new(){Integer=value},name,4);
        public int GetInt(string name)=>(int)Call(Entity.ID,11,default,name,4).Integer;
        public void SetBool(string name,bool value)=>Call(Entity.ID,10,new(){Integer=value?1:0},name,5);
        public bool GetBool(string name)=>Call(Entity.ID,11,default,name,5).Integer!=0;
        public void SetVector2(string name,Vector2 value)=>Call(Entity.ID,10,new(){X=value.X,Y=value.Y},name,1);
        public Vector2 GetVector2(string name){var v=Call(Entity.ID,11,default,name,1);return new(v.X,v.Y);}
        public void SetVector3(string name,Vector3 value)=>Call(Entity.ID,10,new(){X=value.X,Y=value.Y,Z=value.Z},name,2);
        public Vector3 GetVector3(string name){var v=Call(Entity.ID,11,default,name,2);return new(v.X,v.Y,v.Z);}
        public void SetVector4(string name,Vector4 value)=>Call(Entity.ID,10,new(){X=value.X,Y=value.Y,Z=value.Z,W=value.W},name,3);
        public Vector4 GetVector4(string name){var v=Call(Entity.ID,11,default,name,3);return new(v.X,v.Y,v.Z,v.W);}
        public void SetColor(string name,Vector4 value)=>Call(Entity.ID,10,new(){X=value.X,Y=value.Y,Z=value.Z,W=value.W},name,6);
        public Vector4 GetColor(string name){var v=Call(Entity.ID,11,default,name,6);return new(v.X,v.Y,v.Z,v.W);}
    }
    public class ParticleSystemComponent : ParticleSystem { }
    public static class Vfx
    {
        public static unsafe Entity SpawnSystemAtLocation(string asset,Vector3 position,Quaternion rotation,bool autoDestroy=true)
        {
            if(asset==null||asset.Contains('\0'))throw new ArgumentException("Invalid VFX asset ID.",nameof(asset));
            var bytes=Encoding.UTF8.GetBytes(asset+"\0");PropertyValue value=new(){X=position.X,Y=position.Y,Z=position.Z};PropertyValue second=new(){X=rotation.X,Y=rotation.Y,Z=rotation.Z,W=rotation.W,Integer=autoDestroy?1:0};
            int result;fixed(byte* text=bytes)result=InternalCalls.Api.VfxCommand(0,100,0,text,&value,&second);
            if(result<=0)throw new ArgumentException("VFX system is unavailable or invalid: "+asset);
            return new Entity((ulong)value.Integer);
        }
    }
}

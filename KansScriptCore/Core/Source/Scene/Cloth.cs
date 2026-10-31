using Kans.Interop;
namespace Kans
{
    public class Cloth : Component
    {
        private PropertyValue Read(int p) => PhysicsCalls.Read(Entity.ID,74,p);
        private void Write(int p,PropertyValue value) => PhysicsCalls.Write(Entity.ID,74,value,p|1024);
        public bool Enabled { get => Read(0).Integer!=0; set => Write(0,new(){Integer=value?1:0}); }
        public float GravityScale { get => Read(1).X; set => Write(1,new(){X=value}); }
        public float WindScale { get => Read(2).X; set => Write(2,new(){X=value}); }
        public float InertiaScale { get => Read(3).X; set => Write(3,new(){X=value}); }
        public float StretchCompliance { get => Read(4).X; set => Write(4,new(){X=value}); }
        public float BendCompliance { get => Read(5).X; set => Write(5,new(){X=value}); }
        public float Damping { get => Read(6).X; set => Write(6,new(){X=value}); }
        public float VelocityDrag { get => Read(7).X; set => Write(7,new(){X=value}); }
        public void SetMaxDistanceScale(float scale) => Write(8,new(){X=scale});
        public void Reset() => PhysicsCalls.Write(Entity.ID,75,default,0);
    }
    public class ClothComponent : Cloth { }
}

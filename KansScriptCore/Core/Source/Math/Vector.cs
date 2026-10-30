using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace Kans
{
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    public struct Quaternion
    {
        public float X, Y, Z, W;
        public Quaternion(float x, float y, float z, float w) { X=x; Y=y; Z=z; W=w; }
        public static Quaternion Identity => new(0,0,0,1);
        public static implicit operator Vector4(Quaternion q) => new(q.X,q.Y,q.Z,q.W);
        public static implicit operator Quaternion(Vector4 q) => new(q.X,q.Y,q.Z,q.W);
    }

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    public struct Vector2
    {
        public float X, Y;
        public Vector2(float x, float y) { X = x; Y = y; }
    }

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    public struct Vector4
    {
        public float X, Y, Z, W;
        public Vector4(float x, float y, float z, float w) { X = x; Y = y; Z = z; W = w; }
    }

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    public struct Vector3
    {

        static Vector3 Zero => new Vector3(0.0f);
        public float X;
        public float Y;
        public float Z;
        public Vector3(float x, float y, float z)
        {
            X = x;
            Y = y;
            Z = z;
        }
        public Vector3(float Scale)
        {
            X = Scale;
            Y = Scale;
            Z = Scale;
        }
        public static Vector3 operator *(Vector3 vector, float scale)
        {
            return new Vector3(vector.X * scale, vector.Y * scale, vector.Z * scale);
        }

        public static Vector3 operator +(Vector3 v1, Vector3 v2)
        {
            return new Vector3(v1.X+ v2.X, v1.Y + v2.Y, v1.Z + v2.Z);
        }

    }
}

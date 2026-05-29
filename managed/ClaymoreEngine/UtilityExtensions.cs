using System.Drawing;
using System.Numerics;

namespace ClaymoreEngine;

public static class UtilityExtensions
{
    public static Vector4 ToVector4(this Color color)
    {
        return new Vector4(color.R, color.G, color.B, color.A);
    }
}
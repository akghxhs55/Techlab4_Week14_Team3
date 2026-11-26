#include "DepthOfFieldLayeredCommon.hlsli"

Texture2D<float> CoCTexture : register(t0);

DOFVSOutput mainVS(uint VertexID : SV_VertexID)
{
    return DOFMainVS(VertexID);
}

float2 mainPS(DOFVSOutput Input) : SV_Target
{
    int2 Tile = int2(Input.Position.xy);
    int2 Origin = Tile * 16;
    int2 ImageSize = int2(1.0f / InvWidth, 1.0f / InvHeight);
    float2 Maximum = 0.0f;
    [loop] for (int Y = 0; Y < 16; ++Y)
    {
        [loop] for (int X = 0; X < 16; ++X)
        {
            int2 Pixel = Origin + int2(X, Y);
            if (Pixel.x >= ImageSize.x || Pixel.y >= ImageSize.y) continue;
            float CoC = CoCTexture.Load(int3(Pixel, 0));
            Maximum = max(Maximum, float2(max(-CoC, 0.0f), max(CoC, 0.0f)));
        }
    }
    return Maximum;
}

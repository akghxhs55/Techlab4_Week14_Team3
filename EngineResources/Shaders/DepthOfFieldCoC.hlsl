#include "DepthOfFieldLayeredCommon.hlsli"

Texture2D<float> SceneDepth : register(t0);

DOFVSOutput mainVS(uint VertexID : SV_VertexID)
{
    return DOFMainVS(VertexID);
}

float mainPS(DOFVSOutput Input) : SV_Target
{
    int2 Pixel = int2(Input.Position.xy);
    return DOFSignedCoC(DOFViewDepth(SceneDepth.Load(int3(Pixel, 0))));
}

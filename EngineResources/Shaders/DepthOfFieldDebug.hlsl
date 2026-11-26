#include "DepthOfFieldLayeredCommon.hlsli"

Texture2D<float> CoCTexture : register(t0);

DOFVSOutput mainVS(uint VertexID : SV_VertexID)
{
    return DOFMainVS(VertexID);
}

float4 mainPS(DOFVSOutput Input) : SV_Target
{
    float CoC = CoCTexture.Load(int3(int2(Input.Position.xy), 0));
    float Radius = abs(CoC);
    if (Radius <= 0.5f) return float4(0.0f, 0.0f, 0.0f, 1.0f);

	float MaxRadius = CoC < 0.0f ? NearMaxRadiusPixels : FarMaxRadiusPixels;
	float Strength = saturate((Radius - 0.5f) / max(MaxRadius - 0.5f, 0.001f));
    float Brightness = 0.25f + 0.75f * Strength;
    float3 Color = CoC < 0.0f ? float3(0.0f, Brightness, 0.0f) : float3(0.0f, 0.0f, Brightness);
    return float4(Color, 1.0f);
}

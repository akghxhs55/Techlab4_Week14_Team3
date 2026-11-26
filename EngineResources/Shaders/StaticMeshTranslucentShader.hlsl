#pragma pack_matrix(row_major)
#include "VertexFactory.hlsli"
#include "GPUSceneCommon.hlsli"
#include "MaterialUV.hlsli"

cbuffer Viewconstants : register(b0)
{
    matrix VP;
    matrix View;
    matrix Projection;
    float4 ViewTimeParams;
};

cbuffer MaterialParams : register(b1)
{
    float4 BaseColor;
    float2 UVOffset;
    float2 TextureFlags;
    float2 UVTiling;
    float2 WorldUVTileSize;
    float4 Emissive;
    float4 SpecularColorShininess;
    float4 WorldUVParams;
};

cbuffer TranslucentFogConstants : register(b3)
{
    float4x4 FogInverseViewProjection;
    float4 FogCameraPosition;
    float4 FogColor;
    float4 FogDensityHeight;
    float4 FogDistanceViewport;
};

struct PS_INPUT
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
    float2 uv : TEXCOORD0;
    float3 worldPosition : TEXCOORD1;
    float3 localPosition : TEXCOORD2;
    float3 localNormal : TEXCOORD3;
    nointerpolation uint primitiveId : TEXCOORD4;
};

Texture2D g_txColor : register(t0);
SamplerState g_Sample : register(s0);
// 머티리얼 그래프 텍스처 샘플러 이름 호환
#define ColorSampler g_Sample

#ifdef MATERIAL_GRAPH
#include MATERIAL_GRAPH_INCLUDE
#endif

float FogTransmittance(float3 Position)
{
    if (FogDensityHeight.x <= 0.0f || FogDistanceViewport.x <= 0.0f)
        return 1.0f;

    float3 Ray = Position - FogCameraPosition.xyz;
    float Distance = length(Ray);
    float Start = FogDensityHeight.w;
    if (Distance <= Start ||
        (FogDistanceViewport.y > 0.0f && Distance > FogDistanceViewport.y))
        return 1.0f;

    float Length = Distance - Start;
    float StartHeight = FogCameraPosition.z + Ray.z * (Start / Distance);
    float Falloff = FogDensityHeight.y;
    float A = abs(Falloff * (Position.z - StartHeight));
    float HeightExponent = -Falloff *
        (min(StartHeight, Position.z) - FogDensityHeight.z);
    float G;
    if (A < 0.01f)
        G = 1.0f - A * 0.5f + A * A / 6.0f;
    else
        G = (1.0f - exp(-A)) / A;
    float LogTau = log(FogDensityHeight.x) + log(Length) +
        HeightExponent + log(G);
    if (!isfinite(LogTau))
        return 1.0f;

    float T = LogTau >= 4.382026635f ? 0.0f
            : LogTau <= -80.0f ? 1.0f
            : exp(-exp(LogTau));
    return max(saturate(T), 1.0f - FogDistanceViewport.x);
}

PS_INPUT mainVS(FVertexFactoryInput input, uint InstanceId : SV_InstanceID)
{
	PS_INPUT output;
	const uint PrimId = LoadScenePrimitiveId(InstanceId);
	const FPrimitiveSceneData Prim = LoadScenePrimitive(InstanceId);
	const FVFIntermediates VF = VF_GetIntermediates(input, Prim.LocalToWorld, Prim.LocalToWorld);

    float4 WorldPosition = float4(VF.WorldPosition, 1.0f);
    output.position = mul(mul(WorldPosition, View), Projection);
    output.color = VF.Color;
    output.uv = VF.UV;
    output.normal = VF.WorldNormal;
    output.worldPosition = WorldPosition.xyz;
    output.localPosition = VF.LocalPosition;
    output.localNormal = VF.LocalNormal;
    output.primitiveId = PrimId;
    return output;
}

float4 mainPS(PS_INPUT input) : SV_TARGET
{
#if VF_MESH_PARTICLE
	const FPrimitiveSceneData Prim = LoadScenePrimitive(0u);
#else
	const FPrimitiveSceneData Prim = PrimitiveSceneData[input.primitiveId];
#endif
	float3 ProjectedT, ProjectedB;
    float2 ResolvedUV = ResolveMaterialUV(input.uv, input.worldPosition, input.normal,
        input.localPosition, input.localNormal, Prim.LocalToWorld,
        UVTiling, UVOffset, WorldUVTileSize, WorldUVParams.x > 0.5f, ProjectedT, ProjectedB);

    float4 texColor;
#ifdef MATERIAL_GRAPH
    float3 ToCam = normalize(FogCameraPosition.xyz - input.worldPosition);
    FMaterialGraphOutput GraphOutput = EvaluateMaterialGraph(ResolvedUV, input.worldPosition, ToCam, normalize(input.normal), input.color, ViewTimeParams.x);
#ifdef MATERIAL_GRAPH_HAS_OPACITY_MASK
    clip(GraphOutput.OpacityMask - 0.001f);
#endif
#ifdef MATERIAL_GRAPH_HAS_BASE_COLOR
    texColor = GraphOutput.BaseColor;
#else
    texColor = g_txColor.Sample(g_Sample, ResolvedUV) * BaseColor;
#endif
#ifdef MATERIAL_GRAPH_HAS_EMISSIVE_COLOR
    float3 ActiveEmissive = GraphOutput.EmissiveColor;
#ifdef MATERIAL_GRAPH_HAS_EMISSIVE_FACTOR
    ActiveEmissive *= GraphOutput.EmissiveFactor;
#endif
    texColor.rgb += ActiveEmissive;
#endif
#else
    texColor = g_txColor.Sample(g_Sample, ResolvedUV);
#if VF_MESH_PARTICLE
    texColor *= input.color * BaseColor;
#endif
    texColor.rgb += Emissive.rgb;
#endif

    texColor.a *= FogTransmittance(input.worldPosition);
    return texColor;
}

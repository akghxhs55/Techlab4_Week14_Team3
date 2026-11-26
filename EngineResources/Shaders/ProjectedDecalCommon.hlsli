#ifndef PROJECTED_DECAL_COMMON_HLSLI
#define PROJECTED_DECAL_COMMON_HLSLI

#include "NormalMapCommon.hlsli"

cbuffer DecalConstants : register(b4)
{
	matrix WorldToDecal;
	float4 TintOpacity;
	float4 TextureParams;

	float4 NormalTangent;
	float4 NormalBitangent;
	float4 NormalDirection;
	float4 NormalParams;
};

Texture2D<float4> DecalColorTexture : register(t0);
Texture2D<float4> DecalNormalTexture : register(t2);

SamplerState DecalSampler : register(s0);

struct FDecalOutput
{
	float4 Color : SV_Target0;
	float4 Normal : SV_Target1;
};

FDecalOutput MakeEmptyDecalOutput()
{
	FDecalOutput Output;
	Output.Color = float4(0, 0, 0, 0);
	Output.Normal = float4(0, 0, 0, 0);
	return Output;
}

FDecalOutput EvaluateProjectedDecalNoDiscard(float3 WorldPosition)
{
	FDecalOutput Output = MakeEmptyDecalOutput();

	float3 DecalPosition =
		mul(
			float4(WorldPosition, 1.0f),
			WorldToDecal
		).xyz;

	// 로컬 박스 밖의 픽셀 제외
	if (any(abs(DecalPosition) > 0.5f))
		return Output;

	// 로컬 X축 투영, YZ를 UV로 사용
	float2 UV = DecalPosition.yz + 0.5f;
	UV.y = 1.0f - UV.y;

	float4 TextureColor = DecalColorTexture.SampleLevel(DecalSampler, UV, 0.0f);
	float3 TextureRGB = TextureColor.rgb;

	// UNORM SRV는 수동으로 선형화
	if (TextureParams.x > 0.5f)
	{
		TextureRGB = pow(max(TextureRGB, 0.0f), 2.2f);
	}

	// 로컬 X축 양 끝 페이드
	float FadeWidth = 0.1f;
	float DistanceToBoundary = 0.5f - abs(DecalPosition.x);
	float DepthFade = smoothstep(0.0f, FadeWidth, DistanceToBoundary);

	float ColorAlpha =
		saturate(TextureColor.a * TintOpacity.a)
		* DepthFade;

	if (ColorAlpha < 0.0001f)
		return Output;

	float3 Color = TextureRGB * TintOpacity.rgb;

	// AlphaBlend 상태가 RGB에 SrcAlpha를 곱하므로 직선 알파로 출력
	Output.Color = float4(Color, ColorAlpha);

	const float Encoding = TextureParams.y;
	const float NormalOpacity = NormalParams.x;

	if (Encoding > 0.5f && NormalOpacity > 0.0f)
	{
		float3 Sample =
			DecalNormalTexture.SampleLevel(DecalSampler, UV, 0.0f).xyz;

		float3 MapNormal = DecodeMapNormal(Sample, Encoding);

		if (TextureParams.z > 0.5f)
			MapNormal.y = -MapNormal.y;

		MapNormal.xy *= TextureParams.w;

		float LengthSquared = dot(MapNormal, MapNormal);

		MapNormal =
			LengthSquared > 1e-8f
			? MapNormal * rsqrt(LengthSquared)
			: float3(0.0f, 0.0f, 1.0f);

		float3 WorldNormal = normalize(
			NormalTangent.xyz * MapNormal.x +
			NormalBitangent.xyz * MapNormal.y +
			NormalDirection.xyz * MapNormal.z);

		float NormalAlpha = saturate(ColorAlpha * NormalOpacity);

		Output.Normal = float4(WorldNormal, NormalAlpha);
	}

	return Output;
}

#endif

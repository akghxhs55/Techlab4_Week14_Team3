#ifndef PROJECTED_DECAL_DEPTH_COMMON_HLSLI
#define PROJECTED_DECAL_DEPTH_COMMON_HLSLI

#pragma pack_matrix(row_major)

#include "ProjectedDecalCommon.hlsli"

cbuffer DecalDepthViewConstants : register(b5)
{
	matrix InverseViewProjection;
	float4 ViewportSize;
};

Texture2D<float> SceneDepthTexture : register(t1);

FDecalOutput EvaluateDepthProjectedDecal(float4 ScreenPosition)
{
	int2 Pixel = int2(ScreenPosition.xy);

	float Depth =
		SceneDepthTexture.Load(int3(Pixel, 0));

	// 깊이 Clear 값 배경 제외
	if (Depth >= 1.0f)
		return MakeEmptyDecalOutput();

	float2 ScreenUV =
		ScreenPosition.xy * ViewportSize.zw;

	float4 ClipPosition = float4(
		ScreenUV.x * 2.0f - 1.0f,
		1.0f - ScreenUV.y * 2.0f,
		Depth,
		1.0f);

	float4 WorldPositionH =
		mul(ClipPosition, InverseViewProjection);

	if (abs(WorldPositionH.w) < 1.0e-8f)
		return MakeEmptyDecalOutput();

	float3 WorldPosition = WorldPositionH.xyz / WorldPositionH.w;

	return EvaluateProjectedDecalNoDiscard(WorldPosition);
}

#endif

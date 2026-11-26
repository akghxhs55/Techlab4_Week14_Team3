#pragma once

#include "Math/Matrix.h"
#include "Math/Vector.h"
#include "Math/Vector4.h"
#include "Render/RenderStates.h"

#include <cmath>
#include <cstddef>

class FTexture2D;

// 컴포넌트에서 복사한 뷰 기준 데칼 입력
struct FProjectedDecalRenderData
{
	FMatrix DecalToWorld = FMatrix::Identity;
	FVector4 TintOpacity = FVector4(1.0f, 1.0f, 1.0f, 1.0f);

	FTexture2D* ColorTexture = nullptr;
	FTexture2D* NormalTexture = nullptr;

	float NormalStrength = 1.0f;
	float NormalOpacity = 1.0f;
	float NormalEncoding = 0.0f;

	bool bFlipNormalY = false;
	ESamplerState Sampler = ESamplerState::LinearClamp;

	bool bDecodeSRGB = true;
	bool bUnlit = false;

	int32 SortOrder = 0;
	uint32 ComponentID = 0;
};

struct alignas(16) FProjectedDecalConstants
{
	FMatrix WorldToDecal;
	FVector4 TintOpacity;
	FVector4 TextureParams;

	FVector4 NormalTangent;
	FVector4 NormalBitangent;
	FVector4 NormalDirection;
	FVector4 NormalParams;
};

struct alignas(16) FProjectedDecalDepthViewConstants
{
	FMatrix InverseViewProjection;
	FVector4 ViewportSize;
};

inline bool IsValidProjectedDecalRenderData(const FProjectedDecalRenderData& Data)
{
	for (int32 Row = 0; Row < 4; ++Row)
	{
		for (int32 Column = 0; Column < 4; ++Column)
		{
			if (!std::isfinite(Data.DecalToWorld.M[Row][Column]))
			{
				return false;
			}
		}
	}

	const float Determinant = Data.DecalToWorld.Determinant();
	return std::isfinite(Determinant) && std::abs(Determinant) > 1e-12f;
}

inline FProjectedDecalConstants MakeProjectedDecalConstants(const FProjectedDecalRenderData& Data)
{
	FProjectedDecalConstants Constants{};
	Constants.WorldToDecal = Data.DecalToWorld.Inverse();
	Constants.TintOpacity = Data.TintOpacity;
	Constants.TextureParams = FVector4(
		Data.bDecodeSRGB ? 1.0f : 0.0f,
		Data.NormalEncoding,
		Data.bFlipNormalY ? 1.0f : 0.0f,
		Data.NormalStrength);

	FVector T = Data.DecalToWorld.TransformVector(FVector(0.0f, 1.0f, 0.0f));
	FVector B = Data.DecalToWorld.TransformVector(FVector(0.0f, 0.0f, -1.0f));

	bool bValidFrame = false;
	if (FVector::Dot(T, T) > 1e-12f)
	{
		T = T.Normalized();
		B = B - T * FVector::Dot(T, B);

		if (FVector::Dot(B, B) > 1e-12f)
		{
			B = B.Normalized();
			const FVector N = FVector::Cross(T, B).Normalized();

			Constants.NormalTangent = FVector4(T.X, T.Y, T.Z, 0.0f);
			Constants.NormalBitangent = FVector4(B.X, B.Y, B.Z, 0.0f);
			Constants.NormalDirection = FVector4(N.X, N.Y, N.Z, 0.0f);
			bValidFrame = true;
		}
	}

	const bool bUseNormal =
		bValidFrame &&
		!Data.bUnlit &&
		Data.NormalTexture &&
		Data.NormalEncoding > 0.0f;

	Constants.NormalParams = FVector4(
		bUseNormal ? Data.NormalOpacity : 0.0f,
		0.0f,
		0.0f,
		0.0f);

	return Constants;
}

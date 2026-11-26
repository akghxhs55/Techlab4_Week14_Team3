#pragma once

#include "Matrix.h"
#include "Vector.h"
#include <algorithm>
#include <cmath>
#include <limits>

// 뷰나 LOD에 무관한 메시 공통 경계구
struct FLODSphere
{
	FVector Center;
	float RadiusSquared = 0.0f;

	static double DotMatrixRows(const FMatrix& World, int I, int J)
	{
		return static_cast<double>(World.M[I][0]) * World.M[J][0]
			+ static_cast<double>(World.M[I][1]) * World.M[J][1]
			+ static_cast<double>(World.M[I][2]) * World.M[J][2];
	}

	FLODSphere Transform(const FMatrix& World) const
	{
		FLODSphere Result;
		Result.Center = World.TransformPosition(Center);
		// 비균등 스케일과 전단을 포함한 보수적 경계 계산
		const double XY = std::abs(DotMatrixRows(World, 0, 1));
		const double XZ = std::abs(DotMatrixRows(World, 0, 2));
		const double YZ = std::abs(DotMatrixRows(World, 1, 2));
		const double MaxStretchSquared = std::max({
			DotMatrixRows(World, 0, 0) + XY + XZ,
			DotMatrixRows(World, 1, 1) + XY + YZ,
			DotMatrixRows(World, 2, 2) + XZ + YZ });
		Result.RadiusSquared = std::nextafter(
			static_cast<float>(static_cast<double>(RadiusSquared) * MaxStretchSquared),
			std::numeric_limits<float>::infinity());
		return Result;
	}
};

struct FSphere
{
    FSphere(FVector InCenter, float InRadius) : Center(InCenter), Radius(InRadius) {}

    FVector Center;
    float Radius;
};

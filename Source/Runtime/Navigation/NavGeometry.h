#pragma once

#include "Containers/Array.h"
#include "Math/Vector.h"

struct FMatrix;
struct FStaticMeshData;

namespace Navigation
{
	// Recast 좌표계에서의 정점과 삼각형 정보
	struct FRecastGeometry
	{
		TArray<float> Vertices;		// x, y, z 순서
		TArray<int32> Triangles;	// Vertices의 정점 번호 세 개씩
	};

	FVector ToRecast(const FVector& WorldPosition);
	FVector FromRecast(const FVector& RecastPosition);

	FRecastGeometry ConvertStaticMesh(
		const FStaticMeshData& Mesh, const FMatrix& LocalToWorld);

#if !defined(ENGINE_SHIPPING)
	bool RunNavigationSmokeTest();
#endif
}

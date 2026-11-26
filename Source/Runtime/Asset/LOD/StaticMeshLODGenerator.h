#pragma once

#include "Render/StaticMeshData.h"
#include "Core/Types.h"
#include <array>

struct FLODGenerateRequest
{
	// 화면 크기 임계값
	std::array<float, 3> ScreenThresholds{ 0.30f, 0.10f, 0.03f };
	bool bSaveToAsset = true;
};

// 생성 결과와 실패 원인 반환 구조체
struct FLODGenerateResult
{
	bool bSuccess = false;
	uint32 TargetTriangles[4]{};
	uint32 ActualTriangles[4]{};

	// 원본 대비 부피 비율
	float VolumeRatio[4]{ 1.0f, 1.0f, 1.0f, 1.0f };

	uint32 RejectedFeatureEdges = 0;
	uint32 RejectedTopology = 0;
	uint32 RejectedFlips = 0;
	uint32 RejectedError = 0;

	FString FailureReason;
};

class FStaticMeshLODGenerator
{
public:
	// 원본 메시를 기반으로 세 단계 단순화 결과 생성
	static FLODGenerateResult Generate(const FStaticMeshData& LOD0, TArray<FStaticMeshData>& OutLODs);
};

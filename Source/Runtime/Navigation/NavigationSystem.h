#pragma once

#include "Containers/Array.h"
#include "Core/Types.h"
#include "Math/Vector.h"

class UWorld;
struct FNavMeshData;

struct FNavAgentProperties
{
	float AgentRadius = 0.50f;
	float AgentHeight = 2.00f;
	float WalkbleFloorAngle = 45.0f;
	float MaxStepHeight = 0.45f;
};

struct FNavBuildSettings
{
	float CellSize = 0.10f;
	float CellHeight = 0.025f;
};

struct FNavMeshDebugGeometry
{
	TArray<FVector> FaceVertices;
	TArray<FVector> BoundaryVertices;
};

class FNavigationSystem
{
public:
	FNavigationSystem();
	~FNavigationSystem();

	// 월드의 현재 레벨에서 지오메트리와 에이전트 설정을 읽어 NavMesh를 빌드하고 활성화한다.
	// 빌드에 실패하면 기존 데이터도 비우고 false를 반환한다.
	bool BuildNavigation(const UWorld& World);

	// 원본의 빌드된 타일을 복사해 이 시스템의 NavMesh와 쿼리를 구성한다.
	// 원본이 비어 있으면 이 시스템도 비우고 true를 반환한다.
	// 설치 실패 시에는 비우고 false를 반환한다.
	bool CopyBuiltNavigationFrom(const FNavigationSystem& Source);

	// 활성 NavMesh, 쿼리, 디버그 지오메트리를 해제한다.
	// 활성 데이터가 있었을 때만 버전을 올린다.
	void ResetNavigation();

	const FNavMeshDebugGeometry* GetDebugGeometry() const;
	uint64 GetNavMeshVersion() const { return NavMeshVersion; }

private:
	// 빌드된 타일 바이트로 Detour NavMesh, 쿼리, 디버그 지오메트리를 준비한다.
	// 모든 준비가 성공하면 활성 데이터를 교체하고 버전을 올린다.
	bool InstallTile(TArray<unsigned char> BakedTile);

private:
	TUniquePtr<FNavMeshData> ActiveData;
	uint64 NavMeshVersion = 0;
};

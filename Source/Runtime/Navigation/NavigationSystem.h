#pragma once

#include "Containers/Array.h"
#include "Core/Types.h"
#include "Math/Vector.h"
#include "Navigation/NavMeshBuildDebug.h"

class UWorld;
struct FNavMeshData;
class AActor;
class ARecastNavMesh;

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
	//FNavigationSystem();
	//~FNavigationSystem();

	// 월드의 현재 레벨에서 지오메트리와 에이전트 설정을 읽어 NavMesh를 빌드하고 활성화한다.
	// 빌드에 실패하면 기존 데이터도 비우고 false를 반환한다.
	bool BuildNavigation(UWorld& World, bool bCaptureBuildDebug = false);

	void RestoreNavigation(const UWorld& World);
	void UnregisterNavData(const AActor* Actor);

	// 활성 NavMesh, 쿼리, 디버그 지오메트리를 해제한다.
	// 활성 데이터가 있었을 때만 버전을 올린다.
	void ResetNavigation();

	const ARecastNavMesh* GetMainNavData() const { return MainNavData; }
	const FNavMeshDebugGeometry* GetDebugGeometry() const;
	uint64 GetNavMeshVersion() const { return NavMeshVersion; }

	FNavMeshBuildDebugData* GetBuildDebugData() { return BuildDebugData.get(); }
	const FNavMeshBuildDebugData* GetBuildDebugData() const { return BuildDebugData.get(); }

private:

private:
	ARecastNavMesh* MainNavData = nullptr;
	uint64 NavMeshVersion = 0;

	TUniquePtr<FNavMeshBuildDebugData> BuildDebugData;
};

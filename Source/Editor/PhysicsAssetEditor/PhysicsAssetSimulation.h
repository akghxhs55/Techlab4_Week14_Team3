#pragma once

#include "Containers/Array.h"
#include "Core/EngineString.h"
#include "Math/Transform.h"

class USkeletalMeshComponent;
class UStaticMeshComponent;
class USceneComponent;
class UPhysicsAsset;
class FPhysScene;

// 프리뷰 시뮬레이션의 생성/종료와 시작 포즈 복원을 관리합니다.
class FPhysicsAssetSimulation
{
public:
	FPhysicsAssetSimulation() = default;
	FPhysicsAssetSimulation(const FPhysicsAssetSimulation&) = delete;
	FPhysicsAssetSimulation& operator=(const FPhysicsAssetSimulation&) = delete;
	~FPhysicsAssetSimulation();
	// INDEX_NONE이면 전체, 그 외에는 해당 본과 하위 본의 바디만 시뮬레이션한다.
	bool Start(USkeletalMeshComponent& Component, UStaticMeshComponent& PreviewFloor, UPhysicsAsset& Asset, FString& OutError,
		int32 RootBoneIndex = INDEX_NONE);
	void Stop();
	void Tick(float DeltaTime);
	bool IsActive() const { return bActive; }

private:
	USkeletalMeshComponent* MeshComponent = nullptr;
	UStaticMeshComponent* FloorComponent = nullptr;
	USceneComponent* TransformTarget = nullptr;
	FPhysScene* PhysicsScene = nullptr;
	TArray<FTransform> SavedPose;
	FTransform SavedTargetTransform;
	FTransform SavedMeshTransform;
	bool bActive = false;
};

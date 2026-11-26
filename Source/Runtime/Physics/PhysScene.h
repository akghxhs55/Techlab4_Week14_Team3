#pragma once

#include <memory>
#include "Core/Types.h"
#include "Containers/Array.h"
#include "ClothBridge.h"

struct FPhysicsSettings;
class UWorld;
class UPrimitiveComponent;
class USkeletalMeshComponent;
class UClothComponent;
struct FBodyInstance;

namespace physx {
	class PxScene;
	class PxSimulationEventCallback;
}

// 1. 이벤트 데이터 구조체 (Hit용, Overlap용)
struct FPendingHitEvent
{
	UPrimitiveComponent* CompA = nullptr;
	UPrimitiveComponent* CompB = nullptr;
};

struct FPendingTriggerEvent
{
	UPrimitiveComponent* TriggerComp = nullptr;
	UPrimitiveComponent* OtherComp = nullptr;
	bool bIsBegin = true; // true: BeginOverlap, false: EndOverlap
};

struct FPhysicsContactImpact
{
	USkeletalMeshComponent* TargetComponent = nullptr;
	const FBodyInstance* HitBody = nullptr;
	FVector WorldLocation = FVector::ZeroVector;
	FVector WorldNormal = FVector(0.0f, 0.0f, 1.0f);
	FVector WorldImpulse = FVector::ZeroVector;
	FVector WorldSlideVelocity = FVector::ZeroVector;
	float ImpulseMagnitude = 0.0f;
};

// 월드별 마지막 완료 프레임. 카운터는 마지막 서브스텝, 시간은 모든 서브스텝의 합이다.
struct FPhysicsSceneStats
{
	bool bHasSimulatedFrame = false;
	uint32 StaticBodies = 0;
	uint32 DynamicBodies = 0; // Sleep 포함, Kinematic 제외
	uint32 ActiveDynamicBodies = 0;
	uint32 KinematicBodies = 0;
	uint32 ActiveKinematicBodies = 0;
	uint32 Shapes = 0;
	uint32 ActiveConstraints = 0;
	uint32 ContactPairs = 0; // 접촉이 생성된 non-CCD Shape 쌍 (접촉점/Hit 이벤트 수가 아님)
	int32 Substeps = 0;
	float StepDeltaTimeMs = 0.0f;

	// 호출 스레드에서 소비한 시간. Start/EndFrame 사이의 다른 게임 로직은 제외하며,
	// PhysX worker들의 CPU 시간을 합산한 값은 아니다.
	double SimulateCallsMs = 0.0;
	double FetchResultsMs = 0.0; // 완료 대기와 PhysX 콜백 처리 포함
	double SyncComponentsMs = 0.0;
	double DispatchEventsMs = 0.0;
	double GetTotalCallsMs() const { return SimulateCallsMs + FetchResultsMs + SyncComponentsMs + DispatchEventsMs; }
};

class FPhysScene;

// 2. 멀티스레드 씬 락 (Scoped Lock)
class FPhysSceneReadLock
{
public:
	explicit FPhysSceneReadLock(FPhysScene* InScene, const char* InFile = nullptr, uint32 InLine = 0);
	explicit FPhysSceneReadLock(FPhysScene& InScene, const char* InFile = nullptr, uint32 InLine = 0);
	~FPhysSceneReadLock();

	FPhysSceneReadLock(const FPhysSceneReadLock&) = delete;
	FPhysSceneReadLock& operator=(const FPhysSceneReadLock&) = delete;

private:
	FPhysScene* PhysScene = nullptr;
};

class FPhysSceneWriteLock
{
public:
	explicit FPhysSceneWriteLock(FPhysScene* InScene, const char* InFile = nullptr, uint32 InLine = 0);
	explicit FPhysSceneWriteLock(FPhysScene& InScene, const char* InFile = nullptr, uint32 InLine = 0);
	~FPhysSceneWriteLock();

	FPhysSceneWriteLock(const FPhysSceneWriteLock&) = delete;
	FPhysSceneWriteLock& operator=(const FPhysSceneWriteLock&) = delete;

private:
	FPhysScene* PhysScene = nullptr;
};

class FPhysScene
{
public:
	explicit FPhysScene(UWorld* InOwner);
	~FPhysScene();

	void SetUpForFrame(float DeltaSeconds, const FPhysicsSettings& Settings);
	void StartFrame();
	void EndFrame();

	physx::PxScene* GetPxScene() const { return Scene; }
	UWorld* GetOwningWorld() const { return Owner; }
	const FPhysicsSceneStats& GetStats() const { return Stats; }

	// ---- 멀티스레드 락 인터페이스 ----
	void LockRead(const char* File = nullptr, uint32 Line = 0);
	void UnlockRead();
	void LockWrite(const char* File = nullptr, uint32 Line = 0);
	void UnlockWrite();

	void AddPendingHit(UPrimitiveComponent* A, UPrimitiveComponent* B);
	void AddPendingTrigger(UPrimitiveComponent* TriggerComp, UPrimitiveComponent* OtherComp, bool bIsBegin);
	void QueueContactImpact(const FPhysicsContactImpact& Impact);
	void RegisterClothComponent(UClothComponent* Component);
	void UnregisterClothComponent(UClothComponent* Component);

private:
	void SyncComponentsToBodies();
	void DispatchPendingEvents();

	UWorld* Owner = nullptr;
	physx::PxScene* Scene = nullptr;
	FClothSolverHandle ClothSolver = nullptr;
	float FrameDeltaTime = 0.0f;
	float SubstepDeltaTime = 0.0f;
	int32 NumSubsteps = 0;
	bool bPhysXSceneExecuting = false;
	uint64 SimulateCallCycles = 0;
	FPhysicsSceneStats Stats;

	// PhysX 콜백 수신기 (PIMPL로 헤더 의존성 최소화)
	std::unique_ptr<physx::PxSimulationEventCallback> SimEventCallback;

	TArray<UClothComponent*> ClothComponents;
	TArray<FPendingHitEvent> PendingHitEvents;
	TArray<FPendingTriggerEvent> PendingTriggerEvents;
	TArray<FPhysicsContactImpact> PendingContactImpacts;
};

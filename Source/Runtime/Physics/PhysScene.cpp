#include "EnginePCH.h"
#include "PhysScene.h"

#include <PxPhysics.h>
#include <PxScene.h>
#include <PxSceneDesc.h>
#include <PxSimulationEventCallback.h>
#include <PxSimulationStatistics.h>
#include <PxFiltering.h>

#include "BodyInstance.h"
#include "ClothCore.h"
#include "PhysicsCore.h"
#include "PhysicsSettings.h"
#include "PhysXConversion.h"
#include "Components/ClothComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Core/Windows/WindowsPlatformTime.h"
#include <PxRigidDynamic.h>

using namespace physx;

namespace
{
	PxFilterFlags PhysXSimulationFilterShader(
		PxFilterObjectAttributes attributes0, PxFilterData filterData0,
		PxFilterObjectAttributes attributes1, PxFilterData filterData1,
		PxPairFlags& pairFlags, const void* constantBlock, PxU32 constantBlockSize)
	{
		// 트리거 셰이프가 포함된 경우
		if (PxFilterObjectIsTrigger(attributes0) || PxFilterObjectIsTrigger(attributes1))
		{
			pairFlags = PxPairFlag::eTRIGGER_DEFAULT;
			return PxFilterFlag::eDEFAULT;
		}

		// 일반 충돌 바디
		pairFlags = PxPairFlag::eCONTACT_DEFAULT;
		pairFlags |= PxPairFlag::eNOTIFY_TOUCH_FOUND;
		pairFlags |= PxPairFlag::eNOTIFY_TOUCH_PERSISTS;
		pairFlags |= PxPairFlag::eNOTIFY_TOUCH_LOST;
		pairFlags |= PxPairFlag::eNOTIFY_CONTACT_POINTS;
		return PxFilterFlag::eDEFAULT;
	}

	class FPhysXSimulationEventCallback final : public PxSimulationEventCallback
	{
	public:
		explicit FPhysXSimulationEventCallback(FPhysScene* InScene)
			: Scene(InScene)
		{
		}

		void onContact(const PxContactPairHeader& pairHeader, const PxContactPair* pairs, PxU32 nbPairs) override
		{
			if (!Scene) return;

			if (pairHeader.flags & (PxContactPairHeaderFlag::eREMOVED_ACTOR_0 | PxContactPairHeaderFlag::eREMOVED_ACTOR_1))
			{
				return;
			}

			auto* Body0 = pairHeader.actors[0] ? static_cast<FBodyInstance*>(pairHeader.actors[0]->userData) : nullptr;
			auto* Body1 = pairHeader.actors[1] ? static_cast<FBodyInstance*>(pairHeader.actors[1]->userData) : nullptr;

			UPrimitiveComponent* Comp0 = Body0 ? Body0->OwnerComponent : nullptr;
			UPrimitiveComponent* Comp1 = Body1 ? Body1->OwnerComponent : nullptr;

			if (!Comp0 || !Comp1 || Comp0 == Comp1) return;

			bool bAddedHit = false;
			const bool bSkel0 = Body0 && Body0->BoneIndex != INDEX_NONE;
			const bool bSkel1 = Body1 && Body1->BoneIndex != INDEX_NONE;
			const PxRigidDynamic* Dyn0 = (bSkel0 || bSkel1) && pairHeader.actors[0] ? pairHeader.actors[0]->is<PxRigidDynamic>() : nullptr;
			const PxRigidDynamic* Dyn1 = (bSkel0 || bSkel1) && pairHeader.actors[1] ? pairHeader.actors[1]->is<PxRigidDynamic>() : nullptr;
			const FVector Vel0 = Dyn0 ? P2KVector(Dyn0->getLinearVelocity()) : FVector::ZeroVector;
			const FVector Vel1 = Dyn1 ? P2KVector(Dyn1->getLinearVelocity()) : FVector::ZeroVector;
			PxContactPairPoint ContactPoints[16];

			for (PxU32 i = 0; i < nbPairs; ++i)
			{
				const PxContactPair& cp = pairs[i];
				if (cp.flags & (PxContactPairFlag::eREMOVED_SHAPE_0 | PxContactPairFlag::eREMOVED_SHAPE_1))
				{
					continue;
				}

				if (!bAddedHit && (cp.events & PxPairFlag::eNOTIFY_TOUCH_FOUND))
				{
					Scene->AddPendingHit(Comp0, Comp1);
					bAddedHit = true;
				}

				if (bSkel0 || bSkel1)
				{
					const PxU32 PointCount = cp.extractContacts(ContactPoints, 16);
					if (PointCount == 0)
					{
						continue;
					}

					PxVec3 TotalImpulse(0.0f, 0.0f, 0.0f);
					PxVec3 BestPos = ContactPoints[0].position;
					PxVec3 BestNormal = ContactPoints[0].normal;
					float BestPointImpulseSq = -1.0f;
					for (PxU32 c = 0; c < PointCount; ++c)
					{
						TotalImpulse += ContactPoints[c].impulse;
						const float MagSq = ContactPoints[c].impulse.magnitudeSquared();
						if (MagSq > BestPointImpulseSq)
						{
							BestPointImpulseSq = MagSq;
							BestPos = ContactPoints[c].position;
							BestNormal = ContactPoints[c].normal;
						}
					}

					const float ImpulseMag = TotalImpulse.magnitude();
					if (ImpulseMag <= 1e-4f)
					{
						continue;
					}

					const FVector WorldPos = P2KVector(BestPos);
					const FVector WorldNormal = P2KVector(BestNormal);
					const FVector WorldImpulse = P2KVector(TotalImpulse);

					if (bSkel0)
					{
						if (USkeletalMeshComponent* SkelComp = Cast<USkeletalMeshComponent>(Comp0))
						{
							const FVector RelVel = Vel0 - Vel1;
							const FVector SlideVel = RelVel - WorldNormal * RelVel.Dot(WorldNormal);
							FPhysicsContactImpact Impact{};
							Impact.TargetComponent = SkelComp;
							Impact.HitBody = Body0;
							Impact.WorldLocation = WorldPos;
							Impact.WorldNormal = WorldNormal;
							Impact.WorldImpulse = WorldImpulse;
							Impact.WorldSlideVelocity = SlideVel;
							Impact.ImpulseMagnitude = ImpulseMag;
							Scene->QueueContactImpact(Impact);
						}
					}

					if (bSkel1)
					{
						if (USkeletalMeshComponent* SkelComp = Cast<USkeletalMeshComponent>(Comp1))
						{
							const FVector RelVel = Vel1 - Vel0;
							const FVector InvNormal = WorldNormal * -1.0f;
							const FVector SlideVel = RelVel - InvNormal * RelVel.Dot(InvNormal);
							FPhysicsContactImpact Impact{};
							Impact.TargetComponent = SkelComp;
							Impact.HitBody = Body1;
							Impact.WorldLocation = WorldPos;
							Impact.WorldNormal = InvNormal;
							Impact.WorldImpulse = WorldImpulse * -1.0f;
							Impact.WorldSlideVelocity = SlideVel;
							Impact.ImpulseMagnitude = ImpulseMag;
							Scene->QueueContactImpact(Impact);
						}
					}
				}
			}
		}

		void onTrigger(PxTriggerPair* pairs, PxU32 count) override
		{
			if (!Scene) return;

			for (PxU32 i = 0; i < count; ++i)
			{
				const PxTriggerPair& tp = pairs[i];

				if (tp.flags & (PxTriggerPairFlag::eREMOVED_SHAPE_TRIGGER | PxTriggerPairFlag::eREMOVED_SHAPE_OTHER))
				{
					continue;
				}

				auto* TriggerBody = tp.triggerActor ? static_cast<FBodyInstance*>(tp.triggerActor->userData) : nullptr;
				auto* OtherBody = tp.otherActor ? static_cast<FBodyInstance*>(tp.otherActor->userData) : nullptr;

				UPrimitiveComponent* TriggerComp = TriggerBody ? TriggerBody->OwnerComponent : nullptr;
				UPrimitiveComponent* OtherComp = OtherBody ? OtherBody->OwnerComponent : nullptr;

				if (!TriggerComp || !OtherComp || TriggerComp == OtherComp) continue;

				if (tp.status & PxPairFlag::eNOTIFY_TOUCH_FOUND)
				{
					Scene->AddPendingTrigger(TriggerComp, OtherComp, true);
				}
				else if (tp.status & PxPairFlag::eNOTIFY_TOUCH_LOST)
				{
					Scene->AddPendingTrigger(TriggerComp, OtherComp, false);
				}
			}
		}

		void onConstraintBreak(PxConstraintInfo* constraints, PxU32 count) override {}
		void onWake(PxActor** actors, PxU32 count) override {}
		void onSleep(PxActor** actors, PxU32 count) override {}
		void onAdvance(const PxRigidBody*const* bodyBuffer, const PxTransform* poseBuffer, const PxU32 count) override {}

	private:
		FPhysScene* Scene = nullptr;
	};
}

FPhysScene::FPhysScene(UWorld* InOwner)
	: Owner(InOwner)
{
	if (FClothCore::GetFactory())
	{
		ClothSolver = ClothBridge_CreateSolver(FClothCore::GetFactory());
	}

	PxPhysics* Physics = FPhysicsCore::GetPhysics();
	if (!Physics)
	{
		return;
	}

	SimEventCallback = std::make_unique<FPhysXSimulationEventCallback>(this);

	PxSceneDesc SceneDesc{ Physics->getTolerancesScale() };
	SceneDesc.gravity = PxVec3{ 0.0f, 0.0f, -9.81f };
	SceneDesc.cpuDispatcher = FPhysicsCore::GetCpuDispatcher();
	SceneDesc.filterShader = PhysXSimulationFilterShader;
	SceneDesc.simulationEventCallback = SimEventCallback.get();
	SceneDesc.flags |= PxSceneFlag::eENABLE_ACTIVE_ACTORS;
	SceneDesc.userData = this;

	if (!SceneDesc.isValid())
	{
		KRS_LOG(Error, "[PhysX] Invalid PxSceneDesc");
		return;
	}

	Scene = Physics->createScene(SceneDesc);
	if (!Scene)
	{
		KRS_LOG(Error, "[PhysX] Failed to create PxScene");
	}
}

FPhysScene::~FPhysScene()
{
	if (ClothSolver)
	{
		for (UClothComponent* ClothComp : ClothComponents)
		{
			if (ClothComp)
			{
				ClothBridge_RemoveClothFromSolver(ClothSolver, ClothComp->GetClothHandle());
			}
		}
		ClothComponents.Reset();
		ClothBridge_DestroySolver(ClothSolver);
		ClothSolver = nullptr;
	}
	if (Scene)
	{
		if (bPhysXSceneExecuting)
		{
			Scene->fetchResults(true);
		}
		Scene->release();
		Scene = nullptr;
	}
	SimEventCallback.reset();
	PendingHitEvents.Empty();
	PendingTriggerEvents.Empty();
	PendingContactImpacts.Empty();
}

void FPhysScene::SetUpForFrame(float DeltaSeconds, const FPhysicsSettings& Settings)
{
	assert(!bPhysXSceneExecuting);

	FrameDeltaTime = 0.0f;
	SubstepDeltaTime = 0.0f;
	NumSubsteps = 0;

	if (!Scene || DeltaSeconds <= 0.0f)
	{
		return;
	}

	const PxVec3 NewGravity(0.0f, 0.0f, Settings.DefaultGravityZ);
	if (Scene->getGravity() != NewGravity)
	{
		Scene->setGravity(NewGravity);
	}

	const bool bUseSubstep = Settings.bSubstepping
		&& Settings.MaxSubstepDeltaTime > 0.0f
		&& Settings.MaxSubsteps > 0;

	if (bUseSubstep)
	{
		FrameDeltaTime = std::min(DeltaSeconds, Settings.MaxSubstepDeltaTime * static_cast<float>(Settings.MaxSubsteps));

		const int32 Needed = static_cast<int32>(std::ceil(FrameDeltaTime / Settings.MaxSubstepDeltaTime));
		NumSubsteps = std::clamp(Needed, 1, Settings.MaxSubsteps);
		SubstepDeltaTime = FrameDeltaTime / static_cast<float>(NumSubsteps);
	}
	else
	{
		FrameDeltaTime = std::min(DeltaSeconds, Settings.MaxPhysicsDeltaTime);
		NumSubsteps = 1;
		SubstepDeltaTime = FrameDeltaTime;
	}
}

void FPhysScene::StartFrame()
{
	if (ClothSolver && FrameDeltaTime > 0.0f)
	{
		for (UClothComponent* ClothComp : ClothComponents)
		{
			if (ClothComp && ClothComp->IsRegistered())
			{
				ClothComp->PreClothSimulate();
			}
		}
	}

	if (!Scene || NumSubsteps == 0)
	{
		return;
	}

	const uint64 StartCycles = FPlatformTime::GetCycles64();
	Scene->simulate(SubstepDeltaTime);
	SimulateCallCycles = FPlatformTime::GetCycles64() - StartCycles;
	bPhysXSceneExecuting = true;
}

void FPhysScene::EndFrame()
{
	if (!bPhysXSceneExecuting)
	{
		return;
	}

	uint64 StartCycles = FPlatformTime::GetCycles64();
	Scene->fetchResults(true);
	uint64 FetchResultCycles = FPlatformTime::GetCycles64() - StartCycles;
	for (int32 i = 1; i < NumSubsteps; ++i)
	{
		StartCycles = FPlatformTime::GetCycles64();
		Scene->simulate(SubstepDeltaTime);
		SimulateCallCycles += FPlatformTime::GetCycles64() - StartCycles;
		StartCycles = FPlatformTime::GetCycles64();
		Scene->fetchResults(true);
		FetchResultCycles += FPlatformTime::GetCycles64() - StartCycles;
	}
	bPhysXSceneExecuting = false;

	// PhysX 통계는 시뮬레이션 완료 후에만 조회한다. 서브스텝 카운터를 합산하지 않는다.
	PxSimulationStatistics SimulationStats;
	Scene->getSimulationStatistics(SimulationStats);
	FPhysicsSceneStats FrameStats;
	FrameStats.bHasSimulatedFrame = true;
	FrameStats.StaticBodies = SimulationStats.nbStaticBodies;
	FrameStats.DynamicBodies = SimulationStats.nbDynamicBodies;
	FrameStats.ActiveDynamicBodies = SimulationStats.nbActiveDynamicBodies;
	FrameStats.KinematicBodies = SimulationStats.nbKinematicBodies;
	FrameStats.ActiveKinematicBodies = SimulationStats.nbActiveKinematicBodies;
	for (PxU32 ShapeCount : SimulationStats.nbShapes)
	{
		FrameStats.Shapes += ShapeCount;
	}
	FrameStats.ActiveConstraints = SimulationStats.nbActiveConstraints;
	FrameStats.ContactPairs = SimulationStats.nbDiscreteContactPairsWithContacts;
	FrameStats.Substeps = NumSubsteps;
	FrameStats.StepDeltaTimeMs = SubstepDeltaTime * 1000.0f;
	FrameStats.SimulateCallsMs = FPlatformTime::ToMilliseconds64(SimulateCallCycles);
	FrameStats.FetchResultsMs = FPlatformTime::ToMilliseconds64(FetchResultCycles);

	// 1. 컴포넌트 위치 동기화
	StartCycles = FPlatformTime::GetCycles64();
	SyncComponentsToBodies();
	FrameStats.SyncComponentsMs = FPlatformTime::ToMilliseconds64(FPlatformTime::GetCycles64() - StartCycles);

	// 2. 누적된 충돌/오버랩 이벤트 디스패치
	StartCycles = FPlatformTime::GetCycles64();
	DispatchPendingEvents();
	FrameStats.DispatchEventsMs = FPlatformTime::ToMilliseconds64(FPlatformTime::GetCycles64() - StartCycles);
	Stats = FrameStats;

	if (ClothSolver && FrameDeltaTime > 0.0f && !ClothComponents.IsEmpty())
	{
		ClothBridge_StepSolver(ClothSolver, FrameDeltaTime);
		for (UClothComponent* ClothComp : ClothComponents)
		{
			if (ClothComp && ClothComp->IsRegistered())
			{
				ClothComp->PostClothSimulate();
			}
		}
	}
}

void FPhysScene::SyncComponentsToBodies()
{
	PxU32 Num = 0;
	PxActor** Actors = Scene->getActiveActors(Num);
	for (PxU32 i = 0; i < Num; ++i)
	{
		auto* Body = static_cast<FBodyInstance*>(Actors[i]->userData);
		if (Body && Body->BoneIndex == INDEX_NONE && Body->OwnerComponent)
		{
			Body->OwnerComponent->SyncComponentToRBPhysics();
		}
	}
}

void FPhysScene::AddPendingHit(UPrimitiveComponent* A, UPrimitiveComponent* B)
{
	PendingHitEvents.Add({ A, B });
}

void FPhysScene::AddPendingTrigger(UPrimitiveComponent* TriggerComp, UPrimitiveComponent* OtherComp, bool bIsBegin)
{
	PendingTriggerEvents.Add({ TriggerComp, OtherComp, bIsBegin });
}

void FPhysScene::QueueContactImpact(const FPhysicsContactImpact& Impact)
{
	PendingContactImpacts.Add(Impact);
}

void FPhysScene::RegisterClothComponent(UClothComponent* Component)
{
	if (!Component || !ClothSolver || !Component->GetClothHandle())
	{
		return;
	}
	if (!ClothComponents.Contains(Component))
	{
		ClothComponents.Add(Component);
		ClothBridge_AddClothToSolver(ClothSolver, Component->GetClothHandle());
	}
}

void FPhysScene::UnregisterClothComponent(UClothComponent* Component)
{
	if (!Component)
	{
		return;
	}
	if (ClothComponents.Contains(Component))
	{
		if (ClothSolver && Component->GetClothHandle())
		{
			ClothBridge_RemoveClothFromSolver(ClothSolver, Component->GetClothHandle());
		}
		ClothComponents.Remove(Component);
	}
}

void FPhysScene::DispatchPendingEvents()
{
	// 스켈레탈 바디 접촉 충격 이벤트 전달
	TArray<FPhysicsContactImpact> Impacts = std::move(PendingContactImpacts);
	PendingContactImpacts.Empty();
	for (const FPhysicsContactImpact& Impact : Impacts)
	{
		if (Impact.TargetComponent && Impact.TargetComponent->IsRegistered())
		{
			Impact.TargetComponent->NotifyPhysicsContactImpact(Impact);
		}
	}
	// Hit 이벤트 처리
	TArray<FPendingHitEvent> Hits = std::move(PendingHitEvents);
	PendingHitEvents.Empty();

	for (const FPendingHitEvent& Hit : Hits)
	{
		if (!Hit.CompA || !Hit.CompA->IsRegistered() || !Hit.CompB || !Hit.CompB->IsRegistered())
		{
			continue;
		}

		Hit.CompA->DispatchHit(Hit.CompB);
		if (Hit.CompB->IsRegistered())
		{
			Hit.CompB->DispatchHit(Hit.CompA);
		}
	}

	// Trigger(Overlap) 이벤트 처리
	TArray<FPendingTriggerEvent> Triggers = std::move(PendingTriggerEvents);
	PendingTriggerEvents.Empty();

	for (const FPendingTriggerEvent& Trigger : Triggers)
	{
		if (!Trigger.TriggerComp || !Trigger.TriggerComp->IsRegistered() ||
			!Trigger.OtherComp || !Trigger.OtherComp->IsRegistered())
		{
			continue;
		}

		if (Trigger.bIsBegin)
		{
			Trigger.TriggerComp->BeginComponentOverlap(Trigger.OtherComp);
			if (Trigger.OtherComp->IsRegistered())
			{
				Trigger.OtherComp->BeginComponentOverlap(Trigger.TriggerComp);
			}
		}
		else
		{
			Trigger.TriggerComp->EndComponentOverlap(Trigger.OtherComp);
			if (Trigger.OtherComp->IsRegistered())
			{
				Trigger.OtherComp->EndComponentOverlap(Trigger.TriggerComp);
			}
		}
	}
}

void FPhysScene::LockRead(const char* File, uint32 Line)
{
	if (Scene)
	{
		Scene->lockRead(File, Line);
	}
}

void FPhysScene::UnlockRead()
{
	if (Scene)
	{
		Scene->unlockRead();
	}
}

void FPhysScene::LockWrite(const char* File, uint32 Line)
{
	if (Scene)
	{
		Scene->lockWrite(File, Line);
	}
}

void FPhysScene::UnlockWrite()
{
	if (Scene)
	{
		Scene->unlockWrite();
	}
}

FPhysSceneReadLock::FPhysSceneReadLock(FPhysScene* InScene, const char* InFile, uint32 InLine)
	: PhysScene(InScene)
{
	if (PhysScene)
	{
		PhysScene->LockRead(InFile, InLine);
	}
}

FPhysSceneReadLock::FPhysSceneReadLock(FPhysScene& InScene, const char* InFile, uint32 InLine)
	: PhysScene(&InScene)
{
	PhysScene->LockRead(InFile, InLine);
}

FPhysSceneReadLock::~FPhysSceneReadLock()
{
	if (PhysScene)
	{
		PhysScene->UnlockRead();
	}
}

FPhysSceneWriteLock::FPhysSceneWriteLock(FPhysScene* InScene, const char* InFile, uint32 InLine)
	: PhysScene(InScene)
{
	if (PhysScene)
	{
		PhysScene->LockWrite(InFile, InLine);
	}
}

FPhysSceneWriteLock::FPhysSceneWriteLock(FPhysScene& InScene, const char* InFile, uint32 InLine)
	: PhysScene(&InScene)
{
	PhysScene->LockWrite(InFile, InLine);
}

FPhysSceneWriteLock::~FPhysSceneWriteLock()
{
	if (PhysScene)
	{
		PhysScene->UnlockWrite();
	}
}

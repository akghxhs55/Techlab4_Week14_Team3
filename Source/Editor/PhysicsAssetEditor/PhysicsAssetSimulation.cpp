#include "EnginePCH.h"
#include "PhysicsAssetSimulation.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Physics/PhysicsAsset.h"
#include "Physics/BodySetup.h"
#include "Physics/PhysicsCore.h"
#include "Physics/PhysScene.h"
#include "Physics/PhysicsSettings.h"
#include "Render/SkeletalMesh.h"

#include <PxPhysics.h>
#include <PxScene.h>
#include <cmath>

namespace
{
	bool IsFiniteVector(const FVector& Value)
	{
		return std::isfinite(Value.X) && std::isfinite(Value.Y) && std::isfinite(Value.Z);
	}

	bool IsValidShapePose(const FShapeElem& Shape)
	{
		return IsFiniteVector(Shape.Center) && std::isfinite(Shape.Rotation.Pitch) &&
			std::isfinite(Shape.Rotation.Yaw) && std::isfinite(Shape.Rotation.Roll);
	}

	bool ValidateSimulationAsset(const UPhysicsAsset& Asset, const USkeletalMesh& Mesh, FString& OutError)
	{
		if (Asset.BodySetups.IsEmpty())
		{
			OutError = "Add physics bodies before starting simulation.";
			return false;
		}
		// 런타임 검증에 넘기기 전에 null과 PhysX에 전달할 형상 수치를 검사합니다.
		for (const UBodySetup* Body : Asset.BodySetups)
		{
			if (!Body) { OutError = "The physics asset contains a null body."; return false; }
			bool bValid = true;
			for (const FSphereElem& Sphere : Body->AggGeom.SphereElems)
				bValid &= IsValidShapePose(Sphere) && std::isfinite(Sphere.Radius) && Sphere.Radius > 0.0f;
			for (const FBoxElem& Box : Body->AggGeom.BoxElems)
				bValid &= IsValidShapePose(Box) && std::isfinite(Box.X) && std::isfinite(Box.Y) && std::isfinite(Box.Z) &&
					Box.X > 0.0f && Box.Y > 0.0f && Box.Z > 0.0f;
			for (const FCapsuleElem& Capsule : Body->AggGeom.CapsuleElems)
				bValid &= IsValidShapePose(Capsule) && std::isfinite(Capsule.Radius) && std::isfinite(Capsule.Length) &&
					Capsule.Radius > 0.0f && Capsule.Length >= 0.0f;
			if (!bValid)
			{
				OutError = FString("Invalid collision shape on body: ") + Body->BoneName;
				return false;
			}
		}
		for (const FPhysicsAssetConstraint& Constraint : Asset.Constraints)
		{
			const auto ValidMotion = [](EConstraintMotion Motion)
			{
				return Motion == EConstraintMotion::Locked || Motion == EConstraintMotion::Limited || Motion == EConstraintMotion::Free;
			};
			if (!ValidMotion(Constraint.Swing1Motion) || !ValidMotion(Constraint.Swing2Motion) || !ValidMotion(Constraint.TwistMotion) ||
				!std::isfinite(Constraint.Swing1LimitDeg) || !std::isfinite(Constraint.Swing2LimitDeg) ||
				!std::isfinite(Constraint.TwistMinDeg) || !std::isfinite(Constraint.TwistMaxDeg) ||
				!std::isfinite(Constraint.FrameOffset.Pitch) || !std::isfinite(Constraint.FrameOffset.Yaw) ||
				!std::isfinite(Constraint.FrameOffset.Roll))
			{
				OutError = "Constraint motion and angle settings are invalid.";
				return false;
			}
		}
		if (!Asset.Validate(Mesh, "Physics Asset Editor Preview"))
		{
			OutError = "Invalid physics asset. Check body shapes and connect each child body to its nearest ancestor body. See Output Log.";
			return false;
		}
		return true;
	}
}

FPhysicsAssetSimulation::~FPhysicsAssetSimulation()
{
	Stop();
}

bool FPhysicsAssetSimulation::Start(USkeletalMeshComponent& Component, UStaticMeshComponent& PreviewFloor, UPhysicsAsset& Asset, FString& OutError,
	int32 RootBoneIndex)
{
	if (bActive) return true;
	OutError.clear();
	UWorld* World = Component.GetWorld();
	USkeletalMesh* Mesh = Component.GetSkeletalMesh();
	FPhysScene* Scene = World ? World->GetPhysicsScene() : nullptr;
	physx::PxPhysics* Physics = FPhysicsCore::GetPhysics();
	physx::PxMaterial* Material = FPhysicsCore::GetDefaultMaterial();
	if (!World || World->GetWorldType() != EWorldType::EditorPreview || !Mesh ||
		!Scene || !Scene->GetPxScene() || !Physics || !Material || PreviewFloor.GetWorld() != World || !PreviewFloor.GetBodySetup())
	{
		OutError = "The preview physics scene, collision floor or physics asset is unavailable.";
		return false;
	}
	if (!ValidateSimulationAsset(Asset, *Mesh, OutError)) return false;
	if (RootBoneIndex != INDEX_NONE)
	{
		bool bRootBodyFound = false;
		for (const UBodySetup* Body : Asset.BodySetups)
			bRootBodyFound |= Mesh->FindBoneIndex(Body->BoneName) == RootBoneIndex;
		if (RootBoneIndex < 0 || RootBoneIndex >= Mesh->GetNumBones() || !bRootBodyFound)
		{
			OutError = "Select a physics body before starting selected-body simulation.";
			return false;
		}
	}
	const FVector Scale = Component.GetWorldScale3D();
	if (!IsFiniteVector(Scale) || Scale.X <= 0.0f || Scale.Y <= 0.0f || Scale.Z <= 0.0f)
	{
		OutError = "Simulation requires a finite, positive mesh scale.";
		return false;
	}
	Component.RefreshBoneTransformsNow();
	if (Component.GetBoneSpaceTransforms().Num() != Mesh->GetNumBones())
	{
		OutError = "The preview bone pose is not ready.";
		return false;
	}
	MeshComponent = &Component;
	FloorComponent = &PreviewFloor;
	PhysicsScene = Scene;
	AActor* Owner = Component.GetOwner();
	TransformTarget = Owner && Owner->GetRootComponent() ? Owner->GetRootComponent() : &Component;
	SavedTargetTransform = TransformTarget->GetTransform();
	SavedMeshTransform = Component.GetTransform();
	SavedPose = Component.GetBoneSpaceTransforms();
	bool bCreated = false;
	{
		FPhysSceneWriteLock Lock(Scene);
		Component.SetSimulatePhysics(false);
		Component.TermArticulated();
		PreviewFloor.BodyInstance.TermBody();
		physx::PxScene* PxScene = Scene->GetPxScene();
		const physx::PxU32 PreviousBodies = PxScene->getNbActors(physx::PxActorTypeFlag::eRIGID_DYNAMIC);
		const physx::PxU32 PreviousConstraints = PxScene->getNbConstraints();
		Component.InitArticulated(Scene, &Asset);
		bCreated = PxScene->getNbActors(physx::PxActorTypeFlag::eRIGID_DYNAMIC) == PreviousBodies + static_cast<physx::PxU32>(Asset.BodySetups.Num()) &&
			PxScene->getNbConstraints() == PreviousConstraints + static_cast<physx::PxU32>(Asset.Constraints.Num());
		if (bCreated)
		{
			// 프리뷰 월드는 게임용 물리 상태를 자동 생성하지 않으므로 바닥 메시의 바디를 직접 초기화합니다.
			PreviewFloor.BodyInstance.bSimulatePhysics = false;
			PreviewFloor.BodyInstance.bEnableGravity = false;
			PreviewFloor.BodyInstance.CollisionEnabled = ECollisionEnabled::QueryAndPhysics;
			PreviewFloor.BodyInstance.InitBody(PreviewFloor.GetBodySetup(),
				FTransform(PreviewFloor.GetWorldRotation(), PreviewFloor.GetWorldLocation(), PreviewFloor.GetWorldScale3D()),
				&PreviewFloor, Scene);
			bCreated = PreviewFloor.BodyInstance.IsValidBodyInstance();
			if (bCreated)
			{
				if (RootBoneIndex == INDEX_NONE) Component.SetSimulatePhysics(true);
				else bCreated = Component.SetSimulatePhysicsBelow(RootBoneIndex);
			}
		}
	}
	if (!bCreated)
	{
		OutError = "Could not create all preview bodies, joints or the collision floor. See Output Log.";
		Stop();
		return false;
	}
	bActive = true;
	return true;
}

void FPhysicsAssetSimulation::Stop()
{
	if (MeshComponent)
	{
		{
			FPhysSceneWriteLock Lock(PhysicsScene);
			MeshComponent->SetSimulatePhysics(false);
			MeshComponent->TermArticulated();
			if (FloorComponent) FloorComponent->BodyInstance.TermBody();
		}
		if (TransformTarget) TransformTarget->SetTransform(SavedTargetTransform, ETeleportType::TeleportPhysics);
		if (TransformTarget != MeshComponent) MeshComponent->SetTransform(SavedMeshTransform, ETeleportType::TeleportPhysics);
		for (int32 Index = 0; Index < SavedPose.Num(); ++Index) MeshComponent->SetBoneLocalTransform(Index, SavedPose[Index]);
		MeshComponent->RefreshBoneTransformsNow();
	}
	MeshComponent = nullptr;
	FloorComponent = nullptr;
	TransformTarget = nullptr;
	PhysicsScene = nullptr;
	SavedPose.Empty();
	bActive = false;
}

void FPhysicsAssetSimulation::Tick(float DeltaTime)
{
	if (!bActive || !MeshComponent || !PhysicsScene) return;
	if (std::isfinite(DeltaTime) && DeltaTime >= 0.0f)
	{
		FPhysicsSettings Settings;
		Settings.bSubstepping = true;
		Settings.MaxSubstepDeltaTime = 1.0f / 120.0f;
		Settings.MaxSubsteps = 8;
		const float StepTime = FMath::Min(DeltaTime, Settings.MaxSubstepDeltaTime * Settings.MaxSubsteps);
		if (StepTime > 0.0f)
		{
			PhysicsScene->SetUpForFrame(StepTime, Settings);
			PhysicsScene->StartFrame();
			PhysicsScene->EndFrame();
		}
	}
	// 프리뷰 월드는 게임의 EndPhysics 틱 그룹을 실행하지 않으므로 여기서 한 번 반영합니다.
	MeshComponent->EndPhysicsTickComponent(DeltaTime);
}

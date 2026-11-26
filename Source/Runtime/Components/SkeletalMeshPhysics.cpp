#include "EnginePCH.h"
#include "SkeletalMeshComponent.h"

#include "Collision/HitResult.h"
#include "Collision/Ray.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Physics/BodyInstance.h"
#include "Physics/ConstraintInstance.h"
#include "Physics/PhysicsCore.h"
#include "Physics/PhysScene.h"
#include "Physics/PhysicsAsset.h"
#include "Physics/PhysXConversion.h"
#include "Render/SkeletalMesh.h"
#include <PxRigidDynamic.h>
#include <PxScene.h>
#include <PxShape.h>
#include <geometry/PxGeometryQuery.h>

namespace
{
	FTransform TransformFromMatrix(const FMatrix& Matrix)
	{
		FVector Scale(
			FVector(Matrix[0][0], Matrix[0][1], Matrix[0][2]).Size(),
			FVector(Matrix[1][0], Matrix[1][1], Matrix[1][2]).Size(),
			FVector(Matrix[2][0], Matrix[2][1], Matrix[2][2]).Size());
		FMatrix Rotation = Matrix;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const float InvScale = Scale[Axis] > 1.e-6f ? 1.0f / Scale[Axis] : 1.0f;
			for (int32 Column = 0; Column < 3; ++Column)
				Rotation[Axis][Column] *= InvScale;
		}
		const FVector AxisX(Rotation[0][0], Rotation[0][1], Rotation[0][2]);
		const FVector AxisY(Rotation[1][0], Rotation[1][1], Rotation[1][2]);
		const FVector AxisZ(Rotation[2][0], Rotation[2][1], Rotation[2][2]);
		if (FVector::Dot(FVector::Cross(AxisX, AxisY), AxisZ) < 0.0f)
			for (int32 Column = 0; Column < 3; ++Column)
				Rotation[2][Column] = -Rotation[2][Column];
		Rotation[3][0] = Rotation[3][1] = Rotation[3][2] = 0.0f;
		return FTransform(FQuat(MatrixToRotator(Rotation)),
			FVector(Matrix[3][0], Matrix[3][1], Matrix[3][2]), Scale);
	}
}

UPhysicsAsset* USkeletalMeshComponent::GetPhysicsAsset() const
{
	return PhysicsAssetOverride ? PhysicsAssetOverride
		: (GetSkeletalMesh() ? GetSkeletalMesh()->GetPhysicsAsset() : nullptr);
}

void USkeletalMeshComponent::SetPhysicsAsset(UPhysicsAsset* InAsset)
{
	if (PhysicsAssetOverride == InAsset)
		return;
	TermArticulated();
	PhysicsAssetOverride = InAsset;
	if (IsRegistered())
	{
		InitArticulated(GetWorld() ? GetWorld()->GetPhysicsScene() : nullptr);
	}
}

FBodyInstance* USkeletalMeshComponent::FindArticulatedBody(const FString& BoneName) const
{
	const int32 BoneIndex = FindBoneIndex(BoneName);
	if (BoneIndex < 0 || BoneIndex >= BoneToBody.Num())
		return nullptr;
	return BoneToBody[BoneIndex];
}

FBodyInstance* USkeletalMeshComponent::GetBodyInstance(const FString& BoneName) const
{
	return FindArticulatedBody(BoneName);
}

void USkeletalMeshComponent::AddImpulseToBone(const FString& BoneName, const FVector& Impulse, bool bVelocityChange)
{
	FBodyInstance* Body = FindArticulatedBody(BoneName);
	if (!Body)
	{
		return;
	}

	Body->AddImpulse(Impulse, bVelocityChange);

	// 래그돌이나 물리 바디 시뮬레이션 중 외부 충격량이 들어오면 리스너로 전파
	if ((IsRagdollActive() || IsAnyBodySimulatingPhysics()) && !PhysicsImpactListeners.IsEmpty())
	{
		const physx::PxRigidDynamic* Dyn = Body->GetPxRigidDynamic();
		const float MassScale = (bVelocityChange && Dyn) ? Dyn->getMass() : 1.0f;
		const FVector EffectiveImpulse = Impulse * MassScale;
		const float ImpulseMag = EffectiveImpulse.Size();
		if (ImpulseMag > 1e-4f)
		{
			FSkeletalPhysicsImpact Impact{};
			Impact.BoneIndex = FindBoneIndexForBody(Body);
			Impact.WorldLocation = Body->GetUnrealWorldTransform().Location;
			Impact.WorldNormal = EffectiveImpulse / ImpulseMag;
			Impact.WorldImpulse = EffectiveImpulse;
			Impact.WorldSlideVelocity = EffectiveImpulse / std::max(MassScale, 1e-4f);
			Impact.ImpulseMagnitude = ImpulseMag;
			Impact.bFromExternalImpulse = true;

			const TArray<FPhysicsImpactListenerEntry> Snapshot = PhysicsImpactListeners;
			for (int32 i = 0; i < Snapshot.Num(); ++i)
			{
				if (Snapshot[i].Callback)
				{
					Snapshot[i].Callback(Impact);
				}
			}
		}
	}
}

int32 USkeletalMeshComponent::FindBoneIndexForBody(const FBodyInstance* Body) const
{
	if (!Body)
	{
		return -1;
	}
	return Body->BoneIndex;
}

void USkeletalMeshComponent::AddPhysicsImpactListener(const void* ListenerKey, FSkeletalPhysicsImpactCallback Callback)
{
	if (!ListenerKey || !Callback)
	{
		return;
	}
	for (int32 i = 0; i < PhysicsImpactListeners.Num(); ++i)
	{
		if (PhysicsImpactListeners[i].Key == ListenerKey)
		{
			PhysicsImpactListeners[i].Callback = std::move(Callback);
			return;
		}
	}
	PhysicsImpactListeners.Add({ ListenerKey, std::move(Callback) });
}

void USkeletalMeshComponent::RemovePhysicsImpactListener(const void* ListenerKey)
{
	if (!ListenerKey)
	{
		return;
	}
	for (int32 i = PhysicsImpactListeners.Num() - 1; i >= 0; --i)
	{
		if (PhysicsImpactListeners[i].Key == ListenerKey)
		{
			PhysicsImpactListeners.RemoveAt(i, 1);
		}
	}
}

void USkeletalMeshComponent::NotifyPhysicsContactImpact(const FPhysicsContactImpact& ContactImpact)
{
	if (PhysicsImpactListeners.IsEmpty() || (!IsRagdollActive() && !IsAnyBodySimulatingPhysics()))
	{
		return;
	}

	FSkeletalPhysicsImpact Impact{};
	Impact.BoneIndex = FindBoneIndexForBody(ContactImpact.HitBody);
	Impact.WorldLocation = ContactImpact.WorldLocation;
	Impact.WorldNormal = ContactImpact.WorldNormal;
	Impact.WorldImpulse = ContactImpact.WorldImpulse;
	Impact.WorldSlideVelocity = ContactImpact.WorldSlideVelocity;
	Impact.ImpulseMagnitude = ContactImpact.ImpulseMagnitude;
	Impact.bFromExternalImpulse = false;

	const TArray<FPhysicsImpactListenerEntry> Snapshot = PhysicsImpactListeners;
	for (int32 i = 0; i < Snapshot.Num(); ++i)
	{
		if (Snapshot[i].Callback)
		{
			Snapshot[i].Callback(Impact);
		}
	}
}

void USkeletalMeshComponent::SetRagdollLinearVelocity(const FVector& Velocity)
{
	if (!IsRagdollActive())
		return;
	for (const TUniquePtr<FBodyInstance>& Body : Bodies)
	{
		if (Body)
		{
			Body->SetLinearVelocity(Velocity);
		}
	}
}

bool USkeletalMeshComponent::IsAnyBodySimulatingPhysics() const
{
	for (const TUniquePtr<FBodyInstance>& Body : Bodies)
	{
		if (Body && Body->IsDynamic())
		{
			return true;
		}
	}
	return false;
}

bool USkeletalMeshComponent::IsRagdollActive() const
{
	return IsSimulatingRagdoll();
}

bool USkeletalMeshComponent::StartRagdoll()
{
	if (IsRagdollActive())
		return true;
	if (Bodies.IsEmpty())
	{
		InitArticulated(GetWorld() ? GetWorld()->GetPhysicsScene() : nullptr);
	}
	if (Bodies.IsEmpty())
		return false;

	RefreshBoneTransformsNow();
	UpdateKinematicBonesToAnim(ETeleportType::TeleportPhysics);
	for (const TUniquePtr<FBodyInstance>& Body : Bodies)
	{
		if (!Body) continue;
		Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	}
	SetSimulatePhysics(true);
	for (const TUniquePtr<FBodyInstance>& Body : Bodies)
	{
		if (!Body) continue;
		if (physx::PxRigidDynamic* Actor = Body->GetPxRigidDynamic())
		{
			Actor->setLinearVelocity(physx::PxVec3(0.0f));
			Actor->setAngularVelocity(physx::PxVec3(0.0f));
		}
	}
	return IsRagdollActive();
}

void USkeletalMeshComponent::StopRagdoll()
{
	if (!IsRagdollActive() && !IsAnyBodySimulatingPhysics())
		return;

	SetSimulatePhysics(false);
	for (const TUniquePtr<FBodyInstance>& Body : Bodies)
	{
		if (Body)
		{
			Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		}
	}
	if (AnimInstance && BoneSpaceTransforms.Num() == GetBones().Num())
		RefreshAnimationPose();
	else if (BoneSpaceTransforms.Num() == GetBones().Num())
	{
		for (int32 Index = 0; Index < GetBones().Num(); ++Index)
			BoneSpaceTransforms[Index] = GetBones()[Index].LocalBindPose;
		MarkBoneTransformsDirty();
		RefreshBoneTransformsNow();
	}
	UpdateKinematicBonesToAnim(ETeleportType::TeleportPhysics);
}

bool USkeletalMeshComponent::SetRagdollSimulationEnabled(bool bEnable)
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	const bool bIsOwnerCharacterMesh = OwnerCharacter && OwnerCharacter->GetSkeletalMeshComponent() == this;
	if (bEnable)
	{
		return bIsOwnerCharacterMesh ? OwnerCharacter->StartRagdoll() : StartRagdoll();
	}
	if (bIsOwnerCharacterMesh)
	{
		OwnerCharacter->StopRagdoll();
	}
	else
	{
		StopRagdoll();
	}
	return false;
}

bool USkeletalMeshComponent::HasArticulatedBodies() const
{
	return !Bodies.IsEmpty();
}

bool USkeletalMeshComponent::RaycastPhysicsBodies(const FRay& WorldRay, FHitResult& OutHit) const
{
	if (Bodies.IsEmpty())
		return false;
	const FVector Direction = WorldRay.Direction.GetSafeNormal();
	if (Direction.SizeSquared() <= SMALL_NUMBER)
		return false;
	float Nearest = FLT_MAX;
	bool bHit = false;
	for (const TUniquePtr<FBodyInstance>& Body : Bodies)
	{
		if (!Body || Body->BoneIndex < 0 || Body->BoneIndex >= GetBones().Num()) continue;
		if (Body->CollisionEnabled == ECollisionEnabled::NoCollision ||
			Body->CollisionEnabled == ECollisionEnabled::PhysicsOnly) continue;
		physx::PxRigidDynamic* Actor = Body->GetPxRigidDynamic();
		if (!Actor) continue;
		const physx::PxTransform BodyPose = Body->IsDynamic() ? Actor->getGlobalPose()
			: K2PTransform(TransformFromMatrix(GetBoneWorldMatrix(Body->BoneIndex)));
		const physx::PxU32 ShapeCount = Actor->getNbShapes();
		TArray<physx::PxShape*> Shapes;
		Shapes.SetNum(static_cast<int32>(ShapeCount));
		Actor->getShapes(Shapes.GetData(), ShapeCount);
		for (physx::PxShape* Shape : Shapes)
		{
			physx::PxGeomRaycastHit Hit;
			if (!physx::PxGeometryQuery::raycast(K2PVector(WorldRay.Origin), K2PVector(Direction),
				Shape->getGeometry(), BodyPose * Shape->getLocalPose(), Nearest,
				physx::PxHitFlag::eDEFAULT, 1, &Hit))
				continue;
			Nearest = Hit.distance;
			OutHit.HitComponent = const_cast<USkeletalMeshComponent*>(this);
			OutHit.BoneName = GetBones()[Body->BoneIndex].Name;
			OutHit.Distance = Hit.distance;
			OutHit.ImpactPoint = P2KVector(Hit.position);
			OutHit.Location = OutHit.ImpactPoint;
			OutHit.Normal = OutHit.ImpactNormal = P2KVector(Hit.normal);
			OutHit.bBlockingHit = true;
			bHit = true;
		}
	}
	return bHit;
}

bool USkeletalMeshComponent::SweepPhysicsBodies(const FRay& WorldRay, float MaxDistance, float Radius, FHitResult& OutHit) const
{
	if (Bodies.IsEmpty() || Radius < 0.0f || MaxDistance <= 0.0f)
		return false;
	if (Radius == 0.0f)
	{
		if (!RaycastPhysicsBodies(WorldRay, OutHit) || OutHit.Distance > MaxDistance)
			return false;
		OutHit.Time = OutHit.Distance / MaxDistance;
		return true;
	}
	const FVector Direction = WorldRay.Direction.GetSafeNormal();
	if (Direction.SizeSquared() <= SMALL_NUMBER)
		return false;
	const physx::PxSphereGeometry Sphere(Radius);
	const physx::PxTransform Start(K2PVector(WorldRay.Origin));
	float Nearest = MaxDistance;
	bool bHit = false;
	for (const TUniquePtr<FBodyInstance>& Body : Bodies)
	{
		if (!Body || Body->BoneIndex < 0 || Body->BoneIndex >= GetBones().Num()) continue;
		if (Body->CollisionEnabled == ECollisionEnabled::NoCollision ||
			Body->CollisionEnabled == ECollisionEnabled::PhysicsOnly) continue;
		physx::PxRigidDynamic* Actor = Body->GetPxRigidDynamic();
		if (!Actor) continue;
		const physx::PxTransform BodyPose = Body->IsDynamic() ? Actor->getGlobalPose()
			: K2PTransform(TransformFromMatrix(GetBoneWorldMatrix(Body->BoneIndex)));
		const physx::PxU32 ShapeCount = Actor->getNbShapes();
		TArray<physx::PxShape*> Shapes;
		Shapes.SetNum(static_cast<int32>(ShapeCount));
		Actor->getShapes(Shapes.GetData(), ShapeCount);
		for (physx::PxShape* Shape : Shapes)
		{
			const physx::PxTransform ShapePose = BodyPose * Shape->getLocalPose();
			const bool bOverlapping = physx::PxGeometryQuery::overlap(Sphere, Start, Shape->getGeometry(), ShapePose);
			physx::PxGeomSweepHit Hit;
			if (!bOverlapping && (Nearest <= 0.0f || !physx::PxGeometryQuery::sweep(K2PVector(Direction), Nearest,
				Sphere, Start, Shape->getGeometry(), ShapePose, Hit)))
				continue;
			const float Distance = bOverlapping ? 0.0f : Hit.distance;
			if (bHit && Distance >= Nearest) continue;
			Nearest = Distance;
			OutHit.HitComponent = const_cast<USkeletalMeshComponent*>(this);
			OutHit.BoneName = GetBones()[Body->BoneIndex].Name;
			OutHit.Distance = Distance;
			OutHit.Time = Distance / MaxDistance;
			OutHit.bStartPenetrating = bOverlapping;
			OutHit.Location = WorldRay.PointAt(Distance);
			OutHit.ImpactPoint = bOverlapping ? WorldRay.Origin : P2KVector(Hit.position);
			OutHit.Normal = OutHit.ImpactNormal = bOverlapping ? -Direction : P2KVector(Hit.normal);
			OutHit.bBlockingHit = true;
			bHit = true;
		}
	}
	return bHit;
}

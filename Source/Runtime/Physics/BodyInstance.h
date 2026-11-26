#pragma once

#include "Engine/EngineBaseTypes.h"
#include "BodyInstance.generated.h"

class FPhysScene;
class UPrimitiveComponent;
struct FTransform;
class UBodySetup;
struct FStaticMeshData;

namespace physx
{
	class PxRigidActor;
	class PxRigidDynamic;
	class PxAggregate;
}

UENUM()
enum class ECollisionEnabled : uint8
{
	NoCollision,
	QueryOnly,
	PhysicsOnly,
	QueryAndPhysics
};

USTRUCT()
struct FBodyInstance
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	bool bSimulatePhysics = false;

	UPROPERTY(EditAnywhere)
	bool bEnableGravity = true;

	UPROPERTY(EditAnywhere)
	float MassInKgOverride = 0.0f;

	UPROPERTY(EditAnywhere)
	float LinearDamping = 0.01f;

	UPROPERTY(EditAnywhere)
	float AngularDamping = 0.05f;

	UPROPERTY(EditAnywhere)
	ECollisionEnabled CollisionEnabled = ECollisionEnabled::QueryAndPhysics;

	UPROPERTY(EditAnywhere)
	bool bUseTriangleMesh = false;

	void InitBody(UBodySetup* InBodySetup, const FTransform& WorldTransform, UPrimitiveComponent* Owner, FPhysScene* InScene, physx::PxAggregate* Aggregate = nullptr);
	void InitStaticTriangleMeshBody(const FStaticMeshData& Mesh, const FTransform& WorldTransform, UPrimitiveComponent* Owner, FPhysScene* InScene);
	void TermBody();
	bool IsValidBodyInstance() const { return RigidActor != nullptr; }

	physx::PxRigidDynamic* GetPxRigidDynamic() const;
	bool IsDynamic() const;

	void SetBodyTransform(const FTransform& WorldTransform, ETeleportType TeleportType);
	FTransform GetUnrealWorldTransform() const;
	void SetInstanceSimulatePhysics(bool bSimulate);
	void SetCollisionEnabled(ECollisionEnabled InCollisionEnabled);
	void AddImpulse(const FVector& Impulse, bool bVelocityChange);
	void AddForce(const FVector& Force);
	void SetLinearVelocity(const FVector& Velocity);
	FVector GetLinearVelocity() const;

	physx::PxRigidActor* RigidActor = nullptr;
	UPrimitiveComponent* OwnerComponent = nullptr;
	UBodySetup* BodySetup = nullptr;
	FPhysScene* PhysScene = nullptr;
	FVector Scale3D = FVector(1.0f, 1.0f, 1.0f);
	int32 BoneIndex = INDEX_NONE;
};

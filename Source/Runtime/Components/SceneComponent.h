#pragma once

#include "../Math/Transform.h"
#include "ActorComponent.h"
#include "Math/Box.h"
#include "Collision/HitResult.h"
#include "SceneComponent.generated.h"

enum class EAttachmentRule
{
	KeepRelative,
	KeepWorld,
	SnapToTarget
};

UCLASS()
class USceneComponent : public UActorComponent
{
	GENERATED_BODY()


public:
	enum class EVisibilityPropagation : uint8 { NoPropagation, DirtyOnly, Propagate };

	USceneComponent() = default;
	virtual ~USceneComponent() override;

	virtual void PostEditChangeProperty(const FProperty& Property) override;

	// Get & Set
	const FVector& GetRelativeLocation() const { return Transform.Location; }
	void SetRelativeLocation(const FVector& InLocation, ETeleportType TeleportType = ETeleportType::None)
	{
		Transform.Location = InLocation;
		MarkTransformDirty(TeleportType);
	}


	const FRotator& GetRelativeRotation() const { return RelativeRotationCache.QuatToRotator(Transform.Rotation); }
	void SetRelativeRotation(const FRotator& InRotation, ETeleportType TeleportType = ETeleportType::None) 
	{
		Transform.Rotation = RelativeRotationCache.RotatorToQuat(InRotation);
		MarkTransformDirty(TeleportType);
	}
	void SetRelativeLocationAndRotation(const FVector& NewLocation, const FRotator& NewRotation, ETeleportType TeleportType = ETeleportType::None);
	const FVector& GetRelativeScale3D() const { return Transform.Scale; }
	void SetRelativeScale3D(const FVector& InScale, ETeleportType TeleportType = ETeleportType::None)
	{
		Transform.Scale = InScale;
		MarkTransformDirty(TeleportType);
	}

	// 쿼터니언 적용된 회전행렬
	FQuat GetRelativeRotationQuat() const { return Transform.GetOrientation(); }

	const FTransform& GetTransform() const { return Transform; }
	void SetTransform(const FTransform& InTransform, ETeleportType TeleportType = ETeleportType::None)
	{
		Transform = InTransform;
		MarkTransformDirty(TeleportType);
	}

	// Attatch-To
	USceneComponent* GetAttachParent() const { return AttachParent; }
	const TArray<USceneComponent*>& GetAttachChildren() const { return AttachChildren; }
	const FString& GetAttachSocketName() const { return AttachSocketName; }
	void SetupAttachment(USceneComponent* InParent, EAttachmentRule Rule = EAttachmentRule::KeepRelative);
	void SetupAttachmentToSocket(USceneComponent* InParent, const FString& InSocketName, EAttachmentRule Rule = EAttachmentRule::KeepRelative);
	void DetachFromParent(EAttachmentRule Rule = EAttachmentRule::KeepRelative);

	virtual FBox CalcLocalBounds() const { return FBox{ FVector(), FVector() }; }
	// 월드 공간에서 바로 계산하는 컴포넌트(파티클)는 오버라이드한다
	virtual FBox CalcBounds() const { return CalcLocalBounds().GetWorldAABB(GetWorldMatrix()); }

	FVector GetWorldLocation() const;
	FRotator GetWorldRotation() const;
	FVector GetWorldScale3D() const;
	FMatrix GetWorldMatrix() const;
	
	void SetWorldLocation(FVector NewLocation, bool bSweep, ETeleportType TeleportType = ETeleportType::None);
	void SetWorldRotation(FRotator NewRotation, bool bSweep, ETeleportType TeleportType = ETeleportType::None);
	void SetWorldLocationAndRotation(FVector NewLocation, FRotator NewRotation, bool bSweep, ETeleportType TeleportType = ETeleportType::None);
	void SetWorldScale3D(FVector NewScale, ETeleportType TeleportType = ETeleportType::None);	
	void SetWorldTransform(const FMatrix& InWorldMatrix, bool bSweep, ETeleportType TeleportType = ETeleportType::None);

	virtual bool MoveComponent(const FVector& Delta, const FRotator& NewRotation, bool bSweep, FHitResult* Hit = nullptr, ETeleportType TeleportType = ETeleportType::None);
	virtual float GetCollisionRadius() const;
	void SetCollisionRadiusOverride(float Radius) { CollisionRadiusOverride = Radius; }

	void MarkTransformDirty(ETeleportType TeleportType = ETeleportType::None);
	virtual void OnTransformDirty(ETeleportType TeleportType) {};

	using Super::Serialize;
	virtual void Serialize(FStructuredArchive::FRecord Record) override;
	virtual bool IsShown(bool bGameView) const override;
	void SetVisibility(bool bNewVisibility) override
	{ SetVisibility(bNewVisibility, EVisibilityPropagation::NoPropagation); }
	void SetVisibility(bool bNewVisibility, EVisibilityPropagation Propagation);
	void SetVisibility(bool bNewVisibility, bool bPropagateToChildren)
	{ SetVisibility(bNewVisibility, bPropagateToChildren ? EVisibilityPropagation::Propagate : EVisibilityPropagation::NoPropagation); }
	void SetHiddenInGame(bool bNewHidden) override
	{ SetHiddenInGame(bNewHidden, EVisibilityPropagation::NoPropagation); }
	void SetHiddenInGame(bool bNewHidden, EVisibilityPropagation Propagation);
	void SetHiddenInGame(bool bNewHidden, bool bPropagateToChildren)
	{ SetHiddenInGame(bNewHidden, bPropagateToChildren ? EVisibilityPropagation::Propagate : EVisibilityPropagation::NoPropagation); }
	virtual void MarkRenderStateDirty() override {}

public:
	FVector ComponentVelocity;

protected:

	bool bTransformDirty;
	UPROPERTY(EditAnywhere)
	FTransform Transform;
	mutable FRotationConversionCache RelativeRotationCache;

	USceneComponent* AttachParent = nullptr; // Attach 부모 정보
	UPROPERTY()
	FString AttachSocketName;
	TArray<USceneComponent*> AttachChildren;

	bool bAbsoluteLocation = false;
	bool bAbsoluteRotation = false;
	bool bAbsoluteScale = false;
	float CollisionRadiusOverride = -1.0f;
};

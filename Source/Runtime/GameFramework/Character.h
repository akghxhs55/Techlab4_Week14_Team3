#pragma once

#include "Pawn.h"
#include <Math/Vector.h>
#include "Character.generated.h"

class USkeletalMeshComponent;
class UCapsuleComponent;
class UCharacterMovementComponent;
class UMaterialInstanceDynamic;
class UParticleSystemComponent;
struct FHitResult;
struct FParticleEventCollisionData;

UCLASS()
class ACharacter : public APawn
{
	GENERATED_BODY()

public:
	ACharacter();
	virtual void AddControllerYawInput(float Value) override;
	virtual void AddControllerPitchInput(float Value) override;
	virtual FRotator GetMovementInputRotation() const override;

	UPROPERTY(EditAnywhere)
	bool bUseControllerRotationPitch = false;
	UPROPERTY(EditAnywhere)
	bool bUseControllerRotationYaw = true;
	UPROPERTY(EditAnywhere)
	bool bUseControllerRotationRoll = false;

	USkeletalMeshComponent* SkeletalMeshComponent = nullptr;
	UCapsuleComponent* CapsuleComponent = nullptr;
	UCharacterMovementComponent* CharacterMovementComponent = nullptr;

	UCapsuleComponent* GetCapsuleComponent() const { return CapsuleComponent; }
	UCharacterMovementComponent* GetCharacterMovement() const { return CharacterMovementComponent; }
	USkeletalMeshComponent* GetSkeletalMeshComponent() const { return SkeletalMeshComponent; }

	virtual FVector GetVelocity() const override;

	// UE ACharacter::Jump: 바닥에 있으면 CharacterMovementComponent::JumpZVelocity로 뛴다
	virtual void Jump();
	virtual void StopJumping();
	virtual bool CanJump() const;
	bool IsFalling() const;
	void LaunchCharacter(const FVector& Direction, float Strength);
	bool StartRagdoll();
	void StopRagdoll();
	virtual void Attack() {}
	float PlayAnimMontage(const FString& MontagePath, float InPlayRate = 1.0f, const FString& StartSection = "");
	void SetDeathDissolveParams(float InDuration, const FVector4& InEdgeColor);
	void StartDeathDissolve();
	void UpdateDeathDissolve(float DeltaTime);
	bool HasStartedDeathDissolve() const { return bDissolveStarted; }
	bool IsDeathDissolveFinished() const;
	virtual void OnParticleCollide(UParticleSystemComponent* Comp, const FParticleEventCollisionData& Event) {}
	// 공중에서 바닥에 닿은 순간 (점프 착지, 떨어진 뒤 착지 모두)
	virtual void Landed(const FHitResult& Hit) {}

protected:
	void SetDeathParticlePath(const FString& InPath);
	FString DeathParticlePath = "Particles/ZombieDeathParticles.particle";
	FVector4 DissolveEdgeColor = FVector4(0.75f, 0.78f, 0.85f, 1.0f);
	float DissolveDuration = 1.45f;

private:
	friend class USkeletalMeshComponent;
	void RestoreRagdollMovementAndCollision();
	bool bMovementTickBeforeRagdoll = false;
	bool bCapsuleOverlapBeforeRagdoll = false;
	bool bCapsuleBlockBeforeRagdoll = false;
	uint8 CapsuleCollisionBeforeRagdoll = 0;
	void EnsureDeathDissolveSetup();
	UParticleSystemComponent* DeathParticleComponent = nullptr;
	TArray<UMaterialInstanceDynamic*> DissolveMaterials;
	bool bDissolveStarted = false;
	bool bDissolveCompleted = false;
	float DissolveElapsed = 0.0f;
};

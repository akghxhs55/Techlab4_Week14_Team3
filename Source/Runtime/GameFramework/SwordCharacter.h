#pragma once

#include "GameFramework/Character.h"
#include "SwordCharacter.generated.h"

class AZombieActor;
class UCameraComponent;
class UClothComponent;
class UMaterialInstanceDynamic;
class UParticleSystemComponent;
class USpringArmComponent;
class UStaticMeshComponent;
struct FParticleEventCollisionData;

UCLASS()
class ASwordCharacter : public ACharacter
{
	GENERATED_BODY()
public:
	ASwordCharacter();
	void BeginPlay() override;
	void Tick(float DeltaTime) override;
	void AddMovementInput(const FVector& WorldDirection, float ScaleValue = 1.0f) override;
	void Jump() override;
	void Attack() override;
	void CastBeam();
	void ReactToHit();
	void OnParticleCollide(UParticleSystemComponent* Comp, const FParticleEventCollisionData& Event) override;
	bool IsDead() const { return Health <= 0; }
	int32 GetHealth() const { return Health; }
	USpringArmComponent* GetSpringArmComponent() const { return SpringArmComponent; }
	UCameraComponent* GetCameraComponent() const { return CameraComponent; }
	UClothComponent* GetCapeClothComponent() const { return CapeClothComponent; }

private:
	bool IsPlayingFullBodyMontage() const;
	void EnsureCapeSetup();
	USpringArmComponent* SpringArmComponent = nullptr;
	UCameraComponent* CameraComponent = nullptr;
	UStaticMeshComponent* SwordMeshComponent = nullptr;
	UParticleSystemComponent* SwordRibbonComponent = nullptr;
	UClothComponent* CapeClothComponent = nullptr;
	UMaterialInstanceDynamic* CapeMaterialInstance = nullptr;
	TMap<AZombieActor*, float> ZombieHitCooldowns;
	float ElapsedTime = 0.0f;
	UPROPERTY(EditAnywhere)
	int32 Health = 5;
	bool bBeamMontageActive = false;
};

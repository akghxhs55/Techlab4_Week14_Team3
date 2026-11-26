#include "EnginePCH.h"
#include "GameFramework/Actor/ZombieActor.h"
#include "Animation/AnimGraphInstance.h"
#include "Asset/AssetManager.h"
#include "Components/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/ScriptComponent.h"
#include "GameFramework/SwordCharacter.h"
#include "GameFramework/TestCharacter.h"
#include "Render/SkeletalMesh.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include <cmath>

AZombieActor::AZombieActor()
{
	PrimaryActorTick.bCanEverTick = true;
	// CharacterMovement 속도는 cm 단위를 사용한다
	GetCharacterMovement()->MaxWalkSpeed = 400.0f;
	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	MeshComponent->SetRelativeLocation(FVector(0.0f, 0.0f, -1.0f));
	RightHandHitCapsule = CreateDefaultSubobject<UCapsuleComponent>("RightHandHitCapsule");
	RightHandHitCapsule->SetupAttachment(MeshComponent);
	RightHandHitCapsule->CapsuleRadius = 0.12f;
	RightHandHitCapsule->CapsuleHalfHeight = 0.20f;
	RightHandHitCapsule->bBlockComponent = false;
	RightHandHitCapsule->bGenerateOverlapEvents = false;
	USkeletalMesh* Mesh = UAssetManager::GetAssetByPath<USkeletalMesh>("Models/Zombie/SK_Zombie Idle_mixamorig_Hips.fbx.bin");
	if (Mesh)
	{
		MeshComponent->SetSkeletalMesh(Mesh);
		if (!MeshComponent->SetAnimBlueprint("Animations/zombieanimgraph.animblueprint"))
			KRS_LOG(Error, "Failed to assign zombieanimgraph.animblueprint to AZombieActor");
	}
	else KRS_LOG(Error, "Failed to load zombie idle mesh for AZombieActor");
	UScriptComponent* TuningScript = CreateDefaultSubobject<UScriptComponent>("CombatTuningScript");
	TuningScript->SetScriptPath("Scripts/ZombieTuning.lua");
}

void AZombieActor::BeginPlay()
{
	Super::BeginPlay();
	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	if (MeshComponent && MeshComponent->GetSkeletalMesh())
		RightHandBoneIndex = MeshComponent->FindBoneIndex("mixamorig:RightHand");
	if (RightHandHitCapsule)
		RightHandHitCapsule->OnComponentBeginOverlap.AddDynamic(this, &AZombieActor::OnRightHandOverlap);
}

void AZombieActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld()) return;
	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	UAnimGraphInstance* Anim = Cast<UAnimGraphInstance>(MeshComponent->GetAnimInstance());
	if (IsDead())
	{
		if (!MeshComponent->IsRagdollActive())
			StartRagdoll();
		GetCharacterMovement()->StopMovementImmediately();
		RightHandHitCapsule->bGenerateOverlapEvents = false;
		return;
	}
	RightHandHitCapsule->bGenerateOverlapEvents = Anim && Anim->IsStateActiveOrPending("Attack") &&
		RightHandBoneIndex >= 0;
	if (RightHandBoneIndex >= 0)
	{
		const FMatrix HandOffset = FTransform(FRotator(0.0f, 0.0f, 0.0f), FVector(0.0f, 0.0f, 0.10f)).GetLocalMatrix();
		RightHandHitCapsule->SetWorldTransform(HandOffset * MeshComponent->GetBoneWorldMatrix(RightHandBoneIndex), false);
	}
	AttackTimer = FMath::Max(0.0f, AttackTimer - DeltaTime);
	Target = nullptr;
	float ClosestSquared = DetectionRadius * DetectionRadius;
	for (AActor* Actor : World->GetPersistentLevel()->GetActors())
	{
		ACharacter* Candidate = Cast<ACharacter>(Actor);
		if (!IsValid(Candidate) || Candidate == this || Cast<AZombieActor>(Candidate)) continue;
		if (ATestCharacter* PlayerCandidate = Cast<ATestCharacter>(Candidate))
		{
			if (PlayerCandidate->IsDead()) continue;
		}
		if (ASwordCharacter* SwordCandidate = Cast<ASwordCharacter>(Candidate))
		{
			if (SwordCandidate->IsDead()) continue;
		}
		const FVector Difference = Candidate->GetActorLocation() - GetActorLocation();
		const float DistanceSquared = Difference.SizeSquared();
		if (DistanceSquared < ClosestSquared)
		{
			ClosestSquared = DistanceSquared;
			Target = Candidate;
		}
	}

	UCharacterMovementComponent* Movement = GetCharacterMovement();
	if (!Target || (Anim && (Anim->IsStateActiveOrPending("Attack") || Anim->IsStateActiveOrPending("Hit"))))
	{
		if (Movement->IsMovingOnGround() && !Movement->IsLaunchActive()) Movement->StopMovementImmediately();
		return;
	}
	FVector Direction = Target->GetActorLocation() - GetActorLocation();
	Direction.Z = 0.0f;
	if (Direction.SizeSquared() <= 0.0001f) return;
	const float Yaw = FMath::RadiansToDegrees(std::atan2(Direction.Y, Direction.X));
	GetRootComponent()->SetRelativeRotation(FRotator(0.0f, Yaw, 0.0f));
	if (Direction.SizeSquared() <= AttackRadius * AttackRadius)
	{
		if (Movement->IsMovingOnGround() && !Movement->IsLaunchActive()) Movement->StopMovementImmediately();
		if (Anim && AttackTimer <= 0.0f)
		{
			Anim->RequestAttack();
			AttackTimer = AttackCooldown;
		}
	}
	else AddMovementInput(Direction, 1.0f);
}

void AZombieActor::OnRightHandOverlap(UPrimitiveComponent* Self, AActor* OtherActor, UPrimitiveComponent* OtherComponent)
{
	if (Self != RightHandHitCapsule || !RightHandHitCapsule->bGenerateOverlapEvents) return;
	if (ATestCharacter* Character = Cast<ATestCharacter>(OtherActor))
	{
		Character->ReactToHit();
	}
	else if (ASwordCharacter* SwordCharacter = Cast<ASwordCharacter>(OtherActor))
	{
		SwordCharacter->ReactToHit();
	}
}

void AZombieActor::ReactToHit()
{
	if (IsDead()) return;
	--Health;
	if (IsDead())
	{
		RightHandHitCapsule->bGenerateOverlapEvents = false;
		if (!StartRagdoll())
			KRS_LOG(Warning, "Failed to start zombie ragdoll");
	}
	else if (UAnimGraphInstance* Anim = Cast<UAnimGraphInstance>(GetSkeletalMeshComponent()->GetAnimInstance()))
		Anim->RequestHit();
}

bool AZombieActor::SetHealth(int32 InHealth)
{
	if (InHealth <= 0 || IsDead()) return false;
	Health = InHealth;
	return true;
}

bool AZombieActor::SetCombatTuning(float InDetectionRadius, float InAttackRadius,
	float InAttackCooldown, float InWalkSpeed)
{
	if (!std::isfinite(InDetectionRadius) || InDetectionRadius <= 0.0f ||
		!std::isfinite(InAttackRadius) || InAttackRadius <= 0.0f ||
		!std::isfinite(InAttackCooldown) || InAttackCooldown < 0.0f ||
		!std::isfinite(InWalkSpeed) || InWalkSpeed < 0.0f) return false;
	DetectionRadius = InDetectionRadius;
	AttackRadius = InAttackRadius;
	AttackCooldown = InAttackCooldown;
	GetCharacterMovement()->MaxWalkSpeed = InWalkSpeed;
	return true;
}

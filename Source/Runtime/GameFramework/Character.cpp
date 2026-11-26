#include "EnginePCH.h"
#include "Components/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/ParticleSystemComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimGraphInstance.h"
#include "Asset/AssetManager.h"
#include "Particles/ParticleSystem.h"
#include "Render/Material.h"
#include "Render/MaterialInstance.h"
#include "Render/SkeletalMesh.h"
#include "GameFramework/PlayerController.h"
#include "Character.h"
#include <algorithm>
#include <cmath>

ACharacter::ACharacter()
{
	CapsuleComponent = CreateDefaultSubobject<UCapsuleComponent>("CapsuleComponent");
	SetRootComponent(CapsuleComponent);

	SkeletalMeshComponent = CreateDefaultSubobject<USkeletalMeshComponent>("SkeletalMeshComponent");
	SkeletalMeshComponent->SetupAttachment(CapsuleComponent);

	CharacterMovementComponent = CreateDefaultSubobject<UCharacterMovementComponent>("CharacterMovementComponent");
}

bool ACharacter::StartRagdoll()
{
	if (SkeletalMeshComponent && SkeletalMeshComponent->IsRagdollActive())
		return true;
	const FVector InheritedVelocity = GetVelocity();
	KRS_LOG(Info, "[Ragdoll] inherited velocity=({},{},{})",
		InheritedVelocity.X, InheritedVelocity.Y, InheritedVelocity.Z);
	if (!SkeletalMeshComponent || !CapsuleComponent)
		return false;
	if (CharacterMovementComponent)
	{
		bMovementTickBeforeRagdoll = CharacterMovementComponent->IsComponentTickEnabled();
		CharacterMovementComponent->StopMovementImmediately();
		CharacterMovementComponent->SetComponentTickEnabled(false);
	}
	bCapsuleOverlapBeforeRagdoll = CapsuleComponent->bGenerateOverlapEvents;
	bCapsuleBlockBeforeRagdoll = CapsuleComponent->bBlockComponent;
	CapsuleCollisionBeforeRagdoll = static_cast<uint8>(CapsuleComponent->BodyInstance.CollisionEnabled);
	CapsuleComponent->bGenerateOverlapEvents = false;
	CapsuleComponent->bBlockComponent = false;
	CapsuleComponent->BodyInstance.SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (!SkeletalMeshComponent->StartRagdoll())
	{
		RestoreRagdollMovementAndCollision();
		return false;
	}
	SkeletalMeshComponent->SetRagdollLinearVelocity(InheritedVelocity);
	return true;
}

void ACharacter::StopRagdoll()
{
	if (!SkeletalMeshComponent || !SkeletalMeshComponent->IsRagdollActive())
		return;
	SkeletalMeshComponent->StopRagdoll();
	RestoreRagdollMovementAndCollision();
}

void ACharacter::RestoreRagdollMovementAndCollision()
{
	if (CapsuleComponent)
	{
		CapsuleComponent->BodyInstance.SetCollisionEnabled(static_cast<ECollisionEnabled>(CapsuleCollisionBeforeRagdoll));
		CapsuleComponent->bGenerateOverlapEvents = bCapsuleOverlapBeforeRagdoll;
		CapsuleComponent->bBlockComponent = bCapsuleBlockBeforeRagdoll;
	}
	if (CharacterMovementComponent)
		CharacterMovementComponent->SetComponentTickEnabled(bMovementTickBeforeRagdoll);
}

void ACharacter::AddControllerYawInput(float Value)
{
	if (!bUseControllerRotationYaw || !GetRootComponent()) return;
	FRotator Rotation = GetActorRotation();
	Rotation.Yaw += Value;
	GetRootComponent()->SetRelativeRotation(Rotation);
}

void ACharacter::AddControllerPitchInput(float Value)
{
	if (!bUseControllerRotationPitch || !GetRootComponent()) return;
	FRotator Rotation = GetActorRotation();
	Rotation.Pitch = FMath::Clamp(Rotation.Pitch + Value, -89.0f, 89.0f);
	GetRootComponent()->SetRelativeRotation(Rotation);
}

FRotator ACharacter::GetMovementInputRotation() const
{
	// 수평 이동 기준은 컨트롤러의 Yaw만 사용한다
	if (const APlayerController* PC = GetController())
		return FRotator(0.0f, PC->ControlRotation.Yaw, 0.0f);
	return Super::GetMovementInputRotation();
}

FVector ACharacter::GetVelocity() const
{
	return CharacterMovementComponent->Velocity;
}

void ACharacter::Jump()
{
	if (CanJump())
		CharacterMovementComponent->DoJump();
}

void ACharacter::StopJumping()
{
	if (CharacterMovementComponent)
		CharacterMovementComponent->StopJumping();
}

bool ACharacter::CanJump() const
{
	return CharacterMovementComponent && CharacterMovementComponent->IsMovingOnGround();
}

bool ACharacter::IsFalling() const
{
	return CharacterMovementComponent && CharacterMovementComponent->IsFalling();
}

void ACharacter::LaunchCharacter(const FVector& Direction, float Strength)
{
	if (!CharacterMovementComponent || !std::isfinite(Strength) || Strength <= 0.0f ||
		!std::isfinite(Direction.X) || !std::isfinite(Direction.Y) || !std::isfinite(Direction.Z))
		return;
	const FVector Normal = Direction.GetSafeNormal();
	if (Normal.SizeSquared() <= SMALL_NUMBER)
		return;
	CharacterMovementComponent->Launch(Normal * Strength);
}

float ACharacter::PlayAnimMontage(const FString& MontagePath, float InPlayRate, const FString& StartSection)
{
	UAnimGraphInstance* AnimGraph = Cast<UAnimGraphInstance>(SkeletalMeshComponent ? SkeletalMeshComponent->GetAnimInstance() : nullptr);
	return AnimGraph ? AnimGraph->PlayMontage(MontagePath, InPlayRate, StartSection) : 0.0f;
}

void ACharacter::SetDeathParticlePath(const FString& InPath)
{
	DeathParticlePath = InPath;
	if (DeathParticleComponent)
	{
		DeathParticleComponent->InitializeSystem(nullptr);
	}
}

void ACharacter::EnsureDeathDissolveSetup()
{
	if (!DeathParticleComponent && !DeathParticlePath.empty())
	{
		DeathParticleComponent = Cast<UParticleSystemComponent>(AddComponent(UParticleSystemComponent::StaticClass(), "DeathParticleComponent"));
		if (DeathParticleComponent && CapsuleComponent)
		{
			DeathParticleComponent->SetupAttachment(CapsuleComponent);
			DeathParticleComponent->SetAutoActivate(false);
		}
	}
	if (DeathParticleComponent && !DeathParticleComponent->GetTemplate() && !DeathParticlePath.empty())
	{
		if (UParticleSystem* DeathTemplate = UAssetManager::LoadParticleSystem(DeathParticlePath))
			DeathParticleComponent->InitializeSystem(DeathTemplate);
		DeathParticleComponent->SetVisibility(false);
		DeathParticleComponent->Deactivate();
	}

	USkeletalMesh* Mesh = SkeletalMeshComponent ? SkeletalMeshComponent->GetSkeletalMesh() : nullptr;
	if (!Mesh || !DissolveMaterials.IsEmpty())
		return;

	const int32 SlotCount = Mesh->GetMaterialSlots().Num();
	for (int32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
	{
		UMaterialInterface* SlotMat = SkeletalMeshComponent->GetMaterial(SlotIndex);
		UMaterial* BaseMat = SlotMat ? SlotMat->GetBaseMaterial() : nullptr;
		UMaterial* DissolveParent = UMaterial::GetOrCreateDissolveParent(BaseMat);
		UMaterialInstanceDynamic* DynMat = DissolveParent ? UMaterial::CreateDynamicInstance(DissolveParent) : nullptr;
		if (!DynMat)
			continue;

		DynMat->SetScalarParameterValue(FName("DissolveAmount"), -0.10f);
		DynMat->SetVectorParameterValue(FName("DissolveEdgeColor"), FVector4(0.0f, 0.0f, 0.0f, 1.0f));
		SkeletalMeshComponent->SetMaterial(SlotIndex, DynMat);
		DissolveMaterials.Add(DynMat);
	}
}

void ACharacter::SetDeathDissolveParams(float InDuration, const FVector4& InEdgeColor)
{
	if (std::isfinite(InDuration) && InDuration > 0.05f)
		DissolveDuration = InDuration;
	DissolveEdgeColor = InEdgeColor;
}

void ACharacter::StartDeathDissolve()
{
	if (bDissolveStarted)
		return;
	bDissolveStarted = true;
	bDissolveCompleted = false;
	DissolveElapsed = 0.0f;
	EnsureDeathDissolveSetup();

	if (SkeletalMeshComponent)
		SkeletalMeshComponent->bCastShadow = false;

	const FBox MeshBounds = SkeletalMeshComponent ? SkeletalMeshComponent->CalcBounds() : FBox{};
	const float MinZ = SkeletalMeshComponent ? MeshBounds.Min.Z : (GetActorLocation().Z - 1.0f);
	const float MaxZ = SkeletalMeshComponent ? std::max(MeshBounds.Max.Z, MinZ + 0.1f) : (MinZ + 1.8f);

	for (UMaterialInstanceDynamic* DynMat : DissolveMaterials)
	{
		if (!DynMat)
			continue;
		DynMat->SetScalarParameterValue(FName("DissolveAmount"), -0.05f);
		DynMat->SetScalarParameterValue(FName("DissolveMinZ"), MinZ);
		DynMat->SetScalarParameterValue(FName("DissolveMaxZ"), MaxZ);
		DynMat->SetVectorParameterValue(FName("DissolveEdgeColor"), DissolveEdgeColor);
	}
	if (DeathParticleComponent)
	{
		DeathParticleComponent->SetFloatParameter("DissolveHeight", MaxZ);
		DeathParticleComponent->SetVisibility(true);
		DeathParticleComponent->Activate(true);
	}
}

void ACharacter::UpdateDeathDissolve(float DeltaTime)
{
	if (!bDissolveStarted || bDissolveCompleted)
		return;

	DissolveElapsed += DeltaTime;
	const float Alpha = std::clamp(DissolveElapsed / std::max(0.05f, DissolveDuration), 0.0f, 1.0f);
	const float DissolveAmount = -0.05f + Alpha * 1.13f;
	const FBox MeshBounds = SkeletalMeshComponent ? SkeletalMeshComponent->CalcBounds() : FBox{};
	const float MinZ = SkeletalMeshComponent ? MeshBounds.Min.Z : (GetActorLocation().Z - 1.0f);
	const float MaxZ = SkeletalMeshComponent ? std::max(MeshBounds.Max.Z, MinZ + 0.1f) : (MinZ + 1.8f);
	for (UMaterialInstanceDynamic* DynMat : DissolveMaterials)
	{
		if (DynMat)
		{
			DynMat->SetScalarParameterValue(FName("DissolveAmount"), DissolveAmount);
			DynMat->SetScalarParameterValue(FName("DissolveMinZ"), MinZ);
			DynMat->SetScalarParameterValue(FName("DissolveMaxZ"), MaxZ);
		}
	}
	if (DeathParticleComponent)
	{
		const float ActiveNormZ = std::clamp(1.0f - (DissolveAmount - 0.125f) / 0.80f, 0.0f, 1.0f);
		const float DissolveWorldZ = MinZ + ActiveNormZ * (MaxZ - MinZ);
		DeathParticleComponent->SetFloatParameter("DissolveHeight", DissolveWorldZ);
	}
	if (Alpha >= 1.0f)
	{
		bDissolveCompleted = true;
		if (SkeletalMeshComponent)
			SkeletalMeshComponent->SetDissolveHidden(true);
	}
}

bool ACharacter::IsDeathDissolveFinished() const
{
	if (!bDissolveStarted || !bDissolveCompleted)
		return false;
	return !DeathParticleComponent || !DeathParticleComponent->IsActive();
}

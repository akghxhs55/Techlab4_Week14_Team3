#include "EnginePCH.h"
#include "GameFramework/TestCharacter.h"
#include "Animation/AnimGraphInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/CharacterMovementComponent.h"
#include "Components/ClothComponent.h"
#include "Components/ParticleSystemComponent.h"
#include "Components/ScriptComponent.h"
#include "GameFramework/Actor/RifleActor.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraShakeBase.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "UI/UserWidget.h"
#include "Asset/AssetManager.h"
#include "Particles/ParticleSystem.h"
#include "Render/Material.h"
#include "Render/MaterialInstance.h"
#include "Render/SkeletalMesh.h"
#include "Engine/World.h"
#include <algorithm>
#include <cmath>

namespace
{
	void SyncRifleToHandBones(USkeletalMeshComponent* MeshComponent, ARifleActor* EquippedRifle,
		int32 RightHandBoneIndex, int32 LeftHandBoneIndex, const FVector& GripOffset, const FRotator& GripRotation)
	{
		if (!MeshComponent || !IsValid(EquippedRifle)) return;
		if (RightHandBoneIndex < 0 || LeftHandBoneIndex < 0) return;
		const FMatrix RightHand = MeshComponent->GetBoneWorldMatrix(RightHandBoneIndex);
		const FMatrix LeftHand = MeshComponent->GetBoneWorldMatrix(LeftHandBoneIndex);
		const FVector RightPosition(RightHand[3][0], RightHand[3][1], RightHand[3][2]);
		const FVector LeftPosition(LeftHand[3][0], LeftHand[3][1], LeftHand[3][2]);
		const FVector Forward = (LeftPosition - RightPosition).GetSafeNormal();
		if (Forward.SizeSquared() < SMALL_NUMBER) return;
		const FVector HandUp(RightHand[2][0], RightHand[2][1], RightHand[2][2]);
		const FVector Up = (HandUp - Forward * HandUp.Dot(Forward)).GetSafeNormal();
		if (Up.SizeSquared() < SMALL_NUMBER) return;
		const FVector Right = Up.Cross(Forward).GetSafeNormal();
		FMatrix RifleWorld = FMatrix::Identity;
		RifleWorld.SetAxis(0, Forward);
		RifleWorld.SetAxis(1, Right);
		RifleWorld.SetAxis(2, Forward.Cross(Right));
		RifleWorld = FTransform(GripRotation, FVector::ZeroVector).GetLocalMatrix() * RifleWorld;
		const FVector RifleForward(RifleWorld[0][0], RifleWorld[0][1], RifleWorld[0][2]);
		const FVector RifleRight(RifleWorld[1][0], RifleWorld[1][1], RifleWorld[1][2]);
		const FVector RifleUp(RifleWorld[2][0], RifleWorld[2][1], RifleWorld[2][2]);
		RifleWorld.SetOrigin(RightPosition + RifleForward * GripOffset.X +
			RifleRight * GripOffset.Y + RifleUp * GripOffset.Z);
		EquippedRifle->GetRootComponent()->SetWorldTransform(RifleWorld, false);
	}
}

ATestCharacter::ATestCharacter()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = ETickingGroup::PostPhysics;
	SetDeathParticlePath("Particles/CharacterDeath.particle");

	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	MeshComponent->SetRelativeLocation(FVector(0.0f, 0.0f, -1.0f));
	USkeletalMesh* Mesh = UAssetManager::GetAssetByPath<USkeletalMesh>("Models/UnrealModel/SK_SKM_Quinn_Simple_root.fbx.bin");
	if (Mesh)
	{
		MeshComponent->SetSkeletalMesh(Mesh);
		if (!MeshComponent->SetAnimBlueprint(GetAnimationBlueprintPath()))
			KRS_LOG(Error, "Failed to assign rifleanimgraph.animblueprint to ATestCharacter ({})", MeshComponent->GetAnimBlueprintError());
	}
	else KRS_LOG(Error, "Failed to load SK_SKM_Quinn_Simple_root.fbx.bin for ATestCharacter");

	// CharacterMovement 속도는 cm 단위를 사용한다
	GetCharacterMovement()->MaxWalkSpeed = 600.0f;

	SkirtClothComponent = CreateDefaultSubobject<UClothComponent>("SkirtClothComponent");
	SkirtClothComponent->SetupAttachment(CapsuleComponent);
	SkirtClothComponent->SetRelativeLocation(FVector(0.02f, 0.0f, 0.04f));
	SkirtClothComponent->Preset = EClothPreset::Cone;
	SkirtClothComponent->TopRadius = 0.145f;
	SkirtClothComponent->BottomRadius = 0.46f;
	SkirtClothComponent->ClothHeight = 0.44f;
	SkirtClothComponent->SegmentsX = 24;
	SkirtClothComponent->SegmentsY = 10;
	SkirtClothComponent->Damping = FVector(0.05f, 0.05f, 0.05f);
	SkirtClothComponent->StretchStiffness = 1.0f;
	SkirtClothComponent->BendStiffness = 0.18f;
	SkirtClothComponent->ShearStiffness = 0.55f;
	SkirtClothComponent->DragCoefficient = 0.30f;
	SkirtClothComponent->LiftCoefficient = 0.12f;
	SkirtClothComponent->LinearInertiaScale = 0.55f;
	SkirtClothComponent->AngularInertiaScale = 0.35f;
	SkirtClothComponent->CentrifugalInertiaScale = 0.25f;
	SkirtClothComponent->bUseVirtualParticles = true;
	SkirtClothComponent->bUseBackstop = false;
	SkirtClothComponent->bUseMotionConstraints = false;
	SkirtClothComponent->bEnableContinuousCollision = true;
	EnsureSkirtSetup();

	SpringArmComponent = CreateDefaultSubobject<USpringArmComponent>("SpringArmComponent");
	SpringArmComponent->SetupAttachment(CapsuleComponent);
	SpringArmComponent->SetRelativeLocation(FVector(0.0f, 0.0f, 0.7f));
	SpringArmComponent->TargetArmLength = 2.8f;
	SpringArmComponent->TargetOffset = FVector(0.0f, 0.0f, 0.10f);
	SpringArmComponent->SocketOffset = FVector(0.0f, 0.55f, 0.10f);
	SpringArmComponent->bUsePawnControlRotation = true;
	SpringArmComponent->bInheritPitch = true;
	SpringArmComponent->bInheritYaw = true;
	SpringArmComponent->bInheritRoll = false;
	SpringArmComponent->bEnableCameraLag = true;
	SpringArmComponent->CameraLagSpeed = 12.0f;
	SpringArmComponent->bDrawDebug = false;

	CameraComponent = CreateDefaultSubobject<UCameraComponent>("CameraComponent");
	CameraComponent->SetupAttachment(SpringArmComponent);
	CameraComponent->SetRelativeLocation(FVector::ZeroVector);
	CameraComponent->SetExternalInputManaged(true);
	CameraComponent->SetFieldOfView(NormalFOV);

	DashRibbonComponent = CreateDefaultSubobject<UParticleSystemComponent>("DashRibbonComponent");
	DashRibbonComponent->SetupAttachmentToSocket(MeshComponent, "head", EAttachmentRule::KeepRelative);
	if (UParticleSystem* DashTemplate = UAssetManager::LoadParticleSystem("Particles/DashRibbon.particle"))
		DashRibbonComponent->InitializeSystem(DashTemplate);
	DashRibbonComponent->SetAutoActivate(false);
	DashRibbonComponent->SetVisibility(false);
	DashRibbonComponent->Deactivate();

	UParticleSystemComponent* DashAfterimageComponent = CreateDefaultSubobject<UParticleSystemComponent>("DashAfterimageComponent");
	DashAfterimageComponent->SetupAttachment(MeshComponent);
	if (UParticleSystem* Template = UAssetManager::LoadParticleSystem("Particles/DashAfterimage.particle"))
		DashAfterimageComponent->InitializeSystem(Template);
	DashAfterimageComponent->SetAutoActivate(false);
	DashAfterimageComponent->SetVisibility(false);
	DashAfterimageComponent->Deactivate();

	UScriptComponent* TuningScript = CreateDefaultSubobject<UScriptComponent>("CameraTuningScript");
	TuningScript->SetScriptPath("Scripts/TestCharacterTuning.lua");
}

void ATestCharacter::BeginPlay()
{
	Super::BeginPlay();
	UWorld* World = GetWorld();
	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	if (DashRibbonComponent)
	{
		DashRibbonComponent->SetupAttachmentToSocket(MeshComponent, "head", EAttachmentRule::KeepRelative);
		DashRibbonComponent->SetVisibility(false);
		DashRibbonComponent->Deactivate();
		DashRibbonComponent->MarkTransformDirty();
	}
	if (World && World->IsGameWorld() && MeshComponent &&
		!MeshComponent->SetAnimBlueprint(GetAnimationBlueprintPath()))
		KRS_LOG(Error, "Failed to assign rifleanimgraph.animblueprint to ATestCharacter in BeginPlay ({})", MeshComponent->GetAnimBlueprintError());
	EnsureSkirtSetup();
	if (World && World->IsGameWorld() && SpringArmComponent)
	{
		SpringArmComponent->TargetArmLength = NormalArmLength;
		SpringArmComponent->TargetOffset = FVector(0.0f, 0.0f, 0.10f);
		SpringArmComponent->SocketOffset = FVector(0.0f, 0.55f, 0.10f);
		SpringArmComponent->bEnableCameraLag = true;
		SpringArmComponent->CameraLagSpeed = 12.0f;
		SpringArmComponent->bDrawDebug = false;
		SpringArmComponent->ResetCameraLag();
	}
	USkeletalMesh* Mesh = MeshComponent ? MeshComponent->GetSkeletalMesh() : nullptr;
	if (!World || !World->IsGameWorld() || !Mesh) return;
	RightHandBoneIndex = Mesh->FindBoneIndex("hand_r");
	LeftHandBoneIndex = Mesh->FindBoneIndex("hand_l");
	if (RightHandBoneIndex < 0 || LeftHandBoneIndex < 0)
	{
		KRS_LOG(Error, "Quinn hand bone is missing");
		return;
	}
	ARifleActor* Rifle = Cast<ARifleActor>(World->SpawnActor(ARifleActor::StaticClass()));
	if (!Rifle) return;
	if (!Rifle->AttachToActor(this, EAttachmentRule::KeepWorld, MeshComponent))
	{
		Rifle->Destroy();
		return;
	}
	EquippedRifle = Rifle;
	if (UAnimGraphInstance* AnimGraph = Cast<UAnimGraphInstance>(MeshComponent->GetAnimInstance()))
	{
		AnimGraph->BindMontageNotify([this](const FString& NotifyName)
		{
			if (NotifyName == "Fire") FireRifle();
		});
	}
}

void ATestCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (IsDead())
	{
		// 사망 후 이동을 멈추고 디졸브가 끝나면 총기를 정리한다
		if (GetCharacterMovement()) GetCharacterMovement()->StopMovementImmediately();
		UpdateDeathDissolve(DeltaTime);
		if (IsDeathDissolveFinished() && IsValid(EquippedRifle))
		{
			EquippedRifle->Destroy();
			EquippedRifle = nullptr;
		}
		return;
	}
	if (!bAimBlendAlphaOverridden && std::isfinite(DeltaTime) && DeltaTime > 0.0f)
		AimBlendAlpha = std::clamp(AimBlendAlpha + (bIsAiming ? 1.0f : -1.0f) * DeltaTime / 0.18f, 0.0f, 1.0f);
	if (SpringArmComponent)
		SpringArmComponent->TargetArmLength = FMath::Lerp(NormalArmLength, AimArmLength, AimBlendAlpha);
	if (CameraComponent)
		CameraComponent->SetFieldOfView(FMath::Lerp(NormalFOV, AimFOV, AimBlendAlpha));
	if (UAnimGraphInstance* AnimGraph = Cast<UAnimGraphInstance>(GetSkeletalMeshComponent()->GetAnimInstance()))
	{
		AnimGraph->SetIsAiming(bIsAiming);
		// 조준 블렌드 알파와 스크립트 오버라이드 값을 함께 반영한다
		AnimGraph->SetFloat(UAnimGraphInstance::IsAimingVariable, AimBlendAlpha);
		if (AnimGraph->IsStateActiveOrPending("Hit"))
			GetCharacterMovement()->StopMovementImmediately();
	}
	SyncRifleToHandBones(GetSkeletalMeshComponent(), EquippedRifle,
		RightHandBoneIndex, LeftHandBoneIndex, RifleGripOffset, RifleGripRotation);
}

bool ATestCharacter::SetCameraTuning(float InNormalArmLength, float InNormalFOV,
	float InAimArmLength, float InAimFOV)
{
	if (!std::isfinite(InNormalArmLength) || InNormalArmLength < 0.0f ||
		!std::isfinite(InAimArmLength) || InAimArmLength < 0.0f ||
		!std::isfinite(InNormalFOV) || InNormalFOV <= 1.0f || InNormalFOV >= 179.0f ||
		!std::isfinite(InAimFOV) || InAimFOV <= 1.0f || InAimFOV >= 179.0f)
		return false;
	NormalArmLength = InNormalArmLength;
	NormalFOV = InNormalFOV;
	AimArmLength = InAimArmLength;
	AimFOV = InAimFOV;
	return true;
}

bool ATestCharacter::SetAimBlendAlphaOverride(float Alpha)
{
	if (!std::isfinite(Alpha) || Alpha < 0.0f || Alpha > 1.0f) return false;
	AimBlendAlpha = Alpha;
	bAimBlendAlphaOverridden = true;
	return true;
}

void ATestCharacter::EndPlay()
{
	StopAim();
	if (IsValid(EquippedRifle)) EquippedRifle->Destroy();
	EquippedRifle = nullptr;
	Super::EndPlay();
}

void ATestCharacter::AddMovementInput(const FVector& WorldDirection, float ScaleValue)
{
	if (IsDead()) return;
	Super::AddMovementInput(FVector(WorldDirection.X, WorldDirection.Y, 0.0f), ScaleValue);
}

void ATestCharacter::StartAim()
{
	if (IsDead() || bIsAiming) return;
	bIsAiming = true;
	if (UAnimGraphInstance* AnimGraph = Cast<UAnimGraphInstance>(GetSkeletalMeshComponent()->GetAnimInstance()))
		AnimGraph->SetIsAiming(true);
	if (!AimWidget && GetWorld() && GetWorld()->IsGameWorld())
	{
		AimWidget = CreateWidget(GetWorld(), "Textures/Aim.widget");
		if (AimWidget)
		{
			AimWidget->AddToViewport(100);
			if (!AimWidget->IsInViewport()) { AimWidget->Destroy(); AimWidget = nullptr; }
		}
	}
}

void ATestCharacter::StopAim()
{
	bIsAiming = false;
	if (UAnimGraphInstance* AnimGraph = Cast<UAnimGraphInstance>(GetSkeletalMeshComponent()->GetAnimInstance()))
		AnimGraph->SetIsAiming(false);
	if (AimWidget) { AimWidget->Destroy(); AimWidget = nullptr; }
}

void ATestCharacter::Attack()
{
	if (IsDead() || bAttackLocked || !IsValid(EquippedRifle)) return;
	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	if (UAnimGraphInstance* AnimGraph = Cast<UAnimGraphInstance>(MeshComponent ? MeshComponent->GetAnimInstance() : nullptr))
	{
		// 발사 노티파이에서 총알을 발사하도록 콜백을 연결하고 발사 몽타주를 재생한다
		AnimGraph->BindMontageNotify([this](const FString& NotifyName)
		{
			if (NotifyName == "Fire") FireRifle();
		});
		PlayAnimMontage("Animations/RifleFire.animmontage");
	}
}

void ATestCharacter::FireRifle()
{
	if (!IsValid(EquippedRifle)) return;
	UCameraComponent* Camera = GetCameraComponent();
	if (!Camera) return;
	SyncRifleToHandBones(GetSkeletalMeshComponent(), EquippedRifle,
		RightHandBoneIndex, LeftHandBoneIndex, RifleGripOffset, RifleGripRotation);
	if (!EquippedRifle->Fire(Camera->GetWorldLocation(), FQuat(Camera->GetWorldRotation()).GetForwardVector())) return;
	APlayerController* Controller = GetController();
	APlayerCameraManager* CameraManager = Controller ? Controller->GetCameraManager() : nullptr;
	if (CameraManager)
	{
		if (UCameraShakeBase* Shake = CameraManager->StartCameraShake(UCameraShakeBase::StaticClass()))
		{
			Shake->Duration = 0.16f;
			Shake->BlendInTime = 0.0f;
			Shake->BlendOutTime = 0.13f;
			Shake->RotationPitch.Amplitude = 0.65f;
			Shake->RotationPitch.Frequency = 12.0f;
			Shake->RotationPitch.InitialOffsetType = EInitialWaveOscillatorOffsetType::Zero;
			Shake->RotationYaw.Amplitude = 0.25f;
			Shake->RotationYaw.Frequency = 16.0f;
			Shake->StartShake();
		}
	}
}

void ATestCharacter::Jump()
{
	if (IsDead()) return;
	if (!bJumpLocked && !bAttackLocked && CanJump()) ACharacter::Jump();
}

void ATestCharacter::Dash()
{
	if (IsDead()) return;
	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	UAnimGraphInstance* AnimGraph = Cast<UAnimGraphInstance>(MeshComponent ? MeshComponent->GetAnimInstance() : nullptr);
	if (AnimGraph && AnimGraph->IsPlayingMontage("FullBody")) return;
	// 전신 슬롯 대시 몽타주를 실행한다
	PlayAnimMontage("Animations/Dash.animmontage");
}

void ATestCharacter::ReactToHit()
{
	if (IsDead()) return;
	--Health;
	UAnimGraphInstance* AnimGraph = Cast<UAnimGraphInstance>(GetSkeletalMeshComponent()->GetAnimInstance());
	if (IsDead())
	{
		StopAim();
		if (GetCharacterMovement()) GetCharacterMovement()->StopMovementImmediately();
		if (AnimGraph)
		{
			AnimGraph->StopMontage("", 0.08f);
			AnimGraph->RequestDeath();
		}
	}
	else if (AnimGraph)
	{
		AnimGraph->RequestHit();
	}
}

void ATestCharacter::EnsureSkirtSetup()
{
	if (!SkirtClothComponent)
	{
		return;
	}

	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	SkirtClothComponent->SetupAttachment(CapsuleComponent);
	SkirtClothComponent->SetTargetSkeletalMesh(MeshComponent);
	SkirtClothComponent->ClearBonePins();
	SkirtClothComponent->PinRowToBone(0, "pelvis");
	SkirtClothComponent->ClearBoneCapsuleColliders();
	SkirtClothComponent->AddBoneCapsuleCollider("pelvis", "spine_01", 0.115f, 0.115f);
	SkirtClothComponent->AddBoneCapsuleCollider("spine_01", "spine_02", 0.12f, 0.12f);
	SkirtClothComponent->AddBoneCapsuleCollider("thigh_l", "calf_l", 0.09f, 0.08f);
	SkirtClothComponent->AddBoneCapsuleCollider("calf_l", "foot_l", 0.08f, 0.065f);
	SkirtClothComponent->AddBoneCapsuleCollider("thigh_r", "calf_r", 0.09f, 0.08f);
	SkirtClothComponent->AddBoneCapsuleCollider("calf_r", "foot_r", 0.08f, 0.065f);

	if (!SkirtMaterialInstance)
	{
		if (UMaterial* DefaultMat = UAssetManager::GetAssetByPath<UMaterial>("DefaultMaterial"))
		{
			SkirtMaterialInstance = UMaterial::CreateDynamicInstance(DefaultMat);
			if (SkirtMaterialInstance)
			{
				SkirtMaterialInstance->SetBaseColorOverride(FVector4(0.14f, 0.18f, 0.38f, 1.0f));
			}
		}
	}
	if (SkirtMaterialInstance)
	{
		SkirtClothComponent->SetMaterial(0, SkirtMaterialInstance);
	}
}

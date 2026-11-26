#include "EnginePCH.h"
#include "GameFramework/Actor/DragonRagdollDirector.h"
#include "GameFramework/Actor/SkeletalMeshActor.h"
#include "GameFramework/PlayerController.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Asset/AssetManager.h"
#include "Camera/PlayerCameraManager.h"
#include "Camera/CameraShakeBase.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Input/InputSystem.h"
#include "Collision/Ray.h"
#include "Collision/HitResult.h"
#include <cmath>

namespace
{
	const FVector WorldUp(0.0f, 0.0f, 1.0f);

	FVector Horizontal(const FVector& V)
	{
		return FVector(V.X, V.Y, 0.0f);
	}

	// 프레임레이트와 무관한 지수 감쇠 보간 계수
	float LagAlpha(float Speed, float DeltaTime)
	{
		return Speed <= 0.0f ? 1.0f : 1.0f - std::exp(-Speed * DeltaTime);
	}

	// 카메라가 산비탈 속으로 들어가지 않도록 바로 아래 지면보다 MinClearance만큼 위에 둔다
	FVector KeepAboveGround(UWorld* World, const FVector& Location, float MinClearance)
	{
		FRay Ray;
		Ray.Origin = FVector(Location.X, Location.Y, Location.Z + 500.0f);
		Ray.Direction = FVector(0.0f, 0.0f, -1.0f);
		FHitResult Hit;
		if (World && World->LineTraceSingle(Ray, Hit) && Location.Z < Hit.ImpactPoint.Z + MinClearance)
		{
			return FVector(Location.X, Location.Y, Hit.ImpactPoint.Z + MinClearance);
		}
		return Location;
	}

	FVector BoneLocation(const USkeletalMeshComponent* Mesh, int32 BoneIndex)
	{
		return Mesh->GetBoneWorldMatrix(BoneIndex).TransformPosition(FVector::ZeroVector);
	}
}

ADragonRagdollDirector::ADragonRagdollDirector()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ADragonRagdollDirector::BeginPlay()
{
	Super::BeginPlay();

	IdleAnim = UAssetManager::LoadAnimSequence(IdleAnimationPath);
	AttackAnim = UAssetManager::LoadAnimSequence(PowerAttackAnimationPath);
	if (!IdleAnim) KRS_LOG(Warning, "[DragonRagdollDirector] idle animation not found: {}", IdleAnimationPath);
	if (!AttackAnim) KRS_LOG(Warning, "[DragonRagdollDirector] power attack animation not found: {}", PowerAttackAnimationPath);
	VictimIdleAnim = UAssetManager::LoadAnimSequence(VictimIdleAnimationPath);
	VictimBraceAnim = UAssetManager::LoadAnimSequence(VictimBraceAnimationPath);
	if (!VictimIdleAnim) KRS_LOG(Warning, "[DragonRagdollDirector] victim idle animation not found: {}", VictimIdleAnimationPath);
	if (!VictimBraceAnim) KRS_LOG(Warning, "[DragonRagdollDirector] victim brace animation not found: {}", VictimBraceAnimationPath);

	if (!ResolveActors())
	{
		KRS_LOG(Warning, "[DragonRagdollDirector] actors not found (dragon '{}', victim '{}')", DragonActorName, VictimActorName);
		return;
	}

	VictimStartLocation = Victim->GetActorLocation();
	VictimStartRotation = Victim->GetActorRotation();
	StartSequence();
}

bool ADragonRagdollDirector::ResolveActors()
{
	Dragon = nullptr;
	Victim = nullptr;
	UWorld* World = GetWorld();
	if (!World || !World->GetCurrentLevel())
	{
		return false;
	}
	for (AActor* Actor : World->GetCurrentLevel()->GetActors())
	{
		ASkeletalMeshActor* MeshActor = Cast<ASkeletalMeshActor>(Actor);
		if (!MeshActor || !MeshActor->GetSkeletalMeshComponent()) continue;
		if (MeshActor->GetName() == DragonActorName) Dragon = MeshActor;
		else if (MeshActor->GetName() == VictimActorName) Victim = MeshActor;
	}
	return Dragon && Victim;
}

void ADragonRagdollDirector::StartSequence()
{
	Phase = EPhase::Idle;
	PhaseTime = 0.0f;
	bDragonBackToIdle = false;
	bCameraInitialized = false;

	LaunchDirection = Horizontal(GetVictimLocation() - Dragon->GetActorLocation()).GetSafeNormal(SMALL_NUMBER, FVector(1.0f, 0.0f, 0.0f));

	if (IdleAnim)
	{
		Dragon->GetSkeletalMeshComponent()->PlayAnimation(IdleAnim, true);
	}
	if (VictimIdleAnim)
	{
		Victim->GetSkeletalMeshComponent()->PlayAnimation(VictimIdleAnim, true);
	}
}

void ADragonRagdollDirector::StartAttack()
{
	Phase = EPhase::Attack;
	PhaseTime = 0.0f;
	USkeletalMeshComponent* DragonMesh = Dragon->GetSkeletalMeshComponent();
	if (AttackAnim)
	{
		DragonMesh->PlayAnimation(AttackAnim, false);
		if (UAnimSingleNodeInstance* Instance = DragonMesh->GetSingleNodeInstance())
		{
			Instance->SetPlayRate(PowerAttackPlayRate);
		}
	}
	// 용이 몸을 일으키는 순간 놀라 올려다보고, 내려찍기 전에 팔로 얼굴을 막으며 움츠린다
	if (VictimBraceAnim)
	{
		Victim->GetSkeletalMeshComponent()->PlayAnimation(VictimBraceAnim, false);
	}
}

void ADragonRagdollDirector::Impact()
{
	Phase = EPhase::Launched;
	PhaseTime = 0.0f;

	USkeletalMeshComponent* Mesh = Victim->GetSkeletalMeshComponent();
	// 방어 자세 그대로 래그돌이 시작되도록 애니메이션을 그 프레임에서 멈춘다
	Mesh->Stop();
	Mesh->SetSimulatePhysics(true);

	const FVector Launch = LaunchDirection * LaunchBackSpeed + WorldUp * LaunchUpSpeed;
	Mesh->AddImpulseToAllBodiesBelow(Launch, NAME_None, true);

	// 상체는 뒤로·위로, 다리는 앞으로 밀어 뒤로 젖혀지며 회전하게 한다
	const FVector UpperKick = LaunchDirection * LaunchSpinSpeed + WorldUp * (LaunchSpinSpeed * 0.5f);
	const FVector LowerKick = LaunchDirection * (-LaunchSpinSpeed);
	for (const char* Bone : { "Chest", "Head" }) Mesh->AddImpulse(UpperKick, Bone, true);
	for (const char* Bone : { "Leg.L", "Leg.R" }) Mesh->AddImpulse(LowerKick, Bone, true);
	// 팔다리가 제각각 휘날리도록 좌우로 살짝 다르게 준다
	const FVector Side = WorldUp.Cross(LaunchDirection);
	Mesh->AddImpulse(Side * 3.0f + WorldUp * 2.0f, "ForeArm.L", true);
	Mesh->AddImpulse(Side * -3.0f + WorldUp * 1.0f, "ForeArm.R", true);

	PlayCameraShake();
	if (HitStopDuration > 0.0f)
	{
		GetWorld()->StartHitStop(HitStopDuration, 0.05f);
	}
}

void ADragonRagdollDirector::PlayCameraShake()
{
	APlayerController* Controller = GetController();
	APlayerCameraManager* CameraManager = Controller ? Controller->GetCameraManager() : nullptr;
	if (!CameraManager || ImpactShakeScale <= 0.0f)
	{
		return;
	}

	// 짧고 강한 충격 + 길게 남는 낮은 진동 두 겹
	if (UCameraShakeBase* Shake = CameraManager->StartCameraShake(UCameraShakeBase::StaticClass(), ImpactShakeScale))
	{
		Shake->Duration = 0.45f;
		Shake->BlendInTime = 0.0f;
		Shake->BlendOutTime = 0.3f;
		Shake->LocationZ = { 0.35f, 19.0f, EInitialWaveOscillatorOffsetType::Zero };
		Shake->LocationY = { 0.2f, 23.0f, EInitialWaveOscillatorOffsetType::Random };
		Shake->RotationPitch = { 3.0f, 15.0f, EInitialWaveOscillatorOffsetType::Zero };
		Shake->RotationYaw = { 1.5f, 12.0f, EInitialWaveOscillatorOffsetType::Random };
		Shake->RotationRoll = { 2.0f, 9.0f, EInitialWaveOscillatorOffsetType::Random };
		Shake->FOV = { 4.0f, 6.0f, EInitialWaveOscillatorOffsetType::Zero };
		Shake->StartShake(ImpactShakeScale);
	}
	if (UCameraShakeBase* Rumble = CameraManager->StartCameraShake(UCameraShakeBase::StaticClass(), ImpactShakeScale))
	{
		Rumble->Duration = 1.6f;
		Rumble->BlendInTime = 0.05f;
		Rumble->BlendOutTime = 1.0f;
		Rumble->LocationZ = { 0.08f, 7.0f, EInitialWaveOscillatorOffsetType::Random };
		Rumble->RotationPitch = { 0.6f, 5.0f, EInitialWaveOscillatorOffsetType::Random };
		Rumble->RotationRoll = { 0.5f, 4.0f, EInitialWaveOscillatorOffsetType::Random };
		Rumble->StartShake(ImpactShakeScale);
	}
}

FVector ADragonRagdollDirector::GetVictimLocation() const
{
	return Victim ? Victim->GetActorLocation() : FVector::ZeroVector;
}

FVector ADragonRagdollDirector::GetFocusLocation() const
{
	if (!Victim)
	{
		return GetActorLocation();
	}
	const USkeletalMeshComponent* Mesh = Victim->GetSkeletalMeshComponent();
	const int32 Bone = Mesh->FindBoneIndex(FocusBoneName);
	return Bone >= 0 ? BoneLocation(Mesh, Bone) : Victim->GetActorLocation();
}

FVector ADragonRagdollDirector::GetBodyCenter() const
{
	if (!Victim)
	{
		return GetActorLocation();
	}
	// 팔다리가 제각각 휘날려도 몸 전체의 중심을 잡도록 루트를 뺀 모든 본 위치를 평균낸다
	const USkeletalMeshComponent* Mesh = Victim->GetSkeletalMeshComponent();
	const TArray<FBoneInfo>& Bones = Mesh->GetBones();
	FVector Sum = FVector::ZeroVector;
	int32 Count = 0;
	for (int32 B = 0; B < Bones.Num(); ++B)
	{
		if (Bones[B].ParentIndex < 0) continue;
		Sum += BoneLocation(Mesh, B);
		++Count;
	}
	return Count > 0 ? Sum * (1.0f / static_cast<float>(Count)) : GetFocusLocation();
}

void ADragonRagdollDirector::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!Dragon || !Victim)
	{
		return;
	}

	if (FInputSystem::IsKeyPressed(EKeyCode::R))
	{
		USkeletalMeshComponent* Mesh = Victim->GetSkeletalMeshComponent();
		Mesh->SetSimulatePhysics(false);
		Victim->GetRootComponent()->SetWorldLocationAndRotation(VictimStartLocation, VictimStartRotation, false, ETeleportType::TeleportPhysics);
		Mesh->ResetPose();
		Mesh->RefreshBoneTransformsNow();
		Mesh->UpdateKinematicBonesToAnim(ETeleportType::TeleportPhysics);
		if (APlayerController* Controller = GetController())
		{
			if (APlayerCameraManager* CameraManager = Controller->GetCameraManager()) CameraManager->StopAllCameraShakes(true);
		}
		StartSequence();
		return;
	}

	PhaseTime += DeltaTime;
	switch (Phase)
	{
	case EPhase::Idle:
		if (PhaseTime >= IdleTimeBeforeAttack) StartAttack();
		break;
	case EPhase::Attack:
		if (PhaseTime * PowerAttackPlayRate >= ImpactTime) Impact();
		break;
	case EPhase::Launched:
		break;
	}

	// 공격 모션이 끝나면 드래곤은 다시 Idle로 돌아간다
	if (Phase == EPhase::Launched && !bDragonBackToIdle && IdleAnim)
	{
		const UAnimSingleNodeInstance* Instance = Dragon->GetSkeletalMeshComponent()->GetSingleNodeInstance();
		const float EndTime = PowerAttackEndTime > 0.0f ? PowerAttackEndTime : (Instance ? Instance->GetLength() : 0.0f);
		if (!Instance || Instance->GetCurrentAsset() != AttackAnim || Instance->GetCurrentTime() >= EndTime - 0.02f)
		{
			Dragon->GetSkeletalMeshComponent()->PlayAnimation(IdleAnim, true);
			bDragonBackToIdle = true;
		}
	}
}

void ADragonRagdollDirector::UpdateThirdPersonCamera(float DeltaTime, bool bSnap)
{
	// 스카이림식 3인칭: 피해자 어깨 높이를 중심으로 등 뒤·오른쪽 어깨 너머에서 드래곤 쪽(발사 반대 방향)을 바라본다.
	// 날아가는 동안에도 바라보는 방향은 그대로 두고 몸을 따라가므로, 날아가는 몸 뒤로 드래곤이 멀어지는 것이 보인다
	const FVector Forward = -LaunchDirection;               // 피해자가 바라보는 방향 (드래곤 쪽)
	const FVector Right = WorldUp.Cross(Forward);
	const FVector Pivot = GetFocusLocation() + WorldUp * FollowUpOffset + Right * FollowSideDistance;
	const FVector DesiredLocation = KeepAboveGround(GetWorld(), Pivot - Forward * FollowBackDistance, 0.5f);
	const FVector DesiredLook = Pivot + Forward * 10.0f + WorldUp * CameraLookUpOffset;

	if (bSnap || !bCameraInitialized)
	{
		CameraLocation = DesiredLocation;
		CameraLookAt = DesiredLook;
		bCameraInitialized = true;
		return;
	}
	CameraLocation = FMath::Lerp(CameraLocation, DesiredLocation, LagAlpha(FollowLocationLag, DeltaTime));
	CameraLookAt = FMath::Lerp(CameraLookAt, DesiredLook, LagAlpha(FollowLookLag, DeltaTime));
}

void ADragonRagdollDirector::GetRagdollCamera(FVector& OutLocation, FVector& OutLookAt) const
{
	// 어깨 시점 방향(드래곤 쪽)에서 RagdollCameraYaw만큼 옆으로 돌아 살짝 측면에서, 위에서 내려다보며 몸 중심을 시선 정중앙에 둔다.
	// 지연 없이 몸에 붙어 있어 날아가는 동안 몸은 화면 중앙에 고정되고 배경이 흘러간다
	const FVector Forward = -LaunchDirection;
	const FVector Right = WorldUp.Cross(Forward);
	const float Yaw = RagdollCameraYaw * PI / 180.0f;
	const FVector FlatDir = Forward * std::cos(Yaw) + Right * std::sin(Yaw);
	const float Pitch = RagdollCameraPitch * PI / 180.0f;
	const FVector ViewDir = FlatDir * std::cos(Pitch) - WorldUp * std::sin(Pitch);
	OutLookAt = GetBodyCenter();
	OutLocation = KeepAboveGround(GetWorld(), OutLookAt - ViewDir * RagdollCameraDistance, 0.5f);
}

void ADragonRagdollDirector::CalcCamera(float DeltaTime, FMinimalViewInfo& OutPOV)
{
	if (!Dragon || !Victim)
	{
		Super::CalcCamera(DeltaTime, OutPOV);
		return;
	}

	UpdateThirdPersonCamera(DeltaTime, false);

	FVector ViewLocation = CameraLocation;
	FVector ViewLookAt = CameraLookAt;
	float ViewFOV = CameraFOV;
	if (Phase == EPhase::Launched)
	{
		FVector RagdollLocation, RagdollLookAt;
		GetRagdollCamera(RagdollLocation, RagdollLookAt);
		float Alpha = RagdollCameraTransitionTime > 0.0f ? FMath::Clamp(PhaseTime / RagdollCameraTransitionTime, 0.0f, 1.0f) : 1.0f;
		Alpha = Alpha * Alpha * (3.0f - 2.0f * Alpha);
		ViewLocation = FMath::Lerp(CameraLocation, RagdollLocation, Alpha);
		ViewLookAt = FMath::Lerp(CameraLookAt, RagdollLookAt, Alpha);
		ViewFOV = CameraFOV + (RagdollCameraFOV - CameraFOV) * Alpha;
	}

	FRotator Rotation;
	Rotation.LookDirection((ViewLookAt - ViewLocation).GetSafeNormal(SMALL_NUMBER, LaunchDirection));
	OutPOV.Location = ViewLocation;
	OutPOV.Rotation = Rotation;
	OutPOV.FOV = ViewFOV;
}

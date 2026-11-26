#include "EnginePCH.h"
#include "GameFramework/SwordCharacter.h"
#include "Animation/AnimGraphInstance.h"
#include "Asset/AssetManager.h"
#include "Asset/AssetUtils.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/CharacterMovementComponent.h"
#include "Components/ClothComponent.h"
#include "Components/ParticleSystemComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/StockingComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/Actor/StockingActor.h"
#include "GameFramework/Actor/ZombieActor.h"
#include "Particles/ParticleEventTypes.h"
#include "Particles/ParticleSystem.h"
#include "Physics/BodyInstance.h"
#include "Physics/BodySetup.h"
#include "Physics/PhysicsAsset.h"
#include "Render/Material.h"
#include "Render/MaterialInstance.h"
#include "Render/Mesh.h"
#include "Render/SkeletalMesh.h"
#include <cmath>

namespace
{
	void EnsureFallbackMaterialColor(UMaterialInterface* MatInterface, const FVector4& Color)
	{
		UMaterial* Mat = Cast<UMaterial>(MatInterface);
		if (!Mat) return;
		if (Mat->BaseColor.X <= 0.001f && Mat->BaseColor.Y <= 0.001f && Mat->BaseColor.Z <= 0.001f)
			Mat->BaseColor = Color;
	}

	void EnsureSwordMaterials(USkeletalMeshComponent* MeshComponent, UStaticMeshComponent* SwordMeshComponent)
	{
		if (MeshComponent && MeshComponent->GetSkeletalMesh())
		{
			USkeletalMesh* SkelMesh = MeshComponent->GetSkeletalMesh();
			if (SkelMesh->GetMaterialSlots().IsValidIndex(0))
				EnsureFallbackMaterialColor(SkelMesh->GetMaterial(0), FVector4(0.72f, 0.75f, 0.80f, 1.0f));
			if (SkelMesh->GetMaterialSlots().IsValidIndex(1))
				EnsureFallbackMaterialColor(SkelMesh->GetMaterial(1), FVector4(0.18f, 0.20f, 0.24f, 1.0f));
			MeshComponent->MarkRenderStateDirty();
		}
		if (SwordMeshComponent && SwordMeshComponent->GetStaticMesh())
		{
			UStaticMesh* SwordMesh = SwordMeshComponent->GetStaticMesh();
			for (UMaterialInterface* Mat : SwordMesh->Materials)
				EnsureFallbackMaterialColor(Mat, FVector4(0.85f, 0.88f, 0.94f, 1.0f));
			SwordMeshComponent->MarkRenderStateDirty();
		}
	}

	void EnsureSwordSockets(USkeletalMesh* SkeletalMesh)
	{
		if (!SkeletalMesh) return;
		bool bModified = false;

		if (!SkeletalMesh->HasSocket("SwordHandSocket"))
		{
			FSkeletalMeshSocket HandSocket;
			HandSocket.SocketName = "SwordHandSocket";
			HandSocket.BoneName = "hand_r";
			HandSocket.RelativeLocation = FVector(0.035f, 0.085f, -0.01f);
			HandSocket.RelativeRotation = FRotator(0.0f, 0.0f, 180.0f);
			HandSocket.RelativeScale = FVector(1.0f, 1.0f, 1.0f);
			bModified |= SkeletalMesh->AddSocket(HandSocket);
		}

		if (!SkeletalMesh->HasSocket("SwordBaseSocket"))
		{
			const FSkeletalMeshSocket* HandSocket = SkeletalMesh->FindSocket("SwordHandSocket");
			if (HandSocket)
			{
				const FMatrix BaseLocal =
					FTransform(FRotator(0.0f, 0.0f, 0.0f), FVector(0.0f, 0.0f, -0.22f)).GetLocalMatrix() *
					HandSocket->GetSocketLocalMatrix();
				FSkeletalMeshSocket BaseSocket;
				BaseSocket.SocketName = "SwordBaseSocket";
				BaseSocket.BoneName = HandSocket->BoneName;
				BaseSocket.RelativeLocation = FVector(BaseLocal[3][0], BaseLocal[3][1], BaseLocal[3][2]);
				BaseSocket.RelativeRotation = HandSocket->RelativeRotation;
				BaseSocket.RelativeScale = FVector(1.0f, 1.0f, 1.0f);
				bModified |= SkeletalMesh->AddSocket(BaseSocket);
			}
		}

		if (!SkeletalMesh->HasSocket("SwordTipSocket"))
		{
			const FSkeletalMeshSocket* HandSocket = SkeletalMesh->FindSocket("SwordHandSocket");
			if (HandSocket)
			{
				const FMatrix TipLocal =
					FTransform(FRotator(0.0f, 0.0f, 0.0f), FVector(0.0f, 0.0f, -1.36f)).GetLocalMatrix() *
					HandSocket->GetSocketLocalMatrix();
				FSkeletalMeshSocket TipSocket;
				TipSocket.SocketName = "SwordTipSocket";
				TipSocket.BoneName = HandSocket->BoneName;
				TipSocket.RelativeLocation = FVector(TipLocal[3][0], TipLocal[3][1], TipLocal[3][2]);
				TipSocket.RelativeRotation = HandSocket->RelativeRotation;
				TipSocket.RelativeScale = FVector(1.0f, 1.0f, 1.0f);
				bModified |= SkeletalMesh->AddSocket(TipSocket);
			}
		}

		if (!SkeletalMesh->HasSocket("SwordWaveSpawn"))
		{
			const FSkeletalMeshSocket* BaseSocket = SkeletalMesh->FindSocket("SwordBaseSocket");
			const FSkeletalMeshSocket* TipSocket = SkeletalMesh->FindSocket("SwordTipSocket");
			if (BaseSocket && TipSocket && BaseSocket->BoneName == TipSocket->BoneName)
			{
				// 검기 스폰 소켓을 검기 시작과 끝 소켓의 중앙에 배치한다
				FSkeletalMeshSocket WaveSpawnSocket;
				WaveSpawnSocket.SocketName = "SwordWaveSpawn";
				WaveSpawnSocket.BoneName = BaseSocket->BoneName;
				WaveSpawnSocket.RelativeLocation = (BaseSocket->RelativeLocation + TipSocket->RelativeLocation) * 0.5f;
				WaveSpawnSocket.RelativeRotation = FRotator(0.0f, 0.0f, 0.0f);
				WaveSpawnSocket.RelativeScale = FVector(1.0f, 1.0f, 1.0f);
				bModified |= SkeletalMesh->AddSocket(WaveSpawnSocket);
			}
		}

		if (bModified)
		{
			const FString ResolvedFile = FAssetUtils::ResolveAssetFilePath(SkeletalMesh->GetPath());
			if (!ResolvedFile.empty())
				SkeletalMesh->SaveSocketsToFile(ResolvedFile + ".sockets.json");
		}
	}
}

ASwordCharacter::ASwordCharacter()
{
	PrimaryActorTick.bCanEverTick = true;
	SetDeathParticlePath("Particles/CharacterDeath.particle");

	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	MeshComponent->SetRelativeLocation(FVector(0.0f, 0.0f, -1.0f));
	if (USkeletalMesh* Mesh = UAssetManager::GetAssetByPath<USkeletalMesh>("Models/SwordModel/SK_SK_Mannequin_root.fbx.bin"))
	{
		MeshComponent->SetSkeletalMesh(Mesh);
		EnsureSwordSockets(Mesh);
		MeshComponent->SetAnimBlueprint("Animations/SwordMannequin.animblueprint");
	}

	SwordMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>("SwordMeshComponent");
	SwordMeshComponent->SetupAttachmentToSocket(MeshComponent, "SwordHandSocket", EAttachmentRule::KeepRelative);
	SwordMeshComponent->bGenerateOverlapEvents = false;
	SwordMeshComponent->bBlockComponent = false;
	if (UStaticMesh* SwordMesh = UAssetManager::GetAssetByPath<UStaticMesh>("Models/SwordModel/SM_Sword_Sword_Static.fbx.bin"))
		SwordMeshComponent->SetStaticMesh(SwordMesh);
	EnsureSwordMaterials(MeshComponent, SwordMeshComponent);

	SwordRibbonComponent = CreateDefaultSubobject<UParticleSystemComponent>("SwordRibbonComponent");
	SwordRibbonComponent->SetupAttachmentToSocket(MeshComponent, "SwordTipSocket", EAttachmentRule::KeepRelative);
	if (UParticleSystem* RibbonTemplate = UAssetManager::LoadParticleSystem("Particles/SwordRibbon.particle"))
		SwordRibbonComponent->InitializeSystem(RibbonTemplate);
	SwordRibbonComponent->SetAutoActivate(false);
	SwordRibbonComponent->SetVisibility(false);
	SwordRibbonComponent->Deactivate();

	CapeClothComponent = CreateDefaultSubobject<UClothComponent>("CapeClothComponent");
	CapeClothComponent->SetupAttachment(CapsuleComponent);
	CapeClothComponent->SetRelativeLocation(FVector(-0.24f, 0.0f, 0.52f));
	CapeClothComponent->Preset = EClothPreset::Grid;
	CapeClothComponent->ClothWidth = 0.38f;
	CapeClothComponent->BottomWidth = 0.86f;
	CapeClothComponent->ClothHeight = 1.14f;
	CapeClothComponent->SegmentsX = 12;
	CapeClothComponent->SegmentsY = 16;
	CapeClothComponent->Damping = FVector(0.04f, 0.04f, 0.04f);
	CapeClothComponent->StretchStiffness = 1.0f;
	CapeClothComponent->BendStiffness = 0.18f;
	CapeClothComponent->ShearStiffness = 0.55f;
	CapeClothComponent->DragCoefficient = 0.25f;
	CapeClothComponent->LiftCoefficient = 0.10f;
	CapeClothComponent->LinearInertiaScale = 0.55f;
	CapeClothComponent->AngularInertiaScale = 0.30f;
	CapeClothComponent->CentrifugalInertiaScale = 0.15f;
	CapeClothComponent->bUseVirtualParticles = true;
	CapeClothComponent->bUseBackstop = false;
	CapeClothComponent->bUseMotionConstraints = false;
	CapeClothComponent->bEnableContinuousCollision = true;
	EnsureCapeSetup();

	SpringArmComponent = CreateDefaultSubobject<USpringArmComponent>("SpringArmComponent");
	SpringArmComponent->SetupAttachment(CapsuleComponent);
	SpringArmComponent->SetRelativeLocation(FVector(0.0f, 0.0f, 0.7f));
	SpringArmComponent->TargetArmLength = 3.0f;
	SpringArmComponent->TargetOffset = FVector(0.0f, 0.0f, 0.10f);
	SpringArmComponent->SocketOffset = FVector(0.0f, 0.45f, 0.10f);
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
	CameraComponent->SetFieldOfView(80.0f);

	// CharacterMovement 속도는 cm 단위를 사용한다
	GetCharacterMovement()->MaxWalkSpeed = 600.0f;
}

void ASwordCharacter::BeginPlay()
{
	Super::BeginPlay();
	bBeamMontageActive = false;
	ElapsedTime = 0.0f;
	ZombieHitCooldowns.Empty();
	if (SpringArmComponent)
		SpringArmComponent->ResetCameraLag();

	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	if (USkeletalMesh* Mesh = MeshComponent ? MeshComponent->GetSkeletalMesh() : nullptr)
		EnsureSwordSockets(Mesh);

	if (SwordMeshComponent)
	{
		if (!SwordMeshComponent->GetStaticMesh())
		{
			if (UStaticMesh* SwordMesh = UAssetManager::GetAssetByPath<UStaticMesh>("Models/SwordModel/SM_Sword_Sword_Static.fbx.bin"))
				SwordMeshComponent->SetStaticMesh(SwordMesh);
		}
		SwordMeshComponent->SetupAttachmentToSocket(MeshComponent, "SwordHandSocket", EAttachmentRule::KeepRelative);
		SwordMeshComponent->SetVisibility(true);
		SwordMeshComponent->MarkTransformDirty();
	}
	EnsureSwordMaterials(MeshComponent, SwordMeshComponent);
	if (SwordRibbonComponent)
	{
		SwordRibbonComponent->SetupAttachmentToSocket(MeshComponent, "SwordTipSocket", EAttachmentRule::KeepRelative);
		SwordRibbonComponent->SetVisibility(false);
		SwordRibbonComponent->Deactivate();
		SwordRibbonComponent->MarkTransformDirty();
	}
	if (MeshComponent && MeshComponent->GetAnimBlueprintPath() != "Animations/SwordMannequin.animblueprint")
	{
		if (!MeshComponent->SetAnimBlueprint("Animations/SwordMannequin.animblueprint"))
			KRS_LOG(Error, "Failed to assign SwordMannequin.animblueprint to ASwordCharacter ({})", MeshComponent->GetAnimBlueprintError());
	}
	EnsureCapeSetup();
}

void ASwordCharacter::EnsureCapeSetup()
{
	if (!CapeClothComponent)
	{
		return;
	}

	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	CapeClothComponent->SetupAttachment(CapsuleComponent);
	CapeClothComponent->SetTargetSkeletalMesh(MeshComponent);
	CapeClothComponent->ClearBonePins();
	CapeClothComponent->PinRowToBone(0, "spine_03");
	CapeClothComponent->ClearBoneCapsuleColliders();
	CapeClothComponent->AddBoneCapsuleCollider("pelvis", "spine_01", 0.16f, 0.16f);
	CapeClothComponent->AddBoneCapsuleCollider("spine_01", "spine_03", 0.16f, 0.17f);
	CapeClothComponent->AddBoneCapsuleCollider("spine_03", "head", 0.13f, 0.12f);
	CapeClothComponent->AddBoneCapsuleCollider("clavicle_l", "upperarm_l", 0.12f, 0.11f);
	CapeClothComponent->AddBoneCapsuleCollider("upperarm_l", "lowerarm_l", 0.10f, 0.09f);
	CapeClothComponent->AddBoneCapsuleCollider("clavicle_r", "upperarm_r", 0.12f, 0.11f);
	CapeClothComponent->AddBoneCapsuleCollider("upperarm_r", "lowerarm_r", 0.10f, 0.09f);
	CapeClothComponent->AddBoneCapsuleCollider("thigh_l", "calf_l", 0.13f, 0.11f);
	CapeClothComponent->AddBoneCapsuleCollider("calf_l", "foot_l", 0.11f, 0.09f);
	CapeClothComponent->AddBoneCapsuleCollider("thigh_r", "calf_r", 0.13f, 0.11f);
	CapeClothComponent->AddBoneCapsuleCollider("calf_r", "foot_r", 0.11f, 0.09f);

	if (!CapeMaterialInstance)
	{
		if (UMaterial* DefaultMat = UAssetManager::GetAssetByPath<UMaterial>("DefaultMaterial"))
		{
			CapeMaterialInstance = UMaterial::CreateDynamicInstance(DefaultMat);
			if (CapeMaterialInstance)
			{
				CapeMaterialInstance->SetBaseColorOverride(FVector4(0.58f, 0.09f, 0.11f, 1.0f));
			}
		}
	}
	if (CapeMaterialInstance)
	{
		CapeClothComponent->SetMaterial(0, CapeMaterialInstance);
	}
}

bool ASwordCharacter::IsPlayingFullBodyMontage() const
{
	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	UAnimGraphInstance* AnimGraph = Cast<UAnimGraphInstance>(MeshComponent ? MeshComponent->GetAnimInstance() : nullptr);
	return AnimGraph && AnimGraph->IsPlayingMontage("FullBody");
}

void ASwordCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (std::isfinite(DeltaTime) && DeltaTime > 0.0f)
		ElapsedTime += DeltaTime;
	if (IsDead())
	{
		// 사망 후 이동을 멈추고 디졸브가 끝나면 검을 숨긴다
		if (UCharacterMovementComponent* Movement = GetCharacterMovement())
			Movement->StopMovementImmediately();
		UpdateDeathDissolve(DeltaTime);
		if (IsDeathDissolveFinished() && SwordMeshComponent)
			SwordMeshComponent->SetVisibility(false);
		return;
	}
	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	if (UAnimGraphInstance* AnimGraph = Cast<UAnimGraphInstance>(MeshComponent ? MeshComponent->GetAnimInstance() : nullptr))
	{
		if (AnimGraph->IsStateActiveOrPending("Hit"))
		{
			if (UCharacterMovementComponent* Movement = GetCharacterMovement())
				Movement->StopMovementImmediately();
		}
	}
	const bool bFullBodyMontage = IsPlayingFullBodyMontage();
	if (bFullBodyMontage)
	{
		ConsumeMovementInputVector();
		if (bBeamMontageActive)
		{
			if (UCharacterMovementComponent* Movement = GetCharacterMovement())
				Movement->StopMovementImmediately();
		}
	}
	if (bBeamMontageActive && !bFullBodyMontage)
	{
		bBeamMontageActive = false;
		if (SwordMeshComponent)
			SwordMeshComponent->SetVisibility(true);
	}
}

void ASwordCharacter::AddMovementInput(const FVector& WorldDirection, float ScaleValue)
{
	if (IsDead() || IsPlayingFullBodyMontage())
		return;
	Super::AddMovementInput(FVector(WorldDirection.X, WorldDirection.Y, 0.0f), ScaleValue);
}

void ASwordCharacter::Jump()
{
	if (IsDead() || IsPlayingFullBodyMontage())
		return;
	Super::Jump();
}

void ASwordCharacter::Attack()
{
	if (IsDead() || IsPlayingFullBodyMontage()) return;
	if (PlayAnimMontage("Animations/combo02.animmontage") > 0.0f)
	{
		if (UCharacterMovementComponent* Movement = GetCharacterMovement())
			Movement->StopMovementImmediately();
	}
}

void ASwordCharacter::CastBeam()
{
	if (IsDead() || IsPlayingFullBodyMontage()) return;
	if (PlayAnimMontage("Animations/BeamMontage.animmontage") > 0.0f)
	{
		bBeamMontageActive = true;
		if (SwordMeshComponent)
			SwordMeshComponent->SetVisibility(false);
		if (UCharacterMovementComponent* Movement = GetCharacterMovement())
			Movement->StopMovementImmediately();
	}
}

void ASwordCharacter::ReactToHit()
{
	if (IsDead()) return;
	--Health;
	UAnimGraphInstance* AnimGraph = Cast<UAnimGraphInstance>(GetSkeletalMeshComponent()->GetAnimInstance());
	if (IsDead())
	{
		bBeamMontageActive = false;
		// 사망 시 새 방출을 멈춘다
		for (UActorComponent* Comp : GetComponents())
		{
			UParticleSystemComponent* ParticleComp = Cast<UParticleSystemComponent>(Comp);
			if (ParticleComp && ParticleComp->GetName() != "DeathParticleComponent" && ParticleComp->IsActive())
				ParticleComp->DeactivateSystem();
		}
		if (UCharacterMovementComponent* Movement = GetCharacterMovement())
			Movement->StopMovementImmediately();
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

void ASwordCharacter::OnParticleCollide(UParticleSystemComponent* Comp, const FParticleEventCollisionData& Event)
{
	if (IsDead() || !Event.HitComponent) return;
	if (AStockingActor* StockingActor = Cast<AStockingActor>(Event.HitComponent->GetOwner()))
	{
		UStockingComponent* StockingComp = StockingActor->GetStockingComponent();
		USkeletalMeshComponent* TargetMesh = StockingActor->GetSkeletalMeshComponent();
		if (StockingComp && TargetMesh)
		{
			FVector HitDir = Event.Velocity;
			if (HitDir.SizeSquared() <= SMALL_NUMBER)
				HitDir = StockingActor->GetActorLocation() - GetActorLocation();
			if (HitDir.SizeSquared() <= SMALL_NUMBER)
				HitDir = -Event.Normal;
			const FVector SafeDir = HitDir.GetSafeNormal();
			int32 HitBoneIndex = -1;
			FString NearestBone;
			float BestDistSq = FLT_MAX;
			if (const UPhysicsAsset* Asset = TargetMesh->GetPhysicsAsset())
			{
				for (const UBodySetup* Setup : Asset->BodySetups)
				{
					const FBodyInstance* Body = Setup ? TargetMesh->GetBodyInstance(Setup->BoneName) : nullptr;
					if (!Body) continue;
					const float DistSq = (Body->GetUnrealWorldTransform().Location - Event.Location).SizeSquared();
					if (DistSq < BestDistSq)
					{
						BestDistSq = DistSq;
						NearestBone = Setup->BoneName;
						HitBoneIndex = Body->BoneIndex;
					}
				}
			}
			const float ImpactStrength = std::max(StockingComp->ImpactTearImpulseThreshold * 2.0f, 3.5f);
			if (StockingComp->ApplyWorldImpactTear(Event.Location, SafeDir * ImpactStrength, SafeDir * 3.0f, HitBoneIndex) &&
				!NearestBone.empty() && TargetMesh->IsRagdollActive())
			{
				TargetMesh->AddImpulseToBone(NearestBone, SafeDir * 3.5f, true);
			}
		}
		return;
	}
	AZombieActor* Zombie = Cast<AZombieActor>(Event.HitComponent->GetOwner());
	if (!Zombie || Zombie->IsDead()) return;
	if (const float* LastHitTime = ZombieHitCooldowns.Find(Zombie))
	{
		if (ElapsedTime - *LastHitTime < 0.25f) return;
	}
	ZombieHitCooldowns.Add(Zombie, ElapsedTime);

	Zombie->ReactToHit();
	if (!Zombie->IsDead())
	{
		// 공격자에서 좀비 방향으로 소폭 띄워 밀어낸다
		FVector LaunchDir = Zombie->GetActorLocation() - GetActorLocation();
		LaunchDir.Z = 0.0f;
		LaunchDir = LaunchDir.GetSafeNormal();
		if (LaunchDir.SizeSquared() <= SMALL_NUMBER)
			LaunchDir = FVector(-Event.Normal.X, -Event.Normal.Y, 0.0f).GetSafeNormal();
		if (LaunchDir.SizeSquared() > SMALL_NUMBER)
		{
			LaunchDir = (LaunchDir + FVector(0.0f, 0.0f, 0.25f)).GetSafeNormal();
			Zombie->LaunchCharacter(LaunchDir, 5.5f);
		}
	}
}

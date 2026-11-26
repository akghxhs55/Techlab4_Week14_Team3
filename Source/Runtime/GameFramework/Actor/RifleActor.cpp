#include "EnginePCH.h"
#include "GameFramework/Actor/RifleActor.h"
#include "GameFramework/Actor/ProjectileActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/ParticleSystemComponent.h"
#include "Components/ProjectileMovementComponent.h"
#include "Particles/ParticleSystem.h"
#include "Collision/HitResult.h"
#include "Collision/Ray.h"
#include "Asset/AssetManager.h"
#include "Engine/World.h"

ARifleActor::ARifleActor()
{
	SetActorEnableCollision(false);
	SkeletalMeshComponent = CreateDefaultSubobject<USkeletalMeshComponent>("RifleMeshComponent");
	SetRootComponent(SkeletalMeshComponent);
	USkeletalMesh* RifleMesh = UAssetManager::GetAssetByPath<USkeletalMesh>("Models/UnrealModel/SK_SK_Rifle_Root.fbx.bin");
	if (RifleMesh) SkeletalMeshComponent->SetSkeletalMesh(RifleMesh);
	else KRS_LOG(Error, "Failed to load SK_SK_Rifle_Root.fbx.bin for ARifleActor");

	MuzzleParticleComponent = CreateDefaultSubobject<UParticleSystemComponent>("MuzzleParticleComponent");
	MuzzleParticleComponent->SetupAttachment(SkeletalMeshComponent);
	MuzzleParticleComponent->SetRelativeLocation(FVector(MuzzleDistance, 0.0f, 0.089f));
	if (UParticleSystem* MuzzleTemplate = UAssetManager::LoadParticleSystem("Particles/MuzzleFlash.particle"))
		MuzzleParticleComponent->InitializeSystem(MuzzleTemplate);
	MuzzleParticleComponent->SetAutoActivate(false);
	MuzzleParticleComponent->SetVisibility(false);
	MuzzleParticleComponent->Deactivate();
}

void ARifleActor::BeginPlay()
{
	Super::BeginPlay();
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld()) return;
	// 사격 중 프레임 저하가 나지 않도록 발사체 풀을 시작 시 미리 생성한다
	while (Projectiles.Num() < MaxProjectiles)
	{
		if (!SpawnPooledProjectile(World)) break;
	}
}

AProjectileActor* ARifleActor::SpawnPooledProjectile(UWorld* World)
{
	if (!World || Projectiles.Num() >= MaxProjectiles) return nullptr;
	AProjectileActor* Projectile = Cast<AProjectileActor>(World->SpawnActor(AProjectileActor::StaticClass()));
	if (!Projectile) return nullptr;
	Projectile->GetRootComponent()->SetCollisionRadiusOverride(0.035f);
	Projectile->GetStaticMeshComponent()->SetRelativeScale3D(FVector(0.14f, 0.045f, 0.045f));
	Projectiles.Add(Projectile);
	return Projectile;
}

AProjectileActor* ARifleActor::Fire(const FVector& CameraOrigin, const FVector& CameraDirection)
{
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld() || MaxProjectiles <= 0 || ProjectileSpeed <= 0.0f) return nullptr;
	const FVector CameraForward = CameraDirection.GetSafeNormal();
	if (CameraForward.SizeSquared() <= SMALL_NUMBER) return nullptr;
	constexpr float AimTraceDistance = 500.0f;
	FVector Target = CameraOrigin + CameraForward * AimTraceDistance;
	for (AProjectileActor* ExistingProjectile : Projectiles)
	{
		if (IsValid(ExistingProjectile)) ExistingProjectile->SetActorEnableCollision(false);
	}
	FHitResult Hit;
	AActor* AttachParentActor = (GetRootComponent() && GetRootComponent()->GetAttachParent())
		? GetRootComponent()->GetAttachParent()->GetOwner() : nullptr;
	if (World->SweepSingle(FRay(CameraOrigin, CameraForward), AimTraceDistance, 0.0f,
		Hit, AttachParentActor))
	{
		Target = Hit.ImpactPoint;
	}
	for (AProjectileActor* ExistingProjectile : Projectiles)
	{
		if (IsValid(ExistingProjectile)) ExistingProjectile->SetActorEnableCollision(true);
	}

	AProjectileActor* Projectile = nullptr;
	for (AProjectileActor* Candidate : Projectiles)
	{
		if (IsValid(Candidate) && !Candidate->IsInFlight())
		{
			Projectile = Candidate;
			break;
		}
	}
	if (!Projectile)
		Projectile = SpawnPooledProjectile(World);
	if (!Projectile) return nullptr;

	UProjectileMovementComponent* Movement = Projectile->GetProjectileMovementComponent();
	Movement->InintialSpeed = ProjectileSpeed;
	Movement->MaxSpeed = ProjectileSpeed;
	Movement->ProjectileGravityScale = 0.0f;
	Movement->bShouldBounce = false;
	const FVector Muzzle = GetActorLocation() + GetActorQuat().GetForwardVector() * MuzzleDistance;
	FVector FireDirection = (Target - Muzzle).GetSafeNormal();
	if (FireDirection.SizeSquared() <= SMALL_NUMBER) FireDirection = CameraForward;
	Projectile->Launch(Muzzle, FireDirection, true);
	if (MuzzleParticleComponent)
	{
		// 발사 순간 총구 이펙트를 처음부터 다시 재생한다
		MuzzleParticleComponent->SetVisibility(true);
		MuzzleParticleComponent->Activate(true);
	}
	return Projectile;
}

void ARifleActor::EndPlay()
{
	for (AProjectileActor* Projectile : Projectiles)
	{
		if (IsValid(Projectile)) Projectile->Destroy();
	}
	Projectiles.Reset();
	Super::EndPlay();
}

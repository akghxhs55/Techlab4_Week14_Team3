#include "EnginePCH.h"
#include "GameFramework/Actor/ParticleHitEffectActor.h"
#include "GameFramework/Actor/ParticleEventManager.h"
#include "Asset/AssetManager.h"
#include "Particles/ParticleSystem.h"
#include "UObject/UObjectIterator.h"
#include "Engine/World.h"

AParticleHitEffectActor::AParticleHitEffectActor()
{
	UParticleSystem* HitTemplate = UAssetManager::LoadParticleSystem("Particles/HitParticles.particle");
	for (int32 i = 0; i < PoolSize; ++i)
	{
		const FString Name = "Effect_" + std::to_string(i);
		UParticleSystemComponent* Effect = CreateDefaultSubobject<UParticleSystemComponent>(Name);
		EffectPool.Add(Effect);

		Effect->SetAutoActivate(false);
		Effect->InitializeSystem(HitTemplate);
		Effect->SetVisibility(false);
		Effect->Deactivate();

		if (i == 0)
			SetRootComponent(Effect);
		else
			Effect->SetupAttachment(EffectPool[0]);
	}
}

void AParticleHitEffectActor::BeginPlay()
{
	Super::BeginPlay();

	// PIE 복제 시 현재 액터의 실제 컴포넌트로 풀을 재구성한다
	EffectPool.Reset();
	UParticleSystem* DefaultTemplate = UAssetManager::LoadParticleSystem("Particles/HitParticles.particle");
	for (UActorComponent* Comp : GetComponents())
	{
		UParticleSystemComponent* ParticleComp = Cast<UParticleSystemComponent>(Comp);
		if (!ParticleComp || ParticleComp->GetName().rfind("Effect_", 0) != 0)
			continue;
		if (!ParticleComp->GetTemplate() && DefaultTemplate)
			ParticleComp->InitializeSystem(DefaultTemplate);
		ParticleComp->SetVisibility(false);
		ParticleComp->Deactivate();
		EffectPool.Add(ParticleComp);
	}
	NextIndex = 0;

	for (TObjectIterator<AParticleEventManager> Iter; Iter; ++Iter)
	{
		if (Iter->GetWorld() != GetWorld())
			continue;
		Iter->RegisterListener("Hit", this, &AParticleHitEffectActor::OnHit);
	}
}

void AParticleHitEffectActor::OnHit(const FParticleEventCollisionData& Event)
{
	if (EffectPool.IsEmpty())
		return;

	UParticleSystemComponent* Chosen = nullptr;
	for (UParticleSystemComponent* Effect : EffectPool)
	{
		if (Effect && (!Effect->IsActive() || Effect->HasCompleted()))
		{
			Chosen = Effect;
			break;
		}
	}

	if (!Chosen)
	{
		Chosen = EffectPool[NextIndex];
		NextIndex = (NextIndex + 1) % PoolSize;
	}
	if (!Chosen)
		return;

	if (!Chosen->GetTemplate())
	{
		if (UParticleSystem* HitTemplate = UAssetManager::LoadParticleSystem("Particles/HitParticles.particle"))
			Chosen->InitializeSystem(HitTemplate);
	}

	Chosen->SetWorldLocation(Event.Location, false);
	Chosen->SetVisibility(true);
	Chosen->ActivateSystem(true);
}

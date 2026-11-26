#include "EnginePCH.h"
#include "GameFramework/Actor/ParticleEventManager.h"
#include <UObject/UObjectIterator.h>

AParticleEventManager::AParticleEventManager()
{
}

void AParticleEventManager::BeginPlay()
{
	Super::BeginPlay();
	for (TObjectIterator<UParticleSystemComponent> It; It; ++It)
	{
		if (It->GetWorld() != GetWorld())
			continue;
		It->OnParticleCollide.AddDynamic(this, &AParticleEventManager::HandleCollision);
	}
}

void AParticleEventManager::HandleCollision(UParticleSystemComponent* Comp, const FParticleEventCollisionData& Event)
{
	KRS_LOG(Warning, "[Manager] event={}", Event.EventName.ToString());
	FParticleCollisionSignature* Listener = Listeners.Find(Event.EventName.ToString());
	if (!Listener)
		return;
	Listener->Broadcast(Event);
}

void AParticleEventManager::UnregisterListener(const FString& Name, FDelegateHandle Handle)
{
	FParticleCollisionSignature* Listener = Listeners.Find(Name);
	if (!Listener)
		return;
	Listener->Remove(Handle);
}

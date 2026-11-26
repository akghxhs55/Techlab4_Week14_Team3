#pragma once

#include "GameFramework/Actor.h"
#include "Components/ParticleSystemComponent.h"
#include "Core/Delegate/Delegate.h"
#include "ParticleEventManager.generated.h"

DECLARE_DELEGATE(FParticleCollisionSignature, const FParticleEventCollisionData&);

UCLASS()
class AParticleEventManager : public AActor
{
	GENERATED_BODY()
public:
	AParticleEventManager();
	void BeginPlay() override;
	void HandleCollision(UParticleSystemComponent* Comp, const FParticleEventCollisionData& Event);

	template <typename T>
	FDelegateHandle RegisterListener(const FString& Name, T* Instance, void (T::* Func)(const FParticleEventCollisionData& Data))
	{
		return Listeners.FindOrAdd(Name).AddDynamic(Instance, Func);
	}
	void UnregisterListener(const FString& Name, FDelegateHandle Handle);

private:
	TMap<FString, FParticleCollisionSignature> Listeners;
};

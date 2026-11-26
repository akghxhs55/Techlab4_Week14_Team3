#pragma once

#include "GameFramework/Actor.h"
#include "Components/BillboardComponent.h"
#include "Components/DecalComponent.h"
#include "DecalActor.generated.h"


UCLASS()
class ADecalActor : public AActor
{
    GENERATED_BODY()
public:
    ADecalActor();
    void Tick(float DeltaTime) override;
    void SetTemporaryLifetime(float InSeconds);
    UDecalComponent* GetDecalComponent() const { return DecalComponent; }

private:
    UDecalComponent* DecalComponent = nullptr;
    UBillboardComponent* BillboardComponent = nullptr;
	float RemainingLifetime = 0.0f;
	bool bUseTemporaryLifetime = false;
};
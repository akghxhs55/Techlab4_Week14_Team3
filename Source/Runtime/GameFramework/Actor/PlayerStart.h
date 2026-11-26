#pragma once

#include "GameFramework/Actor.h"
#include "PlayerStart.generated.h"

class UCapsuleComponent;
class UBillboardComponent;
class UStaticMeshComponent;

UCLASS()
class APlayerStart : public AActor
{
	GENERATED_BODY()
public:
	APlayerStart();

private:
	UCapsuleComponent* CapsuleComponent = nullptr;
	UBillboardComponent* BillboardComponent = nullptr;
	UStaticMeshComponent* ArrowComponent = nullptr;
};

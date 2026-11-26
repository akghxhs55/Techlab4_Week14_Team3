#pragma once

#include "BodySetup.h"
#include "BodyInstance.h"
#include "SkeletalBodySetup.generated.h"

UCLASS()
class USkeletalBodySetup : public UBodySetup
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere)
	FString BoneName;

	// 물리 인스턴스에는 설정값만 복사한다. RigidActor 포인터는 공유하지 않는다.
	UPROPERTY(EditAnywhere)
	FBodyInstance DefaultInstance;
};

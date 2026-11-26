#pragma once

#include "UObject/Object.h"
#include "ConstraintInstance.h"
#include "PhysicsConstraintTemplate.generated.h"

UCLASS()
class UPhysicsConstraintTemplate : public UObject
{
	GENERATED_BODY()
public:
	// 에셋에서는 설정만 사용하며 Joint는 생성하지 않는다.
	FConstraintInstance DefaultInstance;
};

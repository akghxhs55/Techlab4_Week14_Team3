#pragma once

#include "AggregateGeom.h"
#include "BodySetup.generated.h"

namespace physx
{
	class PxTriangleMesh;
}

UCLASS()
class UBodySetup : public UObject
{
	GENERATED_BODY()
public:
	virtual ~UBodySetup() override;

	UPROPERTY(EditAnywhere)
	FString BoneName;

	UPROPERTY(EditAnywhere)
	FAggregateGeom AggGeom;

	physx::PxTriangleMesh* TriangleMesh = nullptr;
};

#pragma once

#include "Core/Types.h"
#include "Particles/ParticleModule.h"
#include "ParticleModuleSpawnPerUnit.generated.h"

UCLASS()
class UParticleModuleSpawnPerUnit : public UParticleModule
{
	GENERATED_BODY()
public:
	UParticleModuleSpawnPerUnit() = default;

	float GetSpawnPerUnit() const { return SpawnPerUnit; }
	float GetMovementTolerance() const { return MovementTolerance; }
	float GetMaxFrameDistance() const { return MaxFrameDistance; }
	bool ShouldIgnoreSpawnRateWhenMoving() const { return bIgnoreSpawnRateWhenMoving; }

private:
	UPROPERTY(EditAnywhere, meta=(ClampMin=0))
	float SpawnPerUnit = 10.0f;

	UPROPERTY(EditAnywhere, meta=(ClampMin=0))
	float MovementTolerance = 0.01f;

	UPROPERTY(EditAnywhere, meta=(ClampMin=0))
	float MaxFrameDistance = 50.0f;

	UPROPERTY(EditAnywhere)
	bool bIgnoreSpawnRateWhenMoving = false;
};

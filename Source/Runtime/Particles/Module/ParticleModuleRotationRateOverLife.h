#pragma once

#include "Core/Types.h"
#include "Particles/ParticleModule.h"
#include "Particles/Distributions.h"
#include "ParticleModuleRotationRateOverLife.generated.h"

UCLASS()
class UParticleModuleRotationRateOverLife : public UParticleModule
{
	GENERATED_BODY()
public:
	UParticleModuleRotationRateOverLife();

	void Update(FParticleEmitterInstance& Owner, int32 Offset, float DeltaTime) override;
	void BakeDistributions() override;

	FFloatDistribution& GetLifeMultiplier() { return LifeMultiplier; }

private:
	UPROPERTY(EditAnywhere)
	FFloatDistribution LifeMultiplier;

	UPROPERTY(EditAnywhere)
	bool bScale = true;
};

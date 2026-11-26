#pragma once

#include "Core/Types.h"
#include "Particles/ParticleModule.h"
#include "Particles/Distributions.h"
#include "ParticleModuleVelocityOverLife.generated.h"

UCLASS()
class UParticleModuleVelocityOverLife : public UParticleModule
{
	GENERATED_BODY()
public:
	UParticleModuleVelocityOverLife();

	void Update(FParticleEmitterInstance& Owner, int32 Offset, float DeltaTime) override;
	void BakeDistributions() override;

	FVectorDistribution& GetVelOverLife() { return VelOverLife; }

private:
	UPROPERTY(EditAnywhere)
	FVectorDistribution VelOverLife;

	UPROPERTY(EditAnywhere)
	bool bMultiplyVelocity = true;
};

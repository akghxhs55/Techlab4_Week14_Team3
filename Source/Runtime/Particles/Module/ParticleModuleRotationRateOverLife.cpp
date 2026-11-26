#include "EnginePCH.h"
#include "ParticleModuleRotationRateOverLife.h"
#include "Particles/ParticleEmitterInstance.h"
#include "Particles/ParticleHelper.h"

UParticleModuleRotationRateOverLife::UParticleModuleRotationRateOverLife()
{
	bUpdateModule = true;

	LifeMultiplier.Mode = EDistributionMode::Constant;
	LifeMultiplier.Constant = 1.0f;
	BakeDistributions();
}

void UParticleModuleRotationRateOverLife::BakeDistributions()
{
	LifeMultiplier.Bake();
}

void UParticleModuleRotationRateOverLife::Update(FParticleEmitterInstance& Owner, int32 Offset, float DeltaTime)
{
	ParticleHelper::ForEachParticle(Owner, [&](FBaseParticle& Particle, uint8*)
	{
		const float RateScale = LifeMultiplier.GetFloatValue(Particle.RelativeTime, Owner.RandomStream);
		if (bScale)
		{
			Particle.RotationRate *= RateScale;
		}
		else
		{
			Particle.RotationRate += RateScale;
		}
	});
}

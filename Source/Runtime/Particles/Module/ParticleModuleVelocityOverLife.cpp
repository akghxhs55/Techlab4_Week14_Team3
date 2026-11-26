#include "EnginePCH.h"
#include "ParticleModuleVelocityOverLife.h"
#include "Particles/ParticleEmitterInstance.h"
#include "Particles/ParticleHelper.h"

UParticleModuleVelocityOverLife::UParticleModuleVelocityOverLife()
{
	bUpdateModule = true;

	VelOverLife.Mode = EDistributionMode::Constant;
	VelOverLife.Constant = FVector(1.0f, 1.0f, 1.0f);
	BakeDistributions();
}

void UParticleModuleVelocityOverLife::BakeDistributions()
{
	VelOverLife.Bake();
}

void UParticleModuleVelocityOverLife::Update(FParticleEmitterInstance& Owner, int32 Offset, float DeltaTime)
{
	ParticleHelper::ForEachParticle(Owner, [&](FBaseParticle& Particle, uint8*)
	{
		const FVector Scale = VelOverLife.GetVectorValue(Particle.RelativeTime, Owner.RandomStream);
		if (bMultiplyVelocity)
		{
			Particle.Velocity.X *= Scale.X;
			Particle.Velocity.Y *= Scale.Y;
			Particle.Velocity.Z *= Scale.Z;
		}
		else
		{
			Particle.Velocity += Owner.EmitterToSimulationVector(Scale);
		}
	});
}

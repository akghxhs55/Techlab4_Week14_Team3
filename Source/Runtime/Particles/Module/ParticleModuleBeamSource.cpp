#include "EnginePCH.h"
#include "ParticleModuleBeamSource.h"

UParticleModuleBeamSource::UParticleModuleBeamSource()
{
	Source.Mode = EDistributionMode::Constant;
	Source.Constant = FVector::ZeroVector;

	SourceTangent.Mode = EDistributionMode::Constant;
	SourceTangent.Constant = FVector(1.0f, 0.0f, 0.0f);

	SourceStrength.Mode = EDistributionMode::Constant;
	SourceStrength.Constant = 25.0f;

	BakeDistributions();
}

void UParticleModuleBeamSource::BakeDistributions()
{
	Source.Bake();
	SourceTangent.Bake();
	SourceStrength.Bake();
}

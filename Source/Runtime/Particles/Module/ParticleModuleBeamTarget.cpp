#include "EnginePCH.h"
#include "ParticleModuleBeamTarget.h"

UParticleModuleBeamTarget::UParticleModuleBeamTarget()
{
	Target.Mode = EDistributionMode::Constant;
	Target.Constant = FVector(100.0f, 0.0f, 0.0f);

	TargetTangent.Mode = EDistributionMode::Constant;
	TargetTangent.Constant = FVector(1.0f, 0.0f, 0.0f);

	TargetStrength.Mode = EDistributionMode::Constant;
	TargetStrength.Constant = 25.0f;

	BakeDistributions();
}

void UParticleModuleBeamTarget::BakeDistributions()
{
	Target.Bake();
	TargetTangent.Bake();
	TargetStrength.Bake();
}

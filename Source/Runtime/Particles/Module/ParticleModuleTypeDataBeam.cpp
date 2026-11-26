#include "EnginePCH.h"
#include "ParticleModuleTypeDataBeam.h"
#include "Particles/ParticleBeamEmitterInstance.h"

UParticleModuleTypeDataBeam::UParticleModuleTypeDataBeam()
{
	TaperFactor.Mode = EDistributionMode::Constant;
	TaperFactor.Constant = 1.0f;

	TaperScale.Mode = EDistributionMode::Constant;
	TaperScale.Constant = 1.0f;

	BakeDistributions();
}

void UParticleModuleTypeDataBeam::BakeDistributions()
{
	TaperFactor.Bake();
	TaperScale.Bake();
}

FParticleEmitterInstance* UParticleModuleTypeDataBeam::CreateInstance(UParticleEmitter* InEmitter, UParticleSystemComponent* InComponent)
{
	FParticleBeamEmitterInstance* Instance = new FParticleBeamEmitterInstance();
	Instance->Init(InEmitter, InComponent);
	return Instance;
}

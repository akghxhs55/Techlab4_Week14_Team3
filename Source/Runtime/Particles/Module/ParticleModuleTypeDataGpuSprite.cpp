#include "EnginePCH.h"
#include "ParticleModuleTypeDataGpuSprite.h"
#include "Particles/ParticleGpuEmitterInstance.h"

FParticleEmitterInstance* UParticleModuleTypeDataGpuSprite::CreateInstance(UParticleEmitter* InEmitter, UParticleSystemComponent* InComponent)
{
	FParticleGpuEmitterInstance* Instance = new FParticleGpuEmitterInstance();
	Instance->Init(InEmitter, InComponent);
	return Instance;
}
